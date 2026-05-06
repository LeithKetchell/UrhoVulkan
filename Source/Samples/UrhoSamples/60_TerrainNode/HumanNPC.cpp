// HumanNPC — task-driven human character.

#include "HumanNPC.h"
#include "BuildingSystem.h"

#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Scene/SmoothedTransform.h>
#include <Urho3D/Graphics/AnimatedModel.h>
#include <Urho3D/Graphics/AnimationController.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Math/MathDefs.h>

HumanNPC::HumanNPC(Context* context) :
    LandAnimal(context)
{
}

void HumanNPC::Start()
{
    LandAnimal::Start();

    Scene* scene = GetScene();
    if (scene)
    {
        // Discover campfire from the scene if not set externally
        if (!campfireNode_)
            campfireNode_ = scene->GetChild("Campfire", true);
    }

    // Find the model child node and AnimatedModel — used by 4a, 4b, 4c
    Node* modelNode = node_ ? node_->GetChild(node_->GetName() + "Model") : nullptr;
    auto* animModel = modelNode ? modelNode->GetComponent<AnimatedModel>() : nullptr;

    // Equipment visuals — read server-replicated Equip_* Vars and attach models.
    // Bone mapping: hand→RightHand, offhand→LeftHand, head→Head, body→Spine1
    // Animation text keys ("equip"/"unequip") override these at runtime.
    if (animModel && node_)
    {
        auto* cache = GetSubsystem<ResourceCache>();
        if (cache)
        {
            struct SlotBone { const char* slot; const char* bone; };
            static const SlotBone slotBones[] = {
                {"hand",    "RightHand"},
                {"offhand", "LeftHand"},
                {"head",    "Head"},
                {"body",    "Spine1"},
                {"feet",    "RightFoot"},
            };

            for (int s = 0; s < 5; ++s)
            {
                String varName = "Equip_" + String(slotBones[s].slot);
                Variant v = node_->GetVar(StringHash(varName));
                if (v.IsEmpty() || v.GetString().Empty())
                    continue;

                String modelPath = v.GetString();
                Node* boneNode = modelNode->GetChild(slotBones[s].bone, true);
                if (!boneNode)
                    continue;

                auto* mdl = cache->GetResource<Model>(modelPath);
                if (!mdl)
                    continue;

                Node* equipNode = boneNode->CreateChild("Equip_" + String(slotBones[s].bone));
                auto* sm = equipNode->CreateComponent<StaticModel>();
                sm->SetModel(mdl);
                sm->SetCastShadows(false);

                // Hand items: apply default held-item transform
                if (String(slotBones[s].slot) == "hand" || String(slotBones[s].slot) == "offhand")
                {
                    equipNode->SetScale(Vector3(0.15f, 0.15f, 0.15f));
                    equipNode->SetRotation(Quaternion(90.0f, Vector3::RIGHT));
                    equipNode->SetPosition(Vector3(0.0f, 0.05f, 0.0f));
                    heldItemNode_ = equipNode;
                }
            }
        }
    }

    // Phase 4a: Material tint variation — clone material, apply random skin/clothing hue shift
    // Each NPC gets a slightly different tint so groups don't look like clones.
    if (animModel)
    {
        for (unsigned i = 0; i < animModel->GetNumGeometries(); i++)
        {
            Material* orig = animModel->GetMaterial(i);
            if (orig)
            {
                SharedPtr<Material> clone = orig->Clone();
                // Random warm tint: slight variation in R/G/B around (1,1,1)
                float r = 0.85f + Random(0.3f);   // 0.85 - 1.15
                float g = 0.82f + Random(0.25f);   // 0.82 - 1.07
                float b = 0.75f + Random(0.2f);    // 0.75 - 0.95
                clone->SetShaderParameter("MatDiffColor", Color(r, g, b, 1.0f));
                animModel->SetMaterial(i, clone);
            }
        }
    }

    // Phase 4c: Scale variation — ±10% so groups don't look uniform
    if (node_)
    {
        float baseScale = node_->GetScale().x_;
        float variation = 1.0f + Random(-0.1f, 0.1f);
        node_->SetScale(baseScale * variation);
    }
}

// FindNearbyResource() and PickTask() removed — all NPC decision-making
// is server-authoritative. Server runs PickCreatureTask() in AuthServer.cpp
// and broadcasts via MSG_CREATURE_AI_STATE.

