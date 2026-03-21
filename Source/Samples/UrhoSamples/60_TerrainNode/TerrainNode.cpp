// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Graphics/BillboardSet.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/CustomGeometry.h>
#include <Urho3D/Graphics/DebugRenderer.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Light.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Technique.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/RenderPath.h>
#include <Urho3D/Graphics/Skybox.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Graphics/AnimatedModel.h>
#include <Urho3D/Graphics/Animation.h>
#include <Urho3D/Graphics/AnimationController.h>
#include <Urho3D/Graphics/ParticleEffect.h>
#include <Urho3D/Graphics/ParticleEmitter.h>
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/Graphics/TerrainPatch.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/GraphicsAPI/RenderSurface.h>
#include <Urho3D/GraphicsAPI/Texture2D.h>
#include <Urho3D/GraphicsAPI/TextureCube.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/IO/IOEvents.h>
#include <Urho3D/IO/MemoryBuffer.h>
#include <Urho3D/IO/VectorBuffer.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Physics/CollisionShape.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Scene/SceneEvents.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Slider.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>
#include <Urho3D/Core/Timer.h>
#include <Urho3D/Network/Network.h>
#include <Urho3D/Network/Protocol.h>
#include <Urho3D/Resource/Image.h>
#include <Urho3D/GraphicsAPI/Shader.h>
#include <Urho3D/GraphicsAPI/ShaderVariation.h>
#include <Urho3D/GraphicsAPI/VertexBuffer.h>
#include <Urho3D/GraphicsAPI/IndexBuffer.h>
#include <Urho3D/Graphics/Geometry.h>
#ifdef URHO3D_VULKAN
#include <Urho3D/GraphicsAPI/Vulkan/VulkanGraphicsImpl.h>
#endif

#include "TerrainNode.h"

#include <Urho3D/DebugNew.h>
#include <Urho3D/Graphics/ProfilerUI.h>

#include <Urho3D/Math/Random.h>
#include <ctime>
#include <cmath>

URHO3D_DEFINE_APPLICATION_MAIN(TerrainNode)

static const float MELBOURNE_LAT = -37.8136f;
static const float MELBOURNE_LON = 144.96f;
static const float SOLAR_NOON = 12.0f + (11.0f * 15.0f - MELBOURNE_LON) / 15.0f;
static const float CELESTIAL_TIME_SCALE = 1.0f;
static const float DEG_TO_RAD = M_PI / 180.0f;
static const float RAD_TO_DEG = 180.0f / M_PI;

TerrainNode::TerrainNode(Context* context) :
    Sample(context)
{
}

void TerrainNode::Stop()
{
    skyboxMat_.Reset();
    for (int i = 0; i < 4; ++i)
        seasonSkyboxes_[i].Reset();
    sunMat_.Reset();
    moonMat_.Reset();
    profilerUI_.Reset();
    terrainPanel_.Reset();
    hierarchyWindow_.Reset();
    inspectorWindow_.Reset();
    editableHeightMap_.Reset();
    if (prefabBrush_)
    {
        prefabBrush_->Remove();
        prefabBrush_.Reset();
    }
}

bool TerrainNode::OnEscapePressed()
{
    // Peel layers from outside in — last opened, first closed
    if (fileSelector_)
    {
        fileSelector_.Reset();
        return true;
    }
    if (generateMeshPanel_ && generateMeshPanel_->IsVisible())
    {
        generateMeshPanel_->SetVisible(false);
        return true;
    }
    if (inspectorWindow_ && inspectorWindow_->IsVisible())
    {
        inspectorWindow_->SetVisible(false);
        return true;
    }
    if (hierarchyWindow_ && hierarchyWindow_->IsVisible())
    {
        hierarchyWindow_->SetVisible(false);
        return true;
    }
    if (terrainPanel_ && terrainPanel_->IsVisible())
    {
        terrainPanel_->SetVisible(false);
        return true;
    }
    if (brushMode_ != 0)
    {
        brushMode_ = 0;
        return true;
    }
    if (selectedNode_)
    {
        DeselectNode();
        return true;
    }
    if (!menuOpen_)
    {
        // Switch from camera mode to cursor mode
        menuOpen_ = true;
        GetSubsystem<Input>()->SetMouseVisible(true);
        GetSubsystem<Input>()->SetMouseGrabbed(false);
        return true;
    }
    return false;  // nothing to close — let base class exit
}

void TerrainNode::SelectNode(Node* node)
{
    if (!node)
        return;

    DeselectNode();
    selectedNode_ = node;

    auto* model = node->GetComponent<StaticModel>();
    if (model)
    {
        for (unsigned i = 0; i < model->GetNumGeometries(); ++i)
        {
            Material* orig = model->GetMaterial(i);
            originalMaterials_.Push(SharedPtr<Material>(orig));

            if (!orig)
                continue;

            SharedPtr<Material> maskMat(orig->Clone());
            Technique* origTech = maskMat->GetTechnique(0);
            if (origTech)
            {
                SharedPtr<Technique> techClone = origTech->Clone();
                Pass* maskPass = techClone->CreatePass("mask");
                maskPass->SetVertexShader("Silhouette");
                maskPass->SetPixelShader("Silhouette");
                maskPass->SetVertexShaderDefines("MASK");
                maskPass->SetPixelShaderDefines("MASK");
                maskMat->SetTechnique(0, techClone);
            }
            maskMat->SetShaderParameter("MatDiffColor", Color(1.0f, 1.0f, 1.0f, 1.0f));
            model->SetMaterial(i, maskMat);
        }
    }

    // Sync hierarchy tree & inspector
    HighlightInHierarchy(node);
    if (inspectorWindow_ && inspectorWindow_->IsVisible())
        RebuildInspector();
}

void TerrainNode::DeselectNode()
{
    if (selectedNode_ && !selectedNode_.Expired())
    {
        auto* model = selectedNode_->GetComponent<StaticModel>();
        if (model)
        {
            for (unsigned i = 0; i < originalMaterials_.Size() && i < model->GetNumGeometries(); ++i)
                model->SetMaterial(i, originalMaterials_[i]);
        }

        // Wake rigid bodies so physics settles the object after transforms
        if (gizmoMode_ != 0)
            WakeSelectedNode();
    }

    originalMaterials_.Clear();
    selectedNode_.Reset();

    // Clear inspector
    if (inspectorWindow_ && inspectorWindow_->IsVisible())
        RebuildInspector();
}

// Custom message IDs (must match AuthServer)
static const int MSG_AUTH_LOGIN      = 100;
static const int MSG_AUTH_REGISTER   = 101;
static const int MSG_AUTH_RESULT     = 102;
static const int MSG_LOAD_SCENE      = 103;
static const int MSG_REGISTER_GUID   = 104;  // Client → AuthServer: register NAT GUID after auth
static const int MSG_WEATHER_UPDATE  = 120;  // AuthServer → Client: weather forecast

// Remote event for telling clients which avatar node they control
static const StringHash E_CLIENTOBJECTID("ClientObjectID");
static const StringHash P_ID("ID");
// Edit messages: MSG_EDIT_TERRAIN (0xA2), MSG_EDIT_OBJECT (0xA3),
// MSG_EDIT_REJECT (0xA4), MSG_EDIT_BROADCAST (0xA5) — defined in Protocol.h

void TerrainNode::Start()
{
    Sample::Start();

    // Register player character component for scene replication
    PlayerCharacter::RegisterObject(context_);

    // Register animal types
    Rabbit::RegisterObject(context_);
    Deer::RegisterObject(context_);
    Fox::RegisterObject(context_);
    Fish::RegisterObject(context_);
    SchoolFish::RegisterObject(context_);

    // Fallback RNG seed — local time + favourite prime.
    SetRandomSeed((unsigned)time(nullptr) + 25773u);

    auto* cache = GetSubsystem<ResourceCache>();
    auto* uiStyle = cache->GetResource<XMLFile>("UI/WarmStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(uiStyle);

    // Load font from shared theme prefs
    LoadThemePrefs();
    font_ = cache->GetResource<Font>("Fonts/" + currentFontName_ + ".ttf");
    if (!font_)
        font_ = font_;

    // Login scene — animated sky backdrop
    CreateLoginScene();
    SetupViewport();
    SubscribeToEvents();
    CreateLoginUI();

    Sample::InitMouseMode(MM_FREE);

    auto* graphics = GetSubsystem<Graphics>();
    auto* ui = GetSubsystem<UI>();
    profilerUI_ = new ProfilerUI(context_);
    profilerUI_->Initialize(ui, graphics->GetVulkanProfiler(), graphics);
    profilerUI_->SetVisible(false);
}

// ============================================================================
// Login Scene — animated sky backdrop
// ============================================================================

void TerrainNode::CreateLoginScene()
{
    auto* cache = GetSubsystem<ResourceCache>();

    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>();

    // Zone — dark ambient, atmospheric
    Node* zoneNode = scene_->CreateChild("Zone");
    auto* zone = zoneNode->CreateComponent<Zone>();
    zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
    zone->SetAmbientColor(Color(0.15f, 0.15f, 0.15f));
    zone->SetFogColor(Color(0.1f, 0.1f, 0.15f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);
    zone_ = zone;
    origFogColor_ = zone->GetFogColor();
    origFogStart_ = zone->GetFogStart();
    origFogEnd_ = zone->GetFogEnd();

    // Directional light (sun)
    Node* lightNode = scene_->CreateChild("DirectionalLight", LOCAL);
    lightNode->SetDirection(Vector3(0.6f, -1.0f, 0.8f));
    sunLight_ = lightNode->CreateComponent<Light>();
    sunLight_->SetLightType(LIGHT_DIRECTIONAL);
    sunLight_->SetColor(Color(1.2f, 1.2f, 1.2f));

    // Skybox
    Node* skyNode = scene_->CreateChild("Sky");
    skyNode->SetScale(500.0f);
    auto* skybox = skyNode->CreateComponent<Skybox>();
    skybox->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    skyboxMat_ = cache->GetResource<Material>("Materials/Skybox.xml");
    skybox->SetMaterial(skyboxMat_);

    // Preload seasonal skybox cubemaps
    seasonSkyboxes_[0] = cache->GetResource<TextureCube>("Textures/SkyboxSpring.xml");
    seasonSkyboxes_[1] = cache->GetResource<TextureCube>("Textures/SkyboxSummer.xml");
    seasonSkyboxes_[2] = cache->GetResource<TextureCube>("Textures/SkyboxAutumn.xml");
    seasonSkyboxes_[3] = cache->GetResource<TextureCube>("Textures/SkyboxWinter.xml");

    // Initialize time from system clock (Melbourne AEDT = UTC+11)
    {
        time_t now = time(nullptr);
        struct tm* utc = gmtime(&now);
        int hour = utc->tm_hour + 11;
        int yday = utc->tm_yday + 1;
        if (hour >= 24) { hour -= 24; yday++; }
        dayOfYear_ = yday;
        baseDayOfYear_ = yday;
        timeOfDay_ = (float)hour + utc->tm_min / 60.0f + utc->tm_sec / 3600.0f;

        struct tm refMoon = {};
        refMoon.tm_year = 125;
        refMoon.tm_mon = 0;
        refMoon.tm_mday = 29;
        refMoon.tm_hour = 12;
        time_t refTime = mktime(&refMoon);
        double daysSinceNewMoon = difftime(now, refTime) / 86400.0;
        moonAge_ = (float)fmod(daysSinceNewMoon, 29.53);
        if (moonAge_ < 0.0f) moonAge_ += 29.53f;
        baseMoonAge_ = moonAge_;
    }

    // Command-line override: -time 10.5
    bool timeOverride = false;
    const Vector<String>& args = GetArguments();
    for (unsigned i = 0; i < args.Size(); ++i)
    {
        if ((args[i] == "-time" || args[i] == "time") && i + 1 < args.Size())
        {
            timeOfDay_ = ToFloat(args[i + 1]);
            if (timeOfDay_ < 0.0f) timeOfDay_ += 24.0f;
            if (timeOfDay_ >= 24.0f) timeOfDay_ -= 24.0f;
            timeOverride = true;
            break;
        }
    }

    if (!timeOverride)
        SyncMelbourneTime();
    timeSyncTimer_ = timeOverride ? 9999.0f : 300.0f;

    CreateCelestialBodies();

    // Camera — angled up at the sky
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(750.0f);
    cameraNode_->SetPosition(Vector3(0.0f, 0.0f, 0.0f));
    cameraNode_->SetRotation(Quaternion(30.0f, 0.0f, 0.0f)); // look upward
}

// ============================================================================
// Login UI
// ============================================================================

void TerrainNode::CreateLoginUI()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* ui = GetSubsystem<UI>();
    auto* root = ui->GetRoot();
    Font* font = font_;

    // Centered login window
    loginWindow_ = root->CreateChild<Window>();
    loginWindow_->SetStyle("Window");
    loginWindow_->SetLayout(LM_VERTICAL, 10, IntRect(20, 20, 20, 20));
    loginWindow_->SetAlignment(HA_CENTER, VA_CENTER);
    loginWindow_->SetMinSize(340, 300);
    loginWindow_->SetColor(Color(0.12f, 0.12f, 0.18f, 0.92f));

    // Title
    auto* title = loginWindow_->CreateChild<Text>();
    title->SetFont(font, 22);
    title->SetText("TerrainNode");
    title->SetColor(Color(0.8f, 0.85f, 1.0f));
    title->SetHorizontalAlignment(HA_CENTER);

    // Subtitle / server status
    loginStatusText_ = loginWindow_->CreateChild<Text>();
    loginStatusText_->SetFont(font, 12);
    loginStatusText_->SetText("Scanning for server...");
    loginStatusText_->SetColor(Color(0.6f, 0.6f, 0.6f));
    loginStatusText_->SetHorizontalAlignment(HA_CENTER);

    // Spacer
    auto* spacer = loginWindow_->CreateChild<UIElement>();
    spacer->SetFixedHeight(10);

    // Username label + field
    auto* userLabel = loginWindow_->CreateChild<Text>();
    userLabel->SetFont(font, 13);
    userLabel->SetText("Username");
    userLabel->SetColor(Color(0.7f, 0.7f, 0.8f));

    usernameEdit_ = loginWindow_->CreateChild<LineEdit>();
    usernameEdit_->SetStyle("LineEdit");
    usernameEdit_->SetFixedHeight(28);
    usernameEdit_->SetMinWidth(300);

    // Password label + field
    auto* passLabel = loginWindow_->CreateChild<Text>();
    passLabel->SetFont(font, 13);
    passLabel->SetText("Password");
    passLabel->SetColor(Color(0.7f, 0.7f, 0.8f));

    passwordEdit_ = loginWindow_->CreateChild<LineEdit>();
    passwordEdit_->SetStyle("LineEdit");
    passwordEdit_->SetFixedHeight(28);
    passwordEdit_->SetMinWidth(300);
    passwordEdit_->SetEchoCharacter('*');

    // Spacer
    auto* spacer2 = loginWindow_->CreateChild<UIElement>();
    spacer2->SetFixedHeight(8);

    // Button row
    auto* btnRow = loginWindow_->CreateChild<UIElement>();
    btnRow->SetLayout(LM_HORIZONTAL, 10);
    btnRow->SetFixedHeight(36);
    btnRow->SetHorizontalAlignment(HA_CENTER);

    // Login button
    loginBtn_ = btnRow->CreateChild<Button>();
    loginBtn_->SetStyle("Button");
    loginBtn_->SetFixedSize(130, 34);
    auto* loginLabel = loginBtn_->CreateChild<Text>();
    loginLabel->SetFont(font, 15);
    loginLabel->SetText("Login");
    loginLabel->SetHorizontalAlignment(HA_CENTER);
    loginLabel->SetVerticalAlignment(VA_CENTER);
    loginLabel->SetColor(Color(0.9f, 0.9f, 1.0f));

    // Register button
    registerBtn_ = btnRow->CreateChild<Button>();
    registerBtn_->SetStyle("Button");
    registerBtn_->SetFixedSize(130, 34);
    auto* regLabel = registerBtn_->CreateChild<Text>();
    regLabel->SetFont(font, 15);
    regLabel->SetText("Register");
    regLabel->SetHorizontalAlignment(HA_CENTER);
    regLabel->SetVerticalAlignment(VA_CENTER);
    regLabel->SetColor(Color(0.9f, 0.9f, 1.0f));

    // Play Offline button (debug — bypass auth)
    auto* offlineBtn = btnRow->CreateChild<Button>();
    offlineBtn->SetStyle("Button");
    offlineBtn->SetFixedSize(130, 34);
    auto* offLabel = offlineBtn->CreateChild<Text>();
    offLabel->SetFont(font, 15);
    offLabel->SetText("Offline");
    offLabel->SetHorizontalAlignment(HA_CENTER);
    offLabel->SetVerticalAlignment(VA_CENTER);
    offLabel->SetColor(Color(1.0f, 0.8f, 0.4f));

    // Wire up buttons
    SubscribeToEvent(loginBtn_, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleLoginButton));
    SubscribeToEvent(registerBtn_, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleRegisterButton));
    SubscribeToEvent(offlineBtn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleOfflineButton));
}

void TerrainNode::DestroyLoginUI()
{
    if (loginWindow_)
    {
        loginWindow_->Remove();
        loginWindow_ = nullptr;
    }
    usernameEdit_ = nullptr;
    passwordEdit_ = nullptr;
    loginBtn_ = nullptr;
    registerBtn_ = nullptr;
    loginStatusText_ = nullptr;
}

void TerrainNode::HandleLoginButton(StringHash eventType, VariantMap& eventData)
{
    String username = usernameEdit_ ? usernameEdit_->GetText().Trimmed() : String::EMPTY;
    String password = passwordEdit_ ? passwordEdit_->GetText().Trimmed() : String::EMPTY;

    if (username.Empty() || password.Empty())
    {
        if (loginStatusText_)
        {
            loginStatusText_->SetText("Enter username and password");
            loginStatusText_->SetColor(Color(1.0f, 0.6f, 0.2f));
        }
        return;
    }

    if (loginStatusText_)
    {
        loginStatusText_->SetText("Authenticating...");
        loginStatusText_->SetColor(Color(0.7f, 0.7f, 0.7f));
    }

    // PAKE: set credentials, then connect (or reconnect if already connected)
    auto* network = GetSubsystem<Network>();
    network->SetCredentials(username, password);

    // If already connected, disconnect first — need fresh key exchange with PAKE
    if (authConnected_)
    {
        network->Disconnect(100);
        authConnected_ = false;
    }

    // Connect with PAKE — key exchange will include username and mix password hash
    network->Connect(authServerAddress_, authServerPort_, scene_);
}

void TerrainNode::HandleRegisterButton(StringHash eventType, VariantMap& eventData)
{
    String username = usernameEdit_ ? usernameEdit_->GetText().Trimmed() : String::EMPTY;
    String password = passwordEdit_ ? passwordEdit_->GetText().Trimmed() : String::EMPTY;

    if (username.Empty() || password.Empty())
    {
        if (loginStatusText_)
        {
            loginStatusText_->SetText("Enter username and password");
            loginStatusText_->SetColor(Color(1.0f, 0.6f, 0.2f));
        }
        return;
    }

    if (loginStatusText_)
    {
        loginStatusText_->SetText("Registering...");
        loginStatusText_->SetColor(Color(0.7f, 0.7f, 0.7f));
    }

    auto* network = GetSubsystem<Network>();

    // Registration uses plain DH (no PAKE) — no password exists yet
    network->ClearCredentials();

    // If not connected, connect first (plain DH), then send register on connect
    if (!authConnected_)
    {
        // Store credentials for sending after connect
        pendingRegisterUsername_ = username;
        pendingRegisterPassword_ = password;
        network->Connect(authServerAddress_, authServerPort_, scene_);
        return;
    }

    // Already connected — send register directly
    Connection* serverConnection = network->GetServerConnection();
    if (serverConnection)
    {
        VectorBuffer msg;
        msg.WriteString(username);
        msg.WriteString(password);
        serverConnection->SendMessage(MSG_AUTH_REGISTER, true, true, msg);
    }
}

void TerrainNode::HandleOfflineButton(StringHash eventType, VariantMap& eventData)
{
    loggedInUsername_ = "Offline";
    URHO3D_LOGINFO("Playing offline — bypassing auth");
    loginTimer_.Reset();
    loginTimerActive_ = true;
    firstFramePending_ = true;
    EnterWorld();
}

// ============================================================================
// Enter World — called after successful authentication
// ============================================================================

void TerrainNode::EnterWorld()
{
    loggedIn_ = true;
    DestroyLoginUI();

    // Wait for GPU to finish all commands referencing login scene resources
    // before destroying the login scene (prevents use-after-free on textures/imageViews)
#ifdef URHO3D_VULKAN
    auto* graphics = GetSubsystem<Graphics>();
    if (graphics)
    {
        auto* impl = graphics->GetImpl_Vulkan();
        if (impl && impl->GetDevice())
            vkDeviceWaitIdle(impl->GetDevice());
    }
#endif

    // Build the entire world scene procedurally (matching proven Sample 23 pattern)
    CreateScene();

    // Go straight to the world — no async loading needed
    FinishEnterWorld();
}

void TerrainNode::FinishEnterWorld()
{
    // Remove loading overlay
    if (loadingText_)
    {
        loadingText_->Remove();
        loadingText_.Reset();
    }

    CreateInstructions();
    SetupViewport();

    Sample::InitMouseMode(MM_RELATIVE);

    CreateMenuBar();
    CreateMinimap();

    // AuthServer network panel (top-right) — shows connected status
    {
        auto* cache = GetSubsystem<ResourceCache>();
        auto* ui = GetSubsystem<UI>();
        Font* font = font_;

        auto* panel = ui->GetRoot()->CreateChild<Window>();
        panel->SetStyle("Window");
        panel->SetLayout(LM_VERTICAL, 6, IntRect(8, 8, 8, 8));
        panel->SetHorizontalAlignment(HA_RIGHT);
        panel->SetVerticalAlignment(VA_TOP);
        panel->SetPosition(-8, 30);
        panel->SetMinWidth(240);
        panel->SetColor(Color(0.15f, 0.15f, 0.2f, 0.9f));

        auto* title = panel->CreateChild<Text>();
        title->SetFont(font, 16);
        title->SetText("Network");
        title->SetColor(Color(0.8f, 0.8f, 1.0f));

        // Logged-in user label
        auto* userLabel = panel->CreateChild<Text>();
        userLabel->SetFont(font, 13);
        userLabel->SetText("User: " + loggedInUsername_);
        userLabel->SetColor(Color(0.3f, 1.0f, 0.3f));

        authConnectBtn_ = panel->CreateChild<Button>();
        authConnectBtn_->SetStyle("Button");
        authConnectBtn_->SetFixedHeight(32);
        authConnectBtn_->SetMinWidth(220);

        authBtnLabel_ = authConnectBtn_->CreateChild<Text>();
        authBtnLabel_->SetFont(font, 15);
        authBtnLabel_->SetHorizontalAlignment(HA_CENTER);
        authBtnLabel_->SetVerticalAlignment(VA_CENTER);

        networkStatusText_ = panel->CreateChild<Text>();
        networkStatusText_->SetFont(font, 14);
        if (authConnected_)
        {
            networkStatusText_->SetColor(Color(0.3f, 1.0f, 0.3f));
            networkStatusText_->SetText("Connected to " + authServerAddress_ + ":" + String(authServerPort_));
        }
        else
        {
            networkStatusText_->SetColor(Color(0.6f, 0.6f, 0.6f));
            networkStatusText_->SetText("Offline");
        }

        UpdateAuthButtonState();
        SubscribeToEvent(authConnectBtn_, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleAuthConnectButton));
    }

    // Position camera at owned patch center before spawning avatar
    // If initial position is underwater, search nearby for dry land
    if (cameraNode_)
    {
        const float patchWorldSize = 128.0f;
        float camX = (ownedPatchX_ + 0.5f) * patchWorldSize;
        float camZ = (ownedPatchZ_ + 0.5f) * patchWorldSize;
        float camY = 20.0f;
        if (terrain_)
        {
            float terrH = terrain_->GetHeight(Vector3(camX, 0.0f, camZ));
            if (terrH < 7.0f)  // Below or near water level
            {
                float bestH = terrH;
                for (float r = 16.0f; r <= 128.0f; r += 16.0f)
                {
                    for (int angle = 0; angle < 8; ++angle)
                    {
                        float a = angle * (M_PI / 4.0f);
                        float px = camX + r * cosf(a);
                        float pz = camZ + r * sinf(a);
                        float h = terrain_->GetHeight(Vector3(px, 0.0f, pz));
                        if (h > bestH)
                        {
                            bestH = h;
                            camX = px;
                            camZ = pz;
                        }
                    }
                    if (bestH > 12.0f)
                        break;
                }
                terrH = bestH;
            }
            camY = Max(terrH, 5.0f) + 10.0f;
        }
        cameraNode_->SetPosition(Vector3(camX, camY, camZ));
    }

    // Show owned patch boundaries (green)
    CreateOwnedPatchBoundaries();

    // Spawn avatar: standalone creates locally, connected waits for server replication
    auto* network = GetSubsystem<Network>();
    if (!network || !network->GetServerConnection())
    {
        Node* avatar = CreatePlayerAvatar();
        if (avatar)
        {
            characterNode_ = avatar;
            clientObjectID_ = avatar->GetID();
            cameraMode_ = CAM_CHASE;
        }
    }
    else
    {
        // Server creates and replicates our avatar — HandleClientObjectID will set characterNode_
        cameraMode_ = CAM_CHASE;
    }
}

// ============================================================================
// Return to Login — called when server connection is lost while in-world
// ============================================================================

void TerrainNode::ReturnToLogin()
{
    URHO3D_LOGINFO("Server connection lost — returning to login screen");

    // Wait for GPU to finish all in-flight work before destroying scene resources.
    // Without this, Vulkan images/buffers/descriptors are freed while still referenced
    // by in-flight command buffers, causing SEGV in descriptor pool cleanup.
#ifdef URHO3D_VULKAN
    {
        auto* graphics = GetSubsystem<Graphics>();
        if (graphics)
        {
            auto* impl = graphics->GetImpl_Vulkan();
            if (impl && impl->GetDevice())
                vkDeviceWaitIdle(impl->GetDevice());
        }
    }
#endif

    // --- Tear down world UI ---
    // Remove all UI children except the profiler (which persists across scenes)
    auto* ui = GetSubsystem<UI>();
    auto* root = ui->GetRoot();

    // Remove menu bar
    if (menuBar_)
    {
        menuBar_->Remove();
        menuBar_ = nullptr;
    }
    fileMenu_.Reset();
    editMenu_.Reset();
    viewMenu_.Reset();
    environmentMenu_ = nullptr;

    // Remove terrain panel
    terrainPanel_.Reset();

    // Remove hierarchy and inspector windows
    hierarchyWindow_.Reset();
    hierarchyList_ = nullptr;
    inspectorWindow_.Reset();
    inspectorScroll_ = nullptr;
    inspectorContent_ = nullptr;

    // Remove minimap
    if (minimap_)
    {
        minimap_->Remove();
        minimap_ = nullptr;
    }
    minimapCameraDot_ = nullptr;
    minimapTex_.Reset();
    minimapCameraNode_.Reset();

    // Remove instruction text
    if (instructionText_)
    {
        instructionText_->Remove();
        instructionText_ = nullptr;
    }

    // Remove file selector if open
    fileSelector_.Reset();

    // Remove network panel — authConnectBtn_ is a child of the panel Window
    if (authConnectBtn_)
    {
        // Walk up to the panel Window and remove it
        auto* panel = authConnectBtn_->GetParent();
        if (panel)
            panel->Remove();
        authConnectBtn_ = nullptr;
        authBtnLabel_ = nullptr;
        networkStatusText_ = nullptr;
    }

    // --- Reset world state ---
    loggedIn_ = false;
    loggedInUsername_.Clear();
    serverSceneName_.Clear();
    adminLevel_ = 0;

    // Reset cached pointers that refer to world scene nodes/components
    selectedNode_.Reset();
    originalMaterials_.Clear();
    sunNode_ = nullptr;
    moonNode_ = nullptr;
    sunLight_ = nullptr;
    moonLight_ = nullptr;
    sunMat_.Reset();
    moonMat_.Reset();
    skyboxMat_.Reset();
    lastSeasonIndex_ = -1;
    waterNode_.Reset();
    reflectionCameraNode_.Reset();
    zone_.Reset();
    terrain_.Reset();
    editableHeightMap_.Reset();
    renderPath_ = nullptr;
    reflectionRenderPath_ = nullptr;
    prefabBrush_.Reset();
    oofos_.Clear();
    oofosSpawned_ = 0;
    cameraNode_.Reset();

    // Reset undo/redo
    undoStack_.Clear();
    undoCursor_ = 0;
    terrainStrokeActive_ = false;
    terrainStrokeBefore_.Reset();

    // Reset brush state
    brushMode_ = 0;
    hasBrushHit_ = false;
    activeModeBtn_ = nullptr;
    activeShapeBtn_ = nullptr;

    // --- Rebuild login scene ---
    CreateLoginScene();
    SetupViewport();
    CreateLoginUI();

    Sample::InitMouseMode(MM_FREE);

    // Resume LAN discovery so we can reconnect
    authDiscovering_ = true;
    discoveryTimer_ = 0.0f;
}

// ============================================================================
// Scene
// ============================================================================

void TerrainNode::CreateScene()
{
    // Full procedural scene creation — matching proven Sample 23 (Water) pattern.
    // Everything built synchronously, no async XML loading.

    auto* cache = GetSubsystem<ResourceCache>();

    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>();
    scene_->CreateComponent<PhysicsWorld>();
    scene_->CreateComponent<DebugRenderer>();

    // Zone
    Node* zoneNode = scene_->CreateChild("Zone");
    auto* zone = zoneNode->CreateComponent<Zone>();
    zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
    zone->SetAmbientColor(Color(0.15f, 0.15f, 0.15f));
    zone->SetFogColor(Color(0.7f, 0.7f, 0.75f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);
    float fogMinHeight = 5.0f;
    float fogMaxHeight = 18.0f;
    zone->SetFogHeight(fogMinHeight);
    zone->SetFogHeightScale(1.0f / Max(fogMaxHeight - fogMinHeight, M_EPSILON));
    zone_ = zone;
    origFogColor_ = zone->GetFogColor();
    origFogStart_ = zone->GetFogStart();
    origFogEnd_ = zone->GetFogEnd();

    // Directional light (sun)
    Node* lightNode = scene_->CreateChild("DirectionalLight", LOCAL);
    lightNode->SetDirection(Vector3(0.6f, -1.0f, 0.8f));
    sunLight_ = lightNode->CreateComponent<Light>();
    sunLight_->SetLightType(LIGHT_DIRECTIONAL);
    sunLight_->SetCastShadows(true);
    sunLight_->SetShadowBias(BiasParameters(0.00025f, 0.5f));
    sunLight_->SetShadowCascade(CascadeParameters(10.0f, 50.0f, 200.0f, 0.0f, 0.8f));
    sunLight_->SetSpecularIntensity(0.5f);
    sunLight_->SetColor(Color(1.2f, 1.2f, 1.2f));

    // Skybox
    Node* skyNode = scene_->CreateChild("Sky");
    skyNode->SetScale(500.0f);
    auto* skybox = skyNode->CreateComponent<Skybox>();
    skybox->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
    skyboxMat_ = cache->GetResource<Material>("Materials/Skybox.xml");
    skybox->SetMaterial(skyboxMat_);

    // Preload seasonal skybox cubemaps
    seasonSkyboxes_[0] = cache->GetResource<TextureCube>("Textures/SkyboxSpring.xml");
    seasonSkyboxes_[1] = cache->GetResource<TextureCube>("Textures/SkyboxSummer.xml");
    seasonSkyboxes_[2] = cache->GetResource<TextureCube>("Textures/SkyboxAutumn.xml");
    seasonSkyboxes_[3] = cache->GetResource<TextureCube>("Textures/SkyboxWinter.xml");

    // Terrain
    Node* terrainNode = scene_->CreateChild("Terrain");
    terrainNode->SetPosition(Vector3(0.0f, -20.0f, 0.0f));
    auto* terrain = terrainNode->CreateComponent<Terrain>();
    terrain->SetPatchSize(64);
    terrain->SetSpacing(Vector3(2.0f, 0.5f, 2.0f));
    terrain->SetSmoothing(false);
    terrain->SetHeightMap(cache->GetResource<Image>("Textures/HeightMap.png"));
    terrain->SetMaterial(cache->GetResource<Material>("Materials/Terrain.xml"));
    terrain->SetOccluder(true);
    terrain_ = terrain;

    auto* terrainBody = terrainNode->CreateComponent<RigidBody>();
    terrainBody->SetCollisionLayer(2);
    terrainBody->SetFriction(0.75f);
    auto* terrainShape = terrainNode->CreateComponent<CollisionShape>();
    terrainShape->SetTerrain();

    // 32-bit editable heightmap (RGBA — R + G/256 + B/65536 + A/16M)
    {
        Image* origHM = terrain->GetHeightMap();
        int w = origHM->GetWidth();
        int h = origHM->GetHeight();
        int origComps = origHM->GetComponents();
        editableHeightMap_ = new Image(context_);
        editableHeightMap_->SetSize(w, h, 4);
        editableHeightMap_->SetName(origHM->GetName());
        unsigned char* src = origHM->GetData();
        unsigned char* dst = editableHeightMap_->GetData();
        for (int i = 0; i < w * h; ++i)
        {
            dst[i * 4] = src[i * origComps];
            dst[i * 4 + 1] = (origComps >= 2) ? src[i * origComps + 1] : 0;
            dst[i * 4 + 2] = (origComps >= 3) ? src[i * origComps + 2] : 0;
            dst[i * 4 + 3] = (origComps >= 4) ? src[i * origComps + 3] : 0;
        }
        terrain->SetHeightMap(editableHeightMap_);

        // Create water map (same resolution, single-channel, zeroed)
        waterMap_ = new Image(context_);
        waterMap_->SetSize(w, h, 1);
        memset(waterMap_->GetData(), 0, w * h);
    }

    // Create GPU texture from water map and bind to terrain material unit 4
    waterMapTex_ = new Texture2D(context_);
    waterMapTex_->SetFilterMode(FILTER_BILINEAR);
    waterMapTex_->SetAddressMode(COORD_U, ADDRESS_CLAMP);
    waterMapTex_->SetAddressMode(COORD_V, ADDRESS_CLAMP);
    waterMapTex_->SetData(waterMap_);
    auto* terrainMat = terrain->GetMaterial();
    if (terrainMat)
    {
        terrainMat->SetTexture(TU_ENVIRONMENT, waterMapTex_);
        terrainMat->SetShaderParameter("WaterLevel", 5.0f);
        terrainMat->SetShaderParameter("TerrainSpacingY", terrain->GetSpacing().y_);
    }

    // Initialize shared brush
    brush_ = new TerrainBrush(context_);
    brush_->SetTerrain(terrain, editableHeightMap_);
    brush_->SetWaterMap(waterMap_);

    // 1000 boxes
    for (unsigned i = 0; i < 1000; ++i)
    {
        Node* objectNode = scene_->CreateChild("Box");
        Vector3 position(Random(2000.0f) - 1000.0f, 0.0f, Random(2000.0f) - 1000.0f);
        position.y_ = terrain->GetHeight(position) + 2.25f;
        objectNode->SetPosition(position);
        objectNode->SetRotation(Quaternion(Vector3(0.0f, 1.0f, 0.0f), terrain->GetNormal(position)));
        objectNode->SetScale(5.0f);
        auto* object = objectNode->CreateComponent<StaticModel>();
        object->SetModel(cache->GetResource<Model>("Models/Box.mdl"));
        object->SetMaterial(cache->GetResource<Material>("Materials/Stone.xml"));
        object->SetCastShadows(true);

        auto* body = objectNode->CreateComponent<RigidBody>();
        body->SetMass(0.0f);
        auto* shape = objectNode->CreateComponent<CollisionShape>();
        shape->SetBox(Vector3::ONE);
    }

    // Water plane
    waterNode_ = scene_->CreateChild("Water");
    waterNode_->SetScale(Vector3(2048.0f, 1.0f, 2048.0f));
    waterNode_->SetPosition(Vector3(0.0f, 5.0f, 0.0f));
    auto* water = waterNode_->CreateComponent<StaticModel>();
    water->SetModel(cache->GetResource<Model>("Models/Plane.mdl"));
    water->SetMaterial(cache->GetResource<Material>("Materials/Water.xml"));
    water->SetViewMask(0x80000000);

    // Initialize time from system clock (Melbourne AEDT = UTC+11)
    {
        time_t now = time(nullptr);
        struct tm* utc = gmtime(&now);
        int hour = utc->tm_hour + 11;
        int yday = utc->tm_yday + 1;
        if (hour >= 24) { hour -= 24; yday++; }
        dayOfYear_ = yday;
        baseDayOfYear_ = yday;
        timeOfDay_ = (float)hour + utc->tm_min / 60.0f + utc->tm_sec / 3600.0f;

        struct tm refMoon = {};
        refMoon.tm_year = 125;
        refMoon.tm_mon = 0;
        refMoon.tm_mday = 29;
        refMoon.tm_hour = 12;
        time_t refTime = mktime(&refMoon);
        double daysSinceNewMoon = difftime(now, refTime) / 86400.0;
        moonAge_ = (float)fmod(daysSinceNewMoon, 29.53);
        if (moonAge_ < 0.0f) moonAge_ += 29.53f;
        baseMoonAge_ = moonAge_;
    }

    // Command-line override: -time 10.5
    bool timeOverride = false;
    const Vector<String>& args = GetArguments();
    for (unsigned i = 0; i < args.Size(); ++i)
    {
        if ((args[i] == "-time" || args[i] == "time") && i + 1 < args.Size())
        {
            timeOfDay_ = ToFloat(args[i + 1]);
            if (timeOfDay_ < 0.0f) timeOfDay_ += 24.0f;
            if (timeOfDay_ >= 24.0f) timeOfDay_ -= 24.0f;
            timeOverride = true;
            break;
        }
    }

    if (!timeOverride)
        SyncMelbourneTime();
    timeSyncTimer_ = timeOverride ? 9999.0f : 300.0f;

    // Camera — must be created before entities so they can spawn near it
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(750.0f);
    cameraNode_->SetPosition(Vector3(64.0f, 60.0f, 64.0f));

    // Create entities
    CreateCelestialBodies();
    CreateOOFOs();
    CreateFish();
    CreateSchoolFish();
    CreateAnimals();
    CreateGrass();
    CreateRain();
    CreateCampfire();
}

// OnSceneLoaded() — DEAD CODE, preserved for future network scene loading reintegration.
// When network play is re-enabled, this will be called after LoadAsyncXML finishes
// to rebind cached pointers and create dynamic entities.
void TerrainNode::OnSceneLoaded()
{
    URHO3D_LOGWARNING("OnSceneLoaded() called — this path is currently unused (procedural scene creation active)");
}

void TerrainNode::CreateInstructions()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* ui = GetSubsystem<UI>();

    instructionText_ = ui->GetRoot()->CreateChild<Text>();
    instructionText_->SetText(
        "WASD move, Mouse look, Tab cursor mode\n"
        "LMB raise, RMB lower, Scroll brush size\n"
        "F5 debug, F fill, H fog, F11 fullscreen\n"
        "NumPad Enter hide UI, 1 sun, 2 moon");
    instructionText_->SetFont(font_, 15);
    instructionText_->SetTextAlignment(HA_CENTER);
    instructionText_->SetHorizontalAlignment(HA_CENTER);
    instructionText_->SetVerticalAlignment(VA_CENTER);
    instructionText_->SetPosition(0, ui->GetRoot()->GetHeight() / 4);

    // Camera coordinates — lower left
    cameraCoordsText_ = ui->GetRoot()->CreateChild<Text>();
    cameraCoordsText_->SetFont(font_, 13);
    cameraCoordsText_->SetColor(Color(0.8f, 0.8f, 0.6f));
    cameraCoordsText_->SetHorizontalAlignment(HA_LEFT);
    cameraCoordsText_->SetVerticalAlignment(VA_BOTTOM);
    cameraCoordsText_->SetPosition(8, -8);

    // Melbourne clock — lower right
    clockText_ = ui->GetRoot()->CreateChild<Text>();
    clockText_->SetFont(font_, 13);
    clockText_->SetColor(Color(0.9f, 0.9f, 0.8f));
    clockText_->SetHorizontalAlignment(HA_RIGHT);
    clockText_->SetVerticalAlignment(VA_BOTTOM);
    clockText_->SetPosition(-8, -8);
}

// ============================================================================
// Viewport + Water Reflection
// ============================================================================

void TerrainNode::SetupViewport()
{
    auto* graphics = GetSubsystem<Graphics>();
    auto* renderer = GetSubsystem<Renderer>();
    auto* cache = GetSubsystem<ResourceCache>();

    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    viewport->SetDrawDebug(true);
    renderer->SetViewport(0, viewport);

    SharedPtr<RenderPath> renderPath(new RenderPath());
    renderPath->Load(cache->GetResource<XMLFile>("RenderPaths/ForwardHWDepthOutline.xml"));
    viewport->SetRenderPath(renderPath);

    RenderPath* rp = viewport->GetRenderPath();

    // Water-dependent setup — only when the full world is loaded
    if (waterNode_)
    {
        rp->Append(cache->GetResource<XMLFile>("PostProcess/Underwater.xml"));
        rp->SetShaderParameter("WaterLevel", waterNode_->GetWorldPosition().y_);
        rp->SetShaderParameter("NoiseStrength", 0.015f);
        rp->SetShaderParameter("NoiseTiling", 1.0f);
        rp->SetShaderParameter("NoiseSpeed", Vector2(0.05f, 0.05f));
        rp->SetShaderParameter("UnderwaterColor", Vector3(0.0f, 0.2f, 0.3f));
        rp->SetShaderParameter("TintIntensity", 0.3f);
        rp->SetShaderParameter("DepthFalloff", 0.02f);
        rp->SetShaderParameter("BreachDepth", 2.0f);
        rp->SetShaderParameter("MainCameraY", cameraNode_->GetWorldPosition().y_);
        rp->SetEnabled("Underwater", true);

        rp->Append(cache->GetResource<XMLFile>("PostProcess/WaterDroplets.xml"));
        rp->SetEnabled("WaterDroplets", false);

        waterPlane_ = Plane(waterNode_->GetWorldRotation() * Vector3(0.0f, 1.0f, 0.0f), waterNode_->GetWorldPosition());
        waterClipPlane_ = Plane(waterNode_->GetWorldRotation() * Vector3(0.0f, 1.0f, 0.0f), waterNode_->GetWorldPosition());

        reflectionCameraNode_ = cameraNode_->CreateChild();
        auto* reflectionCamera = reflectionCameraNode_->CreateComponent<Camera>();
        reflectionCamera->SetFarClip(750.0);
        reflectionCamera->SetViewMask(0x7fffffff);
        reflectionCamera->SetAutoAspectRatio(false);
        reflectionCamera->SetUseReflection(true);
        reflectionCamera->SetReflectionPlane(waterPlane_);
        reflectionCamera->SetUseClipping(true);
        reflectionCamera->SetClipPlane(waterClipPlane_);
        reflectionCamera->SetAspectRatio((float)graphics->GetWidth() / (float)graphics->GetHeight());

        int texSize = 1024;
        SharedPtr<Texture2D> renderTexture(new Texture2D(context_));
        renderTexture->SetSize(texSize, texSize, Graphics::GetRGBFormat(), TEXTURE_RENDERTARGET);
        renderTexture->SetFilterMode(FILTER_BILINEAR);
        RenderSurface* surface = renderTexture->GetRenderSurface();
        SharedPtr<Viewport> rttViewport(new Viewport(context_, scene_, reflectionCamera));
        rttViewport->SetDrawDebug(false);
        surface->SetViewport(0, rttViewport);
        auto* waterMat = cache->GetResource<Material>("Materials/Water.xml");
        waterMat->SetTexture(TU_DIFFUSE, renderTexture);
    }

    rp->Append(cache->GetResource<XMLFile>("PostProcess/GodRays.xml"));
    rp->Append(cache->GetResource<XMLFile>("PostProcess/MoonRays.xml"));
    rp->SetEnabled("MoonRays", false);
    renderPath_ = rp;
}

void TerrainNode::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(TerrainNode, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(TerrainNode, HandlePostRenderUpdate));

    // AuthServer discovery & connection
    SubscribeToEvent(E_NETWORKHOSTDISCOVERED, URHO3D_HANDLER(TerrainNode, HandleHostDiscovered));
    SubscribeToEvent(E_SERVERCONNECTED, URHO3D_HANDLER(TerrainNode, HandleServerConnected));
    SubscribeToEvent(E_SERVERDISCONNECTED, URHO3D_HANDLER(TerrainNode, HandleServerDisconnected));
    SubscribeToEvent(E_CONNECTFAILED, URHO3D_HANDLER(TerrainNode, HandleConnectFailed));
    SubscribeToEvent(E_NETWORKMESSAGE, URHO3D_HANDLER(TerrainNode, HandleAuthMessage));

    // Peer offload (NAT punchthrough)
    SubscribeToEvent(E_PEERCONNECTED, URHO3D_HANDLER(TerrainNode, HandlePeerConnected));
    SubscribeToEvent(E_PEERDISCONNECTED, URHO3D_HANDLER(TerrainNode, HandlePeerDisconnected));
    SubscribeToEvent(E_NETWORKNATPUNCHTROUGHFAILED, URHO3D_HANDLER(TerrainNode, HandleNATPunchFailed));

    // Player avatar network events
    SubscribeToEvent(E_PHYSICSPRESTEP, URHO3D_HANDLER(TerrainNode, HandlePhysicsPreStep));
    SubscribeToEvent(E_CLIENTCONNECTED, URHO3D_HANDLER(TerrainNode, HandleClientConnected));
    SubscribeToEvent(E_CLIENTDISCONNECTED, URHO3D_HANDLER(TerrainNode, HandleClientDisconnected));
    SubscribeToEvent(E_CLIENTOBJECTID, URHO3D_HANDLER(TerrainNode, HandleClientObjectID));
    GetSubsystem<Network>()->RegisterRemoteEvent(E_CLIENTOBJECTID);

    // Debug log capture
    SubscribeToEvent(E_LOGMESSAGE, URHO3D_HANDLER(TerrainNode, HandleLogMessage));

    // Start LAN discovery immediately
    DiscoverAuthServer();
}

// ============================================================================
// Menu Bar
// ============================================================================

DropDownList* TerrainNode::CreateMenuDropdown(const String& label, const Vector<String>& items)
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    auto* dd = menuBar_->CreateChild<DropDownList>();
    dd->SetStyleAuto();
    dd->SetResizePopup(true);
    dd->SetFixedHeight(24);
    dd->SetMinWidth(70);

    auto* placeholder = dd->GetPlaceholder();
    if (placeholder)
    {
        auto* phText = placeholder->CreateChild<Text>();
        phText->SetFont(font, 12);
        phText->SetText(label);
        phText->SetColor(Color::WHITE);
    }

    for (unsigned i = 0; i < items.Size(); ++i)
    {
        auto* text = new Text(context_);
        text->SetText(items[i]);
        text->SetFont(font, 12);
        text->SetMinWidth(140);
        text->SetMinHeight(22);
        text->SetStyleAuto();
        text->SetColor(Color(0.9f, 0.9f, 0.9f));
        text->SetHoverColor(Color(1.0f, 1.0f, 0.5f));
        dd->AddItem(text);
    }

    dd->SetSelection(M_MAX_UNSIGNED);
    return dd;
}

void TerrainNode::CreateMenuBar()
{
    auto* ui = GetSubsystem<UI>();
    auto* uiRoot = ui->GetRoot();

    menuBar_ = uiRoot->CreateChild<BorderImage>("MenuBar");
    menuBar_->SetStyle("Window");
    menuBar_->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
    menuBar_->SetFixedHeight(28);
    menuBar_->SetMinWidth(uiRoot->GetWidth());
    menuBar_->SetHorizontalAlignment(HA_LEFT);
    menuBar_->SetVerticalAlignment(VA_TOP);
    menuBar_->SetOpacity(0.9f);

    // File (merged with former Create menu)
    {
        Vector<String> items;
        items.Push("Save Scene");           // 0
        items.Push("Load Scene");           // 1
        items.Push("Import Model...");      // 2
        items.Push("Export Prefab");        // 3
        items.Push("From Prefab...");       // 4
        items.Push("Clear Object Brush");   // 5
        items.Push("Screenshot");           // 6
        items.Push("Exit");                 // 7
        fileMenu_ = CreateMenuDropdown("File", items);
        SubscribeToEvent(fileMenu_, E_ITEMSELECTED, URHO3D_HANDLER(TerrainNode, HandleFileMenu));
    }

    // Edit (unchanged)
    {
        Vector<String> items;
        items.Push("Undo (Ctrl+Z)");
        items.Push("Redo (Ctrl+Y)");
        items.Push("Translate (T)");
        items.Push("Rotate (R)");
        items.Push("Scale (S)");
        items.Push("World (G)");
        editMenu_ = CreateMenuDropdown("Edit", items);
        SubscribeToEvent(editMenu_, E_ITEMSELECTED, URHO3D_HANDLER(TerrainNode, HandleEditMenu));
    }

    // View (merged with former Overlay menu)
    {
        Vector<String> items;
        items.Push("Hierarchy");                        // 0
        items.Push("Inspector");                        // 1
        items.Push("Debug Log");                        // 2
        items.Push("Toggle Fullscreen  (F11)");         // 3
        items.Push("Toggle Wireframe  (F)");            // 4
        items.Push("Toggle Debug Geometry  (F5)");      // 5
        items.Push("Toggle Height Fog  (H)");           // 6
        items.Push("Toggle Profiler");                  // 7
        items.Push("Toggle OOFO Detector");             // 8
        items.Push("Toggle Land Animal Rays");           // 9
        items.Push("Toggle Water Animal Rays");          // 10
        items.Push("Toggle God Rays");                   // 11
        items.Push("Toggle Grass Rays");                 // 12
        viewMenu_ = CreateMenuDropdown("View", items);
        SubscribeToEvent(viewMenu_, E_ITEMSELECTED, URHO3D_HANDLER(TerrainNode, HandleViewMenu));
    }

    // Settings (font + size selection)
    {
        Vector<String> items;
        // Scan available fonts
        auto* fs = GetSubsystem<FileSystem>();
        String fontDir = fs->GetProgramDir() + "Data/Fonts/";
        Vector<String> fontFiles;
        fs->ScanDir(fontFiles, fontDir, "*.ttf", SCAN_FILES, false);
        availableFonts_.Clear();
        for (unsigned i = 0; i < fontFiles.Size(); ++i)
        {
            String name = fontFiles[i].Substring(0, fontFiles[i].FindLast('.'));
            availableFonts_.Push(name);
            items.Push("Font: " + name);
        }
        int sizes[] = {9, 10, 11, 12, 13, 14, 16, 18};
        for (int i = 0; i < 8; ++i)
            items.Push("Size: " + String(sizes[i]));

        settingsMenu_ = CreateMenuDropdown("Settings", items);
        SubscribeToEvent(settingsMenu_, E_ITEMSELECTED, URHO3D_HANDLER(TerrainNode, HandleSettingsMenu));
    }

    // Environment (custom popup with collapsible sections)
    {
        auto* cache = GetSubsystem<ResourceCache>();
        Font* font = font_;

        environmentMenu_ = menuBar_->CreateChild<Menu>();
        environmentMenu_->SetStyle("DropDownList");
        environmentMenu_->SetFixedHeight(24);
        environmentMenu_->SetMinWidth(90);

        auto* envLabel = environmentMenu_->CreateChild<Text>();
        envLabel->SetFont(font, 12);
        envLabel->SetText("Environment");
        envLabel->SetColor(Color::WHITE);

        auto* envPopup = new Window(context_);
        envPopup->SetStyleAuto();
        envPopup->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));
        envPopup->SetMinWidth(320);
        envPopup->SetDefaultStyle(GetSubsystem<UI>()->GetRoot()->GetDefaultStyle());
        envPopup->SetOpacity(0.85f);
        environmentMenu_->SetPopup(envPopup);
        environmentMenu_->SetPopupOffset(0, environmentMenu_->GetHeight());

        if (terrain_)
            CreateMenuItem(envPopup, "Terrain Tools", 104);

        // --- Time Scrub section (expanded by default) ---
        auto* timeScrubSection = CreateCollapsibleSection(envPopup, font, "Time Scrub", true);
        {
            // Time of day slider
            auto* todRow = timeScrubSection->CreateChild<UIElement>();
            todRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            todRow->SetMinHeight(22);

            auto* todText = todRow->CreateChild<Text>();
            todText->SetFont(font, 12);
            todText->SetText("Time:");
            todText->SetColor(Color(0.9f, 0.9f, 0.9f));
            todText->SetMinWidth(55);

            auto* todSlider = todRow->CreateChild<Slider>();
            todSlider->SetStyleAuto();
            todSlider->SetFixedHeight(16);
            todSlider->SetMinWidth(200);
            todSlider->SetRange(24.0f);
            todSlider->SetValue(12.0f);
            SubscribeToEvent(todSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleTimeOfDaySlider));

            todLabel_ = todRow->CreateChild<Text>();
            todLabel_->SetFont(font, 12);
            todLabel_->SetText("+0:00");
            todLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            todLabel_->SetMinWidth(55);

            // Date offset sliders
            auto createDateSlider = [&](const String& label, float range, Text*& outLabel, int sliderId) {
                auto* row = timeScrubSection->CreateChild<UIElement>();
                row->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
                row->SetMinHeight(22);

                auto* lbl = row->CreateChild<Text>();
                lbl->SetFont(font, 12);
                lbl->SetText(label);
                lbl->SetColor(Color(0.9f, 0.9f, 0.9f));
                lbl->SetMinWidth(55);

                auto* slider = row->CreateChild<Slider>();
                slider->SetStyleAuto();
                slider->SetFixedHeight(16);
                slider->SetMinWidth(200);
                slider->SetRange(range);
                slider->SetValue(range * 0.5f);
                slider->SetVar("SliderID", sliderId);
                SubscribeToEvent(slider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleDateOffsetSlider));

                outLabel = row->CreateChild<Text>();
                outLabel->SetFont(font, 12);
                outLabel->SetText("0");
                outLabel->SetColor(Color(0.9f, 0.9f, 0.9f));
                outLabel->SetMinWidth(55);
            };

            createDateSlider("Days:", 30.0f, dayOffsetLabel_, 20);
            createDateSlider("Months:", 12.0f, monthOffsetLabel_, 21);
            createDateSlider("Years:", 30.0f, yearOffsetLabel_, 22);
        }

        // --- Weather section (collapsed by default) ---
        auto* weatherSection = CreateCollapsibleSection(envPopup, font, "Weather", false);
        {
            // Cloud cover slider
            auto* cloudRow = weatherSection->CreateChild<UIElement>();
            cloudRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            cloudRow->SetMinHeight(22);

            auto* cloudText = cloudRow->CreateChild<Text>();
            cloudText->SetFont(font, 12);
            cloudText->SetText("Clouds:");
            cloudText->SetColor(Color(0.9f, 0.9f, 0.9f));
            cloudText->SetMinWidth(55);

            auto* cloudSlider = cloudRow->CreateChild<Slider>();
            cloudSlider->SetStyleAuto();
            cloudSlider->SetFixedHeight(16);
            cloudSlider->SetMinWidth(200);
            cloudSlider->SetRange(1.0f);
            cloudSlider->SetValue(0.0f);
            cloudSlider->SetVar("SliderID", 30);
            SubscribeToEvent(cloudSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleWeatherSlider));

            cloudCoverLabel_ = cloudRow->CreateChild<Text>();
            cloudCoverLabel_->SetFont(font, 12);
            cloudCoverLabel_->SetText("Auto");
            cloudCoverLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            cloudCoverLabel_->SetMinWidth(50);

            // Precipitation slider
            auto* precipRow = weatherSection->CreateChild<UIElement>();
            precipRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            precipRow->SetMinHeight(22);

            auto* precipText = precipRow->CreateChild<Text>();
            precipText->SetFont(font, 12);
            precipText->SetText("Precip:");
            precipText->SetColor(Color(0.9f, 0.9f, 0.9f));
            precipText->SetMinWidth(55);

            auto* precipSlider = precipRow->CreateChild<Slider>();
            precipSlider->SetStyleAuto();
            precipSlider->SetFixedHeight(16);
            precipSlider->SetMinWidth(200);
            precipSlider->SetRange(1.0f);
            precipSlider->SetValue(0.0f);
            precipSlider->SetVar("SliderID", 31);
            SubscribeToEvent(precipSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleWeatherSlider));

            precipLabel_ = precipRow->CreateChild<Text>();
            precipLabel_->SetFont(font, 12);
            precipLabel_->SetText("Auto");
            precipLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            precipLabel_->SetMinWidth(50);

            // Rain intensity slider (override)
            auto* rainRow = weatherSection->CreateChild<UIElement>();
            rainRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            rainRow->SetMinHeight(22);

            auto* rainText = rainRow->CreateChild<Text>();
            rainText->SetFont(font, 12);
            rainText->SetText("Rain:");
            rainText->SetColor(Color(0.9f, 0.9f, 0.9f));
            rainText->SetMinWidth(55);

            auto* rainSlider = rainRow->CreateChild<Slider>();
            rainSlider->SetStyleAuto();
            rainSlider->SetFixedHeight(16);
            rainSlider->SetMinWidth(200);
            rainSlider->SetRange(1.0f);
            rainSlider->SetValue(0.0f);
            rainSlider->SetVar("SliderID", 32);
            SubscribeToEvent(rainSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleWeatherSlider));

            rainLabel_ = rainRow->CreateChild<Text>();
            rainLabel_->SetFont(font, 12);
            rainLabel_->SetText("Auto");
            rainLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            rainLabel_->SetMinWidth(50);
        }

        // --- Fish section (collapsed by default) ---
        auto* fishSection = CreateCollapsibleSection(envPopup, font, "Fish", false);
        {
            // Wiggle amplitude
            auto* fishAmpRow = fishSection->CreateChild<UIElement>();
            fishAmpRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            fishAmpRow->SetMinHeight(22);

            auto* fishAmpText = fishAmpRow->CreateChild<Text>();
            fishAmpText->SetFont(font, 12);
            fishAmpText->SetText("Wiggle:");
            fishAmpText->SetColor(Color(0.9f, 0.9f, 0.9f));
            fishAmpText->SetMinWidth(55);

            auto* fishAmpSlider = fishAmpRow->CreateChild<Slider>();
            fishAmpSlider->SetStyleAuto();
            fishAmpSlider->SetFixedHeight(16);
            fishAmpSlider->SetMinWidth(200);
            fishAmpSlider->SetRange(1.0f);
            fishAmpSlider->SetValue(0.3f);
            SubscribeToEvent(fishAmpSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleFishWiggleSlider));

            fishWiggleLabel_ = fishAmpRow->CreateChild<Text>();
            fishWiggleLabel_->SetFont(font, 12);
            fishWiggleLabel_->SetText("0.030");
            fishWiggleLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            fishWiggleLabel_->SetMinWidth(50);

            // Speed
            auto* fishSpdRow = fishSection->CreateChild<UIElement>();
            fishSpdRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
            fishSpdRow->SetMinHeight(22);

            auto* fishSpdText = fishSpdRow->CreateChild<Text>();
            fishSpdText->SetFont(font, 12);
            fishSpdText->SetText("Speed:");
            fishSpdText->SetColor(Color(0.9f, 0.9f, 0.9f));
            fishSpdText->SetMinWidth(55);

            auto* fishSpdSlider = fishSpdRow->CreateChild<Slider>();
            fishSpdSlider->SetStyleAuto();
            fishSpdSlider->SetFixedHeight(16);
            fishSpdSlider->SetMinWidth(200);
            fishSpdSlider->SetRange(1.0f);
            fishSpdSlider->SetValue(0.25f);
            SubscribeToEvent(fishSpdSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleFishSpeedSlider));

            fishSpeedLabel_ = fishSpdRow->CreateChild<Text>();
            fishSpeedLabel_->SetFont(font, 12);
            fishSpeedLabel_->SetText("2.0 Hz");
            fishSpeedLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
            fishSpeedLabel_->SetMinWidth(50);
        }
    }
}

static const char* DEFAULT_INSTRUCTIONS =
    "WASD move, Mouse look, Tab cursor mode\n"
    "LMB raise, RMB lower, Scroll brush size\n"
    "F5 debug, F fill, H fog, F11 fullscreen\n"
    "NumPad Enter hide UI, 1 sun, 2 moon";

void TerrainNode::HandleFileMenu(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemSelected;
    int sel = eventData[P_SELECTION].GetI32();

    if (sel < 0 || sel >= (int)fileMenu_->GetNumItems())
        return;

    if (instructionText_)
        instructionText_->SetText(
            "Save/Load Scene, Import Model, Export Prefab\n"
            "Screenshot saves to ~/Documents/Urho3D/Screenshots/");

    switch (sel)
    {
    case 0: ShowSaveSceneDialog(); break;
    case 1: ShowLoadSceneDialog(); break;
    case 2: ShowImportModelDialog(); break;
    case 3: ShowExportPrefabDialog(); break;
    case 4: ShowLoadPrefabDialog(); break;
    case 5:
        if (prefabBrush_)
        {
            prefabBrush_->Remove();
            prefabBrush_.Reset();
        }
        UpdatePrefabBrushLabel();
        break;
    case 6:
    {
        auto* graphics = GetSubsystem<Graphics>();
        Image screenshot(context_);
        if (graphics->TakeScreenShot(screenshot))
        {
            auto* fs = GetSubsystem<FileSystem>();
            String dir = fs->GetUserDocumentsDir() + "Urho3D/Screenshots/";
            fs->CreateDir(dir);
            String filename = dir + "screenshot_" + String(Time::GetTimeSinceEpoch()) + ".png";
            screenshot.SavePNG(filename);
            URHO3D_LOGINFO("Screenshot saved: " + filename);
        }
        break;
    }
    case 7: engine_->Exit(); break;
    }

    fileMenu_->SetSelection(M_MAX_UNSIGNED);
}

void TerrainNode::HandleEditMenu(StringHash eventType, VariantMap& eventData)
{
    unsigned sel = eventData[ItemSelected::P_SELECTION].GetU32();
    editMenu_->SetSelection(M_MAX_UNSIGNED);

    if (sel >= editMenu_->GetNumItems())
        return;
    if (instructionText_)
        instructionText_->SetText(
            "T translate, R rotate, S scale, G toggle local/world\n"
            "Ctrl+Z undo, Ctrl+Y redo, Backspace delete");

    switch (sel)
    {
    case 0: Undo(); break;
    case 1: Redo(); break;
    case 2: gizmoMode_ = 1; break;  // Translate
    case 3: gizmoMode_ = 2; break;  // Rotate
    case 4: gizmoMode_ = 3; break;  // Scale
    case 5:
        gizmoLocal_ = !gizmoLocal_;
        // Update menu item text to reflect current state
        if (editMenu_->GetNumItems() > 5)
        {
            auto* item = editMenu_->GetItem(5);
            auto* label = item ? item->GetChildStaticCast<Text>(0) : nullptr;
            if (label)
                label->SetText(gizmoLocal_ ? "Local (G)" : "World (G)");
        }
        break;
    }
}

void TerrainNode::HandleViewMenu(StringHash eventType, VariantMap& eventData)
{
    unsigned sel = eventData[ItemSelected::P_SELECTION].GetU32();
    viewMenu_->SetSelection(M_MAX_UNSIGNED);

    if (sel >= viewMenu_->GetNumItems())
        return;

    if (sel <= 2)
    {
        if (instructionText_)
            instructionText_->SetText(
                "Hierarchy: scene node tree, double-click to focus\n"
                "Inspector: edit selected node properties");
    }
    else
    {
        if (instructionText_)
            instructionText_->SetText(
                "F5 debug, F fill mode, H height fog\n"
                "F11 fullscreen, NumPad Enter hide all");
    }

    switch (sel)
    {
    case 0: ToggleHierarchyWindow(); break;
    case 1: ToggleInspectorWindow(); break;
    case 2: ToggleDebugLogWindow(); break;
    case 3: // Toggle Fullscreen
        GetSubsystem<Graphics>()->ToggleFullscreen();
        break;
    case 4: // Toggle Wireframe
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
        break;
    }
    case 5: // Toggle Debug Geometry
        drawDebug_ = !drawDebug_;
        break;
    case 6: // Toggle Height Fog
        if (zone_)
        {
            bool on = !zone_->GetHeightFog();
            zone_->SetHeightFog(on);
            heightFogOverride_ = on ? 1 : -1;
        }
        break;
    case 7: // Toggle Profiler
        if (profilerUI_)
            profilerUI_->SetVisible(!profilerUI_->IsVisible());
        break;
    case 8: // Toggle OOFO Detector
        oofoRayVisible_ = !oofoRayVisible_;
        break;
    case 9: // Toggle Land Animal Rays
        landAnimalRayVisible_ = !landAnimalRayVisible_;
        URHO3D_LOGINFOF("Land animal rays: %s", landAnimalRayVisible_ ? "ON" : "OFF");
        break;
    case 10: // Toggle Water Animal Rays
        waterAnimalRayVisible_ = !waterAnimalRayVisible_;
        URHO3D_LOGINFOF("Water animal rays: %s", waterAnimalRayVisible_ ? "ON" : "OFF");
        break;
    case 11: // Toggle God Rays
        godRaysEnabled_ = !godRaysEnabled_;
        break;
    case 12: // Toggle Grass Rays
        grassRayVisible_ = !grassRayVisible_;
        URHO3D_LOGINFOF("Grass rays: %s", grassRayVisible_ ? "ON" : "OFF");
        break;
    }
}

Menu* TerrainNode::CreateMenuItem(UIElement* parent, const String& text, int actionId)
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    auto* item = parent->CreateChild<Menu>();
    item->SetStyleAuto();
    item->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));

    auto* label = item->CreateChild<Text>();
    label->SetFont(font, 12);
    label->SetText(text);
    label->SetColor(Color(0.9f, 0.9f, 0.9f));

    item->SetMinHeight(22);

    item->SetVar("MenuAction", actionId);
    SubscribeToEvent(item, E_MENUSELECTED, URHO3D_HANDLER(TerrainNode, HandleEnvironmentAction));
    return item;
}

UIElement* TerrainNode::CreateCollapsibleSection(UIElement* parent, Font* font, const String& title, bool expanded)
{
    // Header button — toggle arrow prefix
    auto* header = parent->CreateChild<Button>();
    header->SetStyle("Button");
    header->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
    header->SetMinHeight(22);
    header->SetMinWidth(300);

    auto* headerText = header->CreateChild<Text>();
    headerText->SetFont(font, 12);
    headerText->SetText(String(expanded ? "v " : "> ") + title);
    headerText->SetColor(Color(0.95f, 0.95f, 0.8f));

    // Content container
    auto* content = parent->CreateChild<UIElement>();
    content->SetLayout(LM_VERTICAL, 2, IntRect(8, 2, 4, 2));
    content->SetVisible(expanded);

    // Store content element pointer as a var on the header button
    header->SetVar("SectionContent", content);
    header->SetVar("SectionTitle", title);
    SubscribeToEvent(header, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleCollapsibleToggle));

    return content;
}

void TerrainNode::HandleCollapsibleToggle(StringHash eventType, VariantMap& eventData)
{
    using namespace Released;
    auto* button = static_cast<Button*>(eventData[P_ELEMENT].GetPtr());
    if (!button) return;

    auto* content = static_cast<UIElement*>(button->GetVar("SectionContent").GetPtr());
    if (!content) return;

    bool nowVisible = !content->IsVisible();
    content->SetVisible(nowVisible);

    // Update header text arrow
    String title = button->GetVar("SectionTitle").GetString();
    auto* headerText = button->GetChildStaticCast<Text>(0);
    if (headerText)
        headerText->SetText(String(nowVisible ? "v " : "> ") + title);

    // Shrink-to-fit: reset parent window height so layout recalculates
    auto* popup = button->GetParent();
    if (popup)
    {
        popup->SetHeight(0);
        popup->UpdateLayout();
    }
}

void TerrainNode::HandleEnvironmentAction(StringHash eventType, VariantMap& eventData)
{
    using namespace MenuSelected;
    auto* element = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr());
    if (!element) return;

    int action = element->GetVar("MenuAction").GetI32();

    switch (action)
    {
    case 104:
        ToggleTerrainPanel();
        break;
    }

    // Close the top-level popup
    if (environmentMenu_ && environmentMenu_->GetShowPopup())
        environmentMenu_->ShowPopup(false);
}

void TerrainNode::HandleTimeOfDaySlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    float val = eventData[P_VALUE].GetFloat();  // 0..24
    timeOfDayOffset_ = val - 12.0f;  // -12..+12

    // Update label
    if (todLabel_)
    {
        int hours = (int)timeOfDayOffset_;
        int mins = (int)(Abs(timeOfDayOffset_ - hours) * 60.0f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%+d:%02d", hours, mins);
        todLabel_->SetText(buf);
    }
}

void TerrainNode::HandleDateOffsetSlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    auto* slider = static_cast<Slider*>(eventData[P_ELEMENT].GetPtr());
    float val = eventData[P_VALUE].GetFloat();
    int id = slider->GetVar("SliderID").GetI32();

    if (id == 20)  // ±15 days
    {
        daySliderOffset_ = val - 15.0f;
        if (dayOffsetLabel_)
        {
            char buf[16]; snprintf(buf, sizeof(buf), "%+.0f d", daySliderOffset_);
            dayOffsetLabel_->SetText(buf);
        }
    }
    else if (id == 21)  // ±6 months (in days)
    {
        monthSliderOffset_ = (val - 6.0f) * 30.44f;  // ~30.44 days/month
        if (monthOffsetLabel_)
        {
            float months = val - 6.0f;
            char buf[16]; snprintf(buf, sizeof(buf), "%+.0f mo", months);
            monthOffsetLabel_->SetText(buf);
        }
    }
    else if (id == 22)  // ±15 years (in days)
    {
        yearSliderOffset_ = (val - 15.0f) * 365.25f;
        if (yearOffsetLabel_)
        {
            float years = val - 15.0f;
            char buf[16]; snprintf(buf, sizeof(buf), "%+.0f yr", years);
            yearOffsetLabel_->SetText(buf);
        }
    }

    ApplyDateOffsets();
}

void TerrainNode::ApplyDateOffsets()
{
    float totalDayOffset = daySliderOffset_ + monthSliderOffset_ + yearSliderOffset_;
    int newDay = baseDayOfYear_ + (int)totalDayOffset;
    // Wrap to 1-365
    newDay = ((newDay - 1) % 365 + 365) % 365 + 1;
    dayOfYear_ = newDay;

    // Moon age cycles every 29.53 days
    moonAge_ = baseMoonAge_ + totalDayOffset;
    moonAge_ = fmodf(fmodf(moonAge_, 29.53f) + 29.53f, 29.53f);

    // Force atmosphere update
    UpdateAtmosphere(CalculateSunAltitude());
}

void TerrainNode::HandleFishWiggleSlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    float val = eventData[P_VALUE].GetFloat();  // 0..1
    float amplitude = val * 0.1f;  // 0..0.1 world units

    // Update all fish materials
    const Vector<SharedPtr<Node>>& children = scene_->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
    {
        if (children[i]->GetName() == "Fish")
        {
            auto* sm = children[i]->GetComponent<StaticModel>();
            if (sm && sm->GetMaterial())
            {
                sm->GetMaterial()->SetShaderParameter("WiggleAmplitude", amplitude);
            }
        }
    }

    if (fishWiggleLabel_)
        fishWiggleLabel_->SetText(String((double)amplitude, 3));
}

void TerrainNode::HandleFishSpeedSlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    float val = eventData[P_VALUE].GetFloat();  // 0..1
    float freq = val * 8.0f;  // 0..8 Hz

    // Update all fish materials
    const Vector<SharedPtr<Node>>& children = scene_->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
    {
        if (children[i]->GetName() == "Fish")
        {
            auto* sm = children[i]->GetComponent<StaticModel>();
            if (sm && sm->GetMaterial())
            {
                sm->GetMaterial()->SetShaderParameter("WiggleFrequency", freq);
            }
        }
    }

    if (fishSpeedLabel_)
        fishSpeedLabel_->SetText(String((double)freq, 1) + " Hz");
}

void TerrainNode::HandleWeatherSlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    auto* slider = static_cast<Slider*>(eventData[P_ELEMENT].GetPtr());
    if (!slider) return;

    float val = eventData[P_VALUE].GetFloat();
    int id = slider->GetVar("SliderID").GetI32();

    // Any slider movement enables override mode
    weatherOverride_ = true;

    char buf[16];
    if (id == 30) // cloud cover
    {
        weather_.cloudCover = val;
        weatherTarget_.cloudCover = val;
        // Derive fog/ambient from cloud cover
        weather_.fogDensity = 1.0f - val * 0.3f - weather_.precipitation * 0.3f;
        weather_.ambientDim = 1.0f - val * 0.4f;
        snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f));
        if (cloudCoverLabel_) cloudCoverLabel_->SetText(buf);
    }
    else if (id == 31) // precipitation
    {
        weather_.precipitation = val;
        weatherTarget_.precipitation = val;
        weather_.fogDensity = 1.0f - weather_.cloudCover * 0.3f - val * 0.3f;
        weather_.windSpeed = Clamp(0.1f + val * 0.4f, 0.0f, 1.0f);
        snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f));
        if (precipLabel_) precipLabel_->SetText(buf);
    }
    else if (id == 32) // rain override
    {
        rainOverride_ = val;
        snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f));
        if (rainLabel_) rainLabel_->SetText(buf);
    }

    ApplyWeatherToScene();
}

void TerrainNode::HandleMenuButton(StringHash eventType, VariantMap& eventData)
{
    using namespace Released;
    auto* element = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr());
    if (!element) return;

    int action = element->GetVar("MenuAction").GetI32();

    switch (action)
    {
    // Brush modes — stay in menu mode so user can also pick shape, then Tab to paint
    case 200:
    {
        int wasBrush = brushMode_;
        brushMode_ = 0;
        if (terrainPanel_) terrainPanel_->SetVisible(false);
        if (wasBrush != 0 && cameraMode_ == CAM_GOD && preEditCameraMode_ != CAM_GOD)
            EndEditTransition();
        break;
    }
    case 201: case 202: case 203: case 204: case 205:
    {
        int prev = brushMode_;
        if (action == 201) brushMode_ = 1;
        else if (action == 202) brushMode_ = 3;
        else if (action == 203) brushMode_ = 4;
        else if (action == 204) brushMode_ = 5;
        else brushMode_ = 6;  // river
        // Auto-switch to god cam if not already there
        if (prev == 0 && cameraMode_ != CAM_GOD)
            BeginEditTransition();
        break;
    }

    // Brush shapes
    case 210: brushShape_ = 0; break;
    case 211: brushShape_ = 1; break;
    case 212: brushShape_ = 2; break;
    case 213: brushShape_ = 3; break;
    case 214: brushShape_ = 4; break;
    case 215: brushShape_ = 5; break;
    case 216: brushShape_ = 6; break;

    // Save/Load heightmap via file dialog
    case 230: ShowSaveHeightmapDialog(); break;
    case 231: ShowLoadHeightmapDialog(); break;

    // Compute shaders
    case 240: RunErosion(erosionIterations_); break;
    case 241: TestComputeShader(); break;
    }

    // Highlight selected mode/shape button
    if (action >= 200 && action <= 205)
        HighlightBrushButton(activeModeBtn_, static_cast<Button*>(element));
    else if (action >= 210 && action <= 216)
        HighlightBrushButton(activeShapeBtn_, static_cast<Button*>(element));

    // Auto-enter camera mode only when brush is active (mode != none)
    if ((action >= 201 && action <= 205) || (action >= 210 && action <= 216))
    {
        if (brushMode_ != 0)
        {
            menuOpen_ = false;
            auto* input = GetSubsystem<Input>();
            GetSubsystem<UI>()->SetFocusElement(nullptr);
            input->SetMouseMode(MM_RELATIVE);
            input->SetMouseVisible(false);
            useMouseMode_ = MM_RELATIVE;
        }
    }
}

// ---------------------------------------------------------------------------
// Collapsible UI section helper
// ---------------------------------------------------------------------------

static UIElement* CreateCollapsibleSection(UIElement* parent, Font* font, const String& title, bool startExpanded = true)
{
    auto* context = parent->GetContext();

    // Header button
    auto* header = parent->CreateChild<Button>();
    header->SetStyleAuto();
    header->SetFixedHeight(22);
    header->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 0, 4, 0));

    auto* headerText = header->CreateChild<Text>();
    headerText->SetFont(font, 11);
    headerText->SetText(String(startExpanded ? "v " : "> ") + title);
    headerText->SetColor(Color(0.9f, 0.9f, 0.9f));
    headerText->SetAlignment(HA_LEFT, VA_CENTER);

    // Content container
    auto* content = parent->CreateChild<UIElement>();
    content->SetLayout(LM_VERTICAL, 2, IntRect(2, 2, 2, 2));
    content->SetVisible(startExpanded);

    // Store content and title for toggle
    header->SetVar("SectionContent", content);
    header->SetVar("SectionTitle", title);

    // Toggle handler — subscribe inline
    header->SubscribeToEvent(header, E_RELEASED, [](StringHash, VariantMap& eventData)
    {
        auto* btn = static_cast<Button*>(eventData[Released::P_ELEMENT].GetPtr());
        auto* content = static_cast<UIElement*>(btn->GetVar("SectionContent").GetPtr());
        if (!content) return;
        bool expanded = !content->IsVisible();
        content->SetVisible(expanded);
        // Update header text
        String title = btn->GetVar("SectionTitle").GetString();
        auto* txt = btn->GetChildStaticCast<Text>(0);
        if (txt)
            txt->SetText(String(expanded ? "v " : "> ") + title);

        // Force parent window to shrink-to-fit: walk up to the Window and
        // reset its height so the layout doesn't use the old size as target
        auto* panel = content->GetParent();
        if (panel)
        {
            panel->SetHeight(0);
            panel->UpdateLayout();
        }
    });

    return content;
}

void TerrainNode::CreateTerrainPanel()
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;
    auto* ui = GetSubsystem<UI>();
    auto* uiRoot = ui->GetRoot();

    terrainPanel_ = new Window(context_);
    terrainPanel_->SetStyle("Window");
    terrainPanel_->SetLayout(LM_VERTICAL, 4, IntRect(6, 6, 6, 6));
    terrainPanel_->SetOpacity(0.85f);
    terrainPanel_->SetVisible(false);
    terrainPanel_->SetPosition(10, 34);  // Below menu bar
    terrainPanel_->SetMovable(true);
    uiRoot->AddChild(terrainPanel_);

    // Title
    auto* title = terrainPanel_->CreateChild<Text>();
    title->SetFont(font, 12);
    title->SetText("Terrain Tools");
    title->SetColor(Color::WHITE);

    // ===== Brush Mode section (expanded) =====
    auto* modeContent = CreateCollapsibleSection(terrainPanel_, font, "Brush Mode", true);

    auto* modeRow = modeContent->CreateChild<UIElement>();
    modeRow->SetLayout(LM_HORIZONTAL, 2);
    modeRow->SetFixedHeight(24);
    const char* modeNames[] = {"Off", "Height", "Smooth", "Flatten", "Erode", "River"};
    const int modeActions[] = {200, 201, 202, 203, 204, 205};
    for (int i = 0; i < 6; ++i)
    {
        auto* btn = modeRow->CreateChild<Button>();
        btn->SetStyleAuto();
        btn->SetMinWidth(50);
        btn->SetFixedHeight(22);
        btn->SetVar("MenuAction", modeActions[i]);
        auto* txt = btn->CreateChild<Text>();
        txt->SetFont(font, 11);
        txt->SetText(modeNames[i]);
        txt->SetColor(Color::WHITE);
        txt->SetAlignment(HA_CENTER, VA_CENTER);
        SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleMenuButton));
    }

    // ===== Brush Shape section (expanded) =====
    auto* shapeContent = CreateCollapsibleSection(terrainPanel_, font, "Brush Shape", true);

    auto* shapeRow = shapeContent->CreateChild<UIElement>();
    shapeRow->SetLayout(LM_HORIZONTAL, 2);
    shapeRow->SetFixedHeight(28);
    for (int i = 0; i < 7; ++i)
    {
        auto* btn = shapeRow->CreateChild<Button>();
        btn->SetStyleAuto();
        btn->SetFixedSize(28, 26);
        btn->SetVar("MenuAction", 210 + i);
        auto* icon = btn->CreateChild<BorderImage>();
        icon->SetTexture(GenerateShapeIcon(i));
        icon->SetImageRect(IntRect(0, 0, 24, 24));
        icon->SetBlendMode(BLEND_ALPHA);
        icon->SetFixedSize(24, 24);
        icon->SetAlignment(HA_CENTER, VA_CENTER);
        shapeIcons_[i] = icon;
        SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleMenuButton));
    }

    // ===== Brush Settings section (expanded) =====
    auto* settingsContent = CreateCollapsibleSection(terrainPanel_, font, "Brush Settings", true);

    // Brush size slider (0.25-50)
    auto* sizeRow = settingsContent->CreateChild<UIElement>();
    sizeRow->SetLayout(LM_HORIZONTAL, 4);
    sizeRow->SetFixedHeight(20);
    brushSizeLabel_ = sizeRow->CreateChild<Text>();
    brushSizeLabel_->SetFont(font, 10);
    brushSizeLabel_->SetText("Size: " + String(brushRadius_, 1));
    brushSizeLabel_->SetMinWidth(100);
    auto* sizeSlider = sizeRow->CreateChild<Slider>();
    sizeSlider->SetStyleAuto();
    sizeSlider->SetFixedHeight(16);
    sizeSlider->SetMinWidth(100);
    sizeSlider->SetRange(49.75f);  // 0.25..50
    sizeSlider->SetValue(brushRadius_ - 0.25f);
    sizeSlider->SetVar("SliderID", 10);
    SubscribeToEvent(sizeSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Brush strength slider (0.01-0.2)
    auto* bstrRow = settingsContent->CreateChild<UIElement>();
    bstrRow->SetLayout(LM_HORIZONTAL, 4);
    bstrRow->SetFixedHeight(20);
    brushStrLabel_ = bstrRow->CreateChild<Text>();
    brushStrLabel_->SetFont(font, 10);
    brushStrLabel_->SetText("Strength: " + String(brushStrength_, 2));
    brushStrLabel_->SetMinWidth(100);
    auto* bstrSlider = bstrRow->CreateChild<Slider>();
    bstrSlider->SetStyleAuto();
    bstrSlider->SetFixedHeight(16);
    bstrSlider->SetMinWidth(100);
    bstrSlider->SetRange(1.0f);  // normalized 0..1 -> 0.01..5.0
    bstrSlider->SetValue((brushStrength_ - 0.01f) / 4.99f);
    bstrSlider->SetVar("SliderID", 11);
    SubscribeToEvent(bstrSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Rotation slider (-180..+180) + text edit
    auto* rotRow = settingsContent->CreateChild<UIElement>();
    rotRow->SetLayout(LM_HORIZONTAL, 4);
    rotRow->SetFixedHeight(20);
    brushRotLabel_ = rotRow->CreateChild<Text>();
    brushRotLabel_->SetFont(font, 10);
    { char buf[16]; sprintf(buf, "Rot: %.1f", brushRotation_); brushRotLabel_->SetText(buf); }
    brushRotLabel_->SetMinWidth(70);
    auto* rotSlider = rotRow->CreateChild<Slider>();
    rotSlider->SetStyleAuto();
    rotSlider->SetFixedHeight(16);
    rotSlider->SetMinWidth(100);
    rotSlider->SetRange(360.0f);  // 0..360 maps to -180..+180
    rotSlider->SetValue(brushRotation_ + 180.0f);
    rotSlider->SetVar("SliderID", 12);
    SubscribeToEvent(rotSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    brushRotEdit_ = rotRow->CreateChild<LineEdit>();
    brushRotEdit_->SetStyleAuto();
    brushRotEdit_->SetFixedWidth(50);
    brushRotEdit_->SetFixedHeight(18);
    { char buf[16]; sprintf(buf, "%.1f", brushRotation_); brushRotEdit_->SetText(buf); }
    brushRotEdit_->SetCursorPosition(0);
    SubscribeToEvent(brushRotEdit_, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleBrushRotEdit));

    // ===== Heightmap I/O section (collapsed) =====
    auto* ioContent = CreateCollapsibleSection(terrainPanel_, font, "Heightmap I/O", false);

    auto* ioRow = ioContent->CreateChild<UIElement>();
    ioRow->SetLayout(LM_HORIZONTAL, 4);
    ioRow->SetFixedHeight(26);

    auto* saveBtn = ioRow->CreateChild<Button>();
    saveBtn->SetStyleAuto();
    saveBtn->SetMinWidth(80);
    saveBtn->SetFixedHeight(22);
    saveBtn->SetVar("MenuAction", 230);
    auto* saveTxt = saveBtn->CreateChild<Text>();
    saveTxt->SetFont(font, 11);
    saveTxt->SetText("Save Heightmap");
    saveTxt->SetColor(Color::WHITE);
    saveTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(saveBtn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleMenuButton));

    auto* loadBtn = ioRow->CreateChild<Button>();
    loadBtn->SetStyleAuto();
    loadBtn->SetMinWidth(80);
    loadBtn->SetFixedHeight(22);
    loadBtn->SetVar("MenuAction", 231);
    auto* loadTxt = loadBtn->CreateChild<Text>();
    loadTxt->SetFont(font, 11);
    loadTxt->SetText("Load Heightmap");
    loadTxt->SetColor(Color::WHITE);
    loadTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(loadBtn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleMenuButton));

    // ===== Hydraulic Erosion section (collapsed) =====
    auto* erosionContent = CreateCollapsibleSection(terrainPanel_, font, "Hydraulic Erosion", false);

    // Iterations slider (10-500)
    auto* iterRow = erosionContent->CreateChild<UIElement>();
    iterRow->SetLayout(LM_HORIZONTAL, 4);
    iterRow->SetFixedHeight(20);
    erosionIterLabel_ = iterRow->CreateChild<Text>();
    erosionIterLabel_->SetFont(font, 10);
    erosionIterLabel_->SetText("Iterations: " + String(erosionIterations_));
    erosionIterLabel_->SetMinWidth(100);
    auto* iterSlider = iterRow->CreateChild<Slider>();
    iterSlider->SetStyleAuto();
    iterSlider->SetFixedHeight(16);
    iterSlider->SetMinWidth(100);
    iterSlider->SetRange(99.0f);  // 1..100
    iterSlider->SetValue((float)(erosionIterations_ - 1));
    iterSlider->SetVar("SliderID", 1);
    SubscribeToEvent(iterSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Rainfall slider (0.001-0.05)
    auto* rainRow = erosionContent->CreateChild<UIElement>();
    rainRow->SetLayout(LM_HORIZONTAL, 4);
    rainRow->SetFixedHeight(20);
    erosionRainLabel_ = rainRow->CreateChild<Text>();
    erosionRainLabel_->SetFont(font, 10);
    erosionRainLabel_->SetText("Rainfall: " + String(erosionRainfall_, 3));
    erosionRainLabel_->SetMinWidth(100);
    auto* rainSlider = rainRow->CreateChild<Slider>();
    rainSlider->SetStyleAuto();
    rainSlider->SetFixedHeight(16);
    rainSlider->SetMinWidth(100);
    rainSlider->SetRange(1.0f);  // normalized 0..1 -> 0.001..0.05
    rainSlider->SetValue((erosionRainfall_ - 0.001f) / 0.049f);
    rainSlider->SetVar("SliderID", 2);
    SubscribeToEvent(rainSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Erosion strength slider (0.01-1.0)
    auto* strRow = erosionContent->CreateChild<UIElement>();
    strRow->SetLayout(LM_HORIZONTAL, 4);
    strRow->SetFixedHeight(20);
    erosionStrLabel_ = strRow->CreateChild<Text>();
    erosionStrLabel_->SetFont(font, 10);
    erosionStrLabel_->SetText("Strength: " + String(erosionStrength_, 2));
    erosionStrLabel_->SetMinWidth(100);
    auto* strSlider = strRow->CreateChild<Slider>();
    strSlider->SetStyleAuto();
    strSlider->SetFixedHeight(16);
    strSlider->SetMinWidth(100);
    strSlider->SetRange(1.0f);  // normalized 0..1 -> 0.01..1.0
    strSlider->SetValue((erosionStrength_ - 0.01f) / 0.99f);
    strSlider->SetVar("SliderID", 3);
    SubscribeToEvent(strSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Max Depth slider (0.0-1.0, fraction of original height)
    auto* depthRow = erosionContent->CreateChild<UIElement>();
    depthRow->SetLayout(LM_HORIZONTAL, 4);
    depthRow->SetFixedHeight(20);
    erosionDepthLabel_ = depthRow->CreateChild<Text>();
    erosionDepthLabel_->SetFont(font, 10);
    erosionDepthLabel_->SetText("Max Depth: " + String(erosionMaxDepth_, 2));
    erosionDepthLabel_->SetMinWidth(100);
    auto* depthSlider = depthRow->CreateChild<Slider>();
    depthSlider->SetStyleAuto();
    depthSlider->SetFixedHeight(16);
    depthSlider->SetMinWidth(100);
    depthSlider->SetRange(1.0f);  // 0..1
    depthSlider->SetValue(erosionMaxDepth_);
    depthSlider->SetVar("SliderID", 4);
    SubscribeToEvent(depthSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Min Height Floor slider (0.0-0.5, fraction of height range)
    auto* floorRow = erosionContent->CreateChild<UIElement>();
    floorRow->SetLayout(LM_HORIZONTAL, 4);
    floorRow->SetFixedHeight(20);
    erosionFloorLabel_ = floorRow->CreateChild<Text>();
    erosionFloorLabel_->SetFont(font, 10);
    erosionFloorLabel_->SetText("Min Floor: " + String(erosionMinHeight_, 2));
    erosionFloorLabel_->SetMinWidth(100);
    auto* floorSlider = floorRow->CreateChild<Slider>();
    floorSlider->SetStyleAuto();
    floorSlider->SetFixedHeight(16);
    floorSlider->SetMinWidth(100);
    floorSlider->SetRange(1.0f);  // normalized 0..1 -> 0.0..0.5
    floorSlider->SetValue(erosionMinHeight_ / 0.5f);
    floorSlider->SetVar("SliderID", 5);
    SubscribeToEvent(floorSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Ridge Protection slider (0.0-1.0)
    auto* ridgeRow = erosionContent->CreateChild<UIElement>();
    ridgeRow->SetLayout(LM_HORIZONTAL, 4);
    ridgeRow->SetFixedHeight(20);
    erosionRidgeLabel_ = ridgeRow->CreateChild<Text>();
    erosionRidgeLabel_->SetFont(font, 10);
    erosionRidgeLabel_->SetText("Ridge Prot: " + String(erosionRidgeProtect_, 2));
    erosionRidgeLabel_->SetMinWidth(100);
    auto* ridgeSlider = ridgeRow->CreateChild<Slider>();
    ridgeSlider->SetStyleAuto();
    ridgeSlider->SetFixedHeight(16);
    ridgeSlider->SetMinWidth(100);
    ridgeSlider->SetRange(1.0f);  // 0..1
    ridgeSlider->SetValue(erosionRidgeProtect_);
    ridgeSlider->SetVar("SliderID", 6);
    SubscribeToEvent(ridgeSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Border Padding slider (0-16 cells)
    auto* borderRow = erosionContent->CreateChild<UIElement>();
    borderRow->SetLayout(LM_HORIZONTAL, 4);
    borderRow->SetFixedHeight(20);
    erosionBorderLabel_ = borderRow->CreateChild<Text>();
    erosionBorderLabel_->SetFont(font, 10);
    erosionBorderLabel_->SetText("Border Pad: " + String(erosionBorderPad_));
    erosionBorderLabel_->SetMinWidth(100);
    auto* borderSlider = borderRow->CreateChild<Slider>();
    borderSlider->SetStyleAuto();
    borderSlider->SetFixedHeight(16);
    borderSlider->SetMinWidth(100);
    borderSlider->SetRange(16.0f);  // 0..16
    borderSlider->SetValue((float)erosionBorderPad_);
    borderSlider->SetVar("SliderID", 7);
    SubscribeToEvent(borderSlider, E_SLIDERCHANGED, URHO3D_HANDLER(TerrainNode, HandleErosionSlider));

    // Erode button
    auto* erodeBtn = erosionContent->CreateChild<Button>();
    erodeBtn->SetStyleAuto();
    erodeBtn->SetMinWidth(120);
    erodeBtn->SetFixedHeight(22);
    erodeBtn->SetVar("MenuAction", 240);
    auto* erodeTxt = erodeBtn->CreateChild<Text>();
    erodeTxt->SetFont(font, 11);
    erodeTxt->SetText("Erode World");
    erodeTxt->SetColor(Color(0.5f, 1.0f, 0.5f));
    erodeTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(erodeBtn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleMenuButton));
}

void TerrainNode::HandleErosionSlider(StringHash eventType, VariantMap& eventData)
{
    using namespace SliderChanged;
    auto* slider = static_cast<Slider*>(eventData[P_ELEMENT].GetPtr());
    float value = eventData[P_VALUE].GetFloat();
    int id = slider->GetVar("SliderID").GetI32();

    switch (id)
    {
    case 1:  // Iterations: 1..100
        erosionIterations_ = 1 + (int)value;
        if (erosionIterLabel_)
            erosionIterLabel_->SetText("Iterations: " + String(erosionIterations_));
        break;
    case 2:  // Rainfall: 0.001..0.05
        erosionRainfall_ = 0.001f + value * 0.049f;
        if (erosionRainLabel_)
            erosionRainLabel_->SetText("Rainfall: " + String(erosionRainfall_, 3));
        break;
    case 3:  // Strength: 0.01..1.0
        erosionStrength_ = 0.01f + value * 0.99f;
        if (erosionStrLabel_)
            erosionStrLabel_->SetText("Strength: " + String(erosionStrength_, 2));
        break;
    case 4:  // Max Depth: 0.0..1.0
        erosionMaxDepth_ = value;
        if (erosionDepthLabel_)
            erosionDepthLabel_->SetText("Max Depth: " + String(erosionMaxDepth_, 2));
        break;
    case 5:  // Min Floor: 0.0..0.5
        erosionMinHeight_ = value * 0.5f;
        if (erosionFloorLabel_)
            erosionFloorLabel_->SetText("Min Floor: " + String(erosionMinHeight_, 2));
        break;
    case 6:  // Ridge Protection: 0.0..1.0
        erosionRidgeProtect_ = value;
        if (erosionRidgeLabel_)
            erosionRidgeLabel_->SetText("Ridge Prot: " + String(erosionRidgeProtect_, 2));
        break;
    case 7:  // Border Padding: 0..16
        erosionBorderPad_ = (int)value;
        if (erosionBorderLabel_)
            erosionBorderLabel_->SetText("Border Pad: " + String(erosionBorderPad_));
        break;
    case 10:  // Brush Size: 0.25..50
        brushRadius_ = 0.25f + value;
        if (brushSizeLabel_)
            brushSizeLabel_->SetText("Size: " + String(brushRadius_, 1));
        break;
    case 11:  // Brush Strength: 0.01..5.0
        brushStrength_ = 0.01f + value * 4.99f;
        if (brushStrLabel_)
            brushStrLabel_->SetText("Strength: " + String(brushStrength_, 2));
        break;
    case 12:  // Brush Rotation: -180..+180
    {
        brushRotation_ = value - 180.0f;
        if (brushRotLabel_)
            { char buf[16]; sprintf(buf, "Rot: %.1f", brushRotation_); brushRotLabel_->SetText(buf); }
        if (brushRotEdit_)
            { char buf[16]; sprintf(buf, "%.1f", brushRotation_); brushRotEdit_->SetText(buf); }
        if (activeShapeBtn_ && shapeIcons_[brushShape_])
            shapeIcons_[brushShape_]->SetTexture(GenerateShapeIcon(brushShape_));
        break;
    }
    }
}

void TerrainNode::HandleBrushRotEdit(StringHash eventType, VariantMap& eventData)
{
    float val = (float)strtod(brushRotEdit_->GetText().CString(), nullptr);
    if (val < -180.0f) val = -180.0f;
    if (val > 180.0f) val = 180.0f;
    brushRotation_ = val;
    if (brushRotLabel_)
        { char buf[16]; sprintf(buf, "Rot: %.1f", brushRotation_); brushRotLabel_->SetText(buf); }
    { char buf[16]; sprintf(buf, "%.1f", brushRotation_); brushRotEdit_->SetText(buf); }
    if (shapeIcons_[brushShape_])
        shapeIcons_[brushShape_]->SetTexture(GenerateShapeIcon(brushShape_));
}

void TerrainNode::HighlightBrushButton(Button*& active, Button* btn)
{
    // Reset previous
    if (active)
        active->SetColor(Color::WHITE);
    active = btn;
    if (active)
        active->SetColor(Color(0.4f, 0.8f, 1.0f));  // light blue highlight
}

void TerrainNode::ToggleTerrainPanel()
{
    if (!terrainPanel_)
        CreateTerrainPanel();

    bool show = !terrainPanel_->IsVisible();
    terrainPanel_->SetVisible(show);

    // Close the environment menu popup but stay in menu mode
    // so user can click the panel buttons
    if (environmentMenu_ && environmentMenu_->GetShowPopup())
        environmentMenu_->ShowPopup(false);
}

// ============================================================================
// Heightmap Save/Load
// ============================================================================

void TerrainNode::ShowSaveHeightmapDialog()
{
    if (!editableHeightMap_) return;

    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Save Heightmap");
    fileSelector_->SetButtonTexts("Save", "Cancel");

    Vector<String> filters;
    filters.Push("*.png");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Textures/");
    fileSelector_->SetFileName("HeightMap_edited.png");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandleHeightmapSaveChosen));
}

void TerrainNode::ShowLoadHeightmapDialog()
{
    if (!terrain_ || !editableHeightMap_) return;

    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Load Heightmap");
    fileSelector_->SetButtonTexts("Load", "Cancel");

    Vector<String> filters;
    filters.Push("*.png");
    filters.Push("*.*");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Textures/");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandleHeightmapLoadChosen));
}

void TerrainNode::HandleHeightmapSaveChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;
    SaveHeightmapToFile(path);
}

void TerrainNode::HandleHeightmapLoadChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;
    LoadHeightmapFromFile(path);
}

void TerrainNode::SaveHeightmapToFile(const String& path)
{
    if (!editableHeightMap_) return;

    if (editableHeightMap_->SavePNG(path))
        URHO3D_LOGINFOF("Heightmap saved to %s", path.CString());
    else
        URHO3D_LOGERRORF("Failed to save heightmap to %s", path.CString());
}

void TerrainNode::LoadHeightmapFromFile(const String& path)
{
    if (!terrain_ || !editableHeightMap_) return;

    SharedPtr<Image> loaded(new Image(context_));
    File file(context_, path);
    if (!loaded->Load(file))
    {
        URHO3D_LOGERRORF("Failed to load heightmap from %s", path.CString());
        return;
    }

    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    int loadW = loaded->GetWidth();
    int loadH = loaded->GetHeight();
    int loadComps = loaded->GetComponents();

    if (loadW != hmW || loadH != hmH)
    {
        URHO3D_LOGERRORF("Heightmap size mismatch: expected %dx%d, got %dx%d", hmW, hmH, loadW, loadH);
        return;
    }

    unsigned char* dst = editableHeightMap_->GetData();
    unsigned char* src = loaded->GetData();
    int dstComps = editableHeightMap_->GetComponents();
    for (int i = 0; i < hmW * hmH; ++i)
    {
        dst[i * dstComps] = src[i * loadComps];
        dst[i * dstComps + 1] = (loadComps >= 2) ? src[i * loadComps + 1] : 0;
        dst[i * dstComps + 2] = (loadComps >= 3) ? src[i * loadComps + 2] : 0;
        dst[i * dstComps + 3] = (loadComps >= 4) ? src[i * loadComps + 3] : 0;
    }
    terrain_->ApplyHeightMap();
    WakeSleepingBodiesOnTerrain();
    URHO3D_LOGINFOF("Heightmap loaded from %s", path.CString());
}

void TerrainNode::ShowExportPrefabDialog()
{
    if (!selectedNode_)
    {
        URHO3D_LOGWARNING("Export Prefab: no node selected");
        return;
    }

    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Export Prefab");
    fileSelector_->SetButtonTexts("Save", "Cancel");

    Vector<String> filters;
    filters.Push("*.xml");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Prefabs/");

    String name = selectedNode_->GetName();
    fileSelector_->SetFileName(name.Empty() ? "Prefab.xml" : name + ".xml");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandlePrefabExportChosen));
}

void TerrainNode::HandlePrefabExportChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty() || !selectedNode_) return;

    // Temporarily restore original materials (selection outline replaces them)
    auto* model = selectedNode_->GetComponent<StaticModel>();
    Vector<SharedPtr<Material>> maskMats;
    if (model && originalMaterials_.Size())
    {
        for (unsigned i = 0; i < model->GetNumGeometries(); ++i)
            maskMats.Push(SharedPtr<Material>(model->GetMaterial(i)));
        for (unsigned i = 0; i < originalMaterials_.Size() && i < model->GetNumGeometries(); ++i)
            model->SetMaterial(i, originalMaterials_[i]);
    }

    // Zero out position and rotation so prefab is origin-relative, but keep scale
    Vector3 savedPos = selectedNode_->GetPosition();
    Quaternion savedRot = selectedNode_->GetRotation();
    selectedNode_->SetPosition(Vector3::ZERO);
    selectedNode_->SetRotation(Quaternion::IDENTITY);

    File file(context_, path, FILE_WRITE);
    bool ok2 = selectedNode_->SaveXML(file);

    // Restore original transform
    selectedNode_->SetPosition(savedPos);
    selectedNode_->SetRotation(savedRot);

    // Re-apply mask materials for outline
    if (model && maskMats.Size())
    {
        for (unsigned i = 0; i < maskMats.Size() && i < model->GetNumGeometries(); ++i)
            model->SetMaterial(i, maskMats[i]);
    }

    if (ok2)
        URHO3D_LOGINFOF("Prefab exported to %s", path.CString());
    else
        URHO3D_LOGERRORF("Failed to export prefab to %s", path.CString());
}

void TerrainNode::ShowLoadPrefabDialog()
{
    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Load Prefab");
    fileSelector_->SetButtonTexts("Load", "Cancel");

    Vector<String> filters;
    filters.Push("*.xml");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Prefabs/");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandlePrefabLoadChosen));
}

void TerrainNode::HandlePrefabLoadChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;

    // Clean up previous prefab brush
    if (prefabBrush_)
    {
        prefabBrush_->Remove();
        prefabBrush_.Reset();
    }

    // Load XML into an orphan node (not in scene graph)
    XMLFile xmlFile(context_);
    File file(context_, path, FILE_READ);
    if (!xmlFile.Load(file))
    {
        URHO3D_LOGERRORF("Failed to load prefab XML: %s", path.CString());
        return;
    }

    prefabBrush_ = scene_->CreateChild("_PrefabBrush", LOCAL);
    if (!prefabBrush_->LoadXML(xmlFile.GetRoot()))
    {
        URHO3D_LOGERRORF("Failed to parse prefab node: %s", path.CString());
        prefabBrush_->Remove();
        prefabBrush_.Reset();
        return;
    }

    // Hide the template — it's only used for cloning
    prefabBrush_->SetEnabledRecursive(false);

    URHO3D_LOGINFOF("Prefab brush loaded: %s (%d children)", path.CString(), prefabBrush_->GetNumChildren());
    UpdatePrefabBrushLabel();
}

void TerrainNode::UpdatePrefabBrushLabel()
{
    if (!prefabBrushLabel_) return;
    if (prefabBrush_)
    {
        String name = prefabBrush_->GetName();
        if (name.Empty() || name == "_PrefabBrush")
            name = "Unnamed";
        prefabBrushLabel_->SetText("Object Brush: " + name);
    }
    else
        prefabBrushLabel_->SetText("Object Brush: None");
}

void TerrainNode::HandleRigidBodySleep(StringHash eventType, VariantMap& eventData)
{
    using namespace RigidBodySleep;
    auto* body = static_cast<RigidBody*>(eventData[P_BODY].GetPtr());
    if (!body)
        return;
    body->SetMass(0.0f);
    body->SetLinearDamping(0.0f);
    body->SetAngularDamping(0.0f);
    // Unsubscribe — one-shot event
    if (body->GetNode())
        UnsubscribeFromEvent(body->GetNode(), E_RIGIDBODYSLEEP);
}

void TerrainNode::ShowSaveSceneDialog()
{
    if (!scene_) return;

    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Save Scene");
    fileSelector_->SetButtonTexts("Save", "Cancel");

    Vector<String> filters;
    filters.Push("*.xml");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Scenes/");
    fileSelector_->SetFileName("Scene.xml");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandleSceneSaveChosen));
}

void TerrainNode::ShowLoadSceneDialog()
{
    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Load Scene");
    fileSelector_->SetButtonTexts("Load", "Cancel");

    Vector<String> filters;
    filters.Push("*.xml");
    filters.Push("*.*");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetProgramDir() + "Data/Scenes/");

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandleSceneLoadChosen));
}

void TerrainNode::HandleSceneSaveChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;

    File file(context_, path, FILE_WRITE);
    if (scene_->SaveXML(file))
        URHO3D_LOGINFOF("Scene saved to %s", path.CString());
    else
        URHO3D_LOGERRORF("Failed to save scene to %s", path.CString());
}

void TerrainNode::HandleSceneLoadChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;

    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
    {
        URHO3D_LOGERRORF("Failed to open scene file %s", path.CString());
        return;
    }

    if (scene_->LoadXML(file))
    {
        // Re-bind cached pointers after scene load
        zone_ = scene_->GetComponent<Zone>(true);
        terrain_ = scene_->GetComponent<Terrain>(true);
        if (terrain_)
        {
            Image* origHM = terrain_->GetHeightMap();
            if (origHM)
            {
                int w = origHM->GetWidth();
                int h = origHM->GetHeight();
                int origComps = origHM->GetComponents();
                editableHeightMap_ = new Image(context_);
                editableHeightMap_->SetSize(w, h, 4);
                editableHeightMap_->SetName(origHM->GetName());
                unsigned char* src = origHM->GetData();
                unsigned char* dst = editableHeightMap_->GetData();
                for (int i = 0; i < w * h; ++i)
                {
                    dst[i * 4] = src[i * origComps];
                    dst[i * 4 + 1] = (origComps >= 2) ? src[i * origComps + 1] : 0;
                    dst[i * 4 + 2] = (origComps >= 3) ? src[i * origComps + 2] : 0;
                    dst[i * 4 + 3] = (origComps >= 4) ? src[i * origComps + 3] : 0;
                }
                terrain_->SetHeightMap(editableHeightMap_);

                // Create water map (same resolution, single-channel, zeroed)
                waterMap_ = new Image(context_);
                waterMap_->SetSize(w, h, 1);
                memset(waterMap_->GetData(), 0, w * h);
            }

            // Create GPU texture from water map and bind to terrain material unit 4
            waterMapTex_ = new Texture2D(context_);
            waterMapTex_->SetFilterMode(FILTER_BILINEAR);
            waterMapTex_->SetAddressMode(COORD_U, ADDRESS_CLAMP);
            waterMapTex_->SetAddressMode(COORD_V, ADDRESS_CLAMP);
            waterMapTex_->SetData(waterMap_);
            auto* tMat = terrain_->GetMaterial();
            if (tMat)
            {
                tMat->SetTexture(TU_ENVIRONMENT, waterMapTex_);
                tMat->SetShaderParameter("WaterLevel", 5.0f);
                tMat->SetShaderParameter("TerrainSpacingY", terrain_->GetSpacing().y_);
            }

            // Initialize shared brush
            brush_ = new TerrainBrush(context_);
            brush_->SetTerrain(terrain_, editableHeightMap_);
            brush_->SetWaterMap(waterMap_);
        }
        sunNode_ = scene_->GetChild("Sun", true);
        moonNode_ = scene_->GetChild("Moon", true);
        // sunLight_ is on "DirectionalLight" node, NOT the "Sun" billboard node
        Node* dlNode = scene_->GetChild("DirectionalLight", true);
        sunLight_ = dlNode ? dlNode->GetComponent<Light>() : nullptr;
        Node* mlNode = scene_->GetChild("MoonLight", true);
        moonLight_ = mlNode ? mlNode->GetComponent<Light>() : nullptr;

        // Re-bind water node (it's a scene child, destroyed by LoadXML)
        waterNode_ = scene_->GetChild("Water", true);
        if (waterNode_)
        {
            // Re-derive water planes from loaded water node transform
            waterPlane_ = Plane(waterNode_->GetWorldRotation() * Vector3(0.0f, 1.0f, 0.0f), waterNode_->GetWorldPosition());
            waterClipPlane_ = Plane(waterNode_->GetWorldRotation() * Vector3(0.0f, 1.0f, 0.0f), waterNode_->GetWorldPosition());

            // Update reflection camera planes
            if (reflectionCameraNode_)
            {
                auto* reflectionCamera = reflectionCameraNode_->GetComponent<Camera>();
                if (reflectionCamera)
                {
                    reflectionCamera->SetReflectionPlane(waterPlane_);
                    reflectionCamera->SetClipPlane(waterClipPlane_);
                }
            }

            // Update render path WaterLevel parameter
            if (renderPath_)
                renderPath_->SetShaderParameter("WaterLevel", waterNode_->GetWorldPosition().y_);
        }

        // Re-bind fog parameters from loaded zone
        if (zone_)
        {
            origFogColor_ = zone_->GetFogColor();
            origFogStart_ = zone_->GetFogStart();
            origFogEnd_ = zone_->GetFogEnd();
        }

        // Deselect any previously selected node (it's gone now)
        selectedNode_.Reset();
        originalMaterials_.Clear();

        URHO3D_LOGINFOF("Scene loaded from %s", path.CString());
    }
    else
        URHO3D_LOGERRORF("Failed to load scene from %s", path.CString());
}

void TerrainNode::WakeSleepingBodiesOnTerrain()
{
    auto* physicsWorld = scene_->GetComponent<PhysicsWorld>();
    if (!physicsWorld || !terrain_)
        return;

    // Build bounding box from terrain dimensions
    Vector3 spacing = terrain_->GetSpacing();
    int hmW = terrain_->GetHeightMap() ? terrain_->GetHeightMap()->GetWidth() : 1025;
    int hmH = terrain_->GetHeightMap() ? terrain_->GetHeightMap()->GetHeight() : 1025;
    float halfW = hmW * spacing.x_ * 0.5f;
    float halfH = hmH * spacing.z_ * 0.5f;
    float maxY = 255.0f * spacing.y_;
    Vector3 terrainPos = terrain_->GetNode()->GetWorldPosition();
    BoundingBox terrainBB(
        terrainPos + Vector3(-halfW, 0.0f, -halfH),
        terrainPos + Vector3(halfW, maxY, halfH));

    Vector<RigidBody*> bodies;
    physicsWorld->GetRigidBodies(bodies, terrainBB);
    for (unsigned i = 0; i < bodies.Size(); ++i)
    {
        if (!bodies[i]->IsActive())
            bodies[i]->Activate();
    }
}

// ============================================================================
// Transform Gizmo
// ============================================================================

void TerrainNode::DrawGizmo()
{
    if (gizmoMode_ == 0 || !selectedNode_ || selectedNode_.Expired())
        return;

    auto* debug = scene_->GetComponent<DebugRenderer>();
    if (!debug)
        return;

    Node* node = selectedNode_;
    Vector3 pos = node->GetWorldPosition();

    // Axis length proportional to distance from camera
    float dist = (pos - cameraNode_->GetWorldPosition()).Length();
    float len = dist * 0.15f;
    float thickness = gizmoMode_ == 3 ? 2.0f : 1.0f;

    // Determine axis directions
    Vector3 axisX, axisY, axisZ;
    if (gizmoLocal_)
    {
        Quaternion rot = node->GetWorldRotation();
        axisX = rot * Vector3::RIGHT;
        axisY = rot * Vector3::UP;
        axisZ = rot * Vector3::FORWARD;
    }
    else
    {
        axisX = Vector3::RIGHT;
        axisY = Vector3::UP;
        axisZ = Vector3::FORWARD;
    }

    Color colX = (gizmoAxis_ == 0 && gizmoDragging_) ? Color::YELLOW : Color::RED;
    Color colY = (gizmoAxis_ == 1 && gizmoDragging_) ? Color::YELLOW : Color::GREEN;
    Color colZ = (gizmoAxis_ == 2 && gizmoDragging_) ? Color::YELLOW : Color::BLUE;

    if (gizmoMode_ == 1) // Translate — lines with arrow tips
    {
        debug->AddLine(pos, pos + axisX * len, colX, false);
        debug->AddLine(pos, pos + axisY * len, colY, false);
        debug->AddLine(pos, pos + axisZ * len, colZ, false);
        // Arrow tips
        float tipLen = len * 0.15f;
        debug->AddLine(pos + axisX * len, pos + axisX * (len - tipLen) + axisY * tipLen * 0.3f, colX, false);
        debug->AddLine(pos + axisX * len, pos + axisX * (len - tipLen) - axisY * tipLen * 0.3f, colX, false);
        debug->AddLine(pos + axisY * len, pos + axisY * (len - tipLen) + axisX * tipLen * 0.3f, colY, false);
        debug->AddLine(pos + axisY * len, pos + axisY * (len - tipLen) - axisX * tipLen * 0.3f, colY, false);
        debug->AddLine(pos + axisZ * len, pos + axisZ * (len - tipLen) + axisY * tipLen * 0.3f, colZ, false);
        debug->AddLine(pos + axisZ * len, pos + axisZ * (len - tipLen) - axisY * tipLen * 0.3f, colZ, false);
    }
    else if (gizmoMode_ == 2) // Rotate — circles
    {
        const int segs = 32;
        for (int i = 0; i < segs; ++i)
        {
            float a0 = (float)i / segs * 360.0f * M_DEGTORAD;
            float a1 = (float)(i + 1) / segs * 360.0f * M_DEGTORAD;
            // X axis ring (in YZ plane)
            debug->AddLine(pos + (axisY * cosf(a0) + axisZ * sinf(a0)) * len,
                          pos + (axisY * cosf(a1) + axisZ * sinf(a1)) * len, colX, false);
            // Y axis ring (in XZ plane)
            debug->AddLine(pos + (axisX * cosf(a0) + axisZ * sinf(a0)) * len,
                          pos + (axisX * cosf(a1) + axisZ * sinf(a1)) * len, colY, false);
            // Z axis ring (in XY plane)
            debug->AddLine(pos + (axisX * cosf(a0) + axisY * sinf(a0)) * len,
                          pos + (axisX * cosf(a1) + axisY * sinf(a1)) * len, colZ, false);
        }
    }
    else if (gizmoMode_ == 3) // Scale — lines with box tips
    {
        debug->AddLine(pos, pos + axisX * len, colX, false);
        debug->AddLine(pos, pos + axisY * len, colY, false);
        debug->AddLine(pos, pos + axisZ * len, colZ, false);
        float boxSz = len * 0.06f;
        debug->AddBoundingBox(BoundingBox(pos + axisX * len - Vector3::ONE * boxSz, pos + axisX * len + Vector3::ONE * boxSz), colX, false);
        debug->AddBoundingBox(BoundingBox(pos + axisY * len - Vector3::ONE * boxSz, pos + axisY * len + Vector3::ONE * boxSz), colY, false);
        debug->AddBoundingBox(BoundingBox(pos + axisZ * len - Vector3::ONE * boxSz, pos + axisZ * len + Vector3::ONE * boxSz), colZ, false);
    }

    // Show mode label
    const char* modeNames[] = {"", "Translate", "Rotate", "Scale"};
    const char* spaceNames[] = {"World", "Local"};
    (void)modeNames; (void)spaceNames;
}

void TerrainNode::BeginGizmoDrag(int axis)
{
    if (!selectedNode_ || selectedNode_.Expired() || gizmoMode_ == 0)
        return;

    gizmoAxis_ = axis;
    gizmoDragging_ = true;
    gizmoNodeStart_ = selectedNode_->GetWorldPosition();
    gizmoRotStart_ = selectedNode_->GetRotation();
    gizmoScaleStart_ = selectedNode_->GetScale();

    auto* input = GetSubsystem<Input>();
    IntVector2 mousePos = input->GetMousePosition();

    if (gizmoMode_ == 2) // Rotate — arcball, map mouse to virtual sphere
    {
        auto* camera = cameraNode_->GetComponent<Camera>();
        auto* graphics = GetSubsystem<Graphics>();
        float w = (float)graphics->GetWidth();
        float h = (float)graphics->GetHeight();

        // Project object center to screen
        Vector2 screenCenter = camera->WorldToScreenPoint(gizmoNodeStart_);
        float cx = screenCenter.x_ * w;
        float cy = screenCenter.y_ * h;

        // Virtual sphere radius on screen (based on gizmo visual size)
        float dist = (gizmoNodeStart_ - cameraNode_->GetWorldPosition()).Length();
        float len = dist * 0.15f;
        Vector2 screenEdge = camera->WorldToScreenPoint(gizmoNodeStart_ + Vector3::RIGHT * len);
        float sphereRadius = sqrtf((screenEdge.x_ * w - cx) * (screenEdge.x_ * w - cx) +
                                   (screenEdge.y_ * h - cy) * (screenEdge.y_ * h - cy));
        if (sphereRadius < 10.0f) sphereRadius = 10.0f;

        // Map mouse to sphere point
        float sx = ((float)mousePos.x_ - cx) / sphereRadius;
        float sy = ((float)mousePos.y_ - cy) / sphereRadius;
        float sz2 = 1.0f - sx * sx - sy * sy;
        if (sz2 > 0.0f)
            gizmoDragStart_ = Vector3(sx, -sy, sqrtf(sz2));  // on sphere
        else
            gizmoDragStart_ = Vector3(sx, -sy, 0.0f).Normalized();  // outside sphere, clamp to edge

    }
    else // Translate/Scale — store screen pixel position
    {
        gizmoDragStart_ = Vector3((float)mousePos.x_, (float)mousePos.y_, 0.0f);
    }
}

void TerrainNode::UpdateGizmoDrag()
{
    if (!gizmoDragging_ || !selectedNode_ || selectedNode_.Expired())
        return;

    auto* input = GetSubsystem<Input>();
    auto* camera = cameraNode_->GetComponent<Camera>();
    auto* graphics = GetSubsystem<Graphics>();

    // Current mouse position in screen pixels
    IntVector2 mousePos = input->GetMousePosition();
    float mx = (float)mousePos.x_;
    float my = (float)mousePos.y_;

    // Mouse delta from drag start in pixels
    float dx = mx - gizmoDragStart_.x_;
    float dy = my - gizmoDragStart_.y_;

    // Get world-space axis direction
    Vector3 axisDir;
    if (gizmoLocal_)
    {
        Quaternion rot = selectedNode_->GetWorldRotation();
        Vector3 axes[] = {rot * Vector3::RIGHT, rot * Vector3::UP, rot * Vector3::FORWARD};
        axisDir = axes[gizmoAxis_];
    }
    else
    {
        Vector3 axes[] = {Vector3::RIGHT, Vector3::UP, Vector3::FORWARD};
        axisDir = axes[gizmoAxis_];
    }

    // Project axis direction to screen space to get a 2D direction
    float w = (float)graphics->GetWidth();
    float h = (float)graphics->GetHeight();
    Vector2 screenOrigin = camera->WorldToScreenPoint(gizmoNodeStart_);
    Vector2 screenAxisEnd = camera->WorldToScreenPoint(gizmoNodeStart_ + axisDir);
    Vector2 screenDir(screenAxisEnd.x_ - screenOrigin.x_, screenAxisEnd.y_ - screenOrigin.y_);

    // Convert from normalized [0..1] to pixels
    screenDir.x_ *= w;
    screenDir.y_ *= h;

    float screenDirLen = screenDir.Length();
    if (screenDirLen < 1.0f)
        return;  // axis is edge-on to camera

    screenDir /= screenDirLen;  // normalize

    // Project mouse pixel delta onto screen-space axis direction
    float screenDelta = dx * screenDir.x_ + dy * screenDir.y_;

    // Convert screen delta to world units: scale by distance from camera
    float camDist = (gizmoNodeStart_ - cameraNode_->GetWorldPosition()).Length();
    float worldDelta = screenDelta * camDist * 0.002f;  // sensitivity factor

    if (gizmoMode_ == 1) // Translate
    {
        Vector3 newWorldPos = gizmoNodeStart_ + axisDir * worldDelta;
        if (selectedNode_->GetParent())
            selectedNode_->SetPosition(selectedNode_->GetParent()->WorldToLocal(newWorldPos));
        else
            selectedNode_->SetPosition(newWorldPos);
    }
    else if (gizmoMode_ == 2) // Rotate — arcball
    {
        // Recalculate screen center and sphere radius
        float w2 = (float)graphics->GetWidth();
        float h2 = (float)graphics->GetHeight();
        Vector2 screenCenter = camera->WorldToScreenPoint(gizmoNodeStart_);
        float cx = screenCenter.x_ * w2;
        float cy = screenCenter.y_ * h2;

        float camDist2 = (gizmoNodeStart_ - cameraNode_->GetWorldPosition()).Length();
        float gizmoLen = camDist2 * 0.15f;
        Vector2 screenEdge = camera->WorldToScreenPoint(gizmoNodeStart_ + Vector3::RIGHT * gizmoLen);
        float sphereRadius = sqrtf((screenEdge.x_ * w2 - cx) * (screenEdge.x_ * w2 - cx) +
                                   (screenEdge.y_ * h2 - cy) * (screenEdge.y_ * h2 - cy));
        if (sphereRadius < 10.0f) sphereRadius = 10.0f;

        // Map current mouse to sphere point
        float sx = (mx - cx) / sphereRadius;
        float sy = (my - cy) / sphereRadius;
        float sz2 = 1.0f - sx * sx - sy * sy;
        Vector3 currentSphere;
        if (sz2 > 0.0f)
            currentSphere = Vector3(sx, -sy, sqrtf(sz2));
        else
            currentSphere = Vector3(sx, -sy, 0.0f).Normalized();

        // Quaternion from start sphere point to current sphere point
        Vector3 cross = gizmoDragStart_.CrossProduct(currentSphere);
        float dot = gizmoDragStart_.DotProduct(currentSphere);

        // Build rotation quaternion: (cross.x, cross.y, cross.z, dot) then normalize
        Quaternion arcballRot(dot, cross.x_, cross.y_, cross.z_);
        arcballRot.Normalize();

        // arcballRot is in view space — convert to world space
        // View-space axes: X=right, Y=up, Z=into screen (camera facing -Z)
        Quaternion camRot = cameraNode_->GetWorldRotation();
        Quaternion worldRot = camRot * arcballRot * camRot.Inverse();

        // Apply to original rotation
        if (selectedNode_->GetParent())
        {
            Quaternion parentRot = selectedNode_->GetParent()->GetWorldRotation();
            selectedNode_->SetRotation(parentRot.Inverse() * worldRot * parentRot * gizmoRotStart_);
        }
        else
        {
            selectedNode_->SetRotation(worldRot * gizmoRotStart_);
        }
    }
    else if (gizmoMode_ == 3) // Scale
    {
        float scaleFactor = 1.0f + screenDelta * 0.005f;
        if (scaleFactor < 0.01f) scaleFactor = 0.01f;
        Vector3 newScale = gizmoScaleStart_;
        if (gizmoAxis_ == 0) newScale.x_ *= scaleFactor;
        else if (gizmoAxis_ == 1) newScale.y_ *= scaleFactor;
        else newScale.z_ *= scaleFactor;
        selectedNode_->SetScale(newScale);
    }
}

void TerrainNode::EndGizmoDrag()
{
    if (!gizmoDragging_)
        return;

    gizmoDragging_ = false;
    gizmoAxis_ = -1;
}

void TerrainNode::WakeSelectedNode()
{
    if (!selectedNode_ || selectedNode_.Expired())
        return;

    Vector<RigidBody*> bodies;
    selectedNode_->GetDerivedComponents<RigidBody>(bodies, true);
    for (unsigned b = 0; b < bodies.Size(); ++b)
    {
        RigidBody* rb = bodies[b];
        if (rb->GetMass() == 0.0f)
        {
            rb->SetMass(100.0f);
            rb->SetFriction(0.75f);
            rb->SetLinearDamping(0.9f);
            rb->SetAngularDamping(0.9f);
            SubscribeToEvent(selectedNode_, E_RIGIDBODYSLEEP, URHO3D_HANDLER(TerrainNode, HandleRigidBodySleep));
        }
        rb->Activate();
    }
}

// ============================================================================
// Import Model
// ============================================================================

void TerrainNode::ShowImportModelDialog()
{
    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Import Model");
    fileSelector_->SetButtonTexts("Import", "Cancel");

    Vector<String> filters;
    filters.Push("*.fbx");
    filters.Push("*.obj");
    filters.Push("*.dae");
    filters.Push("*.blend");
    filters.Push("*.gltf");
    filters.Push("*.glb");
    filters.Push("*.*");
    fileSelector_->SetFilters(filters, 0);

    auto* fs = GetSubsystem<FileSystem>();
    fileSelector_->SetPath(fs->GetUserDocumentsDir());

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(TerrainNode, HandleImportModelChosen));
}

void TerrainNode::HandleImportModelChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty())
        return;

    auto* fs = GetSubsystem<FileSystem>();
    auto* cache = GetSubsystem<ResourceCache>();

    // Determine output path
    String baseName = GetFileName(path);
    String outputDir = fs->GetProgramDir() + "Data/Models/";
    String outputMdl = outputDir + baseName + ".mdl";

    // Run AssetImporter
    String toolPath = fs->GetProgramDir() + "tool/AssetImporter";
    String cmd = "\"" + toolPath + "\" model \"" + path + "\" \"" + outputMdl + "\" -t";
    URHO3D_LOGINFOF("Running: %s", cmd.CString());

    int result = fs->SystemCommand(cmd);
    if (result != 0)
    {
        URHO3D_LOGERRORF("AssetImporter failed (exit code %d)", result);
        return;
    }

    // Load the imported model and place it in the scene
    auto* model = cache->GetResource<Model>("Models/" + baseName + ".mdl");
    if (!model)
    {
        URHO3D_LOGERROR("Failed to load imported model");
        return;
    }

    // Place at camera position + forward offset
    auto* camera = cameraNode_->GetComponent<Camera>();
    Vector3 spawnPos = cameraNode_->GetPosition() + cameraNode_->GetDirection() * 10.0f;

    Node* parentNode = (selectedNode_ && !selectedNode_.Expired()) ? selectedNode_.Get() : scene_;
    Node* node = parentNode->CreateChild(baseName);
    auto* sm = node->CreateComponent<StaticModel>();
    sm->SetModel(model);

    // Try to find a matching material
    String matPath = "Materials/" + baseName + ".xml";
    auto* mat = cache->GetResource<Material>(matPath, false);
    if (!mat)
        mat = cache->GetResource<Material>("Materials/Stone.xml", false);
    if (mat)
        sm->SetMaterial(mat);

    sm->SetCastShadows(true);
    node->SetPosition(spawnPos);

    // Add physics
    auto* body = node->CreateComponent<RigidBody>();
    body->SetMass(0.0f);
    auto* shape = node->CreateComponent<CollisionShape>();
    BoundingBox bb = model->GetBoundingBox();
    shape->SetBox(bb.Size(), bb.Center());

    // Record undo
    UndoAction action;
    action.type = UndoAction::NODE_CREATE;
    action.nodeID = node->GetID();
    action.xmlData = SerializeNode(node);
    action.position = node->GetPosition();
    action.rotation = node->GetRotation();
    PushUndo(action);

    SelectNode(node);
    if (hierarchyWindow_ && hierarchyWindow_->IsVisible())
        BuildHierarchyTree();

    URHO3D_LOGINFOF("Imported and placed: %s", baseName.CString());
}

// ============================================================================
// Generate Mesh
// ============================================================================

void TerrainNode::ShowGenerateMeshPanel()
{
    if (generateMeshPanel_)
    {
        generateMeshPanel_->SetVisible(!generateMeshPanel_->IsVisible());
        return;
    }

    auto* ui = GetSubsystem<UI>();
    auto* cache = GetSubsystem<ResourceCache>();
    auto* style = ui->GetRoot()->GetDefaultStyle();
    Font* font = font_;

    generateMeshPanel_ = new Window(context_);
    generateMeshPanel_->SetStyleAuto();
    generateMeshPanel_->SetLayout(LM_VERTICAL, 6, IntRect(8, 8, 8, 8));
    generateMeshPanel_->SetAlignment(HA_CENTER, VA_CENTER);
    generateMeshPanel_->SetSize(320, 340);
    generateMeshPanel_->SetMovable(true);
    ui->GetRoot()->AddChild(generateMeshPanel_);

    // Title
    auto* title = generateMeshPanel_->CreateChild<Text>();
    title->SetFont(font, 14);
    title->SetText("Generate Mesh");
    title->SetColor(Color::WHITE);

    // Shape dropdown
    auto* shapeRow = generateMeshPanel_->CreateChild<UIElement>();
    shapeRow->SetLayout(LM_HORIZONTAL, 4);
    shapeRow->SetMinHeight(24);
    auto* shapeLbl = shapeRow->CreateChild<Text>();
    shapeLbl->SetFont(font, 12);
    shapeLbl->SetText("Shape:");
    shapeLbl->SetColor(Color(0.8f, 0.8f, 0.8f));

    meshShapeList_ = shapeRow->CreateChild<DropDownList>();
    meshShapeList_->SetStyleAuto();
    meshShapeList_->SetMinSize(150, 22);
    const char* shapes[] = {"Box", "Sphere", "Cylinder", "Cone", "Capsule"};
    for (int i = 0; i < 5; ++i)
    {
        auto* item = new Text(context_);
        item->SetFont(font, 12);
        item->SetText(shapes[i]);
        item->SetColor(Color(0.9f, 0.9f, 0.9f));
        item->SetMinSize(0, 20);
        meshShapeList_->AddItem(item);
    }
    meshShapeList_->SetSelection(0);

    // Helper lambda-style: create param row
    auto CreateParamRow = [&](const String& label, float defVal, float maxVal, Text*& outLabel, Slider*& outSlider)
    {
        auto* row = generateMeshPanel_->CreateChild<UIElement>();
        row->SetLayout(LM_HORIZONTAL, 4);
        row->SetMinHeight(24);
        outLabel = row->CreateChild<Text>();
        outLabel->SetFont(font, 12);
        outLabel->SetText(label + ": " + String(defVal, 1));
        outLabel->SetColor(Color(0.8f, 0.8f, 0.8f));
        outLabel->SetMinWidth(140);
        outSlider = row->CreateChild<Slider>();
        outSlider->SetStyleAuto();
        outSlider->SetRange(maxVal);
        outSlider->SetValue(defVal);
        outSlider->SetMinSize(120, 18);
    };

    CreateParamRow("Size X / Radius", 1.0f, 5.0f, meshParam1Label_, meshParam1Slider_);
    CreateParamRow("Size Y / Height", 1.0f, 5.0f, meshParam2Label_, meshParam2Slider_);
    CreateParamRow("Segments", 16.0f, 64.0f, meshParam3Label_, meshParam3Slider_);

    // Name edit
    auto* nameRow = generateMeshPanel_->CreateChild<UIElement>();
    nameRow->SetLayout(LM_HORIZONTAL, 4);
    nameRow->SetMinHeight(24);
    auto* nameLbl = nameRow->CreateChild<Text>();
    nameLbl->SetFont(font, 12);
    nameLbl->SetText("Name:");
    nameLbl->SetColor(Color(0.8f, 0.8f, 0.8f));
    meshNameEdit_ = nameRow->CreateChild<LineEdit>();
    meshNameEdit_->SetStyleAuto();
    meshNameEdit_->SetMinSize(180, 22);
    meshNameEdit_->SetText("NewMesh");

    // Material dropdown
    auto* matRow = generateMeshPanel_->CreateChild<UIElement>();
    matRow->SetLayout(LM_HORIZONTAL, 4);
    matRow->SetMinHeight(24);
    auto* matLbl = matRow->CreateChild<Text>();
    matLbl->SetFont(font, 12);
    matLbl->SetText("Material:");
    matLbl->SetColor(Color(0.8f, 0.8f, 0.8f));
    meshMaterialList_ = matRow->CreateChild<DropDownList>();
    meshMaterialList_->SetStyleAuto();
    meshMaterialList_->SetMinSize(150, 22);
    const char* mats[] = {"Stone", "Mushroom", "Jack", "Terrain"};
    for (int i = 0; i < 4; ++i)
    {
        auto* item = new Text(context_);
        item->SetFont(font, 12);
        item->SetText(mats[i]);
        item->SetColor(Color(0.9f, 0.9f, 0.9f));
        item->SetMinSize(0, 20);
        meshMaterialList_->AddItem(item);
    }
    meshMaterialList_->SetSelection(0);

    // Generate button
    auto* btn = generateMeshPanel_->CreateChild<Button>();
    btn->SetStyleAuto();
    btn->SetMinHeight(28);
    btn->SetLayout(LM_HORIZONTAL, 0, IntRect(8, 4, 8, 4));
    auto* btnText = btn->CreateChild<Text>();
    btnText->SetFont(font, 13);
    btnText->SetText("Generate");
    btnText->SetColor(Color::WHITE);
    btnText->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(TerrainNode, HandleGenerateMesh));
}

void TerrainNode::HandleGenerateMesh(StringHash eventType, VariantMap& eventData)
{
    if (!meshShapeList_ || !meshNameEdit_)
        return;

    auto* fs = GetSubsystem<FileSystem>();
    auto* cache = GetSubsystem<ResourceCache>();

    String name = meshNameEdit_->GetText().Trimmed();
    if (name.Empty()) name = "NewMesh";

    unsigned shapeIdx = meshShapeList_->GetSelection();
    const char* shapeNames[] = {"box", "sphere", "cylinder", "cone", "capsule"};
    String shapeType = shapeNames[shapeIdx];

    float p1 = meshParam1Slider_ ? meshParam1Slider_->GetValue() : 1.0f;
    float p2 = meshParam2Slider_ ? meshParam2Slider_->GetValue() : 1.0f;
    int p3 = meshParam3Slider_ ? (int)meshParam3Slider_->GetValue() : 16;
    if (p1 < 0.1f) p1 = 0.1f;
    if (p2 < 0.1f) p2 = 0.1f;
    if (p3 < 3) p3 = 3;

    // Write temp XML for MeshGenerator
    String tempXml = fs->GetTemporaryDir() + name + ".xml";
    String outputMdl = fs->GetProgramDir() + "Data/Models/" + name + ".mdl";

    {
        File f(context_, tempXml, FILE_WRITE);
        if (shapeType == "box")
            f.WriteString("<mesh type=\"box\" sizeX=\"" + String(p1) + "\" sizeY=\"" + String(p2) + "\" sizeZ=\"" + String(p1) + "\" />\n");
        else if (shapeType == "sphere")
            f.WriteString("<mesh type=\"sphere\" radius=\"" + String(p1 * 0.5f) + "\" segments=\"" + String(p3) + "\" rings=\"" + String(p3 / 2) + "\" />\n");
        else if (shapeType == "cylinder")
            f.WriteString("<mesh type=\"cylinder\" radius=\"" + String(p1 * 0.5f) + "\" height=\"" + String(p2) + "\" segments=\"" + String(p3) + "\" />\n");
        else if (shapeType == "cone")
            f.WriteString("<mesh type=\"cone\" radius=\"" + String(p1 * 0.5f) + "\" height=\"" + String(p2) + "\" segments=\"" + String(p3) + "\" />\n");
        else if (shapeType == "capsule")
            f.WriteString("<mesh type=\"capsule\" radius=\"" + String(p1 * 0.5f) + "\" height=\"" + String(p2) + "\" segments=\"" + String(p3) + "\" rings=\"" + String(p3 / 2) + "\" />\n");
    }

    // Run MeshGenerator
    String toolPath = fs->GetProgramDir() + "tool/MeshGenerator";
    String cmd = "\"" + toolPath + "\" \"" + tempXml + "\" \"" + outputMdl + "\"";
    URHO3D_LOGINFOF("Running: %s", cmd.CString());

    int result = fs->SystemCommand(cmd);
    if (result != 0)
    {
        URHO3D_LOGERRORF("MeshGenerator failed (exit code %d)", result);
        return;
    }

    // Load and place
    auto* model = cache->GetResource<Model>("Models/" + name + ".mdl");
    if (!model)
    {
        URHO3D_LOGERROR("Failed to load generated model");
        return;
    }

    Vector3 spawnPos = cameraNode_->GetPosition() + cameraNode_->GetDirection() * 10.0f;

    Node* parentNode = (selectedNode_ && !selectedNode_.Expired()) ? selectedNode_.Get() : scene_;
    Node* node = parentNode->CreateChild(name);
    auto* sm = node->CreateComponent<StaticModel>();
    sm->SetModel(model);
    sm->SetCastShadows(true);

    // Apply selected material
    unsigned matIdx = meshMaterialList_ ? meshMaterialList_->GetSelection() : 0;
    const char* matFiles[] = {"Materials/Stone.xml", "Materials/Mushroom.xml", "Materials/Jack.xml", "Materials/Terrain.xml"};
    auto* mat = cache->GetResource<Material>(matFiles[matIdx], false);
    if (mat)
        sm->SetMaterial(mat);

    node->SetPosition(spawnPos);

    // Add physics
    auto* body = node->CreateComponent<RigidBody>();
    body->SetMass(0.0f);
    auto* shape = node->CreateComponent<CollisionShape>();
    BoundingBox bb = model->GetBoundingBox();
    shape->SetBox(bb.Size(), bb.Center());

    // Record undo
    UndoAction action;
    action.type = UndoAction::NODE_CREATE;
    action.nodeID = node->GetID();
    action.xmlData = SerializeNode(node);
    action.position = node->GetPosition();
    action.rotation = node->GetRotation();
    PushUndo(action);

    SelectNode(node);
    if (hierarchyWindow_ && hierarchyWindow_->IsVisible())
        BuildHierarchyTree();

    URHO3D_LOGINFOF("Generated and placed: %s (%s)", name.CString(), shapeType.CString());

    // Hide panel
    if (generateMeshPanel_)
        generateMeshPanel_->SetVisible(false);
}

void TerrainNode::HandleMeshShapeChanged(StringHash eventType, VariantMap& eventData)
{
    // Future: update parameter labels based on selected shape
}

// ============================================================================
// Undo / Redo
// ============================================================================

void TerrainNode::PushUndo(const UndoAction& action)
{
    // Truncate any redo history beyond cursor
    while ((int)undoStack_.Size() > undoCursor_)
        undoStack_.Pop();

    undoStack_.Push(action);
    ++undoCursor_;

    // Cap at 50 entries
    if (undoStack_.Size() > 50)
    {
        undoStack_.Erase(0);
        --undoCursor_;
    }
}

void TerrainNode::Undo()
{
    if (undoCursor_ <= 0)
        return;

    --undoCursor_;
    const UndoAction& action = undoStack_[undoCursor_];

    switch (action.type)
    {
    case UndoAction::NODE_DELETE:
        {
            // Re-create the deleted node from saved XML
            XMLFile xmlFile(context_);
            MemoryBuffer buf(action.xmlData.CString(), action.xmlData.Length());
            if (xmlFile.Load(buf))
            {
                XMLElement rootElem = xmlFile.GetRoot();
                Node* restored = scene_->CreateChild(String::EMPTY, LOCAL);
                restored->LoadXML(rootElem);
                restored->SetPosition(action.position);
                restored->SetRotation(action.rotation);
            }
        }
        break;

    case UndoAction::NODE_CREATE:
        {
            Node* node = scene_->GetNode(action.nodeID);
            if (node)
                node->Remove();
        }
        break;

    case UndoAction::TERRAIN_EDIT:
        if (action.beforeHM)
            RestoreHeightMap(action.beforeHM);
        break;
    }
}

void TerrainNode::Redo()
{
    if (undoCursor_ >= (int)undoStack_.Size())
        return;

    const UndoAction& action = undoStack_[undoCursor_];
    ++undoCursor_;

    switch (action.type)
    {
    case UndoAction::NODE_DELETE:
        {
            // Re-delete the node
            Node* node = scene_->GetNode(action.nodeID);
            if (node)
            {
                DeselectNode();
                node->Remove();
            }
        }
        break;

    case UndoAction::NODE_CREATE:
        {
            // Re-create the node
            XMLFile xmlFile(context_);
            MemoryBuffer buf(action.xmlData.CString(), action.xmlData.Length());
            if (xmlFile.Load(buf))
            {
                XMLElement rootElem = xmlFile.GetRoot();
                Node* restored = scene_->CreateChild(String::EMPTY, LOCAL);
                restored->LoadXML(rootElem);
                restored->SetPosition(action.position);
                restored->SetRotation(action.rotation);
            }
        }
        break;

    case UndoAction::TERRAIN_EDIT:
        if (action.afterHM)
            RestoreHeightMap(action.afterHM);
        break;
    }
}

String TerrainNode::SerializeNode(Node* node)
{
    XMLFile xmlFile(context_);
    XMLElement rootElem = xmlFile.CreateRoot("node");
    node->SaveXML(rootElem);
    return xmlFile.ToString();
}

SharedPtr<Image> TerrainNode::CloneHeightMap()
{
    if (brush_)
        return brush_->CloneHeightMap();

    return SharedPtr<Image>();
}

void TerrainNode::RestoreHeightMap(Image* src)
{
    if (brush_)
    {
        brush_->RestoreHeightMap(src);
        WakeSleepingBodiesOnTerrain();
    }
}

void TerrainNode::BeginTerrainStroke()
{
    if (!terrainStrokeActive_)
    {
        terrainStrokeBefore_ = CloneHeightMap();
        terrainStrokeActive_ = true;
    }
}

void TerrainNode::EndTerrainStroke()
{
    if (terrainStrokeActive_)
    {
        terrainStrokeActive_ = false;
        SharedPtr<Image> after = CloneHeightMap();

        if (terrainStrokeBefore_ && after)
        {
            UndoAction action;
            action.type = UndoAction::TERRAIN_EDIT;
            action.beforeHM = terrainStrokeBefore_;
            action.afterHM = after;
            PushUndo(action);
        }
        terrainStrokeBefore_.Reset();
    }
}

// ============================================================================
// Minimap
// ============================================================================

void TerrainNode::CreateMinimap()
{
    if (!terrain_ || !scene_)
        return;

    auto* ui = GetSubsystem<UI>();
    auto* uiRoot = ui->GetRoot();
    const int texSize = 256;
    const int displaySize = 192;

    // Create RTT texture
    minimapTex_ = new Texture2D(context_);
    minimapTex_->SetSize(texSize, texSize, Graphics::GetRGBFormat(), TEXTURE_RENDERTARGET);
    minimapTex_->SetFilterMode(FILTER_BILINEAR);

    // Create top-down orthographic camera
    minimapCameraNode_ = scene_->CreateChild("MinimapCamera", LOCAL);
    auto* minimapCam = minimapCameraNode_->CreateComponent<Camera>();
    minimapCam->SetOrthographic(true);
    minimapCam->SetOrthoSize(320.0f);  // world units visible (160 in each direction)
    minimapCam->SetFarClip(500.0f);
    minimapCam->SetNearClip(1.0f);
    minimapCam->SetFlipVertical(true);  // Vulkan RTT Y-flip compensation
    // Look straight down
    minimapCameraNode_->SetRotation(Quaternion(90.0f, 0.0f, 0.0f));

    // Set up RTT viewport
    RenderSurface* surface = minimapTex_->GetRenderSurface();
    SharedPtr<Viewport> minimapViewport(new Viewport(context_, scene_, minimapCam));
    minimapViewport->SetDrawDebug(false);
    surface->SetViewport(0, minimapViewport);
    surface->SetUpdateMode(SURFACE_UPDATEALWAYS);

    // BorderImage in bottom-right corner
    minimap_ = uiRoot->CreateChild<BorderImage>("Minimap");
    minimap_->SetTexture(minimapTex_);
    minimap_->SetImageRect(IntRect(0, 0, texSize, texSize));
    minimap_->SetFixedSize(displaySize, displaySize);
    minimap_->SetAlignment(HA_RIGHT, VA_BOTTOM);
    minimap_->SetPosition(-8, -8);
    minimap_->SetOpacity(0.85f);

    // Camera dot (always centered)
    minimapCameraDot_ = minimap_->CreateChild<BorderImage>("CameraDot");
    minimapCameraDot_->SetFixedSize(6, 6);
    minimapCameraDot_->SetColor(Color::RED);
    minimapCameraDot_->SetEnabled(false);
    minimapCameraDot_->SetPosition(displaySize / 2 - 3, displaySize / 2 - 3);
}

void TerrainNode::UpdateMinimapCamera()
{
    if (!minimapCameraNode_ || !cameraNode_)
        return;

    // Position minimap camera above the main camera, looking straight down
    Vector3 camPos = cameraNode_->GetWorldPosition();
    minimapCameraNode_->SetPosition(Vector3(camPos.x_, 200.0f, camPos.z_));
    // Rotate to match camera yaw so forward is always "up" on the minimap
    // +180 because at pitch=90 (looking down), screen "up" is -Z, but camera forward at yaw=0 is +Z
    minimapCameraNode_->SetRotation(Quaternion(90.0f, yaw_, 0.0f));
}

static void DrawLineOnImage(Image* img, int x0, int y0, int x1, int y1, const Color& color)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int w = img->GetWidth(), h = img->GetHeight();
    for (;;)
    {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
            img->SetPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

SharedPtr<Texture2D> TerrainNode::GenerateShapeIcon(int shape)
{
    const int sz = 24;
    const int cx = sz / 2, cy = sz / 2;
    const float r = 10.0f;
    Color white(1.0f, 1.0f, 1.0f, 1.0f);

    SharedPtr<Image> img(new Image(context_));
    img->SetSize(sz, sz, 4);
    // Clear to transparent
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x)
            img->SetPixel(x, y, Color(0, 0, 0, 0));

    // Compute vertices
    Vector<IntVector2> verts;

    switch (shape)
    {
    case 0: // Circle — draw using distance field for clean result
    {
        for (int y = 0; y < sz; ++y)
        {
            for (int x = 0; x < sz; ++x)
            {
                float dx = (float)(x - cx), dy = (float)(y - cy);
                float dist = sqrtf(dx * dx + dy * dy);
                // Anti-aliased ring: 1px wide at radius r
                float alpha = 1.0f - fabsf(dist - r);
                if (alpha > 0.0f)
                    img->SetPixel(x, y, Color(1.0f, 1.0f, 1.0f, Clamp(alpha, 0.0f, 1.0f)));
            }
        }
        // No polygon verts needed, skip line drawing
        SharedPtr<Texture2D> tex(new Texture2D(context_));
        tex->SetSize(sz, sz, Graphics::GetRGBAFormat());
        tex->SetData(img);
        tex->SetFilterMode(FILTER_BILINEAR);
        return tex;
    }
    case 1: // Square
        verts.Push(IntVector2(cx - (int)r, cy - (int)r));
        verts.Push(IntVector2(cx + (int)r, cy - (int)r));
        verts.Push(IntVector2(cx + (int)r, cy + (int)r));
        verts.Push(IntVector2(cx - (int)r, cy + (int)r));
        break;
    case 2: // Triangle (point-up)
    {
        float triAngles[3] = {-M_PI / 2.0f, M_PI * 5.0f / 6.0f, M_PI / 6.0f};
        for (int i = 0; i < 3; ++i)
            verts.Push(IntVector2(cx + (int)(cosf(triAngles[i]) * r), cy + (int)(sinf(triAngles[i]) * r)));
        break;
    }
    case 3: // Star (5-pointed)
    {
        float innerR = r * 0.38f;
        for (int i = 0; i < 10; ++i)
        {
            float a = -M_PI / 2.0f + (float)i / 10.0f * 2.0f * M_PI;
            float rad = (i % 2 == 0) ? r : innerR;
            verts.Push(IntVector2(cx + (int)(cosf(a) * rad), cy + (int)(sinf(a) * rad)));
        }
        break;
    }
    default: // 4=pentagon(5), 5=hexagon(6), 6=octagon(8)
    {
        int sides;
        switch (shape) { case 4: sides = 5; break; case 5: sides = 6; break; default: sides = 8; break; }
        for (int i = 0; i < sides; ++i)
        {
            float a = -M_PI / 2.0f + (float)i / (float)sides * 2.0f * M_PI;
            verts.Push(IntVector2(cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r)));
        }
        break;
    }
    }

    // Apply brush rotation to vertices
    if (brushRotation_ != 0.0f)
    {
        float ra = brushRotation_ * M_DEGTORAD;
        float cs = cosf(ra), sn = sinf(ra);
        for (unsigned i = 0; i < verts.Size(); ++i)
        {
            float lx = (float)(verts[i].x_ - cx);
            float ly = (float)(verts[i].y_ - cy);
            verts[i].x_ = cx + (int)(lx * cs - ly * sn);
            verts[i].y_ = cy + (int)(lx * sn + ly * cs);
        }
    }

    // Draw lines between vertices
    for (unsigned i = 0; i < verts.Size(); ++i)
    {
        unsigned j = (i + 1) % verts.Size();
        DrawLineOnImage(img, verts[i].x_, verts[i].y_, verts[j].x_, verts[j].y_, white);
    }

    SharedPtr<Texture2D> tex(new Texture2D(context_));
    tex->SetSize(sz, sz, Graphics::GetRGBAFormat());
    tex->SetData(img);
    tex->SetFilterMode(FILTER_NEAREST);
    return tex;
}

// ============================================================================
// Camera & Input
// ============================================================================

void TerrainNode::MoveCamera(float timeStep)
{
    auto* input = GetSubsystem<Input>();

    // Numpad Enter = toggle all UI and debug draw visibility
    if (input->GetKeyPress(KEY_KP_ENTER))
    {
        auto* uiRoot = GetSubsystem<UI>()->GetRoot();
        uiRoot->SetVisible(!uiRoot->IsVisible());
        drawDebug_ = uiRoot->IsVisible();
    }

    // F1 = cycle camera mode: editor → third person → first person
    if (input->GetKeyPress(KEY_F1))
    {
        static const char* modeNames[] = {"Free-fly Editor", "Third Person", "First Person", "Free Flight"};
        // Cycle: GOD → CHASE → FIRSTPERSON → GOD (skip FREEFLIGHT)
        if (cameraMode_ == CAM_GOD)
            cameraMode_ = CAM_CHASE;
        else if (cameraMode_ == CAM_CHASE)
            cameraMode_ = CAM_FIRSTPERSON;
        else
            cameraMode_ = CAM_GOD;
        URHO3D_LOGINFOF("Camera mode: %s", modeNames[cameraMode_]);
    }

    // Tab = toggle cursor/camera mode (checked before UI focus guard)
    if (input->GetKeyPress(KEY_TAB))
    {
        menuOpen_ = !menuOpen_;
        if (menuOpen_)
        {
            input->SetMouseMode(MM_FREE);
            input->SetMouseVisible(true);
            useMouseMode_ = MM_FREE;
        }
        else
        {
            GetSubsystem<UI>()->SetFocusElement(nullptr);
            input->SetMouseMode(MM_RELATIVE);
            input->SetMouseVisible(false);
            useMouseMode_ = MM_RELATIVE;
        }
    }

    // [ and ] = cycle brush shape (before focus guard so it works with terrain panel open)
    {
        static const char* shapeNames[] = {"Circle", "Square", "Triangle", "Star", "Pentagon", "Hexagon", "Octagon"};
        if (input->GetKeyPress(KEY_LEFTBRACKET))
        {
            brushShape_ = (brushShape_ + 6) % 7;
            URHO3D_LOGINFOF("Brush shape: %s (%d)", shapeNames[brushShape_], brushShape_);
        }
        if (input->GetKeyPress(KEY_RIGHTBRACKET))
        {
            brushShape_ = (brushShape_ + 1) % 7;
            URHO3D_LOGINFOF("Brush shape: %s (%d)", shapeNames[brushShape_], brushShape_);
        }
    }

    // Click outside dismisses floating panels (terrain tools, generate mesh)
    if (input->GetMouseButtonPress(MOUSEB_LEFT) || input->GetMouseButtonPress(MOUSEB_RIGHT))
    {
        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
        if (!uiElem)
        {
            if (terrainPanel_ && terrainPanel_->IsVisible())
                terrainPanel_->SetVisible(false);
            if (generateMeshPanel_ && generateMeshPanel_->IsVisible())
                generateMeshPanel_->SetVisible(false);
        }
    }

    if (GetSubsystem<UI>()->GetFocusElement())
        return;

    // Hotkeys (Space = debug toggle only in editor mode; in character modes it's jump)
    if (input->GetKeyPress(KEY_SPACE) && cameraMode_ == CAM_GOD)
        drawDebug_ = !drawDebug_;

    if (input->GetKeyPress(KEY_Z))
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_F))
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_H) && zone_)
    {
        bool on = !zone_->GetHeightFog();
        zone_->SetHeightFog(on);
        heightFogOverride_ = on ? 1 : -1;
    }

    if (input->GetKeyPress(KEY_F5))
        drawDebug_ = !drawDebug_;

    if (input->GetKeyPress(KEY_F11))
        GetSubsystem<Graphics>()->ToggleFullscreen();

    // Undo/Redo
    if ((input->GetQualifiers() & QUAL_CTRL) && input->GetKeyPress(KEY_Z))
        Undo();
    if ((input->GetQualifiers() & QUAL_CTRL) && input->GetKeyPress(KEY_Y))
        Redo();

    // Gizmo mode shortcuts — only in editor mode, wake physics when exiting the tool entirely
    if (cameraMode_ == CAM_GOD && input->GetKeyPress(KEY_T))
    {
        int prev = gizmoMode_;
        gizmoMode_ = (prev == 1) ? 0 : 1;
        if (prev != 0 && gizmoMode_ == 0)
            WakeSelectedNode();
    }
    if (cameraMode_ == CAM_GOD && input->GetKeyPress(KEY_R))
    {
        int prev = gizmoMode_;
        gizmoMode_ = (prev == 2) ? 0 : 2;
        if (prev != 0 && gizmoMode_ == 0)
            WakeSelectedNode();
    }
    if (cameraMode_ == CAM_GOD && input->GetKeyPress(KEY_S))
    {
        int prev = gizmoMode_;
        gizmoMode_ = (prev == 3) ? 0 : 3;
        if (prev != 0 && gizmoMode_ == 0)
            WakeSelectedNode();
    }
    if (cameraMode_ == CAM_GOD && input->GetKeyPress(KEY_G))
    {
        gizmoLocal_ = !gizmoLocal_;
        // Update edit menu label
        if (editMenu_ && editMenu_->GetNumItems() > 5)
        {
            auto* item = editMenu_->GetItem(5);
            auto* label = item ? item->GetChildStaticCast<Text>(0) : nullptr;
            if (label)
                label->SetText(gizmoLocal_ ? "Local (G)" : "World (G)");
        }
    }

    if (cameraMode_ == CAM_GOD && (input->GetKeyPress(KEY_BACKSPACE) || input->GetKeyPress(KEY_DELETE)) && selectedNode_ && !selectedNode_.Expired())
    {
        Node* node = selectedNode_;
        // Record undo before deleting
        UndoAction action;
        action.type = UndoAction::NODE_DELETE;
        action.nodeID = node->GetID();
        action.xmlData = SerializeNode(node);
        action.position = node->GetPosition();
        action.rotation = node->GetRotation();
        PushUndo(action);

        // Send object delete to server before removing locally
        SendObjectDelete(node->GetID());

        DeselectNode();
        node->Remove();
    }

    // 1/2 = track sun/moon
    if (input->GetKeyPress(KEY_1))
    {
        if (sunNode_)
        {
            Vector3 dir = (sunNode_->GetPosition() - cameraNode_->GetPosition()).Normalized();
            yaw_ = atan2f(dir.x_, dir.z_) * M_RADTODEG;
            pitch_ = asinf(Clamp(dir.y_, -1.0f, 1.0f)) * M_RADTODEG;
            cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
        }
    }
    if (input->GetKeyPress(KEY_2))
    {
        if (moonNode_)
        {
            Vector3 dir = (moonNode_->GetPosition() - cameraNode_->GetPosition()).Normalized();
            yaw_ = atan2f(dir.x_, dir.z_) * M_RADTODEG;
            pitch_ = asinf(Clamp(dir.y_, -1.0f, 1.0f)) * M_RADTODEG;
            cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
        }
    }

    // Scroll wheel: tool active = adjust brush radius, editor mode = inch camera forward/back
    int wheel = input->GetMouseMoveWheel();
    if (wheel != 0)
    {
        if (brushMode_ != 0 && cameraMode_ == CAM_GOD)
            brushRadius_ = Clamp(brushRadius_ + wheel * 0.25f, 0.25f, 50.0f);
        else if (cameraMode_ == CAM_GOD)
            cameraNode_->Translate(Vector3::FORWARD * (float)wheel * 0.5f);
    }

    // Mouse look (when menu closed) — all camera modes use yaw/pitch
    if (!menuOpen_)
    {
        const float MOUSE_SENSITIVITY = 0.1f;
        IntVector2 mouseMove = input->GetMouseMove();
        yaw_ += MOUSE_SENSITIVITY * mouseMove.x_;
        pitch_ += MOUSE_SENSITIVITY * mouseMove.y_;
        pitch_ = Clamp(pitch_, -90.0f, 90.0f);
    }

    // Camera positioning depends on mode
    if (cameraMode_ == CAM_GOD)
    {
        // Editor mode: free-fly camera with WASD + sphere cast collision
        cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));

        const float MOVE_SPEED = 20.0f;
        Vector3 oldPos = cameraNode_->GetPosition();

        if (input->GetKeyDown(KEY_W))
            cameraNode_->Translate(Vector3::FORWARD * MOVE_SPEED * timeStep);
        if (input->GetKeyDown(KEY_S))
            cameraNode_->Translate(Vector3::BACK * MOVE_SPEED * timeStep);
        if (input->GetKeyDown(KEY_A))
            cameraNode_->Translate(Vector3::LEFT * MOVE_SPEED * timeStep);
        if (input->GetKeyDown(KEY_D))
            cameraNode_->Translate(Vector3::RIGHT * MOVE_SPEED * timeStep);

        // Sphere cast collision
        Vector3 newPos = cameraNode_->GetPosition();
        Vector3 moveVec = newPos - oldPos;
        float moveDist = moveVec.Length();
        if (moveDist > 0.001f)
        {
            auto* physicsWorld = scene_->GetComponent<PhysicsWorld>();
            if (physicsWorld)
            {
                const float CAMERA_RADIUS = 0.5f;
                Vector3 moveNorm = moveVec / moveDist;
                PhysicsRaycastResult result;
                physicsWorld->SphereCast(result, Ray(oldPos, moveNorm), CAMERA_RADIUS, moveDist);
                if (result.body_)
                {
                    Vector3 hitPos = oldPos + moveNorm * Max(result.distance_ - 0.01f, 0.0f);
                    float remaining = moveDist - result.distance_;
                    if (remaining > 0.001f)
                    {
                        Vector3 slideVec = moveVec - result.normal_ * moveVec.DotProduct(result.normal_);
                        float slideLen = slideVec.Length();
                        if (slideLen > 0.001f)
                        {
                            Vector3 slideNorm = slideVec / slideLen;
                            float slideDist = Min(slideLen, remaining);
                            PhysicsRaycastResult slideResult;
                            physicsWorld->SphereCast(slideResult, Ray(hitPos, slideNorm), CAMERA_RADIUS, slideDist);
                            if (slideResult.body_)
                                hitPos += slideNorm * Max(slideResult.distance_ - 0.01f, 0.0f);
                            else
                                hitPos += slideNorm * slideDist;
                        }
                    }
                    cameraNode_->SetPosition(hitPos);
                }
            }
        }

        // Clamp camera above terrain
        if (terrain_)
        {
            Vector3 camPos = cameraNode_->GetPosition();
            float terrainHeight = terrain_->GetHeight(camPos) + 1.0f;
            if (camPos.y_ < terrainHeight)
            {
                camPos.y_ = terrainHeight;
                cameraNode_->SetPosition(camPos);
            }
        }
    }
    else
    {
        // First person / third person: camera follows character
        UpdateCharacterCamera();
    }

    // Terrain brush raycast — only in editor camera mode
    hasBrushHit_ = false;
    bool showBrush = cameraMode_ == CAM_GOD && ((brushMode_ != 0) || (terrainPanel_ && terrainPanel_->IsVisible()) || prefabBrush_);
    if (terrain_ && showBrush)
    {
        auto* camera = cameraNode_->GetComponent<Camera>();
        auto* graphics = GetSubsystem<Graphics>();
        if (camera && graphics)
        {
            Ray cameraRay;
            if (menuOpen_)
            {
                IntVector2 pos = input->GetMousePosition();
                cameraRay = camera->GetScreenRay(
                    (float)pos.x_ / (float)graphics->GetWidth(),
                    (float)pos.y_ / (float)graphics->GetHeight());
            }
            else
            {
                cameraRay = camera->GetScreenRay(0.5f, 0.5f);
            }

            auto* octree = scene_->GetComponent<Octree>();
            if (octree)
            {
                Vector<RayQueryResult> results;
                RayOctreeQuery query(results, cameraRay, RAY_TRIANGLE, 750.0f);
                octree->Raycast(query);
                for (unsigned i = 0; i < results.Size(); ++i)
                {
                    if (!dynamic_cast<TerrainPatch*>(results[i].drawable_))
                        continue;

                    cachedBrushHit_ = results[i].position_;
                    cachedBrushNormal_ = results[i].normal_;
                    hasBrushHit_ = true;

                    // Don't paint if mouse is over a UI element (sliders, panels, menus)
                    {
                        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
                        if (uiElem)
                            break;
                    }

                    if (brushMode_ == 1)
                    {
                        if (input->GetMouseButtonDown(MOUSEB_LEFT))
                        {
                            BeginTerrainStroke();
                            ApplyBrush(cachedBrushHit_, timeStep);
                        }
                        if (input->GetMouseButtonDown(MOUSEB_RIGHT))
                        {
                            BeginTerrainStroke();
                            ApplyLowerBrush(cachedBrushHit_, timeStep);
                        }
                    }
                    else
                    {
                        if (input->GetMouseButtonDown(MOUSEB_LEFT) || input->GetMouseButtonDown(MOUSEB_RIGHT))
                        {
                            BeginTerrainStroke();
                            ApplyBrush(cachedBrushHit_, timeStep);
                        }
                    }
                    break;
                }
            }
        }
    }

    // Prefab brush instancing — clone on left click
    if (prefabBrush_ && hasBrushHit_ && input->GetMouseButtonPress(MOUSEB_LEFT))
    {
        // Don't instance if mouse is over UI
        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
        if (!uiElem)
        {
            Node* instance = prefabBrush_->Clone();
            instance->SetEnabledRecursive(true);

            // Force collision shapes to rebuild (may not have been created while disabled)
            Vector<CollisionShape*> shapes;
            instance->GetDerivedComponents<CollisionShape>(shapes, true);
            for (unsigned s = 0; s < shapes.Size(); ++s)
                shapes[s]->ApplyAttributes();

            // Set up mass before positioning so body is in correct state when placed
            Vector<RigidBody*> bodies;
            instance->GetDerivedComponents<RigidBody>(bodies, true);
            for (unsigned b = 0; b < bodies.Size(); ++b)
            {
                RigidBody* rb = bodies[b];
                if (rb->GetMass() == 0.0f)
                {
                    rb->SetMass(100.0f);
                    rb->SetFriction(0.75f);
                    rb->SetLinearDamping(0.9f);
                    rb->SetAngularDamping(0.9f);
                    SubscribeToEvent(instance, E_RIGIDBODYSLEEP, URHO3D_HANDLER(TerrainNode, HandleRigidBodySleep));
                }
            }

            // Orient clone to terrain surface normal
            Quaternion surfaceRot;
            surfaceRot.FromRotationTo(Vector3::UP, cachedBrushNormal_);
            instance->SetRotation(surfaceRot);

            // Offset along surface normal so the object sits on the terrain
            float yOffset = 0.0f;
            {
                Vector<Drawable*> drawables;
                instance->GetDerivedComponents<Drawable>(drawables, true);
                BoundingBox combined;
                for (unsigned d = 0; d < drawables.Size(); ++d)
                    combined.Merge(drawables[d]->GetBoundingBox());
                if (combined.Defined())
                    yOffset = -combined.min_.y_ * instance->GetScale().y_;
            }
            instance->SetPosition(cachedBrushHit_ + cachedBrushNormal_ * (yOffset + 0.2f));

            // Activate after positioning so Bullet sees the correct initial transform
            for (unsigned b = 0; b < bodies.Size(); ++b)
                bodies[b]->Activate();

            URHO3D_LOGINFOF("Prefab instanced at (%.1f, %.1f, %.1f)", cachedBrushHit_.x_, cachedBrushHit_.y_, cachedBrushHit_.z_);

            // Send object create to server
            SendObjectCreate(instance, cachedBrushHit_, cachedBrushNormal_);
        }
    }

    // End terrain stroke on mouse release
    if (terrainStrokeActive_ && !input->GetMouseButtonDown(MOUSEB_LEFT) && !input->GetMouseButtonDown(MOUSEB_RIGHT))
        EndTerrainStroke();

    // Gizmo drag handling
    if (gizmoDragging_ && input->GetMouseButtonDown(MOUSEB_LEFT))
    {
        UpdateGizmoDrag();
    }
    else if (gizmoDragging_ && !input->GetMouseButtonDown(MOUSEB_LEFT))
    {
        EndGizmoDrag();
    }

    // Gizmo picking — screen-space, only in cursor mode (TAB)
    if (menuOpen_ && gizmoMode_ != 0 && selectedNode_ && !selectedNode_.Expired() &&
        brushMode_ == 0 && input->GetMouseButtonPress(MOUSEB_LEFT) && !gizmoDragging_)
    {
        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
        if (!uiElem)
        {
            auto* camera = cameraNode_->GetComponent<Camera>();
            auto* graphics = GetSubsystem<Graphics>();
            IntVector2 mousePos = input->GetMousePosition();
            float w = (float)graphics->GetWidth();
            float h = (float)graphics->GetHeight();
            float mx = (float)mousePos.x_;
            float my = (float)mousePos.y_;

            Vector3 nodePos = selectedNode_->GetWorldPosition();
            float dist = (nodePos - cameraNode_->GetWorldPosition()).Length();
            float len = dist * 0.15f;

            // Project node origin to screen
            Vector2 screenOrigin = camera->WorldToScreenPoint(nodePos);
            float ox = screenOrigin.x_ * w;
            float oy = screenOrigin.y_ * h;

            if (gizmoMode_ == 2) // Rotate — arcball, click anywhere near object
            {
                float screenDist = sqrtf((mx - ox) * (mx - ox) + (my - oy) * (my - oy));
                // Accept click within ~2x the gizmo ring radius on screen
                Vector2 screenRingEdge = camera->WorldToScreenPoint(nodePos + Vector3::RIGHT * len);
                float ringScreenRadius = sqrtf((screenRingEdge.x_ * w - ox) * (screenRingEdge.x_ * w - ox) +
                                               (screenRingEdge.y_ * h - oy) * (screenRingEdge.y_ * h - oy));
                if (screenDist < ringScreenRadius * 2.0f)
                {
                    BeginGizmoDrag(0);  // axis ignored for arcball
                }
            }
            else // Translate/Scale — axis picking
            {
                Vector3 axes[3];
                if (gizmoLocal_)
                {
                    Quaternion rot = selectedNode_->GetWorldRotation();
                    axes[0] = rot * Vector3::RIGHT;
                    axes[1] = rot * Vector3::UP;
                    axes[2] = rot * Vector3::FORWARD;
                }
                else
                {
                    axes[0] = Vector3::RIGHT;
                    axes[1] = Vector3::UP;
                    axes[2] = Vector3::FORWARD;
                }

                int bestAxis = -1;
                float bestDist = 15.0f;  // 15 pixel threshold

                for (int a = 0; a < 3; ++a)
                {
                    Vector3 axisEnd = nodePos + axes[a] * len;
                    Vector2 screenEnd = camera->WorldToScreenPoint(axisEnd);
                    float ex = screenEnd.x_ * w;
                    float ey = screenEnd.y_ * h;

                    float segDx = ex - ox;
                    float segDy = ey - oy;
                    float segLen2 = segDx * segDx + segDy * segDy;
                    if (segLen2 < 4.0f) continue;

                    float t = ((mx - ox) * segDx + (my - oy) * segDy) / segLen2;
                    t = Clamp(t, 0.0f, 1.0f);
                    float closestX = ox + segDx * t;
                    float closestY = oy + segDy * t;
                    float d = sqrtf((mx - closestX) * (mx - closestX) + (my - closestY) * (my - closestY));

                    if (d < bestDist && t > 0.1f)
                    {
                        bestDist = d;
                        bestAxis = a;
                    }
                }

                if (bestAxis >= 0)
                {
                    BeginGizmoDrag(bestAxis);
                }
            }
        }
    }

    // Object selection raycast (when no brush mode active, and not gizmo dragging)
    if (brushMode_ == 0 && !gizmoDragging_ && input->GetMouseButtonPress(MOUSEB_LEFT))
    {
        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
        if (!uiElem)
        {
            auto* camera = cameraNode_->GetComponent<Camera>();
            auto* graphics = GetSubsystem<Graphics>();
            if (camera && graphics)
            {
                Ray pickRay;
                if (menuOpen_)
                {
                    IntVector2 pos = input->GetMousePosition();
                    pickRay = camera->GetScreenRay(
                        (float)pos.x_ / (float)graphics->GetWidth(),
                        (float)pos.y_ / (float)graphics->GetHeight());
                }
                else
                {
                    pickRay = camera->GetScreenRay(0.5f, 0.5f);
                }

                auto* physicsWorld = scene_->GetComponent<PhysicsWorld>();
                if (physicsWorld)
                {
                    Vector<PhysicsRaycastResult> results;
                    physicsWorld->Raycast(results, pickRay, 300.0f);

                    Node* hitNode = nullptr;
                    for (unsigned i = 0; i < results.Size(); ++i)
                    {
                        Node* node = results[i].body_->GetNode();
                        if (node && !node->HasComponent<Terrain>())
                        {
                            hitNode = node;
                            break;
                        }
                    }

                    if (hitNode)
                        SelectNode(hitNode);
                    else
                        DeselectNode();
                }
            }
        }
    }

    // Reset flatten lock when mouse released
    if (brushMode_ == 4 && !input->GetMouseButtonDown(MOUSEB_LEFT) && !input->GetMouseButtonDown(MOUSEB_RIGHT))
        lockedFlattenHeight_ = -1.0f;

    // Reflection camera aspect ratio
    if (reflectionCameraNode_)
    {
        auto* graphics = GetSubsystem<Graphics>();
        auto* reflectionCamera = reflectionCameraNode_->GetComponent<Camera>();
        if (reflectionCamera)
            reflectionCamera->SetAspectRatio((float)graphics->GetWidth() / (float)graphics->GetHeight());
    }

    // Update camera coordinates display
    if (cameraCoordsText_ && cameraNode_)
    {
        Vector3 p = cameraNode_->GetWorldPosition();
        char buf[64];
        snprintf(buf, sizeof(buf), "X:%.1f  Y:%.1f  Z:%.1f", p.x_, p.y_, p.z_);
        cameraCoordsText_->SetText(buf);
    }

    // Update Melbourne clock display
    if (clockText_)
    {
        static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        static const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int hour = (int)timeOfDay_;
        int minute = (int)((timeOfDay_ - hour) * 60.0f);
        if (hour < 0) hour = 0;
        if (hour > 23) hour = 23;
        if (minute < 0) minute = 0;
        if (minute > 59) minute = 59;

        // Convert dayOfYear_ (1-365) to day + month
        int remaining = Clamp(dayOfYear_, 1, 365) - 1;
        int month = 0;
        for (int m = 0; m < 12; ++m)
        {
            if (remaining < daysInMonth[m])
            {
                month = m;
                break;
            }
            remaining -= daysInMonth[m];
        }
        int day = remaining + 1;

        char clockBuf[32];
        snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d  %d %s", hour, minute, day, monthNames[month]);
        clockText_->SetText(clockBuf);
    }
}

// ============================================================================
// Terrain Brush
// ============================================================================

// BrushShapeFalloff logic moved to TerrainBrush::ShapeFalloff().
// DrawBrushOutline builds its own outline vertices independently.

void TerrainNode::ApplyBrush(const Vector3& worldPos, float timeStep)
{
    if (!terrain_ || !editableHeightMap_ || brushMode_ == 0 || !brush_)
        return;

    // Client-side patch ownership check (admins exempt, broadcasts exempt)
    if (adminLevel_ == 0 && !replayingBroadcast_)
    {
        const float patchWorldSize = 128.0f;
        int bpx = (int)floorf(worldPos.x_ / patchWorldSize);
        int bpz = (int)floorf(worldPos.z_ / patchWorldSize);
        if (!OwnsThisPatch(bpx, bpz))
        {
            URHO3D_LOGINFOF("ApplyBrush REJECTED: worldPos=(%.1f,%.1f,%.1f) patch=(%d,%d) ownedPatches=%u adminLevel=%d",
                worldPos.x_, worldPos.y_, worldPos.z_, bpx, bpz, ownedPatches_.Size(), adminLevel_);
            return;
        }
    }

    // Sync brush parameters from UI state
    brush_->SetMode(brushMode_);
    brush_->SetShape(brushShape_);
    brush_->SetRadius(brushRadius_);
    brush_->SetStrength(brushStrength_);
    brush_->SetSmoothStrength(smoothStrength_);
    brush_->SetRotation(brushRotation_);
    brush_->SetFlattenHeight(lockedFlattenHeight_);

    // Delegate to shared brush
    bool modified = brush_->Apply(worldPos, timeStep);

    // Read back flatten height (may have been locked by brush)
    if (brushMode_ == 4)
        lockedFlattenHeight_ = brush_->GetFlattenHeight();

    if (!modified)
        return;

    // Re-upload water map to GPU after river brush edits
    if (brushMode_ == 6 && waterMapTex_ && waterMap_)
        waterMapTex_->SetData(waterMap_);

    // Send terrain edit to server (rate-limited)
    SendTerrainEdit(worldPos, timeStep);

    Vector3 spacing = terrain_->GetSpacing();
    float worldRadius = brushRadius_ * spacing.x_ + 2.0f;
    auto* physicsWorld = scene_->GetComponent<PhysicsWorld>();
    if (physicsWorld)
    {
        Vector<RigidBody*> bodies;
        physicsWorld->GetRigidBodies(bodies, Sphere(worldPos, worldRadius));
        for (unsigned i = 0; i < bodies.Size(); ++i)
            bodies[i]->Activate();
    }
}

void TerrainNode::ApplyLowerBrush(const Vector3& worldPos, float timeStep)
{
    int saved = brushMode_;
    brushMode_ = 2;
    ApplyBrush(worldPos, timeStep);
    brushMode_ = saved;
}

// ---------------------------------------------------------------------------
// Camera transition (edit mode ↔ play mode)
// ---------------------------------------------------------------------------

void TerrainNode::BeginEditTransition()
{
    if (!cameraNode_ || cameraTransitioning_)
        return;

    preEditCameraMode_ = cameraMode_;
    transitionToEdit_ = true;
    cameraTransitioning_ = true;
    transitionTime_ = 0.0f;

    // Start from current camera state
    transitionStartPos_ = cameraNode_->GetPosition();
    transitionStartRot_ = cameraNode_->GetRotation();

    // Target: 66% up, 33% back from current position
    // "Back" = opposite of camera's forward direction projected onto XZ plane
    Vector3 forward = cameraNode_->GetDirection();
    Vector3 backXZ(-forward.x_, 0.0f, -forward.z_);
    if (backXZ.Length() < 0.001f)
        backXZ = Vector3::BACK;  // looking straight down, default to -Z
    backXZ.Normalize();

    float upDist = 20.0f;    // 66% of the offset goes up
    float backDist = 10.0f;  // 33% goes backward

    transitionEndPos_ = transitionStartPos_ + Vector3::UP * upDist + backXZ * backDist;

    // Orient camera to look at the original look-at target
    // The target is where the camera was pointing at the terrain
    Vector3 lookTarget = transitionStartPos_ + forward * 30.0f;  // approximate
    if (terrain_)
    {
        // Raycast to find the actual terrain point we were looking at
        auto* camera = cameraNode_->GetComponent<Camera>();
        if (camera)
        {
            Ray ray = camera->GetScreenRay(0.5f, 0.5f);
            auto* octree = scene_ ? scene_->GetComponent<Octree>() : nullptr;
            if (octree)
            {
                Vector<RayQueryResult> results;
                RayOctreeQuery query(results, ray, RAY_TRIANGLE, 500.0f);
                octree->Raycast(query);
                for (unsigned i = 0; i < results.Size(); ++i)
                {
                    if (dynamic_cast<TerrainPatch*>(results[i].drawable_))
                    {
                        lookTarget = results[i].position_;
                        break;
                    }
                }
            }
        }
    }

    // Calculate rotation that looks from end position toward the target
    Vector3 dir = (lookTarget - transitionEndPos_).Normalized();
    transitionEndRot_.FromLookRotation(dir);
}

void TerrainNode::EndEditTransition()
{
    if (!cameraNode_ || cameraTransitioning_)
        return;

    transitionToEdit_ = false;
    cameraTransitioning_ = true;
    transitionTime_ = 0.0f;

    // Start from current god-cam position
    transitionStartPos_ = cameraNode_->GetPosition();
    transitionStartRot_ = cameraNode_->GetRotation();

    // Target: back to where the avatar is (chase position)
    if (characterNode_ && !characterNode_.Expired())
    {
        Node* charNode = characterNode_;
        Vector3 charPos = charNode->GetPosition();

        if (preEditCameraMode_ == CAM_FIRSTPERSON)
        {
            transitionEndPos_ = charPos + Vector3::UP * 1.7f;  // eye height
            transitionEndRot_.FromLookRotation(charNode->GetDirection());
        }
        else
        {
            // Chase cam: behind and above
            Vector3 charDir = charNode->GetDirection();
            transitionEndPos_ = charPos - charDir * 5.0f + Vector3::UP * 3.0f;
            Vector3 lookDir = (charPos + Vector3::UP * 1.5f - transitionEndPos_).Normalized();
            transitionEndRot_.FromLookRotation(lookDir);
        }
    }
    else
    {
        // No avatar — just stay put and restore mode
        transitionEndPos_ = transitionStartPos_;
        transitionEndRot_ = transitionStartRot_;
    }
}

void TerrainNode::UpdateCameraTransition(float timeStep)
{
    if (!cameraTransitioning_ || !cameraNode_)
        return;

    transitionTime_ += timeStep;
    float t = Clamp(transitionTime_ / transitionDuration_, 0.0f, 1.0f);

    // Smooth ease-in-out
    float smooth = t * t * (3.0f - 2.0f * t);

    cameraNode_->SetPosition(transitionStartPos_.Lerp(transitionEndPos_, smooth));
    cameraNode_->SetRotation(transitionStartRot_.Slerp(transitionEndRot_, smooth));

    if (t >= 1.0f)
    {
        cameraTransitioning_ = false;
        if (transitionToEdit_)
        {
            cameraMode_ = CAM_GOD;
            // Set yaw/pitch from final rotation so god-cam continues smoothly
            Vector3 euler = transitionEndRot_.EulerAngles();
            pitch_ = euler.x_;
            yaw_ = euler.y_;
        }
        else
        {
            cameraMode_ = preEditCameraMode_;
        }
    }
}

// ---------------------------------------------------------------------------
// Patch boundary visualization
// ---------------------------------------------------------------------------

void TerrainNode::CreatePatchBoundary(Node*& node, int patchX, int patchZ, const Color& color)
{
    if (!scene_ || !terrain_)
        return;

    // Remove old boundary node if it exists
    if (node)
    {
        node->Remove();
        node = nullptr;
    }

    const float patchWorldSize = 128.0f;
    float x0 = patchX * patchWorldSize;
    float z0 = patchZ * patchWorldSize;
    float x1 = x0 + patchWorldSize;
    float z1 = z0 + patchWorldSize;
    const float yOffset = 0.3f;
    const float ribbonWidth = 1.5f;  // world units wide
    const int segments = 32;

    // Build a loop of points around the patch boundary
    Vector<Vector3> loop;
    // Edge 1: x0,z0 → x1,z0
    for (int i = 0; i <= segments; ++i)
    {
        float x = x0 + (x1 - x0) * i / (float)segments;
        float y = terrain_->GetHeight(Vector3(x, 0, z0)) + yOffset;
        loop.Push(Vector3(x, y, z0));
    }
    // Edge 2: x1,z0 → x1,z1
    for (int i = 1; i <= segments; ++i)
    {
        float z = z0 + (z1 - z0) * i / (float)segments;
        float y = terrain_->GetHeight(Vector3(x1, 0, z)) + yOffset;
        loop.Push(Vector3(x1, y, z));
    }
    // Edge 3: x1,z1 → x0,z1
    for (int i = 1; i <= segments; ++i)
    {
        float x = x1 + (x0 - x1) * i / (float)segments;
        float y = terrain_->GetHeight(Vector3(x, 0, z1)) + yOffset;
        loop.Push(Vector3(x, y, z1));
    }
    // Edge 4: x0,z1 → x0,z0
    for (int i = 1; i < segments; ++i)
    {
        float z = z1 + (z0 - z1) * i / (float)segments;
        float y = terrain_->GetHeight(Vector3(x0, 0, z)) + yOffset;
        loop.Push(Vector3(x0, y, z));
    }

    // Build triangle strip ribbon — inner and outer edges offset perpendicular to boundary
    node = scene_->CreateChild("PatchBoundary");
    auto* cg = node->CreateComponent<CustomGeometry>();
    cg->SetNumGeometries(1);
    cg->BeginGeometry(0, TRIANGLE_STRIP);

    unsigned n = loop.Size();
    float halfW = ribbonWidth * 0.5f;
    for (unsigned i = 0; i <= n; ++i)
    {
        unsigned idx = i % n;
        unsigned next = (i + 1) % n;
        // Tangent along the boundary
        Vector3 tangent = (loop[next] - loop[idx]).Normalized();
        // Outward normal = tangent cross UP (points inward toward patch center)
        Vector3 outward = tangent.CrossProduct(Vector3::UP).Normalized();

        Vector3 p = loop[idx];
        // Outer vertex
        cg->DefineVertex(p + outward * halfW);
        cg->DefineColor(color);
        cg->DefineNormal(Vector3::UP);
        // Inner vertex
        cg->DefineVertex(p - outward * halfW);
        cg->DefineColor(color);
        cg->DefineNormal(Vector3::UP);
    }

    cg->SetMaterial(GetSubsystem<ResourceCache>()->GetResource<Material>("Materials/VColUnlit.xml"));
    cg->Commit();
    cg->SetOccludee(false);
    cg->SetCastShadows(false);
}

bool TerrainNode::OwnsThisPatch(int px, int pz) const
{
    for (unsigned i = 0; i < ownedPatches_.Size(); ++i)
    {
        if (ownedPatches_[i].x_ == px && ownedPatches_[i].y_ == pz)
            return true;
    }
    return false;
}

void TerrainNode::CreateOwnedPatchBoundaries()
{
    // Remove old boundaries
    for (unsigned i = 0; i < ownedPatchBoundaries_.Size(); ++i)
    {
        if (ownedPatchBoundaries_[i])
            ownedPatchBoundaries_[i]->Remove();
    }
    ownedPatchBoundaries_.Clear();

    URHO3D_LOGINFOF("CreateOwnedPatchBoundaries: %u patches, terrain_=%s, scene_=%s",
        ownedPatches_.Size(), terrain_ ? "yes" : "NULL", scene_ ? "yes" : "NULL");

    // Create gold boundary for each owned patch
    for (unsigned i = 0; i < ownedPatches_.Size(); ++i)
    {
        Node* node = nullptr;
        CreatePatchBoundary(node, ownedPatches_[i].x_, ownedPatches_[i].y_, Color(1.0f, 0.84f, 0.0f, 1.0f));  // gold = owned
        URHO3D_LOGINFOF("  patch (%d,%d) -> node=%s", ownedPatches_[i].x_, ownedPatches_[i].y_, node ? "created" : "NULL");
        ownedPatchBoundaries_.Push(node);
    }
}

void TerrainNode::UpdateCurrentPatchBoundary()
{
    if (!cameraNode_ || !terrain_)
        return;

    const float patchWorldSize = 128.0f;
    Vector3 camPos = cameraNode_->GetPosition();
    int px = (int)floorf(camPos.x_ / patchWorldSize);
    int pz = (int)floorf(camPos.z_ / patchWorldSize);

    if (px == currentPatchX_ && pz == currentPatchZ_)
        return;  // still on the same patch

    currentPatchX_ = px;
    currentPatchZ_ = pz;

    // Notify server of patch change for resource streaming
    if (loggedIn_ && (px != lastReportedPatchX_ || pz != lastReportedPatchZ_))
        SendPatchPosition(px, pz);

    // Don't draw current-patch boundary if it's an owned patch (already shown in green)
    if (OwnsThisPatch(px, pz))
    {
        if (currentPatchBoundary_)
        {
            currentPatchBoundary_->Remove();
            currentPatchBoundary_ = nullptr;
        }
        return;
    }

    CreatePatchBoundary(currentPatchBoundary_, px, pz, Color(1.0f, 1.0f, 1.0f, 1.0f));  // white = current (not owned)
}

void TerrainNode::DrawBrushOutline(const Vector3& worldPos)
{
    auto* debug = scene_->GetComponent<DebugRenderer>();
    if (!debug || !terrain_)
        return;

    Vector3 spacing = terrain_->GetSpacing();
    float worldRadius = brushRadius_ * spacing.x_;

    Color brushColor = Color::GREEN;
    if (brushMode_ == 2) brushColor = Color::RED;
    else if (brushMode_ == 3) brushColor = Color::CYAN;
    else if (brushMode_ == 4) brushColor = Color(1.0f, 0.5f, 0.0f);
    else if (brushMode_ == 5) brushColor = Color(0.5f, 0.3f, 0.1f);  // brown for erosion
    else if (brushMode_ == 6) brushColor = Color(0.2f, 0.4f, 1.0f);  // blue for river

    // Build outline vertices based on shape
    Vector<Vector3> verts;

    switch (brushShape_)
    {
    case 0: // Circle — 32 segments
    {
        const int segs = 32;
        for (int i = 0; i <= segs; ++i)
        {
            float a = (float)i / (float)segs * 2.0f * M_PI;
            verts.Push(worldPos + Vector3(cosf(a) * worldRadius, 0.0f, sinf(a) * worldRadius));
        }
        break;
    }
    case 1: // Square
    {
        float r = worldRadius;
        verts.Push(worldPos + Vector3(-r, 0, -r));
        verts.Push(worldPos + Vector3( r, 0, -r));
        verts.Push(worldPos + Vector3( r, 0,  r));
        verts.Push(worldPos + Vector3(-r, 0,  r));
        verts.Push(worldPos + Vector3(-r, 0, -r)); // close
        break;
    }
    case 2: // Triangle (equilateral, point-up)
    {
        for (int i = 0; i <= 3; ++i)
        {
            float a;
            switch (i % 3)
            {
            case 0: a = 90.0f * DEG_TO_RAD; break;
            case 1: a = 210.0f * DEG_TO_RAD; break;
            case 2: a = 330.0f * DEG_TO_RAD; break;
            default: a = 90.0f * DEG_TO_RAD; break;
            }
            verts.Push(worldPos + Vector3(cosf(a) * worldRadius, 0.0f, sinf(a) * worldRadius));
        }
        break;
    }
    case 3: // Star (5-pointed, 10 vertices alternating outer/inner)
    {
        float innerR = worldRadius * 0.38f;
        for (int i = 0; i <= 10; ++i)
        {
            float a = (float)(i % 10) / 10.0f * 2.0f * M_PI + M_PI / 2.0f; // start at top
            float r = (i % 2 == 0) ? worldRadius : innerR;
            verts.Push(worldPos + Vector3(cosf(a) * r, 0.0f, sinf(a) * r));
        }
        break;
    }
    default: // Regular polygon: 4=pentagon(5), 5=hexagon(6), 6=octagon(8)
    {
        int sides;
        switch (brushShape_)
        {
        case 4: sides = 5; break;
        case 5: sides = 6; break;
        case 6: sides = 8; break;
        default: sides = 6; break;
        }
        for (int i = 0; i <= sides; ++i)
        {
            float a = (float)(i % sides) / (float)sides * 2.0f * M_PI + M_PI / 2.0f; // start at top
            verts.Push(worldPos + Vector3(cosf(a) * worldRadius, 0.0f, sinf(a) * worldRadius));
        }
        break;
    }
    }

    // Apply rotation to outline vertices
    if (brushRotation_ != 0.0f)
    {
        float ra = brushRotation_ * DEG_TO_RAD;
        float cs = cosf(ra), sn = sinf(ra);
        for (unsigned i = 0; i < verts.Size(); ++i)
        {
            float lx = verts[i].x_ - worldPos.x_;
            float lz = verts[i].z_ - worldPos.z_;
            verts[i].x_ = worldPos.x_ + lx * cs - lz * sn;
            verts[i].z_ = worldPos.z_ + lx * sn + lz * cs;
        }
    }

    // Snap each vertex to terrain height
    for (unsigned i = 0; i < verts.Size(); ++i)
        verts[i].y_ = terrain_->GetHeight(verts[i]) + 0.2f;

    for (unsigned i = 1; i < verts.Size(); ++i)
        debug->AddLine(verts[i - 1], verts[i], brushColor);

    // Center crosshair
    float cross = worldRadius * 0.15f;
    float centerY = terrain_->GetHeight(worldPos) + 0.3f;
    Vector3 center(worldPos.x_, centerY, worldPos.z_);
    debug->AddLine(center + Vector3(-cross, 0, 0), center + Vector3(cross, 0, 0), brushColor);
    debug->AddLine(center + Vector3(0, 0, -cross), center + Vector3(0, 0, cross), brushColor);
}

// ============================================================================
// Celestial Bodies
// ============================================================================

void TerrainNode::CreateCelestialBodies()
{
    auto* cache = GetSubsystem<ResourceCache>();
    sunNode_ = scene_->CreateChild("Sun", LOCAL);
    auto* sunTech = cache->GetResource<Technique>("Techniques/CelestialBodyDiffMap.xml");
    sunMat_ = new Material(context_);
    sunMat_->SetTechnique(0, sunTech);
    sunMat_->SetShaderParameter("MatDiffColor", Color(1.0f, 1.0f, 1.0f, 1.0f));
    sunMat_->SetTexture(TU_DIFFUSE, cache->GetResource<Texture2D>("Textures/Sun.png"));
    auto* sunBB = sunNode_->CreateComponent<BillboardSet>();
    sunBB->SetNumBillboards(1);
    sunBB->SetMaterial(sunMat_);
    sunBB->SetFaceCameraMode(FC_ROTATE_XYZ);
    sunBB->SetSorted(false);
    Billboard* sunQuad = sunBB->GetBillboard(0);
    sunQuad->position_ = Vector3::ZERO;
    sunQuad->size_ = Vector2(30.0f, 30.0f);
    sunQuad->enabled_ = true;
    sunBB->Commit();

    moonNode_ = scene_->CreateChild("Moon", LOCAL);
    moonMat_ = new Material(context_);
    moonMat_->SetTechnique(0, sunTech);
    moonMat_->SetShaderParameter("MatDiffColor", Color(1.0f, 1.0f, 1.0f, 1.0f));
    moonMat_->SetTexture(TU_DIFFUSE, cache->GetResource<Texture2D>("Textures/Moon.png"));
    auto* moonBB = moonNode_->CreateComponent<BillboardSet>();
    moonBB->SetNumBillboards(1);
    moonBB->SetMaterial(moonMat_);
    moonBB->SetFaceCameraMode(FC_ROTATE_XYZ);
    moonBB->SetSorted(false);
    Billboard* moonQuad = moonBB->GetBillboard(0);
    moonQuad->position_ = Vector3::ZERO;
    moonQuad->size_ = Vector2(20.0f, 20.0f);
    moonQuad->enabled_ = true;
    moonBB->Commit();

    // Reuse MoonLight from loaded scene XML if present, else create
    Node* moonLightNode = scene_->GetChild("MoonLight", true);
    if (!moonLightNode)
        moonLightNode = scene_->CreateChild("MoonLight", LOCAL);
    moonLight_ = moonLightNode->GetOrCreateComponent<Light>();
    moonLight_->SetLightType(LIGHT_DIRECTIONAL);
    moonLight_->SetColor(Color(0.6f, 0.6f, 1.0f));
    moonLight_->SetCastShadows(true);
    moonLight_->SetShadowBias(BiasParameters(0.00025f, 0.5f));
    moonLight_->SetShadowCascade(CascadeParameters(10.0f, 50.0f, 200.0f, 0.0f, 0.8f));
    moonLight_->SetEnabled(false);

    float sunAlt = CalculateSunAltitude();
    float sunAz = CalculateSunAzimuth(sunAlt);
    sunNode_->SetPosition(AltAzToFlatEarth(sunAlt, sunAz));

    float moonAlt = CalculateMoonAltitude();
    float moonAz = CalculateMoonAzimuth(moonAlt);
    Vector3 initCamPos = cameraNode_ ? cameraNode_->GetWorldPosition() : Vector3::ZERO;
    moonNode_->SetPosition(initCamPos + AltAzToFlatEarth(moonAlt, moonAz));
    moonNode_->LookAt(initCamPos);

    sunLight_->GetNode()->SetDirection(-AltAzToFlatEarth(sunAlt, sunAz).Normalized());

    UpdateAtmosphere(sunAlt);
}

float TerrainNode::CalculateSunAltitude()
{
    float decl = 23.44f * sinf((360.0f / 365.0f) * (dayOfYear_ - 81) * DEG_TO_RAD);
    float hourAngle = 15.0f * (timeOfDay_ + timeOfDayOffset_ - SOLAR_NOON);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = decl * DEG_TO_RAD;
    float haRad = hourAngle * DEG_TO_RAD;
    float sinAlt = sinf(latRad) * sinf(declRad) + cosf(latRad) * cosf(declRad) * cosf(haRad);
    return asinf(Clamp(sinAlt, -1.0f, 1.0f)) * RAD_TO_DEG;
}

float TerrainNode::CalculateSunAzimuth(float altitude)
{
    float decl = 23.44f * sinf((360.0f / 365.0f) * (dayOfYear_ - 81) * DEG_TO_RAD);
    float hourAngle = 15.0f * (timeOfDay_ + timeOfDayOffset_ - SOLAR_NOON);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = decl * DEG_TO_RAD;
    float altRad = altitude * DEG_TO_RAD;
    float cosAlt = cosf(altRad);
    if (fabsf(cosAlt) < 0.001f) return 180.0f;
    float cosAz = (sinf(declRad) - sinf(altRad) * sinf(latRad)) / (cosAlt * cosf(latRad));
    cosAz = Clamp(cosAz, -1.0f, 1.0f);
    float az = acosf(cosAz) * RAD_TO_DEG;
    if (hourAngle > 0.0f) az = 360.0f - az;
    return az;
}

float TerrainNode::CalculateMoonAltitude()
{
    float moonHourOffset = moonAge_ * (360.0f / 29.53f);
    float moonHourAngle = 15.0f * (timeOfDay_ + timeOfDayOffset_ - SOLAR_NOON) - moonHourOffset;
    float moonEclLon = (360.0f / 29.53f) * moonAge_;
    float sunEclLon = (360.0f / 365.0f) * (dayOfYear_ - 81);
    float totalEclLon = sunEclLon + moonEclLon;
    float moonDecl = 23.44f * sinf(totalEclLon * DEG_TO_RAD);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = moonDecl * DEG_TO_RAD;
    float haRad = moonHourAngle * DEG_TO_RAD;
    float sinAlt = sinf(latRad) * sinf(declRad) + cosf(latRad) * cosf(declRad) * cosf(haRad);
    return asinf(Clamp(sinAlt, -1.0f, 1.0f)) * RAD_TO_DEG;
}

float TerrainNode::CalculateMoonAzimuth(float moonAlt)
{
    float moonHourOffset = moonAge_ * (360.0f / 29.53f);
    float moonHourAngle = 15.0f * (timeOfDay_ + timeOfDayOffset_ - SOLAR_NOON) - moonHourOffset;
    float moonEclLon = (360.0f / 29.53f) * moonAge_;
    float sunEclLon = (360.0f / 365.0f) * (dayOfYear_ - 81);
    float totalEclLon = sunEclLon + moonEclLon;
    float moonDecl = 23.44f * sinf(totalEclLon * DEG_TO_RAD);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = moonDecl * DEG_TO_RAD;
    float altRad = moonAlt * DEG_TO_RAD;
    float cosAlt = cosf(altRad);
    if (fabsf(cosAlt) < 0.001f) return 180.0f;
    float cosAz = (sinf(declRad) - sinf(altRad) * sinf(latRad)) / (cosAlt * cosf(latRad));
    cosAz = Clamp(cosAz, -1.0f, 1.0f);
    float az = acosf(cosAz) * RAD_TO_DEG;
    if (moonHourAngle > 0.0f) az = 360.0f - az;
    return az;
}

Vector3 TerrainNode::AltAzToFlatEarth(float altitude, float azimuth, float distance)
{
    float altRad = altitude * DEG_TO_RAD;
    float azRad = azimuth * DEG_TO_RAD;
    float y = distance * sinf(altRad);
    float horizDist = distance * cosf(altRad);
    float x = horizDist * sinf(azRad);
    float z = horizDist * cosf(azRad);
    return Vector3(x, y, z);
}

void TerrainNode::UpdateCelestialBodies(float timeStep)
{
    timeOfDay_ += timeStep * CELESTIAL_TIME_SCALE / 3600.0f;
    if (timeOfDay_ >= 24.0f)
    {
        timeOfDay_ -= 24.0f;
        dayOfYear_++;
        if (dayOfYear_ > 365) dayOfYear_ = 1;
    }
    moonAge_ += timeStep * CELESTIAL_TIME_SCALE / 86400.0f;
    if (moonAge_ >= 29.53f) moonAge_ -= 29.53f;

    cloudAngle_ += timeStep * CELESTIAL_TIME_SCALE * 6.2831853f / (18.0f * 3600.0f);
    if (cloudAngle_ > 6.2831853f) cloudAngle_ -= 6.2831853f;
    // Cloud angle includes time offset so scrubbing visibly rotates clouds
    float cloudTotal = cloudAngle_ + timeOfDayOffset_ * 6.2831853f / 18.0f;

    float starAngle = 6.2831853f * ((timeOfDay_ + timeOfDayOffset_) / 24.0f + (float)dayOfYear_ / 365.25f);

    if (skyboxMat_)
    {
        skyboxMat_->SetShaderParameter("CloudAngle", cloudTotal);
        skyboxMat_->SetShaderParameter("StarAngle", starAngle);
    }

    cachedSunAlt_ = CalculateSunAltitude();
    cachedSunAz_ = CalculateSunAzimuth(cachedSunAlt_);
    Vector3 sunOffset = AltAzToFlatEarth(cachedSunAlt_, cachedSunAz_);
    Vector3 camPos = cameraNode_ ? cameraNode_->GetWorldPosition() : Vector3::ZERO;
    if (sunNode_)
    {
        sunNode_->SetPosition(camPos + sunOffset);
        sunNode_->LookAt(camPos);
        auto* bb = sunNode_->GetComponent<BillboardSet>();
        if (bb) bb->SetEnabled(cachedSunAlt_ > 0.0f);  // hide at/below horizon
    }
    if (sunLight_)
        sunLight_->GetNode()->SetDirection((camPos - (sunNode_ ? sunNode_->GetPosition() : Vector3::ZERO)).Normalized());

    cachedMoonAlt_ = CalculateMoonAltitude();
    cachedMoonAz_ = CalculateMoonAzimuth(cachedMoonAlt_);
    Vector3 moonOffset = AltAzToFlatEarth(cachedMoonAlt_, cachedMoonAz_);
    if (moonNode_)
    {
        moonNode_->SetPosition(camPos + moonOffset);
        moonNode_->LookAt(camPos);
        auto* bb = moonNode_->GetComponent<BillboardSet>();
        if (bb) bb->SetEnabled(cachedMoonAlt_ > 0.0f);  // hide at/below horizon
    }
    if (moonLight_)
        moonLight_->GetNode()->SetDirection(-moonOffset.Normalized());

    UpdateAtmosphere(cachedSunAlt_);

    // --- God rays tracking ---
    if (renderPath_ && cameraNode_)
    {
        auto* cam = cameraNode_->GetComponent<Camera>();
        bool rayActive = false;

        // Try sun first, then moon
        // Screen radius: billboard half-size / distance / tan(FOV/2) * 0.5
        float halfFov = cam->GetFov() * 0.5f * M_DEGTORAD;
        float tanHalf = tanf(halfFov);

        // Sun god rays — uses "GodRays" pass
        bool sunRayActive = false;
        if (godRaysEnabled_ && sunNode_ && cachedSunAlt_ > -5.0f)
        {
            Vector3 toSun = (sunNode_->GetWorldPosition() - camPos);
            float sunDist = toSun.Length();
            toSun /= sunDist;
            if (cameraNode_->GetWorldDirection().DotProduct(toSun) > 0.0f)
            {
                Vector2 screenPos = cam->WorldToScreenPoint(sunNode_->GetWorldPosition());

                // Sun halo varies with altitude in three phases:
                //   High (20+):  pale yellow, tight, bright
                //   Sunset (0-20): deepening red, wider, dimmer
                //   Below horizon (-5 to 0): red fading to white, dimming out
                Vector3 sunColor;
                float intensity, exposure, radiusMult;

                if (cachedSunAlt_ >= 20.0f)
                {
                    // High sky — pale warm white
                    float t = Clamp((cachedSunAlt_ - 20.0f) / 30.0f, 0.0f, 1.0f);
                    sunColor = Vector3(1.0f, Lerp(0.85f, 0.95f, t), Lerp(0.65f, 0.85f, t));
                    intensity = Lerp(0.7f, 0.8f, t);
                    exposure = 0.4f;
                    radiusMult = 1.0f;
                }
                else if (cachedSunAlt_ >= 0.0f)
                {
                    // Sunset — deepening red as altitude drops toward 0
                    float t = cachedSunAlt_ / 20.0f;  // 1=20deg, 0=horizon
                    sunColor = Vector3(1.0f, Lerp(0.25f, 0.85f, t), Lerp(0.1f, 0.65f, t));
                    intensity = Lerp(0.5f, 0.7f, t);
                    exposure = Lerp(0.3f, 0.4f, t);
                    radiusMult = Lerp(2.0f, 1.0f, t);
                }
                else
                {
                    // Below horizon — fade from red toward white, dimming out
                    float t = Clamp((cachedSunAlt_ + 5.0f) / 5.0f, 0.0f, 1.0f);  // 1=horizon, 0=-5deg
                    sunColor = Vector3(1.0f, Lerp(0.8f, 0.25f, t), Lerp(0.7f, 0.1f, t));
                    intensity = Lerp(0.0f, 0.5f, t);
                    exposure = Lerp(0.1f, 0.3f, t);
                    radiusMult = Lerp(1.5f, 2.0f, t);
                }

                float screenRadius = (30.0f / sunDist) / tanHalf * 0.5f * radiusMult;

                renderPath_->SetShaderParameter("LightScreenPos", screenPos);
                renderPath_->SetShaderParameter("LightRadius", screenRadius);
                renderPath_->SetShaderParameter("GodRayColor", sunColor);
                renderPath_->SetShaderParameter("GodRayDensity", 0.5f);
                renderPath_->SetShaderParameter("GodRayDecay", 0.97f);
                renderPath_->SetShaderParameter("GodRayWeight", 0.4f);
                renderPath_->SetShaderParameter("GodRayExposure", exposure);
                renderPath_->SetShaderParameter("GodRayIntensity", intensity);
                sunRayActive = true;
            }
        }
        renderPath_->SetEnabled("GodRays", sunRayActive);

        // Moon god rays — uses separate "MoonRays" pass (can be active alongside sun)
        bool moonRayActive = false;
        if (godRaysEnabled_ && moonNode_ && cachedMoonAlt_ > -5.0f)
        {
            Vector3 toMoon = (moonNode_->GetWorldPosition() - camPos);
            float moonDist = toMoon.Length();
            toMoon /= moonDist;
            if (cameraNode_->GetWorldDirection().DotProduct(toMoon) > 0.0f)
            {
                Vector2 screenPos = cam->WorldToScreenPoint(moonNode_->GetWorldPosition());
                float screenRadius = (20.0f / moonDist) / tanHalf * 0.5f;

                // Moon: simple fade in at moonrise, fade out at moonset
                // Full intensity above 5 deg, fades to zero at -5 deg
                float moonFade = Clamp((cachedMoonAlt_ + 5.0f) / 10.0f, 0.0f, 1.0f);

                // Set parameters directly on the MoonRays command (not shared with GodRays)
                for (int ci = 0; ci < renderPath_->GetNumCommands(); ++ci)
                {
                    RenderPathCommand* cmd = renderPath_->GetCommand(ci);
                    if (cmd && cmd->tag_ == "MoonRays")
                    {
                        cmd->SetShaderParameter("LightScreenPos", screenPos);
                        cmd->SetShaderParameter("LightRadius", screenRadius);
                        cmd->SetShaderParameter("GodRayColor", Vector3(0.6f, 0.7f, 0.9f));
                        cmd->SetShaderParameter("GodRayDensity", 0.5f);
                        cmd->SetShaderParameter("GodRayDecay", 0.97f);
                        cmd->SetShaderParameter("GodRayWeight", 0.4f);
                        cmd->SetShaderParameter("GodRayExposure", 0.4f);
                        cmd->SetShaderParameter("GodRayIntensity", 0.8f * moonFade);
                        break;
                    }
                }
                moonRayActive = true;
            }
        }
        renderPath_->SetEnabled("MoonRays", moonRayActive);

    }
}

// ============================================================================
// Rain Particles
// ============================================================================

void TerrainNode::CreateRain()
{
    auto* cache = GetSubsystem<ResourceCache>();

    // Create rain emitter node (will be repositioned above camera each frame)
    rainNode_ = scene_->CreateChild("Rain");

    // Create particle effect programmatically so we can update wind force dynamically
    rainEffect_ = new ParticleEffect(context_);

    // Material — unlit alpha-blended particle
    auto* rainMat = cache->GetResource<Material>("Materials/RainDrop.xml");
    if (!rainMat)
    {
        // Fallback to smoke texture if custom material missing
        rainMat = cache->GetResource<Material>("Materials/Particle.xml");
    }
    rainEffect_->SetMaterial(rainMat);

    // Emitter shape — wide box above camera
    rainEffect_->SetEmitterType(EMITTER_BOX);
    rainEffect_->SetEmitterSize(Vector3(40.0f, 1.0f, 40.0f));

    // Initial direction — downward with slight randomness
    rainEffect_->SetMinDirection(Vector3(-0.1f, -1.0f, -0.1f));
    rainEffect_->SetMaxDirection(Vector3(0.1f, -1.0f, 0.1f));

    // Fall speed
    rainEffect_->SetMinVelocity(12.0f);
    rainEffect_->SetMaxVelocity(16.0f);

    // Gravity — pulls drops down, wind added per-frame via SetConstantForce
    rainEffect_->SetConstantForce(Vector3(0.0f, -9.8f, 0.0f));
    rainEffect_->SetDampingForce(0.3f);

    // Particle appearance — thin vertical streaks
    rainEffect_->SetMinParticleSize(Vector2(0.015f, 0.15f));
    rainEffect_->SetMaxParticleSize(Vector2(0.025f, 0.25f));

    // Lifetime — enough to fall from emitter to ground
    rainEffect_->SetMinTimeToLive(1.2f);
    rainEffect_->SetMaxTimeToLive(1.8f);

    // World-space particles — don't move with emitter node
    rainEffect_->SetRelative(false);
    rainEffect_->SetUpdateInvisible(true);

    // Direction-aligned stretching — streaks align to velocity
    rainEffect_->SetFaceCameraMode(FC_DIRECTION);

    // Start with zero emission — turned on by precipitation
    rainEffect_->SetMinEmissionRate(0.0f);
    rainEffect_->SetMaxEmissionRate(0.0f);

    // Particle pool
    rainEffect_->SetNumParticles(800);

    // Color — blue-white translucent, fading out over lifetime
    rainEffect_->SetNumColorFrames(2);
    rainEffect_->SetColorFrame(0, ColorFrame(Color(0.7f, 0.75f, 0.85f, 0.4f), 0.0f));
    rainEffect_->SetColorFrame(1, ColorFrame(Color(0.7f, 0.75f, 0.85f, 0.0f), 1.0f));

    // Create emitter component
    rainEmitter_ = rainNode_->CreateComponent<ParticleEmitter>();
    rainEmitter_->SetEffect(rainEffect_);
    rainEmitter_->SetEmitting(false);  // starts off until precipitation > 0.1

    URHO3D_LOGINFO("Rain particle system created");
}

void TerrainNode::UpdateRain(float timeStep)
{
    if (!rainNode_ || !rainEffect_ || !cameraNode_)
        return;

    // Move emitter box above camera
    Vector3 camPos = cameraNode_->GetWorldPosition();
    rainNode_->SetWorldPosition(camPos + Vector3(0.0f, 15.0f, 0.0f));

    // Rain intensity: use override slider if set, otherwise follow precipitation
    float rainIntensity = (rainOverride_ >= 0.0f) ? rainOverride_ : weather_.precipitation;

    // Emission rate driven by rain intensity
    if (rainIntensity < 0.1f)
    {
        if (rainEmitter_->IsEmitting())
            rainEmitter_->SetEmitting(false);
        return;
    }

    if (!rainEmitter_->IsEmitting())
        rainEmitter_->SetEmitting(true);

    // Quadratic ramp: intensity 0.1..1.0 -> 0..800 particles/sec (heavier at top end)
    float t = (rainIntensity - 0.1f) / 0.9f;
    float rate = t * t * 800.0f;
    rainEffect_->SetMinEmissionRate(rate * 0.8f);
    rainEffect_->SetMaxEmissionRate(rate);

    // Wind force — lateral acceleration based on wind speed and angle
    float windMag = weather_.windSpeed * 20.0f;  // 0..20 m/s^2 lateral
    Vector3 force;
    force.x_ = cosf(weather_.windAngle) * windMag;
    force.y_ = -9.8f;  // gravity constant
    force.z_ = sinf(weather_.windAngle) * windMag;
    rainEffect_->SetConstantForce(force);

    // Kill particles that penetrate the water plane
    const float waterY = 5.0f;
    Vector<Billboard>& bbs = rainEmitter_->GetBillboards();
    for (int i = 0; i < bbs.Size(); ++i)
    {
        if (bbs[i].enabled_ && bbs[i].position_.y_ < waterY)
            bbs[i].enabled_ = false;
    }
    rainEmitter_->Commit();
}

// ============================================================================
// Campfire
// ============================================================================

void TerrainNode::CreateCampfire()
{
    if (!terrain_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    const float waterY = 5.0f;

    // Scan terrain for a shoreline position: find a spot just above water
    // Sample in a grid pattern and pick the first point that's 0.5–2.0 above water
    Vector3 bestPos;
    bool found = false;
    for (float x = -200.0f; x < 200.0f && !found; x += 10.0f)
    {
        for (float z = -200.0f; z < 200.0f && !found; z += 10.0f)
        {
            float h = terrain_->GetHeight(Vector3(x, 0.0f, z));
            if (h > waterY + 0.5f && h < waterY + 2.0f)
            {
                bestPos = Vector3(x, h, z);
                found = true;
            }
        }
    }

    if (!found)
    {
        URHO3D_LOGWARNING("CreateCampfire: no suitable shoreline position found");
        return;
    }

    // Create campfire node
    campfireNode_ = scene_->CreateChild("Campfire");
    campfireNode_->SetPosition(bestPos);

    // Fire emitter — load existing Fire.xml particle effect
    auto* fireEffect = cache->GetResource<ParticleEffect>("Particle/Fire.xml");
    if (fireEffect)
    {
        Node* fireNode = campfireNode_->CreateChild("Fire");
        auto* fireEmitter = fireNode->CreateComponent<ParticleEmitter>();
        fireEmitter->SetEffect(fireEffect);
        fireEmitter->SetEmitting(true);
    }

    // Smoke emitter — SmokeStack is continuous (activetime=0), Smoke.xml bursts then stops
    auto* smokeEffect = cache->GetResource<ParticleEffect>("Particle/SmokeStack.xml");
    if (smokeEffect)
    {
        Node* smokeNode = campfireNode_->CreateChild("Smoke");
        smokeNode->SetPosition(Vector3(0.0f, 0.3f, 0.0f));  // slightly above fire
        auto* smokeEmitter = smokeNode->CreateComponent<ParticleEmitter>();
        smokeEmitter->SetEffect(smokeEffect);
        smokeEmitter->SetEmitting(true);
    }

    // Point light for fire glow
    Node* lightNode = campfireNode_->CreateChild("FireLight");
    lightNode->SetPosition(Vector3(0.0f, 1.0f, 0.0f));
    auto* light = lightNode->CreateComponent<Light>();
    light->SetLightType(LIGHT_POINT);
    light->SetRange(12.0f);
    light->SetColor(Color(1.0f, 0.6f, 0.2f));
    light->SetBrightness(1.5f);
    light->SetCastShadows(false);

    URHO3D_LOGINFOF("Campfire placed at (%.1f, %.1f, %.1f) near shoreline",
        bestPos.x_, bestPos.y_, bestPos.z_);
}

// ============================================================================
// Fish
// ============================================================================

void TerrainNode::CreateFish()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* fishModel = cache->GetResource<Model>("Models/UrhoFish.mdl");
    auto* fishMat = cache->GetResource<Material>("Materials/UrhoFish.xml");

    if (!fishModel || !fishMat)
    {
        URHO3D_LOGERROR("Failed to load fish model or material");
        return;
    }
    SharedPtr<Material> baseMat(fishMat);

    SharedPtr<Material> orbitMat(fishMat->Clone());
    orbitMat->SetShaderParameter("WiggleAmplitude", 0.06f);
    orbitMat->SetShaderParameter("WiggleFrequency", 5.0f);

    SharedPtr<Material> stareMat(fishMat->Clone());
    stareMat->SetShaderParameter("WiggleAmplitude", 0.01f);
    stareMat->SetShaderParameter("WiggleFrequency", 0.8f);

    const int NUM_FISH = 50;
    const float waterY = 5.0f;
    const float spawnRadius = 80.0f;
    const float MIN_DEPTH = 1.0f;
    Terrain* t = terrain_;

    Vector3 camPos = cameraNode_ ? cameraNode_->GetPosition() : Vector3::ZERO;
    float centerX = camPos.x_;
    float centerZ = camPos.z_;

    for (int i = 0; i < NUM_FISH; ++i)
    {
        Node* fishNode = scene_->CreateChild("Fish", LOCAL);

        // Pick a random position that's actually underwater with enough depth
        Vector3 pos;
        int tries = 0;
        for (;;)
        {
            float x = centerX + Random(-spawnRadius, spawnRadius);
            float z = centerZ + Random(-spawnRadius, spawnRadius);
            float terrainH = t ? t->GetHeight(Vector3(x, 0.0f, z)) : 0.0f;
            float availableDepth = waterY - terrainH;

            if (availableDepth >= MIN_DEPTH + 0.5f || ++tries > 50)
            {
                float minY = Max(terrainH + 0.5f, waterY - 4.0f);
                float maxY = waterY - MIN_DEPTH;
                if (minY > maxY) minY = maxY;
                pos = Vector3(x, Random(minY, maxY), z);
                break;
            }
        }
        fishNode->SetPosition(pos);
        fishNode->SetRotation(Quaternion(0.0f, Random(0.0f, 360.0f), 0.0f));
        fishNode->SetScale(0.001f);

        auto* sm = fishNode->CreateComponent<StaticModel>();
        sm->SetModel(fishModel, true);
        sm->SetMaterial(fishMat);
        sm->SetCastShadows(false);

        // Attach Fish component — handles its own swim AI
        auto* fish = fishNode->CreateComponent<Fish>();
        fish->SetMaterials(baseMat, orbitMat, stareMat);
        fish->SetCameraNode(cameraNode_);
    }

    URHO3D_LOGINFOF("Spawned %d fish", NUM_FISH);
}

void TerrainNode::CreateSchoolFish()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* fishModel = cache->GetResource<Model>("Models/UrhoFish.mdl");
    auto* fishMat = cache->GetResource<Material>("Materials/UrhoFish.xml");

    if (!fishModel || !fishMat)
        return;

    // Tiny fish material — faster wiggle for smaller bodies
    SharedPtr<Material> schoolMat(fishMat->Clone());
    schoolMat->SetShaderParameter("WiggleAmplitude", 0.03f);
    schoolMat->SetShaderParameter("WiggleFrequency", 8.0f);

    const float waterY = 5.0f;
    const float TINY_SCALE = 0.0003f;  // ~1/3 of regular fish scale (0.001)
    const int NUM_SCHOOLS = 3;
    const int FISH_PER_SCHOOL = 15;
    Terrain* t = terrain_;

    for (int school = 0; school < NUM_SCHOOLS; ++school)
    {
        // Pick a school center — random deep-water location
        Vector3 schoolCenter;
        for (int tries = 0; tries < 50; ++tries)
        {
            float x = Random(-60.0f, 60.0f);
            float z = Random(-60.0f, 60.0f);
            float terrainH = t ? t->GetHeight(Vector3(x, 0.0f, z)) : 0.0f;
            if (waterY - terrainH >= 2.0f)
            {
                schoolCenter = Vector3(x, Lerp(terrainH + 0.5f, waterY - 0.5f, 0.5f), z);
                break;
            }
            if (tries == 49)
                schoolCenter = Vector3(0.0f, waterY - 2.0f, 0.0f);
        }

        // Spawn fish in a tight cluster around school center
        for (int i = 0; i < FISH_PER_SCHOOL; ++i)
        {
            Node* fishNode = scene_->CreateChild("SchoolFish", LOCAL);
            Vector3 offset(Random(-2.0f, 2.0f), Random(-0.3f, 0.3f), Random(-2.0f, 2.0f));
            fishNode->SetPosition(schoolCenter + offset);
            fishNode->SetRotation(Quaternion(0.0f, Random(0.0f, 360.0f), 0.0f));
            fishNode->SetScale(TINY_SCALE);

            auto* sm = fishNode->CreateComponent<StaticModel>();
            sm->SetModel(fishModel, true);
            sm->SetMaterial(schoolMat);
            sm->SetCastShadows(false);

            auto* fish = fishNode->CreateComponent<SchoolFish>();
            fish->SetSchoolID(school);
            fish->SetCameraNode(cameraNode_);
        }
    }

    URHO3D_LOGINFOF("Spawned %d schools of %d tiny fish each", NUM_SCHOOLS, FISH_PER_SCHOOL);
}

// ============================================================================
// Animals
// ============================================================================

void TerrainNode::CreateAnimals()
{
    if (!terrain_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    const float spawnRadius = 80.0f;
    const float waterLevel = 5.5f;

    // Spawn near camera, not at world origin
    Vector3 camPos = cameraNode_ ? cameraNode_->GetPosition() : Vector3::ZERO;
    float centerX = camPos.x_;
    float centerZ = camPos.z_;

    // Spawn rabbits
    for (int i = 0; i < 5; ++i)
    {
        // Find a dry spot
        Vector3 pos;
        for (int tries = 0; tries < 10; ++tries)
        {
            float x = centerX + Random(-spawnRadius, spawnRadius);
            float z = centerZ + Random(-spawnRadius, spawnRadius);
            float y = terrain_->GetHeight(Vector3(x, 0.0f, z));
            if (y > waterLevel)
            {
                pos = Vector3(x, y, z);
                break;
            }
            if (tries == 9)
                pos = Vector3(centerX, terrain_->GetHeight(Vector3(centerX, 0.0f, centerZ)), centerZ);
        }

        Node* node = scene_->CreateChild("Rabbit", LOCAL);
        node->SetPosition(pos);

        // Model on child node with 180° Y flip — mesh faces -Z, Urho3D forward is +Z
        Node* modelNode = node->CreateChild("RabbitModel");
        modelNode->SetRotation(Quaternion(180.0f, Vector3::UP));

        auto* model = modelNode->CreateComponent<AnimatedModel>();
        model->SetModel(cache->GetResource<Model>("Models/Animals/Rabbit.mdl"), true, true);
        model->ApplyMaterialList("Models/Animals/Rabbit.txt");
        model->SetCastShadows(true);

        modelNode->CreateComponent<AnimationController>();
        node->CreateComponent<Rabbit>();

        animalNodes_.Push(WeakPtr<Node>(node));
    }

    // Spawn deer — prefer higher ground, further from water than rabbits
    const float deerSpawnRadius = 120.0f;
    const float deerMinHeight = waterLevel + 3.0f;  // at least 3m above waterline
    for (int i = 0; i < 8; ++i)
    {
        Vector3 pos;
        for (int tries = 0; tries < 20; ++tries)
        {
            float x = centerX + Random(-deerSpawnRadius, deerSpawnRadius);
            float z = centerZ + Random(-deerSpawnRadius, deerSpawnRadius);
            float y = terrain_->GetHeight(Vector3(x, 0.0f, z));
            // Prefer higher ground; relax threshold after many attempts
            float minH = (tries < 10) ? deerMinHeight : waterLevel;
            if (y > minH)
            {
                pos = Vector3(x, y, z);
                break;
            }
            if (tries == 19)
                pos = Vector3(centerX, terrain_->GetHeight(Vector3(centerX, 0.0f, centerZ)), centerZ);
        }

        Node* node = scene_->CreateChild("Deer", LOCAL);
        node->SetPosition(pos);

        // Model on child node with 180° Y flip — deer mesh faces -Z, Urho3D forward is +Z
        Node* modelNode = node->CreateChild("DeerModel");
        modelNode->SetRotation(Quaternion(180.0f, Vector3::UP));

        auto* model = modelNode->CreateComponent<AnimatedModel>();
        model->SetModel(cache->GetResource<Model>("Models/Animals/Deer.mdl"), true, true);
        model->ApplyMaterialList("Models/Animals/Deer.txt");
        model->SetCastShadows(true);

        modelNode->CreateComponent<AnimationController>();
        node->CreateComponent<Deer>();

        animalNodes_.Push(WeakPtr<Node>(node));
    }

    // ── Foxes ──
    for (int i = 0; i < 3; ++i)
    {
        Vector3 pos;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            float centerX = Random(-80.0f, 80.0f);
            float centerZ = Random(-80.0f, 80.0f);
            float groundY = terrain_->GetHeight(Vector3(centerX, 0.0f, centerZ));
            if (groundY > 5.5f)  // above water
            {
                pos = Vector3(centerX, groundY, centerZ);
                break;
            }
            if (attempt == 19)
                pos = Vector3(centerX, terrain_->GetHeight(Vector3(centerX, 0.0f, centerZ)), centerZ);
        }

        Node* node = scene_->CreateChild("Fox", LOCAL);
        node->SetPosition(pos);

        // Model on child node with 180° Y flip — mesh faces -Z, Urho3D forward is +Z
        Node* modelNode = node->CreateChild("FoxModel");
        modelNode->SetRotation(Quaternion(180.0f, Vector3::UP));

        auto* model = modelNode->CreateComponent<AnimatedModel>();
        model->SetModel(cache->GetResource<Model>("Models/Animals/Fox.mdl"), true, true);
        model->ApplyMaterialList("Models/Animals/Fox.txt");
        model->SetCastShadows(true);

        modelNode->CreateComponent<AnimationController>();
        node->CreateComponent<Fox>();

        animalNodes_.Push(WeakPtr<Node>(node));
    }

    URHO3D_LOGINFOF("Created %u animals (5 rabbits, 8 deer, 3 foxes)", animalNodes_.Size());
}

// ============================================================================
// OOFO fleet
// ============================================================================

void TerrainNode::CreateOOFOs()
{
    auto* cache = GetSubsystem<ResourceCache>();
    oofoCloudPositions_ = OOFO::BuildCloudPositions(cache);

    // Stagger spawns with random delays so OOFOs don't all appear at once
    oofosSpawned_ = 0;
    for (int i = 0; i < NUM_OOFOS; ++i)
        oofoSpawnTimers_[i] = Random(1.0f, 8.0f) + i * Random(2.0f, 5.0f);
}

void TerrainNode::UpdateOOFOs(float timeStep)
{
    // Spawn OOFOs on their individual delayed timers
    if (oofosSpawned_ < NUM_OOFOS)
    {
        auto* cache = GetSubsystem<ResourceCache>();
        for (int i = 0; i < NUM_OOFOS; ++i)
        {
            if (oofoSpawnTimers_[i] > -1.0f)  // not yet spawned (-1 = done)
            {
                oofoSpawnTimers_[i] -= timeStep;
                if (oofoSpawnTimers_[i] <= 0.0f)
                {
                    SharedPtr<OOFO> o(new OOFO());
                    o->Init(scene_, cache, &oofoCloudPositions_);
                    oofos_.Push(o);
                    ++oofosSpawned_;
                    oofoSpawnTimers_[i] = -1.0f;  // mark spawned
                }
            }
        }
    }

    if (oofos_.Empty()) return;

    float nightFactor = 0.0f;
    if (skyboxMat_)
    {
        Variant nf = skyboxMat_->GetShaderParameter("NightFactor");
        if (nf.GetType() == VAR_FLOAT)
            nightFactor = nf.GetFloat();
    }

    float cloudTotal = cloudAngle_ + timeOfDayOffset_ * 6.2831853f / 18.0f;
    Vector3 camPos = cameraNode_ ? cameraNode_->GetWorldPosition() : Vector3::ZERO;

    for (unsigned i = 0; i < oofos_.Size(); ++i)
        oofos_[i]->Update(timeStep, camPos, cloudTotal, nightFactor);
}

void TerrainNode::UpdateSeasonalEffects()
{
    if (dayOfYear_ == lastSeasonDay_)
        return;
    lastSeasonDay_ = dayOfYear_;

    // Season factor: 0=winter solstice, 1=summer solstice
    cachedSeasonFactor_ = 0.5f + 0.5f * sinf((dayOfYear_ - 81) * 6.2831853f / 365.0f);

    // Sky/fog seasonal bias (applied per-frame only during daytime)
    Color summerBias(1.02f, 0.98f, 0.92f);
    Color winterBias(0.92f, 0.95f, 1.05f);
    cachedSeasonBias_ = winterBias.Lerp(summerBias, cachedSeasonFactor_);

    // Fog distance
    cachedFogStart_ = Lerp(350.0f, 600.0f, cachedSeasonFactor_);
    cachedFogEnd_ = Lerp(600.0f, 800.0f, cachedSeasonFactor_);

    // Terrain tint — 4-point seasonal lerp
    if (terrain_)
    {
        float seasonAngle = fmodf((dayOfYear_ - 81) / 365.0f + 1.0f, 1.0f);
        Color spring(0.85f, 1.0f, 0.8f);
        Color summer(0.9f, 1.0f, 0.85f);
        Color autumn(1.0f, 0.85f, 0.7f);
        Color winter(0.85f, 0.85f, 0.9f);

        if (seasonAngle < 0.25f)
            cachedTerrainTint_ = spring.Lerp(summer, seasonAngle / 0.25f);
        else if (seasonAngle < 0.5f)
            cachedTerrainTint_ = summer.Lerp(autumn, (seasonAngle - 0.25f) / 0.25f);
        else if (seasonAngle < 0.75f)
            cachedTerrainTint_ = autumn.Lerp(winter, (seasonAngle - 0.5f) / 0.25f);
        else
            cachedTerrainTint_ = winter.Lerp(spring, (seasonAngle - 0.75f) / 0.25f);

        auto* terrainMat = terrain_->GetMaterial();
        if (terrainMat)
            terrainMat->SetShaderParameter("MatDiffColor", cachedTerrainTint_);

        // Grass seasonal tint
        if (grassMat_)
        {
            Color grassSpring(0.5f, 0.85f, 0.35f, 1.0f);
            Color grassSummer(0.75f, 0.8f, 0.3f, 1.0f);
            Color grassAutumn(0.7f, 0.55f, 0.2f, 1.0f);
            Color grassWinter(0.5f, 0.45f, 0.3f, 1.0f);
            Color grassTint;
            if (seasonAngle < 0.25f)
                grassTint = grassSpring.Lerp(grassSummer, seasonAngle / 0.25f);
            else if (seasonAngle < 0.5f)
                grassTint = grassSummer.Lerp(grassAutumn, (seasonAngle - 0.25f) / 0.25f);
            else if (seasonAngle < 0.75f)
                grassTint = grassAutumn.Lerp(grassWinter, (seasonAngle - 0.5f) / 0.25f);
            else
                grassTint = grassWinter.Lerp(grassSpring, (seasonAngle - 0.75f) / 0.25f);
            grassMat_->SetShaderParameter("MatDiffColor", grassTint);
        }
    }

    // Water color
    if (waterNode_)
    {
        auto* sm = waterNode_->GetComponent<StaticModel>();
        auto* waterMat = sm ? sm->GetMaterial() : nullptr;
        if (waterMat)
        {
            Color summerShallow(0.2f, 0.6f, 0.5f);
            Color winterShallow(0.15f, 0.35f, 0.35f);
            Color summerDeep(0.02f, 0.1f, 0.2f);
            Color winterDeep(0.02f, 0.06f, 0.12f);

            cachedShallowColor_ = winterShallow.Lerp(summerShallow, cachedSeasonFactor_);
            cachedDeepColor_ = winterDeep.Lerp(summerDeep, cachedSeasonFactor_);

            waterMat->SetShaderParameter("ShallowColor", cachedShallowColor_);
            waterMat->SetShaderParameter("DeepColor", cachedDeepColor_);
        }
    }

    // Seasonal skybox blending
    // seasonAngle: 0=spring equinox, cycles through spring→summer→autumn→winter
    if (skyboxMat_ && seasonSkyboxes_[0])
    {
        float seasonAngle = fmodf((dayOfYear_ - 81) / 365.0f + 1.0f, 1.0f);
        // Map to season indices: 0=spring, 1=summer, 2=autumn, 3=winter
        float seasonPos = seasonAngle * 4.0f;   // 0..4
        int currentSeason = ((int)seasonPos) % 4;
        int nextSeason = (currentSeason + 1) % 4;
        float blend = seasonPos - (int)seasonPos; // 0..1 within current season

        // Only swap textures when the season index changes
        if (currentSeason != lastSeasonIndex_)
        {
            lastSeasonIndex_ = currentSeason;
            skyboxMat_->SetTexture(TU_DIFFUSE, seasonSkyboxes_[currentSeason]);
            skyboxMat_->SetTexture(TU_SPECULAR, seasonSkyboxes_[nextSeason]);
        }
        skyboxMat_->SetShaderParameter("SeasonBlend", blend);
    }
}

void TerrainNode::UpdateAtmosphere(float sunAltitude)
{
    if (!zone_)
        return;

    // Recompute seasonal cache only when dayOfYear_ changes
    UpdateSeasonalEffects();

    Color ambient, fogColor, sunColor, moonColor;
    bool sunEnabled = true;
    bool moonEnabled = false;

    if (sunAltitude > 10.0f)
    {
        ambient = Color(0.15f, 0.15f, 0.15f);
        fogColor = Color(0.7f, 0.7f, 0.75f);
        sunColor = Color(1.2f, 1.2f, 1.2f);
    }
    else if (sunAltitude > 0.0f)
    {
        float t = sunAltitude / 10.0f;
        ambient = Color(0.15f, 0.15f, 0.15f).Lerp(Color(0.12f, 0.08f, 0.05f), 1.0f - t);
        fogColor = Color(0.7f, 0.7f, 0.75f).Lerp(Color(1.0f, 0.6f, 0.3f), 1.0f - t);
        sunColor = Color(1.2f, 1.2f, 1.2f).Lerp(Color(1.5f, 0.8f, 0.3f), 1.0f - t);
    }
    else if (sunAltitude > -6.0f)
    {
        float t = sunAltitude / -6.0f;
        ambient = Color(0.12f, 0.08f, 0.05f).Lerp(Color(0.03f, 0.03f, 0.06f), t);
        fogColor = Color(1.0f, 0.6f, 0.3f).Lerp(Color(0.1f, 0.08f, 0.2f), t);
        sunColor = Color(1.5f, 0.8f, 0.3f).Lerp(Color(0.0f, 0.0f, 0.0f), t);
        moonColor = Color(0.0f, 0.0f, 0.0f).Lerp(Color(0.6f, 0.6f, 1.0f), t);
        moonEnabled = true;
    }
    else
    {
        ambient = Color(0.02f, 0.02f, 0.04f);
        fogColor = Color(0.05f, 0.05f, 0.15f);
        sunColor = Color(0.0f, 0.0f, 0.0f);
        sunEnabled = false;
        moonColor = Color(0.6f, 0.6f, 1.0f);
        moonEnabled = true;
    }

    // Apply cached seasonal bias during daytime
    if (sunAltitude > 0.0f)
    {
        fogColor = Color(fogColor.r_ * cachedSeasonBias_.r_, fogColor.g_ * cachedSeasonBias_.g_, fogColor.b_ * cachedSeasonBias_.b_);
        ambient = Color(ambient.r_ * cachedSeasonBias_.r_, ambient.g_ * cachedSeasonBias_.g_, ambient.b_ * cachedSeasonBias_.b_);
    }

    zone_->SetFogStart(cachedFogStart_);
    zone_->SetFogEnd(cachedFogEnd_);
    zone_->SetAmbientColor(ambient);
    zone_->SetFogColor(fogColor);

    if (sunLight_)
    {
        sunLight_->SetColor(sunColor);
        sunLight_->SetEnabled(sunEnabled);
    }
    if (moonLight_)
    {
        moonLight_->SetColor(moonColor);
        moonLight_->SetEnabled(moonEnabled);
    }

    // Height fog: auto (time-based) unless user overrode with H key
    if (heightFogOverride_ == 0)
    {
        float normalScale = 1.0f / 13.0f;
        if (sunAltitude > 20.0f)
        {
            zone_->SetHeightFog(false);
        }
        else if (sunAltitude > 5.0f)
        {
            float t = (sunAltitude - 5.0f) / 15.0f;
            zone_->SetHeightFog(true);
            zone_->SetFogHeightScale(Lerp(normalScale, 50.0f, t));
        }
        else
        {
            zone_->SetHeightFog(true);
            zone_->SetFogHeightScale(normalScale);
        }
    }

    if (skyboxMat_)
    {
        Color skyTint;
        if (sunAltitude > 15.0f)
            skyTint = Color(0.85f, 0.85f, 0.9f);
        else if (sunAltitude > 2.5f)
        {
            float t = (sunAltitude - 2.5f) / 12.5f;
            skyTint = Color(0.9f, 0.5f, 0.3f).Lerp(Color(0.85f, 0.85f, 0.9f), t);
        }
        else if (sunAltitude > 0.0f)
        {
            float t = sunAltitude / 2.5f;
            skyTint = Color(0.8f, 0.4f, 0.25f).Lerp(Color(0.9f, 0.5f, 0.3f), t);
        }
        else if (sunAltitude > -6.0f)
        {
            float t = sunAltitude / -6.0f;
            skyTint = Color(1.0f, 0.5f, 0.3f).Lerp(Color(0.05f, 0.05f, 0.15f), t);
        }
        else
            skyTint = Color(0.03f, 0.03f, 0.08f);

        skyboxMat_->SetShaderParameter("MatDiffColor", skyTint);

        float nightFactor;
        if (sunAltitude > 5.0f)
            nightFactor = 0.0f;
        else if (sunAltitude > -6.0f)
            nightFactor = 1.0f - (sunAltitude + 6.0f) / 11.0f;
        else
            nightFactor = 1.0f;
        skyboxMat_->SetShaderParameter("NightFactor", nightFactor);
    }
}

// ============================================================================
// Weather System
// ============================================================================

WeatherState TerrainNode::CalculateSeasonalWeather()
{
    WeatherState target;

    // Season factor: 0=winter solstice, 1=summer solstice
    float sf = cachedSeasonFactor_;

    // Seasonal baselines for cloud cover
    // Spring/autumn = more overcast, summer = clearer, winter = medium-high
    float seasonAngle = fmodf((dayOfYear_ - 81) / 365.0f + 1.0f, 1.0f);
    float baseCloud;
    if (seasonAngle < 0.25f) // spring
        baseCloud = 0.45f;
    else if (seasonAngle < 0.5f) // summer
        baseCloud = 0.25f;
    else if (seasonAngle < 0.75f) // autumn
        baseCloud = 0.55f;
    else // winter
        baseCloud = 0.50f;

    // Lunar influence: new moon (+30% storm/cloud), full moon (-30%)
    // moonAge_ cycles 0..29.53, full moon at ~14.7
    float lunarPhase = cosf(moonAge_ * 6.2831853f / 29.53f); // +1=new, -1=full
    float lunarCloudMod = lunarPhase * 0.15f; // ±15%

    // Diurnal pattern: afternoon buildup (peak at 15:00)
    float effectiveTime = fmodf(timeOfDay_ + timeOfDayOffset_ + 24.0f, 24.0f);
    float diurnalMod = 0.0f;
    if (effectiveTime > 12.0f && effectiveTime < 18.0f)
        diurnalMod = 0.15f * sinf((effectiveTime - 12.0f) * 3.14159f / 6.0f);
    // Morning fog tendency
    if (effectiveTime > 5.0f && effectiveTime < 9.0f)
        diurnalMod += 0.1f * sinf((effectiveTime - 5.0f) * 3.14159f / 4.0f);

    target.cloudCover = Clamp(baseCloud + lunarCloudMod + diurnalMod, 0.0f, 1.0f);

    // Precipitation: only when cloud cover > 0.5
    if (target.cloudCover > 0.5f)
    {
        float precipChance = (target.cloudCover - 0.5f) * 2.0f; // 0..1
        // Summer thunderstorms are intense but brief
        if (seasonAngle >= 0.25f && seasonAngle < 0.5f)
            precipChance *= 1.3f;
        // Winter precipitation more likely as snow (handled later)
        target.precipitation = Clamp(precipChance * 0.6f, 0.0f, 1.0f);
    }

    // Wind: base from season, boosted during storms
    target.windSpeed = Lerp(0.1f, 0.3f, 1.0f - sf) + target.precipitation * 0.4f;
    target.windSpeed = Clamp(target.windSpeed, 0.0f, 1.0f);
    target.windAngle = fmodf(dayOfYear_ * 0.1f + moonAge_ * 0.3f, 6.2831853f);

    // Fog density: cloud cover thickens fog, precipitation thickens more
    target.fogDensity = 1.0f - target.cloudCover * 0.3f - target.precipitation * 0.3f;
    target.fogDensity = Clamp(target.fogDensity, 0.3f, 1.0f);

    // Ambient dimming: overcast dims light
    target.ambientDim = 1.0f - target.cloudCover * 0.4f;
    target.ambientDim = Clamp(target.ambientDim, 0.4f, 1.0f);

    return target;
}

void TerrainNode::UpdateWeather(float timeStep)
{
    if (weatherOverride_)
        return; // manual slider control, no automatic updates

    float gameTimeStep = timeStep * CELESTIAL_TIME_SCALE;

    // Periodically pick a new weather target
    weatherCheckTimer_ -= gameTimeStep;
    if (weatherCheckTimer_ <= 0.0f)
    {
        weatherCheckTimer_ = weatherCheckInterval_;
        weatherTarget_ = CalculateSeasonalWeather();
        weatherTransitionTimer_ = weatherTransitionDuration_;
    }

    // Lerp toward target
    if (weatherTransitionTimer_ > 0.0f)
    {
        weatherTransitionTimer_ -= gameTimeStep;
        float t = 1.0f - Clamp(weatherTransitionTimer_ / weatherTransitionDuration_, 0.0f, 1.0f);
        // Smooth step for natural transitions
        t = t * t * (3.0f - 2.0f * t);
        weather_ = weather_.Lerp(weatherTarget_, t);
    }

    ApplyWeatherToScene();
}

void TerrainNode::ApplyWeatherToScene()
{
    if (!zone_)
        return;

    // Modulate fog distances — weather.fogDensity < 1 = thicker fog
    float weatherFogStart = cachedFogStart_ * weather_.fogDensity;
    float weatherFogEnd = cachedFogEnd_ * weather_.fogDensity;
    zone_->SetFogStart(weatherFogStart);
    zone_->SetFogEnd(weatherFogEnd);

    // Shift fog color toward gray when overcast/raining
    Color stormGray(0.45f, 0.48f, 0.52f);
    Color currentFog = zone_->GetFogColor();
    Color weatherFog = currentFog.Lerp(stormGray, weather_.cloudCover * 0.5f);
    zone_->SetFogColor(weatherFog);

    // Dim ambient by cloud cover
    Color currentAmbient = zone_->GetAmbientColor();
    Color dimmedAmbient(
        currentAmbient.r_ * weather_.ambientDim,
        currentAmbient.g_ * weather_.ambientDim,
        currentAmbient.b_ * weather_.ambientDim
    );
    zone_->SetAmbientColor(dimmedAmbient);

    // Dim sun during overcast
    if (sunLight_ && sunLight_->IsEnabled())
    {
        Color sunColor = sunLight_->GetColor();
        float sunDim = 1.0f - weather_.cloudCover * 0.5f;
        sunLight_->SetColor(Color(sunColor.r_ * sunDim, sunColor.g_ * sunDim, sunColor.b_ * sunDim));
    }

    // Dim skybox tint during overcast
    if (skyboxMat_)
    {
        float skyDim = 1.0f - weather_.cloudCover * 0.3f;
        Variant currentTint = skyboxMat_->GetShaderParameter("MatDiffColor");
        if (currentTint.GetType() == VAR_COLOR)
        {
            Color tint = currentTint.GetColor();
            skyboxMat_->SetShaderParameter("MatDiffColor",
                Color(tint.r_ * skyDim, tint.g_ * skyDim, tint.b_ * skyDim));
        }
    }

    // Modulate cloud rotation speed with wind
    // (cloudAngle_ is already updated in UpdateCelestialBodies — we add wind boost)
    // Future: wind affects particle systems for rain/snow

    // God ray intensity reduced by cloud cover
    if (godRaysEnabled_ && renderPath_)
    {
        float baseIntensity = 0.6f;  // default god ray intensity
        float weatherIntensity = baseIntensity * (1.0f - weather_.cloudCover * 0.7f);
        renderPath_->SetShaderParameter("GodRayIntensity", weatherIntensity);
    }
}

// ============================================================================
// Melbourne Time (via OS timezone database)
// ============================================================================

void TerrainNode::SyncMelbourneTime()
{
    // Temporarily switch to Melbourne timezone, get local time, restore
    const char* oldTZ = getenv("TZ");
    setenv("TZ", "Australia/Melbourne", 1);
    tzset();

    time_t now = time(nullptr);
    struct tm* melb = localtime(&now);

    int hour = melb->tm_hour;
    int minute = melb->tm_min;
    int second = melb->tm_sec;
    int yday = melb->tm_yday + 1;  // tm_yday is 0-based, dayOfYear_ is 1-based

    // Restore original timezone
    if (oldTZ)
        setenv("TZ", oldTZ, 1);
    else
        unsetenv("TZ");
    tzset();

    timeOfDay_ = (float)hour + minute / 60.0f + second / 3600.0f;
    dayOfYear_ = yday;

    // Seed RNG from time — gives different seed each second
    unsigned seed = 5381;
    seed = seed * 33 + (unsigned)(now & 0xFFFFFFFF);
    seed = seed * 33 + 25773;
    SetRandomSeed(seed);
    URHO3D_LOGINFOF("Melbourne time: %02d:%02d:%02d (day %d), RNG seed: %u", hour, minute, second, yday, seed);
}

// ============================================================================
// Update
// ============================================================================

void TerrainNode::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    // AuthServer LAN discovery (skipped if user chose localhost or already connected)
    discoveryTimer_ -= timeStep;
    if (discoveryTimer_ <= 0.0f && !authConnected_ && authDiscovering_)
    {
        DiscoverAuthServer();
        discoveryTimer_ = discoveryInterval_;
    }

    // Send pending registration once encryption is established (plain DH)
    if (!pendingRegisterUsername_.Empty() && authConnected_)
    {
        auto* network = GetSubsystem<Network>();
        Connection* serverConn = network ? network->GetServerConnection() : nullptr;
        if (serverConn && serverConn->IsEncryptionReady())
        {
            VectorBuffer msg;
            msg.WriteString(pendingRegisterUsername_);
            msg.WriteString(pendingRegisterPassword_);
            serverConn->SendMessage(MSG_AUTH_REGISTER, true, true, msg);
            pendingRegisterUsername_.Clear();
            pendingRegisterPassword_.Clear();
        }
    }

    // Deferred avatar node lookup — remote event may arrive before replication delivers the node
    if (clientObjectID_ && !characterNode_ && scene_)
    {
        characterNode_ = scene_->GetNode(clientObjectID_);
        if (characterNode_)
        {
            cameraMode_ = CAM_CHASE;
            URHO3D_LOGINFOF("Deferred avatar node lookup succeeded — node %u found, chase camera active", clientObjectID_);
        }
    }

    // Melbourne time sync (every 5 minutes)
    timeSyncTimer_ -= timeStep;
    if (timeSyncTimer_ <= 0.0f)
    {
        SyncMelbourneTime();
        timeSyncTimer_ = 300.0f;
    }

    // First-frame-rendered timing
    if (firstFramePending_ && loggedIn_ && !asyncSceneLoading_)
    {
        firstFramePending_ = false;
        loginTimerActive_ = false;
    }

    if (!loggedIn_ || asyncSceneLoading_)
    {
        // Login screen — slowly rotate camera for a cinematic sky view
        Quaternion rot = cameraNode_->GetRotation();
        cameraNode_->SetRotation(rot * Quaternion(timeStep * 1.5f, Vector3::UP));

        // Update login status with server connection state
        if (loginStatusText_ && !authConnected_ && authDiscovering_)
            loginStatusText_->SetText("Scanning for server...");
        else if (loginStatusText_ && authConnected_)
        {
            if (loginStatusText_->GetText() == "Scanning for server..." ||
                loginStatusText_->GetText().Contains("connecting"))
                loginStatusText_->SetText("Connected — enter credentials");
            loginStatusText_->SetColor(Color(0.3f, 1.0f, 0.3f));
        }
    }
    else
    {
        // Full world mode
        if (cameraTransitioning_)
            UpdateCameraTransition(timeStep);
        else
            MoveCamera(timeStep);
        UpdateCurrentPatchBoundary();
        if (!oofos_.Empty()) UpdateOOFOs(timeStep);
        if (minimapCameraNode_) UpdateMinimapCamera();
        if (grassRoot_) UpdateGrassPositions();

        if (renderPath_)
        {
            float camY = cameraNode_->GetWorldPosition().y_;
            renderPath_->SetShaderParameter("MainCameraY", camY);
            if (zone_)
            {
                Color fog = zone_->GetFogColor();
                renderPath_->SetShaderParameter("UnderwaterColor", Vector3(fog.r_ * 0.3f, fog.g_ * 0.3f, fog.b_ * 0.3f));
            }

            // Water droplets — two triggers:
            // 1) Camera emerges from water → full drench
            // 2) Character splashing in water (chase/FP cam) → periodic light bursts
            float waterY = waterNode_ ? waterNode_->GetWorldPosition().y_ : 5.0f;
            bool isUnderwater = (camY < waterY);
            float now = GetSubsystem<Time>()->GetElapsedTime();
            bool triggerDroplets = false;

            // Trigger 1: camera emerges from water
            if (wasUnderwater_ && !isUnderwater)
                triggerDroplets = true;

            // Trigger 2: character is in water, camera is above water in chase/FP
            if (!isUnderwater && characterNode_ && (cameraMode_ == CAM_CHASE || cameraMode_ == CAM_FIRSTPERSON))
            {
                Node* charNode = characterNode_;
                if (charNode && charNode->GetWorldPosition().y_ < waterY)
                {
                    splashTimer_ -= timeStep;
                    if (splashTimer_ <= 0.0f)
                    {
                        triggerDroplets = true;
                        // Vary interval — faster when character is moving
                        auto* body = charNode->GetComponent<RigidBody>();
                        float speed = body ? body->GetLinearVelocity().Length() : 0.0f;
                        splashInterval_ = Lerp(2.0f, 0.6f, Clamp(speed / 5.0f, 0.0f, 1.0f));
                        splashTimer_ = splashInterval_;
                    }
                }
                else
                    splashTimer_ = 0.0f;  // reset when character leaves water
            }

            if (triggerDroplets)
            {
                breachTime_ = now;
                renderPath_->SetShaderParameter("BreachTime", breachTime_);
                renderPath_->SetEnabled("WaterDroplets", true);
            }
            if (!isUnderwater && (now - breachTime_) > 6.5f)
                renderPath_->SetEnabled("WaterDroplets", false);
            wasUnderwater_ = isUnderwater;
        }
    }

    // Celestial update AFTER camera movement — god ray screen position must match
    // the camera state that will be used for rendering this frame
    if (!asyncSceneLoading_ && sunNode_)
    {
        UpdateCelestialBodies(timeStep);
        UpdateWeather(timeStep);
        if (rainEmitter_) UpdateRain(timeStep);
    }

    if (profilerUI_)
    {
        GetSubsystem<Graphics>()->GetVulkanProfiler()->RecordFrame(timeStep);
        if (cameraNode_)
            profilerUI_->SetCameraPos(cameraNode_->GetWorldPosition());
        profilerUI_->Update();
    }
}

void TerrainNode::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (asyncSceneLoading_)
        return;

    auto* debug = scene_->GetComponent<DebugRenderer>();

    // Overlay rays — independent of drawDebug_, toggled from Overlay menu
    if (debug && cameraNode_)
    {
        auto* camera = cameraNode_->GetComponent<Camera>();
        if (camera)
        {
            Ray cursorRay = camera->GetScreenRay(0.5f, 0.5f);
            Vector3 cursorNear = cursorRay.origin_ + cursorRay.direction_ * camera->GetNearClip();

            // OOFO detector rays
            if (oofoRayVisible_)
            {
                for (unsigned i = 0; i < oofos_.Size(); ++i)
                {
                    Node* n = oofos_[i]->GetNode();
                    if (n && n->IsEnabled())
                        debug->AddLine(n->GetWorldPosition(), cursorNear, Color::GREEN, true);
                }
            }

            // Sun & Moon locator rays — always draw, even when billboard is hidden below horizon
            if (sunNode_)
                debug->AddLine(sunNode_->GetWorldPosition(), cursorNear, Color(1.0f, 0.6f, 0.0f), true);
            if (moonNode_)
                debug->AddLine(moonNode_->GetWorldPosition(), cursorNear, Color(0.0f, 0.8f, 1.0f), true);

            // Animal rays — lines from camera to each animal, color coded by species
            if (landAnimalRayVisible_ || waterAnimalRayVisible_)
            {
                Vector3 camPos = cameraNode_->GetWorldPosition();
                const Vector<SharedPtr<Node>>& children = scene_->GetChildren();
                for (unsigned i = 0; i < children.Size(); ++i)
                {
                    const String& name = children[i]->GetName();
                    Color color;
                    bool isWater = false;

                    if (name == "Rabbit")
                        color = Color(0.2f, 1.0f, 0.2f);   // green
                    else if (name == "Deer")
                        color = Color(0.8f, 0.4f, 1.0f);   // purple
                    else if (name == "Fox")
                        color = Color(1.0f, 0.4f, 0.1f);   // orange-red
                    else if (name == "Fish")
                    {
                        color = Color(0.2f, 0.7f, 1.0f);   // sky blue
                        isWater = true;
                    }
                    else
                        continue;

                    if (isWater && !waterAnimalRayVisible_)
                        continue;
                    if (!isWater && !landAnimalRayVisible_)
                        continue;

                    Vector3 animalPos = children[i]->GetWorldPosition();
                    debug->AddLine(camPos, animalPos, color, false);
                }
            }
        }
    }

    // Grass clump rays — yellow vertical beacons
    if (grassRayVisible_ && grassRoot_ && debug)
    {
        const Vector<SharedPtr<Node>>& clumps = grassRoot_->GetChildren();
        unsigned visCount = 0;
        for (unsigned i = 0; i < clumps.Size(); ++i)
        {
            if (!clumps[i]->IsEnabled())
                continue;
            Vector3 base = clumps[i]->GetWorldPosition();
            Vector3 top = base + Vector3::UP * 2.0f;
            debug->AddLine(base, top, Color::YELLOW, false);
            ++visCount;
        }
        // Log once when toggled on
        static unsigned lastLoggedCount = 0;
        if (visCount != lastLoggedCount)
        {
            URHO3D_LOGINFOF("Grass rays: %u visible clumps", visCount);
            lastLoggedCount = visCount;
        }
    }

    if (!drawDebug_)
        return;

    // Editor overlays — gated by drawDebug_ (F5 / NumPad Enter)
    if (hasBrushHit_ && brushMode_ != 0)
        DrawBrushOutline(cachedBrushHit_);

    if (prefabBrush_ && hasBrushHit_ && debug)
    {
        BoundingBox combined;
        Vector<Drawable*> drawables;
        prefabBrush_->GetDerivedComponents<Drawable>(drawables, true);
        for (unsigned i = 0; i < drawables.Size(); ++i)
            combined.Merge(drawables[i]->GetBoundingBox());

        if (combined.Defined())
        {
            Vector3 pos(cachedBrushHit_.x_, cachedBrushHit_.y_, cachedBrushHit_.z_);
            Matrix3x4 transform(pos, cachedBrushNormal_ != Vector3::UP ?
                Quaternion(Vector3::UP, cachedBrushNormal_) : Quaternion::IDENTITY,
                prefabBrush_->GetScale());
            debug->AddBoundingBox(combined, transform, Color::CYAN, false);
        }
    }

    DrawGizmo();

    auto* physics = scene_->GetComponent<PhysicsWorld>();
    if (physics)
        physics->DrawDebugGeometry(true);
}

void TerrainNode::TestComputeShader()
{
    auto* graphics = GetSubsystem<Graphics>();
    auto* cache = GetSubsystem<ResourceCache>();

    // Load the test compute shader
    auto* shader = cache->GetResource<Shader>("Shaders/GLSL/TestCompute.glsl");
    if (!shader)
    {
        URHO3D_LOGERROR("TestCompute: Failed to load TestCompute.glsl");
        return;
    }

    ShaderVariation* cs = shader->GetVariation(CS, "");
    if (!cs)
    {
        URHO3D_LOGERROR("TestCompute: Failed to get compute shader variation");
        return;
    }

    // Create input buffer: 64 floats [1.0, 2.0, ... 64.0]
    const unsigned NUM_FLOATS = 64;
    float inputData[NUM_FLOATS];
    for (unsigned i = 0; i < NUM_FLOATS; ++i)
        inputData[i] = (float)(i + 1);

    // Create output buffer: 64 floats initialized to zero
    float outputData[NUM_FLOATS];
    memset(outputData, 0, sizeof(outputData));

    // Use VertexBuffers as SSBOs (they have STORAGE_BUFFER_BIT usage flag)
    // Each "vertex" is one float (4 bytes) — use a single FLOAT element
    Vector<VertexElement> elements;
    elements.Push(VertexElement(TYPE_FLOAT, SEM_POSITION));

    SharedPtr<VertexBuffer> inputBuffer(new VertexBuffer(context_));
    inputBuffer->SetShadowed(true);
    inputBuffer->SetSize(NUM_FLOATS, elements, false);
    inputBuffer->SetData(inputData);

    SharedPtr<VertexBuffer> outputBuffer(new VertexBuffer(context_));
    outputBuffer->SetShadowed(true);
    outputBuffer->SetSize(NUM_FLOATS, elements, false);
    outputBuffer->SetData(outputData);

    URHO3D_LOGINFO("TestCompute: Buffers created, dispatching compute shader...");

    // Bind compute shader and storage buffers
    graphics->SetComputeShader(cs);
    graphics->SetStorageBuffer(0, inputBuffer);
    graphics->SetStorageBuffer(1, outputBuffer);

    // Dispatch: 1 group of 64 threads
    graphics->DispatchCompute(1);

    URHO3D_LOGINFO("TestCompute: Dispatch complete — check log for errors");

    // Clean up
    graphics->SetComputeShader(nullptr);
    graphics->SetStorageBuffer(0, nullptr);
    graphics->SetStorageBuffer(1, nullptr);
}

void TerrainNode::RunErosion(int iterations)
{
    if (!terrain_ || !editableHeightMap_)
    {
        URHO3D_LOGERROR("RunErosion: No terrain or heightmap");
        return;
    }

    // Snapshot for undo before erosion
    SharedPtr<Image> beforeErosion = CloneHeightMap();

    auto* graphics = GetSubsystem<Graphics>();
    auto* cache = GetSubsystem<ResourceCache>();

    // Load erosion shader with two pass variations
    auto* shader = cache->GetResource<Shader>("Shaders/GLSL/Erosion.glsl");
    if (!shader)
    {
        URHO3D_LOGERROR("RunErosion: Failed to load Erosion.glsl");
        return;
    }

    ShaderVariation* passFlux = shader->GetVariation(CS, "PASS_FLUX");
    ShaderVariation* passTransport = shader->GetVariation(CS, "PASS_TRANSPORT");
    if (!passFlux || !passTransport)
    {
        URHO3D_LOGERROR("RunErosion: Failed to get shader variations");
        return;
    }

    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    int numCells = hmW * hmH;
    int comps = editableHeightMap_->GetComponents();
    unsigned char* hmData = editableHeightMap_->GetData();

    URHO3D_LOGINFO("RunErosion: " + String(iterations) + " iterations on " +
                   String(hmW) + "x" + String(hmH) + " heightmap");

    // Scale heights to world-space so the erosion algorithm has real gradients
    // Normalized [0,1] over 1025 cells gives ~0.001 gradient — too flat for meaningful flow
    Vector3 spacing = terrain_->GetSpacing();
    float heightScale = 255.0f * spacing.y_;  // world height range

    // Extract heights into float array (world-space)
    // Buffer layout: [0..N-1] = current heights, [N..2N-1] = original heights (for depth constraint)
    float* heightData = new float[numCells * 2];
    float hMin = 1e9f, hMax = -1e9f;
    for (int i = 0; i < numCells; ++i)
    {
        float h;
        if (comps >= 2)
        {
            int idx = i * comps;
            h = ((float)hmData[idx] + (float)hmData[idx + 1] / 256.0f) / 255.0f;
        }
        else
        {
            h = (float)hmData[i * comps] / 255.0f;
        }
        float worldH = h * heightScale;
        heightData[i] = worldH;              // current heights
        heightData[numCells + i] = worldH;   // original heights (copy)
        if (worldH < hMin) hMin = worldH;
        if (worldH > hMax) hMax = worldH;
    }
    URHO3D_LOGINFO("RunErosion: height range [" + String(hMin) + ", " + String(hMax) +
                   "], scale=" + String(heightScale));

    // Initialize water+sediment buffer (interleaved, all zeros)
    float* waterSedData = new float[numCells * 2];
    memset(waterSedData, 0, numCells * 2 * sizeof(float));

    // Initialize flux buffer (LRTB per cell, all zeros)
    float* fluxData = new float[numCells * 4];
    memset(fluxData, 0, numCells * 4 * sizeof(float));

    // Parameters tuned for world-space heights (~0-128 range)
    float paramData[16];
    memset(paramData, 0, sizeof(paramData));
    paramData[0] = 0.1f;               // dt (larger step = faster simulation)
    paramData[1] = erosionRainfall_ * heightScale;  // rainfall scaled to world-space
    paramData[2] = 9.81f;              // gravity
    paramData[3] = 1.0f;               // pipe area
    paramData[4] = 5.0f;               // Kc (sediment capacity — higher = more erosion before deposition)
    paramData[5] = erosionStrength_;   // Ks (erosion rate)
    paramData[6] = erosionStrength_ * 0.5f;  // Kd (deposition rate — lower than Ks so material moves further)
    paramData[7] = 0.01f;              // Ke (evaporation rate — slower so water persists longer)
    paramData[8] = (float)hmW;         // width
    paramData[9] = (float)hmH;         // height
    paramData[10] = erosionMaxDepth_ * heightScale;  // max carve depth in world units
    paramData[11] = erosionMinHeight_ * heightScale;  // min height floor in world units
    paramData[12] = erosionRidgeProtect_;              // ridge protection [0..1]
    paramData[13] = (float)erosionBorderPad_;          // border padding in cells

    // Create VertexBuffers as SSBOs
    Vector<VertexElement> floatElem;
    floatElem.Push(VertexElement(TYPE_FLOAT, SEM_POSITION));

    SharedPtr<VertexBuffer> heightBuf(new VertexBuffer(context_));
    heightBuf->SetShadowed(false);
    heightBuf->SetSize(numCells * 2, floatElem, false);  // 2x: current + original heights
    heightBuf->SetData(heightData);

    Vector<VertexElement> float2Elem;
    float2Elem.Push(VertexElement(TYPE_VECTOR2, SEM_POSITION));

    SharedPtr<VertexBuffer> waterSedBuf(new VertexBuffer(context_));
    waterSedBuf->SetShadowed(false);
    waterSedBuf->SetSize(numCells, float2Elem, false);
    waterSedBuf->SetData(waterSedData);

    Vector<VertexElement> float4Elem;
    float4Elem.Push(VertexElement(TYPE_VECTOR4, SEM_POSITION));

    SharedPtr<VertexBuffer> fluxBuf(new VertexBuffer(context_));
    fluxBuf->SetShadowed(false);
    fluxBuf->SetSize(numCells, float4Elem, false);
    fluxBuf->SetData(fluxData);

    SharedPtr<VertexBuffer> paramBuf(new VertexBuffer(context_));
    paramBuf->SetShadowed(false);
    paramBuf->SetSize(16, floatElem, false);
    paramBuf->SetData(paramData);

    // Bind all 4 SSBOs
    graphics->SetStorageBuffer(0, heightBuf);
    graphics->SetStorageBuffer(1, waterSedBuf);
    graphics->SetStorageBuffer(2, fluxBuf);
    graphics->SetStorageBuffer(3, paramBuf);

    // Dispatch groups: ceil(width/16) x ceil(height/16)
    unsigned groupsX = (hmW + 15) / 16;
    unsigned groupsY = (hmH + 15) / 16;

    // Batch all dispatches into a single command buffer for performance
    graphics->BeginComputeBatch();

    // Run erosion iterations
    for (int iter = 0; iter < iterations; ++iter)
    {
        // Pass A: Rain + Flux
        graphics->SetComputeShader(passFlux);
        graphics->DispatchCompute(groupsX, groupsY);

        // Pass B: Water transport + Erosion + Evaporation
        graphics->SetComputeShader(passTransport);
        graphics->DispatchCompute(groupsX, groupsY);
    }

    // Submit all dispatches and wait for GPU completion
    graphics->EndComputeBatch();

    URHO3D_LOGINFO("RunErosion: " + String(iterations) + " iterations complete, reading back...");

    // Read back the height buffer
    if (heightBuf->GetData(heightData))
    {
        // Convert from world-space back to normalized [0,1]
        float hMinOut = 1e9f, hMaxOut = -1e9f;
        for (int i = 0; i < numCells; ++i)
        {
            if (heightData[i] < hMinOut) hMinOut = heightData[i];
            if (heightData[i] > hMaxOut) hMaxOut = heightData[i];
        }
        // Compute change stats
        float totalDelta = 0.0f;
        float maxDelta = 0.0f;
        int changedCells = 0;
        for (int i = 0; i < numCells; ++i)
        {
            float origH = heightData[numCells + i];
            float delta = Abs(heightData[i] - origH);
            totalDelta += delta;
            if (delta > maxDelta) maxDelta = delta;
            if (delta > 0.001f) ++changedCells;
        }
        URHO3D_LOGINFOF("RunErosion: post range [%.2f, %.2f] changed=%d/%d maxDelta=%.4f totalDelta=%.2f",
                        hMinOut, hMaxOut, changedCells, numCells, maxDelta, totalDelta);

        for (int i = 0; i < numCells; ++i)
        {
            float h = heightData[i] / heightScale;  // back to [0,1]
            if (h < 0.0f) h = 0.0f;
            if (h > 1.0f) h = 1.0f;

            if (comps >= 4)
            {
                float scaled = h * 255.0f;
                int idx = i * comps;
                hmData[idx] = (unsigned char)scaled;
                float rem = (scaled - (float)hmData[idx]) * 256.0f;
                hmData[idx + 1] = (unsigned char)rem;
                float rem2 = (rem - (float)hmData[idx + 1]) * 256.0f;
                hmData[idx + 2] = (unsigned char)rem2;
                hmData[idx + 3] = (unsigned char)((rem2 - (float)hmData[idx + 2]) * 256.0f);
            }
            else if (comps >= 2)
            {
                float scaled = h * 255.0f;
                int idx = i * comps;
                hmData[idx] = (unsigned char)scaled;
                hmData[idx + 1] = (unsigned char)((scaled - (float)hmData[idx]) * 256.0f);
            }
            else
            {
                hmData[i * comps] = (unsigned char)(h * 255.0f);
            }
        }

        // Apply to terrain
        terrain_->ApplyHeightMap();
        WakeSleepingBodiesOnTerrain();
    

        // Push undo for the erosion operation
        UndoAction action;
        action.type = UndoAction::TERRAIN_EDIT;
        action.beforeHM = beforeErosion;
        action.afterHM = CloneHeightMap();
        PushUndo(action);

        URHO3D_LOGINFO("RunErosion: Complete — terrain updated");
    }
    else
    {
        URHO3D_LOGERROR("RunErosion: Failed to read back height data from GPU");
    }

    // Clean up
    graphics->SetComputeShader(nullptr);
    graphics->SetStorageBuffer(0, nullptr);
    graphics->SetStorageBuffer(1, nullptr);
    graphics->SetStorageBuffer(2, nullptr);
    graphics->SetStorageBuffer(3, nullptr);

    delete[] heightData;
    delete[] waterSedData;
    delete[] fluxData;
}

// ============================================================================
// Hierarchy Window
// ============================================================================

void TerrainNode::ToggleHierarchyWindow()
{
    if (hierarchyWindow_)
    {
        hierarchyWindow_->SetVisible(!hierarchyWindow_->IsVisible());
        if (hierarchyWindow_->IsVisible())
            BuildHierarchyTree();
        return;
    }

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();

    hierarchyWindow_ = new Window(context_);
    uiRoot->AddChild(hierarchyWindow_);
    hierarchyWindow_->SetStyleAuto();
    hierarchyWindow_->SetPosition(8, 36);
    hierarchyWindow_->SetSize(280, 500);
    hierarchyWindow_->SetResizable(true);
    hierarchyWindow_->SetMovable(true);
    hierarchyWindow_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));
    hierarchyWindow_->SetOpacity(0.85f);

    auto* title = hierarchyWindow_->CreateChild<Text>();
    title->SetFont(font, 12);
    title->SetText("Scene");
    title->SetColor(Color(0.9f, 0.9f, 0.3f));

    hierarchyList_ = hierarchyWindow_->CreateChild<ListView>();
    hierarchyList_->SetStyle("HierarchyListView");
    hierarchyList_->SetHighlightMode(HM_ALWAYS);
    hierarchyList_->SetMinSize(260, 440);

    SubscribeToEvent(hierarchyList_, E_SELECTIONCHANGED, URHO3D_HANDLER(TerrainNode, HandleHierarchySelectionChanged));
    SubscribeToEvent(hierarchyList_, E_ITEMDOUBLECLICKED, URHO3D_HANDLER(TerrainNode, HandleHierarchyDoubleClick));

    BuildHierarchyTree();
}

void TerrainNode::BuildHierarchyTree()
{
    if (!hierarchyList_ || !scene_)
        return;

    hierarchyList_->DisableInternalLayoutUpdate();
    hierarchyList_->RemoveAllItems();

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    // Root scene item
    auto* rootItem = new Text(context_);
    rootItem->SetFont(font, 11);
    rootItem->SetText("Scene");
    rootItem->SetColor(Color(1.0f, 1.0f, 0.7f));
    rootItem->SetFixedHeight(16);
    rootItem->SetVar(StringHash("NodeID"), scene_->GetID());
    hierarchyList_->AddItem(rootItem);

    unsigned index = 1;
    const auto& children = scene_->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
        PopulateHierarchy(children[i], rootItem, index);

    hierarchyList_->EnableInternalLayoutUpdate();
    hierarchyList_->UpdateInternalLayout();

    // Expand root
    hierarchyList_->Expand(0, true);
}

void TerrainNode::PopulateHierarchy(Node* node, Text* parentItem, unsigned& index)
{
    if (!node)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    String name = node->GetName().Empty() ? String("Node ") + String(node->GetID()) : node->GetName();

    auto* item = new Text(context_);
    item->SetFont(font, 11);
    item->SetText(name);
    item->SetColor(Color::WHITE);
    item->SetFixedHeight(16);
    item->SetVar(StringHash("NodeID"), node->GetID());
    hierarchyList_->InsertItem(index, item, parentItem);
    index++;

    // Components (green text)
    const auto& components = node->GetComponents();
    for (unsigned c = 0; c < components.Size(); ++c)
    {
        Component* comp = components[c];
        auto* compItem = new Text(context_);
        compItem->SetFont(font, 10);
        compItem->SetText("  [" + comp->GetTypeName() + "]");
        compItem->SetColor(Color(0.4f, 0.9f, 0.4f));
        compItem->SetFixedHeight(16);
        compItem->SetVar(StringHash("NodeID"), node->GetID());
        compItem->SetVar(StringHash("CompIndex"), c);
        hierarchyList_->InsertItem(index, compItem, item);
        index++;
    }

    // Recurse children
    const auto& children = node->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
        PopulateHierarchy(children[i], item, index);
}

void TerrainNode::HandleHierarchySelectionChanged(StringHash eventType, VariantMap& eventData)
{
    auto* selected = hierarchyList_->GetSelectedItem();
    if (!selected)
        return;

    const Variant& nodeIDVar = selected->GetVar(StringHash("NodeID"));
    if (nodeIDVar.IsEmpty())
        return;

    unsigned nodeID = nodeIDVar.GetU32();
    Node* node = scene_->GetNode(nodeID);
    if (node && node != scene_ && node != selectedNode_)
        SelectNode(node);
}

void TerrainNode::HandleHierarchyDoubleClick(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemDoubleClicked;
    auto* item = static_cast<UIElement*>(eventData[P_ITEM].GetPtr());
    if (!item)
        return;

    const Variant& nodeIDVar = item->GetVar(StringHash("NodeID"));
    if (nodeIDVar.IsEmpty())
        return;

    unsigned nodeID = nodeIDVar.GetU32();
    Node* node = scene_->GetNode(nodeID);
    if (node && node != scene_ && cameraNode_)
    {
        // Focus camera on node
        Vector3 target = node->GetWorldPosition();
        Vector3 dir = (target - cameraNode_->GetPosition());
        float dist = dir.Length();
        if (dist > 1.0f)
        {
            dir.Normalize();
            cameraNode_->SetPosition(target - dir * Min(dist, 20.0f));
            yaw_ = atan2f(dir.x_, dir.z_) * M_RADTODEG;
            pitch_ = asinf(Clamp(dir.y_, -1.0f, 1.0f)) * M_RADTODEG;
            cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
        }
    }
}

void TerrainNode::HighlightInHierarchy(Node* node)
{
    if (!hierarchyList_ || !hierarchyWindow_ || !hierarchyWindow_->IsVisible())
        return;

    if (!node)
    {
        hierarchyList_->ClearSelection();
        return;
    }

    unsigned targetID = node->GetID();
    for (int i = 0; i < hierarchyList_->GetNumItems(); ++i)
    {
        auto* item = hierarchyList_->GetItem(i);
        if (!item)
            continue;
        const Variant& v = item->GetVar(StringHash("NodeID"));
        if (!v.IsEmpty() && v.GetU32() == targetID)
        {
            // Only select if it's a node item (not a component item)
            const Variant& compVar = item->GetVar(StringHash("CompIndex"));
            if (compVar.IsEmpty())
            {
                hierarchyList_->SetSelection(i);
                hierarchyList_->EnsureItemVisibility(i);
                break;
            }
        }
    }
}

// ============================================================================
// Inspector Window
// ============================================================================

void TerrainNode::ToggleInspectorWindow()
{
    if (inspectorWindow_)
    {
        inspectorWindow_->SetVisible(!inspectorWindow_->IsVisible());
        if (inspectorWindow_->IsVisible())
            RebuildInspector();
        return;
    }

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();
    auto* graphics = GetSubsystem<Graphics>();

    inspectorWindow_ = new Window(context_);
    uiRoot->AddChild(inspectorWindow_);
    inspectorWindow_->SetStyleAuto();
    inspectorWindow_->SetPosition(graphics->GetWidth() - 308, 36);
    inspectorWindow_->SetSize(300, 500);
    inspectorWindow_->SetResizable(true);
    inspectorWindow_->SetMovable(true);
    inspectorWindow_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));
    inspectorWindow_->SetOpacity(0.85f);

    auto* title = inspectorWindow_->CreateChild<Text>();
    title->SetFont(font, 12);
    title->SetText("Inspector");
    title->SetColor(Color(0.9f, 0.9f, 0.3f));

    inspectorScroll_ = inspectorWindow_->CreateChild<ScrollView>();
    inspectorScroll_->SetStyleAuto();
    inspectorScroll_->SetMinSize(280, 440);

    inspectorContent_ = new UIElement(context_);
    inspectorContent_->SetLayout(LM_VERTICAL, 2, IntRect(2, 2, 2, 2));
    inspectorContent_->SetMinWidth(270);
    inspectorScroll_->SetContentElement(inspectorContent_);

    RebuildInspector();
}

void TerrainNode::RebuildInspector()
{
    if (!inspectorContent_)
        return;

    inspectorContent_->RemoveAllChildren();

    if (!selectedNode_ || selectedNode_.Expired())
        return;

    Node* node = selectedNode_;
    CreateNodeSection(node);

    const auto& components = node->GetComponents();
    for (unsigned i = 0; i < components.Size(); ++i)
        CreateComponentSection(components[i], i);

    inspectorContent_->SetMinHeight(inspectorContent_->GetNumChildren() * 22 + 40);
}

void TerrainNode::CreateNodeSection(Node* node)
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    // Header
    auto* header = inspectorContent_->CreateChild<Text>();
    header->SetFont(font, 12);
    header->SetText("Node: " + (node->GetName().Empty() ? String("ID ") + String(node->GetID()) : node->GetName()));
    header->SetColor(Color(0.9f, 0.9f, 0.3f));

    // Name
    auto* nameRow = inspectorContent_->CreateChild<UIElement>();
    nameRow->SetLayout(LM_HORIZONTAL, 4, IntRect(2, 1, 2, 1));
    nameRow->SetFixedHeight(20);

    auto* nameLabel = nameRow->CreateChild<Text>();
    nameLabel->SetFont(font, 11);
    nameLabel->SetText("Name");
    nameLabel->SetColor(Color(0.8f, 0.8f, 0.8f));
    nameLabel->SetMinWidth(70);

    auto* nameEdit = nameRow->CreateChild<LineEdit>();
    nameEdit->SetStyleAuto();
    nameEdit->SetText(node->GetName());
    nameEdit->SetMinWidth(180);
    nameEdit->SetFixedHeight(18);
    nameEdit->SetVar(StringHash("Field"), String("Name"));
    SubscribeToEvent(nameEdit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorTransformEdit));

    // Position
    CreateVec3Row(inspectorContent_, "Pos", node->GetPosition(), "Position");

    // Rotation (as Euler)
    Vector3 euler = node->GetRotation().EulerAngles();
    CreateVec3Row(inspectorContent_, "Rot", euler, "Rotation");

    // Scale
    CreateVec3Row(inspectorContent_, "Scale", node->GetScale(), "Scale");
}

LineEdit* TerrainNode::CreateVec3Row(UIElement* parent, const String& label, const Vector3& value, const String& tag)
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    auto* row = parent->CreateChild<UIElement>();
    row->SetLayout(LM_HORIZONTAL, 2, IntRect(2, 1, 2, 1));
    row->SetFixedHeight(20);

    auto* lbl = row->CreateChild<Text>();
    lbl->SetFont(font, 11);
    lbl->SetText(label);
    lbl->SetColor(Color(0.8f, 0.8f, 0.8f));
    lbl->SetMinWidth(42);

    const char* axes[] = {"X", "Y", "Z"};
    const float vals[] = {value.x_, value.y_, value.z_};

    for (int i = 0; i < 3; ++i)
    {
        auto* axLbl = row->CreateChild<Text>();
        axLbl->SetFont(font, 10);
        axLbl->SetText(axes[i]);
        axLbl->SetColor(i == 0 ? Color(1.0f, 0.3f, 0.3f) : (i == 1 ? Color(0.3f, 1.0f, 0.3f) : Color(0.3f, 0.3f, 1.0f)));
        axLbl->SetMinWidth(10);

        auto* edit = row->CreateChild<LineEdit>();
        edit->SetStyleAuto();
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", vals[i]);
        edit->SetText(buf);
        edit->SetMinWidth(54);
        edit->SetFixedHeight(18);
        edit->SetVar(StringHash("Field"), tag);
        edit->SetVar(StringHash("Axis"), i);
        SubscribeToEvent(edit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorTransformEdit));
    }

    return nullptr;
}

void TerrainNode::HandleInspectorTransformEdit(StringHash eventType, VariantMap& eventData)
{
    using namespace TextFinished;
    auto* edit = static_cast<LineEdit*>(eventData[P_ELEMENT].GetPtr());
    if (!edit || !selectedNode_ || selectedNode_.Expired())
        return;

    Node* node = selectedNode_;
    String field = edit->GetVar(StringHash("Field")).GetString();

    if (field == "Name")
    {
        node->SetName(edit->GetText());
        if (hierarchyWindow_ && hierarchyWindow_->IsVisible())
            BuildHierarchyTree();
        return;
    }

    int axis = edit->GetVar(StringHash("Axis")).GetI32();
    float val = ToFloat(edit->GetText());

    if (field == "Position")
    {
        Vector3 pos = node->GetPosition();
        if (axis == 0) pos.x_ = val;
        else if (axis == 1) pos.y_ = val;
        else pos.z_ = val;
        node->SetPosition(pos);
    }
    else if (field == "Rotation")
    {
        Vector3 euler = node->GetRotation().EulerAngles();
        if (axis == 0) euler.x_ = val;
        else if (axis == 1) euler.y_ = val;
        else euler.z_ = val;
        node->SetRotation(Quaternion(euler.x_, euler.y_, euler.z_));
    }
    else if (field == "Scale")
    {
        Vector3 scale = node->GetScale();
        if (axis == 0) scale.x_ = val;
        else if (axis == 1) scale.y_ = val;
        else scale.z_ = val;
        node->SetScale(scale);
    }
}

void TerrainNode::CreateComponentSection(Component* component, unsigned compIndex)
{
    if (!component)
        return;

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    // Component header
    auto* header = inspectorContent_->CreateChild<Text>();
    header->SetFont(font, 12);
    header->SetText(component->GetTypeName());
    header->SetColor(Color(0.4f, 0.9f, 0.4f));

    const Vector<AttributeInfo>* attrs = component->GetAttributes();
    if (!attrs)
        return;

    for (unsigned i = 0; i < attrs->Size(); ++i)
    {
        const AttributeInfo& attr = (*attrs)[i];

        // Skip non-editable attributes
        if (attr.mode_ & AM_NOEDIT)
            continue;

        Variant value = component->GetAttribute(i);

        auto* row = inspectorContent_->CreateChild<UIElement>();
        row->SetLayout(LM_HORIZONTAL, 4, IntRect(2, 1, 2, 1));
        row->SetFixedHeight(20);

        auto* lbl = row->CreateChild<Text>();
        lbl->SetFont(font, 10);
        lbl->SetText(attr.name_);
        lbl->SetColor(Color(0.8f, 0.8f, 0.8f));
        lbl->SetMinWidth(90);

        switch (attr.type_)
        {
        case VAR_FLOAT:
        case VAR_INT:
        case VAR_STRING:
        {
            auto* edit = row->CreateChild<LineEdit>();
            edit->SetStyleAuto();
            edit->SetText(value.ToString());
            edit->SetMinWidth(140);
            edit->SetFixedHeight(18);
            edit->SetVar(StringHash("CompIndex"), compIndex);
            edit->SetVar(StringHash("AttrIndex"), i);
            SubscribeToEvent(edit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorAttributeEdit));
            break;
        }
        case VAR_BOOL:
        {
            auto* cb = row->CreateChild<CheckBox>();
            cb->SetStyleAuto();
            cb->SetChecked(value.GetBool());
            cb->SetVar(StringHash("CompIndex"), compIndex);
            cb->SetVar(StringHash("AttrIndex"), i);
            SubscribeToEvent(cb, E_TOGGLED, URHO3D_HANDLER(TerrainNode, HandleInspectorCheckToggle));
            break;
        }
        case VAR_VECTOR3:
        {
            Vector3 v = value.GetVector3();
            const float vals[] = {v.x_, v.y_, v.z_};
            const char* axes[] = {"X", "Y", "Z"};
            for (int a = 0; a < 3; ++a)
            {
                auto* edit = row->CreateChild<LineEdit>();
                edit->SetStyleAuto();
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", vals[a]);
                edit->SetText(buf);
                edit->SetMinWidth(44);
                edit->SetFixedHeight(18);
                edit->SetVar(StringHash("CompIndex"), compIndex);
                edit->SetVar(StringHash("AttrIndex"), i);
                edit->SetVar(StringHash("Axis"), a);
                edit->SetVar(StringHash("AttrType"), (int)VAR_VECTOR3);
                SubscribeToEvent(edit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorAttributeEdit));
            }
            break;
        }
        case VAR_QUATERNION:
        {
            Vector3 euler = value.GetQuaternion().EulerAngles();
            const float vals[] = {euler.x_, euler.y_, euler.z_};
            for (int a = 0; a < 3; ++a)
            {
                auto* edit = row->CreateChild<LineEdit>();
                edit->SetStyleAuto();
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f", vals[a]);
                edit->SetText(buf);
                edit->SetMinWidth(44);
                edit->SetFixedHeight(18);
                edit->SetVar(StringHash("CompIndex"), compIndex);
                edit->SetVar(StringHash("AttrIndex"), i);
                edit->SetVar(StringHash("Axis"), a);
                edit->SetVar(StringHash("AttrType"), (int)VAR_QUATERNION);
                SubscribeToEvent(edit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorAttributeEdit));
            }
            break;
        }
        case VAR_COLOR:
        {
            Color c = value.GetColor();
            const float vals[] = {c.r_, c.g_, c.b_, c.a_};
            const char* chans[] = {"R", "G", "B", "A"};
            for (int a = 0; a < 4; ++a)
            {
                auto* edit = row->CreateChild<LineEdit>();
                edit->SetStyleAuto();
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", vals[a]);
                edit->SetText(buf);
                edit->SetMinWidth(34);
                edit->SetFixedHeight(18);
                edit->SetVar(StringHash("CompIndex"), compIndex);
                edit->SetVar(StringHash("AttrIndex"), i);
                edit->SetVar(StringHash("Axis"), a);
                edit->SetVar(StringHash("AttrType"), (int)VAR_COLOR);
                SubscribeToEvent(edit, E_TEXTFINISHED, URHO3D_HANDLER(TerrainNode, HandleInspectorAttributeEdit));
            }
            break;
        }
        default:
        {
            // Enum type
            if (attr.enumNames_)
            {
                auto* dd = row->CreateChild<DropDownList>();
                dd->SetStyleAuto();
                dd->SetMinWidth(140);
                dd->SetFixedHeight(18);
                dd->SetVar(StringHash("CompIndex"), compIndex);
                dd->SetVar(StringHash("AttrIndex"), i);

                int enumIdx = 0;
                for (const char** en = attr.enumNames_; *en; ++en, ++enumIdx)
                {
                    auto* t = new Text(context_);
                    t->SetFont(font, 10);
                    t->SetText(*en);
                    dd->AddItem(t);
                }
                dd->SetSelection(value.GetI32());
                SubscribeToEvent(dd, E_ITEMSELECTED, URHO3D_HANDLER(TerrainNode, HandleInspectorEnumSelect));
            }
            else
            {
                // Read-only display
                auto* valText = row->CreateChild<Text>();
                valText->SetFont(font, 10);
                valText->SetText(value.ToString());
                valText->SetColor(Color(0.6f, 0.6f, 0.6f));
            }
            break;
        }
        }
    }
}

void TerrainNode::HandleInspectorAttributeEdit(StringHash eventType, VariantMap& eventData)
{
    using namespace TextFinished;
    auto* edit = static_cast<LineEdit*>(eventData[P_ELEMENT].GetPtr());
    if (!edit || !selectedNode_ || selectedNode_.Expired())
        return;

    unsigned compIdx = edit->GetVar(StringHash("CompIndex")).GetU32();
    unsigned attrIdx = edit->GetVar(StringHash("AttrIndex")).GetU32();

    Node* node = selectedNode_;
    const auto& components = node->GetComponents();
    if (compIdx >= components.Size())
        return;

    Component* comp = components[compIdx];
    const Vector<AttributeInfo>* attrs = comp->GetAttributes();
    if (!attrs || attrIdx >= attrs->Size())
        return;

    const AttributeInfo& attr = (*attrs)[attrIdx];
    int attrType = edit->GetVar(StringHash("AttrType")).GetI32();

    if (attrType == (int)VAR_VECTOR3)
    {
        int axis = edit->GetVar(StringHash("Axis")).GetI32();
        Vector3 v = comp->GetAttribute(attrIdx).GetVector3();
        float val = ToFloat(edit->GetText());
        if (axis == 0) v.x_ = val;
        else if (axis == 1) v.y_ = val;
        else v.z_ = val;
        comp->SetAttribute(attrIdx, v);
    }
    else if (attrType == (int)VAR_QUATERNION)
    {
        int axis = edit->GetVar(StringHash("Axis")).GetI32();
        Vector3 euler = comp->GetAttribute(attrIdx).GetQuaternion().EulerAngles();
        float val = ToFloat(edit->GetText());
        if (axis == 0) euler.x_ = val;
        else if (axis == 1) euler.y_ = val;
        else euler.z_ = val;
        comp->SetAttribute(attrIdx, Quaternion(euler.x_, euler.y_, euler.z_));
    }
    else if (attrType == (int)VAR_COLOR)
    {
        int axis = edit->GetVar(StringHash("Axis")).GetI32();
        Color c = comp->GetAttribute(attrIdx).GetColor();
        float val = ToFloat(edit->GetText());
        if (axis == 0) c.r_ = val;
        else if (axis == 1) c.g_ = val;
        else if (axis == 2) c.b_ = val;
        else c.a_ = val;
        comp->SetAttribute(attrIdx, c);
    }
    else
    {
        // Simple types
        switch (attr.type_)
        {
        case VAR_FLOAT:
            comp->SetAttribute(attrIdx, ToFloat(edit->GetText()));
            break;
        case VAR_INT:
            comp->SetAttribute(attrIdx, (int)strtol(edit->GetText().CString(), nullptr, 10));
            break;
        case VAR_STRING:
            comp->SetAttribute(attrIdx, edit->GetText());
            break;
        default:
            break;
        }
    }

    comp->ApplyAttributes();
}

void TerrainNode::HandleInspectorCheckToggle(StringHash eventType, VariantMap& eventData)
{
    using namespace Toggled;
    auto* cb = static_cast<CheckBox*>(eventData[P_ELEMENT].GetPtr());
    if (!cb || !selectedNode_ || selectedNode_.Expired())
        return;

    unsigned compIdx = cb->GetVar(StringHash("CompIndex")).GetU32();
    unsigned attrIdx = cb->GetVar(StringHash("AttrIndex")).GetU32();

    Node* node = selectedNode_;
    const auto& components = node->GetComponents();
    if (compIdx >= components.Size())
        return;

    Component* comp = components[compIdx];
    comp->SetAttribute(attrIdx, eventData[P_STATE].GetBool());
    comp->ApplyAttributes();
}

void TerrainNode::HandleInspectorEnumSelect(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemSelected;
    auto* dd = static_cast<DropDownList*>(eventData[P_ELEMENT].GetPtr());
    if (!dd || !selectedNode_ || selectedNode_.Expired())
        return;

    unsigned compIdx = dd->GetVar(StringHash("CompIndex")).GetU32();
    unsigned attrIdx = dd->GetVar(StringHash("AttrIndex")).GetU32();
    int sel = eventData[P_SELECTION].GetI32();

    Node* node = selectedNode_;
    const auto& components = node->GetComponents();
    if (compIdx >= components.Size())
        return;

    Component* comp = components[compIdx];
    comp->SetAttribute(attrIdx, sel);
    comp->ApplyAttributes();
}

// ============================================================================
// Debug Log Window
// ============================================================================

void TerrainNode::CreateDebugLogWindow()
{
    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;
    auto* uiRoot = GetSubsystem<UI>()->GetRoot();
    auto* graphics = GetSubsystem<Graphics>();

    debugLogWindow_ = new Window(context_);
    uiRoot->AddChild(debugLogWindow_);
    debugLogWindow_->SetStyleAuto();
    debugLogWindow_->SetPosition(10, graphics->GetHeight() - 310);
    debugLogWindow_->SetSize(graphics->GetWidth() - 20, 280);
    debugLogWindow_->SetResizable(true);
    debugLogWindow_->SetMovable(true);
    debugLogWindow_->SetLayout(LM_VERTICAL, 2, IntRect(4, 4, 4, 4));
    debugLogWindow_->SetOpacity(0.85f);

    auto* title = debugLogWindow_->CreateChild<Text>();
    title->SetFont(font, 12);
    title->SetText("Debug Log");
    title->SetColor(Color(0.5f, 0.7f, 1.0f));

    debugLogList_ = debugLogWindow_->CreateChild<ListView>();
    debugLogList_->SetStyleAuto();
    debugLogList_->SetMinHeight(240);
}

void TerrainNode::ToggleDebugLogWindow()
{
    if (debugLogWindow_)
    {
        debugLogWindow_->SetVisible(!debugLogWindow_->IsVisible());
        return;
    }
    CreateDebugLogWindow();
}

void TerrainNode::HandleLogMessage(StringHash eventType, VariantMap& eventData)
{
    using namespace LogMessage;

    if (!debugLogList_ || !debugLogWindow_ || !debugLogWindow_->IsVisible())
        return;

    int level = eventData[P_LEVEL].GetI32();
    String message = eventData[P_MESSAGE].GetString();

    auto* cache = GetSubsystem<ResourceCache>();
    Font* font = font_;

    auto* line = new Text(context_);
    line->SetFont(font, 11);
    line->SetText(message);

    // Color by log level
    switch (level)
    {
    case LOG_ERROR:
        line->SetColor(Color(1.0f, 0.3f, 0.3f));
        break;
    case LOG_WARNING:
        line->SetColor(Color(1.0f, 0.8f, 0.2f));
        break;
    case LOG_INFO:
        line->SetColor(Color(0.75f, 0.75f, 0.75f));
        break;
    default:  // LOG_DEBUG, LOG_TRACE
        line->SetColor(Color(0.5f, 0.5f, 0.5f));
        break;
    }

    debugLogList_->AddItem(line);

    // Cap log length
    while (debugLogList_->GetNumItems() > MAX_DEBUG_LOG_LINES)
        debugLogList_->RemoveItem((i32)0);

    // Auto-scroll to bottom
    debugLogList_->EnsureItemVisibility(debugLogList_->GetNumItems() - 1);
}

// ============================================================================
// AuthServer Discovery & Connection
// ============================================================================

void TerrainNode::DiscoverAuthServer()
{
    auto* network = GetSubsystem<Network>();
    URHO3D_LOGINFOF("LAN discovery ping on port %d...", (int)authServerPort_);
    network->DiscoverHosts(authServerPort_);
}

void TerrainNode::HandleHostDiscovered(StringHash eventType, VariantMap& eventData)
{
    using namespace NetworkHostDiscovered;

    String address = eventData[P_ADDRESS].GetString();
    int port = eventData[P_PORT].GetI32();
    VariantMap beacon = eventData[P_BEACON].GetVariantMap();

    String serverName = beacon["ServerName"].GetString();
    if (serverName != "AuthServer")
        return;  // Not our server

    URHO3D_LOGINFOF("Discovered AuthServer at %s:%d", address.CString(), port);

    authServerAddress_ = address;
    authServerPort_ = (unsigned short)port;
    authDiscovering_ = false;

    // Update whichever status text is active
    String statusMsg = "Found " + address + ":" + String(port) + " — connecting...";
    if (loginStatusText_)
        loginStatusText_->SetText(statusMsg);
    if (networkStatusText_)
        networkStatusText_->SetText(statusMsg);

    // Auto-connect
    auto* network = GetSubsystem<Network>();
    network->Connect(address, (unsigned short)port, scene_);
    UpdateAuthButtonState();
}

void TerrainNode::HandleServerConnected(StringHash eventType, VariantMap& eventData)
{
    authConnected_ = true;
    authDiscovering_ = false;
    URHO3D_LOGINFO("Connected to AuthServer at " + authServerAddress_ + ":" + String(authServerPort_));

    auto* network = GetSubsystem<Network>();
    String statusMsg = "Connected to " + authServerAddress_ + ":" + String(authServerPort_);
    if (loginStatusText_)
    {
        // PAKE: connection + key exchange + auth happen together
        loginStatusText_->SetText(network->HasCredentials() ? "Authenticating..." : "Connected — enter credentials");
        loginStatusText_->SetColor(network->HasCredentials() ? Color(0.7f, 0.7f, 0.7f) : Color(0.3f, 1.0f, 0.3f));
    }
    if (networkStatusText_)
    {
        networkStatusText_->SetText(statusMsg);
        networkStatusText_->SetColor(Color(0.3f, 1.0f, 0.3f));
    }
    UpdateAuthButtonState();
}

void TerrainNode::HandleServerDisconnected(StringHash eventType, VariantMap& eventData)
{
    bool wasPake = false;
    auto* network = GetSubsystem<Network>();
    if (network && network->HasCredentials())
    {
        wasPake = true;
        network->ClearCredentials();
    }

    authConnected_ = false;
    URHO3D_LOGINFO("Disconnected from AuthServer");

    // If we were in-world, tear down and return to login screen
    if (loggedIn_)
    {
        ReturnToLogin();
        // loginStatusText_ is now valid (recreated by ReturnToLogin)
        if (loginStatusText_)
        {
            loginStatusText_->SetText("Lost connection to server");
            loginStatusText_->SetColor(Color(1.0f, 0.6f, 0.2f));
        }
        return;
    }

    // Still on login screen — update status text
    if (loginStatusText_)
    {
        // If PAKE was active and we got disconnected before auth completed = wrong password
        loginStatusText_->SetText(wasPake ? "Authentication failed — check password" : "Disconnected from server");
        loginStatusText_->SetColor(Color(1.0f, 0.3f, 0.3f));
    }
    // networkStatusText_ only exists in-world, handled above
    // if (networkStatusText_)
    // {
    //     networkStatusText_->SetText("Disconnected");
    //     networkStatusText_->SetColor(Color(1.0f, 0.6f, 0.2f));
    // }
    UpdateAuthButtonState();
}

void TerrainNode::HandleConnectFailed(StringHash eventType, VariantMap& eventData)
{
    authConnected_ = false;
    URHO3D_LOGWARNING("Failed to connect to AuthServer");

    String statusMsg = "Connect failed — is AuthServer running?";
    if (loginStatusText_)
    {
        loginStatusText_->SetText(statusMsg);
        loginStatusText_->SetColor(Color(1.0f, 0.3f, 0.3f));
    }
    if (networkStatusText_)
    {
        networkStatusText_->SetText(statusMsg);
        networkStatusText_->SetColor(Color(1.0f, 0.3f, 0.3f));
    }
    UpdateAuthButtonState();
}

void TerrainNode::HandleAuthMessage(StringHash eventType, VariantMap& eventData)
{
    using namespace NetworkMessage;
    int msgID = eventData[P_MESSAGEID].GetI32();
    const auto& data = eventData[P_DATA].GetBuffer();

    if (msgID == MSG_PEER_INTRODUCE)
    {
        MemoryBuffer msg(data);
        String peerGuid = msg.ReadString();
        Vector<unsigned char> token(32);
        msg.Read(token.Buffer(), 32);
        int patchX = msg.ReadI32();
        int patchZ = msg.ReadI32();

        // Store peer introduction state
        peerGuid_ = peerGuid;
        peerToken_ = token;
        peerPatchX_ = patchX;
        peerPatchZ_ = patchZ;

        auto* network = GetSubsystem<Network>();
        network->SetExpectedPeerToken(token);
        network->SetPendingPeerGuid(peerGuid);
        network->SetPendingPeerPatch(patchX, patchZ);

        URHO3D_LOGINFOF("Peer introduced: GUID=%s, patch=(%d,%d) — starting NAT punchthrough",
            peerGuid.CString(), patchX, patchZ);

        // Attempt NAT punchthrough to the peer
        network->AttemptNATPunchtrough(peerGuid, scene_);
        return;
    }

    if (msgID == MSG_RELAY_FROM_AUTH)
    {
        // Subserver relaying an AuthServer response to us (subclient)
        MemoryBuffer msg(data);
        String targetUsername = msg.ReadString();
        int innerMsgID = msg.ReadI32();
        unsigned innerSize = msg.ReadU32();
        Vector<unsigned char> innerData(innerSize);
        if (innerSize > 0)
            msg.Read(innerData.Buffer(), innerSize);

        URHO3D_LOGINFOF("Relay from AuthServer via subserver: innerMsgID=%d, size=%u",
            innerMsgID, innerSize);

        // Re-dispatch the inner message through HandleAuthMessage
        VariantMap relayEvent;
        relayEvent[NetworkMessage::P_MESSAGEID] = innerMsgID;
        relayEvent[NetworkMessage::P_DATA].SetBuffer(reinterpret_cast<const void*>(innerData.Buffer()), innerSize);
        HandleAuthMessage(eventType, relayEvent);
        return;
    }

    if (msgID == MSG_EDIT_REJECT)
    {
        MemoryBuffer rejectMsg(data);
        HandleEditReject(rejectMsg);
        return;
    }

    if (msgID == MSG_EDIT_BROADCAST)
    {
        MemoryBuffer broadcastMsg(data);
        HandleEditBroadcast(broadcastMsg);
        return;
    }

    if (msgID == MSG_WEATHER_UPDATE)
    {
        MemoryBuffer msg(data);
        WeatherState forecast;
        forecast.cloudCover = msg.ReadFloat();
        forecast.precipitation = msg.ReadFloat();
        forecast.windSpeed = msg.ReadFloat();
        forecast.windAngle = msg.ReadFloat();
        float temperature = msg.ReadFloat();
        float humidity = msg.ReadFloat();
        String condition = msg.ReadString();

        // Derive fog/ambient multipliers from cloud cover and precipitation
        forecast.fogDensity = 1.0f - forecast.cloudCover * 0.3f - forecast.precipitation * 0.3f;
        forecast.fogDensity = Clamp(forecast.fogDensity, 0.3f, 1.0f);
        forecast.ambientDim = 1.0f - forecast.cloudCover * 0.4f;
        forecast.ambientDim = Clamp(forecast.ambientDim, 0.4f, 1.0f);

        // Set as target — weather system will lerp toward it
        weatherTarget_ = forecast;
        weatherOverride_ = false;  // server authority overrides manual sliders
        weatherTransitionTimer_ = weatherTransitionDuration_;

        URHO3D_LOGINFOF("Weather from server: %s, cloud=%d%%, precip=%d%%, wind=%.0f, temp=%.1fC",
            condition.CString(), (int)(forecast.cloudCover * 100), (int)(forecast.precipitation * 100),
            forecast.windSpeed, temperature);
        return;
    }

    if (msgID == MSG_RESOURCE_PATCH)
    {
        MemoryBuffer msg(data);
        HandleResourcePatch(msg);
        return;
    }

    if (msgID == MSG_AUTH_RESULT)
    {
        MemoryBuffer msg(data);
        int originalMsgID = msg.ReadI32();  // echoed msg ID (LOGIN or REGISTER)
        bool success = msg.ReadBool();
        String message = msg.ReadString();

        URHO3D_LOGINFOF("Auth result: %s — %s", success ? "OK" : "FAIL", message.CString());

        if (originalMsgID == MSG_AUTH_LOGIN)
        {
            adminLevel_ = msg.ReadI32();
            serverSceneName_ = msg.ReadString();  // scene name packed into auth result
            int patchCount = msg.ReadI32();
            ownedPatches_.Clear();
            for (int i = 0; i < patchCount; ++i)
            {
                int px = msg.ReadI32();
                int pz = msg.ReadI32();
                ownedPatches_.Push(IntVector2(px, pz));
            }
            // First patch is the home patch (for camera positioning)
            if (!ownedPatches_.Empty())
            {
                ownedPatchX_ = ownedPatches_[0].x_;
                ownedPatchZ_ = ownedPatches_[0].y_;
            }
            URHO3D_LOGINFOF("Received %u owned patches, adminLevel=%d:", ownedPatches_.Size(), adminLevel_);
            for (unsigned pi = 0; pi < ownedPatches_.Size(); ++pi)
                URHO3D_LOGINFOF("  patch[%u] = (%d, %d)", pi, ownedPatches_[pi].x_, ownedPatches_[pi].y_);

            if (success)
            {
                loggedInUsername_ = usernameEdit_ ? usernameEdit_->GetText().Trimmed() : "Unknown";
                URHO3D_LOGINFOF("Logged in as %s (admin level %d), scene: %s, patch: (%d,%d)",
                    loggedInUsername_.CString(), adminLevel_, serverSceneName_.CString(),
                    ownedPatchX_, ownedPatchZ_);

                // Register our NAT GUID with AuthServer for peer introductions
                RegisterGuidWithAuthServer();

                loginTimer_.Reset();
                loginTimerActive_ = true;
                firstFramePending_ = true;
                EnterWorld();
            }
            else if (loginStatusText_)
            {
                loginStatusText_->SetText(message);
                loginStatusText_->SetColor(Color(1.0f, 0.3f, 0.3f));
            }
        }
        else if (originalMsgID == MSG_AUTH_REGISTER)
        {
            if (loginStatusText_)
            {
                loginStatusText_->SetText(message);
                loginStatusText_->SetColor(success ? Color(0.3f, 1.0f, 0.3f) : Color(1.0f, 0.3f, 0.3f));
            }
        }
    }
}

void TerrainNode::HandleAsyncLoadFinished(StringHash /*eventType*/, VariantMap& eventData)
{
    asyncSceneLoading_ = false;
    // Single entry point for all post-scene-load work
    OnSceneLoaded();
}

void TerrainNode::HandleAuthConnectButton(StringHash eventType, VariantMap& eventData)
{
    auto* network = GetSubsystem<Network>();

    if (authConnected_)
    {
        // Disconnect
        network->Disconnect(100);
        authConnected_ = false;
        URHO3D_LOGINFO("Disconnected from AuthServer (manual)");
    }
    else
    {
        // Connect to localhost
        URHO3D_LOGINFOF("Connecting to AuthServer at %s:%d...", authServerAddress_.CString(), (int)authServerPort_);
        network->Connect(authServerAddress_, authServerPort_, scene_);
    }
    UpdateAuthButtonState();
}

void TerrainNode::UpdateAuthButtonState()
{
    if (!authBtnLabel_)
        return;

    if (authConnected_)
    {
        authBtnLabel_->SetText("AuthServer: ON");
        authBtnLabel_->SetColor(Color(0.2f, 1.0f, 0.2f));
    }
    else
    {
        authBtnLabel_->SetText("AuthServer: OFF");
        authBtnLabel_->SetColor(Color(1.0f, 0.3f, 0.3f));
    }
}

// ============================================================================
// Peer Offload (NAT Punchthrough)
// ============================================================================

void TerrainNode::RegisterGuidWithAuthServer()
{
    auto* network = GetSubsystem<Network>();
    const String& guid = network->GetGUID();

    if (guid.Empty())
    {
        URHO3D_LOGWARNING("No NAT GUID available — NAT client may not be started");
        return;
    }

    auto* serverConn = network->GetServerConnection();
    if (!serverConn)
        return;

    VectorBuffer msg;
    msg.WriteString(guid);
    serverConn->SendMessage(MSG_REGISTER_GUID, true, true, msg);
    URHO3D_LOGINFOF("Registered GUID '%s' with AuthServer", guid.CString());
}

void TerrainNode::HandlePeerConnected(StringHash eventType, VariantMap& eventData)
{
    using namespace PeerConnected;
    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    bool weAreSubServer = eventData[P_ISSUBSERVER].GetBool();

    isPeered_ = true;
    isSubServer_ = weAreSubServer;
    peerConnection_ = connection;

    if (weAreSubServer)
    {
        // We are the subserver — replicate our scene to the subclient
        URHO3D_LOGINFO("Peered as SUBSERVER — starting replication to subclient");
        connection->SetScene(scene_);
    }
    else
    {
        // We are the subclient — disconnect from AuthServer, receive replication from subserver
        URHO3D_LOGINFO("Peered as SUBCLIENT — migrating from AuthServer to subserver");

        auto* network = GetSubsystem<Network>();
        network->Disconnect(100);  // disconnect from AuthServer
        authConnected_ = false;
        UpdateAuthButtonState();
    }
}

void TerrainNode::HandleNATPunchFailed(StringHash eventType, VariantMap& eventData)
{
    auto* network = GetSubsystem<Network>();

    // Only relevant if we have a pending peer introduction
    if (!network->IsPeerPending())
        return;

    URHO3D_LOGERROR("NAT punchthrough failed for peer at patch (" +
        String(peerPatchX_) + "," + String(peerPatchZ_) + ")");

    // Clear pending state — we stay connected to AuthServer as normal
    network->ClearPendingPeer();
    peerGuid_.Clear();
    peerToken_.Clear();
}

void TerrainNode::HandlePeerDisconnected(StringHash eventType, VariantMap& eventData)
{
    if (!isPeered_)
        return;

    URHO3D_LOGINFO("Peer connection lost");

    if (!isSubServer_)
    {
        // We're a subclient — lost our subserver. Reconnect to AuthServer.
        URHO3D_LOGINFO("Subclient: subserver lost — reconnecting to AuthServer");
        isPeered_ = false;
        peerConnection_.Reset();
        peerGuid_.Clear();

        // Reconnect to AuthServer
        auto* network = GetSubsystem<Network>();
        network->Connect(authServerAddress_, authServerPort_, scene_);
    }
    else
    {
        // We're a subserver — lost our subclient. Just clean up.
        URHO3D_LOGINFO("Subserver: subclient disconnected");
        isPeered_ = false;
        isSubServer_ = false;
        peerConnection_.Reset();
        peerGuid_.Clear();
    }
}

void TerrainNode::RelayToAuth(int innerMsgID, const VectorBuffer& innerPayload)
{
    if (!isPeered_ || isSubServer_ || !peerConnection_)
    {
        URHO3D_LOGWARNING("RelayToAuth: not a peered subclient — cannot relay");
        return;
    }

    VectorBuffer relay;
    relay.WriteString(loggedInUsername_);
    relay.WriteI32(innerMsgID);
    relay.WriteU32(innerPayload.GetSize());
    relay.Write(innerPayload.GetData(), innerPayload.GetSize());
    peerConnection_->SendMessage(MSG_RELAY_TO_AUTH, true, true, relay);
}

// ---------------------------------------------------------------------------
// Server-authoritative edits
// ---------------------------------------------------------------------------

void TerrainNode::SendEditMessage(int msgID, const VectorBuffer& payload)
{
    auto* network = GetSubsystem<Network>();
    if (!network)
        return;

    // Direct connection to AuthServer
    auto* serverConn = network->GetServerConnection();
    if (serverConn && authConnected_)
    {
        serverConn->SendMessage(msgID, true, true, payload);
        return;
    }

    // Relay via peer subserver
    if (isPeered_ && !isSubServer_ && peerConnection_)
    {
        RelayToAuth(msgID, payload);
        return;
    }
}

void TerrainNode::SendTerrainEdit(const Vector3& worldPos, float timeStep)
{
    if (replayingBroadcast_)
        return;
    if (!authConnected_ && !isPeered_)
        return;
    if (!terrain_ || !editableHeightMap_)
        return;

    // Rate limit: only send every EDIT_SEND_INTERVAL seconds
    editSendAccumulator_ += timeStep;
    if (editSendAccumulator_ < EDIT_SEND_INTERVAL)
        return;
    editSendAccumulator_ = 0.0f;

    // Snapshot the brush region for potential rollback
    unsigned editID = nextEditID_++;
    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    IntVector2 center = terrain_->WorldToHeightMap(worldPos);
    int radius = (int)brushRadius_;
    int x0 = Max(center.x_ - radius, 0);
    int y0 = Max(center.y_ - radius, 0);
    int x1 = Min(center.x_ + radius, hmW - 1);
    int y1 = Min(center.y_ + radius, hmH - 1);
    int rw = x1 - x0 + 1;
    int rh = y1 - y0 + 1;

    if (rw > 0 && rh > 0)
    {
        TerrainEditSnapshot snap;
        snap.regionMin = IntVector2(x0, y0);
        snap.regionSize = IntVector2(rw, rh);
        int comps = editableHeightMap_->GetComponents();
        snap.heightData.Resize(rw * rh * comps);
        unsigned char* src = editableHeightMap_->GetData();
        for (int row = 0; row < rh; ++row)
        {
            int srcIdx = ((y0 + row) * hmW + x0) * comps;
            int dstIdx = row * rw * comps;
            memcpy(&snap.heightData[dstIdx], &src[srcIdx], rw * comps);
        }
        terrainEditSnapshots_[editID] = snap;

        // Limit snapshot history to prevent unbounded growth
        if (terrainEditSnapshots_.Size() > 100)
        {
            unsigned oldest = nextEditID_ - 100;
            for (auto it = terrainEditSnapshots_.Begin(); it != terrainEditSnapshots_.End();)
            {
                if (it->first_ < oldest)
                    it = terrainEditSnapshots_.Erase(it);
                else
                    ++it;
            }
        }
    }

    VectorBuffer payload;
    payload.WriteU32(editID);
    payload.WriteVector3(worldPos);
    payload.WriteI32(brushMode_);
    payload.WriteI32(brushShape_);
    payload.WriteFloat(brushRadius_);
    payload.WriteFloat(brushStrength_);
    payload.WriteFloat(smoothStrength_);
    payload.WriteFloat(brushRotation_);
    payload.WriteFloat(timeStep);
    payload.WriteFloat(lockedFlattenHeight_);
    SendEditMessage(MSG_EDIT_TERRAIN, payload);
}

void TerrainNode::SendObjectCreate(Node* node, const Vector3& position, const Vector3& surfaceNormal)
{
    if (!authConnected_ && !isPeered_)
        return;
    if (!node)
        return;

    unsigned editID = nextEditID_++;
    String xmlData = SerializeNode(node);

    // Snapshot for rollback: if rejected, delete the node
    ObjectEditSnapshot snap;
    snap.subtype = 0;  // create
    snap.nodeID = node->GetID();
    snap.xmlData = xmlData;
    snap.position = position;
    objectEditSnapshots_[editID] = snap;

    VectorBuffer payload;
    payload.WriteU32(editID);
    payload.WriteU8(0);  // subtype: create
    payload.WriteString(xmlData);
    payload.WriteVector3(position);
    payload.WriteVector3(surfaceNormal);
    SendEditMessage(MSG_EDIT_OBJECT, payload);
}

void TerrainNode::SendObjectDelete(unsigned nodeID)
{
    if (!authConnected_ && !isPeered_)
        return;

    // Snapshot for rollback: if rejected, recreate the node
    Node* node = scene_->GetNode(nodeID);
    unsigned editID = nextEditID_++;

    ObjectEditSnapshot snap;
    snap.subtype = 1;  // delete
    snap.nodeID = nodeID;
    if (node)
    {
        snap.xmlData = SerializeNode(node);
        snap.position = node->GetPosition();
        snap.rotation = node->GetRotation();
        snap.scale = node->GetScale();
    }
    objectEditSnapshots_[editID] = snap;

    VectorBuffer payload;
    payload.WriteU32(editID);
    payload.WriteU8(1);  // subtype: delete
    payload.WriteU32(nodeID);
    SendEditMessage(MSG_EDIT_OBJECT, payload);
}

void TerrainNode::SendObjectTransform(unsigned nodeID, const Vector3& pos, const Quaternion& rot, const Vector3& scale)
{
    if (!authConnected_ && !isPeered_)
        return;

    Node* node = scene_->GetNode(nodeID);
    unsigned editID = nextEditID_++;

    ObjectEditSnapshot snap;
    snap.subtype = 2;  // transform
    snap.nodeID = nodeID;
    if (node)
    {
        snap.position = node->GetPosition();
        snap.rotation = node->GetRotation();
        snap.scale = node->GetScale();
    }
    objectEditSnapshots_[editID] = snap;

    VectorBuffer payload;
    payload.WriteU32(editID);
    payload.WriteU8(2);  // subtype: transform
    payload.WriteU32(nodeID);
    payload.WriteVector3(pos);
    payload.WriteQuaternion(rot);
    payload.WriteVector3(scale);
    SendEditMessage(MSG_EDIT_OBJECT, payload);
}

void TerrainNode::HandleEditReject(MemoryBuffer& msg)
{
    unsigned editID = msg.ReadU32();
    String reason = msg.ReadString();

    URHO3D_LOGWARNINGF("Edit %u rejected: %s", editID, reason.CString());

    // Rollback terrain edit
    auto terrainIt = terrainEditSnapshots_.Find(editID);
    if (terrainIt != terrainEditSnapshots_.End())
    {
        const TerrainEditSnapshot& snap = terrainIt->second_;
        if (editableHeightMap_ && terrain_)
        {
            unsigned char* dst = editableHeightMap_->GetData();
            int hmW = editableHeightMap_->GetWidth();
            int comps = editableHeightMap_->GetComponents();
            int rw = snap.regionSize.x_;
            int rh = snap.regionSize.y_;
            for (int row = 0; row < rh; ++row)
            {
                int dstIdx = ((snap.regionMin.y_ + row) * hmW + snap.regionMin.x_) * comps;
                int srcIdx = row * rw * comps;
                memcpy(&dst[dstIdx], &snap.heightData[srcIdx], rw * comps);
            }
            terrain_->ApplyHeightMap();
        }
        terrainEditSnapshots_.Erase(terrainIt);
        return;
    }

    // Rollback object edit
    auto objIt = objectEditSnapshots_.Find(editID);
    if (objIt != objectEditSnapshots_.End())
    {
        const ObjectEditSnapshot& snap = objIt->second_;
        switch (snap.subtype)
        {
        case 0:  // create was rejected — remove the node
        {
            Node* node = scene_->GetNode(snap.nodeID);
            if (node)
                node->Remove();
            break;
        }
        case 1:  // delete was rejected — recreate the node
        {
            if (!snap.xmlData.Empty())
            {
                XMLFile xmlFile(context_);
                xmlFile.FromString(snap.xmlData);
                Node* restored = scene_->InstantiateXML(xmlFile.GetRoot(), snap.position, snap.rotation);
                if (restored)
                    URHO3D_LOGINFOF("Rollback: restored deleted node at (%.1f, %.1f, %.1f)",
                        snap.position.x_, snap.position.y_, snap.position.z_);
            }
            break;
        }
        case 2:  // transform was rejected — restore original transform
        {
            Node* node = scene_->GetNode(snap.nodeID);
            if (node)
            {
                node->SetPosition(snap.position);
                node->SetRotation(snap.rotation);
                node->SetScale(snap.scale);
            }
            break;
        }
        }
        objectEditSnapshots_.Erase(objIt);
        return;
    }
}

void TerrainNode::HandleEditBroadcast(MemoryBuffer& msg)
{
    String username = msg.ReadString();
    int editType = msg.ReadI32();  // MSG_EDIT_TERRAIN or MSG_EDIT_OBJECT

    if (editType == MSG_EDIT_TERRAIN)
    {
        unsigned editID = msg.ReadU32();
        Vector3 worldPos = msg.ReadVector3();
        int mode = msg.ReadI32();
        int shape = msg.ReadI32();
        float radius = msg.ReadFloat();
        float strength = msg.ReadFloat();
        float smooth = msg.ReadFloat();
        float rotation = msg.ReadFloat();
        float ts = msg.ReadFloat();
        float flattenH = msg.ReadFloat();

        // Temporarily swap brush settings, apply, restore
        int savedMode = brushMode_;
        int savedShape = brushShape_;
        float savedRadius = brushRadius_;
        float savedStrength = brushStrength_;
        float savedSmooth = smoothStrength_;
        float savedRotation = brushRotation_;
        float savedFlatten = lockedFlattenHeight_;

        brushMode_ = mode;
        brushShape_ = shape;
        brushRadius_ = radius;
        brushStrength_ = strength;
        smoothStrength_ = smooth;
        brushRotation_ = rotation;
        lockedFlattenHeight_ = flattenH;

        // Apply without re-sending to server
        replayingBroadcast_ = true;
        ApplyBrush(worldPos, ts);
        replayingBroadcast_ = false;

        brushMode_ = savedMode;
        brushShape_ = savedShape;
        brushRadius_ = savedRadius;
        brushStrength_ = savedStrength;
        smoothStrength_ = savedSmooth;
        brushRotation_ = savedRotation;
        lockedFlattenHeight_ = savedFlatten;

        (void)editID;
    }
    else if (editType == MSG_EDIT_OBJECT)
    {
        unsigned editID = msg.ReadU32();
        unsigned char subtype = msg.ReadU8();

        switch (subtype)
        {
        case 0:  // create
        {
            String xmlData = msg.ReadString();
            Vector3 position = msg.ReadVector3();
            Vector3 surfaceNormal = msg.ReadVector3();

            XMLFile xmlFile(context_);
            if (xmlFile.FromString(xmlData))
            {
                Quaternion surfaceRot;
                surfaceRot.FromRotationTo(Vector3::UP, surfaceNormal);
                Node* instance = scene_->InstantiateXML(xmlFile.GetRoot(), position, surfaceRot);
                if (instance)
                    URHO3D_LOGINFOF("Broadcast: %s created object at (%.1f, %.1f, %.1f)",
                        username.CString(), position.x_, position.y_, position.z_);
            }
            break;
        }
        case 1:  // delete
        {
            unsigned nodeID = msg.ReadU32();
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                node->Remove();
                URHO3D_LOGINFOF("Broadcast: %s deleted node %u", username.CString(), nodeID);
            }
            break;
        }
        case 2:  // transform
        {
            unsigned nodeID = msg.ReadU32();
            Vector3 pos = msg.ReadVector3();
            Quaternion rot = msg.ReadQuaternion();
            Vector3 scale = msg.ReadVector3();
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                node->SetPosition(pos);
                node->SetRotation(rot);
                node->SetScale(scale);
            }
            break;
        }
        }
        (void)editID;
    }
}

void TerrainNode::HandleResourcePatch(MemoryBuffer& msg)
{
    String resourceID = msg.ReadString();
    int patchX = msg.ReadI32();
    int patchZ = msg.ReadI32();
    int pixelX = msg.ReadI32();
    int pixelZ = msg.ReadI32();
    int pixelW = msg.ReadI32();
    int pixelH = msg.ReadI32();
    int components = msg.ReadI32();
    unsigned dataSize = msg.ReadU32();

    if (resourceID == "water_heightmap" && waterMap_)
    {
        // Blit raw pixels into waterMap_ at (pixelX, pixelZ)
        int mapComponents = waterMap_->GetComponents();
        unsigned char* dest = waterMap_->GetData();
        int destStride = waterMap_->GetWidth() * mapComponents;
        int srcStride = pixelW * components;

        for (int row = 0; row < pixelH; ++row)
        {
            int destRow = pixelZ + row;
            if (destRow < 0 || destRow >= waterMap_->GetHeight())
            {
                msg.Seek(msg.GetPosition() + srcStride);  // skip row
                continue;
            }
            int destCol = pixelX;
            if (destCol < 0 || destCol + pixelW > waterMap_->GetWidth())
            {
                msg.Seek(msg.GetPosition() + srcStride);  // skip row
                continue;
            }
            const unsigned char* srcRow = reinterpret_cast<const unsigned char*>(msg.GetData()) + msg.GetPosition();
            memcpy(dest + destRow * destStride + destCol * mapComponents, srcRow, pixelW * Min(components, mapComponents));
            msg.Seek(msg.GetPosition() + srcStride);
        }

        // Re-upload water map to GPU
        if (waterMapTex_)
            waterMapTex_->SetData(waterMap_);

        URHO3D_LOGINFOF("Resource patch '%s' (%d,%d) → pixels (%d,%d) %dx%d",
            resourceID.CString(), patchX, patchZ, pixelX, pixelZ, pixelW, pixelH);
    }
    else
    {
        // Skip unknown resource data
        msg.Seek(msg.GetPosition() + dataSize);
        URHO3D_LOGWARNINGF("Unknown resource patch: %s", resourceID.CString());
    }
}

void TerrainNode::SendPatchPosition(int patchX, int patchZ)
{
    auto* network = GetSubsystem<Network>();
    Connection* conn = network->GetServerConnection();
    if (!conn)
        return;

    VectorBuffer msg;
    msg.WriteI32(patchX);
    msg.WriteI32(patchZ);
    conn->SendMessage(MSG_PATCH_POSITION, true, true, msg);
    lastReportedPatchX_ = patchX;
    lastReportedPatchZ_ = patchZ;
}

// ============================================================================
// Player Avatar & Camera Modes
// ============================================================================

Node* TerrainNode::CreatePlayerAvatar()
{
    if (!scene_)
        return nullptr;

    auto* cache = GetSubsystem<ResourceCache>();

    // Spawn at center of owned patch, ensure above water
    const float patchWorldSize = 128.0f;
    const float waterLevel = 5.0f;
    float spawnX = (ownedPatchX_ + 0.5f) * patchWorldSize;
    float spawnZ = (ownedPatchZ_ + 0.5f) * patchWorldSize;
    Vector3 spawnPos(spawnX, 20.0f, spawnZ);
    if (terrain_)
    {
        float terrainH = terrain_->GetHeight(spawnPos);

        // If initial position is underwater, scan nearby for dry land
        if (terrainH < waterLevel + 2.0f)
        {
            float bestH = terrainH;
            Vector3 bestPos = spawnPos;
            // Search in expanding rings around spawn center
            for (float r = 16.0f; r <= 128.0f; r += 16.0f)
            {
                for (int angle = 0; angle < 8; ++angle)
                {
                    float a = angle * (M_PI / 4.0f);
                    Vector3 probe(spawnX + r * cosf(a), 0.0f, spawnZ + r * sinf(a));
                    float h = terrain_->GetHeight(probe);
                    if (h > bestH)
                    {
                        bestH = h;
                        bestPos = probe;
                    }
                }
                if (bestH > waterLevel + 5.0f)
                    break;  // Found good high ground
            }
            spawnPos.x_ = bestPos.x_;
            spawnPos.z_ = bestPos.z_;
            terrainH = bestH;
        }

        spawnPos.y_ = Max(terrainH, waterLevel) + 2.0f;
    }

    // Root character node (replicated for network)
    Node* charNode = scene_->CreateChild("Player");
    charNode->SetPosition(spawnPos);

    // Child model node — separate from physics root for future model swap
    Node* modelNode = charNode->CreateChild("PlayerModel");
    modelNode->SetPosition(Vector3(0.0f, 0.9f, 0.0f));  // capsule center offset
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
    body->SetAngularFactor(Vector3::ZERO);  // prevent tumbling
    body->SetCollisionLayer(1);
    body->SetCollisionEventMode(COLLISION_ALWAYS);

    auto* shape = charNode->CreateComponent<CollisionShape>();
    shape->SetCapsule(0.7f, 1.8f, Vector3(0.0f, 0.9f, 0.0f));

    // Character controller component
    charNode->CreateComponent<PlayerCharacter>();

    return charNode;
}

void TerrainNode::UpdateCharacterCamera()
{
    Node* charNode = characterNode_;
    if (!charNode || !cameraNode_)
        return;

    // Hide own model in first person, show in third person/editor
    Node* modelNode = charNode->GetChild("PlayerModel");
    if (modelNode)
        modelNode->SetEnabled(cameraMode_ != CAM_FIRSTPERSON);

    if (cameraMode_ == CAM_FIRSTPERSON)
    {
        // Eye-level inside the character
        Vector3 eyePos = charNode->GetPosition() + Vector3(0.0f, 1.6f, 0.0f);
        cameraNode_->SetPosition(eyePos);
        cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
        // Rotate character to match yaw
        charNode->SetRotation(Quaternion(0.0f, yaw_, 0.0f));
    }
    else if (cameraMode_ == CAM_CHASE)
    {
        const float CAMERA_DISTANCE = 5.0f;
        const float CAMERA_HEIGHT = 2.0f;

        // Rotate character to match yaw
        charNode->SetRotation(Quaternion(0.0f, yaw_, 0.0f));

        // Camera position: behind and above
        Quaternion camRot(pitch_, yaw_, 0.0f);
        Vector3 targetPos = charNode->GetPosition() + Vector3(0.0f, CAMERA_HEIGHT, 0.0f);
        Vector3 desiredCamPos = targetPos + camRot * Vector3::BACK * CAMERA_DISTANCE;

        // Raycast to avoid clipping through walls/terrain
        auto* physicsWorld = scene_ ? scene_->GetComponent<PhysicsWorld>() : nullptr;
        if (physicsWorld)
        {
            Vector3 dir = desiredCamPos - targetPos;
            float dist = dir.Length();
            if (dist > 0.001f)
            {
                PhysicsRaycastResult result;
                physicsWorld->RaycastSingle(result, Ray(targetPos, dir / dist), dist);
                if (result.body_ && result.body_ != charNode->GetComponent<RigidBody>())
                    desiredCamPos = targetPos + (dir / dist) * Max(result.distance_ - 0.1f, 0.5f);
            }
        }

        cameraNode_->SetPosition(desiredCamPos);
        cameraNode_->SetRotation(camRot);
    }
    // CAM_GOD: camera is free-flying, handled by existing MoveCamera code
}

void TerrainNode::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    auto* network = GetSubsystem<Network>();
    Connection* serverConnection = network->GetServerConnection();

    // Client: collect controls and send to server
    if (serverConnection)
    {
        auto* ui = GetSubsystem<UI>();
        auto* input = GetSubsystem<Input>();
        Controls controls;

        controls.yaw_ = yaw_;
        controls.pitch_ = pitch_;

        if (!ui->GetFocusElement() && cameraMode_ != CAM_GOD)
        {
            controls.Set(CTRL_FORWARD, input->GetKeyDown(KEY_W));
            controls.Set(CTRL_BACK, input->GetKeyDown(KEY_S));
            controls.Set(CTRL_LEFT, input->GetKeyDown(KEY_A));
            controls.Set(CTRL_RIGHT, input->GetKeyDown(KEY_D));
            controls.Set(CTRL_JUMP, input->GetKeyDown(KEY_SPACE));
        }

        serverConnection->SetControls(controls);
        serverConnection->SetPosition(cameraNode_ ? cameraNode_->GetPosition() : Vector3::ZERO);
    }
    // Server/subserver: apply controls to each client's avatar
    else if (network->IsServerRunning())
    {
        const Vector<SharedPtr<Connection>>& connections = network->GetClientConnections();

        for (Connection* connection : connections)
        {
            Node* avatarNode = serverObjects_[connection];
            if (!avatarNode)
                continue;

            auto* character = avatarNode->GetComponent<PlayerCharacter>();
            if (character)
            {
                const Controls& controls = connection->GetControls();
                character->controls_ = controls;
                // Rotate character to face the direction the client is looking
                avatarNode->SetRotation(Quaternion(0.0f, controls.yaw_, 0.0f));
            }
        }
    }

    // Local standalone: apply controls directly to own avatar
    if (!serverConnection && !network->IsServerRunning())
    {
        Node* charNode = characterNode_;
        if (charNode && cameraMode_ != CAM_GOD)
        {
            auto* input = GetSubsystem<Input>();
            auto* ui = GetSubsystem<UI>();
            auto* character = charNode->GetComponent<PlayerCharacter>();
            if (character && !ui->GetFocusElement())
            {
                character->controls_.Set(CTRL_FORWARD, input->GetKeyDown(KEY_W));
                character->controls_.Set(CTRL_BACK, input->GetKeyDown(KEY_S));
                character->controls_.Set(CTRL_LEFT, input->GetKeyDown(KEY_A));
                character->controls_.Set(CTRL_RIGHT, input->GetKeyDown(KEY_D));
                character->controls_.Set(CTRL_JUMP, input->GetKeyDown(KEY_SPACE));
                character->controls_.yaw_ = yaw_;
                character->controls_.pitch_ = pitch_;
                // Rotate character to face yaw
                charNode->SetRotation(Quaternion(0.0f, yaw_, 0.0f));
            }
        }
    }
}

void TerrainNode::HandleClientConnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientConnected;

    auto* newConnection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    newConnection->SetScene(scene_);

    // Create avatar for the new client
    Node* newAvatar = CreatePlayerAvatar();
    serverObjects_[newConnection] = newAvatar;

    // Tell the client which node they control
    VariantMap remoteEventData;
    remoteEventData[P_ID] = newAvatar->GetID();
    newConnection->SendRemoteEvent(E_CLIENTOBJECTID, true, remoteEventData);

    URHO3D_LOGINFOF("Client connected — created avatar node %u", newAvatar->GetID());
}

void TerrainNode::HandleClientDisconnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientDisconnected;

    auto* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    Node* avatar = serverObjects_[connection];
    if (avatar)
    {
        URHO3D_LOGINFOF("Client disconnected — removing avatar node %u", avatar->GetID());
        avatar->Remove();
    }
    serverObjects_.Erase(connection);
}

void TerrainNode::HandleClientObjectID(StringHash eventType, VariantMap& eventData)
{
    clientObjectID_ = eventData[P_ID].GetU32();
    characterNode_ = scene_ ? scene_->GetNode(clientObjectID_) : nullptr;
    URHO3D_LOGINFOF("Received avatar node ID: %u (node %s)",
        clientObjectID_, characterNode_ ? "found" : "NOT FOUND — will retry on next frame");

    // Switch to chase camera so WASD controls flow to the avatar
    if (characterNode_)
    {
        cameraMode_ = CAM_CHASE;
        URHO3D_LOGINFO("Switched to chase camera — WASD drives avatar");
    }
    // Node may not exist yet if replication hasn't delivered it — HandleUpdate will retry
}

// ============================================================================
// Grass — cross-billboard clumps written as UMDL .mdl, loaded via cache
// ============================================================================

static bool WriteGrassClumpMDL(const String& path, Context* context)
{
    // 3 cross quads at 0°, 60°, 120° — classic grass clump
    const float halfW = 0.5f;
    const float height = 1.0f;
    const int NUM_QUADS = 3;

    struct GrassVert { Vector3 pos; Vector3 norm; Vector2 uv; };
    Vector<GrassVert> verts;
    Vector<unsigned short> indices;

    for (int q = 0; q < NUM_QUADS; ++q)
    {
        float angle = q * 60.0f * 3.14159265f / 180.0f;
        float cx = cosf(angle) * halfW;
        float cz = sinf(angle) * halfW;
        unsigned short base = (unsigned short)verts.Size();

        // 4 vertices per quad: BL, BR, TR, TL
        verts.Push({ Vector3(-cx, 0.0f, -cz), Vector3::UP, Vector2(0.0f, 1.0f) });
        verts.Push({ Vector3( cx, 0.0f,  cz), Vector3::UP, Vector2(1.0f, 1.0f) });
        verts.Push({ Vector3( cx, height, cz), Vector3::UP, Vector2(1.0f, 0.0f) });
        verts.Push({ Vector3(-cx, height,-cz), Vector3::UP, Vector2(0.0f, 0.0f) });

        // 2 triangles
        indices.Push(base + 0); indices.Push(base + 1); indices.Push(base + 2);
        indices.Push(base + 0); indices.Push(base + 2); indices.Push(base + 3);
    }

    // Compute bounding box
    BoundingBox bbox;
    for (unsigned i = 0; i < verts.Size(); ++i)
        bbox.Merge(verts[i].pos);

    // Write UMDL format — same as MeshGenerator
    File file(context, path, FILE_WRITE);
    if (!file.IsOpen())
        return false;

    file.WriteFileID("UMDL");

    // 1 vertex buffer
    file.WriteU32(1);
    file.WriteU32(verts.Size());
    file.WriteU32(0x0B);  // Position(1) | Normal(2) | TexCoord1(8)
    file.WriteU32(0);     // morph range start
    file.WriteU32(0);     // morph range count
    for (unsigned i = 0; i < verts.Size(); ++i)
    {
        file.WriteVector3(verts[i].pos);
        file.WriteVector3(verts[i].norm);
        file.WriteVector2(verts[i].uv);
    }

    // 1 index buffer
    file.WriteU32(1);
    file.WriteU32(indices.Size());
    file.WriteU32(2);  // 16-bit indices
    for (unsigned i = 0; i < indices.Size(); ++i)
        file.WriteU16(indices[i]);

    // 1 geometry, 1 LOD
    file.WriteU32(1);
    file.WriteU32(0);              // bone mappings: none
    file.WriteU32(1);              // LOD levels
    file.WriteFloat(0.0f);         // LOD distance
    file.WriteU32(0);              // primitive type: TRIANGLE_LIST
    file.WriteU32(0);              // vertex buffer index
    file.WriteU32(0);              // index buffer index
    file.WriteU32(0);              // index start
    file.WriteU32(indices.Size()); // index count

    // No morphs, no skeleton
    file.WriteU32(0);
    file.WriteU32(0);

    // Bounding box + geometry center
    file.WriteVector3(bbox.min_);
    file.WriteVector3(bbox.max_);
    file.WriteVector3(bbox.Center());

    return true;
}

void TerrainNode::CreateGrass()
{
    if (!terrain_)
        return;

    auto* cache = GetSubsystem<ResourceCache>();

    // ── 1. Write grass clump model to disk as UMDL ──
    String mdlPath = GetSubsystem<FileSystem>()->GetProgramDir() + "Data/Models/GrassClump.mdl";
    if (!WriteGrassClumpMDL(mdlPath, context_))
    {
        URHO3D_LOGERROR("Failed to write GrassClump.mdl");
        return;
    }

    grassModel_ = cache->GetResource<Model>("Models/GrassClump.mdl");
    if (!grassModel_)
    {
        URHO3D_LOGERROR("Failed to load GrassClump.mdl");
        return;
    }

    // ── 2. Generate grass blade texture (64x128 RGBA) ──
    const int texW = 64;
    const int texH = 128;
    SharedPtr<Image> grassImg(new Image(context_));
    grassImg->SetSize(texW, texH, 4);

    // Clear to transparent
    for (int y = 0; y < texH; ++y)
        for (int x = 0; x < texW; ++x)
            grassImg->SetPixel(x, y, Color(0.0f, 0.0f, 0.0f, 0.0f));

    // Draw grass blade silhouettes — side view, 5 blades with taper
    struct Blade { float cx; float w; float r; float g; float b; };
    Blade blades[] = {
        { 0.12f, 0.08f, 0.30f, 0.65f, 0.12f },
        { 0.30f, 0.10f, 0.35f, 0.70f, 0.15f },
        { 0.50f, 0.12f, 0.28f, 0.60f, 0.10f },
        { 0.68f, 0.09f, 0.38f, 0.68f, 0.18f },
        { 0.88f, 0.10f, 0.32f, 0.72f, 0.14f },
    };

    for (int b = 0; b < 5; ++b)
    {
        float centerPx = blades[b].cx * texW;
        float widthPx = blades[b].w * texW;

        for (int y = 0; y < texH; ++y)
        {
            float t = (float)y / (float)texH;  // 0=top, 1=bottom
            float taper = 0.2f + 0.8f * t;     // narrow at top, wide at base
            float curHalf = widthPx * taper * 0.5f;
            float sway = sinf(t * 6.28f + b * 1.7f) * 1.5f;
            float center = centerPx + sway;

            for (int x = (int)(center - curHalf); x <= (int)(center + curHalf); ++x)
            {
                if (x < 0 || x >= texW) continue;
                float shade = 0.6f + 0.4f * (1.0f - t);
                grassImg->SetPixel(x, y, Color(
                    blades[b].r * shade, blades[b].g * shade, blades[b].b * shade, 1.0f));
            }
        }
    }

    SharedPtr<Texture2D> grassTex(new Texture2D(context_));
    grassTex->SetData(grassImg);
    grassTex->SetFilterMode(FILTER_BILINEAR);

    // ── 3. Create material — GrassDiffUnlitAlpha with wind ──
    grassMat_ = new Material(context_);
    grassMat_->SetTechnique(0, cache->GetResource<Technique>("Techniques/GrassDiffUnlitAlpha.xml"));
    grassMat_->SetTexture(TU_DIFFUSE, grassTex);
    grassMat_->SetShaderParameter("MatDiffColor", Color(0.5f, 0.85f, 0.35f, 1.0f));
    grassMat_->SetShaderParameter("WindHeightFactor", 0.08f);
    grassMat_->SetShaderParameter("WindHeightPivot", 0.0f);
    grassMat_->SetShaderParameter("WindPeriod", 1.5f);
    grassMat_->SetShaderParameter("WindWorldSpacing", Vector2(0.15f, 0.15f));
    grassMat_->SetCullMode(CULL_NONE);

    // ── 4. Spawn clump nodes ──
    grassRoot_ = scene_->CreateChild("GrassRoot", LOCAL);

    for (int i = 0; i < MAX_GRASS_CLUMPS; ++i)
    {
        Node* clump = grassRoot_->CreateChild("G");
        clump->SetEnabled(false);
        auto* sm = clump->CreateComponent<StaticModel>();
        sm->SetModel(grassModel_);
        sm->SetMaterial(grassMat_);
        sm->SetCastShadows(false);
        sm->SetOccludee(false);
    }

    lastGrassCenter_ = Vector3(M_INFINITY, M_INFINITY, M_INFINITY);
    URHO3D_LOGINFOF("Created grass: %d clumps with UMDL model", MAX_GRASS_CLUMPS);
}

void TerrainNode::UpdateGrassPositions()
{
    if (!grassRoot_ || !terrain_ || !cameraNode_)
        return;

    Vector3 camPos = cameraNode_->GetWorldPosition();
    Vector3 center(camPos.x_, 0.0f, camPos.z_);

    // Hysteresis: only reposition when camera moves >3m
    Vector3 diff = center - lastGrassCenter_;
    diff.y_ = 0.0f;
    if (diff.LengthSquared() < 9.0f)
        return;
    lastGrassCenter_ = center;

    const float RADIUS = 35.0f;
    const float CELL_SIZE = 2.0f;
    const float waterLevel = 5.5f;
    const Vector<SharedPtr<Node>>& clumps = grassRoot_->GetChildren();
    unsigned idx = 0;
    int halfCells = (int)(RADIUS / CELL_SIZE);

    for (int cz = -halfCells; cz <= halfCells && idx < (unsigned)MAX_GRASS_CLUMPS; ++cz)
    {
        for (int cx = -halfCells; cx <= halfCells && idx < (unsigned)MAX_GRASS_CLUMPS; ++cx)
        {
            float wx = center.x_ + cx * CELL_SIZE;
            float wz = center.z_ + cz * CELL_SIZE;

            if ((float)(cx * cx + cz * cz) * CELL_SIZE * CELL_SIZE > RADIUS * RADIUS)
                continue;

            // Deterministic offset within cell
            unsigned h = (unsigned)(wx * 73856093.0f) ^ (unsigned)(wz * 19349663.0f);
            float px = wx + ((h & 0xFF) / 255.0f - 0.5f) * CELL_SIZE * 0.8f;
            float pz = wz + (((h >> 8) & 0xFF) / 255.0f - 0.5f) * CELL_SIZE * 0.8f;
            float gy = terrain_->GetHeight(Vector3(px, 0, pz));

            if (gy < waterLevel + 0.3f) continue;
            if (terrain_->GetNormal(Vector3(px, 0, pz)).y_ < 0.8f) continue;

            Node* c = clumps[idx];
            c->SetEnabled(true);
            c->SetPosition(Vector3(px, gy, pz));
            c->SetRotation(Quaternion(((h >> 16) & 0xFF) / 255.0f * 360.0f, Vector3::UP));
            c->SetScale(0.7f + ((h >> 24) & 0xFF) / 255.0f * 0.6f);
            ++idx;
        }
    }

    for (unsigned i = idx; i < clumps.Size(); ++i)
        clumps[i]->SetEnabled(false);
}

// --- Font / Theme ---

void TerrainNode::LoadThemePrefs()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* fs = GetSubsystem<FileSystem>();
    String path = fs->GetProgramDir() + "Data/UI/theme.json";
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
                val = val.Trimmed();
                if (!val.Empty())
                    currentFontName_ = val;
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

void TerrainNode::SaveThemePrefs()
{
    auto* fs = GetSubsystem<FileSystem>();
    String path = fs->GetProgramDir() + "Data/UI/theme.json";

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

static void UpdateFontsRecursive(UIElement* element, Font* font, int baseSize)
{
    auto* text = dynamic_cast<Text*>(element);
    if (text && text->GetFont())
    {
        int oldSize = text->GetFontSize();
        int offset = oldSize - 11;
        text->SetFont(font, baseSize + offset);
    }
    for (unsigned i = 0; i < element->GetNumChildren(); ++i)
        UpdateFontsRecursive(element->GetChild(i), font, baseSize);
}

void TerrainNode::ApplyFont(const String& fontName, int fontSize)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* newFont = cache->GetResource<Font>("Fonts/" + fontName + ".ttf");
    if (!newFont)
        return;
    font_ = newFont;
    currentFontName_ = fontName;
    currentFontSize_ = fontSize;

    auto* ui = GetSubsystem<UI>();
    UpdateFontsRecursive(ui->GetRoot(), font_, currentFontSize_);
}

void TerrainNode::HandleSettingsMenu(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace ItemSelected;
    auto* list = static_cast<DropDownList*>(eventData[P_ELEMENT].GetPtr());
    unsigned sel = eventData[P_SELECTION].GetU32();
    if (sel == M_MAX_UNSIGNED || !list)
        return;

    auto* item = list->GetItem(sel);
    if (!item)
        return;

    String text = static_cast<Text*>(item)->GetText();

    // Font items are prefixed "Font: "
    if (text.StartsWith("Font: "))
    {
        String fontName = text.Substring(6);
        ApplyFont(fontName, currentFontSize_);
        SaveThemePrefs();
    }
    // Size items are prefixed "Size: "
    else if (text.StartsWith("Size: "))
    {
        int sz = atoi(text.Substring(6).CString());
        if (sz >= 8 && sz <= 24)
        {
            ApplyFont(currentFontName_, sz);
            SaveThemePrefs();
        }
    }

    list->SetSelection(M_MAX_UNSIGNED);
}
