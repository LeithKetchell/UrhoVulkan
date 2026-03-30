// ClaudeTerminal — Urho3D terminal for Claude Code with IPC injection
//
// Phase 2: Virtual screen buffer + cursor tracking + SGR colors.
// The screen buffer mirrors what Claude Code thinks the terminal looks like.
// Cursor position is tracked to determine when injection is safe.

#include "ClaudeTerminal.h"

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Core/ProcessUtils.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Input/InputEvents.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/FontFace.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#ifndef _WIN32
#include <pty.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#endif

URHO3D_DEFINE_APPLICATION_MAIN(ClaudeTerminal);

static const char* StateToString(ClaudeState s)
{
    switch (s)
    {
    case CLAUDE_BUSY: return "BUSY";
    case CLAUDE_SETTLING: return "SETTLING";
    case CLAUDE_READY: return "READY";
    }
    return "?";
}

// ============================================================================
// ANSI color table — matched to GNOME VTE default palette
// ============================================================================

static const Color kAnsiColors[16] = {
    Color(0.090f, 0.078f, 0.129f), // 0  black         #171421
    Color(0.753f, 0.110f, 0.157f), // 1  red           #C01C28
    Color(0.149f, 0.635f, 0.412f), // 2  green         #26A269
    Color(0.635f, 0.451f, 0.298f), // 3  yellow/brown  #A2734C
    Color(0.071f, 0.282f, 0.545f), // 4  blue          #12488B
    Color(0.639f, 0.278f, 0.729f), // 5  magenta       #A347BA
    Color(0.165f, 0.631f, 0.702f), // 6  cyan          #2AA1B3
    Color(0.816f, 0.812f, 0.800f), // 7  white         #D0CFCC
    Color(0.369f, 0.361f, 0.392f), // 8  bright black  #5E5C64
    Color(0.965f, 0.380f, 0.318f), // 9  bright red    #F66151
    Color(0.200f, 0.855f, 0.478f), // 10 bright green  #33DA7A
    Color(0.914f, 0.678f, 0.047f), // 11 bright yellow #E9AD0C
    Color(0.165f, 0.482f, 0.871f), // 12 bright blue   #2A7BDE
    Color(0.753f, 0.380f, 0.796f), // 13 bright magenta #C061CB
    Color(0.200f, 0.780f, 0.871f), // 14 bright cyan   #33C7DE
    Color(1.000f, 1.000f, 1.000f), // 15 bright white  #FFFFFF
};

Color ClaudeTerminal::AnsiToColor(unsigned char idx, bool bold)
{
    if (idx < 16)
    {
        if (bold && idx < 8)
            return kAnsiColors[idx + 8];  // Bold promotes to bright variant
        return kAnsiColors[idx];
    }
    // 256-color: 16-231 = 6x6x6 cube, 232-255 = grayscale
    if (idx < 232)
    {
        int v = idx - 16;
        float r = (float)((v / 36) % 6) / 5.0f;
        float g = (float)((v / 6) % 6) / 5.0f;
        float b = (float)(v % 6) / 5.0f;
        return Color(r, g, b);
    }
    // Grayscale ramp
    float grey = (float)(idx - 232) / 23.0f;
    return Color(grey, grey, grey);
}

Color ClaudeTerminal::CellFgColor(const ScreenCell& cell)
{
    if (cell.hasTrueColorFg)
        return cell.trueColorFg;
    return AnsiToColor(cell.fg, cell.bold);
}

Color ClaudeTerminal::CellBgColor(const ScreenCell& cell)
{
    if (cell.hasTrueColorBg)
        return cell.trueColorBg;
    return AnsiToColor(cell.bg, false);
}

// ============================================================================
// Lifecycle
// ============================================================================

ClaudeTerminal::ClaudeTerminal(Context* context)
    : Application(context)
{
}

ClaudeTerminal::~ClaudeTerminal()
{
    Stop();
}

void ClaudeTerminal::Setup()
{
    // Determine role from command line args for window title
    StringVector arguments = GetArguments();
    String role = "Coder";
    for (unsigned i = 0; i < arguments.Size(); i++)
    {
        if (arguments[i] == "--role" && i + 1 < arguments.Size())
        {
            role = arguments[i + 1];
            role[0] = (char)toupper(role[0]);
        }
        else if (arguments[i] == "--planner")
            role = "Planner";
        else if (arguments[i] == "--coder")
            role = "Coder";
    }
    engineParameters_[EP_WINDOW_TITLE] = "Claudette - " + role;
    engineParameters_[EP_WINDOW_WIDTH] = 1200;
    engineParameters_[EP_WINDOW_HEIGHT] = 800;
    engineParameters_[EP_FULL_SCREEN] = false;
    engineParameters_[EP_WINDOW_RESIZABLE] = true;
    engineParameters_[EP_LOG_NAME] = "ClaudeTerminal.log";
    engineParameters_[EP_RESOURCE_PATHS] = "CoreData;Data";
    engineParameters_[EP_SOUND] = false;
    engineParameters_[EP_FRAME_LIMITER] = true;
}

void ClaudeTerminal::Start()
{
    GetSubsystem<Engine>()->SetMaxFps(30);

    auto* cache = GetSubsystem<ResourceCache>();
    auto* uiStyle = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(uiStyle);

    font_ = cache->GetResource<Font>("Fonts/UbuntuMono-R.ttf");
    if (!font_)
        font_ = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
    if (!font_)
        font_ = cache->GetResource<Font>("Fonts/BlueHighway.ttf");

    // Measure font metrics from the font face directly — exact values
    {
        FontFace* face = font_->GetFace((float)fontSize_);
        if (face)
        {
            const FontGlyph* glyph = face->GetGlyph('M');
            if (glyph)
                cellWidthF_ = glyph->advanceX_;
            cellHeightF_ = face->GetRowHeight();
        }
        // Integer cell size — round to nearest, never truncate
        charWidth_ = (int)(cellWidthF_ + 0.5f);
        charHeight_ = (int)(cellHeightF_ + 0.5f);
        if (charWidth_ < 1) charWidth_ = 7;
        if (charHeight_ < 1) charHeight_ = 14;
        URHO3D_LOGINFO("Font metrics: cellWidth=" + String(cellWidthF_) +
            " cellHeight=" + String(cellHeightF_) +
            " intW=" + String(charWidth_) + " intH=" + String(charHeight_));
    }

    // Init screen buffer
    InitScreen(screenCols_, screenRows_);

    CreateUI();

    // Setup IPC socket
    auto* fs = GetSubsystem<FileSystem>();
    String ttyDir = "/tmp/urho_claude/tty";
    fs->CreateDir("/tmp/urho_claude");
    fs->CreateDir(ttyDir);

    int spawnNum = 1;
    while (fs->FileExists(ttyDir + "/spawn_" + String(spawnNum) + ".sock"))
        spawnNum++;
    socketPath_ = ttyDir + "/spawn_" + String(spawnNum) + ".sock";

    if (!StartIPCListener(socketPath_))
        URHO3D_LOGERROR("Failed to start IPC listener on " + socketPath_);

    // Determine role from command line args — default to coder
    StringVector arguments = GetArguments();
    String role = "coder";
    for (unsigned i = 0; i < arguments.Size(); i++)
    {
        if (arguments[i] == "--role" && i + 1 < arguments.Size())
            role = arguments[i + 1];
        else if (arguments[i] == "--planner")
            role = "planner";
        else if (arguments[i] == "--coder")
            role = "coder";
    }

    String initPrompt = "Follow the Session Startup Protocol: assume the " + role +
        " role by running .claude/hooks/claude_ipc.sh assume " + role +
        " -- then read Claude/WORKBOARD.md and check in "
        "with Leith for your assignment. Use .claude/hooks/safe_build.sh for all builds, never raw make.";

    StringVector args;
    args.Push("--dangerously-skip-permissions");
    args.Push(initPrompt);
    if (!SpawnChild("claude", args))
        URHO3D_LOGERROR("Failed to spawn Claude Code");

    claudeState_ = CLAUDE_BUSY;
    lastOutputTimer_.Reset();
    lastCursorMoveTimer_.Reset();

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(ClaudeTerminal, HandleUpdate));
    SubscribeToEvent(E_KEYDOWN, URHO3D_HANDLER(ClaudeTerminal, HandleKeyDown));
    SubscribeToEvent(E_TEXTINPUT, URHO3D_HANDLER(ClaudeTerminal, HandleTextInput));
    SubscribeToEvent("ScreenMode", URHO3D_HANDLER(ClaudeTerminal, HandleResize));
    SubscribeToEvent(E_DROPFILE, URHO3D_HANDLER(ClaudeTerminal, HandleDropFile));
    SubscribeToEvent(E_MOUSEBUTTONDOWN, URHO3D_HANDLER(ClaudeTerminal, HandleMouseButtonDown));
    SubscribeToEvent(E_MOUSEBUTTONUP, URHO3D_HANDLER(ClaudeTerminal, HandleMouseButtonUp));
    SubscribeToEvent(E_MOUSEMOVE, URHO3D_HANDLER(ClaudeTerminal, HandleMouseMove));

    GetSubsystem<Input>()->SetMouseVisible(true);
}

