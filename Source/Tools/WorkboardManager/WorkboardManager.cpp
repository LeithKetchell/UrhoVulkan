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
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <dirent.h>

URHO3D_DEFINE_APPLICATION_MAIN(WorkboardManager);

// ============================================================================
// Application lifecycle
// ============================================================================

WorkboardManager::WorkboardManager(Context* context) : Application(context) {}

void WorkboardManager::Setup()
{
    // ── Singleton guard — fire before engine init (no window, no GPU) ──
    {
        const char* pidPath = "/tmp/urho_claude/manager.pid";
        mkdir("/tmp/urho_claude", 0777);

        FILE* f = fopen(pidPath, "r");
        if (f)
        {
            int existingPid = 0;
            if (fscanf(f, "%d", &existingPid) == 1 && existingPid > 0 && kill(existingPid, 0) == 0)
            {
                fclose(f);
                fprintf(stderr, "WorkboardManager already running (PID %d). Exiting.\n", existingPid);
                exitCode_ = EXIT_FAILURE;
                return;
            }
            fclose(f);
        }

        // Write our PID
        f = fopen(pidPath, "w");
        if (f)
        {
            fprintf(f, "%d\n", getpid());
            fclose(f);
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
    // Ignore SIGPIPE — writing to a FIFO with no reader would otherwise kill us
    signal(SIGPIPE, SIG_IGN);

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

    CreateUI();
    LoadWorkboard();
    ScanPlanFiles();
    CreateIPCPaths();
    RefreshInstanceStatus();

    // Start beacon server on UDP 31337
    auto* network = GetSubsystem<Network>();
    if (network)
    {
        network->StartServer(BEACON_PORT);
        UpdateBeacon();
        AppendLog("System", "Beacon active on UDP port " + String(BEACON_PORT));
    }

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(WorkboardManager, HandleUpdate));
    SubscribeToEvent(E_KEYDOWN, URHO3D_HANDLER(WorkboardManager, HandleKeyDown));

    GetSubsystem<Input>()->SetMouseVisible(true);
    GetSubsystem<Input>()->SetMouseGrabbed(false);

    AppendLog("System", "WorkboardManager started. Project root: " + projectRoot_);
}

void WorkboardManager::Stop()
{
    auto* network = GetSubsystem<Network>();
    if (network)
        network->StopServer();

    // Only remove PID file if we own it (a rejected duplicate must not delete the real instance's file)
    if (exitCode_ == EXIT_SUCCESS)
        unlink("/tmp/urho_claude/manager.pid");
}

// ============================================================================
// Helpers
// ============================================================================

String WorkboardManager::GetProjectRoot()
{
    // build/bin/ → strip two levels to get project root
    String programDir = GetSubsystem<FileSystem>()->GetProgramDir();
    if (programDir.EndsWith("/"))
        programDir = programDir.Substring(0, programDir.Length() - 1);
    unsigned pos = programDir.FindLast('/');
    if (pos != String::NPOS)
        programDir = programDir.Substring(0, pos);
    pos = programDir.FindLast('/');
    if (pos != String::NPOS)
        programDir = programDir.Substring(0, pos);
    return programDir + "/";
}

String WorkboardManager::GetClaudeDir()
{
    return projectRoot_ + "Claude/";
}

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

void WorkboardManager::CreateWorkboardPanel(UIElement* parent, int x, int y, int w, int h)
{
    workboardPanel_ = parent->CreateChild<Window>("WorkboardPanel");
    workboardPanel_->SetStyle("Window");
    workboardPanel_->SetPosition(x, y);
    workboardPanel_->SetSize(w, h);
    workboardPanel_->SetMovable(true);
    workboardPanel_->SetResizable(true);
    workboardPanel_->SetResizeBorder(IntRect(6, 6, 6, 6));
    workboardPanel_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));

    auto* titleText = workboardPanel_->CreateChild<Text>("WBTitle");
    titleText->SetFont(font_, currentFontSize_ + 3);
    titleText->SetText("WORKBOARD");
    titleText->SetColor(Color(1.0f, 0.9f, 0.5f));

    workboardScroll_ = workboardPanel_->CreateChild<ScrollView>("WBScroll");
    workboardScroll_->SetStyleAuto();
    workboardScroll_->SetFixedHeight(h - 30);  // fill panel minus title

    workboardContent_ = new UIElement(context_);
    workboardContent_->SetLayout(LM_VERTICAL, 2);
    workboardContent_->SetFixedWidth(w - 20);
    workboardScroll_->SetContentElement(workboardContent_);
}

