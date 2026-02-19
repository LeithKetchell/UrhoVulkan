#pragma once

#include "Sample.h"

/// Honest port of Bullet SDK GyroscopicDemo.
/// 4 spinning tops with different gyroscopic force modes (none, explicit,
/// implicit world, implicit body) in zero gravity.
class BS_Gyroscopic : public Sample
{
    URHO3D_OBJECT(BS_Gyroscopic, Sample);

public:
    explicit BS_Gyroscopic(Context* context) : Sample(context) {}

    void Start() override;

private:
    void CreateScene();
    void SetupViewport();
    void SubscribeToEvents();
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

    bool drawDebug_{true};
};
