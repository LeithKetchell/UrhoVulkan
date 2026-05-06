// SchoolFish — tiny schooling fish with flocking behavior.

#include "SchoolFish.h"
#include "FishSpatialHash.h"
#include "SchoolState.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Math/MathDefs.h>

SchoolFish::SchoolFish(Context* context) :
    Fish(context)
{
}

void SchoolFish::RegisterObject(Context* context)
{
    context->RegisterFactory<SchoolFish>();
}

void SchoolFish::Start()
{
    Fish::Start();
}

void SchoolFish::ComputeSchoolState(Vector3& centroid, Vector3& avgHeading, unsigned& count)
{
    centroid = Vector3::ZERO;
    avgHeading = Vector3::ZERO;
    count = 0;

    // Use school state cache — first fish to call triggers computation, rest get cached result
    if (schoolCache_)
    {
        SchoolState& state = schoolCache_->GetState(schoolID_);
        if (state.frameComputed == schoolCache_->GetCurrentFrame())
        {
            // Already computed this frame — use cached
            centroid = state.centroid;
            avgHeading = state.averageHeading;
            count = (unsigned)state.memberCount;
            return;
        }

        // First fish in this school this frame — compute via spatial hash
        if (spatialHash_)
        {
            schoolBuf_.Clear();
            spatialHash_->QuerySchool(node_->GetWorldPosition(), GetWanderRadius(), schoolID_, schoolBuf_);

            for (unsigned i = 0; i < schoolBuf_.Size(); ++i)
            {
                Node* n = schoolBuf_[i]->GetNode();
                centroid += n->GetWorldPosition();
                avgHeading += n->GetWorldRotation() * Vector3::BACK;
                ++count;
            }

            if (count > 0)
            {
                centroid /= (float)count;
                if (avgHeading.LengthSquared() > 0.001f)
                    avgHeading.Normalize();
            }

            state.centroid = centroid;
            state.averageHeading = avgHeading;
            state.memberCount = (int)count;
            state.frameComputed = schoolCache_->GetCurrentFrame();
            return;
        }
    }

    // Fallback: no hash/cache available (shouldn't happen in normal operation)
}

