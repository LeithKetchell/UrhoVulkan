// AuthServer — private central authority for TerrainNode network

#include "AuthServer.h"

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Core/Timer.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/IO/MemoryBuffer.h>
#include <Urho3D/IO/VectorBuffer.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Network/Protocol.h>
#include <Urho3D/Graphics/Light.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/Skybox.h>
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/GraphicsAPI/TextureCube.h>
#include <Urho3D/Resource/Image.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>

#include "PlayerCharacter.h"

#include <libsodium/sodium.h>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

URHO3D_DEFINE_APPLICATION_MAIN(AuthServer);

// Controls duplicate-instance behavior:
//   true  — new instance kills the old one and takes over (hot-replace during development)
//   false — new instance dies immediately if an old one is already running (production safety)
#define SERVER_REPLACE_AT_RUNTIME true

// Cross-platform single-instance lock using a lock file with PID.
// Returns true if this instance acquired the lock and may proceed.
#ifdef _WIN32
static HANDLE lockFileHandle_ = INVALID_HANDLE_VALUE;
static bool AcquireInstanceLock()
{
    if (SERVER_REPLACE_AT_RUNTIME)
    {
        // Read old PID and kill it
        HANDLE h = CreateFileA("authserver.lock", GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            char buf[16] = {};
            DWORD bytesRead = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &bytesRead, nullptr);
            CloseHandle(h);
            DWORD oldPid = (DWORD)atoi(buf);
            if (oldPid > 0)
            {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, oldPid);
                if (proc)
                {
                    TerminateProcess(proc, 0);
                    CloseHandle(proc);
                    Sleep(500);  // give it time to release resources
                }
            }
        }
    }
    lockFileHandle_ = CreateFileA("authserver.lock", GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lockFileHandle_ == INVALID_HANDLE_VALUE)
        return false;
    // Write our PID
    char pidBuf[16];
    int pidLen = snprintf(pidBuf, sizeof(pidBuf), "%lu", (unsigned long)GetCurrentProcessId());
    DWORD written;
    WriteFile(lockFileHandle_, pidBuf, pidLen, &written, nullptr);
    FlushFileBuffers(lockFileHandle_);
    return true;
}
static void ReleaseInstanceLock()
{
    if (lockFileHandle_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(lockFileHandle_);
        lockFileHandle_ = INVALID_HANDLE_VALUE;
    }
}
#else
#include <signal.h>
static int lockFileFd_ = -1;
static const char* lockFilePath_ = "/tmp/urho3d_authserver.lock";
static bool AcquireInstanceLock()
{
    // Robust flock pattern: after acquiring the lock, verify the fd's inode
    // matches the on-disk path's inode. If someone deleted+recreated the file
    // between our open() and flock(), we'd hold a lock on a deleted inode while
    // another process locks the new file. Retry from scratch if inodes diverge.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        lockFileFd_ = open(lockFilePath_, O_CREAT | O_RDWR, 0600);
        if (lockFileFd_ < 0)
            return false;

        if (flock(lockFileFd_, LOCK_EX | LOCK_NB) != 0)
        {
            if (!SERVER_REPLACE_AT_RUNTIME)
            {
                close(lockFileFd_);
                lockFileFd_ = -1;
                return false;
            }
            // Kill old instance: read its PID from the lock file
            char buf[16] = {};
            ssize_t n = pread(lockFileFd_, buf, sizeof(buf) - 1, 0); (void)n;
            pid_t oldPid = (pid_t)atoi(buf);
            if (oldPid > 0)
            {
                kill(oldPid, SIGTERM);
                usleep(500000);  // 500ms for graceful shutdown
            }
            // Retry the lock
            if (flock(lockFileFd_, LOCK_EX | LOCK_NB) != 0)
            {
                // Still held — force kill
                if (oldPid > 0)
                    kill(oldPid, SIGKILL);
                usleep(200000);
                if (flock(lockFileFd_, LOCK_EX | LOCK_NB) != 0)
                {
                    close(lockFileFd_);
                    lockFileFd_ = -1;
                    return false;
                }
            }
        }

        // Verify inode: the file we locked must still be the file on disk.
        // If someone deleted the lock file, our fd points to a deleted inode
        // and another process can create+lock a new file at the same path.
        struct stat fdStat = {};
        struct stat pathStat = {};
        if (fstat(lockFileFd_, &fdStat) == 0 && ::stat(lockFilePath_, &pathStat) == 0
            && fdStat.st_dev == pathStat.st_dev && fdStat.st_ino == pathStat.st_ino)
        {
            // Inodes match — we hold the real lock
            break;
        }
        // Inode mismatch — close stale fd and retry
        close(lockFileFd_);
        lockFileFd_ = -1;
    }

    if (lockFileFd_ < 0)
        return false;

    // Write our PID
    int rc = ftruncate(lockFileFd_, 0); (void)rc;
    char pidBuf[16];
    int pidLen = snprintf(pidBuf, sizeof(pidBuf), "%d", (int)getpid());
    ssize_t w = pwrite(lockFileFd_, pidBuf, pidLen, 0); (void)w;
    return true;
}
static void ReleaseInstanceLock()
{
    if (lockFileFd_ >= 0)
    {
        flock(lockFileFd_, LOCK_UN);
        close(lockFileFd_);
        // Do NOT unlink — the file must persist so the next instance detects
        // the same inode via flock. If we unlink, the next instance creates a
        // new inode and flock succeeds without contention, allowing duplicates.
        lockFileFd_ = -1;
    }
}
#endif

// Custom message IDs (must match TerrainNode client)
static const int MSG_AUTH_LOGIN      = 100;
static const int MSG_AUTH_REGISTER   = 101;
static const int MSG_AUTH_RESULT     = 102;
static const int MSG_PATCH_CLAIM     = 110;
static const int MSG_PATCH_QUERY     = 111;
static const int MSG_PATCH_RESULT    = 112;
static const int MSG_LOAD_SCENE      = 103;
static const int MSG_REGISTER_GUID   = 104;  // Client → AuthServer: register NAT GUID after auth
static const int MSG_WEATHER_UPDATE  = 120;  // AuthServer → Client: weather forecast
// Edit messages: MSG_EDIT_TERRAIN (0xA2), MSG_EDIT_OBJECT (0xA3),
// MSG_EDIT_REJECT (0xA4), MSG_EDIT_BROADCAST (0xA5) — defined in Protocol.h
// MSG_PEER_INTRODUCE (0x9C), MSG_PEER_READY (0x9D), MSG_PEER_CONNECT_FAILED (0x9E),
// MSG_PEER_DISCONNECTED (0x9F), MSG_RELAY_TO_AUTH (0xA0), MSG_RELAY_FROM_AUTH (0xA1)
// are defined in Protocol.h

// Remote event for telling clients which avatar node they control
static const StringHash E_CLIENTOBJECTID("ClientObjectID");
static const StringHash P_ID("ID");

// XOR each 4-byte chunk with 0x31337337 (self-inverse: apply twice = original)
static void XorObfuscate(unsigned char* data, unsigned size)
{
    static const unsigned char key[4] = { 0x31, 0x33, 0x73, 0x37 };
    for (unsigned i = 0; i < size; ++i)
        data[i] ^= key[i % 4];
}

static String HexEncode(const unsigned char* data, unsigned size)
{
    String result;
    result.Reserve(size * 2);
    for (unsigned i = 0; i < size; ++i)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        result += String(buf);
    }
    return result;
}

static Vector<unsigned char> HexDecode(const String& hex)
{
    Vector<unsigned char> result;
    result.Resize(hex.Length() / 2);
    for (unsigned i = 0; i < result.Size(); ++i)
    {
        unsigned byte = 0;
        sscanf(hex.CString() + i * 2, "%02x", &byte);
        result[i] = (unsigned char)byte;
    }
    return result;
}

// Hash password with Argon2id, XOR-obfuscate, return hex string
static String HashPasswordArgon2(const String& password)
{
    char hash[crypto_pwhash_STRBYTES];  // 128 bytes
    memset(hash, 0, sizeof(hash));
    int result = crypto_pwhash_str(hash, password.CString(), password.Length(),
            crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE);
    if (result != 0)
    {
        URHO3D_LOGERRORF("HashPasswordArgon2: crypto_pwhash_str failed (result=%d, pw_len=%u, opslimit=%llu, memlimit=%zu)",
            result, password.Length(),
            (unsigned long long)crypto_pwhash_OPSLIMIT_INTERACTIVE,
            (size_t)crypto_pwhash_MEMLIMIT_INTERACTIVE);
        return String::EMPTY;
    }

    unsigned char buf[crypto_pwhash_STRBYTES];
    memcpy(buf, hash, crypto_pwhash_STRBYTES);
    XorObfuscate(buf, crypto_pwhash_STRBYTES);
    return HexEncode(buf, crypto_pwhash_STRBYTES);
}

// Hash password with BLAKE2b (32 bytes), XOR-obfuscate, return hex string
static String HashPasswordBlake2(const String& password)
{
    unsigned char hash[32];
    crypto_generichash(hash, 32,
        reinterpret_cast<const unsigned char*>(password.CString()), password.Length(),
        nullptr, 0);
    XorObfuscate(hash, 32);
    return HexEncode(hash, 32);
}

AuthServer::AuthServer(Context* context) :
    Application(context)
{
}

void AuthServer::Setup()
{
    engineParameters_[EP_WINDOW_TITLE] = "AuthServer";
    engineParameters_[EP_WINDOW_WIDTH] = 720;
    engineParameters_[EP_WINDOW_HEIGHT] = 512;
    engineParameters_[EP_FULL_SCREEN] = false;
    engineParameters_[EP_WINDOW_RESIZABLE] = true;
    engineParameters_[EP_LOG_NAME] = "AuthServer.log";
    engineParameters_[EP_RESOURCE_PATHS] = "CoreData;Data";
}

void AuthServer::Start()
{
    // Prevent duplicate instances on the same machine
    if (!AcquireInstanceLock())
    {
        URHO3D_LOGERROR("[DUPLICATE INSTANCE] AuthServer is already running — this instance will now exit");
        engine_->Exit();
        return;
    }
    URHO3D_LOGINFOF("Instance lock acquired (PID %d, replace=%s)", (int)getpid(), SERVER_REPLACE_AT_RUNTIME ? "true" : "false");

    // Create debug UI first
    CreateUI();
    LogMessage("AuthServer starting...");

    // Initialize database
    InitDatabase();

    // Register PlayerCharacter component for server-side avatar physics
    PlayerCharacter::RegisterObject(context_);

    // Load shared scene file as LOCAL — gives the server collision geometry (terrain, boxes)
    // for physics simulation without replicating any of it to clients.
    // Only REPLICATED nodes (avatars, AI entities) get sent to clients.
    LoadScene();
    RegisterExistingTerrain();
    InitTerrainBrush();
    SetupGodCamera();

    // Generate a test terrain at grid (1,0) to verify the generator works
    // TODO: remove this test once on-demand generation is wired to patch claims
    GenerateTerrainHeightmap(1, 0);

    // Start network server
    auto* network = GetSubsystem<Network>();
    if (!network->StartServer(listenPort_))
    {
        LogMessage("[ERROR] Failed to start server on port " + String(listenPort_));
        return;
    }
    LogMessage("Listening on port " + String(listenPort_));

    // Register remote events that can be sent to clients
    network->RegisterRemoteEvent(E_CLIENTOBJECTID);

    // Set LAN discovery beacon so clients can auto-find us
    VariantMap beacon;
    beacon["ServerName"] = String("AuthServer");
    beacon["Port"] = (int)listenPort_;
    beacon["Version"] = String("1.0");
    network->SetDiscoveryBeacon(beacon);
    LogMessage("LAN discovery beacon active");

    // Subscribe to events
    SubscribeToEvent(E_KEYDOWN, URHO3D_HANDLER(AuthServer, HandleKeyDown));
    SubscribeToEvent(E_CLIENTCONNECTED, URHO3D_HANDLER(AuthServer, HandleClientConnected));
    SubscribeToEvent(E_CLIENTDISCONNECTED, URHO3D_HANDLER(AuthServer, HandleClientDisconnected));
    SubscribeToEvent(E_CLIENTIDENTITY, URHO3D_HANDLER(AuthServer, HandleClientIdentity));
    SubscribeToEvent(E_NETWORKMESSAGE, URHO3D_HANDLER(AuthServer, HandleNetworkMessage));
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(AuthServer, HandleUpdate));
    // PAKE events
    SubscribeToEvent(E_KEYEXCHANGEAUTH, URHO3D_HANDLER(AuthServer, HandleKeyExchangeAuth));
    SubscribeToEvent(E_CLIENTAUTHENTICATED, URHO3D_HANDLER(AuthServer, HandleClientAuthenticated));
    // Physics pre-step: apply client controls to server-side avatars
    SubscribeToEvent(E_PHYSICSPRESTEP, URHO3D_HANDLER(AuthServer, HandlePhysicsPreStep));

    statusText_->SetText("ONLINE — port " + String(listenPort_));
    statusText_->SetColor(Color(0.2f, 1.0f, 0.2f));

    // Melbourne clock — lower right corner
    auto* clockFont = GetSubsystem<ResourceCache>()->GetResource<Font>("Fonts/Anonymous Pro.ttf");
    melbourneClock_ = new MelbourneClock(context_);
    melbourneClock_->Initialize(GetSubsystem<UI>()->GetRoot(), clockFont, 14);

    LogMessage("AuthServer ready.");
}

void AuthServer::Stop()
{
    SaveWaterMap();

    auto* network = GetSubsystem<Network>();
    network->StopServer();

    if (db_)
    {
        auto* database = GetSubsystem<Database>();
        database->Disconnect(db_);
        db_ = nullptr;
    }

    ReleaseInstanceLock();
}

// ============================================================
// Debug GUI
// ============================================================

void AuthServer::CreateUI()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* ui = GetSubsystem<UI>();
    auto* root = ui->GetRoot();

    auto* style = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    root->SetDefaultStyle(style);

    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    // Dark background
    auto* bg = root->CreateChild<BorderImage>("Background");
    bg->SetStyle("Window");
    bg->SetFixedSize(root->GetWidth(), root->GetHeight());
    bg->SetColor(Color(0.12f, 0.12f, 0.15f));
    bg->SetLayout(LM_VERTICAL, 4, IntRect(8, 8, 8, 8));

    // Title bar
    auto* title = bg->CreateChild<Text>("Title");
    title->SetFont(font, 16);
    title->SetText("AuthServer");
    title->SetColor(Color(0.8f, 0.8f, 1.0f));

    CreateMenuBar(bg, font);
    CreateNetworkingPanel(bg, font);
    CreateDatabasePanel(bg, font);
    CreateWeatherPanel(bg, font);
    CreateSceneViewPanel(bg, font);

    SwitchTab(0);  // Networking tab active by default

    GetSubsystem<Input>()->SetMouseVisible(true);
}

