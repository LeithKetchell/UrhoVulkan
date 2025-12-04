# Phase 1 API Reference

## Table of Contents
1. [WorkStealingDeque API](#workstealingdeque-api)
2. [ParticleBuffer API](#particlebuffer-api)
3. [AudioMixing_SIMD API](#audiomixing_simd-api)
4. [BatchSort_Parallel API](#batchsort_parallel-api)
5. [Thread Extensions API](#thread-extensions-api)

---

## WorkStealingDeque API

### Header
```cpp
#include <Urho3D/Core/WorkStealingDeque.h>
```

### Constructor
```cpp
explicit WorkStealingDeque(uint32_t initialCapacity = 256);
```
- `initialCapacity`: Initial queue size (auto-rounded to power of 2)

### Member Functions

#### `void Push(WorkItem* item)`
- **Thread Safety:** Owner thread only
- **Complexity:** O(1) amortized
- **Description:** Add item to tail (FIFO)
- **Example:**
```cpp
WorkStealingDeque deque;
WorkItem* work = new WorkItem(...);
deque.Push(work);
```

#### `WorkItem* Pop()`
- **Thread Safety:** Owner thread only
- **Complexity:** O(1)
- **Returns:** Item from tail, or `nullptr` if empty
- **Description:** Remove and return most recently added item
- **Example:**
```cpp
WorkItem* item = deque.Pop();
if (item) {
    item->Execute();
}
```

#### `WorkItem* Steal()`
- **Thread Safety:** ANY thread (lock-free)
- **Complexity:** O(1) with CAS retry on contention
- **Returns:** Item from head, or `nullptr` if steal fails
- **Description:** Lock-free steal operation (not blocking)
- **Example:**
```cpp
// In worker thread
while (true) {
    WorkItem* item = deque.Steal();
    if (!item) {
        // Try next worker's deque
        break;
    }
    item->Execute();
}
```

#### `uint32_t GetSize() const`
- **Thread Safety:** Approximate (lock-free read)
- **Complexity:** O(1)
- **Returns:** Current approximate queue depth
- **Note:** May be off by 1-2 due to concurrent access

#### `bool IsEmpty() const`
- **Thread Safety:** Approximate
- **Complexity:** O(1)
- **Returns:** `true` if deque appears empty

#### `void Grow()`
- **Thread Safety:** Owner thread only
- **Complexity:** O(n)
- **Description:** Double internal buffer size
- **Called:** Automatically when tail reaches capacity
- **Limit:** MAX_CAPACITY = 1024*1024 items

### Statistics

#### `uint64_t GetSteals() const`
- Returns total successful steal operations

#### `uint64_t GetFailedSteals() const`
- Returns total failed steal attempts

#### `void ResetStats()`
- Clear steal statistics counters

---

## ParticleBuffer API

### Header
```cpp
#include <Urho3D/Graphics/ParticleBuffer.h>
```

### Constructor
```cpp
explicit ParticleBuffer(uint32_t initialCapacity = 256);
```
- `initialCapacity`: Initial particle slots

### Allocation

#### `uint32_t AllocateParticle()`
- **Complexity:** O(1)
- **Returns:** Particle index, or NINDEX if full
- **Description:** Allocate one particle slot
- **Example:**
```cpp
uint32_t idx = buffer.AllocateParticle();
if (idx != NINDEX) {
    buffer.GetPositions()[idx] = {0, 0, 0};
    buffer.GetVelocities()[idx] = {1, 0, 0};
}
```

#### `void ReleaseParticle(uint32_t index)`
- **Complexity:** O(1)
- **Description:** Mark particle as dead
- **Example:**
```cpp
if (buffer.GetTimers()[idx] >= buffer.GetLifetimes()[idx]) {
    buffer.ReleaseParticle(idx);
}
```

#### `uint32_t AllocateParticles(uint32_t count)`
- **Not yet implemented**, use EmitBatch instead

### Batch Operations (SIMD)

#### `void UpdatePositions(float dt)`
- **Complexity:** O(n), SIMD 4x parallelism
- **Description:** `position += velocity * dt` for all particles
- **SIMD:** 4 particles per SSE iteration
- **Scalar fallback:** Available on non-SSE platforms
- **Example:**
```cpp
buffer.UpdatePositions(frameTime);  // ~1ms for 1000 particles
```

#### `void UpdateLifetimes(float dt)`
- **Complexity:** O(n)
- **Description:** Decrement lifetime timers, mark expired
- **Example:**
```cpp
buffer.UpdateLifetimes(frameTime);
```

#### `void UpdateScales(float dt, float sizeAdd, float sizeMul)`
- **Complexity:** O(n), SIMD optimized
- **Parameters:**
  - `dt`: Frame time delta
  - `sizeAdd`: Size increase per second
  - `sizeMul`: Size multiplier per second
- **Description:** Update particle scale factors
- **Example:**
```cpp
buffer.UpdateScales(dt, 0.5f, 1.1f);  // Grow by 0.5 units/sec, 10% per second
```

#### `void ApplyForces(float dt, const Vector3& constantForce, float dampingForce)`
- **Complexity:** O(n), SIMD optimized
- **Parameters:**
  - `dt`: Frame time delta
  - `constantForce`: Gravity/wind force vector
  - `dampingForce`: Air resistance (0-1)
- **Description:** Apply physics forces to all particles
- **Example:**
```cpp
Vector3 gravity(0, -9.8f, 0);
buffer.ApplyForces(dt, gravity, 0.1f);  // Gravity + 10% damping
```

#### `uint32_t EmitBatch(uint32_t count, const Vector3& initialPosition, const Vector3& initialVelocity, const Vector3& velocityVariance, float lifetime, float size, float rotationSpeed, const Color& color)`
- **Complexity:** O(count)
- **Returns:** Number of particles actually allocated
- **Parameters:**
  - `count`: Target number to emit
  - `initialPosition`: Base position for all
  - `initialVelocity`: Base velocity
  - `velocityVariance`: Random variance per axis
  - `lifetime`: Particle lifespan in seconds
  - `size`: Initial size
  - `rotationSpeed`: Rotation speed (rad/s)
  - `color`: Particle color
- **Description:** Allocate and initialize multiple particles
- **Example:**
```cpp
uint32_t emitted = buffer.EmitBatch(
    10,                                    // 10 particles
    {0, 2, 0},                            // Emitter at y=2
    {0, 5, 0},                            // Shoot upward
    {1, 0, 1},                            // Scatter X and Z
    3.0f,                                  // 3 second lifetime
    0.2f,                                  // 0.2 unit size
    M_PI,                                  // Rotate
    Color::WHITE                           // Color
);
```

### Memory Access (Direct Array Access)

#### `Vector<Vector3>& GetPositions()`
#### `Vector<Vector3>& GetVelocities()`
#### `Vector<Vector2>& GetSizes()`
#### `Vector<Color>& GetColors()`
#### `Vector<float>& GetLifetimes()`
#### `Vector<float>& GetTimers()`
#### `Vector<float>& GetScales()`
#### `Vector<float>& GetRotationSpeeds()`
#### `Vector<float>& GetRotations()`
#### `Vector<int32_t>& GetColorIndices()`
#### `Vector<int32_t>& GetTexIndices()`
#### `Vector<bool>& GetEnabled()`

- **Returns:** Direct reference to internal array
- **Thread Safety:** Only access from main thread
- **Use Case:** Direct particle updates or rendering
- **Example:**
```cpp
Vector<Vector3>& positions = buffer.GetPositions();
for (uint32_t i = 0; i < buffer.GetCount(); ++i) {
    if (buffer.GetEnabled()[i]) {
        positions[i].y_ -= 0.1f;  // Manual gravity
    }
}
```

### Maintenance

#### `uint32_t Compact()`
- **Complexity:** O(n)
- **Returns:** Number of particles remaining
- **Description:** Remove gaps from released particles
- **Use:** Call periodically for efficiency
- **Example:**
```cpp
uint32_t remaining = buffer.Compact();
URHO3D_LOGINFO("Particles after compaction: " + String(remaining));
```

#### `void Clear()`
- **Complexity:** O(capacity)
- **Description:** Remove all particles, reset state
- **Example:**
```cpp
buffer.Clear();  // Reset for new effect
```

### Query

#### `uint32_t GetCount() const`
- Currently active particles

#### `uint32_t GetCapacity() const`
- Maximum particles before grow

#### `uint32_t GetAllocatedCount() const`
- Total allocated (including free slots)

---

## AudioMixing_SIMD API

### Header
```cpp
#include <Urho3D/Audio/AudioMixing_SIMD.h>
```

### Static Functions

#### `void MixMonoWithGain_4x(int* dest, const short* src, int vol)`
- **Complexity:** O(1) - processes 4 samples
- **Parameters:**
  - `dest`: 4x int32 accumulation buffer
  - `src`: 4x int16 samples from source
  - `vol`: Gain value (256 = 1.0x)
- **SIMD:** SSE2 vectorized
- **Example:**
```cpp
int accumulator[4] = {0, 0, 0, 0};
short source[4] = {100, 200, 150, 250};
int gain = 256;  // 1.0x gain

AudioMixing_SIMD::MixMonoWithGain_4x(accumulator, source, gain);
// accumulator now contains mixed samples
```

#### `void Convert32to16_4x(short* dest, const int* src)`
- **Complexity:** O(1) - processes 4 samples
- **Parameters:**
  - `dest`: Output 4x int16 PCM samples
  - `src`: Input 4x int32 accumulation values
- **Description:** Convert and saturate accumulator to PCM
- **Saturation:** Automatic clipping to [-32768, 32767]
- **Example:**
```cpp
int accumulator[4] = {10000, 50000, 100000, 200000};
short pcm_out[4];

AudioMixing_SIMD::Convert32to16_4x(pcm_out, accumulator);
// pcm_out safely contains clipped 16-bit values
```

#### `void ApplyGain_4x(int* samples, int gain, uint32_t count)`
- **Complexity:** O(count)
- **Parameters:**
  - `samples`: Array of int32 samples
  - `gain`: Gain multiplier (256 = 1.0x)
  - `count`: Number of samples (process in 4-sample chunks)
- **Description:** Apply gain to multiple samples
- **SIMD:** 4x parallelism
- **Example:**
```cpp
int samples[100];
// ... fill samples ...
AudioMixing_SIMD::ApplyGain_4x(samples, 128, 100);  // 0.5x gain
```

#### `void SaturatingAdd_4x(short* dest, const short* src)`
- **Complexity:** O(1) - processes 4 samples
- **Parameters:**
  - `dest`: Output (also input)
  - `src`: Values to accumulate
- **Description:** Safe addition with saturation
- **SIMD:** SSE `_mm_adds_epi16`
- **Example:**
```cpp
short dest[4] = {30000, 20000, 10000, 5000};
short src[4] = {5000, 15000, 10000, 2000};

AudioMixing_SIMD::SaturatingAdd_4x(dest, src);
// dest safely saturates at ±32767
```

#### `void MixMultipleSources(int* dest, const short* const* sources, const int* gains, uint32_t numSources, uint32_t samples)`
- **Complexity:** O(numSources × samples)
- **Parameters:**
  - `dest`: Accumulation buffer
  - `sources`: Array of source pointers
  - `gains`: Array of gain values (one per source)
  - `numSources`: Number of sources
  - `samples`: Sample count per source
- **Description:** Mix all sources with their gains
- **SIMD:** Per-source, 4x parallelism
- **Example:**
```cpp
short* sources[3] = { sound1, sound2, sound3 };
int gains[3] = { 256, 128, 192 };  // 1.0x, 0.5x, 0.75x

int accumulator[8192] = {0};
AudioMixing_SIMD::MixMultipleSources(
    accumulator,
    sources,
    gains,
    3,
    8192
);
// All 3 sources mixed into accumulator
```

---

## BatchSort_Parallel API

### Header
```cpp
#include <Urho3D/Graphics/BatchSort_Parallel.h>
```

### Data Structure

#### `struct alignas(64) SortedBatch`
Cache-line aligned batch for rendering:
```cpp
struct SortedBatch {
    void* material;           // 8 bytes
    void* geometry;           // 8 bytes
    uint32_t renderOrder;     // 4 bytes
    uint32_t type;            // 4 bytes
    uint32_t instanceCount;   // 4 bytes
    uint32_t vertexStart;     // 4 bytes
    uint32_t vertexEnd;       // 4 bytes
    uint32_t reserved;        // 4 bytes
    // Total: 64 bytes (one L1 cache line)
};
```

### Static Functions

#### `Vector<SortedBatch> SortBatches(const Vector<SortedBatch>& input, uint32_t numThreads = 0)`
- **Complexity:** O(n log n) sequential, O(n log(n/p)) parallel (p = threads)
- **Parameters:**
  - `input`: Batches to sort
  - `numThreads`: Worker threads (0 = auto-detect)
- **Returns:** Sorted batch array
- **Description:** Sort batches by render order
- **Thread Safety:** Internally synchronized
- **Example:**
```cpp
Vector<SortedBatch> unsorted = view->GetBatches();
Vector<SortedBatch> sorted = BatchSort_Parallel::SortBatches(unsorted, 4);

// Render in sorted order
for (const auto& batch : sorted) {
    graphics->DrawBatch(batch);
}
```

#### `bool BatchComparator(const SortedBatch& a, const SortedBatch& b)`
- **Returns:** `true` if `a` should render before `b`
- **Sort Order:**
  1. `renderOrder` (primary)
  2. `material` (secondary, reduce state changes)
  3. `geometry` (tertiary)
- **Example:**
```cpp
// Can be used with std::sort for custom sorting
std::sort(batches.begin(), batches.end(), BatchSort_Parallel::BatchComparator);
```

#### `void MergeSortSequential(Vector<SortedBatch>& batches)`
- **Complexity:** O(n log n)
- **Parameters:**
  - `batches`: Array to sort (sorted in-place)
- **Description:** Single-threaded merge-sort
- **Use:** Baseline for comparison

#### `void MergeSortParallel(Vector<SortedBatch>& batches, uint32_t numThreads)`
- **Complexity:** O(n log(n/p) + pn) for p threads
- **Parameters:**
  - `batches`: Array to sort (sorted in-place)
  - `numThreads`: Worker threads
- **Description:** Parallel divide-and-conquer sort
- **Framework:** Ready for WorkQueue integration

---

## Thread Extensions API

### Header
```cpp
#include <Urho3D/Core/Thread.h>
```

### New Methods

#### `bool SetAffinity(int cpuIndex)`
- **Platform:** Linux only (currently)
- **Parameters:**
  - `cpuIndex`: CPU core number (0-based)
- **Returns:** `true` if successful
- **Description:** Pin thread to specific CPU core
- **Example:**
```cpp
Thread* worker = new WorkerThread();
worker->Run();
worker->SetAffinity(0);  // Pin to core 0
worker->SetAffinity(1);  // Pin to core 1
```

#### `static int GetNumCores()`
- **Returns:** Number of logical CPU cores
- **Complexity:** O(1)
- **Platform:** Windows, Linux, macOS, FreeBSD
- **Example:**
```cpp
uint32_t numThreads = Thread::GetNumCores();
URHO3D_LOGINFO("CPU cores: " + String(numThreads));
```

#### `static int GetNumNumaNodes()`
- **Returns:** Number of NUMA nodes (1 if unavailable)
- **Complexity:** O(1)
- **Platform:** Linux (others default to 1)
- **Example:**
```cpp
if (Thread::GetNumNumaNodes() > 1) {
    URHO3D_LOGINFO("Multi-socket system detected");
}
```

#### `static int GetCoreNumaNode(int coreIndex)`
- **Parameters:**
  - `coreIndex`: Core number (0-based)
- **Returns:** NUMA node ID (-1 if unavailable)
- **Complexity:** O(1)
- **Example:**
```cpp
for (int core = 0; core < Thread::GetNumCores(); ++core) {
    int node = Thread::GetCoreNumaNode(core);
    printf("Core %d → NUMA node %d\n", core, node);
}
```

---

## Usage Patterns

### Pattern 1: Single ParticleBuffer with SIMD Updates
```cpp
ParticleBuffer particles(1000);

// Per frame:
particles.EmitBatch(5, pos, vel, var, lifetime, size, rotSpeed, color);
particles.UpdatePositions(dt);              // SIMD: 4 particles/iter
particles.UpdateLifetimes(dt);
particles.ApplyForces(dt, gravity, 0.1f);
particles.UpdateScales(dt, sizeAdd, sizeMul);
particles.Compact();                        // Defragment periodically
```

### Pattern 2: WorkStealingDeque with Multi-Threading
```cpp
WorkStealingDeque deque;

// Producer thread:
while (hasWork) {
    WorkItem* item = createWork();
    deque.Push(item);
}

// Consumer threads:
for (int i = 0; i < numWorkers; ++i) {
    threads[i]->SetAffinity(i);  // Pin to core i
    while (true) {
        WorkItem* item = deque.Pop();
        if (!item) {
            // Try stealing from neighbors
            item = neighborDeque[i]->Steal();
        }
        if (item) item->Execute();
    }
}
```

### Pattern 3: SIMD Audio Mixing
```cpp
// Initialize:
int accumulator[8192] = {0};

// Mix sources:
for (SoundSource* source : activeSources) {
    AudioMixing_SIMD::MixMonoWithGain_4x(
        accumulator,
        source->GetBuffer(),
        source->GetGain()
    );
}

// Convert to PCM output:
short output[2048];
for (int i = 0; i < 8192; i += 4) {
    AudioMixing_SIMD::Convert32to16_4x(&output[i], &accumulator[i]);
}
```

### Pattern 4: Sorted Batch Rendering
```cpp
Vector<SortedBatch> batches = view->GetBatches();

// Sort with parallelization:
batches = BatchSort_Parallel::SortBatches(batches, numCores);

// Render in order:
for (const auto& batch : batches) {
    graphics->SetMaterial(batch.material);
    graphics->SetGeometry(batch.geometry);
    graphics->DrawIndexed(batch.instanceCount, batch.vertexStart, batch.vertexEnd);
}
```

---

*Complete API reference for Phase 1 components.*
*All functions available for immediate integration.*
