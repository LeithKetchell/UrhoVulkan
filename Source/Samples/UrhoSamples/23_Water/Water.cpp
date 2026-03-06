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
#include <Urho3D/IO/Log.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Physics/CollisionShape.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/Slider.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>
#include <Urho3D/Network/Network.h>
#include <Urho3D/GraphicsAPI/Shader.h>
#include <Urho3D/GraphicsAPI/ShaderVariation.h>
#include <Urho3D/GraphicsAPI/VertexBuffer.h>

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
    terrainPanel_.Reset();
    editableHeightMap_.Reset();
}

void Water::SelectNode(Node* node)
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
}

void Water::DeselectNode()
{
    if (selectedNode_ && !selectedNode_.Expired())
    {
        auto* model = selectedNode_->GetComponent<StaticModel>();
        if (model)
        {
            for (unsigned i = 0; i < originalMaterials_.Size() && i < model->GetNumGeometries(); ++i)
                model->SetMaterial(i, originalMaterials_[i]);
        }
    }

    originalMaterials_.Clear();
    selectedNode_.Reset();
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
    CreateMinimap();

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
    zone->SetFogColor(Color(0.7f, 0.7f, 0.75f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);
    float fogMinHeight = 5.0f;   // fog base (thickest here)
    float fogMaxHeight = 18.0f;  // fog ceiling (fades to clear)
    zone->SetFogHeight(fogMinHeight);
    zone->SetFogHeightScale(1.0f / Max(fogMaxHeight - fogMinHeight, M_EPSILON));

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
        "Tab = toggle cursor/camera mode\n"
        "LMB = raise terrain, RMB = lower\n"
        "[ ] = cycle brush shape\n"
        "Scroll = brush radius\n"
        "Space = debug geometry, Z = wireframe\n"
        "H = height fog, F11 = fullscreen\n"
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
    renderPath->Load(cache->GetResource<XMLFile>("RenderPaths/ForwardHWDepthOutline.xml"));
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
        items.Push("Save Scene");
        items.Push("Load Scene");
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

    // Environment (Menu-based with Terrain submenu)
    {
        auto* cache = GetSubsystem<ResourceCache>();
        auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

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

        // Action IDs: 100=wireframe, 101=debug, 102=fog, 103=profiler
        CreateMenuItem(envPopup, "Toggle Wireframe  (Z)", 100);
        CreateMenuItem(envPopup, "Toggle Debug Geometry  (Space)", 101);
        CreateMenuItem(envPopup, "Toggle Height Fog  (H)", 102);
        CreateMenuItem(envPopup, "Toggle Profiler", 103);

        if (terrain_)
            CreateMenuItem(envPopup, "Terrain Tools", 104);

        // Time of Day slider: -12h to +12h offset
        auto* todRow = envPopup->CreateChild<UIElement>();
        todRow->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
        todRow->SetMinHeight(22);

        auto* todText = todRow->CreateChild<Text>();
        todText->SetFont(font, 12);
        todText->SetText("Time:");
        todText->SetColor(Color(0.9f, 0.9f, 0.9f));
        todText->SetMinWidth(40);

        auto* todSlider = todRow->CreateChild<Slider>();
        todSlider->SetStyleAuto();
        todSlider->SetFixedHeight(16);
        todSlider->SetMinWidth(220);
        todSlider->SetRange(24.0f);  // 0..24 maps to -12..+12
        todSlider->SetValue(12.0f);  // center = 0 offset
        SubscribeToEvent(todSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleTimeOfDaySlider));

        todLabel_ = todRow->CreateChild<Text>();
        todLabel_->SetFont(font, 12);
        todLabel_->SetText("+0:00");
        todLabel_->SetColor(Color(0.9f, 0.9f, 0.9f));
        todLabel_->SetMinWidth(50);
    }
}