void ClaudeTerminal::Stop()
{
#ifndef _WIN32
    for (int fd : clientFds_)
        if (fd >= 0) close(fd);
    clientFds_.Clear();

    if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
    if (!socketPath_.Empty()) unlink(socketPath_.CString());
    if (masterFd_ >= 0) { close(masterFd_); masterFd_ = -1; }
    if (childPid_ > 0)
    {
        kill(childPid_, SIGTERM);
        int status;
        waitpid(childPid_, &status, WNOHANG);
        childPid_ = 0;
    }
#endif
}

// ============================================================================
// Virtual screen buffer
// ============================================================================

void ClaudeTerminal::InitScreen(int cols, int rows)
{
    screenCols_ = cols;
    screenRows_ = rows;
    screen_.Resize(rows);
    for (int r = 0; r < rows; r++)
        screen_[r].Resize(cols);
    cursorRow_ = 0;
    cursorCol_ = 0;
}

void ClaudeTerminal::ScreenPutChar(char c)
{
    // Single ASCII character — delegate to string version
    char buf[2] = { c, '\0' };
    ScreenPutString(String(buf));
}

void ClaudeTerminal::ScreenPutString(const String& ch)
{
    if (cursorCol_ >= screenCols_)
    {
        ScreenNewLine();
        cursorCol_ = 0;
    }

    // Decode UTF-8 to codepoint for wcwidth
    unsigned cp = 0;
    if (ch.Length() == 1)
        cp = (unsigned char)ch[0];
    else if (ch.Length() == 2)
        cp = ((unsigned char)ch[0] & 0x1F) << 6 | ((unsigned char)ch[1] & 0x3F);
    else if (ch.Length() == 3)
        cp = ((unsigned char)ch[0] & 0x0F) << 12 | ((unsigned char)ch[1] & 0x3F) << 6 | ((unsigned char)ch[2] & 0x3F);
    else if (ch.Length() == 4)
        cp = ((unsigned char)ch[0] & 0x07) << 18 | ((unsigned char)ch[1] & 0x3F) << 12 |
             ((unsigned char)ch[2] & 0x3F) << 6 | ((unsigned char)ch[3] & 0x3F);

    int width = TermWcWidth(cp);
    if (width == 0)
        return;  // Combining character — skip for now

    if (cursorRow_ >= 0 && cursorRow_ < screenRows_ &&
        cursorCol_ >= 0 && cursorCol_ < screenCols_)
    {
        ScreenCell& cell = screen_[cursorRow_][cursorCol_];
        cell.ch = ch;
        cell.fg = curFg_;
        cell.bg = curBg_;
        cell.bold = curBold_;
        cell.dim = curDim_;
        cell.underline = curUnderline_;
        cell.hasTrueColorFg = curHasTrueColorFg_;
        cell.hasTrueColorBg = curHasTrueColorBg_;
        cell.trueColorFg = curTrueColorFg_;
        cell.trueColorBg = curTrueColorBg_;

        // Wide character — blank the next cell
        if (width == 2 && cursorCol_ + 1 < screenCols_)
        {
            ScreenCell& next = screen_[cursorRow_][cursorCol_ + 1];
            next.ch = "";  // Empty — part of wide char
            next.fg = curFg_;
            next.bg = curBg_;
            next.hasTrueColorFg = curHasTrueColorFg_;
            next.hasTrueColorBg = curHasTrueColorBg_;
            next.trueColorFg = curTrueColorFg_;
            next.trueColorBg = curTrueColorBg_;
        }
    }
    cursorCol_ += width;
    screenDirty_ = true;
}

void ClaudeTerminal::ScreenNewLine()
{
    cursorRow_++;
    if (cursorRow_ >= screenRows_)
    {
        ScreenScrollUp();
        cursorRow_ = screenRows_ - 1;
    }
}

void ClaudeTerminal::ScreenCarriageReturn()
{
    cursorCol_ = 0;
}

void ClaudeTerminal::ScreenScrollUp()
{
    // Push top row to scrollback
    PushScrollback(0);

    // Shift all rows up
    for (int r = 0; r < screenRows_ - 1; r++)
        screen_[r] = screen_[r + 1];

    // Clear bottom row
    screen_[screenRows_ - 1].Resize(screenCols_);
    for (int c = 0; c < screenCols_; c++)
        screen_[screenRows_ - 1][c] = ScreenCell();

    screenDirty_ = true;
}

void ClaudeTerminal::ScreenClearLine(int row, int fromCol, int toCol)
{
    if (row < 0 || row >= screenRows_) return;
    if (fromCol < 0) fromCol = 0;
    if (toCol > screenCols_) toCol = screenCols_;
    for (int c = fromCol; c < toCol; c++)
        screen_[row][c] = ScreenCell();
    screenDirty_ = true;
}

void ClaudeTerminal::ScreenClearScreen(int mode)
{
    if (mode == 0)  // Clear from cursor down
    {
        ScreenClearLine(cursorRow_, cursorCol_, screenCols_);
        for (int r = cursorRow_ + 1; r < screenRows_; r++)
            ScreenClearLine(r, 0, screenCols_);
    }
    else if (mode == 1)  // Clear from cursor up
    {
        ScreenClearLine(cursorRow_, 0, cursorCol_ + 1);
        for (int r = 0; r < cursorRow_; r++)
            ScreenClearLine(r, 0, screenCols_);
    }
    else if (mode == 2 || mode == 3)  // Clear entire screen
    {
        for (int r = 0; r < screenRows_; r++)
            ScreenClearLine(r, 0, screenCols_);
        cursorRow_ = 0;
        cursorCol_ = 0;
    }
    screenDirty_ = true;
}

void ClaudeTerminal::ScreenSetCursor(int row, int col)
{
    cursorRow_ = Clamp(row, 0, screenRows_ - 1);
    cursorCol_ = Clamp(col, 0, screenCols_ - 1);
    lastCursorMoveTimer_.Reset();
}

String ClaudeTerminal::GetRowText(int row)
{
    if (row < 0 || row >= screenRows_) return String::EMPTY;
    String text;
    int lastNonSpace = -1;
    for (int c = 0; c < screenCols_; c++)
    {
        if (screen_[row][c].ch != " ")
            lastNonSpace = c;
    }
    for (int c = 0; c <= lastNonSpace; c++)
        text += screen_[row][c].ch;
    return text;
}

void ClaudeTerminal::PushScrollback(int row)
{
    String text = GetRowText(row);
    if (text.Trimmed().Empty())
        return;  // Don't store blank lines

    ScrollLine sl;
    sl.text = text;
    // Use color from first non-space cell
    for (int c = 0; c < screenCols_; c++)
    {
        if (screen_[row][c].ch != " ")
        {
            sl.color = AnsiToColor(screen_[row][c].fg, screen_[row][c].bold);
            break;
        }
    }
    scrollback_.Push(sl);
    while (scrollback_.Size() > maxScrollback_)
        scrollback_.Erase(0);
}

// ============================================================================
// ANSI parser — byte-by-byte state machine
// ============================================================================

