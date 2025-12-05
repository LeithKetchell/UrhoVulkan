# Phase 2: Performance Optimization Implementation Plan

**Phase**: 2 (Optimization & Profiling)
**Status**: STARTING
**Start Date**: December 5, 2025
**Target Duration**: 1 week (December 5-12, 2025)
**Goal**: Implement missing SIMD functions and establish performance gains

---

## Overview

Phase 1 established the framework and integration points. Phase 2 focuses on implementing the actual SIMD optimization functions and measuring real performance gains.

**Key Deliverables**:
1. ✅ AudioMixing_SIMD: Complete mixing functions (MixMonoWithGain_4x, Convert32to16_4x)
2. ✅ BatchSort_Parallel: Complete parallel merge-sort implementation
3. ✅ Real-world profiling with Sample01, Sample34, Sample36
4. ✅ Performance gain measurements and analysis

---

## Phase 2 Work Breakdown

### Week 1: Core Implementation

#### Day 1-2: AudioMixing_SIMD Implementation
**Objective**: Implement actual SIMD audio mixing functions

**Files to Modify**:
- `Source/Urho3D/Audio/AudioMixing_SIMD.h` - Function declarations (already done)
- `Source/Urho3D/Audio/AudioMixing_SIMD.cpp` - Implementations (NEW)

**Functions to Implement**:
1. `MixMonoWithGain_4x(int* accumulator, const short* source, float gain)`
   - Mix 4 audio samples with gain applied
   - Use SSE2 instructions for parallel operations
   - Saturation protection (clamp to short range)

2. `MixStereoWithGain_4x(int* accumulators, const short* sources, float gain)`
   - Mix 4 stereo sample pairs
   - Left and right channels in parallel

3. `Convert32to16_4x(short* output, const int* input)`
   - Convert 4 32-bit samples to 16-bit
   - Handle overflow protection
   - Rounding for quality

4. `ApplyCrossfade_4x(short* output, const short* source, float factor)`
   - Crossfade with factor (0.0-1.0)
   - 4-sample vectorization

**Implementation Strategy**:
```cpp
// Pseudocode structure
void MixMonoWithGain_4x(int32_t* acc, const int16_t* src, float gain) {
    // Load 4 samples from source
    __m128i samples = _mm_loadu_si128((__m128i*)src);

    // Convert to 32-bit and apply gain
    __m128i s0 = _mm_unpack_lo_epi16(samples, zeros);  // First 2 samples
    __m128i s1 = _mm_unpack_hi_epi16(samples, zeros);  // Last 2 samples

    // Convert to float, multiply by gain, convert back
    __m128 f0 = _mm_cvtepi32_ps(s0);
    __m128 f1 = _mm_cvtepi32_ps(s1);
    __m128 gainv = _mm_set1_ps(gain);
    f0 = _mm_mul_ps(f0, gainv);
    f1 = _mm_mul_ps(f1, gainv);

    // Load accumulator values and add
    __m128i acc0 = _mm_loadu_si128((__m128i*)(acc+0));
    __m128i acc1 = _mm_loadu_si128((__m128i*)(acc+2));

    // Convert to float, add, saturation
    // Store back to accumulator
}
```

**Testing**:
- Unit tests for each function
- Compare scalar vs SIMD output (should be identical)
- Measure throughput with sample rates

**Expected Speedup**: 4x for mono mixing, 8x+ simultaneous sources

---

#### Day 2-3: BatchSort_Parallel Implementation
**Objective**: Implement parallel merge-sort for rendering batch sorting

**Files to Modify**:
- `Source/Urho3D/Graphics/BatchSort_Parallel.h` - Framework (already done)
- `Source/Urho3D/Graphics/BatchSort_Parallel.cpp` - Implementations (NEW)

**Functions to Implement**:
1. `ParallelMergeSort(SortedBatch* batches, uint32_t count, uint32_t numThreads)`
   - Divide work across threads
   - Each thread sorts its partition
   - Merge sorted partitions

2. `MergeSortedBatches(SortedBatch* left, uint32_t leftCount, SortedBatch* right, uint32_t rightCount, SortedBatch* output)`
   - Merge two sorted arrays efficiently
   - Cache-aligned writes

3. `PartitionBatches(SortedBatch* batches, uint32_t count, uint32_t numPartitions)`
   - Divide batches into work chunks
   - Balance load across threads

