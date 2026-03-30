// WorkboardManager — GUI dashboard for workboard, plans, and Claude IPC

#include "WorkboardManager.h"

#include <Urho3D/Container/Sort.h>
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
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#include "PlatformUtils.h"

#include <libsodium/sodium.h>

#include <cstdlib>
#include <cerrno>
#include <ctime>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

URHO3D_DEFINE_APPLICATION_MAIN(WorkboardManager);

// ============================================================================
// Application lifecycle
// ============================================================================

WorkboardManager::WorkboardManager(Context* context) : WorkboardBase(context) {}

void WorkboardManager::Setup()
{
    // ── Singleton guard — fire before engine init (no window, no GPU) ──
    {
        // Use platform temp dir — on Linux: /tmp/urho_claude/, on Windows: %TEMP%/urho_claude/
        String tempBase = GetSubsystem<FileSystem>() ?
            GetSubsystem<FileSystem>()->GetTemporaryDir() + "urho_claude/" :
            String("/tmp/urho_claude/");
        String pidPathStr = tempBase + "manager.pid";
        const char* pidPath = pidPathStr.CString();
        auto* setupFs = GetSubsystem<FileSystem>();
        if (setupFs)
            setupFs->CreateDir(tempBase);

        if (setupFs && setupFs->FileExists(pidPathStr))
        {
            File pidFile(context_, pidPathStr);
            if (pidFile.IsOpen())
            {
                int existingPid = atoi(pidFile.ReadLine().Trimmed().CString());
                pidFile.Close();
                if (existingPid > 0 && IsProcessAlive(existingPid))
                {
                    URHO3D_LOGERROR("WorkboardManager already running (PID " + String(existingPid) + "). Exiting.");
                    exitCode_ = EXIT_FAILURE;
                    return;
                }
            }
        }

        // Write our PID
        {
            File pidFile(context_, pidPathStr, FILE_WRITE);
            if (pidFile.IsOpen())
                pidFile.WriteLine(String(GetCurrentPID()));
        }
    }

    engineParameters_[EP_WINDOW_TITLE] = "Workboard Manager";
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
    ipcDir_ = GetSubsystem<FileSystem>()->GetTemporaryDir() + "urho_claude/";
    ttySockDir_ = ipcDir_ + "tty/";

    projectRoot_ = GetProjectRoot();

    auto* cache = GetSubsystem<ResourceCache>();
    auto* style = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(style);

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
    LoadWorkboard();
    ScanPlanFiles();
    CreateIPCPaths();
    RefreshInstanceStatus();

    // Notify any surviving instances that Manager is back online
    {
        const String onlineMsg = "=== WORKBOARD MANAGER ONLINE === The WorkboardManager is back. "
            "Message delivery restored. Resume normal operations.";
        for (const String& role : knownCoderRoles_)
        {
            if (IsInstanceAlive(role))
                InjectViaTTY(role, onlineMsg);
        }
        if (IsInstanceAlive("planner"))
            InjectViaTTY("planner", onlineMsg);
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                InjectViaTTY(role, onlineMsg);
        }
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
}

