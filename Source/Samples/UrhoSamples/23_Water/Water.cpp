// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Graphics/BillboardSet.h>
#include <Urho3D/Graphics/Camera.h>
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
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/Graphics/TerrainPatch.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/GraphicsAPI/RenderSurface.h>
#include <Urho3D/GraphicsAPI/Texture2D.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Physics/CollisionShape.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>
#include <Urho3D/Network/Network.h>

#include "Water.h"

#include <Urho3D/DebugNew.h>
#include <Urho3D/Graphics/ProfilerUI.h>

#include <ctime>
#include <cmath>

URHO3D_DEFINE_APPLICATION_MAIN(Water)

static const float MELBOURNE_LAT = -37.8136f;
static const float MELBOURNE_LON = 144.96f;
static const float SOLAR_NOON = 12.0f + (11.0f * 15.0f - MELBOURNE_LON) / 15.0f;
static const float CELESTIAL_TIME_SCALE = 1.0f;
static const float DEG_TO_RAD = M_PI / 180.0f;
static const float RAD_TO_DEG = 180.0f / M_PI;

Water::Water(Context* context) :
    Sample(context)
{
}

void Water::Stop()
{
    if (timeRequest_)
    {
        HttpRequestState state = timeRequest_->GetState();
        if (state == HTTP_CLOSED || state == HTTP_ERROR)
            timeRequest_.Reset();
        else
            timeRequest_.Detach();
    }

    skyboxMat_.Reset();
    sunMat_.Reset();
    moonMat_.Reset();
    profilerUI_.Reset();
    editableHeightMap_.Reset();
}

void Water::Start()
{
    Sample::Start();

    auto* cache = GetSubsystem<ResourceCache>();
    auto* uiStyle = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(uiStyle);

    CreateScene();
    CreateInstructions();
    SetupViewport();
    SubscribeToEvents();

    Sample::InitMouseMode(MM_RELATIVE);

    CreateMenuBar();

    auto* graphics = GetSubsystem<Graphics>();
    auto* ui = GetSubsystem<UI>();
    profilerUI_ = new ProfilerUI(context_);
    profilerUI_->Initialize(ui, graphics->GetVulkanProfiler(), graphics);
    profilerUI_->SetVisible(true);
}

// ============================================================================
// Scene
// ============================================================================

void Water::CreateScene()
{
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
    zone->SetFogColor(Color(1.0f, 1.0f, 1.0f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);
    zone->SetFogHeight(5.0f);
    zone->SetFogHeightScale(0.3f);

    zone_ = zone;
    origFogColor_ = zone->GetFogColor();
    origFogStart_ = zone->GetFogStart();
    origFogEnd_ = zone->GetFogEnd();

    // Directional light (sun)
    Node* lightNode = scene_->CreateChild("DirectionalLight");
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

    // Terrain
    Node* terrainNode = scene_->CreateChild("Terrain");
    terrainNode->SetPosition(Vector3(0.0f, 0.0f, 0.0f));
    auto* terrain = terrainNode->CreateComponent<Terrain>();
    terrain->SetPatchSize(64);
    terrain->SetSpacing(Vector3(2.0f, 0.5f, 2.0f));
    terrain->SetSmoothing(true);
    terrain->SetHeightMap(cache->GetResource<Image>("Textures/HeightMap.png"));
    terrain->SetMaterial(cache->GetResource<Material>("Materials/Terrain.xml"));
    terrain->SetOccluder(true);
    terrain_ = terrain;

    // 16-bit editable heightmap
    {
        Image* origHM = terrain->GetHeightMap();
        int w = origHM->GetWidth();
        int h = origHM->GetHeight();
        int origComps = origHM->GetComponents();
        editableHeightMap_ = new Image(context_);
        editableHeightMap_->SetSize(w, h, 2);
        editableHeightMap_->SetName(origHM->GetName());
        unsigned char* src = origHM->GetData();
        unsigned char* dst = editableHeightMap_->GetData();
        for (int i = 0; i < w * h; ++i)
        {
            dst[i * 2] = src[i * origComps];
            dst[i * 2 + 1] = 0;
        }
        terrain->SetHeightMap(editableHeightMap_);
    }

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
        FetchNetworkTime();
    timeSyncTimer_ = timeOverride ? 9999.0f : 300.0f;

    CreateCelestialBodies();

    // Camera
    cameraNode_ = new Node(context_);
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(750.0f);
    cameraNode_->SetPosition(Vector3(0.0f, 7.0f, -20.0f));
}

void Water::CreateInstructions()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* ui = GetSubsystem<UI>();

    auto* instructionText = ui->GetRoot()->CreateChild<Text>();
    instructionText->SetText(
        "WASD = move, Mouse = look\n"
        "Tab = toggle cursor/camera\n"
        "LMB = raise terrain, RMB = lower\n"
        "Space = debug, Z = wireframe, H = fog\n"
        "Scroll = brush radius\n"
        "1 = track sun, 2 = track moon");
    instructionText->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 15);
    instructionText->SetTextAlignment(HA_CENTER);
    instructionText->SetHorizontalAlignment(HA_CENTER);
    instructionText->SetVerticalAlignment(VA_CENTER);
    instructionText->SetPosition(0, ui->GetRoot()->GetHeight() / 4);
}

