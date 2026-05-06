// CaveMan — proto-human NPC. Shares animations with CaveWoman via Mixamo skeleton.

#pragma once

#include "HumanNPC.h"
#include <Urho3D/Math/MathDefs.h>

// Shared animation paths for caveman characters
namespace CaveAnims
{
    static const char* DIR = "Models/Characters/";

    // Single animations
    static const char* IDLE    = "Caveman_idle.ani";
    static const char* WALK    = "Caveman_walk.ani";
    static const char* RUN     = "Caveman_running.ani";
    static const char* DIE     = "Caveman_die.ani";
    static const char* GREET   = "Caveman_Standing_Greeting.ani";
    static const char* SWIM    = "Caveman_Swimming.ani";
    static const char* CROUCH  = "Caveman_Crouched_Walking.ani";
    static const char* ATTACK  = "Caveman_attack.ani";
    static const char* VICTORY = "Caveman_victory.ani";
    static const char* SCREAM  = "Caveman_scream.ani";
    static const char* JUMP    = "Caveman_jump.ani";

    // Variants — randomly picked each time
    static const char* EAT_VARIANTS[] = {
        "Caveman_Gathering_Objects.ani",
        "Caveman_Pick_Fruit.ani",
        "Caveman_Picking_Up.ani",
        "Caveman_Watering.ani"
    };
    static const char* SIT_VARIANTS[] = {
        "Caveman_Sitting_Idle.ani",
        "Caveman_Sitting_Dazed.ani"
    };
    static const char* SLEEP_VARIANTS[] = {
        "Caveman_Sleeping_Idle.ani",
        "Caveman_Laying_Sleeping.ani"
    };
    static const char* LOOK_VARIANTS[] = {
        "Caveman_Look_Around.ani",
        "Caveman_Looking_Around.ani"
    };

    inline String Pick(const char* variants[], unsigned count)
    {
        return String(DIR) + variants[Urho3D::Random((int)count)];
    }
}

class CaveMan : public HumanNPC
{
    URHO3D_OBJECT(CaveMan, HumanNPC);

public:
    explicit CaveMan(Context* context) : HumanNPC(context) {}

    static void RegisterObject(Context* context) { context->RegisterFactory<CaveMan>(); }

protected:
    String GetModelPath() const override { return "Models/Characters/CavemanMan.mdl"; }
    String GetModelDir() const override { return CaveAnims::DIR; }

    String GetIdleAnim() const override { return String(CaveAnims::DIR) + CaveAnims::IDLE; }
    String GetWalkAnim() const override { return String(CaveAnims::DIR) + CaveAnims::WALK; }
    String GetRunAnim() const override { return String(CaveAnims::DIR) + CaveAnims::RUN; }
    String GetDieAnim() const override { return String(CaveAnims::DIR) + CaveAnims::DIE; }
    String GetGreetAnim() const override { return String(CaveAnims::DIR) + CaveAnims::GREET; }
    String GetSwimAnim() const override { return String(CaveAnims::DIR) + CaveAnims::SWIM; }
    String GetJumpAnim() const override { return String(CaveAnims::DIR) + CaveAnims::JUMP; }
    String GetCrouchWalkAnim() const override { return String(CaveAnims::DIR) + CaveAnims::CROUCH; }

    String GetEatAnim() const override { return CaveAnims::Pick(CaveAnims::EAT_VARIANTS, 4); }
    String GetSitAnim() const override { return CaveAnims::Pick(CaveAnims::SIT_VARIANTS, 2); }
    String GetSleepAnim() const override { return CaveAnims::Pick(CaveAnims::SLEEP_VARIANTS, 2); }
    String GetLookAnim() const override { return CaveAnims::Pick(CaveAnims::LOOK_VARIANTS, 2); }
    String GetAttackAnim() const override { return String(CaveAnims::DIR) + CaveAnims::ATTACK; }
    String GetVictoryAnim() const override { return String(CaveAnims::DIR) + CaveAnims::VICTORY; }
    String GetScreamAnim() const override { return String(CaveAnims::DIR) + CaveAnims::SCREAM; }
    // Brief reach/stoke gesture overlaid on SIT when arriving at the fire —
    // reuses an existing gather-style anim rather than a new asset.
    String GetTendAnim() const override { return String(CaveAnims::DIR) + "Caveman_Picking_Up.ani"; }

    bool HasVitals() const override { return true; }
    int GetCreatureId() const override { return 20; }
    float GetDesiredSize() const override { return 1.8f; }
    float GetWanderRadius() const override { return 20.0f; }
    float GetWanderSpeed() const override { return 1.8f; }
    float GetFleeSpeed() const override { return 6.0f; }
    float GetFleeDistance() const override { return 25.0f; }
    float GetMinIdleDuration() const override { return 5.0f; }
    float GetMaxIdleDuration() const override { return 12.0f; }
};
