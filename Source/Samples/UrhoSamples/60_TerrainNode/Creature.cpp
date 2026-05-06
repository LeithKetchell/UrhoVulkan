// Creature — base class for ALL living things.

#include "Creature.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Graphics/AnimatedModel.h>
#include <Urho3D/Graphics/AnimationController.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Viewport.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Scene/SceneEvents.h>
#include <Urho3D/Scene/Node.h>
#ifdef URHO3D_PHYSICS
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#endif
#include <Urho3D/Math/MathDefs.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/Font.h>

Creature::Creature(Context* context) :
    LogicComponent(context)
{
    // FixedUpdate for movement/AI (fixed timestep via PhysicsWorld),
    // Update for visual-only things like damage text animation.
    SetUpdateEventMask(LogicComponentEvents::FixedUpdate);
}

void Creature::Start()
{

    terrain_ = GetScene() ? GetScene()->GetComponent<Terrain>(true) : nullptr;

    // Normalize model to real-world size until all models are reimported at correct scale
    auto* mdl = node_->GetComponent<AnimatedModel>(true);
    if (mdl)
    {
        BoundingBox bb = mdl->GetBoundingBox();
        Vector3 size = bb.Size();
        float longest = Max(size.x_, Max(size.y_, size.z_));
        if (longest > 0.001f)
            node_->SetScale(GetDesiredSize() / longest);
    }

    homePos_ = node_->GetWorldPosition();
    stateTimer_ = Random(GetMinIdleDuration(), GetMaxIdleDuration());
    OnStateEnter(CREATURE_IDLE);

    SubscribeToEvent(GetNode(), E_COMBAT_RESULT, URHO3D_HANDLER(Creature, HandleCombatResult));
}

void Creature::SetState(CreatureState newState)
{
    if (state_ == newState)
        return;

    prevState_ = state_;
    state_ = newState;

    switch (newState)
    {
    case CREATURE_IDLE:
        stateTimer_ = Random(GetMinIdleDuration(), GetMaxIdleDuration());
        break;

    case CREATURE_WANDER:
        stateTimer_ = 10.0f;
        break;

    case CREATURE_EAT:
        stateTimer_ = Random(3.0f, 6.0f);
        break;

    case CREATURE_FLEE:
        stateTimer_ = 3.0f;
        break;

    case CREATURE_SIT:
        stateTimer_ = Random(8.0f, 20.0f);
        break;

    case CREATURE_SLEEP:
        stateTimer_ = Random(15.0f, 30.0f);
        break;

    case CREATURE_LOOK:
        stateTimer_ = Random(3.0f, 6.0f);
        break;

    case CREATURE_GREET:
        stateTimer_ = 5.0f;
        break;

    case CREATURE_SWIM:
        stateTimer_ = 10.0f;
        break;

    case CREATURE_CROUCH:
        stateTimer_ = Random(5.0f, 10.0f);
        break;

    case CREATURE_DIE:
        stateTimer_ = 5.0f;
        {
            using namespace CreatureDied;
            VariantMap& eventData = GetEventDataMap();
            eventData[P_CREATUREID] = GetCreatureId();
            eventData[P_POSITION]   = node_ ? node_->GetWorldPosition() : Vector3::ZERO;
            SendEvent(E_CREATUREDIED, eventData);
        }
        break;

    case CREATURE_CORPSE:
        // Body persists as a harvestable static prop. The 120s budget is the
        // visible-corpse window; LandAnimal::FixedUpdate runs the sink-and-remove
        // fade after stateTimer_ goes negative. Tunable in LandAnimal.cpp.
        stateTimer_ = 120.0f;
        break;

    case CREATURE_SCAVENGE:
        // Travel-then-eat phase. The travel portion (variable duration based on
        // distance to scent) doesn't use stateTimer_ — LandAnimal::FixedUpdate
        // walks toward wanderTarget_ and re-enters SCAVENGE on arrival to start
        // the eat phase, which uses this timer. Default = SCAVENGER_EAT_DURATION
        // (6s) defined in LandAnimal.cpp.
        stateTimer_ = 6.0f;
        break;

    case CREATURE_HUNT:
        // Chasing live prey. Timer is the "memory" window — if prey leaves
        // the vision cone, the predator keeps pursuing for this long before
        // giving up. Refreshed each time the prey is re-sighted.
        stateTimer_ = 5.0f;
        break;

    case CREATURE_FIGHT:
        // Melee attack. Timer is one attack cycle — on expiry, re-evaluate
        // (prey dead? fled? still in range?). Kept short for responsive AI.
        stateTimer_ = 2.0f;
        break;

    case CREATURE_ALERT:
        // Freeze, stare at threat. Short random duration before fleeing.
        stateTimer_ = Random(1.0f, 2.0f);
        break;

    case CREATURE_VICTORY:
        // Brief celebration after a successful kill, then return to idle.
        stateTimer_ = Random(2.0f, 3.0f);
        break;
    }

    OnStateEnter(newState);
}

