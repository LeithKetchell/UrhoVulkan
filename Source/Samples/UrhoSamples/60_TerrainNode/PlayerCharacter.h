// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include <Urho3D/Input/Controls.h>
#include <Urho3D/Scene/LogicComponent.h>

using namespace Urho3D;

// Control bits
const unsigned CTRL_FORWARD = 1;
const unsigned CTRL_BACK = 2;
const unsigned CTRL_LEFT = 4;
const unsigned CTRL_RIGHT = 8;
const unsigned CTRL_JUMP = 16;
const unsigned CTRL_SPRINT = 32;

// Movement tuning
const float MOVE_FORCE = 0.8f;
const float INAIR_MOVE_FORCE = 0.02f;
const float BRAKE_FORCE = 0.2f;
const float JUMP_FORCE = 7.0f;
const float INAIR_THRESHOLD_TIME = 0.1f;
const float SPRINT_MULTIPLIER = 1.6f;

// Swimming tuning
const float SWIM_FORCE = 0.4f;
const float SWIM_BRAKE_FORCE = 0.15f;
const float BUOYANCY_FORCE = 0.3f;
const float WATER_LEVEL = 5.0f;

/// Player character component. Drives physics-based movement from Controls input.
/// Designed as a capsule placeholder — add AnimationController calls when swapping to a rigged model.
class PlayerCharacter : public LogicComponent
{
    URHO3D_OBJECT(PlayerCharacter, LogicComponent);

public:
    explicit PlayerCharacter(Context* context);

    static void RegisterObject(Context* context);

    void Start() override;
    void FixedUpdate(float timeStep) override;

    /// Movement controls. Set by the owning client or server each frame.
    Controls controls_;

    /// Current stamina (0-100). Read by TerrainNode for HUD.
    float GetStamina() const { return stamina_; }
    float GetMaxStamina() const { return maxStamina_; }
    bool IsSprinting() const { return sprinting_; }

private:
    void HandleNodeCollision(StringHash eventType, VariantMap& eventData);

    bool onGround_;
    bool okToJump_;
    bool inWater_;
    bool sprinting_;
    float inAirTimer_;

    // Stamina
    float stamina_;
    float maxStamina_;
    static constexpr float STAMINA_SPRINT_COST = 5.0f;   // per second
    static constexpr float STAMINA_SWIM_COST = 4.0f;     // per second
    static constexpr float STAMINA_JUMP_COST = 8.0f;     // per jump
    static constexpr float STAMINA_REGEN_RATE = 2.0f;    // per second when idle
};
