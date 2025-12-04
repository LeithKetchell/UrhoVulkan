# Week 2 Integration Plan - Phase 1 Component Integration

**Start Date:** December 5, 2025
**End Date:** December 12, 2025
**Goal:** Integrate all 5 Phase 1 components into main Urho3D codebase

---

## Daily Breakdown

### Day 1 (Friday, Dec 5)

#### Morning: Thread Extensions Verification (1-2 hours)
- [x] Verify `Thread::GetNumCores()` works on Linux
- [x] Verify CPU affinity `SetAffinity(i)` compiles and runs
- [x] Test NUMA detection framework `GetNumNumaNodes()`
- [ ] Run: `./bin/Sample01_HelloWorld --test-threads`

#### Afternoon: WorkQueue Integration Start (3-4 hours)
- [ ] Read existing WorkQueue.h/cpp thoroughly
- [ ] Create backup of original files
- [ ] Add `#include "WorkStealingDeque.h"`
- [ ] Add `Vector<WorkStealingDeque> workerDeques_` member
- [ ] Initialize deques in `WorkQueue::WorkQueue()`
- [ ] Verify code compiles

**Commit Goal:** `Phase 1: Day 1 - WorkQueue foundation`

---

### Day 2 (Monday, Dec 8) - SKIP WEEKEND

#### Morning: WorkQueue Integration Complete (3-4 hours)
- [ ] Update `AddWorkItem()` to use deques
- [ ] Implement `StealFromNeighbor()` helper
- [ ] Update `ProcessItems()` for stealing
- [ ] Set CPU affinity in `CreateThreads()`
- [ ] Test with stress test

#### Afternoon: ParticleBuffer Integration (3-4 hours)
- [ ] Add `#include "ParticleBuffer.h"` to ParticleEmitter
- [ ] Add `SharedPtr<ParticleBuffer> particleBuffer_` member
- [ ] Initialize in `SetEffect()`
- [ ] Update `EmitNewParticle()` → `EmitBatch()`
- [ ] Verify compiles

**Commit Goal:** `Phase 1: Day 2 - WorkQueue + ParticleBuffer setup`

---

### Day 3 (Tuesday, Dec 9)

#### Morning: ParticleBuffer Integration Complete (3-4 hours)
- [ ] Update particle update loop to use SIMD methods
  - [ ] `UpdatePositions(dt)`
  - [ ] `UpdateLifetimes(dt)`
  - [ ] `ApplyForces(dt, gravity, damping)`
  - [ ] `UpdateScales(dt, sizeAdd, sizeMul)`
- [ ] Update `Commit()` to sync to billboards
- [ ] Test with `Sample36_Particles`

#### Afternoon: AudioMixing Integration (3-4 hours)
- [ ] Add `#include "AudioMixing_SIMD.h"` to SoundSource
- [ ] Create `MixMonoToMono_SIMD()` variant
- [ ] Update mixing selector in `Mix()`
- [ ] Update `Audio::MixOutput()` for format conversion
- [ ] Test with `Sample34_Audio`

**Commit Goal:** `Phase 1: Day 3 - Particles + Audio SIMD integration`

---

### Day 4 (Wednesday, Dec 10)

#### Morning: Batch Sort Integration (2-3 hours)
- [ ] Add `#include "BatchSort_Parallel.h"` to View
- [ ] Change batch structure to `SortedBatch` (or wrapper)
- [ ] Update `GetBatches()` to use `BatchSort_Parallel::SortBatches()`
- [ ] Test rendering with complex scene

#### Afternoon: Integration Testing (3-4 hours)
- [ ] Run all 56 samples
- [ ] Check for crashes/errors
- [ ] Verify no visual artifacts
- [ ] Profile basic scene with `perf`

**Commit Goal:** `Phase 1: Day 4 - All components integrated + basic testing`

---

### Day 5 (Thursday, Dec 11)

#### Morning: Bug Fixes & Polish (3-4 hours)
- [ ] Fix any compilation errors
- [ ] Fix any runtime issues found in testing
- [ ] Update documentation with gotchas
- [ ] Clean up debug code

#### Afternoon: Performance Baseline (3-4 hours)
- [ ] Measure FPS before/after
- [ ] Profile with `perf record`
- [ ] Collect performance metrics
- [ ] Document baseline numbers

**Commit Goal:** `Phase 1: Day 5 - Integration complete, baseline measurements`

---

## Integration Checklist

### WorkQueue Integration
- [ ] `WorkStealingDeque` included
- [ ] Per-thread deques initialized
- [ ] `AddWorkItem()` pushes to deque[0]
- [ ] `ProcessItems()` implements work-stealing
- [ ] `StealFromNeighbor()` works
- [ ] `CreateThreads()` sets affinity
- [ ] Compiles without errors
- [ ] Stress test passes