**Implementation Strategy**:
```cpp
void ParallelMergeSort(SortedBatch* batches, uint32_t count, uint32_t numThreads) {
    if (count <= 1) return;

    // Single-threaded for small counts
    if (numThreads == 1 || count < 100) {
        std::sort(batches, batches + count);
        return;
    }

    uint32_t mid = count / 2;
    SortedBatch* left = batches;
    SortedBatch* right = batches + mid;

    // Parallel sort left and right halves
    // (Use WorkQueue or thread pool from Phase 1)

    // Merge results
    MergeSortedBatches(left, mid, right, count - mid, batches);
}
```

**Testing**:
- Sort reverse-ordered batches (worst case)
- Sort random batches
- Measure time vs std::sort
- Verify correctness (compare sorted output)

**Expected Speedup**: 2.5x with 4 cores

---

#### Day 3-4: Real-World Profiling
**Objective**: Measure actual SIMD performance gains in real samples

**Samples to Profile**:
1. **Sample01_HelloWorld** - Baseline (uses particles)
2. **Sample34_Audio** - Audio mixing
3. **Sample36_Particles** - Heavy particle workload

**Profiling Tools**:
- Linux `time` command for wall-clock timing
- Frame rate measurement (FPS counter)
- CPU load monitoring

**Profiling Strategy**:
```bash
# Baseline (Phase 1 without SIMD work)
time ./bin/Sample01_HelloWorld

# With AudioMixing_SIMD work
time ./bin/Sample34_Audio

# With ParticleBuffer SIMD work
time ./bin/Sample36_Particles --particles 10000
```

**Measurements to Collect**:
- Wall-clock time (total run time)
- FPS (frames per second)
- CPU utilization
- Memory usage
- Audio source count (for audio sample)

**Data Points**:
- Baseline vs Optimized comparison
- Individual component impact
- Combined impact of all 4 components
- Scalability with thread count

---

#### Day 4-5: Performance Analysis & Documentation
**Objective**: Analyze results and document findings

**Analysis Tasks**:
1. Calculate speedup ratios (Optimized / Baseline)
2. Compare against theoretical expectations
3. Identify bottlenecks
4. Suggest further optimizations

**Documentation**:
- Create `PHASE2_RESULTS.md` with findings
- Create `PERFORMANCE_ANALYSIS.md` with detailed breakdown
- Update `PHASE_1_README.md` with actual results
- Generate performance graphs/tables

**Report Contents**:
- Baseline measurements recap
- Optimized measurements
- Speedup analysis (per component and combined)
- Scalability analysis
- Bottleneck identification
- Recommendations for Phase 3

---

## Implementation Details

### AudioMixing_SIMD Functions

#### Function 1: MixMonoWithGain_4x
```cpp
/**
 * Mix 4 mono audio samples with gain applied (SIMD optimized)
 * @param accumulator Array of 4 int32_t accumulators
 * @param source Array of 4 int16_t audio samples
 * @param gain Gain factor (1.0 = no change)
 * @note Prevents saturation by clamping to int16_t range
 * @note SSE2 optimized: 4 samples per iteration
 */
void MixMonoWithGain_4x(int32_t* accumulator, const int16_t* source, float gain);
```

**Algorithm**:
1. Load 4 int16_t samples from source
2. Unpack to int32_t (sign-extend)
3. Convert to float for gain multiplication
4. Apply gain
5. Convert back to int32_t with saturation
6. Add to accumulator (with overflow protection)
7. Store back to accumulator

**Expected Performance**: 4 samples mixed in ~2-3 CPU cycles (with gain)

#### Function 2: Convert32to16_4x
```cpp
/**
 * Convert 4 int32_t samples to int16_t with rounding
 * @param output Array of 4 int16_t output samples
 * @param input Array of 4 int32_t input samples
 * @note Clamps to int16_t range [-32768, 32767]
 * @note Uses proper rounding for quality
 * @note SSE2 optimized
 */
void Convert32to16_4x(int16_t* output, const int32_t* input);
```

**Algorithm**:
1. Load 4 int32_t values
2. Divide by 2^15 (scale to int16_t range)
3. Apply rounding (add 0.5 before truncate)
4. Saturate to int16_t range
5. Store 4 int16_t values

