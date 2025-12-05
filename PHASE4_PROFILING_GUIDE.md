# Phase 4 Stage 3: Profiling Guide for Bottleneck Identification

**Purpose**: Use performance profiling to identify actual CPU bottlenecks in Urho3D samples

**Tools**: `perf` (Linux performance profiler)

---

## Quick Start

### 1. Run Profiling Script
```bash
cd /home/leith/Desktop/URHO/New/urho3d-2.0.1
./phase4_profiling.sh
```

**What it does**:
- Profiles 3 samples (HelloWorld, SoundEffects, Urho2DParticle)
- Records 10 seconds of CPU activity per sample
- Generates reports showing function call times
- Outputs to `build/phase4_profiling/`

### 2. Permissions Issue?
If you see "Permission denied" errors, run:
```bash
sudo sysctl -w kernel.perf_event_paranoid=-1
```

This allows perf to profile user-space code without root.

### 3. View Reports
```bash
# Simple text report (top 20 functions)
cat build/phase4_profiling/01_HelloWorld.perf.txt

# Interactive call graph
perf report -i build/phase4_profiling/01_HelloWorld.perf.data
```

---

## What to Look For

### Key Metrics
1. **Overhead %** - Percentage of total CPU time spent in function
2. **Cumulative %** - Running total of time in function + callees
3. **Function** - Name of the function consuming CPU

### Example Output
```
   Overhead  Cumulative      Function
    12.5%     12.5%        Graphics::Draw()
     8.3%     20.8%        Scene::Update()
     7.2%     28.0%        Audio::Mix()
     6.1%     34.1%        Physics::Simulate()
     ...
```

This would indicate Graphics drawing is the top bottleneck at 12.5% of CPU time.

---

## Interpreting Results

### Scenario 1: Rendering is the Bottleneck (>20% of time)
```
Overhead  Function
   22%    Graphics::Draw
    8%    Graphics::SetShader
    7%    Graphics::SubmitDrawCall
```
**Decision**: Focus optimization on rendering pipeline
- GPU operations might be slow
- Batch sorting could help
- Shader compilation might be bottleneck

### Scenario 2: Audio is the Bottleneck (>10% of time)
```
Overhead  Function
   15%    Audio::Mix
    8%    Audio::ProcessSamples
```
**Decision**: Integrate AudioMixing_SIMD (PATH A)
- SIMD audio mixing directly targets this bottleneck
- Expected 10-30% improvement in audio mixing

### Scenario 3: Physics is the Bottleneck (>20% of time)
```
Overhead  Function
   25%    Physics::Simulate
   12%    Physics::ConstraintSolve
```
**Decision**: Optimize physics, not audio/particles
- None of our Phase 1 & 2 optimizations target physics
- Would need new optimization approach

### Scenario 4: Work Queue/Threading is Bottleneck (>15% of time in lock functions)
```
Overhead  Function
    8%    WorkQueue::Execute
    6%    spinlock/mutex operations
```
**Decision**: Verify WorkStealingDeque is helping
- Check if lock-free deque reduced contention
- May need to tune deque parameters

### Scenario 5: No Single Bottleneck (<5% any function)
```
Overhead  Function
    4%    Function1
    4%    Function2
    4%    Function3
```
**Decision**: System is well-balanced
- No single optimization will have large impact
- Multiple small improvements (2-3% each) could add up
- Current sample might not stress optimizable code

---

## Sample Analysis Strategy

### For 01_HelloWorld
**Expected**: Light workload, no heavy computation
- Likely bottleneck: Event processing, scene update
- AudioMixing_SIMD probably won't show up (no audio)
- ParticleBuffer probably won't show up (few/no particles)
- WorkStealingDeque benefit only if multi-threaded work heavy

### For 14_SoundEffects
**Expected**: Audio-focused workload
- **Key question**: Is Audio::Mix in top 10?
  - YES → AudioMixing_SIMD integration would help (PATH A)
  - NO → Audio isn't actually CPU-heavy in this sample
- May still be graphics-heavy if sample has visual output

### For 25_Urho2DParticle
**Expected**: Particle-focused workload
- **Key question**: Is ParticleBuffer::Update in top 10?
  - YES → ParticleBuffer SIMD is working/has room to improve
  - NO → Particle updates aren't CPU-heavy in this sample
- Will likely have graphics rendering overhead

---

## Understanding perf Reports

### Call Graph View
When you run `perf report` interactively:
- Press `Enter` on a function to see its **children** (called functions)
- This shows the **call tree** - where time is actually spent
- Audio mixing might be called from Audio::ProcessFrame
- Look for the path to expensive functions

### Filtering by Function
To find if specific function is being called:
```bash
perf report -i build/phase4_profiling/14_SoundEffects.perf.data | grep -i audio
perf report -i build/phase4_profiling/25_Urho2DParticle.perf.data | grep -i particle
```

---

## Decision Flowchart

```
Run profiling script
    ↓
Read reports (top 20 functions)
    ↓
    ├─ Audio::Mix in top 5 (>8%)?
    │  ├─ YES → PATH A: Integrate AudioMixing_SIMD
    │  └─ NO → Go to next check
    │
    ├─ Graphics::Draw in top 3 (>15%)?
    │  ├─ YES → PATH B: Optimize rendering (batch sort, etc.)
    │  └─ NO → Go to next check
    │
    ├─ Physics::Simulate in top 5 (>10%)?
    │  ├─ YES → PATH B: New physics optimization
    │  └─ NO → Go to next check
    │
    ├─ Particle::Update in top 10 (>5%)?
    │  ├─ YES → Verify ParticleBuffer is helping
    │  └─ NO → Go to next check
    │
    └─ No single function >5%?
       └─ Multiple small improvements, system well-balanced
```

---

## Next Steps After Profiling

1. **Identify top 5 bottlenecks** by examining reports

2. **For each bottleneck**:
   - Does our Phase 1 & 2 code target it?
   - If YES: Is the optimization already integrated?
   - If NO: Should we integrate it?

3. **Make PATH A vs PATH B decision**:
   - **PATH A**: Integrate unused optimizations (AudioMixing_SIMD, BatchSort_Parallel)
   - **PATH B**: Focus on actual bottlenecks (may require new optimizations)

4. **Implement chosen path** in Phase 4 Stage 3.2

---

## Troubleshooting

### Error: "Permission denied" running perf
```bash
sudo sysctl -w kernel.perf_event_paranoid=-1
# Then try again without sudo
```

### Error: "No stack frame" or incomplete call graphs
- Sample might not have symbols compiled in
- Try rebuilding with debug symbols: `rake build CMAKE_BUILD_TYPE=Debug`

### Report shows "unknown" or symbol names are weird
- Binaries might not have debug symbols
- Check: `strings build/bin/01_HelloWorld | grep _Z` (should show mangled C++ names)

### perf record hangs or times out
- Sample might be waiting for user input
- Check if sample runs in headless mode: `timeout 5 ./build/bin/01_HelloWorld 2>&1 | head`

---

## Expected Profiling Time

- Script runtime: ~40-50 seconds total (10 seconds × 3 samples + overhead)
- Analysis time: 5-10 minutes to review reports and identify bottlenecks
- Decision time: 5 minutes to map findings to PATH A vs B

**Total: ~1 hour for profiling + analysis + decision**

---

## Success Criteria

✅ Profiling complete when:
1. Reports generated for all 3 samples
2. Top 20 functions identified for each
3. Bottlenecks clearly identified
4. Decision made: PATH A vs B based on data

This data-driven approach replaces guessing about what to optimize!