// ============================================================================
// Viewport + Water Reflection
// ============================================================================

void Water::SetupViewport()
{
    auto* graphics = GetSubsystem<Graphics>();
    auto* renderer = GetSubsystem<Renderer>();
    auto* cache = GetSubsystem<ResourceCache>();

    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, cameraNode_->GetComponent<Camera>()));
    viewport->SetDrawDebug(true);
    renderer->SetViewport(0, viewport);

    SharedPtr<RenderPath> renderPath(new RenderPath());
    renderPath->Load(cache->GetResource<XMLFile>("RenderPaths/ForwardHWDepth.xml"));
    viewport->SetRenderPath(renderPath);

    RenderPath* rp = viewport->GetRenderPath();
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
    renderPath_ = rp;

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

void Water::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(Water, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(Water, HandlePostRenderUpdate));
}

// ============================================================================
// Menu Bar
// ============================================================================

DropDownList* Water::CreateMenuDropdown(const String& label, const Vector<String>& items)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

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

void Water::CreateMenuBar()
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

    // File
    {
        Vector<String> items;
        items.Push("Toggle Fullscreen  (F11)");
        items.Push("Exit");
        fileMenu_ = CreateMenuDropdown("File", items);
        SubscribeToEvent(fileMenu_, E_ITEMSELECTED, URHO3D_HANDLER(Water, HandleFileMenu));
    }

    // Create (placeholder)
    {
        Vector<String> items;
        items.Push("(coming soon)");
        createMenu_ = CreateMenuDropdown("Create", items);
        SubscribeToEvent(createMenu_, E_ITEMSELECTED, URHO3D_HANDLER(Water, HandleCreateMenu));
    }

    // Edit
    {
        Vector<String> items;
        items.Push("(coming soon)");
        editMenu_ = CreateMenuDropdown("Edit", items);
        SubscribeToEvent(editMenu_, E_ITEMSELECTED, URHO3D_HANDLER(Water, HandleEditMenu));
    }

    // Environment
    {
        Vector<String> items;
        items.Push("Toggle Wireframe  (Z)");
        items.Push("Toggle Debug Geometry  (Space)");
        items.Push("Toggle Height Fog  (H)");
        items.Push("Toggle Profiler");
        if (terrain_)
            items.Push("Cycle Terrain Brush");
        environmentMenu_ = CreateMenuDropdown("Environment", items);
        SubscribeToEvent(environmentMenu_, E_ITEMSELECTED, URHO3D_HANDLER(Water, HandleEnvironmentMenu));
    }
}

void Water::HandleFileMenu(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemSelected;
    int sel = eventData[P_SELECTION].GetI32();

    switch (sel)
    {
    case 0: GetSubsystem<Graphics>()->ToggleFullscreen(); break;
    case 1: engine_->Exit(); break;
    }

    fileMenu_->SetSelection(M_MAX_UNSIGNED);
}

void Water::HandleCreateMenu(StringHash eventType, VariantMap& eventData)
{
    createMenu_->SetSelection(M_MAX_UNSIGNED);
}

void Water::HandleEditMenu(StringHash eventType, VariantMap& eventData)
{
    editMenu_->SetSelection(M_MAX_UNSIGNED);
}

