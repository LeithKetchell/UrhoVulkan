// Fish — aquatic animal. Swims underwater, avoids shallows, reacts to camera.

#include "Fish.h"
#include "FishSpatialHash.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Math/MathDefs.h>
#include <Urho3D/IO/Log.h>

Fish::Fish(Context* context) :
    Animal(context)
{
    // Fish use regular Update (like original UpdateFish), not FixedUpdate
    SetUpdateEventMask(LogicComponentEvents::Update);
}

void Fish::RegisterObject(Context* context)
{
    context->RegisterFactory<Fish>();
}

void Fish::Start()
{
    // Cache terrain — same as Animal base
    terrain_ = GetScene()->GetComponent<Terrain>(true);

    if (!cameraNode_)
        URHO3D_LOGWARNING("Fish: no camera node set!");

    // Randomize per-fish schedule so they don't all behave identically
    feedPhaseOffset_ = Random(0.0f, 0.4f);       // shifts when this fish considers it "dawn/dusk"
    restingDepthFrac_ = Random(0.1f, 0.4f);      // how deep they rest (0=floor, 1=surface)
    feedEagerness_ = Random(0.5f, 1.0f);         // how strongly they rise to feed
}

void Fish::SetMaterials(Material* base, Material* orbit, Material* stare)
{
    baseMat_ = SharedPtr<Material>(base);
    orbitMat_ = SharedPtr<Material>(orbit);
    stareMat_ = SharedPtr<Material>(stare);
}

void Fish::OnStateEnter(AnimalState /*newState*/)
{
    // Fish have no skeletal animations — wiggle is in the vertex shader
}

void Fish::Update(float timeStep)
{
    Swim(timeStep);
    ClampToWaterColumn();
}

void Fish::Swim(float timeStep)
{
    Vector3 pos = node_->GetWorldPosition();
    Quaternion rot = node_->GetWorldRotation();
    // Fish model faces -Z, so forward is BACK
    Vector3 forward = rot * Vector3::BACK;

    // --- Neighbour avoidance via spatial hash ---
    Vector3 desiredDir = forward;
    float nearestDist = M_INFINITY;
    Vector3 nearestDir;

    if (spatialHash_)
    {
        Vector<Fish*> nearby;
        spatialHash_->Query(pos, comfortDist_, nearby);
        for (unsigned i = 0; i < nearby.Size(); ++i)
        {
            if (nearby[i] == this) continue;
            Vector3 diff = nearby[i]->GetNode()->GetWorldPosition() - pos;
            float dist = diff.Length();
            if (dist < nearestDist)
            {
                nearestDist = dist;
                nearestDir = diff;
            }
        }
    }

    if (nearestDist < comfortDist_ && nearestDist > 0.001f)
    {
        float urgency = 1.0f - (nearestDist / comfortDist_);
        Vector3 awayDir = -nearestDir.Normalized();
        awayDir.y_ = 0.0f;
        if (awayDir.LengthSquared() > 0.001f)
            awayDir.Normalize();
        else
            awayDir = forward;
        desiredDir = forward.Lerp(awayDir, urgency);
    }

    // --- Camera interaction ---
    Node* camNode = cameraNode_;
    int fishZone = 0;  // 0=normal, 1=orbit, 2=stare

    if (camNode)
    {
        Vector3 camPos = camNode->GetWorldPosition();
        Vector3 toCam3D = camPos - pos;
        Vector3 toCamFlat = toCam3D;
        toCamFlat.y_ = 0.0f;
        float camDist = toCamFlat.Length();

        if (camDist < 1.0f && camDist > 0.01f)
        {
            // Too close — slide off tangentially
            Vector3 radial = toCamFlat.LengthSquared() > 0.001f ? toCamFlat.Normalized() : forward;
            Vector3 tangentCW(radial.z_, 0.0f, -radial.x_);
            Vector3 tangentCCW(-radial.z_, 0.0f, radial.x_);
            desiredDir = (forward.DotProduct(tangentCW) >= forward.DotProduct(tangentCCW)) ? tangentCW : tangentCCW;
            fishZone = 1;
        }
        else if (camDist < stareDist_ && camDist > 0.5f)
        {
            // Face the camera in full 3D
            Vector3 faceCam = toCam3D;
            if (faceCam.LengthSquared() > 0.001f)
                faceCam.Normalize();
            desiredDir = faceCam;
            fishZone = 2;
        }
        else if (camDist < orbitFar_ && camDist > 1.0f)
        {
            // Circle around camera
            Vector3 radial = toCamFlat / camDist;
            Vector3 tangentCW(radial.z_, 0.0f, -radial.x_);
            Vector3 tangentCCW(-radial.z_, 0.0f, radial.x_);
            Vector3 tangent = (forward.DotProduct(tangentCW) >= forward.DotProduct(tangentCCW)) ? tangentCW : tangentCCW;
            float orbitStrength = Clamp(1.0f - (camDist - orbitNear_) / (orbitFar_ - orbitNear_), 0.0f, 0.8f);
            desiredDir = desiredDir.Lerp(tangent, orbitStrength);
            fishZone = 1;
        }
    }

    // --- Random wander impulse ---
    if (Random(1.0f) < 0.02f)
    {
        float wanderAngle = Random(-90.0f, 90.0f);
        Quaternion wanderRot(wanderAngle, Vector3::UP);
        desiredDir = wanderRot * desiredDir;
        desiredDir.y_ += Random(-0.15f, 0.15f);
    }

    // --- Boundary avoidance ---
    float distFromCenter = Vector2(pos.x_, pos.z_).Length();
    if (distFromCenter > boundary_)
    {
        Vector3 toCenter = -pos;
        toCenter.y_ = 0.0f;
        toCenter.Normalize();
        float boundaryUrgency = Clamp((distFromCenter - boundary_) / 10.0f, 0.0f, 1.0f);
        desiredDir = desiredDir.Lerp(toCenter, boundaryUrgency);
    }

    // --- Shallow water avoidance ---
    if (terrain_)
    {
        Vector3 probe = pos + forward * 3.0f;
        float probeH = terrain_->GetHeight(probe);
        float probeDepth = waterLevel_ - probeH;
        if (probeDepth < 1.5f)
        {
            Vector3 toDeep = pos - probe;
            toDeep.y_ = 0.0f;
            if (toDeep.LengthSquared() > 0.001f)
                toDeep.Normalize();
            float shallowUrgency = Clamp(1.0f - (probeDepth / 1.5f), 0.0f, 1.0f);
            desiredDir = desiredDir.Lerp(toDeep, shallowUrgency);
        }
    }

    // --- Finalize direction ---
    desiredDir.y_ *= 0.8f;
    if (desiredDir.LengthSquared() > 0.001f)
        desiredDir.Normalize();
    else
        desiredDir = forward;

    // Slerp toward desired heading (model faces -Z, so negate)
    Quaternion targetRot;
    targetRot.FromLookRotation(-desiredDir);
    rot = rot.Slerp(targetRot, turnSpeed_ * timeStep);
    node_->SetRotation(rot);

    // Move forward — slow down near camera
    float speedFactor = 1.0f;
    if (camNode)
    {
        Vector3 toCamFlat = camNode->GetWorldPosition() - pos;
        toCamFlat.y_ = 0.0f;
        float camDist = toCamFlat.Length();
        if (camDist < orbitFar_)
            speedFactor = Lerp(0.15f, 1.0f, Clamp(camDist / orbitFar_, 0.0f, 1.0f));
    }

    forward = rot * Vector3::BACK;
    pos += forward * swimSpeed_ * speedFactor * timeStep;
    node_->SetPosition(pos);

    // --- Material swap based on zone ---
    auto* sm = node_->GetComponent<StaticModel>();
    if (sm)
    {
        Material* want = (fishZone == 2) ? stareMat_.Get() :
                         (fishZone == 1) ? orbitMat_.Get() :
                                           baseMat_.Get();
        if (want && sm->GetMaterial() != want)
            sm->SetMaterial(want);
    }
}

