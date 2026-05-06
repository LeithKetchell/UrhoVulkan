// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "BuildingSystem.h"

#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/IO/VectorBuffer.h>
#include <Urho3D/Network/Protocol.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>

BuildingSystem::BuildingSystem(Context* context)
    : Component(context)
{
}

void BuildingSystem::SetBuildMode(bool enabled, int buildingTypeId)
{
    if (enabled == buildMode_ && buildingTypeId == currentBuildTypeId_)
        return;

    buildMode_ = enabled;
    currentBuildTypeId_ = buildingTypeId;

    if (buildMode_)
        CreateGhostNode(buildingTypeId);
    else
        DestroyGhostNode();
}

const BuildingTypeInfo* BuildingSystem::FindTypeInfo(int typeId) const
{
    auto it = typeMap_.Find(typeId);
    return (it != typeMap_.End()) ? it->second_ : nullptr;
}

SnapResult BuildingSystem::FindSnapPoint(const Vector3& cursorPos, int buildingTypeId, Terrain* terrain) const
{
    SnapResult result;

    const BuildingTypeInfo* newInfo = FindTypeInfo(buildingTypeId);
    if (!newInfo || newInfo->snapType == "free" || newInfo->snapType == "interior")
        return result;

    float bestDist = SNAP_DISTANCE;

    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        const PlacedBuilding& existing = placedBuildings_[i];
        if (existing.snapType.Empty() || existing.snapType == "free" || existing.snapType == "interior")
            continue;

        // Check if there's a snap rule for this combination
        const SnapRule* rule = nullptr;
        for (unsigned r = 0; r < snapRules_.Size(); ++r)
        {
            if (snapRules_[r].fromType == newInfo->snapType && snapRules_[r].toType == existing.snapType)
            {
                rule = &snapRules_[r];
                break;
            }
        }
        if (!rule)
            continue;

        // Compute the two endpoint anchors of the existing building
        Quaternion existingRot(existing.rotation, Vector3::UP);
        Vector3 dir = existingRot * Vector3::RIGHT;  // wall extends along local X
        float halfW = existing.footprintX * 0.5f;
        Vector3 anchorRight = existing.position + dir * halfW;
        Vector3 anchorLeft  = existing.position - dir * halfW;

        // Check distance to each anchor (XZ plane only for snap detection)
        float dxR = cursorPos.x_ - anchorRight.x_, dzR = cursorPos.z_ - anchorRight.z_;
        float distRight = sqrtf(dxR * dxR + dzR * dzR);
        float dxL = cursorPos.x_ - anchorLeft.x_, dzL = cursorPos.z_ - anchorLeft.z_;
        float distLeft = sqrtf(dxL * dxL + dzL * dzL);

        // Test right anchor
        if (distRight < bestDist)
        {
            float halfNew = newInfo->footprintX * 0.5f;
            Vector3 snapPos = existing.position + dir * (halfW + halfNew);
            if (terrain)
                snapPos.y_ = terrain->GetHeight(snapPos);

            float snapRot = existing.rotation;
            if (rule->align == "corner")
                snapRot += 90.0f;

            result.found = true;
            result.position = snapPos;
            result.rotation = snapRot;
            result.snappedToId = existing.placedId;
            bestDist = distRight;
        }

        // Test left anchor
        if (distLeft < bestDist)
        {
            float halfNew = newInfo->footprintX * 0.5f;
            Vector3 snapPos = existing.position - dir * (halfW + halfNew);
            if (terrain)
                snapPos.y_ = terrain->GetHeight(snapPos);

            float snapRot = existing.rotation;
            if (rule->align == "corner")
                snapRot -= 90.0f;

            result.found = true;
            result.position = snapPos;
            result.rotation = snapRot;
            result.snappedToId = existing.placedId;
            bestDist = distLeft;
        }
    }

    return result;
}