void AuthServer::CreateMenuBar(BorderImage* bg, Font* font)
{
    auto* menuBar = bg->CreateChild<BorderImage>("MenuBar");
    menuBar->SetStyle("Window");
    menuBar->SetColor(Color(0.18f, 0.18f, 0.22f));
    menuBar->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
    menuBar->SetFixedHeight(28);

    networkingTab_ = menuBar->CreateChild<Button>("NetworkingTab");
    networkingTab_->SetStyle("Button");
    networkingTab_->SetFixedSize(100, 24);
    auto* netLabel = networkingTab_->CreateChild<Text>("Label");
    netLabel->SetFont(font, 12);
    netLabel->SetText("Networking");
    netLabel->SetAlignment(HA_CENTER, VA_CENTER);

    databaseTab_ = menuBar->CreateChild<Button>("DatabaseTab");
    databaseTab_->SetStyle("Button");
    databaseTab_->SetFixedSize(100, 24);
    auto* dbLabel = databaseTab_->CreateChild<Text>("Label");
    dbLabel->SetFont(font, 12);
    dbLabel->SetText("Database");
    dbLabel->SetAlignment(HA_CENTER, VA_CENTER);

    weatherTab_ = menuBar->CreateChild<Button>("WeatherTab");
    weatherTab_->SetStyle("Button");
    weatherTab_->SetFixedSize(100, 24);
    auto* wxLabel = weatherTab_->CreateChild<Text>("Label");
    wxLabel->SetFont(font, 12);
    wxLabel->SetText("Weather");
    wxLabel->SetAlignment(HA_CENTER, VA_CENTER);

    sceneViewTab_ = menuBar->CreateChild<Button>("SceneViewTab");
    sceneViewTab_->SetStyle("Button");
    sceneViewTab_->SetFixedSize(100, 24);
    auto* svLabel = sceneViewTab_->CreateChild<Text>("Label");
    svLabel->SetFont(font, 12);
    svLabel->SetText("Scene");
    svLabel->SetAlignment(HA_CENTER, VA_CENTER);

    SubscribeToEvent(networkingTab_, "Released", URHO3D_HANDLER(AuthServer, HandleTabClicked));
    SubscribeToEvent(databaseTab_, "Released", URHO3D_HANDLER(AuthServer, HandleTabClicked));
    SubscribeToEvent(weatherTab_, "Released", URHO3D_HANDLER(AuthServer, HandleTabClicked));
    SubscribeToEvent(sceneViewTab_, "Released", URHO3D_HANDLER(AuthServer, HandleTabClicked));
}

void AuthServer::CreateNetworkingPanel(BorderImage* bg, Font* font)
{
    networkingPanel_ = bg->CreateChild<BorderImage>("NetworkingPanel");
    networkingPanel_->SetColor(Color(0.12f, 0.12f, 0.15f, 0.0f));  // transparent bg
    networkingPanel_->SetLayout(LM_VERTICAL, 6, IntRect(0, 4, 0, 0));

    // Status line
    statusText_ = networkingPanel_->CreateChild<Text>("Status");
    statusText_->SetFont(font, 13);
    statusText_->SetText("STARTING...");
    statusText_->SetColor(Color(1.0f, 1.0f, 0.4f));

    // Client count
    clientCountText_ = networkingPanel_->CreateChild<Text>("ClientCount");
    clientCountText_->SetFont(font, 12);
    clientCountText_->SetText("Clients: 0");
    clientCountText_->SetColor(Color(0.7f, 0.7f, 0.7f));

    // Section: Connected Clients
    auto* clientHeader = networkingPanel_->CreateChild<Text>("ClientHeader");
    clientHeader->SetFont(font, 12);
    clientHeader->SetText("--- Connected Clients ---");
    clientHeader->SetColor(Color(0.5f, 0.7f, 1.0f));

    clientList_ = networkingPanel_->CreateChild<ListView>("ClientList");
    clientList_->SetStyle("ListView");
    clientList_->SetMinHeight(100);
    clientList_->SetMaxHeight(140);

    // Section: Activity Log
    auto* logHeader = networkingPanel_->CreateChild<Text>("LogHeader");
    logHeader->SetFont(font, 12);
    logHeader->SetText("--- Activity Log ---");
    logHeader->SetColor(Color(0.5f, 0.7f, 1.0f));

    logList_ = networkingPanel_->CreateChild<ListView>("LogList");
    logList_->SetStyle("ListView");
    logList_->SetMinHeight(200);
}

void AuthServer::CreateDatabasePanel(BorderImage* bg, Font* font)
{
    databasePanel_ = bg->CreateChild<BorderImage>("DatabasePanel");
    databasePanel_->SetColor(Color(0.12f, 0.12f, 0.15f, 0.0f));
    databasePanel_->SetLayout(LM_VERTICAL, 6, IntRect(0, 4, 0, 0));

    // Top bar: table selector + buttons
    auto* topBar = databasePanel_->CreateChild<BorderImage>("TopBar");
    topBar->SetColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
    topBar->SetLayout(LM_HORIZONTAL, 6, IntRect(0, 0, 0, 0));
    topBar->SetFixedHeight(28);

    auto* tableLabel = topBar->CreateChild<Text>("TableLabel");
    tableLabel->SetFont(font, 12);
    tableLabel->SetText("Table:");
    tableLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    tableSelector_ = topBar->CreateChild<DropDownList>("TableSelector");
    tableSelector_->SetStyle("DropDownList");
    tableSelector_->SetFixedSize(160, 24);

    auto* addBtn = topBar->CreateChild<Button>("AddRowBtn");
    addBtn->SetStyle("Button");
    addBtn->SetFixedSize(70, 24);
    auto* addLabel = addBtn->CreateChild<Text>("Label");
    addLabel->SetFont(font, 11);
    addLabel->SetText("Add Row");
    addLabel->SetAlignment(HA_CENTER, VA_CENTER);

    auto* delBtn = topBar->CreateChild<Button>("DeleteRowBtn");
    delBtn->SetStyle("Button");
    delBtn->SetFixedSize(80, 24);
    auto* delLabel = delBtn->CreateChild<Text>("Label");
    delLabel->SetFont(font, 11);
    delLabel->SetText("Delete Row");
    delLabel->SetAlignment(HA_CENTER, VA_CENTER);

    auto* refreshBtn = topBar->CreateChild<Button>("RefreshBtn");
    refreshBtn->SetStyle("Button");
    refreshBtn->SetFixedSize(70, 24);
    auto* refreshLabel = refreshBtn->CreateChild<Text>("Label");
    refreshLabel->SetFont(font, 11);
    refreshLabel->SetText("Refresh");
    refreshLabel->SetAlignment(HA_CENTER, VA_CENTER);

    // Column headers
    tableSchemaText_ = databasePanel_->CreateChild<Text>("TableSchema");
    tableSchemaText_->SetFont(font, 11);
    tableSchemaText_->SetColor(Color(0.5f, 0.7f, 1.0f));

    // Table data view
    tableView_ = databasePanel_->CreateChild<ListView>("TableView");
    tableView_->SetStyle("ListView");
    tableView_->SetMinHeight(120);
    tableView_->SetMaxHeight(200);

    // SQL Console section
    auto* sqlLabel = databasePanel_->CreateChild<Text>("SqlLabel");
    sqlLabel->SetFont(font, 12);
    sqlLabel->SetText("--- SQL Console ---");
    sqlLabel->SetColor(Color(0.5f, 0.7f, 1.0f));

    auto* sqlBar = databasePanel_->CreateChild<BorderImage>("SqlBar");
    sqlBar->SetColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
    sqlBar->SetLayout(LM_HORIZONTAL, 4, IntRect(0, 0, 0, 0));
    sqlBar->SetFixedHeight(28);

    sqlInput_ = sqlBar->CreateChild<LineEdit>("SqlInput");
    sqlInput_->SetStyle("LineEdit");
    sqlInput_->SetFixedHeight(24);
    sqlInput_->SetMinWidth(500);

    auto* runBtn = sqlBar->CreateChild<Button>("RunBtn");
    runBtn->SetStyle("Button");
    runBtn->SetFixedSize(50, 24);
    auto* runLabel = runBtn->CreateChild<Text>("Label");
    runLabel->SetFont(font, 11);
    runLabel->SetText("Run");
    runLabel->SetAlignment(HA_CENTER, VA_CENTER);

    // SQL results
    sqlResultView_ = databasePanel_->CreateChild<ListView>("SqlResultView");
    sqlResultView_->SetStyle("ListView");
    sqlResultView_->SetMinHeight(100);

    // Wire events
    SubscribeToEvent(tableSelector_, "ItemSelected", URHO3D_HANDLER(AuthServer, HandleTableSelected));
    SubscribeToEvent(addBtn, "Released", URHO3D_HANDLER(AuthServer, HandleAddRow));
    SubscribeToEvent(delBtn, "Released", URHO3D_HANDLER(AuthServer, HandleDeleteRow));
    SubscribeToEvent(refreshBtn, "Released", URHO3D_HANDLER(AuthServer, HandleTableSelected));  // reuse to refresh
    SubscribeToEvent(runBtn, "Released", URHO3D_HANDLER(AuthServer, HandleSqlExecute));
    SubscribeToEvent(tableView_, "ItemDoubleClicked", URHO3D_HANDLER(AuthServer, HandleEditRow));
}

void AuthServer::CreateWeatherPanel(BorderImage* bg, Font* font)
{
    weatherPanel_ = bg->CreateChild<BorderImage>("WeatherPanel");
    weatherPanel_->SetColor(Color(0.12f, 0.12f, 0.15f, 0.0f));
    weatherPanel_->SetLayout(LM_VERTICAL, 8, IntRect(8, 8, 8, 8));

    // Title
    auto* title = weatherPanel_->CreateChild<Text>("WeatherTitle");
    title->SetFont(font, 14);
    title->SetText("Melbourne Weather (BOM)");
    title->SetColor(Color(0.5f, 0.8f, 1.0f));

    // Condition
    weatherConditionText_ = weatherPanel_->CreateChild<Text>("Condition");
    weatherConditionText_->SetFont(font, 16);
    weatherConditionText_->SetText("Awaiting first fetch...");
    weatherConditionText_->SetColor(Color(1.0f, 1.0f, 0.8f));

    // Temperature
    weatherTempText_ = weatherPanel_->CreateChild<Text>("Temperature");
    weatherTempText_->SetFont(font, 13);
    weatherTempText_->SetText("Temperature: --");
    weatherTempText_->SetColor(Color(0.9f, 0.9f, 0.9f));

    // Humidity
    weatherHumidityText_ = weatherPanel_->CreateChild<Text>("Humidity");
    weatherHumidityText_->SetFont(font, 13);
    weatherHumidityText_->SetText("Humidity: --");
    weatherHumidityText_->SetColor(Color(0.9f, 0.9f, 0.9f));

    // Wind
    weatherWindText_ = weatherPanel_->CreateChild<Text>("Wind");
    weatherWindText_->SetFont(font, 13);
    weatherWindText_->SetText("Wind: --");
    weatherWindText_->SetColor(Color(0.9f, 0.9f, 0.9f));

    // Cloud cover
    weatherCloudText_ = weatherPanel_->CreateChild<Text>("CloudCover");
    weatherCloudText_->SetFont(font, 13);
    weatherCloudText_->SetText("Cloud Cover: --");
    weatherCloudText_->SetColor(Color(0.9f, 0.9f, 0.9f));

    // Precipitation
    weatherPrecipText_ = weatherPanel_->CreateChild<Text>("Precipitation");
    weatherPrecipText_->SetFont(font, 13);
    weatherPrecipText_->SetText("Precipitation: --");
    weatherPrecipText_->SetColor(Color(0.9f, 0.9f, 0.9f));

    // Separator
    auto* sep = weatherPanel_->CreateChild<Text>("Sep");
    sep->SetFont(font, 10);
    sep->SetText("---");
    sep->SetColor(Color(0.4f, 0.4f, 0.4f));

    // Last fetch time
    weatherFetchTimeText_ = weatherPanel_->CreateChild<Text>("FetchTime");
    weatherFetchTimeText_->SetFont(font, 11);
    weatherFetchTimeText_->SetText("Last Fetch: never");
    weatherFetchTimeText_->SetColor(Color(0.6f, 0.6f, 0.6f));

    // Broadcast info
    weatherBroadcastText_ = weatherPanel_->CreateChild<Text>("Broadcast");
    weatherBroadcastText_->SetFont(font, 11);
    weatherBroadcastText_->SetText("Broadcast: on join + every 3 hours");
    weatherBroadcastText_->SetColor(Color(0.6f, 0.6f, 0.6f));
}

void AuthServer::RefreshWeatherPanel()
{
    if (!weatherReady_)
        return;

    // Pretty-print the condition
    String condition = weatherCondition_;
    condition.Replace("_", " ");
    // Capitalize first letter
    if (condition.Length() > 0)
        condition[0] = (char)toupper(condition[0]);

    if (weatherConditionText_)
        weatherConditionText_->SetText(condition);

    char buf[64];

    if (weatherTempText_)
    {
        snprintf(buf, sizeof(buf), "Temperature: %.1f C", weatherTemperature_);
        weatherTempText_->SetText(buf);
    }

    if (weatherHumidityText_)
    {
        snprintf(buf, sizeof(buf), "Humidity: %.0f%%", weatherHumidity_);
        weatherHumidityText_->SetText(buf);
    }

    if (weatherWindText_)
    {
        // Convert angle back to compass direction for display
        const char* dirs[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                              "S","SSW","SW","WSW","W","WNW","NW","NNW"};
        int dirIdx = ((int)(weatherWindAngle_ / 22.5f + 0.5f)) % 16;
        if (weatherWindSpeed_ < 0.5f)
            snprintf(buf, sizeof(buf), "Wind: Calm");
        else
            snprintf(buf, sizeof(buf), "Wind: %.0f km/h %s", weatherWindSpeed_, dirs[dirIdx]);
        weatherWindText_->SetText(buf);
    }

    if (weatherCloudText_)
    {
        snprintf(buf, sizeof(buf), "Cloud Cover: %d%%", (int)(weatherCloudCover_ * 100));
        weatherCloudText_->SetText(buf);
    }

    if (weatherPrecipText_)
    {
        snprintf(buf, sizeof(buf), "Precipitation: %d%%", (int)(weatherPrecipitation_ * 100));
        weatherPrecipText_->SetText(buf);
    }

    if (weatherFetchTimeText_)
    {
        int mins = (int)(uptime_ / 60.0f);
        int secs = (int)uptime_ % 60;
        snprintf(buf, sizeof(buf), "Last Fetch: %dm %ds uptime", mins, secs);
        weatherFetchTimeText_->SetText(buf);
    }

    if (weatherBroadcastText_)
    {
        float nextBroadcast = weatherBroadcastTimer_;
        int nextMins = (int)(nextBroadcast / 60.0f);
        snprintf(buf, sizeof(buf), "Next Broadcast: %d min (on join + every 3h)", nextMins);
        weatherBroadcastText_->SetText(buf);
    }
}

void AuthServer::CreateSceneViewPanel(BorderImage* bg, Font* font)
{
    sceneViewPanel_ = bg->CreateChild<BorderImage>("SceneViewPanel");
    sceneViewPanel_->SetColor(Color(0.0f, 0.0f, 0.0f, 0.0f));  // transparent — 3D renders behind
    sceneViewPanel_->SetLayout(LM_VERTICAL, 4, IntRect(8, 8, 8, 8));

    // Instructions
    auto* instrText = sceneViewPanel_->CreateChild<Text>("SceneInstr");
    instrText->SetFont(font, 11);
    instrText->SetText("RMB+WASD: fly camera  |  LMB: raise water  |  MMB: lower water  |  Scroll: brush size");
    instrText->SetColor(Color(0.6f, 0.6f, 0.7f));

    // Scene stats (updated each frame)
    sceneStatsText_ = sceneViewPanel_->CreateChild<Text>("SceneStats");
    sceneStatsText_->SetFont(font, 11);
    sceneStatsText_->SetText("Nodes: -- | Clients: --");
    sceneStatsText_->SetColor(Color(0.5f, 0.8f, 0.5f));

    // Brush info
    auto* brushText = sceneViewPanel_->CreateChild<Text>("BrushInfo");
    brushText->SetFont(font, 11);
    brushText->SetText("Water Brush: radius 8, strength 0.02");
    brushText->SetColor(Color(0.4f, 0.6f, 0.9f));
    brushText->SetVar("IsBrushInfo", true);
}