void WorkboardManager::Stop()
{
    // Broadcast shutdown notice to all live instances via TTY injection
    const String shutdownMsg = "=== WORKBOARD MANAGER SHUTTING DOWN === The WorkboardManager is temporarily offline. "
        "Continue your current task. TTY injection will resume when Manager restarts. "
        "Do NOT attempt to send messages to Manager until you receive a 'Manager back online' notice.";

    for (const String& role : knownCoderRoles_)
    {
        if (IsInstanceAlive(role))
            InjectViaTTY(role, shutdownMsg);
    }
    if (IsInstanceAlive("planner"))
        InjectViaTTY("planner", shutdownMsg);
    for (const String& role : knownUnassignedRoles_)
    {
        if (IsInstanceAlive(role))
            InjectViaTTY(role, shutdownMsg);
    }

    // Notify remote workboard clients of graceful shutdown
    for (auto it = wbClients_.Begin(); it != wbClients_.End(); ++it)
    {
        if (it->second_.authenticated_ && it->first_)
        {
            VariantMap data;
            data["Success"] = false;
            data["Reason"] = String("Server shutting down");
            it->first_->SendRemoteEvent(E_WB_MUTATION_ACK, true, data);
            it->first_->Disconnect();
        }
    }
    wbClients_.Clear();

    auto* network = GetSubsystem<Network>();
    if (network)
        network->StopServer();

    // Only remove PID file if we own it (a rejected duplicate must not delete the real instance's file)
    if (exitCode_ == EXIT_SUCCESS)
        GetSubsystem<FileSystem>()->Delete(ipcDir_ + "manager.pid");
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
    auto* graphics = GetSubsystem<Graphics>();
    int w = graphics->GetWidth();
    int h = graphics->GetHeight();

    // No root layout — all windows are freely positioned and resizable
    int statusH = 28;
    int composerH = 36;
    int downloadH = 36;
    int themeH = 32;
    int logH = 180;
    int gap = 4;
    int contentTop = statusH + gap;
    int contentH = h - statusH - composerH - downloadH - themeH - logH - gap * 6;
    int halfW = (w - gap * 3) / 2;

    // ── Instance status bar (top, full width) ──
    CreateInstanceStatusBar(uiRoot, w, statusH);

    // ── Workboard window (left) ──
    CreateWorkboardPanel(uiRoot, gap, contentTop, halfW, contentH);

    // ── Plans window (right) ──
    CreatePlanPanel(uiRoot, halfW + gap * 2, contentTop, halfW, contentH);

    // ── Composer bar ──
    int bottomY = contentTop + contentH + gap;
    CreateComposer(uiRoot, gap, bottomY, w - gap * 2, composerH);
    bottomY += composerH + gap;

    // ── Download bar ──
    CreateDownloadBar(uiRoot, gap, bottomY, w - gap * 2, downloadH);
    bottomY += downloadH + gap;

    // ── Theme bar ──
    CreateThemeBar(uiRoot, gap, bottomY, w - gap * 2, themeH);
    bottomY += themeH + gap;

    // ── Message log ──
    CreateMessageLog(uiRoot, gap, bottomY, w - gap * 2, logH);
}

void WorkboardManager::CreateInstanceStatusBar(UIElement* parent, int w, int h)
{
    auto* bar = parent->CreateChild<UIElement>("StatusBar");
    bar->SetPosition(0, 0);
    bar->SetFixedSize(w, h);
    bar->SetLayoutMode(LM_FREE);

    // Distribute evenly across the bar width
    int quarter = w / 4;

    auto* label = bar->CreateChild<Text>();
    label->SetFont(font_, currentFontSize_);
    label->SetText("Instances:");
    label->SetColor(Color(0.6f, 0.6f, 0.6f));
    label->SetPosition(8, 6);

    // Dropdown listing coder instances with PIDs — centered in first quarter
    coderStatusDropdown_ = bar->CreateChild<DropDownList>("CoderStatusDropdown");
    coderStatusDropdown_->SetStyleAuto();
    coderStatusDropdown_->SetFixedSize(quarter - 50, 22);
    coderStatusDropdown_->SetResizePopup(true);
    coderStatusDropdown_->SetPosition(quarter - 40, 3);

    plannerStatusText_ = bar->CreateChild<Text>("PlannerStatus");
    plannerStatusText_->SetFont(font_, currentFontSize_);
    plannerStatusText_->SetText("Planner: OFFLINE");
    plannerStatusText_->SetColor(Color(0.5f, 0.5f, 0.5f));
    plannerStatusText_->SetPosition(quarter * 2, 6);

    unassignedStatusDropdown_ = bar->CreateChild<DropDownList>("UnassignedStatusDropdown");
    unassignedStatusDropdown_->SetStyleAuto();
    unassignedStatusDropdown_->SetFixedSize(quarter - 50, 22);
    unassignedStatusDropdown_->SetResizePopup(true);
    unassignedStatusDropdown_->SetPosition(quarter * 3, 3);
    {
        auto* item = new Text(context_);
        item->SetFont(font_, currentFontSize_);
        item->SetText("Unassigned: none");
        item->SetColor(Color(0.5f, 0.5f, 0.5f));
        item->SetMinSize(170, 20);
        unassignedStatusDropdown_->AddItem(item);
    }
}