void WorkboardManager::CreatePlanPanel(UIElement* parent, int x, int y, int w, int h)
{
    planPanel_ = parent->CreateChild<Window>("PlanPanel");
    planPanel_->SetStyle("Window");
    planPanel_->SetPosition(x, y);
    planPanel_->SetSize(w, h);
    planPanel_->SetMovable(true);
    planPanel_->SetResizable(true);
    planPanel_->SetResizeBorder(IntRect(6, 6, 6, 6));
    planPanel_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));

    auto* titleText = planPanel_->CreateChild<Text>("PlanTitle");
    titleText->SetFont(font_, currentFontSize_ + 3);
    titleText->SetText("PLANS");
    titleText->SetColor(Color(0.5f, 0.8f, 1.0f));

    // Plan list (top ~30%)
    planListView_ = planPanel_->CreateChild<ListView>("PlanList");
    planListView_->SetStyleAuto();
    planListView_->SetMinHeight(120);
    planListView_->SetMaxHeight(180);
    SubscribeToEvent(planListView_, "ItemClicked", URHO3D_HANDLER(WorkboardManager, HandlePlanSelected));

    // Plan content (bottom ~70%) — fixed height prevents layout from expanding to fit content
    planContentScroll_ = planPanel_->CreateChild<ScrollView>("PlanScroll");
    planContentScroll_->SetStyleAuto();
    int scrollHeight = h - 180;  // panel height minus title + list + padding
    planContentScroll_->SetFixedHeight(scrollHeight > 100 ? scrollHeight : 100);
    planContentScroll_->SetClipChildren(true);
    planContentScroll_->SetScrollBarsAutoVisible(true);
    // Vertical scroll only — no horizontal overflow
    auto* hBar = planContentScroll_->GetHorizontalScrollBar();
    if (hBar)
        hBar->SetVisible(false);

    planContentText_ = new Text(context_);
    planContentText_->SetFont(font_, currentFontSize_);
    planContentText_->SetColor(Color(0.85f, 0.85f, 0.85f));
    planContentText_->SetWordwrap(true);
    planContentText_->SetFixedWidth(w - 20);
    planContentText_->SetText("Select a plan file to view its contents.");
    planContentScroll_->SetContentElement(planContentText_);
}

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

    // Run curl in background via fork — no blocking system() call
    curlPid_ = fork();
    if (curlPid_ == 0)
    {
        // Child process — redirect stdout/stderr to log, exec curl
        int logFd = open("/tmp/urho_curl.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) { dup2(logFd, STDOUT_FILENO); dup2(logFd, STDERR_FILENO); close(logFd); }
        execlp("curl", "curl", "-L", "-o", downloadOutputPath_.CString(), url.CString(), (char*)nullptr);
        _exit(1);  // exec failed
    }

    downloadInProgress_ = (curlPid_ > 0);
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

    // Non-blocking check: if curl PID is stored, check if it's still alive
    if (curlPid_ > 0)
    {
        if (kill(curlPid_, 0) == 0)
            return;  // still running
        curlPid_ = 0;
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

void WorkboardManager::ParseWorkboard(const String& content)
{
    sections_.Clear();

    Vector<String> lines = content.Split('\n');

    WorkboardSection currentSection;
    bool inSection = false;
    bool headersParsed = false;

    for (unsigned i = 0; i < lines.Size(); ++i)
    {
        String line = lines[i].Trimmed();

        if (line.StartsWith("## "))
        {
            if (inSection && !currentSection.headers.Empty())
                sections_.Push(currentSection);

            currentSection = WorkboardSection();
            currentSection.title = line.Substring(3).Trimmed();
            inSection = true;
            headersParsed = false;
            continue;
        }

        if (!inSection)
            continue;

        if (line.StartsWith("|") && line.EndsWith("|"))
        {
            if (line.Contains("---"))
                continue;

            Vector<String> cells;
            Vector<String> parts = line.Split('|');
            for (unsigned j = 0; j < parts.Size(); ++j)
            {
                String cell = parts[j].Trimmed();
                if (!cell.Empty())
                    cells.Push(cell);
            }

            if (!headersParsed)
            {
                currentSection.headers = cells;
                headersParsed = true;
            }
            else
            {
                WorkboardRow row;
                row.cells = cells;
                currentSection.rows.Push(row);
            }
        }
    }

    if (inSection && !currentSection.headers.Empty())
        sections_.Push(currentSection);
}

void WorkboardManager::RenderWorkboardUI()
{
    if (!workboardContent_)
        return;

    // Save scroll position before rebuild
    IntVector2 savedScroll = workboardScroll_ ? workboardScroll_->GetViewPosition() : IntVector2::ZERO;

    // Update content width to match current panel size
    if (workboardPanel_)
        workboardContent_->SetFixedWidth(workboardPanel_->GetWidth() - 20);

    workboardContent_->RemoveAllChildren();

    for (unsigned i = 0; i < sections_.Size(); ++i)
        AddSectionToUI(sections_[i]);

    URHO3D_LOGINFOF("RenderWorkboardUI: %u children, content size %dx%d, panel size %dx%d",
        workboardContent_->GetNumChildren(),
        workboardContent_->GetWidth(), workboardContent_->GetHeight(),
        workboardPanel_ ? workboardPanel_->GetWidth() : -1,
        workboardPanel_ ? workboardPanel_->GetHeight() : -1);

    // Restore scroll position after rebuild
    if (workboardScroll_)
        workboardScroll_->SetViewPosition(savedScroll);
}

void WorkboardManager::AddSectionToUI(const WorkboardSection& section)
{
    Color titleColor(0.7f, 0.7f, 0.7f);
    if (section.title.Contains("Ready"))
        titleColor = Color(1.0f, 0.9f, 0.3f);
    else if (section.title.Contains("In Progress"))
        titleColor = Color(0.3f, 0.9f, 1.0f);
    else if (section.title.Contains("Done"))
        titleColor = Color(0.3f, 1.0f, 0.5f);
    else if (section.title.Contains("Team"))
        titleColor = Color(0.8f, 0.6f, 1.0f);
    else if (section.title.Contains("Coder Status"))
        titleColor = Color(0.3f, 0.9f, 1.0f);
    else if (section.title.Contains("Archive"))
        titleColor = Color(0.5f, 0.5f, 0.5f);

    // Clickable section title — looks like plain text
    auto* titleText = workboardContent_->CreateChild<Text>();
    titleText->SetFont(font_, currentFontSize_ + 2);
    titleText->SetText("> " + section.title);
    titleText->SetColor(titleColor);
    titleText->SetEnabled(true);  // required for click events

    // Content container — collapsed by default
    auto* content = workboardContent_->CreateChild<UIElement>();
    content->SetLayout(LM_VERTICAL, 2);
    content->SetVisible(false);

    // Store content pointer in title for toggle
    titleText->SetVar("SectionContent", content);
    SubscribeToEvent(titleText, E_CLICK, URHO3D_HANDLER(WorkboardManager, HandleSectionToggle));

    // Determine available width for columns
    int totalW = workboardContent_->GetWidth();
    if (totalW < 100) totalW = 900;

    unsigned numCols = section.headers.Size();
    if (numCols == 0)
    {
        for (unsigned r = 0; r < section.rows.Size(); ++r)
        {
            const WorkboardRow& row = section.rows[r];
            String rowLine;
            for (unsigned c = 0; c < row.cells.Size(); ++c)
            {
                if (c > 0) rowLine += "  |  ";
                rowLine += row.cells[c];
            }
            auto* rowText = content->CreateChild<Text>();
            rowText->SetFont(font_, currentFontSize_ - 1);
            rowText->SetText(rowLine);
            rowText->SetColor(Color(0.8f, 0.8f, 0.8f));
            rowText->SetWordwrap(true);
        }
        return;
    }

    // Build column widths
    Vector<int> colWidths;
    int usedW = 0;
    int flexCount = 0;
    colWidths.Resize(numCols);

    for (unsigned h = 0; h < numCols; ++h)
    {
        String hdr = section.headers[h].ToLower().Trimmed();
        if (hdr == "pri")
            colWidths[h] = 30;
        else if (hdr == "owner" || hdr == "started" || hdr == "completed")
            colWidths[h] = 65;
        else if (hdr == "review")
            colWidths[h] = 75;
        else
        {
            colWidths[h] = 0;
            ++flexCount;
        }
        usedW += colWidths[h];
    }

    int remaining = totalW - usedW - 4;
    if (flexCount > 0 && remaining > 0)
    {
        int perFlex = remaining / flexCount;
        for (unsigned h = 0; h < numCols; ++h)
        {
            if (colWidths[h] == 0)
                colWidths[h] = perFlex;
        }
    }

    // Column header row
    auto* headerRow = content->CreateChild<UIElement>("HeaderRow");
    headerRow->SetLayout(LM_HORIZONTAL, 2);
    headerRow->SetFixedHeight(currentFontSize_ + 6);
    Color headerColor = titleColor * 0.7f + Color(0.3f, 0.3f, 0.3f);
    for (unsigned h = 0; h < numCols; ++h)
    {
        auto* cell = headerRow->CreateChild<Text>();
        cell->SetFont(font_, currentFontSize_ - 2);
        cell->SetText(section.headers[h]);
        cell->SetColor(headerColor);
        cell->SetFixedWidth(colWidths[h]);
    }

    // Data rows
    for (unsigned r = 0; r < section.rows.Size(); ++r)
    {
        const WorkboardRow& row = section.rows[r];
        auto* rowElem = content->CreateChild<UIElement>("DataRow");
        rowElem->SetLayout(LM_HORIZONTAL, 2);

        for (unsigned c = 0; c < row.cells.Size() && c < numCols; ++c)
        {
            auto* cell = rowElem->CreateChild<Text>();
            cell->SetFont(font_, currentFontSize_ - 1);
            cell->SetText(row.cells[c].Trimmed());
            cell->SetFixedWidth(colWidths[c]);
            cell->SetWordwrap(true);

            String hdr = (c < numCols) ? section.headers[c].ToLower().Trimmed() : String::EMPTY;
            if (hdr == "review")
            {
                String val = row.cells[c].Trimmed().ToLower();
                if (val.Contains("accepted"))
                    cell->SetColor(Color(0.3f, 1.0f, 0.5f));
                else if (val.Contains("pending"))
                    cell->SetColor(Color(1.0f, 0.9f, 0.3f));
                else if (val.Contains("unacceptable"))
                    cell->SetColor(Color(1.0f, 0.3f, 0.3f));
                else
                    cell->SetColor(Color(0.5f, 0.5f, 0.5f));
            }
            else
            {
                cell->SetColor(Color(0.8f, 0.8f, 0.8f));
            }
        }
    }

    // Separator
    auto* sep = workboardContent_->CreateChild<BorderImage>();
    sep->SetFixedHeight(1);
    sep->SetColor(titleColor * 0.3f);
}

void WorkboardManager::HandleSectionToggle(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace Click;
    auto* titleText = static_cast<Text*>(eventData[P_ELEMENT].GetPtr());
    if (!titleText) return;

    auto* content = static_cast<UIElement*>(titleText->GetVar("SectionContent").GetPtr());
    if (!content) return;

    bool wasVisible = content->IsVisible();
    content->SetVisible(!wasVisible);

    // Toggle prefix: "v " (expanded) / "> " (collapsed)
    String text = titleText->GetText();
    if (wasVisible && text.StartsWith("v "))
        titleText->SetText("> " + text.Substring(2));
    else if (!wasVisible && text.StartsWith("> "))
        titleText->SetText("v " + text.Substring(2));

    // Force parent reflow
    workboardContent_->SetHeight(0);
    workboardContent_->UpdateLayout();
}

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
        rename(tmpPath.CString(), path.CString());
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

        // Non-blocking fork+exec instead of system()
        curlPid_ = fork();
        if (curlPid_ == 0)
        {
            int logFd = open("/tmp/urho_curl.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (logFd >= 0) { dup2(logFd, STDOUT_FILENO); dup2(logFd, STDERR_FILENO); close(logFd); }
            execlp("curl", "curl", "-L", "-o", downloadOutputPath_.CString(), url.CString(), (char*)nullptr);
            _exit(1);
        }

        downloadInProgress_ = (curlPid_ > 0);
        downloadCheckTimer_ = 0.0f;
        if (downloadStatusText_)
        {
            downloadStatusText_->SetText("Downloading...");
            downloadStatusText_->SetColor(Color(1.0f, 0.9f, 0.3f));
        }
        AppendLog("Download", "Started: " + url + " -> " + fullDest);
        return true;
    }

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
    else if (command == "update-review" && fields.Size() >= 2)
        UpdateReview(fields[0].Trimmed(), fields[1].Trimmed());
    else
    {
        AppendLog("System", "Unknown or malformed WB command: " + message);
        return true;
    }

    WriteWorkboard();
    RenderWorkboardUI();
    return true;
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

void WorkboardManager::HandlePlanSelected(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ItemClicked;
    int index = eventData[P_SELECTION].GetI32();
    if (index >= 0 && (unsigned)index < planFiles_.Size())
        LoadPlanContent(planFiles_[index]);
}

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
    mkdir(IPC_DIR, 0777);

    // Create instances directory
    char instDir[128];
    snprintf(instDir, sizeof(instDir), "%s/instances", IPC_DIR);
    mkdir(instDir, 0777);

    // Create spool directories
    mkdir(SPOOL_DIR, 0777);
    const char* roles[] = {"coder", "planner", "unassigned", "manager"};
    for (int i = 0; i < 4; ++i)
    {
        char spoolPath[256];
        snprintf(spoolPath, sizeof(spoolPath), "%s/to_%s", SPOOL_DIR, roles[i]);
        mkdir(spoolPath, 0777);
    }

    // Create wake FIFOs (kept for instant notification — lightweight nudge)
    const char* wakeFifos[] = {
        "/tmp/urho_claude/wake_coder",
        "/tmp/urho_claude/wake_planner",
        "/tmp/urho_claude/wake_unassigned"
    };
    for (int i = 0; i < 3; ++i)
    {
        struct stat st;
        if (stat(wakeFifos[i], &st) != 0)
        {
            if (mkfifo(wakeFifos[i], 0666) != 0)
                URHO3D_LOGERRORF("Failed to create FIFO %s: %s", wakeFifos[i], strerror(errno));
        }
    }
}

// (Activity timer update moved inline — uses HashMap for multi-coder support)

void WorkboardManager::PollSpoolDir(const String& dirName, const String& sourceName, float& activityTimer)
{
    char spoolPath[256];
    snprintf(spoolPath, sizeof(spoolPath), "%s/%s", SPOOL_DIR, dirName.CString());

    DIR* dir = opendir(spoolPath);
    if (!dir)
        return;

    // Collect .msg filenames first, then sort for sequence order
    Vector<String> msgFiles;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".msg") == 0)
            msgFiles.Push(String(entry->d_name));
    }
    closedir(dir);

    if (msgFiles.Empty())
        return;

    Sort(msgFiles.Begin(), msgFiles.End());

    for (const String& filename : msgFiles)
    {
        char filePath[512];
        snprintf(filePath, sizeof(filePath), "%s/%s", spoolPath, filename.CString());

        // Read the entire file
        FILE* f = fopen(filePath, "r");
        if (!f)
            continue;

        // Parse headers and body
        String from, type, body;
        char line[4096];
        bool inBody = false;

        while (fgets(line, sizeof(line), f))
        {
            String sline(line);
            sline = sline.Trimmed();

            if (!inBody)
            {
                if (sline == "---")
                {
                    inBody = true;
                    continue;
                }
                if (sline.StartsWith("From:"))
                    from = sline.Substring(5).Trimmed();
                else if (sline.StartsWith("Type:"))
                    type = sline.Substring(5).Trimmed();
            }
            else
            {
                if (!body.Empty())
                    body += "\n";
                body += sline;
            }
        }
        fclose(f);

        // Delete the message file (consumed)
        remove(filePath);

        if (body.Empty())
            continue;

        // Reset the correct role's liveness timer based on From: header
        if (from == "planner")
            lastPlannerActivity_ = 0.0f;
        else if (from == "unassigned")
            lastUnassignedActivity_ = 0.0f;
        else if (from.StartsWith("coder"))
            coderActivityTimers_[from] = 0.0f;

        // Capitalize source name from header for display
        String displayFrom = from.Empty() ? sourceName : from;
        if (!displayFrom.Empty())
            displayFrom[0] = (char)toupper((unsigned char)displayFrom[0]);

        URHO3D_LOGINFOF("PollSpool: %s [%s] from %s: %s",
            dirName.CString(), type.CString(), displayFrom.CString(), body.CString());

        // Route by message type
        if (type == "command")
        {
            if (!HandleWorkboardCommand(body))
                AppendLog(displayFrom + " \xe2\x86\x92 Manager", body);
        }
        else if (type == "status")
        {
            // Status messages are heartbeats — activity timer already reset above.
            // Optionally log them (currently filtered by AppendLog).
            AppendLog(displayFrom, body);
        }
        else
        {
            // chat, relay, or unknown — display in log
            AppendLog(displayFrom + " \xe2\x86\x92 Manager", body);
        }
    }
}

