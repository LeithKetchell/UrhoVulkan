// Bull — large, aggressive bovine. Territorial, charges when threatened.

#pragma once

#include "LandAnimal.h"

class Bull : public LandAnimal
{
    URHO3D_OBJECT(Bull, LandAnimal);

public:
    explicit Bull(Context* context) : LandAnimal(context) {}

    static void RegisterObject(Context* context) { context->RegisterFactory<Bull>(); }

protected:
    String GetModelPath() const override { return "Models/Animals/Bull.mdl"; }
    String GetAnimPrefix() const override { return "Bull_AnimalArmature"; }

    int GetCreatureId() const override { return 6; }
    float GetFoodGrassWeight() const override { return 0.7f; }
    float GetFoodShrubWeight() const override { return 0.2f; }  // heavy grazer
    float GetDesiredSize() const override { return 1.8f; }
    float GetWanderRadius() const override { return 20.0f; }
    float GetWanderSpeed() const override { return 1.5f; }
    float GetFleeSpeed() const override { return 7.0f; }
    float GetFleeDistance() const override { return 15.0f; }
    float GetMinIdleDuration() const override { return 5.0f; }
    float GetMaxIdleDuration() const override { return 15.0f; }
    float GetVisionRange() const override { return 25.0f; }
    float GetVisionCosAngle() const override { return 0.5f; }  // 120 degrees
};
