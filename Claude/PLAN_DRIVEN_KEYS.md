# PLAN: Driven Keys — Universal Parameter-to-Parameter Curves

**Status:** DRAFT
**Owner:** Planner
**Priority:** 0 — foundational system, every other plan benefits
**Hardware target:** CPU-side evaluation, negligible cost

---

## Problem

Every system in the game has hardcoded relationships between values:

```cpp
// TerrainNode.cpp — rain drives fog
fogEnd *= 1.0f - (cloudDensity - 0.4f) * 0.8f;

// Weather — temperature from season + altitude
temperature = baseTemp - altitudeEffect - nightEffect;

// Animal — flee speed from health
fleeSpeed = baseSpeed * (health / maxHealth);

// God rays — intensity from cloud cover
godRayIntensity = baseIntensity * (1.0f - cloudDensity);
```

These are all the same pattern: **one value drives another through a response function.** Currently every response is a linear formula buried in code. Want a different curve? Edit C++, recompile. Want an artist to tune the feel of rain-to-fog? They can't — it's in code.

Maya solved this in 1998 with Set Driven Key. A driver parameter controls a driven parameter through an editable curve. Any parameter can drive any other parameter. Curves are data, not code.

---

## Core Concept

A **driven key** is:

```
driver_parameter  →  curve  →  driven_parameter
```

- **Driver:** Any readable value (float). Animation time, cloud density, player speed, altitude, health, slider position, time of day.
- **Curve:** A set of control points defining the response. Linear, smooth (Hermite/Catmull-Rom), stepped. Editable.
- **Driven:** Any writable value (float). Fog distance, particle rate, sound volume, blend weight, color channel, shader uniform.

### What Makes It Universal

The system doesn't know or care what the values represent. It reads a float, evaluates a curve, writes a float. The semantics are in the binding — which parameter is the driver, which is the driven. The curve just maps input range to output range.

Animation text keys are a special case: driver = playback time, driven = event trigger (curve is a step function: 0 before the key time, 1 at the key time).

---

## Architecture

### DrivenKey Struct

```cpp
struct CurvePoint
{
    float in;           // Driver value at this point
    float out;          // Driven value at this point
    float tangentIn;    // Incoming tangent (for Hermite)
    float tangentOut;   // Outgoing tangent (for Hermite)
};

enum InterpolationMode
{
    INTERP_LINEAR,      // Straight lines between points
    INTERP_SMOOTH,      // Catmull-Rom or Hermite spline
    INTERP_STEP,        // Hold previous value until next point (events, boolean triggers)
    INTERP_CONSTANT     // Output is always the same regardless of input
};

struct DrivenKey
{
    String name;                    // Human-readable label ("Rain→Fog", "Speed→Lean")
    StringHash driverParam;         // What we read
    StringHash drivenParam;         // What we write
    InterpolationMode mode;         // How we interpolate between points
    Vector<CurvePoint> points;      // The curve data (sorted by in)
    float preInfinity;              // Value when driver < first point (clamp or extrapolate)
    float postInfinity;             // Value when driver > last point (clamp or extrapolate)
    bool enabled;                   // Can be toggled without removing
};
```

### Evaluation

```cpp
float DrivenKey::Evaluate(float driverValue) const
{
    // Binary search for the segment containing driverValue
    // Interpolate between surrounding points using mode
    // Handle pre/post infinity
    return drivenValue;
}
```

This is a single function. O(log n) for the search, trivial math for the interpolation. Evaluating 100 driven keys per frame costs nothing measurable.

### DrivenKeySet — A Collection

A `DrivenKeySet` groups related driven keys. Examples:

- **"Weather Atmosphere"** set: cloud density drives fog, ambient, god rays, wind sound volume
- **"Player Movement"** set: speed drives camera lean, footstep rate, breath sound
- **"Fox Attack"** set: animation time drives hit window, sound cue, particle spawn
- **"Day Cycle"** set: time of day drives ambient color, bird volume, shadow softness

Sets are the unit of authoring, saving, and loading. One JSON file per set.

### DrivenKeySystem — The Evaluator

A subsystem (or component) that holds all active DrivenKeySets and evaluates them each frame:

```cpp
class DrivenKeySystem : public Object
{
    // Registration
    void AddSet(const DrivenKeySet& set);
    void RemoveSet(StringHash name);

    // Per-frame
    void Update(float timeStep);
    // For each enabled key in each set:
    //   1. Read driver value (from parameter source)
    //   2. Evaluate curve
    //   3. Write driven value (to parameter target)

    // Parameter sources/targets
    void RegisterSource(StringHash param, std::function<float()> getter);
    void RegisterTarget(StringHash param, std::function<void(float)> setter);
};
```

Parameter sources and targets are registered by the systems that own them:

```cpp
// Weather registers cloud density as a readable parameter
drivenKeys->RegisterSource("CloudDensity"_hash, [this]() { return localCloudDensity_; });

// Fog registers fog end as a writable parameter
drivenKeys->RegisterTarget("FogEnd"_hash, [this](float v) { zone_->SetFogEnd(v); });

// Now anyone can create a driven key: CloudDensity → FogEnd with any curve shape
```

