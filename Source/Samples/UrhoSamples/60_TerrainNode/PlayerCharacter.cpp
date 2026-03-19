// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include <Urho3D/Core/Context.h>
#include <Urho3D/IO/MemoryBuffer.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Scene/Scene.h>

#include "PlayerCharacter.h"

PlayerCharacter::PlayerCharacter(Context* context) :
    LogicComponent(context),
    onGround_(false),
    okToJump_(true),
    inAirTimer_(0.0f)
{
    SetUpdateEventMask(LogicComponentEvents::FixedUpdate);
}

void PlayerCharacter::RegisterObject(Context* context)
{
    context->RegisterFactory<PlayerCharacter>();

    // Network-replicated attributes
    URHO3D_ATTRIBUTE("Controls Yaw", controls_.yaw_, 0.0f, AM_DEFAULT);
    URHO3D_ATTRIBUTE("Controls Pitch", controls_.pitch_, 0.0f, AM_DEFAULT);
    URHO3D_ATTRIBUTE("On Ground", onGround_, false, AM_DEFAULT);
    URHO3D_ATTRIBUTE("OK To Jump", okToJump_, true, AM_DEFAULT);
    URHO3D_ATTRIBUTE("In Air Timer", inAirTimer_, 0.0f, AM_DEFAULT);
}

void PlayerCharacter::Start()
{
    SubscribeToEvent(GetNode(), E_NODECOLLISION, URHO3D_HANDLER(PlayerCharacter, HandleNodeCollision));
}

void PlayerCharacter::FixedUpdate(float timeStep)
{
    auto* body = GetComponent<RigidBody>();
    if (!body)
        return;

    // Update in-air timer
    if (!onGround_)
        inAirTimer_ += timeStep;
    else
        inAirTimer_ = 0.0f;

    bool softGrounded = inAirTimer_ < INAIR_THRESHOLD_TIME;

    // Build movement direction from controls
    const Quaternion& rot = node_->GetRotation();
    Vector3 moveDir = Vector3::ZERO;
    const Vector3& velocity = body->GetLinearVelocity();
    Vector3 planeVelocity(velocity.x_, 0.0f, velocity.z_);

    if (controls_.IsDown(CTRL_FORWARD))
        moveDir += Vector3::FORWARD;
    if (controls_.IsDown(CTRL_BACK))
        moveDir += Vector3::BACK;
    if (controls_.IsDown(CTRL_LEFT))
        moveDir += Vector3::LEFT;
    if (controls_.IsDown(CTRL_RIGHT))
        moveDir += Vector3::RIGHT;

    if (moveDir.LengthSquared() > 0.0f)
        moveDir.Normalize();

    // Apply movement impulse (reduced force in air)
    body->ApplyImpulse(rot * moveDir * (softGrounded ? MOVE_FORCE : INAIR_MOVE_FORCE));

    if (softGrounded)
    {
        // Brake to limit ground velocity
        Vector3 brakeForce = -planeVelocity * BRAKE_FORCE;
        body->ApplyImpulse(brakeForce);

        // Jump
        if (controls_.IsDown(CTRL_JUMP))
        {
            if (okToJump_)
            {
                body->ApplyImpulse(Vector3::UP * JUMP_FORCE);
                okToJump_ = false;
            }
        }
        else
            okToJump_ = true;
    }

    // Reset grounded flag for next frame
    onGround_ = false;
}

void PlayerCharacter::HandleNodeCollision(StringHash eventType, VariantMap& eventData)
{
    using namespace NodeCollision;

    MemoryBuffer contacts(eventData[P_CONTACTS].GetBuffer());

    while (!contacts.IsEof())
    {
        Vector3 contactPosition = contacts.ReadVector3();
        Vector3 contactNormal = contacts.ReadVector3();
        /*float contactDistance = */contacts.ReadFloat();
        /*float contactImpulse = */contacts.ReadFloat();

        // Contact below node center with upward normal = ground
        if (contactPosition.y_ < (node_->GetPosition().y_ + 1.0f))
        {
            if (contactNormal.y_ > 0.75f)
                onGround_ = true;
        }
    }
}