void BuildingSystem::UpdateGhostPreview(Camera* camera, Terrain* terrain, float waterLevel)
{
    if (!buildMode_ || !ghostNode_ || !camera || !terrain)
        return;

    auto* graphics = GetSubsystem<Graphics>();
    auto* input = GetSubsystem<Input>();
    IntVector2 mousePos = input->GetMousePosition();

    float mx = (float)mousePos.x_ / (float)graphics->GetWidth();
    float my = (float)mousePos.y_ / (float)graphics->GetHeight();
    Ray ray = camera->GetScreenRay(mx, my);

    // Intersect ray with terrain — coarse step then binary refinement
    // Coarse: 8-unit steps to find bracket, then 8 binary iterations to refine.
    // Worst case: ~33 terrain queries instead of ~400.
    Vector3 hitPos;
    bool hit = false;
    float tLo = 1.0f, tHi = 200.0f;
    {
        float prevT = tLo;
        for (float t = tLo; t < tHi; t += 8.0f)
        {
            Vector3 p = ray.origin_ + ray.direction_ * t;
            if (p.y_ <= terrain->GetHeight(p))
            {
                // Found bracket [prevT, t] — binary search to refine
                float lo = prevT, hi = t;
                for (int iter = 0; iter < 8; ++iter)
                {
                    float mid = (lo + hi) * 0.5f;
                    Vector3 pm = ray.origin_ + ray.direction_ * mid;
                    if (pm.y_ <= terrain->GetHeight(pm))
                        hi = mid;
                    else
                        lo = mid;
                }
                Vector3 ph = ray.origin_ + ray.direction_ * hi;
                hitPos = Vector3(ph.x_, terrain->GetHeight(ph), ph.z_);
                hit = true;
                break;
            }
            prevT = t;
        }
    }

    if (!hit)
        return;

    // Try snap detection for wall/gate/corner pieces
    SnapResult snap = FindSnapPoint(hitPos, currentBuildTypeId_, terrain);
    if (snap.found)
    {
        ghostPosition_ = snap.position;
        ghostRotation_ = snap.rotation;
        snappedToId_ = snap.snappedToId;
    }
    else
    {
        ghostPosition_ = hitPos;
        snappedToId_ = -1;
    }

    ghostNode_->SetPosition(ghostPosition_);
    ghostNode_->SetRotation(Quaternion(ghostRotation_, Vector3::UP));

    ghostValid_ = ValidatePlacement(ghostPosition_, currentBuildTypeId_, terrain, waterLevel);
    UpdateGhostColor();
}

void BuildingSystem::RotateGhost()
{
    ghostRotation_ += 45.0f;
    if (ghostRotation_ >= 360.0f)
        ghostRotation_ -= 360.0f;

    if (ghostNode_)
        ghostNode_->SetRotation(Quaternion(ghostRotation_, Vector3::UP));
}

bool BuildingSystem::ValidatePlacement(const Vector3& pos, int buildingTypeId,
                                        Terrain* terrain, float waterLevel) const
{
    const BuildingTypeInfo* info = FindTypeInfo(buildingTypeId);
    if (!info)
        return false;

    // 1. Slope check — terrain normal must be mostly upward
    Vector3 normal = terrain->GetNormal(pos);
    if (normal.y_ < 0.85f)  // ~30 degree slope max
        return false;

    // 2. Water check — can't build underwater (except fish weir)
    if (pos.y_ < waterLevel && info->name != "Fish Weir")
        return false;

    // 3. Overlap check — no existing building too close
    //    Skip the building we're snapping to (it's supposed to be adjacent)
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == snappedToId_)
            continue;
        float minDist = (info->footprintX + info->footprintZ) * 0.5f;
        float minDist2 = minDist * minDist;
        if ((placedBuildings_[i].position - pos).LengthSquared() < minDist2)
            return false;
    }

    return true;
}

void BuildingSystem::RequestBuild(Connection* serverConn)
{
    if (!buildMode_ || !ghostValid_)
        return;

    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(currentBuildTypeId_);
        buf.WriteFloat(ghostPosition_.x_);
        buf.WriteFloat(ghostPosition_.y_);
        buf.WriteFloat(ghostPosition_.z_);
        buf.WriteFloat(ghostRotation_);
        buf.WriteI32(snappedToId_);  // -1 = freestanding
        serverConn->SendMessage(MSG_BUILD, true, true, buf);
    }
    else
    {
        // Offline mode — create locally with a fake ID
        static int offlineId = 10000;
        HandleBuildingSpawn(offlineId++, currentBuildTypeId_,
                           ghostPosition_, ghostRotation_, 100);
    }
}

void BuildingSystem::RequestDemolish(Connection* serverConn, int placedBuildingId)
{
    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(placedBuildingId);
        serverConn->SendMessage(MSG_DEMOLISH, true, true, buf);
    }
    else
    {
        HandleBuildingRemove(placedBuildingId);
    }
}

