#pragma once

#include "Sample.h"

/// Bullet SDK BasicExample — 125 falling cubes.
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
};
