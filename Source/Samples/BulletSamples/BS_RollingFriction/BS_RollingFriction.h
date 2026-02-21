#pragma once

#include "Sample.h"

/// Bullet SDK RollingFrictionDemo — 125 shapes on a slope.
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
};
