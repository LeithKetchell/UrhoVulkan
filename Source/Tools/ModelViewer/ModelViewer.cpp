#include "ModelViewer.h"

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Core/ProcessUtils.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Engine/EngineDefs.h>
#include <Urho3D/Graphics/Animation.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/Geometry.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Light.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Skeleton.h>
#include <Urho3D/Graphics/Technique.h>
#include <Urho3D/Graphics/Viewport.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/GraphicsAPI/Texture2D.h>
#include <Urho3D/Resource/Image.h>
#include <Urho3D/GraphicsAPI/IndexBuffer.h>
#include <Urho3D/GraphicsAPI/VertexBuffer.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Input/InputEvents.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/JSONFile.h>
#include <Urho3D/Resource/JSONValue.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Graphics/DrawableEvents.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/ScrollView.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/UIEvents.h>

#include <climits>

URHO3D_DEFINE_APPLICATION_MAIN(ModelViewer);

ModelViewer::ModelViewer(Context* context) : Application(context) {}

void ModelViewer::Setup()
{
    engineParameters_[EP_WINDOW_TITLE] = "ModelViewer";
    engineParameters_[EP_WINDOW_WIDTH] = 1280;
    engineParameters_[EP_WINDOW_HEIGHT] = 720;
    engineParameters_[EP_FULL_SCREEN] = false;
    engineParameters_[EP_LOG_NAME] = "ModelViewer.log";
    engineParameters_[EP_RESOURCE_PATHS] = "CoreData;Data";
    engineParameters_[EP_SOUND] = false;
    engineParameters_[EP_WINDOW_RESIZABLE] = true;
}

void ModelViewer::Start()
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* style = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(style);
    font_ = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    CreateScene();
    SetupViewport();
    CreateUI();

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(ModelViewer, HandleUpdate));
    SubscribeToEvent(E_POSTRENDERUPDATE, URHO3D_HANDLER(ModelViewer, HandlePostRenderUpdate));
    SubscribeToEvent(E_MOUSEMOVE, URHO3D_HANDLER(ModelViewer, HandleMouseMove));
    SubscribeToEvent(E_MOUSEWHEEL, URHO3D_HANDLER(ModelViewer, HandleMouseWheel));

    // Command-line arguments
    const Vector<String>& args = GetArguments();
    for (unsigned i = 0; i < args.Size(); ++i)
    {
        if ((args[i] == "-model" || args[i] == "model") && i + 1 < args.Size())
        {
            LoadModel(args[i + 1]);
        }
        else if ((args[i] == "-folder" || args[i] == "folder") && i + 1 < args.Size())
        {
            ScanFolder(args[i + 1]);
        }
    }

    GetSubsystem<Input>()->SetMouseVisible(true);
    GetSubsystem<Input>()->SetMouseGrabbed(false);
}

void ModelViewer::Stop() {}

// ============================================================================
// Scene
// ============================================================================

void ModelViewer::CreateScene()
{
    scene_ = new Scene(context_);
    scene_->CreateComponent<Octree>();
    scene_->CreateComponent<DebugRenderer>();

    Node* zoneNode = scene_->CreateChild("Zone");
    auto* zone = zoneNode->CreateComponent<Zone>();
    zone->SetBoundingBox(BoundingBox(-1000.0f, 1000.0f));
    zone->SetAmbientColor(Color(0.3f, 0.3f, 0.3f));
    zone->SetFogColor(Color(0.2f, 0.2f, 0.25f));
    zone->SetFogStart(500.0f);
    zone->SetFogEnd(750.0f);

    lightNode_ = scene_->CreateChild("Light");
    lightNode_->SetDirection(Vector3(0.6f, -1.0f, 0.8f));
    auto* light = lightNode_->CreateComponent<Light>();
    light->SetLightType(LIGHT_DIRECTIONAL);
    light->SetColor(Color(1.0f, 0.95f, 0.9f));
    light->SetCastShadows(true);

    // Ground plane for visual reference
    auto* cache = GetSubsystem<ResourceCache>();
    Node* planeNode = scene_->CreateChild("Plane");
    planeNode->SetScale(Vector3(20.0f, 1.0f, 20.0f));
    auto* planeObject = planeNode->CreateComponent<StaticModel>();
    planeObject->SetModel(cache->GetResource<Model>("Models/Plane.mdl"));
    planeObject->SetMaterial(cache->GetResource<Material>("Materials/StoneTiled.xml"));

    modelNode_ = scene_->CreateChild("Model");

    cameraNode_ = scene_->CreateChild("Camera");
    auto* camera = cameraNode_->CreateComponent<Camera>();
    camera->SetFarClip(300.0f);
    camera->SetNearClip(0.1f);
    cameraNode_->SetPosition(Vector3(0.0f, 2.0f, -5.0f));
    cameraNode_->LookAt(Vector3::ZERO);
}

void ModelViewer::SetupViewport()
{
    auto* renderer = GetSubsystem<Renderer>();
    auto* camera = cameraNode_->GetComponent<Camera>();
    SharedPtr<Viewport> viewport(new Viewport(context_, scene_, camera));
    renderer->SetViewport(0, viewport);
}

// ============================================================================
// Model Loading
// ============================================================================

void ModelViewer::LoadModel(const String& path)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* model = cache->GetResource<Model>(path);
    if (!model)
    {
        URHO3D_LOGERRORF("Failed to load model: %s", path.CString());
        if (statusText_) statusText_->SetText("ERROR: " + path);
        return;
    }

    modelNode_->RemoveAllComponents();
    modelNode_->RemoveAllChildren();
    modelNode_->SetPosition(Vector3::ZERO);
    modelNode_->SetRotation(Quaternion::IDENTITY);
    modelNode_->SetScale(1.0f);

    URHO3D_LOGINFOF("Loaded model: %s (%d geometries, %d bones)",
        path.CString(), model->GetNumGeometries(), model->GetSkeleton().GetNumBones());

    currentModel_ = model;
    currentModelPath_ = path;
    isAnimated_ = false;
    staticModelComp_ = nullptr;
    animatedModelComp_ = nullptr;
    animController_ = nullptr;
    currentAnimIndex_ = -1;
    animPlaying_ = false;
    availableAnims_.Clear();

    Skeleton& skeleton = model->GetSkeleton();
    if (skeleton.GetNumBones() > 1)
    {
        isAnimated_ = true;
        animatedModelComp_ = modelNode_->CreateComponent<AnimatedModel>();
        animatedModelComp_->SetModel(model, true, true);
        animatedModelComp_->SetCastShadows(true);
        staticModelComp_ = animatedModelComp_;
        animController_ = modelNode_->CreateComponent<AnimationController>();
        ScanAnimations();
    }
    else
    {
        staticModelComp_ = modelNode_->CreateComponent<StaticModel>();
        staticModelComp_->SetModel(model, true);
        staticModelComp_->SetCastShadows(true);
    }

    // --- Material resolution ---
    // Try: 1) material list .txt, 2) per-geometry Materials/Name_N.xml,
    // 3) single Materials/Name.xml for all, 4) fallback default lit material
    bool materialsApplied = false;
    String matListPath = ReplaceExtension(path, ".txt");
    if (cache->Exists(matListPath))
    {
        staticModelComp_->ApplyMaterialList(matListPath);
        materialsApplied = true;
    }

    if (!materialsApplied)
    {
        // Try Materials/ directory relative to model path, and also root Materials/
        String modelDir = GetPath(path);       // e.g. "Models/"
        String baseName = GetFileName(path);   // e.g. "Jack"
        unsigned numGeoms = model->GetNumGeometries();

        // Try per-geometry: Materials/Name_0.xml, Materials/Name_1.xml, ...
        bool allFound = true;
        for (unsigned i = 0; i < numGeoms; ++i)
        {
            String matPath = modelDir + "Materials/" + baseName + "_" + String(i) + ".xml";
            if (!cache->Exists(matPath))
                matPath = "Materials/" + baseName + "_" + String(i) + ".xml";
            if (cache->Exists(matPath))
            {
                auto* mat = cache->GetResource<Material>(matPath);
                if (mat) staticModelComp_->SetMaterial(i, mat);
            }
            else
            {
                allFound = false;
                break;
            }
        }

        if (allFound && numGeoms > 0)
            materialsApplied = true;
    }

    if (!materialsApplied)
    {
        // Try single material: Materials/Name.xml
        String modelDir = GetPath(path);
        String baseName = GetFileName(path);
        unsigned numGeoms = model->GetNumGeometries();

        String matPath = modelDir + "Materials/" + baseName + ".xml";
        if (!cache->Exists(matPath))
            matPath = "Materials/" + baseName + ".xml";
        if (cache->Exists(matPath))
        {
            auto* mat = cache->GetResource<Material>(matPath);
            if (mat)
            {
                for (unsigned i = 0; i < numGeoms; ++i)
                    staticModelComp_->SetMaterial(i, mat);
                materialsApplied = true;
            }
        }
    }

    if (!materialsApplied)
    {
        // Scan Materials/ directory for any .xml files and apply them to geometries
        auto* fs = GetSubsystem<FileSystem>();
        String modelDir = GetPath(path);
        unsigned numGeoms = model->GetNumGeometries();
        const Vector<String>& resourceDirs = cache->GetResourceDirs();

        // Check Materials/ subdirs relative to model dir and at root
        Vector<String> matDirs;
        matDirs.Push(modelDir + "Materials/");
        matDirs.Push("Materials/");
        // Also check sibling: if model is in Models/, check ../Materials/
        if (modelDir.EndsWith("Models/") || modelDir.EndsWith("models/"))
        {
            String parent = GetParentPath(modelDir.Substring(0, modelDir.Length() - 1));
            matDirs.Push(parent + "Materials/");
        }

        Vector<String> foundMats;
        for (unsigned d = 0; d < matDirs.Size() && foundMats.Empty(); ++d)
        {
            for (unsigned r = 0; r < resourceDirs.Size() && foundMats.Empty(); ++r)
            {
                String absDir = resourceDirs[r] + matDirs[d];
                if (fs->DirExists(absDir))
                {
                    Vector<String> xmlFiles;
                    fs->ScanDir(xmlFiles, absDir, "*.xml", SCAN_FILES, false);
                    for (unsigned f = 0; f < xmlFiles.Size(); ++f)
                    {
                        // Verify it's actually a material file by trying to load it
                        String matResPath = matDirs[d] + xmlFiles[f];
                        auto* mat = cache->GetResource<Material>(matResPath, false);
                        if (mat)
                            foundMats.Push(matResPath);
                    }
                }
            }
        }

        if (!foundMats.Empty())
        {
            // Apply found materials: one per geometry, cycling if fewer materials than geometries
            for (unsigned i = 0; i < numGeoms; ++i)
            {
                unsigned matIdx = i < foundMats.Size() ? i : foundMats.Size() - 1;
                auto* mat = cache->GetResource<Material>(foundMats[matIdx]);
                if (mat)
                    staticModelComp_->SetMaterial(i, mat);
            }
            materialsApplied = true;
            URHO3D_LOGINFOF("Applied %d scanned materials to %s", foundMats.Size(), path.CString());
        }
    }

    if (!materialsApplied)
    {
        // Fallback: apply a default lit material so the model isn't invisible
        auto* defaultMat = cache->GetResource<Material>("Materials/DefaultGrey.xml");
        if (!defaultMat)
        {
            // Create a basic grey lit material on the fly
            defaultMat = new Material(context_);
            defaultMat->SetTechnique(0, cache->GetResource<Technique>("Techniques/DiffUnlit.xml"));
            defaultMat->SetShaderParameter("MatDiffColor", Color(0.6f, 0.6f, 0.6f, 1.0f));
        }
        unsigned numGeoms = model->GetNumGeometries();
        for (unsigned i = 0; i < numGeoms; ++i)
            staticModelComp_->SetMaterial(i, defaultMat);
        URHO3D_LOGWARNINGF("No materials found for %s — using default grey", path.CString());
    }

    // Auto-scale oversized models (e.g. FBX exported in centimeters)
    BoundingBox bbox = currentModel_->GetBoundingBox();
    float diagonal = (bbox.max_ - bbox.min_).Length();
    if (diagonal > 50.0f)
    {
        float scale = 10.0f / diagonal;
        modelNode_->SetScale(scale);
        URHO3D_LOGINFOF("Model oversized (diag=%.1f), auto-scaled to %.4f", diagonal, scale);
    }

    AutoFrameCamera();
    RebuildInfoText();
    RebuildAnimList();

    if (statusText_) statusText_->SetText(path);
    if (playbackPanel_) playbackPanel_->SetVisible(isAnimated_);
}

void ModelViewer::AutoFrameCamera()
{
    if (!currentModel_ || !modelNode_) return;

    BoundingBox bbox = currentModel_->GetBoundingBox();
    float scale = modelNode_->GetScale().x_;

    // Work in world space
    Vector3 worldMin = bbox.min_ * scale + modelNode_->GetPosition();
    Vector3 worldMax = bbox.max_ * scale + modelNode_->GetPosition();
    modelCenter_ = (worldMin + worldMax) * 0.5f;
    float diagonal = (worldMax - worldMin).Length();

    URHO3D_LOGINFOF("AutoFrame: scale=%.4f worldDiag=%.2f center=(%f,%f,%f)",
        scale, diagonal, modelCenter_.x_, modelCenter_.y_, modelCenter_.z_);

    if (diagonal < 0.001f) diagonal = 2.0f;

    auto* camera = cameraNode_->GetComponent<Camera>();
    float halfAngle = camera->GetFov() * 0.5f * M_DEGTORAD;
    cameraDistance_ = (diagonal * 0.5f) / tanf(halfAngle) * 1.3f;
    cameraYaw_ = 45.0f;
    cameraPitch_ = 20.0f;

    URHO3D_LOGINFOF("AutoFrame: cameraDistance=%.2f", cameraDistance_);
}

void ModelViewer::ScanAnimations()
{
    availableAnims_.Clear();
    auto* cache = GetSubsystem<ResourceCache>();
    auto* fs = GetSubsystem<FileSystem>();

    // TODO: .animations file support (explicit list, shared anims across skeletons)
    // String animListPath = ReplaceExtension(currentModelPath_, ".animations");
    // if (cache->Exists(animListPath)) { ... read line-by-line, # comments ... }

    // Scan directory for .ani files matching model name prefix
    String modelName = GetFileName(currentModelPath_);
    String modelDir = GetParentPath(currentModelPath_);
    const Vector<String>& resourceDirs = cache->GetResourceDirs();

    for (unsigned d = 0; d < resourceDirs.Size(); ++d)
    {
        String fullDir = resourceDirs[d] + modelDir;
        if (!fs->DirExists(fullDir)) continue;

        Vector<String> files;
        fs->ScanDir(files, fullDir, "*.ani", SCAN_FILES, false);
        for (unsigned f = 0; f < files.Size(); ++f)
        {
            if (GetFileName(files[f]).StartsWith(modelName, false))
            {
                String animPath = modelDir + files[f];
                if (availableAnims_.Find(animPath) == availableAnims_.End())
                    availableAnims_.Push(animPath);
            }
        }
    }

    Sort(availableAnims_.Begin(), availableAnims_.End());

    // Generate a real bind pose animation from skeleton initial transforms
    if (isAnimated_ && animatedModelComp_)
    {
        const Skeleton& skel = animatedModelComp_->GetSkeleton();
        const Vector<Bone>& bones = skel.GetBones();
        if (bones.Size() > 0)
        {
            SharedPtr<Animation> bindPoseAnim(new Animation(context_));
            bindPoseAnim->SetName("[Bind Pose]");
            bindPoseAnim->SetAnimationName("[Bind Pose]");
            bindPoseAnim->SetLength(0.0f);

            for (unsigned i = 0; i < bones.Size(); ++i)
            {
                const Bone& bone = bones[i];
                AnimationTrack* track = bindPoseAnim->CreateTrack(bone.name_);
                track->channelMask_ = AnimationChannels::Position | AnimationChannels::Rotation | AnimationChannels::Scale;

                AnimationKeyFrame kf;
                kf.time_ = 0.0f;
                kf.position_ = bone.initialPosition_;
                kf.rotation_ = bone.initialRotation_;
                kf.scale_ = bone.initialScale_;
                track->AddKeyFrame(kf);
            }

            auto* cache = GetSubsystem<ResourceCache>();
            cache->AddManualResource(bindPoseAnim);
        }
    }
    availableAnims_.Insert(0, "[Bind Pose]");

    if (availableAnims_.Size() > 1)
    {
        // Prefer Idle/Sitting animation over alphabetical first (which is often Die)
        int bestIndex = 1; // skip bind pose
        for (unsigned i = 1; i < availableAnims_.Size(); ++i)
        {
            String name = GetFileName(availableAnims_[i]).ToLower();
            if (name.Contains("idle") || name.Contains("sitting"))
            {
                bestIndex = (int)i;
                break;
            }
        }
        PlayAnimation(bestIndex);
    }
}

