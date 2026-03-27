# PLAN: Ecosystem — Texture-Based World Database

**Status:** CODER-READY (Phase 1 spec sharpened for cold handoff)
**Owner:** Planner
**Priority:** 19 — backbone connecting weather, grass, animals, resources, economy
**Hardware target:** Server-authoritative maps, client-side GPU sampling

---

## Problem

Weather generates rainfall. Grass needs density. Animals need food. Resources need spatial distribution. Trees need placement logic. Currently these are all disconnected: grass is procedural noise, animals wander randomly, resources don't exist yet. There's no world state that ties them together.

We need a single spatial database that every system reads from and some systems write to. The Metal Deposits plan proved the pattern: **one RGBA texture per terrain, pixel = grid point, channels encode properties, O(1) lookup, easy to generate, trivial to debug (open in GIMP).**

The ecosystem extends this to living systems: moisture, vegetation, soil quality, and food availability — all as texture layers the server owns and clients sample.

---

## Architecture: Layered Texture Maps

### The Stack

Four terrain-resolution textures form the ecosystem database. Each pixel maps 1:1 to a heightmap grid point. Server owns all of them. Clients receive periodic snapshots for rendering (grass density, tree placement).

```
Layer 4: RESOURCE MAP     ← Metal Deposits plan (already designed)
Layer 3: VEGETATION MAP   ← What grows here, how much, what state
Layer 2: SOIL MAP         ← Ground quality, nutrients, composition
Layer 1: MOISTURE MAP     ← Water availability (Weather System output)
Layer 0: HEIGHTMAP        ← Terrain shape (already exists)
```

Each layer feeds the one above. Moisture drives soil. Soil drives vegetation. Vegetation drives animal food, grass density, tree placement. Resources sit alongside but are geologically fixed (non-renewable).

### Layer 1: Moisture Map (Weather System Output)

Already designed in PLAN_WEATHER_SYSTEM.md §4. Summarized here for completeness.

| Channel | Meaning | Range | Update Rate |
|---------|---------|-------|-------------|
| R | Surface water | 0–255 | Per weather tick (~1/sec) |
| G | Soil moisture | 0–255 | Per weather tick |
| B | Snow depth | 0–255 | Per weather tick |
| A | Reserved (groundwater?) | — | — |

**Inputs:** Rainfall from cloud density sampling (weather system), terrain slope (drainage).
**Outputs:** Feeds soil fertility, vegetation growth rate, stream formation.

Surface water flows downhill (terrain normal). Soil moisture drains slowly. Snow melts in spring → surface water surge. This layer is the most dynamic — updates every weather tick.

### Layer 2: Soil Map

| Channel | Meaning | Range | Notes |
|---------|---------|-------|-------|
| R | Fertility | 0–255 | How well plants grow here. Degrades with farming (Economic Doctrine). |
| G | Composition | 0–255 | 0=sand (drains fast), 128=loam (ideal), 255=clay (retains water, slow drain) |
| B | Erosion state | 0–255 | 0=bedrock exposed, 255=deep topsoil. Decreases with water flow + deforestation. |
| A | Trampling | 0–255 | 0=pristine, 255=packed dirt (paths). Increases with foot traffic, slowly recovers. |

**Inputs:** Moisture (layer 1), player activity (farming, walking), deforestation.
**Outputs:** Vegetation growth rate, farming yield, erosion susceptibility.

**Key dynamics:**
- Fertility degrades with each harvest cycle: `fertility *= degradationFactor` (Economic Doctrine §3)
- Composition is fixed at world gen (geological, not player-modifiable)
- Erosion increases where surface water flows fast AND topsoil is exposed (no vegetation cover)
- Trampling creates visible paths — high foot traffic wears grass to dirt. Paths form organically where players walk.

**Update rate:** Slow — once per game-day or on player action. Soil doesn't change fast.

### Layer 3: Vegetation Map

| Channel | Meaning | Range | Notes |
|---------|---------|-------|-------|
| R | Grass density | 0–255 | **Direct input to GPU Grass system** density sampler |
| G | Shrub/bush density | 0–255 | Drives bush instance placement |
| B | Tree maturity | 0–255 | 0=none, 1–50=sapling, 51–200=growing, 201–255=mature |
| A | Biome type | 0–255 | Enum: 0=barren, 1=grassland, 2=forest, 3=wetland, 4=alpine, ... |