void Water::HandleFileMenu(StringHash eventType, VariantMap& eventData)
{
    using namespace ItemSelected;
    int sel = eventData[P_SELECTION].GetI32();

    switch (sel)
    {
    case 0: ShowSaveSceneDialog(); break;
    case 1: ShowLoadSceneDialog(); break;
    case 2: GetSubsystem<Graphics>()->ToggleFullscreen(); break;
    case 3: engine_->Exit(); break;
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

Menu* Water::CreateMenuItem(UIElement* parent, const String& text, int actionId)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    auto* item = parent->CreateChild<Menu>();
    item->SetStyleAuto();
    item->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));

    auto* label = item->CreateChild<Text>();
    label->SetFont(font, 12);
    label->SetText(text);
    label->SetColor(Color(0.9f, 0.9f, 0.9f));

    item->SetMinHeight(22);

    item->SetVar("MenuAction", actionId);
    SubscribeToEvent(item, E_MENUSELECTED, URHO3D_HANDLER(Water, HandleEnvironmentAction));
    return item;
}

void Water::HandleEnvironmentAction(StringHash eventType, VariantMap& eventData)
{
    using namespace MenuSelected;
    auto* element = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr());
    if (!element) return;

    int action = element->GetVar("MenuAction").GetI32();

    switch (action)
    {
    case 100: // Wireframe
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
        break;
    }
    case 101: drawDebug_ = !drawDebug_; break;
    case 102:
        if (zone_)
        {
            bool on = !zone_->GetHeightFog();
            zone_->SetHeightFog(on);
            heightFogOverride_ = on ? 1 : -1;
        }
        break;
    case 103:
        if (profilerUI_)
            profilerUI_->SetVisible(!profilerUI_->IsVisible());
        break;
    case 104:
        ToggleTerrainPanel();
        break;
    }

    // Close the top-level popup
    if (environmentMenu_ && environmentMenu_->GetShowPopup())
        environmentMenu_->ShowPopup(false);
}

void Water::HandleTimeOfDaySlider(StringHash eventType, VariantMap& eventData)
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

void Water::HandleMenuButton(StringHash eventType, VariantMap& eventData)
{
    using namespace Released;
    auto* element = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr());
    if (!element) return;

    int action = element->GetVar("MenuAction").GetI32();

    switch (action)
    {
    // Brush modes — stay in menu mode so user can also pick shape, then Tab to paint
    case 200:
        brushMode_ = 0;
        if (terrainPanel_) terrainPanel_->SetVisible(false);
        break;
    case 201: brushMode_ = 1; break;
    case 202: brushMode_ = 3; break;
    case 203: brushMode_ = 4; break;
    case 204: brushMode_ = 5; break;

    // Brush shapes
    case 210: brushShape_ = 0; break;
    case 211: brushShape_ = 1; break;
    case 212: brushShape_ = 2; break;
    case 213: brushShape_ = 3; break;
    case 214: brushShape_ = 4; break;
    case 215: brushShape_ = 5; break;
    case 216: brushShape_ = 6; break;

    // Rotation
    case 220: brushRotation_ -= 15.0f; if (brushRotation_ < 0.0f) brushRotation_ += 360.0f; break;
    case 221: brushRotation_ += 15.0f; if (brushRotation_ >= 360.0f) brushRotation_ -= 360.0f; break;
    case 222: brushRotation_ = 0.0f; break;

    // Save/Load heightmap via file dialog
    case 230: ShowSaveHeightmapDialog(); break;
    case 231: ShowLoadHeightmapDialog(); break;

    // Compute shaders
    case 240: RunErosion(erosionIterations_); break;
    case 241: TestComputeShader(); break;
    }

    // Refresh shape icons when rotation changes
    if (action >= 220 && action <= 222)
    {
        for (int i = 0; i < 7; ++i)
        {
            if (shapeIcons_[i])
                shapeIcons_[i]->SetTexture(GenerateShapeIcon(i));
        }
    }

    // Auto-enter camera mode only when a brush mode button was just clicked
    if (action >= 201 && action <= 204)
    {
        menuOpen_ = false;
        auto* input = GetSubsystem<Input>();
        GetSubsystem<UI>()->SetFocusElement(nullptr);
        input->SetMouseMode(MM_RELATIVE);
        input->SetMouseVisible(false);
        useMouseMode_ = MM_RELATIVE;
    }
}

