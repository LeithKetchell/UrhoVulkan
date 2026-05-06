// WorkboardManager — GUI dashboard for workboard, plans, and Claude IPC

#include "WorkboardManager.h"

#include <Urho3D/Container/Sort.h>
#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Core/ProcessUtils.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Input/InputEvents.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#include "PlatformUtils.h"

#include <libsodium/sodium.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <climits>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
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

WorkboardManager::WorkboardManager(Context* context) : WorkboardBase(context) {}

void WorkboardManager::Setup()
{
    // ── Singleton guard — abstract Unix socket (kernel-managed, no file to delete) ──
    {
#ifndef _WIN32
        String tempBase("/tmp/urho_claude/");
        auto* setupFs = GetSubsystem<FileSystem>();
        if (setupFs)
        {
            setupFs->CreateDir(tempBase);
            setupFs->CreateDir(tempBase + "instances/");
        }

        // Primary guard: abstract socket. bind() is atomic, namespace is
        // kernel-managed (no filesystem file to accidentally delete), and
        // the socket auto-closes when the process dies. Immune to all the
        // TOCTOU and ghost-inode races that plague flock-on-file approaches.
        singletonLockFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (singletonLockFd_ >= 0)
        {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            // Abstract socket: sun_path[0] = '\0', rest is the name.
            // Abstract sockets live in kernel namespace, not filesystem.
            const char abstractName[] = "\0urho_claude_workboard_manager";
            memcpy(addr.sun_path, abstractName, sizeof(abstractName));
            socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + sizeof(abstractName);

            if (bind(singletonLockFd_, (struct sockaddr*)&addr, addrLen) != 0)
            {
                // EADDRINUSE = another Manager holds this socket = already running
                close(singletonLockFd_);
                singletonLockFd_ = -1;
                URHO3D_LOGERROR("WorkboardManager already running (singleton socket bound). Exiting.");
                exitCode_ = EXIT_FAILURE;
                return;
            }
            // listen() so the socket stays bound for the process lifetime
            listen(singletonLockFd_, 1);
        }
        else
        {
            URHO3D_LOGERROR("Failed to create singleton socket. Exiting.");
            exitCode_ = EXIT_FAILURE;
            return;
        }

        // Singleton confirmed — write PID for other tools to read.
        {
            String pidPath = tempBase + "instances/manager.pid";
            File pidFile(context_, pidPath, FILE_WRITE);
            if (pidFile.IsOpen())
                pidFile.WriteLine(String(GetCurrentPID()));
        }
#endif
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
            {
                // Parse markdown first, then bootstrap DB
                LoadWorkboardFromMarkdown();
                workboardDB_.BootstrapFromSections(sections_);
                AppendLog("System", "SQL backing bootstrapped from WORKBOARD.md");
            }
        }
    }

    LoadWorkboard();
    ScanPlanFiles();
    CreateIPCPaths();
    StartRelaySocket();
    RefreshInstanceStatus();

    // Notify any surviving instances that Manager is back online.
    // Deliver once per unique backend socket. Use specific role names so
    // SendToSocket takes the single-target path (role == "coders" triggers
    // internal broadcast to ALL coder sockets, causing N*M deliveries).
    {
        const String onlineMsg = "=== WORKBOARD MANAGER ONLINE === The WorkboardManager is back. "
            "Message delivery restored. Resume normal operations.";
        HashSet<String> deliveredBackends;

        auto deliverOnce = [&](const String& role)
        {
            if (!IsInstanceAlive(role))
                return;
            String sockPath = ttySockDir_ + role + ".sock";
            char resolved[PATH_MAX];
            ssize_t len = readlink(sockPath.CString(), resolved, sizeof(resolved) - 1);
            String key = sockPath;
            if (len > 0) { resolved[len] = '\0'; key = String(resolved); }
            if (deliveredBackends.Contains(key))
                return;
            deliveredBackends.Insert(key);
            SendToSocket(role, onlineMsg);
        };

        for (const String& role : knownCoderRoles_)
            deliverOnce(role);
        deliverOnce("planner");
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

    // Pre-compute BLAKE2b hash of shared secret for PAKE authentication
    if (!wbSecret_.Empty())
    {
        if (sodium_init() < 0)
            URHO3D_LOGERROR("sodium_init() failed — PAKE auth unavailable");
        else
        {
            crypto_generichash(pakeSecretHash_, sizeof(pakeSecretHash_),
                reinterpret_cast<const unsigned char*>(wbSecret_.CString()), wbSecret_.Length(),
                nullptr, 0);
            pakeSecretValid_ = true;
            URHO3D_LOGINFO("PAKE secret hash computed (BLAKE2b-256)");
        }
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

    GetSubsystem<Input>()->SetMouseVisible(true);
    GetSubsystem<Input>()->SetMouseGrabbed(false);

    AppendLog("System", "WorkboardManager started. Project root: " + projectRoot_);

    // ── Yuki Training Phase 4: kick offline fine-tune if collected data exists ──
    {
        String collectedPath = projectRoot_ + "/Source/Tools/YukiHoho/training/yuki_collected.jsonl";
        auto* fs2 = GetSubsystem<FileSystem>();
        if (fs2->FileExists(collectedPath))
        {
            File checkFile(context_, collectedPath, FILE_READ);
            if (checkFile.IsOpen() && checkFile.GetSize() > 0)
            {
                checkFile.Close();
                AppendLog("Yuki", "Training data found (" + String(checkFile.GetSize()) + " bytes) — checking for finetune script");

                String scriptPath = projectRoot_ + "/Source/Tools/YukiHoho/scripts/finetune.sh";
                if (fs2->FileExists(scriptPath))
                {
                    // Archive collected data before fine-tune consumes it
                    time_t now = time(nullptr);
                    char ts[32];
                    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));
                    String archivePath = projectRoot_ + "/Source/Tools/YukiHoho/training/yuki_collected_" + String(ts) + ".jsonl";
                    fs2->Rename(collectedPath, archivePath);
                    AppendLog("Yuki", "Archived training data to " + GetFileNameAndExtension(archivePath));

                    // Kick fine-tune in background
                    String cmd = "nohup " + scriptPath + " " + archivePath + " > /tmp/yuki_finetune.log 2>&1 &";
                    fs2->SystemCommand(cmd);
                    AppendLog("Yuki", "Fine-tune script launched in background");
                }
                else
                {
                    AppendLog("Yuki", "Training data waiting — no finetune.sh yet (Leith writes this)");
                }
            }
        }
    }
}