void ClaudeTerminal::ProcessByte(unsigned char c)
{
    switch (parseState_)
    {
    case PS_NORMAL:
        if (c == 0x1B)
            parseState_ = PS_ESC;
        else if (c == 0x9B)  // 8-bit CSI
        {
            csiParams_.Clear();
            parseState_ = PS_CSI;
        }
        else if (c == '\n')
            ScreenNewLine();
        else if (c == '\r')
            ScreenCarriageReturn();
        else if (c == '\b')
        {
            if (cursorCol_ > 0) cursorCol_--;
        }
        else if (c == '\t')
        {
            int nextTab = ((cursorCol_ / 8) + 1) * 8;
            cursorCol_ = Min(nextTab, screenCols_ - 1);
        }
        else if (c == 0x07)
        {
            // BEL — ignore
        }
        else if (c >= 0xC0 && c <= 0xF7)
        {
            // UTF-8 lead byte — start accumulating
            utf8Buf_[0] = (char)c;
            utf8Got_ = 1;
            if (c < 0xE0)      utf8Expected_ = 2;
            else if (c < 0xF0) utf8Expected_ = 3;
            else               utf8Expected_ = 4;
            parseState_ = PS_UTF8;
        }
        else if (c >= 32 && c < 0x80)
        {
            // Plain ASCII
            ScreenPutChar((char)c);
        }
        // Control chars 0x00-0x1F and 0x80-0xBF stray continuations — drop
        break;

    case PS_UTF8:
        if ((c & 0xC0) == 0x80)  // Valid continuation byte
        {
            utf8Buf_[utf8Got_++] = (char)c;
            if (utf8Got_ >= utf8Expected_)
            {
                utf8Buf_[utf8Got_] = '\0';
                ScreenPutString(String(utf8Buf_));
                utf8Got_ = 0;
                utf8Expected_ = 0;
                parseState_ = PS_NORMAL;
            }
        }
        else
        {
            // Broken UTF-8 — discard accumulated bytes, reprocess this byte
            utf8Got_ = 0;
            utf8Expected_ = 0;
            parseState_ = PS_NORMAL;
            ProcessByte(c);  // Re-enter with the unexpected byte
        }
        break;

    case PS_ESC:
        if (c == '[')
        {
            csiParams_.Clear();
            parseState_ = PS_CSI;
        }
        else if (c == ']')
        {
            oscBuffer_.Clear();
            parseState_ = PS_OSC;
        }
        else if (c == 'P')
            parseState_ = PS_DCS;
        else if (c == '(' || c == ')' || c == '*' || c == '+')
            parseState_ = PS_CHARSET;
        else if (c == '7')
        {
            // Save cursor
            savedCursorRow_ = cursorRow_;
            savedCursorCol_ = cursorCol_;
            parseState_ = PS_NORMAL;
        }
        else if (c == '8')
        {
            // Restore cursor
            ScreenSetCursor(savedCursorRow_, savedCursorCol_);
            parseState_ = PS_NORMAL;
        }
        else if (c == 'M')
        {
            // Reverse index — scroll down
            if (cursorRow_ > 0)
                cursorRow_--;
            parseState_ = PS_NORMAL;
        }
        else if (c == 'c')
        {
            // Reset terminal
            ScreenClearScreen(2);
            curFg_ = 7; curBg_ = 0; curBold_ = false; curDim_ = false; curUnderline_ = false;
            curHasTrueColorFg_ = false; curHasTrueColorBg_ = false;
            parseState_ = PS_NORMAL;
        }
        else
        {
            parseState_ = PS_NORMAL;
        }
        break;

    case PS_CSI:
        if (c >= 0x40 && c <= 0x7E)
        {
            // Final byte — append it then execute
            csiParams_ += (char)c;
            ProcessCSI();
            csiParams_.Clear();
            parseState_ = PS_NORMAL;
        }
        else
        {
            // Parameter or intermediate byte
            csiParams_ += (char)c;
        }
        break;

    case PS_OSC:
        if (c == 0x07)
            parseState_ = PS_NORMAL;
        else if (c == 0x1B)
            parseState_ = PS_OSC_ESC;
        else
            oscBuffer_ += (char)c;
        break;

    case PS_OSC_ESC:
        parseState_ = PS_NORMAL;  // ST = ESC backslash
        break;

    case PS_DCS:
        if (c == 0x1B)
            parseState_ = PS_DCS_ESC;
        break;

    case PS_DCS_ESC:
        parseState_ = PS_NORMAL;
        break;

    case PS_CHARSET:
        // Skip one byte (the charset designator)
        parseState_ = PS_NORMAL;
        break;
    }
}

void ClaudeTerminal::ProcessCSI()
{
    if (csiParams_.Empty())
        return;

    char cmd = csiParams_[csiParams_.Length() - 1];
    String params = csiParams_.Substring(0, csiParams_.Length() - 1);

    // Strip leading '?' for private modes
    bool privateMode = false;
    if (!params.Empty() && params[0] == '?')
    {
        privateMode = true;
        params = params.Substring(1);
    }

    // Parse semicolon-separated numeric params
    Vector<int> nums;
    StringVector parts = params.Split(';');
    for (unsigned i = 0; i < parts.Size(); i++)
    {
        int val = ToI32(parts[i]);
        nums.Push(val);
    }

    int p1 = nums.Size() > 0 ? nums[0] : 0;
    int p2 = nums.Size() > 1 ? nums[1] : 0;

    switch (cmd)
    {
    case 'H': case 'f':  // Cursor position
        ScreenSetCursor(p1 > 0 ? p1 - 1 : 0, p2 > 0 ? p2 - 1 : 0);
        break;
    case 'A':  // Cursor up
        ScreenSetCursor(cursorRow_ - Max(p1, 1), cursorCol_);
        break;
    case 'B':  // Cursor down
        ScreenSetCursor(cursorRow_ + Max(p1, 1), cursorCol_);
        break;
    case 'C':  // Cursor forward
        ScreenSetCursor(cursorRow_, cursorCol_ + Max(p1, 1));
        break;
    case 'D':  // Cursor back
        ScreenSetCursor(cursorRow_, cursorCol_ - Max(p1, 1));
        break;
    case 'E':  // Cursor next line
        ScreenSetCursor(cursorRow_ + Max(p1, 1), 0);
        break;
    case 'F':  // Cursor previous line
        ScreenSetCursor(cursorRow_ - Max(p1, 1), 0);
        break;
    case 'G':  // Cursor horizontal absolute
        ScreenSetCursor(cursorRow_, p1 > 0 ? p1 - 1 : 0);
        break;
    case 'J':  // Erase in display
        ScreenClearScreen(p1);
        break;
    case 'K':  // Erase in line
        if (p1 == 0)
            ScreenClearLine(cursorRow_, cursorCol_, screenCols_);
        else if (p1 == 1)
            ScreenClearLine(cursorRow_, 0, cursorCol_ + 1);
        else if (p1 == 2)
            ScreenClearLine(cursorRow_, 0, screenCols_);
        break;
    case 'S':  // Scroll up
        for (int i = 0; i < Max(p1, 1); i++)
            ScreenScrollUp();
        break;
    case 'm':  // SGR — colors and attributes
        ProcessSGR();
        break;
    case 's':  // Save cursor
        savedCursorRow_ = cursorRow_;
        savedCursorCol_ = cursorCol_;
        break;
    case 'u':  // Restore cursor
        ScreenSetCursor(savedCursorRow_, savedCursorCol_);
        break;
    case 'h':  // Set mode
        if (privateMode && p1 == 1049)
            altScreenActive_ = true;  // Alternate screen buffer
        break;
    case 'l':  // Reset mode
        if (privateMode && p1 == 1049)
            altScreenActive_ = false;
        break;
    case 'L':  // Insert lines
    {
        int count = Max(p1, 1);
        for (int i = 0; i < count && cursorRow_ + i < screenRows_; i++)
            ScreenClearLine(cursorRow_ + i, 0, screenCols_);
        break;
    }
    case 'M':  // Delete lines — treat as scroll
    {
        int count = Max(p1, 1);
        for (int i = 0; i < count; i++)
            ScreenScrollUp();
        break;
    }
    // r, d, and others — ignore for now
    default:
        break;
    }
}

void ClaudeTerminal::ProcessSGR()
{
    // Parse the params string (before the 'm')
    String params = csiParams_.Substring(0, csiParams_.Length() - 1);
    if (params.Empty() || params[0] == '?')
        params = "0";  // No params = reset

    // Strip leading '?'
    if (!params.Empty() && params[0] == '?')
        params = params.Substring(1);

    StringVector parts = params.Split(';');

    for (unsigned i = 0; i < parts.Size(); i++)
    {
        int code = ToI32(parts[i]);

        if (code == 0)
        {
            curFg_ = 7; curBg_ = 0; curBold_ = false; curDim_ = false; curUnderline_ = false;
            curHasTrueColorFg_ = false; curHasTrueColorBg_ = false;
        }
        else if (code == 1)
            curBold_ = true;
        else if (code == 2)
            curDim_ = true;
        else if (code == 4)
            curUnderline_ = true;
        else if (code == 22)
        {
            curBold_ = false; curDim_ = false;
        }
        else if (code == 24)
            curUnderline_ = false;
        else if (code >= 30 && code <= 37)
        {
            curFg_ = (unsigned char)(code - 30);
            curHasTrueColorFg_ = false;
        }
        else if (code == 38)
        {
            // Extended foreground: 38;5;N or 38;2;R;G;B
            if (i + 1 < parts.Size())
            {
                int mode = ToI32(parts[i + 1]);
                if (mode == 5 && i + 2 < parts.Size())
                {
                    curFg_ = (unsigned char)ToI32(parts[i + 2]);
                    curHasTrueColorFg_ = false;
                    i += 2;
                }
                else if (mode == 2 && i + 4 < parts.Size())
                {
                    // True color — store exact RGB
                    int r = ToI32(parts[i + 2]);
                    int g = ToI32(parts[i + 3]);
                    int b = ToI32(parts[i + 4]);
                    curTrueColorFg_ = Color(r / 255.0f, g / 255.0f, b / 255.0f);
                    curHasTrueColorFg_ = true;
                    i += 4;
                }
            }
        }
        else if (code == 39)
        {
            curFg_ = 7;  // Default foreground
            curHasTrueColorFg_ = false;
        }
        else if (code >= 40 && code <= 47)
        {
            curBg_ = (unsigned char)(code - 40);
            curHasTrueColorBg_ = false;
        }
        else if (code == 48)
        {
            // Extended background: 48;5;N or 48;2;R;G;B
            if (i + 1 < parts.Size())
            {
                int mode = ToI32(parts[i + 1]);
                if (mode == 5 && i + 2 < parts.Size())
                {
                    curBg_ = (unsigned char)ToI32(parts[i + 2]);
                    curHasTrueColorBg_ = false;
                    i += 2;
                }
                else if (mode == 2 && i + 4 < parts.Size())
                {
                    // True color — store exact RGB
                    int r = ToI32(parts[i + 2]);
                    int g = ToI32(parts[i + 3]);
                    int b = ToI32(parts[i + 4]);
                    curTrueColorBg_ = Color(r / 255.0f, g / 255.0f, b / 255.0f);
                    curHasTrueColorBg_ = true;
                    i += 4;
                }
            }
        }
        else if (code == 49)
        {
            curBg_ = 0;  // Default background
            curHasTrueColorBg_ = false;
        }
        else if (code >= 90 && code <= 97)
        {
            curFg_ = (unsigned char)(code - 90 + 8);  // Bright foreground
            curHasTrueColorFg_ = false;
        }
        else if (code >= 100 && code <= 107)
        {
            curBg_ = (unsigned char)(code - 100 + 8);  // Bright background
            curHasTrueColorBg_ = false;
        }
    }
}

