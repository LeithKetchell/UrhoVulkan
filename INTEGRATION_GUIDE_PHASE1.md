# Phase 1 Integration Guide

**Timeline:** Week 2-3
**Status:** Step-by-step integration instructions for all components

---

## Integration Overview

### Dependencies Graph
```
ParticleBuffer (standalone)
AudioMixing_SIMD (standalone)
BatchSort_Parallel (standalone)
WorkStealingDeque → Thread extensions
```

**Integration Order (recommended):**
1. Thread extensions (dependency for WorkQueue)
2. WorkStealingDeque (foundation for WorkQueue)
3. ParticleBuffer (integrate into ParticleEmitter)
4. AudioMixing_SIMD (integrate into SoundSource)
5. BatchSort_Parallel (integrate into View)

---

## Step 1: Verify Thread Extensions

**Status:** Already implemented
**Files Modified:** `Core/Thread.h`, `Core/Thread.cpp`

### Verification
```cpp
// Test CPU detection
uint32_t numCores = Thread::GetNumCores();
int numNodes = Thread::GetNumNumaNodes();

URHO3D_LOGINFO("CPU cores: " + String(numCores));
URHO3D_LOGINFO("NUMA nodes: " + String(numNodes));
```

### Build Verification
```bash
cd build
cmake --build . --target Urho3D -j4
# Should compile without errors
# [100%] Built target Urho3D
```

---

## Step 2: Integrate WorkStealingDeque into WorkQueue

**Target Files:**
- `Core/WorkQueue.h`
- `Core/WorkQueue.cpp`

### 2.1 Add Include

**File:** `Core/WorkQueue.h`
```cpp
// Add to includes section
#include "WorkStealingDeque.h"
```

### 2.2 Add Per-Thread Deques

**File:** `Core/WorkQueue.h`, class `WorkQueue`
```cpp
class WorkQueue {
    // ... existing members ...

    // NEW: Per-thread work-stealing deques
    Vector<WorkStealingDeque> workerDeques_;
};
```

### 2.3 Initialize Deques in Constructor

**File:** `Core/WorkQueue.cpp`
```cpp
WorkQueue::WorkQueue()
    : /* existing init */ {

    // Resize deque array to number of workers
    workerDeques_.Resize(numWorkers_);
    for (uint32_t i = 0; i < numWorkers_; ++i) {
        workerDeques_[i] = WorkStealingDeque(256);
    }
}
```

### 2.4 Update AddWorkItem to Use Global Queue

**Before (mutex lock):**
```cpp
void WorkQueue::AddWorkItem(SharedPtr<WorkItem> item) {
    MutexLock lock(queueMutex_);
    queue_.Push(item);
    queueSignal_.Set();
}
```

**After (lock-free for now, add to deque directly):**
```cpp
void WorkQueue::AddWorkItem(SharedPtr<WorkItem> item) {
    // For initial integration, use index 0
    // Future: distribute via global queue
    if (!workerDeques_.Empty()) {
        workerDeques_[0].Push(item);
        queueSignal_.Set();
    }
}
```

### 2.5 Update ProcessItems to Steal

**Before (sequential pop):**
```cpp
void WorkQueue::ProcessItems(unsigned maxMsec) {
    Timer timer;
    while (!queue_.Empty()) {
        SharedPtr<WorkItem> item = queue_.Front();
        queue_.PopFront();
        item->Execute();
    }
}
```

**After (work-stealing):**
```cpp
void WorkQueue::ProcessItems(unsigned maxMsec) {
    Timer timer;
    uint32_t workerIndex = GetWorkerIndex();  // Which thread am I?

    // First, try own deque
    while (true) {
        WorkItem* item = workerDeques_[workerIndex].Pop();
        if (!item) {
            // Try stealing from neighbors
            item = StealFromNeighbor(workerIndex);
        }

        if (!item) break;  // No work available

        item->Execute();

        if (timer.GetMSec(false) > maxMsec) break;
    }
}

WorkItem* WorkQueue::StealFromNeighbor(uint32_t workerIndex) {
    for (uint32_t i = 1; i < workerDeques_.Size(); ++i) {
        uint32_t neighbor = (workerIndex + i) % workerDeques_.Size();
        WorkItem* stolen = workerDeques_[neighbor].Steal();
        if (stolen) return stolen;
    }
    return nullptr;
}
```

