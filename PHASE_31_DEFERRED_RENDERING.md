# Phase 31: Deferred Rendering (Multi-pass Architecture)

**Status:** Planning
**Phase Number:** 31
**Priority:** High (enables 100+ dynamic lights)
**Estimated LOC:** 1500-2000
**Complexity:** High

---

## Overview

Implement deferred rendering pipeline for Urho3D Vulkan backend, enabling support for 100+ dynamic lights per scene. Replaces forward rendering's per-light shader passes with a unified G-Buffer generation followed by light shading.

### Key Objectives

- Generate G-Buffer in first pass (geometry pass)
- Implement deferred lighting calculations in second pass
- Support unlimited dynamic lights (GPU limited)
- Maintain backward compatibility with forward rendering
- Optimize memory bandwidth via G-Buffer compression

---

## Architecture

### G-Buffer Layout

**Standard 4-attachment setup:**

| Attachment | Format | Purpose | Size |
|-----------|--------|---------|------|
| **Position** | RGBA32F | World position / depth | 16 bytes/pixel |
| **Normal** | RGBA16F | World normal + material ID | 8 bytes/pixel |
| **Albedo** | RGBA8 | Base color + roughness | 4 bytes/pixel |
| **Specular** | RGBA8 | Specular color + metallic | 4 bytes/pixel |

**Total G-Buffer Memory:** ~32 bytes/pixel (at 1920x1080: ~80 MB)

### Render Pass Structure

```
Frame Begin
  ├─ [GEOMETRY PASS] - Generate G-Buffer
  │  ├─ Clear G-Buffer attachments
  │  ├─ Render all opaque geometry
  │  └─ Output: Position, Normal, Albedo, Specular
  │
  ├─ [LIGHTING PASS] - Deferred Light Shading
  │  ├─ Input: G-Buffer textures
  │  ├─ Process all lights (point, spot, directional)
  │  ├─ Accumulate lighting into final color
  │  └─ Output: Lit scene
  │
  ├─ [COMPOSITION PASS] - Post-process
  │  ├─ Tonemapping
  │  ├─ Gamma correction
  │  └─ Output: Final backbuffer
  │
Frame Present
```

### Attachment Configuration

**Render Pass with Subpasses:**

```cpp
// Subpass 0: Geometry Pass
- Input attachments: None
- Color outputs: Position, Normal, Albedo, Specular
- Depth: Read/Write

// Subpass 1: Lighting Pass
- Input attachments: Position, Normal, Albedo, Specular (from subpass 0)
- Color output: Final lit scene
- Depth: Read-only

// Subpass 2: (Optional) Composition Pass
- Input attachments: Final scene
- Color output: Backbuffer
- Depth: None
```

---

## Implementation Steps

### Step 1: Create G-Buffer Attachments

**Files to Modify:**
- `VulkanGraphicsImpl.h` - Add G-Buffer image members
- `VulkanGraphicsImpl.cpp` - Implement CreateGBuffer()

**Tasks:**
- Create 4 VkImage handles for G-Buffer textures
- Allocate via VMA with optimal format/tiling
- Create corresponding VkImageView objects
- Initialize layouts to GENERAL or SHADER_READ_ONLY

**Code Structure:**
```cpp
// Members
VkImage gBufferPositionImage_;
VkImage gBufferNormalImage_;
VkImage gBufferAlbedoImage_;
VkImage gBufferSpecularImage_;
VkImageView gBufferPositionView_;
// ... etc

bool CreateGBuffer(int width, int height);
void DestroyGBuffer();
```

### Step 2: Update Render Pass for Subpasses

**Files to Modify:**
- `VulkanGraphicsImpl.cpp` - Update CreateRenderPass()

**Tasks:**
- Extend RenderPassDescriptor to support subpass count
- Create VkSubpassDescription for each pass
- Set up VkAttachmentReference for input/output
- Define VkSubpassDependency between passes
- Handle input attachments in subpass 1+

**Subpass Dependencies:**
```cpp
// Geometry Pass → Lighting Pass
dependency.srcSubpass = 0;
dependency.dstSubpass = 1;
dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
```

### Step 3: Implement Geometry Pass

**Files to Modify:**
- `Graphics_Vulkan.cpp` - Add geometry pass rendering
- `VulkanGraphicsImpl.cpp` - BeginGeometryPass()

**Tasks:**
- Render all opaque geometry to G-Buffer
- Output position, normal, albedo, specular
- Use G-Buffer specific shaders
- Handle MSAA if enabled

**Shader Outputs:**
```glsl
// Fragment shader
layout(location = 0) out vec4 outPosition;    // World position
layout(location = 1) out vec4 outNormal;      // World normal
layout(location = 2) out vec4 outAlbedo;      // Diffuse color
layout(location = 3) out vec4 outSpecular;    // Specular properties
```

### Step 4: Implement Lighting Pass

**Files to Modify:**
- `VulkanGraphicsImpl.cpp` - Add LightingPassShader
- `Graphics_Vulkan.cpp` - Lighting pass execution

**Tasks:**
- Read G-Buffer as input attachments
- Process all dynamic lights
- Accumulate lighting results
- Support point, spot, directional lights

