// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include <Urho3D/Scene/Component.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Network/Connection.h>

using namespace Urho3D;

/// Info about a building type from the DB.
struct BuildingTypeInfo
{
    int id{};
    String name;
    String category;
    int tier{1};
    float footprintX{2.0f};
    float footprintZ{2.0f};
    float height{2.5f};
    int maxHp{100};
    float decayRate{1.0f};
    float warmth{0.0f};
    int storageSlots{0};
    int sleepCapacity{0};
    bool respawn{false};
    String snapType;
    String modelPath;
    String ghostModelPath;
    String description;
};

/// Info about a placed building instance.
struct PlacedBuilding
{
    int placedId{};
    int buildingTypeId{};
    Vector3 position;
    float rotation{};
    int hp{};
    int maxHp{};
    String name;
    WeakPtr<Node> node;
};

/// Client-side building placement system.
/// Handles build mode, ghost preview, validation, and server communication.
class BuildingSystem : public Component
{
    URHO3D_OBJECT(BuildingSystem, Component);

public:
    explicit BuildingSystem(Context* context);

    /// Enter/exit build mode.
    void SetBuildMode(bool enabled, int buildingTypeId = 0);
    bool IsBuildMode() const { return buildMode_; }

    /// Per-frame: update ghost position from mouse raycast.
    void UpdateGhostPreview(Camera* camera, Terrain* terrain, float waterLevel);

    /// Rotate ghost by 45 degrees.
    void RotateGhost();

    /// Validate placement at current ghost position.
    bool ValidatePlacement(const Vector3& pos, int buildingTypeId,
                           Terrain* terrain, float waterLevel) const;

    /// Request server to place building at ghost position.
    void RequestBuild(Connection* serverConn);

    /// Request server to demolish a building.
    void RequestDemolish(Connection* serverConn, int placedBuildingId);

    /// Handle server building spawn — creates client-side node.
    void HandleBuildingSpawn(int placedId, int typeId,
                             const Vector3& pos, float rotation, int hp);

    /// Handle server building removal.
    void HandleBuildingRemove(int placedId);

    /// Set available building types (loaded from GameDB).
    void SetBuildingTypes(const Vector<BuildingTypeInfo>& types) { buildingTypes_ = types; }
    const Vector<BuildingTypeInfo>& GetBuildingTypes() const { return buildingTypes_; }

    /// Get placed buildings for interaction queries.
    const Vector<PlacedBuilding>& GetPlacedBuildings() const { return placedBuildings_; }

    /// Find nearest placed building within range of a world position.
    int FindNearestBuilding(const Vector3& pos, float maxDist) const;

    /// Get current ghost validity.
    bool IsGhostValid() const { return ghostValid_; }

    /// Get current selected building type ID.
    int GetCurrentBuildTypeId() const { return currentBuildTypeId_; }

private:
    void CreateGhostNode(int buildingTypeId);
    void DestroyGhostNode();
    void UpdateGhostColor();

    bool buildMode_{false};
    int currentBuildTypeId_{0};
    WeakPtr<Node> ghostNode_;
    Vector3 ghostPosition_;
    float ghostRotation_{0.0f};
    bool ghostValid_{false};

    Vector<BuildingTypeInfo> buildingTypes_;
    Vector<PlacedBuilding> placedBuildings_;
};
