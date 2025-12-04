# Phase 1 Implementation Plan - Step by Step

## Overview
Implementing 5 critical optimizations in Phase 1:
1. **1.2 WorkQueue** (Lock-free + CPU affinity) - FOUNDATION
2. **1.3 Particles** (SOA + SIMD) - HIGH IMPACT
3. **1.1 Culling** (SIMD + Parallelization) - HIGHEST SPEEDUP
4. **1.4 Audio** (SIMD mixing) - EASY WIN
5. **1.5 Batch Sort** (Parallel merge-sort) - DEPENDS ON 1.2

**Expected Total Effort:** 132 hours (3 weeks full-time, 6-8 weeks part-time)

---

## 1.2: WorkQueue Lock-Free Redesign

### Architecture Design
```
Current (Mutex-based):
┌─────────────────────────────┐
│   Central Priority Queue    │
│  (Protected by Mutex)       │
└────────┬────────┬───────────┘
         ↓        ↓
    Worker 1  Worker 2  ...

New (Work-Stealing):
┌──────────────────┐      ┌──────────────────┐
│  Worker 1 Deque  │      │  Worker 2 Deque  │
│  (Lock-Free)     │ ←──→ │  (Lock-Free)     │
└──────────────────┘      └──────────────────┘
         ↑
┌──────────────────────────┐
│ Global Priority Queue    │ (for new items)
│ (Lock-free MPSC)         │
└──────────────────────────┘
```

### Implementation Steps
1. Create `WorkStealingDeque` class (lock-free Chase-Lev algorithm)
2. Add per-thread deques to WorkQueue
3. Modify AddWorkItem to push to global queue
4. Modify ProcessItems to steal from neighbors
5. Add CPU affinity setup in CreateThreads
6. Add NUMA detection (optional first step)

### Key Data Structures
```cpp
// New: Per-thread work-stealing deque
struct WorkStealingDeque {
    std::atomic<uint64_t> head{0};      // Pop position
    std::atomic<uint64_t> tail{0};      // Push position
    Vector<WorkItem*> buffer;            // Task array

    void Push(WorkItem* item);           // Owner thread only
    WorkItem* Pop();                     // Owner thread only
    WorkItem* Steal();                   // Other threads
};

// Modified WorkQueue member
Vector<WorkStealingDeque> workerDeques_;  // Per-thread
```

### Performance Targets
- Mutex wait time: <1% on 8-core, <3% on 16-core (from 15-20%)
- Work distribution variance: <5% between threads
- Steal success rate: >95%

---

## 1.3: Particles SOA Conversion

### Current Structure (AoS - Poor Cache)
```cpp
struct Particle {
    Vector3 position;       // 12 bytes
    Vector3 velocity;       // 12 bytes
    Color color;            // 4 bytes
    float lifetime;         // 4 bytes
    float size;             // 4 bytes
    // Total: 36 bytes per particle
    // Cache line: 64 bytes = ~1.7 particles per line
};
Vector<Particle> particles;  // Scattered memory access
```

### New Structure (SOA - Cache Friendly)
```cpp
struct ParticleBuffer {
    Vector<Vector3> positions;       // Contiguous 12*N bytes
    Vector<Vector3> velocities;      // Contiguous 12*N bytes
    Vector<Color> colors;            // Contiguous 4*N bytes
    Vector<float> lifetimes;         // Contiguous 4*N bytes
    Vector<float> sizes;             // Contiguous 4*N bytes
    uint32_t count;
};
```

### Implementation Steps
1. Create ParticleBuffer class with SOA layout
2. Migrate ParticleEmitter to use ParticleBuffer
3. Implement SIMD Update functions (4 particles at once)
4. Convert lifetime/size updates to vectorized form
5. Add SIMD emission (batch emit 4 particles)
6. Validate correctness vs original

### SIMD Update Template
```cpp
void UpdateParticlesSIMD(ParticleBuffer& buffer, float dt) {
    __m128 dtime = _mm_set_ps1(dt);

    for (uint32_t i = 0; i < buffer.count; i += 4) {
        // Load
        __m128 vx = _mm_loadu_ps(&buffer.velocities[i].x);
        __m128 px = _mm_loadu_ps(&buffer.positions[i].x);

        // Compute: p += v * dt
        __m128 newPx = _mm_add_ps(px, _mm_mul_ps(vx, dtime));

        // Store
        _mm_storeu_ps(&buffer.positions[i].x, newPx);
    }
}
```

