// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include <Urho3D/Math/Plane.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Menu.h>
#include <Urho3D/UI/Window.h>
#include <Urho3D/UI/DockManager.h>
#include <Urho3D/UI/DockPanel.h>
#include <Urho3D/UI/DockSplit.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/ListView.h>
#include <Urho3D/UI/CheckBox.h>
#include <Urho3D/UI/ScrollView.h>
// HttpRequest removed — Melbourne time computed locally via tzdata
#include <Urho3D/Network/Network.h>
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Network/Connection.h>
#include <Urho3D/UI/FileSelector.h>

#include "Sample.h"
#include "OOFO.h"
#include <Urho3D/Graphics/ParticleEmitter.h>
#include <Urho3D/Graphics/ParticleEffect.h>
#include "PlayerCharacter.h"
#include "Rabbit.h"
#include "Deer.h"
#include "Fox.h"
#include "Fish.h"
#include "SchoolFish.h"
#include "GrassSystem.h"
#include <Urho3D/Graphics/TerrainBrush.h>
#include <Urho3D/Graphics/ProfilerUI.h>

namespace Urho3D
{

class Node;
class Scene;

}

/// Weather state for atmospheric effects.
struct WeatherState
{
    float cloudCover{};       // 0.0 clear → 1.0 overcast
    float precipitation{};    // 0.0 none → 1.0 heavy
    float windSpeed{};        // 0.0 calm → 1.0 gale
    float windAngle{};        // radians, world-space direction
    float fogDensity{1.0f};   // multiplier on zone fog distance (lower = thicker)
    float ambientDim{1.0f};   // multiplier on zone ambient color

    WeatherState Lerp(const WeatherState& target, float t) const
    {
        WeatherState result;
        result.cloudCover = cloudCover + (target.cloudCover - cloudCover) * t;
        result.precipitation = precipitation + (target.precipitation - precipitation) * t;
        result.windSpeed = windSpeed + (target.windSpeed - windSpeed) * t;
        result.windAngle = windAngle + (target.windAngle - windAngle) * t;
        result.fogDensity = fogDensity + (target.fogDensity - fogDensity) * t;
        result.ambientDim = ambientDim + (target.ambientDim - ambientDim) * t;
        return result;
    }
};

/// Undo/redo action record.
struct UndoAction
{
    enum Type { NODE_CREATE, NODE_DELETE, TERRAIN_EDIT };
    Type type;
    // Node actions
    unsigned nodeID{};
    String xmlData;
    Vector3 position;
    Quaternion rotation;
    // Terrain actions — full heightmap snapshots
    SharedPtr<Image> beforeHM;
    SharedPtr<Image> afterHM;
};

/// TerrainNode example with dropdown menus, terrain editing, and celestial day/night cycle.
class TerrainNode : public Sample
{
    URHO3D_OBJECT(TerrainNode, Sample);

public:
    explicit TerrainNode(Context* context);

    void Start() override;
    void Stop() override;
    bool OnEscapePressed() override;

private:
    // --- Login screen ---
    void CreateLoginScene();
    void CreateLoginUI();
    void DestroyLoginUI();
    void EnterWorld();
    void FinishEnterWorld();
    void ReturnToLogin();
    void HandleLoginButton(StringHash eventType, VariantMap& eventData);
    void HandleRegisterButton(StringHash eventType, VariantMap& eventData);
    void HandleOfflineButton(StringHash eventType, VariantMap& eventData);

    Window* loginWindow_{};
    LineEdit* usernameEdit_{};
    LineEdit* passwordEdit_{};
    Button* loginBtn_{};
    Button* registerBtn_{};
    Text* loginStatusText_{};
    String loggedInUsername_;
    String serverSceneName_;  // scene filename received from AuthServer
    bool loggedIn_{false};
    bool asyncSceneLoading_{false};  // true while waiting for async scene load
    bool loginTimerActive_{false};   // debug: timing login-to-render
    bool firstFramePending_{false};  // debug: waiting for first rendered frame
    HiresTimer loginTimer_;          // debug: measures login-to-render time
    int adminLevel_{0};       // admin level granted by AuthServer (0 = regular user)
    int ownedPatchX_{};       // first/home patch coordinates (for camera positioning)
    int ownedPatchZ_{};
    Vector<IntVector2> ownedPatches_;  // all patches owned by this user
    bool OwnsThisPatch(int px, int pz) const;
    SharedPtr<Text> loadingText_;    // "Loading..." overlay during async scene load