void Water::CreateTerrainPanel()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
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

    // Brush mode label
    auto* modeLabel = terrainPanel_->CreateChild<Text>();
    modeLabel->SetFont(font, 11);
    modeLabel->SetText("Brush Mode:");
    modeLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    // Brush mode buttons
    auto* modeRow = terrainPanel_->CreateChild<UIElement>();
    modeRow->SetLayout(LM_HORIZONTAL, 2);
    modeRow->SetFixedHeight(24);
    const char* modeNames[] = {"Off", "Height", "Smooth", "Flatten", "Erode"};
    const int modeActions[] = {200, 201, 202, 203, 204};
    for (int i = 0; i < 5; ++i)
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
        SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));
    }

    // Shape label
    auto* shapeLabel = terrainPanel_->CreateChild<Text>();
    shapeLabel->SetFont(font, 11);
    shapeLabel->SetText("Brush Shape:");
    shapeLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    // Shape buttons with programmatic icons
    auto* shapeRow = terrainPanel_->CreateChild<UIElement>();
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
        SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));
    }

    // Brush size slider (1-50)
    auto* sizeRow = terrainPanel_->CreateChild<UIElement>();
    sizeRow->SetLayout(LM_HORIZONTAL, 4);
    sizeRow->SetFixedHeight(20);
    brushSizeLabel_ = sizeRow->CreateChild<Text>();
    brushSizeLabel_->SetFont(font, 10);
    brushSizeLabel_->SetText("Size: " + String((int)brushRadius_));
    brushSizeLabel_->SetMinWidth(100);
    auto* sizeSlider = sizeRow->CreateChild<Slider>();
    sizeSlider->SetStyleAuto();
    sizeSlider->SetFixedHeight(16);
    sizeSlider->SetMinWidth(100);
    sizeSlider->SetRange(49.0f);  // 1..50
    sizeSlider->SetValue(brushRadius_ - 1.0f);
    sizeSlider->SetVar("SliderID", 10);
    SubscribeToEvent(sizeSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Brush strength slider (0.01-0.2)
    auto* bstrRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(bstrSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Rotation controls
    auto* rotLabel = terrainPanel_->CreateChild<Text>();
    rotLabel->SetFont(font, 11);
    rotLabel->SetText("Rotation:");
    rotLabel->SetColor(Color(0.7f, 0.7f, 0.7f));

    auto* rotRow = terrainPanel_->CreateChild<UIElement>();
    rotRow->SetLayout(LM_HORIZONTAL, 4);
    rotRow->SetFixedHeight(24);

    auto* rotLeft = rotRow->CreateChild<Button>();
    rotLeft->SetStyleAuto();
    rotLeft->SetMinWidth(40);
    rotLeft->SetFixedHeight(22);
    rotLeft->SetVar("MenuAction", 220);
    auto* rlTxt = rotLeft->CreateChild<Text>();
    rlTxt->SetFont(font, 11);
    rlTxt->SetText("<< 15");
    rlTxt->SetColor(Color::WHITE);
    rlTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(rotLeft, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));

    auto* rotRight = rotRow->CreateChild<Button>();
    rotRight->SetStyleAuto();
    rotRight->SetMinWidth(40);
    rotRight->SetFixedHeight(22);
    rotRight->SetVar("MenuAction", 221);
    auto* rrTxt = rotRight->CreateChild<Text>();
    rrTxt->SetFont(font, 11);
    rrTxt->SetText("15 >>");
    rrTxt->SetColor(Color::WHITE);
    rrTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(rotRight, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));

    auto* rotReset = rotRow->CreateChild<Button>();
    rotReset->SetStyleAuto();
    rotReset->SetMinWidth(30);
    rotReset->SetFixedHeight(22);
    rotReset->SetVar("MenuAction", 222);
    auto* resTxt = rotReset->CreateChild<Text>();
    resTxt->SetFont(font, 11);
    resTxt->SetText("0");
    resTxt->SetColor(Color::WHITE);
    resTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(rotReset, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));

    // Save/Load heightmap
    auto* ioRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(saveBtn, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));

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
    SubscribeToEvent(loadBtn, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));

    // --- Erosion section ---
    auto* divider = terrainPanel_->CreateChild<BorderImage>();
    divider->SetStyle("EditorDivider");

    auto* erosionTitle = terrainPanel_->CreateChild<Text>();
    erosionTitle->SetFont(font, 11);
    erosionTitle->SetText("Hydraulic Erosion");
    erosionTitle->SetColor(Color(0.5f, 1.0f, 0.5f));

    // Iterations slider (10-500)
    auto* iterRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(iterSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Rainfall slider (0.001-0.05)
    auto* rainRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(rainSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Erosion strength slider (0.01-1.0)
    auto* strRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(strSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Max Depth slider (0.0-1.0, fraction of original height)
    auto* depthRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(depthSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Min Height Floor slider (0.0-0.5, fraction of height range)
    auto* floorRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(floorSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Ridge Protection slider (0.0-1.0)
    auto* ridgeRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(ridgeSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Border Padding slider (0-16 cells)
    auto* borderRow = terrainPanel_->CreateChild<UIElement>();
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
    SubscribeToEvent(borderSlider, E_SLIDERCHANGED, URHO3D_HANDLER(Water, HandleErosionSlider));

    // Erode button
    auto* erodeBtn = terrainPanel_->CreateChild<Button>();
    erodeBtn->SetStyleAuto();
    erodeBtn->SetMinWidth(120);
    erodeBtn->SetFixedHeight(22);
    erodeBtn->SetVar("MenuAction", 240);
    auto* erodeTxt = erodeBtn->CreateChild<Text>();
    erodeTxt->SetFont(font, 11);
    erodeTxt->SetText("Erode World");
    erodeTxt->SetColor(Color(0.5f, 1.0f, 0.5f));
    erodeTxt->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(erodeBtn, E_RELEASED, URHO3D_HANDLER(Water, HandleMenuButton));
}

void Water::HandleErosionSlider(StringHash eventType, VariantMap& eventData)
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
    case 10:  // Brush Size: 1..50
        brushRadius_ = 1.0f + value;
        if (brushSizeLabel_)
            brushSizeLabel_->SetText("Size: " + String((int)brushRadius_));
        break;
    case 11:  // Brush Strength: 0.01..5.0
        brushStrength_ = 0.01f + value * 4.99f;
        if (brushStrLabel_)
            brushStrLabel_->SetText("Strength: " + String(brushStrength_, 2));
        break;
    }
}

void Water::ToggleTerrainPanel()
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

void Water::ShowSaveHeightmapDialog()
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

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(Water, HandleHeightmapSaveChosen));
}

void Water::ShowLoadHeightmapDialog()
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

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(Water, HandleHeightmapLoadChosen));
}