**Expected Performance**: 4 samples converted in ~1-2 CPU cycles

### BatchSort_Parallel Functions

#### Function 1: ParallelMergeSort
```cpp
/**
 * Sort rendering batches in parallel using merge-sort
 * @param batches Array of SortedBatch structures
 * @param count Number of batches
 * @param numThreads Number of worker threads
 * @note Uses work-stealing from Phase 1 WorkQueue
 * @note Cache-friendly 64-byte aligned access
 */
void ParallelMergeSort(SortedBatch* batches, uint32_t count, uint32_t numThreads);
```

**Algorithm**:
1. Divide batches into `numThreads` partitions
2. Each thread sorts its partition with std::sort
3. Merge all partitions into final sorted order
4. Use atomic operations for synchronization (from Phase 1)

**Expected Performance**: 2.5x speedup with 4 cores

---

## Success Criteria

### Code Quality
- ✅ All functions compile without errors
- ✅ No undefined behavior or buffer overflows
- ✅ Proper error handling
- ✅ Clear, documented code

### Functional Correctness
- ✅ AudioMixing_SIMD produces identical output to scalar version
- ✅ BatchSort_Parallel produces correctly sorted output
- ✅ No crashes or segmentation faults
- ✅ All samples run successfully

### Performance Targets
- ✅ AudioMixing: 8x+ simultaneous sources
- ✅ ParticleBuffer: 8x particle update speedup
- ✅ BatchSort: 2.5x sorting speedup
- ✅ Combined: 100-150% FPS improvement

### Testing
- ✅ Unit tests for each function
- ✅ Integration tests in real samples
- ✅ Performance benchmarks
- ✅ Correctness validation

---

## Risks & Mitigation

| Risk | Impact | Mitigation |
|------|--------|-----------|
| SSE2 not available | Can't use SIMD | Scalar fallback always available |
| SIMD saturation issues | Wrong audio output | Careful range checking, unit tests |
| Load imbalance in parallel sort | Poor speedup | Dynamic work stealing from Phase 1 |
| Cache contention | Performance regression | 64-byte alignment (already in design) |
| Integration bugs | Compilation failures | Incremental testing after each function |

---

## Schedule

**Week 1 (Immediate)**:
- Day 1-2: AudioMixing_SIMD functions (3 functions)
- Day 2-3: BatchSort_Parallel functions (3 functions)
- Day 3-4: Real-world profiling
- Day 4-5: Analysis and documentation

**Checkpoint Days**:
- Day 2: AudioMixing functions compile and run
- Day 3: BatchSort functions compile and run
- Day 4: All samples profile successfully
- Day 5: Analysis report complete

---

## Deliverables

### Code (Phase 2)
- ✅ AudioMixing_SIMD.cpp (4 functions)
- ✅ BatchSort_Parallel.cpp (3 functions)
- ✅ Unit tests for each function
- ✅ Integration validation in samples

### Documentation
- ✅ PHASE2_RESULTS.md (performance results)
- ✅ PERFORMANCE_ANALYSIS.md (detailed analysis)
- ✅ Updated README with actual measurements
- ✅ Function documentation in headers

### Testing
- ✅ Functional correctness tests
- ✅ Performance benchmarks
- ✅ Sample integration tests
- ✅ Regression testing

### Git Commits
- ✅ AudioMixing_SIMD implementation
- ✅ BatchSort_Parallel implementation
- ✅ Performance measurements commit
- ✅ Analysis documentation commit

---

## References

### Phase 1 Components Used
- WorkStealingDeque: For thread coordination in parallel sort
- ParticleBuffer: Already using SIMD (baseline for comparison)
- Thread extensions: CPU affinity for profiling

### External References
- Intel Intrinsics Guide: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html
- SSE2 Documentation: x86-64 SIMD instruction set
- Merge-sort Algorithm: Classic divide-and-conquer approach

---

## Next Phase (Phase 3)

After Phase 2 completes:
1. GPU compute integration (if measured gains suggest it)
2. NUMA topology awareness (for multi-socket systems)
3. Advanced memory pooling and allocation optimization
4. Lock-free ring buffer for audio (if needed)
5. Multi-GPU rendering support

---

**Phase 2 Plan: Implementation-focused optimization of Phase 1 framework components.**

Ready to begin Day 1: AudioMixing_SIMD implementation.