void WorkboardManager::Stop()
{
    // Broadcast shutdown notice to all live instances via TTY injection
    const String shutdownMsg = "=== WORKBOARD MANAGER SHUTTING DOWN === The WorkboardManager is temporarily offline. "
        "Continue your current task. TTY injection will resume when Manager restarts. "
        "Do NOT attempt to send messages to Manager until you receive a 'Manager back online' notice.";

    // Deliver once per unique backend socket. SendToSocket("coder") triggers
    // internal broadcast to ALL coder*.sock, so calling it per-role causes
    // N*M deliveries. Instead, resolve every known role to its real socket
    // and deliver exactly once per backend.
    {
        HashSet<String> deliveredBackends;

        auto deliverOnce = [&](const String& role)
        {
            if (!IsInstanceAlive(role))
                return;
            String sockPath = ttySockDir_ + role + ".sock";
            char resolved[PATH_MAX];
            ssize_t len = readlink(sockPath.CString(), resolved, sizeof(resolved) - 1);
            String key = sockPath;
            if (len > 0) { resolved[len] = '\0'; key = String(resolved); }
            if (deliveredBackends.Contains(key))
                return;
            deliveredBackends.Insert(key);
            // Use the specific role name so SendToSocket takes the single-target
            // path, not the "coders" broadcast path.
            SendToSocket(role, shutdownMsg);
        };

        for (const String& role : knownCoderRoles_)
            deliverOnce(role);
        deliverOnce("planner");
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

    // Only remove PID file if we own it (a rejected duplicate must not delete the real instance's file)
    if (exitCode_ == EXIT_SUCCESS)
        GetSubsystem<FileSystem>()->Delete(ipcDir_ + "instances/manager.pid");

    // Release singleton socket (abstract socket auto-unbinds on close)
#ifndef _WIN32
    if (singletonLockFd_ >= 0)
    {
        close(singletonLockFd_);
        singletonLockFd_ = -1;
    }
#endif
}

// ============================================================================
// Helpers
// ============================================================================

// GetProjectRoot() and GetClaudeDir() are in WorkboardBase

// ============================================================================
// UI Creation
// ============================================================================

void WorkboardManager::CreateUI()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    // UV-based layout — all panels expressed as fractions of window size.
    // Anchors recalculate automatically on resize. No pixel math needed.
    //
    // Vertical bands (approximate):
    //   Status bar:   0.000 – 0.035   (instances + settings button)
    //   Content:      0.040 – 0.630   (workboard left, plans right)
    //   Composer:     0.635 – 0.680
    //   Message log:  0.685 – 1.000

    const float pad = 0.003f;  // ~4px at 1280 — small UV gap between panels

    // ── Instance status bar (top, full width) ──
    CreateInstanceStatusBar(uiRoot, pad, 0.0f, 1.0f - pad, 0.035f);

    // ── Workboard window (left half) ──
    CreateWorkboardPanel(uiRoot, pad, 0.04f, 0.5f - pad, 0.63f);

    // ── Plans window (right half, upper) ──
    CreatePlanPanel(uiRoot, 0.5f + pad, 0.04f, 1.0f - pad, 0.40f);

    // ── Yuki chat panel (right half, lower) ──
    CreateYukiChatPanel(uiRoot, 0.5f + pad, 0.40f + pad, 1.0f - pad, 0.63f);

    // ── Composer bar (full width) ──
    CreateComposer(uiRoot, pad, 0.635f, 1.0f - pad, 0.68f);

    // ── Message log (full width, fills bottom) ──
    CreateMessageLog(uiRoot, pad, 0.685f, 1.0f - pad, 1.0f - pad);

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
    bar->SetLayout(LM_HORIZONTAL, 3, IntRect(6, 2, 6, 2));
    bar->SetClipChildren(true);

    // Roles dropdown
    localsDropdown_ = bar->CreateChild<DropDownList>("RolesDropdown");
    localsDropdown_->SetStyleAuto();
    localsDropdown_->SetResizePopup(true);
    localsDropdown_->SetVerticalAlignment(VA_CENTER);

    // Build status
    buildStatusText_ = bar->CreateChild<Text>("BuildStatus");
    buildStatusText_->SetFont(font_, currentFontSize_);
    buildStatusText_->SetText("");
    buildStatusText_->SetColor(COL_GREEN);
    buildStatusText_->SetVerticalAlignment(VA_CENTER);

    // Tools
    toolsBtn_ = bar->CreateChild<Button>("ToolsBtn");
    toolsBtn_->SetStyleAuto();
    toolsBtn_->SetVerticalAlignment(VA_CENTER);
    toolsBtn_->SetClipChildren(true);
    auto* toolsBtnText = toolsBtn_->CreateChild<Text>();
    toolsBtnText->SetFont(font_, currentFontSize_);
    toolsBtnText->SetText("Tools");
    toolsBtnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(toolsBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleToolsToggle));

    // Settings
    settingsBtn_ = bar->CreateChild<Button>("SettingsBtn");
    settingsBtn_->SetStyleAuto();
    settingsBtn_->SetVerticalAlignment(VA_CENTER);
    settingsBtn_->SetClipChildren(true);
    auto* settingsBtnText = settingsBtn_->CreateChild<Text>();
    settingsBtnText->SetFont(font_, currentFontSize_);
    settingsBtnText->SetText("Settings");
    settingsBtnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(settingsBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSettingsToggle));

    // CPU
    cpuText_ = bar->CreateChild<Text>("CpuStatus");
    cpuText_->SetFont(font_, currentFontSize_);
    cpuText_->SetText("CPU: --");
    cpuText_->SetColor(COL_YELLOW);
    cpuText_->SetVerticalAlignment(VA_CENTER);

    // GPU
    gpuText_ = bar->CreateChild<Text>("GpuStatus");
    gpuText_->SetFont(font_, currentFontSize_);
    gpuText_->SetText("GPU: --");
    gpuText_->SetColor(COL_YELLOW);
    gpuText_->SetVerticalAlignment(VA_CENTER);
}