void Water::HandleHeightmapSaveChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;
    SaveHeightmapToFile(path);
}

void Water::HandleHeightmapLoadChosen(StringHash eventType, VariantMap& eventData)
{
    using namespace FileSelected;
    String path = eventData[P_FILENAME].GetString();
    bool ok = eventData[P_OK].GetBool();
    fileSelector_.Reset();

    if (!ok || path.Empty()) return;
    LoadHeightmapFromFile(path);
}

void Water::SaveHeightmapToFile(const String& path)
{
    if (!editableHeightMap_) return;

    if (editableHeightMap_->SavePNG(path))
        URHO3D_LOGINFOF("Heightmap saved to %s", path.CString());
    else
        URHO3D_LOGERRORF("Failed to save heightmap to %s", path.CString());
}

void Water::LoadHeightmapFromFile(const String& path)
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
    }
    terrain_->ApplyHeightMap();
    WakeSleepingBodiesOnTerrain();
    URHO3D_LOGINFOF("Heightmap loaded from %s", path.CString());
}

void Water::ShowSaveSceneDialog()
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

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(Water, HandleSceneSaveChosen));
}

void Water::ShowLoadSceneDialog()
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

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(Water, HandleSceneLoadChosen));
}

void Water::HandleSceneSaveChosen(StringHash eventType, VariantMap& eventData)
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

void Water::HandleSceneLoadChosen(StringHash eventType, VariantMap& eventData)
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
                editableHeightMap_->SetSize(w, h, 2);
                editableHeightMap_->SetName(origHM->GetName());
                unsigned char* src = origHM->GetData();
                unsigned char* dst = editableHeightMap_->GetData();
                for (int i = 0; i < w * h; ++i)
                {
                    dst[i * 2] = src[i * origComps];
                    dst[i * 2 + 1] = (origComps >= 2) ? src[i * origComps + 1] : 0;
                }
                terrain_->SetHeightMap(editableHeightMap_);
            }
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

void Water::WakeSleepingBodiesOnTerrain()
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
// Minimap
// ============================================================================