    void CreateScene();
    void OnSceneLoaded();
    void CreateInstructions();
    void SetupViewport();
    void SubscribeToEvents();
    void MoveCamera(float timeStep);
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

    // --- Menu bar ---
    void CreateMenuBar();
    DropDownList* CreateMenuDropdown(const String& label, const Vector<String>& items);
    void HandleFileMenu(StringHash eventType, VariantMap& eventData);
    void HandleEditMenu(StringHash eventType, VariantMap& eventData);
    void HandleViewMenu(StringHash eventType, VariantMap& eventData);
    void HandleEnvironmentAction(StringHash eventType, VariantMap& eventData);
    void HandleFishWiggleSlider(StringHash eventType, VariantMap& eventData);
    void HandleFishSpeedSlider(StringHash eventType, VariantMap& eventData);
    void HandleMenuButton(StringHash eventType, VariantMap& eventData);
    void HandleCollapsibleToggle(StringHash eventType, VariantMap& eventData);
    void CreateTerrainPanel();
    void ToggleTerrainPanel();
    Menu* CreateMenuItem(UIElement* parent, const String& text, int actionId);
    UIElement* CreateCollapsibleSection(UIElement* parent, Font* font, const String& title, bool expanded);
    SharedPtr<Texture2D> GenerateShapeIcon(int shape);

    UIElement* menuBar_{};
    SharedPtr<DockManager> dockManager_;
    SharedPtr<DropDownList> fileMenu_;
    SharedPtr<DropDownList> editMenu_;
    SharedPtr<DropDownList> viewMenu_;
    Menu* environmentMenu_{};
    SharedPtr<Window> terrainPanel_;
    Text* fishWiggleLabel_{};
    Text* fishSpeedLabel_{};

    // --- Patch boundaries ---
    void CreatePatchBoundary(Node*& node, int patchX, int patchZ, const Color& color);
    void CreateOwnedPatchBoundaries();
    void UpdateCurrentPatchBoundary();
    Vector<Node*> ownedPatchBoundaries_;
    Node* currentPatchBoundary_{};
    int currentPatchX_{0x7FFFFFFF};  // force first update
    int currentPatchZ_{0x7FFFFFFF};

    // --- Camera transition (edit mode) ---
    void BeginEditTransition();   // chase/FP → god cam for editing
    void EndEditTransition();     // god cam → back to previous mode
    void UpdateCameraTransition(float timeStep);
    bool cameraTransitioning_{false};
    bool transitionToEdit_{true};  // true=entering edit, false=leaving
    float transitionTime_{0.0f};
    float transitionDuration_{0.75f};
    Vector3 transitionStartPos_;
    Vector3 transitionEndPos_;
    Quaternion transitionStartRot_;
    Quaternion transitionEndRot_;
    CameraMode preEditCameraMode_{CAM_GOD};  // mode to restore after editing

    // --- Terrain editing ---
    void ApplyBrush(const Vector3& worldPos, float timeStep);
    void ApplyLowerBrush(const Vector3& worldPos, float timeStep);
    void DrawBrushOutline(const Vector3& worldPos);

    SharedPtr<TerrainBrush> brush_;
    SharedPtr<Image> editableHeightMap_;
    SharedPtr<Image> waterMap_;
    SharedPtr<Texture2D> waterMapTex_;
    int brushMode_{0};         // 0=off, 1=raise, 2=lower, 3=smooth, 4=flatten, 5=erosion, 6=river
    int brushShape_{0};        // 0=circle, 1=square, 2=triangle, 3=star, 4=pentagon, 5=hexagon, 6=octagon
    float brushRotation_{0.0f}; // degrees
    float brushRadius_{5.0f};
    float brushStrength_{0.05f};
    float smoothStrength_{0.3f};
    Vector3 cachedBrushHit_;
    Vector3 cachedBrushNormal_{Vector3::UP};
    bool hasBrushHit_{false};
    Text* brushSizeLabel_{};
    Text* brushStrLabel_{};
    Text* brushRotLabel_{};
    LineEdit* brushRotEdit_{};
    void HandleBrushRotEdit(StringHash eventType, VariantMap& eventData);
    float lockedFlattenHeight_{-1.0f};  // locked on first click, reset on release
    BorderImage* shapeIcons_[7]{};     // shape icon images for rotation updates
    Button* activeModeBtn_{};           // currently selected mode button
    Button* activeShapeBtn_{};          // currently selected shape button
    void HighlightBrushButton(Button*& active, Button* btn);