void ModelViewer::PlayAnimation(int index)
{
    if (index < 0 || index >= (int)availableAnims_.Size()) return;

    currentAnimIndex_ = index;

    if (animController_)
    {
        animController_->PlayExclusive(availableAnims_[index], 0, animLooped_, 0.2f);
        float spd = animReversed_ ? -animSpeed_ : animSpeed_;
        animController_->SetSpeed(availableAnims_[index], spd);
        animPlaying_ = true;

        // Track as sole active animation on layer 0
        activeAnims_.Clear();
        ActiveAnim aa;
        aa.name = availableAnims_[index];
        aa.layer = 0;
        aa.weight = 1.0f;
        aa.blendMode = ABM_LERP;
        activeAnims_.Push(aa);
        RebuildActiveAnimDisplay();
    }

    if (animNameText_)
        animNameText_->SetText(GetFileName(availableAnims_[index]));

    // Update ListView selection (suppress handler to avoid recursion)
    if (animListView_)
    {
        suppressAnimSelect_ = true;
        animListView_->SetSelection(index);
        suppressAnimSelect_ = false;
    }

    RebuildInfoText();
    RebuildTextKeyList();
}

void ModelViewer::AddBlendAnimation(int index)
{
    if (index < 0 || index >= (int)availableAnims_.Size() || !animController_)
        return;

    const String& animName = availableAnims_[index];

    // Check if already active
    for (unsigned i = 0; i < activeAnims_.Size(); ++i)
    {
        if (activeAnims_[i].name == animName)
        {
            URHO3D_LOGWARNINGF("Animation '%s' already blended on layer %d", GetFileName(animName).CString(), activeAnims_[i].layer);
            return;
        }
    }

    // Find next free layer
    unsigned char nextLayer = 0;
    for (unsigned i = 0; i < activeAnims_.Size(); ++i)
    {
        if (activeAnims_[i].layer >= nextLayer)
            nextLayer = activeAnims_[i].layer + 1;
    }

    animController_->Play(animName, nextLayer, animLooped_, 0.2f);
    float spd = animReversed_ ? -animSpeed_ : animSpeed_;
    animController_->SetSpeed(animName, spd);

    ActiveAnim aa;
    aa.name = animName;
    aa.layer = nextLayer;
    aa.weight = 1.0f;
    aa.blendMode = ABM_LERP;
    activeAnims_.Push(aa);

    URHO3D_LOGINFOF("Blended '%s' on layer %d", GetFileName(animName).CString(), nextLayer);
    RebuildActiveAnimDisplay();
}

void ModelViewer::RemoveBlendAnimation(unsigned index)
{
    if (index >= activeAnims_.Size() || !animController_)
        return;

    animController_->Stop(activeAnims_[index].name, 0.2f);
    activeAnims_.Erase(index);
    RebuildActiveAnimDisplay();
}

void ModelViewer::RebuildActiveAnimDisplay()
{
    if (!activeAnimContainer_) return;

    activeAnimContainer_->RemoveAllChildren();

    for (unsigned i = 0; i < activeAnims_.Size(); ++i)
    {
        ActiveAnim& aa = activeAnims_[i];

        auto* row = activeAnimContainer_->CreateChild<UIElement>();
        row->SetLayout(LM_HORIZONTAL, 4, IntRect(2, 1, 2, 1));
        row->SetFixedHeight(22);

        // Layer label
        auto* layerLbl = row->CreateChild<Text>();
        layerLbl->SetFont(font_, 10);
        layerLbl->SetText("L" + String((int)aa.layer));
        layerLbl->SetColor(Color(0.6f, 0.8f, 0.6f));
        layerLbl->SetMinWidth(22);

        // Animation name
        auto* nameLbl = row->CreateChild<Text>();
        nameLbl->SetFont(font_, 10);
        nameLbl->SetText(GetFileName(aa.name));
        nameLbl->SetColor(Color(0.9f, 0.85f, 0.5f));
        nameLbl->SetMinWidth(80);

        // Weight slider
        auto* wSlider = row->CreateChild<Slider>();
        wSlider->SetStyleAuto();
        wSlider->SetFixedSize(80, 14);
        wSlider->SetRange(1.0f);
        wSlider->SetValue(aa.weight);
        wSlider->SetVar("BlendIdx", (int)i);
        SubscribeToEvent(wSlider, E_SLIDERCHANGED, [this](StringHash, VariantMap& eventData)
        {
            auto* sl = static_cast<Slider*>(eventData[SliderChanged::P_ELEMENT].GetPtr());
            int idx = sl->GetVar("BlendIdx").GetI32();
            if (idx >= 0 && idx < (int)activeAnims_.Size() && animController_)
            {
                activeAnims_[idx].weight = eventData[SliderChanged::P_VALUE].GetFloat();
                animController_->SetWeight(activeAnims_[idx].name, activeAnims_[idx].weight);
            }
        });

        // Weight value label
        auto* wLbl = row->CreateChild<Text>();
        wLbl->SetFont(font_, 10);
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", aa.weight);
        wLbl->SetText(buf);
        wLbl->SetColor(Color(0.7f, 0.7f, 0.7f));
        wLbl->SetMinWidth(28);

        // Additive toggle button
        auto* modeBtn = row->CreateChild<Button>();
        modeBtn->SetStyleAuto();
        modeBtn->SetFixedSize(32, 18);
        auto* modeLbl = modeBtn->CreateChild<Text>();
        modeLbl->SetFont(font_, 9);
        modeLbl->SetText(aa.blendMode == ABM_ADDITIVE ? "Add" : "Lerp");
        modeLbl->SetAlignment(HA_CENTER, VA_CENTER);
        modeBtn->SetVar("BlendIdx", (int)i);
        SubscribeToEvent(modeBtn, E_RELEASED, [this](StringHash, VariantMap& eventData)
        {
            auto* btn = static_cast<Button*>(eventData[Released::P_ELEMENT].GetPtr());
            int idx = btn->GetVar("BlendIdx").GetI32();
            if (idx >= 0 && idx < (int)activeAnims_.Size() && animController_)
            {
                ActiveAnim& a = activeAnims_[idx];
                a.blendMode = (a.blendMode == ABM_LERP) ? ABM_ADDITIVE : ABM_LERP;
                animController_->SetBlendMode(a.name, a.blendMode);
                RebuildActiveAnimDisplay();
            }
        });

        // Remove button
        auto* rmBtn = row->CreateChild<Button>();
        rmBtn->SetStyleAuto();
        rmBtn->SetFixedSize(20, 18);
        auto* rmLbl = rmBtn->CreateChild<Text>();
        rmLbl->SetFont(font_, 10);
        rmLbl->SetText("X");
        rmLbl->SetAlignment(HA_CENTER, VA_CENTER);
        rmBtn->SetVar("BlendIdx", (int)i);
        SubscribeToEvent(rmBtn, E_RELEASED, [this](StringHash, VariantMap& eventData)
        {
            auto* btn = static_cast<Button*>(eventData[Released::P_ELEMENT].GetPtr());
            int idx = btn->GetVar("BlendIdx").GetI32();
            if (idx >= 0 && idx < (int)activeAnims_.Size())
                RemoveBlendAnimation((unsigned)idx);
        });
    }
}

// ============================================================================
// UI
// ============================================================================

void ModelViewer::CreateUI()
{
    CreateMenuBar();
    CreateInfoPanel();
    CreatePlaybackPanel();
    CreateTextKeyPanel();

    auto* ui = GetSubsystem<UI>();

    // Status bar
    statusText_ = ui->GetRoot()->CreateChild<Text>();
    statusText_->SetFont(font_, 14);
    statusText_->SetColor(Color(0.7f, 0.7f, 0.7f));
    statusText_->SetHorizontalAlignment(HA_LEFT);
    statusText_->SetVerticalAlignment(VA_BOTTOM);
    statusText_->SetPosition(8, -8);
    statusText_->SetText("No model -- File > Open or -model <path>");
}

void ModelViewer::CreateMenuBar()
{
    auto* ui = GetSubsystem<UI>();
    auto* graphics = GetSubsystem<Graphics>();

    auto* bar = ui->GetRoot()->CreateChild<BorderImage>("MenuBar");
    bar->SetLayout(LM_HORIZONTAL, 2, IntRect(2, 2, 2, 2));
    bar->SetFixedHeight(28);
    bar->SetFixedWidth(graphics->GetWidth());
    bar->SetHorizontalAlignment(HA_LEFT);
    bar->SetVerticalAlignment(VA_TOP);
    bar->SetColor(Color(0.12f, 0.12f, 0.15f, 0.9f));

    // Helper: create a styled dropdown menu item
    auto makeItem = [this](const String& label) -> Text*
    {
        auto* item = new Text(context_);
        item->SetFont(font_, 12);
        item->SetText(label);
        item->SetStyleAuto();
        item->SetMinWidth(100);
        item->SetMinHeight(22);
        item->SetColor(Color(0.9f, 0.9f, 0.9f));
        return item;
    };

    // Helper: create a placeholder (the always-visible label in the bar)
    auto makePlaceholder = [this](const String& label) -> Text*
    {
        auto* item = new Text(context_);
        item->SetFont(font_, 12);
        item->SetText(label);
        item->SetStyleAuto();
        item->SetColor(Color(0.8f, 0.8f, 0.85f));
        return item;
    };

    // Helper: create a dropdown with consistent styling
    auto makeDropDown = [&](int width) -> DropDownList*
    {
        auto* dd = bar->CreateChild<DropDownList>();
        dd->SetStyleAuto();
        dd->SetFixedSize(width, 24);
        dd->SetResizePopup(true);
        // Darken the dropdown button to match bar
        dd->SetColor(Color(0.18f, 0.18f, 0.22f));
        // Style the popup background so items have a visible container
        auto* popup = dd->GetPopup();
        if (popup)
            popup->SetColor(Color(0.16f, 0.16f, 0.20f, 0.95f));
        return dd;
    };

    // ---- File menu ----
    auto* fileMenu = makeDropDown(120);
    fileMenu->AddItem(makePlaceholder("File"));       // 0
    fileMenu->AddItem(makeItem("Open Model"));        // 1
    fileMenu->AddItem(makeItem("Open Folder"));       // 2
    fileMenu->AddItem(makeItem("Save Model"));        // 3
    fileMenu->AddItem(makeItem("Export FBX"));        // 4
    fileMenu->AddItem(makeItem("Quit"));              // 5
    fileMenu->SetSelection(0);

    SubscribeToEvent(fileMenu, E_ITEMSELECTED, [this, fileMenu](StringHash, VariantMap& eventData)
    {
        int sel = eventData[ItemSelected::P_SELECTION].GetI32();
        if (sel == 1)
        {
            auto* ui = GetSubsystem<UI>();
            auto* style = ui->GetRoot()->GetDefaultStyle();
            auto* fs = GetSubsystem<FileSystem>();

            fileSelector_ = new FileSelector(context_);
            fileSelector_->SetDefaultStyle(style);
            fileSelector_->SetTitle("Open Model");
            fileSelector_->SetButtonTexts("Open", "Cancel");

            Vector<String> filters;
            filters.Push("*.mdl");
            fileSelector_->SetFilters(filters, 0);

            auto* cache = GetSubsystem<ResourceCache>();
            const Vector<String>& dirs = cache->GetResourceDirs();
            String fallback;
            for (unsigned i = 0; i < dirs.Size(); ++i)
            {
                if (fs->DirExists(dirs[i] + "Models/"))
                {
                    if (dirs[i].Contains("/Data/") || dirs[i].EndsWith("/Data"))
                    {
                        fileSelector_->SetPath(dirs[i] + "Models/");
                        fallback.Clear();
                        break;
                    }
                    if (fallback.Empty())
                        fallback = dirs[i] + "Models/";
                }
            }
            if (!fallback.Empty())
                fileSelector_->SetPath(fallback);
            SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(ModelViewer, HandleFileOpen));
        }
        else if (sel == 2)
        {
            // Open Folder — use file selector to pick any .mdl, then scan its parent directory
            auto* ui = GetSubsystem<UI>();
            auto* style = ui->GetRoot()->GetDefaultStyle();
            auto* fs = GetSubsystem<FileSystem>();

            fileSelector_ = new FileSelector(context_);
            fileSelector_->SetDefaultStyle(style);
            fileSelector_->SetTitle("Open Folder (pick any .mdl in target folder)");
            fileSelector_->SetButtonTexts("Open", "Cancel");

            Vector<String> filters;
            filters.Push("*.mdl");
            fileSelector_->SetFilters(filters, 0);

            auto* cache = GetSubsystem<ResourceCache>();
            const Vector<String>& dirs = cache->GetResourceDirs();
            for (unsigned i = 0; i < dirs.Size(); ++i)
            {
                if (fs->DirExists(dirs[i] + "Models/"))
                {
                    fileSelector_->SetPath(dirs[i] + "Models/");
                    break;
                }
            }

            SubscribeToEvent(fileSelector_, E_FILESELECTED, [this](StringHash, VariantMap& ed)
            {
                String path = ed[FileSelected::P_FILENAME].GetString();
                bool ok = ed[FileSelected::P_OK].GetBool();
                fileSelector_.Reset();
                if (!ok || path.Empty()) return;
                ScanFolder(GetPath(path));
            });
        }
        else if (sel == 3)
            SaveModel();
        else if (sel == 4)
            ExportFBX();
        else if (sel == 5)
            engine_->Exit();
        fileMenu->SetSelection(0);
    });

    // ---- View menu ----
    auto* viewMenu = makeDropDown(180);
    viewMenu->AddItem(makePlaceholder("View"));            // 0
    viewMenu->AddItem(makeItem("Wireframe      [F]"));     // 1
    viewMenu->AddItem(makeItem("Skeleton       [S]"));     // 2
    viewMenu->AddItem(makeItem("Bounding Box   [B]"));     // 3
    viewMenu->AddItem(makeItem("Reset Camera   [R]"));     // 4
    viewMenu->AddItem(makeItem("Toggle Info    [Tab]"));   // 5
    viewMenu->AddItem(makeItem("Vertex Editor  [V]"));    // 6
    viewMenu->SetSelection(0);

    SubscribeToEvent(viewMenu, E_ITEMSELECTED, [this, viewMenu](StringHash, VariantMap& eventData)
    {
        int sel = eventData[ItemSelected::P_SELECTION].GetI32();
        if (sel == 1)
        {
            showWireframe_ = !showWireframe_;
            auto* camera = cameraNode_->GetComponent<Camera>();
            if (camera) camera->SetFillMode(showWireframe_ ? FILL_WIREFRAME : FILL_SOLID);
        }
        else if (sel == 2)
            showSkeleton_ = !showSkeleton_;
        else if (sel == 3)
            showBoundingBox_ = !showBoundingBox_;
        else if (sel == 4)
            AutoFrameCamera();
        else if (sel == 5)
        {
            if (infoPanel_) infoPanel_->SetVisible(!infoPanel_->IsVisible());
        }
        else if (sel == 6)
        {
            if (currentModel_)
            {
                if (!vertexEditMode_)
                    EnterVertexEditMode();
                else
                    ExitVertexEditMode();
            }
        }
        viewMenu->SetSelection(0);
    });

    // ---- Help menu ----
    auto* helpMenu = makeDropDown(120);
    helpMenu->AddItem(makePlaceholder("Help"));        // 0
    helpMenu->AddItem(makeItem("Instructions"));       // 1
    helpMenu->SetSelection(0);

    SubscribeToEvent(helpMenu, E_ITEMSELECTED, [this, helpMenu](StringHash, VariantMap& eventData)
    {
        int sel = eventData[ItemSelected::P_SELECTION].GetI32();
        if (sel == 1)
            ShowHelpWindow();
        helpMenu->SetSelection(0);
    });

    // ---- Browse status text (right side of menu bar) ----
    browseStatusText_ = bar->CreateChild<Text>("BrowseStatus");
    browseStatusText_->SetFont(font_, 12);
    browseStatusText_->SetColor(Color(0.7f, 0.85f, 0.7f));
    browseStatusText_->SetText("");
    browseStatusText_->SetHorizontalAlignment(HA_RIGHT);

}