void AuthServer::PaintWater(float worldX, float worldZ, bool raise)
{
    if (!waterHeightMap_)
        return;

    int width = waterHeightMap_->GetWidth();
    int height = waterHeightMap_->GetHeight();

    // Convert world coords to pixel coords (same math as HandleWaterEdit)
    float terrainSpacingX = 2.0f;
    float terrainSpacingZ = 2.0f;
    float halfSize = (width - 1) * terrainSpacingX * 0.5f;

    int centerPX = (int)((worldX + halfSize) / terrainSpacingX);
    int centerPZ = (int)((worldZ + halfSize) / terrainSpacingZ);
    int pixelRadius = (int)(waterBrushRadius_ / terrainSpacingX);

    HashSet<unsigned long long> touchedPatches;
    int modified = 0;

    for (int pz = Max(0, centerPZ - pixelRadius); pz <= Min(height - 1, centerPZ + pixelRadius); ++pz)
    {
        for (int px = Max(0, centerPX - pixelRadius); px <= Min(width - 1, centerPX + pixelRadius); ++px)
        {
            float dx = (float)(px - centerPX);
            float dz = (float)(pz - centerPZ);
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist > pixelRadius)
                continue;

            float falloff = 1.0f - (dist / (float)pixelRadius);
            float delta = waterBrushStrength_ * falloff;

            Color c = waterHeightMap_->GetPixel(px, pz);
            float val = c.r_;
            if (raise)
                val = Min(val + delta, 1.0f);
            else
                val = Max(val - delta, 0.0f);
            waterHeightMap_->SetPixel(px, pz, Color(val, val, val));
            ++modified;

            // Record which patch this pixel belongs to
            int patchPixels = 64;  // PATCH_PIXELS
            int patchPX = (px - width / 2) / patchPixels;
            int patchPZ = (pz - height / 2) / patchPixels;
            if (px < width / 2) patchPX--;
            if (pz < height / 2) patchPZ--;
            touchedPatches.Insert(PatchKey(patchPX, patchPZ));
        }
    }

    if (modified == 0)
        return;

    // Broadcast affected patches to all connected clients
    for (auto pit = touchedPatches.Begin(); pit != touchedPatches.End(); ++pit)
    {
        int tpx = (int)(*pit >> 32);
        int tpz = (int)(*pit & 0xFFFFFFFF);
        BroadcastAffectedPatch(tpx, tpz, "water_heightmap", waterHeightMap_);
    }
}

void AuthServer::UpdateSceneView(float timeStep)
{
    if (activeTab_ != 3)
        return;

    // Update scene stats
    if (sceneStatsText_ && scene_)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "Nodes: %d | Clients: %d | Brush: r=%.0f s=%.3f",
                 scene_->GetNumChildren(true), sessions_.Size(),
                 waterBrushRadius_, waterBrushStrength_);
        sceneStatsText_->SetText(buf);
    }

    auto* input = GetSubsystem<Input>();
    auto* ui = GetSubsystem<UI>();

    // Scroll wheel adjusts brush radius
    int wheel = input->GetMouseMoveWheel();
    if (wheel != 0)
    {
        waterBrushRadius_ = Clamp(waterBrushRadius_ + wheel * 2.0f, 2.0f, 64.0f);
    }

    // LMB = raise water, MMB = lower water (only when not over UI)
    bool lmb = input->GetMouseButtonDown(MOUSEB_LEFT);
    bool mmb = input->GetMouseButtonDown(MOUSEB_MIDDLE);
    bool rmb = input->GetMouseButtonDown(MOUSEB_RIGHT);

    if ((lmb || mmb) && !rmb && !ui->GetFocusElement())
    {
        // Raycast from camera through cursor
        auto* camera = godCamNode_ ? godCamNode_->GetComponent<Camera>() : nullptr;
        if (camera)
        {
            auto* graphics = GetSubsystem<Graphics>();
            IntVector2 mousePos = input->GetMousePosition();
            Ray ray = camera->GetScreenRay(
                (float)mousePos.x_ / graphics->GetWidth(),
                (float)mousePos.y_ / graphics->GetHeight());

            auto* physicsWorld = scene_->GetComponent<PhysicsWorld>();
            if (physicsWorld)
            {
                PhysicsRaycastResult result;
                physicsWorld->RaycastSingle(result, ray, 2000.0f);
                if (result.body_)
                {
                    PaintWater(result.position_.x_, result.position_.z_, lmb);
                }
            }
        }
    }
}

void AuthServer::HandleKeyDown(StringHash eventType, VariantMap& eventData)
{
    using namespace KeyDown;
    int key = eventData[P_KEY].GetI32();
    if (key == KEY_ESCAPE)
        engine_->Exit();
}

void AuthServer::HandleTabClicked(StringHash eventType, VariantMap& eventData)
{
    auto* element = static_cast<UIElement*>(eventData["Element"].GetPtr());
    if (element == networkingTab_)
        SwitchTab(0);
    else if (element == databaseTab_)
        SwitchTab(1);
    else if (element == weatherTab_)
        SwitchTab(2);
    else if (element == sceneViewTab_)
        SwitchTab(3);
}

void AuthServer::SwitchTab(int tab)
{
    activeTab_ = tab;
    networkingPanel_->SetVisible(tab == 0);
    databasePanel_->SetVisible(tab == 1);
    if (weatherPanel_)
        weatherPanel_->SetVisible(tab == 2);
    if (sceneViewPanel_)
        sceneViewPanel_->SetVisible(tab == 3);

    // Update tab button colors
    auto* netLabel = static_cast<Text*>(networkingTab_->GetChild("Label", false));
    auto* dbLabel = static_cast<Text*>(databaseTab_->GetChild("Label", false));
    auto* wxLabel = weatherTab_ ? static_cast<Text*>(weatherTab_->GetChild("Label", false)) : nullptr;
    auto* svLabel = sceneViewTab_ ? static_cast<Text*>(sceneViewTab_->GetChild("Label", false)) : nullptr;
    if (netLabel)
        netLabel->SetColor(tab == 0 ? Color(0.8f, 0.8f, 1.0f) : Color(0.5f, 0.5f, 0.5f));
    if (dbLabel)
        dbLabel->SetColor(tab == 1 ? Color(0.8f, 0.8f, 1.0f) : Color(0.5f, 0.5f, 0.5f));
    if (wxLabel)
        wxLabel->SetColor(tab == 2 ? Color(0.8f, 0.8f, 1.0f) : Color(0.5f, 0.5f, 0.5f));
    if (svLabel)
        svLabel->SetColor(tab == 3 ? Color(0.8f, 0.8f, 1.0f) : Color(0.5f, 0.5f, 0.5f));

    // Refresh weather panel when switching to it
    if (tab == 2)
        RefreshWeatherPanel();

    // Populate table list on first switch to Database
    if (tab == 1 && tableSelector_->GetNumItems() == 0 && db_)
        RefreshTableList();
}

void AuthServer::RefreshClientList()
{
    if (!clientList_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    clientList_->RemoveAllItems();

    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        auto* line = new Text(context_);
        line->SetFont(font, 11);

        String label = it->first_->ToString();
        if (!it->second_.username.Empty())
            label += "  [" + it->second_.username + "]";
        if (it->second_.authenticated)
            label += "  (auth)";

        line->SetText(label);
        line->SetColor(it->second_.authenticated ? Color(0.3f, 1.0f, 0.3f) : Color(0.8f, 0.8f, 0.8f));
        clientList_->AddItem(line);
    }

    clientCountText_->SetText("Clients: " + String(sessions_.Size()));
}

