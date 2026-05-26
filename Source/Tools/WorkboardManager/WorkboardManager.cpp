// WorkboardManager — GUI dashboard for workboard, plans, and Claude IPC

#include "WorkboardManager.h"

#include <Urho3D/Container/Sort.h>
#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Core/ProcessUtils.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/GraphicsEvents.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Input/InputEvents.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Network/SHA256.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#include "PlatformUtils.h"


#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <climits>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <windows.h>
#endif

URHO3D_DEFINE_APPLICATION_MAIN(WorkboardManager);

// Status bar title colors: reverse ROYGBIV
// Alpha explicitly 1.0 — never use * scalar to dim (it kills alpha)
static const Color COL_VIOLET(0.56f, 0.0f, 1.0f, 1.0f);
static const Color COL_INDIGO(0.29f, 0.0f, 0.82f, 1.0f);
static const Color COL_BLUE(0.0f, 0.47f, 1.0f, 1.0f);
static const Color COL_GREEN(0.0f, 1.0f, 0.0f, 1.0f);
static const Color COL_YELLOW(1.0f, 1.0f, 0.0f, 1.0f);
static const Color COL_ORANGE(1.0f, 0.5f, 0.0f, 1.0f);
static const Color COL_RED(1.0f, 0.0f, 0.0f, 1.0f);

// Dimmed versions for inactive/empty states (half brightness, full alpha)
static const Color COL_VIOLET_DIM(0.28f, 0.0f, 0.5f, 1.0f);
static const Color COL_INDIGO_DIM(0.15f, 0.0f, 0.41f, 1.0f);
static const Color COL_BLUE_DIM(0.0f, 0.24f, 0.5f, 1.0f);
static const Color COL_YELLOW_DIM(0.5f, 0.5f, 0.0f, 1.0f);
static const Color COL_ORANGE_DIM(0.5f, 0.25f, 0.0f, 1.0f);
static const Color COL_RED_DIM(0.5f, 0.0f, 0.0f, 1.0f);

// ============================================================================
// Application lifecycle
// ============================================================================

WorkboardManager::WorkboardManager(Context* context) : WorkboardBase(context), yukiLLM_(context) {}

void WorkboardManager::Setup()
{
    // ── Singleton guard ──
    {
        auto* setupFs = GetSubsystem<FileSystem>();
        String tempBase = setupFs ? setupFs->GetTemporaryDir() + "urho_claude/" : "/tmp/urho_claude/";
        if (setupFs)
        {
            setupFs->CreateDir(tempBase);
            setupFs->CreateDir(tempBase + "instances/");
        }

#ifndef _WIN32
        // Linux: abstract socket. bind() is atomic, namespace is
        // kernel-managed (no filesystem file to accidentally delete), and
        // the socket auto-closes when the process dies.
        singletonLockFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (singletonLockFd_ >= 0)
        {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            const char abstractName[] = "\0urho_claude_workboard_manager";
            memcpy(addr.sun_path, abstractName, sizeof(abstractName));
            socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + sizeof(abstractName);

            if (bind(singletonLockFd_, (struct sockaddr*)&addr, addrLen) != 0)
            {
                close(singletonLockFd_);
                singletonLockFd_ = -1;
                URHO3D_LOGERROR("WorkboardManager already running (singleton socket bound). Exiting.");
                exitCode_ = EXIT_FAILURE;
                return;
            }
            listen(singletonLockFd_, 1);
        }
        else
        {
            URHO3D_LOGERROR("Failed to create singleton socket. Exiting.");
            exitCode_ = EXIT_FAILURE;
            return;
        }
#else
        // Windows: named mutex. Auto-releases when the process dies.
        singletonMutex_ = CreateMutexA(nullptr, TRUE, "Global\\urho_claude_workboard_manager");
        if (!singletonMutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (singletonMutex_)
            {
                ReleaseMutex(singletonMutex_);
                CloseHandle(singletonMutex_);
                singletonMutex_ = nullptr;
            }
            URHO3D_LOGERROR("WorkboardManager already running (singleton mutex held). Exiting.");
            exitCode_ = EXIT_FAILURE;
            return;
        }
#endif

        // Singleton confirmed — write PID for other tools to read.
        {
            String pidPath = tempBase + "instances/manager.pid";
            File pidFile(context_, pidPath, FILE_WRITE);
            if (pidFile.IsOpen())
                pidFile.WriteLine(String(GetCurrentPID()));
        }
    }

    engineParameters_[EP_WINDOW_TITLE] = String("Workboard Manager v") + WORKBOARD_MANAGER_VERSION;
    engineParameters_[EP_WINDOW_ICON] = "Icons/WorkboardManager.png";
    engineParameters_[EP_WINDOW_WIDTH] = 1280;
    engineParameters_[EP_WINDOW_HEIGHT] = 800;
    engineParameters_[EP_FULL_SCREEN] = false;
    engineParameters_[EP_LOG_NAME] = "WorkboardManager.log";
    engineParameters_[EP_RESOURCE_PATHS] = "CoreData;Data";
    engineParameters_[EP_SOUND] = false;
    engineParameters_[EP_WINDOW_RESIZABLE] = true;
}

void WorkboardManager::Start()
{
    IgnoreSigPipe();

    // Cap FPS — this is a static dashboard, not a game
    engine_->SetMaxFps(30);
    engine_->SetMaxInactiveFps(10);

    // Initialize IPC paths from platform temp directory
    auto* fs0 = GetSubsystem<FileSystem>();
    ipcDir_ = fs0->GetTemporaryDir() + "urho_claude/";
    ttySockDir_ = ipcDir_ + "tty/";

    projectRoot_ = GetProjectRoot();

    auto* cache = GetSubsystem<ResourceCache>();
    auto* style = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();
    uiRoot->SetDefaultStyle(style);

    // Claudette-style warm dark background
    // Warm near-black — slightly warmer than pure black, less purple than Claudette's VTE palette
    GetSubsystem<Renderer>()->GetDefaultZone()->SetFogColor(Color(0.08f, 0.08f, 0.09f));

    // Scan available fonts
    auto* fs = GetSubsystem<FileSystem>();
    Vector<String> fontFiles;
    fs->ScanDir(fontFiles, projectRoot_ + "bin/Data/Fonts/", "*.ttf", SCAN_FILES, false);
    for (unsigned i = 0; i < fontFiles.Size(); ++i)
    {
        String name = fontFiles[i].Substring(0, fontFiles[i].FindLast('.'));
        // Skip SDF companion files
        if (!name.Empty())
            availableFonts_.Push(name);
    }
    Urho3D::Sort(availableFonts_.Begin(), availableFonts_.End());

    LoadThemePrefs();
    font_ = cache->GetResource<Font>("Fonts/" + currentFontName_ + ".ttf");
    if (!font_)
        font_ = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
    fontSize_ = currentFontSize_;  // sync base class font size

    CreateUI();

    // Open SQL backing — bootstrap from markdown if DB is empty or missing
    {
        String dbPath = ipcDir_ + "workboard.db";
        String schemaPath = projectRoot_ + "bin/Data/Workboard/workboard_schema.sql";
        if (workboardDB_.Open(dbPath, schemaPath))
        {
            // Check if DB is empty — if so, bootstrap from markdown
            Vector<WorkboardSection> dbSections = workboardDB_.LoadAllSections();
            bool dbEmpty = true;
            for (unsigned i = 0; i < dbSections.Size(); ++i)
            {
                if (!dbSections[i].rows.Empty())
                { dbEmpty = false; break; }
            }
            if (dbEmpty)
                AppendLog("System", "Workboard DB is empty — add tasks via IPC or Manager UI");

            // Boot cleanup — prune stale shared memories
            workboardDB_.PruneStaleMemories(7);
        }
    }

    LoadWorkboard();
    ScanPlanFiles();
    CreateIPCPaths();
    UpdateCoderCapText();  // Write initial cap file
    StartRelaySocket();
    RefreshInstanceStatus();

    // Notify any surviving instances that Manager is back online.
    // Deliver once per unique PID — dedup by process, not socket path.
    {
        const String onlineMsg = "=== WORKBOARD MANAGER ONLINE === The WorkboardManager is back. "
            "Message delivery restored. Resume normal operations.";
        HashSet<int> deliveredPIDs;

        auto deliverOnce = [&](const String& role)
        {
            int pid = ReadInstancePID(role);
            if (pid <= 0 || !IsProcessAlive(pid))
                return;
            if (deliveredPIDs.Contains(pid))
                return;
            deliveredPIDs.Insert(pid);
            SendToSocket(role, onlineMsg);
        };

        for (const String& role : knownCoderRoles_)
            deliverOnce(role);

        for (const String& role : knownUnassignedRoles_)
            deliverOnce(role);
    }

    // Parse --secret from command line for workboard sync auth
    {
        const Vector<String>& args = GetArguments();
        for (unsigned i = 0; i < args.Size(); ++i)
        {
            if (args[i] == "--secret" && i + 1 < args.Size())
            {
                wbSecret_ = args[i + 1];
                break;
            }
        }
    }

    // Pre-compute SHA-256 hash of shared secret for PAKE authentication
    if (!wbSecret_.Empty())
    {
        SHA256Hash(reinterpret_cast<const unsigned char*>(wbSecret_.CString()),
                   wbSecret_.Length(), pakeSecretHash_);
        pakeSecretValid_ = true;
        URHO3D_LOGINFO("PAKE secret hash computed (SHA-256)");
    }

    // Start beacon server on UDP 31337
    auto* network = GetSubsystem<Network>();
    if (network)
    {
        network->StartServer(BEACON_PORT);
        UpdateBeacon();
        AppendLog("System", "Beacon active on UDP port " + String(BEACON_PORT));

        // Register workboard remote events and subscribe to connection lifecycle
        RegisterWorkboardRemoteEvents();
        SubscribeToEvent(E_CLIENTCONNECTED, URHO3D_HANDLER(WorkboardManager, HandleClientConnected));
        SubscribeToEvent(E_CLIENTDISCONNECTED, URHO3D_HANDLER(WorkboardManager, HandleClientDisconnected));
        SubscribeToEvent(E_CLIENTIDENTITY, URHO3D_HANDLER(WorkboardManager, HandleClientIdentity));
        // PAKE authentication events
        SubscribeToEvent(E_KEYEXCHANGEAUTH, URHO3D_HANDLER(WorkboardManager, HandleKeyExchangeAuth));
        SubscribeToEvent(E_CLIENTAUTHENTICATED, URHO3D_HANDLER(WorkboardManager, HandleClientAuthenticated));
        // Client → Server remote events
        SubscribeToEvent(E_WB_REQUEST_PLAN, URHO3D_HANDLER(WorkboardManager, HandleWbRequestPlan));
        SubscribeToEvent(E_WB_MUTATION, URHO3D_HANDLER(WorkboardManager, HandleWbMutation));
        SubscribeToEvent(E_WB_SET_IDENTITY, URHO3D_HANDLER(WorkboardManager, HandleWbSetIdentity));
        SubscribeToEvent(E_WB_INSTANCE_STATUS, URHO3D_HANDLER(WorkboardManager, HandleWbInstanceStatus));

        if (!wbSecret_.Empty())
            AppendLog("System", "Workboard sync ready (LAN open, WAN PAKE auth enabled)");
        else
            AppendLog("System", "Workboard sync ready (LAN open, WAN blocked — use --secret to allow WAN)");
    }

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(WorkboardManager, HandleUpdate));
    SubscribeToEvent(E_KEYDOWN, URHO3D_HANDLER(WorkboardManager, HandleKeyDown));
    SubscribeToEvent(E_SCREENMODE, URHO3D_HANDLER(WorkboardManager, HandleScreenMode));

    GetSubsystem<Input>()->SetMouseVisible(true);
    GetSubsystem<Input>()->SetMouseGrabbed(false);

    AppendLog("System", "WorkboardManager started. Project root: " + projectRoot_);

    // ── Embedded Yuki: prepare wiring, but do NOT load model automatically ──
    // Model is unstable and runs hot — Leith loads it manually when needed.
    {
        String dbPath = projectRoot_ + "/bin/Data/GameDB/yuki_memory.db";
        if (yukiMemoryDB_.Open(dbPath))
        {
            yukiMemoryDB_.RecoverStaleTraining();
            yukiMemoryDB_.PruneConsumed(30);
        }

        yukiLLM_.SetProjectRoot(projectRoot_ + "/");
        yukiLLM_.SetMemoryDB(&yukiMemoryDB_);
        AppendLog("Yuki", "Ready (model not loaded — use UI to activate)");
    }
}

void WorkboardManager::Stop()
{
    // Broadcast shutdown notice to all live instances via TTY injection
    const String shutdownMsg = "=== WORKBOARD MANAGER SHUTTING DOWN === The WorkboardManager is temporarily offline. "
        "Continue your current task. TTY injection will resume when Manager restarts. "
        "Do NOT attempt to send messages to Manager until you receive a 'Manager back online' notice.";

    // Deliver once per unique PID — dedup by process, not socket path.
    {
        HashSet<int> deliveredPIDs;

        auto deliverOnce = [&](const String& role)
        {
            int pid = ReadInstancePID(role);
            if (pid <= 0 || !IsProcessAlive(pid))
                return;
            if (deliveredPIDs.Contains(pid))
                return;
            deliveredPIDs.Insert(pid);
            SendToSocket(role, shutdownMsg);
        };

        for (const String& role : knownCoderRoles_)
            deliverOnce(role);

        for (const String& role : knownUnassignedRoles_)
            deliverOnce(role);
    }

    // Notify remote workboard clients of graceful shutdown via the dedicated
    // event. Phase 2c: replaces the earlier shoehorn through MUTATION_ACK,
    // which clients couldn't distinguish from a regular mutation failure.
    for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
    {
        if (it->second_.authenticated_ && it->first_)
        {
            VariantMap data;
            data[WbServerShutdown::P_REASON] = String("Server shutting down");
            it->first_->SendRemoteEvent(E_WB_SERVER_SHUTDOWN, true, data);
            it->first_->Disconnect();
        }
    }
    wbClients_.Clear();

    auto* network = GetSubsystem<Network>();
    if (network)
        network->StopServer();

    StopRelaySocket();

    // Shut down embedded Yuki
    yukiLLM_.UnloadModel();
    yukiMemoryDB_.Close();

    // Only remove PID file if we own it (a rejected duplicate must not delete the real instance's file)
    if (exitCode_ == EXIT_SUCCESS)
        GetSubsystem<FileSystem>()->Delete(ipcDir_ + "instances/manager.pid");

    // Release singleton lock
#ifndef _WIN32
    if (singletonLockFd_ >= 0)
    {
        close(singletonLockFd_);
        singletonLockFd_ = -1;
    }
#else
    if (singletonMutex_)
    {
        ReleaseMutex(singletonMutex_);
        CloseHandle(singletonMutex_);
        singletonMutex_ = nullptr;
    }
#endif
}

#ifdef _WIN32
String WorkboardManager::PipeName(const String& role) const
{
    return "\\\\.\\pipe\\urho_claude_" + role;
}
#endif

// ============================================================================
// Helpers
// ============================================================================

// GetProjectRoot() and GetClaudeDir() are in WorkboardBase

String WorkboardManager::FindYukiModel()
{
    auto* fs = GetSubsystem<FileSystem>();
    String modelsDir = projectRoot_ + "/Source/Tools/YukiHoho/models/";

    // DeepSeek family only — Qwen models can't learn
    const char* candidates[] = {
        "deepseek-r1-7b-q4_k_m.gguf",
        "deepcoder-1.5b.gguf",
        "deepcoder-planner.gguf",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i)
    {
        String path = modelsDir + candidates[i];
        if (fs->FileExists(path))
            return path;
    }

    // Fallback: first .gguf in the directory
    Vector<String> files;
    fs->ScanDir(files, modelsDir, "*.gguf", SCAN_FILES, false);
    if (!files.Empty())
        return modelsDir + files[0];

    return String::EMPTY;
}

// ============================================================================
// UI Creation
// ============================================================================

void WorkboardManager::CreateUI()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    // UV-based layout — all panels expressed as fractions of window size.
    // Anchors recalculate automatically on resize. No pixel math needed.
    // All gaps use the same pad value — no overlap.
    //
    //   Row 0:  Status bar    0.000 – 0.100  (3 rows: instances, stats, buttons)
    //   Row 1:  Content       0.103 – 0.630  (workboard left, plans+yuki right)
    //   Row 2:  Composer      0.633 – 0.673
    //   Row 3:  Message log   0.676 – 0.997

    const float pad = 0.003f;  // ~4px at 1280

    const float row0Top = 0.0f;
    const float row0Bot = 0.130f;  // 4 rows: dropdowns, stats x2, buttons
    const float row1Top = row0Bot + pad;
    const float row1Bot = 0.630f;
    const float row2Top = row1Bot + pad;
    const float row2Bot = 0.700f;   // Composer needs ~56px for two 24px rows + padding
    const float row3Top = row2Bot + pad;
    const float row3Bot = 1.0f - pad;

    const float midX = 0.5f;  // left/right split
    const float rightSplit = 0.40f;  // plans/yuki split within right half

    // ── Instance status bar (top, full width) ──
    CreateInstanceStatusBar(uiRoot, pad, row0Top, 1.0f - pad, row0Bot);

    // ── Workboard window (left half) ──
    CreateWorkboardPanel(uiRoot, pad, row1Top, midX - pad, row1Bot);

    // ── Plans window (right half, upper) ──
    CreatePlanPanel(uiRoot, midX + pad, row1Top, 1.0f - pad, rightSplit);

    // ── Yuki chat panel (right half, lower) ──
    CreateYukiChatPanel(uiRoot, midX + pad, rightSplit + pad, 1.0f - pad, row1Bot);

    // ── Composer bar (full width) ──
    CreateComposer(uiRoot, pad, row2Top, 1.0f - pad, row2Bot);

    // ── Message log (full width, fills bottom) ──
    CreateMessageLog(uiRoot, pad, row3Top, 1.0f - pad, row3Bot);

    // ── Popups (hidden, toggled from status bar buttons) ──
    CreateToolsPopup();
    CreateSettingsPopup();
}

