// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "ResourceMap.h"
#include "EcosystemManager.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Math/MathDefs.h>
#include <Urho3D/Graphics/Terrain.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Resource/Image.h>
#include <Urho3D/Resource/ResourceCache.h>

#include <Urho3D/DebugNew.h>

ResourceMap::ResourceMap(Context* context)
    : LogicComponent(context)
{
}

void ResourceMap::RegisterObject(Context* context)
{
    context->RegisterFactory<ResourceMap>();
}

IntVector2 ResourceMap::WorldToPixel(float worldX, float worldZ) const
{
    int px = (int)(((worldX - terrainOriginX_) / terrainSizeX_) * (float)MAP_SIZE);
    int pz = (int)(((worldZ - terrainOriginZ_) / terrainSizeZ_) * (float)MAP_SIZE);
    px = Clamp(px, 0, MAP_SIZE - 1);
    pz = Clamp(pz, 0, MAP_SIZE - 1);
    return IntVector2(px, pz);
}

Vector2 ResourceMap::PixelToWorld(int px, int pz) const
{
    float x = terrainOriginX_ + ((float)px + 0.5f) / (float)MAP_SIZE * terrainSizeX_;
    float z = terrainOriginZ_ + ((float)pz + 0.5f) / (float)MAP_SIZE * terrainSizeZ_;
    return Vector2(x, z);
}

