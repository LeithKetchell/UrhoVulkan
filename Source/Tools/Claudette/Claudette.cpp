// Claudette — Urho3D terminal for Claude Code with IPC injection
//
// Phase 2: Virtual screen buffer + cursor tracking + SGR colors.
// The screen buffer mirrors what Claude Code thinks the terminal looks like.
// Cursor position is tracked to determine when injection is safe.

#include "Claudette.h"

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
#include <Urho3D/Resource/Image.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/JSONFile.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/FontFace.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/ToolTip.h>
#include <Urho3D/UI/UIEvents.h>

#include <SDL/SDL_hints.h>

#ifndef _WIN32
#include <pty.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#if defined(__x86_64__)
#include <sys/ptrace.h>
#include <sys/user.h>
#endif
#endif

URHO3D_DEFINE_APPLICATION_MAIN(Claudette);

static const char* StateToString(ClaudeState s)
{
    switch (s)
    {
    case CLAUDE_BUSY: return "BUSY";
    case CLAUDE_SETTLING: return "SETTLING";
    case CLAUDE_READY: return "READY";
    case CLAUDE_STUCK: return "STUCK";
    }
    return "?";
}

// ============================================================================
// PID file reading — strips \r\n that C++ writers (WorkboardManager) add.
// Urho3D String::Trimmed() only strips space+tab. This strips all whitespace.
// HARDCODED FAIL #1: declared Manager dead because \r in PID file made
// /proc/<pid>\r not exist. Deleted live Manager's PID file and lock.
// Never again.
// ============================================================================
static pid_t ReadPIDFile(Context* context, const String& path)
{
    File f(context, path, FILE_READ);
    if (!f.IsOpen())
        return 0;
    String line = f.ReadLine();
    // Strip ALL trailing whitespace: space, tab, \r, \n
    while (!line.Empty())
    {
        char c = line[line.Length() - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            line.Resize(line.Length() - 1);
        else
            break;
    }
    if (line.Empty())
        return 0;
    return (pid_t)atoi(line.CString());
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

Color Claudette::AnsiToColor(unsigned char idx, bool bold)
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

Color Claudette::CellFgColor(const ScreenCell& cell)
{
    Color fg = cell.hasTrueColorFg ? cell.trueColorFg : AnsiToColor(cell.fg, cell.bold);
    Color bg = cell.hasTrueColorBg ? cell.trueColorBg : AnsiToColor(cell.bg, false);

    // Force contrast when fg ≈ bg — perceived luminance distance
    float fgL = 0.299f * fg.r_ + 0.587f * fg.g_ + 0.114f * fg.b_;
    float bgL = 0.299f * bg.r_ + 0.587f * bg.g_ + 0.114f * bg.b_;
    if (Abs(fgL - bgL) < 0.15f)
        fg = bgL > 0.5f ? Color::BLACK : Color::WHITE;

    return fg;
}

Color Claudette::CellBgColor(const ScreenCell& cell)
{
    if (cell.hasTrueColorBg)
        return cell.trueColorBg;
    return AnsiToColor(cell.bg, false);
}

// ============================================================================
// Lifecycle
// ============================================================================

Claudette::Claudette(Context* context)
    : Application(context)
{
}

Claudette::~Claudette()
{
    Stop();
}

void Claudette::Setup()
{
    // Determine role from command line args for window title
    StringVector arguments = GetArguments();
    String role = "Coder";
    bool isCatch = false;
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
        else if (arguments[i] == "catch")
        {
            isCatch = true;
            if (i + 1 < arguments.Size() && !arguments[i + 1].StartsWith("-"))
                role = arguments[i + 1];
            else
                role = "Catching...";
        }
    }
    // Allow clicks that give the window focus to pass through to the UI
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    engineParameters_[EP_WINDOW_TITLE] = isCatch ? ("Claudette - " + role) : ("Claudette - " + role);

    // Role-specific window icon at startup. Normal startup paths know their role
    // from argv before Engine::Initialize; fall through to generic Claudette only
    // when role is unassigned (auto-detect will update the icon later via
    // ApplyRoleIcon()). All coder* instances share the Coder icon.
    String iconRole = role;
    while (!iconRole.Empty() && iconRole.Back() >= '0' && iconRole.Back() <= '9')
        iconRole.Resize(iconRole.Length() - 1);
    if (isCatch)
        engineParameters_[EP_WINDOW_ICON] = "Icons/Claudette_Catch.png";
    else if (iconRole == "Planner")
        engineParameters_[EP_WINDOW_ICON] = "Icons/Claudette_Planner.png";
    else if (iconRole == "Coder")
        engineParameters_[EP_WINDOW_ICON] = "Icons/Claudette_Coder.png";
    else
        engineParameters_[EP_WINDOW_ICON] = "Icons/Claudette.png";
    engineParameters_[EP_WINDOW_WIDTH] = 1200;
    engineParameters_[EP_WINDOW_HEIGHT] = 1024;
    engineParameters_[EP_FULL_SCREEN] = false;
    engineParameters_[EP_WINDOW_RESIZABLE] = true;
    engineParameters_[EP_LOG_NAME] = "Claudette.log";
    engineParameters_[EP_RESOURCE_PATHS] = "CoreData;Data";
    engineParameters_[EP_SOUND] = false;
    engineParameters_[EP_FRAME_LIMITER] = true;
}

void Claudette::Start()
{
    GetSubsystem<Engine>()->SetMaxFps(30);
    GetSubsystem<Engine>()->SetMaxInactiveFps(5);

    auto* cache = GetSubsystem<ResourceCache>();
    auto* uiStyle = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    auto* ui = GetSubsystem<UI>();
    ui->GetRoot()->SetDefaultStyle(uiStyle);
    ui->SetUseSystemClipboard(true);

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

    // Sweep stale sockets — probe each with connect(). Dead sockets get unlinked.
    // No symlinks in the new model: each Claudette binds directly to {role}.sock.
    {
        StringVector sockEntries;
        fs->ScanDir(sockEntries, ttyDir, "*.sock", SCAN_FILES, false);
        for (const String& entry : sockEntries)
        {
            if (entry == "manager_relay.sock")
                continue;  // Manager owns this
            String sockPath = ttyDir + "/" + entry;
            int probe = socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe >= 0)
            {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);
                if (connect(probe, (struct sockaddr*)&addr, sizeof(addr)) != 0)
                {
                    // Nobody listening — stale socket
                    close(probe);
                    unlink(sockPath.CString());
                    URHO3D_LOGINFO("Removed stale socket (nobody listening): " + sockPath);
                }
                else
                    close(probe);
            }
        }

        // Clean up stale artifacts (.ready files, old spawn_*.pid, etc.)
        StringVector miscEntries;
        fs->ScanDir(miscEntries, ttyDir, "*", SCAN_FILES, false);
        for (const String& entry : miscEntries)
        {
            if (entry.EndsWith(".sock"))
                continue;  // Already handled
            // Remove spawn_*.pid (legacy) and .ready files
            if (entry.StartsWith("spawn_") || entry.EndsWith(".ready"))
            {
                String path = ttyDir + "/" + entry;
                unlink(path.CString());
                URHO3D_LOGINFO("Removed stale artifact: " + path);
            }
        }
    }

    // Bind directly to {role}.sock — no spawn_N indirection, no symlinks
    if (!role_.Empty())
        socketPath_ = ttyDir + "/" + role_.ToLower() + ".sock";
    else
        socketPath_ = ttyDir + "/unassigned_" + String(getpid()) + ".sock";

    if (!StartIPCListener(socketPath_))
        URHO3D_LOGERROR("Failed to start IPC listener on " + socketPath_);
    else
    {
        // Greet any sibling Claudette peers — local IPC health check
        GreetPeers();
    }

    // Claudette wants a Manager — spawn one if nobody's home
    EnsureManagerRunning();

    // Determine role and mode from CLI flags
    StringVector arguments = GetArguments();
    String role;
    for (unsigned i = 0; i < arguments.Size(); i++)
    {
        if (arguments[i] == "--role" && i + 1 < arguments.Size())
            role = arguments[i + 1];
        else if (arguments[i] == "--planner")
            role = "planner";
        else if (arguments[i] == "--coder")
            role = "coder";
        else if (arguments[i] == "catch")
        {
            catchMode_ = true;
            if (i + 1 < arguments.Size() && !arguments[i + 1].StartsWith("-"))
                catchTarget_ = arguments[i + 1];
        }
    }

    if (catchMode_)
    {
        // ── Catch mode: adopt an already-running claude instance ──
        pid_t targetPid = 0;

        if (!catchTarget_.Empty())
        {
            // Specific role requested
            targetPid = FindInstancePID(catchTarget_);
            if (targetPid <= 0)
            {
                URHO3D_LOGERROR("No live instance found for role: " + catchTarget_);
                engine_->Exit();
                return;
            }
        }
        else
        {
            // Auto-detect: find the only running instance (excluding us).
            // Phase 2 cutover: .role scan removed. .pid files are canonical;
            // role is derived from filename, PID is line 1, TTY_ID is line 2
            // (we only need PID here for liveness).
            auto* fsSys = GetSubsystem<FileSystem>();
            String instDir = "/tmp/urho_claude/instances/";
            StringVector pidFiles;
            fsSys->ScanDir(pidFiles, instDir, "*.pid", SCAN_FILES, false);

            Vector<Pair<String, pid_t>> candidates;
            for (const String& entry : pidFiles)
            {
                String roleName = entry;
                roleName.Replace(".pid", "");
                if (roleName == "manager" || roleName.StartsWith("unassigned"))
                    continue;

                pid_t pid = ReadPIDFile(context_, instDir + entry);
                if (pid > 0 && kill(pid, 0) == 0)
                    candidates.Push(MakePair(roleName, pid));
            }

            if (candidates.Empty())
            {
                URHO3D_LOGERROR("No running claude instances found to catch");
                engine_->Exit();
                return;
            }
            if (candidates.Size() > 1)
            {
                String list;
                for (const auto& c : candidates)
                    list += "  " + c.first_ + " (PID " + String((int)c.second_) + ")\n";
                URHO3D_LOGERROR("Multiple instances running — specify which:\n" + list);
                engine_->Exit();
                return;
            }
            catchTarget_ = candidates[0].first_;
            targetPid = candidates[0].second_;
        }

        // Verify it's not already a Claudette
        {
            String commPath = "/proc/" + String((int)targetPid) + "/comm";
            FILE* f = fopen(commPath.CString(), "r");
            if (f)
            {
                char comm[256];
                if (fgets(comm, sizeof(comm), f))
                {
                    String procName(comm);
                    procName = procName.Trimmed();
                    if (procName == "Claudette")
                    {
                        URHO3D_LOGERROR("PID " + String((int)targetPid) + " is already a Claudette");
                        fclose(f);
                        engine_->Exit();
                        return;
                    }
                }
                fclose(f);
            }
        }

        URHO3D_LOGINFO("Catching " + catchTarget_ + " (PID " + String((int)targetPid) + ")...");

        if (!CatchProcess(targetPid))
        {
            URHO3D_LOGERROR("Failed to catch PID " + String((int)targetPid));
            engine_->Exit();
            return;
        }

        // Set role and window title
        String titleRole = catchTarget_;
        titleRole[0] = (char)toupper(titleRole[0]);
        role_ = titleRole;
        GetSubsystem<Graphics>()->SetWindowTitle("Claudette - " + titleRole + " (caught)");
        ApplyRoleIcon();

        // Rebind socket directly to {role}.sock — no symlinks
        String ttyDir = "/tmp/urho_claude/tty";
        String roleSocket = ttyDir + "/" + catchTarget_.ToLower() + ".sock";
        if (roleSocket != socketPath_)
        {
            if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
            for (unsigned ci = 0; ci < clientFds_.Size(); ci++)
                close(clientFds_[ci]);
            clientFds_.Clear();
            unlink(socketPath_.CString());
            socketPath_ = roleSocket;
            if (StartIPCListener(socketPath_))
                URHO3D_LOGINFO("Socket rebound for catch: " + socketPath_);
            else
                URHO3D_LOGERROR("Failed to rebind socket for catch: " + socketPath_);
        }
    }
    else
    {
        // ── Normal mode: spawn a new claude instance ──
        // Role auto-detection is handled by claude_ipc.sh `assume` under flock
        String initPrompt;
        if (!role.Empty())
        {
            String titleRole = role;
            titleRole[0] = (char)toupper(titleRole[0]);
            role_ = titleRole;
            GetSubsystem<Graphics>()->SetWindowTitle("Claudette - " + titleRole);
            ApplyRoleIcon();

            initPrompt = "Follow the Session Startup Protocol: assume the " + role +
                " role by running .claude/hooks/claude_ipc.sh assume " + role +
                " -- then check in with Leith for your assignment. "
                "Use .claude/hooks/safe_build.sh for all builds, never raw make.";
        }
        else
        {
            role_ = "";
            GetSubsystem<Graphics>()->SetWindowTitle("Claudette - starting...");

            initPrompt = "Follow the Session Startup Protocol: run "
                ".claude/hooks/claude_ipc.sh assume auto "
                "to get your role assignment, then check in "
                "with Leith for your assignment. Use .claude/hooks/safe_build.sh for all builds, never raw make.";
        }

        StringVector args;
        args.Push("--dangerously-skip-permissions");
        args.Push(initPrompt);
        if (!SpawnChild("claude", args))
            URHO3D_LOGERROR("Failed to spawn Claude Code");
    }

    claudeState_ = CLAUDE_BUSY;
    lastOutputTimer_.Reset();
    lastCursorMoveTimer_.Reset();

    // Start the PTY reader thread — reads from masterFd_ off the main thread
    // so input remains responsive during heavy Claude output
    if (masterFd_ >= 0)
    {
        ptyThread_ = new PTYReaderThread(this);
        ptyThread_->Run();
        URHO3D_LOGINFO("PTY reader thread started");
    }

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(Claudette, HandleUpdate));
    SubscribeToEvent(E_KEYDOWN, URHO3D_HANDLER(Claudette, HandleKeyDown));
    SubscribeToEvent(E_TEXTINPUT, URHO3D_HANDLER(Claudette, HandleTextInput));
    SubscribeToEvent("ScreenMode", URHO3D_HANDLER(Claudette, HandleResize));
    SubscribeToEvent(E_DROPFILE, URHO3D_HANDLER(Claudette, HandleDropFile));
    SubscribeToEvent(E_MOUSEBUTTONDOWN, URHO3D_HANDLER(Claudette, HandleMouseButtonDown));
    SubscribeToEvent(E_MOUSEBUTTONUP, URHO3D_HANDLER(Claudette, HandleMouseButtonUp));
    SubscribeToEvent(E_MOUSEMOVE, URHO3D_HANDLER(Claudette, HandleMouseMove));
    SubscribeToEvent(E_INPUTFOCUS, URHO3D_HANDLER(Claudette, HandleInputFocus));


    GetSubsystem<Input>()->SetMouseVisible(true);
}

void Claudette::Stop()
{
#ifndef _WIN32
    // Stop the PTY reader thread before closing the fd it reads from
    if (ptyThread_)
    {
        ptyThread_->Stop();  // Sets shouldRun_ = false and waits for join
        delete ptyThread_;
        ptyThread_ = nullptr;
        URHO3D_LOGINFO("PTY reader thread stopped");
    }

    for (int fd : clientFds_)
        if (fd >= 0) close(fd);
    clientFds_.Clear();

    if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
    if (!socketPath_.Empty())
        unlink(socketPath_.CString());
    if (masterFd_ >= 0) { close(masterFd_); masterFd_ = -1; }
    if (childPid_ > 0)
    {
        kill(childPid_, SIGTERM);
        int status;
        waitpid(childPid_, &status, WNOHANG);
        childPid_ = 0;
    }

    // Remove health file on clean exit
    if (!role_.Empty())
    {
        String healthPath = "/tmp/urho_claude/health/" + role_ + ".json";
        unlink(healthPath.CString());
    }
#endif
}