void WorkboardManager::PollSpool()
{
    // All Claude messages go to to_manager spool.
    // We pass a dummy timer here — real timer update happens inside PollSpoolDir
    // by matching the From: header to the correct role timer.
    float dummyTimer = 0.0f;
    PollSpoolDir("to_manager", "Manager", dummyTimer);
}


// ============================================================================
// Instance discovery & wake-up
// ============================================================================

int WorkboardManager::ReadInstancePID(const String& role)
{
    // Primary: read <role>.pid file
    char path[128];
    snprintf(path, sizeof(path), "%s/instances/%s.pid", IPC_DIR, role.CString());

    FILE* f = fopen(path, "r");
    if (f)
    {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0)
        {
            fclose(f);
            return pid;
        }
        fclose(f);
    }

    // Fallback: scan *.role files for one matching this role name
    char instDir[128];
    snprintf(instDir, sizeof(instDir), "%s/instances", IPC_DIR);
    DIR* dir = opendir(instDir);
    if (!dir)
        return -1;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".role") != 0)
            continue;

        char rolePath[512];
        snprintf(rolePath, sizeof(rolePath), "%s/%s", instDir, entry->d_name);

        FILE* rf = fopen(rolePath, "r");
        if (!rf)
            continue;

        char roleName[64] = {};
        char pidStr[32] = {};
        if (!fgets(roleName, sizeof(roleName), rf)) roleName[0] = '\0';
        if (!fgets(pidStr, sizeof(pidStr), rf)) pidStr[0] = '\0';
        fclose(rf);

        // Strip newlines
        char* nl = strchr(roleName, '\n');
        if (nl) *nl = '\0';
        nl = strchr(pidStr, '\n');
        if (nl) *nl = '\0';

        if (role == roleName)
        {
            int pid = atoi(pidStr);
            closedir(dir);
            return pid > 0 ? pid : -1;
        }
    }

    closedir(dir);
    return -1;
}