void Fish::ClampToWaterColumn()
{
    Vector3 pos = node_->GetWorldPosition();
    float terrainH = terrain_ ? terrain_->GetHeight(pos) : 0.0f;
    float floorY = terrainH + minDepth_;
    float ceilY = waterLevel_ - minDepth_;
    if (floorY > ceilY)
        floorY = ceilY;

    // Dawn/dusk feeding — fish rise toward the surface only near horizon.
    // Default: stay deep. Each fish has a randomized schedule offset + eagerness.
    float feedBias = 0.0f;  // 0 = resting depth, 1 = surface
    Node* sunLightNode = GetScene() ? GetScene()->GetChild("DirectionalLight", true) : nullptr;
    if (sunLightNode)
    {
        Vector3 lightDir = sunLightNode->GetDirection();
        // Sun altitude proxy: -lightDir.y_ (positive when sun is up)
        float sunUp = -lightDir.y_;
        // Near horizon: sunUp close to 0 (dawn/dusk)
        // Per-fish offset shifts the "horizon" threshold so they don't all react at once
        float horizonProximity = 1.0f - Abs(sunUp) - feedPhaseOffset_;
        horizonProximity = Clamp(horizonProximity, 0.0f, 1.0f);
        // Sharpen: only strong effect when very close to horizon, scaled by eagerness
        feedBias = horizonProximity * horizonProximity * feedEagerness_;
    }

    // Resting depth: each fish has its own preferred resting fraction (deep by default)
    float restingY = Lerp(floorY, ceilY, restingDepthFrac_);
    // Feeding depth: near the surface
    float feedingY = ceilY;
    float preferredY = Lerp(restingY, feedingY, feedBias);

    // Nudge toward preferred depth — 5% per frame for visible movement
    pos.y_ = Lerp(pos.y_, preferredY, 0.05f);

    pos.y_ = Clamp(pos.y_, floorY, ceilY);
    node_->SetPosition(pos);
}
