// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include <Urho3D/Math/Plane.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/UI/Menu.h>
#include <Urho3D/UI/Window.h>
#include <Urho3D/Network/HttpRequest.h>
#include <Urho3D/UI/FileSelector.h>

#include "Sample.h"
#include <Urho3D/Graphics/ProfilerUI.h>

namespace Urho3D
{

class Node;
class Scene;

}

/// Water example with dropdown menus, terrain editing, and celestial day/night cycle.
class Water : public Sample
{
    URHO3D_OBJECT(Water, Sample);

public:
    explicit Water(Context* context);

    void Start() override;
    void Stop() override;

private:
    void CreateScene();
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
    void HandleCreateMenu(StringHash eventType, VariantMap& eventData);
    void HandleEditMenu(StringHash eventType, VariantMap& eventData);
    void HandleEnvironmentAction(StringHash eventType, VariantMap& eventData);
    void HandleMenuButton(StringHash eventType, VariantMap& eventData);
    void CreateTerrainPanel();
    void ToggleTerrainPanel();
    Menu* CreateMenuItem(UIElement* parent, const String& text, int actionId);
    SharedPtr<Texture2D> GenerateShapeIcon(int shape);

    UIElement* menuBar_{};
    SharedPtr<DropDownList> fileMenu_;
    SharedPtr<DropDownList> createMenu_;
    SharedPtr<DropDownList> editMenu_;
    Menu* environmentMenu_{};
    SharedPtr<Window> terrainPanel_;

    // --- Terrain editing ---
    void ApplyBrush(const Vector3& worldPos, float timeStep);
    void ApplyLowerBrush(const Vector3& worldPos, float timeStep);
    void DrawBrushOutline(const Vector3& worldPos);
    float BrushShapeFalloff(float dx, float dz, float radius) const;

    SharedPtr<Image> editableHeightMap_;
    int brushMode_{0};         // 0=off, 1=raise/lower, 3=smooth, 4=flatten
    int brushShape_{0};        // 0=circle, 1=square, 2=triangle, 3=star, 4=pentagon, 5=hexagon, 6=octagon
    float brushRotation_{0.0f}; // degrees
    float brushRadius_{5.0f};
    float brushStrength_{0.05f};
    float smoothStrength_{0.3f};
    Vector3 cachedBrushHit_;
    bool hasBrushHit_{false};
    Text* brushSizeLabel_{};
    Text* brushStrLabel_{};
    float lockedFlattenHeight_{-1.0f};  // locked on first click, reset on release
    BorderImage* shapeIcons_[7]{};     // shape icon images for rotation updates

    // --- Celestial bodies ---
    void CreateCelestialBodies();
    void UpdateCelestialBodies(float timeStep);
    void UpdateAtmosphere(float sunAltitude);
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

    // --- Network time ---
    void FetchNetworkTime();
    void ProcessTimeResponse();
    float timeOfDay_{};
    int dayOfYear_{};
    float moonAge_{};
    SharedPtr<HttpRequest> timeRequest_;
    float timeSyncTimer_{};

    // --- Cached celestial calculations ---
    float cachedSunAlt_{};
    float cachedSunAz_{};
    float cachedMoonAlt_{};
    float cachedMoonAz_{};

    // --- State ---
    bool menuOpen_{false};

    // --- Heightmap I/O ---
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
    void UpdateMinimapTexture();
    void UpdateMinimapCamera();
    BorderImage* minimap_{};
    BorderImage* minimapCameraDot_{};
    SharedPtr<Texture2D> minimapTex_;
    SharedPtr<Image> minimapImg_;
    SharedPtr<Image> weightMapImg_;

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

    // --- Water / rendering ---
    SharedPtr<Node> reflectionCameraNode_;
    SharedPtr<Node> waterNode_;
    Plane waterPlane_;
    Plane waterClipPlane_;
    SharedPtr<ProfilerUI> profilerUI_;
    RenderPath* renderPath_{};
    WeakPtr<Zone> zone_;
    WeakPtr<Terrain> terrain_;
    Color origFogColor_;
    float origFogStart_{};
    float origFogEnd_{};
};