void ModelViewer::CreateInfoPanel()
{
    auto* ui = GetSubsystem<UI>();
    auto* graphics = GetSubsystem<Graphics>();

    // Window container
    infoPanel_ = ui->GetRoot()->CreateChild<Window>("InfoPanel");
    infoPanel_->SetStyleAuto();
    infoPanel_->SetPosition(graphics->GetWidth() - 420, 36);
    infoPanel_->SetSize(400, graphics->GetHeight() - 120);
    infoPanel_->SetMovable(true);
    infoPanel_->SetResizable(true);
    infoPanel_->SetLayout(LM_VERTICAL, 0, IntRect(0, 0, 0, 0));
    infoPanel_->SetOpacity(0.92f);
    infoPanel_->SetColor(Color(0.20f, 0.20f, 0.25f));

    // Title bar — distinct from content, acts as drag handle
    auto* titleBar = infoPanel_->CreateChild<BorderImage>("TitleBar");
    titleBar->SetLayout(LM_HORIZONTAL, 4, IntRect(8, 4, 4, 4));
    titleBar->SetFixedHeight(28);
    titleBar->SetColor(Color(0.14f, 0.14f, 0.18f));

    auto* title = titleBar->CreateChild<Text>();
    title->SetFont(font_, 13);
    title->SetText("Model Info");
    title->SetColor(Color(0.9f, 0.9f, 0.4f));
    title->SetAlignment(HA_LEFT, VA_CENTER);

    // Spacer to push close button right
    auto* spacer = titleBar->CreateChild<UIElement>();
    spacer->SetMinWidth(10);
    spacer->SetLayoutFlexScale(Vector2(1.0f, 0.0f));

    // Close button
    auto* closeBtn = titleBar->CreateChild<Button>();
    closeBtn->SetStyleAuto();
    closeBtn->SetFixedSize(22, 20);
    closeBtn->SetColor(Color(0.4f, 0.15f, 0.15f));
    auto* closeLbl = closeBtn->CreateChild<Text>();
    closeLbl->SetFont(font_, 12);
    closeLbl->SetText("X");
    closeLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(closeBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (infoPanel_) infoPanel_->SetVisible(false);
    });

    // Separator line
    auto* sep = infoPanel_->CreateChild<BorderImage>();
    sep->SetFixedHeight(1);
    sep->SetColor(Color(0.35f, 0.35f, 0.4f));

    // ScrollView for collapsible sections
    infoScroll_ = infoPanel_->CreateChild<ScrollView>();
    infoScroll_->SetStyleAuto();
    infoScroll_->SetMinSize(380, graphics->GetHeight() - 200);

    infoContent_ = infoScroll_->CreateChild<UIElement>();
    infoContent_->SetLayout(LM_VERTICAL, 2, IntRect(6, 4, 6, 4));
    infoContent_->SetMinWidth(370);
    infoScroll_->SetContentElement(infoContent_);

    auto* placeholder = infoContent_->CreateChild<Text>();
    placeholder->SetFont(font_, 11);
    placeholder->SetText("No model loaded.");
    placeholder->SetColor(Color(0.6f, 0.6f, 0.6f));
}

UIElement* ModelViewer::CreateCollapsibleSection(const String& title, bool startExpanded)
{
    if (!infoContent_) return nullptr;

    // Separator between sections
    if (infoContent_->GetNumChildren() > 0)
    {
        auto* sep = infoContent_->CreateChild<BorderImage>();
        sep->SetFixedHeight(1);
        sep->SetColor(Color(0.30f, 0.30f, 0.35f, 0.5f));
    }

    // Header button
    auto* header = infoContent_->CreateChild<Button>();
    header->SetStyleAuto();
    header->SetFixedHeight(24);
    header->SetLayout(LM_HORIZONTAL, 4, IntRect(6, 0, 6, 0));
    header->SetColor(startExpanded ? Color(0.28f, 0.28f, 0.34f) : Color(0.15f, 0.15f, 0.19f));

    auto* headerText = header->CreateChild<Text>();
    headerText->SetFont(font_, 12);
    headerText->SetText(String(startExpanded ? "v " : "> ") + title);
    headerText->SetColor(startExpanded ? Color(0.95f, 0.9f, 0.45f) : Color(0.6f, 0.55f, 0.3f));
    headerText->SetAlignment(HA_LEFT, VA_CENTER);

    // Content container
    auto* content = infoContent_->CreateChild<UIElement>();
    content->SetLayout(LM_VERTICAL, 1, IntRect(8, 2, 4, 4));
    content->SetVisible(startExpanded);

    // Store refs in button vars for toggle
    header->SetVar("SectionContent", content);
    header->SetVar("SectionTitle", title);

    SubscribeToEvent(header, E_RELEASED, [this](StringHash, VariantMap& eventData)
    {
        auto* btn = static_cast<Button*>(eventData[Released::P_ELEMENT].GetPtr());
        auto* content = static_cast<UIElement*>(btn->GetVar("SectionContent").GetPtr());
        if (!content) return;

        bool expanded = !content->IsVisible();
        content->SetVisible(expanded);

        // Depth coloring: expanded = bright (active), collapsed = dark (unreached)
        btn->SetColor(expanded ? Color(0.28f, 0.28f, 0.34f) : Color(0.15f, 0.15f, 0.19f));

        String title = btn->GetVar("SectionTitle").GetString();
        auto* txt = btn->GetChildStaticCast<Text>(0);
        if (txt)
        {
            txt->SetText(String(expanded ? "v " : "> ") + title);
            txt->SetColor(expanded ? Color(0.95f, 0.9f, 0.45f) : Color(0.6f, 0.55f, 0.3f));
        }

        // Force layout refresh
        if (infoContent_)
        {
            infoContent_->SetHeight(0);
            infoContent_->UpdateLayout();
        }
    });

    currentSection_ = content;
    return content;
}

void ModelViewer::AddInfoLine(const String& text, const Color& color)
{
    UIElement* target = currentSection_ ? currentSection_ : infoContent_;
    if (!target) return;

    auto* line = target->CreateChild<Text>();
    line->SetFont(font_, 11);
    line->SetText(text);
    line->SetColor(color);
}

void ModelViewer::AddInfoSeparator()
{
    UIElement* target = currentSection_ ? currentSection_ : infoContent_;
    if (!target) return;

    auto* sep = target->CreateChild<BorderImage>();
    sep->SetFixedHeight(1);
    sep->SetColor(Color(0.35f, 0.35f, 0.4f, 0.4f));
}

void ModelViewer::ShowHelpWindow()
{
    // Toggle - if already visible, close it
    if (helpWindow_ && helpWindow_->IsVisible())
    {
        helpWindow_->SetVisible(false);
        return;
    }

    auto* ui = GetSubsystem<UI>();

    if (!helpWindow_)
    {
        helpWindow_ = ui->GetRoot()->CreateChild<Window>("HelpWindow");
        helpWindow_->SetStyleAuto();
        helpWindow_->SetLayout(LM_VERTICAL, 4, IntRect(12, 8, 12, 8));
        helpWindow_->SetSize(420, 540);
        helpWindow_->SetMovable(true);
        helpWindow_->SetOpacity(0.95f);
        helpWindow_->SetColor(Color(0.30f, 0.30f, 0.36f));
        helpWindow_->SetAlignment(HA_CENTER, VA_CENTER);

        auto* title = helpWindow_->CreateChild<Text>();
        title->SetFont(font_, 15);
        title->SetText("ModelViewer Instructions");
        title->SetColor(Color(0.9f, 0.9f, 0.4f));

        auto addLine = [this](UIElement* parent, const String& text, const Color& color = Color(0.85f, 0.85f, 0.85f))
        {
            auto* line = parent->CreateChild<Text>();
            line->SetFont(font_, 12);
            line->SetText(text);
            line->SetColor(color);
        };

        addLine(helpWindow_, "");
        addLine(helpWindow_, "MOUSE", Color(0.7f, 0.9f, 0.4f));
        addLine(helpWindow_, "  Right-click drag    Orbit camera");
        addLine(helpWindow_, "  Scroll wheel        Zoom in/out");
        addLine(helpWindow_, "  Middle-click drag   Rotate light");
        addLine(helpWindow_, "");
        addLine(helpWindow_, "KEYBOARD", Color(0.7f, 0.9f, 0.4f));
        addLine(helpWindow_, "  F                   Toggle wireframe");
        addLine(helpWindow_, "  S                   Toggle skeleton");
        addLine(helpWindow_, "  B                   Toggle bounding box");
        addLine(helpWindow_, "  R                   Reset camera");
        addLine(helpWindow_, "  Tab                 Toggle info panel");
        addLine(helpWindow_, "  Space               Play / Pause animation");
        addLine(helpWindow_, "  [ / ]               Prev / Next animation");
        addLine(helpWindow_, "  Escape              Quit");
        addLine(helpWindow_, "");
        addLine(helpWindow_, "ANIMATION PANEL", Color(0.7f, 0.9f, 0.4f));
        addLine(helpWindow_, "  Click list item     Switch animation");
        addLine(helpWindow_, "  Loop checkbox       Toggle looping");
        addLine(helpWindow_, "  Fwd/Rev button      Toggle reverse playback");
        addLine(helpWindow_, "  + Blend             Add anim on next layer");
        addLine(helpWindow_, "  Weight slider       Per-layer blend weight");
        addLine(helpWindow_, "  Lerp/Add toggle     Blend mode per layer");
        addLine(helpWindow_, "  X button            Remove blended anim");
        addLine(helpWindow_, "");
        addLine(helpWindow_, "VERTEX EDITOR", Color(0.7f, 0.9f, 0.4f));
        addLine(helpWindow_, "  V                   Toggle vertex edit mode");
        addLine(helpWindow_, "  LMB click           Select nearest vertex");
        addLine(helpWindow_, "  LMB drag            Move selected vertex");
        addLine(helpWindow_, "  DEL                 Delete vertex + triangles");
        addLine(helpWindow_, "  Ctrl+S              Save edited model");
        addLine(helpWindow_, "");

        auto* closeBtn = helpWindow_->CreateChild<Button>();
        closeBtn->SetStyleAuto();
        closeBtn->SetFixedSize(80, 26);
        closeBtn->SetHorizontalAlignment(HA_CENTER);
        auto* closeLbl = closeBtn->CreateChild<Text>();
        closeLbl->SetFont(font_, 13);
        closeLbl->SetText("Close");
        closeLbl->SetAlignment(HA_CENTER, VA_CENTER);
        SubscribeToEvent(closeBtn, E_RELEASED, [this](StringHash, VariantMap&)
        {
            if (helpWindow_) helpWindow_->SetVisible(false);
        });
    }

    helpWindow_->SetVisible(true);
    helpWindow_->BringToFront();
}

void ModelViewer::CreatePlaybackPanel()
{
    auto* ui = GetSubsystem<UI>();
    auto* graphics = GetSubsystem<Graphics>();

    playbackPanel_ = ui->GetRoot()->CreateChild<Window>("PlaybackPanel");
    playbackPanel_->SetStyleAuto();
    playbackPanel_->SetFixedWidth(340);
    playbackPanel_->SetLayout(LM_VERTICAL, 4, IntRect(8, 6, 8, 6));
    playbackPanel_->SetPosition(8, 36);
    playbackPanel_->SetMovable(true);
    playbackPanel_->SetOpacity(0.92f);
    playbackPanel_->SetColor(Color(0.20f, 0.20f, 0.25f));
    playbackPanel_->SetVisible(false);

    // Title
    auto* title = playbackPanel_->CreateChild<Text>();
    title->SetFont(font_, 13);
    title->SetText("Animation");
    title->SetColor(Color(0.9f, 0.9f, 0.4f));

    // Animation list
    animListView_ = playbackPanel_->CreateChild<ListView>();
    animListView_->SetStyleAuto();
    animListView_->SetHighlightMode(HM_ALWAYS);
    animListView_->SetMinHeight(120);
    animListView_->SetMaxHeight(200);
    SubscribeToEvent(animListView_, E_ITEMSELECTED, URHO3D_HANDLER(ModelViewer, HandleAnimSelected));

    // Current animation name
    animNameText_ = playbackPanel_->CreateChild<Text>();
    animNameText_->SetFont(font_, 12);
    animNameText_->SetColor(Color(0.9f, 0.7f, 0.3f));
    animNameText_->SetText("(none)");

    // Button row: Prev | Play/Pause | Next
    auto* btnRow = playbackPanel_->CreateChild<UIElement>();
    btnRow->SetLayout(LM_HORIZONTAL, 4);
    btnRow->SetFixedHeight(28);

    auto* prevBtn = btnRow->CreateChild<Button>();
    prevBtn->SetStyleAuto();
    prevBtn->SetFixedSize(70, 26);
    auto* prevLbl = prevBtn->CreateChild<Text>();
    prevLbl->SetFont(font_, 12);
    prevLbl->SetText("<< Prev");
    prevLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(prevBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (availableAnims_.Empty()) return;
        int idx = currentAnimIndex_ - 1;
        if (idx < 0) idx = (int)availableAnims_.Size() - 1;
        PlayAnimation(idx);
    });

    auto* playBtn = btnRow->CreateChild<Button>();
    playBtn->SetStyleAuto();
    playBtn->SetFixedSize(90, 26);
    auto* playLbl = playBtn->CreateChild<Text>();
    playLbl->SetFont(font_, 12);
    playLbl->SetText("Play/Pause");
    playLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(playBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (!animController_ || currentAnimIndex_ < 0) return;
        animPlaying_ = !animPlaying_;
        float spd = animReversed_ ? -animSpeed_ : animSpeed_;
        animController_->SetSpeed(availableAnims_[currentAnimIndex_], animPlaying_ ? spd : 0.0f);
    });

    auto* nextBtn = btnRow->CreateChild<Button>();
    nextBtn->SetStyleAuto();
    nextBtn->SetFixedSize(70, 26);
    auto* nextLbl = nextBtn->CreateChild<Text>();
    nextLbl->SetFont(font_, 12);
    nextLbl->SetText("Next >>");
    nextLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(nextBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (availableAnims_.Empty()) return;
        int idx = (currentAnimIndex_ + 1) % (int)availableAnims_.Size();
        PlayAnimation(idx);
    });

    // Animation time
    animTimeText_ = playbackPanel_->CreateChild<Text>();
    animTimeText_->SetFont(font_, 12);
    animTimeText_->SetColor(Color(0.9f, 0.9f, 0.5f));
    animTimeText_->SetText("Time: 0.00 / 0.00");

    animSlider_ = playbackPanel_->CreateChild<Slider>();
    animSlider_->SetStyleAuto();
    animSlider_->SetFixedHeight(18);
    animSlider_->SetRange(1.0f);
    SubscribeToEvent(animSlider_, E_SLIDERCHANGED, URHO3D_HANDLER(ModelViewer, HandleAnimSlider));

    // Speed
    animSpeedText_ = playbackPanel_->CreateChild<Text>();
    animSpeedText_->SetFont(font_, 12);
    animSpeedText_->SetColor(Color(0.7f, 0.9f, 0.7f));
    animSpeedText_->SetText("Speed: 1.0x");

    speedSlider_ = playbackPanel_->CreateChild<Slider>();
    speedSlider_->SetStyleAuto();
    speedSlider_->SetFixedHeight(18);
    speedSlider_->SetRange(1.0f);
    speedSlider_->SetValue(0.31f);
    SubscribeToEvent(speedSlider_, E_SLIDERCHANGED, URHO3D_HANDLER(ModelViewer, HandleSpeedSlider));

    // --- Options row: Loop, Reverse ---
    auto* optRow = playbackPanel_->CreateChild<UIElement>();
    optRow->SetLayout(LM_HORIZONTAL, 8);
    optRow->SetFixedHeight(24);

    // Loop checkbox
    auto* loopRow = optRow->CreateChild<UIElement>();
    loopRow->SetLayout(LM_HORIZONTAL, 4);
    loopCheck_ = loopRow->CreateChild<CheckBox>();
    loopCheck_->SetStyleAuto();
    loopCheck_->SetChecked(true);
    auto* loopLbl = loopRow->CreateChild<Text>();
    loopLbl->SetFont(font_, 12);
    loopLbl->SetText("Loop");
    loopLbl->SetColor(Color(0.85f, 0.85f, 0.85f));
    SubscribeToEvent(loopCheck_, E_TOGGLED, [this](StringHash, VariantMap&)
    {
        animLooped_ = loopCheck_->IsChecked();
        if (animController_ && currentAnimIndex_ >= 0)
            animController_->SetLooped(availableAnims_[currentAnimIndex_], animLooped_);
    });

    // Reverse button
    auto* revBtn = optRow->CreateChild<Button>();
    revBtn->SetStyleAuto();
    revBtn->SetFixedSize(55, 22);
    reverseLabel_ = revBtn->CreateChild<Text>();
    reverseLabel_->SetFont(font_, 11);
    reverseLabel_->SetText("Fwd");
    reverseLabel_->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(revBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        animReversed_ = !animReversed_;
        if (reverseLabel_) reverseLabel_->SetText(animReversed_ ? "Rev" : "Fwd");
        if (animController_ && currentAnimIndex_ >= 0 && animPlaying_)
        {
            float spd = animReversed_ ? -animSpeed_ : animSpeed_;
            animController_->SetSpeed(availableAnims_[currentAnimIndex_], spd);
        }
    });

    // --- Blend button + active animations ---
    auto* blendRow = playbackPanel_->CreateChild<UIElement>();
    blendRow->SetLayout(LM_HORIZONTAL, 4);
    blendRow->SetFixedHeight(26);

    auto* blendBtn = blendRow->CreateChild<Button>();
    blendBtn->SetStyleAuto();
    blendBtn->SetFixedSize(90, 24);
    auto* blendLbl = blendBtn->CreateChild<Text>();
    blendLbl->SetFont(font_, 12);
    blendLbl->SetText("+ Blend");
    blendLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(blendBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (animListView_)
        {
            int sel = animListView_->GetSelection();
            if (sel >= 0 && sel < (int)availableAnims_.Size())
                AddBlendAnimation(sel);
        }
    });

    auto* clearBtn = blendRow->CreateChild<Button>();
    clearBtn->SetStyleAuto();
    clearBtn->SetFixedSize(90, 24);
    auto* clearLbl = clearBtn->CreateChild<Text>();
    clearLbl->SetFont(font_, 12);
    clearLbl->SetText("Clear All");
    clearLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(clearBtn, E_RELEASED, [this](StringHash, VariantMap&)
    {
        if (animController_) animController_->StopAll(0.2f);
        activeAnims_.Clear();
        RebuildActiveAnimDisplay();
    });

    // Active animations container
    activeAnimContainer_ = playbackPanel_->CreateChild<UIElement>();
    activeAnimContainer_->SetLayout(LM_VERTICAL, 2);
}