void WorkboardManager::CreateInstanceStatusBar(UIElement* parent, float minX, float minY, float maxX, float maxY)
{
    auto* bar = parent->CreateChild<Window>("StatusBar");
    bar->SetStyle("Window");
    bar->SetOpacity(0.6f);
    bar->SetEnableAnchor(true);
    bar->SetMinAnchor(minX, minY);
    bar->SetMaxAnchor(maxX, maxY);
    bar->SetMovable(false);
    bar->SetResizable(false);
    bar->SetLayout(LM_VERTICAL, 4, IntRect(6, 4, 6, 4));
    bar->SetMinHeight(110);

    // ── Row 1: Instance dropdowns + build status ──
    auto* row1 = bar->CreateChild<UIElement>("StatusRow1");
    row1->SetLayout(LM_HORIZONTAL, 6);
    row1->SetFixedHeight(22);

    localsDropdown_ = row1->CreateChild<DropDownList>("RolesDropdown");
    localsDropdown_->SetStyleAuto();
    localsDropdown_->SetResizePopup(true);
    localsDropdown_->SetMinSize(120, 20);
    localsDropdown_->SetLayoutFlexScale(Vector2(2.0f, 0.0f));

    remotesDropdown_ = row1->CreateChild<DropDownList>("RemotesDropdown");
    remotesDropdown_->SetStyleAuto();
    remotesDropdown_->SetResizePopup(true);
    remotesDropdown_->SetMinSize(100, 20);
    remotesDropdown_->SetLayoutFlexScale(Vector2(1.5f, 0.0f));

    unassignedStatusDropdown_ = row1->CreateChild<DropDownList>("UnassignedDropdown");
    unassignedStatusDropdown_->SetStyleAuto();
    unassignedStatusDropdown_->SetResizePopup(true);
    unassignedStatusDropdown_->SetMinSize(100, 20);
    unassignedStatusDropdown_->SetLayoutFlexScale(Vector2(1.5f, 0.0f));

    buildStatusText_ = row1->CreateChild<Text>("BuildStatus");
    buildStatusText_->SetFont(font_, currentFontSize_);
    buildStatusText_->SetText("");
    buildStatusText_->SetColor(COL_GREEN);
    buildStatusText_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));

    // ── Row 2a: System stats (CPU, GPU, RAM) — each with progress bar ──
    auto* row2a = bar->CreateChild<UIElement>("StatusRow2a");
    row2a->SetLayout(LM_HORIZONTAL, 8);
    row2a->SetFixedHeight(20);
    row2a->SetClipChildren(true);

    CreateStatCell(row2a, "Cpu", cpuText_, cpuBar_, Color(0.2f, 0.6f, 1.0f, 0.35f), 90, 20);
    CreateStatCell(row2a, "Gpu", gpuText_, gpuBar_, Color(0.2f, 0.8f, 0.3f, 0.35f), 90, 20);
    CreateStatCell(row2a, "Ram", ramText_, ramBar_, Color(0.9f, 0.6f, 0.1f, 0.35f), 180, 20);

    // ── Row 2b: System stats (Swap, Disk, Yuki) ──
    auto* row2b = bar->CreateChild<UIElement>("StatusRow2b");
    row2b->SetLayout(LM_HORIZONTAL, 8);
    row2b->SetFixedHeight(18);
    row2b->SetClipChildren(true);

    CreateStatCell(row2b, "Swap", swapText_, swapBar_, Color(0.7f, 0.3f, 0.9f, 0.35f), 0, 18);
    CreateStatCell(row2b, "Disk", diskText_, diskBar_, Color(0.9f, 0.3f, 0.3f, 0.35f), 0, 18);

    // Yuki gets plain text — no progress bar
    yukiCpuText_ = row2b->CreateChild<Text>("YukiCpu");
    yukiCpuText_->SetFont(font_, currentFontSize_);
    yukiCpuText_->SetText("Yuki: --");
    yukiCpuText_->SetColor(Color(1.0f, 0.5f, 0.8f));
    yukiCpuText_->SetVerticalAlignment(VA_CENTER);

    // ── Row 3: Buttons ──
    auto* row3 = bar->CreateChild<UIElement>("StatusRow3");
    row3->SetLayout(LM_HORIZONTAL, 6);
    row3->SetFixedHeight(24);

    toolsBtn_ = row3->CreateChild<Button>("ToolsBtn");
    toolsBtn_->SetStyleAuto();
    toolsBtn_->SetMinSize(60, 20);
    toolsBtn_->SetMaxWidth(120);
    toolsBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    toolsBtn_->SetClipChildren(true);
    auto* toolsBtnText = toolsBtn_->CreateChild<Text>();
    toolsBtnText->SetFont(font_, currentFontSize_);
    toolsBtnText->SetText("Tools");
    toolsBtnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(toolsBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleToolsToggle));

    settingsBtn_ = row3->CreateChild<Button>("SettingsBtn");
    settingsBtn_->SetStyleAuto();
    settingsBtn_->SetMinSize(60, 20);
    settingsBtn_->SetMaxWidth(120);
    settingsBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    settingsBtn_->SetClipChildren(true);
    auto* settingsBtnText = settingsBtn_->CreateChild<Text>();
    settingsBtnText->SetFont(font_, currentFontSize_);
    settingsBtnText->SetText("Settings");
    settingsBtnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(settingsBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSettingsToggle));
}

// CreateWorkboardPanel() is in WorkboardBase

// CreatePlanPanel() is in WorkboardBase

UIElement* WorkboardManager::CreateStatCell(UIElement* parent, const String& name, Text*& textOut,
                                            BorderImage*& barOut, const Color& barColor, int minW, int fixedH)
{
    auto* cell = parent->CreateChild<UIElement>(name + "Cell");
    cell->SetLayout(LM_FREE);
    cell->SetFixedHeight(fixedH);
    if (minW > 0)
        cell->SetMinWidth(minW);
    cell->SetClipChildren(true);

    // Bar fill — anchor-based so it scales with cell width automatically
    barOut = cell->CreateChild<BorderImage>(name + "Bar");
    barOut->SetColor(barColor);
    barOut->SetEnableAnchor(true);
    barOut->SetMinAnchor(0.0f, 0.0f);
    barOut->SetMaxAnchor(0.0f, 1.0f);  // starts empty
    barOut->SetPriority(0);

    // Text label — rendered on top
    textOut = cell->CreateChild<Text>(name + "Status");
    textOut->SetFont(font_, currentFontSize_);
    textOut->SetText(name + ": --");
    textOut->SetColor(COL_YELLOW);
    textOut->SetVerticalAlignment(VA_CENTER);
    textOut->SetPriority(1);

    return cell;
}

void WorkboardManager::SetBarPercent(BorderImage* bar, UIElement* /*cell*/, int pct)
{
    if (!bar)
        return;
    pct = Clamp(pct, 0, 100);
    bar->SetMaxAnchor(pct / 100.0f, 1.0f);
}

void WorkboardManager::CreateComposer(UIElement* parent, float minX, float minY, float maxX, float maxY)
{
    auto* bar = parent->CreateChild<BorderImage>("ComposerBar");
    bar->SetStyle("Window");
    bar->SetOpacity(0.6f);
    bar->SetEnableAnchor(true);
    bar->SetMinAnchor(minX, minY);
    bar->SetMaxAnchor(maxX, maxY);
    bar->SetLayout(LM_VERTICAL, 2, IntRect(4, 2, 4, 2));

    // ── Row 1: Message input + receiver + send ──
    auto* row1 = bar->CreateChild<UIElement>("ComposerRow1");
    row1->SetLayout(LM_HORIZONTAL, 4);
    row1->SetFixedHeight(24);

    // Message input
    messageInput_ = row1->CreateChild<LineEdit>("MsgInput");
    messageInput_->SetStyle("LineEdit");
    messageInput_->SetMinSize(100, 24);
    messageInput_->SetLayoutFlexScale(Vector2(3.0f, 0.0f));
    messageInput_->SetVerticalAlignment(VA_CENTER);

    // Receiver dropdown
    coderDropdown_ = row1->CreateChild<DropDownList>("ReceiverDropdown");
    coderDropdown_->SetStyleAuto();
    coderDropdown_->SetMinSize(80, 24);
    coderDropdown_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    coderDropdown_->SetResizePopup(true);
    coderDropdown_->SetVerticalAlignment(VA_CENTER);

    // Send
    sendCoderBtn_ = row1->CreateChild<Button>("SendBtn");
    sendCoderBtn_->SetStyleAuto();
    sendCoderBtn_->SetMinSize(60, 24);
    sendCoderBtn_->SetMaxWidth(140);
    sendCoderBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    sendCoderBtn_->SetVerticalAlignment(VA_CENTER);
    sendCoderBtn_->SetClipChildren(true);
    auto* cl = sendCoderBtn_->CreateChild<Text>();
    cl->SetFont(font_, currentFontSize_ - 1);
    cl->SetText("Send");
    cl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendCoder));

    // ── Row 2: Action buttons ──
    auto* row2 = bar->CreateChild<UIElement>("ComposerRow2");
    row2->SetLayout(LM_HORIZONTAL, 4);
    row2->SetFixedHeight(24);

    // Clear Locks
    clearFileLocksBtn_ = row2->CreateChild<Button>("ClearLocks");
    clearFileLocksBtn_->SetStyleAuto();
    clearFileLocksBtn_->SetMinSize(80, 24);
    clearFileLocksBtn_->SetMaxWidth(160);
    clearFileLocksBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    clearFileLocksBtn_->SetVerticalAlignment(VA_CENTER);
    clearFileLocksBtn_->SetClipChildren(true);
    auto* cfl = clearFileLocksBtn_->CreateChild<Text>();
    cfl->SetFont(font_, currentFontSize_ - 1);
    cfl->SetText("Break Locks");
    cfl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(clearFileLocksBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleClearFileLocks));

    // Spawn
    spawnCoderBtn_ = row2->CreateChild<Button>("SpawnCoder");
    spawnCoderBtn_->SetStyleAuto();
    spawnCoderBtn_->SetMinSize(60, 24);
    spawnCoderBtn_->SetMaxWidth(140);
    spawnCoderBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    spawnCoderBtn_->SetVerticalAlignment(VA_CENTER);
    spawnCoderBtn_->SetClipChildren(true);
    auto* sc = spawnCoderBtn_->CreateChild<Text>();
    sc->SetFont(font_, currentFontSize_ - 1);
    sc->SetText("Spawn");
    sc->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(spawnCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSpawnCoder));

    // Coder cap controls: [−] count/max [+]
    coderCapMinusBtn_ = row2->CreateChild<Button>("CoderCapMinus");
    coderCapMinusBtn_->SetStyleAuto();
    coderCapMinusBtn_->SetFixedSize(24, 24);
    coderCapMinusBtn_->SetLayoutFlexScale(Vector2(0.0f, 0.0f));
    coderCapMinusBtn_->SetVerticalAlignment(VA_CENTER);
    coderCapMinusBtn_->SetClipChildren(true);
    auto* capM = coderCapMinusBtn_->CreateChild<Text>();
    capM->SetFont(font_, currentFontSize_);
    capM->SetText("-");
    capM->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(coderCapMinusBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleCoderCapMinus));

    coderCapText_ = row2->CreateChild<Text>("CoderCapText");
    coderCapText_->SetFont(font_, currentFontSize_ - 1);
    coderCapText_->SetFixedWidth(30);
    coderCapText_->SetLayoutFlexScale(Vector2(0.0f, 0.0f));
    coderCapText_->SetAlignment(HA_CENTER, VA_CENTER);
    UpdateCoderCapText();

    coderCapPlusBtn_ = row2->CreateChild<Button>("CoderCapPlus");
    coderCapPlusBtn_->SetStyleAuto();
    coderCapPlusBtn_->SetFixedSize(24, 24);
    coderCapPlusBtn_->SetLayoutFlexScale(Vector2(0.0f, 0.0f));
    coderCapPlusBtn_->SetVerticalAlignment(VA_CENTER);
    coderCapPlusBtn_->SetClipChildren(true);
    auto* capP = coderCapPlusBtn_->CreateChild<Text>();
    capP->SetFont(font_, currentFontSize_);
    capP->SetText("+");
    capP->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(coderCapPlusBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleCoderCapPlus));

    // Screenshot toggle — blocks all Claude instances from taking screenshots
    screenshotToggleBtn_ = row2->CreateChild<Button>("ScreenshotToggle");
    screenshotToggleBtn_->SetStyleAuto();
    screenshotToggleBtn_->SetMinSize(80, 24);
    screenshotToggleBtn_->SetMaxWidth(160);
    screenshotToggleBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    screenshotToggleBtn_->SetVerticalAlignment(VA_CENTER);
    screenshotToggleBtn_->SetClipChildren(true);
    auto* ssText = screenshotToggleBtn_->CreateChild<Text>();
    ssText->SetFont(font_, currentFontSize_ - 1);
    // Check initial state
    // Default: screenshots blocked. Create flag file if absent.
    String ssFlag = ipcDir_ + "screenshots_blocked";
    if (!GetSubsystem<FileSystem>()->FileExists(ssFlag))
    {
        File flagFile(context_, ssFlag, FILE_WRITE);
        if (flagFile.IsOpen())
            flagFile.WriteLine("blocked");
    }
    screenshotsBlocked_ = GetSubsystem<FileSystem>()->FileExists(ssFlag);
    ssText->SetText(screenshotsBlocked_ ? "Snoop: OFF" : "Snoop: ON");
    ssText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(screenshotToggleBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleToggleScreenshots));
}

void WorkboardManager::CreateToolsPopup()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    toolsPopup_ = uiRoot->CreateChild<Window>("ToolsPopup");
    toolsPopup_->SetStyle("Window");
    toolsPopup_->SetColor(Color(0.09f, 0.078f, 0.129f));
    toolsPopup_->SetEnableAnchor(true);
    toolsPopup_->SetMinAnchor(0.3f, 0.04f);
    toolsPopup_->SetMaxAnchor(0.98f, 0.10f);
    toolsPopup_->SetLayout(LM_VERTICAL, 6, IntRect(8, 8, 8, 8));
    toolsPopup_->SetVisible(false);

    // ── Download row ──
    auto* dlRow = toolsPopup_->CreateChild<UIElement>("DLRow");
    dlRow->SetLayout(LM_HORIZONTAL, 4);
    dlRow->SetFixedHeight(28);

    auto* dlLabel = dlRow->CreateChild<Text>();
    dlLabel->SetFont(font_, currentFontSize_);
    dlLabel->SetText("curl:");
    dlLabel->SetColor(Color(0.7f, 0.7f, 0.7f));
    dlLabel->SetVerticalAlignment(VA_CENTER);

    downloadUrlInput_ = dlRow->CreateChild<LineEdit>("DLUrl");
    downloadUrlInput_->SetStyleAuto();
    downloadUrlInput_->SetMinWidth(100);
    downloadUrlInput_->SetMinHeight(24);
    downloadUrlInput_->SetLayoutFlexScale(Vector2(4.0f, 0.0f));

    downloadBtn_ = dlRow->CreateChild<Button>("DLBtn");
    downloadBtn_->SetStyleAuto();
    downloadBtn_->SetMinSize(60, 24);
    downloadBtn_->SetMaxWidth(120);
    downloadBtn_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    auto* btnText = downloadBtn_->CreateChild<Text>();
    btnText->SetFont(font_, currentFontSize_);
    btnText->SetText("Download");
    btnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(downloadBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleDownload));

    downloadStatusText_ = dlRow->CreateChild<Text>("DLStatus");
    downloadStatusText_->SetFont(font_, currentFontSize_ - 1);
    downloadStatusText_->SetText("");
    downloadStatusText_->SetColor(Color(0.5f, 0.8f, 0.5f));
    downloadStatusText_->SetAlignment(HA_RIGHT, VA_CENTER);
}

void WorkboardManager::HandleToolsToggle(StringHash, VariantMap&)
{
    if (toolsPopup_)
    {
        toolsPopup_->SetVisible(!toolsPopup_->IsVisible());
        if (settingsPopup_ && settingsPopup_->IsVisible())
            settingsPopup_->SetVisible(false);
    }
}

