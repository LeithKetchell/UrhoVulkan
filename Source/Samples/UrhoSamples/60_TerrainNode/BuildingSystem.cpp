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

    // Intersect ray with terrain — walk the ray in steps
    Vector3 hitPos;
    bool hit = false;
    for (float t = 1.0f; t < 200.0f; t += 0.5f)
    {
        Vector3 p = ray.origin_ + ray.direction_ * t;
        float terrainY = terrain->GetHeight(p);
        if (p.y_ <= terrainY)
        {
            hitPos = Vector3(p.x_, terrainY, p.z_);
            hit = true;
            break;
        }
    }

    if (!hit)
        return;

    ghostPosition_ = hitPos;
    ghostNode_->SetPosition(hitPos);
    ghostNode_->SetRotation(Quaternion(ghostRotation_, Vector3::UP));

    ghostValid_ = ValidatePlacement(hitPos, currentBuildTypeId_, terrain, waterLevel);
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
    // Find the building type info
    const BuildingTypeInfo* info = nullptr;
    for (unsigned i = 0; i < buildingTypes_.Size(); ++i)
    {
        if (buildingTypes_[i].id == buildingTypeId)
        {
            info = &buildingTypes_[i];
            break;
        }
    }
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
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        float dist = (placedBuildings_[i].position - pos).Length();
        float minDist = (info->footprintX + info->footprintZ) * 0.5f;
        if (dist < minDist)
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

    // Find building type info
    const BuildingTypeInfo* info = nullptr;
    for (unsigned i = 0; i < buildingTypes_.Size(); ++i)
    {
        if (buildingTypes_[i].id == typeId)
        {
            info = &buildingTypes_[i];
            break;
        }
    }

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
    pb.node = node;
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
    float bestDist = maxDist;
    for (unsigned i = 0; i < placedBuildings_.Size(); ++i)
    {
        float dist = (placedBuildings_[i].position - pos).Length();
        if (dist < bestDist)
        {
            bestDist = dist;
            bestId = placedBuildings_[i].placedId;
        }
    }
    return bestId;
}

void BuildingSystem::CreateGhostNode(int buildingTypeId)
{
    DestroyGhostNode();

    auto* scene = GetScene();
    if (!scene)
        return;

    const BuildingTypeInfo* info = nullptr;
    for (unsigned i = 0; i < buildingTypes_.Size(); ++i)
    {
        if (buildingTypes_[i].id == buildingTypeId)
        {
            info = &buildingTypes_[i];
            break;
        }
    }

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

void BuildingSystem::UpdateGhostColor()
{
    if (!ghostNode_)
        return;

    auto* model = ghostNode_->GetComponent<StaticModel>();
    if (!model)
        return;

    auto* cache = GetSubsystem<ResourceCache>();

    // Use a translucent green or red material based on validity
    // We'll clone the existing material and tint it
    for (unsigned i = 0; i < model->GetNumGeometries(); ++i)
    {
        auto* mat = model->GetMaterial(i);
        if (mat)
        {
            SharedPtr<Material> cloned = mat->Clone();
            if (ghostValid_)
                cloned->SetShaderParameter("MatDiffColor", Variant(Color(0.2f, 0.8f, 0.2f, 0.4f)));
            else
                cloned->SetShaderParameter("MatDiffColor", Variant(Color(0.8f, 0.2f, 0.2f, 0.4f)));
            model->SetMaterial(i, cloned);
        }
    }
}