void Water::HandleEnvironmentMenu(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemSelected;
    int sel = eventData[P_SELECTION].GetI32();

    switch (sel)
    {
    case 0:
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
        break;
    }
    case 1: drawDebug_ = !drawDebug_; break;
    case 2:
        if (zone_)
            zone_->SetHeightFog(!zone_->GetHeightFog());
        break;
    case 3:
        if (profilerUI_)
            profilerUI_->SetVisible(!profilerUI_->IsVisible());
        break;
    case 4:
        if (terrain_)
        {
            switch (brushMode_)
            {
            case 0: brushMode_ = 1; break;
            case 1: brushMode_ = 3; break;
            case 3: brushMode_ = 4; break;
            default: brushMode_ = 0; break;
            }
            static const char* modeNames[] = {"Off", "Raise/Lower", "Lower", "Smooth", "Flatten"};
            URHO3D_LOGINFO("Terrain brush: " + String(modeNames[brushMode_]));
        }
        break;
    }

    environmentMenu_->SetSelection(M_MAX_UNSIGNED);
}

// ============================================================================
// Camera & Input
// ============================================================================

void Water::MoveCamera(float timeStep)
{
    if (GetSubsystem<UI>()->GetFocusElement())
        return;

    auto* input = GetSubsystem<Input>();

    // Tab = toggle cursor/camera mode
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

    // Hotkeys
    if (input->GetKeyPress(KEY_SPACE))
        drawDebug_ = !drawDebug_;

    if (input->GetKeyPress(KEY_Z))
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_F11))
        GetSubsystem<Graphics>()->ToggleFullscreen();

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

    // Scroll wheel = adjust brush radius
    int wheel = input->GetMouseMoveWheel();
    if (wheel != 0)
        brushRadius_ = Clamp(brushRadius_ + wheel * 0.5f, 1.0f, 50.0f);

    // Mouse look (when menu closed)
    if (!menuOpen_)
    {
        const float MOUSE_SENSITIVITY = 0.1f;
        IntVector2 mouseMove = input->GetMouseMove();
        yaw_ += MOUSE_SENSITIVITY * mouseMove.x_;
        pitch_ += MOUSE_SENSITIVITY * mouseMove.y_;
        pitch_ = Clamp(pitch_, -90.0f, 90.0f);
        cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));
    }

    // WASD movement
    {
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

    // Terrain brush raycast
    hasBrushHit_ = false;
    if (terrain_ && brushMode_ != 0)
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
                    hasBrushHit_ = true;

                    auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
                    if (uiElem)
                        break;

                    if (brushMode_ == 1)
                    {
                        if (input->GetMouseButtonDown(MOUSEB_LEFT))
                            ApplyBrush(cachedBrushHit_, timeStep);
                        if (input->GetMouseButtonDown(MOUSEB_RIGHT))
                            ApplyLowerBrush(cachedBrushHit_, timeStep);
                    }
                    else
                    {
                        if (input->GetMouseButtonDown(MOUSEB_LEFT) || input->GetMouseButtonDown(MOUSEB_RIGHT))
                            ApplyBrush(cachedBrushHit_, timeStep);
                    }
                    break;
                }
            }
        }
    }

    // Reflection camera aspect ratio
    if (reflectionCameraNode_)
    {
        auto* graphics = GetSubsystem<Graphics>();
        auto* reflectionCamera = reflectionCameraNode_->GetComponent<Camera>();
        if (reflectionCamera)
            reflectionCamera->SetAspectRatio((float)graphics->GetWidth() / (float)graphics->GetHeight());
    }
}

// ============================================================================
// Terrain Brush
// ============================================================================

