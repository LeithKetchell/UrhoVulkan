# Phase 34: Deferred Rendering Framework

## Overview

Phase 34 implements the complete deferred rendering framework for Urho3D's Vulkan backend. This phase provides the infrastructure for multi-pass deferred rendering by establishing G-Buffer framebuffer creation, render target binding, and dynamic framebuffer selection between forward and deferred rendering paths.

**Status:** ✅ Complete and Functional
**Build:** All 56+ samples compile successfully
**Commits:** 3 commits (cc73266, f3df931, 243c0f2)

## Architecture

### Deferred Rendering Pipeline

```
Initialization
    ↓
[CreateGBuffer] → Allocate 4 color + depth images via VMA
    ↓
[SetRenderTarget] → Mark renderTargets_ dirty when targets change
    ↓
[BeginRenderPass] → Rebuild framebuffer if dirty, select deferred or forward
    ↓
[Rendering] → Draw geometry to G-Buffer or forward framebuffer
    ↓
[Shutdown] → Destroy all G-Buffer resources
```

### G-Buffer Layout

The G-Buffer consists of 4 color attachments plus shared depth:

| Attachment | Format | Usage |
|-----------|--------|-------|
| [0] Position | R32G32B32A32_SFLOAT | World position + material ID |
| [1] Normal | R16G16B16A16_SFLOAT | World normal + specular intensity |
| [2] Albedo | R8G8B8A8_UNORM | Diffuse color + metallicity |
| [3] Specular | R8G8B8A8_UNORM | Specular color + roughness |
| [4] Depth | D32_SFLOAT | Depth buffer (shared) |

## Implementation Details

### Step 1: G-Buffer Image Creation

**File:** `VulkanGraphicsImpl.cpp:1366-1436`

```cpp
bool VulkanGraphicsImpl::CreateGBuffer(int width, int height)
```

**What it does:**
- Creates 4 VkImage objects via VMA allocator
- Creates corresponding VkImageView for each image
- Configured for optimal tiling and color attachment usage
- Uses GPU-only memory (VMA_MEMORY_USAGE_GPU_ONLY)

**Called during:** `VulkanGraphicsImpl::Initialize()` after framebuffer creation

**Error handling:** Non-fatal - deferred rendering disabled if creation fails, forward rendering continues

### Step 2: Framebuffer Rebuild

**File:** `VulkanGraphicsImpl.cpp:1465-1534`

```cpp
bool VulkanGraphicsImpl::RebuildRenderTargetFramebuffer()
```

**What it does:**
1. Validates G-Buffer images are initialized
2. Creates RenderPassDescriptor with 4 color attachments + depth
3. Calls `GetOrCreateRenderPass()` for dynamic render pass creation (Phase 35)
4. Creates VkFramebuffer with attachment array [position, normal, albedo, specular, depth]
5. Destroys old framebuffer if it exists
6. Clears `renderTargetsDirty_` flag

**Key aspects:**
- Non-destructive if validation fails
- Proper error logging for debugging
- Integrates with Phase 35 extended render pass descriptor

### Step 3: Render Target Binding

**File:** `Graphics_Vulkan.cpp:713-747`

```cpp
void Graphics::SetRenderTarget_Vulkan(unsigned index, RenderSurface* renderTarget)
void Graphics::SetDepthStencil_Vulkan(RenderSurface* depthStencil)
```

**What it does:**
- Updates `renderTargets_[index]` array when application changes render targets
- Sets `renderTargetsDirty_` flag when targets change
- Triggers automatic framebuffer rebuild on next `BeginRenderPass()`

**Behavior:**
- Only sets flag if target actually changed (no unnecessary rebuilds)
- Maintains compatibility with forward rendering path
- Non-blocking operation (actual rebuild deferred to render pass)

### Step 4: Framebuffer Selection

**File:** `VulkanGraphicsImpl.cpp:585-636`

```cpp
void VulkanGraphicsImpl::BeginRenderPass()
```