void Water::CreateMinimap()
{
    if (!editableHeightMap_ || !terrain_)
        return;

    auto* ui = GetSubsystem<UI>();
    auto* uiRoot = ui->GetRoot();
    const int displaySize = 192;
    const int viewRadius = 80; // heightmap pixels visible around camera

    // Load weight map for terrain coloring
    auto* cache = GetSubsystem<ResourceCache>();
    weightMapImg_ = cache->GetResource<Image>("Textures/TerrainWeights.png");

    // Create display image (RGBA) — fixed size, shows zoomed region
    minimapImg_ = new Image(context_);
    minimapImg_->SetSize(displaySize, displaySize, 4);

    minimapTex_ = new Texture2D(context_);
    minimapTex_->SetSize(displaySize, displaySize, Graphics::GetRGBAFormat());
    minimapTex_->SetFilterMode(FILTER_BILINEAR);

    // BorderImage in bottom-right corner
    minimap_ = uiRoot->CreateChild<BorderImage>("Minimap");
    minimap_->SetTexture(minimapTex_);
    minimap_->SetImageRect(IntRect(0, 0, displaySize, displaySize));
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

void Water::UpdateMinimapTexture()
{
    // Called on terrain edits — just flag that the next camera update should refresh
    // The actual rendering happens in UpdateMinimapCamera every frame
}

void Water::UpdateMinimapCamera()
{
    if (!minimap_ || !minimapImg_ || !minimapTex_ || !terrain_ || !cameraNode_ || !editableHeightMap_)
        return;

    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    int displaySize = minimapImg_->GetWidth();
    const int viewRadius = 80; // heightmap pixels visible in each direction from camera

    unsigned char* src = editableHeightMap_->GetData();
    int srcComps = editableHeightMap_->GetComponents();

    IntVector2 hmPos = terrain_->WorldToHeightMap(cameraNode_->GetWorldPosition());
    int camHX = hmPos.x_;
    int camHY = hmPos.y_;

    float waterH = 0.0f;
    if (waterNode_ && terrain_)
        waterH = waterNode_->GetWorldPosition().y_ / terrain_->GetSpacing().y_;

    // Camera facing direction from yaw — stable regardless of pitch
    // Heightmap Y is inverted from world Z, so negate Z component
    float yawRad = yaw_ * DEG_TO_RAD;
    float dirX = sinf(yawRad);
    float dirZ = -cosf(yawRad);

    float coneHalf = cosf(30.0f * DEG_TO_RAD);

    // Render zoomed view: each display pixel maps to a heightmap pixel
    float scale = (float)(viewRadius * 2) / (float)displaySize;

    for (int py = 0; py < displaySize; ++py)
    {
        for (int px = 0; px < displaySize; ++px)
        {
            // Heightmap coordinate for this display pixel
            int hx = camHX + (int)((px - displaySize / 2) * scale);
            int hy = camHY + (int)((py - displaySize / 2) * scale);

            if (hx < 0 || hx >= hmW || hy < 0 || hy >= hmH)
            {
                minimapImg_->SetPixel(px, py, Color(0.02f, 0.02f, 0.05f, 1.0f));
                continue;
            }

            int si = (hy * hmW + hx) * srcComps;
            float h = (float)src[si] + (float)src[si + 1] / 256.0f;
            float brightness = Clamp(h / 255.0f, 0.0f, 1.0f) * 0.5f + 0.5f;

            // Sample weight map for terrain layer blending
            float r, gb, b;
            if (h < waterH)
            {
                // Below water: blue tint
                r = brightness * 0.2f;
                gb = brightness * 0.3f;
                b = Clamp(brightness * 0.5f + 0.3f, 0.0f, 1.0f);
            }
            else if (weightMapImg_)
            {
                // Layer colors: 1=grass, 2=rock, 3=snow, 4=sand
                static const float layerR[] = {0.3f, 0.45f, 0.95f, 0.76f};
                static const float layerG[] = {0.55f, 0.38f, 0.95f, 0.65f};
                static const float layerB[] = {0.15f, 0.30f, 1.0f, 0.40f};

                int wmW = weightMapImg_->GetWidth();
                int wmH = weightMapImg_->GetHeight();
                int wx = hx * wmW / hmW;
                int wy = hy * wmH / hmH;
                wx = Clamp(wx, 0, wmW - 1);
                wy = Clamp(wy, 0, wmH - 1);
                Color wc = weightMapImg_->GetPixel(wx, wy);
                float weights[4] = {wc.r_, wc.g_, wc.b_, wc.a_};

                r = gb = b = 0.0f;
                for (int l = 0; l < 4; ++l)
                {
                    r += weights[l] * layerR[l];
                    gb += weights[l] * layerG[l];
                    b += weights[l] * layerB[l];
                }
                r *= brightness;
                gb *= brightness;
                b *= brightness;
            }
            else
            {
                float t = Clamp((h - waterH) / 40.0f, 0.0f, 1.0f);
                r = brightness * (0.3f + t * 0.5f);
                gb = brightness * (0.55f - t * 0.2f);
                b = brightness * 0.15f;
            }

            // Spotlight cone overlay
            float offX = (float)(px - displaySize / 2);
            float offY = (float)(py - displaySize / 2);
            float dist = sqrtf(offX * offX + offY * offY);
            if (dist > 5.0f && dist < displaySize * 0.45f)
            {
                float pdx = offX / dist;
                float pdy = offY / dist;
                float dot = pdx * dirX + pdy * dirZ;
                if (dot > coneHalf)
                {
                    float angleFade = (dot - coneHalf) / (1.0f - coneHalf);
                    float distFade = 1.0f - dist / (displaySize * 0.45f);
                    float bright = angleFade * distFade * 0.4f;
                    r = Clamp(r + bright, 0.0f, 1.0f);
                    gb = Clamp(gb + bright, 0.0f, 1.0f);
                    b = Clamp(b + bright * 0.7f, 0.0f, 1.0f);
                }
            }

            minimapImg_->SetPixel(px, py, Color(r, gb, b, 1.0f));
        }
    }

    minimapTex_->SetData(minimapImg_);
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

SharedPtr<Texture2D> Water::GenerateShapeIcon(int shape)
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

void Water::MoveCamera(float timeStep)
{
    auto* input = GetSubsystem<Input>();

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

    if (GetSubsystem<UI>()->GetFocusElement())
        return;

    // Hotkeys
    if (input->GetKeyPress(KEY_SPACE))
        drawDebug_ = !drawDebug_;

    if (input->GetKeyPress(KEY_Z))
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_H))
    {
        if (zone_)
        {
            bool on = !zone_->GetHeightFog();
            zone_->SetHeightFog(on);
            heightFogOverride_ = on ? 1 : -1;
        }
    }

    if (input->GetKeyPress(KEY_F))
    {
        auto* camera = cameraNode_ ? cameraNode_->GetComponent<Camera>() : nullptr;
        if (camera)
            camera->SetFillMode(camera->GetFillMode() == FILL_SOLID ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_F5))
        drawDebug_ = !drawDebug_;

    if (input->GetKeyPress(KEY_F11))
        GetSubsystem<Graphics>()->ToggleFullscreen();

    if ((input->GetKeyPress(KEY_BACKSPACE) || input->GetKeyPress(KEY_DELETE)) && selectedNode_ && !selectedNode_.Expired())
    {
        Node* node = selectedNode_;
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
    bool showBrush = (brushMode_ != 0) || (terrainPanel_ && terrainPanel_->IsVisible());
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
                    hasBrushHit_ = true;

                    // In menu mode, don't paint if mouse is over a UI element
                    if (menuOpen_)
                    {
                        auto* uiElem = GetSubsystem<UI>()->GetElementAt(input->GetMousePosition());
                        if (uiElem)
                            break;
                    }

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

    // Object selection raycast (when no brush mode active)
    if (brushMode_ == 0 && input->GetMouseButtonPress(MOUSEB_LEFT))
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
}

// ============================================================================
// Terrain Brush
// ============================================================================

// Unified shape falloff: build the same vertices as DrawBrushOutline,
// then test point-in-polygon + distance-to-nearest-edge for falloff.
float Water::BrushShapeFalloff(float dx, float dz, float radius) const
{
    if (radius < 0.001f)
        return -1.0f;

    // Apply inverse rotation to test point (rotates shape visually)
    if (brushRotation_ != 0.0f)
    {
        float ra = -brushRotation_ * DEG_TO_RAD;
        float cs = cosf(ra), sn = sinf(ra);
        float rdx = dx * cs - dz * sn;
        float rdz = dx * sn + dz * cs;
        dx = rdx;
        dz = rdz;
    }

    // Circle uses simple radial distance
    if (brushShape_ == 0)
    {
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist > radius) return -1.0f;
        return 1.0f - dist / radius;
    }

    // Square uses max-axis distance
    if (brushShape_ == 1)
    {
        float adx = fabsf(dx), adz = fabsf(dz);
        if (adx > radius || adz > radius) return -1.0f;
        float edgeDist = Max(adx, adz);
        return 1.0f - edgeDist / radius;
    }

    // All other shapes: build vertex list, test point-in-polygon
    float vx[20], vz[20];
    int nv = 0;

    switch (brushShape_)
    {
    case 2: // Triangle (point-up) — must match DrawBrushOutline angles
    {
        float angles[3] = {90.0f * DEG_TO_RAD, 210.0f * DEG_TO_RAD, 330.0f * DEG_TO_RAD};
        for (int i = 0; i < 3; ++i)
        {
            vx[i] = radius * cosf(angles[i]);
            vz[i] = radius * sinf(angles[i]);
        }
        nv = 3;
        break;
    }
    case 3: // Star (5-pointed, 10 vertices) — start at top (+π/2)
    {
        float innerR = radius * 0.38f;
        for (int i = 0; i < 10; ++i)
        {
            float a = (float)(M_PI / 2.0) + (float)i / 10.0f * 2.0f * (float)M_PI;
            float r = (i % 2 == 0) ? radius : innerR;
            vx[i] = r * cosf(a);
            vz[i] = r * sinf(a);
        }
        nv = 10;
        break;
    }
    default: // 4=pentagon(5), 5=hexagon(6), 6=octagon(8) — start at top (+π/2)
    {
        int sides;
        switch (brushShape_) { case 4: sides = 5; break; case 5: sides = 6; break; default: sides = 8; break; }
        for (int i = 0; i < sides; ++i)
        {
            float a = (float)(M_PI / 2.0) + (float)i / (float)sides * 2.0f * (float)M_PI;
            vx[i] = radius * cosf(a);
            vz[i] = radius * sinf(a);
        }
        nv = sides;
        break;
    }
    }

    // Point-in-polygon (ray casting / crossing number)
    int crossings = 0;
    for (int i = 0; i < nv; ++i)
    {
        int j = (i + 1) % nv;
        if ((vz[i] <= dz && vz[j] > dz) || (vz[j] <= dz && vz[i] > dz))
        {
            float t = (dz - vz[i]) / (vz[j] - vz[i]);
            if (dx < vx[i] + t * (vx[j] - vx[i]))
                crossings++;
        }
    }
    if ((crossings & 1) == 0)
        return -1.0f; // outside

    // Falloff: distance to nearest edge, normalized
    float minDist = 1e9f;
    for (int i = 0; i < nv; ++i)
    {
        int j = (i + 1) % nv;
        float ex = vx[j] - vx[i], ez = vz[j] - vz[i];
        float len = sqrtf(ex * ex + ez * ez);
        if (len < 0.001f) continue;
        // Perpendicular distance from point to edge line
        float d = fabsf((dx - vx[i]) * (-ez / len) + (dz - vz[i]) * (ex / len));
        if (d < minDist) minDist = d;
    }
    // Normalize by inradius approximation (half of radius)
    float inradius = radius * 0.4f;
    return Clamp(minDist / inradius, 0.0f, 1.0f);
}