void AuthServer::LogMessage(const String& msg)
{
    URHO3D_LOGINFO(msg);

    if (!logList_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    // Timestamp
    Time* time = GetSubsystem<Time>();
    unsigned secs = time->GetTimeSinceEpoch();
    unsigned h = (secs / 3600) % 24;
    unsigned m = (secs / 60) % 60;
    unsigned s = secs % 60;
    char tsBuf[16];
    snprintf(tsBuf, sizeof(tsBuf), "[%02u:%02u:%02u] ", h, m, s);
    String timestamp(tsBuf);

    auto* line = new Text(context_);
    line->SetFont(font, 11);
    line->SetText(timestamp + msg);

    // Color by content
    if (msg.Contains("[ERROR]"))
        line->SetColor(Color(1.0f, 0.3f, 0.3f));
    else if (msg.Contains("connected") || msg.Contains("authenticated") || msg.Contains("ready"))
        line->SetColor(Color(0.3f, 1.0f, 0.3f));
    else if (msg.Contains("disconnected"))
        line->SetColor(Color(1.0f, 0.6f, 0.2f));
    else
        line->SetColor(Color(0.75f, 0.75f, 0.75f));

    logList_->AddItem(line);

    // Cap log length
    while (logList_->GetNumItems() > MAX_LOG_LINES)
        logList_->RemoveItem((i32)0);

    // Auto-scroll to bottom
    logList_->EnsureItemVisibility(logList_->GetNumItems() - 1);
}

// ============================================================
// Database Editor
// ============================================================

void AuthServer::RefreshTableList()
{
    if (!db_ || !tableSelector_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    tableSelector_->RemoveAllItems();

    DbResult tables = db_->Execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    for (unsigned i = 0; i < tables.GetNumRows(); ++i)
    {
        String name = tables.GetRows()[i][0].GetString();
        auto* item = new Text(context_);
        item->SetFont(font, 11);
        item->SetText(name);
        item->SetColor(Color(0.9f, 0.9f, 0.9f));
        tableSelector_->AddItem(item);
    }

    if (tableSelector_->GetNumItems() > 0)
    {
        tableSelector_->SetSelection(0);
        auto* selected = static_cast<Text*>(tableSelector_->GetSelectedItem());
        if (selected)
            LoadTableData(selected->GetText());
    }
}

void AuthServer::LoadTableData(const String& tableName)
{
    if (!db_ || tableName.Empty())
        return;

    currentTable_ = tableName;
    currentColumns_.Clear();
    primaryKeyIndices_.Clear();

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    // Get column info via PRAGMA
    DbResult schema = db_->Execute("PRAGMA table_info(" + tableName + ")");
    Vector<String> colTypes;
    for (unsigned i = 0; i < schema.GetNumRows(); ++i)
    {
        const VariantVector& row = schema.GetRows()[i];
        String colName = row[1].GetString();
        String colType = row[2].GetString();
        int pk = row[5].GetI32();

        currentColumns_.Push(colName);
        colTypes.Push(colType);
        if (pk > 0)
            primaryKeyIndices_.Push(i);
    }

    // Load data to compute column widths
    DbResult data = db_->Execute("SELECT * FROM " + tableName);
    unsigned numCols = currentColumns_.Size();

    // Compute per-column character widths (monospace: char count = alignment)
    Vector<unsigned> colChars(numCols, 0);
    for (unsigned j = 0; j < numCols; ++j)
    {
        unsigned headerLen = currentColumns_[j].Length() + colTypes[j].Length() + 3;
        bool isPK = false;
        for (unsigned k = 0; k < primaryKeyIndices_.Size(); ++k)
            if (primaryKeyIndices_[k] == (int)j) { isPK = true; break; }
        if (isPK) headerLen += 3;
        colChars[j] = headerLen;
    }
    for (unsigned i = 0; i < data.GetNumRows(); ++i)
    {
        const VariantVector& row = data.GetRows()[i];
        for (unsigned j = 0; j < row.Size() && j < numCols; ++j)
        {
            unsigned len = row[j].ToString().Length();
            if (len > 24) len = 24;  // cap display width
            if (len > colChars[j])
                colChars[j] = len;
        }
    }
    // Add 2 chars padding between columns
    for (unsigned j = 0; j < numCols; ++j)
        colChars[j] += 2;

    // Helper: pad string to fixed width (monospace)
    auto padTo = [](const String& s, unsigned width) -> String
    {
        if (s.Length() >= width)
            return s.Substring(0, width);
        String result = s;
        for (unsigned i = s.Length(); i < width; ++i)
            result += " ";
        return result;
    };

    // Build header line
    String headerLine;
    for (unsigned j = 0; j < numCols; ++j)
    {
        String label = currentColumns_[j] + " (" + colTypes[j] + ")";
        bool isPK = false;
        for (unsigned k = 0; k < primaryKeyIndices_.Size(); ++k)
            if (primaryKeyIndices_[k] == (int)j) { isPK = true; break; }
        if (isPK) label += " PK";
        headerLine += padTo(label, colChars[j]);
    }

    if (tableSchemaText_)
        tableSchemaText_->SetText(headerLine);

    // Load data rows
    if (!tableView_)
        return;

    tableView_->RemoveAllItems();

    for (unsigned i = 0; i < data.GetNumRows(); ++i)
    {
        const VariantVector& row = data.GetRows()[i];
        String line;
        for (unsigned j = 0; j < row.Size() && j < numCols; ++j)
        {
            String val = row[j].ToString();
            if (val.Length() > 24)
                val = val.Substring(0, 21) + "...";
            line += padTo(val, colChars[j]);
        }

        auto* item = new Text(context_);
        item->SetFont(font, 11);
        item->SetText(line);
        item->SetColor(i % 2 == 0 ? Color(0.85f, 0.85f, 0.85f) : Color(0.7f, 0.7f, 0.75f));
        tableView_->AddItem(item);
    }
}

void AuthServer::HandleTableSelected(StringHash eventType, VariantMap& eventData)
{
    if (!tableSelector_)
        return;

    auto* selected = static_cast<Text*>(tableSelector_->GetSelectedItem());
    if (selected)
        LoadTableData(selected->GetText());
}

void AuthServer::HandleSqlExecute(StringHash eventType, VariantMap& eventData)
{
    if (!db_ || !sqlInput_ || !sqlResultView_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    String sql = sqlInput_->GetText().Trimmed();
    if (sql.Empty())
        return;

    sqlResultView_->RemoveAllItems();

    DbResult result = db_->Execute(sql);

    // Show column headers if we have them
    const StringVector& cols = result.GetColumns();
    if (!cols.Empty())
    {
        String headerLine;
        for (unsigned i = 0; i < cols.Size(); ++i)
        {
            if (i > 0)
                headerLine += " | ";
            headerLine += cols[i];
        }
        auto* header = new Text(context_);
        header->SetFont(font, 11);
        header->SetText(headerLine);
        header->SetColor(Color(0.5f, 0.7f, 1.0f));
        sqlResultView_->AddItem(header);
    }

    // Show rows
    for (unsigned i = 0; i < result.GetNumRows(); ++i)
    {
        const VariantVector& row = result.GetRows()[i];
        String line;
        for (unsigned j = 0; j < row.Size(); ++j)
        {
            if (j > 0)
                line += " | ";
            line += row[j].ToString();
        }
        auto* item = new Text(context_);
        item->SetFont(font, 11);
        item->SetText(line);
        item->SetColor(Color(0.85f, 0.85f, 0.85f));
        sqlResultView_->AddItem(item);
    }

    // Show affected rows for non-SELECT queries
    if (result.GetRows().Empty() && cols.Empty())
    {
        long affected = result.GetNumAffectedRows();
        auto* info = new Text(context_);
        info->SetFont(font, 11);
        info->SetText(affected >= 0 ? String(affected) + " row(s) affected" : "Query executed");
        info->SetColor(Color(0.3f, 1.0f, 0.3f));
        sqlResultView_->AddItem(info);
    }

    // Refresh table view if current table might have been modified
    if (!currentTable_.Empty())
        LoadTableData(currentTable_);
}

void AuthServer::HandleAddRow(StringHash eventType, VariantMap& eventData)
{
    if (!db_ || currentTable_.Empty())
        return;

    // Try INSERT with DEFAULT VALUES first
    DbResult result = db_->Execute("INSERT INTO " + currentTable_ + " DEFAULT VALUES");
    if (result.GetNumAffectedRows() <= 0 && !currentColumns_.Empty())
    {
        // Fallback: insert with empty strings for each non-PK column
        String cols, vals;
        bool first = true;
        for (unsigned i = 0; i < currentColumns_.Size(); ++i)
        {
            // Skip autoincrement PKs
            bool isPK = false;
            for (unsigned k = 0; k < primaryKeyIndices_.Size(); ++k)
            {
                if (primaryKeyIndices_[k] == (int)i)
                {
                    isPK = true;
                    break;
                }
            }
            if (isPK)
                continue;

            if (!first)
            {
                cols += ", ";
                vals += ", ";
            }
            cols += currentColumns_[i];
            vals += "''";
            first = false;
        }
        if (!cols.Empty())
            db_->Execute("INSERT INTO " + currentTable_ + " (" + cols + ") VALUES (" + vals + ")");
    }

    LoadTableData(currentTable_);
    LogMessage("Added row to " + currentTable_);
}

void AuthServer::HandleDeleteRow(StringHash eventType, VariantMap& eventData)
{
    if (!db_ || currentTable_.Empty() || !tableView_ || primaryKeyIndices_.Empty())
        return;

    unsigned sel = tableView_->GetSelection();
    if (sel == M_MAX_UNSIGNED)
        return;

    // Re-query to get the actual data for the selected row
    DbResult data = db_->Execute("SELECT * FROM " + currentTable_);
    if (sel >= data.GetNumRows())
        return;

    const VariantVector& row = data.GetRows()[sel];

    // Build WHERE clause from PKs
    String where;
    for (unsigned i = 0; i < primaryKeyIndices_.Size(); ++i)
    {
        int pkIdx = primaryKeyIndices_[i];
        if (pkIdx >= (int)row.Size())
            continue;

        if (i > 0)
            where += " AND ";
        where += currentColumns_[pkIdx] + " = '" + row[pkIdx].ToString() + "'";
    }

    if (!where.Empty())
    {
        db_->Execute("DELETE FROM " + currentTable_ + " WHERE " + where);
        LoadTableData(currentTable_);
        LogMessage("Deleted row from " + currentTable_);
    }
}

void AuthServer::HandleEditRow(StringHash eventType, VariantMap& eventData)
{
    if (!db_ || currentTable_.Empty() || !tableView_)
        return;

    unsigned sel = tableView_->GetSelection();
    if (sel == M_MAX_UNSIGNED)
        return;

    CreateEditDialog(sel);
}

void AuthServer::CreateEditDialog(unsigned rowIndex)
{
    if (!db_ || currentTable_.Empty())
        return;

    // Get current row data
    DbResult data = db_->Execute("SELECT * FROM " + currentTable_);
    if (rowIndex >= data.GetNumRows())
        return;

    const VariantVector& row = data.GetRows()[rowIndex];
    editRowIndex_ = rowIndex;

    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
    auto* ui = GetSubsystem<UI>();
    auto* root = ui->GetRoot();

    // Remove existing dialog
    if (editDialog_)
    {
        editDialog_->Remove();
        editDialog_.Reset();
    }
    editFields_.Clear();

    // Create modal window
    editDialog_ = new Window(context_);
    root->AddChild(editDialog_);
    editDialog_->SetStyle("Window");
    editDialog_->SetColor(Color(0.15f, 0.15f, 0.2f));
    editDialog_->SetLayout(LM_VERTICAL, 6, IntRect(10, 10, 10, 10));
    editDialog_->SetFixedWidth(400);
    editDialog_->SetPosition(160, 80);
    editDialog_->SetMovable(true);
    editDialog_->SetBringToFront(true);
    editDialog_->SetModal(true);

    auto* titleText = editDialog_->CreateChild<Text>("EditTitle");
    titleText->SetFont(font, 13);
    titleText->SetText("Edit Row — " + currentTable_);
    titleText->SetColor(Color(0.8f, 0.8f, 1.0f));

    // One field per column
    for (unsigned i = 0; i < currentColumns_.Size() && i < row.Size(); ++i)
    {
        auto* fieldRow = editDialog_->CreateChild<BorderImage>("FieldRow_" + String(i));
        fieldRow->SetColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
        fieldRow->SetLayout(LM_HORIZONTAL, 4, IntRect(0, 0, 0, 0));
        fieldRow->SetFixedHeight(26);

        auto* label = fieldRow->CreateChild<Text>("Label");
        label->SetFont(font, 11);
        label->SetText(currentColumns_[i]);
        label->SetColor(Color(0.6f, 0.6f, 0.7f));
        label->SetFixedWidth(120);

        auto* field = fieldRow->CreateChild<LineEdit>("Field_" + String(i));
        field->SetStyle("LineEdit");
        field->SetFixedHeight(22);
        field->SetMinWidth(240);
        field->SetText(row[i].ToString());

        editFields_.Push(field);
    }

    // Button row
    auto* btnRow = editDialog_->CreateChild<BorderImage>("BtnRow");
    btnRow->SetColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
    btnRow->SetLayout(LM_HORIZONTAL, 8, IntRect(0, 4, 0, 0));
    btnRow->SetFixedHeight(30);

    auto* okBtn = btnRow->CreateChild<Button>("OkBtn");
    okBtn->SetStyle("Button");
    okBtn->SetFixedSize(60, 24);
    auto* okLabel = okBtn->CreateChild<Text>("Label");
    okLabel->SetFont(font, 11);
    okLabel->SetText("Save");
    okLabel->SetAlignment(HA_CENTER, VA_CENTER);

    auto* cancelBtn = btnRow->CreateChild<Button>("CancelBtn");
    cancelBtn->SetStyle("Button");
    cancelBtn->SetFixedSize(60, 24);
    auto* cancelLabel = cancelBtn->CreateChild<Text>("Label");
    cancelLabel->SetFont(font, 11);
    cancelLabel->SetText("Cancel");
    cancelLabel->SetAlignment(HA_CENTER, VA_CENTER);

    SubscribeToEvent(okBtn, "Released", URHO3D_HANDLER(AuthServer, HandleEditOK));
    SubscribeToEvent(cancelBtn, "Released", URHO3D_HANDLER(AuthServer, HandleEditCancel));
}

void AuthServer::HandleEditOK(StringHash eventType, VariantMap& eventData)
{
    if (!db_ || currentTable_.Empty() || !editDialog_ || primaryKeyIndices_.Empty())
        return;

    // Re-query to get the original PK values
    DbResult data = db_->Execute("SELECT * FROM " + currentTable_);
    if (editRowIndex_ >= data.GetNumRows())
        return;

    const VariantVector& origRow = data.GetRows()[editRowIndex_];

    // Build SET clause
    String setClause;
    for (unsigned i = 0; i < editFields_.Size() && i < currentColumns_.Size(); ++i)
    {
        if (i > 0)
            setClause += ", ";
        String val = editFields_[i]->GetText();
        // Escape single quotes
        val.Replace("'", "''");
        setClause += currentColumns_[i] + " = '" + val + "'";
    }

    // Build WHERE from PKs using original values
    String where;
    for (unsigned i = 0; i < primaryKeyIndices_.Size(); ++i)
    {
        int pkIdx = primaryKeyIndices_[i];
        if (pkIdx >= (int)origRow.Size())
            continue;
        if (i > 0)
            where += " AND ";
        String val = origRow[pkIdx].ToString();
        val.Replace("'", "''");
        where += currentColumns_[pkIdx] + " = '" + val + "'";
    }

    if (!setClause.Empty() && !where.Empty())
    {
        db_->Execute("UPDATE " + currentTable_ + " SET " + setClause + " WHERE " + where);
        LoadTableData(currentTable_);
        LogMessage("Updated row in " + currentTable_);
    }

    editDialog_->Remove();
    editDialog_.Reset();
    editFields_.Clear();
}

void AuthServer::HandleEditCancel(StringHash eventType, VariantMap& eventData)
{
    if (editDialog_)
    {
        editDialog_->Remove();
        editDialog_.Reset();
    }
    editFields_.Clear();
}

// ============================================================
// Database
// ============================================================

void AuthServer::InitDatabase()
{
    // Ensure libsodium is initialized before any hashing
    if (sodium_init() < 0)
    {
        LogMessage("[ERROR] sodium_init() failed — password hashing unavailable");
        return;
    }

    auto* database = GetSubsystem<Database>();
    if (!database)
    {
        LogMessage("[ERROR] Database subsystem not available");
        return;
    }

    db_ = database->Connect("authserver.db");
    if (!db_ || !db_->IsConnected())
    {
        LogMessage("[ERROR] Failed to open authserver.db");
        return;
    }

    db_->Execute(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  pake_hash TEXT NOT NULL DEFAULT '',"
        "  admin_level INTEGER NOT NULL DEFAULT 0,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")"
    );

    // Add pake_hash column if it doesn't exist (migration for existing DBs)
    db_->Execute("ALTER TABLE users ADD COLUMN pake_hash TEXT NOT NULL DEFAULT ''");

    db_->Execute(
        "CREATE TABLE IF NOT EXISTS patches ("
        "  patch_x INTEGER NOT NULL,"
        "  patch_z INTEGER NOT NULL,"
        "  owner_name TEXT NOT NULL,"
        "  claimed_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (patch_x, patch_z),"
        "  FOREIGN KEY (owner_name) REFERENCES users(username)"
        ")"
    );

    // Seed default superuser if DB is fresh (no users yet)
    {
        DbResult check = db_->Execute("SELECT COUNT(*) FROM users");
        if (!check.GetRows().Empty() && check.GetRows()[0][0].GetI32() == 0)
        {
            String argonHex = HashPasswordArgon2("admin");
            String blakeHex = HashPasswordBlake2("admin");
            LogMessage("Admin seed: argon2 len=" + String(argonHex.Length()) + ", blake2 len=" + String(blakeHex.Length()));
            if (argonHex.Empty())
                LogMessage("[ERROR] Argon2id hashing failed for admin seed!");
            if (blakeHex.Empty())
                LogMessage("[ERROR] BLAKE2b hashing failed for admin seed!");
            db_->Execute(
                "INSERT INTO users (username, password_hash, pake_hash, admin_level) "
                "VALUES ('admin', '" + argonHex + "', '" + blakeHex + "', 25773)"
            );
            LogMessage("Created default superuser 'admin' (level 25773) — password hashed");
        }
    }

    // Migration: hash any existing plaintext passwords (pake_hash is empty)
    {
        DbResult unhashed = db_->Execute(
            "SELECT username, password_hash FROM users WHERE pake_hash = ''"
        );
        for (unsigned i = 0; i < unhashed.GetNumRows(); ++i)
        {
            String user = unhashed.GetRows()[i][0].GetString();
            String plaintext = unhashed.GetRows()[i][1].GetString();
            String argonHex = HashPasswordArgon2(plaintext);
            String blakeHex = HashPasswordBlake2(plaintext);
            if (!argonHex.Empty())
            {
                db_->Execute(
                    "UPDATE users SET password_hash = '" + argonHex +
                    "', pake_hash = '" + blakeHex +
                    "' WHERE username = '" + user + "'"
                );
                LogMessage("Migrated password hash for user '" + user + "'");
            }
            else
                LogMessage("[ERROR] Argon2id hash failed for user '" + user + "'");
        }
    }

    // Migration: allocate random patches to users who don't have one
    {
        DbResult unpatched = db_->Execute(
            "SELECT username FROM users WHERE username NOT IN (SELECT owner_name FROM patches)"
        );
        for (unsigned i = 0; i < unpatched.GetNumRows(); ++i)
        {
            String user = unpatched.GetRows()[i][0].GetString();
            int px, pz;
            if (AllocateRandomPatch(user, px, pz))
                LogMessage("Allocated patch (" + String(px) + "," + String(pz) + ") to " + user);
            else
                LogMessage("[WARN] No free patches for user " + user);
        }
    }

    // Report existing data
    DbResult userCount = db_->Execute("SELECT COUNT(*) FROM users");
    DbResult patchCount = db_->Execute("SELECT COUNT(*) FROM patches");
    String users = !userCount.GetRows().Empty() ? String(userCount.GetRows()[0][0].GetI32()) : "0";
    String patches = !patchCount.GetRows().Empty() ? String(patchCount.GetRows()[0][0].GetI32()) : "0";

    LogMessage("Database: " + users + " users, " + patches + " patches");
}

// ============================================================
// Scene loading
// ============================================================

void AuthServer::LoadScene()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* sceneFile = cache->GetResource<XMLFile>(sceneName_, false);

    if (sceneFile)
    {
        // Load existing scene directly — no wrapper node
        scene_ = new Scene(context_);
        // Use File-based LoadXML (not XMLElement) so scene->GetFileName() is set.
        // Connection::SetScene() sends GetFileName() in MSG_LOADSCENE to clients.
        SharedPtr<File> sceneFileStream = cache->GetFile(sceneName_);
        if (sceneFileStream && scene_->LoadXML(*sceneFileStream))
            LogMessage("Loaded scene: " + sceneName_);
        else
        {
            LogMessage("[ERROR] Failed to load scene: " + sceneName_);
            scene_.Reset();
        }
        return;
    }

    // --- Bootstrap: generate a new scene from scratch ---
    LogMessage("[BOOTSTRAP] Scene '" + sceneName_ + "' not found — generating new world");

    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>(LOCAL);
    scene_->CreateComponent<PhysicsWorld>(LOCAL);

    // Zone — fog and ambient
    Node* zoneNode = scene_->CreateChild("Zone", LOCAL);
    auto* zone = zoneNode->CreateComponent<Zone>(LOCAL);
    zone->SetBoundingBox(BoundingBox(-2000.0f, 2000.0f));
    zone->SetAmbientColor(Color(0.15f, 0.15f, 0.15f));
    zone->SetFogColor(Color(0.5f, 0.6f, 0.7f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);

    // Directional light (sun)
    Node* lightNode = scene_->CreateChild("DirectionalLight", LOCAL);
    lightNode->SetDirection(Vector3(0.6f, -1.0f, 0.8f));
    auto* light = lightNode->CreateComponent<Light>(LOCAL);
    light->SetLightType(LIGHT_DIRECTIONAL);
    light->SetColor(Color(1.0f, 0.95f, 0.8f));
    light->SetBrightness(1.0f);
    light->SetCastShadows(true);
    light->SetShadowBias(BiasParameters(0.00025f, 0.5f));
    light->SetShadowCascade(CascadeParameters(20.0f, 60.0f, 180.0f, 560.0f, 0.1f));

    // Moon light
    Node* moonNode = scene_->CreateChild("MoonLight", LOCAL);
    moonNode->SetDirection(Vector3(-0.6f, -1.0f, -0.8f));
    auto* moonLight = moonNode->CreateComponent<Light>(LOCAL);
    moonLight->SetLightType(LIGHT_DIRECTIONAL);
    moonLight->SetColor(Color(0.3f, 0.35f, 0.5f));
    moonLight->SetBrightness(0.0f);  // off by default, night cycle turns it on

    // Skybox
    Node* skyNode = scene_->CreateChild("Sky", LOCAL);
    auto* skybox = skyNode->CreateComponent<Skybox>(LOCAL);
    skybox->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    skybox->SetMaterial(cache->GetResource<Material>("Materials/Skybox.xml"));

    // Generate terrain heightmap
    SharedPtr<Image> heightMap = terrainGen_.GenerateWithEdges(context_, nullptr, nullptr, nullptr, nullptr);
    if (!heightMap)
    {
        LogMessage("[ERROR] Failed to generate bootstrap heightmap");
        return;
    }

    // Save heightmap to Data/Textures/ so both server and client can find it
    String hmResourcePath = "Textures/TerrainHeightMap.png";
    String hmFullPath = cache->GetResourceDirs()[0] + hmResourcePath;
    heightMap->SavePNG(hmFullPath);
    LogMessage("[BOOTSTRAP] Saved heightmap: " + hmFullPath);

    // Register in ResourceCache so Terrain::GetHeightMapAttr() can serialize the reference
    heightMap->SetName(hmResourcePath);
    cache->AddManualResource(heightMap);

    // Terrain node
    Node* terrainNode = scene_->CreateChild("Terrain", LOCAL);
    auto* terrain = terrainNode->CreateComponent<Terrain>(LOCAL);
    terrain->SetSpacing(Vector3(2.0f, 0.5f, 2.0f));
    terrain->SetHeightMap(heightMap);
    auto* terrainMat = cache->GetResource<Material>("Materials/Terrain.xml", false);
    if (terrainMat)
        terrain->SetMaterial(terrainMat);
    terrain->SetCastShadows(true);

    // Terrain collision — static body so avatars don't fall through
    auto* terrainBody = terrainNode->CreateComponent<RigidBody>(LOCAL);
    terrainBody->SetCollisionLayer(2);
    auto* terrainShape = terrainNode->CreateComponent<CollisionShape>(LOCAL);
    terrainShape->SetTerrain();

    // Load or create water heightmap (same resolution as terrain)
    LoadOrCreateWaterMap();

    // Water plane (flat quad at y=5.0)
    Node* waterNode = scene_->CreateChild("Water", LOCAL);
    waterNode->SetPosition(Vector3(0.0f, 5.0f, 0.0f));
    waterNode->SetScale(Vector3(2048.0f, 1.0f, 2048.0f));
    auto* waterModel = waterNode->CreateComponent<StaticModel>(LOCAL);
    waterModel->SetModel(cache->GetResource<Model>("Models/Plane.mdl"));
    auto* waterMat = cache->GetResource<Material>("Materials/Water.xml", false);
    if (waterMat)
        waterModel->SetMaterial(waterMat);

    // Save the bootstrapped scene to disk so it persists
    auto* fs = GetSubsystem<FileSystem>();
    String scenesDir = cache->GetResourceDirs()[0] + "Scenes";
    if (!fs->DirExists(scenesDir))
        fs->CreateDir(scenesDir);

    String sceneFullPath = cache->GetResourceDirs()[0] + sceneName_;
    File saveFile(context_, sceneFullPath, FILE_WRITE);
    if (scene_->SaveXML(saveFile))
        LogMessage("[BOOTSTRAP] Saved scene: " + sceneFullPath);
    else
        LogMessage("[ERROR] Failed to save bootstrap scene");
}

void AuthServer::InitTerrainBrush()
{
    if (!scene_)
        return;

    auto* terrain = scene_->GetComponent<Terrain>(true);
    if (!terrain)
    {
        LogMessage("[WARN] No terrain in scene — server brush not available");
        return;
    }

    auto* heightMap = terrain->GetHeightMap();
    if (!heightMap)
    {
        LogMessage("[WARN] Terrain has no heightmap — server brush not available");
        return;
    }

    terrainBrush_ = new TerrainBrush(context_);
    terrainBrush_->SetTerrain(terrain, heightMap);
    if (waterHeightMap_)
        terrainBrush_->SetWaterMap(waterHeightMap_);

    LogMessage("Server terrain brush initialized (" +
               String(heightMap->GetWidth()) + "x" + String(heightMap->GetHeight()) + ")");
}

void AuthServer::SetupGodCamera()
{
    if (!scene_)
        return;

    godCamNode_ = scene_->CreateChild("GodCamera", LOCAL);
    auto* camera = godCamNode_->CreateComponent<Camera>(LOCAL);
    camera->SetFarClip(2000.0f);

    // Start high above terrain center looking down at an angle
    godCamNode_->SetPosition(Vector3(0.0f, 80.0f, -50.0f));
    godCamNode_->SetRotation(Quaternion(30.0f, 0.0f, 0.0f));  // pitch down
    yaw_ = 0.0f;
    pitch_ = 30.0f;

    // Set up viewport
    auto* renderer = GetSubsystem<Renderer>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, camera));
    renderer->SetViewport(0, viewport);

    LogMessage("God camera active — WASD move, mouse look, RMB to fly");
}