bool WorkboardManager::IsInstanceAlive(const String& role)
{
    // Primary: recent FIFO activity
    float lastActivity = GetLastActivity(role);
    if (lastActivity < LIVENESS_TIMEOUT)
        return true;

    // Fallback: PID file + process check
    int pid = ReadInstancePID(role);
    if (pid > 0 && kill(pid, 0) == 0)
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

        if (prevSel < coderStatusDropdown_->GetNumItems())
            coderStatusDropdown_->SetSelection(prevSel);
        else if (coderStatusDropdown_->GetNumItems() > 0)
            coderStatusDropdown_->SetSelection(0);
    }

    if (anyChanged)
        UpdateBeacon();
}

void WorkboardManager::WakeInstance(const String& role)
{
    int pid = ReadInstancePID(role);
    if (pid <= 0 || kill(pid, 0) != 0)
    {
        AppendLog("System", role + " is not running — message saved for next session");
        return;
    }

    // Write to wake FIFO — Claude's background FIFO listener will trigger a task-notification
    char fifoPath[128];
    snprintf(fifoPath, sizeof(fifoPath), "%s/wake_%s", IPC_DIR, role.CString());

    int fd = open(fifoPath, O_WRONLY | O_NONBLOCK);
    if (fd >= 0)
    {
        const char* nudge = "wake\n";
        ssize_t written = write(fd, nudge, strlen(nudge));
        close(fd);
        if (written > 0)
            AppendLog("System", "Woke " + role + " via FIFO");
        else
            AppendLog("System", "Wake FIFO write failed for " + role + ": " + String(strerror(errno)));
    }
    else
    {
        // FIFO not yet created or no reader — message stays in drop-file for next hook check
        AppendLog("System", role + " wake FIFO not available — message queued for next check");
    }
}