// ============================================================================
// Manager lifecycle — Claudette wants a Manager but doesn't require one
// ============================================================================

void Claudette::EnsureManagerRunning()
{
#ifndef _WIN32
    auto* fs = GetSubsystem<FileSystem>();
    String pidPath = "/tmp/urho_claude/instances/manager.pid";

    // Check if a Manager is already alive
    if (fs->FileExists(pidPath))
    {
        pid_t pid = ReadPIDFile(context_, pidPath);
        if (pid > 0 && kill(pid, 0) == 0)
        {
            URHO3D_LOGINFO("WorkboardManager already running (PID " + String((int)pid) + ")");
            return;
        }
        if (pid > 0)
        {
            // Stale PID — remove it so the new Manager can write its own
            unlink(pidPath.CString());
            URHO3D_LOGINFO("Removed stale manager.pid (PID " + String((int)pid) + " dead)");
        }
    }

    // No Manager running — find the binary next to us
    String binDir = fs->GetProgramDir();
    String managerPath = binDir + "WorkboardManager";
    if (!fs->FileExists(managerPath))
    {
        URHO3D_LOGWARNING("WorkboardManager binary not found at " + managerPath + " — running without Manager");
        return;
    }

    // Spawn as independent process (not a child) via double-fork + setsid
    pid_t first = fork();
    if (first < 0)
    {
        URHO3D_LOGERROR("fork() failed spawning Manager: " + String(strerror(errno)));
        return;
    }
    if (first == 0)
    {
        // First child — detach from parent session
        setsid();
        pid_t second = fork();
        if (second < 0)
            _exit(1);
        if (second > 0)
            _exit(0);  // First child exits — grandchild is fully orphaned

        // Grandchild — this becomes the independent Manager process
        // Close inherited fds to avoid holding Claudette's PTY/sockets open
        for (int fd = 3; fd < 1024; fd++)
            close(fd);

        execl(managerPath.CString(), "WorkboardManager", (char*)nullptr);
        _exit(1);  // exec failed
    }

    // Parent — reap the first child immediately (it exits right away)
    int status;
    waitpid(first, &status, 0);
    URHO3D_LOGINFO("Spawned WorkboardManager as independent process from " + managerPath);
#endif
}

void Claudette::ApplyRoleIcon()
{
    // Role string is capitalised in role_: "Planner", "Coder", "Coder2", ...
    // Strip trailing digits so all coder instances share the Coder icon.
    String base = role_;
    while (!base.Empty() && base.Back() >= '0' && base.Back() <= '9')
        base.Resize(base.Length() - 1);

    String iconName;
    if (base == "Planner")
        iconName = "Icons/Claudette_Planner.png";
    else if (base == "Coder")
        iconName = "Icons/Claudette_Coder.png";
    else
        iconName = "Icons/Claudette_Coder.png";

    auto* cache = GetSubsystem<ResourceCache>();
    auto* graphics = GetSubsystem<Graphics>();
    if (!cache || !graphics)
        return;

    Image* icon = cache->GetResource<Image>(iconName);
    if (!icon)
    {
        URHO3D_LOGWARNING("Role icon not found: " + iconName);
        return;
    }
    graphics->SetWindowIcon(icon);
}

void Claudette::WriteHealthFile()
{
#ifndef _WIN32
    auto* fs = GetSubsystem<FileSystem>();
    String healthDir = "/tmp/urho_claude/health";
    if (!fs->DirExists(healthDir))
        fs->CreateDir(healthDir);

    String roleName = role_.Empty() ? "unknown" : role_;
    String healthPath = healthDir + "/" + roleName + ".json";

    // Build JSON manually — lightweight, no dependency on RapidJSON
    String stateStr = StateToString(claudeState_);
    unsigned lastOutputMs = lastOutputTimer_.GetMSec(false);
    pid_t pid = getpid();

    String json = "{\n"
        "  \"role\": \"" + roleName + "\",\n"
        "  \"state\": \"" + stateStr + "\",\n"
        "  \"pid\": " + String((int)pid) + ",\n"
        "  \"lastOutputMs\": " + String(lastOutputMs) + ",\n"
        "  \"queueSize\": " + String(injectionQueue_.Size()) + ",\n"
        "  \"scrollbackLines\": " + String(scrollback_.Size()) + ",\n"
        "  \"childAlive\": " + String(childPid_ > 0 && IsChildAlive() ? "true" : "false") + "\n"
        "}\n";

    File file(context_, healthPath, FILE_WRITE);
    if (file.IsOpen())
        file.Write(json.CString(), json.Length());
#endif
}

// ============================================================================
// Virtual screen buffer
// ============================================================================

void Claudette::InitScreen(int cols, int rows)
{
    MutexLock lock(bufferMutex_);
    screenCols_ = cols;
    screenRows_ = rows;
    screen_.Resize(rows);
    renderSnapshot_.Resize(rows);
    for (int r = 0; r < rows; r++)
    {
        screen_[r].Resize(cols);
        renderSnapshot_[r].Resize(cols);
    }
    cursorRow_ = 0;
    cursorCol_ = 0;
    rowContainersValid_ = false;  // Force row container rebuild
}

void Claudette::ScreenPutChar(char c)
{
    // Single ASCII character — delegate to string version
    char buf[2] = { c, '\0' };
    ScreenPutString(String(buf));
}

void Claudette::ScreenPutString(const String& ch)
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
    substantiveOutput_ = true;
}

void Claudette::ScreenNewLine()
{
    cursorRow_++;
    if (cursorRow_ >= screenRows_)
    {
        ScreenScrollUp();
        cursorRow_ = screenRows_ - 1;
    }
    substantiveOutput_ = true;
}

void Claudette::ScreenCarriageReturn()
{
    cursorCol_ = 0;
}

void Claudette::ScreenScrollUp()
{
    // Alt screen content is ephemeral — never push to scrollback.
    // Without this guard, Ink redraws cause the same lines to appear
    // in both scrollback and the live screen (duplicate greeting bug).
    if (!altScreenActive_)
        PushScrollback(0);

    // Shift all rows up
    for (int r = 0; r < screenRows_ - 1; r++)
        screen_[r] = screen_[r + 1];

    // Clear bottom row
    screen_[screenRows_ - 1].Resize(screenCols_);
    for (int c = 0; c < screenCols_; c++)
        screen_[screenRows_ - 1][c] = ScreenCell();

    screenDirty_ = true;
    substantiveOutput_ = true;
}

void Claudette::ScreenClearLine(int row, int fromCol, int toCol)
{
    if (row < 0 || row >= screenRows_) return;
    if (fromCol < 0) fromCol = 0;
    if (toCol > screenCols_) toCol = screenCols_;
    for (int c = fromCol; c < toCol; c++)
        screen_[row][c] = ScreenCell();
    screenDirty_ = true;
    substantiveOutput_ = true;
}

void Claudette::ScreenClearScreen(int mode)
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

void Claudette::ScreenSetCursor(int row, int col)
{
    int newRow = Clamp(row, 0, screenRows_ - 1);
    int newCol = Clamp(col, 0, screenCols_ - 1);
    if (newRow != cursorRow_ || newCol != cursorCol_)
    {
        // Flag cursor movement but DON'T treat it as substantive output.
        // Ink redraws reposition the cursor while idle (prompt repaints),
        // which was resetting the state machine to BUSY and blocking IPC
        // message delivery. Cursor move timer is only reset when the move
        // accompanies real content output (characters, clears, scrolls).
        cursorRepositioned_ = true;
    }
    cursorRow_ = newRow;
    cursorCol_ = newCol;
}

String Claudette::GetRowText(int row)
{
    MutexLock lock(bufferMutex_);
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

void Claudette::PushScrollback(int row)
{
    if (row < 0 || row >= screenRows_) return;

    // Find last non-space cell
    int lastNonSpace = -1;
    for (int c = 0; c < screenCols_; c++)
    {
        if (screen_[row][c].ch != " ")
            lastNonSpace = c;
    }
    if (lastNonSpace < 0)
        return;  // Don't store blank lines

    ScrollLine sl;
    sl.cells.Resize(lastNonSpace + 1);
    for (int c = 0; c <= lastNonSpace; c++)
        sl.cells[c] = screen_[row][c];

    scrollback_.Push(sl);
    while (scrollback_.Size() > maxScrollback_)
        scrollback_.Erase(0);
}

// ============================================================================
// ANSI parser — byte-by-byte state machine
// ============================================================================

void Claudette::ProcessByte(unsigned char c)
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

void Claudette::ProcessCSI()
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
    case 'r':  // DECSTBM — Set scroll region (top;bottom)
    {
        int top = (p1 > 0) ? p1 : 1;
        int bottom = (p2 > 0) ? p2 : screenRows_;
        scrollRegionBottom_ = bottom;
        bool pinned = (bottom < screenRows_);
        if (pinned != inkPinnedInput_)
        {
            inkPinnedInput_ = pinned;
            if (pinned)
                URHO3D_LOGWARNING("INK PINNED REGION DETECTED: " + String(screenRows_ - bottom) +
                    " rows pinned — input routed to PTY");
            else
                URHO3D_LOGINFO("INK PINNED REGION CLEARED — input routed to input bar");
        }
        break;
    }
    case 'd':  // VPA — Vertical line position absolute
    {
        ScreenSetCursor(p1 > 0 ? p1 - 1 : 0, cursorCol_);
        break;
    }
    default:
        break;
    }
}

void Claudette::ProcessSGR()
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

int Claudette::TermWcWidth(unsigned cp)
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

bool Claudette::IsBlockChar(const String& ch)
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

void Claudette::RenderBlockChar(UIElement* target, const String& ch, int x, int y, Color fg, Color bg, int poolRow)
{
    if (ch.Length() < 3) return;
    unsigned char b2 = (unsigned char)ch[2];

    // Helper: get a BorderImage from the pool (Phase 3) or create a new child
    auto getImage = [&]() -> BorderImage* {
        if (poolRow >= 0)
            return PoolImage(poolRow);
        return target->CreateChild<BorderImage>();
    };

    int cw = (int)(cellWidthF_ + 0.5f);
    int ch_ = (int)(cellHeightF_ + 0.5f);

    if (b2 == 0x80)  // ▀ upper half
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_ / 2);
        r->SetColor(fg);
    }
    else if (b2 >= 0x81 && b2 <= 0x87)  // Lower N/8 blocks
    {
        int eighths = b2 - 0x80;  // 1-7
        int h = (ch_ * eighths) / 8;
        BorderImage* r = getImage();
        r->SetPosition(x, y + ch_ - h);
        r->SetSize(cw, h);
        r->SetColor(fg);
    }
    else if (b2 == 0x88)  // █ full block
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(fg);
    }
    else if (b2 >= 0x89 && b2 <= 0x8F)  // Left N/8 blocks
    {
        int eighths = 0x90 - b2;  // 7 down to 1
        int w = (cw * eighths) / 8;
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(w, ch_);
        r->SetColor(fg);
    }
    else if (b2 == 0x90)  // ▐ right half
    {
        BorderImage* r = getImage();
        r->SetPosition(x + cw / 2, y);
        r->SetSize(cw - cw / 2, ch_);
        r->SetColor(fg);
    }
    else if (b2 == 0x91)  // ░ light shade
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.25f, fg.g_ * 0.25f, fg.b_ * 0.25f));
    }
    else if (b2 == 0x92)  // ▒ medium shade
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.5f, fg.g_ * 0.5f, fg.b_ * 0.5f));
    }
    else if (b2 == 0x93)  // ▓ dark shade
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(Color(fg.r_ * 0.75f, fg.g_ * 0.75f, fg.b_ * 0.75f));
    }
    else if (b2 == 0x94)  // ▔ upper 1/8
    {
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_ / 8);
        r->SetColor(fg);
    }
    else if (b2 == 0x95)  // ▕ right 1/8
    {
        BorderImage* r = getImage();
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
            BorderImage* r = getImage();
            r->SetPosition(x, y);
            r->SetSize(hw, hh);
            r->SetColor(fg);
        }
        if (ur)
        {
            BorderImage* r = getImage();
            r->SetPosition(x + hw, y);
            r->SetSize(rw, hh);
            r->SetColor(fg);
        }
        if (ll)
        {
            BorderImage* r = getImage();
            r->SetPosition(x, y + hh);
            r->SetSize(hw, rh);
            r->SetColor(fg);
        }
        if (lr)
        {
            BorderImage* r = getImage();
            r->SetPosition(x + hw, y + hh);
            r->SetSize(rw, rh);
            r->SetColor(fg);
        }
    }
    else
    {
        // Unknown block — render as full block
        BorderImage* r = getImage();
        r->SetPosition(x, y);
        r->SetSize(cw, ch_);
        r->SetColor(fg);
    }
}

// ============================================================================
// Rich scrollback line rendering — same visual fidelity as the live screen
// ============================================================================

