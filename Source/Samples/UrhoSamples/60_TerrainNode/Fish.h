// Fish — aquatic creature. Swims underwater, avoids shallows, reacts to camera.

#pragma once

#include "WaterAnimal.h"
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/StaticModel.h>

class FishSpatialHash;

class Fish : public WaterAnimal
{
    URHO3D_OBJECT(Fish, WaterAnimal);

public:
    explicit Fish(Context* context);

    static void RegisterObject(Context* context);

    void Start() override;
    void Update(float timeStep) override;

    void SetMaterials(Material* base, Material* orbit, Material* stare);
    void SetCameraNode(Node* camNode) { cameraNode_ = WeakPtr<Node>(camNode); }
    void SetSpatialHash(FishSpatialHash* hash) { spatialHash_ = hash; }

    /// Type flag for fast RTTI avoidance in spatial hash queries.
    virtual bool IsSchoolFish() const { return false; }

    // --- Vision: big fish are predators ---
    bool IsPredator() const override { return true; }
    float GetVisionRange() const override { return 20.0f; }
    float GetVisionCosAngle() const override { return 0.5f; }  // 120 degrees

protected:
    String GetModelPath() const override { return "Models/UrhoFish.mdl"; }

    float GetWanderSpeed() const override { return 0.5f; }
    float GetWanderRadius() const override { return 75.0f; }

private:
    void Swim(float timeStep);

    float turnSpeed_{2.0f};
    float swimSpeed_{0.5f};
    float comfortDist_{12.0f};
    float boundary_{75.0f};

    // Camera interaction
    float stareDist_{3.0f};
    float orbitNear_{5.0f};
    float orbitFar_{15.0f};

    WeakPtr<Node> cameraNode_;

    // Materials for wiggle zones
    SharedPtr<Material> baseMat_;
    SharedPtr<Material> orbitMat_;
    SharedPtr<Material> stareMat_;

    /// Cached StaticModel — avoids GetComponent hash lookup every frame.
    WeakPtr<StaticModel> cachedModel_;

    /// Reusable query buffer — avoids heap allocation every frame.
    mutable Vector<Fish*> nearbyBuf_;

    /// Hunt target (big fish chasing school fish). WeakPtr auto-nulls on removal.
    WeakPtr<Node> huntTarget_;
    /// Seconds remaining of "memory" after prey leaves vision cone.
    float huntTimer_{0.0f};
    /// True while actively pursuing prey.
    bool hunting_{false};

protected:
    FishSpatialHash* spatialHash_{nullptr};
};