void HumanNPC::SetPossessed(bool possessed)
{
    if (possessed_ == possessed)
        return;
    possessed_ = possessed;

    if (!possessed_)
    {
        // Returning to AI — clear controls, reset to idle
        controls_.Reset();
        possessedAnim_.Clear();
        inJump_ = false;
        jumpVelocity_ = 0.0f;
        possessedInWater_ = false;
        SetState(CREATURE_IDLE);
    }
    else
    {
        // Being possessed — ensure idle animation starts clean
        possessedAnim_.Clear();
        inJump_ = false;
        jumpVelocity_ = 0.0f;
    }
}

// Possessed movement tuning
static constexpr float SPRINT_STAMINA_DRAIN = 5.0f;   // extra stamina/sec while sprinting
static constexpr float SWIM_STAMINA_DRAIN = 4.0f;     // extra stamina/sec while swimming
static constexpr float JUMP_STAMINA_COST = 8.0f;      // stamina per jump
static constexpr float JUMP_IMPULSE = 7.0f;           // initial vertical velocity (m/s)
static constexpr float JUMP_GRAVITY = 20.0f;          // downward acceleration (m/s^2)
static constexpr float JUMP_AIR_CONTROL = 0.4f;       // fraction of ground speed while airborne
static constexpr float SWIM_SPEED_MULT = 0.6f;        // fraction of wander speed in water
static constexpr float MIN_SPRINT_STAMINA = 5.0f;     // can't sprint below this

void HumanNPC::SetSmoothedPosition(const Vector3& pos)
{
    auto* smooth = node_->GetComponent<SmoothedTransform>();
    if (smooth)
        smooth->SetTargetWorldPosition(pos);
    else
        node_->SetWorldPosition(pos);
}