**Inputs:** Moisture (layer 1), soil fertility (layer 2), season, player actions (chopping, planting, grazing).
**Outputs:** Grass density (GPU Grass), tree placement (Imposter Trees), animal food availability, player harvestable resources.

**Key dynamics:**
- Grass grows where: `moisture.G > 30 AND soil.R > 20 AND soil.A < 200 (not trampled flat)`
- Growth rate: `baseRate * (moisture.G / 255) * (soil.R / 255) * seasonMultiplier`
- Spring multiplier: 1.5, Summer: 1.0, Autumn: 0.5, Winter: 0.0 (dormant)
- Tree maturity increments slowly (~1 unit per game-week). Chopping sets to 0 and increments soil erosion.
- Deforestation cascades: no trees → no root structure → erosion accelerates → soil fertility drops → grass thins → animals leave

**Degradation (Economic Doctrine):**
- Grazing animals reduce grass density (they eat it)
- Player harvesting reduces density
- Recovery rate: `regenRate * pow(degradationFactor, depletionCount)` — each cycle recovers less
- Pristine grassland recovers in days. Overgrazed land takes seasons. Strip-mined land may never fully recover.

### Layer 4: Resource Map (Metal Deposits)

Already fully designed in PLAN_METAL_DEPOSITS.md. R=quantity, G=metal type, B=purity, A=depth. Non-renewable. Server-authoritative. Folded in as the mineral layer of the ecosystem.

---

## How Systems Connect

```
                    WEATHER SYSTEM
                         │
                    cloud density
                         │
                    ▼ rainfall ▼
                   MOISTURE MAP (L1)
                    │         │
              soil moisture  surface water
                    │         │
                 SOIL MAP (L2)  → erosion
                    │
               fertility + composition
                    │
              VEGETATION MAP (L3)
               /    |    \      \
         grass   shrubs  trees   biome
           │       │       │
    GPU GRASS  INSTANCING  IMPOSTER TREES
           │       │       │
           └───────┴───────┘
                   │
            ANIMAL FOOD MAP
                   │
           ANIMAL POPULATIONS
                   │
            RESOURCE CHAIN
                   │
            ECONOMIC DOCTRINE
                   (entropy)
```

### Cross-System Interfaces

| Producer | Consumer | Interface |
|----------|----------|-----------|
| Weather → Moisture | Cloud density sampling | `rainfallMap_` texture, updated per weather tick |
| Moisture → Soil | Water availability | Server reads moisture.G, applies to soil fertility |
| Soil → Vegetation | Growth conditions | Server reads soil.R + soil.G, computes growth rate |
| Vegetation → GPU Grass | Density value | Client samples vegetation.R in grass vertex shader |
| Vegetation → Imposter Trees | Tree presence | Client reads vegetation.B > threshold → place tree |
| Vegetation → Animals | Food availability | Server samples vegetation.R at animal position |
| Animals → Vegetation | Grazing | Server decrements vegetation.R where animals feed |
| Player → Soil | Trampling, farming | Server increments soil.A on foot traffic, degrades soil.R on harvest |
| Player → Vegetation | Harvesting, chopping | Server decrements vegetation channels on action |
| Economic Doctrine → All | Degradation curve | `capacity = base * pow(degradationFactor, depletionCount)` |

---

## Animal Food System

Animals currently wander randomly. With the ecosystem, they follow food.

### Food Availability at Position

```
foodValue(pos) = vegetation.R(pos) * 0.6    // grass
               + vegetation.G(pos) * 0.3    // shrubs (berries, leaves)
               + nearWater(pos) * 0.1       // water access
```

Each species weights these differently:
- **Rabbit:** grass 0.8, shrubs 0.2, water 0.0 (gets moisture from food)
- **Deer:** grass 0.4, shrubs 0.5, water 0.1 (browsers, prefer shrubs)
- **Fox:** 0.0 — foxes don't eat vegetation, they eat rabbits. Fox food = rabbit density nearby.

### Animal Behavior Changes