// ============================================================================
// Text Key Panel
// ============================================================================

void ModelViewer::CreateTextKeyPanel()
{
    auto* ui = GetSubsystem<UI>();

    // Text key section sits inside the playback panel
    auto* separator = playbackPanel_->CreateChild<UIElement>();
    separator->SetFixedHeight(2);
    separator->SetLayoutMode(LM_FREE);
    auto* sepLine = separator->CreateChild<BorderImage>();
    sepLine->SetColor(Color(0.5f, 0.5f, 0.5f, 0.4f));
    sepLine->SetFixedHeight(1);
    sepLine->SetFixedWidth(320);

    textKeyTitle_ = playbackPanel_->CreateChild<Text>();
    textKeyTitle_->SetFont(font_, 12);
    textKeyTitle_->SetColor(Color(0.5f, 0.9f, 0.9f));
    textKeyTitle_->SetText("Text Keys (0)");

    // Flash text — shows key name when fired during playback
    textKeyFlash_ = playbackPanel_->CreateChild<Text>();
    textKeyFlash_->SetFont(font_, 14);
    textKeyFlash_->SetColor(Color(1.0f, 1.0f, 0.3f));
    textKeyFlash_->SetText("");

    // Scrollable container for text key list
    textKeyContainer_ = playbackPanel_->CreateChild<UIElement>();
    textKeyContainer_->SetLayout(LM_VERTICAL, 2);
    textKeyContainer_->SetMaxHeight(150);

    // Add key controls: name + data line edits
    auto* addRow = playbackPanel_->CreateChild<UIElement>();
    addRow->SetLayout(LM_HORIZONTAL, 4);
    addRow->SetFixedHeight(24);

    auto* nameLbl = addRow->CreateChild<Text>();
    nameLbl->SetFont(font_, 11);
    nameLbl->SetText("Name:");
    nameLbl->SetColor(Color(0.7f, 0.7f, 0.7f));
    nameLbl->SetFixedWidth(40);

    textKeyNameEdit_ = addRow->CreateChild<LineEdit>();
    textKeyNameEdit_->SetStyleAuto();
    textKeyNameEdit_->SetFixedHeight(22);
    textKeyNameEdit_->SetMinWidth(120);
    textKeyNameEdit_->SetText("FootDown");

    auto* dataRow = playbackPanel_->CreateChild<UIElement>();
    dataRow->SetLayout(LM_HORIZONTAL, 4);
    dataRow->SetFixedHeight(24);

    auto* dataLbl = dataRow->CreateChild<Text>();
    dataLbl->SetFont(font_, 11);
    dataLbl->SetText("Data:");
    dataLbl->SetColor(Color(0.7f, 0.7f, 0.7f));
    dataLbl->SetFixedWidth(40);

    textKeyDataEdit_ = dataRow->CreateChild<LineEdit>();
    textKeyDataEdit_->SetStyleAuto();
    textKeyDataEdit_->SetFixedHeight(22);
    textKeyDataEdit_->SetMinWidth(120);

    // Button row: Add | Delete | Save
    auto* keyBtnRow = playbackPanel_->CreateChild<UIElement>();
    keyBtnRow->SetLayout(LM_HORIZONTAL, 4);
    keyBtnRow->SetFixedHeight(26);

    auto* addBtn = keyBtnRow->CreateChild<Button>();
    addBtn->SetStyleAuto();
    addBtn->SetFixedSize(70, 24);
    auto* addLbl = addBtn->CreateChild<Text>();
    addLbl->SetFont(font_, 11);
    addLbl->SetText("+ Add");
    addLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(addBtn, E_RELEASED, [this](StringHash, VariantMap&) { AddTextKeyAtCurrentTime(); });

    auto* delBtn = keyBtnRow->CreateChild<Button>();
    delBtn->SetStyleAuto();
    delBtn->SetFixedSize(70, 24);
    auto* delLbl = delBtn->CreateChild<Text>();
    delLbl->SetFont(font_, 11);
    delLbl->SetText("Delete");
    delLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(delBtn, E_RELEASED, [this](StringHash, VariantMap&) { DeleteSelectedTextKey(); });

    auto* saveBtn = keyBtnRow->CreateChild<Button>();
    saveBtn->SetStyleAuto();
    saveBtn->SetFixedSize(90, 24);
    auto* saveLbl = saveBtn->CreateChild<Text>();
    saveLbl->SetFont(font_, 11);
    saveLbl->SetText("Save Keys");
    saveLbl->SetAlignment(HA_CENTER, VA_CENTER);
    SubscribeToEvent(saveBtn, E_RELEASED, [this](StringHash, VariantMap&) { SaveTextKeys(); });

    // Subscribe to text key events from AnimationState
    SubscribeToEvent(E_ANIMATIONTEXTKEY, URHO3D_HANDLER(ModelViewer, HandleTextKeyEvent));
}

void ModelViewer::RebuildTextKeyList()
{
    if (!textKeyContainer_) return;

    textKeyContainer_->RemoveAllChildren();
    textKeyButtons_.Clear();
    selectedTextKey_ = -1;

    // Find the current animation resource
    Animation* anim = nullptr;
    if (animController_ && currentAnimIndex_ >= 0 && currentAnimIndex_ < (int)availableAnims_.Size())
    {
        auto* cache = GetSubsystem<ResourceCache>();
        anim = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_], false);
    }

    if (!anim || anim->GetNumTextKeys() == 0)
    {
        if (textKeyTitle_)
            textKeyTitle_->SetText("Text Keys (0)");
        return;
    }

    if (textKeyTitle_)
        textKeyTitle_->SetText("Text Keys (" + String(anim->GetNumTextKeys()) + ")");

    const Vector<AnimationTextKey>& keys = anim->GetTextKeys();
    for (int i = 0; i < (int)keys.Size(); ++i)
    {
        auto* btn = textKeyContainer_->CreateChild<Button>();
        btn->SetStyleAuto();
        btn->SetFixedHeight(20);
        btn->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 1, 4, 1));

        auto* timeText = btn->CreateChild<Text>();
        timeText->SetFont(font_, 11);
        timeText->SetText(String((double)keys[i].time_, 3) + "s");
        timeText->SetColor(Color(0.9f, 0.9f, 0.5f));
        timeText->SetFixedWidth(55);

        auto* nameText = btn->CreateChild<Text>();
        nameText->SetFont(font_, 11);
        nameText->SetText(keys[i].name_);
        nameText->SetColor(Color(0.5f, 0.9f, 0.9f));

        if (!keys[i].data_.IsEmpty())
        {
            auto* dataText = btn->CreateChild<Text>();
            dataText->SetFont(font_, 10);
            dataText->SetText("(" + keys[i].data_.ToString() + ")");
            dataText->SetColor(Color(0.7f, 0.7f, 0.7f));
        }

        int keyIndex = i;
        SubscribeToEvent(btn, E_RELEASED, [this, keyIndex](StringHash, VariantMap&)
        {
            selectedTextKey_ = keyIndex;
            // Highlight selected
            for (int j = 0; j < (int)textKeyButtons_.Size(); ++j)
            {
                if (textKeyButtons_[j])
                    textKeyButtons_[j]->SetColor(j == selectedTextKey_ ? Color(0.3f, 0.4f, 0.5f) : Color::WHITE);
            }
            // Scrub to key time
            if (animController_ && currentAnimIndex_ >= 0)
            {
                auto* cache = GetSubsystem<ResourceCache>();
                Animation* a = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_], false);
                if (a)
                {
                    AnimationTextKey* k = a->GetTextKey(keyIndex);
                    if (k)
                    {
                        animController_->SetTime(availableAnims_[currentAnimIndex_], k->time_);
                        // Populate edit fields
                        if (textKeyNameEdit_) textKeyNameEdit_->SetText(k->name_);
                        if (textKeyDataEdit_) textKeyDataEdit_->SetText(k->data_.IsEmpty() ? "" : k->data_.ToString());
                    }
                }
            }
        });

        textKeyButtons_.Push(btn);
    }
}

void ModelViewer::AddTextKeyAtCurrentTime()
{
    if (!animController_ || currentAnimIndex_ < 0) return;

    auto* cache = GetSubsystem<ResourceCache>();
    Animation* anim = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_], false);
    if (!anim) return;

    float t = animController_->GetTime(availableAnims_[currentAnimIndex_]);
    String name = textKeyNameEdit_ ? textKeyNameEdit_->GetText().Trimmed() : "Key";
    if (name.Empty()) name = "Key";

    String dataStr = textKeyDataEdit_ ? textKeyDataEdit_->GetText().Trimmed() : "";
    Variant data = dataStr.Empty() ? Variant::EMPTY : Variant(dataStr);

    anim->AddTextKey(t, false, name, data);
    URHO3D_LOGINFOF("Added text key '%s' at %.3fs", name.CString(), t);

    RebuildTextKeyList();
}

void ModelViewer::DeleteSelectedTextKey()
{
    if (selectedTextKey_ < 0) return;
    if (!animController_ || currentAnimIndex_ < 0) return;

    auto* cache = GetSubsystem<ResourceCache>();
    Animation* anim = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_], false);
    if (!anim) return;

    AnimationTextKey* key = anim->GetTextKey(selectedTextKey_);
    if (key)
        URHO3D_LOGINFOF("Deleted text key '%s' at %.3fs", key->name_.CString(), key->time_);

    anim->RemoveTextKey(selectedTextKey_);
    selectedTextKey_ = -1;
    RebuildTextKeyList();
}

void ModelViewer::SaveTextKeys()
{
    if (!animController_ || currentAnimIndex_ < 0) return;

    auto* cache = GetSubsystem<ResourceCache>();
    Animation* anim = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_], false);
    if (!anim) return;

    const Vector<AnimationTextKey>& keys = anim->GetTextKeys();
    if (keys.Empty())
    {
        URHO3D_LOGINFO("No text keys to save");
        return;
    }

    // Build JSON sidecar path from the animation resource name
    // Find the actual file path on disk
    String animPath = availableAnims_[currentAnimIndex_];
    String jsonPath = ReplaceExtension(animPath, ".json");

    // Try to resolve to an absolute path
    auto* fs = GetSubsystem<FileSystem>();
    String absPath;

    // Check if animPath is already absolute
    if (fs->FileExists(animPath))
        absPath = ReplaceExtension(animPath, ".json");
    else
    {
        // Try resource dirs
        const Vector<String>& resourceDirs = cache->GetResourceDirs();
        for (const String& dir : resourceDirs)
        {
            if (fs->FileExists(dir + animPath))
            {
                absPath = dir + ReplaceExtension(animPath, ".json");
                break;
            }
        }
    }

    if (absPath.Empty())
    {
        URHO3D_LOGERRORF("Cannot resolve path for: %s", animPath.CString());
        return;
    }

    // Build JSON
    JSONFile jsonFile(context_);
    JSONValue& root = jsonFile.GetRoot();
    JSONArray emptyArr;
    JSONValue keyArray(emptyArr);

    for (const AnimationTextKey& key : keys)
    {
        JSONValue entry;
        entry.Set("name", JSONValue(key.name_));
        entry.Set("time", JSONValue((double)key.time_));
        if (!key.data_.IsEmpty())
            entry.Set("data", JSONValue(key.data_.ToString()));
        keyArray.Push(entry);
    }

    root.Set("textkeys", keyArray);

    File outFile(context_, absPath, FILE_WRITE);
    if (outFile.IsOpen())
    {
        jsonFile.Save(outFile, "  ");
        URHO3D_LOGINFOF("Saved %d text keys to %s", keys.Size(), absPath.CString());
    }
    else
    {
        URHO3D_LOGERRORF("Failed to write: %s", absPath.CString());
    }
}

void ModelViewer::HandleTextKeyEvent(StringHash, VariantMap& eventData)
{
    using namespace AnimationTextKeyEvent;
    String name = eventData[P_NAME].GetString();
    float time = eventData[P_TIME].GetFloat();

    // Flash the key name on screen
    if (textKeyFlash_)
    {
        textKeyFlash_->SetText(String((double)time, 3) + "s  " + name);
        textKeyFlashTimer_ = 0.8f;
    }
}

// ============================================================================
// Info Text — rebuild as ListView items
// ============================================================================