void Water::ApplyBrush(const Vector3& worldPos, float timeStep)
{
    if (!terrain_ || !editableHeightMap_ || brushMode_ == 0)
        return;

    IntVector2 center = terrain_->WorldToHeightMap(worldPos);
    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    int radius = (int)brushRadius_;
    float baseStrength = (brushMode_ == 3 || brushMode_ == 4) ? smoothStrength_ : brushStrength_;
    float strength = baseStrength * timeStep * 2.5f;

    unsigned char* data = editableHeightMap_->GetData();
    int comps = editableHeightMap_->GetComponents();

    auto readH = [&](int px, int py) -> float {
        int idx = (py * hmW + px) * comps;
        return ((float)data[idx] + (float)data[idx + 1] / 256.0f) / 255.0f;
    };
    auto writeH = [&](int px, int py, float h) {
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        float scaled = h * 255.0f;
        int idx = (py * hmW + px) * comps;
        data[idx] = (unsigned char)scaled;
        data[idx + 1] = (unsigned char)((scaled - (float)data[idx]) * 256.0f);
    };

    float flattenHeight = 0.0f;
    if (brushMode_ == 4)
    {
        int cx = Clamp(center.x_, 0, hmW - 1);
        int cy = Clamp(center.y_, 0, hmH - 1);
        flattenHeight = readH(cx, cy);
    }

    for (int dz = -radius; dz <= radius; ++dz)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            float dist = sqrtf((float)(dx * dx + dz * dz));
            if (dist > brushRadius_)
                continue;

            int px = center.x_ + dx;
            int py = center.y_ + dz;
            if (px < 0 || px >= hmW || py < 0 || py >= hmH)
                continue;

            float falloff = 1.0f - dist / brushRadius_;
            float h = readH(px, py);

            switch (brushMode_)
            {
            case 1: h += strength * falloff; break;
            case 2: h -= strength * falloff; break;
            case 3:
            {
                float sum = 0.0f;
                int count = 0;
                for (int nz = -1; nz <= 1; ++nz)
                {
                    for (int nx = -1; nx <= 1; ++nx)
                    {
                        int npx = px + nx;
                        int npy = py + nz;
                        if (npx >= 0 && npx < hmW && npy >= 0 && npy < hmH)
                        {
                            sum += readH(npx, npy);
                            count++;
                        }
                    }
                }
                float avg = sum / (float)count;
                h = h + (avg - h) * falloff * baseStrength;
                break;
            }
            case 4:
                h = h + (flattenHeight - h) * falloff * baseStrength;
                break;
            }

            writeH(px, py, h);
        }
    }

    terrain_->ApplyHeightMap();

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

void Water::ApplyLowerBrush(const Vector3& worldPos, float timeStep)
{
    int saved = brushMode_;
    brushMode_ = 2;
    ApplyBrush(worldPos, timeStep);
    brushMode_ = saved;
}

void Water::DrawBrushCircle(const Vector3& worldPos)
{
    auto* debug = scene_->GetComponent<DebugRenderer>();
    if (!debug || !terrain_)
        return;

    Vector3 spacing = terrain_->GetSpacing();
    float worldRadius = brushRadius_ * spacing.x_;

    const int segments = 32;
    Color brushColor = Color::GREEN;
    if (brushMode_ == 2) brushColor = Color::RED;
    else if (brushMode_ == 3) brushColor = Color::CYAN;
    else if (brushMode_ == 4) brushColor = Color(1.0f, 0.5f, 0.0f);

    Vector3 prevPoint;
    for (int i = 0; i <= segments; ++i)
    {
        float angle = (float)i / (float)segments * 360.0f * DEG_TO_RAD;
        Vector3 point = worldPos + Vector3(cosf(angle) * worldRadius, 0.0f, sinf(angle) * worldRadius);
        point.y_ = terrain_->GetHeight(point) + 0.2f;
        if (i > 0)
            debug->AddLine(prevPoint, point, brushColor);
        prevPoint = point;
    }

    float cross = worldRadius * 0.15f;
    float centerY = terrain_->GetHeight(worldPos) + 0.3f;
    Vector3 center(worldPos.x_, centerY, worldPos.z_);
    debug->AddLine(center + Vector3(-cross, 0, 0), center + Vector3(cross, 0, 0), brushColor);
    debug->AddLine(center + Vector3(0, 0, -cross), center + Vector3(0, 0, cross), brushColor);
}

// ============================================================================
// Celestial Bodies
// ============================================================================