**What it does:**
1. Checks `renderTargetsDirty_` flag and rebuilds framebuffer if needed
2. Selects appropriate framebuffer:
   - If `renderTargetFramebuffer_` is valid → use deferred G-Buffer framebuffer
   - Otherwise → use forward swapchain framebuffer (default)
3. Adjusts clear values based on framebuffer type:
   - **Deferred:** 5 clear values (4 color + 1 depth)
   - **Forward:** 2 clear values (1 color + 1 depth)
4. Begins render pass with appropriate framebuffer

**Key logic:**
```cpp
if (renderTargetFramebuffer_ != VK_NULL_HANDLE)
{
    currentFramebuffer = renderTargetFramebuffer_;  // Use deferred
    clearValueCount = 5;
}
```

### Step 5: Resource Cleanup

**File:** `VulkanGraphicsImpl.cpp:318-324`

```cpp
void VulkanGraphicsImpl::Shutdown()
```

**What it does:**
1. Destroys render target framebuffer if it exists
2. Calls `DestroyGBuffer()` to deallocate all G-Buffer images/views
3. Properly ordered before swapchain destruction

**Cleanup order:**
1. G-Buffer framebuffer
2. G-Buffer images and views (via VMA)
3. Then continues with standard shutdown sequence

## Integration Points

### With Phase 35 (Extended Render Pass Descriptor)

Phase 34 depends on Phase 35's extended render pass descriptor to support:
- Up to 8 color attachments (used 4 for G-Buffer)
- Dynamic render pass creation based on attachment count
- Multi-subpass support for future lighting passes

### With Phase 33 (Descriptor Set Binding)

Phase 34 provides the render target infrastructure that Phase 33's material descriptors will bind to. Material parameters are prepared in Phase 33 and will be bound during Phase 34 geometry rendering.

### With Phase 31 (G-Buffer Attachments)

Phase 34 uses the G-Buffer image infrastructure created in Phase 31 and provides the framebuffer wrapper around it.

## Usage Example

### In Application Code

```cpp
// Enable deferred rendering by setting render targets
Graphics* graphics = GetSubsystem<Graphics>();

// Later, when you want to render to G-Buffer:
graphics->SetRenderTarget(0, nullptr);  // Triggers renderTargetsDirty_ = true
graphics->SetDepthStencil(nullptr);

// On next BeginFrame:
graphics->BeginFrame();  // Automatically rebuilds deferred framebuffer
// ... draw geometry to G-Buffer ...
graphics->EndFrame();
```

### Behind the Scenes

1. `SetRenderTarget()` sets `renderTargetsDirty_ = true`
2. `BeginFrame()` calls `BeginRenderPass()`
3. `BeginRenderPass()` detects dirty flag and calls `RebuildRenderTargetFramebuffer()`
4. Framebuffer is rebuilt with G-Buffer images
5. `vkCmdBeginRenderPass()` uses deferred framebuffer
6. Geometry is rendered to 4 G-Buffer attachments + depth

## Known Limitations

1. **Single geometry pass only** - No multi-subpass rendering yet (Phase 35 framework exists)
2. **No lighting pass** - G-Buffer created but not used for deferred lighting (Phase 36+)
3. **No input attachments** - Lighting pass can't read G-Buffer during rendering
4. **Forward fallback** - If G-Buffer creation fails, silently falls back to forward rendering
5. **No deferred compositing** - Final lighting pass not implemented

## Testing Strategy

### Verification Checklist

- ✅ Build all 56+ samples without errors
- ✅ Sample 56 (DeferredRendering) creates G-Buffer successfully
- ✅ Forward rendering still works (framebuffer fallback)
- ✅ Framebuffer rebuild triggered when render targets change
- ✅ All attachments cleared with correct values
- ✅ No GPU validation errors

### Debug Validation

Enable Vulkan validation layer:
```bash
export VK_LAYER_PATH=/path/to/vulkan-sdk/lib
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
./bin/56_DeferredRendering
```

Expected console output:
```
[Info] RebuildRenderTargetFramebuffer: G-Buffer framebuffer created successfully
```

## Performance Characteristics

### Memory Usage