void HumanNPC::UpdatePossessedMovement(float timeStep)
{
    if (!node_)
        return;

    Vector3 pos = node_->GetWorldPosition();
    float terrainY = terrain_ ? terrain_->GetHeight(pos) : pos.y_;

    // Detect water — terrain below water level means we're in water
    bool wasInWater = possessedInWater_;
    possessedInWater_ = (terrainY < waterLevel_);

    // Entering water cancels jump
    if (possessedInWater_ && !wasInWater)
    {
        inJump_ = false;
        jumpVelocity_ = 0.0f;
    }

    // Delegate to swim handler when in water
    if (possessedInWater_)
    {
        UpdatePossessedSwim(timeStep);
        return;
    }

    auto* animCtrl = node_->GetComponent<AnimationController>(true);

    // Build movement direction from controls
    Vector3 moveDir = Vector3::ZERO;
    if (controls_.IsDown(CTRL_FORWARD))
        moveDir += Vector3::FORWARD;
    if (controls_.IsDown(CTRL_BACK))
        moveDir += Vector3::BACK;
    if (controls_.IsDown(CTRL_LEFT))
        moveDir += Vector3::LEFT;
    if (controls_.IsDown(CTRL_RIGHT))
        moveDir += Vector3::RIGHT;

    bool moving = moveDir.LengthSquared() > 0.0f;
    bool sprinting = controls_.IsDown(CTRL_SPRINT) && moving && !inJump_ && stamina_ > MIN_SPRINT_STAMINA;

    // Extra stamina drain while sprinting
    if (sprinting)
        stamina_ = Max(0.0f, stamina_ - SPRINT_STAMINA_DRAIN * timeStep);

    // ── Jump ──
    if (inJump_)
    {
        jumpVelocity_ -= JUMP_GRAVITY * timeStep;
        pos.y_ += jumpVelocity_ * timeStep;

        // Land when back on terrain
        if (pos.y_ <= terrainY)
        {
            pos.y_ = terrainY;
            inJump_ = false;
            jumpVelocity_ = 0.0f;
            possessedAnim_.Clear();  // force anim refresh on landing
        }

        // Air control — reduced horizontal movement
        if (moving)
        {
            moveDir.Normalize();
            Quaternion yawRot(0.0f, controls_.yaw_, 0.0f);
            moveDir = yawRot * moveDir;

            float airSpeed = GetWanderSpeed() * JUMP_AIR_CONTROL;
            Vector3 newPos = pos + moveDir * airSpeed * timeStep;
            newPos.y_ = pos.y_;  // preserve jump Y

            if (!IsBlockedByWall(pos, newPos))
                pos = newPos;
        }

        SetSmoothedPosition(pos);
        return;
    }

    // Initiate jump
    if (controls_.IsDown(CTRL_JUMP) && stamina_ > JUMP_STAMINA_COST)
    {
        inJump_ = true;
        jumpVelocity_ = JUMP_IMPULSE;
        stamina_ = Max(0.0f, stamina_ - JUMP_STAMINA_COST);

        String jumpAnim = GetJumpAnim();
        if (!jumpAnim.Empty() && animCtrl)
        {
            animCtrl->PlayExclusive(jumpAnim, 0, false, 0.1f);
            possessedAnim_ = jumpAnim;
        }
        return;
    }

    // ── Ground movement ──
    if (moving)
    {
        moveDir.Normalize();

        // Rotate movement direction by yaw (camera-relative)
        Quaternion yawRot(0.0f, controls_.yaw_, 0.0f);
        moveDir = yawRot * moveDir;

        // Smooth turning toward movement direction
        Vector3 currentFwd = node_->GetWorldDirection();
        currentFwd.y_ = 0.0f;
        currentFwd.Normalize();
        float turnRate = 8.0f * timeStep;
        Vector3 newFwd = currentFwd.Lerp(moveDir, Min(turnRate, 1.0f));
        newFwd.y_ = 0.0f;
        if (newFwd.LengthSquared() > 0.001f)
        {
            newFwd.Normalize();
            node_->SetWorldDirection(newFwd);
        }

        float speed = sprinting ? GetFleeSpeed() : GetWanderSpeed();
        Vector3 newPos = pos + newFwd * speed * timeStep;

        // Wall check
        if (!IsBlockedByWall(pos, newPos))
            SetSmoothedPosition(newPos);

        // Play run or walk animation
        String targetAnim = sprinting ? GetRunAnim() : GetWalkAnim();
        if (!targetAnim.Empty() && targetAnim != possessedAnim_ && animCtrl)
        {
            animCtrl->PlayExclusive(targetAnim, 0, true, 0.2f);
            possessedAnim_ = targetAnim;
        }
    }
    else
    {
        // Idle — play idle animation
        String idleAnim = GetIdleAnim();
        if (!idleAnim.Empty() && idleAnim != possessedAnim_ && animCtrl)
        {
            animCtrl->PlayExclusive(idleAnim, 0, true, 0.3f);
            possessedAnim_ = idleAnim;
        }
    }
}

void HumanNPC::UpdatePossessedSwim(float timeStep)
{
    if (!node_)
        return;

    auto* animCtrl = node_->GetComponent<AnimationController>(true);
    Vector3 pos = node_->GetWorldPosition();

    // Extra stamina drain while swimming
    stamina_ = Max(0.0f, stamina_ - SWIM_STAMINA_DRAIN * timeStep);

    // Build movement direction from controls
    Vector3 moveDir = Vector3::ZERO;
    if (controls_.IsDown(CTRL_FORWARD))
        moveDir += Vector3::FORWARD;
    if (controls_.IsDown(CTRL_BACK))
        moveDir += Vector3::BACK;
    if (controls_.IsDown(CTRL_LEFT))
        moveDir += Vector3::LEFT;
    if (controls_.IsDown(CTRL_RIGHT))
        moveDir += Vector3::RIGHT;

    bool moving = moveDir.LengthSquared() > 0.0f;
    float swimSpeed = GetWanderSpeed() * SWIM_SPEED_MULT;

    if (moving)
    {
        moveDir.Normalize();

        // 3D swim direction — pitch + yaw so looking down + W = dive
        Quaternion swimRot(controls_.pitch_, controls_.yaw_, 0.0f);
        Vector3 swimDir = swimRot * moveDir;

        // Smooth turning (horizontal component only for model facing)
        Vector3 horizDir(swimDir.x_, 0.0f, swimDir.z_);
        if (horizDir.LengthSquared() > 0.001f)
        {
            horizDir.Normalize();
            Vector3 currentFwd = node_->GetWorldDirection();
            currentFwd.y_ = 0.0f;
            currentFwd.Normalize();
            float turnRate = 6.0f * timeStep;
            Vector3 newFwd = currentFwd.Lerp(horizDir, Min(turnRate, 1.0f));
            newFwd.Normalize();
            node_->SetWorldDirection(newFwd);
        }

        Vector3 newPos = pos + swimDir * swimSpeed * timeStep;

        // Don't swim below terrain floor
        if (terrain_)
        {
            float groundY = terrain_->GetHeight(newPos);
            if (newPos.y_ < groundY + 0.2f)
                newPos.y_ = groundY + 0.2f;
        }

        if (!IsBlockedByWall(pos, newPos))
            SetSmoothedPosition(newPos);
    }

    // Space (CTRL_JUMP) = rise toward surface
    if (controls_.IsDown(CTRL_JUMP))
    {
        pos = node_->GetWorldPosition();
        pos.y_ += swimSpeed * timeStep;
        // Don't breach too far above water surface
        if (pos.y_ > waterLevel_ + 0.3f)
            pos.y_ = waterLevel_ + 0.3f;
        SetSmoothedPosition(pos);
    }

    // Buoyancy — gently push toward water surface when not actively diving
    if (!moving || controls_.pitch_ >= 0.0f)
    {
        pos = node_->GetWorldPosition();
        float surfaceY = waterLevel_;
        if (pos.y_ < surfaceY - 0.5f)
        {
            pos.y_ += 1.5f * timeStep;  // gentle upward drift
            SetSmoothedPosition(pos);
        }
    }

    // Play swim animation
    String swimAnim = GetSwimAnim();
    if (!swimAnim.Empty() && swimAnim != possessedAnim_ && animCtrl)
    {
        animCtrl->PlayExclusive(swimAnim, 0, true, 0.3f);
        possessedAnim_ = swimAnim;
    }
}