void Creature::OnStateEnter(CreatureState newState)
{
    auto* animCtrl = node_->GetComponent<AnimationController>(true);
    if (spawnId_ == 50)
    {
        static int enterCount = 0;
        if (++enterCount <= 5)
            URHO3D_LOGINFOF("[Trace50 OnStateEnter] state=%d animCtrl=%s idleAnim='%s'",
                (int)newState, animCtrl ? "YES" : "NO", GetIdleAnim().CString());
    }
    if (!animCtrl)
    {
        pendingAnimApply_ = true;
        return;
    }
    pendingAnimApply_ = false;

    switch (newState)
    {
    case CREATURE_IDLE:
        if (!GetIdleAnim().Empty())
        {
            URHO3D_LOGINFOF("[AnimPlay] %s playing '%s'", node_->GetName().CString(), GetIdleAnim().CString());
            animCtrl->PlayExclusive(GetIdleAnim(), 0, true, 0.3f);
        }
        break;

    case CREATURE_WANDER:
    {
        // Use walk anim if available, otherwise run at half speed
        String walkAnim = GetWalkAnim();
        if (!walkAnim.Empty())
        {
            animCtrl->PlayExclusive(walkAnim, 0, true, 0.3f);
        }
        else if (!GetRunAnim().Empty())
        {
            animCtrl->PlayExclusive(GetRunAnim(), 0, true, 0.3f);
            animCtrl->SetSpeed(GetRunAnim(), 0.5f);
        }
        break;
    }

    case CREATURE_EAT:
    {
        String eatAnim = GetEatAnim();
        if (!eatAnim.Empty())
            animCtrl->PlayExclusive(eatAnim, 0, true, 0.3f);
        break;
    }

    case CREATURE_FLEE:
        if (!GetRunAnim().Empty())
        {
            animCtrl->PlayExclusive(GetRunAnim(), 0, true, 0.2f);
            animCtrl->SetSpeed(GetRunAnim(), 1.5f);
        }
        break;

    case CREATURE_DIE:
        if (!GetDieAnim().Empty())
            animCtrl->PlayExclusive(GetDieAnim(), 0, false, 0.2f);
        break;

    case CREATURE_SIT:
        if (!GetSitAnim().Empty())
            animCtrl->PlayExclusive(GetSitAnim(), 0, true, 0.5f);

        // Tend-fire overlay: on any SIT entry, play a brief one-shot "reaching
        // to stoke" gesture on layer 1. CREATURE_SIT implies campfire context
        // (see Creature.h:38), so the overlay is always contextually correct.
        // Previous check (prevState_ == CREATURE_WANDER) missed server-driven
        // state injection, SLEEP→SIT transitions, and direct SIT assignments.
        {
            String tendAnim = GetTendAnim();
            if (!tendAnim.Empty())
            {
                animCtrl->PlayExclusive(tendAnim, 1, false, 0.2f);
                animCtrl->SetAutoFade(tendAnim, 0.3f);
                animCtrl->SetRemoveOnCompletion(tendAnim, true);
            }
        }
        break;

    case CREATURE_SLEEP:
        if (!GetSleepAnim().Empty())
            animCtrl->PlayExclusive(GetSleepAnim(), 0, true, 0.5f);
        break;

    case CREATURE_LOOK:
        if (!GetLookAnim().Empty())
            animCtrl->PlayExclusive(GetLookAnim(), 0, false, 0.3f);
        break;

    case CREATURE_GREET:
        if (!GetGreetAnim().Empty())
            animCtrl->PlayExclusive(GetGreetAnim(), 0, false, 0.3f);
        break;

    case CREATURE_SWIM:
        if (!GetSwimAnim().Empty())
            animCtrl->PlayExclusive(GetSwimAnim(), 0, true, 0.3f);
        break;

    case CREATURE_CROUCH:
        if (!GetCrouchWalkAnim().Empty())
            animCtrl->PlayExclusive(GetCrouchWalkAnim(), 0, true, 0.3f);
        break;

    case CREATURE_TRAPPED:
        // Use scream anim if available (cavemen), otherwise fall back to idle
        if (!GetScreamAnim().Empty())
            animCtrl->PlayExclusive(GetScreamAnim(), 0, false, 0.3f);
        else if (!GetIdleAnim().Empty())
            animCtrl->PlayExclusive(GetIdleAnim(), 0, true, 0.5f);
        break;

    case CREATURE_CORPSE:
        // Freeze the death pose. Disabling the controller stops it advancing
        // through the animation graph; whatever frame the die anim was on (the
        // last frame, for non-looped die anims) stays painted on the mesh.
        animCtrl->SetEnabled(false);
        break;

    case CREATURE_SCAVENGE:
        // Travel phase starts here — kick walk/run anim. The on-arrival
        // switch to eat anim is driven from LandAnimal::FixedUpdate so that
        // OnStateEnter doesn't have to know about phase transitions.
        if (!GetWalkAnim().Empty())
            animCtrl->PlayExclusive(GetWalkAnim(), 0, true, 0.3f);
        else if (!GetRunAnim().Empty())
            animCtrl->PlayExclusive(GetRunAnim(), 0, true, 0.3f);
        break;

    case CREATURE_HUNT:
        // Full-speed pursuit — gallop after prey
        if (!GetRunAnim().Empty())
        {
            animCtrl->PlayExclusive(GetRunAnim(), 0, true, 0.2f);
            animCtrl->SetSpeed(GetRunAnim(), 1.2f);
        }
        break;

    case CREATURE_FIGHT:
        if (!GetAttackAnim().Empty())
            animCtrl->PlayExclusive(GetAttackAnim(), 0, false, 0.2f);
        break;

    case CREATURE_ALERT:
        // Freeze and look — reuse look animation for the stare
        if (!GetLookAnim().Empty())
            animCtrl->PlayExclusive(GetLookAnim(), 0, false, 0.3f);
        else if (!GetIdleAnim().Empty())
            animCtrl->PlayExclusive(GetIdleAnim(), 0, true, 0.3f);
        break;

    case CREATURE_VICTORY:
        if (!GetVictoryAnim().Empty())
            animCtrl->PlayExclusive(GetVictoryAnim(), 0, false, 0.3f);
        else if (!GetIdleAnim().Empty())
            animCtrl->PlayExclusive(GetIdleAnim(), 0, true, 0.3f);
        break;

    case CREATURE_FORAGE:
        // Slow walk toward food — head-low idle for the nosing look, else walk
        if (!GetWalkAnim().Empty())
        {
            animCtrl->PlayExclusive(GetWalkAnim(), 0, true, 0.3f);
            animCtrl->SetSpeed(GetWalkAnim(), 0.6f);
        }
        break;
    }
}

