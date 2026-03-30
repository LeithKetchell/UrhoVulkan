// WorkboardManager — GUI dashboard for workboard viewing, plan browsing,
// and bidirectional IPC with Claude Code instances via TTY injection.

#pragma once

#include "WorkboardBase.h"

#include <Urho3D/Network/Network.h>
#include <Urho3D/Network/Connection.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/MultiLineEdit.h>
#include <Urho3D/UI/ScrollBar.h>

using namespace Urho3D;

class WorkboardManager : public WorkboardBase
{
    URHO3D_OBJECT(WorkboardManager, WorkboardBase);

public:
    explicit WorkboardManager(Context* context);

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    // ── UI creation ──
    void CreateUI();
    void CreateInstanceStatusBar(UIElement* parent, int w, int h);
    void CreateComposer(UIElement* parent, int x, int y, int w, int h);
    void CreateMessageLog(UIElement* parent, int x, int y, int w, int h);

    // ── Workboard ──
    void LoadWorkboard();

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
    bool AssignTask(const String& taskName, const String& coderRole);
    void WriteWorkboard();
    void EmitTableRows(String& output, const WorkboardSection* sec);

    // ── Plans ──
    void ScanPlanFiles();
    void LoadPlanContent(const String& filename);
    void OnPlanSelected(const String& filename) override;

    // ── IPC ──
    void CreateIPCPaths();
    void SendMessage(const String& target, const String& message);
    bool InjectViaTTY(const String& role, const String& message);
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
    void HandleClearFileLocks(StringHash eventType, VariantMap& eventData);
    void HandleSpawnCoder(StringHash eventType, VariantMap& eventData);

    // ── Multi-Coder discovery ──
    Vector<String> DiscoverCoderRoles();
    Vector<String> DiscoverUnassignedRoles();
    String GetSelectedCoderRole();

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

    // ── Remote workboard sync (Phase 2a) ──
    void RegisterWorkboardRemoteEvents();
    void PushWorkboardToClient(Connection* conn);
    void PushPlanListToClient(Connection* conn);
    void PushClientListToAll();
    void PushWorkboardToAllClients();
    String BuildPlanListString();
    String BuildClientListString();

    void HandleClientConnected(StringHash eventType, VariantMap& eventData);
    void HandleClientDisconnected(StringHash eventType, VariantMap& eventData);
    void HandleClientIdentity(StringHash eventType, VariantMap& eventData);
    void HandleKeyExchangeAuth(StringHash eventType, VariantMap& eventData);
    void HandleClientAuthenticated(StringHash eventType, VariantMap& eventData);
    void HandleWbRequestPlan(StringHash eventType, VariantMap& eventData);
    void HandleWbMutation(StringHash eventType, VariantMap& eventData);
    void HandleWbSetIdentity(StringHash eventType, VariantMap& eventData);

    // ── Events ──
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandleKeyDown(StringHash eventType, VariantMap& eventData);

    // ── Font / Theme ──
    String currentFontName_{"Anonymous Pro"};
    int currentFontSize_{11};
    Vector<String> availableFonts_;
    DropDownList* fontSelector_{};
    DropDownList* fontSizeSelector_{};

    // ── Workboard state (Manager-specific) ──
    float refreshAccumulator_{};
    static constexpr float REFRESH_INTERVAL = 5.0f;
    unsigned lastWriteMtime_{0};

    // ── Instance status UI ──
    DropDownList* coderStatusDropdown_{};
    Vector<String> knownCoderRoles_;
    Text* plannerStatusText_{};
    DropDownList* unassignedStatusDropdown_{};
    Vector<String> knownUnassignedRoles_;

    // ── Composer UI ──
    LineEdit* messageInput_{};
    DropDownList* coderDropdown_{};
    Button* sendCoderBtn_{};
    Button* sendPlannerBtn_{};
    Button* sendUnassignedBtn_{};
    Button* sendBroadcastBtn_{};
    Button* clearFileLocksBtn_{};
    Button* spawnCoderBtn_{};

    // ── Download UI ──
    LineEdit* downloadUrlInput_{};
    Button* downloadBtn_{};
    Text* downloadStatusText_{};
    bool downloadInProgress_{false};
    String downloadOutputPath_;
    unsigned curlRequestId_{0};
    float downloadCheckTimer_{0.0f};

    // ── Message log UI ──
    Window* logPanel_{};
    ListView* logListView_{};
    static const unsigned MAX_LOG_LINES = 500;

    // ── IPC state (initialized in Start() via FileSystem::GetTemporaryDir()) ──
    String ipcDir_;
    String ttySockDir_;

    // ── Beacon liveness ──
    HashMap<String, float> coderActivityTimers_;
    float lastPlannerActivity_{999.0f};
    float lastUnassignedActivity_{999.0f};
    static constexpr float LIVENESS_TIMEOUT = 300.0f;
    static constexpr unsigned short BEACON_PORT = 31337;

    // ── Remote workboard sync (Phase 2a) ──
    HashMap<Connection*, WbClientInfo> wbClients_;
    String wbSecret_;
    unsigned char pakeSecretHash_[32]{};
    bool pakeSecretValid_{false};
    unsigned lastWorkboardMtime_{0};
    unsigned lastPlanListHash_{0};
};