// CreateWorkboardPanel() is in WorkboardBase

// CreatePlanPanel() is in WorkboardBase

void WorkboardManager::CreateComposer(UIElement* parent, int x, int y, int w, int h)
{
    auto* bar = parent->CreateChild<BorderImage>("ComposerBar");
    bar->SetStyle("Window");
    bar->SetPosition(x, y);
    bar->SetFixedSize(w, h);
    bar->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));

    messageInput_ = bar->CreateChild<LineEdit>("MsgInput");
    messageInput_->SetStyleAuto();
    messageInput_->SetMinWidth(350);

    // Coder dropdown — lists all connected coder instances
    coderDropdown_ = bar->CreateChild<DropDownList>("CoderDropdown");
    coderDropdown_->SetStyleAuto();
    coderDropdown_->SetFixedSize(100, 28);
    coderDropdown_->SetResizePopup(true);
    // Populated dynamically in RefreshInstanceStatus

    sendCoderBtn_ = bar->CreateChild<Button>("SendCoder");
    sendCoderBtn_->SetStyleAuto();
    sendCoderBtn_->SetFixedSize(50, 28);
    auto* cl = sendCoderBtn_->CreateChild<Text>();
    cl->SetFont(font_, currentFontSize_);
    cl->SetText("Send");
    cl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendCoder));

    sendPlannerBtn_ = bar->CreateChild<Button>("SendPlanner");
    sendPlannerBtn_->SetStyleAuto();
    sendPlannerBtn_->SetFixedSize(80, 28);
    auto* pl = sendPlannerBtn_->CreateChild<Text>();
    pl->SetFont(font_, currentFontSize_);
    pl->SetText("Planner");
    pl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendPlannerBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendPlanner));

    sendUnassignedBtn_ = bar->CreateChild<Button>("SendUnassigned");
    sendUnassignedBtn_->SetStyleAuto();
    sendUnassignedBtn_->SetFixedSize(90, 28);
    auto* ul = sendUnassignedBtn_->CreateChild<Text>();
    ul->SetFont(font_, currentFontSize_);
    ul->SetText("Unassigned");
    ul->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendUnassignedBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendUnassigned));

    sendBroadcastBtn_ = bar->CreateChild<Button>("SendBcast");
    sendBroadcastBtn_->SetStyleAuto();
    sendBroadcastBtn_->SetFixedSize(80, 28);
    auto* bl = sendBroadcastBtn_->CreateChild<Text>();
    bl->SetFont(font_, currentFontSize_);
    bl->SetText("Bcast");
    bl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(sendBroadcastBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSendBroadcast));

    clearFileLocksBtn_ = bar->CreateChild<Button>("ClearLocks");
    clearFileLocksBtn_->SetStyleAuto();
    clearFileLocksBtn_->SetFixedSize(120, 28);
    auto* cfl = clearFileLocksBtn_->CreateChild<Text>();
    cfl->SetFont(font_, currentFontSize_);
    cfl->SetText("Clear File Locks");
    cfl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(clearFileLocksBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleClearFileLocks));

    spawnCoderBtn_ = bar->CreateChild<Button>("SpawnCoder");
    spawnCoderBtn_->SetStyleAuto();
    spawnCoderBtn_->SetFixedSize(100, 28);
    auto* sc = spawnCoderBtn_->CreateChild<Text>();
    sc->SetFont(font_, currentFontSize_);
    sc->SetText("Spawn Coder");
    sc->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(spawnCoderBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleSpawnCoder));
}