bool Creature::CanSee(const Creature* other) const
{
    if (!other || !node_ || !other->GetNode())
        return false;

    float range = GetVisionRange();
    if (range <= 0.0f)
        return false;

    Vector3 myPos = node_->GetWorldPosition();
    Vector3 otherPos = other->GetNode()->GetWorldPosition();
    Vector3 toOther = otherPos - myPos;

    float distSq = toOther.LengthSquared();
    if (distSq > range * range || distSq < 0.01f)
        return false;

    // Flatten to XZ for the cone check — animals don't look up/down meaningfully
    Vector3 forward = node_->GetWorldDirection();
    forward.y_ = 0.0f;
    toOther.y_ = 0.0f;

    float fwdLen = forward.Length();
    float toLen = toOther.Length();
    if (fwdLen < 0.001f || toLen < 0.001f)
        return false;

    float dot = forward.DotProduct(toOther) / (fwdLen * toLen);
    return dot > GetVisionCosAngle();
}

void Creature::MoveToward(const Vector3& target, float speed, float timeStep)
{
    Vector3 pos = node_->GetWorldPosition();
    Vector3 dir = target - pos;
    dir.y_ = 0.0f;
    float dist = dir.Length();

    if (dist < 0.1f)
        return;

    dir.Normalize();

    // Smooth turn
    Vector3 currentFwd = node_->GetWorldDirection();
    currentFwd.y_ = 0.0f;
    currentFwd.Normalize();

    float turnRate = 4.0f * timeStep;
    Vector3 newFwd = currentFwd.Lerp(dir, Min(turnRate, 1.0f));
    newFwd.y_ = 0.0f;
    if (newFwd.LengthSquared() > 0.001f)
    {
        newFwd.Normalize();
        node_->SetWorldDirection(newFwd);
    }

    Vector3 newPos = pos + newFwd * speed * timeStep;
    node_->SetWorldPosition(newPos);
}