void AuthServer::UpdateGodCamera(float timeStep)
{
    if (!godCamNode_)
        return;

    auto* input = GetSubsystem<Input>();
    auto* ui = GetSubsystem<UI>();

    // Only capture mouse when RMB is held (so UI stays usable)
    bool flying = input->GetMouseButtonDown(MOUSEB_RIGHT);

    if (flying)
    {
        IntVector2 mouseMove = input->GetMouseMove();
        const float sensitivity = 0.1f;
        yaw_ += mouseMove.x_ * sensitivity;
        pitch_ += mouseMove.y_ * sensitivity;
        pitch_ = Clamp(pitch_, -89.0f, 89.0f);
        godCamNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
    }

    // WASD + QE movement (only when RMB held or no UI element focused)
    if (flying || !ui->GetFocusElement())
    {
        float speed = 50.0f * timeStep;
        if (input->GetKeyDown(KEY_SHIFT))
            speed *= 3.0f;

        if (input->GetKeyDown(KEY_W))
            godCamNode_->Translate(Vector3::FORWARD * speed);
        if (input->GetKeyDown(KEY_S))
            godCamNode_->Translate(Vector3::BACK * speed);
        if (input->GetKeyDown(KEY_A))
            godCamNode_->Translate(Vector3::LEFT * speed);
        if (input->GetKeyDown(KEY_D))
            godCamNode_->Translate(Vector3::RIGHT * speed);
        if (input->GetKeyDown(KEY_Q))
            godCamNode_->Translate(Vector3::DOWN * speed);
        if (input->GetKeyDown(KEY_E))
            godCamNode_->Translate(Vector3::UP * speed);
    }
}

Node* AuthServer::CreatePlayerAvatar(int patchX, int patchZ)
{
    if (!scene_)
        return nullptr;

    auto* cache = GetSubsystem<ResourceCache>();

    const float patchWorldSize = 128.0f;
    float spawnX = (patchX + 0.5f) * patchWorldSize;
    float spawnZ = (patchZ + 0.5f) * patchWorldSize;
    Vector3 spawnPos(spawnX, 20.0f, spawnZ);

    // Try to get terrain height at spawn position — clamp above water level
    const float waterLevel = 5.0f;
    auto* terrain = scene_->GetComponent<Terrain>(true);
    if (terrain)
    {
        float terrH = terrain->GetHeight(spawnPos);
        spawnPos.y_ = Max(terrH, waterLevel) + 2.0f;
        LogMessage("Avatar spawn: terrainH=" + String(terrH) + " spawnY=" + String(spawnPos.y_));
    }

    Node* charNode = scene_->CreateChild("Player");
    charNode->SetPosition(spawnPos);

    // Visual model
    Node* modelNode = charNode->CreateChild("PlayerModel");
    modelNode->SetPosition(Vector3(0.0f, 0.9f, 0.0f));
    auto* model = modelNode->CreateComponent<StaticModel>();
    model->SetModel(cache->GetResource<Model>("Models/Capsule.mdl"));
    model->SetMaterial(cache->GetResource<Material>("Materials/Stone.xml"));
    model->SetCastShadows(true);

    // Physics
    auto* body = charNode->CreateComponent<RigidBody>();
    body->SetMass(1.0f);
    body->SetFriction(1.0f);
    body->SetLinearDamping(0.5f);
    body->SetAngularDamping(0.5f);
    body->SetAngularFactor(Vector3::ZERO);
    body->SetCollisionLayer(1);
    body->SetCollisionEventMode(COLLISION_ALWAYS);

    auto* shape = charNode->CreateComponent<CollisionShape>();
    shape->SetCapsule(0.7f, 1.8f, Vector3(0.0f, 0.9f, 0.0f));

    // Character controller
    charNode->CreateComponent<PlayerCharacter>();

    LogMessage("Created avatar at patch (" + String(patchX) + "," + String(patchZ) +
               ") pos (" + String(spawnPos.x_) + "," + String(spawnPos.y_) + "," + String(spawnPos.z_) + ")");

    return charNode;
}

void AuthServer::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    // Apply each connected client's controls to their server-side avatar
    for (auto it = serverObjects_.Begin(); it != serverObjects_.End(); ++it)
    {
        Connection* connection = it->first_;
        Node* avatarNode = it->second_;
        if (!avatarNode || !connection)
            continue;

        auto* character = avatarNode->GetComponent<PlayerCharacter>();
        if (!character)
            continue;

        const Controls& controls = connection->GetControls();
        character->controls_ = controls;
        avatarNode->SetRotation(Quaternion(0.0f, controls.yaw_, 0.0f));
    }
}

// ============================================================
// Network events
// ============================================================

void AuthServer::HandleClientConnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientConnected;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());

    sessions_[connection] = ClientSession{};
    LogMessage("Client connected: " + connection->ToString());
    RefreshClientList();
}

void AuthServer::HandleClientDisconnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientDisconnected;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());

    // Remove avatar from scene
    auto objIt = serverObjects_.Find(connection);
    if (objIt != serverObjects_.End())
    {
        Node* avatar = objIt->second_;
        if (avatar)
        {
            LogMessage("Removing avatar node " + String(avatar->GetID()));
            avatar->Remove();
        }
        serverObjects_.Erase(objIt);
    }

    auto it = sessions_.Find(connection);
    if (it != sessions_.End())
    {
        String who = it->second_.username.Empty() ? "<unauthenticated>" : it->second_.username;
        LogMessage("Client disconnected: " + connection->ToString() + " (" + who + ")");
        sessions_.Erase(it);
    }
    RefreshClientList();
}

void AuthServer::HandleClientIdentity(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientIdentity;
    eventData[P_ALLOW] = true;
}

void AuthServer::HandleNetworkMessage(StringHash eventType, VariantMap& eventData)
{
    using namespace NetworkMessage;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    int msgID = eventData[P_MESSAGEID].GetI32();
    const auto& data = eventData[P_DATA].GetBuffer();

    MemoryBuffer msg(data);

    switch (msgID)
    {
    case MSG_AUTH_LOGIN:
    {
        String username = msg.ReadString();
        String passwordHash = msg.ReadString();
        int adminLevel = 0;
        bool ok = AuthenticateUser(username, passwordHash, adminLevel);

        // Reject if this username is already logged in on another connection (admins exempt)
        String failReason = "Invalid credentials";
        if (ok && adminLevel == 0)
        {
            Connection* existing = FindSessionByUsername(username);
            if (existing && existing != connection)
            {
                ok = false;
                failReason = "Already logged in";
                LogMessage("Login rejected for '" + username + "': already logged in");
            }
        }

        VectorBuffer reply;
        reply.WriteI32(MSG_AUTH_LOGIN);  // echo original msg type so client knows which op
        reply.WriteBool(ok);
        reply.WriteString(ok ? "Login successful" : failReason);
        reply.WriteI32(adminLevel);  // admin level — 0 = regular user, >0 = admin privileges
        reply.WriteString(ok ? sceneName_ : String::EMPTY);  // scene name (empty on failure)
        // Append ALL owned patch coordinates
        if (ok && db_)
        {
            DbResult patchResult = db_->Execute(
                "SELECT patch_x, patch_z FROM patches WHERE owner_name = '" + username + "'"
            );
            const auto& rows = patchResult.GetRows();
            reply.WriteI32((int)rows.Size());
            for (unsigned i = 0; i < rows.Size(); ++i)
            {
                reply.WriteI32(rows[i][0].GetI32());
                reply.WriteI32(rows[i][1].GetI32());
            }
        }
        else
        {
            reply.WriteI32(0);  // no patches
        }
        connection->SendMessage(MSG_AUTH_RESULT, true, true, reply);

        if (ok)
        {
            sessions_[connection].username = username;
            sessions_[connection].authenticated = true;
            sessions_[connection].adminLevel = adminLevel;
            LogMessage("User '" + username + "' authenticated (level " + String(adminLevel) + "), scene: " + sceneName_);
            RefreshClientList();
        }
        else
            LogMessage("Login failed for '" + username + "'");
        break;
    }

    case MSG_AUTH_REGISTER:
    {
        String username = msg.ReadString();
        String passwordHash = msg.ReadString();
        bool ok = RegisterUser(username, passwordHash);

        VectorBuffer reply;
        reply.WriteI32(MSG_AUTH_REGISTER);  // echo original msg type
        reply.WriteBool(ok);
        reply.WriteString(ok ? "Registration successful" : "Username already taken");
        connection->SendMessage(MSG_AUTH_RESULT, true, true, reply);

        LogMessage(ok ? "Registered user '" + username + "'" : "Registration failed: '" + username + "' taken");
        break;
    }

    case MSG_REGISTER_GUID:
    {
        String guid = msg.ReadString();
        auto it = sessions_.Find(connection);
        if (it != sessions_.End() && it->second_.authenticated)
        {
            it->second_.guid = guid;
            LogMessage("GUID registered for '" + it->second_.username + "': " + guid);
            RefreshClientList();
        }
        else
            LogMessage("[WARN] GUID registration from unauthenticated client — ignoring");
        break;
    }

    case MSG_PATCH_CLAIM:
    {
        auto it = sessions_.Find(connection);
        if (it == sessions_.End() || !it->second_.authenticated)
        {
            VectorBuffer reply;
            reply.WriteBool(false);
            reply.WriteString("Not authenticated");
            connection->SendMessage(MSG_PATCH_RESULT, true, true, reply);
            break;
        }

        int px = msg.ReadI32();
        int pz = msg.ReadI32();
        bool ok = ClaimPatch(px, pz, it->second_.username);

        VectorBuffer reply;
        reply.WriteBool(ok);
        reply.WriteString(ok ? "Patch claimed" : "Patch already owned");
        connection->SendMessage(MSG_PATCH_RESULT, true, true, reply);

        LogMessage(ok
            ? it->second_.username + " claimed patch (" + String(px) + "," + String(pz) + ")"
            : "Patch claim denied (" + String(px) + "," + String(pz) + ")");
        break;
    }

    case MSG_PATCH_QUERY:
    {
        int px = msg.ReadI32();
        int pz = msg.ReadI32();
        PatchInfo info = QueryPatchOwner(px, pz);

        // Send normal patch result regardless
        VectorBuffer reply;
        reply.WriteI32(px);
        reply.WriteI32(pz);
        reply.WriteString(info.ownerName);
        reply.WriteString(info.ownerAddress);
        reply.WriteU16(info.ownerPort);
        connection->SendMessage(MSG_PATCH_RESULT, true, true, reply);

        // If the patch is owned by another online, authenticated client, introduce peers
        if (!info.ownerName.Empty())
        {
            auto reqIt = sessions_.Find(connection);
            if (reqIt != sessions_.End() && reqIt->second_.authenticated)
            {
                // Don't introduce to yourself
                if (info.ownerName != reqIt->second_.username)
                {
                    Connection* owner = FindSessionByUsername(info.ownerName);
                    if (owner)
                        IntroducePeers(connection, owner, px, pz);
                }
            }
        }
        break;
    }

    case MSG_RELAY_TO_AUTH:
    {
        HandleRelayToAuth(connection, msg);
        break;
    }

    case MSG_EDIT_TERRAIN:
    {
        HandleEditTerrain(connection, msg);
        break;
    }

    case MSG_EDIT_OBJECT:
    {
        HandleEditObject(connection, msg);
        break;
    }

    case MSG_WATER_EDIT:
    {
        HandleWaterEdit(connection, msg);
        break;
    }

    case MSG_PATCH_POSITION:
    {
        HandlePatchPosition(connection, msg);
        break;
    }

    case MSG_TERRAIN_SYNC:
    {
        HandleTerrainSync(connection, msg);
        break;
    }

    default:
        LogMessage("Unknown msg ID " + String(msgID) + " from " + connection->ToString());
        break;
    }
}

