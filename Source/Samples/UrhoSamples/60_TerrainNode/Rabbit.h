// Rabbit — small herbivore. Sits, hops, flees when startled.

#pragma once

#include "Animal.h"

class Rabbit : public Animal
{
    URHO3D_OBJECT(Rabbit, Animal);

public:
    explicit Rabbit(Context* context);

    static void RegisterObject(Context* context);

protected:
    String GetModelPath() const override { return "Models/Animals/Rabbit.mdl"; }
    String GetIdleAnim() const override { return "Models/Animals/Rabbit_ArmatureSitting.000.ani"; }
    String GetRunAnim() const override { return "Models/Animals/Rabbit_ArmatureRunning.ani"; }
    String GetDieAnim() const override { return "Models/Animals/Rabbit_ArmatureDying.000.ani"; }

    float GetDesiredSize() const override { return 0.35f; }  // ~35cm body length
    float GetWanderRadius() const override { return 12.0f; }
    float GetWanderSpeed() const override { return 1.5f; }
    float GetFleeSpeed() const override { return 8.0f; }
    float GetFleeDistance() const override { return 15.0f; }
    float GetMinIdleDuration() const override { return 2.0f; }
    float GetMaxIdleDuration() const override { return 6.0f; }

    void OnStateEnter(AnimalState newState) override;
};