    // --- Weather system ---
    void UpdateWeather(float timeStep);
    void ApplyWeatherToScene();
    WeatherState CalculateSeasonalWeather();
    void HandleWeatherSlider(StringHash eventType, VariantMap& eventData);

    // --- Celestial bodies ---
    void CreateCelestialBodies();
    void UpdateCelestialBodies(float timeStep);
    void UpdateAtmosphere(float sunAltitude);
    void UpdateSeasonalEffects();
    float CalculateSunAltitude();
    float CalculateSunAzimuth(float altitude);
    float CalculateMoonAltitude();
    float CalculateMoonAzimuth(float moonAlt);
    Vector3 AltAzToFlatEarth(float altitude, float azimuth, float distance = 500.0f);

    Node* sunNode_{};
    Node* moonNode_{};
    SharedPtr<Material> sunMat_;
    SharedPtr<Material> moonMat_;
    float cloudAngle_{};
    Light* sunLight_{};
    Light* moonLight_{};
    SharedPtr<Material> skyboxMat_;

    // --- Melbourne time (computed locally via tzdata) ---
    void SyncMelbourneTime();
    void HandleTimeOfDaySlider(StringHash eventType, VariantMap& eventData);
    void HandleDateOffsetSlider(StringHash eventType, VariantMap& eventData);
    void ApplyDateOffsets();
    float timeOfDay_{};
    float timeOfDayOffset_{};  // manual offset from Melbourne time (-12..+12 hours)
    int dayOfYear_{};
    int baseDayOfYear_{};      // from Melbourne time, before offsets
    float baseMoonAge_{};      // from Melbourne time, before offsets
    float moonAge_{};
    float daySliderOffset_{};    // ±15 days
    float monthSliderOffset_{};  // ±6 months (in days)
    float yearSliderOffset_{};   // ±15 years (in days)
    float timeSyncTimer_{};
    Text* todLabel_{};
    Text* dayOffsetLabel_{};
    Text* monthOffsetLabel_{};
    Text* yearOffsetLabel_{};

    // --- Cached celestial calculations ---
    float cachedSunAlt_{};
    float cachedSunAz_{};
    float cachedMoonAlt_{};
    float cachedMoonAz_{};

    // --- Selection ---
    void SelectNode(Node* node);
    void DeselectNode();
    WeakPtr<Node> selectedNode_;
    Vector<SharedPtr<Material>> originalMaterials_;

    // --- Undo/Redo ---
    void PushUndo(const UndoAction& action);
    void Undo();
    void Redo();
    void BeginTerrainStroke();
    void EndTerrainStroke();
    String SerializeNode(Node* node);
    SharedPtr<Image> CloneHeightMap();
    void RestoreHeightMap(Image* src);
    Vector<UndoAction> undoStack_;
    int undoCursor_{0};
    bool terrainStrokeActive_{false};
    SharedPtr<Image> terrainStrokeBefore_;

    // --- Transform gizmo ---
    void DrawGizmo();
    void BeginGizmoDrag(int axis);
    void UpdateGizmoDrag();
    void EndGizmoDrag();
    void WakeSelectedNode();
    int gizmoMode_{0};        // 0=none, 1=translate, 2=rotate, 3=scale
    bool gizmoLocal_{false};  // false=world, true=local
    bool gizmoDragging_{false};
    int gizmoAxis_{-1};       // 0=X, 1=Y, 2=Z
    Vector3 gizmoDragStart_;  // mouse ray hit at drag start
    Vector3 gizmoNodeStart_;  // node position at drag start
    Quaternion gizmoRotStart_; // node rotation at drag start
    Vector3 gizmoScaleStart_; // node scale at drag start