void Creature::PickWanderTarget()
{
    float radius = GetWanderRadius();
    const float waterLevel = 5.5f;

    for (int tries = 0; tries < 8; ++tries)
    {
        float shrink = (tries > 3) ? 0.5f : 1.0f;
        float x = homePos_.x_ + Random(-radius * shrink, radius * shrink);
        float z = homePos_.z_ + Random(-radius * shrink, radius * shrink);
        float y = terrain_ ? terrain_->GetHeight(Vector3(x, 0.0f, z)) : 0.0f;

        if (y >= waterLevel)
        {
            wanderTarget_ = Vector3(x, y, z);
            return;
        }
    }

    // All attempts landed in water — stay put
    wanderTarget_ = node_->GetWorldPosition();
}

void Creature::Respawn()
{
    if (!terrain_)
        return;

    float radius = GetWanderRadius();
    Vector3 spawnPos = homePos_;

    const float waterLevel = 5.5f;
    for (int tries = 0; tries < 20; ++tries)
    {
        float x = homePos_.x_ + Random(-radius, radius);
        float z = homePos_.z_ + Random(-radius, radius);
        float y = terrain_->GetHeight(Vector3(x, 0.0f, z));
        if (y >= waterLevel)
        {
            spawnPos = Vector3(x, y, z);
            break;
        }
    }

    node_->SetWorldPosition(spawnPos);
    state_ = CREATURE_IDLE;
    stateTimer_ = Random(GetMinIdleDuration(), GetMaxIdleDuration());
    OnStateEnter(CREATURE_IDLE);
}

// ============================================================================
// Combat
// ============================================================================

void Creature::InitCombatStats(int hp, int attack, int defense, int damage, int damageVar,
                                int speed, float fleeFraction, const String& aggression)
{
    hp_           = hp;
    maxHp_        = hp;
    attack_       = attack;
    defense_      = defense;
    damage_       = damage;
    damageVar_    = damageVar;
    speed_        = speed;
    fleeFraction_ = fleeFraction;
    aggression_   = aggression;
}

bool Creature::TakeDamage(int amount)
{
    if (state_ == CREATURE_DIE)
        return false;

    hp_ -= amount;
    if (hp_ < 0) hp_ = 0;

    URHO3D_LOGINFOF("Creature %s took %d damage — HP %d/%d",
        GetNode() ? GetNode()->GetName().CString() : "?", amount, hp_, maxHp_);

    if (hp_ <= 0)
    {
        // Phase 5d: server-tracked creatures (spawnId_ != 0) die via
        // MSG_CREATURE_DEATH from the server, not local TakeDamage.
        // Offline/client-local creatures still die here.
        if (spawnId_ == 0)
        {
            SetState(CREATURE_DIE);
            return true;
        }
        // Server-tracked: clamp HP at 0 but don't trigger death — server will.
        return false;
    }

    float hpFrac = (float)hp_ / (float)maxHp_;
    if (hpFrac <= fleeFraction_)
    {
        if (aggression_ == "aggressive")
            SetState(CREATURE_FIGHT);
        else
            SetState(CREATURE_FLEE);
    }
    else if (aggression_ == "aggressive" && state_ != CREATURE_FIGHT)
    {
        SetState(CREATURE_FIGHT);
    }

    return false;
}