void WorkboardManager::SendToSpool(const String& targetRole, const String& from, const String& type, const String& body)
{
    char spoolPath[256];
    snprintf(spoolPath, sizeof(spoolPath), "%s/to_%s", SPOOL_DIR, targetRole.CString());
    mkdir(spoolPath, 0777);

    // Read sequence number
    char seqPath[256];
    snprintf(seqPath, sizeof(seqPath), "%s/.seq", spoolPath);

    int seq = 1;
    FILE* sf = fopen(seqPath, "r");
    if (sf)
    {
        fscanf(sf, "%d", &seq);
        fclose(sf);
    }

    // Write message atomically: .tmp → .msg
    char tmpPath[512], msgPath[512];
    snprintf(tmpPath, sizeof(tmpPath), "%s/%03d_%s.tmp", spoolPath, seq, from.CString());
    snprintf(msgPath, sizeof(msgPath), "%s/%03d_%s.msg", spoolPath, seq, from.CString());

    FILE* f = fopen(tmpPath, "w");
    if (!f)
    {
        URHO3D_LOGERRORF("SendToSpool: failed to create %s: %s", tmpPath, strerror(errno));
        return;
    }

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%S", t);

    fprintf(f, "From: %s\nTime: %s\nType: %s\n---\n%s\n", from.CString(), timeStr, type.CString(), body.CString());
    fclose(f);
    rename(tmpPath, msgPath);

    // Increment sequence
    sf = fopen(seqPath, "w");
    if (sf)
    {
        fprintf(sf, "%d\n", seq + 1);
        fclose(sf);
    }
}

