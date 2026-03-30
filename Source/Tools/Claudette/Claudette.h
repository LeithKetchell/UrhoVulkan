// Claudette — Urho3D terminal emulator for Claude Code.
// Owns the PTY master fd. Accepts IPC messages via Unix socket.
// Virtual screen buffer with cursor tracking for deterministic injection.
// No dependency on gnome-terminal or Anthropic's Ink layer.

#pragma once

#include <Urho3D/Engine/Application.h>
#include <Urho3D/Core/Timer.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/ScrollView.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/Window.h>

using namespace Urho3D;

/// A single cell in the virtual screen buffer
struct ScreenCell
{
    String ch{" "};        // UTF-8 character (1-4 bytes, or space)
    unsigned char fg{7};   // ANSI foreground color index (0-255), 7=white default
    unsigned char bg{0};   // ANSI background color index (0-255), 0=black default
    bool bold{false};
    bool dim{false};
    bool underline{false};
    // True color (24-bit RGB) support — when set, overrides indexed color
    Color trueColorFg;
    Color trueColorBg;
    bool hasTrueColorFg{false};
    bool hasTrueColorBg{false};
};

/// Claude Code process state — observed from PTY traffic
enum ClaudeState
{
    CLAUDE_BUSY,        // Receiving output — Ink is rendering
    CLAUDE_SETTLING,    // Output stopped, waiting for stability
    CLAUDE_READY        // Stable for threshold ms — safe to inject
};

class Claudette : public Application
{
    URHO3D_OBJECT(Claudette, Application);

public:
    explicit Claudette(Context* context);
    ~Claudette() override;

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    // ── PTY management ──
    bool SpawnChild(const String& command, const StringVector& args);
    void ReadPTY();
    void WritePTY(const String& data);
    void WritePTYRaw(const char* data, unsigned len);
    bool IsChildAlive();

    // ── IPC socket ──
    bool StartIPCListener(const String& socketPath);
    void PollIPC();
    void HandleIPCMessage(const String& message);

    // ── Virtual screen buffer ──
    void InitScreen(int cols, int rows);
    void ScreenPutChar(char c);
    void ScreenPutString(const String& ch);
    void ScreenNewLine();
    void ScreenCarriageReturn();
    void ScreenScrollUp();
    void ScreenClearLine(int row, int fromCol, int toCol);
    void ScreenClearScreen(int mode);  // 0=below, 1=above, 2=all
    void ScreenSetCursor(int row, int col);

    // ── ANSI parser ──
    void ProcessByte(unsigned char c);
    void ProcessCSI();
    void ProcessSGR();  // Select Graphic Rendition (colors/attributes)
    void ProcessOSC(unsigned char c);

    // ── Display ──
    void RenderScreen();

    // ── State machine ──
    void UpdateState();
    void DrainInjectionQueue();

    // ── Scrollback ──
    void PushScrollback(int row);
    String GetRowText(int row);

    // ── UI ──
    void CreateUI();
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandleKeyDown(StringHash eventType, VariantMap& eventData);
    void HandleTextInput(StringHash eventType, VariantMap& eventData);
    void HandleResize(StringHash eventType, VariantMap& eventData);
    void HandleDropFile(StringHash eventType, VariantMap& eventData);

    // ── Color conversion ──
    Color AnsiToColor(unsigned char idx, bool bold);
    Color CellFgColor(const ScreenCell& cell);
    Color CellBgColor(const ScreenCell& cell);

    // ── Selection (copy/paste) ──
    void HandleMouseButtonDown(StringHash eventType, VariantMap& eventData);
    void HandleMouseButtonUp(StringHash eventType, VariantMap& eventData);
    void HandleMouseMove(StringHash eventType, VariantMap& eventData);
    bool ScreenToCell(int screenX, int screenY, int& row, int& col);
    String GetSelectedText();
    void ClearSelection();
    bool HasSelection() const;
    bool IsCellSelected(int row, int col) const;

    // ── Search ──
    void ToggleSearchBar();
    void DoSearch(const String& query, bool forward);
    void ClearSearchHighlights();
    bool IsCellSearchMatch(int row, int col) const;

    // ── Block character rendering ──
    bool IsBlockChar(const String& ch);
    void RenderBlockChar(const String& ch, int x, int y, Color fg, Color bg);

