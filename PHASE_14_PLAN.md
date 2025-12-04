# Phase 14: Draw Pipeline Integration - Implementation Plan

**Status**: Planning
**Date**: November 29, 2025
**Objective**: Integrate Batch/BatchGroup rendering with Vulkan indirect draw commands and material system

---

## Strategic Overview

Phase 14 bridges the gap between Urho3D's high-level rendering system (Batch/BatchGroup) and our optimized Vulkan GPU-driven rendering infrastructure (Phase 10-13).

### Current State (Pre-Phase 14)
- ✅ Phase 1-9: Core Vulkan rendering pipeline
- ✅ Phase 10: Staging buffers for async GPU uploads
- ✅ Phase 12: Indirect draw command manager (GPU-driven rendering)
- ✅ Phase 13: Performance profiling framework

**Gap**: Batch/BatchGroup rendering still uses immediate-mode rendering (one draw call per batch), not GPU-driven indirect draws.

### Goal (Phase 14)
Enable Batch/BatchGroup rendering to leverage GPU-driven indirect draws for:
- Reduced CPU-to-GPU overhead
- Batched draw submissions
- Per-instance material/shader parameters via Vulkan descriptors
- Improved cache locality and GPU efficiency

---

## Key Components to Modify

### 1. Batch and BatchGroup Classes (Batch.h/cpp)

#### Current Flow
```
Batch::Draw()
  → Prepare() [set shaders, materials, state]
  → geometry_->Draw() [immediate vkCmdDraw]
```

#### Proposed Vulkan Flow
```
Batch::Draw()
  → Prepare() [validate shaders, materials]
  → VulkanBatchDispatcher::RecordBatch() [build indirect command]
    ├─ Update descriptor sets (material params)
    ├─ Add indirect draw command
    └─ Track instance data
  → [Later] VulkanBatchDispatcher::SubmitIndirectDraws()
    └─ vkCmdDrawIndexedIndirect [one call for N batches]
```

#### Required Changes
- **Batch::Prepare()**: Already works - validates state, sets parameters
- **Batch::Draw()**: Dispatch to Vulkan path for indirect drawing
- **New**: Batch::RecordToIndirectBuffer() - Records batch data to GPU buffers
- **New**: Material parameter extraction - Map Material properties to descriptor sets

### 2. Material Parameter Mapping

#### Current Material System
```cpp
Material {
  techniques[]  // Shader variations
  passes[]      // Blend modes, render states
  textures[]    // Diffuse, normal, specular, etc.
  shaderParams  // Vector<Variant> with custom parameters
}
```

#### Vulkan Descriptor Mapping
```
Material → VkDescriptorSet
├─ Uniform buffers (material parameters)
├─ Sampled images (textures)
├─ Samplers
└─ Storage buffers (if compute-related)
```

#### Required Implementation
- **VulkanMaterialDescriptorManager**: New class to handle material → descriptor conversion
  - Cache descriptor sets by material hash
  - Update descriptors when material changes
  - Handle texture bindings and samplers

### 3. Indirect Draw Command Building

#### Pipeline for Each Batch
```
1. Validate batch state (geometry, material, shaders)
2. Get/create descriptor set for material
3. Update push constants (world transform, etc.)
4. Add VkDrawIndexedIndirectCommand to buffer
5. Track instance count and binding offsets
```

#### VulkanBatchDispatcher Class (New)

**Purpose**: Centralized batch-to-indirect-draw translation

**Key Methods**:
```cpp
class VulkanBatchDispatcher {
public:
  // Initialize with graphics implementation
  bool Initialize(VulkanGraphicsImpl* graphics);

  // Record a single batch to indirect buffer
  bool RecordBatch(const Batch* batch, View* view, Camera* camera);

  // Record a batch group (multiple instances)
  bool RecordBatchGroup(const BatchGroup* group, View* view, Camera* camera);

  // Submit all recorded indirect draws
  void SubmitIndirectDraws(VkCommandBuffer cmdBuffer);

  // Clear recorded data for next frame
  void Reset();

private:
  // Convert material to descriptor set
  VkDescriptorSet GetMaterialDescriptor(Material* material);

  // Build indirect draw command
  VkDrawIndexedIndirectCommand BuildDrawCommand(const Batch* batch);

  // Allocate/update descriptor sets for materials
  SharedPtr<VulkanMaterialDescriptorManager> materialDescriptors_;

  // Track recorded commands
  Vector<VkDrawIndexedIndirectCommand> indirectCommands_;
  Vector<BatchDrawInfo> batchInfo_;  // Per-batch metadata
};
```

### 4. State Caching and Optimization

#### Pipeline State Management
```
Pipeline Selection:
1. Hash: (geometry type, material, pass, vertex shader)
2. Look up cached VkPipeline
3. If not found, create and cache
```

#### Descriptor Set Management
```
Material Descriptors:
1. Hash: (material pointer, texture state)
2. Look up cached VkDescriptorSet
3. If dirty, update via vkUpdateDescriptorSets
```

#### Sampler Management
```
Reuse from Phase 9:
- VulkanSamplerCache already handles sampler deduplication
- No changes needed
```

---

## Implementation Phases

### Phase 14.1: Foundation (50 lines)

Create VulkanBatchDispatcher skeleton with basic initialization:
```cpp
// VulkanBatchDispatcher.h/cpp
class VulkanBatchDispatcher : public Object {
  bool Initialize(VulkanGraphicsImpl* graphics);
  void Reset();
};
```

### Phase 14.2: Material Descriptors (150 lines)