    // --- Import / Generate ---
    void ShowImportModelDialog();
    void HandleImportModelChosen(StringHash eventType, VariantMap& eventData);
    void ShowGenerateMeshPanel();
    void HandleGenerateMesh(StringHash eventType, VariantMap& eventData);
    void HandleMeshShapeChanged(StringHash eventType, VariantMap& eventData);
    SharedPtr<Window> generateMeshPanel_;
    DropDownList* meshShapeList_{};
    Slider* meshParam1Slider_{};
    Slider* meshParam2Slider_{};
    Slider* meshParam3Slider_{};
    Text* meshParam1Label_{};
    Text* meshParam2Label_{};
    Text* meshParam3Label_{};
    DropDownList* meshMaterialList_{};
    LineEdit* meshNameEdit_{};

    // --- Hierarchy window ---
    void ToggleHierarchyWindow();
    void BuildHierarchyTree();
    void PopulateHierarchy(Node* node, Text* parentItem, unsigned& index);
    void HandleHierarchySelectionChanged(StringHash eventType, VariantMap& eventData);
    void HandleHierarchyDoubleClick(StringHash eventType, VariantMap& eventData);
    void HighlightInHierarchy(Node* node);
    SharedPtr<Window> hierarchyWindow_;
    ListView* hierarchyList_{};

    // --- Inspector window ---
    void ToggleInspectorWindow();
    void RebuildInspector();
    void CreateNodeSection(Node* node);
    void CreateComponentSection(Component* component, unsigned compIndex);
    LineEdit* CreateVec3Row(UIElement* parent, const String& label, const Vector3& value, const String& tag);
    void HandleInspectorTransformEdit(StringHash eventType, VariantMap& eventData);
    void HandleInspectorAttributeEdit(StringHash eventType, VariantMap& eventData);
    void HandleInspectorCheckToggle(StringHash eventType, VariantMap& eventData);
    void HandleInspectorEnumSelect(StringHash eventType, VariantMap& eventData);
    SharedPtr<Window> inspectorWindow_;
    ScrollView* inspectorScroll_{};
    UIElement* inspectorContent_{};

    // --- Debug Log window ---
    void CreateDebugLogWindow();
    void ToggleDebugLogWindow();
    void HandleLogMessage(StringHash eventType, VariantMap& eventData);
    SharedPtr<Window> debugLogWindow_;
    ListView* debugLogList_{};
    static const unsigned MAX_DEBUG_LOG_LINES = 500;

    // --- Font / Theme ---
    void LoadThemePrefs();
    void SaveThemePrefs();
    void ApplyFont(const String& fontName, int fontSize);
    void HandleSettingsMenu(StringHash eventType, VariantMap& eventData);
    SharedPtr<Font> font_;
    String currentFontName_{"Anonymous Pro"};
    int currentFontSize_{11};
    Vector<String> availableFonts_;
    SharedPtr<DropDownList> settingsMenu_;

    // --- State ---
    Text* instructionText_{};
    Text* cameraCoordsText_{};
    Text* clockText_{};
    bool menuOpen_{false};
    int heightFogOverride_{0};  // 0=auto (time-based), 1=forced on, -1=forced off
    bool godRaysEnabled_{true};

    // --- Weather state ---
    WeatherState weather_;              // current interpolated state
    WeatherState weatherTarget_;        // what we're transitioning toward
    float weatherTransitionTimer_{};    // seconds remaining in current transition
    float weatherTransitionDuration_{60.0f}; // total transition time (game-seconds)
    float weatherCheckTimer_{};         // countdown to next weather re-evaluation
    float weatherCheckInterval_{300.0f}; // re-evaluate every 5 game-minutes
    bool weatherOverride_{false};       // true = manual slider control, false = automatic
    Text* cloudCoverLabel_{};
    Text* precipLabel_{};
    Text* rainLabel_{};
    float rainOverride_{-1.0f};  // -1 = auto (follows precipitation), 0..1 = manual override