void WorkboardManager::CreateDownloadBar(UIElement* parent, int x, int y, int w, int h)
{
    auto* bar = parent->CreateChild<BorderImage>("DownloadBar");
    bar->SetStyle("Window");
    bar->SetPosition(x, y);
    bar->SetFixedSize(w, h);
    bar->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));

    auto* label = bar->CreateChild<Text>("DLLabel");
    label->SetFont(font_, currentFontSize_);
    label->SetText("curl:");
    label->SetColor(Color(0.7f, 0.7f, 0.7f));
    label->SetAlignment(HA_LEFT, VA_CENTER);

    downloadUrlInput_ = bar->CreateChild<LineEdit>("DLUrl");
    downloadUrlInput_->SetStyleAuto();
    downloadUrlInput_->SetMinWidth(500);

    downloadBtn_ = bar->CreateChild<Button>("DLBtn");
    downloadBtn_->SetStyleAuto();
    downloadBtn_->SetFixedSize(80, 28);
    auto* btnText = downloadBtn_->CreateChild<Text>();
    btnText->SetFont(font_, currentFontSize_);
    btnText->SetText("Download");
    btnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(downloadBtn_, "Released", URHO3D_HANDLER(WorkboardManager, HandleDownload));

    downloadStatusText_ = bar->CreateChild<Text>("DLStatus");
    downloadStatusText_->SetFont(font_, currentFontSize_ - 1);
    downloadStatusText_->SetText("Ready");
    downloadStatusText_->SetColor(Color(0.5f, 0.8f, 0.5f));
    downloadStatusText_->SetAlignment(HA_LEFT, VA_CENTER);
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

void WorkboardManager::CreateThemeBar(UIElement* parent, int x, int y, int w, int h)
{
    auto* bar = parent->CreateChild<BorderImage>("ThemeBar");
    bar->SetStyle("Window");
    bar->SetPosition(x, y);
    bar->SetFixedSize(w, h);
    bar->SetLayout(LM_HORIZONTAL, 8, IntRect(8, 4, 8, 4));

    auto* fontLabel = bar->CreateChild<Text>();
    fontLabel->SetFont(font_, currentFontSize_);
    fontLabel->SetText("Font:");
    fontLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    // Font selector dropdown
    fontSelector_ = bar->CreateChild<DropDownList>();
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

    auto* sizeLabel = bar->CreateChild<Text>();
    sizeLabel->SetFont(font_, currentFontSize_);
    sizeLabel->SetText("Size:");
    sizeLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    // Font size dropdown
    fontSizeSelector_ = bar->CreateChild<DropDownList>();
    fontSizeSelector_->SetStyle("DropDownList");
    fontSizeSelector_->SetFixedSize(70, 22);
    fontSizeSelector_->SetResizePopup(true);

    int sizes[] = {9, 10, 11, 12, 13, 14, 16, 18};
    int sizeSelectedIdx = 2;  // default 11
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

void WorkboardManager::CreateMessageLog(UIElement* parent, int x, int y, int w, int h)
{
    logPanel_ = parent->CreateChild<Window>("LogPanel");
    logPanel_->SetStyle("Window");
    logPanel_->SetPosition(x, y);
    logPanel_->SetSize(w, h);
    logPanel_->SetMovable(true);
    logPanel_->SetResizable(true);
    logPanel_->SetResizeBorder(IntRect(6, 6, 6, 6));
    logPanel_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));

    auto* logTitle = logPanel_->CreateChild<Text>("LogTitle");
    logTitle->SetFont(font_, currentFontSize_ + 1);
    logTitle->SetText("MESSAGE LOG");
    logTitle->SetColor(Color(0.6f, 0.6f, 0.6f));

    logListView_ = logPanel_->CreateChild<ListView>("LogList");
    logListView_->SetStyleAuto();
    logListView_->SetMinHeight(100);
}

// ============================================================================
// Workboard Loading & Parsing
// ============================================================================

