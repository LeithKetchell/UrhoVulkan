// HumanNPC — task-driven human character base class.
// Inherits LandAnimal for terrain physics (snap, drowning, walls, vision)
// but replaces the random animal state machine with a priority task queue.
// CaveMan and CaveWoman inherit from this instead of LandAnimal directly.
// When possessed by a player, AI suspends and controls drive movement.

#pragma once

#include "LandAnimal.h"
#include "ResourceMap.h"
#include <Urho3D/Input/Controls.h>
#include <Urho3D/Scene/Node.h>

/// Control bit flags for possession input routing.
const unsigned CTRL_FORWARD = 1;
const unsigned CTRL_BACK = 2;
const unsigned CTRL_LEFT = 4;
const unsigned CTRL_RIGHT = 8;
const unsigned CTRL_JUMP = 16;
const unsigned CTRL_SPRINT = 32;

class HumanNPC : public LandAnimal
{
    URHO3D_OBJECT(HumanNPC, LandAnimal);

public:
    explicit HumanNPC(Context* context);

    void Start() override;
    void FixedUpdate(float timeStep) override;

    /// Set the campfire node this NPC is associated with.
    void SetCampfireNode(Node* node) { campfireNode_ = node; }

    /// Set the camera node for proximity awareness (god-cam or possessed view).
    void SetCameraNode(Node* node) { cameraNode_ = node; }

    /// Set the resource map for O(1) gather queries instead of scene-walking.
    void SetResourceMap(ResourceMap* map) { resourceMap_ = map; }

    /// Possession — when true, AI suspends and player controls drive movement.
    void SetPossessed(bool possessed);
    bool IsPossessed() const { return possessed_; }

    /// Movement controls — set by TerrainNode each frame when possessed.
    Controls controls_;

protected:
    /// Process player controls when possessed.
    void UpdatePossessedMovement(float timeStep);

    /// Process swim controls when possessed and in water.
    void UpdatePossessedSwim(float timeStep);

    /// Set position through SmoothedTransform if present, else direct.
    void SetSmoothedPosition(const Vector3& pos);

    /// True when being controlled by a player.
    bool possessed_{false};

    /// Vertical velocity for kinematic jump arc (possessed mode).
    float jumpVelocity_{0.0f};
    /// True when airborne during a possessed jump.
    bool inJump_{false};
    /// True when in water during possessed movement.
    bool possessedInWater_{false};

    /// Tracks current animation in possessed mode to avoid redundant PlayExclusive calls.
    String possessedAnim_;

    /// Distance within which the campfire provides warmth and is a valid sit target.
    float campfireRadius_{8.0f};

    /// Maximum distance to search for gatherable resources.
    float gatherSearchRadius_{25.0f};

    /// Campfire this NPC gravitates toward.
    WeakPtr<Node> campfireNode_;

    /// Resource node currently being targeted for gathering (scene-walk fallback).
    WeakPtr<Node> gatherTarget_;

    /// Resource map for O(1) spatial queries.
    WeakPtr<ResourceMap> resourceMap_;

    /// Map-based gather target position and type.
    Vector3 gatherMapPos_;
    ResourceType gatherMapType_{RES_NONE};

    /// Held item node attached to right hand bone.
    WeakPtr<Node> heldItemNode_;

    /// Camera node — set externally, used for proximity awareness (wave at god).
    WeakPtr<Node> cameraNode_;

    /// Distance at which NPC notices the camera or another NPC and may greet.
    float greetRadius_{12.0f};

    /// Cooldown after greeting to avoid spamming.
    float greetCooldown_{0.0f};
    static constexpr float GREET_COOLDOWN_TIME = 30.0f;

};