    // --- Cached seasonal state (only recomputed when dayOfYear_ changes) ---
    int lastSeasonDay_{-1};
    float cachedSeasonFactor_{};
    Color cachedSeasonBias_;
    float cachedFogStart_{500.0f};
    float cachedFogEnd_{750.0f};
    Color cachedTerrainTint_{Color::WHITE};
    Color cachedShallowColor_;
    Color cachedDeepColor_;
    // Seasonal skybox cubemaps: spring=0, summer=1, autumn=2, winter=3
    SharedPtr<TextureCube> seasonSkyboxes_[4];
    int lastSeasonIndex_{-1};

    // --- Weather system (Phase 1: cloud density sampling) ---
    float SampleCloudDensity(const Vector3& worldPos);
    void UpdateLocalWeather(float timeStep);
    SharedPtr<Image> cloudFaces_[6];   // BrightDay1 cubemap faces for CPU sampling
    float localCloudDensity_{};        // 0.0 = clear, 1.0 = thick cloud

    // --- Lightning ---
    void UpdateLightning(float timeStep);
    float lightningTimer_{};           // countdown to next strike chance
    float lightningIntensity_{};       // current flash brightness (0=off, 1=peak)
    int lightningFlickerCount_{};      // remaining flickers in current strike
    float lightningFlickerPhase_{};    // time within current flicker on/off cycle
    bool lightningFlickerOn_{};        // current flicker state
    float lightningFadeTimer_{};       // post-flash fade duration
    float lightningAfterDark_{};       // pupil contraction darkening (0=none, peaks ~0.15)

    // --- Heightmap I/O ---
    void ShowExportPrefabDialog();
    void HandlePrefabExportChosen(StringHash eventType, VariantMap& eventData);
    void ShowLoadPrefabDialog();
    void HandlePrefabLoadChosen(StringHash eventType, VariantMap& eventData);
    SharedPtr<Node> prefabBrush_;   // loaded prefab template, cloned on each instance
    void HandleRigidBodySleep(StringHash eventType, VariantMap& eventData);
    Text* prefabBrushLabel_{};      // "Object Brush: X" label in File menu
    void UpdatePrefabBrushLabel();
    void ShowSaveSceneDialog();
    void ShowLoadSceneDialog();
    void HandleSceneSaveChosen(StringHash eventType, VariantMap& eventData);
    void HandleSceneLoadChosen(StringHash eventType, VariantMap& eventData);
    void ShowSaveHeightmapDialog();
    void ShowLoadHeightmapDialog();
    void HandleHeightmapSaveChosen(StringHash eventType, VariantMap& eventData);
    void HandleHeightmapLoadChosen(StringHash eventType, VariantMap& eventData);
    void HandleFileSelectorCancel(StringHash eventType, VariantMap& eventData);
    void SaveHeightmapToFile(const String& path);
    void LoadHeightmapFromFile(const String& path);
    SharedPtr<FileSelector> fileSelector_;

    // --- Minimap ---
    void CreateMinimap();
    void UpdateMinimapCamera();
    BorderImage* minimap_{};
    BorderImage* minimapCameraDot_{};
    SharedPtr<Texture2D> minimapTex_;
    SharedPtr<Node> minimapCameraNode_;

    // --- Compute shader / Erosion ---
    void TestComputeShader();
    void RunErosion(int iterations);
    void WakeSleepingBodiesOnTerrain();
    void HandleErosionSlider(StringHash eventType, VariantMap& eventData);
    int erosionIterations_{50};
    float erosionRainfall_{0.012f};
    float erosionStrength_{0.3f};  // Ks (erosion rate)
    float erosionMaxDepth_{0.5f};  // max carve depth (fraction of original height)
    float erosionMinHeight_{0.0f}; // absolute minimum height floor [0..1]
    float erosionRidgeProtect_{0.0f}; // ridge protection strength [0..1]
    int erosionBorderPad_{4};      // no-erode border in cells
    Text* erosionIterLabel_{};
    Text* erosionRainLabel_{};
    Text* erosionStrLabel_{};
    Text* erosionDepthLabel_{};
    Text* erosionFloorLabel_{};
    Text* erosionRidgeLabel_{};
    Text* erosionBorderLabel_{};

    // --- Animals ---
    void CreateAnimals();
    Vector<WeakPtr<Node>> animalNodes_;

