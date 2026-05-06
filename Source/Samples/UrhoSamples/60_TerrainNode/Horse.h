// Horse — large, fast herbivore. Wide wander, quick flee.

#pragma once

#include "LandAnimal.h"

class Horse : public LandAnimal
{
    URHO3D_OBJECT(Horse, LandAnimal);

public:
    explicit Horse(Context* context) : LandAnimal(context) {}

    static void RegisterObject(Context* context) { context->RegisterFactory<Horse>(); }

protected:
    String GetModelPath() const override { return "Models/Animals/Horse.mdl"; }
    String GetAnimPrefix() const override { return "Horse_AnimalArmature"; }

    int GetCreatureId() const override { return 10; }
    float GetFoodGrassWeight() const override { return 0.8f; }
    float GetFoodShrubWeight() const override { return 0.1f; }  // true grazer
    float GetDesiredSize() const override { return 2.0f; }
    float GetWanderRadius() const override { return 40.0f; }
    float GetWanderSpeed() const override { return 2.5f; }
    float GetFleeSpeed() const override { return 12.0f; }
    float GetFleeDistance() const override { return 45.0f; }
    float GetMinIdleDuration() const override { return 4.0f; }
    float GetMaxIdleDuration() const override { return 10.0f; }
    float GetVisionRange() const override { return 35.0f; }
    float GetVisionCosAngle() const override { return 0.3f; }  // ~145 degrees — wide-set eyes
};