### ParticleBuffer Integration
- [ ] `ParticleBuffer` included
- [ ] Initialized in `SetEffect()`
- [ ] `EmitBatch()` replaces `EmitNewParticle()`
- [ ] SIMD update methods called
- [ ] `Compact()` called periodically
- [ ] Billboard synchronization works
- [ ] `Sample36_Particles` works
- [ ] 1000+ particles without slowdown

### AudioMixing_SIMD Integration
- [ ] `AudioMixing_SIMD` included
- [ ] `MixMonoToMono_SIMD()` variant created
- [ ] Format conversion uses SIMD
- [ ] All 8 mixing variants updated or have fallbacks
- [ ] `Sample34_Audio` works
- [ ] 20+ simultaneous sounds possible
- [ ] No audio artifacts

### BatchSort_Parallel Integration
- [ ] `BatchSort_Parallel` included
- [ ] Batches use 64-byte aligned structure
- [ ] `GetBatches()` calls `SortBatches()`
- [ ] Complex scenes render correctly
- [ ] No visual artifacts from sort change
- [ ] Sort order maintained

### General
- [ ] All 56 samples compile
- [ ] No crashes on startup
- [ ] Performance baseline recorded
- [ ] Documentation updated

---

## Expected Outcomes

### Performance Improvements (After Integration)

| Metric | Before | After | Gain |
|--------|--------|-------|------|
| Particle update (1000) | 8ms | 1ms | 8x |
| Simultaneous audio | 8 | 64+ | 8x+ |
| Batch sort | 2ms | 0.8ms | 2.5x |
| Lock wait (per frame) | 2-3ms | <50μs | 40-60x |
| Overall FPS | 60 | 100-120 | 1.7-2x |

### Build Status
- [ ] 0 compilation errors
- [ ] <5 compiler warnings (unrelated)
- [ ] All tests pass
- [ ] CI/CD green

---

## Risk Mitigation

### Risk 1: WorkQueue Breaking Task System
**Mitigation:**
- Backup original code
- Feature flag to switch back: `URHO3D_USE_WORK_STEALING`
- Keep old code path available

### Risk 2: Particle Physics Accuracy Issues
**Mitigation:**
- Compare particle positions vs original
- Numerical tolerance: 1e-5 for physics
- Validation sample: Particle trails

### Risk 3: Audio Quality Degradation
**Mitigation:**
- Bit-level comparison of output
- Sample multiple audio scenarios
- Listen tests for clipping/artifacts

### Risk 4: Performance Not Meeting Targets
**Mitigation:**
- Profile with `perf` to identify bottleneck
- May need additional optimization (Phase 1.5)
- Document actual improvements vs estimates

---

## Daily Standup Format

**Each day, update:**
```
Day X Status:
- ✅ Completed: [list]
- 🔄 In Progress: [list]
- ⏳ Pending: [list]
- 🐛 Issues: [list]
- Commits: [hash] - [message]
- Performance metrics: [if measured]
```

---

## Success Criteria (End of Week 2)

**Must Have:**
- ✅ All 5 components integrated
- ✅ All samples compile and run
- ✅ No major crashes
- ✅ Baseline performance measured

**Should Have:**
- ✅ 30%+ performance improvement
- ✅ <50μs lock contention
- ✅ Documentation updated

**Nice to Have:**
- ✅ 50%+ performance improvement
- ✅ Multi-core scaling validated
- ✅ Performance graphs generated

---

## Documentation Updates Needed

1. **WEEK2_INTEGRATION_RESULTS.md** (after Friday)
   - Summary of changes
   - Performance baseline
   - Known issues

2. **TROUBLESHOOTING.md** (ongoing)
   - Issues found during integration
   - Solutions applied
   - Workarounds

3. **PERFORMANCE_BASELINE.md** (after profiling)
   - Before/after measurements
   - Per-component breakdown
   - Scaling characteristics

---

## Tools & Commands

### Build
```bash
cd /home/leith/Desktop/URHO/New/urho3d-2.0.1/build
cmake --build . --target Urho3D -j4
```

### Run Sample
```bash
./bin/Sample36_Particles
./bin/Sample34_Audio
./bin/Sample01_HelloWorld
```

### Profile
```bash
perf record -g ./bin/Sample01_HelloWorld
perf report
```

### Git
```bash
git add .
git commit -m "Phase 1: Day X - [changes]"
git push origin master
```

---

## Contingencies

### If Behind Schedule
- Skip Day 5 polish, move to Week 3
- Integrate only 3/5 components in Week 2
- Full integration in Week 3

### If Issues Found
- Roll back to backup
- Investigate root cause
- Fix in isolated branch
- Reintegrate

---

*Week 2 Integration Plan - Phase 1 Component Integration*
*Target: 5/5 components integrated by Friday EOD*