    // --- Rain particles ---
    void CreateRain();
    void UpdateRain(float timeStep);
    SharedPtr<Node> rainNode_;
    ParticleEmitter* rainEmitter_{};
    SharedPtr<ParticleEffect> rainEffect_;

    // --- Snow particles ---
    void CreateSnow();
    void UpdateSnow(float timeStep);
    float CalculateTemperature() const;
    SharedPtr<Node> snowNode_;
    ParticleEmitter* snowEmitter_{};
    SharedPtr<ParticleEffect> snowEffect_;

    // --- Grass (GPU-driven) ---
    void CreateGrass();
    SharedPtr<class GrassSystem> grassSystem_;

    // --- Fish ---
    void CreateFish();
    void CreateSchoolFish();

    // --- Campfire ---
    void CreateCampfire();
    Node* campfireNode_{};

    // --- OOFO fleet ---
    void CreateOOFOs();
    void UpdateOOFOs(float timeStep);
    static const int NUM_OOFOS = 3;
    Vector<SharedPtr<OOFO>> oofos_;
    Vector<Vector3> oofoCloudPositions_;
    float oofoSpawnTimers_[3]{};  // countdown per OOFO before spawning
    int oofosSpawned_{0};
    bool oofoRayVisible_{false};
    bool landAnimalRayVisible_{false};
    bool waterAnimalRayVisible_{false};
    bool fireRayVisible_{false};
    bool grassRayVisible_{false};

    // --- AuthServer discovery & connection ---
    void DiscoverAuthServer();
    void HandleHostDiscovered(StringHash eventType, VariantMap& eventData);
    void HandleServerConnected(StringHash eventType, VariantMap& eventData);
    void HandleServerDisconnected(StringHash eventType, VariantMap& eventData);
    void HandleConnectFailed(StringHash eventType, VariantMap& eventData);
    void HandleAuthMessage(StringHash eventType, VariantMap& eventData);
    void HandleAsyncLoadFinished(StringHash eventType, VariantMap& eventData);
    void HandleAuthConnectButton(StringHash eventType, VariantMap& eventData);
    void UpdateAuthButtonState();
    String authServerAddress_{"127.0.0.1"};
    unsigned short authServerPort_{9090};
    float discoveryTimer_{0.0f};
    float discoveryInterval_{3.0f};  // retry every 3 seconds
    bool authConnected_{false};
    bool authDiscovering_{true};
    Button* authConnectBtn_{};
    Text* authBtnLabel_{};
    Text* networkStatusText_{};
    /// Pending registration credentials (sent after plain DH connect completes).
    String pendingRegisterUsername_;
    String pendingRegisterPassword_;

    // --- Peer offload (NAT punchthrough) ---
    void HandlePeerConnected(StringHash eventType, VariantMap& eventData);
    void HandleNATPunchFailed(StringHash eventType, VariantMap& eventData);
    void HandlePeerDisconnected(StringHash eventType, VariantMap& eventData);
    void RegisterGuidWithAuthServer();
    void RelayToAuth(int innerMsgID, const VectorBuffer& innerPayload);

    bool isPeered_{false};
    bool isSubServer_{false};
    String peerGuid_;
    Vector<unsigned char> peerToken_;
    int peerPatchX_{0};
    int peerPatchZ_{0};
    SharedPtr<Connection> peerConnection_;

    // --- Server-authoritative edits ---
    void SendTerrainEdit(const Vector3& worldPos, float timeStep);
    void SendObjectCreate(Node* node, const Vector3& position, const Vector3& surfaceNormal);
    void SendObjectDelete(unsigned nodeID);
    void SendObjectTransform(unsigned nodeID, const Vector3& pos, const Quaternion& rot, const Vector3& scale);
    void SendEditMessage(int msgID, const VectorBuffer& payload);
    void HandleEditReject(MemoryBuffer& msg);
    void HandleEditBroadcast(MemoryBuffer& msg);
    void HandleResourcePatch(MemoryBuffer& msg);
    void SendPatchPosition(int patchX, int patchZ);
    int lastReportedPatchX_{0x7FFFFFFF};
    int lastReportedPatchZ_{0x7FFFFFFF};

