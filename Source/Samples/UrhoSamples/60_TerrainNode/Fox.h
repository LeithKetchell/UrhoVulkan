// Fox — small predator. Hunts rabbits, flees from players.

#pragma once

#include "Animal.h"

class Fox : public Animal
{
    URHO3D_OBJECT(Fox, Animal);

public:
    explicit Fox(Context* context);

    static void RegisterObject(Context* context);

protected:
    String GetModelPath() const override { return "Models/Animals/Fox.mdl"; }
    String GetIdleAnim() const override { return "Models/Animals/Fox_Idle.ani"; }
    String GetRunAnim() const override { return "Models/Animals/Fox_Gallop.ani"; }
    String GetDieAnim() const override { return "Models/Animals/Fox_Death.ani"; }

    float GetDesiredSize() const override { return 0.8f; }   // ~0.8m shoulder height
    float GetWanderRadius() const override { return 25.0f; }
    float GetWanderSpeed() const override { return 3.0f; }
    float GetFleeSpeed() const override { return 8.0f; }
    float GetFleeDistance() const override { return 25.0f; }
    float GetMinIdleDuration() const override { return 2.0f; }
    float GetMaxIdleDuration() const override { return 6.0f; }
};
