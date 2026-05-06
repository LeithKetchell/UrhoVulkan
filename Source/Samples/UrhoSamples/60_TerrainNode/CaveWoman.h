// CaveWoman — proto-human NPC. Shares animations with CaveMan via Mixamo skeleton.

#pragma once

#include "CaveMan.h"  // for CaveAnims namespace

class CaveWoman : public HumanNPC
{
    URHO3D_OBJECT(CaveWoman, HumanNPC);

public:
    explicit CaveWoman(Context* context) : HumanNPC(context) {}

    static void RegisterObject(Context* context) { context->RegisterFactory<CaveWoman>(); }

protected:
    String GetModelPath() const override { return "Models/Characters/CavemanWoman.mdl"; }
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
    // Shared caveman rig — reuses the same pickup anim as CaveMan for the
    // brief reach/stoke gesture overlaid on SIT arrival.
    String GetTendAnim() const override { return String(CaveAnims::DIR) + "Caveman_Picking_Up.ani"; }

    bool HasVitals() const override { return true; }
    int GetCreatureId() const override { return 21; }
    float GetDesiredSize() const override { return 1.65f; }
    float GetWanderRadius() const override { return 20.0f; }
    float GetWanderSpeed() const override { return 1.8f; }
    float GetFleeSpeed() const override { return 5.5f; }
    float GetFleeDistance() const override { return 25.0f; }
    float GetMinIdleDuration() const override { return 5.0f; }
    float GetMaxIdleDuration() const override { return 12.0f; }
};