void Water::CreateCelestialBodies()
{
    auto* cache = GetSubsystem<ResourceCache>();
    sunNode_ = scene_->CreateChild("Sun");
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

    moonNode_ = scene_->CreateChild("Moon");
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

    Node* moonLightNode = scene_->CreateChild("MoonLight");
    moonLight_ = moonLightNode->CreateComponent<Light>();
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

float Water::CalculateSunAltitude()
{
    float decl = 23.44f * sinf((360.0f / 365.0f) * (dayOfYear_ - 81) * DEG_TO_RAD);
    float hourAngle = 15.0f * (timeOfDay_ - SOLAR_NOON);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = decl * DEG_TO_RAD;
    float haRad = hourAngle * DEG_TO_RAD;
    float sinAlt = sinf(latRad) * sinf(declRad) + cosf(latRad) * cosf(declRad) * cosf(haRad);
    return asinf(Clamp(sinAlt, -1.0f, 1.0f)) * RAD_TO_DEG;
}

float Water::CalculateSunAzimuth(float altitude)
{
    float decl = 23.44f * sinf((360.0f / 365.0f) * (dayOfYear_ - 81) * DEG_TO_RAD);
    float hourAngle = 15.0f * (timeOfDay_ - SOLAR_NOON);
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

float Water::CalculateMoonAltitude()
{
    float moonHourOffset = moonAge_ * (360.0f / 29.53f);
    float moonHourAngle = 15.0f * (timeOfDay_ - SOLAR_NOON) - moonHourOffset;
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

float Water::CalculateMoonAzimuth(float moonAlt)
{
    float moonHourOffset = moonAge_ * (360.0f / 29.53f);
    float moonHourAngle = 15.0f * (timeOfDay_ - SOLAR_NOON) - moonHourOffset;
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

Vector3 Water::AltAzToFlatEarth(float altitude, float azimuth, float distance)
{
    float altRad = altitude * DEG_TO_RAD;
    float azRad = azimuth * DEG_TO_RAD;
    float y = distance * sinf(altRad);
    float horizDist = distance * cosf(altRad);
    float x = horizDist * sinf(azRad);
    float z = horizDist * cosf(azRad);
    return Vector3(x, y, z);
}

void Water::UpdateCelestialBodies(float timeStep)
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

    float starAngle = 6.2831853f * (timeOfDay_ / 24.0f + (float)dayOfYear_ / 365.25f);

    if (skyboxMat_)
    {
        skyboxMat_->SetShaderParameter("CloudAngle", cloudAngle_);
        skyboxMat_->SetShaderParameter("StarAngle", starAngle);
    }

    cachedSunAlt_ = CalculateSunAltitude();
    cachedSunAz_ = CalculateSunAzimuth(cachedSunAlt_);
    Vector3 sunOffset = AltAzToFlatEarth(cachedSunAlt_, cachedSunAz_);
    Vector3 camPos = cameraNode_ ? cameraNode_->GetWorldPosition() : Vector3::ZERO;
    sunNode_->SetPosition(camPos + sunOffset);
    sunNode_->LookAt(camPos);
    sunLight_->GetNode()->SetDirection((camPos - sunNode_->GetPosition()).Normalized());

    cachedMoonAlt_ = CalculateMoonAltitude();
    cachedMoonAz_ = CalculateMoonAzimuth(cachedMoonAlt_);
    Vector3 moonOffset = AltAzToFlatEarth(cachedMoonAlt_, cachedMoonAz_);
    moonNode_->SetPosition(camPos + moonOffset);
    moonNode_->LookAt(camPos);
    moonLight_->GetNode()->SetDirection(-moonOffset.Normalized());

    UpdateAtmosphere(cachedSunAlt_);
}

void Water::UpdateAtmosphere(float sunAltitude)
{
    if (!zone_)
        return;

    Color ambient, fogColor, sunColor, moonColor;
    bool sunEnabled = true;
    bool moonEnabled = false;

    if (sunAltitude > 10.0f)
    {
        ambient = Color(0.15f, 0.15f, 0.15f);
        fogColor = Color(1.0f, 1.0f, 1.0f);
        sunColor = Color(1.2f, 1.2f, 1.2f);
    }
    else if (sunAltitude > 0.0f)
    {
        float t = sunAltitude / 10.0f;
        ambient = Color(0.15f, 0.15f, 0.15f).Lerp(Color(0.12f, 0.08f, 0.05f), 1.0f - t);
        fogColor = Color(1.0f, 1.0f, 1.0f).Lerp(Color(1.0f, 0.6f, 0.3f), 1.0f - t);
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

    zone_->SetAmbientColor(ambient);
    zone_->SetFogColor(fogColor);
    sunLight_->SetColor(sunColor);
    sunLight_->SetEnabled(sunEnabled);
    moonLight_->SetColor(moonColor);
    moonLight_->SetEnabled(moonEnabled);

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
// Network Time
// ============================================================================

void Water::FetchNetworkTime()
{
    auto* network = GetSubsystem<Network>();
    if (!network) return;
    timeRequest_ = network->MakeHttpRequest("http://worldclockapi.com/api/json/AUS%20Eastern%20Standard%20Time/now");
}

void Water::ProcessTimeResponse()
{
    if (!timeRequest_ || timeRequest_->GetState() == HTTP_INITIALIZING)
        return;

    if (timeRequest_->GetState() == HTTP_ERROR)
    {
        timeRequest_.Reset();
        return;
    }

    if (timeRequest_->GetState() != HTTP_OPEN && timeRequest_->GetState() != HTTP_CLOSED)
    {
        timeRequest_.Reset();
        return;
    }

    if (timeRequest_->GetAvailableSize() <= 0 && timeRequest_->GetState() != HTTP_CLOSED)
        return;

    String response;
    while (timeRequest_->GetAvailableSize() > 0)
    {
        char buf[256];
        int read = timeRequest_->Read(buf, Min((int)sizeof(buf) - 1, timeRequest_->GetAvailableSize()));
        if (read > 0)
        {
            buf[read] = 0;
            response += String(buf);
        }
    }

    if (timeRequest_->GetState() != HTTP_CLOSED)
        return;

    timeRequest_.Reset();

    if (response.Empty())
        return;

    unsigned dtPos = response.Find("\"currentDateTime\":\"");
    unsigned doyPos = response.Find("\"ordinalDate\":\"");

    if (dtPos == String::NPOS || doyPos == String::NPOS)
        return;

    dtPos += 19;
    unsigned dtEnd = response.Find("\"", dtPos);
    if (dtEnd == String::NPOS) return;
    String datetime = response.Substring(dtPos, dtEnd - dtPos);

    doyPos += 15;
    unsigned doyEnd = response.Find("\"", doyPos);
    if (doyEnd == String::NPOS) return;
    String ordinalStr = response.Substring(doyPos, doyEnd - doyPos);
    unsigned dashPos = ordinalStr.Find("-");
    if (dashPos == String::NPOS) return;
    String doyStr = ordinalStr.Substring(dashPos + 1);

    unsigned tPos = datetime.Find("T");
    if (tPos == String::NPOS) return;
    String timeStr = datetime.Substring(tPos + 1);

    unsigned plusPos = timeStr.Find("+");
    unsigned minusPos = timeStr.Find("-", 1);
    unsigned tzPos = String::NPOS;
    if (plusPos != String::NPOS) tzPos = plusPos;
    if (minusPos != String::NPOS && (tzPos == String::NPOS || minusPos < tzPos)) tzPos = minusPos;
    if (tzPos != String::NPOS) timeStr = timeStr.Substring(0, tzPos);

    Vector<String> timeParts = timeStr.Split(':');
    if (timeParts.Size() < 2) return;

    int hour = atoi(timeParts[0].CString());
    int minute = atoi(timeParts[1].CString());
    float seconds = 0.0f;
    if (timeParts.Size() >= 3)
        seconds = (float)atof(timeParts[2].CString());

    int newDayOfYear = atoi(doyStr.CString());
    float newTimeOfDay = (float)hour + minute / 60.0f + seconds / 3600.0f;

    timeOfDay_ = newTimeOfDay;
    dayOfYear_ = newDayOfYear;
}

// ============================================================================
// Update
// ============================================================================

void Water::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    MoveCamera(timeStep);

    // Network time sync
    ProcessTimeResponse();
    timeSyncTimer_ -= timeStep;
    if (timeSyncTimer_ <= 0.0f)
    {
        FetchNetworkTime();
        timeSyncTimer_ = 300.0f;
    }

    // Update celestial bodies
    UpdateCelestialBodies(timeStep);

    if (renderPath_)
        renderPath_->SetShaderParameter("MainCameraY", cameraNode_->GetWorldPosition().y_);

    if (profilerUI_)
    {
        GetSubsystem<Graphics>()->GetVulkanProfiler()->RecordFrame(timeStep);
        profilerUI_->SetCameraPos(cameraNode_->GetWorldPosition());
        profilerUI_->Update();
    }
}

void Water::HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    if (hasBrushHit_ && brushMode_ != 0)
        DrawBrushCircle(cachedBrushHit_);

    if (!drawDebug_)
        return;

    auto* physics = scene_->GetComponent<PhysicsWorld>();
    if (physics)
        physics->DrawDebugGeometry(true);
}
