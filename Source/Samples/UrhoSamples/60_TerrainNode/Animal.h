// Animal — base class for terrain-roaming wildlife.
// Subclasses override asset paths and behavior tuning.

#pragma once

#include <Urho3D/Scene/LogicComponent.h>
#include <Urho3D/Graphics/Terrain.h>

using namespace Urho3D;

/// Animal behavior states.
enum AnimalState
{
    ANIMAL_IDLE,
    ANIMAL_WANDER,
    ANIMAL_FLEE,
    ANIMAL_DIE
};

/// Base class for all animals. Provides state machine, terrain following, wander AI.
class Animal : public LogicComponent
{
    URHO3D_OBJECT(Animal, LogicComponent);

public:
    explicit Animal(Context* context);

    static void RegisterObject(Context* context);

    void Start() override;
    void FixedUpdate(float timeStep) override;

    /// Get current behavior state.
    AnimalState GetState() const { return state_; }

    /// Set home position (center of wander area).
    void SetHomePosition(const Vector3& pos) { homePos_ = pos; }

protected:
    // --- Subclass overrides: asset paths ---
    virtual String GetModelPath() const = 0;
    virtual String GetIdleAnim() const = 0;
    virtual String GetRunAnim() const = 0;
    virtual String GetDieAnim() const = 0;

    // --- Subclass overrides: behavior tuning ---
    /// Real-world size in metres for the longest bounding box axis.
    virtual float GetDesiredSize() const { return 1.0f; }
    virtual float GetWanderRadius() const { return 20.0f; }
    virtual float GetWanderSpeed() const { return 2.0f; }
    virtual float GetFleeSpeed() const { return 6.0f; }
    virtual float GetFleeDistance() const { return 30.0f; }
    virtual float GetMinIdleDuration() const { return 3.0f; }
    virtual float GetMaxIdleDuration() const { return 8.0f; }
    /// Seconds the animal can survive submerged before dying. Override to -1 for aquatic.
    virtual float GetDrownTime() const { return 5.0f; }

    /// Called when entering a new state. Subclasses can play extra animations.
    virtual void OnStateEnter(AnimalState newState);

    /// Transition to a new state.
    void SetState(AnimalState newState);

    /// Cached terrain pointer.
    WeakPtr<Terrain> terrain_;

    /// Current state.
    AnimalState state_{ANIMAL_IDLE};

private:
    void PickWanderTarget();
    void MoveToward(const Vector3& target, float speed, float timeStep);
    void SnapToTerrain();
    void Respawn();

    Vector3 homePos_;
    Vector3 wanderTarget_;
    float stateTimer_{0.0f};
    float drownTimer_{0.0f};
    float waterLevel_{5.5f};

    /// Terrain height cache — avoid redundant GetHeight calls.
    float cachedHeight_{0.0f};
    Vector3 cachedHeightPos_{Vector3::ZERO};

    /// Stagger ID for drowning scan distribution across frames.
    unsigned staggerID_{0};
    static unsigned nextStaggerID_;
};