void BuildingSystem::HandleBuildingSpawn(int placedId, int typeId,
                                          const Vector3& pos, float rotation, int hp)
{
    auto* scene = GetScene();
    if (!scene)
        return;

    const BuildingTypeInfo* info = FindTypeInfo(typeId);

    auto* cache = GetSubsystem<ResourceCache>();
    Node* node = scene->CreateChild(info ? info->name : "Building", LOCAL);
    node->SetPosition(pos);
    node->SetRotation(Quaternion(rotation, Vector3::UP));
    node->SetVar("PlacedBuildingId", placedId);
    node->SetVar("BuildingTypeId", typeId);

    // Try to load model — use a box placeholder if model doesn't exist
    auto* model = node->CreateComponent<StaticModel>(LOCAL);
    Model* mdl = nullptr;
    if (info && info->modelPath.Length() > 0)
        mdl = cache->GetResource<Model>(info->modelPath, false);

    if (mdl)
    {
        model->SetModel(mdl);
    }
    else
    {
        // Placeholder box scaled to footprint
        auto* boxMdl = cache->GetResource<Model>("Models/Box.mdl");
        if (boxMdl)
        {
            model->SetModel(boxMdl);
            float sx = info ? info->footprintX : 2.0f;
            float sy = info ? info->height : 2.5f;
            float sz = info ? info->footprintZ : 2.0f;
            node->SetScale(Vector3(sx, sy, sz));
            node->SetPosition(pos + Vector3(0, sy * 0.5f, 0));
        }
    }

    PlacedBuilding pb;
    pb.placedId = placedId;
    pb.buildingTypeId = typeId;
    pb.position = pos;
    pb.rotation = rotation;
    pb.hp = hp;
    pb.maxHp = info ? info->maxHp : 100;
    pb.name = info ? info->name : "Building";
    pb.snapType = info ? info->snapType : "free";
    pb.footprintX = info ? info->footprintX : 2.0f;
    pb.gateOpen = false;
    pb.node = node;
    // Fire system Phase 2b: Woodpile reuses storageSlots field as per-wood-type capacity.
    // Other building types ignore — counters stay 0, hover hint won't fire.
    static constexpr int BUILDING_TYPE_WOODPILE = 56;  // matches buildings_seed.sql
    if (info && typeId == BUILDING_TYPE_WOODPILE)
        pb.woodCapacity = info->storageSlots;
    // Fire system Phase 2a: Stone Ring starts UNLIT (field default, explicit for clarity).
    if (typeId == BUILDING_TYPE_STONE_RING)
        pb.fireState = FIRE_UNLIT;
    placedBuildings_.Push(pb);

    URHO3D_LOGINFOF("Building spawned: %s (id=%d) at %.1f,%.1f,%.1f",
                    pb.name.CString(), placedId, pos.x_, pos.y_, pos.z_);
}

void BuildingSystem::HandleBuildingRemove(int placedId)
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == placedId)
        {
            if (placedBuildings_[i].node)
                placedBuildings_[i].node->Remove();
            placedBuildings_.Erase(i);
            URHO3D_LOGINFOF("Building removed: id=%d", placedId);
            return;
        }
    }
}

int BuildingSystem::FindNearestBuilding(const Vector3& pos, float maxDist) const
{
    int bestId = -1;
    float bestDist2 = maxDist * maxDist;
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        float dist2 = (placedBuildings_[i].position - pos).LengthSquared();
        if (dist2 < bestDist2)
        {
            bestDist2 = dist2;
            bestId = placedBuildings_[i].placedId;
        }
    }
    return bestId;
}

bool BuildingSystem::IsGate(int placedBuildingId) const
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == placedBuildingId)
            return placedBuildings_[i].snapType == "gate";
    }
    return false;
}

bool BuildingSystem::IsGateOpen(int placedBuildingId) const
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == placedBuildingId)
            return placedBuildings_[i].gateOpen;
    }
    return false;
}

void BuildingSystem::RequestGateToggle(Connection* serverConn, int placedBuildingId)
{
    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(placedBuildingId);
        serverConn->SendMessage(MSG_GATE_TOGGLE, true, true, buf);
    }
    else
    {
        // Offline: toggle immediately
        for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
        {
            if (placedBuildings_[i].placedId == placedBuildingId)
            {
                HandleGateState(placedBuildingId, !placedBuildings_[i].gateOpen);
                return;
            }
        }
    }
}