**Light Computation:**
```glsl
layout(input_attachment_index = 0) uniform subpassInput inputPosition;
layout(input_attachment_index = 1) uniform subpassInput inputNormal;
layout(input_attachment_index = 2) uniform subpassInput inputAlbedo;
layout(input_attachment_index = 3) uniform subpassInput inputSpecular;

// For each light:
vec3 lightContribution = CalculateLight(light, position, normal, albedo);
outColor += lightContribution;
```

### Step 5: Handle Forward Rendering Objects

**Files to Modify:**
- `VulkanGraphicsImpl.cpp` - BeginRenderPass()

**Tasks:**
- Detect render mode (forward vs. deferred)
- Route geometry to appropriate pipeline
- Support mixed forward + deferred rendering
- Handle transparent objects in forward pass

### Step 6: Memory Optimization

**Tasks:**
- Compress G-Buffer formats where possible
  - Normal: Use 16-bit snorm (quantized)
  - Depth: Reconstruct from lighting
- Tile-based deferred rendering for mobile
- G-Buffer reuse across frames (if applicable)

### Step 7: Integration & Testing

**Tasks:**
- Integrate into frame pipeline
- Test with 01_HelloWorld, 02_HelloGUI
- Verify lighting correctness
- Profile memory bandwidth
- Test with 50+ dynamic lights

---

## Memory Requirements

**Per-frame G-Buffer:**
- Position: 1920×1080×16 bytes = ~32 MB
- Normal: 1920×1080×8 bytes = ~16 MB
- Albedo: 1920×1080×4 bytes = ~8 MB
- Specular: 1920×1080×4 bytes = ~8 MB
- **Total: ~64 MB** (plus depth buffer ~8 MB)

**Light Buffer:**
- Per light: ~256 bytes
- 100 lights: ~25 KB
- Negligible compared to G-Buffer

---

## Performance Impact

**Estimated Impact:**

| Metric | Forward (1 light) | Deferred (100 lights) | Improvement |
|--------|-------------------|----------------------|-------------|
| Geometry Pass | 1.0x | 1.2x | -20% (extra outputs) |
| Lighting | 1.0x | 0.1x | **+1000%** |
| Total Frame Time | 1.0x | 0.5x | **+100%** |
| Memory | Baseline | +64 MB | -64 MB (vs. forward) |

**Why Deferred Wins:**
- Forward: O(lights × geometry) complexity
- Deferred: O(geometry + lights) complexity
- At 100 lights: 100x improvement in light calculations

---

## Backward Compatibility

**Forward Rendering Preserved:**
- Keep existing forward path for transparent objects
- Allow per-material render mode selection
- Support both paths in same frame

**Configuration:**
```cpp
enum class RenderingMode {
    FORWARD,      // Original forward rendering
    DEFERRED,     // New deferred rendering
    AUTO          // Choose based on light count
};
```

---

## Known Limitations (v1.0)

- Single render target (no MRT resolve for MSAA yet)
- Shading limited to opaque objects (transparent in forward)
- No dynamic resolution scaling
- No tile-based variant for mobile
- Bandwidth intensive on memory-limited systems

---

## Future Enhancements (Phase 32+)

### Phase 32A: Compute Shader Lighting
- Use compute shaders for deferred lighting
- Better cache coherency
- GPU memory optimization

### Phase 32B: Light Culling
- Cluster-based light culling
- Reduce shader invocations
- Support 1000+ lights

### Phase 32C: Tile-based Deferred
- Mobile-friendly variant
- Reduced bandwidth requirements
- Smaller G-Buffer footprint

---

## Vulkan API Usage

**Key Vulkan Features:**
- Input attachments (subpass inputs)
- Subpass dependencies
- Framebuffer with multiple color attachments
- VkImageUsageFlags: COLOR_ATTACHMENT_BIT | INPUT_ATTACHMENT_BIT

**Shader Features:**
- subpassLoad() for input attachments
- layout(input_attachment_index) qualifiers
- Subpass input chain-loading

---

## Testing Checklist

- [ ] G-Buffer creation and destruction
- [ ] Geometry pass renders correctly
- [ ] Lighting pass accumulates properly
- [ ] 100+ lights render without artifacts
- [ ] MSAA compatibility
- [ ] Forward + deferred mixed rendering
- [ ] Memory profiling (<100 MB total)
- [ ] Performance benchmarking
- [ ] All 55+ samples compile and run

---

## References

- **Vulkan Subpasses:** https://www.khronos.org/registry/vulkan/specs/1.3/html/vkspec.html#renderpass-subpasses
- **Input Attachments:** https://www.khronos.org/registry/vulkan/specs/1.3/html/vkspec.html#descriptorsets-inputattachment
- **Deferred Rendering:** https://learnopengl.com/Advanced-Lighting/Deferred-Shading
- **Modern Deferred Techniques:** https://www.ea.com/seed/article/frostbite-engine-gdc-2015-deferred-rendering

---

**Phase 31 Deferred Rendering Plan**
Created: 2025-12-04
Version: 1.0