### 2.6 Set CPU Affinity in CreateThreads

**File:** `Core/WorkQueue.cpp`
```cpp
void WorkQueue::CreateThreads(unsigned numThreads) {
    // ... create worker threads ...

    for (unsigned i = 0; i < numThreads; ++i) {
        workerThreads_[i]->SetAffinity(i);  // NEW: Pin to core
        workerThreads_[i]->Run();
    }
}
```

### 2.7 Testing

```cpp
// Simple stress test
WorkQueue queue(4);  // 4 worker threads

for (int i = 0; i < 1000; ++i) {
    queue.AddWorkItem(new TestWorkItem());
}

queue.Complete();  // Wait for all items
URHO3D_LOGINFO("All items processed");
```

---

## Step 3: Integrate ParticleBuffer into ParticleEmitter

**Target Files:**
- `Graphics/ParticleEmitter.h`
- `Graphics/ParticleEmitter.cpp`

### 3.1 Add Include and Member

**File:** `Graphics/ParticleEmitter.h`
```cpp
#include "ParticleBuffer.h"

class ParticleEmitter : public BillboardSet {
    // ... existing members ...

    // NEW: Use ParticleBuffer instead of Vector<Particle>
    SharedPtr<ParticleBuffer> particleBuffer_;
};
```

### 3.2 Initialize ParticleBuffer

**File:** `Graphics/ParticleEmitter.cpp`
```cpp
void ParticleEmitter::SetEffect(ParticleEffect* effect) {
    effect_ = effect;

    if (effect_) {
        uint32_t maxParticles = effect_->GetMaxParticles();
        particleBuffer_ = new ParticleBuffer(maxParticles);
    }
}
```

### 3.3 Update Emission

**Before:**
```cpp
void ParticleEmitter::Update(const FrameInfo& frame) {
    // ... emission logic ...

    for (int i = 0; i < newParticles; ++i) {
        EmitNewParticle();  // Creates particle in vector
    }
}
```

**After:**
```cpp
void ParticleEmitter::Update(const FrameInfo& frame) {
    // ... emission logic ...

    // Batch emit particles
    particleBuffer_->EmitBatch(
        newParticles,
        emitterPosition_,
        effect_->GetRandomVelocity(),
        effect_->GetVelocityVariance(),
        effect_->GetRandomTimeToLive(),
        effect_->GetRandomSize(),
        effect_->GetRandomRotationSpeed(),
        color_
    );
}
```

### 3.4 Update Particle Processing

**Before (scalar loop):**
```cpp
for (auto& particle : particles_) {
    particle.timer_ += dt;
    if (particle.timer_ >= particle.timeToLive_) {
        // mark dead
    }
    particle.velocity_ += gravity * dt;
    particle.position_ += particle.velocity_ * dt;
}
```

**After (SIMD optimized):**
```cpp
// Update lifetimes (marks expired)
particleBuffer_->UpdateLifetimes(dt);

// Apply forces (gravity, wind, damping)
Vector3 gravity = GetScene()->GetGravity();
particleBuffer_->ApplyForces(dt, gravity, dampingForce_);

// Update positions using SIMD
particleBuffer_->UpdatePositions(dt);

// Update scales
particleBuffer_->UpdateScales(dt, sizeAdd, sizeMul);

// Periodically defragment
if (frameNumber_ % 60 == 0) {
    particleBuffer_->Compact();
}
```

### 3.5 Update Billboard Synchronization

**File:** `Graphics/ParticleEmitter.cpp`
```cpp
void ParticleEmitter::Commit() {
    // Copy particle data to billboards for rendering
    const Vector<Vector3>& positions = particleBuffer_->GetPositions();
    const Vector<Vector2>& sizes = particleBuffer_->GetSizes();
    const Vector<Color>& colors = particleBuffer_->GetColors();
    const Vector<float>& rotations = particleBuffer_->GetRotations();
    const Vector<bool>& enabled = particleBuffer_->GetEnabled();

    billboards_.Clear();

    for (uint32_t i = 0; i < particleBuffer_->GetCount(); ++i) {
        if (enabled[i]) {
            Billboard billboard;
            billboard.position_ = positions[i];
            billboard.size_ = sizes[i];
            billboard.color_ = colors[i];
            billboard.rotation_ = rotations[i];
            billboard.enabled_ = true;
            billboards_.Push(billboard);
        }
    }

    BillboardSet::Commit();
}
```