void BuildingSystem::HandleGateState(int placedBuildingId, bool open)
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        PlacedBuilding& pb = placedBuildings_[i];
        if (pb.placedId == placedBuildingId)
        {
            pb.gateOpen = open;
            if (pb.node)
            {
                float rot = pb.rotation + (open ? 90.0f : 0.0f);
                pb.node->SetRotation(Quaternion(rot, Vector3::UP));
            }
            URHO3D_LOGINFOF("Gate %s: id=%d", open ? "opened" : "closed", placedBuildingId);
            return;
        }
    }
}

void BuildingSystem::CreateGhostNode(int buildingTypeId)
{
    DestroyGhostNode();

    // Reset cached ghost materials for the new ghost model
    ghostMatValid_.Reset();
    ghostMatSnapped_.Reset();
    ghostMatInvalid_.Reset();
    ghostColorState_ = -1;

    auto* scene = GetScene();
    if (!scene)
        return;

    const BuildingTypeInfo* info = FindTypeInfo(buildingTypeId);

    auto* cache = GetSubsystem<ResourceCache>();
    Node* node = scene->CreateChild("BuildGhost", LOCAL);

    auto* model = node->CreateComponent<StaticModel>(LOCAL);

    // Try ghost model, then regular model, then placeholder box
    Model* mdl = nullptr;
    if (info && info->ghostModelPath.Length() > 0)
        mdl = cache->GetResource<Model>(info->ghostModelPath, false);
    if (!mdl && info && info->modelPath.Length() > 0)
        mdl = cache->GetResource<Model>(info->modelPath, false);

    if (mdl)
    {
        model->SetModel(mdl);
    }
    else
    {
        auto* boxMdl = cache->GetResource<Model>("Models/Box.mdl");
        if (boxMdl)
        {
            model->SetModel(boxMdl);
            float sx = info ? info->footprintX : 2.0f;
            float sy = info ? info->height : 2.5f;
            float sz = info ? info->footprintZ : 2.0f;
            node->SetScale(Vector3(sx, sy, sz));
        }
    }

    ghostNode_ = node;
    ghostRotation_ = 0.0f;
}

void BuildingSystem::DestroyGhostNode()
{
    if (ghostNode_)
    {
        ghostNode_->Remove();
        ghostNode_ = nullptr;
    }
}

void BuildingSystem::HandleBuildingHpUpdate(int placedBuildingId, int newHp)
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == placedBuildingId)
        {
            placedBuildings_[i].hp = newHp;
            return;
        }
    }
}

void BuildingSystem::RequestRepair(Connection* serverConn, int placedBuildingId)
{
    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(placedBuildingId);
        serverConn->SendMessage(MSG_REPAIR, true, true, buf);
    }
}

void BuildingSystem::RequestSleep(Connection* serverConn, int placedBuildingId)
{
    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(placedBuildingId);
        serverConn->SendMessage(MSG_SLEEP, true, true, buf);
    }
}

void BuildingSystem::RequestSetRespawn(Connection* serverConn, int placedBuildingId)
{
    if (serverConn)
    {
        VectorBuffer buf;
        buf.WriteI32(placedBuildingId);
        serverConn->SendMessage(MSG_SET_RESPAWN, true, true, buf);
    }
}