### Performance Targets
- Cache line utilization: 60% → 95%
- L1 cache misses: -80%
- Update time: 8ms → 1ms (1000 particles)

---

## 1.1: Scene Culling SIMD

### Current Implementation (Scalar Frustum)
```cpp
// O(8) scalar FP operations per object
bool Frustum::IsInside(const BoundingBox& box) {
    for (int i = 0; i < 6; ++i) {
        if (planes[i].Distance(box.center) < -box.halfSize.Length())
            return false;
    }
    return true;
}
```

### SIMD Optimization (Batch 8 Objects)
```cpp
// O(1) operation for 8 objects with SIMD
__m256 FrustumIsInsideSIMD_x8(const Frustum& f,
                              const BoundingBox* boxes) {
    // Process 8 objects simultaneously
    // Result: 8 boolean values packed in __m256
}
```

### Implementation Steps
1. Create SIMD frustum plane representation
2. Implement `IsInside_SIMD(8 objects)` function
3. Parallelize octree traversal with WorkQueue
4. Update View culling to use SIMD batch tests
5. Align octree nodes to cache lines (64 bytes)
6. Profile and optimize memory layout

### Performance Targets
- Culling time: 15ms → 2-3ms (85% reduction)
- L1 cache misses: -60%
- Scalability: Linear to 8 cores

---

## 1.4: Audio SIMD Mixing

### Current Implementation (Scalar)
```cpp
void Mix(float* dest, const float* src, int samples, float volume) {
    for (int i = 0; i < samples; ++i) {
        dest[i] += src[i] * volume;  // 1 multiply + 1 add per sample
    }
}
```

### SIMD Implementation (4 samples at once)
```cpp
void MixSIMD(float* dest, const float* src, int samples, float volume) {
    __m128 vol = _mm_set_ps1(volume);

    for (int i = 0; i < samples; i += 4) {
        __m128 srcVec = _mm_loadu_ps(&src[i]);
        __m128 destVec = _mm_loadu_ps(&dest[i]);
        __m128 result = _mm_add_ps(destVec, _mm_mul_ps(srcVec, vol));
        _mm_storeu_ps(&dest[i], result);
    }
}
```

### Implementation Steps
1. Add SIMD detect (cpuid for SSE support)
2. Implement Mix_SIMD function
3. Add fallback to scalar if no SIMD
4. Parallelize multi-source mixing via WorkQueue
5. Add lock-free audio buffer queue
6. Validate bit-level output matches original

### Performance Targets
- Mix time per source: 0.5ms → 0.1ms
- Simultaneous sources: 16 → 64
- Audio latency: <2ms maintained

---

## 1.5: Batch Rendering Sort

### Current Implementation (Single-threaded)
```cpp
std::sort(batches.begin(), batches.end(), BatchComparator);  // O(n log n)
```

### Parallel Implementation
```cpp
// Divide into chunks, sort in parallel, merge
// 4-core: n log(n/4) + 4n = 2n log(n) + 4n faster for large n
```

### Implementation Steps
1. Add batch structure cache alignment (64-byte lines)
2. Implement parallel merge-sort framework
3. Create SortTask for WorkQueue
4. Divide batches into per-thread chunks
5. Implement merge phase (sequential but fast)
6. Test scalability on 4, 8, 16 core systems

### Cache-Aligned Batch Structure
```cpp
struct alignas(64) Batch {
    Material* material;           // 8 bytes
    Geometry* geometry;           // 8 bytes
    uint32_t renderOrder;         // 4 bytes
    uint32_t type;               // 4 bytes
    // Total: 24 bytes + 40 bytes padding = 64 bytes
};
```

### Performance Targets
- Sort time: 2ms → 0.8ms (60% reduction)
- Cache misses: -50%
- Linear scalability to 8 cores

---

## Build & Testing Strategy

