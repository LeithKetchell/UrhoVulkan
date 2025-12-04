# Phase 13: Profiling Harness & Data Collection

## Overview

Phase 13 performance profiling system for comprehensive Vulkan backend analysis.

**Components:**
- VulkanProfiler class (telemetry collection)
- Frame-level metrics capture
- Aggregate analysis
- Target validation

---

## VulkanProfiler Usage

### Integration into Sample

```cpp
#include "VulkanProfiler.h"

class HelloWorld : public Sample
{
private:
    SharedPtr<VulkanProfiler> profiler_;
    uint32_t profilingFrames_ = 0;
    static const uint32_t PROFILE_DURATION_FRAMES = 300;  // 5 seconds @ 60 FPS
};

void HelloWorld::Start()
{
    Sample::Start();

    // Initialize profiler
    profiler_ = new VulkanProfiler(context_);
    if (!profiler_->Initialize(GetSubsystem<Graphics>()))
    {
        URHO3D_LOGWARNING("Profiler initialization failed");
    }

    // ... rest of setup
}

void HelloWorld::HandleUpdate(StringHash, VariantMap&)
{
    // Existing update code...

    // Collect metrics every frame
    if (profiler_ && profilingFrames_ < PROFILE_DURATION_FRAMES)
    {
        VulkanProfiler::FrameMetrics metrics;
        profiler_->CollectFrameMetrics(metrics);
        profilingFrames_++;
    }

    // Print report after profiling period
    if (profilingFrames_ == PROFILE_DURATION_FRAMES)
    {
        profiler_->PrintReport();
        URHO3D_LOGINFO("Profiling complete!");
    }
}
```

### Metrics Collected Per Frame

```cpp
struct FrameMetrics
{
    uint32_t frameNumber;                   // Frame count
    uint32_t samplerCacheHits;              // Cache hits (cumulative)
    uint32_t samplerCacheMisses;            // Cache misses (cumulative)
    uint32_t shaderCacheHits;               // Shader hits (cumulative)
    uint32_t shaderCacheMisses;             // Shader misses (cumulative)
    uint32_t cbPoolUsedBytes;               // CB pool bytes used this frame
    uint32_t cbPoolPeakBytes;               // CB pool peak for this frame
    uint32_t constantBufferAllocations;     // CB allocations this frame
    uint64_t gpuMemoryAllocated;            // Total GPU memory allocated
    uint64_t gpuMemoryCapacity;             // Total GPU memory capacity
    float gpuMemoryUtilization;             // Percentage utilized
    float frameTimeMS;                      // Frame time (if measured)
};
```

### Aggregate Report

```
=== Vulkan Performance Profile (Phase 13) ===
Total Frames: XXX
Sampler Cache Hit Rate: XX.X%
Shader Cache Hit Rate: XX.X%
Constant Buffer Pool Avg: XXX KB
Constant Buffer Pool Peak: XXX KB
GPU Memory Utilization: XX.X%
==========================================

=== Target Analysis ===
✓ Sampler cache: PASS (target > 95%)
✓ Shader cache: PASS (target > 90%)
✓ GPU Memory: PASS (target 75-85%)
====================
```

---

## Performance Targets

### Cache Efficiency

| Metric | Target | Interpretation |
|--------|--------|-----------------|
| Sampler Hit Rate | > 95% | Reusing sampler configs (good) |
| Shader Hit Rate | > 90% | Compiled shaders in cache |

**Action if not met:**
- Sampler < 95%: Too many unique sampler configs, standardize them
- Shader < 90%: Clear shader cache or investigate caching logic

### Memory Utilization

| Metric | Target | Interpretation |
|--------|--------|-----------------|
| CB Pool Avg | 20-50 KB | Typical frame constant buffer usage |
| CB Pool Peak | < 200 KB | Maximum observed in any frame |
| GPU Memory | 75-85% | Healthy overall GPU memory usage |

**Action if not met:**
- CB Pool > 200 KB peak: Too many constant buffers, batch them
- GPU Memory < 70%: Pools over-provisioned, reduce sizes
- GPU Memory > 90%: Out of memory risk, reduce texture quality