void ModelViewer::RebuildInfoText()
{
    if (!infoContent_ || !currentModel_) return;

    infoContent_->RemoveAllChildren();
    currentSection_ = nullptr;

    auto* cache = GetSubsystem<ResourceCache>();

    // ---- Assets (expanded) ----
    CreateCollapsibleSection("Assets", true);
    AddInfoLine("Model: " + currentModelPath_);

    String matListPath = ReplaceExtension(currentModelPath_, ".txt");
    if (cache->Exists(matListPath))
        AddInfoLine("MatList: " + matListPath);

    if (staticModelComp_ && staticModelComp_->GetNumGeometries() > 0)
    {
        AddInfoSeparator();
        for (unsigned g = 0; g < staticModelComp_->GetNumGeometries(); ++g)
        {
            Material* mat = staticModelComp_->GetMaterial(g);
            if (!mat) continue;
            AddInfoLine("Material[" + String(g) + "]: " + mat->GetName());
            for (int u = 0; u < MAX_TEXTURE_UNITS; ++u)
            {
                Texture* tex = mat->GetTexture((TextureUnit)u);
                if (tex)
                    AddInfoLine("  Texture: " + tex->GetName());
            }
        }
    }

    if (!availableAnims_.Empty())
    {
        AddInfoSeparator();
        for (unsigned i = 0; i < availableAnims_.Size(); ++i)
            AddInfoLine("Anim: " + GetFileName(availableAnims_[i]));
    }

    // ---- Model Summary (expanded) ----
    CreateCollapsibleSection("Model Summary", true);
    AddInfoLine("Path: " + currentModelPath_);
    AddInfoLine("Type: " + String(isAnimated_ ? "AnimatedModel" : "StaticModel"));

    BoundingBox bbox = currentModel_->GetBoundingBox();
    Vector3 dims = bbox.max_ - bbox.min_;
    AddInfoLine("Size: " + String((double)dims.x_, 2) + " x " + String((double)dims.y_, 2) + " x " + String((double)dims.z_, 2));

    int totalVerts = 0, totalIndices = 0;
    for (int g = 0; g < currentModel_->GetNumGeometries(); ++g)
    {
        Geometry* geom = currentModel_->GetGeometry(g, 0);
        if (geom) { totalVerts += geom->GetVertexCount(); totalIndices += geom->GetIndexCount(); }
    }

    AddInfoLine("Geometries: " + String(currentModel_->GetNumGeometries()));
    AddInfoLine("Vertices: " + String(totalVerts) + "  Triangles: " + String(totalIndices / 3));

    Skeleton& skel = currentModel_->GetSkeleton();
    AddInfoLine("Bones: " + String(skel.GetNumBones()) + "  Morphs: " + String(currentModel_->GetNumMorphs()));

    if (modelNode_->GetScale().x_ < 0.99f)
    {
        AddInfoLine("Auto-scale: " + String((double)modelNode_->GetScale().x_, 4) + " (oversized model)", Color(0.9f, 0.6f, 0.3f));
    }

    // ---- Geometry (collapsed) ----
    for (int g = 0; g < currentModel_->GetNumGeometries(); ++g)
    {
        CreateCollapsibleSection("Geometry " + String(g), false);
        int numLods = currentModel_->GetNumGeometryLodLevels(g);
        AddInfoLine("LOD levels: " + String(numLods));

        for (int l = 0; l < numLods; ++l)
        {
            Geometry* geom = currentModel_->GetGeometry(g, l);
            if (!geom) continue;

            if (numLods > 1)
                AddInfoLine("LOD " + String(l) + " (dist=" + String((double)geom->GetLodDistance(), 1) + ")");

            AddInfoLine("Verts: " + String(geom->GetVertexCount()) +
                "  Idx: " + String(geom->GetIndexCount()) +
                "  Tris: " + String(geom->GetIndexCount() / 3));

            for (int vb = 0; vb < geom->GetNumVertexBuffers(); ++vb)
            {
                VertexBuffer* vbuf = geom->GetVertexBuffer(vb);
                if (!vbuf) continue;

                AddInfoLine("VB" + String(vb) + ": " + String(vbuf->GetVertexSize()) + " bytes/vert");

                const Vector<VertexElement>& elems = vbuf->GetElements();
                for (unsigned e = 0; e < elems.Size(); ++e)
                {
                    const VertexElement& el = elems[e];
                    String s = "  " + SemanticName(el.semantic_);
                    if (el.index_ > 0) s += String((int)el.index_);
                    s += ": " + TypeName(el.type_) + " @ " + String(el.offset_);
                    AddInfoLine(s);
                }
            }

            IndexBuffer* ib = geom->GetIndexBuffer();
            if (ib)
                AddInfoLine("IB: " + String(ib->GetIndexCount()) + " idx, " + String(ib->GetIndexSize()) + " bytes/idx");
        }
    }

    // ---- Skeleton (collapsed) — clickable bone buttons ----
    boneButtons_.Clear();
    if (skel.GetNumBones() > 1)
    {
        CreateCollapsibleSection("Skeleton (" + String(skel.GetNumBones()) + " bones)", false);
        const Vector<Bone>& bones = skel.GetBones();
        for (unsigned i = 0; i < bones.Size(); ++i)
        {
            const Bone& bone = bones[i];
            int depth = 0;
            int p = bone.parentIndex_;
            while (p > 0 && depth < 10) { p = bones[p].parentIndex_; depth++; }

            String indent;
            for (int d = 0; d < depth; ++d) indent += "  ";

            UIElement* target = currentSection_ ? currentSection_ : infoContent_;
            if (!target) continue;

            auto* btn = target->CreateChild<Button>();
            btn->SetStyleAuto();
            btn->SetFixedHeight(18);
            btn->SetLayout(LM_HORIZONTAL, 0, IntRect(2, 0, 2, 0));
            btn->SetColor(Color(0.18f, 0.18f, 0.22f, 0.6f));
            btn->SetVar("BoneIndex", (int)i);

            auto* label = btn->CreateChild<Text>();
            label->SetFont(font_, 11);
            label->SetText(indent + bone.name_ + "  r=" + String((double)bone.radius_, 3));
            label->SetColor(Color(0.85f, 0.85f, 0.85f));
            label->SetAlignment(HA_LEFT, VA_CENTER);

            SubscribeToEvent(btn, E_RELEASED, URHO3D_HANDLER(ModelViewer, HandleBoneListClick));
            boneButtons_.Push(btn);
        }
    }

    // ---- Animation Inspector (collapsed) ----
    if (currentAnimIndex_ >= 0 && currentAnimIndex_ < (int)availableAnims_.Size())
    {
        auto* anim = cache->GetResource<Animation>(availableAnims_[currentAnimIndex_]);
        if (anim)
        {
            CreateCollapsibleSection("Animation Inspector", false);
            AddInfoLine("Name: " + anim->GetAnimationName());
            AddInfoLine("Length: " + String(anim->GetLength(), 3) + "s");
            AddInfoLine("Tracks: " + String(anim->GetNumTracks()));

            const Vector<AnimationTriggerPoint>& triggers = anim->GetTriggers();
            if (!triggers.Empty())
            {
                AddInfoLine("Triggers: " + String(triggers.Size()));
                for (unsigned tr = 0; tr < triggers.Size(); ++tr)
                    AddInfoLine("  @" + String((double)triggers[tr].time_, 3) + "s: " + triggers[tr].data_.ToString());
            }

            const HashMap<StringHash, AnimationTrack>& tracks = anim->GetTracks();
            int totalKeys = 0;
            for (HashMap<StringHash, AnimationTrack>::ConstIterator it = tracks.Begin(); it != tracks.End(); ++it)
            {
                const AnimationTrack& track = it->second_;
                totalKeys += track.GetNumKeyFrames();

                String ch;
                if ((track.channelMask_ & AnimationChannels::Position) != AnimationChannels::None) ch += "P";
                if ((track.channelMask_ & AnimationChannels::Rotation) != AnimationChannels::None) ch += "R";
                if ((track.channelMask_ & AnimationChannels::Scale) != AnimationChannels::None) ch += "S";

                String s = "  " + track.name_ + " [" + ch + "] " + String(track.GetNumKeyFrames()) + " keys";

                if (track.GetNumKeyFrames() > 0)
                {
                    float t0 = track.keyFrames_[0].time_;
                    float t1 = track.keyFrames_[track.keyFrames_.Size() - 1].time_;
                    s += "  t:" + String((double)t0, 2) + "->" + String((double)t1, 2);
                }
                AddInfoLine(s);
            }
            AddInfoLine("Total keyframes: " + String(totalKeys));
        }
    }

    // ---- Animation Catalogue (collapsed) ----
    if (availableAnims_.Size() > 1) // >1 because [Bind Pose] is always there
    {
        CreateCollapsibleSection("Animation Catalogue (" + String(availableAnims_.Size() - 1) + " anims)", false);

        // Check for essential animation types
        const char* essentials[] = {"idle", "walk", "run", "eat", "die", "attack"};
        bool found[6] = {};
        for (unsigned i = 1; i < availableAnims_.Size(); ++i) // skip bind pose
        {
            String lower = GetFileName(availableAnims_[i]).ToLower();
            for (int e = 0; e < 6; ++e)
                if (lower.Contains(essentials[e])) found[e] = true;
        }

        String missing;
        for (int e = 0; e < 6; ++e)
            if (!found[e]) { if (!missing.Empty()) missing += ", "; missing += essentials[e]; }
        if (!missing.Empty())
            AddInfoLine("MISSING: " + missing, Color(1.0f, 0.4f, 0.4f));
        else
            AddInfoLine("All essentials present", Color(0.4f, 1.0f, 0.4f));

        AddInfoSeparator();

        // List each animation with details
        for (unsigned i = 1; i < availableAnims_.Size(); ++i)
        {
            auto* anim = cache->GetResource<Animation>(availableAnims_[i]);
            if (!anim) continue;

            String name = GetFileName(availableAnims_[i]);
            float duration = anim->GetLength();
            unsigned numTracks = anim->GetNumTracks();

            unsigned totalKeys = 0;
            const HashMap<StringHash, AnimationTrack>& tracks = anim->GetTracks();
            for (HashMap<StringHash, AnimationTrack>::ConstIterator it = tracks.Begin(); it != tracks.End(); ++it)
                totalKeys += it->second_.GetNumKeyFrames();

            String line = name + "  " + String((double)duration, 2) + "s  " +
                String(numTracks) + "tr  " + String(totalKeys) + "kf";
            AddInfoLine(line, Color(0.85f, 0.85f, 0.65f));
        }
    }

    // ---- Materials (collapsed) ----
    if (staticModelComp_)
    {
        CreateCollapsibleSection("Materials", false);
        for (unsigned g = 0; g < staticModelComp_->GetNumGeometries(); ++g)
        {
            Material* mat = staticModelComp_->GetMaterial(g);
            if (!mat) { AddInfoLine("Geom " + String(g) + ": <none>"); continue; }

            AddInfoLine("Geom " + String(g) + ": " + mat->GetName(), Color(0.85f, 0.85f, 0.5f));

            for (unsigned tc = 0; tc < (unsigned)mat->GetNumTechniques(); ++tc)
            {
                Technique* tech = mat->GetTechnique(tc);
                if (tech) AddInfoLine("  Tech: " + tech->GetName());
            }

            const String unitNames[] = {"diffuse","normal","specular","emissive","environment","volume","custom1","custom2"};
            for (int u = 0; u < MAX_TEXTURE_UNITS; ++u)
            {
                Texture* tex = mat->GetTexture((TextureUnit)u);
                if (tex)
                {
                    String uname = (u < 8) ? unitNames[u] : String(u);
                    AddInfoLine("  " + uname + ": " + tex->GetName());
                }
            }

            const HashMap<StringHash, MaterialShaderParameter>& params = mat->GetShaderParameters();
            for (HashMap<StringHash, MaterialShaderParameter>::ConstIterator it = params.Begin(); it != params.End(); ++it)
                AddInfoLine("  " + it->second_.name_ + " = " + it->second_.value_.ToString());

            if (!mat->GetVertexShaderDefines().Empty())
                AddInfoLine("  VS: " + mat->GetVertexShaderDefines());
            if (!mat->GetPixelShaderDefines().Empty())
                AddInfoLine("  PS: " + mat->GetPixelShaderDefines());
        }
    }

    // ---- Morph Targets (collapsed) ----
    if (animatedModelComp_ && animatedModelComp_->GetNumMorphs() > 0)
    {
        CreateCollapsibleSection("Morph Targets", false);
        const Vector<ModelMorph>& morphs = currentModel_->GetMorphs();
        for (unsigned i = 0; i < morphs.Size(); ++i)
            AddInfoLine(morphs[i].name_ + ": " + String((double)animatedModelComp_->GetMorphWeight(i), 2));
    }

    currentSection_ = nullptr;
}

void ModelViewer::RebuildAnimList()
{
    if (!animListView_) return;

    animListView_->RemoveAllItems();

    auto* cache = GetSubsystem<ResourceCache>();
    for (unsigned i = 0; i < availableAnims_.Size(); ++i)
    {
        auto* item = new Text(context_);
        item->SetFont(font_, 11);

        String label = GetFileName(availableAnims_[i]);
        auto* anim = cache->GetResource<Animation>(availableAnims_[i]);
        if (anim && anim->GetLength() > 0.0f)
            label += "  (" + String((double)anim->GetLength(), 2) + "s)";

        item->SetText(label);
        item->SetStyleAuto();
        animListView_->AddItem(item);
    }

    if (currentAnimIndex_ >= 0)
        animListView_->SetSelection(currentAnimIndex_);
}

// ============================================================================
// Camera
// ============================================================================

void ModelViewer::UpdateCamera(float)
{
    float yawRad = cameraYaw_ * M_DEGTORAD;
    float pitchRad = cameraPitch_ * M_DEGTORAD;

    float x = modelCenter_.x_ + cameraDistance_ * cosf(pitchRad) * sinf(yawRad);
    float y = modelCenter_.y_ + cameraDistance_ * sinf(pitchRad);
    float z = modelCenter_.z_ + cameraDistance_ * cosf(pitchRad) * cosf(yawRad);

    cameraNode_->SetPosition(Vector3(x, y, z));
    cameraNode_->LookAt(modelCenter_);
}

// ============================================================================
// Events
// ============================================================================

void ModelViewer::HandleUpdate(StringHash, VariantMap& eventData)
{
    float timeStep = eventData[Update::P_TIMESTEP].GetFloat();
    UpdateCamera(timeStep);

    auto* input = GetSubsystem<Input>();

    // Keyboard shortcuts via Input (not E_KEYDOWN) so they work even when UI has focus
    if (input->GetKeyPress(KEY_ESCAPE))
        engine_->Exit();

    // V key — toggle vertex edit mode
    if (input->GetKeyPress(KEY_V) && currentModel_)
    {
        if (!vertexEditMode_)
            EnterVertexEditMode();
        else
            ExitVertexEditMode();
    }

    // Vertex edit mode input
    if (vertexEditMode_)
    {
        auto* ui = GetSubsystem<UI>();

        // LMB click — pick or start drag
        if (input->GetMouseButtonPress(MOUSEB_LEFT))
        {
            if (!ui->GetElementAt(input->GetMousePosition()))
            {
                PickVertex(input->GetMousePosition().x_, input->GetMousePosition().y_);
                if (selectedVertex_ >= 0)
                {
                    vertexDragging_ = true;
                    dragStartPos_ = GetVertexWorldPosition(selectedGeometry_, selectedVertex_);
                    dragPlaneNormal_ = (cameraNode_->GetPosition() - dragStartPos_).Normalized();
                }
            }
        }

        // Mouse drag — move vertex
        if (vertexDragging_ && input->GetMouseButtonDown(MOUSEB_LEFT))
            MoveSelectedVertex(input->GetMousePosition().x_, input->GetMousePosition().y_);

        // LMB release — end drag
        if (vertexDragging_ && !input->GetMouseButtonDown(MOUSEB_LEFT))
        {
            vertexDragging_ = false;
            if (selectedVertex_ >= 0) vertexDirty_ = true;
        }

        // DEL — delete selected vertex
        if (input->GetKeyPress(KEY_DELETE) && selectedVertex_ >= 0)
            DeleteSelectedVertex();

        // Ctrl+S — save
        if (input->GetKeyDown(KEY_CTRL) && input->GetKeyPress(KEY_S) && editModel_)
            SaveModel();
    }

    // LMB click — bone picking (only when not over UI, not in vertex mode)
    if (input->GetMouseButtonPress(MOUSEB_LEFT) && isAnimated_ && !vertexEditMode_)
    {
        auto* ui = GetSubsystem<UI>();
        if (!ui->GetElementAt(input->GetMousePosition()))
            PickBone(input->GetMousePosition().x_, input->GetMousePosition().y_);
    }

    if (input->GetKeyPress(KEY_F))
    {
        showWireframe_ = !showWireframe_;
        auto* camera = cameraNode_->GetComponent<Camera>();
        if (camera) camera->SetFillMode(showWireframe_ ? FILL_WIREFRAME : FILL_SOLID);
    }

    if (input->GetKeyPress(KEY_S))
        showSkeleton_ = !showSkeleton_;

    if (input->GetKeyPress(KEY_B))
        showBoundingBox_ = !showBoundingBox_;

    if (input->GetKeyPress(KEY_R))
        AutoFrameCamera();

    if (input->GetKeyPress(KEY_TAB))
    {
        if (infoPanel_) infoPanel_->SetVisible(!infoPanel_->IsVisible());
    }

    if (input->GetKeyPress(KEY_SPACE))
    {
        if (animController_ && currentAnimIndex_ >= 0)
        {
            animPlaying_ = !animPlaying_;
            float spd = animReversed_ ? -animSpeed_ : animSpeed_;
            animController_->SetSpeed(availableAnims_[currentAnimIndex_], animPlaying_ ? spd : 0.0f);
        }
    }

    if (input->GetKeyPress(KEY_RIGHTBRACKET))
    {
        if (!availableAnims_.Empty())
        {
            int idx = (currentAnimIndex_ + 1) % (int)availableAnims_.Size();
            PlayAnimation(idx);
        }
    }

    if (input->GetKeyPress(KEY_LEFTBRACKET))
    {
        if (!availableAnims_.Empty())
        {
            int idx = currentAnimIndex_ - 1;
            if (idx < 0) idx = (int)availableAnims_.Size() - 1;
            PlayAnimation(idx);
        }
    }

    // Folder browse: Left/Right to navigate, G = keep, X = reject
    if (!folderModels_.Empty())
    {
        if (input->GetKeyPress(KEY_RIGHT))
            BrowseNext();
        if (input->GetKeyPress(KEY_LEFT))
            BrowsePrev();
        if (input->GetKeyPress(KEY_G))
            BrowseFlagKeep();
        if (input->GetKeyPress(KEY_X))
            BrowseFlagReject();
    }

    // T key — add text key at current time (only when not editing a LineEdit)
    if (input->GetKeyPress(KEY_T) && !vertexEditMode_)
    {
        auto* ui = GetSubsystem<UI>();
        auto* focused = ui->GetFocusElement();
        bool editingText = focused && (focused->GetType() == LineEdit::GetTypeStatic());
        if (!editingText)
            AddTextKeyAtCurrentTime();
    }

    // Delete key — remove selected text key (only when not in vertex edit and not editing text)
    if (input->GetKeyPress(KEY_DELETE) && !vertexEditMode_ && selectedTextKey_ >= 0)
    {
        auto* ui = GetSubsystem<UI>();
        auto* focused = ui->GetFocusElement();
        bool editingText = focused && (focused->GetType() == LineEdit::GetTypeStatic());
        if (!editingText)
            DeleteSelectedTextKey();
    }

    // Ctrl+S — save text keys (when not in vertex edit mode)
    if (input->GetKeyDown(KEY_CTRL) && input->GetKeyPress(KEY_S) && !vertexEditMode_ && currentAnimIndex_ >= 0)
        SaveTextKeys();

    // Text key flash fade
    if (textKeyFlashTimer_ > 0.0f)
    {
        textKeyFlashTimer_ -= timeStep;
        if (textKeyFlash_)
        {
            float alpha = Clamp(textKeyFlashTimer_ / 0.8f, 0.0f, 1.0f);
            textKeyFlash_->SetColor(Color(1.0f, 1.0f, 0.3f, alpha));
            if (textKeyFlashTimer_ <= 0.0f)
                textKeyFlash_->SetText("");
        }
    }

    // Update playback display
    if (animController_ && currentAnimIndex_ >= 0 && currentAnimIndex_ < (int)availableAnims_.Size())
    {
        float t = animController_->GetTime(availableAnims_[currentAnimIndex_]);
        float len = animController_->GetLength(availableAnims_[currentAnimIndex_]);
        String state = animPlaying_ ? " PLAY" : " PAUSED";

        if (animTimeText_)
        {
            animTimeText_->SetText(GetFileName(availableAnims_[currentAnimIndex_]) + "  " + String((double)t, 2) + " / " + String((double)len, 2) + state);
        }

        if (animSlider_ && len > 0.0f)
            animSlider_->SetValue(t / len);
    }
}