This decouples everything. Weather doesn't know about fog. Fog doesn't know about weather. The driven key connects them through data.

---

## Data Format (JSON)

```json
{
    "name": "Weather Atmosphere",
    "keys": [
        {
            "name": "Cloud→Fog",
            "driver": "CloudDensity",
            "driven": "FogEnd",
            "mode": "smooth",
            "points": [
                {"in": 0.0, "out": 300.0},
                {"in": 0.4, "out": 250.0},
                {"in": 0.7, "out": 100.0},
                {"in": 1.0, "out": 30.0}
            ],
            "preInfinity": "clamp",
            "postInfinity": "clamp"
        },
        {
            "name": "Cloud→GodRays",
            "driver": "CloudDensity",
            "driven": "GodRayIntensity",
            "mode": "linear",
            "points": [
                {"in": 0.0, "out": 1.0},
                {"in": 0.5, "out": 0.3},
                {"in": 1.0, "out": 0.0}
            ]
        },
        {
            "name": "Cloud→AmbientBrightness",
            "driver": "CloudDensity",
            "driven": "AmbientMultiplier",
            "mode": "smooth",
            "points": [
                {"in": 0.0, "out": 1.0},
                {"in": 0.6, "out": 0.6},
                {"in": 1.0, "out": 0.3}
            ]
        }
    ]
}
```

An artist opens this in a curve editor, drags points, saves. The game loads it. No recompile.

---

## Animation Text Keys as Driven Keys

Animation text keys are step-function driven keys where:
- **Driver:** animation playback time (normalized 0–1 or absolute seconds)
- **Driven:** event trigger (0 or 1)
- **Mode:** STEP
- **Points:** one point per text key at its time, out=1

```json
{
    "name": "Fox_Gallop Events",
    "keys": [
        {
            "name": "FootDown_Left",
            "driver": "AnimTime",
            "driven": "Event_FootDown",
            "mode": "step",
            "data": "left_foot",
            "points": [
                {"in": 0.35, "out": 1.0},
                {"in": 0.36, "out": 0.0}
            ]
        },
        {
            "name": "FootDown_Right",
            "driver": "AnimTime",
            "driven": "Event_FootDown",
            "data": "right_foot",
            "points": [
                {"in": 0.85, "out": 1.0},
                {"in": 0.86, "out": 0.0}
            ]
        }
    ]
}
```

The existing engine text key system (`AnimationTextKey`, `E_ANIMATIONTEXTKEY`) already handles the animation-time case. The driven key system generalizes it. Both can coexist — text keys for simple animation events, driven keys for everything else.

---

## Use Cases (Concrete)

### Already In Codebase (Currently Hardcoded)

| Driver | Driven | Current Code | Driven Key Replacement |
|--------|--------|-------------|----------------------|
| Cloud density | Fog end distance | `fogEnd *= 1.0 - (cd-0.4)*0.8` | Smooth curve, 4 points |
| Cloud density | God ray intensity | `intensity * (1-cd)` | Linear, 2 points |
| Cloud density | Ambient brightness | inline multiply | Smooth curve |
| Cloud density | Rain particle rate | `(cd-0.6)/0.4 * maxRate` | Linear ramp |
| Cloud density | Wind sound volume | threshold check | Smooth S-curve |
| Time of day | Sun direction | trig in code | Could stay in code (math, not tuning) |
| Season | Grass tint | existing tint system | 4-point seasonal curve |
| Altitude | Temperature | `baseTemp - altEffect` | Linear, but tunable |
| Player speed | Footstep interval | not yet implemented | Inverse curve |

### Future (Enabled by System)

| Driver | Driven | Use Case |
|--------|--------|----------|
| Animation time | Event trigger | Footstep sound at exact frame |
| Animation time | IK blend weight | Foot plants at contact frame |
| Player health | Screen desaturation | Low health → color drains |
| Hunger | Movement speed | Starving → slow |
| Wind speed | Tree sway amplitude | Storm → violent sway |
| Distance to fire | Light intensity | Flickering campfire falloff |
| Moisture map value | Grass density | Wet areas → thick grass |
| Soil fertility | Crop growth rate | Rich soil → fast harvest |
| Population density | Animal spawn rate | Overpopulated → fewer births |
| Weapon swing time | Damage window | HitStart → HitEnd as curve region |
| Vehicle speed | Wheel spin rate | Driven literally |
| Plane altitude | Landing gear blend | Gear retracts above threshold |
| Bow draw | Arrow power | Pull further → shoot harder |

The system doesn't know about any of these domains. It just maps floats through curves.

---

## Curve Editor (ModelViewer Extension)

### Why ModelViewer

The Animation Editor plan (PLAN_ANIMATION_EDITOR.md) already adds a timeline to ModelViewer for text key authoring. A curve editor extends this naturally:

