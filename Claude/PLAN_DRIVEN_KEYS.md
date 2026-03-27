# PLAN: Driven Keys — Universal Parameter-to-Parameter Curves

**Status:** CODER-READY — v3 (Phase 1 spec sharpened for cold handoff)
**Owner:** Planner
**Priority:** 0 — foundational system, every other plan benefits
**Hardware target:** CPU-side evaluation, negligible cost
**Catalog:** See `Planner_NOTE_PARAMETER_CATALOG.md` for TerrainNode.cpp hardcoded relationships
**Wiring Diagram:** See `Planner_DRIVEN_KEYS_WIRING_DIAGRAM.md` — complete cross-system interconnect map (32 drivers, ~140 driven params, 58 JSON files, all 20+ plans)

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

A subsystem that holds all active DrivenKeySets, evaluates curves, and **broadcasts results as Urho3D events**.

```cpp
class DrivenKeySystem : public Object
{
    URHO3D_OBJECT(DrivenKeySystem, Object);

    // Registration
    void AddSet(const DrivenKeySet& set);
    void RemoveSet(StringHash name);

    // Driver input — game code pushes values in
    void SetDriver(StringHash param, float value);
    float GetDriver(StringHash param) const;

    // Per-frame evaluation
    void Update(float timeStep);
    // For each enabled key in each set:
    //   1. Read driver value from internal map
    //   2. Evaluate curve
    //   3. SendEvent(E_DRIVENKEY_OUTPUT, param, value)
    //   Only fires event if value actually changed (dead-band threshold)
};
```

### Output Via Urho3D Events (The Key Insight)

Driven keys can output their values to **almost any interested party**. A fog system, a material, a sound emitter, a particle effect, a UI widget — any of them might care about a driven value. The system can't and shouldn't know who's listening.

**Solution: Urho3D's native event system.**

```cpp
// DrivenKeyEvents.h
URHO3D_EVENT(E_DRIVENKEY_OUTPUT, DrivenKeyOutput)
{
    URHO3D_PARAM(P_PARAM, Param);       // StringHash — which parameter changed
    URHO3D_PARAM(P_VALUE, Value);       // float — new value
    URHO3D_PARAM(P_DRIVER, Driver);     // StringHash — what drove it
    URHO3D_PARAM(P_DRIVERVALUE, DriverValue); // float — driver's current value
}

// Fired once per changed output per frame. Not fired if value didn't change.
```

**Any system subscribes to the event and filters by parameter name:**

```cpp
// Zone atmosphere handler — subscribes once at setup
SubscribeToEvent(E_DRIVENKEY_OUTPUT, URHO3D_HANDLER(TerrainNode, HandleDrivenKeyOutput));

void TerrainNode::HandleDrivenKeyOutput(StringHash eventType, VariantMap& eventData)
{
    StringHash param = eventData[DrivenKeyOutput::P_PARAM].GetStringHash();
    float value = eventData[DrivenKeyOutput::P_VALUE].GetFloat();

    if (param == "zone.fogStart"_hash)
        zone_->SetFogStart(value);
    else if (param == "zone.fogEnd"_hash)
        zone_->SetFogEnd(value);
    else if (param == "godray.intensity"_hash)
        renderPath_->SetShaderParameter("GodRayIntensity", value);
    // ...
}
```

**Or a material handler — completely independent, doesn't know TerrainNode exists:**

```cpp
// GrassBehavior component — subscribes to grass-related driven values
void GrassBehavior::HandleDrivenKeyOutput(StringHash eventType, VariantMap& eventData)
{
    StringHash param = eventData[DrivenKeyOutput::P_PARAM].GetStringHash();
    float value = eventData[DrivenKeyOutput::P_VALUE].GetFloat();

    if (param == "grass.tint.r"_hash)
    { Color c = grassMat_->GetShaderParameter("MatDiffColor").GetColor(); c.r_ = value; grassMat_->SetShaderParameter("MatDiffColor", c); }
}
```

**Or a future sound system:**

```cpp
void AmbientSound::HandleDrivenKeyOutput(StringHash eventType, VariantMap& eventData)
{
    if (eventData[P_PARAM].GetStringHash() == "wind.intensity"_hash)
        windLoop_->SetGain(eventData[P_VALUE].GetFloat());
}
```

### Why Events, Not Callbacks

| Approach | Coupling | Runtime Cost | New Subscriber |
|----------|----------|-------------|----------------|
| Direct function call | Tight — system must know targets | Fastest | Modify system code |
| Callback/lambda registration | Medium — system stores closures | Fast | Register at setup |
| **Urho3D events** | **Zero — publisher doesn't know subscribers** | Slightly more (hash dispatch) | **Subscribe anywhere, no system changes** |