void WorkboardManager::LoadWorkboard()
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

    // Diagnostic — remove after debugging
    URHO3D_LOGINFOF("LoadWorkboard: read %u bytes, %u sections parsed", size, sections_.Size());
    for (unsigned i = 0; i < sections_.Size(); ++i)
        URHO3D_LOGINFOF("  Section '%s': %u headers, %u rows", sections_[i].title.CString(), sections_[i].headers.Size(), sections_[i].rows.Size());

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

    // Find task in Planned section
    WorkboardSection* planned = FindSection("Planned");
    if (!planned)
    {
        AppendLog("System", "WB assign REJECTED: Planned section not found");
        return false;
    }

    int foundIdx = -1;
    for (unsigned i = 0; i < planned->rows.Size(); ++i)
    {
        if (planned->rows[i].cells.Size() > 0 && planned->rows[i].cells[0].Contains(taskName))
        {
            foundIdx = (int)i;
            break;
        }
    }

    if (foundIdx < 0)
    {
        AppendLog("System", "WB assign REJECTED: '" + taskName + "' not found in Planned");
        return false;
    }

    // Remove from Planned
    planned->rows.Erase((unsigned)foundIdx);

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
    InjectViaTTY(coderRole, msg);
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

    // Fallback: scan *.role files for one matching this role name
    Vector<String> roleFiles;
    fs->ScanDir(roleFiles, instDir, "*.role", SCAN_FILES, false);

    for (const String& filename : roleFiles)
    {
        File rf(context_, instDir + filename);
        if (!rf.IsOpen())
            continue;

        String roleName = rf.ReadLine().Trimmed();
        String pidStr = rf.ReadLine().Trimmed();

        if (role == roleName)
        {
            int pid = atoi(pidStr.CString());
            return pid > 0 ? pid : -1;
        }
    }

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