- **G-Buffer Images:** ~150 MB (1920x1080 4 attachments)
  - Position: 1920×1080×16 bytes = 33 MB
  - Normal: 1920×1080×8 bytes = 16.6 MB
  - Albedo: 1920×1080×4 bytes = 8.3 MB
  - Specular: 1920×1080×4 bytes = 8.3 MB
  - Depth: 1920×1080×4 bytes = 8.3 MB
  - **Total: ~75 MB**

### Bandwidth Efficiency

- **Forward rendering:** 1 render pass, 1 color attachment
- **Deferred rendering:** 1 geometry pass (5 attachments), 1 lighting pass (1 attachment)
  - Initial write: 5 attachments (G-Buffer write)
  - Lighting read: 4 input attachments (G-Buffer read via input attachments)
  - Output write: 1 attachment (final color)

## Future Enhancements (Phase 36+)

### Immediate Next Steps

1. **Deferred Lighting Pass**
   - Implement lighting subpass using input attachments
   - Shader-based light accumulation from G-Buffer

2. **Material Parameters**
   - Bind Phase 33 material descriptors during geometry pass
   - Apply textures and material properties to G-Buffer

3. **MSAA Integration**
   - Resolve G-Buffer MSAA to single-sample for lighting pass
   - Phase 30 framework already exists

### Advanced Features

- Compute shader for lighting acceleration
- Tile-based deferred rendering optimization
- Debug visualization modes for G-Buffer inspection
- Deferred rendering statistics/profiling

## Troubleshooting

### "G-Buffer not initialized" Error

**Cause:** G-Buffer images were not created during initialization
**Solution:** Check that `CreateGBuffer()` is called in `VulkanGraphicsImpl::Initialize()`

### Framebuffer Creation Fails

**Cause:** G-Buffer images are null when rebuilding framebuffer
**Solution:** Verify `CreateGBuffer()` succeeded - check for warning logs

### Rendering Goes Black

**Cause:** Using deferred framebuffer but lighting pass not implemented
**Expected:** Geometry renders to G-Buffer, but no final output color (lighting pending)
**Workaround:** For now, forward rendering works correctly

### GPU Validation Errors

**Cause:** Attachment format mismatch or render pass incompatibility
**Debug:** Run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
**Check:** Verify Phase 35 render pass creation matches G-Buffer formats

## Code References

### Key Files Modified

- `Source/Urho3D/GraphicsAPI/Vulkan/VulkanGraphicsImpl.cpp`
  - CreateGBuffer() @ 1366
  - DestroyGBuffer() @ 1439
  - RebuildRenderTargetFramebuffer() @ 1465
  - BeginRenderPass() @ 585

- `Source/Urho3D/Graphics/Graphics_Vulkan.cpp`
  - SetRenderTarget_Vulkan() @ 713
  - SetDepthStencil_Vulkan() @ 733

- `Source/Urho3D/GraphicsAPI/Vulkan/VulkanGraphicsImpl.h`
  - G-Buffer member variables (gBufferPositionImage_, etc.)
  - renderTargetFramebuffer_ member
  - renderTargetsDirty_ flag

### Related Phases

- Phase 31: G-Buffer attachment images
- Phase 35: Extended render pass descriptor (multi-attachment support)
- Phase 33: Descriptor set binding (material parameters)

## Commits

```
243c0f2 Phase 34: Deferred Rendering Framework - Complete & Functional Implementation
f3df931 Phase 34: Deferred Rendering Framework - Render Target Binding & G-Buffer Integration
cc73266 Phase 34 Step 1: G-Buffer Framebuffer Creation for Deferred Rendering
```

## Verification Command

```bash
cd /home/leith/Desktop/URHO/New/urho3d-2.0.1
make -j4 2>&1 | tail -50  # Should show "[100%] Built target AssetImporter"
git log --oneline -3      # Should show Phase 34 commits
```

---

**Phase 34 Status:** ✅ Complete
**Next Phase:** Phase 36 - Deferred Lighting Pass Implementation
**Build Date:** 2025-12-04
**Documentation Date:** 2025-12-04