Events win because:
1. **Open/closed principle** — new consumers don't modify the driven key system
2. **Standard Urho3D pattern** — every component already knows `SubscribeToEvent`
3. **Network-ready** — events can be forwarded over the network (AuthServer broadcasts driven key changes to clients)
4. **Debuggable** — event flow is visible in profiler, can be logged
5. **Composable** — a handler can subscribe to multiple driven outputs and combine them (e.g. ambient = base_ambient * weather_dim * lightning_flash)

### Driver Input

Game code pushes driver values each frame. The system evaluates and broadcasts:

```cpp
// In TerrainNode::UpdateWeather():
auto* dk = GetSubsystem<DrivenKeySystem>();
dk->SetDriver("weather.cloudCover"_hash, weather_.cloudCover);
dk->SetDriver("weather.precipitation"_hash, weather_.precipitation);
dk->SetDriver("sun.altitude"_hash, cachedSunAlt_);
dk->SetDriver("season.angle"_hash, cachedSeasonAngle_);
// ... etc

// DrivenKeySystem::Update() evaluates all curves and fires events
// Subscribers react — zone, materials, render path, sound, particles
```

### Chaining

Driven outputs can themselves be drivers. Cloud cover (driver) → fog density (driven+driver) → visibility distance (driven). The system handles this by evaluating in declared order. If chain order matters, the JSON file controls it. Phase 2 adds topological sort if artists find manual ordering confusing.

### Dead-Band Filtering

Events only fire when the output actually changes beyond a threshold (default 0.001). This prevents flooding the event bus with identical values every frame when a driver is stable. The threshold is configurable per curve.

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

### Phase 1: Core Evaluation Engine — CODER SPEC

**Deliverable:** Three source files, one event header, one test JSON. System registers as subsystem, evaluates curves, fires events. No UI, no integration with TerrainNode yet.

#### File 1: `Source/Urho3D/Scene/DrivenKeyEvents.h`

```cpp
#pragma once
#include "../Core/Object.h"

namespace Urho3D
{

URHO3D_EVENT(E_DRIVENKEY_OUTPUT, DrivenKeyOutput)
{
    URHO3D_PARAM(P_PARAM, Param);           // StringHash — which driven parameter changed
    URHO3D_PARAM(P_VALUE, Value);           // float — new driven value
    URHO3D_PARAM(P_DRIVER, Driver);         // StringHash — which driver caused this
    URHO3D_PARAM(P_DRIVERVALUE, DriverValue); // float — driver's current value
}

}
```

#### File 2: `Source/Urho3D/Scene/DrivenKey.h`

```cpp
#pragma once
#include "../Container/Str.h"
#include "../Container/Vector.h"
#include "../Math/StringHash.h"

namespace Urho3D
{

class JSONValue;

struct CurvePoint
{
    float in;
    float out;
    float tangentIn;    // for INTERP_SMOOTH (Hermite)
    float tangentOut;
};

enum InterpolationMode
{
    INTERP_LINEAR = 0,
    INTERP_SMOOTH,      // Catmull-Rom
    INTERP_STEP,        // hold previous until next
    INTERP_CONSTANT     // always returns first point's out
};

enum InfinityMode
{
    INFINITY_CLAMP = 0,
    INFINITY_EXTRAPOLATE
};

struct DrivenKey
{
    String name;
    StringHash driverParam;
    StringHash drivenParam;
    InterpolationMode mode{INTERP_LINEAR};
    InfinityMode preInfinity{INFINITY_CLAMP};
    InfinityMode postInfinity{INFINITY_CLAMP};
    Vector<CurvePoint> points;      // sorted by in
    float deadBand{0.001f};         // event only fires if |newVal - lastVal| > this
    bool enabled{true};

    /// Evaluate the curve at the given driver value.
    float Evaluate(float driverValue) const;
    /// Load from a JSON object (one key entry).
    void LoadJSON(const JSONValue& value);
    /// Save to a JSON object.
    JSONValue SaveJSON() const;

    // Runtime — not serialized
    float lastOutput{0.0f};
};

struct DrivenKeySet
{
    String name;
    Vector<DrivenKey> keys;

    /// Load from a JSON file root object.
    bool LoadJSON(const JSONValue& root);
    /// Save to a JSON object.
    JSONValue SaveJSON() const;
};

}
```