// ============================================================================
// wcwidth — terminal character width (1 or 2 columns)
// ============================================================================

int ClaudeTerminal::TermWcWidth(unsigned cp)
{
    // Control characters and zero-width
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    // Combining characters (common ranges)
    if (cp >= 0x0300 && cp <= 0x036F) return 0;  // Combining diacriticals
    if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;  // Combining half marks
    // CJK ranges — width 2
    if (cp >= 0x1100 && cp <= 0x115F) return 2;   // Hangul Jamo
    if (cp >= 0x2E80 && cp <= 0x303E) return 2;   // CJK radicals, symbols
    if (cp >= 0x3040 && cp <= 0x33BF) return 2;   // Hiragana, Katakana, CJK compat
    if (cp >= 0x3400 && cp <= 0x4DBF) return 2;   // CJK Extension A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;   // CJK Unified
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 2;   // Hangul syllables
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;   // CJK compatibility ideographs
    if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;   // CJK compatibility forms
    if (cp >= 0xFF01 && cp <= 0xFF60) return 2;   // Fullwidth forms
    if (cp >= 0x20000 && cp <= 0x2FFFF) return 2; // CJK Extension B+
    if (cp >= 0x30000 && cp <= 0x3FFFF) return 2; // CJK Extension G+
    // Emoji (most are width 2 in terminals)
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return 2;
    if (cp >= 0x2600 && cp <= 0x27BF) return 1;   // Misc symbols (width 1 in most terms)
    // Everything else — width 1
    return 1;
}

// ============================================================================
// Block character rendering — geometric rectangles instead of font glyphs
// ============================================================================

bool ClaudeTerminal::IsBlockChar(const String& ch)
{
    if (ch.Length() < 3) return false;
    unsigned char b0 = (unsigned char)ch[0];
    unsigned char b1 = (unsigned char)ch[1];
    unsigned char b2 = (unsigned char)ch[2];
    // U+2580-U+259F → UTF-8: E2 96 80 - E2 96 9F
    if (b0 == 0xE2 && b1 == 0x96 && b2 >= 0x80 && b2 <= 0x9F)
        return true;
    // U+2588 (full block) is in that range
    return false;
}

void ClaudeTerminal::RenderBlockChar(const String& ch, int x, int y, Color fg, Color bg)
{
    if (ch.Length() < 3) return;
    unsigned char b2 = (unsigned char)ch[2];

    // Decode the block element
    // U+2580 = ▀ upper half block
    // U+2581-2587 = lower 1/8 to 7/8 blocks
    // U+2588 = █ full block
    // U+2589-258F = left 7/8 to 1/8 blocks
    // U+2590 = ▐ right half block
    // U+2591 = ░ light shade
    // U+2592 = ▒ medium shade
    // U+2593 = ▓ dark shade
    // U+2594 = ▔ upper 1/8 block
    // U+2595 = ▕ right 1/8 block
    // U+2596-259F = quadrant blocks

    int cw = (int)(cellWidthF_ + 0.5f);
    int ch_ = (int)(cellHeightF_ + 0.5f);

    if (b2 == 0x80)  // ▀ upper half
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_ / 2);
        r->SetColor(fg);
    }
    else if (b2 >= 0x81 && b2 <= 0x87)  // Lower N/8 blocks
    {
        int eighths = b2 - 0x80;  // 1-7
        int h = (ch_ * eighths) / 8;
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y + ch_ - h);
        r->SetSize(cw, h);
        r->SetColor(fg);
    }
    else if (b2 == 0x88)  // █ full block
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(fg);
    }
    else if (b2 >= 0x89 && b2 <= 0x8F)  // Left N/8 blocks
    {
        int eighths = 0x90 - b2;  // 7 down to 1
        int w = (cw * eighths) / 8;
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(w, ch_);
        r->SetColor(fg);
    }
    else if (b2 == 0x90)  // ▐ right half
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x + cw / 2, y);
        r->SetSize(cw - cw / 2, ch_);
        r->SetColor(fg);
    }
    else if (b2 == 0x91)  // ░ light shade
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.25f, fg.g_ * 0.25f, fg.b_ * 0.25f));
    }
    else if (b2 == 0x92)  // ▒ medium shade
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.5f, fg.g_ * 0.5f, fg.b_ * 0.5f));
    }
    else if (b2 == 0x93)  // ▓ dark shade
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.75f, fg.g_ * 0.75f, fg.b_ * 0.75f));
    }
    else if (b2 == 0x94)  // ▔ upper 1/8
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_ / 8);
        r->SetColor(fg);
    }
    else if (b2 == 0x95)  // ▕ right 1/8
    {
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x + cw - cw / 8, y);
        r->SetSize(cw / 8, ch_);
        r->SetColor(fg);
    }
    else if (b2 >= 0x96 && b2 <= 0x9F)
    {
        // Quadrant blocks — each fills specific quarter-cells
        // UL = upper-left, UR = upper-right, LL = lower-left, LR = lower-right
        int hw = cw / 2;
        int hh = ch_ / 2;
        int rw = cw - hw;  // Right half width (handles odd widths)
        int rh = ch_ - hh; // Bottom half height

        // Which quadrants are filled for each codepoint
        bool ul = false, ur = false, ll = false, lr = false;
        switch (b2)
        {
        case 0x96: ll = true; break;                          // ▖
        case 0x97: lr = true; break;                          // ▗
        case 0x98: ul = true; break;                          // ▘
        case 0x99: ul = true; ll = true; lr = true; break;   // ▙
        case 0x9A: ul = true; lr = true; break;               // ▚
        case 0x9B: ul = true; ur = true; ll = true; break;   // ▛
        case 0x9C: ul = true; ur = true; lr = true; break;   // ▜
        case 0x9D: ur = true; break;                          // ▝
        case 0x9E: ur = true; ll = true; break;               // ▞
        case 0x9F: ur = true; ll = true; lr = true; break;   // ▟
        }

        if (ul)
        {
            BorderImage* r = screenPanel_->CreateChild<BorderImage>();
            r->SetPosition(x, y);
            r->SetSize(hw, hh);
            r->SetColor(fg);
        }
        if (ur)
        {
            BorderImage* r = screenPanel_->CreateChild<BorderImage>();
            r->SetPosition(x + hw, y);
            r->SetSize(rw, hh);
            r->SetColor(fg);
        }
        if (ll)
        {
            BorderImage* r = screenPanel_->CreateChild<BorderImage>();
            r->SetPosition(x, y + hh);
            r->SetSize(hw, rh);
            r->SetColor(fg);
        }
        if (lr)
        {
            BorderImage* r = screenPanel_->CreateChild<BorderImage>();
            r->SetPosition(x + hw, y + hh);
            r->SetSize(rw, rh);
            r->SetColor(fg);
        }
    }
    else
    {
        // Unknown block — render as full block
        BorderImage* r = screenPanel_->CreateChild<BorderImage>();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(fg);
    }
}

// ============================================================================
// PTY management
// ============================================================================

bool ClaudeTerminal::SpawnChild(const String& command, const StringVector& args)
{
#ifndef _WIN32
    struct winsize ws;
    ws.ws_col = (unsigned short)screenCols_;
    ws.ws_row = (unsigned short)screenRows_;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    childPid_ = forkpty(&masterFd_, nullptr, nullptr, &ws);
    if (childPid_ < 0)
    {
        URHO3D_LOGERROR("forkpty() failed: " + String(strerror(errno)));
        return false;
    }

    if (childPid_ == 0)
    {
        Vector<const char*> argv;
        String cmd = command;
        argv.Push(cmd.CString());
        for (unsigned i = 0; i < args.Size(); i++)
            argv.Push(args[i].CString());
        argv.Push(nullptr);

        setenv("TERM", "xterm-256color", 1);
        setenv("FORCE_COLOR", "1", 1);
        unsetenv("CLAUDECODE");

        // Tell the assume command where our IPC socket is
        // so it creates a symlink (coder.sock → our socket) instead of
        // spawning a standalone TTY listener that bypasses us
        setenv("PTY_PROXY_SOCK", socketPath_.CString(), 1);

        execvp(command.CString(), const_cast<char* const*>(&argv[0]));
        _exit(127);
    }

    int flags = fcntl(masterFd_, F_GETFL, 0);
    fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK);
    URHO3D_LOGINFO("Spawned child PID " + String((int)childPid_) + " on fd " + String(masterFd_));
    return true;