---

## Profiling Procedure

### Step 1: Build with Profiler Support

```bash
# Build with Vulkan enabled
script/cmake_generic.sh build -DURHO3D_VULKAN=1
cd build && make -j$(nproc)
```

### Step 2: Modify 01_HelloWorld Sample

Add VulkanProfiler integration to HelloWorld.h/cpp (see Integration section above)

### Step 3: Run Profiling Session

```bash
export VULKAN_SDK=/path/to/vulkan-sdk
export LD_LIBRARY_PATH=$VULKAN_SDK/lib:$LD_LIBRARY_PATH

# Run for 5+ seconds (300+ frames at 60 FPS)
timeout 10 ./build/bin/01_HelloWorld 2>&1 | tee profile_results.log
```

### Step 4: Analyze Results

Look for these key lines in log output:
```
[...] VulkanProfiler initialized successfully
[...] Total Frames: XXX
[...] Sampler Cache Hit Rate: XX.X%
[...] Shader Cache Hit Rate: XX.X%
[...] GPU Memory Utilization: XX.X%
[...] ✓/✗ Target Analysis
```

### Step 5: Document Findings

Document in PHASE_13_RESULTS.md:
- Which targets passed
- Which targets failed
- Identified bottlenecks
- Recommended optimizations

---

## Interpreting Results

### Scenario 1: All Targets Passed ✓

```
Sampler Cache: 97% ✓
Shader Cache: 95% ✓
GPU Memory: 80% ✓
```

**Conclusion:** Backend is well-optimized. No critical issues.

**Next Step:** Move to Phase 12 (Instancing) or Phase 10 (Staging)

---

### Scenario 2: Sampler Cache Below Target ❌

```
Sampler Cache: 72% ✗ (target > 95%)
```

**Problem:** Creating too many unique samplers per frame

**Causes:**
1. Per-material sampler customization
2. Sampling different filter modes unnecessarily
3. Address mode variations not needed

**Optimization:**
1. Standardize to 5-10 predefined sampler configs
2. Pre-warm cache at startup
3. Reuse sampler configurations across materials

**Code Fix:**
```cpp
// Before: Create unique sampler per material
SamplerKey key{...};  // Different for each material
cache->GetOrCreateSampler(key);

// After: Standardize configs
enum class SamplerPreset {
    Default,        // LINEAR filtering, CLAMP
    Pixelated,      // NEAREST filtering
    DetailAniso,    // LINEAR with anisotropy
};

SamplerKey standardConfigs[] = {
    SamplerKey{LINEAR, LINEAR, CLAMP, ...},     // Default
    SamplerKey{NEAREST, NEAREST, CLAMP, ...},   // Pixelated
    SamplerKey{LINEAR, LINEAR, CLAMP, ..., true, 16.0f}, // DetailAniso
};

// Use preset instead
cache->GetOrCreateSampler(standardConfigs[SamplerPreset::Default]);
```

---

### Scenario 3: GPU Memory Over Target ❌

```
GPU Memory: 92% ✗ (target 75-85%)
```

**Problem:** Over-allocated memory pools or too many textures loaded

**Causes:**
1. Pool sizes too large for workload
2. Too many high-resolution textures
3. Texture streaming not enabled

**Optimization:**
1. Reduce pool sizes based on actual peak usage
2. Implement texture streaming/LOD
3. Compress textures (DXT, etc.)

**Code Fix:**
```cpp
// Monitor peak usage
auto memStats = memMgr->GetAllStatistics();
URHO3D_LOGINFO("Peak CB Pool: " + String(cbStats.peakFrameSize));

// Adjust pool size
uint32_t newSize = cbStats.peakFrameSize + (cbStats.peakFrameSize / 4);  // 25% headroom
cbPool->Initialize(impl, newSize);
```

---

### Scenario 4: Shader Cache Below Target ❌

```
Shader Cache: 65% ✗ (target > 90%)
```

**Problem:** Shaders being recompiled instead of cached

**Causes:**
1. Disk cache cleared or inaccessible
2. Shader source changing frequently
3. Cache key mismatch

