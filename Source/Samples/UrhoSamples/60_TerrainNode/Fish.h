// Fish — aquatic animal. Swims underwater, avoids shallows, reacts to camera.

#pragma once

#include "Animal.h"
#include <Urho3D/Graphics/Material.h>

class FishSpatialHash;

class Fish : public Animal
{
    URHO3D_OBJECT(Fish, Animal);

public:
    explicit Fish(Context* context);

    static void RegisterObject(Context* context);

    void Start() override;
    void Update(float timeStep) override;

    void SetMaterials(Material* base, Material* orbit, Material* stare);
    void SetCameraNode(Node* camNode) { cameraNode_ = WeakPtr<Node>(camNode); }
    void SetSpatialHash(FishSpatialHash* hash) { spatialHash_ = hash; }

protected:
    String GetModelPath() const override { return "Models/UrhoFish.mdl"; }
    String GetIdleAnim() const override { return String::EMPTY; }
    String GetRunAnim() const override { return String::EMPTY; }
    String GetDieAnim() const override { return String::EMPTY; }

    float GetWanderSpeed() const override { return 0.5f; }
    float GetWanderRadius() const override { return 75.0f; }
    float GetDrownTime() const override { return -1.0f; }  // aquatic — immune to drowning

    /// Fish override — no terrain snap, no land-based state machine
    void OnStateEnter(AnimalState newState) override;

private:
    void Swim(float timeStep);
    void ClampToWaterColumn();

    float waterLevel_{5.0f};
    float minDepth_{0.3f};
    float maxDepth_{4.0f};
    float turnSpeed_{2.0f};
    float swimSpeed_{0.5f};
    float comfortDist_{12.0f};
    float boundary_{75.0f};

    // Camera interaction
    float stareDist_{3.0f};
    float orbitNear_{5.0f};
    float orbitFar_{15.0f};

    // Player camera (passed in, not searched)
    WeakPtr<Node> cameraNode_;

    // Per-fish randomized schedule offset (0-1 range, shifts dawn/dusk sensitivity)
    float feedPhaseOffset_{0.0f};
    // Per-fish preferred depth fraction (0=floor, 1=ceiling) when not feeding
    float restingDepthFrac_{0.2f};
    // Per-fish feeding eagerness (how strongly this fish is drawn to surface)
    float feedEagerness_{1.0f};

    // Materials for wiggle zones
    SharedPtr<Material> baseMat_;
    SharedPtr<Material> orbitMat_;
    SharedPtr<Material> stareMat_;

protected:
    // Spatial hash (owned by TerrainNode, not by Fish)
    FishSpatialHash* spatialHash_{nullptr};
};
