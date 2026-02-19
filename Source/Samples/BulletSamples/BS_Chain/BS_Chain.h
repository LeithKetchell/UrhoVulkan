#pragma once

#include "Sample.h"

/// Chain bridge — linked rigid bodies with hinge constraints.
class BS_Chain : public Sample
{
    URHO3D_OBJECT(BS_Chain, Sample);

public:
    explicit BS_Chain(Context* context) : Sample(context) {}

    void Start() override;

private:
    void CreateScene();
    void SetupViewport();
    void SubscribeToEvents();
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

    bool drawDebug_{true};
};
