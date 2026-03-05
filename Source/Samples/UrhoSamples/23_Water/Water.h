// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include <Urho3D/Math/Plane.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/UI/DropDownList.h>
#include <Urho3D/UI/BorderImage.h>
#include <Urho3D/Network/HttpRequest.h>

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
    void HandleEnvironmentMenu(StringHash eventType, VariantMap& eventData);

    UIElement* menuBar_{};
    SharedPtr<DropDownList> fileMenu_;
    SharedPtr<DropDownList> createMenu_;
    SharedPtr<DropDownList> editMenu_;
    SharedPtr<DropDownList> environmentMenu_;

    // --- Terrain editing ---
    void ApplyBrush(const Vector3& worldPos, float timeStep);
    void ApplyLowerBrush(const Vector3& worldPos, float timeStep);
    void DrawBrushCircle(const Vector3& worldPos);

    SharedPtr<Image> editableHeightMap_;
    int brushMode_{0};         // 0=off, 1=raise/lower, 3=smooth, 4=flatten
    float brushRadius_{5.0f};
    float brushStrength_{0.05f};
    float smoothStrength_{0.3f};
    Vector3 cachedBrushHit_;
    bool hasBrushHit_{false};

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