void Claudette::RenderScrollbackLine(UIElement* container, const Vector<ScreenCell>& cells, int y)
{
    int numCols = (int)cells.Size();
    int c = 0;
    while (c < numCols)
    {
        const ScreenCell& sc = cells[c];
        bool hasBg = sc.bg != 0 || sc.hasTrueColorBg;

        if (sc.ch == " " && !hasBg)
        {
            c++;
            continue;
        }

        // Block characters — geometric rectangles
        if (IsBlockChar(sc.ch))
        {
            if (hasBg)
            {
                BorderImage* bgR = container->CreateChild<BorderImage>();
                bgR->SetPosition((int)(c * cellWidthF_), y);
                bgR->SetSize((int)(cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                bgR->SetColor(CellBgColor(sc));
            }
            RenderBlockChar(container, sc.ch, (int)(c * cellWidthF_), y,
                CellFgColor(sc), CellBgColor(sc));
            c++;
            continue;
        }

        // Background span: consecutive cells with same bg color
        if (hasBg)
        {
            Color bgColor = CellBgColor(sc);
            int startC = c;
            while (c < numCols)
            {
                const ScreenCell& nc = cells[c];
                if (IsBlockChar(nc.ch))
                    break;
                Color ncBg = CellBgColor(nc);
                bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
                if (!ncHasBg || ncBg != bgColor)
                    break;
                c++;
            }

            BorderImage* bgRect = container->CreateChild<BorderImage>();
            bgRect->SetPosition((int)(startC * cellWidthF_), y);
            bgRect->SetSize((int)((c - startC) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
            bgRect->SetColor(bgColor);

            // Foreground text over bg span
            int fc = startC;
            while (fc < c)
            {
                Color fgColor = CellFgColor(cells[fc]);
                int fStart = fc;
                String spanText;
                while (fc < c && CellFgColor(cells[fc]) == fgColor)
                {
                    spanText += cells[fc].ch;
                    fc++;
                }
                if (spanText.Trimmed().Length() > 0)
                {
                    Text* t = container->CreateChild<Text>();
                    t->SetFont(font_, fontSize_);
                    t->SetText(spanText);
                    t->SetColor(fgColor);
                    t->SetPosition((int)(fStart * cellWidthF_), y);
                }
            }
            continue;
        }

        // Foreground-only span
        Color fgColor = CellFgColor(sc);
        int startC = c;
        String spanText;
        while (c < numCols)
        {
            const ScreenCell& nc = cells[c];
            if (IsBlockChar(nc.ch))
                break;
            bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
            if (ncHasBg || CellFgColor(nc) != fgColor)
                break;
            spanText += nc.ch;
            c++;
        }

        while (!spanText.Empty() && spanText[spanText.Length() - 1] == ' ')
            spanText.Resize(spanText.Length() - 1);

        if (!spanText.Empty())
        {
            Text* t = container->CreateChild<Text>();
            t->SetFont(font_, fontSize_);
            t->SetText(spanText);
            t->SetColor(fgColor);
            t->SetPosition((int)(startC * cellWidthF_), y);
        }
    }
}

// ============================================================================
// PTY management
// ============================================================================

bool Claudette::SpawnChild(const String& command, const StringVector& args)
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
        setenv("COLORTERM", "truecolor", 1);
        setenv("FORCE_COLOR", "1", 1);
        unsetenv("CLAUDECODE");

        // Tell the assume command where our IPC socket is
        // so it creates a symlink (coder.sock → our socket) instead of
        // spawning a standalone TTY listener that bypasses us
        setenv("CLAUDETTE_SOCK", socketPath_.CString(), 1);

        // Install a make wrapper that serializes builds via flock.
        // Claude sometimes runs raw make instead of safe_build.sh — this catches it.
        {
            const char* wrapperDir = "/tmp/urho_claude/bin";
            mkdir("/tmp/urho_claude", 0755);
            mkdir(wrapperDir, 0755);
            FILE* f = fopen("/tmp/urho_claude/bin/make", "w");
            if (f)
            {
                fprintf(f,
                    "#!/bin/sh\n"
                    "# Claudette make wrapper — serializes with safe_build.sh via same project lock\n"
                    "mkdir -p /tmp/urho_claude/locks/builds\n"
                    "LOCK=/tmp/urho_claude/locks/builds/project.lock\n"
                    "exec 201>\"$LOCK\"\n"
                    "flock 201\n"
                    "/usr/bin/make \"$@\"\n"
                    "RET=$?\n"
                    "flock -u 201\n"
                    "exit $RET\n");
                fclose(f);
                chmod("/tmp/urho_claude/bin/make", 0755);
            }
            // Prepend wrapper dir to PATH
            const char* oldPath = getenv("PATH");
            String newPath = String(wrapperDir) + ":" + String(oldPath ? oldPath : "/usr/bin");
            setenv("PATH", newPath.CString(), 1);
        }

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

pid_t Claudette::FindInstancePID(const String& role)
{
#ifndef _WIN32
    String instDir = "/tmp/urho_claude/instances/";

    // Try <role>.pid first (direct lookup)
    String pidPath = instDir + role + ".pid";
    if (access(pidPath.CString(), F_OK) == 0)
    {
        pid_t pid = ReadPIDFile(context_, pidPath);
        if (pid > 0 && kill(pid, 0) == 0)
            return pid;
    }

    // Phase 2 cutover: .role fallback removed. The direct .pid lookup above
    // is the canonical path; if it returns nothing, the role isn't live.

    return 0;
#else
    return 0;
#endif
}

bool Claudette::CatchProcess(pid_t targetPid)
{
#if defined(__x86_64__) && !defined(_WIN32)
    // 1. Create a new PTY pair
    int master, slave;
    char slaveName[256];
    struct winsize ws;
    ws.ws_col = (unsigned short)screenCols_;
    ws.ws_row = (unsigned short)screenRows_;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    if (openpty(&master, &slave, slaveName, nullptr, &ws) < 0)
    {
        URHO3D_LOGERROR("openpty() failed: " + String(strerror(errno)));
        return false;
    }
    URHO3D_LOGINFO("Created PTY: master fd " + String(master) + ", slave " + String(slaveName));

    // 2. Attach to target via ptrace
    if (ptrace(PTRACE_ATTACH, targetPid, nullptr, nullptr) < 0)
    {
        URHO3D_LOGERROR("ptrace ATTACH failed (PID " + String((int)targetPid) + "): " + String(strerror(errno))
            + "\nIf permission denied, check: cat /proc/sys/kernel/yama/ptrace_scope (needs 0)");
        close(master);
        close(slave);
        return false;
    }

    int status;
    waitpid(targetPid, &status, 0);
    if (!WIFSTOPPED(status))
    {
        URHO3D_LOGERROR("Target did not stop after PTRACE_ATTACH");
        ptrace(PTRACE_DETACH, targetPid, nullptr, nullptr);
        close(master);
        close(slave);
        return false;
    }

    // 3. Save registers and code at RIP
    struct user_regs_struct savedRegs, regs;
    if (ptrace(PTRACE_GETREGS, targetPid, nullptr, &savedRegs) < 0)
    {
        URHO3D_LOGERROR("PTRACE_GETREGS failed: " + String(strerror(errno)));
        ptrace(PTRACE_DETACH, targetPid, nullptr, nullptr);
        close(master);
        close(slave);
        return false;
    }

    long savedCode = ptrace(PTRACE_PEEKTEXT, targetPid, (void*)savedRegs.rip, nullptr);

    // 4. Write syscall + int3 trap at RIP: 0f 05 cc 90 90 90 90 90
    long syscallTrap = 0x909090909090cc05L;
    syscallTrap = (syscallTrap << 8) | 0x0f;
    // Correct little-endian: bytes at RIP will be 0f 05 cc 90 90 90 90 90
    syscallTrap = 0x90909090cc050fL;
    ptrace(PTRACE_POKETEXT, targetPid, (void*)savedRegs.rip, (void*)syscallTrap);

    // 5. Write slave PTY path below the stack pointer (safe scratch area)
    long stackScratch = (long)savedRegs.rsp - 256;
    int pathLen = strlen(slaveName) + 1;
    for (int i = 0; i < pathLen; i += (int)sizeof(long))
    {
        long word = 0;
        int copyLen = pathLen - i;
        if (copyLen > (int)sizeof(long)) copyLen = (int)sizeof(long);
        memcpy(&word, slaveName + i, copyLen);
        ptrace(PTRACE_POKETEXT, targetPid, (void*)(stackScratch + i), (void*)word);
    }

    // Helper: inject a single syscall and return its result
    bool injectOk = true;
    auto injectSyscall = [&](long sysno, long a1, long a2, long a3) -> long
    {
        if (!injectOk) return -1;
        regs = savedRegs;
        regs.rax = sysno;
        regs.rdi = a1;
        regs.rsi = a2;
        regs.rdx = a3;
        regs.rip = savedRegs.rip;
        ptrace(PTRACE_SETREGS, targetPid, nullptr, &regs);
        ptrace(PTRACE_CONT, targetPid, nullptr, nullptr);
        waitpid(targetPid, &status, 0);
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
        {
            URHO3D_LOGERROR("Unexpected signal after syscall injection: " + String(WSTOPSIG(status)));
            injectOk = false;
            return -1;
        }
        ptrace(PTRACE_GETREGS, targetPid, nullptr, &regs);
        return regs.rax;
    };

    // 6. openat(AT_FDCWD, slavePath, O_RDWR) → newFd
    long newFd = injectSyscall(257 /*SYS_openat*/, -100 /*AT_FDCWD*/, stackScratch, O_RDWR);
    if (newFd < 0)
    {
        URHO3D_LOGERROR("Injected openat() failed: " + String((int)newFd));
        // Restore and bail
        ptrace(PTRACE_POKETEXT, targetPid, (void*)savedRegs.rip, (void*)savedCode);
        ptrace(PTRACE_SETREGS, targetPid, nullptr, &savedRegs);
        ptrace(PTRACE_DETACH, targetPid, nullptr, nullptr);
        close(master);
        close(slave);
        return false;
    }

    // 7. dup2(newFd, 0), dup2(newFd, 1), dup2(newFd, 2)
    injectSyscall(33 /*SYS_dup2*/, newFd, 0, 0);
    injectSyscall(33 /*SYS_dup2*/, newFd, 1, 0);
    injectSyscall(33 /*SYS_dup2*/, newFd, 2, 0);

    // 8. close(newFd) if it wasn't 0, 1, or 2
    if (newFd > 2)
        injectSyscall(3 /*SYS_close*/, newFd, 0, 0);

    // 9. Restore original code and registers
    ptrace(PTRACE_POKETEXT, targetPid, (void*)savedRegs.rip, (void*)savedCode);
    ptrace(PTRACE_SETREGS, targetPid, nullptr, &savedRegs);

    // 10. Detach — target resumes on its new PTY
    ptrace(PTRACE_DETACH, targetPid, nullptr, nullptr);
    URHO3D_LOGINFO("Detached from PID " + String((int)targetPid) + " — fds migrated to " + String(slaveName));

    // 11. Close slave (we only need the master side)
    close(slave);

    // 12. Set master fd non-blocking and adopt the process
    int flags = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    masterFd_ = master;
    childPid_ = targetPid;

    // 13. Send SIGWINCH to force the terminal app to re-query size and redraw
    usleep(50000); // 50ms settle
    ioctl(masterFd_, TIOCSWINSZ, &ws);
    kill(targetPid, SIGWINCH);
    URHO3D_LOGINFO("Caught PID " + String((int)targetPid) + " successfully");
    return injectOk;
#else
    URHO3D_LOGERROR("CatchProcess requires x86_64 Linux");
    (void)targetPid;
    return false;
#endif
}

void Claudette::ReadPTY()
{
#ifndef _WIN32
    if (masterFd_ < 0) return;

    // Per-frame byte budget. The kernel PTY buffer holds the rest; next frame
    // picks it up 33 ms later (at 30 FPS). Without this cap, a fast-streaming
    // Claude response pushes 100 KB+ of bytes through ProcessByte in a single
    // HandleUpdate, saturating the core and starving the engine's frame limiter.
    // 16 KB/frame × 30 FPS = ~480 KB/s — more than enough for any Claude output
    // rate. The display is throttled to 10 Hz anyway, so extra parsing is waste.
    static constexpr unsigned PTY_FRAME_BUDGET = 16384;
    unsigned totalRead = 0;

    char buf[4096];
    bool gotData = false;
    for (;;)
    {
        // Stop draining once we've hit the frame budget. The remaining bytes
        // stay in the kernel PTY buffer and we'll pick them up next frame.
        if (totalRead >= PTY_FRAME_BUDGET)
            break;

        ssize_t n = read(masterFd_, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            gotData = true;
            totalRead += (unsigned)n;
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
            // EIO on PTY master = slave side closed. Child is dead or dying.
            // Close immediately — no point reading a dead PTY.
            if (errno == EIO)
            {
                URHO3D_LOGWARNING("PTY slave closed (EIO) — closing master fd");
                close(masterFd_);
                masterFd_ = -1;
                break;
            }
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

// ============================================================================
// PTY reader thread — reads from master fd off the main thread
// ============================================================================

void PTYReaderThread::ThreadFunction()
{
#ifndef _WIN32
    char buf[4096];
    while (shouldRun_)
    {
        int fd = owner_->masterFd_;
        if (fd < 0)
        {
            usleep(10000);
            continue;
        }

        // Wait for data with 50ms timeout so we can check shouldRun_ periodically
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  // 50ms
        int sel = select(fd + 1, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0)
            continue;  // Timeout or error — loop back to check shouldRun_

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0)
        {
            // Log traffic if enabled (file I/O is outside buffer lock)
            if (owner_->logTraffic_ && !owner_->logPath_.Empty())
            {
                File logFile(owner_->context_, owner_->logPath_, FILE_WRITE);
                if (logFile.IsOpen())
                {
                    logFile.Seek(logFile.GetSize());
                    logFile.Write(buf, (unsigned)n);
                }
            }

            // Parse bytes under the buffer lock.
            // Ink periodically redraws the idle prompt (clear line + rewrite
            // same text).  Individual screen ops set substantiveOutput_, but
            // the VISIBLE screen is unchanged.  Compare a fingerprint of the
            // screen before/after the batch to detect these cosmetic redraws
            // and prevent the state machine from resetting to BUSY.
            {
                MutexLock lock(owner_->bufferMutex_);
                owner_->substantiveOutput_ = false;
                owner_->cursorRepositioned_ = false;

                unsigned preFP = owner_->ScreenFingerprint();
                for (ssize_t i = 0; i < n; i++)
                    owner_->ProcessByte((unsigned char)buf[i]);
                unsigned postFP = owner_->ScreenFingerprint();

                // Screen looks the same → cosmetic Ink redraw, not new content
                if (preFP == postFP)
                    owner_->substantiveOutput_ = false;
            }
            if (owner_->substantiveOutput_)
            {
                owner_->dataReceived_ = true;
                // Only reset cursor move timer when cursor moved as part of
                // real content output — not on cosmetic Ink redraws.
                if (owner_->cursorRepositioned_)
                    owner_->lastCursorMoveTimer_.Reset();
            }
        }
        else if (n == 0)
        {
            // EOF — slave closed
            close(fd);
            owner_->masterFd_ = -1;
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (errno == EIO)
            {
                close(fd);
                owner_->masterFd_ = -1;
                break;
            }
            usleep(10000);
        }
    }
#endif
}

void Claudette::WritePTY(const String& data)
{
#ifndef _WIN32
    if (masterFd_ < 0) return;
    WritePTYRaw(data.CString(), data.Length());
#endif
}

void Claudette::WritePTYRaw(const char* data, unsigned len)
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

bool Claudette::IsChildAlive()
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

bool Claudette::StartIPCListener(const String& socketPath)
{
#ifndef _WIN32
    // Only unlink if nobody is listening (safety net against race)
    {
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0)
        {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socketPath.CString(), sizeof(addr.sun_path) - 1);
            if (connect(probe, (struct sockaddr*)&addr, sizeof(addr)) == 0)
            {
                // Someone is already listening — don't steal their socket
                close(probe);
                URHO3D_LOGERROR("Socket already in use by another process: " + socketPath);
                return false;
            }
            close(probe);
        }
    }
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

void Claudette::PollIPC()
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

void Claudette::CheckSocketHealth()
{
#ifndef _WIN32
    if (socketPath_.Empty())
        return;

    // Only check every 5 seconds
    if (socketHealthTimer_.GetMSec(false) < 5000)
        return;
    socketHealthTimer_.Reset();

    // 1. Check our socket still exists on disk
    if (access(socketPath_.CString(), F_OK) != 0)
    {
        URHO3D_LOGWARNING("IPC socket vanished, re-creating: " + socketPath_);
        if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
        StartIPCListener(socketPath_);
    }

    // 2. If role_ is empty, poll for the role assigned by claude_ipc.sh assume.
    //    Phase 2 cutover: .pid files are canonical. Scan instances/*.pid for
    //    one whose PID (line 1) is our child. Role is derived from filename.
    if (role_.Empty() && childPid_ > 0)
    {
        String instDir = "/tmp/urho_claude/instances/";
        StringVector entries;
        auto* fsSys = GetSubsystem<FileSystem>();
        if (fsSys)
            fsSys->ScanDir(entries, instDir, "*.pid", SCAN_FILES, false);
        for (unsigned i = 0; i < entries.Size(); i++)
        {
            String roleName = entries[i];
            roleName.Replace(".pid", "");
            if (roleName == "manager" || roleName.Empty())
                continue;
            pid_t filePid = ReadPIDFile(context_, instDir + entries[i]);
            if (filePid <= 0)
                continue;
            // Check if this PID is our child (or a descendant — Claude may re-exec)
            bool isOurChild = false;
            pid_t walk = filePid;
            for (int depth = 0; depth < 8 && walk > 1; depth++)
            {
                if (walk == childPid_ || walk == (pid_t)getpid())
                {
                    isOurChild = true;
                    break;
                }
                // Read PPID from /proc
                String statPath = "/proc/" + String((int)walk) + "/stat";
                FILE* f = fopen(statPath.CString(), "r");
                if (!f) break;
                int pid_read, ppid_read;
                char comm[256], state;
                if (fscanf(f, "%d %255s %c %d", &pid_read, comm, &state, &ppid_read) == 4)
                    walk = (pid_t)ppid_read;
                else
                    walk = 0;
                fclose(f);
            }
            if (isOurChild && roleName != "unassigned")
            {
                String titleRole = roleName;
                titleRole[0] = (char)toupper(titleRole[0]);
                // Rebind socket from unassigned_PID.sock to {role}.sock
                String ttyDir2 = "/tmp/urho_claude/tty";
                String newSockPath = ttyDir2 + "/" + roleName + ".sock";
                if (newSockPath != socketPath_)
                {
                    if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
                    for (unsigned ci = 0; ci < clientFds_.Size(); ci++)
                        close(clientFds_[ci]);
                    clientFds_.Clear();
                    unlink(socketPath_.CString());
                    socketPath_ = newSockPath;
                    StartIPCListener(socketPath_);
                }
                role_ = titleRole;
                GetSubsystem<Graphics>()->SetWindowTitle("Claudette - " + titleRole);
                ApplyRoleIcon();
                URHO3D_LOGINFO("Role resolved from shell: " + titleRole + " → socket rebound to " + socketPath_);
                break;
            }
        }
    }
#endif
}

// ============================================================================
// Peer greeting — IPC health check on startup
// ============================================================================
//
// Scan *.sock in tty dir, connect to each peer, send hello. Peers ack back.

void Claudette::GreetPeers()
{
#ifndef _WIN32
    if (socketPath_.Empty())
        return;

    String ttyDir = "/tmp/urho_claude/tty";
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs)
        return;

    String myRole = role_.Empty() ? String("unassigned") : role_;
    unsigned slash = socketPath_.FindLast('/');
    String mySockName = (slash != String::NPOS) ? socketPath_.Substring(slash + 1) : socketPath_;
    String helloMsg = "__HELLO__:" + myRole + ":" + String((int)getpid()) + ":" + mySockName + "\n";

    StringVector sockEntries;
    fs->ScanDir(sockEntries, ttyDir, "*.sock", SCAN_FILES, false);

    int sent = 0;
    int failed = 0;

    for (const String& entry : sockEntries)
    {
        // Skip ourselves and Manager
        if (entry == mySockName || entry == "manager_relay.sock")
            continue;

        String peerSockPath = ttyDir + "/" + entry;

        int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sockFd < 0) { failed++; continue; }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, peerSockPath.CString(), sizeof(addr.sun_path) - 1);

        if (connect(sockFd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        {
            close(sockFd);
            failed++;
            continue;
        }

        ssize_t w = write(sockFd, helloMsg.CString(), helloMsg.Length());
        close(sockFd);

        if (w <= 0) { failed++; continue; }

        peerAckMap_[entry] = false;
        sent++;
        URHO3D_LOGINFO("Greeted peer: " + entry);
    }

    if (sent == 0 && failed == 0)
    {
        URHO3D_LOGINFO("Peer greeting: no other peers found (running solo)");
        peerGreetingDone_ = true;
    }
    else
    {
        URHO3D_LOGINFO("Peer greeting: sent=" + String(sent) +
                       " failed=" + String(failed) +
                       " — awaiting acks (timeout " + String((int)peerGreetingTimeoutMs_) + "ms)");
        peerGreetingTimer_.Reset();
        peerGreetingDone_ = false;
    }
#endif
}

void Claudette::SendPeerAck(const String& senderSockName)
{
#ifndef _WIN32
    if (socketPath_.Empty() || senderSockName.Empty())
        return;

    String senderSockPath = "/tmp/urho_claude/tty/" + senderSockName;
    if (access(senderSockPath.CString(), F_OK) != 0)
    {
        URHO3D_LOGWARNING("Cannot ack peer — sender socket missing: " + senderSockPath);
        return;
    }

    unsigned slash = socketPath_.FindLast('/');
    String mySockName = (slash != String::NPOS) ? socketPath_.Substring(slash + 1) : socketPath_;
    String myRole = role_.Empty() ? String("unassigned") : role_;
    String ackMsg = "__HELLO_ACK__:" + myRole + ":" + String((int)getpid()) + ":" + mySockName + "\n";

    int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd < 0)
        return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, senderSockPath.CString(), sizeof(addr.sun_path) - 1);

    if (connect(sockFd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
    {
        ssize_t w = write(sockFd, ackMsg.CString(), ackMsg.Length());
        if (w > 0)
            URHO3D_LOGINFO("Sent ack to peer: " + senderSockName);
        else
            URHO3D_LOGWARNING("Failed to write ack to peer: " + senderSockName);
    }
    else
    {
        URHO3D_LOGWARNING("Failed to ack peer: " + senderSockPath +
                          " (" + String(strerror(errno)) + ")");
    }
    close(sockFd);
#endif
}

void Claudette::CheckPeerGreetingTimeouts()
{
    if (peerGreetingDone_ || peerAckMap_.Empty())
        return;
    if (peerGreetingTimer_.GetMSec(false) < peerGreetingTimeoutMs_)
        return;

    int acked = 0;
    int missing = 0;
    StringVector missingPeers;
    for (auto it = peerAckMap_.Begin(); it != peerAckMap_.End(); ++it)
    {
        if (it->second_)
            acked++;
        else
        {
            missing++;
            missingPeers.Push(it->first_);
        }
    }

    if (missing == 0)
    {
        URHO3D_LOGINFO("Peer greeting: all " + String(acked) + " peers acked — local IPC healthy");
    }
    else
    {
        String missingList;
        for (unsigned i = 0; i < missingPeers.Size(); i++)
        {
            if (i > 0) missingList += ", ";
            missingList += missingPeers[i];
        }
        URHO3D_LOGWARNING("Peer greeting: " + String(acked) + " acked, " +
                          String(missing) + " unresponsive: " + missingList);
    }
    peerGreetingDone_ = true;
}

void Claudette::HandleIPCMessage(const String& message)
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

    // ── Protocol messages — handled internally, never forwarded to PTY ──
    // __HELLO__:role:pid:sockname  → reply with __HELLO_ACK__
    // __HELLO_ACK__:role:pid:sockname  → mark peer as alive
    if (clean.StartsWith("__HELLO__:"))
    {
        String body = clean.Substring(10);
        URHO3D_LOGINFO("Peer hello received: " + body);
        Vector<String> fields = body.Split(':');
        if (fields.Size() >= 3)
            SendPeerAck(fields[2]);
        return;
    }
    if (clean.StartsWith("__HELLO_ACK__:"))
    {
        String body = clean.Substring(14);
        URHO3D_LOGINFO("Peer ack received: " + body);
        Vector<String> fields = body.Split(':');
        if (fields.Size() >= 3)
        {
            const String& peerSockName = fields[2];
            if (peerAckMap_.Contains(peerSockName))
                peerAckMap_[peerSockName] = true;
        }
        return;
    }

    // __STATE__:role:STATE — peer state change broadcast from Manager
    if (clean.StartsWith("__STATE__:"))
    {
        String body = clean.Substring(10);
        Vector<String> fields = body.Split(':');
        if (fields.Size() >= 2)
        {
            const String& peerRole = fields[0];
            const String& peerState = fields[1];
            peerStates_[peerRole] = peerState;
            URHO3D_LOGINFOF("Peer state: %s -> %s", peerRole.CString(), peerState.CString());
        }
        return;  // Protocol message — do NOT inject into PTY
    }

    // __ROLE__:newrole — role assignment from claude_ipc.sh assume.
    // Rebind our socket from unassigned_PID.sock (or old role) to {newrole}.sock.
    if (clean.StartsWith("__ROLE__:"))
    {
        String newRole = clean.Substring(9).Trimmed();
        if (!newRole.Empty() && newRole.ToLower() != role_.ToLower())
        {
            String ttyDir = "/tmp/urho_claude/tty";
            String newSockPath = ttyDir + "/" + newRole.ToLower() + ".sock";
            // Close old listener and clean up old socket file
            if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
            for (unsigned i = 0; i < clientFds_.Size(); i++)
                close(clientFds_[i]);
            clientFds_.Clear();
            unlink(socketPath_.CString());
            // Bind to new role socket
            socketPath_ = newSockPath;
            if (StartIPCListener(socketPath_))
            {
                role_ = newRole;
                role_[0] = (char)toupper(role_[0]);
                GetSubsystem<Graphics>()->SetWindowTitle("Claudette - " + role_);
                ApplyRoleIcon();
                URHO3D_LOGINFO("Role socket rebound: " + socketPath_);
            }
            else
                URHO3D_LOGERROR("Failed to rebind socket for role: " + newRole);
        }
        return;
    }

    URHO3D_LOGINFO("IPC queued (waiting for CLAUDE_READY): " + clean);

    // Queue — drain only when claude is READY to receive input
    if (injectionQueue_.Empty())
        injectionQueueTimer_.Reset();
    injectionQueue_.Push(clean);
}

// ============================================================================
// State machine
// ============================================================================

void Claudette::UpdateState()
{
    unsigned elapsed = lastOutputTimer_.GetMSec(false);
    unsigned cursorIdle = lastCursorMoveTimer_.GetMSec(false);

    switch (claudeState_)
    {
    case CLAUDE_BUSY:
        if (elapsed >= stuckThresholdMs_)
        {
            claudeState_ = CLAUDE_STUCK;
            URHO3D_LOGWARNING("Instance stuck — no PTY output for " + String(stuckThresholdMs_ / 1000) + "s while BUSY");
        }
        else if (elapsed >= busyToSettleMs_)
            claudeState_ = CLAUDE_SETTLING;
        break;
    case CLAUDE_SETTLING:
        // Require BOTH output silence AND cursor stability
        if (elapsed >= stuckThresholdMs_)
        {
            claudeState_ = CLAUDE_STUCK;
            URHO3D_LOGWARNING("Instance stuck — no PTY output for " + String(stuckThresholdMs_ / 1000) + "s while SETTLING");
        }
        else if (elapsed >= settleThresholdMs_ && cursorIdle >= settleThresholdMs_)
            claudeState_ = CLAUDE_READY;
        break;
    case CLAUDE_READY:
        break;
    case CLAUDE_STUCK:
        // Stay stuck until new output arrives (dataReceived_ resets to CLAUDE_BUSY)
        break;
    }
}

unsigned Claudette::ScreenFingerprint() const
{
    // Lightweight FNV-1a hash of visible screen characters and cursor position.
    // Called under bufferMutex_ by the reader thread before/after processing a
    // batch of PTY bytes.  If the hash is identical, the batch was a cosmetic
    // Ink redraw (clear line + rewrite same prompt text) rather than new output
    // from claude — so the state machine should NOT reset to BUSY.
    unsigned hash = 2166136261u;
    for (int r = 0; r < screenRows_; r++)
    {
        const auto& cells = screen_[r];
        for (int c = 0; c < screenCols_; c++)
        {
            const String& ch = cells[c].ch;
            for (unsigned i = 0; i < ch.Length(); i++)
                hash = (hash ^ (unsigned char)ch[i]) * 16777619u;
        }
    }
    hash = (hash ^ (unsigned)cursorRow_) * 16777619u;
    hash = (hash ^ (unsigned)cursorCol_) * 16777619u;
    return hash;
}

void Claudette::BroadcastStateToManager(ClaudeState state)
{
#ifndef _WIN32
    // Send state change to Manager relay socket for broadcast to peers.
    // Format: "__BROADCAST_STATE__:role:STATE" sent to manager.sock
    // Manager rebroadcasts to all other instances.
    String relayPath = "/tmp/urho_claude/tty/manager.sock";
    struct stat st;
    if (stat(relayPath.CString(), &st) != 0 || !S_ISSOCK(st.st_mode))
        return;  // No Manager running — silent

    String myRole = role_.Empty() ? String("unassigned") : role_;
    // Relay format is "target:message" — use "__ALL__" as broadcast target
    String payload = "__ALL__:__STATE__:" + myRole + ":" + String(StateToString(state)) + "\n";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, relayPath.CString(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
        (void)write(fd, payload.CString(), payload.Length());
    close(fd);
#endif
}

void Claudette::DrainInjectionQueue()
{
    if (injectionQueue_.Empty())
        return;

    // Gate on CLAUDE_READY so messages land when claude is actually listening.
    // 30s timeout prevents stalling if claude never settles (e.g. long tool run).
    bool timedOut = !injectionQueue_.Empty() && injectionQueueTimer_.GetMSec(false) > 3000;
    if (claudeState_ != CLAUDE_READY && !timedOut)
        return;

    // Save user's partial input before injecting IPC messages
    String savedInput;
    unsigned savedCursorPos = 0;
    if (inputLine_)
    {
        savedInput = inputLine_->GetText();
        savedCursorPos = inputLine_->GetCursorPosition();
    }

    while (!injectionQueue_.Empty())
    {
        String msg = injectionQueue_[0];
        injectionQueue_.Erase(0);
        URHO3D_LOGINFO("Queue → PTY (READY): " + msg);
        if (inputLine_) inputLine_->SetText(msg);
        WritePTY(msg + "\r");
    }

    // Restore user's partial input after injection
    if (inputLine_)
    {
        if (!savedInput.Empty())
        {
            inputLine_->SetText(savedInput);
            inputLine_->SetCursorPosition(savedCursorPos);
        }
        else
        {
            inputLine_->SetText("");
        }
    }
}

// ============================================================================
// Display — render screen buffer to scrollable terminal view
// ============================================================================

unsigned Claudette::HashRow(int row) const
{
    unsigned hash = 2166136261u;  // FNV-1a offset basis
    const auto& cells = renderSnapshot_[row];
    for (int c = 0; c < screenCols_; c++)
    {
        const ScreenCell& sc = cells[c];
        // Hash character bytes
        for (unsigned i = 0; i < sc.ch.Length(); i++)
            hash = (hash ^ (unsigned char)sc.ch[i]) * 16777619u;
        hash = (hash ^ sc.fg) * 16777619u;
        hash = (hash ^ sc.bg) * 16777619u;
        unsigned flags = (sc.bold ? 1u : 0u) | (sc.dim ? 2u : 0u) |
                         (sc.underline ? 4u : 0u) |
                         (sc.hasTrueColorFg ? 8u : 0u) | (sc.hasTrueColorBg ? 16u : 0u);
        hash = (hash ^ flags) * 16777619u;
        if (sc.hasTrueColorFg)
        {
            unsigned rgb = ((unsigned)(sc.trueColorFg.r_ * 255.0f) << 16) |
                           ((unsigned)(sc.trueColorFg.g_ * 255.0f) << 8) |
                            (unsigned)(sc.trueColorFg.b_ * 255.0f);
            hash = (hash ^ rgb) * 16777619u;
        }
        if (sc.hasTrueColorBg)
        {
            unsigned rgb = ((unsigned)(sc.trueColorBg.r_ * 255.0f) << 16) |
                           ((unsigned)(sc.trueColorBg.g_ * 255.0f) << 8) |
                            (unsigned)(sc.trueColorBg.b_ * 255.0f);
            hash = (hash ^ rgb) * 16777619u;
        }
    }
    // Include search match state so search highlights trigger row rebuilds
    for (unsigned m = 0; m < searchMatches_.Size(); m++)
    {
        if (searchMatches_[m].row == row)
        {
            hash = (hash ^ (unsigned)(searchMatches_[m].col + 9999)) * 16777619u;
            hash = (hash ^ (unsigned)(searchMatches_[m].len + 7777)) * 16777619u;
        }
    }
    return hash;
}

// ── Phase 3: Element pool helpers ──

void Claudette::RowElementPool::HideUnused()
{
    for (unsigned i = nextText; i < texts.Size(); i++)
        texts[i]->SetVisible(false);
    for (unsigned i = nextImage; i < images.Size(); i++)
        images[i]->SetVisible(false);
}

Text* Claudette::PoolText(int row)
{
    RowElementPool& pool = rowPools_[row];
    if (pool.nextText < pool.texts.Size())
    {
        Text* t = pool.texts[pool.nextText++];
        t->SetVisible(true);
        return t;
    }
    // Create new — font is set here so callers don't repeat it
    Text* t = rowContainers_[row]->CreateChild<Text>();
    t->SetFont(font_, fontSize_);
    t->SetPriority(1);  // Text draws on top of background images
    t->SetEnabled(false);  // Don't intercept mouse events — selection must pass through
    pool.texts.Push(SharedPtr<Text>(t));
    pool.nextText++;
    return t;
}

BorderImage* Claudette::PoolImage(int row)
{
    RowElementPool& pool = rowPools_[row];
    if (pool.nextImage < pool.images.Size())
    {
        BorderImage* img = pool.images[pool.nextImage++];
        img->SetVisible(true);
        return img;
    }
    BorderImage* img = rowContainers_[row]->CreateChild<BorderImage>();
    img->SetEnabled(false);  // Don't intercept mouse events — selection must pass through
    pool.images.Push(SharedPtr<BorderImage>(img));
    pool.nextImage++;
    return img;
}

void Claudette::EnsureRowContainers()
{
    if (rowContainersValid_ && (int)rowContainers_.Size() == screenRows_)
        return;

    // Tear down old containers
    if (screenPanel_)
        screenPanel_->RemoveAllChildren();

    rowContainers_.Resize(screenRows_);
    rowHashes_.Resize(screenRows_);
    rowPools_.Resize(screenRows_);
    for (int r = 0; r < screenRows_; r++)
    {
        UIElement* rc = screenPanel_->CreateChild<UIElement>();
        rc->SetPosition(0, (int)(r * cellHeightF_));
        rc->SetSize(screenPanel_->GetWidth(), (int)(cellHeightF_ + 0.5f));
        rc->SetSortChildren(true);
        rowContainers_[r] = rc;
        rowHashes_[r] = 0;  // Force rebuild on first render
        rowPools_[r] = RowElementPool();  // Fresh pool
    }
    rowContainersValid_ = true;
}

void Claudette::RebuildRow(int row)
{
    // Phase 3: reuse pooled elements instead of RemoveAllChildren + CreateChild
    RowElementPool& pool = rowPools_[row];
    pool.Reset();

    // Phase 2: read from renderSnapshot_ (taken under brief lock in RenderScreen)
    const auto& snapRow = renderSnapshot_[row];

    // Walk cells, build foreground spans and background spans
    int c = 0;
    while (c < screenCols_)
    {
        const ScreenCell& sc = snapRow[c];
        bool hasBg = sc.bg != 0 || sc.hasTrueColorBg;
        bool searchHit = IsCellSearchMatch(row, c);

        // Skip empty cells with no special highlighting
        if (sc.ch == " " && !hasBg && !searchHit)
        {
            c++;
            continue;
        }

        // ── Block characters — render as geometric rectangles ──
        if (IsBlockChar(sc.ch))
        {
            if (searchHit)
            {
                BorderImage* hlRect = PoolImage(row);
                hlRect->SetPosition((int)(c * cellWidthF_), 0);
                hlRect->SetSize((int)(cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                hlRect->SetColor(Color(0.6f, 0.5f, 0.1f, 0.7f));
            }
            else if (hasBg)
            {
                BorderImage* bgR = PoolImage(row);
                bgR->SetPosition((int)(c * cellWidthF_), 0);
                bgR->SetSize((int)(cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
                bgR->SetColor(CellBgColor(sc));
            }
            RenderBlockChar(rowContainers_[row], sc.ch, (int)(c * cellWidthF_), 0,
                CellFgColor(sc), CellBgColor(sc), row);
            c++;
            continue;
        }

        // ── Background span: consecutive cells with same bg color ──
        if (hasBg && !searchHit)
        {
            Color bgColor = CellBgColor(sc);
            int startC = c;
            while (c < screenCols_)
            {
                const ScreenCell& nc = snapRow[c];
                if (IsCellSearchMatch(row, c))
                    break;
                if (IsBlockChar(nc.ch))
                    break;
                Color ncBg = CellBgColor(nc);
                bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
                if (!ncHasBg || ncBg != bgColor)
                    break;
                c++;
            }

            BorderImage* bgRect = PoolImage(row);
            bgRect->SetPosition((int)(startC * cellWidthF_), 0);
            bgRect->SetSize((int)((c - startC) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
            bgRect->SetColor(bgColor);

            // Render foreground text over this bg span
            int fc = startC;
            while (fc < c)
            {
                Color fgColor = CellFgColor(snapRow[fc]);
                int fStart = fc;
                String spanText;
                while (fc < c && CellFgColor(snapRow[fc]) == fgColor)
                {
                    spanText += snapRow[fc].ch;
                    fc++;
                }
                if (spanText.Trimmed().Length() > 0)
                {
                    Text* t = PoolText(row);
                    t->SetText(spanText);
                    t->SetColor(fgColor);
                    t->SetPosition((int)(fStart * cellWidthF_), 0);
                }
            }
            continue;
        }

        // ── Foreground-only span ──
        Color fgColor = CellFgColor(sc);
        int startC = c;
        String spanText;
        while (c < screenCols_)
        {
            const ScreenCell& nc = snapRow[c];
            if (IsBlockChar(nc.ch))
                break;
            bool ncHasBg = nc.bg != 0 || nc.hasTrueColorBg;
            bool ncSearch = IsCellSearchMatch(row, c);
            if (ncHasBg || ncSearch != searchHit ||
                CellFgColor(nc) != fgColor)
                break;
            spanText += nc.ch;
            c++;
        }

        // Draw span-wide search highlight
        if (searchHit)
        {
            BorderImage* hlRect = PoolImage(row);
            hlRect->SetPosition((int)(startC * cellWidthF_), 0);
            hlRect->SetSize((int)((c - startC) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
            hlRect->SetColor(Color(0.6f, 0.5f, 0.1f, 0.7f));
        }

        // Trim trailing spaces from fg-only spans
        while (!spanText.Empty() && spanText[spanText.Length() - 1] == ' ')
            spanText.Resize(spanText.Length() - 1);

        if (!spanText.Empty())
        {
            Text* t = PoolText(row);
            t->SetText(spanText);
            t->SetColor(fgColor);
            t->SetPosition((int)(startC * cellWidthF_), 0);
        }
    }

    // Hide any pool elements not used this frame
    pool.HideUnused();
}

void Claudette::RenderScreen()
{
    if (!screenDirty_ || !screenPanel_) return;

    // ── Throttle during BUSY bursts only ──
    if (claudeState_ == CLAUDE_BUSY &&
        displayTimer_.GetMSec(false) < displayIntervalMs_)
    {
        return;  // leave screenDirty_ set; we'll render next eligible frame
    }
    displayTimer_.Reset();

    // ── Phase 2: Snapshot under brief lock, render without lock ──
    // The reader thread is only blocked during this copy (~sub-ms for 120×40).
    // All UI element creation happens after the lock is released.
    {
        MutexLock lock(bufferMutex_);
        renderSnapshot_ = screen_;
        // Phase 4: snapshot new scrollback lines into pending queue
        if (lastSnapshotScrollback_ < scrollback_.Size())
        {
            for (unsigned i = lastSnapshotScrollback_; i < scrollback_.Size(); i++)
                pendingScrollback_.Push(scrollback_[i]);
            lastSnapshotScrollback_ = scrollback_.Size();
        }
        screenDirty_ = false;
    }
    // ── Lock released — reader thread can proceed ──

    // ── Phase 4: Batched scrollback rendering ──
    // Render at most maxScrollbackPerFrame_ lines per frame to keep frames short.
    if (scrollbackPanel_)
    {
        unsigned toRender = Min((unsigned)pendingScrollback_.Size(), maxScrollbackPerFrame_);
        bool renderedAny = toRender > 0;

        for (unsigned i = 0; i < toRender; i++)
        {
            UIElement* lineContainer = scrollbackPanel_->CreateChild<UIElement>();
            lineContainer->SetPosition(0, (int)(scrollbackUICount_ * cellHeightF_));
            lineContainer->SetSize(scrollbackPanel_->GetWidth(), (int)(cellHeightF_ + 0.5f));
            RenderScrollbackLine(lineContainer, pendingScrollback_[i].cells, 0);
            scrollbackUICount_++;
        }

        // Remove rendered lines from front of queue
        if (toRender > 0)
            pendingScrollback_.Erase(0, toRender);

        // If more pending, stay dirty so we come back next frame
        if (!pendingScrollback_.Empty())
            screenDirty_ = true;

        // Trim oldest scrollback UI if over limit — batch removals, reposition once
        if (scrollbackUICount_ > maxScrollback_)
        {
            unsigned trimCount = scrollbackUICount_ - maxScrollback_;
            for (unsigned t = 0; t < trimCount; t++)
                scrollbackPanel_->RemoveChildAtIndex(0);
            scrollbackUICount_ -= trimCount;
            // Reposition all remaining children once
            for (unsigned j = 0; j < scrollbackPanel_->GetNumChildren(); j++)
                scrollbackPanel_->GetChild(j)->SetPosition(0, (int)(j * cellHeightF_));
        }

        // Update scrollback panel height
        int sbHeight = (int)(scrollbackUICount_ * cellHeightF_);
        scrollbackPanel_->SetSize(scrollbackPanel_->GetWidth(), sbHeight);

        // Reposition live screen below scrollback
        int screenHeight = screenRows_ * charHeight_;
        if (screenBg_)
        {
            screenBg_->SetPosition(0, sbHeight);
            screenBg_->SetSize(scrollContent_->GetWidth(), screenHeight);
            screenPanel_->SetSize(scrollContent_->GetWidth(), screenHeight);
            if (selectionOverlay_)
                selectionOverlay_->SetSize(scrollContent_->GetWidth(), screenHeight);
        }

        // Resize content to fit scrollback + screen
        int totalHeight = sbHeight + screenHeight;
        if (scrollContent_)
            scrollContent_->SetSize(scrollContent_->GetWidth(), totalHeight);

        // Auto-scroll to bottom when new content arrives (unless user scrolled up)
        if (renderedAny && autoScroll_ && scrollView_)
        {
            int viewHeight = scrollView_->GetHeight();
            int maxScroll = totalHeight - viewHeight;
            if (maxScroll > 0)
                scrollView_->SetViewPosition(IntVector2(0, maxScroll));
        }
    }

    // ── Live screen grid — per-row dirty tracking ──
    // HashRow and RebuildRow read from renderSnapshot_ (no lock needed).
    EnsureRowContainers();

    int rowsRebuilt = 0;
    for (int r = 0; r < screenRows_; r++)
    {
        unsigned hash = HashRow(r);
        if (hash != rowHashes_[r])
        {
            RebuildRow(r);
            rowHashes_[r] = hash;
            rowsRebuilt++;
        }
    }

    RenderSelectionOverlay();
}

// ============================================================================
// UI
// ============================================================================

void Claudette::CreateUI()
{
    auto* root = GetSubsystem<UI>()->GetRoot();

    // ── Status bar (top) ──
    statusBar_ = root->CreateChild<Text>("StatusBar");
    statusBar_->SetFont(font_, fontSize_);
    statusBar_->SetText("Initializing...");
    statusBar_->SetColor(Color(0.6f, 0.8f, 0.6f));

    // ── Input bar (bottom) — MultiLineEdit inside ScrollView ──
    int inputFontSize = 16;
    inputScrollView_ = root->CreateChild<ScrollView>("InputScrollView");
    inputScrollView_->SetStyleAuto();
    inputScrollView_->SetScrollBarsVisible(false, true);

    inputLine_ = new MultiLineEdit(context_);
    inputLine_->SetName("InputLine");
    inputLine_->SetStyleAuto();
    inputLine_->SetClipChildren(false);  // ScrollView handles clipping
    inputLine_->SetFocusMode(FM_FOCUSABLE_DEFOCUSABLE);
    inputLine_->SetWordWrap(true);
    if (inputLine_->GetTextElement())
    {
        inputLine_->GetTextElement()->SetFont(font_, inputFontSize);
        inputLine_->GetTextElement()->SetColor(Color(0.9f, 1.0f, 0.9f));
    }
    inputLine_->SetColor(Color(0.12f, 0.13f, 0.16f));
    inputScrollView_->SetContentElement(inputLine_);
    inputLine_->SetFocus(true);

    // ── Scrollable terminal area (scrollback + live screen) ──
    scrollView_ = root->CreateChild<ScrollView>("TermScrollView");
    scrollView_->SetStyleAuto();
    scrollView_->SetScrollBarsVisible(false, true);
    scrollView_->SetFocusMode(FM_NOTFOCUSABLE);
    scrollView_->SetScrollStep(0.02f);

    // Content element — sized to fit scrollback + live screen
    scrollContent_ = new UIElement(context_);
    scrollView_->SetContentElement(scrollContent_);

    // Scrollback panel — Text items stack here vertically
    scrollbackPanel_ = scrollContent_->CreateChild<UIElement>("ScrollbackPanel");
    scrollbackPanel_->SetPosition(0, 0);

    // ── Live screen panel (grid-rendered) — dark terminal background ──
    screenBg_ = scrollContent_->CreateChild<BorderImage>("ScreenBg");
    screenBg_->SetPosition(0, 0);
    screenBg_->SetColor(Color(0.035f, 0.038f, 0.047f));  // #090A0C — Melbourne night sky

    screenPanel_ = screenBg_->CreateChild<UIElement>("ScreenPanel");
    screenPanel_->SetPosition(0, 0);
    screenPanel_->SetClipChildren(true);

    // Selection overlay — lightweight layer on top of screen text for drag highlights
    selectionOverlay_ = screenBg_->CreateChild<UIElement>("SelectionOverlay");
    selectionOverlay_->SetPosition(0, 0);
    selectionOverlay_->SetClipChildren(true);

    // ── Slash command dropdown (hidden until '/' typed) ──
    InitSlashCommands();

    slashMenu_ = root->CreateChild<Window>("SlashMenu");
    slashMenu_->SetStyleAuto();
    slashMenu_->SetColor(Color(0.15f, 0.15f, 0.18f));
    slashMenu_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));
    slashMenu_->SetVisible(false);
    slashMenu_->SetFocusMode(FM_NOTFOCUSABLE);
    slashMenu_->SetBringToFront(true);
    slashMenu_->SetPriority(1000);

    // ── Right-click context menu ──
    contextMenu_ = root->CreateChild<Window>("ContextMenu");
    contextMenu_->SetStyleAuto();
    contextMenu_->SetColor(Color(0.18f, 0.18f, 0.22f));
    contextMenu_->SetLayout(LM_VERTICAL, 1, IntRect(6, 6, 6, 6));
    contextMenu_->SetVisible(false);
    contextMenu_->SetFocusMode(FM_NOTFOCUSABLE);
    contextMenu_->SetBringToFront(true);
    contextMenu_->SetPriority(2000);

    // ── Tooltips on non-obvious elements ──
    auto addTip = [this](UIElement* parent, const String& text)
    {
        auto* tip = parent->CreateChild<ToolTip>();
        tip->SetDelay(0.5f);
        auto* label = tip->CreateChild<Text>();
        label->SetFont(font_, fontSize_);
        label->SetText(text);
        label->SetColor(Color(0.9f, 0.9f, 0.8f));
    };

    addTip(statusBar_, "Role, version, cursor position, state. Right-click for options.");

    // Apply initial layout from normalized coordinates
    ApplyLayout();
}

void Claudette::ApplyLayout()
{
    auto* graphics = GetSubsystem<Graphics>();
    int w = graphics->GetWidth();
    int h = graphics->GetHeight();

    // Convert normalized constants to pixel values
    int marginX   = Max(2, (int)(kMarginX * w));
    int marginY   = Max(1, (int)(kMarginY * h));
    int termTop   = Max(16, (int)(kTermTop * h));
    int inputH    = Max(28, (int)(kInputH * h));
    int inputGap  = Max(2, (int)(kInputGap * h));
    int scrollbarW = Max(8, (int)(kScrollbarW * w));
    int contentWidth = w - 2 * marginX - scrollbarW;
    int termHeight = h - termTop - inputH - inputGap;
    if (termHeight < 40) termHeight = 40;
    int screenHeight = screenRows_ * charHeight_;

    // Status bar
    if (statusBar_)
        statusBar_->SetPosition((int)(kStatusBarX * w), (int)(kStatusBarY * h));

    // Input ScrollView (anchored to bottom)
    if (inputScrollView_)
    {
        inputScrollView_->SetPosition(marginX, h - inputH - marginY);
        inputScrollView_->SetSize(w - 2 * marginX, inputH);
        // inputLine_ auto-sizes its height via Update(); width matches ScrollView content
        if (inputLine_)
            inputLine_->SetFixedWidth(w - 2 * marginX - 16);  // minus scrollbar gutter
    }

    // Scroll view (fills between status bar and input)
    if (scrollView_)
    {
        scrollView_->SetPosition(marginX, termTop);
        scrollView_->SetSize(w - 2 * marginX, termHeight);
        scrollView_->SetPageStep((float)(termHeight / charHeight_ - 2));
    }

    // Content and screen panels
    if (scrollContent_)
        scrollContent_->SetWidth(contentWidth);
    if (screenBg_)
        screenBg_->SetSize(contentWidth, screenHeight);
    if (screenPanel_)
    {
        screenPanel_->SetSize(contentWidth, screenHeight);
        rowContainersValid_ = false;  // Containers need new width
    }
    if (selectionOverlay_)
        selectionOverlay_->SetSize(contentWidth, screenHeight);

    // Search bar (if it exists)
    if (searchBar_)
        searchBar_->SetSize(w - 2 * marginX, Max(20, (int)(kSearchBarH * h)));

    screenDirty_ = true;
}

// ============================================================================
// Slash command menu
// ============================================================================

void Claudette::InitSlashCommands()
{
    // Local commands (intercepted client-side)
    slashCommands_.Push({"exit", "End this session", true});
    slashCommands_.Push({"revive", "Respawn Claude in this terminal", true});
    slashCommands_.Push({"bury", "Clean up and close this terminal", true});
    slashCommands_.Push({"limit", "Set scrollback line limit (e.g. /limit 500)", true});
    slashCommands_.Push({"boss", "Take over Planner — kill and replace", true});

    // PTY commands (forwarded to Claude Code)
    slashCommands_.Push({"bug", "Report a bug", false});
    slashCommands_.Push({"clear", "Clear conversation history", false});
    slashCommands_.Push({"compact", "Compact conversation to save context", false});
    slashCommands_.Push({"config", "Open settings configuration", false});
    slashCommands_.Push({"cost", "Show token and cost usage", false});
    slashCommands_.Push({"doctor", "Check Claude Code health", false});
    slashCommands_.Push({"fast", "Toggle fast mode", false});
    slashCommands_.Push({"help", "Show help information", false});
    slashCommands_.Push({"init", "Create a CLAUDE.md file", false});
    slashCommands_.Push({"login", "Log in to your account", false});
    slashCommands_.Push({"logout", "Log out of your account", false});
    slashCommands_.Push({"memory", "View and manage memory", false});
    slashCommands_.Push({"model", "Switch AI model", false});
    slashCommands_.Push({"permissions", "View and manage permissions", false});
    slashCommands_.Push({"resume", "Resume a previous conversation", false});
    slashCommands_.Push({"review", "Review code changes", false});
    slashCommands_.Push({"status", "Show session status", false});
    slashCommands_.Push({"vim", "Toggle vim keybindings", false});
}

void Claudette::UpdateSlashMenu(const String& input)
{
    if (!slashMenu_)
        return;

    // Only show when input starts with '/'
    if (input.Empty() || input[0] != '/')
    {
        HideSlashMenu();
        return;
    }

    // Extract the partial command (without the leading '/')
    String partial = input.Substring(1).ToLower();

    // Filter matching commands
    Vector<int> matches;
    for (unsigned i = 0; i < slashCommands_.Size(); i++)
    {
        if (partial.Empty() || slashCommands_[i].name.ToLower().StartsWith(partial))
            matches.Push(i);
    }

    if (matches.Empty())
    {
        HideSlashMenu();
        return;
    }

    // Rebuild menu items
    slashMenu_->RemoveAllChildren();
    slashMenuItems_.Clear();
    slashMenuSelection_ = 0;

    for (unsigned m = 0; m < matches.Size(); m++)
    {
        const SlashCommand& cmd = slashCommands_[matches[m]];
        Text* item = slashMenu_->CreateChild<Text>("SlashItem_" + String(m));
        item->SetFont(font_, 13);
        item->SetText("/" + cmd.name + "  " + cmd.description);
        item->SetColor(Color(0.85f, 0.85f, 0.85f));
        item->SetVar("CmdIndex", (int)matches[m]);
        slashMenuItems_.Push(item);
    }

    // Highlight first item
    if (!slashMenuItems_.Empty())
        slashMenuItems_[0]->SetColor(Color(0.3f, 1.0f, 0.5f));

    // Position above input line
    int menuHeight = (int)matches.Size() * 20 + 8;
    if (menuHeight > 400) menuHeight = 400;
    int inputY = inputScrollView_ ? inputScrollView_->GetPosition().y_ : 0;
    auto* graphics = GetSubsystem<Graphics>();
    int w = graphics->GetWidth();
    int marginX = Max(2, (int)(kMarginX * w));
    int gap = Max(2, (int)(kInputGap * graphics->GetHeight()));
    slashMenu_->SetPosition(marginX, inputY - menuHeight - gap);
    slashMenu_->SetSize(w - 2 * marginX, menuHeight);
    slashMenu_->SetVisible(true);
    slashMenuVisible_ = true;
}

void Claudette::HideSlashMenu()
{
    if (slashMenu_)
        slashMenu_->SetVisible(false);
    slashMenuVisible_ = false;
    slashMenuSelection_ = -1;
    slashMenuItems_.Clear();
}

void Claudette::SelectSlashCommand(int index)
{
    if (index < 0 || index >= (int)slashMenuItems_.Size())
        return;

    int cmdIdx = slashMenuItems_[index]->GetVar("CmdIndex").GetI32();
    if (cmdIdx < 0 || cmdIdx >= (int)slashCommands_.Size())
        return;

    const SlashCommand& cmd = slashCommands_[cmdIdx];

    // Fill the input line with the full command
    if (inputLine_)
        inputLine_->SetText("/" + cmd.name);

    HideSlashMenu();
}

bool Claudette::HandleSlashSubmit(const String& text)
{
    if (text.Empty() || text[0] != '/')
        return false;

    String cmd = text.Substring(1).ToLower().Trimmed();

    // Client-side: /exit
    if (cmd == "exit" || cmd == "quit")
    {
        // Send Ctrl+C first to interrupt Claude, then exit cleanly
        WritePTY("\x03");
        engine_->Exit();
        return true;
    }

    // Client-side: /revive — spawn a fresh Claudette process, then self-terminate
    if (cmd == "revive" || cmd == "respawn")
    {
#ifndef _WIN32
        auto* fs = GetSubsystem<FileSystem>();
        String binDir = fs->GetProgramDir();
        String claudettePath = binDir + "Claudette";
        if (!fs->FileExists(claudettePath))
        {
            URHO3D_LOGERROR("/revive: Claudette binary not found at " + claudettePath);
            return true;
        }

        // Build args: pass current role so the new instance inherits it
        String roleArg = role_.Empty() ? String::EMPTY : role_.ToLower();

        URHO3D_LOGINFO("/revive: spawning new Claudette" +
            (roleArg.Empty() ? String("") : (" --role " + roleArg)) +
            " then exiting");

        // Spawn as independent process via double-fork + setsid (same pattern as Manager)
        pid_t first = fork();
        if (first < 0)
        {
            URHO3D_LOGERROR("/revive: fork() failed: " + String(strerror(errno)));
            return true;
        }
        if (first == 0)
        {
            setsid();
            pid_t second = fork();
            if (second < 0)
                _exit(1);
            if (second > 0)
                _exit(0);  // First child exits — grandchild is fully orphaned

            // Grandchild — close inherited fds to avoid holding our PTY/sockets open
            for (int fd = 3; fd < 1024; fd++)
                close(fd);

            if (!roleArg.Empty())
                execl(claudettePath.CString(), "Claudette", "--role", roleArg.CString(), (char*)nullptr);
            else
                execl(claudettePath.CString(), "Claudette", (char*)nullptr);
            _exit(1);  // exec failed
        }

        // Parent — reap the first child immediately
        int status;
        waitpid(first, &status, 0);

        // Self-terminate — Stop() handles child cleanup, PID files, sockets
        engine_->Exit();
#endif
        return true;
    }

    // Client-side: /limit N — set scrollback line limit
    if (cmd.StartsWith("limit"))
    {
        String arg = cmd.Substring(5).Trimmed();
        unsigned n = (unsigned)atoi(arg.CString());
        if (n < 100)
            n = 100;  // Floor — anything less is unusable
        maxScrollback_ = n;
        // Trim data buffer immediately if over new limit
        while (scrollback_.Size() > maxScrollback_)
            scrollback_.Erase(0);
        URHO3D_LOGINFO("/limit: scrollback set to " + String(maxScrollback_) + " lines");
        return true;
    }

    // Client-side: /bury — kill child, clean up PID files, close terminal
    if (cmd == "bury" || cmd == "kill")
    {
        if (childPid_ > 0)
        {
            kill(childPid_, SIGTERM);
            usleep(200000);  // 200ms grace
            int status;
            if (waitpid(childPid_, &status, WNOHANG) == 0)
            {
                kill(childPid_, SIGKILL);
                waitpid(childPid_, &status, 0);
            }
            childPid_ = 0;
        }

        // Clean up instance PID file
        String pidDir = "/tmp/urho_claude/instances/";
        String roleLower = role_.ToLower();
        auto* fs = GetSubsystem<FileSystem>();
        if (fs)
        {
            String pidFile = pidDir + roleLower + ".pid";
            if (fs->FileExists(pidFile))
                fs->Delete(pidFile);
        }

        engine_->Exit();
        return true;
    }

    // Client-side: /resources — dump all loaded resource filenames to live terminal screen and file
    if (cmd == "resources" || cmd == "res")
    {
        auto* cache = GetSubsystem<ResourceCache>();
        const HashMap<StringHash, ResourceGroup>& groups = cache->GetAllResources();

        // Helper: inject a string into the live terminal via the ANSI byte processor
        auto emitLine = [this](const String& line) {
            for (unsigned k = 0; k < line.Length(); k++)
                ProcessByte((unsigned char)line[k]);
            ProcessByte('\r');
            ProcessByte('\n');
        };

        emitLine("\033[96m=== Loaded Resources ===\033[0m");
        String fileContent;
        unsigned count = 0;
        for (auto i = groups.Begin(); i != groups.End(); ++i)
        {
            const HashMap<StringHash, SharedPtr<Resource>>& resources = i->second_.resources_;
            for (auto j = resources.Begin(); j != resources.End(); ++j)
            {
                String name = j->second_->GetName();
                emitLine(name);
                fileContent += name + "\n";
                ++count;
            }
        }
        emitLine("\033[96m=== Total: " + String(count) + " resources ===\033[0m");

        // Save to file
        auto* fs = GetSubsystem<FileSystem>();
        String outPath = fs->GetProgramDir() + "claudette_resources.txt";
        File outFile(context_, outPath, FILE_WRITE);
        if (outFile.IsOpen())
        {
            outFile.Write(fileContent.CString(), fileContent.Length());
            emitLine("\033[96mSaved to: " + outPath + "\033[0m");
        }
        else
            emitLine("\033[91mFailed to save to: " + outPath + "\033[0m");

        screenDirty_ = true;
        return true;
    }

    // Client-side: /boss — forcibly take over the planner role
    // Kills the current planner's claude process, claims the role for ourselves
    if (cmd == "boss")
    {
#ifndef _WIN32
        auto* fs = GetSubsystem<FileSystem>();
        const String instDir = "/tmp/urho_claude/instances/";
        const String ttyDir = "/tmp/urho_claude/tty/";
        const String plannerPidFile = instDir + "planner.pid";

        // Emit feedback to terminal
        auto emitLine = [this](const String& line) {
            for (unsigned k = 0; k < line.Length(); k++)
                ProcessByte((unsigned char)line[k]);
            ProcessByte('\r');
            ProcessByte('\n');
        };

        // Read planner's PID
        pid_t plannerPid = ReadPIDFile(context_, plannerPidFile);

        if (plannerPid > 0 && plannerPid == childPid_)
        {
            emitLine("\033[93m/boss: we ARE the planner already.\033[0m");
            screenDirty_ = true;
            return true;
        }

        // Kill the planner's claude process (and its Claudette parent via SIGCHLD)
        if (plannerPid > 0)
        {
            // The PID in planner.pid is the claude child process.
            // Find its Claudette parent to kill the whole terminal.
            char statPath[64];
            snprintf(statPath, sizeof(statPath), "/proc/%d/stat", (int)plannerPid);
            pid_t plannerParent = 0;
            FILE* statFile = fopen(statPath, "r");
            if (statFile)
            {
                int pid; char comm[256]; char state;
                if (fscanf(statFile, "%d %255s %c %d", &pid, comm, &state, (int*)&plannerParent) < 4)
                    plannerParent = 0;
                fclose(statFile);
            }

            emitLine("\033[91m/boss: killing planner (claude PID " + String((int)plannerPid) + ")\033[0m");
            kill(plannerPid, SIGTERM);
            usleep(300000);
            // Force if still alive
            if (kill(plannerPid, 0) == 0)
                kill(plannerPid, SIGKILL);

            // Also terminate the Claudette parent if it's a Claudette process
            if (plannerParent > 0)
            {
                char commPath[64];
                snprintf(commPath, sizeof(commPath), "/proc/%d/comm", (int)plannerParent);
                File commFile(context_, String(commPath), FILE_READ);
                if (commFile.IsOpen())
                {
                    String comm = commFile.ReadLine().Trimmed();
                    if (comm == "Claudette")
                    {
                        emitLine("\033[91m/boss: killing planner Claudette (PID " + String((int)plannerParent) + ")\033[0m");
                        kill(plannerParent, SIGTERM);
                    }
                }
            }
        }
        else
        {
            emitLine("\033[93m/boss: no live planner found — claiming role.\033[0m");
        }

        // Clean up planner's instance files
        if (fs->FileExists(plannerPidFile))
            fs->Delete(plannerPidFile);
        // Remove planner socket symlink
        String plannerSock = ttyDir + "planner.sock";
        if (fs->FileExists(plannerSock))
            fs->Delete(plannerSock);

        // Remove our old role's PID file
        String oldRole = role_.ToLower();
        if (!oldRole.Empty() && oldRole != "planner")
        {
            String oldPidFile = instDir + oldRole + ".pid";
            if (fs->FileExists(oldPidFile))
                fs->Delete(oldPidFile);
            // Remove old role socket symlink
            String oldSock = ttyDir + oldRole + ".sock";
            if (fs->FileExists(oldSock))
                fs->Delete(oldSock);
        }

        // Claim planner: write our claude PID into planner.pid
        {
            File f(context_, plannerPidFile, FILE_WRITE);
            if (f.IsOpen())
            {
                f.WriteLine(String((int)childPid_));
                // TTY ID — match format from claude_ipc.sh
                String ttyId;
                char ttyLink[256];
                snprintf(ttyLink, sizeof(ttyLink), "/proc/%d/fd/0", (int)childPid_);
                char ttyBuf[256];
                ssize_t len = readlink(ttyLink, ttyBuf, sizeof(ttyBuf) - 1);
                if (len > 0)
                {
                    ttyBuf[len] = '\0';
                    ttyId = String(ttyBuf);
                    ttyId.Replace("/", "_");
                }
                f.WriteLine(ttyId);
            }
        }

        // Rebind socket to planner.sock directly
        {
            String plannerSock = ttyDir + "planner.sock";
            if (plannerSock != socketPath_)
            {
                if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
                for (unsigned ci = 0; ci < clientFds_.Size(); ci++)
                    close(clientFds_[ci]);
                clientFds_.Clear();
                unlink(socketPath_.CString());
                socketPath_ = plannerSock;
                StartIPCListener(socketPath_);
            }
            emitLine("\033[92m/boss: socket rebound to " + socketPath_ + "\033[0m");
        }

        // Update internal role
        role_ = "Planner";
        GetSubsystem<Graphics>()->SetWindowTitle("Claudette - Planner");
        ApplyRoleIcon();

        emitLine("\033[92m/boss: role assumed — you are now Planner.\033[0m");

        // Tell the claude child to re-register via its hooks
        String assumeCmd = ".claude/hooks/claude_ipc.sh assume planner\r";
        WritePTY(assumeCmd);

        screenDirty_ = true;
#endif
        return true;
    }

    // Everything else goes to the PTY as-is
    return false;
}

// ============================================================================
// Right-click context menu
// ============================================================================

void Claudette::ShowContextMenu(int x, int y)
{
    if (!contextMenu_)
        return;

    contextMenu_->RemoveAllChildren();

    auto addItem = [&](const String& label, const String& shortcut, const String& action, bool enabled)
    {
        // Use Button so the row receives click events reliably
        auto* row = contextMenu_->CreateChild<Button>("Row_" + action);
        row->SetStyleAuto();
        row->SetLayout(LM_HORIZONTAL, 8);
        row->SetMinHeight(24);
        row->SetColor(Color(0.22f, 0.22f, 0.26f));

        Text* text = row->CreateChild<Text>("Label");
        text->SetFont(font_, 12);
        text->SetText(label);
        text->SetColor(enabled ? Color(0.9f, 0.9f, 0.9f) : Color(0.45f, 0.45f, 0.45f));

        if (!shortcut.Empty())
        {
            Text* hint = row->CreateChild<Text>("Hint");
            hint->SetFont(font_, 11);
            hint->SetText("  " + shortcut);
            hint->SetColor(Color(0.5f, 0.5f, 0.55f));
        }

        row->SetVar("Action", action);
        row->SetVar("Enabled", enabled);
    };

    bool hasSel = HasSelection();
    bool hasClip = !GetSubsystem<UI>()->GetClipboardText().Empty();

    addItem("Copy",             "Ctrl+C", "copy",       hasSel);
    addItem("Paste",            "Ctrl+V", "paste",      hasClip);
    addItem("Select All",       "",        "selectall",  true);

    // Separator
    auto* sep = contextMenu_->CreateChild<BorderImage>("Separator");
    sep->SetMinHeight(1);
    sep->SetMaxHeight(1);
    sep->SetColor(Color(0.35f, 0.35f, 0.4f));

    addItem("Search",           "Ctrl+F", "search",     true);
    addItem("Clear Scrollback", "",        "clearscroll", scrollback_.Size() > 0);
    addItem("Toggle Traffic Log","Ctrl+L", "togglelog",  true);

    // Size and position — clamp to window bounds
    contextMenu_->SetFixedWidth(220);
    contextMenu_->UpdateLayout();
    int menuH = contextMenu_->GetHeight();

    auto* graphics = GetSubsystem<Graphics>();
    int winW = graphics->GetWidth();
    int winH = graphics->GetHeight();
    if (x + 220 > winW) x = winW - 224;
    if (y + menuH > winH) y = winH - menuH - 4;
    if (x < 0) x = 4;
    if (y < 0) y = 4;

    contextMenu_->SetPosition(x, y);
    contextMenu_->SetVisible(true);
    contextMenuVisible_ = true;

    SubscribeToEvent(E_UIMOUSECLICK, URHO3D_HANDLER(Claudette, HandleContextMenuClick));
}

void Claudette::HideContextMenu()
{
    if (contextMenu_)
        contextMenu_->SetVisible(false);
    contextMenuVisible_ = false;
    UnsubscribeFromEvent(E_UIMOUSECLICK);
}

void Claudette::HandleContextMenuClick(StringHash, VariantMap& eventData)
{
    using namespace UIMouseClick;
    auto* element = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr());
    if (!element)
    {
        HideContextMenu();
        return;
    }

    // Walk up to find the row with the Action var — must be a child of contextMenu_
    UIElement* row = element;
    while (row && row != contextMenu_ && row->GetVar("Action").IsEmpty())
        row = row->GetParent();

    // Click was outside the context menu — dismiss
    if (!row || row == contextMenu_)
    {
        HideContextMenu();
        return;
    }
    element = row;

    if (!element->GetVar("Enabled").GetBool())
    {
        HideContextMenu();
        return;
    }

    String action = element->GetVar("Action").GetString();

    if (action == "copy" && HasSelection())
    {
        GetSubsystem<UI>()->SetClipboardText(GetSelectedText());
        ClearSelection();
    }
    else if (action == "paste")
    {
        // Paste from clipboard — ALWAYS into inputLine if it exists.
        // Previous behaviour required inputLine_ to be focused, which meant
        // right-click→Paste from anywhere else dumped the clip straight to
        // the PTY and submitted on embedded newlines. Intent: dump paste
        // into input UI, never auto-submit.
        const String& clip = GetSubsystem<UI>()->GetClipboardText();
        if (!clip.Empty())
        {
            if (inputLine_)
            {
                if (!inputLine_->HasFocus())
                    inputLine_->SetFocus(true);
                unsigned cursor = inputLine_->GetCursorPosition();
                String text = inputLine_->GetText();
                text.Insert(cursor, clip);
                inputLine_->SetText(text);
                inputLine_->SetCursorPosition(cursor + clip.LengthUTF8());
            }
            else
                WritePTY("\x1b[200~" + clip + "\x1b[201~");
        }
    }
    else if (action == "selectall")
    {
        // Select entire visible screen
        selStartRow_ = 0;
        selStartCol_ = 0;
        selEndRow_ = screenRows_ - 1;
        selEndCol_ = screenCols_ - 1;
        hasSelection_ = true;
        selecting_ = false;
        screenDirty_ = true;
    }
    else if (action == "search")
    {
        ToggleSearchBar();
    }
    else if (action == "clearscroll")
    {
        scrollback_.Clear();
        lastSnapshotScrollback_ = 0;
        scrollbackUICount_ = 0;
        pendingScrollback_.Clear();
        if (scrollbackPanel_)
            scrollbackPanel_->RemoveAllChildren();
        screenDirty_ = true;
    }
    else if (action == "togglelog")
    {
        logTraffic_ = !logTraffic_;
        if (logTraffic_ && logPath_.Empty())
            logPath_ = "/tmp/urho_claude/pty_traffic.log";
    }

    HideContextMenu();
}

// ============================================================================
// Event handlers
// ============================================================================

void Claudette::HandleUpdate(StringHash, VariantMap&)
{
    // Focus click-through: replay a synthetic left-click after focus settles
    if (replayClickPending_ && replayClickTimer_.GetMSec(false) > 50)
    {
        replayClickPending_ = false;
        auto* input = GetSubsystem<Input>();
        if (input->GetMouseButtonDown(MOUSEB_LEFT))
        {
            // Mouse button is still held from the focus click — synthesize the event
            using namespace MouseButtonDown;
            VariantMap& data = GetEventDataMap();
            data[P_BUTTON] = (int)MOUSEB_LEFT;
            data[P_BUTTONS] = (int)MOUSEB_LEFT;
            data[P_QUALIFIERS] = (int)input->GetQualifiers();
            data[P_CLICKS] = 1;
            SendEvent(E_MOUSEBUTTONDOWN, data);
        }
    }

    // Auto-scroll input ScrollView to keep cursor visible
    if (inputScrollView_ && inputLine_ && inputLine_->GetTextElement())
    {
        Vector2 curPos = inputLine_->GetTextElement()->GetCharPosition(inputLine_->GetCursorPosition());
        int rowHeight = inputLine_->GetTextElement()->GetRowHeight();
        IntVector2 viewPos = inputScrollView_->GetViewPosition();
        int viewHeight = inputScrollView_->GetHeight();
        int cursorBottom = static_cast<int>(curPos.y_) + rowHeight;

        if (cursorBottom > viewPos.y_ + viewHeight)
            inputScrollView_->SetViewPosition(IntVector2(0, cursorBottom - viewHeight + 4));
        else if (static_cast<int>(curPos.y_) < viewPos.y_)
            inputScrollView_->SetViewPosition(IntVector2(0, Max(0, static_cast<int>(curPos.y_) - 4)));
    }

    // PTY reading happens on the reader thread now. Check if it signaled new data.
    // Only reset to BUSY when screen content actually changed (substantiveOutput_),
    // not on cosmetic PTY traffic like cursor blink or no-op repositions from Ink redraws.
    // This lets the state machine reach READY while claude is idle at a prompt,
    // which unblocks DrainInjectionQueue() for IPC message delivery.
    if (dataReceived_)
    {
        dataReceived_ = false;
        if (substantiveOutput_)
        {
            substantiveOutput_ = false;
            claudeState_ = CLAUDE_BUSY;
            lastOutputTimer_.Reset();
        }
    }

    PollIPC();
    CheckSocketHealth();
    CheckPeerGreetingTimeouts();
    UpdateState();

    // Broadcast meaningful state changes to Manager for peer awareness
    // Only broadcast READY, BUSY, STUCK — not transient SETTLING
    if (claudeState_ != lastBroadcastState_ && claudeState_ != CLAUDE_SETTLING)
    {
        BroadcastStateToManager(claudeState_);
        lastBroadcastState_ = claudeState_;
    }

    // Periodic role check — detect when claude_ipc.sh moved our PID to a
    // different role file and update the window title + status bar to match.
    if (roleCheckTimer_.GetMSec(false) > 5000)
    {
        roleCheckTimer_.Reset();
        pid_t myPid = getpid();
        // Our PTY proxy owns the PID files, so check the proxy PID instead
        pid_t checkPid = childPid_ > 0 ? childPid_ : myPid;

        const char* roles[] = {"planner", "general", "coder", "coder2", "coder3", "coder4", "coder5"};
        String instDir = "/tmp/urho_claude/instances/";
        auto* fs = GetSubsystem<FileSystem>();
        for (unsigned i = 0; i < 7; ++i)
        {
            String path = instDir + roles[i] + ".pid";
            if (!fs->FileExists(path))
                continue;
            File f(context_, path, FILE_READ);
            if (!f.IsOpen())
                continue;
            int filePid = atoi(f.ReadLine().Trimmed().CString());
            if (filePid == (int)checkPid)
            {
                String newRole(roles[i]);
                newRole[0] = (char)toupper(newRole[0]);
                if (newRole != role_)
                {
                    URHO3D_LOGINFOF("Role changed: %s -> %s (detected from PID file)",
                        role_.CString(), newRole.CString());
                    role_ = newRole;
                    GetSubsystem<Graphics>()->SetWindowTitle("Claudette - " + role_);
                    ApplyRoleIcon();
                }
                break;
            }
        }
    }

    // Manager watchdog — relaunch WorkboardManager if it died mid-session
    if (managerCheckTimer_.GetMSec(false) > 30000)
    {
        managerCheckTimer_.Reset();
        EnsureManagerRunning();
    }

    // Health file — write periodically so WorkboardManager can read instance state
    if (healthFileTimer_.GetMSec(false) > healthFileIntervalMs_)
    {
        healthFileTimer_.Reset();
        WriteHealthFile();
    }

    // Adaptive FPS — no reason to burn cycles when nothing is happening.
    // 30 FPS while actively streaming, 5 FPS when idle. The engine's frame
    // limiter sleeps between frames, so lower FPS = lower CPU. Per-row dirty
    // tracking means idle frames cost near-zero even at 5 FPS. 200ms input
    // latency is imperceptible for a terminal waiting on a human.
    {
        auto* engine = GetSubsystem<Engine>();
        unsigned idleMs = lastOutputTimer_.GetMSec(false);
        bool activelyStreaming = (claudeState_ == CLAUDE_BUSY && idleMs < 200);
        engine->SetMaxFps(activelyStreaming ? 30 : 5);
    }

    // Drain injection queue — places messages in input box, auto-submits when READY
    DrainInjectionQueue();

    // Apply debounced PTY resize — one SIGWINCH after the drag settles
#ifndef _WIN32
    if (pendingPtyResize_ && resizeDebounceTimer_.GetMSec(false) >= resizeDebounceMs_)
    {
        pendingPtyResize_ = false;
        if (masterFd_ >= 0)
        {
            auto* graphics = GetSubsystem<Graphics>();
            struct winsize ws;
            ws.ws_col = (unsigned short)pendingPtyCols_;
            ws.ws_row = (unsigned short)pendingPtyRows_;
            ws.ws_xpixel = (unsigned short)graphics->GetWidth();
            ws.ws_ypixel = (unsigned short)graphics->GetHeight();
            ioctl(masterFd_, TIOCSWINSZ, &ws);
        }
        if (pendingPtyCols_ != screenCols_ || pendingPtyRows_ != screenRows_)
            InitScreen(pendingPtyCols_, pendingPtyRows_);
    }
#endif

    // Child health
    if (childPid_ > 0 && !IsChildAlive())
    {
        childPid_ = 0;
        if (statusBar_)
        {
            statusBar_->SetText((role_.Empty() ? "Claudette" : role_) + " | EXITED — /revive or /bury");
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
        String displayRole = role_.Empty() ? "starting..." : role_;
        String newText = displayRole + " v" + CLAUDETTE_VERSION + " | PID " + String((int)getpid()) + cursorStr +
            " | Lines:" + String(scrollback_.Size()) + queueStr + " | " + stateStr;


        if (newText != lastStatusText_)
        {
            lastStatusText_ = newText;
            statusBar_->SetText(newText);
            if (claudeState_ == CLAUDE_STUCK)
                statusBar_->SetColor(Color(1.0f, 0.2f, 0.2f));  // Red — stuck
            else if (claudeState_ == CLAUDE_READY)
                statusBar_->SetColor(Color(0.4f, 1.0f, 0.4f));  // Green — ready
            else if (claudeState_ == CLAUDE_SETTLING)
                statusBar_->SetColor(Color(0.9f, 0.8f, 0.3f));  // Yellow — settling
            else
                statusBar_->SetColor(Color(0.8f, 0.5f, 0.3f));  // Orange — busy
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

    // Update slash command menu based on current input
    if (inputLine_ && inputLine_->HasFocus())
    {
        String text = inputLine_->GetText();
        UpdateSlashMenu(text);
    }
    else
        HideSlashMenu();

    RenderScreen();
    RenderSelectionOverlay();
}

void Claudette::HandleKeyDown(StringHash, VariantMap& eventData)
{
    using namespace KeyDown;
    int key = eventData[P_KEY].GetI32();
    int quals = eventData[P_QUALIFIERS].GetI32();

    // NOTE: inkPinnedInput_ detection (DECSTBM scroll regions) is tracked for
    // informational purposes but does NOT hijack keyboard input. Claude Code's
    // Ink TUI routinely sets scroll regions during normal rendering, which would
    // permanently steal input from the input bar. The input bar must always work.

    // ── Slash menu navigation ──
    if (slashMenuVisible_ && !slashMenuItems_.Empty())
    {
        if (key == KEY_UP)
        {
            // Unhighlight current
            if (slashMenuSelection_ >= 0 && slashMenuSelection_ < (int)slashMenuItems_.Size())
                slashMenuItems_[slashMenuSelection_]->SetColor(Color(0.85f, 0.85f, 0.85f));
            slashMenuSelection_--;
            if (slashMenuSelection_ < 0)
                slashMenuSelection_ = (int)slashMenuItems_.Size() - 1;
            slashMenuItems_[slashMenuSelection_]->SetColor(Color(0.3f, 1.0f, 0.5f));
            return;
        }
        if (key == KEY_DOWN)
        {
            if (slashMenuSelection_ >= 0 && slashMenuSelection_ < (int)slashMenuItems_.Size())
                slashMenuItems_[slashMenuSelection_]->SetColor(Color(0.85f, 0.85f, 0.85f));
            slashMenuSelection_++;
            if (slashMenuSelection_ >= (int)slashMenuItems_.Size())
                slashMenuSelection_ = 0;
            slashMenuItems_[slashMenuSelection_]->SetColor(Color(0.3f, 1.0f, 0.5f));
            return;
        }
        if (key == KEY_TAB)
        {
            // Tab-complete: fill input with selected command
            SelectSlashCommand(slashMenuSelection_);
            return;
        }
        if (key == KEY_ESCAPE)
        {
            HideSlashMenu();
            if (inputLine_)
                inputLine_->SetText("");
            return;
        }
    }

    // Shift+Enter — insert newline into input (multi-line editing)
    if ((key == KEY_RETURN || key == KEY_KP_ENTER) && (quals & QUAL_SHIFT))
    {
        if (inputLine_ && inputLine_->HasFocus())
        {
            unsigned cursor = inputLine_->GetCursorPosition();
            String text = inputLine_->GetText();
            text.Insert(cursor, "\n");
            inputLine_->SetText(text);
            inputLine_->SetCursorPosition(cursor + 1);
        }
        return;
    }

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

            // If slash menu is open, select the highlighted command first
            if (slashMenuVisible_ && slashMenuSelection_ >= 0)
            {
                SelectSlashCommand(slashMenuSelection_);
                text = inputLine_->GetText();
            }

            if (!text.Empty())
            {
                // Check for client-side slash commands
                if (HandleSlashSubmit(text))
                {
                    inputLine_->SetText("");
                    HideSlashMenu();
                    return;
                }
                WritePTY(text + "\r");
                inputLine_->SetText("");
                HideSlashMenu();
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
            // Paste from clipboard — ALWAYS into inputLine if it exists.
            // Previous behaviour fell through to WritePTY(clip) when inputLine_
            // was not focused, which submitted the pasted content on embedded
            // newlines. Intent: dump paste into input UI, never auto-submit.
            const String& clip = GetSubsystem<UI>()->GetClipboardText();
            if (!clip.Empty())
            {
                if (inputLine_)
                {
                    if (!inputLine_->HasFocus())
                        inputLine_->SetFocus(true);
                    unsigned cursor = inputLine_->GetCursorPosition();
                    String text = inputLine_->GetText();
                    text.Insert(cursor, clip);
                    inputLine_->SetText(text);
                    inputLine_->SetCursorPosition(cursor + clip.LengthUTF8());
                }
                else
                {
                    // No input line — fall back to PTY but wrap in bracketed
                    // paste markers so the inner app (claude) treats it as a
                    // paste, not as a sequence of keypresses + Enter.
                    WritePTY("\x1b[200~" + clip + "\x1b[201~");
                }
            }
        }
        else if (key == KEY_F)
        {
            ToggleSearchBar();
        }
        else if (key == KEY_A) WritePTYRaw("\x01", 1);  // Ctrl+A — accept all
        else if (key == KEY_B) WritePTYRaw("\x02", 1);  // Ctrl+B — back/toggle
        else if (key == KEY_D) WritePTYRaw("\x04", 1);  // Ctrl+D — EOF
        else if (key == KEY_E) WritePTYRaw("\x05", 1);  // Ctrl+E — end of line
        else if (key == KEY_K) WritePTYRaw("\x0b", 1);  // Ctrl+K — clear/kill
        else if (key == KEY_N) WritePTYRaw("\x0e", 1);  // Ctrl+N — next
        else if (key == KEY_O) WritePTYRaw("\x0f", 1);  // Ctrl+O — open
        else if (key == KEY_P) WritePTYRaw("\x10", 1);  // Ctrl+P — previous
        else if (key == KEY_R) WritePTYRaw("\x12", 1);  // Ctrl+R — retry/reverse
        else if (key == KEY_T) WritePTYRaw("\x14", 1);  // Ctrl+T — transpose
        else if (key == KEY_U) WritePTYRaw("\x15", 1);  // Ctrl+U — kill line
        else if (key == KEY_W) WritePTYRaw("\x17", 1);  // Ctrl+W — kill word
        else if (key == KEY_Z) WritePTYRaw("\x1a", 1);  // Ctrl+Z — suspend
        else if (key == KEY_BACKSLASH) WritePTYRaw("\x1c", 1);  // Ctrl+\ — SIGQUIT
        else if (key == KEY_L)
        {
            logTraffic_ = !logTraffic_;
            if (logTraffic_ && logPath_.Empty())
                logPath_ = "/tmp/urho_claude/pty_traffic.log";
        }
        // Suppress TEXTINPUT so Ctrl combos don't leak into the input bar
        suppressNextTextInput_ = true;
        if (inputLine_)
            savedInputText_ = inputLine_->GetText();
        return;
    }

    // Escape — close menus/selection, or forward to PTY
    if (key == KEY_ESCAPE)
    {
        if (contextMenuVisible_)
        {
            HideContextMenu();
            return;
        }
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

    // Tab — forward to PTY for autocomplete (Shift+Tab = reverse-tab \033[Z)
    if (key == KEY_TAB)
    {
        WritePTY((quals & QUAL_SHIFT) ? "\033[Z" : "\t");
        return;
    }

    // Backspace and Delete — let LineEdit handle them (selection-aware).
    // When input is empty, swallow the key (no PTY forwarding, no fall-through).
    if ((key == KEY_BACKSPACE || key == KEY_DELETE) && inputLine_ && inputLine_->GetText().Empty())
        return;

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

    // Numpad digits + Enter — always pass directly to PTY, bypassing inputLine_.
    // This allows responding to Claude's survey prompts and other raw-input UIs.
    // NOTE: SDL numpad scancodes go 1,2,3...9,0 (KP_0=98 > KP_9=97) so we
    // must use scancodes and check KP_1..KP_9 and KP_0 separately.
    {
        using namespace KeyDown;
        int sc = eventData[P_SCANCODE].GetI32();
        char digit = 0;
        if (sc >= SCANCODE_KP_1 && sc <= SCANCODE_KP_9)
            digit = '1' + (sc - SCANCODE_KP_1);
        else if (sc == SCANCODE_KP_0)
            digit = '0';

        if (digit)
        {
            WritePTYRaw(&digit, 1);
            suppressNextTextInput_ = true;
            if (inputLine_)
                savedInputText_ = inputLine_->GetText();
            return;
        }
    }
    if (key == KEY_KP_ENTER)
    {
        WritePTY("\r");
        return;
    }

    // Arrow keys — ONLY forward to PTY if input box is empty
    // (when input box has text, arrows should navigate within the input box)
    if (inputLine_ && inputLine_->GetText().Empty())
    {
        if (key == KEY_UP) WritePTY("\033[A");
        else if (key == KEY_DOWN) WritePTY("\033[B");
        else if (key == KEY_RIGHT) WritePTY("\033[C");
        else if (key == KEY_LEFT) WritePTY("\033[D");
    }

    // All other keys: let the LineEdit handle them (no PTY forwarding)
}

void Claudette::HandleTextInput(StringHash, VariantMap&)
{
    // Numpad keys were already sent to PTY in HandleKeyDown — restore inputLine_ text
    if (suppressNextTextInput_)
    {
        suppressNextTextInput_ = false;
        if (inputLine_)
            inputLine_->SetText(savedInputText_);
    }
}

void Claudette::HandleResize(StringHash, VariantMap&)
{
    // Reposition all UI elements from normalized coordinates
    ApplyLayout();

#ifndef _WIN32
    if (masterFd_ >= 0)
    {
        auto* graphics = GetSubsystem<Graphics>();
        int w = graphics->GetWidth();
        int h = graphics->GetHeight();

        // Derive PTY cols/rows from actual font metrics, not magic numbers
        int marginX = Max(2, (int)(kMarginX * w));
        int scrollbarW = Max(8, (int)(kScrollbarW * w));
        int termTop = Max(16, (int)(kTermTop * h));
        int inputH = Max(28, (int)(kInputH * h));
        int inputGap = Max(2, (int)(kInputGap * h));

        int contentWidth = w - 2 * marginX - scrollbarW;
        int termHeight = h - termTop - inputH - inputGap;

        int cols = contentWidth / charWidth_;
        int rows = termHeight / charHeight_;
        if (cols < 40) cols = 40;
        if (rows < 10) rows = 10;

        // Debounce: don't blast SIGWINCH on every pixel of a drag resize.
        // Store the pending size; HandleUpdate applies it after 150ms of quiet.
        if (cols != screenCols_ || rows != screenRows_)
        {
            pendingPtyCols_ = cols;
            pendingPtyRows_ = rows;
            pendingPtyResize_ = true;
            resizeDebounceTimer_.Reset();
        }
    }
#endif
}

void Claudette::HandleDropFile(StringHash, VariantMap& eventData)
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

bool Claudette::ScreenToCell(int screenX, int screenY, int& row, int& col)
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

void Claudette::HandleInputFocus(StringHash, VariantMap& eventData)
{
    using namespace InputFocus;
    bool focused = eventData[P_FOCUS].GetBool();
    if (focused)
    {
        // On Linux/X11, the WM consumes the click that gives us focus.
        // Schedule a synthetic click replay after a short delay to let focus settle.
        replayClickPending_ = true;
        replayClickTimer_.Reset();
    }

    // Subtle background tint so the active window is distinguishable
    if (screenBg_)
    {
        if (focused)
            screenBg_->SetColor(Color(0.045f, 0.048f, 0.062f));  // slightly warmer/lighter
        else
            screenBg_->SetColor(Color(0.035f, 0.038f, 0.047f));  // Melbourne night sky (default)
    }
}

void Claudette::HandleMouseButtonDown(StringHash, VariantMap& eventData)
{
    // Cancel any pending replay — a real click arrived
    replayClickPending_ = false;

    using namespace MouseButtonDown;
    int button = eventData[P_BUTTON].GetI32();

    if (button == MOUSEB_RIGHT)
    {
        auto* input = GetSubsystem<Input>();
        IntVector2 pos = input->GetMousePosition();
        ShowContextMenu(pos.x_, pos.y_);
        return;
    }

    if (button != MOUSEB_LEFT)
        return;

    // Dismiss context menu on left click
    if (contextMenuVisible_)
        HideContextMenu();

    auto* input = GetSubsystem<Input>();
    IntVector2 pos = input->GetMousePosition();
    int row, col;
    if (!ScreenToCell(pos.x_, pos.y_, row, col))
        return;

    // Multi-click detection: double=word, triple=line
    // Reset if too slow (>500ms) or different row
    if (clickTimer_.GetMSec(false) > 500 || row != lastClickRow_)
        clickCount_ = 0;
    clickTimer_.Reset();
    lastClickRow_ = row;
    lastClickCol_ = col;
    ++clickCount_;

    if (clickCount_ == 2)
    {
        // Double-click: select word under cursor
        int wordStart = col, wordEnd = col;
        if (row >= 0 && row < screenRows_)
        {
            MutexLock lock(bufferMutex_);
            // Expand left while alphanumeric/underscore
            while (wordStart > 0)
            {
                const String& ch = screen_[row][wordStart - 1].ch;
                if (ch.Length() == 1 && (IsAlpha(ch[0]) || IsDigit(ch[0]) || ch[0] == '_' || ch[0] == '-'))
                    --wordStart;
                else
                    break;
            }
            // Expand right
            while (wordEnd < screenCols_ - 1)
            {
                const String& ch = screen_[row][wordEnd + 1].ch;
                if (ch.Length() == 1 && (IsAlpha(ch[0]) || IsDigit(ch[0]) || ch[0] == '_' || ch[0] == '-'))
                    ++wordEnd;
                else
                    break;
            }
        }
        selStartRow_ = row; selStartCol_ = wordStart;
        selEndRow_ = row; selEndCol_ = wordEnd;
        hasSelection_ = (wordStart != wordEnd);
        selecting_ = false;
        if (hasSelection_)
        {
            GetSubsystem<UI>()->SetClipboardText(GetSelectedText());
            screenDirty_ = true;
        }
        return;
    }
    else if (clickCount_ >= 3)
    {
        // Triple-click: select entire line and copy to clipboard
        // (standard terminal behaviour — xterm, gnome-terminal, iTerm2 all do this)
        selStartRow_ = row; selStartCol_ = 0;
        selEndRow_ = row; selEndCol_ = screenCols_ - 1;
        hasSelection_ = true;
        selecting_ = false;
        clickCount_ = 0;  // reset so next click starts fresh
        GetSubsystem<UI>()->SetClipboardText(GetSelectedText());
        screenDirty_ = true;
        return;
    }

    // Single click: begin drag selection
    selecting_ = true;
    hasSelection_ = false;
    selStartRow_ = row;
    selStartCol_ = col;
    selEndRow_ = row;
    selEndCol_ = col;
}

void Claudette::HandleMouseButtonUp(StringHash, VariantMap& eventData)
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

void Claudette::HandleMouseMove(StringHash, VariantMap&)
{
    if (!selecting_)
        return;

    auto* input = GetSubsystem<Input>();
    IntVector2 pos = input->GetMousePosition();
    int row, col;
    if (ScreenToCell(pos.x_, pos.y_, row, col))
    {
        // Only update overlay when the cell actually changes
        if (row != selEndRow_ || col != selEndCol_)
        {
            selEndRow_ = row;
            selEndCol_ = col;
            hasSelection_ = true;
        }
    }
}

bool Claudette::HasSelection() const
{
    return hasSelection_;
}

void Claudette::ClearSelection()
{
    hasSelection_ = false;
    selecting_ = false;
}

bool Claudette::IsCellSelected(int row, int col) const
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

String Claudette::GetSelectedText()
{
    if (!hasSelection_)
        return String::EMPTY;

    MutexLock lock(bufferMutex_);

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

void Claudette::RenderSelectionOverlay()
{
    if (!selectionOverlay_)
        return;

    if (!hasSelection_)
    {
        if (selectionOverlay_->GetNumChildren() > 0)
            selectionOverlay_->RemoveAllChildren();
        return;
    }

    if (!screenDirty_ && !selecting_)
        return;

    selectionOverlay_->RemoveAllChildren();

    // Normalize so start <= end in reading order
    int r1 = selStartRow_, c1 = selStartCol_;
    int r2 = selEndRow_, c2 = selEndCol_;
    if (r1 > r2 || (r1 == r2 && c1 > c2))
    {
        int tr = r1; r1 = r2; r2 = tr;
        int tc = c1; c1 = c2; c2 = tc;
    }

    // One highlight rectangle per selected row — very lightweight
    for (int r = r1; r <= r2; r++)
    {
        int startC = (r == r1) ? c1 : 0;
        int endC = (r == r2) ? c2 : screenCols_ - 1;
        int y = (int)(r * cellHeightF_);

        BorderImage* hl = selectionOverlay_->CreateChild<BorderImage>();
        hl->SetPosition((int)(startC * cellWidthF_), y);
        hl->SetSize((int)((endC - startC + 1) * cellWidthF_ + 0.5f), (int)(cellHeightF_ + 0.5f));
        hl->SetColor(Color(0.2f, 0.4f, 0.7f, 0.7f));
    }
}

// ============================================================================
// Search
// ============================================================================

void Claudette::ToggleSearchBar()
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
            int h = graphics->GetHeight();
            int marginX = Max(2, (int)(kMarginX * w));
            int searchH = Max(20, (int)(kSearchBarH * h));

            searchBar_ = root->CreateChild<UIElement>("SearchBar");
            searchBar_->SetLayout(LM_HORIZONTAL, 4);
            searchBar_->SetPosition(marginX, (int)(kStatusBarY * h));
            searchBar_->SetSize(w - 2 * marginX, searchH);

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

void Claudette::DoSearch(const String& query, bool forward)
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

    MutexLock lock(bufferMutex_);

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

void Claudette::ClearSearchHighlights()
{
    searchMatches_.Clear();
    currentMatch_ = -1;
    searchQuery_.Clear();
    if (searchStatus_)
        searchStatus_->SetText("");
}

bool Claudette::IsCellSearchMatch(int row, int col) const
{
    for (unsigned i = 0; i < searchMatches_.Size(); i++)
    {
        const SearchMatch& m = searchMatches_[i];
        if (row == m.row && col >= m.col && col < m.col + m.len)
            return true;
    }
    return false;
}