void ModelViewer::HandlePostRenderUpdate(StringHash, VariantMap&)
{
    auto* debug = scene_->GetComponent<DebugRenderer>();
    if (!debug) return;

    if (showBoundingBox_ && currentModel_)
    {
        BoundingBox bbox = currentModel_->GetBoundingBox();
        debug->AddBoundingBox(bbox, modelNode_->GetWorldTransform(), Color::YELLOW, false);
    }

    if ((showSkeleton_ || selectedBone_ >= 0) && isAnimated_ && animatedModelComp_)
    {
        const Vector<Bone>& bones = animatedModelComp_->GetSkeleton().GetBones();

        // Build selected subtree mask — mark selected, propagate to children
        // Bones are stored parent-before-child, so one forward pass is enough
        Vector<bool> inSubtree(bones.Size(), false);
        if (selectedBone_ >= 0 && selectedBone_ < (int)bones.Size())
        {
            inSubtree[selectedBone_] = true;
            for (unsigned i = (unsigned)selectedBone_ + 1; i < bones.Size(); ++i)
            {
                int p = bones[i].parentIndex_;
                if (p >= 0 && p < (int)bones.Size() && inSubtree[p])
                    inSubtree[i] = true;
            }
        }

        for (unsigned i = 0; i < bones.Size(); ++i)
        {
            Node* boneNode = bones[i].node_;
            if (!boneNode) continue;

            Vector3 pos = boneNode->GetWorldPosition();
            if (bones[i].parentIndex_ >= 0 && bones[i].parentIndex_ < (int)bones.Size())
            {
                Node* parentNode = bones[bones[i].parentIndex_].node_;
                if (parentNode)
                {
                    Color lineColor = inSubtree[i] ? Color::YELLOW : Color::CYAN;
                    if (showSkeleton_ || inSubtree[i])
                        debug->AddLine(parentNode->GetWorldPosition(), pos, lineColor, false);
                }
            }
        }
    }

    // Draw vertex overlay
    if (vertexEditMode_ && editModel_)
        DrawVertexOverlay(debug);
}

void ModelViewer::HandleMouseMove(StringHash, VariantMap& eventData)
{
    auto* input = GetSubsystem<Input>();

    if (input->GetMouseButtonDown(MOUSEB_RIGHT))
    {
        int dx = eventData[MouseMove::P_DX].GetI32();
        int dy = eventData[MouseMove::P_DY].GetI32();
        cameraYaw_ += dx * 0.3f;
        cameraPitch_ = Clamp(cameraPitch_ + dy * 0.3f, -89.0f, 89.0f);
    }

    if (input->GetMouseButtonDown(MOUSEB_MIDDLE) && lightNode_)
    {
        int dx = eventData[MouseMove::P_DX].GetI32();
        int dy = eventData[MouseMove::P_DY].GetI32();
        Quaternion rot = lightNode_->GetRotation();
        rot = Quaternion(dy * 0.5f, lightNode_->GetRight()) * rot;
        rot = Quaternion(dx * 0.5f, Vector3::UP) * rot;
        lightNode_->SetRotation(rot);
    }
}

void ModelViewer::HandleMouseWheel(StringHash, VariantMap& eventData)
{
    // Don't zoom camera when mouse is over a UI element that handles scroll
    auto* ui = GetSubsystem<UI>();
    UIElement* hover = ui->GetElementAt(ui->GetCursorPosition());
    if (hover && hover != ui->GetRoot())
        return;

    int wheel = eventData[MouseWheel::P_WHEEL].GetI32();
    cameraDistance_ *= (wheel > 0) ? 0.9f : 1.1f;
    cameraDistance_ = Max(cameraDistance_, 0.1f);
}

void ModelViewer::HandleFileOpen(StringHash, VariantMap& eventData)
{
    String path = eventData[FileSelected::P_FILENAME].GetString();
    bool ok = eventData[FileSelected::P_OK].GetBool();
    fileSelector_.Reset();
    if (!ok || path.Empty()) return;

    // Resolve symlinks so path matches registered resource dirs
    char resolvedBuf[PATH_MAX];
    String resolvedPath = path;
    if (realpath(path.CString(), resolvedBuf))
        resolvedPath = String(resolvedBuf);

    auto* cache = GetSubsystem<ResourceCache>();
    const Vector<String>& dirs = cache->GetResourceDirs();
    for (unsigned i = 0; i < dirs.Size(); ++i)
    {
        String resolvedDir = dirs[i];
        if (realpath(dirs[i].CString(), resolvedBuf))
            resolvedDir = String(resolvedBuf) + "/";

        if (resolvedPath.StartsWith(resolvedDir))
        {
            path = resolvedPath.Substring(resolvedDir.Length());
            break;
        }
        if (path.StartsWith(dirs[i]))
        {
            path = path.Substring(dirs[i].Length());
            break;
        }
    }

    if (!path.EndsWith(".mdl", false))
    {
        URHO3D_LOGERRORF("FileOpen: not a model file: %s", path.CString());
        if (statusText_)
            statusText_->SetText("Error: only .mdl files supported");
        return;
    }

    URHO3D_LOGINFOF("FileOpen: resolved path = %s", path.CString());
    LoadModel(path);
}

void ModelViewer::HandleAnimSlider(StringHash, VariantMap& eventData)
{
    if (!animController_ || currentAnimIndex_ < 0) return;
    float value = eventData[SliderChanged::P_VALUE].GetFloat();
    float length = animController_->GetLength(availableAnims_[currentAnimIndex_]);
    animController_->SetTime(availableAnims_[currentAnimIndex_], value * length);
}

void ModelViewer::HandleSpeedSlider(StringHash, VariantMap& eventData)
{
    float value = eventData[SliderChanged::P_VALUE].GetFloat();
    animSpeed_ = 0.1f + value * 2.9f;
    if (animSpeedText_) animSpeedText_->SetText("Speed: " + String((double)animSpeed_, 1) + "x");
    if (animController_ && currentAnimIndex_ >= 0 && animPlaying_)
    {
        float spd = animReversed_ ? -animSpeed_ : animSpeed_;
        animController_->SetSpeed(availableAnims_[currentAnimIndex_], spd);
    }
}

void ModelViewer::HandleAnimSelected(StringHash, VariantMap& eventData)
{
    if (suppressAnimSelect_) return;

    using namespace ItemSelected;
    int sel = eventData[P_SELECTION].GetI32();
    if (sel >= 0 && sel < (int)availableAnims_.Size())
        PlayAnimation(sel);
}

// ============================================================================
// Bone picking
// ============================================================================

void ModelViewer::PickBone(int screenX, int screenY)
{
    if (!animatedModelComp_ || !cameraNode_) return;

    auto* camera = cameraNode_->GetComponent<Camera>();
    if (!camera) return;

    auto* graphics = GetSubsystem<Graphics>();
    float nx = (float)screenX / (float)graphics->GetWidth();
    float ny = (float)screenY / (float)graphics->GetHeight();
    Ray ray = camera->GetScreenRay(nx, ny);

    Skeleton& skeleton = animatedModelComp_->GetSkeleton();
    const Vector<Bone>& bones = skeleton.GetBones();

    float bestDist = M_INFINITY;
    int bestBone = -1;

    for (unsigned i = 0; i < bones.Size(); ++i)
    {
        Node* boneNode = bones[i].node_;
        if (!boneNode) continue;

        Vector3 boneWorldPos = boneNode->GetWorldPosition();

        // Distance from ray to bone position
        Vector3 toPoint = boneWorldPos - ray.origin_;
        float along = toPoint.DotProduct(ray.direction_);
        if (along < 0.0f) continue;  // behind camera

        Vector3 closest = ray.origin_ + ray.direction_ * along;
        float dist = (closest - boneWorldPos).Length();

        // Scale threshold by distance from camera so far bones are still pickable
        float threshold = along * 0.03f;
        if (threshold < 0.1f) threshold = 0.1f;

        if (dist < threshold && dist < bestDist)
        {
            bestDist = dist;
            bestBone = (int)i;
        }
    }

    SelectBone(bestBone);
}

void ModelViewer::DrawBoneSubtree(DebugRenderer* debug, const Skeleton& skel, unsigned boneIndex, const Color& color)
{
    const Vector<Bone>& bones = skel.GetBones();
    if (boneIndex >= bones.Size()) return;

    Node* boneNode = bones[boneIndex].node_;
    if (!boneNode) return;

    // Draw sphere at this bone
    Vector3 pos = boneNode->GetWorldPosition();
    debug->AddSphere(Sphere(pos, 0.02f * cameraDistance_), color, false);

    // Draw lines to children and recurse
    for (unsigned i = 0; i < bones.Size(); ++i)
    {
        if (bones[i].parentIndex_ == (int)boneIndex && i != boneIndex)
        {
            Node* childNode = bones[i].node_;
            if (childNode)
            {
                debug->AddLine(pos, childNode->GetWorldPosition(), color, false);
                DrawBoneSubtree(debug, skel, i, Color(color.r_ * 0.8f, color.g_ * 0.8f, color.b_, color.a_));
            }
        }
    }
}

void ModelViewer::SelectBone(int boneIndex)
{
    selectedBone_ = boneIndex;

    // Highlight the selected button in the bone list, dehighlight others
    for (unsigned i = 0; i < boneButtons_.Size(); ++i)
    {
        Button* btn = boneButtons_[i];
        bool selected = (btn->GetVar("BoneIndex").GetI32() == boneIndex);
        btn->SetColor(selected ? Color(0.35f, 0.35f, 0.15f, 0.9f) : Color(0.18f, 0.18f, 0.22f, 0.6f));
        auto* label = btn->GetChildStaticCast<Text>(0);
        if (label)
            label->SetColor(selected ? Color(1.0f, 1.0f, 0.4f) : Color(0.85f, 0.85f, 0.85f));
    }

    // Update status text
    if (boneIndex >= 0 && isAnimated_ && animatedModelComp_)
    {
        Skeleton& skeleton = animatedModelComp_->GetSkeleton();
        const Vector<Bone>& bones = skeleton.GetBones();
        if (boneIndex < (int)bones.Size() && statusText_)
            statusText_->SetText("Bone: " + bones[boneIndex].name_);
    }
    else if (statusText_ && currentModelPath_.Length())
    {
        statusText_->SetText(currentModelPath_);
    }

    UpdateBonePopover();
}

void ModelViewer::HandleBoneListClick(StringHash, VariantMap& eventData)
{
    auto* btn = static_cast<Button*>(eventData[Released::P_ELEMENT].GetPtr());
    if (!btn) return;
    int boneIndex = btn->GetVar("BoneIndex").GetI32();
    SelectBone(boneIndex);
}

void ModelViewer::UpdateBonePopover()
{
    if (selectedBone_ < 0 || !isAnimated_ || !animatedModelComp_)
    {
        if (bonePopover_) bonePopover_->SetVisible(false);
        return;
    }

    Skeleton& skeleton = animatedModelComp_->GetSkeleton();
    const Vector<Bone>& bones = skeleton.GetBones();
    if (selectedBone_ >= (int)bones.Size())
    {
        if (bonePopover_) bonePopover_->SetVisible(false);
        return;
    }

    auto* ui = GetSubsystem<UI>();

    // Create side panel once — anchored to the left edge
    if (!bonePopover_)
    {
        bonePopover_ = ui->GetRoot()->CreateChild<Window>("BonePanel");
        bonePopover_->SetStyleAuto();
        bonePopover_->SetLayout(LM_VERTICAL, 2, IntRect(8, 6, 8, 6));
        bonePopover_->SetFixedWidth(250);
        bonePopover_->SetPosition(10, 36);
        bonePopover_->SetMovable(true);
        bonePopover_->SetOpacity(0.93f);
        bonePopover_->SetColor(Color(0.18f, 0.18f, 0.24f));

        // Title bar
        auto* titleBar = bonePopover_->CreateChild<BorderImage>();
        titleBar->SetLayout(LM_HORIZONTAL, 4, IntRect(4, 2, 4, 2));
        titleBar->SetFixedHeight(22);
        titleBar->SetColor(Color(0.14f, 0.14f, 0.18f));

        auto* title = titleBar->CreateChild<Text>();
        title->SetFont(font_, 12);
        title->SetText("Bone");
        title->SetColor(Color(0.9f, 0.9f, 0.4f));
    }

    // Remove old content lines (keep title bar at index 0)
    while (bonePopover_->GetNumChildren() > 1)
        bonePopover_->RemoveChildAtIndex(1);

    const Bone& bone = bones[selectedBone_];

    auto addField = [this](const String& text, const Color& color = Color(0.85f, 0.85f, 0.85f))
    {
        auto* line = bonePopover_->CreateChild<Text>();
        line->SetFont(font_, 10);
        line->SetText(text);
        line->SetColor(color);
    };

    // Title
    addField(bone.name_ + "  [" + String(selectedBone_) + "]", Color(0.95f, 0.9f, 0.4f));

    // Parent
    String parentName = "ROOT";
    if (bone.parentIndex_ >= 0 && bone.parentIndex_ < (int)bones.Size())
        parentName = bones[bone.parentIndex_].name_ + " [" + String(bone.parentIndex_) + "]";
    addField("Parent: " + parentName);

    // Depth
    int depth = 0;
    int p = bone.parentIndex_;
    while (p > 0 && depth < 20) { p = bones[p].parentIndex_; depth++; }
    addField("Depth: " + String(depth));

    // Initial position
    const Vector3& pos = bone.initialPosition_;
    addField("Position: " + String((double)pos.x_, 3) + ", " + String((double)pos.y_, 3) + ", " + String((double)pos.z_, 3));

    // Initial rotation (as Euler)
    Vector3 euler = bone.initialRotation_.EulerAngles();
    addField("Rotation: " + String((double)euler.x_, 1) + ", " + String((double)euler.y_, 1) + ", " + String((double)euler.z_, 1));

    // Initial scale
    const Vector3& scl = bone.initialScale_;
    addField("Scale: " + String((double)scl.x_, 3) + ", " + String((double)scl.y_, 3) + ", " + String((double)scl.z_, 3));

    // Radius
    addField("Radius: " + String((double)bone.radius_, 4));

    // Bounding box
    const BoundingBox& bb = bone.boundingBox_;
    if (bb.Defined())
    {
        addField("BBox min: " + String((double)bb.min_.x_, 2) + ", " + String((double)bb.min_.y_, 2) + ", " + String((double)bb.min_.z_, 2));
        addField("BBox max: " + String((double)bb.max_.x_, 2) + ", " + String((double)bb.max_.y_, 2) + ", " + String((double)bb.max_.z_, 2));
    }
    else
        addField("BBox: undefined");

    // Collision type
    String collStr = "None";
    if (bone.collisionMask_ & BONECOLLISION_SPHERE) collStr = "Sphere";
    if (bone.collisionMask_ & BONECOLLISION_BOX) collStr = "Box";
    if ((bone.collisionMask_ & BONECOLLISION_SPHERE) && (bone.collisionMask_ & BONECOLLISION_BOX)) collStr = "Sphere+Box";
    addField("Collision: " + collStr);

    // Animated flag
    addField(String("Animated: ") + (bone.animated_ ? "Yes" : "No"));

    // World position (runtime, from node)
    if (bone.node_)
    {
        Vector3 wp = bone.node_->GetWorldPosition();
        addField("World pos: " + String((double)wp.x_, 3) + ", " + String((double)wp.y_, 3) + ", " + String((double)wp.z_, 3),
                 Color(0.6f, 0.85f, 0.6f));
    }

    bonePopover_->SetVisible(true);
}