void Creature::SetHp(int newHp)
{
    // Combat Phase 2: server-authoritative HP overwrite. Mirrors what the
    // server tells us. Does NOT trigger flee/fight transitions — the server
    // owns the death decision and broadcasts MSG_CREATURE_DEATH separately.
    // Local flee/fight reactions can still fire from below-threshold HP, but
    // we keep that decision in this setter so the floating damage path stays
    // simple.
    if (state_ == CREATURE_DIE)
        return;

    hp_ = Clamp(newHp, 0, maxHp_);

    if (hp_ == 0)
    {
        // Don't transition to CREATURE_DIE here — wait for the explicit
        // MSG_CREATURE_DEATH from the server. This avoids a race where the
        // client kills the creature locally before the server's death
        // broadcast lands and then ignores the broadcast.
        return;
    }

    float hpFrac = (float)hp_ / (float)maxHp_;
    if (hpFrac <= fleeFraction_)
    {
        if (aggression_ == "aggressive")
            SetState(CREATURE_FIGHT);
        else
            SetState(CREATURE_FLEE);
    }
}

// ============================================================================
// Damage Text (event-driven, self-managed)
// ============================================================================

void Creature::HandleCombatResult(StringHash /*eventType*/, VariantMap& eventData)
{
    using namespace CombatResult;
    int  damage = eventData[P_DAMAGE].GetI32();
    bool crit   = eventData[P_CRIT].GetBool();
    bool miss   = eventData[P_MISS].GetBool();

    auto* ui    = GetSubsystem<UI>();
    auto* cache = GetSubsystem<ResourceCache>();
    if (!ui || !cache)
        return;

    // Remove any previous damage text for this creature
    if (!damageText_.Expired())
        damageText_->Remove();

    auto* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");
    if (!font)
        font = cache->GetResource<Font>("Fonts/BlueHighway.ttf");
    if (!font)
        return;

    auto* text = ui->GetRoot()->CreateChild<Text>();
    String label = miss ? "Miss" : (crit ? "CRIT " + String(damage) + "!" : String(damage));
    text->SetText(label);
    text->SetFont(font, crit ? 18 : 14);
    text->SetColor(miss ? Color(0.7f, 0.7f, 0.7f) : (crit ? Color(1.0f, 0.8f, 0.0f) : Color(1.0f, 0.3f, 0.3f)));
    text->SetTextAlignment(HA_CENTER);

    // Position above creature in screen space
    auto* renderer = GetSubsystem<Renderer>();
    auto* graphics = GetSubsystem<Graphics>();
    Viewport* vp = renderer ? renderer->GetViewport(0) : nullptr;
    Camera* camera = vp ? vp->GetCamera() : nullptr;
    if (camera && graphics && node_)
    {
        Vector3 worldPos = node_->GetWorldPosition() + Vector3(0.0f, 1.5f, 0.0f);
        Vector2 screenPos = camera->WorldToScreenPoint(worldPos);
        int sx = (int)(screenPos.x_ * graphics->GetWidth());
        int sy = (int)(screenPos.y_ * graphics->GetHeight());
        text->SetPosition(IntVector2(sx - 20, sy - 20));
    }

    damageText_     = text;
    damageTextTimer_  = 0.0f;
    damageTextStartY_ = (float)text->GetPosition().y_;

    // Play hit reaction flinch as one-shot overlay (layer 1) on actual hits
    if (!miss && node_)
    {
        String hitAnim = GetHitReactAnim();
        if (!hitAnim.Empty())
        {
            auto* animCtrl = node_->GetComponent<AnimationController>(true);
            if (animCtrl)
            {
                animCtrl->Play(hitAnim, 1, false, 0.15f);
                animCtrl->SetWeight(hitAnim, 0.6f);
            }
        }
    }

    // Enable Update so we can animate the text each frame
    SetUpdateEventMask(GetUpdateEventMask() | LogicComponentEvents::Update);
}

