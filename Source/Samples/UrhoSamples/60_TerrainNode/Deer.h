// Deer — medium herbivore. Grazes, wanders wide, flees at distance.

#pragma once

#include "Animal.h"

class Deer : public Animal
{
    URHO3D_OBJECT(Deer, Animal);

public:
    explicit Deer(Context* context);

    static void RegisterObject(Context* context);

protected:
    String GetModelPath() const override { return "Models/Animals/Deer.mdl"; }
    String GetIdleAnim() const override { return "Models/Animals/Deer_Idle_ArmatureIdle.ani"; }
    String GetRunAnim() const override { return "Models/Animals/Deer_Run_ArmatureRun.ani"; }
    String GetDieAnim() const override { return "Models/Animals/Deer_Die_ArmatureDie.001.ani"; }

    float GetDesiredSize() const override { return 1.5f; }  // ~1.5m shoulder height
    float GetWanderRadius() const override { return 30.0f; }
    float GetWanderSpeed() const override { return 2.5f; }
    float GetFleeSpeed() const override { return 10.0f; }
    float GetFleeDistance() const override { return 40.0f; }
    float GetMinIdleDuration() const override { return 4.0f; }
    float GetMaxIdleDuration() const override { return 10.0f; }
};