// ============================================================================
// Vertex Editor
// ============================================================================

void ModelViewer::EnterVertexEditMode()
{
    if (!currentModel_ || vertexEditMode_) return;

    editModel_ = currentModel_->Clone();
    if (!editModel_) return;

    // Swap cloned model onto the component
    if (animatedModelComp_)
    {
        animatedModelComp_->SetModel(editModel_);
    }
    else if (staticModelComp_)
    {
        staticModelComp_->SetModel(editModel_);
    }

    vertexEditMode_ = true;
    selectedVertex_ = -1;
    selectedGeometry_ = -1;
    vertexDirty_ = false;
    vertexDragging_ = false;

    // Create billboard overlay for vertex dots
    vertexOverlayNode_ = scene_->CreateChild("VertexOverlay");
    vertexBillboards_ = vertexOverlayNode_->CreateComponent<BillboardSet>();
    vertexBillboards_->SetFixedScreenSize(true);
    vertexBillboards_->SetFaceCameraMode(FC_ROTATE_XYZ);
    vertexBillboards_->SetSorted(false);

    // Create a small white circle texture (8x8) for the dot sprite
    SharedPtr<Image> dotImage(new Image(context_));
    dotImage->SetSize(8, 8, 4);
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 8; ++x)
        {
            float dx = x - 3.5f, dy = y - 3.5f;
            float dist = sqrtf(dx * dx + dy * dy);
            unsigned char a = dist < 3.0f ? 255 : (dist < 3.5f ? (unsigned char)(255.0f * (3.5f - dist) / 0.5f) : 0);
            dotImage->SetPixel(x, y, Color(1.0f, 1.0f, 1.0f, a / 255.0f));
        }
    }
    SharedPtr<Texture2D> dotTex(new Texture2D(context_));
    dotTex->SetData(dotImage);

    auto* cache = GetSubsystem<ResourceCache>();
    SharedPtr<Material> dotMat(new Material(context_));
    dotMat->SetTechnique(0, cache->GetResource<Technique>("Techniques/DiffUnlitParticleAlpha.xml"));
    dotMat->SetTexture(TU_DIFFUSE, dotTex);
    dotMat->SetCullMode(CULL_NONE);
    vertexBillboards_->SetMaterial(dotMat);

    if (statusText_)
        statusText_->SetText("VERTEX EDIT — Click=select, DEL=delete, Ctrl+S=save, V=exit");

    URHO3D_LOGINFO("Entered vertex edit mode");
}

void ModelViewer::ExitVertexEditMode()
{
    if (!vertexEditMode_) return;

    if (vertexDirty_)
    {
        if (statusText_)
            statusText_->SetText("UNSAVED CHANGES — Ctrl+S to save, V again to discard");
        vertexEditMode_ = false;  // Will still prompt on next V
        // Set a flag so next V truly exits
        // Actually, just let them exit — they were warned
    }

    // Restore original model
    if (animatedModelComp_)
        animatedModelComp_->SetModel(currentModel_);
    else if (staticModelComp_)
        staticModelComp_->SetModel(currentModel_);

    editModel_.Reset();
    vertexEditMode_ = false;
    selectedVertex_ = -1;
    selectedGeometry_ = -1;
    vertexDirty_ = false;
    vertexDragging_ = false;

    // Remove billboard overlay
    if (vertexOverlayNode_)
    {
        vertexOverlayNode_->Remove();
        vertexOverlayNode_ = nullptr;
        vertexBillboards_ = nullptr;
    }

    if (statusText_)
        statusText_->SetText(currentModelPath_);

    URHO3D_LOGINFO("Exited vertex edit mode");
}

Vector3 ModelViewer::SkinVertex(const unsigned char* vertData, unsigned stride, unsigned posOffset,
    unsigned weightOffset, unsigned indexOffset, bool hasSkinning, unsigned vertIndex,
    const Vector<Bone>& bones, const Matrix3x4& worldTransform)
{
    Vector3 localPos = *reinterpret_cast<const Vector3*>(vertData + vertIndex * stride + posOffset);

    if (!hasSkinning)
        return worldTransform * localPos;

    const float* weights = reinterpret_cast<const float*>(vertData + vertIndex * stride + weightOffset);
    const unsigned char* indices = vertData + vertIndex * stride + indexOffset;

    Vector3 skinnedPos = Vector3::ZERO;
    for (int b = 0; b < 4; ++b)
    {
        float w = weights[b];
        if (w < 0.001f) continue;

        unsigned boneIdx = indices[b];
        if (boneIdx >= bones.Size() || !bones[boneIdx].node_) continue;

        Matrix3x4 skinMatrix = bones[boneIdx].node_->GetWorldTransform() * bones[boneIdx].offsetMatrix_;
        skinnedPos += (skinMatrix * localPos) * w;
    }
    return skinnedPos;
}

void ModelViewer::DrawVertexOverlay(DebugRenderer* debug)
{
    if (!editModel_ || !modelNode_ || !vertexBillboards_) return;

    Matrix3x4 worldTransform = modelNode_->GetWorldTransform();

    // Get skeleton for CPU skinning
    const Vector<Bone>* bonesPtr = nullptr;
    if (isAnimated_ && animatedModelComp_)
        bonesPtr = &animatedModelComp_->GetSkeleton().GetBones();

    // Count total vertices across all geometries
    unsigned totalVerts = 0;
    for (unsigned g = 0; g < editModel_->GetNumGeometries(); ++g)
    {
        Geometry* geom = editModel_->GetGeometry(g, 0);
        if (!geom) continue;
        VertexBuffer* vb = geom->GetVertexBuffer(0);
        if (vb && vb->GetShadowData())
            totalVerts += vb->GetVertexCount();
    }

    vertexBillboards_->SetNumBillboards(totalVerts);

    unsigned bbIndex = 0;
    for (unsigned g = 0; g < editModel_->GetNumGeometries(); ++g)
    {
        Geometry* geom = editModel_->GetGeometry(g, 0);
        if (!geom) continue;

        VertexBuffer* vb = geom->GetVertexBuffer(0);
        if (!vb) continue;

        const auto* data = vb->GetShadowData();
        if (!data) continue;

        unsigned stride = vb->GetVertexSize();
        unsigned posOffset = vb->GetElementOffset(SEM_POSITION);
        unsigned vertCount = vb->GetVertexCount();

        bool hasSkinning = vb->HasElement(SEM_BLENDWEIGHTS) && vb->HasElement(SEM_BLENDINDICES);
        unsigned weightOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDWEIGHTS) : 0;
        unsigned indexOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDINDICES) : 0;

        const Vector<Bone> emptyBones;
        const Vector<Bone>& bones = bonesPtr ? *bonesPtr : emptyBones;

        for (unsigned i = 0; i < vertCount; ++i)
        {
            Vector3 worldPos = SkinVertex(reinterpret_cast<const unsigned char*>(data), stride, posOffset, weightOffset, indexOffset,
                hasSkinning && bonesPtr, i, bones, worldTransform);

            Billboard* bb = vertexBillboards_->GetBillboard(bbIndex);
            bb->position_ = worldPos;
            bb->enabled_ = true;

            if ((int)g == selectedGeometry_ && (int)i == selectedVertex_)
            {
                bb->size_ = Vector2(8.0f, 8.0f);
                bb->color_ = Color::YELLOW;
            }
            else
            {
                bb->size_ = Vector2(4.0f, 4.0f);
                bb->color_ = Color(0.9f, 0.2f, 0.2f);
            }
            ++bbIndex;
        }
    }

    vertexBillboards_->Commit();
}

void ModelViewer::PickVertex(int screenX, int screenY)
{
    if (!editModel_ || !cameraNode_) return;

    auto* camera = cameraNode_->GetComponent<Camera>();
    if (!camera) return;

    auto* graphics = GetSubsystem<Graphics>();
    float nx = (float)screenX / (float)graphics->GetWidth();
    float ny = (float)screenY / (float)graphics->GetHeight();
    Ray ray = camera->GetScreenRay(nx, ny);

    Matrix3x4 worldTransform = modelNode_->GetWorldTransform();

    // Get skeleton for CPU skinning (same as DrawVertexOverlay)
    const Vector<Bone>* bonesPtr = nullptr;
    if (isAnimated_ && animatedModelComp_)
        bonesPtr = &animatedModelComp_->GetSkeleton().GetBones();

    float bestDist = M_INFINITY;
    int bestGeom = -1, bestVert = -1;

    for (unsigned g = 0; g < editModel_->GetNumGeometries(); ++g)
    {
        Geometry* geom = editModel_->GetGeometry(g, 0);
        if (!geom) continue;

        VertexBuffer* vb = geom->GetVertexBuffer(0);
        if (!vb) continue;

        const auto* data = vb->GetShadowData();
        if (!data) continue;

        unsigned stride = vb->GetVertexSize();
        unsigned posOffset = vb->GetElementOffset(SEM_POSITION);
        unsigned vertCount = vb->GetVertexCount();

        bool hasSkinning = vb->HasElement(SEM_BLENDWEIGHTS) && vb->HasElement(SEM_BLENDINDICES);
        unsigned weightOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDWEIGHTS) : 0;
        unsigned indexOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDINDICES) : 0;

        const Vector<Bone> emptyBones;
        const Vector<Bone>& bones = bonesPtr ? *bonesPtr : emptyBones;

        for (unsigned i = 0; i < vertCount; ++i)
        {
            Vector3 worldPos = SkinVertex(reinterpret_cast<const unsigned char*>(data), stride, posOffset, weightOffset, indexOffset,
                hasSkinning && bonesPtr, i, bones, worldTransform);

            Vector3 toPoint = worldPos - ray.origin_;
            float along = toPoint.DotProduct(ray.direction_);
            if (along < 0.0f) continue;

            Vector3 closest = ray.origin_ + ray.direction_ * along;
            float dist = (closest - worldPos).Length();

            float threshold = along * 0.02f;
            if (threshold < 0.05f) threshold = 0.05f;

            if (dist < threshold && dist < bestDist)
            {
                bestDist = dist;
                bestGeom = (int)g;
                bestVert = (int)i;
            }
        }
    }

    selectedGeometry_ = bestGeom;
    selectedVertex_ = bestVert;
    UpdateVertexStatusText();
}

void ModelViewer::DeleteSelectedVertex()
{
    if (!editModel_ || selectedGeometry_ < 0 || selectedVertex_ < 0) return;

    Geometry* geom = editModel_->GetGeometry((unsigned)selectedGeometry_, 0);
    if (!geom) return;

    VertexBuffer* vb = geom->GetVertexBuffer(0);
    IndexBuffer* ib = geom->GetIndexBuffer();
    if (!vb || !ib) return;

    const auto* vertData = vb->GetShadowData();
    const auto* indexData = ib->GetShadowData();
    if (!vertData || !indexData) return;

    unsigned vertSize = vb->GetVertexSize();
    unsigned vertCount = vb->GetVertexCount();
    unsigned indexSize = ib->GetIndexSize();
    unsigned indexCount = ib->GetIndexCount();
    unsigned vertToDelete = (unsigned)selectedVertex_;

    if (vertToDelete >= vertCount) return;

    // --- Rebuild index buffer: skip triangles referencing deleted vert, remap ---
    Vector<unsigned> newIndices;
    for (unsigned i = 0; i + 2 < indexCount; i += 3)
    {
        unsigned i0, i1, i2;
        if (indexSize == 2)
        {
            i0 = ((const unsigned short*)indexData)[i];
            i1 = ((const unsigned short*)indexData)[i + 1];
            i2 = ((const unsigned short*)indexData)[i + 2];
        }
        else
        {
            i0 = ((const unsigned*)indexData)[i];
            i1 = ((const unsigned*)indexData)[i + 1];
            i2 = ((const unsigned*)indexData)[i + 2];
        }

        // Skip triangles that reference the deleted vertex
        if (i0 == vertToDelete || i1 == vertToDelete || i2 == vertToDelete)
            continue;

        // Remap indices above deleted vertex down by 1
        if (i0 > vertToDelete) i0--;
        if (i1 > vertToDelete) i1--;
        if (i2 > vertToDelete) i2--;

        newIndices.Push(i0);
        newIndices.Push(i1);
        newIndices.Push(i2);
    }

    // --- Rebuild vertex buffer: remove the deleted vertex ---
    unsigned newVertCount = vertCount - 1;
    SharedArrayPtr<byte> newVertData(new byte[newVertCount * vertSize]);

    // Copy vertices before the deleted one
    if (vertToDelete > 0)
        memcpy(newVertData.Get(), vertData, vertToDelete * vertSize);

    // Copy vertices after the deleted one
    if (vertToDelete < vertCount - 1)
        memcpy(newVertData.Get() + vertToDelete * vertSize,
               vertData + (vertToDelete + 1) * vertSize,
               (vertCount - vertToDelete - 1) * vertSize);

    // --- Upload to GPU ---
    vb->SetSize(newVertCount, vb->GetElements());
    vb->SetData(newVertData.Get());

    unsigned newIndexCount = newIndices.Size();
    bool largeIndices = newVertCount > 65535;
    ib->SetSize(newIndexCount, largeIndices);

    if (largeIndices)
    {
        ib->SetData(newIndices.Buffer());
    }
    else
    {
        Vector<unsigned short> shortIndices(newIndexCount);
        for (unsigned i = 0; i < newIndexCount; ++i)
            shortIndices[i] = (unsigned short)newIndices[i];
        ib->SetData(shortIndices.Buffer());
    }

    geom->SetDrawRange(TRIANGLE_LIST, 0, newIndexCount);

    // --- Recalculate bounding box ---
    BoundingBox newBBox;
    for (unsigned g = 0; g < editModel_->GetNumGeometries(); ++g)
    {
        Geometry* gm = editModel_->GetGeometry(g, 0);
        if (!gm) continue;
        VertexBuffer* gvb = gm->GetVertexBuffer(0);
        if (!gvb || !gvb->GetShadowData()) continue;

        unsigned gStride = gvb->GetVertexSize();
        unsigned gPosOff = gvb->GetElementOffset(SEM_POSITION);
        const auto* gData = gvb->GetShadowData();
        for (unsigned v = 0; v < gvb->GetVertexCount(); ++v)
        {
            Vector3 pos = *reinterpret_cast<const Vector3*>(gData + v * gStride + gPosOff);
            newBBox.Merge(pos);
        }
    }
    editModel_->SetBoundingBox(newBBox);

    unsigned removedTris = (indexCount - newIndexCount) / 3;
    URHO3D_LOGINFOF("Deleted vertex %d from geometry %d — removed %d triangles", selectedVertex_, selectedGeometry_, removedTris);

    selectedVertex_ = -1;
    selectedGeometry_ = -1;
    vertexDirty_ = true;
    UpdateVertexStatusText();
    RebuildInfoText();
}