void WorkboardManager::SendMessage(const String& target, const String& message)
{
    URHO3D_LOGINFOF("SendMessage: target=[%s] message=[%s]", target.CString(), message.CString());
    if (message.Empty())
        return;

    SendToSpool(target, "manager", "chat", message);
    AppendLog("Manager \xe2\x86\x92 " + target, message);

    // Wake the instance via FIFO for instant notification
    WakeInstance(target);
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
    const char* lockDir = "/tmp/urho_claude/locks";
    int cleared = 0;

    // Remove all lock directories and files
    Vector<String> entries;
    auto* fs = GetSubsystem<FileSystem>();
    if (fs)
    {
        fs->ScanDir(entries, lockDir, "*", SCAN_FILES | SCAN_DIRS, false);
        for (const String& entry : entries)
        {
            if (entry == "." || entry == "..")
                continue;
            String fullPath = String(lockDir) + "/" + entry;
            // Lock entries are directories (mkdir-based locks)
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
                rmdir(fullPath.CString());
                cleared++;
            }
            else
            {
                // Stale flat files from old locking approach
                fs->Delete(fullPath);
                cleared++;
            }
        }
    }

    AppendLog("Manager", cleared > 0
        ? String("Cleared ") + String(cleared) + " file lock(s)"
        : "No file locks to clear");
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
    char pidFile[128], roleFile[128];
    snprintf(pidFile, sizeof(pidFile), "%s/instances/%s.pid", IPC_DIR, role.CString());
    snprintf(roleFile, sizeof(roleFile), "%s/instances/%d.role", IPC_DIR, pid);
    remove(pidFile);
    remove(roleFile);
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
    char instDir[128];
    snprintf(instDir, sizeof(instDir), "%s/instances", IPC_DIR);

    DIR* dir = opendir(instDir);
    if (!dir)
        return;

    // Collect filenames first — removing files while iterating is undefined
    Vector<String> roleFiles;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".role") == 0)
            roleFiles.Push(String(entry->d_name));
    }
    closedir(dir);

    for (const String& filename : roleFiles)
    {
        char rolePath[512];
        snprintf(rolePath, sizeof(rolePath), "%s/%s", instDir, filename.CString());

        // Read role name (line 1) and PID (line 2) from the file
        FILE* f = fopen(rolePath, "r");
        if (!f)
            continue;

        char roleName[64] = {};
        char pidStr[32] = {};
        if (!fgets(roleName, sizeof(roleName), f)) roleName[0] = '\0';
        if (!fgets(pidStr, sizeof(pidStr), f)) pidStr[0] = '\0';
        fclose(f);

        // Strip newlines
        char* nl = strchr(roleName, '\n');
        if (nl) *nl = '\0';
        nl = strchr(pidStr, '\n');
        if (nl) *nl = '\0';

        int pid = atoi(pidStr);
        if (pid <= 0)
        {
            // No PID stored (legacy format) — can't check liveness, remove it
            remove(rolePath);
            AppendLog("System", String("Swept role file with no PID: ") + filename);
            continue;
        }

        // Is this PID still alive?
        if (kill(pid, 0) == 0)
            continue;  // Still alive, keep it

        // Dead — remove the .role file and its matching .pid file
        if (roleName[0])
        {
            char pidFile[512];
            snprintf(pidFile, sizeof(pidFile), "%s/%s.pid", instDir, roleName);
            FILE* pf = fopen(pidFile, "r");
            if (pf)
            {
                int storedPid = 0;
                if (fscanf(pf, "%d", &storedPid) == 1 && storedPid == pid)
                    remove(pidFile);
                fclose(pf);
            }
        }

        remove(rolePath);
        AppendLog("System", String("Swept dead instance: ") + filename +
                  " (role=" + roleName + ", PID=" + String(pid) + ")");
    }

    // Also sweep orphaned .pid files whose PID is dead
    dir = opendir(instDir);
    if (!dir)
        return;

    Vector<String> pidFiles;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".pid") == 0)
            pidFiles.Push(String(entry->d_name));
    }
    closedir(dir);

    for (const String& filename : pidFiles)
    {
        char pidPath[512];
        snprintf(pidPath, sizeof(pidPath), "%s/%s", instDir, filename.CString());

        FILE* f = fopen(pidPath, "r");
        if (!f)
            continue;

        int pid = 0;
        if (fscanf(f, "%d", &pid) != 1)
            pid = 0;
        fclose(f);

        if (pid > 0 && kill(pid, 0) == 0)
            continue;  // Still alive

        remove(pidPath);
        AppendLog("System", String("Swept orphaned PID file: ") + filename);
    }
}