- Timeline shows keyframe markers (already planned)
- Curve editor shows the response curve for a selected driven key
- Same scrub bar drives both — scrub time, see where keys fire AND where curves evaluate
- For non-animation driven keys, the X axis is the driver parameter instead of time

### Minimal Curve Editor UI

```
┌─ Curve: Cloud→Fog ──────────────┐
│                                  │
│  300 ─●                          │
│       │ ╲                        │
│  200 ─│   ╲                      │
│       │    ╲                     │
│  100 ─│     ●───╲               │
│       │          ╲              │
│   30 ─│           ●             │
│       └──┼───┼───┼───┼──        │
│        0.0  0.4  0.7  1.0       │
│         CloudDensity →           │
│                                  │
│ Points: 4   Mode: [Smooth ▼]    │
│ Selected: (0.7, 100.0)          │
│ [Add] [Delete] [Save]           │
└──────────────────────────────────┘
```

- Click to add point
- Drag to move point
- Right-click to delete
- Dropdown for interpolation mode
- Save writes JSON sidecar

This doesn't need to be in ModelViewer exclusively — it could be a standalone CurveEditor tool or a panel in any tool. But ModelViewer is the natural first home because animation-time driven keys are the first use case.

### Workflow

1. Load a model + animation in ModelViewer
2. Scrub to the frame where the sword hits the scabbard
3. Add a text key: "SwordSheathe" at t=1.2s (step trigger)
4. Switch to curve view: create "SwingTime→SwordGrip" driven key
5. At t=0.0: grip=1.0 (holding sword)
6. At t=1.0: grip=1.0 (still holding)
7. At t=1.2: grip=0.0 (released into scabbard)
8. Smooth interpolation between 1.0→1.2 for the release motion
9. Save. Game loads the curve. Sword release is data-driven.

---

## Implementation Phases

### Phase 1: Core Evaluation Engine

1. `DrivenKey` struct with CurvePoint, interpolation modes
2. `Evaluate(float)` — binary search + interpolation
3. `DrivenKeySet` — collection with JSON load/save
4. `DrivenKeySystem` — RegisterSource, RegisterTarget, Update loop
5. Unit test: create curve, evaluate at known points, verify output

**Scope:** ~300 lines of code. Pure data structures + math. No UI, no integration.

### Phase 2: Wire to Existing Systems

1. Register weather parameters as sources (CloudDensity, Temperature, WindSpeed, etc.)
2. Register atmosphere parameters as targets (FogEnd, GodRayIntensity, AmbientMultiplier, etc.)
3. Create `weather_atmosphere.json` curve set replacing hardcoded multipliers
4. Load curve set in TerrainNode, evaluate per frame
5. Delete the hardcoded lines. Verify identical behavior from curves.
6. **Tweak curves without recompiling** — edit JSON, restart, see results

### Phase 3: Animation Integration

1. Register AnimationState time as a source
2. Text keys evaluate as step-function driven keys
3. Extend ModelViewer text key editor to show curve view
4. Verify: existing text key behavior preserved, new curve keys work alongside

### Phase 4: Curve Editor UI

1. Canvas widget in ModelViewer — draw curve from points
2. Click/drag point manipulation
3. Interpolation mode selector
4. JSON save/load
5. Live preview: scrub driver value, see driven value update in real time

### Phase 5: Generalization

1. Any LogicComponent can own a DrivenKeySet
2. Components register their own sources/targets on creation
3. Driven keys become part of the scene serialization (node attribute or resource)
4. Multiple objects can share the same curve data (resource, not per-instance)

---

## Files To Create

| File | Purpose |
|------|---------|
| `Source/Urho3D/Scene/DrivenKey.h` | DrivenKey, CurvePoint, DrivenKeySet structs |
| `Source/Urho3D/Scene/DrivenKey.cpp` | Evaluate, JSON load/save, interpolation |
| `Source/Urho3D/Scene/DrivenKeySystem.h` | System header — registration, update |
| `Source/Urho3D/Scene/DrivenKeySystem.cpp` | System implementation |

Or for prototype scope (Sample 60 only):

| File | Purpose |
|------|---------|
| `60_TerrainNode/DrivenKeys.h` | All-in-one header |
| `60_TerrainNode/DrivenKeys.cpp` | Implementation |

## Files To Modify

| File | Change |
|------|--------|
| `TerrainNode.cpp` | Replace hardcoded weather→atmosphere multipliers with curve evaluation |
| `ModelViewer.cpp` | Curve editor panel (Phase 4) |

---

## Why This Matters

Every game system we build generates coupling. Weather knows about fog. Fog knows about god rays. Animals know about vegetation density. Each connection is a hardcoded formula in C++.

Driven keys turn coupling into data. Systems expose parameters. Curves connect them. Artists tune the feel without touching code. New connections are created by writing JSON, not C++.

The plane's landing gear retracts because someone put a curve point at altitude=10 mapping to gear_blend=0. The sword goes into the scabbard because someone put a step key at t=1.2. The fog rolls in because someone shaped a curve from cloud density to fog distance. Same system. Same editor. Same evaluation loop.

One mechanism, infinite uses.