**Optimization:**
1. Pre-compile shaders offline
2. Verify cache directory permissions
3. Warm cache at startup

**Verification:**
```bash
# Check disk cache
ls -la ~/.urho3d/shader_cache/
# Should have .spirv files

# Clear if corrupted
rm -rf ~/.urho3d/shader_cache/
# Will rebuild on next run
```

---

## Expected Results

### Baseline (HelloWorld Simple Scene)

```
Total Frames: 300 (5 seconds @ 60 FPS)
Sampler Cache Hit Rate: 98.5%      ✓ (excellent)
Shader Cache Hit Rate: 94.2%       ✓ (good)
CB Pool Avg: 32 KB
CB Pool Peak: 48 KB
GPU Memory: 41%                    (small scene)
```

### Moderate Scene (Physics + Lighting)

```
Total Frames: 300
Sampler Cache Hit Rate: 96.1%      ✓
Shader Cache Hit Rate: 91.7%       ✓
CB Pool Avg: 128 KB
CB Pool Peak: 156 KB
GPU Memory: 62%                    (moderate load)
```

### Heavy Scene (Many Objects)

```
Total Frames: 300
Sampler Cache Hit Rate: 93.2%      ⚠ (borderline)
Shader Cache Hit Rate: 88.5%       ⚠ (borderline)
CB Pool Avg: 512 KB
CB Pool Peak: 640 KB
GPU Memory: 81%                    (high load)

Actions:
- Standardize samplers (currently 47 unique)
- Pre-compile shaders (currently 156 variants)
- Monitor peak CB pool (may need increase)
```

---

## Data Export & Analysis

### Exporting Metrics

The profiler stores all frame metrics internally:

```cpp
// Get raw data for external analysis
const Vector<FrameMetrics>& allMetrics = profiler_->GetAllMetrics();

// Export to CSV for spreadsheet analysis
for (const auto& m : allMetrics)
{
    printf("%u,%u,%u,%u,%u,%u,%llu,%.1f\n",
        m.frameNumber,
        m.samplerCacheHits,
        m.samplerCacheMisses,
        m.shaderCacheHits,
        m.shaderCacheMisses,
        m.cbPoolUsedBytes,
        m.gpuMemoryAllocated,
        m.gpuMemoryUtilization);
}
```

### Trend Analysis

```
Frame | Sampler | Shader | CB Pool | GPU Mem
------|---------|--------|---------|--------
  0   |   0/0   |  0/0   |   0 KB  |  10%
 50   |  125/3  | 45/2   |  32 KB  |  15%
100   |  251/5  | 89/3   |  31 KB  |  18%
150   |  376/6  | 133/4  |  33 KB  |  20%
200   |  502/7  | 178/5  |  32 KB  |  22%
250   |  627/8  | 222/6  |  31 KB  |  24%
300   |  752/9  | 266/7  |  33 KB  |  25%

Observations:
- Sampler hit rate stabilizing at ~98%
- Shader hit rate converging to ~97%
- CB pool usage consistent (~32 KB)
- GPU memory growing linearly (texture loads?)
```

---

## Next Steps After Profiling

1. **If all targets met:** Move to Phase 12 (Instancing) or Phase 10 (Staging)
2. **If sampler cache low:** Implement sampler standardization
3. **If shader cache low:** Pre-warm shader cache at startup
4. **If memory high:** Profile texture usage, implement LOD/streaming
5. **If CB pool high:** Batch constant buffers, reduce per-frame updates

---

## Success Criteria

Phase 13 complete when:
- [ ] Profiler implemented and tested
- [ ] 300+ frames of metrics collected
- [ ] Aggregate report generated
- [ ] All targets analyzed (pass/fail)
- [ ] Bottlenecks identified
- [ ] Optimization recommendations documented
- [ ] Results saved in PHASE_13_RESULTS.md

---

**Status:** Profiling harness ready
**Next Action:** Integrate into HelloWorld and run profiling session
**Expected Duration:** 30 minutes (collection) + 30 minutes (analysis)

