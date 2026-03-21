// WorkboardManager — GUI dashboard for workboard viewing, plan browsing,
// and bidirectional IPC with Claude Code instances via Unix FIFOs + drop-files.

#pragma once

#include <Urho3D/Engine/Application.h>
#include <Urho3D/Network/Network.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/ListView.h>
#include <Urho3D/UI/MultiLineEdit.h>
#include <Urho3D/UI/ScrollView.h>
#include <Urho3D/UI/ScrollBar.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/Window.h>

using namespace Urho3D;

/// A parsed table row from the workboard markdown.
struct WorkboardRow
{
    Vector<String> cells;
};

/// A parsed table section (Ready, In Progress, Done, etc.)
struct WorkboardSection
{
    String title;
    Vector<String> headers;
    Vector<WorkboardRow> rows;
};

class WorkboardManager : public Application
{
    URHO3D_OBJECT(WorkboardManager, Application);

public:
    explicit WorkboardManager(Context* context);

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    // ── UI creation ──
    void CreateUI();
    void CreateInstanceStatusBar(UIElement* parent, int w, int h);
    void CreateWorkboardPanel(UIElement* parent, int x, int y, int w, int h);
    void CreatePlanPanel(UIElement* parent, int x, int y, int w, int h);
    void CreateComposer(UIElement* parent, int x, int y, int w, int h);
    void CreateMessageLog(UIElement* parent, int x, int y, int w, int h);

    // ── Workboard ──
    void LoadWorkboard();
    void ParseWorkboard(const String& content);
    void RenderWorkboardUI();
    void AddSectionToUI(const WorkboardSection& section);

    // ── Workboard mutations (Manager is single authority) ──
    bool HandleWorkboardCommand(const String& message);
    WorkboardSection* FindSection(const String& keyword);
    void AddReadyRow(const Vector<String>& fields);
    void AddInProgressRow(const Vector<String>& fields);
    void AddDoneRow(const Vector<String>& fields);
    void MoveToDone(const String& taskName);
    void RemoveRow(const String& matchText);
    void AddSharedFile(const Vector<String>& fields);
    void UpdateReview(const String& taskName, const String& newReview);
    void WriteWorkboard();
    void EmitTableRows(String& output, const WorkboardSection* sec);

    // ── Plans ──
    void ScanPlanFiles();
    void LoadPlanContent(const String& filename);
    void HandlePlanSelected(StringHash eventType, VariantMap& eventData);

    // ── IPC ──
    void CreateIPCPaths();
    void OpenFIFOs();
    void CloseFIFOs();
    void PollFIFOs();
    void SendMessage(const String& target, const String& message);
    void WakeInstance(const String& role);
    int ReadInstancePID(const String& role);
    bool IsInstanceAlive(const String& role);
    void RefreshInstanceStatus();

    // ── Beacon ──
    void UpdateBeacon();
    void CleanupStalePID(const String& role, int pid);
    void SweepStaleRoleFiles();
    float GetLastActivity(const String& role);

    void HandleSendCoder(StringHash eventType, VariantMap& eventData);
    void HandleSendPlanner(StringHash eventType, VariantMap& eventData);
    void HandleSendUnassigned(StringHash eventType, VariantMap& eventData);
    void HandleSendBroadcast(StringHash eventType, VariantMap& eventData);

    // ── Download ──
    void CreateDownloadBar(UIElement* parent, int x, int y, int w, int h);
    void HandleDownload(StringHash eventType, VariantMap& eventData);
    void CheckDownloadProgress();

    // ── Message log ──
    void AppendLog(const String& source, const String& message);
    Color LogColorForSource(const String& source);

    // ── Theme ──
    void CreateThemeBar(UIElement* parent, int x, int y, int w, int h);
    void HandleFontSelected(StringHash eventType, VariantMap& eventData);
    void HandleFontSizeChanged(StringHash eventType, VariantMap& eventData);
    void ApplyFont(const String& fontName, int fontSize);
    void LoadThemePrefs();
    void SaveThemePrefs();
    void RebuildAllUI();

    // ── Events ──
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandleKeyDown(StringHash eventType, VariantMap& eventData);
    void HandleSectionToggle(StringHash eventType, VariantMap& eventData);

    // ── Helpers ──
    String GetProjectRoot();
    String GetClaudeDir();

    // ── Font / Theme ──
    SharedPtr<Font> font_;
    String currentFontName_{"Anonymous Pro"};
    int currentFontSize_{11};
    Vector<String> availableFonts_;
    DropDownList* fontSelector_{};
    DropDownList* fontSizeSelector_{};

    // ── Workboard state ──
    String projectRoot_;
    Vector<WorkboardSection> sections_;
    float refreshAccumulator_{};
    static constexpr float REFRESH_INTERVAL = 3.0f;
    unsigned lastWriteMtime_{0};

    // ── Workboard UI ──
    Window* workboardPanel_{};
    ScrollView* workboardScroll_{};
    UIElement* workboardContent_{};

    // ── Plan UI ──
    Window* planPanel_{};
    ListView* planListView_{};
    ScrollView* planContentScroll_{};
    Text* planContentText_{};
    Vector<String> planFiles_;
    String currentPlanFile_;

    // ── Instance status UI ──
    Text* coderStatusText_{};
    Text* plannerStatusText_{};
    Text* unassignedStatusText_{};

    // ── Composer UI ──
    LineEdit* messageInput_{};
    Button* sendCoderBtn_{};
    Button* sendPlannerBtn_{};
    Button* sendUnassignedBtn_{};
    Button* sendBroadcastBtn_{};

    // ── Download UI ──
    LineEdit* downloadUrlInput_{};
    Button* downloadBtn_{};
    Text* downloadStatusText_{};
    bool downloadInProgress_{false};
    String downloadOutputPath_;

    // ── Message log UI ──
    Window* logPanel_{};
    ListView* logListView_{};
    static const unsigned MAX_LOG_LINES = 500;

    // ── IPC state ──
    static constexpr const char* IPC_DIR = "/tmp/urho_claude";
    int fdFromCoderRead_{-1};
    int fdFromPlannerRead_{-1};
    int fdFromUnassignedRead_{-1};
    String fromCoderBuffer_;
    String fromPlannerBuffer_;
    String fromUnassignedBuffer_;
    static constexpr const char* MSG_DELIMITER = "---END---";

    // ── Beacon liveness ──
    float lastCoderActivity_{999.0f};
    float lastPlannerActivity_{999.0f};
    float lastUnassignedActivity_{999.0f};
    static constexpr float LIVENESS_TIMEOUT = 300.0f;  // 5 min — Claude sessions can idle
    static constexpr unsigned short BEACON_PORT = 31337;
};
