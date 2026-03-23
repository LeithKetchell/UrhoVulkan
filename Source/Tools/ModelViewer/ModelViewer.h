#pragma once

#include <Urho3D/Engine/Application.h>
#include <Urho3D/Graphics/AnimatedModel.h>
#include <Urho3D/Graphics/BillboardSet.h>
#include <Urho3D/Graphics/AnimationController.h>
#include <Urho3D/Graphics/AnimationState.h>
#include <Urho3D/Graphics/DebugRenderer.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/FileSelector.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/ListView.h>
#include <Urho3D/UI/ScrollView.h>
#include <Urho3D/UI/Slider.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/CheckBox.h>
#include <Urho3D/UI/Window.h>

using namespace Urho3D;

class ModelViewer : public Application
{
    URHO3D_OBJECT(ModelViewer, Application);

public:
    explicit ModelViewer(Context* context);

    void Setup() override;
    void Start() override;
    void Stop() override;

private:
    // Scene
    void CreateScene();
    void SetupViewport();

    // Model
    void LoadModel(const String& path);
    void AutoFrameCamera();
    void ScanAnimations();
    void PlayAnimation(int index);
    void AddBlendAnimation(int index);
    void RemoveBlendAnimation(unsigned index);
    void RebuildActiveAnimDisplay();

    // UI
    void CreateUI();
    void CreateMenuBar();
    void CreateInfoPanel();
    void CreatePlaybackPanel();
    void RebuildInfoText();
    void RebuildAnimList();
    UIElement* CreateCollapsibleSection(const String& title, bool startExpanded);
    void AddInfoLine(const String& text, const Color& color = Color(0.85f, 0.85f, 0.85f));
    void AddInfoSeparator();
    void ShowHelpWindow();

    // Camera
    void UpdateCamera(float timeStep);

    // Events
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);
    void HandleMouseMove(StringHash eventType, VariantMap& eventData);
    void HandleMouseWheel(StringHash eventType, VariantMap& eventData);
    void HandleFileOpen(StringHash eventType, VariantMap& eventData);
    void HandleAnimSlider(StringHash eventType, VariantMap& eventData);
    void HandleSpeedSlider(StringHash eventType, VariantMap& eventData);
    void HandleAnimSelected(StringHash eventType, VariantMap& eventData);

    // Helpers
    String SemanticName(VertexElementSemantic sem);
    String TypeName(VertexElementType type);

    // Scene
    SharedPtr<Scene> scene_;
    Node* modelNode_{};
    Node* cameraNode_{};
    Node* lightNode_{};

    // Model state
    SharedPtr<Model> currentModel_;
    StaticModel* staticModelComp_{};
    AnimatedModel* animatedModelComp_{};
    AnimationController* animController_{};
    bool isAnimated_{};
    String currentModelPath_;
    Vector<String> availableAnims_;
    int currentAnimIndex_{-1};
    bool animPlaying_{};

    // Camera
    float cameraDistance_{5.0f};
    float cameraYaw_{45.0f};
    float cameraPitch_{30.0f};
    Vector3 modelCenter_;

    // Debug overlays
    bool showWireframe_{};
    bool showSkeleton_{};
    bool showBoundingBox_{};

    // Bone picking & inspector
    int selectedBone_{-1};
    void PickBone(int screenX, int screenY);
    void DrawBoneSubtree(DebugRenderer* debug, const Skeleton& skel, unsigned boneIndex, const Color& color);
    void SelectBone(int boneIndex);
    void UpdateBonePopover();
    void HandleBoneListClick(StringHash eventType, VariantMap& eventData);
    Window* bonePopover_{};
    Vector<Button*> boneButtons_;

    // Vertex editor
    void EnterVertexEditMode();
    void ExitVertexEditMode();
    void DrawVertexOverlay(DebugRenderer* debug);
    void PickVertex(int screenX, int screenY);
    void DeleteSelectedVertex();
    void MoveSelectedVertex(int screenX, int screenY);
    void SaveModel();
    Vector3 GetVertexWorldPosition(int geomIndex, int vertIndex);
    Vector3 SkinVertex(const unsigned char* vertData, unsigned stride, unsigned posOffset,
        unsigned weightOffset, unsigned indexOffset, bool hasSkinning, unsigned vertIndex,
        const Vector<Bone>& bones, const Matrix3x4& worldTransform);
    void UpdateVertexStatusText();

    bool vertexEditMode_{};
    SharedPtr<Model> editModel_;
    int selectedGeometry_{-1};
    int selectedVertex_{-1};
    bool vertexDirty_{};
    bool vertexDragging_{};
    Vector3 dragStartPos_;
    Vector3 dragPlaneNormal_;
    Node* vertexOverlayNode_{};
    BillboardSet* vertexBillboards_{};

    // UI
    SharedPtr<Font> font_;
    Window* infoPanel_{};
    ScrollView* infoScroll_{};
    UIElement* infoContent_{};
    UIElement* currentSection_{};
    Window* playbackPanel_{};
    ListView* animListView_{};
    Slider* animSlider_{};
    Slider* speedSlider_{};
    Text* animTimeText_{};
    Text* animSpeedText_{};
    Text* statusText_{};
    Text* animNameText_{};
    SharedPtr<FileSelector> fileSelector_;
    Window* helpWindow_{};
    float animSpeed_{1.0f};
    bool suppressAnimSelect_{};
    bool animLooped_{true};
    bool animReversed_{false};
    CheckBox* loopCheck_{};
    Text* reverseLabel_{};

    // Multi-animation blending
    struct ActiveAnim {
        String name;
        unsigned char layer;
        float weight{1.0f};
        AnimationBlendMode blendMode{ABM_LERP};
    };
    Vector<ActiveAnim> activeAnims_;
    UIElement* activeAnimContainer_{};
};