float BuildingSystem::GetShelterWarmth(const Vector3& pos) const
{
    float bestWarmth = 0.0f;

    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        const PlacedBuilding& pb = placedBuildings_[i];
        // Find the type info for warmth
        const BuildingTypeInfo* info = FindTypeInfo(pb.buildingTypeId);
        if (!info || info->warmth <= 0.0f)
            continue;

        // Fire System Phase 2a: Stone Ring warmth is state-derived, not static.
        // UNLIT/COLD → 0 (cold stones don't warm you), EMBERS → 4 (low glow),
        // LIT → info->warmth (full fire, 15 per buildings_seed.sql). Non-pit
        // buildings (shelters, huts, etc.) keep using their static info->warmth.
        float effectiveWarmth = info->warmth;
        if (pb.buildingTypeId == BUILDING_TYPE_STONE_RING)
        {
            switch (pb.fireState)
            {
            case FIRE_LIT:    effectiveWarmth = info->warmth; break;
            case FIRE_EMBERS: effectiveWarmth = 4.0f; break;
            case FIRE_UNLIT:
            case FIRE_COLD:
            default:          effectiveWarmth = 0.0f; break;
            }
            if (effectiveWarmth <= 0.0f)
                continue;  // No heat — skip distance check
        }

        float dx = pos.x_ - pb.position.x_;
        float dz = pos.z_ - pb.position.z_;
        float dist2 = dx * dx + dz * dz;

        // Range: footprint for shelters, 5m for utility (fire pits)
        float range = Max(info->footprintX, info->footprintZ) * 0.75f;
        if (info->category == "utility")
            range = 5.0f;

        if (dist2 <= range * range && effectiveWarmth > bestWarmth)
            bestWarmth = effectiveWarmth;
    }

    return bestWarmth;
}

FireState BuildingSystem::GetFirePitState(int placedBuildingId) const
{
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        if (placedBuildings_[i].placedId == placedBuildingId)
            return placedBuildings_[i].fireState;
    }
    return FIRE_UNLIT;  // Unknown id — safe default
}

void BuildingSystem::SetFirePitState(int placedBuildingId, FireState state)
{
    PlacedBuilding* pb = FindPlacedMutable(placedBuildingId);
    if (!pb)
        return;
    // Guard — state field is only meaningful for Stone Rings. Silently ignore
    // attempts to set state on other building types so callers can't corrupt
    // unrelated placements.
    if (pb->buildingTypeId != BUILDING_TYPE_STONE_RING)
        return;
    pb->fireState = state;
}

bool BuildingSystem::HasNearbyWorkshop(const Vector3& pos, float range) const
{
    float range2 = range * range;
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        const PlacedBuilding& pb = placedBuildings_[i];
        float dx = pos.x_ - pb.position.x_;
        float dz = pos.z_ - pb.position.z_;
        if (dx * dx + dz * dz > range2)
            continue;  // Distance cull before type lookup
        const BuildingTypeInfo* info = FindTypeInfo(pb.buildingTypeId);
        if (info && info->category == "workshop")
            return true;
    }
    return false;
}

bool BuildingSystem::HasNearbyWatchtower(const Vector3& pos, float range) const
{
    float range2 = range * range;
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        const PlacedBuilding& pb = placedBuildings_[i];
        float dx = pos.x_ - pb.position.x_;
        float dz = pos.z_ - pb.position.z_;
        if (dx * dx + dz * dz > range2)
            continue;  // Distance cull before type lookup
        const BuildingTypeInfo* info = FindTypeInfo(pb.buildingTypeId);
        if (info && info->name == "Watchtower")
            return true;
    }
    return false;
}

void BuildingSystem::UpdateGhostColor()
{
    if (!ghostNode_)
        return;

    auto* model = ghostNode_->GetComponent<StaticModel>();
    if (!model)
        return;

    // Determine desired state: 0=valid, 1=snapped, 2=invalid
    int desiredState = !ghostValid_ ? 2 : (snappedToId_ >= 0 ? 1 : 0);
    if (desiredState == ghostColorState_)
        return;  // No change — skip material work entirely
    ghostColorState_ = desiredState;

    Color ghostColor;
    SharedPtr<Material>* cached;
    if (desiredState == 2)
    {
        ghostColor = Color(0.8f, 0.2f, 0.2f, 0.4f);
        cached = &ghostMatInvalid_;
    }
    else if (desiredState == 1)
    {
        ghostColor = Color(0.2f, 0.6f, 1.0f, 0.5f);
        cached = &ghostMatSnapped_;
    }
    else
    {
        ghostColor = Color(0.2f, 0.8f, 0.2f, 0.4f);
        cached = &ghostMatValid_;
    }

    // Clone once per state, reuse thereafter
    if (!*cached)
    {
        auto* baseMat = model->GetMaterial(0);
        if (baseMat)
        {
            *cached = baseMat->Clone();
            (*cached)->SetShaderParameter("MatDiffColor", Variant(ghostColor));
        }
    }

    if (*cached)
    {
        for (unsigned i = 0; i < model->GetNumGeometries(); ++i)
            model->SetMaterial(i, *cached);
    }
}