#else
    return false;
#endif
}

void ClaudeTerminal::ReadPTY()
{
#ifndef _WIN32
    if (masterFd_ < 0) return;

    char buf[4096];
    bool gotData = false;
    for (;;)
    {
        ssize_t n = read(masterFd_, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            gotData = true;
            if (logTraffic_ && !logPath_.Empty())
            {
                File logFile(context_, logPath_, FILE_WRITE);
                if (logFile.IsOpen())
                {
                    logFile.Seek(logFile.GetSize());
                    logFile.Write(buf, (unsigned)n);
                }
            }
            // Feed each byte to the ANSI parser
            for (ssize_t i = 0; i < n; i++)
                ProcessByte((unsigned char)buf[i]);
        }
        else if (n == 0)
        {
            close(masterFd_);
            masterFd_ = -1;
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            URHO3D_LOGERROR("PTY read error: " + String(strerror(errno)));
            break;
        }
    }

    if (gotData)
    {
        claudeState_ = CLAUDE_BUSY;
        lastOutputTimer_.Reset();
    }
#endif
}

void ClaudeTerminal::WritePTY(const String& data)
{
#ifndef _WIN32
    if (masterFd_ < 0) return;
    WritePTYRaw(data.CString(), data.Length());
#endif
}

void ClaudeTerminal::WritePTYRaw(const char* data, unsigned len)
{
#ifndef _WIN32
    if (masterFd_ < 0) return;
    unsigned written = 0;
    while (written < len)
    {
        ssize_t n = write(masterFd_, data + written, len - written);
        if (n > 0) written += (unsigned)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) usleep(1000);
        else { URHO3D_LOGERROR("PTY write error: " + String(strerror(errno))); break; }
    }
#endif
}

bool ClaudeTerminal::IsChildAlive()
{
#ifndef _WIN32
    if (childPid_ <= 0) return false;
    int status;
    pid_t result = waitpid(childPid_, &status, WNOHANG);
    if (result == 0) return true;
    childPid_ = 0;
    return false;
#else
    return false;
#endif
}

// ============================================================================
// IPC Socket
// ============================================================================

bool ClaudeTerminal::StartIPCListener(const String& socketPath)
{
#ifndef _WIN32
    unlink(socketPath.CString());
    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.CString(), sizeof(addr.sun_path) - 1);

    if (bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { close(listenFd_); listenFd_ = -1; return false; }
    if (listen(listenFd_, 5) < 0)
    { close(listenFd_); listenFd_ = -1; return false; }

    int flags = fcntl(listenFd_, F_GETFL, 0);
    fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
    chmod(socketPath.CString(), 0770);
    URHO3D_LOGINFO("IPC listening on " + socketPath);
    return true;
#else
    return false;
#endif
}

void ClaudeTerminal::PollIPC()
{
#ifndef _WIN32
    if (listenFd_ < 0) return;
    for (;;)
    {
        int clientFd = accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) break;
        int flags = fcntl(clientFd, F_GETFL, 0);
        fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
        clientFds_.Push(clientFd);
    }

    Vector<int> dead;
    for (unsigned i = 0; i < clientFds_.Size(); i++)
    {
        String accumulated;
        char buf[4096];
        for (;;)
        {
            ssize_t n = read(clientFds_[i], buf, sizeof(buf) - 1);
            if (n > 0) { buf[n] = '\0'; accumulated += String(buf, (unsigned)n); }
            else { if (n == 0) { close(clientFds_[i]); dead.Push(i); } break; }
        }
        if (!accumulated.Empty())
            HandleIPCMessage(accumulated);
    }
    for (int i = dead.Size() - 1; i >= 0; i--)
        clientFds_.Erase(dead[i]);
#endif
}

void ClaudeTerminal::HandleIPCMessage(const String& message)
{
    // Strip bracketed paste escapes and control chars sent by WorkboardManager
    String clean = message;
    clean.Replace("\x1b[200~", "");
    clean.Replace("\x1b[201~", "");
    clean.Replace("\r", "");
    clean.Replace("\n", " ");
    clean = clean.Trimmed();

    if (clean.Empty())
        return;

    URHO3D_LOGINFO("IPC → input box → submit: " + clean);

    // Put in input box, then immediately submit — same path as Enter key
    if (inputLine_)
    {
        inputLine_->SetText(clean);
        WritePTY(clean + "\r");
        inputLine_->SetText("");
    }
}

// ============================================================================
// State machine
// ============================================================================

void ClaudeTerminal::UpdateState()
{
    unsigned elapsed = lastOutputTimer_.GetMSec(false);
    unsigned cursorIdle = lastCursorMoveTimer_.GetMSec(false);

    switch (claudeState_)
    {
    case CLAUDE_BUSY:
        if (elapsed >= busyToSettleMs_)
            claudeState_ = CLAUDE_SETTLING;
        break;
    case CLAUDE_SETTLING:
        // Require BOTH output silence AND cursor stability
        if (elapsed >= settleThresholdMs_ && cursorIdle >= settleThresholdMs_)
            claudeState_ = CLAUDE_READY;
        break;
    case CLAUDE_READY:
        break;
    }
}

void ClaudeTerminal::DrainInjectionQueue()
{
    if (injectionQueue_.Empty())
        return;

    // Drain all queued messages — Claude Code handles input buffering internally.
    // Previous code gated on claudeState_ == CLAUDE_READY which caused messages
    // to stall forever if Claude stayed busy. HandleIPCMessage already writes
    // directly to PTY regardless of state and works fine.
    while (!injectionQueue_.Empty())
    {
        String msg = injectionQueue_[0];
        injectionQueue_.Erase(0);
        URHO3D_LOGINFO("Queue → PTY: " + msg);
        WritePTY(msg + "\r");
    }
}

// ============================================================================
// Display — render screen buffer to scrollable terminal view
// ============================================================================