void AuthServer::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float dt = eventData[P_TIMESTEP].GetFloat();
    uptime_ += dt;

    // God camera movement
    if (godCamActive_)
        UpdateGodCamera(dt);

    // Scene view water brush
    UpdateSceneView(dt);

    // Melbourne clock
    if (melbourneClock_)
        melbourneClock_->Update();

    // BOM weather fetch (every 10 minutes, first fetch at startup)
    weatherFetchTimer_ -= dt;
    if (weatherFetchTimer_ <= 0.0f)
    {
        weatherFetchTimer_ = WEATHER_FETCH_INTERVAL;
        FetchBOMWeather();
    }

    // Check for completed HTTP responses
    ProcessBOMResponse();

    // Broadcast weather to all clients periodically
    if (weatherReady_)
    {
        weatherBroadcastTimer_ -= dt;
        if (weatherBroadcastTimer_ <= 0.0f)
        {
            weatherBroadcastTimer_ = WEATHER_BROADCAST_INTERVAL;
            BroadcastWeather();
        }
    }

    // Trim terrain edit journals periodically
    journalTrimTimer_ -= dt;
    if (journalTrimTimer_ <= 0.0f)
    {
        journalTrimTimer_ = JOURNAL_TRIM_INTERVAL;
        journalManager_.TrimAll();
    }
}

void AuthServer::HandleKeyExchangeAuth(StringHash eventType, VariantMap& eventData)
{
    using namespace KeyExchangeAuth;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    String username = eventData[P_USERNAME].GetString();

    if (!db_)
    {
        eventData[P_FOUND] = false;
        return;
    }

    // Look up stored PAKE hash for this user
    DbResult result = db_->Execute(
        "SELECT pake_hash FROM users WHERE username = '" + username + "'"
    );

    if (result.GetRows().Empty())
    {
        eventData[P_FOUND] = false;
        LogMessage("PAKE: unknown user '" + username + "' (random hash will be mixed)");
        return;
    }

    String storedHex = result.GetRows()[0][0].GetString();
    if (storedHex.Empty())
    {
        eventData[P_FOUND] = false;
        LogMessage("PAKE: user '" + username + "' has no PAKE hash (migration needed)");
        return;
    }

    // Hex-decode and XOR-decode to recover the raw 32-byte BLAKE2b hash
    Vector<unsigned char> hash = HexDecode(storedHex);
    XorObfuscate(hash.Buffer(), hash.Size());

    eventData[P_PASSWORDHASH].SetBuffer(reinterpret_cast<const void*>(hash.Buffer()), hash.Size());
    eventData[P_FOUND] = true;
    LogMessage("PAKE: password hash provided for user '" + username + "'");
}

void AuthServer::HandleClientAuthenticated(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientAuthenticated;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    String username = eventData[P_USERNAME].GetString();

    // Look up admin level
    int adminLevel = 0;
    if (db_)
    {
        DbResult result = db_->Execute(
            "SELECT admin_level FROM users WHERE username = '" + username + "'"
        );
        if (!result.GetRows().Empty())
            adminLevel = result.GetRows()[0][0].GetI32();
    }

    // Reject if this username is already logged in on another connection (admins exempt)
    Connection* existing = FindSessionByUsername(username);
    if (existing && existing != connection && adminLevel == 0)
    {
        LogMessage("PAKE: Login rejected for '" + username + "': already logged in");
        VectorBuffer reply;
        reply.WriteI32(MSG_AUTH_LOGIN);
        reply.WriteBool(false);
        reply.WriteString("Already logged in");
        reply.WriteI32(0);
        reply.WriteString(String::EMPTY);
        reply.WriteI32(0);  // 0 patches
        connection->SendMessage(MSG_AUTH_RESULT, true, true, reply);
        return;
    }

    // Update session
    sessions_[connection].username = username;
    sessions_[connection].authenticated = true;
    sessions_[connection].adminLevel = adminLevel;

    LogMessage("PAKE: User '" + username + "' authenticated (level " + String(adminLevel) + ")");
    RefreshClientList();

    // Send auth result to client
    VectorBuffer reply;
    reply.WriteI32(MSG_AUTH_LOGIN);  // echo MSG_AUTH_LOGIN so client handles it the same way
    reply.WriteBool(true);
    reply.WriteString("Login successful");
    reply.WriteI32(adminLevel);
    reply.WriteString(sceneName_);
    // Append ALL owned patch coordinates
    int homePatchX = 0, homePatchZ = 0;
    if (db_)
    {
        DbResult patchResult = db_->Execute(
            "SELECT patch_x, patch_z FROM patches WHERE owner_name = '" + username + "'"
        );
        const auto& rows = patchResult.GetRows();
        reply.WriteI32((int)rows.Size());
        for (unsigned i = 0; i < rows.Size(); ++i)
        {
            int px = rows[i][0].GetI32();
            int pz = rows[i][1].GetI32();
            reply.WriteI32(px);
            reply.WriteI32(pz);
            if (i == 0) { homePatchX = px; homePatchZ = pz; }
        }
    }
    else
    {
        reply.WriteI32(0);
    }
    connection->SendMessage(MSG_AUTH_RESULT, true, true, reply);

    // Replicate scene to client
    if (scene_)
        connection->SetScene(scene_);

    // Create server-authoritative avatar at the client's home patch
    Node* avatar = CreatePlayerAvatar(homePatchX, homePatchZ);
    if (avatar)
    {
        serverObjects_[connection] = avatar;

        // Tell client which node is their avatar
        VariantMap remoteEventData;
        remoteEventData[P_ID] = avatar->GetID();
        connection->SendRemoteEvent(E_CLIENTOBJECTID, true, remoteEventData);
        LogMessage("Assigned avatar node " + String(avatar->GetID()) + " to " + username);
    }

    // Send current weather to newly connected client
    if (weatherReady_)
        SendWeatherToClient(connection);

    // Per-patch resource streaming — send 3×3 neighbourhood around home patch
    sessions_[connection].lastPatchPos = IntVector2(homePatchX, homePatchZ);
    sessions_[connection].sentPatches.Clear();
    SendPatchNeighbourhood(connection, homePatchX, homePatchZ);
}

// ============================================================
// Auth / Patch logic
// ============================================================

bool AuthServer::AuthenticateUser(const String& username, const String& password, int& adminLevel)
{
    adminLevel = 0;
    if (!db_)
        return false;

    DbResult result = db_->Execute(
        "SELECT password_hash, admin_level FROM users WHERE username = '" + username + "'"
    );
    if (result.GetRows().Empty())
        return false;

    String storedHex = result.GetRows()[0][0].GetString();
    Vector<unsigned char> storedBytes = HexDecode(storedHex);

    // XOR-decode to recover the Argon2id hash string
    XorObfuscate(storedBytes.Buffer(), storedBytes.Size());

    // Verify password against the Argon2id hash
    if (crypto_pwhash_str_verify(
            reinterpret_cast<const char*>(storedBytes.Buffer()),
            password.CString(), password.Length()) != 0)
        return false;

    adminLevel = result.GetRows()[0][1].GetI32();
    return true;
}

bool AuthServer::RegisterUser(const String& username, const String& password)
{
    if (!db_)
        return false;

    DbResult check = db_->Execute(
        "SELECT id FROM users WHERE username = '" + username + "'"
    );
    if (!check.GetRows().Empty())
        return false;

    String argonHex = HashPasswordArgon2(password);
    String blakeHex = HashPasswordBlake2(password);
    if (argonHex.Empty())
    {
        LogMessage("[ERROR] Argon2id hash failed during registration for '" + username + "'");
        return false;
    }

    db_->Execute(
        "INSERT INTO users (username, password_hash, pake_hash) VALUES ('" +
        username + "', '" + argonHex + "', '" + blakeHex + "')"
    );

    // Auto-allocate a random patch to the new user
    int px, pz;
    if (AllocateRandomPatch(username, px, pz))
        LogMessage("Allocated patch (" + String(px) + "," + String(pz) + ") to new user " + username);
    else
        LogMessage("[WARN] No free patches for new user " + username);

    return true;
}

bool AuthServer::ClaimPatch(int patchX, int patchZ, const String& username)
{
    if (!db_)
        return false;

    DbResult check = db_->Execute(
        "SELECT owner_name FROM patches WHERE patch_x = " + String(patchX) +
        " AND patch_z = " + String(patchZ)
    );
    if (!check.GetRows().Empty())
        return false;

    db_->Execute(
        "INSERT INTO patches (patch_x, patch_z, owner_name) VALUES (" +
        String(patchX) + ", " + String(patchZ) + ", '" + username + "')"
    );
    return true;
}

bool AuthServer::AllocateRandomPatch(const String& username, int& outPatchX, int& outPatchZ)
{
    if (!db_)
        return false;

    // Collect already-claimed patches
    DbResult claimed = db_->Execute("SELECT patch_x, patch_z FROM patches");
    Vector<IntVector2> taken;
    for (unsigned i = 0; i < claimed.GetNumRows(); ++i)
    {
        int cx = claimed.GetRows()[i][0].GetI32();
        int cz = claimed.GetRows()[i][1].GetI32();
        taken.Push(IntVector2(cx, cz));
    }

    // Build list of free patches in valid range -8..7
    Vector<IntVector2> freePatch;
    for (int x = -8; x <= 7; ++x)
    {
        for (int z = -8; z <= 7; ++z)
        {
            IntVector2 candidate(x, z);
            bool isTaken = false;
            for (unsigned i = 0; i < taken.Size(); ++i)
            {
                if (taken[i] == candidate)
                {
                    isTaken = true;
                    break;
                }
            }
            if (!isTaken)
                freePatch.Push(candidate);
        }
    }

    if (freePatch.Empty())
        return false;

    // Pick one at random
    SetRandomSeed(Time::GetTimeSinceEpoch() ^ (unsigned)freePatch.Size());
    unsigned idx = Rand() % freePatch.Size();
    outPatchX = freePatch[idx].x_;
    outPatchZ = freePatch[idx].y_;

    // Insert into patches table
    db_->Execute(
        "INSERT INTO patches (patch_x, patch_z, owner_name) VALUES (" +
        String(outPatchX) + ", " + String(outPatchZ) + ", '" + username + "')"
    );
    return true;
}

AuthServer::PatchInfo AuthServer::QueryPatchOwner(int patchX, int patchZ)
{
    PatchInfo info{patchX, patchZ, String::EMPTY, String::EMPTY, 0};

    if (!db_)
        return info;

    DbResult result = db_->Execute(
        "SELECT owner_name FROM patches WHERE patch_x = " + String(patchX) +
        " AND patch_z = " + String(patchZ)
    );

    if (!result.GetRows().Empty())
    {
        info.ownerName = result.GetRows()[0][0].GetString();

        for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
        {
            if (it->second_.username == info.ownerName && it->second_.authenticated)
            {
                info.ownerAddress = it->first_->ToString();
                break;
            }
        }
    }

    return info;
}

// ============================================================
// Peer brokering
// ============================================================

Connection* AuthServer::FindSessionByUsername(const String& username)
{
    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        if (it->second_.username == username && it->second_.authenticated)
            return it->first_;
    }
    return nullptr;
}

Connection* AuthServer::FindSessionByGuid(const String& guid)
{
    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        if (it->second_.guid == guid)
            return it->first_;
    }
    return nullptr;
}

void AuthServer::IntroducePeers(Connection* requester, Connection* owner, int patchX, int patchZ)
{
    auto reqIt = sessions_.Find(requester);
    auto ownIt = sessions_.Find(owner);
    if (reqIt == sessions_.End() || ownIt == sessions_.End())
        return;

    // Both clients need GUIDs registered
    if (reqIt->second_.guid.Empty() || ownIt->second_.guid.Empty())
    {
        LogMessage("Peer introduction skipped — one or both clients have no GUID registered");
        return;
    }

    // Generate a 32-byte random token
    Vector<unsigned char> token(32);
    randombytes_buf(token.Buffer(), 32);

    // Send MSG_PEER_INTRODUCE to requester
    {
        VectorBuffer intro;
        intro.WriteString(ownIt->second_.guid);
        intro.Write(token.Buffer(), token.Size());
        intro.WriteI32(patchX);
        intro.WriteI32(patchZ);
        requester->SendMessage(MSG_PEER_INTRODUCE, true, true, intro);
    }

    // Send MSG_PEER_INTRODUCE to owner
    {
        VectorBuffer intro;
        intro.WriteString(reqIt->second_.guid);
        intro.Write(token.Buffer(), token.Size());
        intro.WriteI32(patchX);
        intro.WriteI32(patchZ);
        owner->SendMessage(MSG_PEER_INTRODUCE, true, true, intro);
    }

    LogMessage("Introduced peers: " + reqIt->second_.username + " <-> " + ownIt->second_.username +
        " for patch (" + String(patchX) + "," + String(patchZ) + ")");
}

void AuthServer::HandleRelayToAuth(Connection* connection, MemoryBuffer& msg)
{
    // A subserver is relaying a message from its subclient to us
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
    {
        LogMessage("[WARN] MSG_RELAY_TO_AUTH from unauthenticated client — ignoring");
        return;
    }

    String subClientUsername = msg.ReadString();
    int innerMsgID = msg.ReadI32();
    unsigned innerSize = msg.ReadU32();

    // Read the inner message payload
    Vector<unsigned char> innerData(innerSize);
    if (innerSize > 0)
        msg.Read(innerData.Buffer(), innerSize);

    MemoryBuffer innerMsg(innerData.Buffer(), innerData.Size());

    LogMessage("Relay from subclient '" + subClientUsername + "' via subserver '" +
        it->second_.username + "': msgID=" + String(innerMsgID));

    // Process the inner message as if it came from the subclient directly
    // For now, handle MSG_PATCH_QUERY relayed from subclients
    switch (innerMsgID)
    {
    case MSG_PATCH_QUERY:
    {
        int px = innerMsg.ReadI32();
        int pz = innerMsg.ReadI32();
        PatchInfo info = QueryPatchOwner(px, pz);

        // Build the inner reply
        VectorBuffer innerReply;
        innerReply.WriteI32(px);
        innerReply.WriteI32(pz);
        innerReply.WriteString(info.ownerName);
        innerReply.WriteString(info.ownerAddress);
        innerReply.WriteU16(info.ownerPort);

        // Wrap in MSG_RELAY_FROM_AUTH for the subserver to forward to subclient
        VectorBuffer relay;
        relay.WriteString(subClientUsername);
        relay.WriteI32(MSG_PATCH_RESULT);
        relay.WriteU32(innerReply.GetSize());
        relay.Write(innerReply.GetData(), innerReply.GetSize());
        connection->SendMessage(MSG_RELAY_FROM_AUTH, true, true, relay);

        // If patch is owned by a different online client, introduce peers
        // (the introduction goes to subclient via relay, and directly to owner)
        if (!info.ownerName.Empty() && info.ownerName != subClientUsername)
        {
            Connection* ownerConn = FindSessionByUsername(info.ownerName);
            if (ownerConn)
            {
                // Find subclient's GUID through the subserver's session
                // The subclient's GUID was stored when we introduced them originally
                LogMessage("Relay: subclient '" + subClientUsername +
                    "' needs introduction to patch owner '" + info.ownerName + "'");
                // New introductions for migrating subclients would be handled here
                // For now, log it — full migration will be handled in a follow-up
            }
        }
        break;
    }
    default:
        LogMessage("Relay: unhandled inner msgID " + String(innerMsgID));
        break;
    }
}

// ---------------------------------------------------------------------------
// Server-authoritative edits
// ---------------------------------------------------------------------------

