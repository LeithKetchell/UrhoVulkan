// SchoolFish — tiny schooling fish with flocking behavior.

#include "SchoolFish.h"

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

    const Vector<SharedPtr<Node>>& children = GetScene()->GetChildren();
    for (unsigned i = 0; i < children.Size(); ++i)
    {
        Node* other = children[i];
        if (other == node_ || other->GetName() != "SchoolFish")
            continue;

        auto* otherFish = other->GetComponent<SchoolFish>();
        if (!otherFish || otherFish->GetSchoolID() != schoolID_)
            continue;

        Vector3 otherPos = other->GetWorldPosition();
        Quaternion otherRot = other->GetWorldRotation();
        Vector3 otherFwd = otherRot * Vector3::BACK;  // fish face -Z

        centroid += otherPos;
        avgHeading += otherFwd;
        ++count;
    }

    if (count > 0)
    {
        centroid /= (float)count;
        if (avgHeading.LengthSquared() > 0.001f)
            avgHeading.Normalize();
    }
}

void SchoolFish::Update(float timeStep)
{
    // Let base Fish handle water column clamping, shallow avoidance, boundary, camera interaction
    // But we override the movement to add schooling

    Vector3 pos = node_->GetWorldPosition();
    Quaternion rot = node_->GetWorldRotation();
    Vector3 forward = rot * Vector3::BACK;

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

        // Separation — push away from too-close neighbours
        const Vector<SharedPtr<Node>>& children = GetScene()->GetChildren();
        for (unsigned i = 0; i < children.Size(); ++i)
        {
            Node* other = children[i];
            if (other == node_ || other->GetName() != "SchoolFish")
                continue;

            auto* otherFish = other->GetComponent<SchoolFish>();
            if (!otherFish || otherFish->GetSchoolID() != schoolID_)
                continue;

            Vector3 diff = pos - other->GetWorldPosition();
            float dist = diff.Length();
            if (dist < separationDist_ && dist > 0.001f)
            {
                diff.Normalize();
                float urgency = 1.0f - (dist / separationDist_);
                desiredDir += diff * urgency * separationWeight_;
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