### Phase 1A: WorkQueue (Days 1-2)
```bash
# Build
cmake --build build -j4

# Test
./bin/RunTests
perf stat ./bin/Sample01_HelloWorld

# Verify: No deadlocks, correct task completion
```

### Phase 1B: Particles (Days 3-4)
```bash
# Build particle sample
./bin/Sample36_Particles

# Profile
perf record -g ./bin/Sample36_Particles
perf report

# Measure: 1000 particles should reach 60 FPS easily
```

### Phase 1C: Culling (Days 5-6)
```bash
# Build complex scene
./bin/Sample11_Physics  # Octree-heavy scene

# Profile culling
perf record -g ./bin/Sample11_Physics
grep -i "IsInside" perf.report

# Measure: Culling should be <3ms
```

### Phase 1D: Audio (Days 7)
```bash
# Build audio sample
./bin/Sample34_DynamicGeometry (or audio-heavy scene)

# Profile audio mixing
# Measure: 20+ simultaneous sounds at 60 FPS
```

### Phase 1E: Batch Sort (Days 8)
```bash
# Large scene with many batches
./bin/Sample13_UISceneUI (many UI elements) or custom

# Profile batch collection
# Measure: Sort time <1ms
```

### Integration Testing
```bash
# Build all 56 samples
cmake --build build -j4

# Run profile check
./bin/56_DeferredRendering

# Verify FPS improvement: 60 FPS → 120-150 FPS
```

---

## Risk Mitigation

### WorkQueue Risks
- **Risk:** Lock-free algorithm complexity
- **Mitigation:** Extensive unit tests, stress tests with high contention
- **Fallback:** Keep old mutex-based code, switch at runtime

### Particles Risks
- **Risk:** Physics correctness with SIMD
- **Mitigation:** Compare numerical output vs original
- **Fallback:** Keep AoS as fallback, detect issues

### Culling Risks
- **Risk:** Frustum test accuracy
- **Mitigation:** Unit test all frustum configurations
- **Fallback:** Bitwise compare results with scalar

### Audio Risks
- **Risk:** Audio quality degradation
- **Mitigation:** Bit-level output comparison
- **Fallback:** Scalar fallback if audio clipping detected

### Batch Sort Risks
- **Risk:** Incorrect sort order affecting rendering
- **Mitigation:** Verify sort output matches scalar
- **Fallback:** Use scalar sort, profile contention

---

## Commit Strategy

Each optimization gets its own commit:
```
2aa22e2 Phase 1.2: WorkQueue Lock-Free Work-Stealing
2bb33e3 Phase 1.3: Particles SOA + SIMD Update
2cc44e4 Phase 1.1: Scene Culling SIMD Batch Tests
2dd55e5 Phase 1.4: Audio SIMD Mixing
2ee66e6 Phase 1.5: Batch Sort Parallel Merge
2ff77e7 Phase 1: All optimizations - Final validation
```

---

## Success Metrics

### Individual Optimization Targets
- **1.2 WorkQueue:** <1% lock contention
- **1.3 Particles:** 3-4x speedup, 4000 particles @60FPS
- **1.1 Culling:** 85% faster, linear scaling
- **1.4 Audio:** 20+ simultaneous sounds
- **1.5 Batch Sort:** 60% faster, <1ms time

### Combined (Phase 1 Complete)
- FPS: 60 → 120-150 FPS (2-2.5x)
- CPU utilization: 80% → 40-50%
- Memory bandwidth: Reduced by 30-40%
- Lock contention: Eliminated

### Timeline
- **Week 1:** 1.2 (WorkQueue) + 1.3 (Particles)
- **Week 2:** 1.1 (Culling) + 1.4 (Audio) + 1.5 (Batch Sort)
- **Week 3:** Integration, testing, profiling, documentation

---

## Next Steps

Starting with **1.2: WorkQueue Lock-Free Implementation**

Files to create/modify:
- `Core/WorkStealingDeque.h` (NEW)
- `Core/WorkStealingDeque.cpp` (NEW)
- `Core/WorkQueue.h` (MODIFY)
- `Core/WorkQueue.cpp` (MODIFY)
- `Core/Thread.h` (MODIFY - add CPU affinity)
- `Core/Thread.cpp` (MODIFY - add Linux support)

Let's begin! 🚀