void ModelViewer::MoveSelectedVertex(int screenX, int screenY)
{
    if (!editModel_ || selectedGeometry_ < 0 || selectedVertex_ < 0 || !cameraNode_) return;

    auto* camera = cameraNode_->GetComponent<Camera>();
    if (!camera) return;

    auto* graphics = GetSubsystem<Graphics>();
    float nx = (float)screenX / (float)graphics->GetWidth();
    float ny = (float)screenY / (float)graphics->GetHeight();
    Ray ray = camera->GetScreenRay(nx, ny);

    Plane dragPlane(dragPlaneNormal_, dragStartPos_);
    float hitDist = ray.HitDistance(dragPlane);
    if (hitDist >= M_INFINITY) return;

    Vector3 newWorldPos = ray.origin_ + ray.direction_ * hitDist;
    Vector3 newLocalPos = modelNode_->GetWorldTransform().Inverse() * newWorldPos;

    Geometry* geom = editModel_->GetGeometry((unsigned)selectedGeometry_, 0);
    if (!geom) return;

    VertexBuffer* vb = geom->GetVertexBuffer(0);
    if (!vb) return;

    auto* data = const_cast<byte*>(vb->GetShadowData());
    if (!data) return;

    unsigned stride = vb->GetVertexSize();
    unsigned posOffset = vb->GetElementOffset(SEM_POSITION);
    *reinterpret_cast<Vector3*>(data + (unsigned)selectedVertex_ * stride + posOffset) = newLocalPos;

    vb->SetData(data);
}

void ModelViewer::SaveModel()
{
    if (!editModel_)
    {
        if (statusText_) statusText_->SetText("ERROR: No edited model to save");
        return;
    }

    auto* cache = GetSubsystem<ResourceCache>();
    auto* fs = GetSubsystem<FileSystem>();
    const Vector<String>& dirs = cache->GetResourceDirs();

    String absolutePath;
    for (unsigned i = 0; i < dirs.Size(); ++i)
    {
        String candidate = dirs[i] + currentModelPath_;
        if (fs->FileExists(candidate))
        {
            absolutePath = candidate;
            break;
        }
    }

    if (absolutePath.Empty())
    {
        if (statusText_) statusText_->SetText("ERROR: Cannot resolve path for " + currentModelPath_);
        return;
    }

    File file(context_, absolutePath, FILE_WRITE);
    if (file.IsOpen())
    {
        editModel_->Save(file);
        vertexDirty_ = false;
        if (statusText_) statusText_->SetText("SAVED: " + absolutePath);
        URHO3D_LOGINFOF("Saved edited model to %s", absolutePath.CString());
    }
    else
    {
        if (statusText_) statusText_->SetText("ERROR: Cannot write " + absolutePath);
    }
}

void ModelViewer::ExportFBX()
{
    if (currentModelPath_.Empty())
    {
        if (statusText_) statusText_->SetText("ERROR: No model loaded to export");
        return;
    }

    auto* ui = GetSubsystem<UI>();
    auto* style = ui->GetRoot()->GetDefaultStyle();
    auto* fs = GetSubsystem<FileSystem>();

    fileSelector_ = new FileSelector(context_);
    fileSelector_->SetDefaultStyle(style);
    fileSelector_->SetTitle("Export FBX");
    fileSelector_->SetButtonTexts("Export", "Cancel");

    Vector<String> filters;
    filters.Push("*.fbx");
    fileSelector_->SetFilters(filters, 0);

    // Default to same directory as the model
    auto* cache = GetSubsystem<ResourceCache>();
    const Vector<String>& dirs = cache->GetResourceDirs();
    for (unsigned i = 0; i < dirs.Size(); ++i)
    {
        String candidate = dirs[i] + currentModelPath_;
        if (fs->FileExists(candidate))
        {
            fileSelector_->SetPath(GetPath(candidate));
            // Pre-fill filename with model name but .fbx extension
            String baseName = GetFileName(currentModelPath_);
            fileSelector_->SetFileName(baseName + ".fbx");
            break;
        }
    }

    SubscribeToEvent(fileSelector_, E_FILESELECTED, URHO3D_HANDLER(ModelViewer, HandleExportFBXSelected));
}

void ModelViewer::HandleExportFBXSelected(StringHash, VariantMap& eventData)
{
    String fbxPath = eventData[FileSelected::P_FILENAME].GetString();
    bool ok = eventData[FileSelected::P_OK].GetBool();
    fileSelector_.Reset();
    if (!ok || fbxPath.Empty()) return;

    // Ensure .fbx extension
    if (!fbxPath.EndsWith(".fbx", false))
        fbxPath += ".fbx";

    // Resolve the absolute path to the source .mdl
    auto* cache = GetSubsystem<ResourceCache>();
    auto* fs = GetSubsystem<FileSystem>();
    const Vector<String>& dirs = cache->GetResourceDirs();
    String mdlAbsPath;
    for (unsigned i = 0; i < dirs.Size(); ++i)
    {
        String candidate = dirs[i] + currentModelPath_;
        if (fs->FileExists(candidate))
        {
            mdlAbsPath = candidate;
            break;
        }
    }

    if (mdlAbsPath.Empty())
    {
        if (statusText_) statusText_->SetText("ERROR: Cannot resolve " + currentModelPath_);
        return;
    }

    // Build AssetImporter command
    String assetImporter = fs->GetProgramDir() + "AssetImporter";
    String cmd = "\"" + assetImporter + "\" export \"" + mdlAbsPath + "\" \"" + fbxPath + "\"";

    // Include loaded animations
    for (unsigned i = 0; i < availableAnims_.Size(); ++i)
    {
        // Resolve animation path
        for (unsigned d = 0; d < dirs.Size(); ++d)
        {
            String animCandidate = dirs[d] + availableAnims_[i];
            if (fs->FileExists(animCandidate))
            {
                cmd += " -anim \"" + animCandidate + "\"";
                break;
            }
        }
    }

    if (statusText_) statusText_->SetText("Exporting FBX...");
    URHO3D_LOGINFOF("Export FBX: %s", cmd.CString());

    int result = system(cmd.CString());
    if (result == 0)
    {
        if (statusText_) statusText_->SetText("EXPORTED: " + fbxPath);
        URHO3D_LOGINFOF("FBX export successful: %s", fbxPath.CString());
    }
    else
    {
        if (statusText_) statusText_->SetText("ERROR: FBX export failed (code " + String(result) + ")");
        URHO3D_LOGERRORF("FBX export failed with code %d", result);
    }
}

Vector3 ModelViewer::GetVertexWorldPosition(int geomIndex, int vertIndex)
{
    if (!editModel_ || geomIndex < 0 || vertIndex < 0) return Vector3::ZERO;

    Geometry* geom = editModel_->GetGeometry((unsigned)geomIndex, 0);
    if (!geom) return Vector3::ZERO;

    VertexBuffer* vb = geom->GetVertexBuffer(0);
    if (!vb) return Vector3::ZERO;

    const auto* data = vb->GetShadowData();
    if (!data) return Vector3::ZERO;

    unsigned stride = vb->GetVertexSize();
    unsigned posOffset = vb->GetElementOffset(SEM_POSITION);

    bool hasSkinning = vb->HasElement(SEM_BLENDWEIGHTS) && vb->HasElement(SEM_BLENDINDICES);
    unsigned weightOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDWEIGHTS) : 0;
    unsigned indexOffset = hasSkinning ? vb->GetElementOffset(SEM_BLENDINDICES) : 0;

    const Vector<Bone>* bonesPtr = nullptr;
    if (isAnimated_ && animatedModelComp_)
        bonesPtr = &animatedModelComp_->GetSkeleton().GetBones();

    const Vector<Bone> emptyBones;
    const Vector<Bone>& bones = bonesPtr ? *bonesPtr : emptyBones;

    return SkinVertex(reinterpret_cast<const unsigned char*>(data), stride, posOffset, weightOffset, indexOffset,
        hasSkinning && bonesPtr, (unsigned)vertIndex, bones, modelNode_->GetWorldTransform());
}

void ModelViewer::UpdateVertexStatusText()
{
    if (!statusText_) return;

    if (selectedVertex_ >= 0 && selectedGeometry_ >= 0)
    {
        Vector3 pos = GetVertexWorldPosition(selectedGeometry_, selectedVertex_);
        char buf[128];
        snprintf(buf, sizeof(buf), "VERTEX EDIT — Geom %d Vert %d (%.3f, %.3f, %.3f)",
                 selectedGeometry_, selectedVertex_, pos.x_, pos.y_, pos.z_);

        // Count total verts
        unsigned totalVerts = 0;
        if (editModel_)
        {
            for (unsigned g = 0; g < editModel_->GetNumGeometries(); ++g)
            {
                Geometry* gm = editModel_->GetGeometry(g, 0);
                if (gm && gm->GetVertexBuffer(0))
                    totalVerts += gm->GetVertexBuffer(0)->GetVertexCount();
            }
        }

        String s(buf);
        s += " | Total: " + String(totalVerts) + " verts";
        if (vertexDirty_) s += " [MODIFIED]";
        statusText_->SetText(s);
    }
    else
    {
        String s = "VERTEX EDIT — Click to select";
        if (vertexDirty_) s += " [MODIFIED]";
        statusText_->SetText(s);
    }
}

// ============================================================================
// Helpers
// ============================================================================

String ModelViewer::SemanticName(VertexElementSemantic sem)
{
    switch (sem)
    {
    case SEM_POSITION: return "POSITION";
    case SEM_NORMAL: return "NORMAL";
    case SEM_BINORMAL: return "BINORMAL";
    case SEM_TANGENT: return "TANGENT";
    case SEM_TEXCOORD: return "TEXCOORD";
    case SEM_COLOR: return "COLOR";
    case SEM_BLENDWEIGHTS: return "BLENDWEIGHTS";
    case SEM_BLENDINDICES: return "BLENDINDICES";
    case SEM_OBJECTINDEX: return "OBJECTINDEX";
    default: return "UNKNOWN";
    }
}

String ModelViewer::TypeName(VertexElementType type)
{
    switch (type)
    {
    case TYPE_INT: return "Int";
    case TYPE_FLOAT: return "Float";
    case TYPE_VECTOR2: return "Vec2";
    case TYPE_VECTOR3: return "Vec3";
    case TYPE_VECTOR4: return "Vec4";
    case TYPE_UBYTE4: return "UB4";
    case TYPE_UBYTE4_NORM: return "UB4N";
    default: return "?";
    }
}

// ============================================================================
// Folder Browse
// ============================================================================

void ModelViewer::ScanFolder(const String& folderPath)
{
    auto* fs = GetSubsystem<FileSystem>();

    String absFolder = folderPath;
    if (!absFolder.EndsWith("/"))
        absFolder += "/";

    // Resolve to absolute path if relative
    if (!absFolder.StartsWith("/"))
    {
        auto* cache = GetSubsystem<ResourceCache>();
        const Vector<String>& dirs = cache->GetResourceDirs();
        for (unsigned i = 0; i < dirs.Size(); ++i)
        {
            if (fs->DirExists(dirs[i] + absFolder))
            {
                absFolder = dirs[i] + absFolder;
                break;
            }
        }
    }

    if (!fs->DirExists(absFolder))
    {
        URHO3D_LOGERRORF("Folder does not exist: %s", absFolder.CString());
        if (statusText_) statusText_->SetText("ERROR: Folder not found: " + absFolder);
        return;
    }

    browseFolderPath_ = absFolder;
    folderModels_.Clear();
    folderReview_.Clear();

    Vector<String> files;
    fs->ScanDir(files, absFolder, "*.mdl", SCAN_FILES, false);
    Sort(files.Begin(), files.End());

    // Convert to resource-relative paths
    auto* cache = GetSubsystem<ResourceCache>();
    const Vector<String>& dirs = cache->GetResourceDirs();

    for (unsigned i = 0; i < files.Size(); ++i)
    {
        String absPath = absFolder + files[i];
        String resourcePath;

        // Try to make resource-relative
        char resolvedBuf[PATH_MAX];
        String resolvedAbs = absPath;
        if (realpath(absPath.CString(), resolvedBuf))
            resolvedAbs = String(resolvedBuf);

        for (unsigned d = 0; d < dirs.Size(); ++d)
        {
            String resolvedDir = dirs[d];
            if (realpath(dirs[d].CString(), resolvedBuf))
                resolvedDir = String(resolvedBuf) + "/";

            if (resolvedAbs.StartsWith(resolvedDir))
            {
                resourcePath = resolvedAbs.Substring(resolvedDir.Length());
                break;
            }
        }

        if (!resourcePath.Empty())
            folderModels_.Push(resourcePath);
        else
            URHO3D_LOGWARNINGF("Skipping %s — not in any resource directory", absPath.CString());
    }

    if (folderModels_.Empty())
    {
        if (statusText_) statusText_->SetText("No .mdl files found in " + absFolder);
        return;
    }

    URHO3D_LOGINFOF("Folder browse: %d models in %s", folderModels_.Size(), absFolder.CString());
    browseIndex_ = 0;
    LoadModel(folderModels_[browseIndex_]);

    if (browseStatusText_)
        browseStatusText_->SetText(String(browseIndex_ + 1) + "/" + String(folderModels_.Size()) + ": " + GetFileName(folderModels_[browseIndex_]));
}

void ModelViewer::BrowseNext()
{
    if (folderModels_.Empty()) return;
    browseIndex_ = (browseIndex_ + 1) % folderModels_.Size();
    LoadModel(folderModels_[browseIndex_]);

    String flag;
    String fname = GetFileName(folderModels_[browseIndex_]);
    if (folderReview_.Contains(fname))
        flag = " [" + folderReview_[fname] + "]";

    if (browseStatusText_)
        browseStatusText_->SetText(String(browseIndex_ + 1) + "/" + String(folderModels_.Size()) + ": " + fname + flag);
}

void ModelViewer::BrowsePrev()
{
    if (folderModels_.Empty()) return;
    browseIndex_ = (browseIndex_ == 0) ? folderModels_.Size() - 1 : browseIndex_ - 1;
    LoadModel(folderModels_[browseIndex_]);

    String flag;
    String fname = GetFileName(folderModels_[browseIndex_]);
    if (folderReview_.Contains(fname))
        flag = " [" + folderReview_[fname] + "]";

    if (browseStatusText_)
        browseStatusText_->SetText(String(browseIndex_ + 1) + "/" + String(folderModels_.Size()) + ": " + fname + flag);
}

void ModelViewer::BrowseFlagKeep()
{
    if (folderModels_.Empty()) return;
    String fname = GetFileName(folderModels_[browseIndex_]);
    folderReview_[fname] = "keep";
    WriteFolderReview();

    if (browseStatusText_)
        browseStatusText_->SetText(String(browseIndex_ + 1) + "/" + String(folderModels_.Size()) + ": " + fname + " [keep]");
    if (statusText_) statusText_->SetText("Flagged KEEP: " + fname);
}

void ModelViewer::BrowseFlagReject()
{
    if (folderModels_.Empty()) return;
    String fname = GetFileName(folderModels_[browseIndex_]);
    folderReview_[fname] = "reject";
    WriteFolderReview();

    if (browseStatusText_)
        browseStatusText_->SetText(String(browseIndex_ + 1) + "/" + String(folderModels_.Size()) + ": " + fname + " [reject]");
    if (statusText_) statusText_->SetText("Flagged REJECT: " + fname);
}

void ModelViewer::WriteFolderReview()
{
    if (browseFolderPath_.Empty()) return;

    String reviewPath = browseFolderPath_ + "folder_review.txt";
    File file(context_, reviewPath, FILE_WRITE);
    if (!file.IsOpen())
    {
        URHO3D_LOGERRORF("Cannot write review file: %s", reviewPath.CString());
        return;
    }

    for (HashMap<String, String>::ConstIterator it = folderReview_.Begin(); it != folderReview_.End(); ++it)
    {
        file.WriteString(it->first_ + " | " + it->second_ + "\n");
    }
}