#### File 3: `Source/Urho3D/Scene/DrivenKey.cpp`

Key implementation details:

```cpp
float DrivenKey::Evaluate(float driverValue) const
{
    if (points.Empty()) return 0.0f;
    if (points.Size() == 1) return points[0].out;

    // Pre-infinity
    if (driverValue <= points[0].in)
    {
        if (preInfinity == INFINITY_EXTRAPOLATE && points.Size() >= 2)
        {
            float slope = (points[1].out - points[0].out) / (points[1].in - points[0].in);
            return points[0].out + slope * (driverValue - points[0].in);
        }
        return points[0].out;
    }

    // Post-infinity
    if (driverValue >= points.Back().in)
    {
        if (postInfinity == INFINITY_EXTRAPOLATE && points.Size() >= 2)
        {
            unsigned n = points.Size();
            float slope = (points[n-1].out - points[n-2].out) / (points[n-1].in - points[n-2].in);
            return points[n-1].out + slope * (driverValue - points[n-1].in);
        }
        return points.Back().out;
    }

    // Binary search for segment
    unsigned lo = 0, hi = points.Size() - 1;
    while (hi - lo > 1)
    {
        unsigned mid = (lo + hi) / 2;
        if (points[mid].in <= driverValue) lo = mid; else hi = mid;
    }

    float t = (driverValue - points[lo].in) / (points[hi].in - points[lo].in);

    switch (mode)
    {
    case INTERP_STEP:
        return points[lo].out;
    case INTERP_CONSTANT:
        return points[0].out;
    case INTERP_SMOOTH:
    {
        // Catmull-Rom or Hermite — use tangents if nonzero, else auto-compute
        float p0 = points[lo].out, p1 = points[hi].out;
        float m0 = points[lo].tangentOut, m1 = points[hi].tangentIn;
        // Hermite basis
        float t2 = t * t, t3 = t2 * t;
        return (2*t3 - 3*t2 + 1)*p0 + (t3 - 2*t2 + t)*m0
             + (-2*t3 + 3*t2)*p1 + (t3 - t2)*m1;
    }
    case INTERP_LINEAR:
    default:
        return Lerp(points[lo].out, points[hi].out, t);
    }
}
```

JSON load/save: Use `#include "../Resource/JSONValue.h"`. Read `"mode"` as string → enum map. Points array: `[{"in": 0.0, "out": 300.0}, ...]`. See existing pattern in `Animation.cpp:203-247`.

#### File 4: `Source/Urho3D/Scene/DrivenKeySystem.h`

```cpp
#pragma once
#include "../Core/Object.h"
#include "DrivenKey.h"

namespace Urho3D
{

class URHO3D_API DrivenKeySystem : public Object
{
    URHO3D_OBJECT(DrivenKeySystem, Object);

public:
    explicit DrivenKeySystem(Context* context);

    /// Add a set of driven keys. Replaces if name matches.
    void AddSet(const DrivenKeySet& set);
    /// Remove a set by name.
    void RemoveSet(const StringHash& name);
    /// Load a set from a JSON resource path (e.g. "DrivenKeys/weather_atmosphere.json").
    bool LoadSet(const String& resourcePath);

    /// Push a driver value. Call each frame for active drivers.
    void SetDriver(const StringHash& param, float value);
    /// Read current driver value.
    float GetDriver(const StringHash& param) const;

    /// Evaluate all curves and fire E_DRIVENKEY_OUTPUT for changed values.
    /// Call once per frame after all drivers are set.
    void Update();

private:
    HashMap<StringHash, DrivenKeySet> sets_;
    HashMap<StringHash, float> drivers_;
};

}
```

#### File 5: `Source/Urho3D/Scene/DrivenKeySystem.cpp`