// CreateWorkboardPanel() is in WorkboardBase

// CreatePlanPanel() is in WorkboardBase

void WorkboardManager::CreateComposer(UIElement* parent, float minX, float minY, float maxX, float maxY)
{
    auto* bar = parent->CreateChild<BorderImage>("ComposerBar");
    bar->SetStyle("Window");
    bar->SetOpacity(0.6f);
    bar->SetEnableAnchor(true);
    bar->SetMinAnchor(minX, minY);
    bar->SetMaxAnchor(maxX, maxY);
    bar->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));

    // Message input
    messageInput_ = bar->CreateChild<LineEdit>("MsgInput");
    messageInput_->SetStyle("LineEdit");
    messageInput_->SetFixedSize(200, 24);
    messageInput_->SetVerticalAlignment(VA_CENTER);

    // Receiver dropdown
    coderDropdown_ = bar->CreateChild<DropDownList>("ReceiverDropdown");
    coderDropdown_->SetStyleAuto();
    coderDropdown_->SetFixedSize(110, 24);
    coderDropdown_->SetResizePopup(true);
    coderDropdown_->SetVerticalAlignment(VA_CENTER);

    // Send
    sendCoderBtn_ = bar->CreateChild<Button>("SendBtn");
    sendCoderBtn_->SetStyleAuto();
    sendCoderBtn_->SetFixedSize(100, 24);
    sendCoderBtn_->SetVerticalAlignment(VA_CENTER);
    sendCoderBtn_->SetClipChildren(true);
    auto* cl = sendCoderBtn_->CreateChild<Text>();
    cl->SetFont(font_, currentFontSize_ - 1);
    cl->SetText("Send");
    cl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendCoder));

    // Clear Locks
    clearFileLocksBtn_ = bar->CreateChild<Button>("ClearLocks");
    clearFileLocksBtn_->SetStyleAuto();
    clearFileLocksBtn_->SetFixedSize(120, 24);
    clearFileLocksBtn_->SetVerticalAlignment(VA_CENTER);
    clearFileLocksBtn_->SetClipChildren(true);
    auto* cfl = clearFileLocksBtn_->CreateChild<Text>();
    cfl->SetFont(font_, currentFontSize_ - 1);
    cfl->SetText("Break Locks");
    cfl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(clearFileLocksBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleClearFileLocks));

    // Spawn
    spawnCoderBtn_ = bar->CreateChild<Button>("SpawnCoder");
    spawnCoderBtn_->SetStyleAuto();
    spawnCoderBtn_->SetFixedSize(100, 24);
    spawnCoderBtn_->SetVerticalAlignment(VA_CENTER);
    spawnCoderBtn_->SetClipChildren(true);
    auto* sc = spawnCoderBtn_->CreateChild<Text>();
    sc->SetFont(font_, currentFontSize_ - 1);
    sc->SetText("Spawn");
    sc->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(spawnCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSpawnCoder));
}