    // ── UI elements ──
    SharedPtr<Font> font_;
    int fontSize_{11};
    float cellWidthF_{7.0f};   // Exact float advance from font
    float cellHeightF_{14.0f}; // Exact float row height from font
    int charWidth_{7};         // Integer cell width (rounded)
    int charHeight_{14};       // Integer cell height (rounded)

    // ── wcwidth ──
    static int TermWcWidth(unsigned codepoint);
    Window* termWindow_{};
    UIElement* screenPanel_{};  // Direct-positioned grid container
    ScrollView* scrollView_{};     // Main scrollable terminal area
    UIElement* scrollContent_{};   // Content element inside ScrollView
    UIElement* scrollbackPanel_{}; // Holds scrollback Text items
    BorderImage* screenBg_{};      // Live screen background (child of scrollContent_)
    Text* statusBar_{};
    LineEdit* inputLine_{};
    bool autoScroll_{true};        // Auto-scroll to bottom on new output

    // ── Virtual screen buffer ──
    int screenCols_{120};
    int screenRows_{40};
    int cursorRow_{0};
    int cursorCol_{0};
    int savedCursorRow_{0};
    int savedCursorCol_{0};
    Vector<Vector<ScreenCell>> screen_;   // screen_[row][col]
    bool screenDirty_{true};

    // ── Current SGR attributes ──
    unsigned char curFg_{7};
    unsigned char curBg_{0};
    bool curBold_{false};
    bool curDim_{false};
    bool curUnderline_{false};
    Color curTrueColorFg_;
    Color curTrueColorBg_;
    bool curHasTrueColorFg_{false};
    bool curHasTrueColorBg_{false};

    // ── ANSI parser state ──
    enum ParseState { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC, PS_OSC_ESC, PS_DCS, PS_DCS_ESC, PS_CHARSET, PS_UTF8 };
    ParseState parseState_{PS_NORMAL};
    String csiParams_;        // Accumulated CSI parameter bytes
    String oscBuffer_;        // Accumulated OSC content
    bool altScreenActive_{false};

    // ── UTF-8 accumulator ──
    char utf8Buf_[5]{};        // Up to 4 bytes + null
    int utf8Expected_{0};      // Total bytes expected
    int utf8Got_{0};           // Bytes received so far

    // ── Scrollback buffer ──
    struct ScrollLine
    {
        String text;
        Color color{0.75f, 0.8f, 0.75f, 1.0f};
    };
    Vector<ScrollLine> scrollback_;
    unsigned maxScrollback_{2000};
    unsigned lastRenderedScrollback_{0};

    // ── PTY state ──
    int masterFd_{-1};
    pid_t childPid_{0};

    // ── IPC state ──
    int listenFd_{-1};
    String socketPath_;
    Vector<int> clientFds_;

    // ── State machine ──
    ClaudeState claudeState_{CLAUDE_BUSY};
    Timer lastOutputTimer_;
    Timer lastCursorMoveTimer_;
    unsigned settleThresholdMs_{250};
    unsigned busyToSettleMs_{120};

    // ── Injection queue ──
    StringVector injectionQueue_;   // Messages waiting for input box
    bool autoSubmitPending_{false}; // True when input box has IPC content to submit
    Timer autoSubmitTimer_;         // Delay before auto-submitting
    unsigned autoSubmitDelayMs_{1500}; // Visible delay so user can see the message
    String pacedBuffer_;
    Timer pacedTimer_;

    // ── Display throttle ──
    String lastStatusText_;
    Timer displayTimer_;
    unsigned displayIntervalMs_{300};

    // ── Selection state (copy/paste) ──
    bool selecting_{false};
    bool hasSelection_{false};
    int selStartRow_{0}, selStartCol_{0};
    int selEndRow_{0}, selEndCol_{0};

    // ── Search state ──
    bool searchVisible_{false};
    LineEdit* searchInput_{};
    Text* searchStatus_{};
    UIElement* searchBar_{};
    String searchQuery_;
    struct SearchMatch { int row; int col; int len; };
    Vector<SearchMatch> searchMatches_;
    int currentMatch_{-1};

    // ── PTY traffic log ──
    bool logTraffic_{false};
    String logPath_;
};