void WorkboardManager::CreateSettingsPopup()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    settingsPopup_ = uiRoot->CreateChild<Window>("SettingsPopup");
    settingsPopup_->SetStyle("Window");
    settingsPopup_->SetColor(Color(0.09f, 0.078f, 0.129f));
    settingsPopup_->SetEnableAnchor(true);
    settingsPopup_->SetMinAnchor(0.3f, 0.10f);
    settingsPopup_->SetMaxAnchor(0.98f, 0.16f);
    settingsPopup_->SetLayout(LM_VERTICAL, 6, IntRect(8, 8, 8, 8));
    settingsPopup_->SetVisible(false);

    // ── Theme row ──
    auto* themeRow = settingsPopup_->CreateChild<UIElement>("ThemeRow");
    themeRow->SetLayout(LM_HORIZONTAL, 8);
    themeRow->SetFixedHeight(28);

    auto* fontLabel = themeRow->CreateChild<Text>();
    fontLabel->SetFont(font_, currentFontSize_);
    fontLabel->SetText("Font:");
    fontLabel->SetColor(Color(0.7f, 0.7f, 0.7f));
    fontLabel->SetVerticalAlignment(VA_CENTER);

    fontSelector_ = themeRow->CreateChild<DropDownList>();
    fontSelector_->SetStyle("DropDownList");
    fontSelector_->SetMinSize(100, 22);
    fontSelector_->SetLayoutFlexScale(Vector2(2.0f, 0.0f));
    fontSelector_->SetResizePopup(true);

    int selectedIdx = 0;
    for (unsigned i = 0; i < availableFonts_.Size(); ++i)
    {
        auto* item = new Text(context_);
        item->SetFont(font_, currentFontSize_ + 1);
        item->SetText(availableFonts_[i]);
        item->SetColor(Color(0.85f, 0.85f, 0.85f));
        item->SetMinSize(200, 20);
        fontSelector_->AddItem(item);
        if (availableFonts_[i] == currentFontName_)
            selectedIdx = i;
    }
    fontSelector_->SetSelection(selectedIdx);
    SubscribeToEvent(fontSelector_, E_ITEMSELECTED, URHO3D_HANDLER(WorkboardManager, HandleFontSelected));

    auto* sizeLabel = themeRow->CreateChild<Text>();
    sizeLabel->SetFont(font_, currentFontSize_);
    sizeLabel->SetText("Size:");
    sizeLabel->SetColor(Color(0.7f, 0.7f, 0.7f));
    sizeLabel->SetVerticalAlignment(VA_CENTER);

    fontSizeSelector_ = themeRow->CreateChild<DropDownList>();
    fontSizeSelector_->SetStyle("DropDownList");
    fontSizeSelector_->SetMinSize(50, 22);
    fontSizeSelector_->SetMaxWidth(100);
    fontSizeSelector_->SetLayoutFlexScale(Vector2(1.0f, 0.0f));
    fontSizeSelector_->SetResizePopup(true);

    int sizes[] = {9, 10, 11, 12, 13, 14, 16, 18};
    int sizeSelectedIdx = 2;
    for (int i = 0; i < 8; ++i)
    {
        auto* item = new Text(context_);
        item->SetFont(font_, currentFontSize_ + 1);
        item->SetText(String(sizes[i]));
        item->SetColor(Color(0.85f, 0.85f, 0.85f));
        item->SetMinSize(60, 20);
        fontSizeSelector_->AddItem(item);
        if (sizes[i] == currentFontSize_)
            sizeSelectedIdx = i;
    }
    fontSizeSelector_->SetSelection(sizeSelectedIdx);
    SubscribeToEvent(fontSizeSelector_, E_ITEMSELECTED, URHO3D_HANDLER(WorkboardManager, HandleFontSizeChanged));
}

void WorkboardManager::HandleSettingsToggle(StringHash, VariantMap&)
{
    if (settingsPopup_)
    {
        settingsPopup_->SetVisible(!settingsPopup_->IsVisible());
        if (toolsPopup_ && toolsPopup_->IsVisible())
            toolsPopup_->SetVisible(false);
    }
}

void WorkboardManager::HandleDownload(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (downloadInProgress_)
    {
        AppendLog("Download", "Download already in progress");
        return;
    }

    String url = downloadUrlInput_->GetText().Trimmed();
    if (url.Empty())
    {
        AppendLog("Download", "No URL entered");
        return;
    }

    // Derive filename from URL
    String filename = url;
    unsigned lastSlash = filename.FindLast('/');
    if (lastSlash != String::NPOS)
        filename = filename.Substring(lastSlash + 1);
    // Strip query params
    unsigned qmark = filename.Find('?');
    if (qmark != String::NPOS)
        filename = filename.Substring(0, qmark);
    if (filename.Empty())
        filename = "download";

    // Download to project root's downloads/ dir
    String downloadDir = projectRoot_ + "/downloads";
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs->DirExists(downloadDir))
        fs->CreateDir(downloadDir);

    downloadOutputPath_ = downloadDir + "/" + filename;

    // Run curl in background via Urho3D async process
    {
        auto* fs = GetSubsystem<FileSystem>();
        Vector<String> args;
        args.Push("-L");
        args.Push("-o");
        args.Push(downloadOutputPath_);
        args.Push(url);
        curlRequestId_ = fs->SystemRunAsync("curl", args);
    }

    downloadInProgress_ = (curlRequestId_ > 0);
    downloadCheckTimer_ = 0.0f;
    if (downloadStatusText_)
    {
        downloadStatusText_->SetText("Downloading...");
        downloadStatusText_->SetColor(Color(1.0f, 0.9f, 0.3f));
    }
    AppendLog("Download", "Started: " + url + " → " + downloadOutputPath_);
}

void WorkboardManager::CheckDownloadProgress()
{
    if (!downloadInProgress_)
        return;

    // Throttle checks to once per second — no need to hammer the filesystem every frame
    downloadCheckTimer_ += GetSubsystem<Engine>()->GetNextTimeStep();
    if (downloadCheckTimer_ < 1.0f)
        return;
    downloadCheckTimer_ = 0.0f;

    auto* fs = GetSubsystem<FileSystem>();

    // Non-blocking check: if output file doesn't exist yet, curl is still running
    if (curlRequestId_ > 0)
    {
        if (!fs->FileExists(downloadOutputPath_))
            return;  // still running
        curlRequestId_ = 0;
    }

    // curl finished (or was never tracked) — check result
    {
        // curl finished
        downloadInProgress_ = false;

        if (fs->FileExists(downloadOutputPath_))
        {
            // Get file size
            File f(context_, downloadOutputPath_);
            unsigned size = f.GetSize();
            f.Close();

            String sizeStr;
            if (size > 1048576)
                sizeStr = String((int)(size / 1048576)) + " MB";
            else if (size > 1024)
                sizeStr = String((int)(size / 1024)) + " KB";
            else
                sizeStr = String(size) + " bytes";

            if (downloadStatusText_)
            {
                downloadStatusText_->SetText("Done: " + sizeStr);
                downloadStatusText_->SetColor(Color(0.3f, 1.0f, 0.5f));
            }
            AppendLog("Download", "Complete: " + downloadOutputPath_ + " (" + sizeStr + ")");
        }
        else
        {
            if (downloadStatusText_)
            {
                downloadStatusText_->SetText("Failed");
                downloadStatusText_->SetColor(Color(1.0f, 0.3f, 0.3f));
            }
            AppendLog("Download", "Failed — file not created. Check /tmp/urho_curl.log");
        }
    }
}

// ============================================================================
// Theme
// ============================================================================

void WorkboardManager::HandleFontSelected(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ItemSelected;
    unsigned sel = eventData[P_SELECTION].GetU32();
    if (sel < availableFonts_.Size())
    {
        currentFontName_ = availableFonts_[sel];
        ApplyFont(currentFontName_, currentFontSize_);
        SaveThemePrefs();
    }
}

void WorkboardManager::HandleFontSizeChanged(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ItemSelected;
    unsigned sel = eventData[P_SELECTION].GetU32();
    int sizes[] = {9, 10, 11, 12, 13, 14, 16, 18};
    if (sel < 8)
    {
        currentFontSize_ = sizes[sel];
        ApplyFont(currentFontName_, currentFontSize_);
        SaveThemePrefs();
    }
}

static void UpdateFontsRecursive(UIElement* element, Font* font, int oldBase, int newBase);

void WorkboardManager::ApplyFont(const String& fontName, int fontSize)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* newFont = cache->GetResource<Font>("Fonts/" + fontName + ".ttf");
    if (!newFont)
    {
        AppendLog("System", "Font not found: " + fontName);
        return;
    }
    int oldBase = currentFontSize_;
    font_ = newFont;
    currentFontName_ = fontName;
    currentFontSize_ = fontSize;

    auto* uiRoot = GetSubsystem<UI>()->GetRoot();
    UpdateFontsRecursive(uiRoot, font_, oldBase, currentFontSize_);
    // Update ListView items explicitly (they may not be in direct child tree)
    if (logListView_)
    {
        for (unsigned i = 0; i < logListView_->GetNumItems(); ++i)
        {
            auto* item = dynamic_cast<Text*>(logListView_->GetItem(i));
            if (item)
                item->SetFont(font_, currentFontSize_ - 1);
        }
    }
    if (planListView_)
    {
        for (unsigned i = 0; i < planListView_->GetNumItems(); ++i)
        {
            auto* item = dynamic_cast<Text*>(planListView_->GetItem(i));
            if (item)
                item->SetFont(font_, currentFontSize_);
        }
    }

    SaveThemePrefs();
    // Force workboard rebuild with new font
    RenderWorkboardUI();
    AppendLog("System", "Font changed to " + fontName + " " + String(fontSize) + "pt");
}

static void UpdateFontsRecursive(UIElement* element, Font* font, int oldBase, int newBase)
{
    auto* text = dynamic_cast<Text*>(element);
    if (text && text->GetFont())
    {
        int oldSize = text->GetFontSize();
        int offset = oldSize - oldBase;
        int newSize = newBase + offset;
        if (newSize < 7) newSize = 7;
        text->SetFont(font, newSize);
    }
    for (unsigned i = 0; i < element->GetNumChildren(); ++i)
        UpdateFontsRecursive(element->GetChild(i), font, oldBase, newBase);
}

void WorkboardManager::RebuildAllUI()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();
    UpdateFontsRecursive(uiRoot, font_, currentFontSize_, currentFontSize_);
}

void WorkboardManager::LoadThemePrefs()
{
    String path = projectRoot_ + "bin/Data/UI/theme.json";
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs->FileExists(path))
        return;

    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
        return;

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

    // Simple key:value parsing (no JSON lib needed)
    Vector<String> lines = content.Split('\n');
    for (unsigned i = 0; i < lines.Size(); ++i)
    {
        String line = lines[i].Trimmed();
        if (line.StartsWith("\"font\""))
        {
            unsigned colon = line.Find(':');
            if (colon != String::NPOS)
            {
                String val = line.Substring(colon + 1).Trimmed();
                val.Replace("\"", "");
                val.Replace(",", "");
                if (!val.Empty())
                    currentFontName_ = val.Trimmed();
            }
        }
        else if (line.StartsWith("\"fontSize\""))
        {
            unsigned colon = line.Find(':');
            if (colon != String::NPOS)
            {
                String val = line.Substring(colon + 1).Trimmed();
                val.Replace(",", "");
                int sz = atoi(val.Trimmed().CString());
                if (sz >= 8 && sz <= 24)
                    currentFontSize_ = sz;
            }
        }
    }
}

void WorkboardManager::SaveThemePrefs()
{
    String path = projectRoot_ + "bin/Data/UI/theme.json";

    // Ensure directory exists
    auto* fs = GetSubsystem<FileSystem>();
    String dir = path.Substring(0, path.FindLast('/'));
    if (!fs->DirExists(dir))
        fs->CreateDir(dir);

    File file(context_, path, FILE_WRITE);
    if (!file.IsOpen())
        return;

    String json = "{\n";
    json += "  \"font\": \"" + currentFontName_ + "\",\n";
    json += "  \"fontSize\": " + String(currentFontSize_) + "\n";
    json += "}\n";

    file.Write(json.CString(), json.Length());
    file.Close();
}

void WorkboardManager::CreateMessageLog(UIElement* parent, float minX, float minY, float maxX, float maxY)
{
    logPanel_ = parent->CreateChild<Window>("LogPanel");
    logPanel_->SetStyle("Window");
    logPanel_->SetOpacity(0.6f);
    logPanel_->SetEnableAnchor(true);
    logPanel_->SetMinAnchor(minX, minY);
    logPanel_->SetMaxAnchor(maxX, maxY);
    logPanel_->SetMovable(false);
    logPanel_->SetResizable(false);
    logPanel_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));

    auto* logTitle = logPanel_->CreateChild<Text>("LogTitle");
    logTitle->SetFont(font_, currentFontSize_ + 1);
    logTitle->SetText("MESSAGE LOG");
    logTitle->SetColor(Color(0.6f, 0.6f, 0.6f));
    logTitle->SetFixedHeight(20);
    logTitle->SetLayoutFlexScale(Vector2(0.0f, 0.0f));

    logListView_ = logPanel_->CreateChild<ListView>("LogList");
    logListView_->SetStyleAuto();
    logListView_->SetLayoutFlexScale(Vector2(1.0f, 1.0f));  // Fill remaining space
}

void WorkboardManager::CreateYukiChatPanel(UIElement* parent, float minX, float minY, float maxX, float maxY)
{
    yukiChatPanel_ = parent->CreateChild<Window>("YukiChat");
    yukiChatPanel_->SetStyle("Window");
    yukiChatPanel_->SetOpacity(0.6f);
    yukiChatPanel_->SetEnableAnchor(true);
    yukiChatPanel_->SetMinAnchor(minX, minY);
    yukiChatPanel_->SetMaxAnchor(maxX, maxY);
    yukiChatPanel_->SetMovable(false);
    yukiChatPanel_->SetResizable(false);
    yukiChatPanel_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));

    // Title row with load/unload button
    auto* titleRow = yukiChatPanel_->CreateChild<UIElement>("YukiTitleRow");
    titleRow->SetLayout(LM_HORIZONTAL, 6);
    titleRow->SetFixedHeight(24);
    titleRow->SetHorizontalAlignment(HA_LEFT);

    auto* title = titleRow->CreateChild<Text>("YukiTitle");
    title->SetFont(font_, currentFontSize_ + 1);
    title->SetText("YUKI");
    title->SetColor(Color(1.0f, 0.5f, 0.8f));
    title->SetVerticalAlignment(VA_CENTER);
    title->SetLayoutFlexScale(Vector2(0.0f, 0.0f));

    yukiToggleBtn_ = titleRow->CreateChild<Button>("YukiToggle");
    yukiToggleBtn_->SetStyleAuto();
    yukiToggleBtn_->SetFixedSize(80, 22);
    yukiToggleBtn_->SetVerticalAlignment(VA_CENTER);
    yukiToggleBtn_->SetLayoutFlexScale(Vector2(0.0f, 0.0f));
    yukiToggleBtn_->SetClipChildren(true);
    yukiToggleBtnText_ = yukiToggleBtn_->CreateChild<Text>();
    yukiToggleBtnText_->SetFont(font_, currentFontSize_ - 1);
    yukiToggleBtnText_->SetText("Load AI");
    yukiToggleBtnText_->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(yukiToggleBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleToggleYuki));

    yukiChatLog_ = yukiChatPanel_->CreateChild<ListView>("YukiLog");
    yukiChatLog_->SetStyleAuto();
    yukiChatLog_->SetMinHeight(60);
    yukiChatLog_->SetScrollBarsVisible(false, true);
}

void WorkboardManager::AppendYukiChat(const String& sender, const String& message)
{
    if (!yukiChatLog_) return;

    auto* item = new Text(context_);
    item->SetFont(font_, currentFontSize_ - 1);
    item->SetWordwrap(true);

    // Determine max width for word wrap — use panel width if available, fallback to 300
    int maxW = 300;
    if (yukiChatPanel_)
    {
        int w = yukiChatPanel_->GetWidth();
        if (w > 40)
            maxW = w - 20;
    }
    item->SetMaxWidth(maxW);

    if (sender == "Yuki" || sender == "yuki")
    {
        item->SetText("Yuki: " + message);
        item->SetColor(Color(1.0f, 0.5f, 0.8f));
    }
    else
    {
        item->SetText(sender + ": " + message);
        item->SetColor(Color(0.9f, 0.9f, 0.9f));
    }

    yukiChatLog_->AddItem(item);

    while (yukiChatLog_->GetNumItems() > 200)
        yukiChatLog_->RemoveItem((i32)0);

    yukiChatLog_->EnsureItemVisibility(yukiChatLog_->GetNumItems() - 1);
}

// ============================================================================
// Workboard Loading & Parsing
// ============================================================================

void WorkboardManager::LoadWorkboard()
{
    if (!workboardDB_.IsOpen())
        return;

    // SQL is the sole source of truth
    String dbPath = ipcDir_ + "workboard.db";
    auto* fs = GetSubsystem<FileSystem>();
    unsigned mtime = fs->FileExists(dbPath) ? fs->GetLastModifiedTime(dbPath) : 0;
    if (mtime != 0 && mtime == lastWriteMtime_)
        return;

    sections_ = workboardDB_.LoadAllSections();
    lastWriteMtime_ = mtime;
    RenderWorkboardUI();
}

// ParseWorkboard(), RenderWorkboardUI(), AddSectionToUI(), HandleSectionToggle()
// are all in WorkboardBase.

// HandlePlanSelected() is in WorkboardBase — calls virtual OnPlanSelected().
void WorkboardManager::OnPlanSelected(const String& filename)
{
    LoadPlanContent(filename);
}

// (ParseWorkboard, RenderWorkboardUI, AddSectionToUI, HandleSectionToggle
//  removed — now in WorkboardBase)

// ============================================================================
// Workboard Mutations (Manager is single authority)
// ============================================================================

WorkboardSection* WorkboardManager::FindSection(const String& keyword)
{
    for (unsigned i = 0; i < sections_.Size(); ++i)
    {
        if (sections_[i].title.Contains(keyword))
            return &sections_[i];
    }
    return nullptr;
}

void WorkboardManager::AddReadyRow(const Vector<String>& fields)
{
    WorkboardSection* sec = FindSection("Ready");
    if (!sec) { AppendLog("System", "WB: Ready section not found"); return; }

    WorkboardRow row;
    for (unsigned i = 0; i < fields.Size(); ++i)
        row.cells.Push(fields[i].Trimmed());
    sec->rows.Push(row);
    AppendLog("System", "WB: Added to Ready: " + fields[1].Trimmed());
}

void WorkboardManager::AddInProgressRow(const Vector<String>& fields)
{
    WorkboardSection* sec = FindSection("In Progress");
    if (!sec) { AppendLog("System", "WB: In Progress section not found"); return; }

    WorkboardRow row;
    for (unsigned i = 0; i < fields.Size(); ++i)
        row.cells.Push(fields[i].Trimmed());
    sec->rows.Push(row);
    AppendLog("System", "WB: Added to In Progress: " + fields[0].Trimmed());
}

void WorkboardManager::AddDoneRow(const Vector<String>& fields)
{
    WorkboardSection* sec = FindSection("Done");
    if (!sec) { AppendLog("System", "WB: Done section not found"); return; }

    WorkboardRow row;
    for (unsigned i = 0; i < fields.Size(); ++i)
        row.cells.Push(fields[i].Trimmed());
    sec->rows.Push(row);
    AppendLog("System", "WB: Added to Done: " + fields[0].Trimmed());
}

