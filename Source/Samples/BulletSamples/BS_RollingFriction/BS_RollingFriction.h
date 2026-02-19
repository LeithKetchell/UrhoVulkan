#pragma once

#include "Sample.h"

/// Honest port of Bullet SDK RollingFrictionDemo.
/// 125 objects (spheres, capsules, cones, cylinders) on a sloped plane.
/// Demonstrates rolling friction, spinning friction, and anisotropic friction.
class BS_RollingFriction : public Sample
{
    URHO3D_OBJECT(BS_RollingFriction, Sample);

public:
    explicit BS_RollingFriction(Context* context) : Sample(context) {}

    void Start() override;

private:
    void CreateScene();
    void SetupViewport();
    void SubscribeToEvents();
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

    bool drawDebug_{true};
};
