# PLAN: Ecosystem — Texture-Based World Database

**Status:** DRAFT
**Owner:** Planner
**Priority:** High — backbone connecting weather, grass, animals, resources, economy
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

### Phase 1: Vegetation Map + Grass Integration

**Goal:** Replace procedural grass density with a real vegetation map.

1. Create a vegetation texture (1024×1024 RGBA) at terrain resolution
2. Seed it procedurally from heightmap (altitude/slope → biome → density)
3. Bind to GPU Grass system as density source (vegetation.R)
4. Verify: grass grows thick in valleys, thin on hills, absent on rock
5. Add seasonal multiplier to vegetation.R sampling

**Dependencies:** GPU Grass system (Phase 1 of that plan) must exist first.

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

## Open Questions

1. **Resolution:** Should ecosystem maps match heightmap resolution exactly (1024×1024) or run at half-res (512×512) for performance? Vegetation doesn't need per-meter precision.
2. **Biome boundaries:** Sharp or blended? Current plan uses per-pixel biome type (L3.A). Could use Voronoi noise for natural-looking boundaries.
3. **Fire propagation:** Dry vegetation + lightning = fire. Fire burns vegetation.R/G/B to 0 in a spreading pattern. Design now or defer?
4. **Underwater vegetation:** Seagrass, kelp? Different layer or same vegetation map with depth check?
5. **Seed dispersal:** Do trees spread (maturity at pixel X seeds adjacent pixels)? Or only grow where procedurally seeded + player-planted?