void WorkboardManager::MoveToDone(const String& taskName)
{
    WorkboardSection* inProg = FindSection("In Progress");
    WorkboardSection* done = FindSection("Done");
    if (!inProg || !done) { AppendLog("System", "WB: Section not found for move-done"); return; }

    for (unsigned i = 0; i < inProg->rows.Size(); ++i)
    {
        if (inProg->rows[i].cells.Size() > 0 && inProg->rows[i].cells[0].Contains(taskName))
        {
            WorkboardRow moved = inProg->rows[i];
            inProg->rows.Erase(i);
            done->rows.Push(moved);
            AppendLog("System", "WB: Moved to Done: " + taskName);
            return;
        }
    }
    AppendLog("System", "WB: Task not found in In Progress: " + taskName);
}

void WorkboardManager::RemoveRow(const String& matchText)
{
    for (unsigned s = 0; s < sections_.Size(); ++s)
    {
        for (unsigned r = 0; r < sections_[s].rows.Size(); ++r)
        {
            for (unsigned c = 0; c < sections_[s].rows[r].cells.Size(); ++c)
            {
                if (sections_[s].rows[r].cells[c].Contains(matchText))
                {
                    AppendLog("System", "WB: Removed row matching: " + matchText);
                    sections_[s].rows.Erase(r);
                    return;
                }
            }
        }
    }
    AppendLog("System", "WB: No row found matching: " + matchText);
}

void WorkboardManager::AddSharedFile(const Vector<String>& fields)
{
    // fields[0] = file path, fields[1] = reason
    WorkboardSection* sec = FindSection("Shared Files");
    if (!sec)
    {
        AppendLog("System", "WB: Shared Files section not found");
        return;
    }
    WorkboardRow row;
    row.cells.Push("`" + fields[0].Trimmed() + "`");
    row.cells.Push(fields[1].Trimmed());
    sec->rows.Push(row);
    AppendLog("System", "WB: Added shared file: " + fields[0].Trimmed());
}

void WorkboardManager::UpdateReview(const String& taskName, const String& newReview)
{
    for (unsigned s = 0; s < sections_.Size(); ++s)
    {
        int reviewCol = -1;
        for (unsigned h = 0; h < sections_[s].headers.Size(); ++h)
        {
            if (sections_[s].headers[h].ToLower().Trimmed() == "review")
            { reviewCol = h; break; }
        }
        if (reviewCol < 0) continue;

        for (unsigned r = 0; r < sections_[s].rows.Size(); ++r)
        {
            if (sections_[s].rows[r].cells.Size() > 0 &&
                sections_[s].rows[r].cells[0].Contains(taskName))
            {
                if ((unsigned)reviewCol < sections_[s].rows[r].cells.Size())
                {
                    sections_[s].rows[r].cells[reviewCol] = newReview;
                    AppendLog("System", "WB: Updated review for " + taskName + " -> " + newReview);
                    return;
                }
            }
        }
    }
    AppendLog("System", "WB: Task not found for review update: " + taskName);
}

bool WorkboardManager::AssignTask(const String& taskName, const String& coderRole)
{
    // Validate: coder must be alive
    if (!IsInstanceAlive(coderRole))
    {
        AppendLog("System", "WB assign REJECTED: Coder '" + coderRole + "' is not alive");
        return false;
    }

    // Validate: task must NOT already be in In Progress (first-wins conflict resolution)
    WorkboardSection* inProg = FindSection("In Progress");
    if (inProg)
    {
        for (unsigned i = 0; i < inProg->rows.Size(); ++i)
        {
            if (inProg->rows[i].cells.Size() > 0 && inProg->rows[i].cells[0].Contains(taskName))
            {
                AppendLog("System", "WB assign REJECTED: '" + taskName + "' already in In Progress");
                return false;
            }
        }
    }

    // Phase 2c: search Ready first, then Planned. Mirrors the shell wb-assign
    // behavior. The original code only searched Planned, so any client trying
    // to claim a Ready task via remote mutation would silently fail.
    WorkboardSection* sourceSec = nullptr;
    int foundIdx = -1;
    const char* sectionNames[] = { "Ready", "Planned" };
    for (const char* sname : sectionNames)
    {
        WorkboardSection* sec = FindSection(sname);
        if (!sec) continue;
        for (unsigned i = 0; i < sec->rows.Size(); ++i)
        {
            // Match by checking each cell — the task name typically lives in
            // the second cell ("Plan" column) for Ready/Planned, but tolerate
            // any column for forward compatibility.
            const WorkboardRow& row = sec->rows[i];
            for (unsigned c = 0; c < row.cells.Size(); ++c)
            {
                if (row.cells[c].Contains(taskName))
                {
                    sourceSec = sec;
                    foundIdx = (int)i;
                    break;
                }
            }
            if (foundIdx >= 0) break;
        }
        if (foundIdx >= 0) break;
    }

    if (foundIdx < 0 || !sourceSec)
    {
        AppendLog("System", "WB assign REJECTED: '" + taskName + "' not found in Ready or Planned");
        return false;
    }

    // Remove from source section (Ready or Planned)
    sourceSec->rows.Erase((unsigned)foundIdx);

    // Add to In Progress: | Task | Owner | Started | Review | Notes |
    if (!inProg)
    {
        AppendLog("System", "WB assign REJECTED: In Progress section not found");
        return false;
    }

    // Get today's date
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", t);

    WorkboardRow row;
    row.cells.Push(taskName);
    row.cells.Push(coderRole);
    row.cells.Push(String(dateBuf));
    row.cells.Push(String::EMPTY + "\xe2\x80\x94");  // em dash
    row.cells.Push("Assigned via wb-assign");
    inProg->rows.Push(row);

    AppendLog("System", "WB ASSIGNED: " + taskName + " -> " + coderRole);

    // Send TTY notification to the assigned coder
    String msg = "TASK ASSIGNED: " + taskName + " -- You own this. Check the workboard and start working.";
    SendToSocket(coderRole, msg);
    return true;
}

void WorkboardManager::EmitTableRows(String& output, const WorkboardSection* sec)
{
    for (unsigned r = 0; r < sec->rows.Size(); ++r)
    {
        output += "|";
        for (unsigned c = 0; c < sec->rows[r].cells.Size(); ++c)
        {
            String cell = sec->rows[r].cells[c];
            // Re-add backticks for File column
            if (c < sec->headers.Size() && sec->headers[c].ToLower().Trimmed() == "file"
                && !cell.StartsWith("`") && cell != "\u2014" && !cell.Empty())
                cell = "`" + cell + "`";
            output += " " + cell + " |";
        }
        output += "\n";
    }
}

String WorkboardManager::SerializeSectionsToMarkdown()
{
    String output;
    output += "# Workboard\n";

    for (unsigned s = 0; s < sections_.Size(); ++s)
    {
        const WorkboardSection& sec = sections_[s];
        output += "## " + sec.title + "\n";

        if (sec.headers.Size() > 0)
        {
            // Header row
            output += "|";
            for (unsigned h = 0; h < sec.headers.Size(); ++h)
                output += " " + sec.headers[h] + " |";
            output += "\n";

            // Separator
            output += "|";
            for (unsigned h = 0; h < sec.headers.Size(); ++h)
                output += "------|";
            output += "\n";

            // Data rows
            EmitTableRows(output, const_cast<WorkboardSection*>(&sec));
        }
        output += "\n";
    }
    return output;
}

void WorkboardManager::WriteWorkboard()
{
    // SQL is the sole authority — no markdown write-back needed.
    // Remote clients receive serialized sections via SerializeSectionsToMarkdown().
}

bool WorkboardManager::HandleWorkboardCommand(const String& message)
{
    URHO3D_LOGINFOF("HandleWorkboardCommand: '%s' (len=%u)", message.CString(), message.Length());
    if (!message.StartsWith("WB:"))
        return false;

    // Split: "WB:add-ready:1|Plan|..." -> command, payload
    unsigned firstColon = message.Find(':');
    unsigned secondColon = message.Find(':', firstColon + 1);

    String command;
    String payload;
    if (secondColon != String::NPOS)
    {
        command = message.Substring(firstColon + 1, secondColon - firstColon - 1).Trimmed();
        payload = message.Substring(secondColon + 1);
    }
    else
    {
        command = message.Substring(firstColon + 1).Trimmed();
    }

    Vector<String> fields = payload.Split('|');

    URHO3D_LOGINFOF("  command='%s', payload='%s', fields=%u", command.CString(), payload.CString(), fields.Size());

    // Download command — doesn't mutate workboard
    if (command == "download" && fields.Size() >= 2)
    {
        String url = fields[0].Trimmed();
        String dest = fields[1].Trimmed();

        if (downloadInProgress_)
        {
            AppendLog("Download", "Busy — download already in progress, queued: " + url);
            return true;
        }

        // Resolve destination relative to project root
        String fullDest = projectRoot_ + dest;

        // Ensure parent directory exists
        auto* fs = GetSubsystem<FileSystem>();
        String parentDir = fullDest.Substring(0, fullDest.FindLast('/'));
        if (!fs->DirExists(parentDir))
            fs->CreateDir(parentDir);

        downloadOutputPath_ = fullDest;

        // Non-blocking async curl via Urho3D
        {
            Vector<String> args;
            args.Push("-L");
            args.Push("-o");
            args.Push(downloadOutputPath_);
            args.Push(url);
            curlRequestId_ = fs->SystemRunAsync("curl", args);
        }

        downloadInProgress_ = (curlRequestId_ > 0);
        downloadCheckTimer_ = 0.0f;
        if (downloadStatusText_)
        {
            downloadStatusText_->SetText("Downloading...");
            downloadStatusText_->SetColor(Color(1.0f, 0.9f, 0.3f));
        }
        AppendLog("Download", "Started: " + url + " -> " + fullDest);
        return true;
    }

    bool mutationOk = true;

    if (command == "add-ready" && fields.Size() >= 6)
        AddReadyRow(fields);
    else if (command == "add-inprogress" && fields.Size() >= 5)
        AddInProgressRow(fields);
    else if (command == "add-done" && fields.Size() >= 5)
        AddDoneRow(fields);
    else if (command == "move-done" && !payload.Trimmed().Empty())
        MoveToDone(payload.Trimmed());
    else if (command == "remove" && !payload.Trimmed().Empty())
        RemoveRow(payload.Trimmed());
    else if (command == "add-shared" && fields.Size() >= 2)
        AddSharedFile(fields);
    else if (command == "assign" && fields.Size() >= 2)
        mutationOk = AssignTask(fields[0].Trimmed(), fields[1].Trimmed());
    else if (command == "update-review" && fields.Size() >= 2)
        UpdateReview(fields[0].Trimmed(), fields[1].Trimmed());
    else
    {
        AppendLog("System", "Unknown or malformed WB command: " + message);
        return false;
    }

    if (mutationOk)
    {
        // Persist in-memory state to SQL (incremental sync)
        if (workboardDB_.IsOpen())
            workboardDB_.SyncFromSections(sections_);

        RenderWorkboardUI();
    }
    return mutationOk;
}

// ============================================================================
// Plan Files
// ============================================================================

void WorkboardManager::ScanPlanFiles()
{
    String claudeDir = GetClaudeDir();
    auto* fs = GetSubsystem<FileSystem>();

    Vector<String> files;
    fs->ScanDir(files, claudeDir, "PLAN_*.md", SCAN_FILES, false);
    Urho3D::Sort(files.Begin(), files.End());

    // Skip rebuild if file list hasn't changed — avoids scroll reset
    if (files == planFiles_)
        return;

    planFiles_ = files;

    if (planListView_)
    {
        // Preserve selection
        unsigned prevSel = planListView_->GetSelection();

        planListView_->RemoveAllItems();
        for (unsigned i = 0; i < planFiles_.Size(); ++i)
        {
            auto* item = new Text(context_);
            item->SetFont(font_, currentFontSize_);
            item->SetText(planFiles_[i]);
            item->SetColor(Color(0.7f, 0.85f, 1.0f));
            planListView_->AddItem(item);
        }

        // Restore selection if still valid
        if (prevSel < planListView_->GetNumItems())
            planListView_->SetSelection(prevSel);
    }
}

// HandlePlanSelected() is in WorkboardBase — calls virtual OnPlanSelected()

void WorkboardManager::LoadPlanContent(const String& filename)
{
    String path = GetClaudeDir() + filename;
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs->FileExists(path))
    {
        if (planContentText_)
            planContentText_->SetText("File not found: " + path);
        return;
    }

    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
    {
        if (planContentText_)
            planContentText_->SetText("Failed to open: " + filename);
        return;
    }

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

    currentPlanFile_ = filename;
    if (planContentText_)
    {
        if (planPanel_)
            planContentText_->SetFixedWidth(planPanel_->GetWidth() - 20);
        planContentText_->SetText(content);
    }
}

// ============================================================================
// IPC — Drop-files (outgoing) + FIFOs (incoming)
// ============================================================================

void WorkboardManager::CreateIPCPaths()
{
    auto* fs = GetSubsystem<FileSystem>();
    fs->CreateDir(ipcDir_);
    fs->CreateDir(ipcDir_ + "instances");
    fs->CreateDir(ttySockDir_);
}



// ============================================================================
// Instance discovery & wake-up
// ============================================================================

int WorkboardManager::ReadInstancePID(const String& role)
{
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // Primary: read <role>.pid file
    String path = instDir + role + ".pid";
    if (fs->FileExists(path))
    {
        File f(context_, path);
        if (f.IsOpen())
        {
            int pid = atoi(f.ReadLine().Trimmed().CString());
            if (pid > 0)
                return pid;
        }
    }

    // Phase 2 cutover: .role files no longer written. Direct .pid lookup
    // is now the canonical path; the fallback scan was for the legacy .role
    // format and is gone. .pid files are keyed by role name and contain
    // PID on line 1, TTY_ID on line 2 (Phase 1 dual-write format).

    return -1;
}

bool WorkboardManager::IsInstanceAlive(const String& role)
{
    // PID is the authority — if the process is alive, it's alive
    int pid = ReadInstancePID(role);
    if (pid > 0 && IsProcessAlive(pid))
        return true;

    // Dead — clean up stale PID file
    if (pid > 0)
        CleanupStalePID(role, pid);

    return false;
}

String WorkboardManager::ReadHealthState(const String& role)
{
    auto* fs = GetSubsystem<FileSystem>();
    String healthPath = ipcDir_ + "/health/" + role + ".json";
    if (!fs->FileExists(healthPath))
        return String::EMPTY;

    File file(context_, healthPath, FILE_READ);
    if (!file.IsOpen())
        return String::EMPTY;

    // Simple JSON parse — just extract "state": "VALUE"
    String content;
    while (!file.IsEof())
        content += file.ReadLine() + "\n";
    unsigned pos = content.Find("\"state\"");
    if (pos == String::NPOS)
        return String::EMPTY;

    unsigned colon = content.Find(":", pos);
    if (colon == String::NPOS)
        return String::EMPTY;

    unsigned quote1 = content.Find("\"", colon + 1);
    if (quote1 == String::NPOS)
        return String::EMPTY;

    unsigned quote2 = content.Find("\"", quote1 + 1);
    if (quote2 == String::NPOS)
        return String::EMPTY;

    return content.Substring(quote1 + 1, quote2 - quote1 - 1);
}

String WorkboardManager::ReadBuildStatus()
{
    auto* fs = GetSubsystem<FileSystem>();
    String statusPath = ipcDir_ + "/build_active.json";
    if (!fs->FileExists(statusPath))
        return String::EMPTY;

    File file(context_, statusPath, FILE_READ);
    if (!file.IsOpen())
        return String::EMPTY;

    String content;
    while (!file.IsEof())
        content += file.ReadLine() + "\n";

    // Extract role and target from JSON
    String role, target;
    unsigned pos = content.Find("\"role\"");
    if (pos != String::NPOS)
    {
        unsigned q1 = content.Find("\"", content.Find(":", pos) + 1);
        unsigned q2 = (q1 != String::NPOS) ? content.Find("\"", q1 + 1) : String::NPOS;
        if (q1 != String::NPOS && q2 != String::NPOS)
            role = content.Substring(q1 + 1, q2 - q1 - 1);
    }
    pos = content.Find("\"target\"");
    if (pos != String::NPOS)
    {
        unsigned q1 = content.Find("\"", content.Find(":", pos) + 1);
        unsigned q2 = (q1 != String::NPOS) ? content.Find("\"", q1 + 1) : String::NPOS;
        if (q1 != String::NPOS && q2 != String::NPOS)
            target = content.Substring(q1 + 1, q2 - q1 - 1);
    }

    if (target.Empty())
        return String::EMPTY;

    return "BUILD: " + role + " -> " + target;
}