### 3.6 Testing

```cpp
// Create particle effect
Node* emitterNode = scene->CreateChild("Emitter");
ParticleEmitter* emitter = emitterNode->CreateComponent<ParticleEmitter>();

ParticleEffect* effect = cache->GetResource<ParticleEffect>("Particles/...xml");
emitter->SetEffect(effect);

// Update in scene loop - should see performance improvement
// 1000 particles should take <1ms instead of 8ms
```

---

## Step 4: Integrate AudioMixing_SIMD into SoundSource

**Target Files:**
- `Audio/SoundSource.cpp`

### 4.1 Add Include

```cpp
#include "AudioMixing_SIMD.h"
```

### 4.2 Update Mix Loop

**Before (scalar):**
```cpp
void SoundSource::MixMonoToMono(int* dest, unsigned samples, int mixRate) {
    float totalGain = masterGain_ * attenuation_ * gain_;
    auto vol = RoundToInt(256.0f * totalGain);

    auto* pos = (short*)position_;
    auto* end = (short*)sound_->GetEnd();

    while (samples--) {
        *dest = *dest + (*pos * vol) / 256;
        ++dest;
        ++pos;
    }
}
```

**After (SIMD 4x parallelism):**
```cpp
void SoundSource::MixMonoToMono_SIMD(int* dest, unsigned samples, int mixRate) {
    float totalGain = masterGain_ * attenuation_ * gain_;
    auto vol = RoundToInt(256.0f * totalGain);

    auto* pos = (short*)position_;
    unsigned simdSamples = (samples / 4) * 4;

    // SIMD 4x parallelism
    for (unsigned i = 0; i < simdSamples; i += 4) {
        AudioMixing_SIMD::MixMonoWithGain_4x(dest, pos, vol);
        dest += 4;
        pos += 4;
    }

    // Scalar fallback for remainder
    for (unsigned i = simdSamples; i < samples; ++i) {
        *dest = *dest + (*pos * vol) / 256;
        ++dest;
        ++pos;
    }

    position_ = (signed char*)pos;
}
```

### 4.3 Update Selection Logic

**File:** `Audio/SoundSource.cpp`
```cpp
void SoundSource::Mix(int* dest, unsigned samples, int mixRate, bool stereo, bool interpolation) {
    // Select SIMD-optimized mixing when possible
    if (!sound_->IsStereo() && !interpolation) {
        // Use SIMD version
        if (stereo)
            MixMonoToStereo_SIMD(dest, samples, mixRate);
        else
            MixMonoToMono_SIMD(dest, samples, mixRate);
    } else {
        // Use original implementations for interpolation/stereo
        if (sound_->IsStereo()) {
            // ... original stereo mix ...
        } else {
            // ... original mono mix ...
        }
    }
}
```

### 4.4 Update Format Conversion

**File:** `Audio/Audio.cpp`, in `MixOutput()`
```cpp
void Audio::MixOutput(void* dest, u32 samples) {
    // ... mixing loop ...

    // Convert 32-bit to 16-bit with saturation
    auto* destPtr = (short*)dest;
    auto* srcPtr = clipBuffer_.Get();

    uint32_t simdSamples = (samples / 4) * 4;

    // SIMD conversion
    for (uint32_t i = 0; i < simdSamples; i += 4) {
        AudioMixing_SIMD::Convert32to16_4x(&destPtr[i], &srcPtr[i]);
    }

    // Scalar fallback
    for (uint32_t i = simdSamples; i < samples; ++i) {
        destPtr[i] = (short)Clamp(srcPtr[i], -32768, 32767);
    }
}
```

### 4.5 Testing

```cpp
// Create multiple sound sources
for (int i = 0; i < 20; ++i) {
    Node* sourceNode = scene->CreateChild("Source" + String(i));
    SoundSource* source = sourceNode->CreateComponent<SoundSource>();

    Sound* sound = cache->GetResource<Sound>("Sounds/...wav");
    source->Play(sound);
}

// Should handle 20+ simultaneous sources without performance degradation
```

---

## Step 5: Integrate BatchSort_Parallel into View