    /// Terrain edit snapshot for rollback — stores small heightmap region keyed by editID
    struct TerrainEditSnapshot
    {
        IntVector2 regionMin;   // top-left in heightmap coords
        IntVector2 regionSize;  // width x height of snapshotted region
        Vector<unsigned char> heightData;  // raw pixel data of the region before edit
    };
    unsigned nextEditID_{1};
    HashMap<unsigned, TerrainEditSnapshot> terrainEditSnapshots_;
    /// Object edit snapshot for rollback
    struct ObjectEditSnapshot
    {
        unsigned char subtype;  // 0=create, 1=delete, 2=transform
        unsigned nodeID;
        String xmlData;         // serialized node (for create rollback = delete, delete rollback = recreate)
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
    };
    HashMap<unsigned, ObjectEditSnapshot> objectEditSnapshots_;
    float editSendAccumulator_{0.0f};
    static constexpr float EDIT_SEND_INTERVAL = 0.1f;  // 100ms rate limit
    bool replayingBroadcast_{false};  // true while replaying a remote edit — suppresses re-send

    // --- Player avatar & camera modes ---
    Node* CreatePlayerAvatar();
    void UpdateCharacterCamera();
    void HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData);
    void HandleClientConnected(StringHash eventType, VariantMap& eventData);
    void HandleClientDisconnected(StringHash eventType, VariantMap& eventData);
    void HandleClientObjectID(StringHash eventType, VariantMap& eventData);

    // cameraMode_ is inherited from Sample (CAM_GOD, CAM_CHASE, CAM_FIRSTPERSON)
    unsigned clientObjectID_{};
    WeakPtr<Node> characterNode_;
    HashMap<Connection*, WeakPtr<Node>> serverObjects_;

    // --- Water / rendering ---
    SharedPtr<Node> reflectionCameraNode_;
    SharedPtr<Node> waterNode_;
    Plane waterPlane_;
    Plane waterClipPlane_;
    SharedPtr<ProfilerUI> profilerUI_;
    RenderPath* renderPath_{};
    RenderPath* reflectionRenderPath_{};
    WeakPtr<Zone> zone_;
    WeakPtr<Terrain> terrain_;
    Color origFogColor_;
    float origFogStart_{};
    float origFogEnd_{};

    // --- Water droplets post-process ---
    bool wasUnderwater_{false};
    float breachTime_{-100.0f};
    float splashTimer_{0.0f};       // countdown to next splash burst
    float splashInterval_{1.2f};    // seconds between splash bursts

    // --- Survival HUD (driven by MSG_VITAL_UPDATE from AuthServer) ---
    void HandleVitalUpdate(MemoryBuffer& msg);
    void UpdateVitalBars();

    int vitalHp_{20};
    int vitalMaxHp_{20};
    float vitalHunger_{100.0f};
    float vitalThirst_{100.0f};
    float vitalStamina_{100.0f};
    float vitalWarmth_{15.0f};
    bool vitalAlive_{true};
    float vitalSpeedMult_{1.0f};

    BorderImage* hungerBar_{};
    BorderImage* hungerBarBg_{};
    BorderImage* thirstBar_{};
    BorderImage* thirstBarBg_{};
    Text* hungerLabel_{};
    Text* thirstLabel_{};

    // --- Inventory UI (driven by MSG_INVENTORY_UPDATE/DELTA from AuthServer) ---
    void HandleInventoryUpdate(MemoryBuffer& msg);
    void HandleInventoryDelta(MemoryBuffer& msg);
    void CreateInventoryUI();
    void RefreshInventoryGrid();
    void ToggleInventory();
    void SendPickup(unsigned nodeId);
    void SendDrop(int itemId, int qty);

    struct ClientInventorySlot
    {
        int itemId{};
        int quantity{};
        int durability{-1};
        String slotType;
        String itemName;
        float itemWeight{};
    };
    Vector<ClientInventorySlot> inventory_;
    float inventoryWeight_{};
    float inventoryMaxWeight_{30.0f};
    float inventoryAbsWeight_{60.0f};
    int inventoryMaxSlots_{10};
    bool inventoryOpen_{false};

    SharedPtr<Window> inventoryWindow_;
    Vector<Button*> inventorySlotButtons_;
    Text* inventoryWeightText_{};
    BorderImage* inventoryWeightBar_{};
    int selectedSlotIndex_{-1};
};