void WorkboardManager::RefreshInstanceStatus()
{
    // Self-repair: if our PID file or relay socket was swept by a janitor,
    // restore them. We're alive — the janitor was wrong to remove them.
    auto* fs = GetSubsystem<FileSystem>();
    {
        String pidPath = ipcDir_ + "instances/manager.pid";
        if (!fs->FileExists(pidPath))
        {
            File pidFile(context_, pidPath, FILE_WRITE);
            if (pidFile.IsOpen())
            {
                pidFile.WriteLine(String(GetCurrentPID()));
                URHO3D_LOGINFO("Self-repair: restored instances/manager.pid");
            }
        }

#ifndef _WIN32
        String sockPath = ttySockDir_ + "manager_relay.sock";
        if (relayListenFd_ >= 0 && !fs->FileExists(sockPath))
        {
            // Socket FD is still valid but the filesystem entry was deleted.
            // Must close and re-bind to recreate the socket file.
            URHO3D_LOGINFO("Self-repair: relay socket swept — rebinding");
            close(relayListenFd_);
            relayListenFd_ = -1;
            StartRelaySocket();
        }
        else if (relayListenFd_ < 0)
        {
            // Socket was never started or failed — try again
            URHO3D_LOGINFO("Self-repair: relay socket down — restarting");
            StartRelaySocket();
        }
#else
        // Windows named pipes live in kernel namespace — no filesystem entry
        // to sweep. Just check if the handle went bad.
        if (relayPipeHandle_ == INVALID_HANDLE_VALUE)
        {
            URHO3D_LOGINFO("Self-repair: relay pipe down — restarting");
            StartRelaySocket();
        }
#endif
    }

    // Sweep orphaned .role files from dead sessions before checking liveness
    SweepStaleRoleFiles();

    bool anyChanged = false;

    // Update build status indicator
    if (buildStatusText_)
    {
        String buildStatus = ReadBuildStatus();
        if (buildStatus != buildStatusText_->GetText())
        {
            buildStatusText_->SetText(buildStatus);
            buildStatusText_->SetColor(buildStatus.Empty()
                ? COL_GREEN
                : COL_GREEN);
        }
    }

    // --- Unified roles dropdown (all instances with PIDs) ---
    if (localsDropdown_)
    {
        unsigned prevSel = localsDropdown_->GetSelection();
        localsDropdown_->RemoveAllItems();

        auto* fs = GetSubsystem<FileSystem>();
        String instDir = ipcDir_ + "instances/";

        auto addRoleItem = [&](const String& role, int pid, bool alive, const Color& color) {
            auto* item = new Text(context_);
            item->SetFont(font_, currentFontSize_);
            String label = role + " (" + String(pid) + ")";
            item->SetText(label);
            item->SetColor(alive ? color : Color(0.4f, 0.4f, 0.4f));
            item->SetAlignment(HA_LEFT, VA_CENTER);
            item->SetMinSize(180, 18);
            localsDropdown_->AddItem(item);
        };

        // Scan all PID files
        Vector<String> pidFiles;
        fs->ScanDir(pidFiles, instDir, "*.pid", SCAN_FILES, false);
        Sort(pidFiles.Begin(), pidFiles.End());

        unsigned aliveCount = 0;
        for (const String& filename : pidFiles)
        {
            String role = filename.Substring(0, filename.FindLast('.'));
            if (role == "manager")
                continue;  // Don't list ourselves

            File f(context_, instDir + filename);
            if (!f.IsOpen()) continue;
            int pid = atoi(f.ReadLine().Trimmed().CString());
            if (pid <= 0) continue;

            bool alive = IsProcessAlive(pid);
            if (alive) ++aliveCount;

            Color color;
            if (role == "yuki")
                color = Color(1.0f, 0.5f, 0.8f);
            else if (role.StartsWith("coder"))
                color = Color(0.3f, 0.9f, 1.0f);
            else if (role.StartsWith("unassigned"))
                color = COL_RED;
            else
                color = COL_YELLOW;

            addRoleItem(role, pid, alive, color);
        }

        // Header shows count
        if (localsDropdown_->GetNumItems() > 0)
        {
            auto* first = dynamic_cast<Text*>(localsDropdown_->GetItem(0));
            if (first)
            {
                // Prepend count to first item isn't right — add header
            }
        }

        if (prevSel < localsDropdown_->GetNumItems())
            localsDropdown_->SetSelection(prevSel);
        else if (localsDropdown_->GetNumItems() > 0)
            localsDropdown_->SetSelection(0);
    }

    // --- Remotes (network WorkboardClient connections) ---
    if (remotesDropdown_)
    {
        unsigned prevSel = remotesDropdown_->GetSelection();
        remotesDropdown_->RemoveAllItems();

        auto* header = new Text(context_);
        header->SetFont(font_, currentFontSize_);
        header->SetText("Remotes: " + String(wbClients_.Size()));
        header->SetColor(wbClients_.Empty() ? COL_ORANGE_DIM : COL_ORANGE);
        header->SetMinSize(95, 18);
        remotesDropdown_->AddItem(header);

        for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
        {
            const WbClientInfo& info = it->second_;
            auto* item = new Text(context_);
            item->SetFont(font_, currentFontSize_);
            item->SetMinSize(95, 18);

            String label = info.name_.Empty() ? "unknown" : info.name_;
            // Show remote instance counts if available
            if (info.remoteCoderCount_ > 0 || info.remoteYukiAlive_)
            {
                label += " (Y:" + String(info.remoteYukiAlive_ ? 1 : 0) +
                         " C:" + String(info.remoteCoderCount_) + ")";
            }
            else if (!info.role_.Empty())
                label += " (" + info.role_ + ")";
            if (info.authenticated_)
            {
                item->SetText(label);
                item->SetColor(Color(0.3f, 1.0f, 0.5f));
            }
            else
            {
                item->SetText(label + " [auth...]");
                item->SetColor(Color(0.8f, 0.6f, 0.2f));
            }
            remotesDropdown_->AddItem(item);
        }

        if (prevSel < remotesDropdown_->GetNumItems())
            remotesDropdown_->SetSelection(prevSel);
        else if (remotesDropdown_->GetNumItems() > 0)
            remotesDropdown_->SetSelection(0);
    }

    // --- Unassigned (dynamic, multiple) ---
    Vector<String> liveUnassigned = DiscoverUnassignedRoles();

    if (liveUnassigned != knownUnassignedRoles_)
    {
        anyChanged = true;

        knownUnassignedRoles_ = liveUnassigned;
    }

    if (unassignedStatusDropdown_)
    {
        unsigned prevSel = unassignedStatusDropdown_->GetSelection();
        unassignedStatusDropdown_->RemoveAllItems();

        if (knownUnassignedRoles_.Empty())
        {
            auto* item = new Text(context_);
            item->SetFont(font_, currentFontSize_);
            item->SetText("Unassigned: 0");
            item->SetColor(COL_RED_DIM);
            item->SetMinSize(95, 18);
            unassignedStatusDropdown_->AddItem(item);
        }
        else
        {
            auto* countItem = new Text(context_);
            countItem->SetFont(font_, currentFontSize_);
            countItem->SetText("Unassigned: " + String(knownUnassignedRoles_.Size()));
            countItem->SetColor(COL_RED);
            countItem->SetMinSize(95, 18);
            unassignedStatusDropdown_->AddItem(countItem);

            for (const String& role : knownUnassignedRoles_)
            {
                if (!IsInstanceAlive(role))
                    continue;

                String label = role;
                if (!label.Empty())
                    label[0] = (char)toupper(label[0]);

                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetMinSize(95, 18);
                item->SetText(label);
                item->SetColor(COL_RED);
                unassignedStatusDropdown_->AddItem(item);
            }
        }

        if (prevSel < unassignedStatusDropdown_->GetNumItems())
            unassignedStatusDropdown_->SetSelection(prevSel);
        else if (unassignedStatusDropdown_->GetNumItems() > 0)
            unassignedStatusDropdown_->SetSelection(0);
    }

    // --- Coders (dynamic, multiple) ---
    Vector<String> liveCoders = DiscoverCoderRoles();

    // Check if the set of coder roles changed
    if (liveCoders != knownCoderRoles_)
    {
        anyChanged = true;

        knownCoderRoles_ = liveCoders;
        UpdateCoderCapText();

        // Rebuild unified receiver dropdown
        if (coderDropdown_)
        {
            unsigned prevSelection = coderDropdown_->GetSelection();
            coderDropdown_->RemoveAllItems();

            // Helper to add a centered, colored dropdown item
            auto addReceiverItem = [&](const String& label, const Color& color) {
                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetText(label);
                item->SetColor(color);
                item->SetAlignment(HA_CENTER, VA_CENTER);
                item->SetStyleAuto();
                coderDropdown_->AddItem(item);
            };

            // Dynamic: live coders
            for (const String& role : knownCoderRoles_)
                addReceiverItem(role, Color(0.3f, 0.9f, 1.0f));
            // Static entries
            addReceiverItem("yuki", Color(1.0f, 0.5f, 0.8f));
            // Unassigned excluded from send targets — ghosts of living PIDs
            addReceiverItem("broadcast", Color(1.0f, 0.55f, 0.45f));

            if (prevSelection < coderDropdown_->GetNumItems())
                coderDropdown_->SetSelection(prevSelection);
            else if (coderDropdown_->GetNumItems() > 0)
                coderDropdown_->SetSelection(0);
        }
    }

    // Rebuild status bar dropdown with current PID info
    if (coderStatusDropdown_)
    {
        unsigned prevSel = coderStatusDropdown_->GetSelection();
        coderStatusDropdown_->RemoveAllItems();

        if (knownCoderRoles_.Empty())
        {
            auto* item = new Text(context_);
            item->SetFont(font_, currentFontSize_);
            item->SetText("Coders: 0");
            item->SetColor(COL_INDIGO_DIM);
            item->SetMinSize(95, 18);
            coderStatusDropdown_->AddItem(item);
        }
        else
        {
            // Header: count only
            auto* header = new Text(context_);
            header->SetFont(font_, currentFontSize_);
            header->SetMinSize(95, 18);
            header->SetText("Coders: " + String(knownCoderRoles_.Size()));
            header->SetColor(COL_INDIGO);
            coderStatusDropdown_->AddItem(header);

            for (const String& role : knownCoderRoles_)
            {
                if (!IsInstanceAlive(role))
                    continue;  // dead ones don't show

                String label = role;
                if (!label.Empty())
                    label[0] = (char)toupper(label[0]);

                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetMinSize(95, 18);
                item->SetText(label);

                String health = ReadHealthState(role);
                if (health == "STUCK")
                    item->SetColor(COL_RED);
                else if (health == "BUSY")
                    item->SetColor(COL_ORANGE);
                else
                    item->SetColor(COL_INDIGO);

                coderStatusDropdown_->AddItem(item);
            }
        }

        // When multiple coders, select the summary header (index 0)
        if (knownCoderRoles_.Size() > 1)
            coderStatusDropdown_->SetSelection(0);
        else if (prevSel < coderStatusDropdown_->GetNumItems())
            coderStatusDropdown_->SetSelection(prevSel);
        else if (coderStatusDropdown_->GetNumItems() > 0)
            coderStatusDropdown_->SetSelection(0);
    }

    // Auto-spawn disabled — Leith spawns coders manually

    if (anyChanged)
        UpdateBeacon();
}


bool WorkboardManager::SendToSocket(const String& role, const String& message, const String& excludeRole)
{
#ifndef _WIN32
    // Broadcast to all coders: scan coder*.pid, send to each live one
    if (role == "coders")
    {
        Vector<String> pidFiles;
        auto* fs = GetSubsystem<FileSystem>();
        String instDir = ipcDir_ + "instances/";
        fs->ScanDir(pidFiles, instDir, "coder*.pid", SCAN_FILES, false);

        int delivered = 0;
        HashSet<int> seen;  // deduplicate by PID (stale files may share a PID)
        for (unsigned i = 0; i < pidFiles.Size(); ++i)
        {
            String coderRole = pidFiles[i];
            coderRole.Replace(".pid", "");
            if (!excludeRole.Empty() && coderRole == excludeRole)
                continue;

            int pid = ReadInstancePID(coderRole);
            if (pid <= 0 || !IsProcessAlive(pid) || seen.Contains(pid))
                continue;
            seen.Insert(pid);

            if (SendToSocket(coderRole, message))
                ++delivered;
        }

        if (delivered > 0)
        {
            URHO3D_LOGINFOF("SendToSocket: broadcast 'coders' delivered to %d instance(s)", delivered);
            return true;
        }
        URHO3D_LOGWARNING("SendToSocket: broadcast 'coders' found no live instances");
        return false;
    }

    // Single-target: resolve role → PID → find socket owned by that PID
    int targetPid = ReadInstancePID(role);
    if (targetPid <= 0 || !IsProcessAlive(targetPid))
    {
        URHO3D_LOGWARNINGF("SendToSocket: '%s' not alive (PID %d)", role.CString(), targetPid);
        return false;
    }

    // Scan tty socket dir for a socket this PID owns
    String sockPath;
    {
        auto* fs = GetSubsystem<FileSystem>();
        Vector<String> sockFiles;
        fs->ScanDir(sockFiles, ttySockDir_, "*.sock", SCAN_FILES, false);
        String pidStr = String(targetPid);

        for (unsigned i = 0; i < sockFiles.Size(); ++i)
        {
            String candidate = ttySockDir_ + sockFiles[i];
            struct stat cst;
            if (stat(candidate.CString(), &cst) != 0 || !S_ISSOCK(cst.st_mode))
                continue;

            // Try connecting — if the target PID is listening, it will accept
            int probe = socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0) continue;

            struct sockaddr_un probeAddr;
            memset(&probeAddr, 0, sizeof(probeAddr));
            probeAddr.sun_family = AF_UNIX;
            strncpy(probeAddr.sun_path, candidate.CString(), sizeof(probeAddr.sun_path) - 1);

            if (connect(probe, (struct sockaddr*)&probeAddr, sizeof(probeAddr)) == 0)
            {
                // Connected — check if this socket's listener is our target PID
                // by reading /proc/net/unix or using fuser-style lookup.
                // Simpler: if the socket filename contains the PID, it's ours.
                // Claudette names sockets as {role}_{pid}.sock or spawn_{n}.sock.
                // For spawn sockets, check the companion .pid file.
                close(probe);

                // Check spawn_N.pid companion file
                String baseName = sockFiles[i];
                baseName.Replace(".sock", ".pid");
                String pidFile = ttySockDir_ + baseName;
                if (fs->FileExists(pidFile))
                {
                    File pf(context_, pidFile);
                    if (pf.IsOpen())
                    {
                        int ownerPid = atoi(pf.ReadLine().Trimmed().CString());
                        if (ownerPid == targetPid)
                        {
                            sockPath = candidate;
                            break;
                        }
                    }
                    continue;
                }

                // Check if socket name contains the PID (Claudette pattern: unassigned_PID.sock)
                if (sockFiles[i].Contains(pidStr))
                {
                    sockPath = candidate;
                    break;
                }
            }
            else
                close(probe);
        }
    }

    if (sockPath.Empty())
    {
        URHO3D_LOGWARNINGF("SendToSocket: no socket found for '%s' (PID %d)", role.CString(), targetPid);
        return false;
    }

    struct stat st;
    if (stat(sockPath.CString(), &st) != 0 || !S_ISSOCK(st.st_mode))
    {
        URHO3D_LOGWARNINGF("SendToSocket: resolved path not a socket for '%s'", role.CString());
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { close(fd); return false; }

    String flat = message;
    flat.Replace("\n", " ");
    flat.Replace("\r", "");

    ssize_t written = write(fd, flat.CString(), flat.Length());
    close(fd);

    if (written < 0) return false;

    URHO3D_LOGINFOF("SendToSocket: sent %d bytes to %s", (int)written, role.CString());
    return true;
#else
    // ── Windows: named pipes ──
    // Pipe names map 1:1 with Unix socket files: \\.\pipe\urho_claude_{role}

    auto sendToPipe = [&](const String& pipeName, const String& msg) -> bool
    {
        HANDLE hPipe = CreateFileA(
            pipeName.CString(),
            GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE)
            return false;

        String flat = msg;
        flat.Replace("\n", " ");
        flat.Replace("\r", "");

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, flat.CString(), (DWORD)flat.Length(), &written, nullptr);
        CloseHandle(hPipe);
        return ok && written > 0;
    };

    if (role == "coders")
    {
        // Broadcast: try coder, coder2, coder3, ... coder16
        int delivered = 0;
        const char* names[] = { "coder", "coder2", "coder3", "coder4",
                                "coder5", "coder6", "coder7", "coder8",
                                "coder9", "coder10", "coder11", "coder12",
                                "coder13", "coder14", "coder15", "coder16" };
        for (int i = 0; i < 16; ++i)
        {
            if (!excludeRole.Empty() && excludeRole == names[i])
                continue;
            if (sendToPipe(PipeName(names[i]), message))
            {
                ++delivered;
                URHO3D_LOGINFOF("SendToSocket [broadcast]: %s", names[i]);
            }
        }
        if (delivered > 0)
        {
            URHO3D_LOGINFOF("SendToSocket: broadcast 'coders' delivered to %d pipe(s)", delivered);
            return true;
        }
        URHO3D_LOGWARNING("SendToSocket: broadcast 'coders' found no live pipes");
        return false;
    }

    // Single target
    if (sendToPipe(PipeName(role), message))
    {
        URHO3D_LOGINFOF("SendToSocket: sent to %s", role.CString());
        return true;
    }
    URHO3D_LOGWARNINGF("SendToSocket: no pipe for '%s'", role.CString());
    return false;
#endif
}


// ============================================================================
// Relay socket — message broker for inter-instance communication
// ============================================================================