void HumanNPC::FixedUpdate(float timeStep)
{
    // When possessed by a player, skip AI entirely
    if (possessed_)
    {
        // Vitals still tick when possessed
        if (HasVitals())
        {
            float sunAlt = 0.5f;
            Scene* scene = GetScene();
            if (scene)
            {
                Node* dl = scene->GetChild("DirectionalLight", true);
                if (dl)
                    sunAlt = -dl->GetDirection().y_;
            }
            UpdateVitals(timeStep, sunAlt);
        }

        UpdatePossessedMovement(timeStep);
        PostMovementUpdate(timeStep);
        return;
    }

    // Server-driven mode: server owns the brain. Client lerps toward
    // server position and plays server-chosen animation. No local decisions.
    if (serverDriven_ && !possessed_)
    {
        if (node_)
        {
            Vector3 pos = node_->GetWorldPosition();
            Vector3 diff = serverPos_ - pos;
            float dist = diff.Length();

            if (dist > 0.1f)
            {
                // Smooth lerp toward server position — snap if too far behind
                float lerpSpeed = Max(serverMoveSpeed_, 3.0f);
                float step = lerpSpeed * timeStep;
                if (dist > 10.0f)
                    node_->SetWorldPosition(serverPos_); // Snap
                else
                {
                    Vector3 newPos = pos + diff.Normalized() * Min(step, dist);
                    // Y-snap to local terrain
                    if (terrain_)
                        newPos.y_ = terrain_->GetHeight(newPos);
                    node_->SetWorldPosition(newPos);
                    // Face movement direction
                    Vector3 faceDir = diff;
                    faceDir.y_ = 0.0f;
                    if (faceDir.LengthSquared() > 0.01f)
                        node_->SetWorldDirection(faceDir.Normalized());
                }
            }
        }

        // Phase 4b: Show/hide held item based on server state
        if (heldItemNode_)
        {
            bool showItem = (state_ == CREATURE_IDLE || state_ == CREATURE_WANDER ||
                             state_ == CREATURE_FIGHT || state_ == CREATURE_GREET ||
                             state_ == CREATURE_ALERT || state_ == CREATURE_FLEE ||
                             state_ == CREATURE_LOOK);
            heldItemNode_->SetEnabled(showItem);
        }

        PostMovementUpdate(timeStep);
        return;
    }

    // Not possessed and not server-driven: waiting for server AI state.
    // All NPC decision-making is server-authoritative (even offline mode
    // has a local AuthServer). Client just idles until first MSG_CREATURE_AI_STATE.
    PostMovementUpdate(timeStep);
}