void WorkboardManager::RefreshInstanceStatus()
{
    // Sweep orphaned .role files from dead sessions before checking liveness
    SweepStaleRoleFiles();

    bool anyChanged = false;

    // --- Planner (singleton) ---
    if (plannerStatusText_)
    {
        bool alive = IsInstanceAlive("planner");
        int pid = ReadInstancePID("planner");
        bool wasOnline = plannerStatusText_->GetText().Contains("ONLINE");
        if (alive != wasOnline) anyChanged = true;

        if (alive)
        {
            plannerStatusText_->SetText("Planner: ONLINE " + String(pid));
            plannerStatusText_->SetColor(Color(0.3f, 1.0f, 0.5f));
        }
        else
        {
            plannerStatusText_->SetText("Planner: OFFLINE");
            plannerStatusText_->SetColor(Color(0.5f, 0.5f, 0.5f));
        }
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
            item->SetText("Unassigned: none");
            item->SetColor(Color(0.5f, 0.5f, 0.5f));
            item->SetMinSize(170, 20);
            unassignedStatusDropdown_->AddItem(item);
        }
        else
        {
            for (const String& role : knownUnassignedRoles_)
            {
                bool alive = IsInstanceAlive(role);
                int pid = ReadInstancePID(role);

                String label = role;
                if (!label.Empty())
                    label[0] = (char)toupper(label[0]);

                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetMinSize(170, 20);

                if (alive)
                {
                    item->SetText(label + "  PID " + String(pid) + "  ONLINE");
                    item->SetColor(Color(0.3f, 1.0f, 0.5f));
                }
                else
                {
                    item->SetText(label + "  OFFLINE");
                    item->SetColor(Color(0.5f, 0.5f, 0.5f));
                }
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

        // Rebuild composer dropdown
        if (coderDropdown_)
        {
            unsigned prevSelection = coderDropdown_->GetSelection();
            coderDropdown_->RemoveAllItems();

            for (const String& role : knownCoderRoles_)
            {
                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetText(role);
                item->SetStyleAuto();
                coderDropdown_->AddItem(item);
            }

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
            item->SetText("Coders: none");
            item->SetColor(Color(0.5f, 0.5f, 0.5f));
            item->SetMinSize(170, 20);
            coderStatusDropdown_->AddItem(item);
        }
        else
        {
            // Count online coders
            unsigned onlineCount = 0;
            for (const String& role : knownCoderRoles_)
            {
                if (IsInstanceAlive(role))
                    ++onlineCount;
            }

            // If more than one coder, add summary header as first item
            if (knownCoderRoles_.Size() > 1)
            {
                auto* header = new Text(context_);
                header->SetFont(font_, currentFontSize_);
                header->SetMinSize(170, 20);
                header->SetText("Coders: " + String(onlineCount) + "/" + String(knownCoderRoles_.Size()));
                header->SetColor(onlineCount > 0 ? Color(0.3f, 1.0f, 0.5f) : Color(0.5f, 0.5f, 0.5f));
                coderStatusDropdown_->AddItem(header);
            }

            for (const String& role : knownCoderRoles_)
            {
                bool alive = IsInstanceAlive(role);
                int pid = ReadInstancePID(role);

                String label = role;
                if (!label.Empty())
                    label[0] = (char)toupper(label[0]);

                auto* item = new Text(context_);
                item->SetFont(font_, currentFontSize_);
                item->SetMinSize(170, 20);

                if (alive)
                {
                    item->SetText(label + "  PID " + String(pid) + "  ONLINE");
                    item->SetColor(Color(0.3f, 1.0f, 0.5f));
                }
                else
                {
                    item->SetText(label + "  OFFLINE");
                    item->SetColor(Color(0.5f, 0.5f, 0.5f));
                }
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

    if (anyChanged)
        UpdateBeacon();
}


bool WorkboardManager::InjectViaTTY(const String& role, const String& message)
{
#ifndef _WIN32
    // Inject message directly into the target's TTY via pty-proxy Unix socket
    String sockPath = ttySockDir_ + role + ".sock";

    struct stat st;
    if (stat(sockPath.CString(), &st) != 0 || !S_ISSOCK(st.st_mode))
        return false;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.CString(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return false;
    }

    // Flatten to single line — multi-line triggers bracketed paste mode in terminal,
    // which swallows the trailing Enter and requires manual user intervention
    String flat = message;
    flat.Replace("\n", " ");
    flat.Replace("\r", "");

    // Bracketed paste: Ink routes content atomically to input buffer
    const char* pasteStart = "\x1b[200~";
    const char* pasteEnd = "\x1b[201~";
    write(fd, pasteStart, 6);
    ssize_t written = write(fd, flat.CString(), flat.Length());
    write(fd, pasteEnd, 6);
    if (written > 0)
    {
        usleep(150000);  // 150ms — let Ink process paste before submit
        write(fd, "\r", 1);  // Enter keypress
    }
    close(fd);

    if (written < 0)
        return false;

    URHO3D_LOGINFOF("InjectViaTTY: sent %d bytes to %s via %s", (int)written, role.CString(), sockPath.CString());
    return true;
#else
    return false;
#endif
}


void WorkboardManager::SendMessage(const String& target, const String& message)
{
    URHO3D_LOGINFOF("SendMessage: target=[%s] message=[%s]", target.CString(), message.CString());
    if (message.Empty())
        return;

    bool injected = InjectViaTTY(target, message);

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

    String target = GetSelectedCoderRole();
    if (target.Empty())
    {
        AppendLog("System", "No coder instance selected");
        return;
    }
    SendMessage(target, text);
    messageInput_->SetText("");
}

void WorkboardManager::HandleSendPlanner(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (!messageInput_) return;
    String text = messageInput_->GetText().Trimmed();
    if (!text.Empty())
    {
        SendMessage("planner", text);
        messageInput_->SetText("");
    }
}

void WorkboardManager::HandleSendUnassigned(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (!messageInput_) return;
    String text = messageInput_->GetText().Trimmed();
    if (!text.Empty())
    {
        // Send to all live unassigned instances
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        messageInput_->SetText("");
    }
}

void WorkboardManager::HandleSendBroadcast(StringHash /*eventType*/, VariantMap& /*eventData*/)
{
    if (!messageInput_) return;
    String text = messageInput_->GetText().Trimmed();
    if (!text.Empty())
    {
        // Send to all coder roles that are alive
        for (const String& role : knownCoderRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        if (IsInstanceAlive("planner"))
            SendMessage("planner", text);
        // Send to all live unassigned instances
        for (const String& role : knownUnassignedRoles_)
        {
            if (IsInstanceAlive(role))
                SendMessage(role, text);
        }
        messageInput_->SetText("");
    }
}

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

    prefix += "[" + source + "] ";

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

void WorkboardManager::CleanupStalePID(const String& role, int pid)
{
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";
    fs->Delete(instDir + role + ".pid");
    fs->Delete(instDir + String(pid) + ".role");
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
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // Collect .role filenames first — removing files while iterating is undefined
    Vector<String> roleFiles;
    fs->ScanDir(roleFiles, instDir, "*.role", SCAN_FILES, false);

    for (const String& filename : roleFiles)
    {
        String rolePath = instDir + filename;

        // Read role name (line 1) and PID (line 2) from the file
        File f(context_, rolePath);
        if (!f.IsOpen())
            continue;

        String roleName = f.ReadLine().Trimmed();
        String pidStr = f.ReadLine().Trimmed();
        f.Close();

        int pid = atoi(pidStr.CString());
        if (pid <= 0)
        {
            // No PID stored (legacy format) — can't check liveness, remove it
            fs->Delete(rolePath);
            AppendLog("System", "Swept role file with no PID: " + filename);
            continue;
        }

        // Is this PID still alive?
        if (IsProcessAlive(pid))
            continue;  // Still alive, keep it

        // Dead — remove the .role file and its matching .pid file
        if (!roleName.Empty())
        {
            String pidFilePath = instDir + roleName + ".pid";
            if (fs->FileExists(pidFilePath))
            {
                File pf(context_, pidFilePath);
                if (pf.IsOpen())
                {
                    int storedPid = atoi(pf.ReadLine().Trimmed().CString());
                    pf.Close();
                    if (storedPid == pid)
                        fs->Delete(pidFilePath);
                }
            }
        }

        fs->Delete(rolePath);
        AppendLog("System", "Swept dead instance: " + filename +
                  " (role=" + roleName + ", PID=" + String(pid) + ")");
    }

    // Also sweep orphaned .pid files whose PID is dead
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
// Multi-Coder Discovery
// ============================================================================

Vector<String> WorkboardManager::DiscoverCoderRoles()
{
    Vector<String> roles;
    auto* fs = GetSubsystem<FileSystem>();
    String instDir = ipcDir_ + "instances/";

    // Scan .role files for any role starting with "coder"
    Vector<String> roleFiles;
    fs->ScanDir(roleFiles, instDir, "*.role", SCAN_FILES, false);

    for (const String& filename : roleFiles)
    {
        File f(context_, instDir + filename);
        if (!f.IsOpen())
            continue;

        String role = f.ReadLine().Trimmed();
        String pidStr = f.ReadLine().Trimmed();

        if (!role.StartsWith("coder"))
            continue;

        int pid = atoi(pidStr.CString());
        if (pid > 0 && IsProcessAlive(pid) && !roles.Contains(role))
        {
            roles.Push(role);
            if (coderActivityTimers_.Find(role) == coderActivityTimers_.End())
                coderActivityTimers_[role] = 0.0f;
        }
    }

    // Also check .pid files (direct role-name format: coder.pid, coder1.pid, etc.)
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

    // Scan .role files for any role starting with "unassigned"
    Vector<String> roleFiles;
    fs->ScanDir(roleFiles, instDir, "*.role", SCAN_FILES, false);

    for (const String& filename : roleFiles)
    {
        File f(context_, instDir + filename);
        if (!f.IsOpen())
            continue;

        String role = f.ReadLine().Trimmed();
        String pidStr = f.ReadLine().Trimmed();

        if (!role.StartsWith("unassigned"))
            continue;

        int pid = atoi(pidStr.CString());
        if (pid > 0 && IsProcessAlive(pid) && !roles.Contains(role))
            roles.Push(role);
    }

    // Also check .pid files (unassigned.pid, unassigned2.pid, etc.)
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
    if (sel < knownCoderRoles_.Size())
        return knownCoderRoles_[sel];

    return String::EMPTY;
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

    CheckDownloadProgress();

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

        // Push workboard to remote clients if it changed
        if (lastWorkboardMtime_ != oldMtime && !wbClients_.Empty())
            PushWorkboardToAllClients();
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
    network->RegisterRemoteEvent(E_WB_REQUEST_PLAN);
    network->RegisterRemoteEvent(E_WB_MUTATION);
    network->RegisterRemoteEvent(E_WB_SET_IDENTITY);
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