void Creature::Update(float timeStep)
{
    // Damage text animation (only when active)
    if (damageText_.Expired())
        return;

    damageTextTimer_ += timeStep;
    float t = damageTextTimer_;

    // Float upward
    IntVector2 pos = damageText_->GetPosition();
    pos.y_ = (int)(damageTextStartY_ - t * 40.0f);
    damageText_->SetPosition(pos);

    // Fade out
    float alpha = 1.0f - (t / 1.5f);
    if (alpha <= 0.0f)
    {
        damageText_->Remove();
        damageText_.Reset();
        SetUpdateEventMask(GetUpdateEventMask() & ~LogicComponentEvents::Update);
    }
    else
    {
        damageText_->SetOpacity(alpha);
    }
}

// ============================================================================
// Vitals — depleting needs system
// ============================================================================

void Creature::UpdateVitals(float timeStep, float sunAltitude)
{
    // sunAltitude: 1.0 = noon, 0.0 = horizon, -1.0 = midnight
    // Convert to a 0..1 "darkness" factor — higher at night
    float darkness = Clamp(0.5f - sunAltitude * 0.5f, 0.0f, 1.0f);

    // Base depletion rates (units per second)
    const float hungerRate     = 0.15f;   // hungry every ~10 minutes
    const float thirstRate     = 0.20f;   // thirsty every ~8 minutes
    const float staminaActive  = 0.30f;   // tired faster while doing things
    const float staminaPassive = 0.05f;   // slow drain at rest
    const float warmthBase     = 0.05f;
    const float warmthNight    = 0.40f;   // cold at night

    hunger_ -= hungerRate * timeStep;
    thirst_ -= thirstRate * timeStep;

    // Stamina depletes faster while moving/working, less when sitting or sleeping
    bool resting = (state_ == CREATURE_SIT || state_ == CREATURE_SLEEP || state_ == CREATURE_IDLE);
    stamina_ -= (resting ? staminaPassive : staminaActive) * timeStep;

    // Warmth — base loss + extra at night
    warmth_ -= (warmthBase + warmthNight * darkness) * timeStep;

    // Clamp
    hunger_  = Clamp(hunger_,  0.0f, 100.0f);
    thirst_  = Clamp(thirst_,  0.0f, 100.0f);
    stamina_ = Clamp(stamina_, 0.0f, 100.0f);
    warmth_  = Clamp(warmth_,  0.0f, 100.0f);
}

CreatureState Creature::PickNeedState()
{
    // Find the most depleted vital — only act on those below 50
    struct Need { float value; CreatureState state; };
    Need needs[] = {
        { stamina_, CREATURE_SLEEP },
        { hunger_,  CREATURE_EAT   },
        { thirst_,  CREATURE_EAT   },  // drinking uses gather/water anim
        { warmth_,  CREATURE_SIT   },  // sit by fire (proximity check elsewhere)
    };

    float lowest = 50.0f;
    CreatureState pick = CREATURE_IDLE;
    for (const Need& n : needs)
    {
        if (n.value < lowest)
        {
            lowest = n.value;
            pick = n.state;
        }
    }
    return pick;
}

void Creature::OnActionComplete(CreatureState completedState)
{
    switch (completedState)
    {
    case CREATURE_EAT:
        hunger_ = Min(100.0f, hunger_ + 35.0f);
        thirst_ = Min(100.0f, thirst_ + 25.0f);
        break;
    case CREATURE_SIT:
        stamina_ = Min(100.0f, stamina_ + 20.0f);
        warmth_  = Min(100.0f, warmth_ + 30.0f);  // sitting near fire
        break;
    case CREATURE_SLEEP:
        stamina_ = Min(100.0f, stamina_ + 60.0f);
        break;
    default:
        break;
    }
}

void Creature::ApplyServerState(CreatureState newState, const Vector3& pos, float moveSpeed)
{
    bool firstApply = !serverDriven_;
    serverDriven_ = true;
    serverPos_ = pos;
    serverMoveSpeed_ = moveSpeed;

    // Check if AnimationController is available
    auto* animCtrl = node_ ? node_->GetComponent<AnimationController>(true) : nullptr;
    if (firstApply)
    {
        URHO3D_LOGINFOF("[ApplyServer] %s firstApply state=%d animCtrl=%s children=%u",
            node_ ? node_->GetName().CString() : "?", (int)newState,
            animCtrl ? "YES" : "NO",
            node_ ? node_->GetNumChildren() : 0);
    }

    if (state_ != newState || firstApply)
        SetState(newState);
}