// ============================================================================
// Multi-Coder Discovery
// ============================================================================

Vector<String> WorkboardManager::DiscoverCoderRoles()
{
    Vector<String> roles;
    char instDir[128];
    snprintf(instDir, sizeof(instDir), "%s/instances", IPC_DIR);

    DIR* dir = opendir(instDir);
    if (!dir)
        return roles;

    // Scan .role files for any role starting with "coder"
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".role") != 0)
            continue;

        char rolePath[512];
        snprintf(rolePath, sizeof(rolePath), "%s/%s", instDir, entry->d_name);

        FILE* f = fopen(rolePath, "r");
        if (!f)
            continue;

        char roleName[64] = {};
        char pidStr[32] = {};
        if (!fgets(roleName, sizeof(roleName), f)) roleName[0] = '\0';
        if (!fgets(pidStr, sizeof(pidStr), f)) pidStr[0] = '\0';
        fclose(f);

        // Strip newlines
        char* nl = strchr(roleName, '\n');
        if (nl) *nl = '\0';
        nl = strchr(pidStr, '\n');
        if (nl) *nl = '\0';

        String role(roleName);
        if (!role.StartsWith("coder"))
            continue;

        // Only include if the PID is alive
        int pid = atoi(pidStr);
        if (pid > 0 && kill(pid, 0) == 0)
        {
            if (!roles.Contains(role))
            {
                roles.Push(role);
                // Ensure activity timer exists for this role
                if (coderActivityTimers_.Find(role) == coderActivityTimers_.End())
                    coderActivityTimers_[role] = 0.0f;
            }
        }
    }
    closedir(dir);

    // Also check .pid files (direct role-name format: coder.pid, coder1.pid, etc.)
    dir = opendir(instDir);
    if (dir)
    {
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || strcmp(dot, ".pid") != 0)
                continue;

            // Extract role name from filename (e.g., "coder1.pid" → "coder1")
            String filename(entry->d_name);
            String role = filename.Substring(0, filename.FindLast('.'));
            if (!role.StartsWith("coder"))
                continue;

            // Check if PID is alive
            char pidPath[512];
            snprintf(pidPath, sizeof(pidPath), "%s/%s", instDir, entry->d_name);
            FILE* f = fopen(pidPath, "r");
            if (!f)
                continue;
            int pid = 0;
            if (fscanf(f, "%d", &pid) != 1) pid = 0;
            fclose(f);

            if (pid > 0 && kill(pid, 0) == 0 && !roles.Contains(role))
            {
                roles.Push(role);
                if (coderActivityTimers_.Find(role) == coderActivityTimers_.End())
                    coderActivityTimers_[role] = 0.0f;
            }
        }
        closedir(dir);
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
    char instDir[128];
    snprintf(instDir, sizeof(instDir), "%s/instances", IPC_DIR);

    DIR* dir = opendir(instDir);
    if (!dir)
        return roles;

    // Scan .role files for any role starting with "unassigned"
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".role") != 0)
            continue;

        char rolePath[512];
        snprintf(rolePath, sizeof(rolePath), "%s/%s", instDir, entry->d_name);

        FILE* f = fopen(rolePath, "r");
        if (!f)
            continue;

        char roleName[64] = {};
        char pidStr[32] = {};
        if (!fgets(roleName, sizeof(roleName), f)) roleName[0] = '\0';
        if (!fgets(pidStr, sizeof(pidStr), f)) pidStr[0] = '\0';
        fclose(f);

        char* nl = strchr(roleName, '\n');
        if (nl) *nl = '\0';
        nl = strchr(pidStr, '\n');
        if (nl) *nl = '\0';

        String role(roleName);
        if (!role.StartsWith("unassigned"))
            continue;

        int pid = atoi(pidStr);
        if (pid > 0 && kill(pid, 0) == 0)
        {
            if (!roles.Contains(role))
                roles.Push(role);
        }
    }
    closedir(dir);

    // Also check .pid files (unassigned.pid, unassigned2.pid, etc.)
    dir = opendir(instDir);
    if (dir)
    {
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || strcmp(dot, ".pid") != 0)
                continue;

            String filename(entry->d_name);
            String role = filename.Substring(0, filename.FindLast('.'));
            if (!role.StartsWith("unassigned"))
                continue;

            char pidPath[512];
            snprintf(pidPath, sizeof(pidPath), "%s/%s", instDir, entry->d_name);
            FILE* f = fopen(pidPath, "r");
            if (!f)
                continue;
            int pid = 0;
            if (fscanf(f, "%d", &pid) != 1) pid = 0;
            fclose(f);

            if (pid > 0 && kill(pid, 0) == 0 && !roles.Contains(role))
                roles.Push(role);
        }
        closedir(dir);
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

    PollSpool();
    CheckDownloadProgress();

    refreshAccumulator_ += timeStep;
    if (refreshAccumulator_ >= REFRESH_INTERVAL)
    {
        refreshAccumulator_ = 0.0f;
        LoadWorkboard();
        ScanPlanFiles();
        RefreshInstanceStatus();
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