Registration pattern (in Engine.cpp or Sample 60's Start()):
```cpp
context_->RegisterSubsystem(new DrivenKeySystem(context_));
```

Update loop:
```cpp
void DrivenKeySystem::Update()
{
    for (auto& setPair : sets_)
    {
        DrivenKeySet& set = setPair.second_;
        for (DrivenKey& key : set.keys)
        {
            if (!key.enabled) continue;

            auto it = drivers_.Find(key.driverParam);
            if (it == drivers_.End()) continue;

            float driverVal = it->second_;
            float newVal = key.Evaluate(driverVal);

            if (Abs(newVal - key.lastOutput) > key.deadBand)
            {
                key.lastOutput = newVal;

                using namespace DrivenKeyOutput;
                VariantMap& eventData = GetEventDataMap();
                eventData[P_PARAM] = key.drivenParam;
                eventData[P_VALUE] = newVal;
                eventData[P_DRIVER] = key.driverParam;
                eventData[P_DRIVERVALUE] = driverVal;
                SendEvent(E_DRIVENKEY_OUTPUT, eventData);
            }
        }
    }
}
```

#### Test JSON: `bin/Data/DrivenKeys/test_curves.json`

```json
{
    "name": "Test Curves",
    "keys": [
        {
            "name": "Linear_0_to_1",
            "driver": "TestDriver",
            "driven": "TestLinear",
            "mode": "linear",
            "points": [
                {"in": 0.0, "out": 0.0},
                {"in": 1.0, "out": 100.0}
            ]
        },
        {
            "name": "Step_Trigger",
            "driver": "TestDriver",
            "driven": "TestStep",
            "mode": "step",
            "points": [
                {"in": 0.0, "out": 0.0},
                {"in": 0.5, "out": 1.0},
                {"in": 0.51, "out": 0.0}
            ]
        }
    ]
}
```

#### Phase 1 Acceptance Criteria

1. `DrivenKeySystem` registers as subsystem
2. `LoadSet("DrivenKeys/test_curves.json")` succeeds
3. `SetDriver("TestDriver", 0.5f)` + `Update()` fires `E_DRIVENKEY_OUTPUT` with `TestLinear=50.0`
4. Step curve fires output=1.0 at driver=0.5, output=0.0 at driver=0.6
5. Dead-band works: setting same driver value twice fires event only once
6. All 55+ samples still compile (no header pollution)

**Scope:** ~500 lines across 5 files. Pure data + math + events. No UI, no integration.

### Phase 2: Wire to Existing Systems

1. Push driver values from TerrainNode each frame (weather, sun, season, etc.)
2. Subscribe handlers in TerrainNode for zone/material/renderpath outputs
3. Create JSON curve files from catalog (`Planner_NOTE_PARAMETER_CATALOG.md`):
   - `weather_atmosphere.json` — rain/cloud → fog, ambient, sun, god rays
   - `time_of_day.json` — sun altitude → colors, skybox tint, night factor, height fog
   - `seasons.json` — season angle → terrain/grass/water tints, fog distance, skybox blend
   - `moon.json` — moon altitude → moon rays
   - `underwater.json` — camera depth → underwater tint
4. A/B test: `useDrivenKeys_` flag, compare visual output with hardcoded logic
5. Once curves match, delete the hardcoded lines (~600 lines of C++)
6. **Tweak curves without recompiling** — edit JSON, restart, see results

### Phase 3: Decoupled Subscribers

1. Move subscriber handlers OUT of TerrainNode into dedicated components
2. `AtmosphereController` — subscribes to zone-related driven outputs
3. `WeatherEffects` — subscribes to particle/wind/lightning outputs
4. `SeasonalTinter` — subscribes to material tint outputs
5. Each component is self-contained — knows only about its own targets
6. TerrainNode becomes a pure driver-pusher, not an output-applier

### Phase 4: Animation Integration

1. Register AnimationState time as a driver
2. Text keys evaluate as step-function driven keys
3. Extend ModelViewer text key editor to show curve view
4. Verify: existing text key behavior preserved, new curve keys work alongside

### Phase 5: Curve Editor UI

1. Canvas widget in ModelViewer — draw curve from points
2. Click/drag point manipulation
3. Interpolation mode selector
4. JSON save/load
5. Live preview: scrub driver value, see driven value update in real time

### Phase 6: Generalization

1. Any LogicComponent can own a DrivenKeySet
2. Components subscribe to E_DRIVENKEY_OUTPUT and filter by parameter name
3. Driven keys become part of scene serialization (node attribute or resource)
4. Multiple objects share same curve data (resource, not per-instance)
5. Network forwarding: AuthServer can broadcast driven key events to clients

---

## Files To Create

| File | Purpose |
|------|---------|
| `Source/Urho3D/Scene/DrivenKey.h` | DrivenKey, CurvePoint, DrivenKeySet structs |
| `Source/Urho3D/Scene/DrivenKey.cpp` | Evaluate, JSON load/save, interpolation |
| `Source/Urho3D/Scene/DrivenKeySystem.h` | System header — SetDriver, Update, event dispatch |
| `Source/Urho3D/Scene/DrivenKeySystem.cpp` | System implementation |
| `Source/Urho3D/Scene/DrivenKeyEvents.h` | E_DRIVENKEY_OUTPUT event definition |

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