void WorkboardManager::CreateToolsPopup()
{
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    toolsPopup_ = uiRoot->CreateChild<Window>("ToolsPopup");
    toolsPopup_->SetStyle("Window");
    toolsPopup_->SetColor(Color(0.09f, 0.078f, 0.129f));
    toolsPopup_->SetFixedSize(600, 50);
    toolsPopup_->SetPosition(uiRoot->GetWidth() - 620, 40);
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
    downloadUrlInput_->SetMinWidth(340);
    downloadUrlInput_->SetFixedHeight(24);

    downloadBtn_ = dlRow->CreateChild<Button>("DLBtn");
    downloadBtn_->SetStyleAuto();
    downloadBtn_->SetFixedSize(80, 24);
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
    settingsPopup_->SetFixedSize(600, 50);
    settingsPopup_->SetPosition(uiRoot->GetWidth() - 620, 95);
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
    fontSelector_->SetFixedSize(200, 22);
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
    fontSizeSelector_->SetFixedSize(70, 22);
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

    logListView_ = logPanel_->CreateChild<ListView>("LogList");
    logListView_->SetStyleAuto();
    logListView_->SetMinHeight(100);
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

    auto* title = yukiChatPanel_->CreateChild<Text>("YukiTitle");
    title->SetFont(font_, currentFontSize_ + 1);
    title->SetText("YUKI");
    title->SetColor(Color(1.0f, 0.5f, 0.8f));

    yukiChatLog_ = yukiChatPanel_->CreateChild<ListView>("YukiLog");
    yukiChatLog_->SetStyleAuto();
    yukiChatLog_->SetMinHeight(60);
}

void WorkboardManager::AppendYukiChat(const String& sender, const String& message)
{
    if (!yukiChatLog_) return;

    auto* item = new Text(context_);
    item->SetFont(font_, currentFontSize_ - 1);
    item->SetWordwrap(true);
    if (yukiChatPanel_)
    {
        int w = yukiChatPanel_->GetWidth();
        if (w > 40)
            item->SetMaxWidth(w - 20);
    }

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

    yukiChatLog_->DisableLayoutUpdate();

    yukiChatLog_->AddItem(item);

    while (yukiChatLog_->GetNumItems() > 200)
        yukiChatLog_->RemoveItem((i32)0);

    yukiChatLog_->EnsureItemVisibility(yukiChatLog_->GetNumItems() - 1);
    yukiChatLog_->EnableLayoutUpdate();
    yukiChatLog_->UpdateLayout();
}

// ============================================================================
// Workboard Loading & Parsing
// ============================================================================

void WorkboardManager::LoadWorkboard()
{
    if (workboardDB_.IsOpen())
    {
        // Preferred path: read from SQL
        // Check DB mtime to avoid redundant reloads
        String dbPath = ipcDir_ + "workboard.db";
        auto* fs = GetSubsystem<FileSystem>();
        unsigned mtime = fs->FileExists(dbPath) ? fs->GetLastModifiedTime(dbPath) : 0;
        if (mtime != 0 && mtime == lastWriteMtime_)
            return;

        sections_ = workboardDB_.LoadAllSections();
        lastWriteMtime_ = mtime;
        RenderWorkboardUI();
        return;
    }

    // Fallback: parse markdown directly
    LoadWorkboardFromMarkdown();
}