void Water::ApplyBrush(const Vector3& worldPos, float timeStep)
{
    if (!terrain_ || !editableHeightMap_ || brushMode_ == 0)
        return;

    // Diagnostic: log shape on first frame of each stroke
    static bool wasActive = false;
    bool active = true;
    if (!wasActive)
        URHO3D_LOGINFOF("ApplyBrush: shape=%d mode=%d radius=%.1f", brushShape_, brushMode_, brushRadius_);
    wasActive = active;

    IntVector2 center = terrain_->WorldToHeightMap(worldPos);
    int hmW = editableHeightMap_->GetWidth();
    int hmH = editableHeightMap_->GetHeight();
    int radius = (int)brushRadius_;
    float baseStrength = (brushMode_ == 3) ? smoothStrength_ : brushStrength_;
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
        // Lock height on first click, keep it until mouse released
        if (lockedFlattenHeight_ < 0.0f)
        {
            int cx = Clamp(center.x_, 0, hmW - 1);
            int cy = Clamp(center.y_, 0, hmH - 1);
            lockedFlattenHeight_ = readH(cx, cy);
        }
        flattenHeight = lockedFlattenHeight_;
    }

    for (int dz = -radius; dz <= radius; ++dz)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            float falloff = BrushShapeFalloff((float)dx, (float)dz, brushRadius_);
            if (falloff < 0.0f)
                continue;

            int px = center.x_ + dx;
            int py = center.y_ + dz;
            if (px < 0 || px >= hmW || py < 0 || py >= hmH)
                continue;

            float h = readH(px, py);

            switch (brushMode_)
            {
            case 1: h += strength * falloff; break;
            case 2: h -= strength * falloff; break;
            case 3: // Smooth toward water height
            {
                float waterTarget = waterNode_->GetWorldPosition().y_ / (255.0f * terrain_->GetSpacing().y_);
                h = h + (waterTarget - h) * falloff * baseStrength;
                break;
            }
            case 4:
                // Center (falloff=1): snap to target height
                // Edge (falloff→0): keep original height
                h = h + (flattenHeight - h) * falloff;
                break;
            case 5:
            {
                // Erosion brush: move material downhill to lowest neighbor
                float lowest = h;
                int lx = px, ly = py;
                for (int ny = -1; ny <= 1; ++ny)
                {
                    for (int nx = -1; nx <= 1; ++nx)
                    {
                        if (nx == 0 && ny == 0) continue;
                        int npx = px + nx, npy = py + ny;
                        if (npx < 0 || npx >= hmW || npy < 0 || npy >= hmH) continue;
                        float nh = readH(npx, npy);
                        if (nh < lowest) { lowest = nh; lx = npx; ly = npy; }
                    }
                }
                float diff = h - lowest;
                if (diff > 0.0001f)
                {
                    float transfer = diff * falloff * baseStrength;
                    h -= transfer;
                    writeH(lx, ly, lowest + transfer);
                }
                break;
            }
            }

            writeH(px, py, h);
        }
    }

    terrain_->ApplyHeightMap();
    UpdateMinimapTexture();

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