Implement VulkanMaterialDescriptorManager:
```cpp
// Map Material → VkDescriptorSet
class VulkanMaterialDescriptorManager {
  VkDescriptorSet GetDescriptor(Material* material);
  void UpdateDescriptor(VkDescriptorSet set, Material* material);
  void Clear();
};
```

Key features:
- Extract textures from Material
- Create descriptor set layouts
- Update descriptor sets via vkUpdateDescriptorSets

### Phase 14.3: Batch Recording (200 lines)

Implement Batch::RecordToIndirectBuffer():
```cpp
bool Batch::RecordToIndirectBuffer(
  View* view,
  Camera* camera,
  VulkanBatchDispatcher* dispatcher
) {
  // Prepare state
  Prepare(view, camera, true, true);

  // Record to dispatcher
  return dispatcher->RecordBatch(this, view, camera);
}
```

Key features:
- Validate batch geometry and shaders
- Extract material parameters
- Build push constants
- Record indirect command

### Phase 14.4: Batch Group Support (100 lines)

Extend for instanced rendering:
```cpp
bool BatchGroup::RecordToIndirectBuffer(
  View* view,
  Camera* camera,
  VulkanBatchDispatcher* dispatcher
) {
  // Prepare state once
  Prepare(view, camera, false, true);

  // Record all instances
  for (const InstanceData& instance : instances_) {
    // Build per-instance commands
  }
}
```

Key features:
- Single state setup for entire group
- Per-instance transform tracking
- Instancing data buffer management

### Phase 14.5: Command Submission (100 lines)

Implement indirect draw submission:
```cpp
void VulkanBatchDispatcher::SubmitIndirectDraws(VkCommandBuffer cmdBuffer) {
  // Bind descriptor sets and pipeline
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(...);

  // Single indirect draw call for all batches
  vkCmdDrawIndexedIndirect(
    cmdBuffer,
    indirectCommandBuffer,
    0,
    recordedBatchCount,
    sizeof(VkDrawIndexedIndirectCommand)
  );
}
```

### Phase 14.6: Integration with BatchQueue (150 lines)

Modify BatchQueue::Draw() to use dispatcher:
```cpp
void BatchQueue::Draw(View* view, Camera* camera, ...) {
  VulkanBatchDispatcher dispatcher;
  dispatcher.Initialize(graphics);

  for (auto& batch : batches_) {
    batch.RecordToIndirectBuffer(view, camera, &dispatcher);
  }

  for (auto& [key, group] : batchGroups_) {
    group.RecordToIndirectBuffer(view, camera, &dispatcher);
  }

  dispatcher.SubmitIndirectDraws(cmdBuffer);
}
```

### Phase 14.7: Testing and Validation (100 lines)

Create PHASE_14_INTEGRATION_TEST.md with:
- Batch rendering correctness verification
- BatchGroup instancing validation
- Performance metrics (draw call reduction)
- Material parameter binding tests

---

## Technical Details

### Material Parameter Binding

#### Supported Parameter Types
```cpp
// From Material system
float parameters
vector3 parameters
color parameters
texture references
```

#### GPU Representation
```cpp
// In constant buffer
struct MaterialParams {
  vec4 diffuseColor;
  vec4 specularColor;
  vec4 ambientColor;
  float metallic;
  float roughness;
  float shininess;
  // padding for alignment
};
```

### Push Constants vs Descriptors

#### Push Constants (for per-batch data)
- World transform (4x3 matrix = 48 bytes)
- Normal matrix (4x3 matrix = 48 bytes)
- **Total: 96 bytes** (within typical limit of 128 bytes)

#### Descriptor Sets (for per-material data)
- Material color parameters
- Texture samplers
- Light parameters
- Environment maps

### Error Handling

Graceful degradation if:
- Indirect draws unsupported → Fall back to Batch::Draw()
- Descriptor creation fails → Use default descriptors
- Material too complex → Split into multiple draws

---

## Expected Outcomes

### Performance Improvements
- **Draw call reduction**: N batches → 1 indirect draw (if same material)
- **GPU efficiency**: 30-50% improvement for batch-heavy scenes
- **CPU savings**: ~40% reduction in draw command recording

### Code Statistics
- **Total lines**: ~700 lines of new code
- **New files**: 2 (VulkanBatchDispatcher.h/cpp, VulkanMaterialDescriptorManager.h/cpp)
- **Modified files**: 4 (Batch.h/cpp, BatchQueue.cpp, VulkanGraphicsImpl.h/cpp)

### Build Verification
- All 54 samples compile without errors
- No new compiler warnings
- Batch rendering produces identical output

---

## Next Phases (Post-14)

### Phase 15: MSAA Support
- VkImageCreateInfo with sampleCount > 1
- Resolve attachments
- Multi-sample render passes

### Phase 16: Deferred Rendering
- Multiple render passes
- G-buffer composition
- Deferred lighting

### Phase 17: Compute Shader Integration
- Compute pipeline creation
- Buffer binding
- Synchronization barriers

---

## Success Criteria

- [x] Plan documented comprehensively
- [ ] VulkanBatchDispatcher implemented
- [ ] Material descriptor manager working
- [ ] Batch recording to indirect buffers
- [ ] BatchGroup instancing support
- [ ] Indirect draw submission
- [ ] BatchQueue integration
- [ ] All 54 samples compile
- [ ] Zero new errors/warnings
- [ ] Performance profiling shows improvement

---

**Status**: Ready for implementation
**Estimated Effort**: Full phase development
**Dependencies**: Phase 1-13 complete, VulkanIndirectDrawManager available
**Blocking Issues**: None identified