bool AuthServer::ValidateEditLocation(const String& username, int adminLevel, const Vector3& worldPos)
{
    // Admins can edit anywhere
    if (adminLevel > 0)
        return true;

    // Convert world position to patch coordinates
    // Terrain spacing = 2.0, patch size = 64 cells → 128 world units per patch
    // Terrain is centered at origin, offset by half-terrain-size
    const float patchWorldSize = 128.0f;  // 64 cells * 2.0 spacing
    int patchX = (int)floorf(worldPos.x_ / patchWorldSize);
    int patchZ = (int)floorf(worldPos.z_ / patchWorldSize);

    PatchInfo info = QueryPatchOwner(patchX, patchZ);
    if (info.ownerName.Empty())
        return false;  // unclaimed patch — no one can edit

    return info.ownerName == username;
}

void AuthServer::HandleEditTerrain(Connection* connection, MemoryBuffer& msg)
{
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
    {
        // Not authenticated — reject
        unsigned editID = msg.ReadU32();
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("Not authenticated");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        return;
    }

    const String& username = it->second_.username;
    int adminLevel = it->second_.adminLevel;

    // Read the full payload
    unsigned editID = msg.ReadU32();
    Vector3 worldPos = msg.ReadVector3();
    int brushMode = msg.ReadI32();
    int brushShape = msg.ReadI32();
    float brushRadius = msg.ReadFloat();
    float brushStrength = msg.ReadFloat();
    float smoothStrength = msg.ReadFloat();
    float brushRotation = msg.ReadFloat();
    float timeStep = msg.ReadFloat();
    float lockedFlattenHeight = msg.ReadFloat();

    // Validate patch ownership
    if (!ValidateEditLocation(username, adminLevel, worldPos))
    {
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("No permission to edit this patch");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        LogMessage("Edit rejected: " + username + " has no permission at (" +
            String((int)worldPos.x_) + "," + String((int)worldPos.z_) + ")");
        return;
    }

    // Sanity checks
    if (brushRadius < 0.25f || brushRadius > 50.0f ||
        brushStrength < 0.0f || brushStrength > 5.0f)
    {
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("Invalid brush parameters");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        return;
    }

    // Apply edit to server's authoritative terrain
    if (terrainBrush_)
    {
        terrainBrush_->SetMode(brushMode);
        terrainBrush_->SetShape(brushShape);
        terrainBrush_->SetRadius(brushRadius);
        terrainBrush_->SetStrength(brushStrength);
        terrainBrush_->SetSmoothStrength(smoothStrength);
        terrainBrush_->SetRotation(brushRotation);
        terrainBrush_->SetFlattenHeight(lockedFlattenHeight);
        terrainBrush_->Apply(worldPos, timeStep);
    }

    // Record edit in journal for reconnect sync
    {
        // Determine which grid cell this edit belongs to (currently only grid 0,0)
        int gridX = 0, gridZ = 0;
        TerrainJournal& journal = journalManager_.GetJournal(gridX, gridZ);
        TerrainEdit terrainEdit;
        terrainEdit.version = journal.GetVersion() + 1;
        terrainEdit.worldPos = worldPos;
        terrainEdit.brushMode = brushMode;
        terrainEdit.brushShape = brushShape;
        terrainEdit.brushRadius = brushRadius;
        terrainEdit.brushStrength = brushStrength;
        terrainEdit.smoothStrength = smoothStrength;
        terrainEdit.brushRotation = brushRotation;
        terrainEdit.flattenHeight = lockedFlattenHeight;
        terrainEdit.timeStep = timeStep;
        terrainEdit.timestamp = (unsigned)time(nullptr);
        journalManager_.RecordEdit(gridX, gridZ, terrainEdit);

        // Update hash periodically (every 50 edits to avoid per-edit cost)
        if (terrainEdit.version % 50 == 0)
        {
            auto* terrain = scene_->GetComponent<Terrain>(true);
            if (terrain && terrain->GetHeightMap())
                journal.UpdateHash(terrain->GetHeightMap());
        }
    }

    // Broadcast to all other authenticated clients
    VectorBuffer broadcastPayload;
    broadcastPayload.WriteU32(editID);
    broadcastPayload.WriteVector3(worldPos);
    broadcastPayload.WriteI32(brushMode);
    broadcastPayload.WriteI32(brushShape);
    broadcastPayload.WriteFloat(brushRadius);
    broadcastPayload.WriteFloat(brushStrength);
    broadcastPayload.WriteFloat(smoothStrength);
    broadcastPayload.WriteFloat(brushRotation);
    broadcastPayload.WriteFloat(timeStep);
    broadcastPayload.WriteFloat(lockedFlattenHeight);

    BroadcastEdit(MSG_EDIT_TERRAIN, broadcastPayload, username, connection);
}

void AuthServer::HandleEditObject(Connection* connection, MemoryBuffer& msg)
{
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
    {
        unsigned editID = msg.ReadU32();
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("Not authenticated");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        return;
    }

    const String& username = it->second_.username;
    int adminLevel = it->second_.adminLevel;

    unsigned editID = msg.ReadU32();
    unsigned char subtype = msg.ReadU8();

    // Read position for ownership validation
    Vector3 validationPos;
    VectorBuffer broadcastPayload;
    broadcastPayload.WriteU32(editID);
    broadcastPayload.WriteU8(subtype);

    switch (subtype)
    {
    case 0:  // create
    {
        String xmlData = msg.ReadString();
        Vector3 position = msg.ReadVector3();
        Vector3 surfaceNormal = msg.ReadVector3();
        validationPos = position;

        // Sanity: XML size limit (64KB)
        if (xmlData.Length() > 65536)
        {
            VectorBuffer reject;
            reject.WriteU32(editID);
            reject.WriteString("Object XML too large");
            connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
            return;
        }

        broadcastPayload.WriteString(xmlData);
        broadcastPayload.WriteVector3(position);
        broadcastPayload.WriteVector3(surfaceNormal);

        // Apply to server scene
        if (scene_)
        {
            XMLFile xmlFile(context_);
            if (xmlFile.FromString(xmlData))
            {
                Quaternion surfaceRot;
                surfaceRot.FromRotationTo(Vector3::UP, surfaceNormal);
                scene_->InstantiateXML(xmlFile.GetRoot(), position, surfaceRot);
            }
        }
        break;
    }
    case 1:  // delete
    {
        unsigned nodeID = msg.ReadU32();
        broadcastPayload.WriteU32(nodeID);

        // Use node's position for validation
        if (scene_)
        {
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                validationPos = node->GetPosition();
                node->Remove();
            }
            else
            {
                VectorBuffer reject;
                reject.WriteU32(editID);
                reject.WriteString("Node not found");
                connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
                return;
            }
        }
        break;
    }
    case 2:  // transform
    {
        unsigned nodeID = msg.ReadU32();
        Vector3 pos = msg.ReadVector3();
        Quaternion rot = msg.ReadQuaternion();
        Vector3 scale = msg.ReadVector3();
        validationPos = pos;

        broadcastPayload.WriteU32(nodeID);
        broadcastPayload.WriteVector3(pos);
        broadcastPayload.WriteQuaternion(rot);
        broadcastPayload.WriteVector3(scale);

        // Apply to server scene
        if (scene_)
        {
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                node->SetPosition(pos);
                node->SetRotation(rot);
                node->SetScale(scale);
            }
        }
        break;
    }
    default:
    {
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("Unknown object edit subtype");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        return;
    }
    }

    // Validate ownership
    if (!ValidateEditLocation(username, adminLevel, validationPos))
    {
        VectorBuffer reject;
        reject.WriteU32(editID);
        reject.WriteString("No permission to edit objects in this patch");
        connection->SendMessage(MSG_EDIT_REJECT, true, true, reject);
        return;
    }

    BroadcastEdit(MSG_EDIT_OBJECT, broadcastPayload, username, connection);
}

void AuthServer::BroadcastEdit(int editMsgType, const VectorBuffer& data, const String& username, Connection* excludeConnection)
{
    VectorBuffer broadcast;
    broadcast.WriteString(username);
    broadcast.WriteI32(editMsgType);
    broadcast.Write(data.GetData(), data.GetSize());

    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        if (it->first_ == excludeConnection)
            continue;
        if (!it->second_.authenticated)
            continue;

        it->first_->SendMessage(MSG_EDIT_BROADCAST, true, true, broadcast);
    }
}

// ---------------------------------------------------------------------------
// Terrain generation
// ---------------------------------------------------------------------------

void AuthServer::RegisterExistingTerrain()
{
    if (!scene_)
        return;

    auto* terrain = scene_->GetComponent<Terrain>(true);
    if (terrain)
    {
        terrainGrid_[IntVector2(0, 0)] = terrain;
        LogMessage("Registered existing terrain as grid (0,0)");

        // Initialize journal hash from current heightmap
        if (terrain->GetHeightMap())
        {
            TerrainJournal& journal = journalManager_.GetJournal(0, 0);
            journal.UpdateHash(terrain->GetHeightMap());
            LogMessage("Terrain journal initialized — hash: " + String(journal.GetHash()));
        }
    }
}

void AuthServer::HandleTerrainSync(Connection* connection, MemoryBuffer& msg)
{
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
    {
        LogMessage("[WARN] MSG_TERRAIN_SYNC from unauthenticated client — ignoring");
        return;
    }

    journalManager_.HandleSyncRequest(connection, msg, terrainGrid_);
    LogMessage("Terrain sync request from " + it->second_.username);
}

SharedPtr<Image> AuthServer::GenerateTerrainHeightmap(int gridX, int gridZ)
{
    // Derive unique seed for this grid cell
    unsigned cellSeed = worldSeed_ ^ ((unsigned)(gridX * 73856093) ^ (unsigned)(gridZ * 19349663));
    terrainGen_.params.seed = cellSeed;

    // Check for neighbour terrains and extract edge data for stitching
    float* northEdge = nullptr;
    float* southEdge = nullptr;
    float* westEdge = nullptr;
    float* eastEdge = nullptr;
    int res = terrainGen_.params.resolution;

    // Helper: extract edge heights from an existing terrain's heightmap
    auto extractEdge = [&](IntVector2 neighbourGrid, int edgeType) -> float*
    {
        auto it = terrainGrid_.Find(neighbourGrid);
        if (it == terrainGrid_.End() || it->second_.Expired())
            return nullptr;

        Image* hm = it->second_->GetHeightMap();
        if (!hm || hm->GetWidth() != res)
            return nullptr;

        float* edge = new float[res];
        unsigned char* data = hm->GetData();
        int comps = hm->GetComponents();

        for (int i = 0; i < res; ++i)
        {
            int px, py;
            if (edgeType == 0)      { px = i; py = res - 1; }  // neighbour's south edge
            else if (edgeType == 1) { px = i; py = 0; }        // neighbour's north edge
            else if (edgeType == 2) { px = res - 1; py = i; }  // neighbour's east edge
            else                    { px = 0; py = i; }        // neighbour's west edge

            int idx = (py * res + px) * comps;
            edge[i] = (float)data[idx] / 255.0f;
            if (comps >= 2)
                edge[i] += (float)data[idx + 1] / 65280.0f;
        }
        return edge;
    };

    // North neighbour (gridZ - 1) provides our north edge (their south edge)
    northEdge = extractEdge(IntVector2(gridX, gridZ - 1), 0);
    // South neighbour (gridZ + 1) provides our south edge (their north edge)
    southEdge = extractEdge(IntVector2(gridX, gridZ + 1), 1);
    // West neighbour (gridX - 1) provides our west edge (their east edge)
    westEdge = extractEdge(IntVector2(gridX - 1, gridZ), 2);
    // East neighbour (gridX + 1) provides our east edge (their west edge)
    eastEdge = extractEdge(IntVector2(gridX + 1, gridZ), 3);

    SharedPtr<Image> image = terrainGen_.GenerateWithEdges(context_, northEdge, southEdge, westEdge, eastEdge);

    delete[] northEdge;
    delete[] southEdge;
    delete[] westEdge;
    delete[] eastEdge;

    if (image)
    {
        // Save to disk
        String filename = "Data/Terrains/terrain_" + String(gridX) + "_" + String(gridZ) + ".png";
        auto* cache = GetSubsystem<ResourceCache>();
        String fullPath = cache->GetResourceDirs()[1] + "../" + filename;  // relative to Data/
        // Ensure directory exists
        String dirPath = cache->GetResourceDirs()[1] + "../Data/Terrains";

        image->SavePNG(fullPath);
        LogMessage("Generated terrain heightmap: " + filename + " (seed=" + String(cellSeed) + ")");

        // Register in ResourceCache so Terrain serialization works
        String resName = "Terrains/terrain_" + String(gridX) + "_" + String(gridZ) + ".png";
        image->SetName(resName);
        cache->AddManualResource(image);

        // Create terrain node in scene
        if (scene_)
        {
            const float terrainWorldSize = 2048.0f;  // 1025 verts * 2.0 spacing
            float posX = gridX * terrainWorldSize - (terrainWorldSize * 0.5f);
            float posZ = gridZ * terrainWorldSize - (terrainWorldSize * 0.5f);

            Node* terrainNode = scene_->CreateChild("Terrain_" + String(gridX) + "_" + String(gridZ), LOCAL);
            terrainNode->SetPosition(Vector3(posX, 0.0f, posZ));

            auto* terrain = terrainNode->CreateComponent<Terrain>(LOCAL);
            terrain->SetSpacing(Vector3(2.0f, 0.5f, 2.0f));
            terrain->SetHeightMap(image);

            // Use same material as the original terrain
            auto* origTerrain = terrainGrid_.Contains(IntVector2(0, 0)) ?
                terrainGrid_[IntVector2(0, 0)].Get() : nullptr;
            if (origTerrain && origTerrain->GetMaterial())
                terrain->SetMaterial(origTerrain->GetMaterial());

            // Terrain collision — static body so avatars don't fall through
            auto* gridBody = terrainNode->CreateComponent<RigidBody>(LOCAL);
            gridBody->SetCollisionLayer(2);
            auto* gridShape = terrainNode->CreateComponent<CollisionShape>(LOCAL);
            gridShape->SetTerrain();

            terrainGrid_[IntVector2(gridX, gridZ)] = terrain;
            LogMessage("Created terrain node at grid (" + String(gridX) + "," + String(gridZ) +
                ") world pos (" + String(posX) + ", 0, " + String(posZ) + ")");
        }
    }

    return image;
}

// ============================================================
// BOM Weather
// ============================================================

static const String BOM_OBS_URL = "https://api.weather.bom.gov.au/v1/locations/r1r143/observations";
static const String BOM_FORECAST_URL = "https://api.weather.bom.gov.au/v1/locations/r1r143/forecasts/daily";

void AuthServer::FetchBOMWeather()
{
    auto* network = GetSubsystem<Network>();
    if (!network)
        return;

    // Don't start new requests while previous ones are in-flight
    if (bomObsRequest_ && bomObsRequest_->GetState() == HTTP_INITIALIZING)
        return;
    if (bomForecastRequest_ && bomForecastRequest_->GetState() == HTTP_INITIALIZING)
        return;

    bomObsData_.Clear();
    bomForecastData_.Clear();
    bomObsRequest_ = network->MakeHttpRequest(BOM_OBS_URL);
    bomForecastRequest_ = network->MakeHttpRequest(BOM_FORECAST_URL);

    LogMessage("BOM: Fetching weather from api.weather.bom.gov.au...");
}