void WorkboardManager::LoadWorkboardFromMarkdown()
{
    String path = GetClaudeDir() + "WORKBOARD.md";
    auto* fs = GetSubsystem<FileSystem>();
    if (!fs->FileExists(path))
    {
        AppendLog("System", "WORKBOARD.md not found at: " + path);
        return;
    }

    // Skip reload if we were the last writer (avoid clobbering our own changes)
    unsigned mtime = fs->GetLastModifiedTime(path);
    if (mtime != 0 && mtime == lastWriteMtime_)
        return;

    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
    {
        AppendLog("System", "Failed to open WORKBOARD.md");
        return;
    }

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

    lastWriteMtime_ = mtime;
    ParseWorkboard(content);
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

void WorkboardManager::WriteWorkboard()
{
    String path = GetClaudeDir() + "WORKBOARD.md";

    // Re-read raw file to preserve non-table content
    File readFile(context_, path, FILE_READ);
    if (!readFile.IsOpen()) return;
    unsigned size = readFile.GetSize();
    String raw;
    raw.Resize(size);
    readFile.Read(&raw[0], size);
    readFile.Close();

    Vector<String> lines = raw.Split('\n', false);
    String output;

    WorkboardSection* currentSec = nullptr;
    bool headerEmitted = false;
    bool separatorEmitted = false;
    bool rowsEmitted = false;

    for (unsigned i = 0; i < lines.Size(); ++i)
    {
        String trimmed = lines[i].Trimmed();

        // Detect new section header
        if (trimmed.StartsWith("## "))
        {
            // If previous section had rows pending, emit them
            if (currentSec && !rowsEmitted && separatorEmitted)
                EmitTableRows(output, currentSec);

            String sectionTitle = trimmed.Substring(3).Trimmed();
            currentSec = FindSection(sectionTitle);
            headerEmitted = false;
            separatorEmitted = false;
            rowsEmitted = false;
            output += lines[i] + "\n";
            continue;
        }

        // Inside a section with an in-memory model
        if (currentSec && currentSec->headers.Size() > 0
            && trimmed.StartsWith("|") && trimmed.EndsWith("|"))
        {
            if (!headerEmitted)
            {
                // Header row — pass through
                output += lines[i] + "\n";
                headerEmitted = true;
                continue;
            }
            if (!separatorEmitted && trimmed.Contains("---"))
            {
                // Separator row — pass through, then emit our rows
                output += lines[i] + "\n";
                separatorEmitted = true;
                EmitTableRows(output, currentSec);
                rowsEmitted = true;
                continue;
            }
            // Skip old data rows — we already emitted ours
            if (separatorEmitted)
                continue;
        }

        // Pass through all non-table lines
        output += lines[i] + "\n";
    }

    // Atomic write via .tmp + rename
    String tmpPath = path + ".tmp";
    File writeFile(context_, tmpPath, FILE_WRITE);
    if (writeFile.IsOpen())
    {
        writeFile.Write(output.CString(), output.Length());
        writeFile.Close();
        GetSubsystem<FileSystem>()->Rename(tmpPath, path);
        lastWriteMtime_ = GetSubsystem<FileSystem>()->GetLastModifiedTime(path);
    }
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
        WriteWorkboard();

        // SQL dual-write: re-sync DB from current in-memory sections
        if (workboardDB_.IsOpen())
            workboardDB_.BootstrapFromSections(sections_);

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
    }

    // ── Yuki socket health check ──
    RepairYukiSocket();

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
            if (role == "planner")
                color = COL_BLUE;
            else if (role == "yuki")
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
            if (info.remoteCoderCount_ > 0 || info.remotePlannerAlive_)
            {
                label += " (P:" + String(info.remotePlannerAlive_ ? 1 : 0) +
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
            addReceiverItem("planner", Color(1.0f, 0.8f, 0.3f));
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

    // ── Auto-spawn (doorkeeper) ──
    // When the shop is empty, open the door: planner first, then a coder.
    // Cooldown persists across frames but NOT across Manager restarts.
    // Grace period on startup: wait 60s before first spawn to let existing
    // instances re-register after a Manager restart.
    if (autoSpawnCooldown_ <= 0.0f)
    {
#ifndef _WIN32
        int assumeFd = open("/tmp/urho_claude/assume.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (assumeFd >= 0 && flock(assumeFd, LOCK_EX | LOCK_NB) == 0)
        {
#endif
            bool plannerAlive = IsInstanceAlive("planner");
            bool anyCoderAlive = !DiscoverCoderRoles().Empty();

            if (!plannerAlive)
            {
                AppendLog("Doorkeeper", "No planner alive — spawning");
                auto* fs2 = GetSubsystem<FileSystem>();
                String scriptPath = GetProjectRoot() + "/.claude/hooks/claude_ipc.sh";
                if (fs2->FileExists(scriptPath))
                {
                    fs2->SystemCommand(scriptPath + " spawn-coder");
                    autoSpawnCooldown_ = AUTO_SPAWN_COOLDOWN;
                }
            }
            else if (!anyCoderAlive)
            {
                AppendLog("Doorkeeper", "Planner alive but no coders — spawning");
                auto* fs2 = GetSubsystem<FileSystem>();
                String scriptPath = GetProjectRoot() + "/.claude/hooks/claude_ipc.sh";
                if (fs2->FileExists(scriptPath))
                {
                    fs2->SystemCommand(scriptPath + " spawn-coder");
                    autoSpawnCooldown_ = AUTO_SPAWN_COOLDOWN;
                }
            }

            // Yuki auto-spawn — singleton, independent of planner/coder cooldown
            if (!IsInstanceAlive("yuki") && autoSpawnCooldown_ <= 0.0f)
            {
                String yukiBin = GetProjectRoot() + "/build/bin/YukiHoho";
                auto* fs2 = GetSubsystem<FileSystem>();
                if (fs2->FileExists(yukiBin))
                {
                    AppendLog("Doorkeeper", "Yuki is down — spawning YukiHoho");
                    String cmd = "nohup \"" + yukiBin + "\" > /dev/null 2>&1 &";
                    system(cmd.CString());
                    autoSpawnCooldown_ = AUTO_SPAWN_COOLDOWN;
                }
            }
#ifndef _WIN32
        }
        if (assumeFd >= 0)
            close(assumeFd);
#endif
    }

    if (anyChanged)
        UpdateBeacon();
}


bool WorkboardManager::SendToSocket(const String& role, const String& message, const String& excludeRole)
{
#ifndef _WIN32
    // Broadcast to all coders: scan coder*.sock, write to each
    if (role == "coders")
    {
        Vector<String> sockNames;
        auto* fs = GetSubsystem<FileSystem>();
        fs->ScanDir(sockNames, ttySockDir_, "coder*.sock", SCAN_FILES, false);

        int delivered = 0;
        for (unsigned i = 0; i < sockNames.Size(); ++i)
        {
            // Skip sender by role name (no symlink dedup needed — each socket is real)
            String sockRole = sockNames[i];
            sockRole.Replace(".sock", "");
            if (!excludeRole.Empty() && sockRole == excludeRole)
                continue;

            String sockPath = ttySockDir_ + sockNames[i];
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);

            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            { close(fd); continue; }

            String flat = message;
            flat.Replace("\n", " ");
            flat.Replace("\r", "");

            ssize_t written = write(fd, flat.CString(), flat.Length());
            close(fd);

            if (written > 0)
            {
                ++delivered;
                URHO3D_LOGINFOF("SendToSocket [broadcast]: %s", sockNames[i].CString());
            }
        }

        if (delivered > 0)
        {
            URHO3D_LOGINFOF("SendToSocket: broadcast 'coders' delivered to %d socket(s)", delivered);
            return true;
        }
        URHO3D_LOGWARNING("SendToSocket: broadcast 'coders' found no live sockets");
        return false;
    }

    // Single-target: connect directly to {role}.sock
    String sockPath = ttySockDir_ + role + ".sock";

    struct stat st;
    if (stat(sockPath.CString(), &st) != 0 || !S_ISSOCK(st.st_mode))
    {
        URHO3D_LOGWARNINGF("SendToSocket: no socket for '%s'", role.CString());
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
#endif
}

void WorkboardManager::PollRelaySocket()
{
#ifndef _WIN32
    if (relayListenFd_ < 0)
        return;

    // Accept all pending connections (non-blocking)
    for (;;)
    {
        int clientFd = accept(relayListenFd_, nullptr, nullptr);
        if (clientFd < 0)
            break;

        // Read up to 4KB: "target:message"
        char buf[4096];
        ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
        close(clientFd);

        if (n <= 0)
            continue;
        buf[n] = '\0';

        // Parse "target:sender:message" (sender may be empty for legacy callers)
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
            // Sender roles are short alphanumeric (coder, coder2, planner, etc.)
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

            // Deliver to each socket except sender — no symlink dedup needed,
            // each Claudette binds directly to {role}.sock
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
            continue;
        }

        // __BUILD_REQUEST__:target — coder requests a managed build
        if (message.StartsWith("__BUILD_REQUEST__:"))
        {
            String buildTarget = message.Substring(18).Trimmed();
            EnqueueBuild(buildTarget, target);
            continue;
        }

        // Yuki responses addressed to manager — show in chat panel, don't relay
        if (sender == "yuki" || (target == "yuki" && !sender.Empty() && sender != "yuki"))
        {
            // If Yuki is sending TO manager_relay, she's reporting her inference result
            if (sender == "yuki")
            {
                AppendLog("Yuki", message);
                AppendYukiChat("Yuki", message);
                continue;
            }
        }

        // Log the relay
        AppendLog(String("Relay \xe2\x86\x92 ") + target, message);

        // Deliver — sender excluded from broadcast (no echo-back)
        SendToSocket(target, message, sender);
    }
#endif
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

    // Yuki reads her own socket directly — no TTY injection needed
    if (target == "yuki")
    {
#ifndef _WIN32
        String sockPath = ttySockDir_ + "yuki.sock";
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0)
        {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);

            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
            {
                write(fd, message.CString(), message.Length());
                close(fd);
                AppendLog(String("Manager \xe2\x86\x92 Yuki"), message);
                return;
            }
            close(fd);
        }
        AppendLog(String("Manager \xe2\x86\x92 Yuki [FAILED]"), "Socket connect failed");
#endif
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
        if (IsInstanceAlive("planner"))
            SendMessage("planner", text);
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        if (IsInstanceAlive("yuki"))
            SendMessage("yuki", "cc:" + text);
    }
    else if (target == "yuki")
    {
        SendMessage("yuki", "prompt:" + text);
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
void WorkboardManager::HandleSendPlanner(StringHash, VariantMap&) {}
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
        AppendLog("Manager", "Spawn Coder command executed");
    else
        AppendLog("Manager", "Spawn Coder failed (exit code " + String(ret) + ")");
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
    if (source.Contains("Planner"))
        return Color(1.0f, 0.8f, 0.3f);          // amber
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
    beacon["Planner"]    = IsInstanceAlive("planner") ? String("ONLINE") : String("OFFLINE");
    beacon["Yuki"]       = IsInstanceAlive("yuki") ? String("ONLINE") : String("OFFLINE");
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

void WorkboardManager::RepairYukiSocket()
{
#ifndef _WIN32
    // Cooldown — don't spam on startup or after a recent repair attempt
    if (yukiRepairCooldown_ > 0.0f)
    {
        yukiRepairCooldown_ -= REFRESH_INTERVAL;
        return;
    }

    auto* fs = GetSubsystem<FileSystem>();
    String sockPath = ttySockDir_ + "yuki.sock";
    String pidPath = ipcDir_ + "instances/yuki.pid";
    String lockPath = ipcDir_ + "instances/yuki.lock";

    int yukiPid = ReadInstancePID("yuki");
    bool alive = (yukiPid > 0) && IsProcessAlive(yukiPid);
    bool sockExists = fs->FileExists(sockPath);

    if (alive && sockExists)
        return;  // All good

    if (alive && !sockExists)
    {
        // Yuki is running but socket vanished — she self-heals every ~5s.
        // Just log it and give her time. Don't kill a live process.
        URHO3D_LOGINFO("Yuki PID alive but socket missing — waiting for self-heal");
        yukiRepairCooldown_ = YUKI_REPAIR_INTERVAL;
        return;
    }

    // Yuki is dead (or never started). Clean up stale files and relaunch.
    if (yukiPid > 0 && !alive)
    {
        // Clean stale PID file (but NEVER delete yuki.lock — only Yuki owns that)
        fs->Delete(pidPath);
        unlink(sockPath.CString());  // Remove stale socket file if lingering
    }

    // Find the model file
    String modelPath = projectRoot_ + "/Source/Tools/YukiHoho/models/qwen2.5-coder-3b-instruct-q4_k_m.gguf";
    if (!fs->FileExists(modelPath))
    {
        // Try 1.5b fallback
        modelPath = projectRoot_ + "/Source/Tools/YukiHoho/models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf";
    }
    if (!fs->FileExists(modelPath))
    {
        URHO3D_LOGWARNING("Yuki repair: no model file found, cannot relaunch");
        yukiRepairCooldown_ = 60.0f;  // Don't spam
        return;
    }

    // Launch Yuki — singleton guard in the binary prevents duplicates
    String binPath = fs->GetProgramDir() + "YukiHoho";
    if (!fs->FileExists(binPath))
    {
        URHO3D_LOGWARNING("Yuki repair: binary not found at " + binPath);
        yukiRepairCooldown_ = 60.0f;
        return;
    }

    String cmd = binPath + " --model " + modelPath + " --ctx 4096 > /dev/null 2>&1 &";
    system(cmd.CString());

    AppendLog("Yuki", "Relaunched (singleton guard active)");
    URHO3D_LOGINFO("Yuki repair: launched fresh instance");
    yukiRepairCooldown_ = YUKI_REPAIR_INTERVAL;
#endif
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
    if (role == "planner")
        return lastPlannerActivity_;
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
        AppendLog("System", "Swept orphaned PID file: " + filename);
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
        // Fine-tune just finished — clean up PID and tell Yuki to reload
        fs->Delete(pidLockPath);
        AppendLog("Yuki", "Fine-tune complete — sending reload");
        SendMessage("yuki", "!reload");
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
    lastPlannerActivity_ += timeStep;
    lastUnassignedActivity_ += timeStep;
    for (auto it = coderActivityTimers_.Begin(); it != coderActivityTimers_.End(); ++it)
        it->second_ += timeStep;

    if (autoSpawnCooldown_ > 0.0f)
        autoSpawnCooldown_ -= timeStep;

    CheckDownloadProgress();
    PollRelaySocket();
    ProcessBuildQueue();

    refreshAccumulator_ += timeStep;
    if (refreshAccumulator_ >= REFRESH_INTERVAL)
    {
        refreshAccumulator_ = 0.0f;

        // Track mtime before reload to detect changes
        auto* fs = GetSubsystem<FileSystem>();
        String wbPath = GetClaudeDir() + "WORKBOARD.md";
        unsigned oldMtime = lastWorkboardMtime_;
        if (fs->FileExists(wbPath))
            lastWorkboardMtime_ = fs->GetLastModifiedTime(wbPath);

        LoadWorkboard();
        ScanPlanFiles();
        RefreshInstanceStatus();
        SampleSystemStats();

        // Push workboard to remote clients if it changed
        if (lastWorkboardMtime_ != oldMtime && !wbClients_.Empty())
            PushWorkboardToAllClients();
    }

    // Reconciliation — every 60 seconds, compare DB vs markdown
    if (workboardDB_.IsOpen())
    {
        reconcileAccumulator_ += timeStep;
        if (reconcileAccumulator_ >= RECONCILE_INTERVAL)
        {
            reconcileAccumulator_ = 0.0f;

            // Parse markdown fresh
            String wbPath = GetClaudeDir() + "WORKBOARD.md";
            auto* fs2 = GetSubsystem<FileSystem>();
            if (fs2->FileExists(wbPath))
            {
                File mdFile(context_, wbPath, FILE_READ);
                if (mdFile.IsOpen())
                {
                    unsigned sz = mdFile.GetSize();
                    String mdContent;
                    mdContent.Resize(sz);
                    mdFile.Read(&mdContent[0], sz);
                    mdFile.Close();

                    Vector<WorkboardSection> mdSections;
                    // Temporarily parse into local sections
                    Vector<WorkboardSection> savedSections = sections_;
                    ParseWorkboard(mdContent);
                    mdSections = sections_;
                    sections_ = savedSections;

                    Vector<WorkboardDiscrepancy> discs = workboardDB_.Reconcile(mdSections);
                    if (!discs.Empty())
                    {
                        URHO3D_LOGWARNINGF("WorkboardDB: %u discrepancies found, re-bootstrapping from markdown", discs.Size());
                        for (unsigned d = 0; d < discs.Size(); ++d)
                        {
                            URHO3D_LOGWARNINGF("  %s [%s]: %s",
                                discs[d].type == WorkboardDiscrepancy::MARKDOWN_ONLY ? "MARKDOWN_ONLY" : "DB_ONLY",
                                discs[d].section.CString(), discs[d].taskName.CString());
                        }
                        workboardDB_.BootstrapFromSections(mdSections);
                        // Force a reload from the freshly-synced DB
                        lastWriteMtime_ = 0;
                    }
                }
            }
        }
    }

    // ── Yuki training lump check ──
    trainingCheckAccumulator_ += timeStep;
    if (trainingCheckAccumulator_ >= TRAINING_CHECK_INTERVAL)
    {
        trainingCheckAccumulator_ = 0.0f;
        CheckTrainingLump();
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
    // PAKE: provide the BLAKE2b hash of the shared secret for key exchange
    using namespace KeyExchangeAuth;
    String username = eventData[P_USERNAME].GetString();

    if (!pakeSecretValid_)
    {
        // No secret configured — cannot authenticate
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
    String wbPath = GetClaudeDir() + "WORKBOARD.md";
    File file(context_, wbPath, FILE_READ);
    if (!file.IsOpen())
        return;

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

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

    // Read workboard content once
    String wbPath = GetClaudeDir() + "WORKBOARD.md";
    File file(context_, wbPath, FILE_READ);
    if (!file.IsOpen())
        return;

    unsigned size = file.GetSize();
    String content;
    content.Resize(size);
    file.Read(&content[0], size);
    file.Close();

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

    it->second_.remotePlannerAlive_ = eventData["PlannerAlive"].GetBool();
    it->second_.remoteYukiAlive_ = eventData["YukiAlive"].GetBool();
    it->second_.remoteCoderCount_ = eventData["CoderCount"].GetI32();
    it->second_.remoteCoderRoles_ = eventData["CoderRoles"].GetString();
}

void WorkboardManager::SampleSystemStats()
{
#ifdef __linux__
    // ── CPU usage from /proc/stat ──
    {
        File procStat(context_, "/proc/stat", FILE_READ);
        if (procStat.IsOpen())
        {
            String line = procStat.ReadLine();  // "cpu  user nice system idle iowait irq softirq steal ..."
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
                        cpuText_->SetText("CPU: " + String(pct) + "%");
                }
                prevCpuTotal_ = total;
                prevCpuIdle_ = idle;
            }
        }
    }

    // ── GPU usage ──
    {
        bool found = false;
        // Try AMD sysfs first (no subprocess needed)
        auto* fs = GetSubsystem<FileSystem>();
        if (fs->FileExists("/sys/class/drm/card0/device/gpu_busy_percent"))
        {
            File gpuFile(context_, "/sys/class/drm/card0/device/gpu_busy_percent", FILE_READ);
            if (gpuFile.IsOpen())
            {
                String val = gpuFile.ReadLine().Trimmed();
                if (!val.Empty())
                {
                    if (gpuText_)
                        gpuText_->SetText("GPU: " + val + "%");
                    found = true;
                }
            }
        }
        // Try NVIDIA via nvidia-smi
        if (!found)
        {
            FILE* pipe = popen("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
            if (pipe)
            {
                char buf[64];
                if (fgets(buf, sizeof(buf), pipe))
                {
                    String val(buf);
                    val = val.Trimmed();
                    if (!val.Empty() && val != "N/A")
                    {
                        if (gpuText_)
                            gpuText_->SetText("GPU: " + val + "%");
                        found = true;
                    }
                }
                pclose(pipe);
            }
        }
        if (!found && gpuText_)
            gpuText_->SetText("GPU: N/A");
    }
#else
    if (cpuText_)
        cpuText_->SetText("CPU: N/A");
    if (gpuText_)
        gpuText_->SetText("GPU: N/A");
#endif
}