Current Animal base class has `IDLE/WANDER/FLEE/DIE`. Add:
- **FORAGE** state: move toward highest food value within perception radius
- **MIGRATE** state: if local food drops below starvation threshold, pick a direction and travel far
- Wander radius shrinks in food-rich areas (why leave?), expands in food-poor areas

### Population Dynamics (Server-Side)

Server tracks per-species population per terrain patch:
```
birthRate = baseBirthRate * (avgFoodValue / foodThreshold) * seasonFactor
deathRate = baseDeathRate * (1.0 - avgFoodValue / foodThreshold) + predationRate + huntingRate
population += (birthRate - deathRate) * timeStep
```

- Spring: birthRate × 2.0 (breeding season)
- Winter: deathRate × 1.5 (cold, scarce food)
- Overpopulation: if population > carryingCapacity, birthRate drops to 0
- Carrying capacity = f(total food value across patch)

When population drops, fewer animal instances spawn. When it rises, more appear. Players see the ecosystem responding to their actions — overhunt rabbits, fewer rabbits spawn. Let the land recover, rabbits return (slowly, degradation curve applies).

---

## Procedural World Generation

At terrain creation, the ecosystem stack is seeded procedurally.

### Seed Order (bottom-up)

1. **Heightmap** — already exists (Perlin noise or hand-authored)
2. **Soil composition (L2.G)** — geological: sand near coast/rivers, clay in lowlands, loam in temperate zones. Derived from heightmap slope + altitude.
3. **Soil fertility (L2.R)** — starts at 200-255 everywhere (pristine). Reduced in rocky/steep terrain.
4. **Moisture (L1)** — initial state from first weather pass. Or seed from proximity to water table (altitude-based).
5. **Vegetation (L3)** — derived from moisture + soil:
   - Low altitude + high moisture + good soil → forest (B=200+, R=200+)
   - Mid altitude + moderate moisture → grassland (R=200+, B=0-50)
   - High altitude + low moisture → alpine (R=50, B=0, A=4)
   - Near water + flat → wetland (R=150, G=100, A=3)
   - Rocky/steep → barren (all low, A=0)
6. **Biome type (L3.A)** — classified from the vegetation channels after seeding
7. **Metal deposits (L4)** — independent geological layer (see Metal Deposits plan §5)

### Paintable Brushes (AuthServer / Editor)

Server admin or world editor can paint ecosystem layers manually:
- "Plant forest here" → set vegetation.B to 200 in a radius
- "Create meadow" → set vegetation.R to 255, B to 0
- "Drought zone" → set moisture.G to 20
- "Rich soil" → set soil.R to 255

This uses the same terrain brush UI pattern already working for heightmap editing. Texture target changes, brush logic stays the same.

---

## Seasonal Dynamics

The ecosystem breathes with the seasons. Melbourne time (already implemented) drives the cycle.

| Season | Moisture | Vegetation | Animals | Visual |
|--------|----------|------------|---------|--------|
| **Spring** | Snow melt → surface water surge, soil moisture peaks | Growth rate × 1.5, grass greens, saplings sprout | Breeding season, populations rise | Bright green, flowers (future) |
| **Summer** | Evaporation, soil moisture drops in exposed areas | Growth rate × 1.0, full density, mature growth | Peak population, active foraging | Deep green, full canopy |
| **Autumn** | Rainfall increases (skybox alpha), soil moisture stabilizes | Growth rate × 0.5, grass yellows, leaves drop (future) | Migration begins, less breeding | Yellow/brown tint, thinning |
| **Winter** | Snow accumulates, surface water freezes (future), soil moisture locked | Growth rate × 0.0, dormant, no new growth | Population pressure, starvation culls weak | Brown/gray, bare branches (future) |

### Seasonal Tint (Already Exists)

The day-of-year tint system already shifts grass color seasonally. The ecosystem plan doesn't replace this — it adds the underlying data that makes the tint meaningful. Brown grass in winter isn't just a color change; the density map actually drops.

---

## Client-Side Rendering Integration

### GPU Grass (PLAN_GPU_GRASS.md)