void Water::DrawBrushOutline(const Vector3& worldPos)
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
    float hourAngle = 15.0f * (timeOfDay_ + timeOfDayOffset_ - SOLAR_NOON);
    float latRad = MELBOURNE_LAT * DEG_TO_RAD;
    float declRad = decl * DEG_TO_RAD;
    float haRad = hourAngle * DEG_TO_RAD;
    float sinAlt = sinf(latRad) * sinf(declRad) + cosf(latRad) * cosf(declRad) * cosf(haRad);
    return asinf(Clamp(sinAlt, -1.0f, 1.0f)) * RAD_TO_DEG;
}

float Water::CalculateSunAzimuth(float altitude)
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

float Water::CalculateMoonAltitude()
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

float Water::CalculateMoonAzimuth(float moonAlt)
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
    if (sunNode_)
    {
        sunNode_->SetPosition(camPos + sunOffset);
        sunNode_->LookAt(camPos);
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
    }
    if (moonLight_)
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
        float normalScale = 1.0f / 13.0f;  // 1/(fogMaxHeight - fogMinHeight) = 1/(18-5)
        if (sunAltitude > 20.0f)
        {
            zone_->SetHeightFog(false);
        }
        else if (sunAltitude > 5.0f)
        {
            float t = (sunAltitude - 5.0f) / 15.0f;  // 0 at 5°, 1 at 20°
            zone_->SetHeightFog(true);
            zone_->SetFogHeightScale(Lerp(normalScale, 50.0f, t));  // scale up = range shrinks = fog vanishes
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

    // Update minimap camera position
    UpdateMinimapCamera();

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
    if (hasBrushHit_ && (brushMode_ != 0 || (terrainPanel_ && terrainPanel_->IsVisible())))
        DrawBrushOutline(cachedBrushHit_);

    if (!drawDebug_)
        return;

    auto* physics = scene_->GetComponent<PhysicsWorld>();
    if (physics)
        physics->DrawDebugGeometry(true);
}

void Water::TestComputeShader()
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

void Water::RunErosion(int iterations)
{
    if (!terrain_ || !editableHeightMap_)
    {
        URHO3D_LOGERROR("RunErosion: No terrain or heightmap");
        return;
    }

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

            if (comps >= 2)
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
        UpdateMinimapTexture();
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