void SchoolFish::Update(float timeStep)
{
    Vector3 pos = node_->GetWorldPosition();
    Quaternion rot = node_->GetWorldRotation();
    Vector3 forward = rot * Vector3::BACK;

    // --- Predator scan: check for visible big fish ---
    if (!scattering_ && spatialHash_)
    {
        separationBuf_.Clear();
        spatialHash_->Query(pos, GetVisionRange(), separationBuf_);
        for (unsigned i = 0; i < separationBuf_.Size(); ++i)
        {
            Fish* other = separationBuf_[i];
            if (other == static_cast<Fish*>(this)) continue;
            if (other->IsSchoolFish()) continue;  // only flee from big fish
            if (!CanSee(other)) continue;

            // Predator spotted — scatter!
            Vector3 awayDir = pos - other->GetNode()->GetWorldPosition();
            awayDir.y_ *= 0.3f;  // mostly horizontal scatter
            if (awayDir.LengthSquared() < 0.01f)
                awayDir = Vector3(Random(-1.0f, 1.0f), 0.0f, Random(-1.0f, 1.0f));
            awayDir.Normalize();
            // Add individual randomness so the school doesn't flee in a sheet
            awayDir += Vector3(Random(-0.4f, 0.4f), Random(-0.1f, 0.1f), Random(-0.4f, 0.4f));
            awayDir.Normalize();

            scattering_ = true;
            scatterTimer_ = SCATTER_DURATION;
            scatterDir_ = awayDir;
            break;  // one predator is enough
        }
    }

    // --- Scatter mode: flee independently, ignore school ---
    if (scattering_)
    {
        scatterTimer_ -= timeStep;
        if (scatterTimer_ <= 0.0f)
        {
            scattering_ = false;
            // Fall through to normal schooling — cohesion will reform
        }
        else
        {
            // Flee in scatter direction at burst speed
            Vector3 desiredDir = scatterDir_;

            // Boundary avoidance still applies
            float boundary = GetWanderRadius();
            float distFromCenter = Vector2(pos.x_, pos.z_).Length();
            if (distFromCenter > boundary)
            {
                Vector3 toCenter = -pos;
                toCenter.y_ = 0.0f;
                toCenter.Normalize();
                float boundaryUrgency = Clamp((distFromCenter - boundary) / 10.0f, 0.0f, 1.0f);
                desiredDir = desiredDir.Lerp(toCenter, boundaryUrgency);
            }

            // Shallow avoidance
            if (terrain_)
            {
                Vector3 probe = pos + desiredDir * 2.0f;
                float probeH = terrain_->GetHeight(probe);
                float waterLevel = 5.0f;
                float probeDepth = waterLevel - probeH;
                if (probeDepth < 1.0f)
                {
                    Vector3 toDeep = pos - probe;
                    toDeep.y_ = 0.0f;
                    if (toDeep.LengthSquared() > 0.001f)
                        toDeep.Normalize();
                    desiredDir = desiredDir.Lerp(toDeep, 0.8f);
                }
            }

            desiredDir.y_ *= 0.5f;
            if (desiredDir.LengthSquared() > 0.001f)
                desiredDir.Normalize();

            Quaternion targetRot;
            targetRot.FromLookRotation(-desiredDir);
            rot = rot.Slerp(targetRot, schoolTurnSpeed_ * 1.5f * timeStep);
            node_->SetRotation(rot);

            forward = rot * Vector3::BACK;
            pos += forward * SCATTER_SPEED * timeStep;

            // Water column clamp
            float waterLevel = 5.0f;
            float terrainH = terrain_ ? terrain_->GetHeight(pos) : 0.0f;
            float floorY = terrainH + 0.2f;
            float ceilY = waterLevel - 0.2f;
            if (floorY > ceilY) floorY = ceilY;
            pos.y_ = Clamp(pos.y_, floorY, ceilY);

            node_->SetPosition(pos);
            return;  // skip normal schooling this frame
        }
    }

    // --- Normal schooling behavior ---

    // --- Schooling forces ---
    Vector3 centroid;
    Vector3 avgHeading;
    unsigned schoolSize;
    ComputeSchoolState(centroid, avgHeading, schoolSize);

    Vector3 desiredDir = forward;

    if (schoolSize > 0)
    {
        // Cohesion — steer toward school centroid
        Vector3 toCentroid = centroid - pos;
        float distToCentroid = toCentroid.Length();
        if (distToCentroid > 0.5f)
        {
            toCentroid.Normalize();
            desiredDir += toCentroid * cohesionWeight_;
        }

        // Alignment — match average school heading
        desiredDir += avgHeading * alignmentWeight_;

        // Separation — push away from too-close neighbours via spatial hash
        if (spatialHash_)
        {
            separationBuf_.Clear();
            spatialHash_->Query(pos, separationDist_, separationBuf_);
            for (unsigned i = 0; i < separationBuf_.Size(); ++i)
            {
                if (separationBuf_[i] == static_cast<Fish*>(this)) continue;
                Vector3 diff = pos - separationBuf_[i]->GetNode()->GetWorldPosition();
                float dist = diff.Length();
                if (dist < separationDist_ && dist > 0.001f)
                {
                    diff.Normalize();
                    float urgency = 1.0f - (dist / separationDist_);
                    desiredDir += diff * urgency * separationWeight_;
                }
            }
        }
    }

    // --- Random wander (smaller than regular fish) ---
    if (Random(1.0f) < 0.01f)
    {
        float wanderAngle = Random(-45.0f, 45.0f);
        Quaternion wanderRot(wanderAngle, Vector3::UP);
        desiredDir = wanderRot * desiredDir;
        desiredDir.y_ += Random(-0.05f, 0.05f);
    }

    // --- Boundary avoidance ---
    float boundary = GetWanderRadius();
    float distFromCenter = Vector2(pos.x_, pos.z_).Length();
    if (distFromCenter > boundary)
    {
        Vector3 toCenter = -pos;
        toCenter.y_ = 0.0f;
        toCenter.Normalize();
        float boundaryUrgency = Clamp((distFromCenter - boundary) / 10.0f, 0.0f, 1.0f);
        desiredDir = desiredDir.Lerp(toCenter, boundaryUrgency);
    }

    // --- Shallow water avoidance ---
    if (terrain_)
    {
        Vector3 probe = pos + forward * 2.0f;
        float probeH = terrain_->GetHeight(probe);
        float waterLevel = 5.0f;
        float probeDepth = waterLevel - probeH;
        if (probeDepth < 1.0f)
        {
            Vector3 toDeep = pos - probe;
            toDeep.y_ = 0.0f;
            if (toDeep.LengthSquared() > 0.001f)
                toDeep.Normalize();
            float shallowUrgency = Clamp(1.0f - (probeDepth / 1.0f), 0.0f, 1.0f);
            desiredDir = desiredDir.Lerp(toDeep, shallowUrgency);
        }
    }

    // --- Finalize direction ---
    desiredDir.y_ *= 0.5f;  // dampen vertical movement — schools stay at level
    if (desiredDir.LengthSquared() > 0.001f)
        desiredDir.Normalize();
    else
        desiredDir = forward;

    // Slerp toward desired heading
    Quaternion targetRot;
    targetRot.FromLookRotation(-desiredDir);
    rot = rot.Slerp(targetRot, schoolTurnSpeed_ * timeStep);
    node_->SetRotation(rot);

    // Move forward
    forward = rot * Vector3::BACK;
    float speed = GetWanderSpeed();
    pos += forward * speed * timeStep;

    // Clamp to water column
    float waterLevel = 5.0f;
    float terrainH = terrain_ ? terrain_->GetHeight(pos) : 0.0f;
    float floorY = terrainH + 0.2f;
    float ceilY = waterLevel - 0.2f;
    if (floorY > ceilY)
        floorY = ceilY;
    pos.y_ = Clamp(pos.y_, floorY, ceilY);

    node_->SetPosition(pos);
}