void ResourceMap::Generate(Terrain* terrain, EcosystemManager* eco, float waterLevel)
{
    if (!terrain)
    {
        URHO3D_LOGERROR("ResourceMap::Generate — no terrain");
        return;
    }

    // Compute terrain world bounds
    Vector3 spacing = terrain->GetSpacing();
    IntVector2 numVerts = terrain->GetNumVertices();
    terrainSizeX_ = (float)(numVerts.x_ - 1) * spacing.x_;
    terrainSizeZ_ = (float)(numVerts.y_ - 1) * spacing.z_;
    terrainOriginX_ = -terrainSizeX_ * 0.5f;
    terrainOriginZ_ = -terrainSizeZ_ * 0.5f;

    // Create the resource image
    resourceImage_ = new Image(context_);
    resourceImage_->SetSize(MAP_SIZE, MAP_SIZE, 4);
    resourceImage_->Clear(Color::BLACK);

    // Seeded RNG for deterministic scatter
    unsigned seed = 42u;
    auto nextRand = [&seed]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed & 0xFFFF) / 65536.f;
    };

    resourceCount_ = 0;

    for (int pz = 0; pz < MAP_SIZE; ++pz)
    {
        for (int px = 0; px < MAP_SIZE; ++px)
        {
            Vector2 world = PixelToWorld(px, pz);
            float height = terrain->GetHeight(Vector3(world.x_, 0.f, world.y_));
            float distToWater = height - waterLevel;

            // Skip underwater pixels
            if (height < waterLevel)
                continue;

            // Slope estimate from terrain normal
            Vector3 normal = terrain->GetNormal(Vector3(world.x_, 0.f, world.y_));
            float slope = 1.0f - normal.y_;  // 0 = flat, 1 = vertical

            // Ecosystem data (if available)
            float grassDensity = 0.f;
            float shrubDensity = 0.f;
            unsigned char treeMaturity = 0;

            if (eco)
            {
                grassDensity  = eco->SampleGrassDensity(world.x_, world.y_);
                shrubDensity  = eco->SampleShrubDensity(world.x_, world.y_);
                treeMaturity  = eco->SampleTreeMaturity(world.x_, world.y_);
            }

            // Score each resource type — pick dominant, add randomness
            // Most pixels stay RES_NONE. Resources are sparse.
            float roll = nextRand();
            ResourceType chosen = RES_NONE;
            unsigned char qty = 0;
            unsigned char variant = 0;
            unsigned char flags = 0;

            // Stone: high altitude + steep slope (mountain scree) — original concentration
            if (height > 15.f && slope > 0.15f && roll < 0.03f)
            {
                chosen = RES_STONE;
                qty = 1 + (unsigned char)(nextRand() * 2.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Stone: plentiful uniform spawn anywhere above waterline (Phase 1b — Fire system).
            // Same item as mountain scree (Rough Stone, item 1). Fire-pit construction needs
            // ~5 stones per pit; this keeps gathering quick instead of forcing a mountain hike.
            else if (slope < 0.5f && roll < 0.018f)
            {
                chosen = RES_STONE;
                qty = 1 + (unsigned char)(nextRand() * 2.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Flint: mountain biome, altitude > 15m, rarer than stone
            else if (height > 15.f && slope > 0.1f && roll < 0.01f)
            {
                chosen = RES_FLINT;
                qty = 1 + (unsigned char)(nextRand() * 1.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Log: near large trees (high maturity)
            else if (treeMaturity > 200 && roll < 0.02f)
            {
                chosen = RES_LOG;
                qty = 1;
                flags = RFLAG_RESPAWNABLE;
            }
            // Stick: under trees (moderate maturity)
            else if (treeMaturity > 80 && roll < 0.05f)
            {
                chosen = RES_STICK;
                qty = 1 + (unsigned char)(nextRand() * 2.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Berries: forest with shrub density
            else if (shrubDensity > 0.3f && treeMaturity > 40 && roll < 0.02f)
            {
                chosen = RES_BERRIES;
                qty = 2 + (unsigned char)(nextRand() * 3.f);
                flags = RFLAG_RESPAWNABLE | RFLAG_SEASONAL;
            }
            // Fiber: grassland/riverbank with grass density
            else if (grassDensity > 0.4f && distToWater < 10.f && roll < 0.03f)
            {
                chosen = RES_FIBER;
                qty = 1 + (unsigned char)(nextRand() * 3.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Clay: within 5m of waterline + low slope
            else if (distToWater > 0.f && distToWater < 5.f && slope < 0.1f && roll < 0.04f)
            {
                chosen = RES_CLAY;
                qty = 2 + (unsigned char)(nextRand() * 3.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Hardwood: bias toward old/large trees (high maturity); fewer per pixel.
            // Phase 1b — Fire system. "Trees are decorative spawn bias, not harvested entities."
            else if (treeMaturity > 200 && roll < 0.025f)
            {
                chosen = RES_HARDWOOD;
                qty = 1 + (unsigned char)(nextRand() * 1.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Softwood: bias toward any tree presence; more plentiful than hardwood.
            else if (treeMaturity > 60 && roll < 0.05f)
            {
                chosen = RES_SOFTWOOD;
                qty = 1 + (unsigned char)(nextRand() * 2.f);
                flags = RFLAG_RESPAWNABLE;
            }
            // Softwood (uniform fallback): any above-waterline pixel, low rate.
            // Plan: "Uniform distribution above waterline."
            else if (slope < 0.4f && roll < 0.012f)
            {
                chosen = RES_SOFTWOOD;
                qty = 1 + (unsigned char)(nextRand() * 1.f);
                flags = RFLAG_RESPAWNABLE;
            }

            if (chosen != RES_NONE)
            {
                variant = (unsigned char)(nextRand() * 255.f);
                resourceImage_->SetPixelInt(px, pz, (unsigned)chosen | ((unsigned)qty << 8) |
                    ((unsigned)variant << 16) | ((unsigned)flags << 24));
                ++resourceCount_;
            }
        }
    }

    URHO3D_LOGINFOF("ResourceMap: generated %u resource pixels over %.0fx%.0f terrain",
                    resourceCount_, terrainSizeX_, terrainSizeZ_);
}

bool ResourceMap::LoadMap(const String& path)
{
    auto* cache = GetSubsystem<ResourceCache>();
    auto* img = cache->GetResource<Image>(path);
    if (!img)
    {
        URHO3D_LOGERROR("ResourceMap: failed to load " + path);
        return false;
    }

    if (img->GetWidth() != MAP_SIZE || img->GetHeight() != MAP_SIZE)
    {
        URHO3D_LOGERROR("ResourceMap: image size mismatch (expected " +
                        String(MAP_SIZE) + "x" + String(MAP_SIZE) + ")");
        return false;
    }

    // Make a deep copy so we own the pixel data
    resourceImage_ = new Image(context_);
    resourceImage_->SetSize(img->GetWidth(), img->GetHeight(), img->GetComponents());
    memcpy(resourceImage_->GetData(), img->GetData(),
           (size_t)img->GetWidth() * img->GetHeight() * img->GetComponents());

    // Count non-empty pixels
    resourceCount_ = 0;
    for (int pz = 0; pz < MAP_SIZE; ++pz)
    {
        for (int px = 0; px < MAP_SIZE; ++px)
        {
            unsigned pixel = resourceImage_->GetPixelInt(px, pz);
            if ((pixel & 0xFF) != RES_NONE)
                ++resourceCount_;
        }
    }

    URHO3D_LOGINFOF("ResourceMap: loaded %u resources from %s", resourceCount_, path.CString());
    return true;
}

bool ResourceMap::LoadMapFromFile(const String& absolutePath)
{
    File file(context_, absolutePath, FILE_READ);
    if (!file.IsOpen())
    {
        URHO3D_LOGERROR("ResourceMap: failed to open " + absolutePath);
        return false;
    }

    SharedPtr<Image> img(new Image(context_));
    if (!img->Load(file))
    {
        URHO3D_LOGERROR("ResourceMap: failed to decode " + absolutePath);
        return false;
    }

    if (img->GetWidth() != MAP_SIZE || img->GetHeight() != MAP_SIZE)
    {
        URHO3D_LOGERROR("ResourceMap: image size mismatch (expected " +
                        String(MAP_SIZE) + "x" + String(MAP_SIZE) + ")");
        return false;
    }

    // Take ownership
    resourceImage_ = img;

    // Count non-empty pixels
    resourceCount_ = 0;
    for (int pz = 0; pz < MAP_SIZE; ++pz)
    {
        for (int px = 0; px < MAP_SIZE; ++px)
        {
            unsigned pixel = resourceImage_->GetPixelInt(px, pz);
            if ((pixel & 0xFF) != RES_NONE)
                ++resourceCount_;
        }
    }

    URHO3D_LOGINFOF("ResourceMap: loaded %u resources from %s", resourceCount_, absolutePath.CString());
    return true;
}

void ResourceMap::SetTerrainBounds(Terrain* terrain)
{
    if (!terrain)
        return;
    Vector3 spacing = terrain->GetSpacing();
    IntVector2 numVerts = terrain->GetNumVertices();
    terrainSizeX_ = (float)(numVerts.x_ - 1) * spacing.x_;
    terrainSizeZ_ = (float)(numVerts.y_ - 1) * spacing.z_;
    terrainOriginX_ = -terrainSizeX_ * 0.5f;
    terrainOriginZ_ = -terrainSizeZ_ * 0.5f;
}

bool ResourceMap::SaveMap(const String& path) const
{
    if (!resourceImage_)
        return false;

    return resourceImage_->SavePNG(path);
}

ResourceType ResourceMap::Sample(float worldX, float worldZ,
                                  unsigned char& outQty,
                                  unsigned char& outVariant) const
{
    outQty = 0;
    outVariant = 0;

    if (!resourceImage_)
        return RES_NONE;

    IntVector2 p = WorldToPixel(worldX, worldZ);
    unsigned pixel = resourceImage_->GetPixelInt(p.x_, p.y_);

    ResourceType type = (ResourceType)(pixel & 0xFF);
    outQty = (unsigned char)((pixel >> 8) & 0xFF);
    outVariant = (unsigned char)((pixel >> 16) & 0xFF);
    unsigned char flags = (unsigned char)((pixel >> 24) & 0xFF);

    // If depleted, report as empty
    if (flags & RFLAG_DEPLETED)
        return RES_NONE;

    return type;
}

int ResourceMap::Harvest(float worldX, float worldZ, int amount)
{
    if (!resourceImage_ || amount <= 0)
        return 0;

    IntVector2 p = WorldToPixel(worldX, worldZ);
    unsigned pixel = resourceImage_->GetPixelInt(p.x_, p.y_);

    ResourceType type = (ResourceType)(pixel & 0xFF);
    if (type == RES_NONE)
        return 0;

    unsigned char qty = (unsigned char)((pixel >> 8) & 0xFF);
    unsigned char variant = (unsigned char)((pixel >> 16) & 0xFF);
    unsigned char flags = (unsigned char)((pixel >> 24) & 0xFF);

    if (flags & RFLAG_DEPLETED)
        return 0;

    int taken = Min(amount, (int)qty);
    qty -= (unsigned char)taken;

    if (qty == 0 && (flags & RFLAG_RESPAWNABLE))
        flags |= RFLAG_DEPLETED;

    if (qty == 0 && !(flags & RFLAG_RESPAWNABLE))
    {
        // Permanent depletion — clear the pixel
        resourceImage_->SetPixelInt(p.x_, p.y_, 0);
        --resourceCount_;
    }
    else
    {
        resourceImage_->SetPixelInt(p.x_, p.y_,
            (unsigned)type | ((unsigned)qty << 8) |
            ((unsigned)variant << 16) | ((unsigned)flags << 24));
    }

    return taken;
}

bool ResourceMap::FindNearest(float worldX, float worldZ, float maxRadius,
                               ResourceType typeFilter,
                               float& outX, float& outZ) const
{
    if (!resourceImage_)
        return false;

    // Convert radius to pixel search bounds
    float pixelsPerMeter = (float)MAP_SIZE / terrainSizeX_;
    int searchPixels = (int)(maxRadius * pixelsPerMeter);

    IntVector2 center = WorldToPixel(worldX, worldZ);

    float bestDistSq = maxRadius * maxRadius;
    bool found = false;

    int minPx = Max(0, center.x_ - searchPixels);
    int maxPx = Min(MAP_SIZE - 1, center.x_ + searchPixels);
    int minPz = Max(0, center.y_ - searchPixels);
    int maxPz = Min(MAP_SIZE - 1, center.y_ + searchPixels);

    for (int pz = minPz; pz <= maxPz; ++pz)
    {
        for (int px = minPx; px <= maxPx; ++px)
        {
            unsigned pixel = resourceImage_->GetPixelInt(px, pz);
            ResourceType type = (ResourceType)(pixel & 0xFF);
            if (type != typeFilter)
                continue;

            unsigned char flags = (unsigned char)((pixel >> 24) & 0xFF);
            if (flags & (RFLAG_DEPLETED | RFLAG_RESERVED))
                continue;

            unsigned char qty = (unsigned char)((pixel >> 8) & 0xFF);
            if (qty == 0)
                continue;

            Vector2 w = PixelToWorld(px, pz);
            float dx = w.x_ - worldX;
            float dz = w.y_ - worldZ;
            float distSq = dx * dx + dz * dz;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                outX = w.x_;
                outZ = w.y_;
                found = true;
            }
        }
    }

    return found;
}

void ResourceMap::TickRespawn(Vector<RespawnEvent>* outRespawned)
{
    if (!resourceImage_)
        return;

    // Per-type respawn chance per tick (called every 60s).
    // Higher = faster respawn. 1.0 = every tick, 0.01 = ~once per 100 ticks (~100 min).
    auto respawnChance = [](ResourceType type) -> float {
        switch (type)
        {
        case RES_BERRIES:  return 0.15f;  // fast — organic regrowth
        case RES_FIBER:    return 0.12f;  // moderate — grass regrows
        case RES_CLAY:     return 0.08f;  // slower — needs water cycle
        case RES_STICK:    return 0.10f;  // moderate — fallen branches
        case RES_LOG:      return 0.02f;  // very slow — trees take time
        case RES_STONE:    return 0.03f;  // geological — slow
        case RES_FLINT:    return 0.01f;  // rare — slowest
        case RES_SOFTWOOD: return 0.12f;  // fast — smaller branches fall often (Phase 1b)
        case RES_HARDWOOD: return 0.04f;  // slow — mature wood takes time (Phase 1b)
        default:           return 0.05f;
        }
    };

    // Per-type respawn quantity
    auto respawnQty = [](ResourceType type) -> unsigned char {
        switch (type)
        {
        case RES_BERRIES:  return 3;
        case RES_FIBER:    return 2;
        case RES_CLAY:     return 2;
        case RES_SOFTWOOD: return 2;
        default:           return 1;
        }
    };

    unsigned respawned = 0;
    for (int pz = 0; pz < MAP_SIZE; ++pz)
    {
        for (int px = 0; px < MAP_SIZE; ++px)
        {
            unsigned pixel = resourceImage_->GetPixelInt(px, pz);
            unsigned char flags = (unsigned char)((pixel >> 24) & 0xFF);
            if (!(flags & RFLAG_DEPLETED) || !(flags & RFLAG_RESPAWNABLE))
                continue;

            ResourceType type = (ResourceType)(pixel & 0xFF);

            // Probabilistic respawn — not every depleted pixel respawns each tick
            if (Random() > respawnChance(type))
                continue;

            unsigned char variant = (unsigned char)((pixel >> 16) & 0xFF);
            unsigned char qty = respawnQty(type);
            flags &= ~RFLAG_DEPLETED;

            resourceImage_->SetPixelInt(px, pz,
                (unsigned)type | ((unsigned)qty << 8) |
                ((unsigned)variant << 16) | ((unsigned)flags << 24));
            ++respawned;

            // Collect for broadcast
            if (outRespawned)
            {
                RespawnEvent ev;
                ev.worldX = PixelToWorld(px, pz).x_;
                ev.worldZ = PixelToWorld(px, pz).y_;
                ev.qty = qty;
                ev.type = (unsigned char)type;
                outRespawned->Push(ev);
            }
        }
    }

    if (respawned > 0)
        URHO3D_LOGINFOF("ResourceMap: respawned %u resource pixels", respawned);
}