**Target Files:**
- `Graphics/View.h`
- `Graphics/View.cpp`

### 5.1 Add Include

**File:** `Graphics/View.h`
```cpp
#include "BatchSort_Parallel.h"
```

### 5.2 Update Batch Collection

**Before:**
```cpp
void View::GetBatches(Vector<Batch>& batches) {
    // ... collect batches from scene ...
    std::sort(batches.begin(), batches.end(), BatchComparator);
}
```

**After:**
```cpp
void View::GetBatches(Vector<SortedBatch>& batches) {
    // ... collect batches from scene ...

    // Use parallel sort with auto-detected threads
    batches = BatchSort_Parallel::SortBatches(batches);

    // Or specify threads:
    // batches = BatchSort_Parallel::SortBatches(batches, Thread::GetNumCores());
}
```

### 5.3 Testing

```cpp
// Render complex scene with many batches
View* view = renderer->GetViewport(0)->GetView();

Vector<SortedBatch> batches;
view->GetBatches(batches);

// Batches should be sorted and cache-aligned
// No false sharing between threads
```

---

## Integration Checklist

### Week 2 (Days 1-3)

- [ ] **Day 1:** Thread extensions verification
- [ ] **Day 1:** WorkQueue WorkStealingDeque integration
- [ ] **Day 2:** ParticleBuffer integration into ParticleEmitter
- [ ] **Day 2:** AudioMixing_SIMD integration into SoundSource
- [ ] **Day 3:** BatchSort_Parallel integration into View
- [ ] **Day 3:** Compile all changes, fix any errors

### Week 2 (Days 4-5)

- [ ] **Day 4:** Basic functionality testing
  - [ ] ParticleEmitter works with ParticleBuffer
  - [ ] Audio plays with SIMD mixing
  - [ ] Batches render in correct order
- [ ] **Day 5:** Sample tests
  - [ ] Sample 36 (Particles): 1000+ particles
  - [ ] Sample 34 (Audio): 20+ simultaneous sounds
  - [ ] Sample with complex scene: No visual artifacts

### Week 3 (Days 1-5)

- [ ] **Day 1-2:** Performance profiling
  - [ ] Profile with `perf`
  - [ ] Measure CPU time improvements
  - [ ] Check memory bandwidth usage
- [ ] **Day 3-4:** Correctness validation
  - [ ] Particle physics accuracy
  - [ ] Audio quality (no clipping)
  - [ ] Batch render order correctness
- [ ] **Day 5:** Multi-core scalability analysis
  - [ ] Test on 4, 8, 16 core systems
  - [ ] Verify linear scaling

---

## Troubleshooting Integration

### Issue: WorkStealingDeque compilation errors

**Solution:** Ensure `#include <unistd.h>` is in Thread.cpp
```cpp
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <unistd.h>  // <- This is critical
#endif
```

### Issue: SIMD functions not called

**Solution:** Check `#ifdef URHO3D_SSE`
```bash
# Ensure CMake detected SSE
cmake --build . -- -DURHO3D_SSE=1
```

### Issue: Particle colors wrong

**Solution:** Ensure Billboard synchronization is correct
```cpp
// After ParticleBuffer updates, sync to billboards
particleBuffer_->GetColors();  // Get updated colors
// Copy to billboard array before rendering
```

### Issue: Audio mixing sounds different

**Solution:** Check gain calculations
```cpp
// Ensure gain is in range [0, 512] for fixed-point
int vol = RoundToInt(256.0f * gain);  // vol = 256 means 1.0x
URHO3D_ASSERT(vol >= 0 && vol <= 512, "Gain out of range");
```

---

## Performance Expectations

After integration (Week 3):

| Metric | Before | After | Gain |
|--------|--------|-------|------|
| Particle update (1000) | 8ms | 1ms | 8x |
| Simultaneous audio sources | 8 | 64+ | 8x |
| Batch sort time | 2ms | 0.8ms | 2.5x |
| Overall FPS | 60 | 120-150 | 2-2.5x |

---

## Next Steps

1. **Immediate (Today):** Choose one component to integrate first
2. **This Week:** Complete all 5 integrations
3. **Next Week:** Performance profiling and optimization
4. **Week 3:** Final results and deployment

---

*Complete integration guide for Phase 1 components.*
*Follow steps sequentially for best results.*