void AuthServer::ProcessBOMResponse()
{
    // Read observation data as it arrives
    if (bomObsRequest_)
    {
        if (bomObsRequest_->GetState() == HTTP_INITIALIZING)
            return; // still connecting

        if (bomObsRequest_->GetState() == HTTP_ERROR)
        {
            LogMessage("BOM: Observation fetch error: " + bomObsRequest_->GetError());
            bomObsRequest_.Reset();
            return;
        }

        // Read available data
        while (bomObsRequest_->GetAvailableSize() > 0)
            bomObsData_ += bomObsRequest_->ReadLine() + "\n";

        // Check if complete
        if (bomObsRequest_->GetState() == HTTP_CLOSED && bomObsRequest_->GetAvailableSize() == 0)
            bomObsRequest_.Reset();  // done reading
        else
            return; // still reading
    }

    // Read forecast data as it arrives
    if (bomForecastRequest_)
    {
        if (bomForecastRequest_->GetState() == HTTP_INITIALIZING)
            return;

        if (bomForecastRequest_->GetState() == HTTP_ERROR)
        {
            LogMessage("BOM: Forecast fetch error: " + bomForecastRequest_->GetError());
            bomForecastRequest_.Reset();
            return;
        }

        while (bomForecastRequest_->GetAvailableSize() > 0)
            bomForecastData_ += bomForecastRequest_->ReadLine() + "\n";

        if (bomForecastRequest_->GetState() == HTTP_CLOSED && bomForecastRequest_->GetAvailableSize() == 0)
            bomForecastRequest_.Reset();
        else
            return;
    }

    // Both requests complete — parse
    if (bomObsData_.Empty() && bomForecastData_.Empty())
        return;

    // Parse observations
    if (!bomObsData_.Empty())
    {
        SharedPtr<JSONFile> json(new JSONFile(context_));
        if (json->FromString(bomObsData_))
        {
            const JSONValue& root = json->GetRoot();
            const JSONValue& data = root.Get("data");

            if (!data.IsNull())
            {
                weatherTemperature_ = data.Get("temp").GetFloat();
                weatherHumidity_ = data.Get("humidity").GetFloat();

                const JSONValue& wind = data.Get("wind");
                if (!wind.IsNull())
                {
                    weatherWindSpeed_ = wind.Get("speed_kilometre").GetFloat();
                    // Convert wind direction string to angle
                    String dir = wind.Get("direction").GetString();
                    if (dir == "N") weatherWindAngle_ = 0.0f;
                    else if (dir == "NNE") weatherWindAngle_ = 22.5f;
                    else if (dir == "NE") weatherWindAngle_ = 45.0f;
                    else if (dir == "ENE") weatherWindAngle_ = 67.5f;
                    else if (dir == "E") weatherWindAngle_ = 90.0f;
                    else if (dir == "ESE") weatherWindAngle_ = 112.5f;
                    else if (dir == "SE") weatherWindAngle_ = 135.0f;
                    else if (dir == "SSE") weatherWindAngle_ = 157.5f;
                    else if (dir == "S") weatherWindAngle_ = 180.0f;
                    else if (dir == "SSW") weatherWindAngle_ = 202.5f;
                    else if (dir == "SW") weatherWindAngle_ = 225.0f;
                    else if (dir == "WSW") weatherWindAngle_ = 247.5f;
                    else if (dir == "W") weatherWindAngle_ = 270.0f;
                    else if (dir == "WNW") weatherWindAngle_ = 292.5f;
                    else if (dir == "NW") weatherWindAngle_ = 315.0f;
                    else if (dir == "NNW") weatherWindAngle_ = 337.5f;
                    // CALM = 0 angle, 0 speed
                }

                const JSONValue& rainSince9am = data.Get("rain_since_9am");
                if (!rainSince9am.IsNull())
                    weatherPrecipitation_ = Clamp(rainSince9am.GetFloat() / 20.0f, 0.0f, 1.0f); // 20mm = heavy
            }
        }
        bomObsData_.Clear();
    }

    // Parse forecast for cloud cover / conditions
    if (!bomForecastData_.Empty())
    {
        SharedPtr<JSONFile> json(new JSONFile(context_));
        if (json->FromString(bomForecastData_))
        {
            const JSONValue& root = json->GetRoot();
            const JSONValue& data = root.Get("data");

            if (data.IsArray() && data.GetArray().Size() > 0)
            {
                const JSONValue& today = data.GetArray()[0];
                weatherCondition_ = today.Get("icon_descriptor").GetString();

                // Map icon_descriptor to cloud cover
                if (weatherCondition_ == "sunny" || weatherCondition_ == "clear")
                    weatherCloudCover_ = 0.05f;
                else if (weatherCondition_ == "mostly_sunny")
                    weatherCloudCover_ = 0.2f;
                else if (weatherCondition_ == "partly_cloudy")
                    weatherCloudCover_ = 0.4f;
                else if (weatherCondition_ == "mostly_cloudy" || weatherCondition_ == "cloudy")
                    weatherCloudCover_ = 0.7f;
                else if (weatherCondition_ == "hazy")
                    weatherCloudCover_ = 0.5f;
                else if (weatherCondition_ == "fog")
                    weatherCloudCover_ = 0.9f;
                else if (weatherCondition_ == "shower" || weatherCondition_ == "light_shower")
                    weatherCloudCover_ = 0.7f;
                else if (weatherCondition_ == "rain" || weatherCondition_ == "light_rain")
                    weatherCloudCover_ = 0.8f;
                else if (weatherCondition_ == "heavy_rain" || weatherCondition_ == "storm" || weatherCondition_ == "thunderstorm")
                    weatherCloudCover_ = 0.95f;
                else if (weatherCondition_ == "snow" || weatherCondition_ == "hail")
                    weatherCloudCover_ = 0.85f;
                else if (weatherCondition_ == "wind" || weatherCondition_ == "windy")
                    weatherCloudCover_ = 0.3f;
                else
                    weatherCloudCover_ = 0.3f; // default fallback

                // Rain chance from forecast if observations don't show rain yet
                const JSONValue& rain = today.Get("rain");
                if (!rain.IsNull())
                {
                    float chance = rain.Get("chance").GetFloat() / 100.0f;
                    float amount = rain.Get("amount").Get("max").GetFloat();
                    // Use forecast rain if observed rain is 0
                    if (weatherPrecipitation_ < 0.01f && chance > 0.5f)
                        weatherPrecipitation_ = Clamp(chance * amount / 20.0f, 0.0f, 1.0f);
                }
            }
        }
        bomForecastData_.Clear();
    }

    // Normalize wind to 0-1 range (0-80 km/h scale)
    float normalizedWind = Clamp(weatherWindSpeed_ / 80.0f, 0.0f, 1.0f);

    weatherReady_ = true;
    weatherBroadcastTimer_ = 0.0f; // broadcast immediately after first fetch

    LogMessage("BOM: Weather updated — " + weatherCondition_ +
        ", " + String((int)weatherTemperature_) + "C" +
        ", cloud " + String((int)(weatherCloudCover_ * 100)) + "%" +
        ", wind " + String((int)weatherWindSpeed_) + "km/h" +
        ", precip " + String((int)(weatherPrecipitation_ * 100)) + "%");

    RefreshWeatherPanel();
}

void AuthServer::SendWeatherToClient(Connection* connection)
{
    VectorBuffer msg;
    msg.WriteFloat(weatherCloudCover_);
    msg.WriteFloat(weatherPrecipitation_);
    msg.WriteFloat(Clamp(weatherWindSpeed_ / 80.0f, 0.0f, 1.0f)); // normalized 0-1
    msg.WriteFloat(weatherWindAngle_ * 3.14159f / 180.0f);         // degrees to radians
    msg.WriteFloat(weatherTemperature_);
    msg.WriteFloat(weatherHumidity_);
    msg.WriteString(weatherCondition_);
    connection->SendMessage(MSG_WEATHER_UPDATE, true, true, msg);
}

void AuthServer::BroadcastWeather(Connection* singleClient)
{
    if (singleClient)
    {
        SendWeatherToClient(singleClient);
        return;
    }

    int count = 0;
    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        if (!it->second_.authenticated)
            continue;
        SendWeatherToClient(it->first_);
        count++;
    }

    if (count > 0)
        LogMessage("BOM: Weather broadcast to " + String(count) + " client(s)");
}

// ============================================================
// Water heightmap (server-authoritative)
// ============================================================

void AuthServer::LoadOrCreateWaterMap()
{
    auto* cache = GetSubsystem<ResourceCache>();

    // Try to load existing water heightmap from disk
    String resourcePath = "Textures/WaterHeightMap.png";
    auto* existing = cache->GetResource<Image>(resourcePath, false);
    if (existing)
    {
        waterHeightMap_ = existing;
        LogMessage("Water map loaded: " + resourcePath +
                   " (" + String(waterHeightMap_->GetWidth()) + "x" + String(waterHeightMap_->GetHeight()) + ")");
        return;
    }

    // Create empty water heightmap (same resolution as terrain: 1025x1025, single-channel)
    waterHeightMap_ = new Image(context_);
    waterHeightMap_->SetSize(1025, 1025, 1);  // single-channel grayscale

    // Zero-fill — no water anywhere
    for (int y = 0; y < 1025; ++y)
        for (int x = 0; x < 1025; ++x)
            waterHeightMap_->SetPixel(x, y, Color(0.0f, 0.0f, 0.0f));

    waterHeightMap_->SetName(resourcePath);
    cache->AddManualResource(waterHeightMap_);
    LogMessage("Water map created: 1025x1025 (empty)");
}

void AuthServer::SaveWaterMap()
{
    if (!waterHeightMap_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    String fullPath = cache->GetResourceDirs()[0] + "Textures/WaterHeightMap.png";
    waterHeightMap_->SavePNG(fullPath);
    LogMessage("Water map saved: " + fullPath);
}

// ============================================================
// Per-patch resource streaming
// ============================================================

static const float PATCH_WORLD_SIZE = 128.0f;  // 64 cells × 2.0 spacing
static const int PATCH_PIXELS = 64;             // pixels per patch in resource map

void AuthServer::SendResourcePatch(Connection* connection, const String& resourceID, Image* resourceMap, int patchX, int patchZ)
{
    if (!resourceMap)
        return;

    int imgW = resourceMap->GetWidth();
    int imgH = resourceMap->GetHeight();
    int components = resourceMap->GetComponents();

    // Patch pixel origin in the resource image (0,0 patch is centered)
    int pixelX = imgW / 2 + patchX * PATCH_PIXELS;
    int pixelZ = imgH / 2 + patchZ * PATCH_PIXELS;

    // Clamp to image bounds
    int x0 = Clamp(pixelX, 0, imgW);
    int z0 = Clamp(pixelZ, 0, imgH);
    int x1 = Clamp(pixelX + PATCH_PIXELS, 0, imgW);
    int z1 = Clamp(pixelZ + PATCH_PIXELS, 0, imgH);
    int pixelW = x1 - x0;
    int pixelH = z1 - z0;

    if (pixelW <= 0 || pixelH <= 0)
        return;  // patch is entirely outside the image

    // Copy raw pixel data
    unsigned dataSize = pixelW * pixelH * components;
    const unsigned char* src = resourceMap->GetData();
    int stride = imgW * components;

    VectorBuffer msg;
    msg.WriteString(resourceID);
    msg.WriteI32(patchX);
    msg.WriteI32(patchZ);
    msg.WriteI32(x0);
    msg.WriteI32(z0);
    msg.WriteI32(pixelW);
    msg.WriteI32(pixelH);
    msg.WriteI32(components);
    msg.WriteU32(dataSize);

    for (int row = 0; row < pixelH; ++row)
        msg.Write(src + (z0 + row) * stride + x0 * components, pixelW * components);

    connection->SendMessage(MSG_RESOURCE_PATCH, true, true, msg);
}

void AuthServer::SendPatchNeighbourhood(Connection* connection, int centerX, int centerZ)
{
    auto it = sessions_.Find(connection);
    if (it == sessions_.End())
        return;

    int sent = 0;
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int px = centerX + dx;
            int pz = centerZ + dz;
            unsigned long long key = PatchKey(px, pz);

            if (it->second_.sentPatches.Contains(key))
                continue;  // already sent this patch

            // Send water heightmap patch
            if (waterHeightMap_)
                SendResourcePatch(connection, "water_heightmap", waterHeightMap_, px, pz);

            // Future: send other resource maps here (moisture, vegetation, etc.)

            it->second_.sentPatches.Insert(key);
            ++sent;
        }
    }

    if (sent > 0)
        LogMessage("Streamed " + String(sent) + " resource patch(es) to " + connection->ToString() +
                   " around (" + String(centerX) + "," + String(centerZ) + ")");
}

void AuthServer::HandlePatchPosition(Connection* connection, MemoryBuffer& msg)
{
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
        return;

    int patchX = msg.ReadI32();
    int patchZ = msg.ReadI32();

    it->second_.lastPatchPos = IntVector2(patchX, patchZ);
    SendPatchNeighbourhood(connection, patchX, patchZ);
}

void AuthServer::BroadcastAffectedPatch(int editPatchX, int editPatchZ, const String& resourceID, Image* resourceMap)
{
    for (auto it = sessions_.Begin(); it != sessions_.End(); ++it)
    {
        if (!it->second_.authenticated)
            continue;

        IntVector2 clientPatch = it->second_.lastPatchPos;
        // Check if editPatch falls within client's 3×3 window
        if (Abs(editPatchX - clientPatch.x_) <= 1 && Abs(editPatchZ - clientPatch.y_) <= 1)
            SendResourcePatch(it->first_, resourceID, resourceMap, editPatchX, editPatchZ);
    }
}

void AuthServer::HandleWaterEdit(Connection* connection, MemoryBuffer& msg)
{
    // Verify sender is authenticated
    auto it = sessions_.Find(connection);
    if (it == sessions_.End() || !it->second_.authenticated)
    {
        LogMessage("Water edit rejected: unauthenticated client");
        return;
    }

    // Read brush parameters from the message
    float worldX = msg.ReadFloat();
    float worldZ = msg.ReadFloat();
    float radius = msg.ReadFloat();
    float strength = msg.ReadFloat();
    bool raise = msg.ReadBool();

    if (!waterHeightMap_)
        return;

    int width = waterHeightMap_->GetWidth();
    int height = waterHeightMap_->GetHeight();

    // Convert world coords to pixel coords
    float terrainSpacingX = 2.0f;
    float terrainSpacingZ = 2.0f;
    float halfSize = (width - 1) * terrainSpacingX * 0.5f;

    int centerPX = (int)((worldX + halfSize) / terrainSpacingX);
    int centerPZ = (int)((worldZ + halfSize) / terrainSpacingZ);
    int pixelRadius = (int)(radius / terrainSpacingX);

    // Track which patches are touched by this edit
    HashSet<unsigned long long> touchedPatches;

    int modified = 0;
    for (int pz = Max(0, centerPZ - pixelRadius); pz <= Min(height - 1, centerPZ + pixelRadius); ++pz)
    {
        for (int px = Max(0, centerPX - pixelRadius); px <= Min(width - 1, centerPX + pixelRadius); ++px)
        {
            float dx = (float)(px - centerPX);
            float dz = (float)(pz - centerPZ);
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist > pixelRadius)
                continue;

            float falloff = 1.0f - (dist / (float)pixelRadius);
            float delta = strength * falloff;

            Color c = waterHeightMap_->GetPixel(px, pz);
            float val = c.r_;
            if (raise)
                val = Min(val + delta, 1.0f);
            else
                val = Max(val - delta, 0.0f);
            waterHeightMap_->SetPixel(px, pz, Color(val, val, val));
            ++modified;

            // Record which patch this pixel belongs to
            int patchPX = (px - width / 2) / PATCH_PIXELS;
            int patchPZ = (pz - height / 2) / PATCH_PIXELS;
            if (px < width / 2) patchPX--;  // negative side rounds down
            if (pz < height / 2) patchPZ--;
            touchedPatches.Insert(PatchKey(patchPX, patchPZ));
        }
    }

    if (modified == 0)
        return;

    // Broadcast affected patches to all clients who can see them
    for (auto pit = touchedPatches.Begin(); pit != touchedPatches.End(); ++pit)
    {
        int tpx = (int)(*pit >> 32);
        int tpz = (int)(*pit & 0xFFFFFFFF);
        BroadcastAffectedPatch(tpx, tpz, "water_heightmap", waterHeightMap_);
    }
}
