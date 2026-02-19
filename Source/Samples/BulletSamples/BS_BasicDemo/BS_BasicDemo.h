#pragma once

#include "Sample.h"

/// Bullet SDK BasicExample ported to Urho3D/Vulkan.
/// Drops a 5x5x5 grid of 125 dynamic cubes onto a static ground plane.
class BS_BasicDemo : public Sample
{
    URHO3D_OBJECT(BS_BasicDemo, Sample);

public:
    explicit BS_BasicDemo(Context* context) : Sample(context) {}

    void Start() override;

private:
    void CreateScene();
    void SetupViewport();
    void SubscribeToEvents();
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

    bool drawDebug_{false};
};
