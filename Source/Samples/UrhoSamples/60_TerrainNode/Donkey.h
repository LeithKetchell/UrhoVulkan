// Donkey — sturdy pack animal. Slower than horse, calmer.

#pragma once

#include "LandAnimal.h"

class Donkey : public LandAnimal
{
    URHO3D_OBJECT(Donkey, LandAnimal);

public:
    explicit Donkey(Context* context) : LandAnimal(context)
    {
        vocalizationSoundPath_ = "Sounds/Animals/DonkeyVocalization.ogg";
    }

    static void RegisterObject(Context* context) { context->RegisterFactory<Donkey>(); }

protected:
    String GetModelPath() const override { return "Models/Animals/Donkey.mdl"; }
    String GetAnimPrefix() const override { return "Donkey_AnimalArmature"; }

    int GetCreatureId() const override { return 9; }
    float GetFoodGrassWeight() const override { return 0.7f; }
    float GetFoodShrubWeight() const override { return 0.2f; }  // grazer, some browse
    float GetDesiredSize() const override { return 1.3f; }
    float GetWanderRadius() const override { return 20.0f; }
    float GetWanderSpeed() const override { return 1.5f; }
    float GetFleeSpeed() const override { return 6.0f; }
    float GetFleeDistance() const override { return 25.0f; }
    float GetMinIdleDuration() const override { return 5.0f; }
    float GetMaxIdleDuration() const override { return 12.0f; }
    float GetVisionRange() const override { return 25.0f; }
    float GetVisionCosAngle() const override { return 0.4f; }  // ~130 degrees
};