void WorkboardManager::StartRelaySocket()
{
#ifndef _WIN32
    String sockPath = ttySockDir_ + "manager_relay.sock";
    unlink(sockPath.CString());

    relayListenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (relayListenFd_ < 0)
    {
        URHO3D_LOGERROR("Failed to create relay socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);

    if (bind(relayListenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(relayListenFd_, 8) < 0)
    {
        URHO3D_LOGERROR("Failed to bind relay socket");
        close(relayListenFd_);
        relayListenFd_ = -1;
        return;
    }

    URHO3D_LOGINFO("Relay socket listening: " + sockPath);
#else
    // Windows: create a named pipe instance for the relay.
    // PIPE_ACCESS_INBOUND — clients write, we read.
    // FILE_FLAG_OVERLAPPED — non-blocking via overlapped I/O.
    String pipeName = PipeName("manager_relay");
    relayPipeHandle_ = CreateNamedPipeA(
        pipeName.CString(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096,
        0, nullptr);

    if (relayPipeHandle_ == INVALID_HANDLE_VALUE)
    {
        URHO3D_LOGERROR("Failed to create relay named pipe");
        return;
    }

    // Start async connect — completes when a client connects
    memset(&relayOverlapped_, 0, sizeof(relayOverlapped_));
    relayOverlapped_.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    relayConnectPending_ = !ConnectNamedPipe(relayPipeHandle_, &relayOverlapped_);
    if (!relayConnectPending_ && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        URHO3D_LOGERROR("Failed to start relay pipe connect");
        CloseHandle(relayPipeHandle_);
        relayPipeHandle_ = INVALID_HANDLE_VALUE;
        return;
    }

    URHO3D_LOGINFO("Relay pipe listening: " + pipeName);
#endif
}

void WorkboardManager::StopRelaySocket()
{
#ifndef _WIN32
    if (relayListenFd_ >= 0)
    {
        close(relayListenFd_);
        relayListenFd_ = -1;
    }
    String sockPath = ttySockDir_ + "manager_relay.sock";
    unlink(sockPath.CString());
#else
    if (relayPipeHandle_ != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(relayPipeHandle_);
        CloseHandle(relayPipeHandle_);
        relayPipeHandle_ = INVALID_HANDLE_VALUE;
    }
    if (relayOverlapped_.hEvent)
    {
        CloseHandle(relayOverlapped_.hEvent);
        relayOverlapped_.hEvent = nullptr;
    }
#endif
}

void WorkboardManager::PollRelaySocket()
{
#ifdef _WIN32
    if (relayPipeHandle_ == INVALID_HANDLE_VALUE)
        return;
#else
    if (relayListenFd_ < 0)
        return;
#endif

    // Accept all pending connections (non-blocking) and read messages.
    // Transport differs per platform, message parsing is shared.
    for (;;)
    {
        char buf[4096];
        int n = 0;

#ifndef _WIN32
        int clientFd = accept(relayListenFd_, nullptr, nullptr);
        if (clientFd < 0)
            break;

        ssize_t r = read(clientFd, buf, sizeof(buf) - 1);
        close(clientFd);

        if (r <= 0)
            continue;
        n = (int)r;
#else
        // Check if a client has connected (non-blocking via overlapped)
        if (relayConnectPending_)
        {
            DWORD dummy;
            if (!GetOverlappedResult(relayPipeHandle_, &relayOverlapped_, &dummy, FALSE))
                break;  // No client yet
            relayConnectPending_ = false;
        }

        // Client connected — read the message
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(relayPipeHandle_, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        // Done with this client — disconnect and re-arm for next
        DisconnectNamedPipe(relayPipeHandle_);
        ResetEvent(relayOverlapped_.hEvent);
        relayConnectPending_ = !ConnectNamedPipe(relayPipeHandle_, &relayOverlapped_);

        if (!ok || bytesRead == 0)
            continue;
        n = (int)bytesRead;
#endif
        buf[n] = '\0';

        // ── Shared message parsing (platform-independent) ──

        String payload(buf, (unsigned)n);
        unsigned firstColon = payload.Find(':');
        if (firstColon == String::NPOS || firstColon == 0)
        {
            URHO3D_LOGWARNING("Relay: malformed message (no target separator)");
            continue;
        }

        String target = payload.Substring(0, firstColon).Trimmed();
        String rest = payload.Substring(firstColon + 1);

        // Try to extract sender from "sender:message" — if second colon exists
        String sender;
        String message;
        unsigned secondColon = rest.Find(':');
        if (secondColon != String::NPOS && secondColon < rest.Length() - 1)
        {
            String maybeSender = rest.Substring(0, secondColon).Trimmed();
            // Sender roles are short alphanumeric (coder, coder2, coder3, etc.)
            // If maybeSender looks like a role (no spaces, short), treat it as sender
            if (!maybeSender.Empty() && maybeSender.Length() <= 20 && !maybeSender.Contains(' '))
            {
                sender = maybeSender;
                message = rest.Substring(secondColon + 1).Trimmed();
            }
            else
            {
                // Not a role — treat entire rest as message (legacy format)
                message = rest.Trimmed();
            }
        }
        else
        {
            message = rest.Trimmed();
        }

        if (target.Empty() || message.Empty())
            continue;

        // __ALL__ broadcast: deliver to every instance except the sender.
        // Message format: __STATE__:senderRole:STATE
        if (target == "__ALL__")
        {
            // Extract sender role from __STATE__:role:STATE
            String senderRole;
            if (message.StartsWith("__STATE__:"))
            {
                String body = message.Substring(10);
                unsigned sep = body.Find(':');
                if (sep != String::NPOS)
                    senderRole = body.Substring(0, sep);
            }

            AppendLog("Broadcast", message);

            // Deliver to each socket/pipe except sender
#ifndef _WIN32
            auto* fs = GetSubsystem<FileSystem>();
            Vector<String> sockNames;
            fs->ScanDir(sockNames, ttySockDir_, "*.sock", SCAN_FILES, false);

            for (unsigned i = 0; i < sockNames.Size(); ++i)
            {
                if (sockNames[i] == "manager.sock" || sockNames[i] == "manager_relay.sock")
                    continue;

                // Skip sender by role name
                String sockRole = sockNames[i].Substring(0, sockNames[i].Find('.'));
                if (!senderRole.Empty() && sockRole == senderRole)
                    continue;

                SendToSocket(sockRole, message);
            }
#else
            // Windows: try all known coder pipes
            const char* names[] = { "coder", "coder2", "coder3", "coder4",
                                    "coder5", "coder6", "coder7", "coder8",
                                    "coder9", "coder10", "coder11", "coder12",
                                    "coder13", "coder14", "coder15", "coder16" };
            for (int i = 0; i < 16; ++i)
            {
                if (!senderRole.Empty() && senderRole == names[i])
                    continue;
                SendToSocket(names[i], message);
            }
#endif
            continue;
        }

        // __REMEMBER__:fact — coder stores a shared memory
        if (message.StartsWith("__REMEMBER__:"))
        {
            String fact = message.Substring(13).Trimmed();
            String source = sender.Empty() ? "unknown" : sender;
            if (!fact.Empty() && workboardDB_.IsOpen())
                workboardDB_.Remember(source, fact);
            continue;
        }

        // __HELLO__ — new coder announcing itself; send shared memory
        if (message.StartsWith("__HELLO__") && !sender.Empty() && workboardDB_.IsOpen())
        {
            String memories = workboardDB_.GetAllMemories();
            if (!memories.Empty())
                SendToSocket(sender, "[SHARED MEMORY]\n" + memories);
            continue;
        }

        // __BUILD_REQUEST__:target — coder requests a managed build
        if (message.StartsWith("__BUILD_REQUEST__:"))
        {
            String buildTarget = message.Substring(18).Trimmed();
            EnqueueBuild(buildTarget, target);
            continue;
        }

        // !reload — hot-reload finetuned model (can be sent to manager or yuki)
        if (message.Trimmed() == "!reload" && (target == "manager" || target == "yuki"))
        {
            String result = yukiLLM_.ReloadModel();
            URHO3D_LOGINFOF("LLM reload: %s", result.CString());
            AppendLog("Yuki", result);
            AppendYukiChat("System", result);

            // Notify the sender if known
            if (!sender.Empty())
                SendToSocket(sender, "Yuki reload: " + result);
            continue;
        }

        // Yuki — route to embedded LLM, not a standalone process
        if (target == "yuki")
        {
            if (yukiLLM_.IsModelLoaded() && !yukiLLM_.IsTrainingInProgress())
            {
                String from = sender.Empty() ? "relay" : sender;
                AppendYukiChat(from, message);
                yukiLLM_.QueueInference(message);
            }
            else
                AppendLog("Yuki", "Message dropped — model not ready");
            continue;
        }

        // Log the relay
        AppendLog(String("Relay \xe2\x86\x92 ") + target, message);

        // Deliver — sender excluded from broadcast (no echo-back)
        SendToSocket(target, message, sender);
    }
}

// ============================================================================
// Build Queue
// ============================================================================

void WorkboardManager::EnqueueBuild(const String& target, const String& requester)
{
    // Dedup — reject if already queued or currently building
    if (activeBuildTarget_ == target)
    {
        AppendLog("Build", target + " already building (requested by " + requester + ")");
        SendToSocket(requester, "Build REJECTED: " + target + " already building");
        return;
    }
    for (unsigned i = 0; i < buildQueue_.Size(); ++i)
    {
        if (buildQueue_[i].target == target)
        {
            AppendLog("Build", target + " already queued (requested by " + requester + ")");
            SendToSocket(requester, "Build REJECTED: " + target + " already queued at position " + String(i + 1));
            return;
        }
    }

    BuildQueueEntry entry;
    entry.target = target;
    entry.requester = requester;

    // Dependency: Urho3D always goes to front of queue
    if (target == "Urho3D" && !buildQueue_.Empty())
        buildQueue_.Insert(0, entry);
    else
        buildQueue_.Push(entry);

    unsigned pos = 0;
    for (unsigned i = 0; i < buildQueue_.Size(); ++i)
        if (buildQueue_[i].target == target) { pos = i + 1; break; }

    AppendLog("Build", "Queued " + target + " at position " + String(pos) + " (from " + requester + ")");
    SendToSocket(requester, "Build QUEUED: " + target + " at position " + String(pos) +
        (buildQueue_.Size() > 1 ? " (" + String(buildQueue_.Size()) + " in queue)" : ""));

    // If nothing building, start immediately
    if (activeBuildPid_ == 0)
        ProcessBuildQueue();
}

void WorkboardManager::ProcessBuildQueue()
{
#ifndef _WIN32
    // Check if active build finished
    if (activeBuildPid_ > 0)
    {
        int status = 0;
        pid_t result = waitpid(activeBuildPid_, &status, WNOHANG);
        if (result == 0)
            return;  // Still running

        // Build finished
        bool success = (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        String resultStr = success ? "SUCCESS" : "FAILED (exit " + String(WEXITSTATUS(status)) + ")";
        AppendLog("Build", activeBuildTarget_ + " " + resultStr);

        if (!activeBuildRequester_.Empty())
            SendToSocket(activeBuildRequester_, "Build DONE: " + activeBuildTarget_ + " " + resultStr);

        activeBuildPid_ = 0;
        activeBuildTarget_.Clear();
        activeBuildRequester_.Clear();
    }

    // Start next build
    if (buildQueue_.Empty())
        return;

    BuildQueueEntry next = buildQueue_[0];
    buildQueue_.Erase(0);

    auto* fs = GetSubsystem<FileSystem>();
    String scriptPath = fs->GetProgramDir() + "../../.claude/hooks/safe_build.sh";

    AppendLog("Build", "Starting " + next.target + " (requested by " + next.requester + ")");

    pid_t pid = fork();
    if (pid == 0)
    {
        // Child — exec safe_build.sh
        // Redirect stdout/stderr to /dev/null so build output doesn't pollute Manager
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("/bin/bash", "bash", scriptPath.CString(), next.target.CString(), (char*)nullptr);
        _exit(127);
    }
    else if (pid > 0)
    {
        activeBuildPid_ = pid;
        activeBuildTarget_ = next.target;
        activeBuildRequester_ = next.requester;
    }
    else
    {
        AppendLog("Build", "fork() failed for " + next.target);
        if (!next.requester.Empty())
            SendToSocket(next.requester, "Build FAILED: fork() error for " + next.target);
    }
#endif
}

void WorkboardManager::SendMessage(const String& target, const String& message)
{
    URHO3D_LOGINFOF("SendMessage: target=[%s] message=[%s]", target.CString(), message.CString());
    if (message.Empty())
        return;

    // Yuki is embedded — route directly to LLM
    if (target == "yuki")
    {
        if (yukiLLM_.IsModelLoaded())
            yukiLLM_.QueueInference(message);
        else
            AppendLog("Yuki", "Message dropped — model not ready");
        return;
    }

    bool injected = SendToSocket(target, message);

    if (!injected)
        AppendLog(String("Manager \xe2\x86\x92 ") + target + " [FAILED]", "TTY injection failed — no socket for " + target);
    else
        AppendLog(String("Manager \xe2\x86\x92 ") + target + " [TTY]", message);
}

void WorkboardManager::HandleSendCoder(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (!messageInput_) return;
    String text = messageInput_->GetText().Trimmed();
    if (text.Empty()) return;

    // Unified send — route based on receiver dropdown selection
    String target = GetSelectedCoderRole();  // Returns selected item text (lowercase)
    if (target.Empty())
    {
        AppendLog("System", "No receiver selected");
        return;
    }

    if (target == "broadcast")
    {
        for (const String& role : knownCoderRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        // CC Yuki on broadcasts via embedded LLM
        if (yukiLLM_.IsModelLoaded())
            yukiLLM_.QueueInference("cc:" + text);
    }
    else if (target == "yuki")
    {
        // Direct to embedded Yuki
        if (yukiLLM_.IsModelLoaded())
        {
            AppendYukiChat("Leith", text);
            yukiLLM_.QueueInference(text);
        }
        else
            AppendYukiChat("System", "Model not loaded");
    }
    else if (target == "unassigned")
    {
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
    }
    else
    {
        SendMessage(target, text);
    }
    messageInput_->SetText("");
}

// Legacy handlers kept as stubs — routing now goes through unified HandleSendCoder
// HandleSendPlanner removed — planner role no longer exists
void WorkboardManager::HandleSendUnassigned(StringHash, VariantMap&) {}
void WorkboardManager::HandleSendBroadcast(StringHash, VariantMap&) {}

void WorkboardManager::HandleClearFileLocks(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    String lockDir = ipcDir_ + "locks";
    int cleared = 0;

    auto* fs = GetSubsystem<FileSystem>();
    if (fs)
    {
        Vector<String> entries;
        fs->ScanDir(entries, lockDir, "*", SCAN_FILES | SCAN_DIRS, false);
        for (const String& entry : entries)
        {
            if (entry == "." || entry == "..")
                continue;
            String fullPath = lockDir + "/" + entry;
            if (fs->DirExists(fullPath))
            {
                // Remove contents first
                Vector<String> inner;
                fs->ScanDir(inner, fullPath, "*", SCAN_FILES, false);
                for (const String& f : inner)
                {
                    if (f != "." && f != "..")
                        fs->Delete(fullPath + "/" + f);
                }
                fs->SystemCommand("rmdir \"" + fullPath + "\"");
                cleared++;
            }
            else
            {
                fs->Delete(fullPath);
                cleared++;
            }
        }
    }

    AppendLog("Manager", cleared > 0
        ? String("Cleared ") + String(cleared) + " file lock(s)"
        : "No file locks to clear");
}

void WorkboardManager::HandleSpawnCoder(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    // Enforce local instance cap
    Vector<String> liveCoders = DiscoverCoderRoles();
    if (liveCoders.Size() >= maxLocalCoders_)
    {
        AppendLog("Manager", "Spawn refused: " + String(liveCoders.Size()) + "/" + String(maxLocalCoders_) + " local coders already running");
        return;
    }

    String scriptPath = GetProjectRoot() + "/.claude/hooks/claude_ipc.sh";

    // Check script exists
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs->FileExists(scriptPath))
    {
        AppendLog("Manager", "Cannot spawn coder: " + scriptPath + " not found");
        return;
    }

    // SystemCommand inherits environment (DISPLAY, DBUS_SESSION_BUS_ADDRESS)
    // which gnome-terminal needs. The script launches in the background (&).
    int ret = fs->SystemCommand(scriptPath + " spawn-coder");
    if (ret == 0)
        AppendLog("Manager", "Spawn Coder command executed (" + String(liveCoders.Size() + 1) + "/" + String(maxLocalCoders_) + ")");
    else
        AppendLog("Manager", "Spawn Coder failed (exit code " + String(ret) + ")");
}

void WorkboardManager::HandleCoderCapMinus(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (maxLocalCoders_ > 1)
    {
        --maxLocalCoders_;
        UpdateCoderCapText();
    }
}

void WorkboardManager::HandleCoderCapPlus(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (maxLocalCoders_ < 8)
    {
        ++maxLocalCoders_;
        UpdateCoderCapText();
    }
}

void WorkboardManager::UpdateCoderCapText()
{
    if (!coderCapText_)
        return;
    Vector<String> liveCoders = DiscoverCoderRoles();
    coderCapText_->SetText(String(liveCoders.Size()) + "/" + String(maxLocalCoders_));

    // Write cap to file so shell scripts can enforce it
    String capPath = ipcDir_ + "coder_cap";
    File capFile(context_, capPath, FILE_WRITE);
    if (capFile.IsOpen())
        capFile.WriteLine(String(maxLocalCoders_));
}

void WorkboardManager::HandleToggleScreenshots(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    auto* fs = GetSubsystem<FileSystem>();
    String flagPath = ipcDir_ + "screenshots_blocked";

    screenshotsBlocked_ = !screenshotsBlocked_;

    if (screenshotsBlocked_)
    {
        // Create the block flag — all Claudette hooks check for this
        File flagFile(context_, flagPath, FILE_WRITE);
        if (flagFile.IsOpen())
            flagFile.WriteLine("blocked");
        AppendLog("Manager", "Screenshots BLOCKED — all instances blinded");
    }
    else
    {
        fs->Delete(flagPath);
        AppendLog("Manager", "Screenshots ALLOWED");
    }

    // Update button label
    if (screenshotToggleBtn_)
    {
        auto* text = screenshotToggleBtn_->GetChildStaticCast<Text>(0);
        if (text)
            text->SetText(screenshotsBlocked_ ? "Snoop: OFF" : "Snoop: ON");
    }
}

void WorkboardManager::HandleToggleYuki(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (yukiLLM_.IsModelLoaded())
    {
        yukiLLM_.UnloadModel();
        AppendLog("Yuki", "Model unloaded");
        AppendYukiChat("System", "AI offline");
        if (yukiToggleBtnText_)
            yukiToggleBtnText_->SetText("Load AI");
    }
    else
    {
        String modelPath = FindYukiModel();
        if (modelPath.Empty())
        {
            AppendLog("Yuki", "No GGUF model found in YukiHoho/models/");
            return;
        }
        yukiLLM_.LoadModelAsync(modelPath);
        AppendLog("Yuki", "Loading: " + GetFileNameAndExtension(modelPath));
        AppendYukiChat("System", "Loading AI...");
        if (yukiToggleBtnText_)
            yukiToggleBtnText_->SetText("Unload");
    }
}

// ============================================================================
// Message Log
// ============================================================================

void WorkboardManager::AppendLog(const String& source, const String& message)
{
    if (!logListView_)
        return;

    // Skip noisy turn-complete messages from the Stop hook
    if (message.Contains("Turn complete at"))
        return;

    // Timestamp
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

    // Format: "HH:MM:SS [Source] message"
    String prefix = String(ts) + "  ";

    String srcPad = source;
    while (srcPad.Length() < 10)
        srcPad += " ";
    prefix += "[" + srcPad + "] ";

    auto* item = new Text(context_);
    item->SetFont(font_, currentFontSize_ - 1);
    item->SetText(prefix + message);
    item->SetColor(LogColorForSource(source));
    item->SetWordwrap(true);
    if (logPanel_)
        item->SetMaxWidth(logPanel_->GetWidth() - 30);
    logListView_->AddItem(item);

    while (logListView_->GetNumItems() > MAX_LOG_LINES)
        logListView_->RemoveItem((i32)0);

    logListView_->EnsureItemVisibility(logListView_->GetNumItems() - 1);
}

Color WorkboardManager::LogColorForSource(const String& source)
{
    // Color by sender (first word before arrow)
    if (source.Contains("Yuki") || source.Contains("yuki"))
        return Color(1.0f, 0.5f, 0.8f);          // pink/magenta
    if (source.Contains("Coder"))
        return Color(0.3f, 0.9f, 1.0f);         // cyan
    if (source == "Coder" || source == "coder")
        return Color(1.0f, 0.8f, 0.3f);          // amber — elder coder
    if (source.Contains("Manager"))
        return Color(0.6f, 1.0f, 0.6f);          // light green
    if (source.Contains("Unassigned"))
        return Color(0.7f, 0.7f, 1.0f);          // light blue
    if (source == "Broadcast")
        return Color(1.0f, 0.55f, 0.45f);        // salmon
    if (source == "Download")
        return Color(0.5f, 0.85f, 1.0f);         // sky blue
    return Color(0.5f, 0.5f, 0.5f);              // gray (System)
}

// ============================================================================
// Beacon & Liveness
// ============================================================================

void WorkboardManager::UpdateBeacon()
{
    auto* network = GetSubsystem<Network>();
    if (!network || !network->IsServerRunning())
        return;

    VariantMap beacon;
    beacon["Service"]    = String("WorkboardManager");
    beacon["Version"]    = String("1.0");
    beacon["Yuki"]       = yukiLLM_.IsModelLoaded() ? String("ONLINE") : String("OFFLINE");
    // Report all known unassigned roles
    for (const String& role : knownUnassignedRoles_)
    {
        String key = role;
        if (!key.Empty()) key[0] = (char)toupper(key[0]);
        beacon[key] = IsInstanceAlive(role) ? String("ONLINE") : String("OFFLINE");
    }
    if (knownUnassignedRoles_.Empty())
        beacon["Unassigned"] = String("OFFLINE");
    // Report all known coder roles
    for (const String& role : knownCoderRoles_)
    {
        String key = role;
        if (!key.Empty()) key[0] = (char)toupper(key[0]);
        beacon[key] = IsInstanceAlive(role) ? String("ONLINE") : String("OFFLINE");
    }
    network->SetDiscoveryBeacon(beacon);
}

void WorkboardManager::PollYukiInference()
{
    if (!yukiLLM_.IsInferenceComplete())
        return;

    String result = yukiLLM_.TakeResult();
    if (result.Empty())
        return;

    // Handle remember mode — extract training pairs from Yuki's Q&A output
    if (yukiLLM_.IsRememberInFlight())
    {
        yukiLLM_.ExtractAndSaveTrainingPairs(result);
        AppendYukiChat("Yuki", "[Remembered]");
        return;
    }

    // Show response in chat panel
    AppendYukiChat("Yuki", result);
    AppendLog("Yuki", result.Length() > 120 ? result.Substring(0, 120) + "..." : result);

    // Execute any tool commands in the output
    String toolResults = yukiLLM_.ExecuteTools(result);
    if (!toolResults.Empty())
        AppendLog("Yuki-Tools", toolResults.Length() > 200 ? toolResults.Substring(0, 200) + "..." : toolResults);

    // Auto-continue if tools produced results or remember is pending
    if (yukiLLM_.ShouldAutoContinue())
        yukiLLM_.AutoContinue();
}

void WorkboardManager::CleanupStalePID(const String& role, int pid)
{
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";
    fs->Delete(instDir + role + ".pid");
    // Phase 2 cutover: .role files no longer written; nothing to clean.
    AppendLog("System", role + " (PID " + String(pid) + ") detected dead — cleaned up stale files");
    UpdateBeacon();
}

float WorkboardManager::GetLastActivity(const String& role)
{
    if (role == "unassigned")
        return lastUnassignedActivity_;
    // Any coder role (coder, coder1, coder2, ...)
    if (role.StartsWith("coder"))
    {
        auto it = coderActivityTimers_.Find(role);
        if (it != coderActivityTimers_.End())
            return it->second_;
    }
    return 999.0f;
}

void WorkboardManager::SweepStaleRoleFiles()
{
    // Phase 2 cutover: .role files no longer written. Function renamed in
    // spirit to "sweep stale instance pid files" but the old name is kept
    // because callers (RefreshInstanceStatus) reference it. Behavior is now
    // .pid-only — sweep any .pid file whose stored PID is dead, plus any
    // legacy .role file left behind from a pre-cutover session (one-time
    // migration cleanup).
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // One-time legacy cleanup: delete any .role files left over from before
    // the Phase 2 cutover. They're now meaningless.
    Vector<String> legacyRoleFiles;
    fs->ScanDir(legacyRoleFiles, instDir, "*.role", SCAN_FILES, false);
    for (const String& filename : legacyRoleFiles)
    {
        fs->Delete(instDir + filename);
        AppendLog("System", "Removed legacy .role file (Phase 2 cutover): " + filename);
    }

    // Sweep .pid files whose PID is dead. .pid format is PID on line 1,
    // TTY_ID on line 2 (Phase 1 dual-write); we only need line 1 here.
    Vector<String> pidFiles;
    fs->ScanDir(pidFiles, instDir, "*.pid", SCAN_FILES, false);

    bool elderDied = false;
    for (const String& filename : pidFiles)
    {
        String pidPath = instDir + filename;

        File f(context_, pidPath);
        if (!f.IsOpen())
            continue;

        int pid = atoi(f.ReadLine().Trimmed().CString());
        f.Close();

        if (pid > 0 && IsProcessAlive(pid))
            continue;  // Still alive

        fs->Delete(pidPath);
        // Also clean heartbeat and socket
        String role = GetFileNameAndExtension(filename).Substring(0, filename.Find('.'));
        fs->Delete(instDir + role + ".heartbeat");
        String sockPath = ipcDir_ + "tty/" + role + ".sock";
        fs->Delete(sockPath);

        if (role == "coder")
            elderDied = true;

        AppendLog("System", "Swept dead instance: " + role);
    }

    // Elder died — promote next oldest and renumber everyone down
    if (elderDied)
    {
        // Collect living numbered coders with their uptime (merit = longest serving)
        struct CoderInfo { int num; unsigned uptimeSec; };
        Vector<CoderInfo> liveCandidates;
        String healthDir = ipcDir_ + "health/";

        for (int n = 2; n <= 20; ++n)
        {
            String pf = instDir + "coder" + String(n) + ".pid";
            if (!fs->FileExists(pf))
                continue;
            File cf(context_, pf);
            if (!cf.IsOpen())
                continue;
            int cpid = atoi(cf.ReadLine().Trimmed().CString());
            cf.Close();
            if (cpid <= 0 || !IsProcessAlive(cpid))
                continue;

            // Read uptime from health file (written by Claudette every 5s)
            unsigned uptime = 0;
            String hpath = healthDir + "coder" + String(n) + ".json";
            if (fs->FileExists(hpath))
            {
                File hf(context_, hpath);
                if (hf.IsOpen())
                {
                    String content;
                    while (!hf.IsEof())
                        content += hf.ReadLine() + "\n";
                    hf.Close();
                    // Parse uptimeSec from JSON
                    unsigned pos = content.Find("\"uptimeSec\":");
                    if (pos != String::NPOS)
                        uptime = (unsigned)atoi(content.CString() + pos + 13);
                }
            }
            liveCandidates.Push({n, uptime});
        }

        if (!liveCandidates.Empty())
        {
            // Sort by uptime descending — longest serving wins
            for (unsigned i = 0; i < liveCandidates.Size(); ++i)
                for (unsigned j = i + 1; j < liveCandidates.Size(); ++j)
                    if (liveCandidates[j].uptimeSec > liveCandidates[i].uptimeSec)
                        Swap(liveCandidates[i], liveCandidates[j]);

            String ttyDir = ipcDir_ + "tty/";

            // Rename helper — moves pid and heartbeat for one role.
            // Sockets are resolved by PID at send time — no symlinks to manage.
            auto renameRole = [&](const String& oldRole, const String& newRole)
            {
                if (fs->FileExists(instDir + oldRole + ".pid"))
                    fs->Rename(instDir + oldRole + ".pid", instDir + newRole + ".pid");
                if (fs->FileExists(instDir + oldRole + ".heartbeat"))
                    fs->Rename(instDir + oldRole + ".heartbeat", instDir + newRole + ".heartbeat");
                // Clean up any stale symlinks from the old system
#ifndef _WIN32
                String oldSock = ttyDir + oldRole + ".sock";
                unlink(oldSock.CString());
#endif
            };

            // Promote most meritorious (longest uptime) to elder
            int elderNum = liveCandidates[0].num;
            String promoted = "coder" + String(elderNum);
            // Send __ROLE__ before renaming so Claudette updates its banner
            SendToSocket(promoted, "__ROLE__:coder");
            renameRole(promoted, "coder");
            AppendLog("System", promoted + " promoted to elder (merit: " +
                String(liveCandidates[0].uptimeSec) + "s uptime)");

            // Notify the new elder via their new socket
            SendToSocket("coder",
                "[SUCCESSION] You (" + promoted +
                ") are promoted to elder by merit (longest serving).");

            // Transfer shared memory to the new elder
            if (workboardDB_.IsOpen())
            {
                String memories = workboardDB_.GetAllMemories();
                if (!memories.Empty())
                    SendToSocket("coder", "[SHARED MEMORY]\n" + memories);
            }

            // Renumber the rest contiguously by uptime (next longest = coder2, etc.)
            int slot = 2;
            for (unsigned i = 1; i < liveCandidates.Size(); ++i)
            {
                String oldName = "coder" + String(liveCandidates[i].num);
                String newName = "coder" + String(slot);
                if (oldName != newName)
                {
                    SendToSocket(oldName, "__ROLE__:" + newName);
                    renameRole(oldName, newName);
                    AppendLog("System", oldName + " renumbered to " + newName);
                }
                ++slot;
            }
        }
    }
}

// ============================================================================
// Yuki Training Lump Collection
// ============================================================================

void WorkboardManager::CheckTrainingLump()
{
#ifndef _WIN32
    String collectedPath = projectRoot_ + "/Source/Tools/YukiHoho/training/yuki_collected.jsonl";
    auto* fs = GetSubsystem<FileSystem>();

    if (!fs->FileExists(collectedPath))
        return;

    // Check if a fine-tune is already running
    String pidLockPath = projectRoot_ + "/Source/Tools/YukiHoho/training/.finetune.pid";
    if (fs->FileExists(pidLockPath))
    {
        File pidFile(context_, pidLockPath, FILE_READ);
        if (pidFile.IsOpen())
        {
            int ftPid = atoi(pidFile.ReadLine().Trimmed().CString());
            if (ftPid > 0 && IsProcessAlive(ftPid))
                return;  // Fine-tune still running
        }
        // Fine-tune just finished — clean up PID (model is embedded, no reload needed)
        fs->Delete(pidLockPath);
        AppendLog("Yuki", "Fine-tune complete");
    }

    // Count lines
    File f(context_, collectedPath, FILE_READ);
    if (!f.IsOpen())
        return;

    unsigned lineCount = 0;
    while (!f.IsEof())
    {
        String line = f.ReadLine().Trimmed();
        if (!line.Empty())
            ++lineCount;
    }
    f.Close();

    if (lineCount < TRAINING_LUMP_THRESHOLD)
        return;

    // Threshold reached — archive and kick digest
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));
    String archivePath = projectRoot_ + "/Source/Tools/YukiHoho/training/yuki_collected_" + String(ts) + ".jsonl";
    fs->Rename(collectedPath, archivePath);
    AppendLog("Yuki", "Training lump: " + String(lineCount) + " pairs — digesting");

    String scriptPath = projectRoot_ + "/Source/Tools/YukiHoho/scripts/finetune.sh";
    if (fs->FileExists(scriptPath))
    {
        String cmd = "nohup " + scriptPath + " " + archivePath + " > /tmp/yuki_finetune.log 2>&1 & echo $! > " + pidLockPath;
        fs->SystemCommand(cmd);
        AppendLog("Yuki", "Fine-tune kicked in background");
    }
    else
    {
        AppendLog("Yuki", "Training data archived — no finetune.sh yet");
    }
#endif
}

// ============================================================================
// Workboard Task Lookup
// ============================================================================

String WorkboardManager::GetCurrentTask(const String& owner)
{
    // Search In Progress table for rows where the Owner column matches
    for (const auto& sec : sections_)
    {
        if (!sec.title.Contains("In Progress"))
            continue;

        // Find the Owner column index
        int ownerCol = -1;
        for (unsigned i = 0; i < sec.headers.Size(); ++i)
        {
            if (sec.headers[i].Trimmed().ToLower().Contains("owner"))
            {
                ownerCol = (int)i;
                break;
            }
        }
        if (ownerCol < 0)
            break;

        // Find the Task column (usually column 0)
        int taskCol = 0;
        for (unsigned i = 0; i < sec.headers.Size(); ++i)
        {
            if (sec.headers[i].Trimmed().ToLower().Contains("task"))
            {
                taskCol = (int)i;
                break;
            }
        }

        // Collect all tasks for this owner
        String tasks;
        for (const auto& row : sec.rows)
        {
            if (ownerCol < (int)row.cells.Size() &&
                row.cells[ownerCol].Trimmed().ToLower() == owner.ToLower())
            {
                if (taskCol < (int)row.cells.Size())
                {
                    if (!tasks.Empty()) tasks += ", ";
                    tasks += row.cells[taskCol].Trimmed();
                }
            }
        }
        return tasks;
    }
    return String::EMPTY;
}

// ============================================================================
// Multi-Coder Discovery
// ============================================================================

Vector<String> WorkboardManager::DiscoverCoderRoles()
{
    Vector<String> roles;
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // Phase 2 cutover: .role scan removed. .pid files are now the canonical
    // source; role is derived from the filename, PID is line 1, TTY_ID is
    // line 2 (we only need PID here for liveness check).
    Vector<String> pidFiles;
    fs->ScanDir(pidFiles, instDir, "coder*.pid", SCAN_FILES, false);

    for (const String& filename : pidFiles)
    {
        String role = filename.Substring(0, filename.FindLast('.'));
        if (!role.StartsWith("coder"))
            continue;

        File f(context_, instDir + filename);
        if (!f.IsOpen())
            continue;
        int pid = atoi(f.ReadLine().Trimmed().CString());

        if (pid > 0 && IsProcessAlive(pid) && !roles.Contains(role))
        {
            roles.Push(role);
            if (coderActivityTimers_.Find(role) == coderActivityTimers_.End())
                coderActivityTimers_[role] = 0.0f;
        }
    }

    Sort(roles.Begin(), roles.End());
    return roles;
}

// ============================================================================
// Multi-Unassigned Discovery
// ============================================================================

Vector<String> WorkboardManager::DiscoverUnassignedRoles()
{
    Vector<String> roles;
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // Phase 2 cutover: .role scan removed. .pid files are the canonical source.
    Vector<String> pidFiles;
    fs->ScanDir(pidFiles, instDir, "unassigned*.pid", SCAN_FILES, false);

    for (const String& filename : pidFiles)
    {
        String role = filename.Substring(0, filename.FindLast('.'));
        if (!role.StartsWith("unassigned"))
            continue;

        File f(context_, instDir + filename);
        if (!f.IsOpen())
            continue;
        int pid = atoi(f.ReadLine().Trimmed().CString());

        if (pid > 0 && IsProcessAlive(pid) && !roles.Contains(role))
            roles.Push(role);
    }

    Sort(roles.Begin(), roles.End());
    return roles;
}

String WorkboardManager::GetSelectedCoderRole()
{
    if (!coderDropdown_ || coderDropdown_->GetNumItems() == 0)
        return String::EMPTY;

    unsigned sel = coderDropdown_->GetSelection();
    auto* item = coderDropdown_->GetItem(sel);
    if (!item)
        return String::EMPTY;

    auto* text = dynamic_cast<Text*>(item);
    if (!text)
        return String::EMPTY;

    return text->GetText().ToLower().Trimmed();
}

// ============================================================================
// Event Handlers
// ============================================================================

void WorkboardManager::HandleUpdate(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    // Increment liveness timers
    lastUnassignedActivity_ += timeStep;
    for (auto it = coderActivityTimers_.Begin(); it != coderActivityTimers_.End(); ++it)
        it->second_ += timeStep;

    if (autoSpawnCooldown_ > 0.0f)
        autoSpawnCooldown_ -= timeStep;

    CheckDownloadProgress();

    relayPollAccumulator_ += timeStep;
    if (relayPollAccumulator_ >= RELAY_POLL_INTERVAL)
    {
        relayPollAccumulator_ = 0.0f;
        PollRelaySocket();
    }

    ProcessBuildQueue();

    refreshAccumulator_ += timeStep;
    if (refreshAccumulator_ >= REFRESH_INTERVAL)
    {
        refreshAccumulator_ = 0.0f;

        LoadWorkboard();
        ScanPlanFiles();
        RefreshInstanceStatus();
        SampleSystemStats();
    }

    // SQL is the sole authority — no markdown reconciliation.

    // ── Yuki training lump check ──
    trainingCheckAccumulator_ += timeStep;
    if (trainingCheckAccumulator_ >= TRAINING_CHECK_INTERVAL)
    {
        trainingCheckAccumulator_ = 0.0f;
        CheckTrainingLump();
    }

    // ── Embedded Yuki: tick cooldowns and poll inference ──
    yukiLLM_.Tick(timeStep);
    PollYukiInference();
}

void WorkboardManager::HandleScreenMode(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    // Window resized — force all anchored children to recompute position/size
    auto* root = GetSubsystem<UI>()->GetRoot();
    if (!root)
        return;

    const auto& children = root->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
    {
        if (children[i]->GetEnableAnchor())
            children[i]->UpdateAnchoring();
    }
}

void WorkboardManager::HandleKeyDown(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace KeyDown;
    int key = eventData[P_KEY].GetI32();

    if (key == KEY_ESCAPE)
        engine_->Exit();
    else if (key == KEY_F5)
    {
        LoadWorkboard();
        ScanPlanFiles();
        RefreshInstanceStatus();
        AppendLog("System", "Refreshed.");
    }
}

// ============================================================================
// Remote Workboard Sync (Phase 2a)
// ============================================================================

void WorkboardManager::RegisterWorkboardRemoteEvents()
{
    auto* network = GetSubsystem<Network>();
    if (!network)
        return;

    // Register all workboard events so they pass the remote event whitelist
    network->RegisterRemoteEvent(E_WB_WELCOME);
    network->RegisterRemoteEvent(E_WB_WORKBOARD_FULL);
    network->RegisterRemoteEvent(E_WB_PLAN_LIST);
    network->RegisterRemoteEvent(E_WB_PLAN_CONTENT);
    network->RegisterRemoteEvent(E_WB_MUTATION_ACK);
    network->RegisterRemoteEvent(E_WB_CLIENT_LIST);
    network->RegisterRemoteEvent(E_WB_SERVER_SHUTDOWN);  // Phase 2c
    network->RegisterRemoteEvent(E_WB_REQUEST_PLAN);
    network->RegisterRemoteEvent(E_WB_MUTATION);
    network->RegisterRemoteEvent(E_WB_SET_IDENTITY);
    network->RegisterRemoteEvent(E_WB_INSTANCE_STATUS);
}

void WorkboardManager::HandleClientConnected(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ClientConnected;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    AppendLog("Network", "Client connected: " + conn->ToString());
}

void WorkboardManager::HandleClientDisconnected(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ClientDisconnected;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());

    auto it = wbClients_.Find(conn);
    if (it != wbClients_.End())
    {
        AppendLog("Network", "Workboard client disconnected: " + it->second_.name_);
        wbClients_.Erase(it);
        PushClientListToAll();
    }
    else
    {
        AppendLog("Network", "Client disconnected (unauthenticated)");
    }
}

void WorkboardManager::HandleClientIdentity(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ClientIdentity;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());

    // LAN clients are trusted (no auth). WAN clients must provide --secret password.
    String addr = conn->GetAddress();
    bool isLan = addr.StartsWith("127.") || addr.StartsWith("10.") ||
                 addr.StartsWith("192.168.") || addr == "::1";
    // 172.16.0.0 – 172.31.255.255
    if (!isLan && addr.StartsWith("172."))
    {
        unsigned dot1 = addr.Find('.');
        if (dot1 != String::NPOS)
        {
            int second = atoi(addr.Substring(dot1 + 1).CString());
            if (second >= 16 && second <= 31)
                isLan = true;
        }
    }

    if (!isLan)
    {
        // WAN — require PAKE authentication (password never sent in plaintext)
        if (wbSecret_.Empty())
        {
            AppendLog("Network", "WAN client rejected — no --secret configured: " + conn->ToString());
            eventData[P_ALLOW] = false;
            return;
        }
        if (!conn->IsPakeAuthenticated())
        {
            AppendLog("Network", "WAN client rejected — PAKE auth failed from " + conn->ToString());
            eventData[P_ALLOW] = false;
            return;
        }
        AppendLog("Network", "WAN client PAKE-authenticated: " + conn->ToString());
    }

    // Accept the connection
    eventData[P_ALLOW] = true;

    // Register as authenticated workboard client
    const VariantMap& ident = conn->GetIdentity();
    auto nameIt = ident.Find(StringHash("Name"));
    auto roleIt = ident.Find(StringHash("Role"));

    WbClientInfo info;
    info.connection_ = conn;
    info.name_ = (nameIt != ident.End()) ? nameIt->second_.GetString() : String::EMPTY;
    info.role_ = (roleIt != ident.End()) ? roleIt->second_.GetString() : String::EMPTY;
    info.authenticated_ = true;
    if (info.name_.Empty())
        info.name_ = conn->ToString();
    wbClients_[conn] = info;

    AppendLog("Network", "Workboard client authenticated: " + info.name_ + " (" + info.role_ + ")");

    // Send welcome
    {
        VariantMap data;
        data["ServerName"] = String("WorkboardManager");
        data["Version"] = String("1.0");
        data["ClientCount"] = (int)wbClients_.Size();
        conn->SendRemoteEvent(E_WB_WELCOME, true, data);
    }

    // Push initial state
    PushWorkboardToClient(conn);
    PushPlanListToClient(conn);
    PushClientListToAll();
}

void WorkboardManager::HandleKeyExchangeAuth(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace KeyExchangeAuth;
    String username = eventData[P_USERNAME].GetString();

    if (!pakeSecretValid_)
    {
        eventData[P_FOUND] = false;
        AppendLog("Network", "PAKE: no secret hash available for user '" + username + "'");
        return;
    }

    eventData[P_PASSWORDHASH].SetBuffer(pakeSecretHash_, sizeof(pakeSecretHash_));
    eventData[P_FOUND] = true;
    AppendLog("Network", "PAKE: password hash provided for '" + username + "'");
}

void WorkboardManager::HandleClientAuthenticated(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ClientAuthenticated;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    String username = eventData[P_USERNAME].GetString();
    AppendLog("Network", "PAKE: client authenticated — user '" + username + "' from " + conn->ToString());
}

void WorkboardManager::PushWorkboardToClient(Connection* conn)
{
    String content = SerializeSectionsToMarkdown();
    if (content.Empty())
        return;

    VariantMap data;
    data["Markdown"] = content;
    conn->SendRemoteEvent(E_WB_WORKBOARD_FULL, true, data);
}

void WorkboardManager::PushPlanListToClient(Connection* conn)
{
    VariantMap data;
    data["Filenames"] = BuildPlanListString();
    conn->SendRemoteEvent(E_WB_PLAN_LIST, true, data);
}

void WorkboardManager::PushClientListToAll()
{
    VariantMap data;
    data["Clients"] = BuildClientListString();

    for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
        it->first_->SendRemoteEvent(E_WB_CLIENT_LIST, true, data);
}

void WorkboardManager::PushWorkboardToAllClients()
{
    if (wbClients_.Empty())
        return;

    String content = SerializeSectionsToMarkdown();

    VariantMap data;
    data["Markdown"] = content;

    for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
        it->first_->SendRemoteEvent(E_WB_WORKBOARD_FULL, true, data);

    AppendLog("Network", "Pushed workboard update to " + String(wbClients_.Size()) + " client(s)");
}

String WorkboardManager::BuildPlanListString()
{
    String result;
    for (unsigned i = 0; i < planFiles_.Size(); ++i)
    {
        if (i > 0)
            result += "\n";
        result += planFiles_[i];
    }
    return result;
}

String WorkboardManager::BuildClientListString()
{
    String result;
    unsigned idx = 0;
    for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
    {
        if (idx > 0)
            result += "\n";
        result += it->second_.name_ + ":" + it->second_.role_;
        ++idx;
    }
    return result;
}

void WorkboardManager::HandleWbRequestPlan(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace RemoteEventData;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    if (wbClients_.Find(conn) == wbClients_.End())
        return;  // not authenticated

    String filename = eventData["Filename"].GetString();
    if (filename.Empty())
        return;

    // Sanitize — only allow PLAN_*.md files from Claude dir
    if (!filename.StartsWith("PLAN_") || !filename.EndsWith(".md") || filename.Contains(".."))
    {
        AppendLog("Network", "Rejected plan request: " + filename);
        return;
    }

    String path = GetClaudeDir() + filename;
    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
    {
        AppendLog("Network", "Plan not found: " + filename);
        return;
    }

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

    VariantMap data;
    data["Filename"] = filename;
    data["Content"] = content;
    conn->SendRemoteEvent(E_WB_PLAN_CONTENT, true, data);

    AppendLog("Network", "Sent plan " + filename + " to " + wbClients_[conn].name_);
}

void WorkboardManager::HandleWbMutation(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace RemoteEventData;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    if (wbClients_.Find(conn) == wbClients_.End())
        return;  // not authenticated

    String command = eventData["Command"].GetString();
    String args = eventData["Args"].GetString();
    String clientName = wbClients_[conn].name_;

    AppendLog("Network", "Mutation from " + clientName + ": " + command + " " + args);

    // Construct the wb-* command string and run it through existing handler
    String fullCommand = "wb-" + command + " " + args;
    bool success = HandleWorkboardCommand(fullCommand);

    // Send ack
    VariantMap ack;
    ack["Success"] = success;
    ack["Reason"] = success ? String("OK") : String("Command failed: " + fullCommand);
    conn->SendRemoteEvent(E_WB_MUTATION_ACK, true, ack);

    // If mutation succeeded, push updated workboard to all clients
    if (success)
    {
        LoadWorkboard();
        PushWorkboardToAllClients();
    }
}

void WorkboardManager::HandleWbSetIdentity(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace RemoteEventData;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    auto it = wbClients_.Find(conn);
    if (it == wbClients_.End())
        return;  // not authenticated

    String name = eventData["Name"].GetString();
    String role = eventData["Role"].GetString();

    if (!name.Empty())
        it->second_.name_ = name;
    if (!role.Empty())
        it->second_.role_ = role;

    AppendLog("Network", "Client identity updated: " + it->second_.name_ + " (" + it->second_.role_ + ")");
    PushClientListToAll();
}

void WorkboardManager::HandleWbInstanceStatus(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace RemoteEventData;
    auto* conn = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    auto it = wbClients_.Find(conn);
    if (it == wbClients_.End())
        return;

    it->second_.remoteYukiAlive_ = eventData["YukiAlive"].GetBool();
    it->second_.remoteCoderCount_ = eventData["CoderCount"].GetI32();
    it->second_.remoteCoderRoles_ = eventData["CoderRoles"].GetString();
}

void WorkboardManager::SampleSystemStats()
{
#ifdef __linux__
    // ── CPU usage from /proc/stat ──
    {
        File procStat(context_);
        if (procStat.Open("/proc/stat", FILE_READ))
        {
            String line = procStat.ReadLine().Trimmed();

            Vector<String> parts = line.Split(' ');
            // Split may produce empty strings from consecutive spaces — filter
            Vector<String> fields;
            for (unsigned i = 0; i < parts.Size(); ++i)
                if (!parts[i].Trimmed().Empty())
                    fields.Push(parts[i].Trimmed());

            if (fields.Size() >= 5 && fields[0] == "cpu")
            {
                unsigned long long user = strtoull(fields[1].CString(), nullptr, 10);
                unsigned long long nice = strtoull(fields[2].CString(), nullptr, 10);
                unsigned long long system = strtoull(fields[3].CString(), nullptr, 10);
                unsigned long long idle = strtoull(fields[4].CString(), nullptr, 10);
                unsigned long long iowait = fields.Size() > 5 ? strtoull(fields[5].CString(), nullptr, 10) : 0;
                unsigned long long total = user + nice + system + idle + iowait;
                if (fields.Size() > 6) total += strtoull(fields[6].CString(), nullptr, 10);  // irq
                if (fields.Size() > 7) total += strtoull(fields[7].CString(), nullptr, 10);  // softirq
                if (fields.Size() > 8) total += strtoull(fields[8].CString(), nullptr, 10);  // steal

                if (prevCpuTotal_ > 0)
                {
                    unsigned long long dTotal = total - prevCpuTotal_;
                    unsigned long long dIdle = idle - prevCpuIdle_;
                    int pct = (dTotal > 0) ? (int)(100 * (dTotal - dIdle) / dTotal) : 0;
                    if (cpuText_)
                    {
                        cpuText_->SetText("CPU: " + String(pct) + "%");
                        SetBarPercent(cpuBar_, cpuText_->GetParent(), pct);
                    }
                }
                prevCpuTotal_ = total;
                prevCpuIdle_ = idle;
            }
        }
    }

    // ── GPU usage ──
    {
        bool found = false;
        // Try AMD sysfs — scan card0..card7
        for (int card = 0; card < 8 && !found; ++card)
        {
            String path = "/sys/class/drm/card" + String(card) + "/device/gpu_busy_percent";
            File gpuFile(context_);
            if (gpuFile.Open(path, FILE_READ))
            {
                String val = gpuFile.ReadLine().Trimmed();
                if (!val.Empty())
                {
                    if (gpuText_)
                    {
                        gpuText_->SetText("GPU: " + val + "%");
                        SetBarPercent(gpuBar_, gpuText_->GetParent(), atoi(val.CString()));
                    }
                    found = true;
                }
            }
        }
        // Try NVIDIA via nvidia-smi — redirect to temp file, read with Urho File
        if (!found)
        {
            auto* fs = GetSubsystem<FileSystem>();
            if (fs)
            {
                String tmpPath = fs->GetTemporaryDir() + "urho_gpu_query.tmp";
                int ret = fs->SystemCommand("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits > " + tmpPath + " 2>/dev/null");
                if (ret == 0)
                {
                    File tmpFile(context_);
                    if (tmpFile.Open(tmpPath, FILE_READ))
                    {
                        String val = tmpFile.ReadLine().Trimmed();
                        if (!val.Empty() && val != "N/A")
                        {
                            if (gpuText_)
                            {
                                gpuText_->SetText("GPU: " + val + "%");
                                SetBarPercent(gpuBar_, gpuText_->GetParent(), atoi(val.CString()));
                            }
                            found = true;
                        }
                    }
                }
                fs->Delete(tmpPath);
            }
        }
        // Try Intel sysfs
        if (!found)
        {
            File intelGpu(context_);
            if (intelGpu.Open("/sys/class/drm/card0/gt/gt0/rps_act_freq_mhz", FILE_READ))
            {
                String val = intelGpu.ReadLine().Trimmed();
                if (!val.Empty())
                {
                    if (gpuText_)
                        gpuText_->SetText("GPU: " + val + " MHz");
                    found = true;
                }
            }
        }
        if (!found && gpuText_)
            gpuText_->SetText("GPU: N/A");
    }

    // ── RAM from /proc/meminfo ──
    {
        File memFile(context_);
        if (memFile.Open("/proc/meminfo", FILE_READ))
        {
            unsigned long long memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
            while (!memFile.IsEof())
            {
                String line = memFile.ReadLine();
                if (line.StartsWith("MemTotal:"))
                    memTotal = strtoull(line.CString() + 9, nullptr, 10);
                else if (line.StartsWith("MemAvailable:"))
                    memAvail = strtoull(line.CString() + 13, nullptr, 10);
                else if (line.StartsWith("SwapTotal:"))
                    swapTotal = strtoull(line.CString() + 10, nullptr, 10);
                else if (line.StartsWith("SwapFree:"))
                    swapFree = strtoull(line.CString() + 9, nullptr, 10);
            }

            if (memTotal > 0)
            {
                unsigned long long memUsed = memTotal - memAvail;
                int ramPct = (int)(100 * memUsed / memTotal);
                if (ramText_)
                {
                    ramText_->SetText("RAM: " + String(memUsed / 1024) + "/" + String(memTotal / 1024) + "MB (" + String(ramPct) + "%)");
                    SetBarPercent(ramBar_, ramText_->GetParent(), ramPct);
                }
            }
            if (swapTotal > 0)
            {
                unsigned long long swapUsed = swapTotal - swapFree;
                int swapPct = (int)(100 * swapUsed / swapTotal);
                if (swapText_)
                {
                    swapText_->SetText("Swap: " + String(swapUsed / 1024) + "/" + String(swapTotal / 1024) + "MB (" + String(swapPct) + "%)");
                    SetBarPercent(swapBar_, swapText_->GetParent(), swapPct);
                }
            }
            else if (swapText_)
                swapText_->SetText("Swap: none");
        }
    }

    // ── Yuki CPU ──
    {
        yukiLLM_.SampleCpuUsage();
        float yukiPct = yukiLLM_.GetCpuUsage();
        if (yukiCpuText_)
        {
            if (yukiLLM_.IsModelLoaded() || yukiLLM_.IsModelLoading())
                yukiCpuText_->SetText("Yuki: " + String((int)yukiPct) + "%");
            else
                yukiCpuText_->SetText("Yuki: off");
        }
    }

    // ── Disk usage via statvfs ──
    {
        struct statvfs stat;
        if (statvfs(projectRoot_.CString(), &stat) == 0)
        {
            unsigned long long totalGB = (stat.f_blocks * stat.f_frsize) / (1024ULL * 1024 * 1024);
            unsigned long long freeGB = (stat.f_bavail * stat.f_frsize) / (1024ULL * 1024 * 1024);
            unsigned long long usedGB = totalGB - freeGB;
            int diskPct = totalGB > 0 ? (int)(100 * usedGB / totalGB) : 0;
            if (diskText_)
            {
                diskText_->SetText("Disk: " + String((unsigned)usedGB) + "/" + String((unsigned)totalGB) + "GB (" + String(diskPct) + "%)");
                SetBarPercent(diskBar_, diskText_->GetParent(), diskPct);
            }
        }
    }
#else
    if (cpuText_)
        cpuText_->SetText("CPU: N/A");
    if (gpuText_)
        gpuText_->SetText("GPU: N/A");
    if (ramText_)
        ramText_->SetText("RAM: N/A");
    if (swapText_)
        swapText_->SetText("Swap: N/A");
    if (diskText_)
        diskText_->SetText("Disk: N/A");
#endif
}