void ClaudeTerminal::RenderScreen()
{
    if (!screenDirty_ || !screenPanel_) return;

    // ── Scrollback items ──
    if (scrollbackPanel_)
    {
        bool hadNewScrollback = lastRenderedScrollback_ < scrollback_.Size();

        for (unsigned i = lastRenderedScrollback_; i < scrollback_.Size(); i++)
        {
            Text* t = scrollbackPanel_->CreateChild<Text>();
            t->SetFont(font_, fontSize_);
            t->SetText(scrollback_[i].text);
            t->SetColor(scrollback_[i].color);
            t->SetPosition(0, (int)(i * cellHeightF_));
        }
        lastRenderedScrollback_ = scrollback_.Size();

        // Trim oldest scrollback items if over limit
        while (scrollbackPanel_->GetNumChildren() > 500)
        {
            scrollbackPanel_->RemoveChildAtIndex(0);
            // Reposition remaining items after trim
            for (unsigned j = 0; j < scrollbackPanel_->GetNumChildren(); j++)
                scrollbackPanel_->GetChild(j)->SetPosition(0, (int)(j * cellHeightF_));
            // Adjust tracking for trimmed entries
            if (lastRenderedScrollback_ > 0)
                lastRenderedScrollback_--;
        }

        // Update scrollback panel height
        int sbHeight = (int)(scrollbackPanel_->GetNumChildren() * cellHeightF_);
        scrollbackPanel_->SetSize(scrollbackPanel_->GetWidth(), sbHeight);

        // Reposition live screen below scrollback
        int screenHeight = screenRows_ * charHeight_;
        if (screenBg_)
        {
            screenBg_->SetPosition(0, sbHeight);
            screenBg_->SetSize(scrollContent_->GetWidth(), screenHeight);
            screenPanel_->SetSize(scrollContent_->GetWidth(), screenHeight);
        }

        // Resize content to fit scrollback + screen
        int totalHeight = sbHeight + screenHeight;
        if (scrollContent_)
            scrollContent_->SetSize(scrollContent_->GetWidth(), totalHeight);

        // Auto-scroll to bottom when new content arrives (unless user scrolled up)
        if (hadNewScrollback && autoScroll_ && scrollView_)
        {
            int viewHeight = scrollView_->GetHeight();
            int maxScroll = totalHeight - viewHeight;
            if (maxScroll > 0)
                scrollView_->SetViewPosition(IntVector2(0, maxScroll));
        }
    }

    // ── Live screen grid — pixel-positioned spans with true color + selection/search ──
    screenPanel_->RemoveAllChildren();

    for (int r = 0; r < screenRows_; r++)
    {
        int y = (int)(r * cellHeightF_);

        // Walk cells, build foreground spans and background spans
        int c = 0;
        while (c < screenCols_)
        {
            const ScreenCell& sc = screen_[r][c];
            bool hasBg = sc.bg != 0 || sc.hasTrueColorBg;
            bool selected = IsCellSelected(r, c);
            bool searchHit = IsCellSearchMatch(r, c);

            // Skip empty cells with no special highlighting
            if (sc.ch == " " && !hasBg && !selected && !searchHit)
            {
                c++;
                continue;
            }

            // ── Block characters — render as geometric rectangles ──
            if (IsBlockChar(sc.ch))
            {
                if (selected || searchHit)
                {
                    BorderImage* hlRect = screenPanel_->CreateChild<BorderImage>();
                    hlRect->SetPosition((int)(c * cellWidthF_), y);
                    hlRect->SetSize((int)(cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                    hlRect->SetColor(selected ? Color(0.2f, 0.4f, 0.7f, 0.7f) : Color(0.6f, 0.5f, 0.1f, 0.7f));
                }
                else if (hasBg)
                {
                    BorderImage* bgR = screenPanel_->CreateChild<BorderImage>();
                    bgR->SetPosition((int)(c * cellWidthF_), y);
                    bgR->SetSize((int)(cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                    bgR->SetColor(CellBgColor(sc));
                }
                RenderBlockChar(sc.ch, (int)(c * cellWidthF_), y,
                    CellFgColor(sc), CellBgColor(sc));
                c++;
                continue;
            }

            // ── Background span: consecutive cells with same bg color ──
            if (hasBg && !selected && !searchHit)
            {
                Color bgColor = CellBgColor(sc);
                int startC = c;
                // Group cells with matching bg (index match for indexed, color match for true color)
                while (c < screenCols_)
                {
                    const ScreenCell& nc = screen_[r][c];
                    if (IsCellSelected(r, c) || IsCellSearchMatch(r, c))
                        break;
                    Color ncBg = CellBgColor(nc);
                    bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
                    if (!ncHasBg || ncBg != bgColor)
                        break;
                    c++;
                }

                BorderImage* bgRect = screenPanel_->CreateChild<BorderImage>();
                bgRect->SetPosition((int)(startC * cellWidthF_), y);
                bgRect->SetSize((int)((c - startC) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                bgRect->SetColor(bgColor);

                // Render foreground text over this bg span
                int fc = startC;
                while (fc < c)
                {
                    Color fgColor = CellFgColor(screen_[r][fc]);
                    int fStart = fc;
                    String spanText;
                    while (fc < c && CellFgColor(screen_[r][fc]) == fgColor)
                    {
                        spanText += screen_[r][fc].ch;
                        fc++;
                    }
                    if (spanText.Trimmed().Length() > 0)
                    {
                        Text* t = screenPanel_->CreateChild<Text>();
                        t->SetFont(font_, fontSize_);
                        t->SetText(spanText);
                        t->SetColor(fgColor);
                        t->SetPosition((int)(fStart * cellWidthF_), y);
                    }
                }
                continue;
            }

            // ── Foreground-only span (or highlighted single cells fall through) ──
            Color fgColor = CellFgColor(sc);
            int startC = c;
            String spanText;
            while (c < screenCols_)
            {
                const ScreenCell& nc = screen_[r][c];
                bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
                bool ncSel = IsCellSelected(r, c);
                bool ncSearch = IsCellSearchMatch(r, c);
                // Break span on: bg change, selection boundary, search boundary, or fg color change
                if (ncHasBg || ncSel != selected || ncSearch != searchHit ||
                    CellFgColor(nc) != fgColor)
                    break;
                spanText += nc.ch;
                c++;
            }

            // Draw span-wide selection/search highlight
            if (selected || searchHit)
            {
                BorderImage* hlRect = screenPanel_->CreateChild<BorderImage>();
                hlRect->SetPosition((int)(startC * cellWidthF_), y);
                hlRect->SetSize((int)((c - startC) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                hlRect->SetColor(selected ? Color(0.2f, 0.4f, 0.7f, 0.7f) : Color(0.6f, 0.5f, 0.1f, 0.7f));
            }

            // Trim trailing spaces from fg-only spans
            while (!spanText.Empty() && spanText[spanText.Length() - 1] == ' ')
                spanText.Resize(spanText.Length() - 1);

            if (!spanText.Empty())
            {
                Text* t = screenPanel_->CreateChild<Text>();
                t->SetFont(font_, fontSize_);
                t->SetText(spanText);
                t->SetColor(selected ? Color(1.0f, 1.0f, 1.0f) : fgColor);
                t->SetPosition((int)(startC * cellWidthF_), y);
            }
        }
    }

    screenDirty_ = false;
}

// ============================================================================
// UI
// ============================================================================

void ClaudeTerminal::CreateUI()
{
    auto* root = GetSubsystem<UI>()->GetRoot();
    auto* graphics = GetSubsystem<Graphics>();
    int w = graphics->GetWidth();
    int h = graphics->GetHeight();

    // ── Status bar (top) ──
    statusBar_ = root->CreateChild<Text>("StatusBar");
    statusBar_->SetFont(font_, fontSize_);
    statusBar_->SetText("Initializing...");
    statusBar_->SetColor(Color(0.6f, 0.8f, 0.6f));
    statusBar_->SetPosition(8, 2);

    // ── Input bar (bottom) — larger font for readability ──
    int inputFontSize = 16;
    int inputHeight = inputFontSize + 20;  // Padding around text
    inputLine_ = root->CreateChild<LineEdit>("InputLine");
    inputLine_->SetStyleAuto();
    inputLine_->SetPosition(4, h - inputHeight - 2);
    inputLine_->SetSize(w - 8, inputHeight);
    inputLine_->SetFocusMode(FM_FOCUSABLE_DEFOCUSABLE);
    inputLine_->SetFocus(true);
    // Set larger font on the LineEdit's text element
    if (inputLine_->GetTextElement())
        inputLine_->GetTextElement()->SetFont(font_, inputFontSize);

    int termTop = 20;
    int termHeight = h - termTop - inputHeight - 4;
    int screenHeight = screenRows_ * charHeight_;

    // ── Scrollable terminal area (scrollback + live screen) ──
    scrollView_ = root->CreateChild<ScrollView>("TermScrollView");
    scrollView_->SetStyleAuto();
    scrollView_->SetPosition(4, termTop);
    scrollView_->SetSize(w - 8, termHeight);
    scrollView_->SetScrollBarsVisible(false, true);  // Vertical scrollbar only
    scrollView_->SetFocusMode(FM_NOTFOCUSABLE);        // Arrow keys go to PTY, not scrollbar
    scrollView_->SetScrollStep((float)charHeight_ * 3.0f);  // 3 lines per arrow key
    scrollView_->SetPageStep((float)(termHeight / charHeight_ - 2));  // PageUp/Down

    // Content element — sized to fit scrollback + live screen
    scrollContent_ = new UIElement(context_);
    scrollContent_->SetSize(w - 24, screenHeight);  // Start with just screen height; grows with scrollback
    scrollView_->SetContentElement(scrollContent_);

    // Scrollback panel — Text items stack here vertically
    scrollbackPanel_ = scrollContent_->CreateChild<UIElement>("ScrollbackPanel");
    scrollbackPanel_->SetPosition(0, 0);
    // Height starts at 0, grows as scrollback lines are added

    // ── Live screen panel (grid-rendered) — dark terminal background ──
    screenBg_ = scrollContent_->CreateChild<BorderImage>("ScreenBg");
    screenBg_->SetPosition(0, 0);  // Repositioned in RenderScreen when scrollback grows
    screenBg_->SetSize(w - 24, screenHeight);
    screenBg_->SetColor(Color(0.133f, 0.133f, 0.149f));  // #222226 — matches gnome-terminal

    screenPanel_ = screenBg_->CreateChild<UIElement>("ScreenPanel");
    screenPanel_->SetPosition(0, 0);
    screenPanel_->SetSize(w - 24, screenHeight);
    screenPanel_->SetClipChildren(true);
}

// ============================================================================
// Event handlers
// ============================================================================

void ClaudeTerminal::HandleUpdate(StringHash, VariantMap&)
{
    ReadPTY();
    PollIPC();
    UpdateState();

    // Drain injection queue — places messages in input box, auto-submits when READY
    DrainInjectionQueue();

    // Child health
    if (childPid_ > 0 && !IsChildAlive())
    {
        childPid_ = 0;
        if (statusBar_)
        {
            statusBar_->SetText("EXITED");
            statusBar_->SetColor(Color(1.0f, 0.4f, 0.4f));
        }
    }

    // Status bar
    if (statusBar_ && childPid_ > 0)
    {
        String stateStr = StateToString(claudeState_);
        unsigned queueSize = injectionQueue_.Size() + (pacedBuffer_.Empty() ? 0 : 1);
        String queueStr = queueSize > 0 ? " | Q:" + String(queueSize) : "";
        String cursorStr = " | Cur:" + String(cursorRow_) + "," + String(cursorCol_);
        String newText = stateStr + " | PID " + String((int)childPid_) + cursorStr +
            " | SB:" + String(scrollback_.Size()) + queueStr;

        if (newText != lastStatusText_)
        {
            lastStatusText_ = newText;
            statusBar_->SetText(newText);
            if (claudeState_ == CLAUDE_READY)
                statusBar_->SetColor(Color(0.4f, 1.0f, 0.4f));
            else if (claudeState_ == CLAUDE_SETTLING)
                statusBar_->SetColor(Color(0.9f, 0.8f, 0.3f));
            else
                statusBar_->SetColor(Color(0.8f, 0.5f, 0.3f));
        }
    }

    // Auto-scroll management — detect manual scroll to pause, re-enable at bottom
    if (scrollView_ && scrollContent_)
    {
        int viewHeight = scrollView_->GetHeight();
        int contentHeight = scrollContent_->GetHeight();
        int maxScroll = contentHeight - viewHeight;
        int currentY = scrollView_->GetViewPosition().y_;

        // If user scrolled up (not at bottom), disable auto-scroll
        if (maxScroll > 0 && currentY < maxScroll - charHeight_)
            autoScroll_ = false;
        // If at bottom (within one line), re-enable auto-scroll
        else if (maxScroll <= 0 || currentY >= maxScroll - charHeight_)
            autoScroll_ = true;
    }

    RenderScreen();
}

void ClaudeTerminal::HandleKeyDown(StringHash, VariantMap& eventData)
{
    using namespace KeyDown;
    int key = eventData[P_KEY].GetI32();
    int quals = eventData[P_QUALIFIERS].GetI32();

    // Enter — search next if search bar focused, otherwise submit to PTY
    if (key == KEY_RETURN || key == KEY_KP_ENTER)
    {
        if (searchVisible_ && searchInput_ && searchInput_->HasFocus())
        {
            String query = searchInput_->GetText();
            if (query != searchQuery_)
                DoSearch(query, true);
            else if (searchMatches_.Size() > 0)
            {
                // Cycle to next match
                currentMatch_ = (currentMatch_ + 1) % (int)searchMatches_.Size();
                if (searchStatus_)
                    searchStatus_->SetText(String(currentMatch_ + 1) + "/" + String(searchMatches_.Size()));
                screenDirty_ = true;
            }
            return;
        }
        if (inputLine_)
        {
            String text = inputLine_->GetText();
            if (!text.Empty())
            {
                WritePTY(text + "\r");
                inputLine_->SetText("");
            }
            else
            {
                WritePTY("\r");
            }
        }
        return;
    }

    // Ctrl combos
    if (quals & QUAL_CTRL)
    {
        if (key == KEY_C)
        {
            // With selection: copy to clipboard. Without: SIGINT.
            if (HasSelection())
            {
                GetSubsystem<UI>()->SetClipboardText(GetSelectedText());
                ClearSelection();
                screenDirty_ = true;
            }
            else
                WritePTY("\x03");
        }
        else if (key == KEY_V)
        {
            // Paste from clipboard — into inputLine if focused, otherwise PTY
            const String& clip = GetSubsystem<UI>()->GetClipboardText();
            if (!clip.Empty())
            {
                if (inputLine_ && inputLine_->HasFocus())
                    inputLine_->SetText(inputLine_->GetText() + clip);
                else
                    WritePTY(clip);
            }
        }
        else if (key == KEY_F)
        {
            ToggleSearchBar();
        }
        else if (key == KEY_D) WritePTY("\x04");
        else if (key == KEY_Z) WritePTY("\x1a");
        else if (key == KEY_BACKSLASH) WritePTY("\x1c");  // SIGQUIT
        else if (key == KEY_A) WritePTY("\x01");  // Ctrl+A — accept
        else if (key == KEY_R) WritePTY("\x12");  // Ctrl+R — retry
        else if (key == KEY_K) WritePTY("\x0b");  // Ctrl+K — clear
        else if (key == KEY_L)
        {
            logTraffic_ = !logTraffic_;
            if (logTraffic_ && logPath_.Empty())
                logPath_ = "/tmp/urho_claude/pty_traffic.log";
        }
        return;
    }

    // Escape — close search bar if open, otherwise forward to PTY
    if (key == KEY_ESCAPE)
    {
        if (searchVisible_)
        {
            ToggleSearchBar();
            return;
        }
        if (HasSelection())
        {
            ClearSelection();
            screenDirty_ = true;
            return;
        }
        WritePTY("\033");
        return;
    }

    // Tab — forward to PTY for autocomplete
    if (key == KEY_TAB)
    {
        WritePTY("\t");
        return;
    }

    // Backspace — forward to PTY when input is empty
    if (key == KEY_BACKSPACE && inputLine_ && inputLine_->GetText().Empty())
    {
        WritePTY("\x7f");
        return;
    }

    // Delete — forward to PTY when input is empty
    if (key == KEY_DELETE && inputLine_ && inputLine_->GetText().Empty())
    {
        WritePTY("\033[3~");
        return;
    }

    // Shift+PageUp/PageDown, Shift+Home/End — scroll history
    if (quals & QUAL_SHIFT)
    {
        if (scrollView_ && scrollContent_)
        {
            int viewHeight = scrollView_->GetHeight();
            int contentHeight = scrollContent_->GetHeight();
            int maxScroll = contentHeight - viewHeight;
            int currentY = scrollView_->GetViewPosition().y_;
            int pageSize = viewHeight - charHeight_ * 2;  // One page minus 2 lines overlap
            if (pageSize < charHeight_) pageSize = charHeight_;

            if (key == KEY_PAGEUP)
            {
                int newY = currentY - pageSize;
                if (newY < 0) newY = 0;
                scrollView_->SetViewPosition(IntVector2(0, newY));
                autoScroll_ = false;
                return;
            }
            else if (key == KEY_PAGEDOWN)
            {
                int newY = currentY + pageSize;
                if (newY > maxScroll) newY = maxScroll;
                scrollView_->SetViewPosition(IntVector2(0, newY));
                return;
            }
            else if (key == KEY_HOME)
            {
                scrollView_->SetViewPosition(IntVector2(0, 0));
                autoScroll_ = false;
                return;
            }
            else if (key == KEY_END)
            {
                if (maxScroll > 0)
                    scrollView_->SetViewPosition(IntVector2(0, maxScroll));
                autoScroll_ = true;
                return;
            }
        }
    }

    // Arrow keys — ONLY forward to PTY if input box is empty
    // (when input box has text, arrows should navigate within the input box)
    if (inputLine_ && inputLine_->GetText().Empty())
    {
        if (key == KEY_UP) WritePTY("\033[A");
        else if (key == KEY_DOWN) WritePTY("\033[B");
    }

    // All other keys: let the LineEdit handle them (no PTY forwarding)
}

void ClaudeTerminal::HandleTextInput(StringHash, VariantMap&)
{
    // LineEdit captures text input
}

void ClaudeTerminal::HandleResize(StringHash, VariantMap&)
{
    auto* graphics = GetSubsystem<Graphics>();
    int w = graphics->GetWidth();
    int h = graphics->GetHeight();
    int inputHeight = 36;  // Matches larger input font
    int termTop = 24;
    int termHeight = h - termTop - inputHeight - 8;

    if (scrollView_)
    {
        scrollView_->SetPosition(4, termTop);
        scrollView_->SetSize(w - 8, termHeight);
    }
    if (scrollContent_)
    {
        int contentWidth = w - 24;  // Account for scrollbar
        scrollContent_->SetWidth(contentWidth);
        if (screenBg_)
            screenBg_->SetWidth(contentWidth);
        if (screenPanel_)
            screenPanel_->SetWidth(contentWidth);
    }
    if (inputLine_)
    {
        inputLine_->SetPosition(4, h - inputHeight - 4);
        inputLine_->SetSize(w - 8, inputHeight);
    }

#ifndef _WIN32
    if (masterFd_ >= 0)
    {
        int cols = (w - 16) / 7;  // Approx char width for Anonymous Pro 11pt
        int rows = (h - 60) / 14;
        if (cols < 40) cols = 40;
        if (rows < 10) rows = 10;

        struct winsize ws;
        ws.ws_col = cols;
        ws.ws_row = rows;
        ws.ws_xpixel = w;
        ws.ws_ypixel = h;
        ioctl(masterFd_, TIOCSWINSZ, &ws);

        // Resize screen buffer
        if (cols != screenCols_ || rows != screenRows_)
            InitScreen(cols, rows);
    }
#endif

    screenDirty_ = true;
}

void ClaudeTerminal::HandleDropFile(StringHash, VariantMap& eventData)
{
    using namespace DropFile;
    String fileName = eventData[P_FILENAME].GetString();

    if (fileName.Empty())
        return;

    // Check if it's an image file
    String lower = fileName.ToLower();
    bool isImage = lower.EndsWith(".png") || lower.EndsWith(".jpg") || lower.EndsWith(".jpeg") ||
                   lower.EndsWith(".gif") || lower.EndsWith(".bmp") || lower.EndsWith(".webp") ||
                   lower.EndsWith(".svg") || lower.EndsWith(".tiff") || lower.EndsWith(".tif");

    if (isImage)
    {
        // Send the image path to Claude Code's PTY — Claude reads images by path
        WritePTY(fileName + "\r");
        URHO3D_LOGINFO("Dropped image sent to PTY: " + fileName);
    }
    else
    {
        // Non-image file — put path in input box so user can decide what to do
        if (inputLine_)
            inputLine_->SetText(fileName);
        URHO3D_LOGINFO("Dropped file placed in input: " + fileName);
    }
}

// ============================================================================
// Selection (copy/paste)
// ============================================================================

bool ClaudeTerminal::ScreenToCell(int screenX, int screenY, int& row, int& col)
{
    if (!screenBg_ || !scrollView_)
        return false;

    // Convert screen coordinates to content-relative coordinates
    IntVector2 viewPos = scrollView_->GetViewPosition();
    IntVector2 scrollPos = scrollView_->GetScreenPosition();
    int contentX = screenX - scrollPos.x_ + viewPos.x_;
    int contentY = screenY - scrollPos.y_ + viewPos.y_;

    // Subtract screenBg position to get live-screen-relative coords
    IntVector2 bgPos = screenBg_->GetPosition();
    int localX = contentX - bgPos.x_;
    int localY = contentY - bgPos.y_;

    col = (int)(localX / cellWidthF_);
    row = (int)(localY / cellHeightF_);

    if (row < 0) row = 0;
    if (row >= screenRows_) row = screenRows_ - 1;
    if (col < 0) col = 0;
    if (col >= screenCols_) col = screenCols_ - 1;

    return localY >= 0 && localY < screenRows_ * (int)cellHeightF_;
}

void ClaudeTerminal::HandleMouseButtonDown(StringHash, VariantMap& eventData)
{
    using namespace MouseButtonDown;
    int button = eventData[P_BUTTON].GetI32();
    if (button != MOUSEB_LEFT)
        return;

    auto* input = GetSubsystem<Input>();
    IntVector2 pos = input->GetMousePosition();
    int row, col;
    if (ScreenToCell(pos.x_, pos.y_, row, col))
    {
        selecting_ = true;
        hasSelection_ = false;
        selStartRow_ = row;
        selStartCol_ = col;
        selEndRow_ = row;
        selEndCol_ = col;
        screenDirty_ = true;
    }
}

void ClaudeTerminal::HandleMouseButtonUp(StringHash, VariantMap& eventData)
{
    using namespace MouseButtonUp;
    int button = eventData[P_BUTTON].GetI32();
    if (button != MOUSEB_LEFT)
        return;

    if (selecting_)
    {
        selecting_ = false;
        // If start == end, treat as no selection (just a click)
        if (selStartRow_ == selEndRow_ && selStartCol_ == selEndCol_)
            hasSelection_ = false;
    }
}

void ClaudeTerminal::HandleMouseMove(StringHash, VariantMap&)
{
    if (!selecting_)
        return;

    auto* input = GetSubsystem<Input>();
    IntVector2 pos = input->GetMousePosition();
    int row, col;
    if (ScreenToCell(pos.x_, pos.y_, row, col))
    {
        selEndRow_ = row;
        selEndCol_ = col;
        hasSelection_ = true;
        screenDirty_ = true;
    }
}

bool ClaudeTerminal::HasSelection() const
{
    return hasSelection_;
}

void ClaudeTerminal::ClearSelection()
{
    hasSelection_ = false;
    selecting_ = false;
}

bool ClaudeTerminal::IsCellSelected(int row, int col) const
{
    if (!hasSelection_)
        return false;

    // Normalize so start <= end in reading order
    int r1 = selStartRow_, c1 = selStartCol_;
    int r2 = selEndRow_, c2 = selEndCol_;
    if (r1 > r2 || (r1 == r2 && c1 > c2))
    {
        int tr = r1; r1 = r2; r2 = tr;
        int tc = c1; c1 = c2; c2 = tc;
    }

    if (row < r1 || row > r2)
        return false;
    if (row == r1 && row == r2)
        return col >= c1 && col <= c2;
    if (row == r1)
        return col >= c1;
    if (row == r2)
        return col <= c2;
    return true;  // Middle rows are fully selected
}

String ClaudeTerminal::GetSelectedText()
{
    if (!hasSelection_)
        return String::EMPTY;

    // Normalize
    int r1 = selStartRow_, c1 = selStartCol_;
    int r2 = selEndRow_, c2 = selEndCol_;
    if (r1 > r2 || (r1 == r2 && c1 > c2))
    {
        int tr = r1; r1 = r2; r2 = tr;
        int tc = c1; c1 = c2; c2 = tc;
    }

    String result;
    for (int r = r1; r <= r2; r++)
    {
        int startC = (r == r1) ? c1 : 0;
        int endC = (r == r2) ? c2 : screenCols_ - 1;
        String line;
        for (int c = startC; c <= endC && c < screenCols_; c++)
            line += screen_[r][c].ch;
        // Trim trailing spaces from each line
        while (!line.Empty() && line[line.Length() - 1] == ' ')
            line.Resize(line.Length() - 1);
        result += line;
        if (r < r2)
            result += "\n";
    }
    return result;
}

// ============================================================================
// Search
// ============================================================================

void ClaudeTerminal::ToggleSearchBar()
{
    searchVisible_ = !searchVisible_;

    if (searchVisible_)
    {
        // Create search bar if not already created
        if (!searchBar_)
        {
            auto* root = GetSubsystem<UI>()->GetRoot();
            auto* graphics = GetSubsystem<Graphics>();
            int w = graphics->GetWidth();

            searchBar_ = root->CreateChild<UIElement>("SearchBar");
            searchBar_->SetLayout(LM_HORIZONTAL, 4);
            searchBar_->SetPosition(4, 4);
            searchBar_->SetSize(w - 8, 24);

            Text* label = searchBar_->CreateChild<Text>("SearchLabel");
            label->SetFont(font_, fontSize_);
            label->SetText("Find: ");
            label->SetColor(Color(0.8f, 0.8f, 0.8f));
            label->SetMinWidth(40);

            searchInput_ = searchBar_->CreateChild<LineEdit>("SearchInput");
            searchInput_->SetStyleAuto();
            searchInput_->SetMinHeight(22);
            searchInput_->SetFixedHeight(22);

            searchStatus_ = searchBar_->CreateChild<Text>("SearchStatus");
            searchStatus_->SetFont(font_, fontSize_);
            searchStatus_->SetColor(Color(0.6f, 0.6f, 0.6f));
            searchStatus_->SetMinWidth(80);
        }
        searchBar_->SetVisible(true);
        if (searchInput_)
            searchInput_->SetFocus(true);
    }
    else
    {
        if (searchBar_)
            searchBar_->SetVisible(false);
        ClearSearchHighlights();
        // Return focus to input line
        if (inputLine_)
            inputLine_->SetFocus(true);
    }
    screenDirty_ = true;
}

void ClaudeTerminal::DoSearch(const String& query, bool forward)
{
    searchMatches_.Clear();
    currentMatch_ = -1;
    searchQuery_ = query;

    if (query.Empty())
    {
        if (searchStatus_)
            searchStatus_->SetText("");
        screenDirty_ = true;
        return;
    }

    String lowerQuery = query.ToLower();

    // Search live screen buffer
    for (int r = 0; r < screenRows_; r++)
    {
        // Build row text
        String rowText;
        for (int c = 0; c < screenCols_; c++)
            rowText += screen_[r][c].ch;
        String lowerRow = rowText.ToLower();

        // Find all occurrences in this row
        unsigned pos = 0;
        while (pos < lowerRow.Length())
        {
            unsigned found = lowerRow.Find(lowerQuery, pos);
            if (found == String::NPOS)
                break;
            SearchMatch m;
            m.row = r;
            m.col = (int)found;
            m.len = (int)lowerQuery.Length();
            searchMatches_.Push(m);
            pos = found + 1;
        }
    }

    if (searchMatches_.Size() > 0)
    {
        currentMatch_ = 0;
        if (searchStatus_)
            searchStatus_->SetText("1/" + String(searchMatches_.Size()));
    }
    else
    {
        if (searchStatus_)
            searchStatus_->SetText("0 found");
    }
    screenDirty_ = true;
}

void ClaudeTerminal::ClearSearchHighlights()
{
    searchMatches_.Clear();
    currentMatch_ = -1;
    searchQuery_.Clear();
    if (searchStatus_)
        searchStatus_->SetText("");
}

bool ClaudeTerminal::IsCellSearchMatch(int row, int col) const
{
    for (unsigned i = 0; i < searchMatches_.Size(); i++)
    {
        const SearchMatch& m = searchMatches_[i];
        if (row == m.row && col >= m.col && col < m.col + m.len)
            return true;
    }
    return false;
}
