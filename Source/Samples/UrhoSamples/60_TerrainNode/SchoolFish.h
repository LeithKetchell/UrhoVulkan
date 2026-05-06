// SchoolFish — tiny fish that school together around their center of mass.
// Uses the same UrhoFish model at reduced scale with flocking behavior:
// cohesion (steer toward centroid), alignment (match school heading),
// separation (avoid crowding neighbours).

#pragma once

#include "Fish.h"

class SchoolStateCache;

class SchoolFish : public Fish
{
    URHO3D_OBJECT(SchoolFish, Fish);

public:
    explicit SchoolFish(Context* context);

    static void RegisterObject(Context* context);

    void Start() override;
    void Update(float timeStep) override;

    /// Assign this fish to a school. All fish sharing the same school ID will flock together.
    void SetSchoolID(unsigned id) { schoolID_ = id; }
    unsigned GetSchoolID() const { return schoolID_; }
    void SetSchoolCache(SchoolStateCache* cache) { schoolCache_ = cache; }

    bool IsSchoolFish() const override { return true; }

    // --- Vision: school fish are prey ---
    bool IsPredator() const override { return false; }
    float GetVisionRange() const override { return 15.0f; }
    float GetVisionCosAngle() const override { return 0.3f; }  // ~145 degrees — wide prey vision

protected:
    float GetWanderSpeed() const override { return 0.8f; }
    float GetWanderRadius() const override { return 40.0f; }

private:
    /// Compute school centroid and average heading from scene siblings.
    void ComputeSchoolState(Vector3& centroid, Vector3& avgHeading, unsigned& count);

    unsigned schoolID_{0};

    // Flocking weights
    float cohesionWeight_{1.5f};     // pull toward centroid
    float alignmentWeight_{1.0f};    // match school heading
    float separationWeight_{2.0f};   // push away from close neighbours
    float separationDist_{0.8f};     // min distance before separation kicks in
    float schoolTurnSpeed_{3.0f};    // faster turning for tight schooling

    // School state cache (owned by TerrainNode, not by SchoolFish)
    SchoolStateCache* schoolCache_{nullptr};

    /// Reusable query buffers — avoid per-frame heap allocations.
    mutable Vector<SchoolFish*> schoolBuf_;
    mutable Vector<Fish*> separationBuf_;

    /// Scatter state — when a predator is sighted, break cohesion temporarily.
    bool scattering_{false};
    float scatterTimer_{0.0f};
    Vector3 scatterDir_;  ///< Individual flee direction (away from predator)
    static constexpr float SCATTER_DURATION = 4.0f;   ///< How long to flee before reforming
    static constexpr float SCATTER_SPEED    = 1.6f;    ///< Flee speed (2x normal wander)
};
