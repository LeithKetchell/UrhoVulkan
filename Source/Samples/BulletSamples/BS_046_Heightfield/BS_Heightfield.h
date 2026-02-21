#pragma once

#include "Sample.h"

namespace Urho3D
{
class Node;
class Scene;
}

/// Heightfield terrain collision example - faithful port of Bullet SDK HeightfieldExample
class BS_Heightfield : public Sample
{
    URHO3D_OBJECT(BS_Heightfield, Sample);

public:
    /// Construct.
    explicit BS_Heightfield(Context* context);

    /// Setup after engine initialization and before running the main loop.
    void Start() override;

private:
    /// Create the scene content.
    void CreateScene();
    /// Setup the viewport.
    void SetupViewport();
    /// Subscribe to application-wide logic update events.
    void SubscribeToEvents();
    /// Handle the logic update event.
    void HandleUpdate(StringHash eventType, VariantMap& eventData);
    /// Handle the post-render update event for debug drawing.
    void HandlePostRenderUpdate(StringHash eventType, VariantMap& eventData);

};