The grass vertex shader samples `vegetation.R` as its density input:
```glsl
uniform sampler2D sVegetationMap;  // Layer 3

float density = texture2D(sVegetationMap, terrainUV).r;
if (density < densityThreshold)
    // Collapse blade to degenerate triangle
```

Server updates the vegetation map. Client receives periodic snapshots (every few seconds or on significant change). Grass density responds to seasons, grazing, player activity — all through one texture sample.

### Imposter Trees (PLAN_IMPOSTER_TREES.md)

Tree placement reads `vegetation.B`:
```
For each grid point on terrain:
    if vegetation.B(x,z) > SAPLING_THRESHOLD:
        place tree instance
        LOD stage = f(vegetation.B value, distance to camera)
```

Small B values = saplings (simple model). Large B values = mature trees (full mesh or imposter). Trees don't just appear/disappear — they grow through the maturity channel.

### Bush/Shrub Instances

Same pattern as trees but using `vegetation.G`. Below a threshold = nothing. Above = place bush instance. Could use StaticModelGroup or GPU instancing depending on count.

---

## Server Authority

### Why Server Owns All Maps

- Prevents client-side cheating (can't claim more resources than exist)
- Single source of truth for population dynamics, degradation, resource depletion
- Clients get read-only snapshots for rendering
- Server saves maps to disk periodically (PNG files, same as Metal Deposits)

### Network: Map Synchronization

Full-resolution texture sync is expensive. Options:

1. **Delta compression**: Server tracks changed pixels, sends only diffs
2. **Region of interest**: Client only needs maps within render distance (~200m)
3. **Low-frequency updates**: Vegetation changes slowly — sync every 5-10 seconds
4. **Event-driven**: Server sends point updates on player actions (chop tree → clear pixel)
5. **Initial load**: Client downloads full maps on connect (4 × 1024² × 4 bytes = 16 MB uncompressed, ~2 MB compressed)

For prototype: client loads maps from disk (same files as server). Multiplayer sync deferred.

### Persistence

Server saves ecosystem state to disk:
```
terrain_data/
  heightmap.png          ← existing
  moisture.png           ← Layer 1
  soil.png               ← Layer 2
  vegetation.png         ← Layer 3
  metal_deposits.png     ← Layer 4
```

Saved periodically and on server shutdown. Loaded on startup. World state persists across sessions.

---

## Implementation Phases

### Phase 1: Vegetation Map + Grass Integration — CODER SPEC

**Deliverable:** Create a 1024x1024 RGBA vegetation texture seeded from the heightmap. Bind as density source for GPU Grass (vegetation.R channel). Server-authoritative, client receives snapshot. No weather or soil input yet — Phase 1 is static generation from terrain shape.

**Dependencies:** GPU Grass system (Phase 1 of that plan) must exist first.

#### Header: `60_TerrainNode/EcosystemManager.h`

```cpp
#pragma once
#include <Urho3D/Core/Object.h>
#include <Urho3D/Graphics/Texture2D.h>
#include <Urho3D/Resource/Image.h>

using namespace Urho3D;

class Terrain;

/// Manages ecosystem texture layers. Phase 1: vegetation map only.
class EcosystemManager : public Object
{
    URHO3D_OBJECT(EcosystemManager, Object);

public:
    explicit EcosystemManager(Context* context);

    /// Generate vegetation map from terrain heightmap.
    void Initialize(Terrain* terrain, float waterLevel);

    /// Get vegetation texture for GPU binding.
    Texture2D* GetVegetationTexture() const { return vegetationTex_; }

    /// Sample grass density at a world position (0.0–1.0).
    float SampleGrassDensity(float worldX, float worldZ) const;

    /// Sample biome type at a world position.
    unsigned char SampleBiome(float worldX, float worldZ) const;

    /// Apply seasonal multiplier (0.0 winter → 1.5 spring).
    void SetSeasonMultiplier(float mult);

private:
    void SeedFromHeightmap(Terrain* terrain, float waterLevel);
    IntVector2 WorldToPixel(float worldX, float worldZ) const;

    SharedPtr<Image> vegetationImage_;     // CPU-side RGBA data
    SharedPtr<Texture2D> vegetationTex_;   // GPU texture for grass shader
    float terrainOriginX_{0.0f};
    float terrainOriginZ_{0.0f};
    float terrainSizeX_{1.0f};
    float terrainSizeZ_{1.0f};
    float seasonMultiplier_{1.0f};
    static constexpr int MAP_SIZE = 1024;
};
```

#### Implementation: `60_TerrainNode/EcosystemManager.cpp` (key logic)

```cpp
void EcosystemManager::SeedFromHeightmap(Terrain* terrain, float waterLevel)
{
    vegetationImage_ = new Image(context_);
    vegetationImage_->SetSize(MAP_SIZE, MAP_SIZE, 4);  // RGBA

    Vector3 terrainPos = terrain->GetNode()->GetWorldPosition();
    Vector3 terrainSize = terrain->GetBoundingBox().Size();
    terrainOriginX_ = terrainPos.x_ - terrainSize.x_ * 0.5f;
    terrainOriginZ_ = terrainPos.z_ - terrainSize.z_ * 0.5f;
    terrainSizeX_ = terrainSize.x_;
    terrainSizeZ_ = terrainSize.z_;

    for (int y = 0; y < MAP_SIZE; ++y)
    {
        for (int x = 0; x < MAP_SIZE; ++x)
        {
            // Map pixel to world position
            float wx = terrainOriginX_ + (float(x) / MAP_SIZE) * terrainSizeX_;
            float wz = terrainOriginZ_ + (float(y) / MAP_SIZE) * terrainSizeZ_;
            float height = terrain->GetHeight(Vector3(wx, 0.0f, wz));
            Vector3 normal = terrain->GetNormal(Vector3(wx, 0.0f, wz));
            float slope = 1.0f - normal.y_;  // 0=flat, 1=vertical

            // Classify
            unsigned char grassDensity = 0;   // R
            unsigned char shrubDensity = 0;   // G
            unsigned char treeMaturity = 0;   // B
            unsigned char biomeType = 0;      // A: 0=barren,1=grassland,2=forest,3=wetland,4=alpine

            if (height < waterLevel)
            {
                // Underwater — no land vegetation
                biomeType = 0;
            }
            else if (slope > 0.6f)
            {
                // Steep cliff — barren
                biomeType = 0;
                grassDensity = (unsigned char)(30 * (1.0f - slope));
            }
            else if (height > waterLevel + 80.0f)
            {
                // High altitude — alpine
                biomeType = 4;
                grassDensity = (unsigned char)(80 * (1.0f - (height - waterLevel - 80.0f) / 40.0f));
            }
            else if (height < waterLevel + 5.0f && slope < 0.2f)
            {
                // Low flat near water — wetland
                biomeType = 3;
                grassDensity = 150;
                shrubDensity = 60;
            }
            else if (height < waterLevel + 30.0f && slope < 0.3f)
            {
                // Low/mid altitude, gentle slope — forest
                biomeType = 2;
                grassDensity = 120;
                shrubDensity = 80;
                treeMaturity = 200;
            }
            else
            {
                // Mid altitude — grassland
                biomeType = 1;
                grassDensity = 200;
                shrubDensity = 30;
            }

            vegetationImage_->SetPixel(x, y, Color(
                grassDensity / 255.0f,
                shrubDensity / 255.0f,
                treeMaturity / 255.0f,
                biomeType / 255.0f
            ));
        }
    }

    // Upload to GPU
    vegetationTex_ = new Texture2D(context_);
    vegetationTex_->SetFilterMode(FILTER_BILINEAR);
    vegetationTex_->SetAddressMode(COORD_U, ADDRESS_CLAMP);
    vegetationTex_->SetAddressMode(COORD_V, ADDRESS_CLAMP);
    vegetationTex_->SetData(vegetationImage_);
}

float EcosystemManager::SampleGrassDensity(float worldX, float worldZ) const
{
    IntVector2 px = WorldToPixel(worldX, worldZ);
    if (px.x_ < 0 || px.x_ >= MAP_SIZE || px.y_ < 0 || px.y_ >= MAP_SIZE)
        return 0.0f;
    Color c = vegetationImage_->GetPixel(px.x_, px.y_);
    return c.r_ * seasonMultiplier_;
}
```

#### Grass Shader Integration

```glsl
// In Grass.glsl vertex shader:
uniform sampler2D sVegetationMap;  // bind vegetation texture to TU slot

// Replace procedural density with:
float density = texture2D(sVegetationMap, vTerrainUV).r;
if (density < uDensityThreshold)
{
    // Collapse blade to degenerate triangle
    gl_Position = vec4(0.0);
    return;
}
```

#### Phase 1 Acceptance Criteria

1. 1024x1024 RGBA vegetation texture generated from terrain heightmap
2. R channel = grass density (0–255), varies by altitude/slope/biome
3. G channel = shrub density, B channel = tree maturity, A = biome type enum
4. Underwater pixels have zero vegetation
5. Steep slopes have reduced grass, high altitude has alpine thinning
6. Low flat areas near water classified as wetland with moderate grass
7. `SampleGrassDensity(x, z)` returns correct value for any world position
8. `SampleBiome(x, z)` returns biome classification
9. Vegetation texture bound to GPU Grass shader as density source
10. Seasonal multiplier scales grass density (spring 1.5, winter 0.0)

### Phase 2: Moisture Map Integration

**Goal:** Weather rainfall feeds into vegetation growth.

1. Weather system writes to moisture map (already designed in Weather §4)
2. Server update loop: moisture.G → vegetation growth/decay per tick
3. Verify: rain makes grass grow, drought makes it thin
4. Surface water flow (simple downhill propagation)

**Dependencies:** Weather System Phase 4 (rainfall accumulation).

### Phase 3: Soil Layer

**Goal:** Soil quality modulates vegetation.

1. Generate soil map from heightmap (composition from geology, fertility starts high)
2. Wire soil into vegetation growth formula
3. Implement trampling (player foot traffic → soil.A increases → grass density drops → visible paths)
4. Implement farming degradation (harvest → soil.R decreases per Economic Doctrine)

### Phase 4: Animal Food Integration

**Goal:** Animals respond to vegetation.

1. Expose `foodValue(pos)` function reading vegetation map
2. Add FORAGE state to Animal base class
3. Animals move toward food, graze (decrement vegetation.R locally)
4. Population dynamics: server tracks birth/death rates from food availability
5. Verify: overgrazed areas thin out, animals migrate to greener pastures

### Phase 5: Tree Lifecycle

**Goal:** Trees grow, get chopped, regrow (slowly, with degradation).

1. vegetation.B tracks tree maturity (0–255 lifecycle)
2. Maturity increments per game-week where moisture + soil permit
3. Chopping sets maturity to 0, increments soil erosion, reduces local canopy
4. Deforestation cascade: erosion → fertility drop → grass thins → food drops → animals leave
5. Wire to Imposter Trees rendering (maturity → LOD stage)

### Phase 6: Paintable Brushes + Admin Tools

**Goal:** Server admin can paint ecosystem layers.

1. Extend terrain brush system to target ecosystem textures
2. Brush types: plant forest, create meadow, set fertility, drought, flood
3. Preview overlay showing selected layer (like heightmap wireframe)

---

## Performance Considerations

| Concern | Mitigation |
|---------|-----------|
| 4 extra textures per terrain | 1024² RGBA = 4 MB each, 16 MB total. Trivial. |
| Server update loop | Run at reduced rate (1 Hz for moisture, 0.1 Hz for vegetation). Not per-frame. |
| Client texture upload | Only on snapshot receive. Not per-frame. Moisture/vegetation change slowly. |
| Vegetation sampling in grass VS | One extra texture fetch per vertex. VTF already required for heightmap — this adds ~5% cost. |
| Animal food queries | Server samples vegetation map at animal position. O(1) per animal per tick. |
| Disk I/O for persistence | Write PNGs asynchronously on a timer. 4 × 4 MB every 5 minutes. Negligible. |

---

## Files To Create

| File | Purpose |
|------|---------|
| `Source/Game/Ecosystem.h` | Ecosystem component — owns texture layers, update loop, food queries |
| `Source/Game/Ecosystem.cpp` | Layer management, procedural seeding, growth/decay simulation |

Or, for prototype scope:

| File | Purpose |
|------|---------|
| `60_TerrainNode/EcosystemManager.h` | Header — texture layers, update methods |
| `60_TerrainNode/EcosystemManager.cpp` | Implementation — seed, update, query API |

## Files To Modify

| File | Change |
|------|--------|
| `TerrainNode.cpp` | Create EcosystemManager, wire to grass system + animals |
| `TerrainNode.h` | `SharedPtr<EcosystemManager> ecosystem_` member |
| `GrassSystem.cpp` (future) | Sample vegetation.R instead of splat map for density |
| `Animal.cpp` | Query ecosystem for food value, add FORAGE state |
| `Grass.glsl` (future) | Bind sVegetationMap, sample R channel for density |

---

## Relationship to Other Plans

| Plan | Relationship |
|------|-------------|
| **Economic Doctrine** | Provides the degradation model. Every extraction reduces max capacity. Ecosystem enforces it spatially. |
| **Metal Deposits** | Proved the texture-as-database pattern. Resource map is Layer 4 in the ecosystem stack. |
| **Weather System** | Produces rainfall → moisture map (Layer 1). Wind drives cloud movement → spatial weather. |
| **GPU Grass** | Consumes vegetation.R as density input. One texture sample in vertex shader. |
| **Imposter Trees** | Consumes vegetation.B for tree placement and maturity → LOD selection. |
| **Animal Classes** | Consumes vegetation map as food source. Animals graze → decrement vegetation. Population tracks food. |
| **Resource Chain** | Harvestable resources (fiber, berries, wood) are spatial queries on vegetation map. |
| **World Expansion** | New terrain patches spawn with pristine ecosystem (full fertility, mature vegetation). |
| **Driven Keys** | Moisture → vegetation → grass density is a natural driven-key chain. Curves replace linear formulas later. |
| **Paintable Water** | Surface water from moisture map could feed into paintable water system for dynamic streams. |

---

## Resolved Questions

1. **Resolution: Match heightmap exactly (1024×1024).** The GPU already samples the heightmap at this resolution via VTF. Adding vegetation as a second VTF sample at the same resolution is trivial — same UV coordinates, same grid alignment, no interpolation mismatch. Half-res would save 12 MB total but introduce sampling artifacts at grid boundaries. Not worth the complexity.

2. **Biome boundaries: Blended via vegetation channels, not sharp enum boundaries.** The biome type (L3.A) is a classification label, not a hard edge. At biome transitions, grass density (R) and tree maturity (B) blend naturally because they're computed from moisture + soil which are themselves continuous fields. Visual blending happens automatically — a forest edge is where tree maturity gradually drops below the sapling threshold. No Voronoi needed.

3. **Fire propagation: Design now, implement in Phase 5.** Lightning (already implemented) + dry vegetation (low moisture.G + high vegetation.R) = ignition. Fire spreads to adjacent pixels per tick: `if neighbor.vegetation.R > 50 AND neighbor.moisture.G < 80 → ignite`. Burned pixels: vegetation.R/G/B → 0, soil.B (erosion) increases. Recovery follows Economic Doctrine degradation curve — burned forest recovers slower each time. Implementation is just another server-side update loop on the vegetation map. Design is clean; defer coding until the base ecosystem loop works.

4. **Underwater vegetation: Same vegetation map, depth check.** If `heightmap(x,z) < waterLevel`, vegetation.R represents seagrass/kelp instead of land grass. The GPU Grass shader already knows water level (existing uniform). Below water: render as seagrass (different sway, shorter, blue-green tint). Above water: normal grass. One texture, one system, biome type (L3.A) can encode "submerged" to select the right visual. No separate layer needed.

5. **Seed dispersal: Yes, but slow and bounded.** Trees spread to adjacent pixels where conditions permit: `if vegetation.B(x,z) > 200 (mature) AND neighbor.B == 0 AND neighbor.soil.R > 50 AND neighbor.moisture.G > 40 → set neighbor.B = 1 (seedling)`. Rate: one spread check per mature tree per game-month. Max spread distance: 1 pixel per check (trees spread ~1 meter per month). This means a pristine forest slowly colonizes adjacent grassland — realistic secondary succession. Deforested areas can regrow IF soil hasn't degraded too far. Player-planted seeds bypass the adjacency check (can plant anywhere soil permits).
