//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan-specific graphics implementations - Minimal version

#include "../Precompiled.h"
#include "../GraphicsAPI/Vulkan/VulkanGraphicsImpl.h"
#include "../GraphicsAPI/Vulkan/VulkanMaterialDescriptorManager.h"
#include "../GraphicsAPI/Vulkan/VulkanShaderModule.h"
#include "../GraphicsAPI/Vulkan/VulkanConstantBufferPool.h"
#include "Graphics.h"
#include "Geometry.h"
#include "../GraphicsAPI/Texture.h"
#include "../GraphicsAPI/RenderSurface.h"
#include "../GraphicsAPI/Shader.h"
#include "../GraphicsAPI/VertexBuffer.h"
#include "../Resource/ResourceCache.h"
#include "../IO/Log.h"
#include <SDL/SDL.h>

#ifdef URHO3D_VULKAN

namespace Urho3D
{

// ============================================
// Graphics Constructor & Initialization (Phase 3)
// ============================================

void Graphics::Constructor_Vulkan()
{
    URHO3D_LOGINFO("Vulkan graphics constructor called");
    impl_ = new VulkanGraphicsImpl();

    // Set shader paths (same as OpenGL - Vulkan uses GLSL too)
    shaderPath_ = "Shaders/GLSL/";
    shaderExtension_ = ".glsl";

    // Set default window position
    position_ = IntVector2(SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED);
    orientations_ = "LandscapeLeft LandscapeRight";
    apiName_ = "Vulkan";

    // Register Graphics library object factories
    RegisterGraphicsLibrary(context_);
}

bool Graphics::SetScreenMode_Vulkan(int width, int height, const ScreenModeParams& params, bool maximize)
{
    URHO3D_PROFILE(SetScreenMode_Vulkan);

    if (!impl_)
    {
        URHO3D_LOGERROR("Vulkan implementation not initialized");
        return false;
    }

    // Create SDL window with Vulkan support
    unsigned flags = SDL_WINDOW_SHOWN;
    if (params.resizable_)
        flags |= SDL_WINDOW_RESIZABLE;
    if (params.borderless_)
        flags |= SDL_WINDOW_BORDERLESS;
    if (!externalWindow_)
        flags |= SDL_WINDOW_VULKAN;
    if (params.fullscreen_)
        flags |= SDL_WINDOW_FULLSCREEN;
    if (maximize)
        flags |= SDL_WINDOW_MAXIMIZED;
    if (params.highDPI_)
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, orientations_.CString());

    if (!externalWindow_)
    {
        window_ = SDL_CreateWindow(windowTitle_.CString(), position_.x_, position_.y_, width, height, flags);
    }
    else
    {
        window_ = SDL_CreateWindowFrom(externalWindow_, 0);
    }

    if (!window_)
    {
        URHO3D_LOGERROR(String("Failed to create SDL window: ") + SDL_GetError());
        return false;
    }

    CreateWindowIcon();

    // Use sensible default size if width or height is 0 (1024x768 is standard default)
    int actualWidth = width;
    int actualHeight = height;
    if (width == 0 || height == 0)
    {
        // Query actual window size first in case maximize worked
        SDL_GetWindowSize(window_, &actualWidth, &actualHeight);

        // If still invalid (1x1 or 0x0), use default 1024x768
        if (actualWidth <= 1 || actualHeight <= 1)
        {
            actualWidth = 1024;
            actualHeight = 768;
            SDL_SetWindowSize(window_, actualWidth, actualHeight);
            URHO3D_LOGDEBUG(String("[VULKAN] Window size was invalid, set to default: ") + String(actualWidth) + "x" + String(actualHeight));
        }
        else
        {
            URHO3D_LOGDEBUG(String("[VULKAN] Window maximized to: ") + String(actualWidth) + "x" + String(actualHeight));
        }
    }

    // Initialize Vulkan implementation
    VulkanGraphicsImpl* vkImpl = static_cast<VulkanGraphicsImpl*>(impl_);
    if (!vkImpl->Initialize(this, window_, actualWidth, actualHeight))
    {
        URHO3D_LOGERROR("Failed to initialize Vulkan graphics implementation");
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    // Set Graphics dimensions so UI and viewports work correctly
    width_ = actualWidth;
    height_ = actualHeight;

    screenParams_ = params;

    URHO3D_LOGINFO(String("Vulkan window and swapchain created: ") + String(actualWidth) + "x" + String(actualHeight));

    // Notify subsystems that screen mode has changed (triggers Renderer::Initialize())
    OnScreenModeChanged();

    return true;
}

// ============================================
// Frame Lifecycle Methods (Phase 3)
// ============================================

/// \brief Begin frame rendering on Vulkan backend
/// \details Part of the frame lifecycle: BeginFrame() -> render scene -> EndFrame().
/// Performs the following operations in sequence:
///   1. Acquire next swapchain image from presentation engine
///   2. Wait for frame fence (ensures GPU isn't still rendering previous frame)
///   3. Reset command buffer for frame pipelining (triple-buffering)
///   4. Begin recording commands into command buffer
///   5. Begin render pass with color and depth attachments
///
/// **Frame Pipelining Context:**
/// Vulkan uses triple-buffering: Frame N renders while GPU processes Frame N-1,
/// and Frame N-2 completes presentation. This returns after step 5, ready for draw calls.
///
/// **Error Handling:**
/// Returns false on any critical failure (no swapchain image, command buffer error).
/// Application should skip rendering for this frame but continue with next frame.
///
/// \returns true if frame begun successfully, false on swapchain/command buffer error
bool Graphics::BeginFrame_Vulkan()
{
    URHO3D_LOGDEBUG("[VULKAN] BeginFrame_Vulkan called");

    if (!impl_)
    {
        URHO3D_LOGERROR("[VULKAN] BeginFrame_Vulkan: impl_ is null");
        return false;
    }

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGERROR("[VULKAN] BeginFrame_Vulkan: vkImpl is null");
        return false;
    }

    // Apply user-requested MSAA setting from screen parameters
    vkImpl->SetRequestedSampleCount(screenParams_.multiSample_);

    // Acquire next swapchain image (waits for frame fence internally)
    if (!vkImpl->AcquireNextImage())
    {
        URHO3D_LOGERROR("Failed to acquire next swapchain image");
        return false;
    }

    // DISABLED: Descriptor pool reset causes validation errors with triple buffering
    // (pool still in use by other frames in flight)
    // Solution: Increased VULKAN_DESCRIPTOR_POOL_SIZE to 50000 to avoid running out
    // TODO: Implement per-frame descriptor pools for proper solution
    // VkDescriptorPool descriptorPool = vkImpl->GetDescriptorPool();
    // if (descriptorPool)
    // {
    //     VkDevice device = vkImpl->GetDevice();
    //     if (device)
    //     {
    //         vkResetDescriptorPool(device, descriptorPool, 0);
    //         URHO3D_LOGDEBUG("BeginFrame_Vulkan: Descriptor pool reset (after fence wait)");
    //     }
    // }

    // Reset constant buffer pool allocations for this frame
    // Safe after AcquireNextImage() because it waits for this frame's fence
    VulkanConstantBufferPool* constantBufferPool = vkImpl->GetConstantBufferPool();
    if (constantBufferPool)
    {
        constantBufferPool->ResetFrameAllocations();
        URHO3D_LOGDEBUG("BeginFrame_Vulkan: Constant buffer pool reset");
    }

    // Reset command buffer for this frame
    vkImpl->ResetFrameCommandBuffer();

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
    {
        URHO3D_LOGERROR("Failed to get command buffer");
        return false;
    }

    // Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmdBuffer, &beginInfo) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to begin command buffer");
        return false;
    }

    // Begin render pass
    vkImpl->BeginRenderPass();

    return true;
}

/// \brief End frame rendering on Vulkan backend
/// \details Completes the frame lifecycle: BeginFrame() -> render scene -> EndFrame().
/// Performs the following operations in sequence:
///   1. End render pass (closes render pass recording)
///   2. End command buffer recording
///   3. Submit command buffer to graphics queue for execution
///   4. Present swapchain image to display (vsync)
///
/// **Synchronization:**
/// - Uses semaphores to synchronize GPU and presentation queue
/// - Uses fences to ensure CPU doesn't write to this frame's buffers until GPU finishes
/// - Advances frameIndex_ (0 -> 1 -> 2 -> 0) for triple-buffering
///
/// **Error Handling:**
/// Logs errors but continues execution (frame is skipped). Next frame will attempt
/// to recover. Critical failures (e.g., device lost) should trigger full re-initialization.
///
/// \returns void (errors logged, frame skipped silently)
void Graphics::EndFrame_Vulkan()
{
    URHO3D_LOGDEBUG("[VULKAN] EndFrame_Vulkan called");

    if (!impl_)
    {
        URHO3D_LOGERROR("[VULKAN] EndFrame_Vulkan: impl_ is null");
        return;
    }

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGERROR("[VULKAN] EndFrame_Vulkan: vkImpl is null");
        return;
    }

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // End render pass
    vkImpl->EndRenderPass();

    // End command buffer recording
    if (vkEndCommandBuffer(cmdBuffer) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to end command buffer");
        return;
    }

    // Submit and present
    vkImpl->Present();
}

// ============================================
// Viewport and Scissor (Phase 3)
// ============================================

/// \brief Set viewport transformation on Vulkan backend
/// \param x Viewport origin X coordinate in pixels
/// \param y Viewport origin Y coordinate in pixels
/// \param width Viewport width in pixels
/// \param height Viewport height in pixels
///
/// \details Records a vkCmdSetViewport command to the current frame's command buffer.
/// Viewport defines the transformation from normalized device coordinates (-1 to +1)
/// to screen space. Must be called after BeginFrame() and before draw calls.
///
/// **Typical Usage:**
/// - Called once per frame with full window dimensions
/// - Can be changed between draw calls for multiple viewports (split-screen, etc.)
/// - Depth range fixed at minDepth=0.0, maxDepth=1.0 (standard OpenGL mapping)
///
/// **Note:** Vulkan viewports use inverted Y-axis compared to OpenGL (Y points down).
/// Urho3D handles this inversion transparently via projection matrix.
///
/// \returns void (errors silently skipped if command buffer unavailable)
void Graphics::SetViewport_Vulkan(int x, int y, int width, int height)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Validate and fix invalid viewport dimensions (like OpenGL backend does)
    if (width <= 0 || height <= 0)
    {
        IntVector2 rtSize = GetRenderTargetDimensions();
        if (rtSize.x_ > 0 && rtSize.y_ > 0)
        {
            width = rtSize.x_;
            height = rtSize.y_;
        }
        else
        {
            // Last resort: use swapchain dimensions
            width = vkImpl->GetSwapchainExtent().width;
            height = vkImpl->GetSwapchainExtent().height;
        }
        x = 0;
        y = 0;
    }

    // Store viewport for later use (needed by Draw_Vulkan for scissor rect)
    viewport_ = IntRect(x, y, x + width, y + height);

    VkViewport viewport{};
    viewport.x = static_cast<float>(x);
    viewport.y = static_cast<float>(y);
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

    URHO3D_LOGDEBUG("SetViewport_Vulkan: x=" + String(viewport.x) + ", y=" + String(viewport.y) +
                    ", w=" + String(viewport.width) + ", h=" + String(viewport.height) +
                    ", depth=[" + String(viewport.minDepth) + "," + String(viewport.maxDepth) + "]");
}

/// \brief Set scissor rectangle on Vulkan backend
/// \param x Scissor rectangle origin X coordinate in pixels
/// \param y Scissor rectangle origin Y coordinate in pixels
/// \param width Scissor rectangle width in pixels
/// \param height Scissor rectangle height in pixels
///
/// \details Records a vkCmdSetScissor command to the current frame's command buffer.
/// Scissor defines a rectangle beyond which fragments are discarded before rasterization.
/// Must be called after BeginFrame() and before draw calls.
///
/// **Typical Usage:**
/// - Called once per frame with full window dimensions
/// - Can be changed between draw calls to restrict rasterization to specific regions
/// - Used for UI rendering (clip to UI element bounds)
/// - Used for split-screen rendering (different scissor per viewport)
///
/// **Coordinate System:**
/// - Origin (0,0) is top-left corner
/// - X increases rightward, Y increases downward (standard screen coordinates)
/// - Rectangle must fit within render target dimensions
///
/// \returns void (errors silently skipped if command buffer unavailable)
void Graphics::SetScissor_Vulkan(int x, int y, int width, int height)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    VkRect2D scissor{};
    scissor.offset = {x, y};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
}

// ============================================
// Clear Screen (Phase 3)
// ============================================

void Graphics::Clear_Vulkan(ClearTargetFlags flags, const Color& color, float depth, unsigned stencil)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Clear color and/or depth based on flags
    // This will be fully implemented in Phase 4
    URHO3D_LOGDEBUG("Clear_Vulkan called");
}

// ============================================
// Phase 27: Descriptor Binding and Rendering
// ============================================

VkDescriptorSet Graphics::CreateReflectionBasedDescriptorSet_Vulkan()
{
    // Phase 36 Step 5: Create descriptor set from reflection-based layout
    // Allocates descriptor set using the layout created from SPIR-V reflection
    // This ensures descriptor set layout matches the pipeline layout

    URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: ENTRY");

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: vkImpl is NULL");
        return VK_NULL_HANDLE;
    }

    VkDevice device = vkImpl->GetDevice();
    if (!device)
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: device is NULL");
        return VK_NULL_HANDLE;
    }

    // Get the reflection-based descriptor set layout (set by Draw functions)
    VkDescriptorSetLayout descriptorSetLayout = vkImpl->GetCurrentDescriptorSetLayout();
    if (descriptorSetLayout == VK_NULL_HANDLE)
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: No reflection-based layout available (not set yet)");
        return VK_NULL_HANDLE;
    }

    URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: Using reflection-based layout");

    // Allocate descriptor set from pool using reflection-based layout
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkImpl->GetDescriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout;

    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS)
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: Failed to allocate descriptor set (result=" +
                        String((int)result) + ")");
        return VK_NULL_HANDLE;
    }

    // Build descriptor writes for uniform buffers and textures
    // STEP 5: Full implementation including uniform buffers
    // CRITICAL: Reserve space to prevent vector reallocation invalidating pointers
    Vector<VkDescriptorImageInfo> imageInfos;
    Vector<VkDescriptorBufferInfo> bufferInfos;
    Vector<VkWriteDescriptorSet> writes;
    imageInfos.Reserve(MAX_TEXTURE_UNITS);
    bufferInfos.Reserve(11);  // 11 uniform buffer bindings (0-10)
    writes.Reserve(11 + MAX_TEXTURE_UNITS);

    // Get constant buffer for uniform buffer descriptors
    VkBuffer constantBuffer = vkImpl->GetCurrentConstantBuffer();
    size_t constantBufferSize = vkImpl->GetCurrentConstantBufferSize();

    // Bind uniform buffers (bindings 0-10 per preprocessing scheme)
    // Use per-block offsets from currentBlockOffsets_ for correct std140 layout
    if (constantBuffer != VK_NULL_HANDLE && constantBufferSize > 0)
    {
        // Create descriptor for each block that was actually uploaded
        // Each binding points to its specific offset within the concatenated buffer
        for (auto it = currentBlockOffsets_.Begin(); it != currentBlockOffsets_.End(); ++it)
        {
            unsigned binding = it->first_;
            size_t offset = it->second_;
            size_t size = currentBlockSizes_[binding];

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = constantBuffer;
            bufferInfo.offset = offset;  // Correct offset for this uniform block
            bufferInfo.range = size;     // Size of this uniform block
            bufferInfos.Push(bufferInfo);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = binding;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &bufferInfos.Back();
            writes.Push(write);
        }

        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: Added " + String(currentBlockOffsets_.Size()) +
                        " uniform buffer bindings with per-block offsets");
    }
    else
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: No constant buffer available");
    }

    // Bind textures (bindings 100+ per preprocessing scheme)
    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        Texture* texture = textures_[i];
        if (!texture)
            continue;

        VkImageView imageView = texture->GetVkImageView();
        VkSampler sampler = static_cast<VkSampler>(texture->GetSampler_Vulkan());

        URHO3D_LOGDEBUG("Texture unit " + String(i) + " -> binding " + String(100 + i) +
                        ": imageView=" + String((unsigned long long)imageView) +
                        " sampler=" + String((unsigned long long)sampler) +
                        " name=" + texture->GetName());

        if (!imageView || !sampler)
        {
            URHO3D_LOGWARNING("Texture unit " + String(i) + " has null imageView or sampler, skipping");
            continue;
        }

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.sampler = sampler;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.Push(imageInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 100 + i;  // Samplers start at binding 100 per preprocessing
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos.Back();
        writes.Push(write);
    }

    // Update descriptor set with uniform buffer and texture bindings
    if (!writes.Empty())
    {
        vkUpdateDescriptorSets(device, writes.Size(), &writes[0], 0, nullptr);
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: Updated " + String(writes.Size()) +
                        " descriptors (uniform buffers + textures)");
    }
    else
    {
        URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: No descriptors to bind");
    }

    URHO3D_LOGDEBUG("CreateReflectionBasedDescriptorSet_Vulkan: Successfully created descriptor set");
    return descriptorSet;
}

bool Graphics::BindMaterialDescriptors_Vulkan(Material* material) const
{
    // Phase 27A.1: Material descriptor binding for GPU access
    // Binds material descriptor sets (textures, samplers, parameters) before draw calls
    // Returns false if descriptors unavailable, true on success
    // Phase 36 Step 5: Now uses reflection-based descriptor sets instead of hardcoded layouts
    // material parameter is optional - binds textures from textures_[] array

    URHO3D_LOGDEBUG("BindMaterialDescriptors_Vulkan: ENTRY");

    if (!impl_)
    {
        URHO3D_LOGDEBUG("BindMaterialDescriptors_Vulkan: impl_ is NULL");
        return false;
    }

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGDEBUG("BindMaterialDescriptors_Vulkan: vkImpl is NULL");
        return false;
    }

    // Get command buffer for recording descriptor binding commands
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
    {
        URHO3D_LOGDEBUG("BindMaterialDescriptors_Vulkan: cmdBuffer is NULL");
        return false;
    }

    URHO3D_LOGDEBUG("BindMaterialDescriptors_Vulkan: About to call CreateReflectionBasedDescriptorSet");

    // STEP 5: Create descriptor set using reflection-based layout
    // This replaces the hardcoded VulkanMaterialDescriptorManager approach
    VkDescriptorSet descriptorSet = const_cast<Graphics*>(this)->CreateReflectionBasedDescriptorSet_Vulkan();
    if (descriptorSet == VK_NULL_HANDLE)
    {
        URHO3D_LOGWARNING("BindMaterialDescriptors_Vulkan: Failed to create reflection-based descriptor set");
        return false;
    }

    // Phase 27A.2: Bind descriptor set for fragment shader textures and samplers
    // Descriptor set 0: Material textures, samplers, and material parameters
    // Pipeline layout is already set by Draw functions via SetCurrentPipelineLayout()
    VkPipelineLayout pipelineLayout = vkImpl->GetCurrentPipelineLayout();
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        URHO3D_LOGDEBUG("No pipeline layout available for descriptor binding");
        return false;
    }

    // Bind descriptor set to graphics pipeline
    vkCmdBindDescriptorSets(
        cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,  // firstSet
        1,  // descriptorSetCount
        &descriptorSet,
        0,  // dynamicOffsetCount
        nullptr  // pDynamicOffsets
    );

    URHO3D_LOGDEBUG("Reflection-based material descriptors bound successfully");
    return true;
}

VkDescriptorSet Graphics::CreateGBufferTextureDescriptorSet_Vulkan()
{
    // Phase 36 Step 2: G-Buffer texture descriptor creation for deferred lighting
    // Creates descriptor set containing currently bound textures for lighting pass shader access
    // Descriptor set layout: Set 1, bindings 0-7 for up to 8 texture units

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return VK_NULL_HANDLE;

    VkDevice device = vkImpl->GetDevice();
    if (!device)
        return VK_NULL_HANDLE;

    // Use the pre-created G-Buffer texture layout from VulkanGraphicsImpl
    VkDescriptorSetLayout textureLayout = vkImpl->GetGBufferTextureLayout();
    if (textureLayout == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("CreateGBufferTextureDescriptorSet_Vulkan: G-Buffer texture layout not initialized");
        return VK_NULL_HANDLE;
    }

    // Allocate descriptor set from pool
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkImpl->GetDescriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &textureLayout;

    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS)
    {
        // Descriptor pool exhausted - this is expected behavior, just skip binding
        // The draw will proceed without texture descriptors (may show black)
        URHO3D_LOGDEBUG("CreateGBufferTextureDescriptorSet_Vulkan: Descriptor pool exhausted (result=" +
                        String((int)result) + "), skipping texture binding");
        return VK_NULL_HANDLE;
    }

    // Build descriptor writes for all bound textures
    Vector<VkDescriptorImageInfo> imageInfos;
    Vector<VkWriteDescriptorSet> writes;

    for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        Texture* texture = textures_[i];
        if (!texture)
            continue;

        VkImageView imageView = texture->GetVkImageView();
        VkSampler sampler = static_cast<VkSampler>(texture->GetSampler_Vulkan());

        if (!imageView || !sampler)
            continue;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.sampler = sampler;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.Push(imageInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = i;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos.Back();
        writes.Push(write);
    }

    // Update descriptor set with texture bindings
    if (!writes.Empty())
    {
        vkUpdateDescriptorSets(device, writes.Size(), &writes[0], 0, nullptr);
        URHO3D_LOGDEBUG("CreateGBufferTextureDescriptorSet_Vulkan: Bound " + String(writes.Size()) + " textures");
    }

    return descriptorSet;
}

bool Graphics::BindGBufferTextureDescriptors_Vulkan()
{
    // Phase 36 Step 3: Bind G-Buffer texture descriptors for deferred lighting
    // Binds texture descriptor set to slot 1 for shader access during lighting pass

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return false;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    VkPipelineLayout pipelineLayout = vkImpl->GetCurrentPipelineLayout();

    if (!cmdBuffer || !pipelineLayout)
    {
        URHO3D_LOGWARNING("BindGBufferTextureDescriptors_Vulkan: Command buffer or pipeline layout not available");
        return false;
    }

    // Create descriptor set from current texture bindings
    VkDescriptorSet descriptorSet = CreateGBufferTextureDescriptorSet_Vulkan();
    if (descriptorSet == VK_NULL_HANDLE)
    {
        URHO3D_LOGDEBUG("BindGBufferTextureDescriptors_Vulkan: No textures bound, skipping");
        return true;  // Not an error if no textures are bound
    }

    // Bind to descriptor set slot 1 (after materials at 0)
    vkCmdBindDescriptorSets(
        cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        1,  // Set slot 1 for G-Buffer textures
        1,
        &descriptorSet,
        0, nullptr
    );

    URHO3D_LOGDEBUG("BindGBufferTextureDescriptors_Vulkan: Bound texture descriptors to set 1");
    return true;
}

void Graphics::SetVertexBuffer_Vulkan(unsigned index, VertexBuffer* buffer)
{
    if (!impl_ || index >= MAX_VERTEX_STREAMS)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Store for pipeline vertex input state configuration
    vertexBuffers_[index] = buffer;

    if (buffer)
    {
        VkBuffer vkBuffer = static_cast<VkBuffer>(buffer->GetGPUObject());
        VkDeviceSize offset = 0;

        if (vkBuffer)
        {
            URHO3D_LOGDEBUG("SetVertexBuffer_Vulkan: Binding vertex buffer at index " + String(index) + ", buffer=" + String((unsigned long long)vkBuffer));
            vkCmdBindVertexBuffers(cmdBuffer, index, 1, &vkBuffer, &offset);
            URHO3D_LOGDEBUG("SetVertexBuffer_Vulkan: vkCmdBindVertexBuffers completed");
        }
        else
        {
            URHO3D_LOGDEBUG("SetVertexBuffer_Vulkan: vkBuffer is NULL for buffer at index " + String(index));
        }
    }
    else
    {
        URHO3D_LOGDEBUG("SetVertexBuffer_Vulkan: buffer parameter is NULL for index " + String(index));
    }
}

bool Graphics::SetVertexBuffers_Vulkan(const Vector<VertexBuffer*>& buffers, unsigned instanceOffset)
{
    if (buffers.Empty())
        return true;

    URHO3D_LOGDEBUG("SetVertexBuffers_Vulkan: Setting " + String(buffers.Size()) + " vertex buffers");

    for (unsigned i = 0; i < buffers.Size(); ++i)
    {
        SetVertexBuffer_Vulkan(i, buffers[i]);
    }

    return true;
}

bool Graphics::SetVertexBuffers_Vulkan(const Vector<SharedPtr<VertexBuffer>>& buffers, unsigned instanceOffset)
{
    if (buffers.Empty())
        return true;

    URHO3D_LOGDEBUG("SetVertexBuffers_Vulkan: Setting " + String(buffers.Size()) + " vertex buffers (SharedPtr)");

    for (unsigned i = 0; i < buffers.Size(); ++i)
    {
        SetVertexBuffer_Vulkan(i, buffers[i]);
    }

    return true;
}

void Graphics::Draw_Vulkan(Geometry* geometry, Material* material)
{
    // Phase 27B + Phase 36: Render command recording with full descriptor binding
    // Records geometry draw command with material + texture descriptors bound
    // Integrates Phases 17-26 descriptor pipeline + Phase 36 G-Buffer textures

    if (!geometry || geometry->IsEmpty() || !material)
        return;

    // STEP 5 FIX: Descriptor binding now happens in primitive Draw functions after shader compilation
    // This wrapper just forwards to geometry->Draw() which eventually calls the primitive Draw functions

    // STEP 5 FIX: Disabled - reflection-based layout binds everything in set 0
    // // Bind G-Buffer texture descriptors for deferred lighting (Set 1)
    // // This provides access to G-Buffer textures during lighting pass
    // if (!BindGBufferTextureDescriptors_Vulkan())
    // {
    //     URHO3D_LOGDEBUG("Failed to bind G-Buffer texture descriptors");
    //     // Non-fatal - forward rendering doesn't need G-Buffer textures
    // }

    // Upload pending shader parameters (Set 2) - light parameters for deferred rendering
    // This uploads constant buffers containing light position, color, intensity, etc.
    UploadPendingShaderParameters_Vulkan();

    // Bind input attachment descriptors for lighting subpass (Set 3) - tile-local optimization
    // This provides fast access to G-Buffer attachments during deferred lighting
    VkDescriptorSet inputAttachmentSet = CreateInputAttachmentDescriptorSet_Vulkan();
    if (inputAttachmentSet != VK_NULL_HANDLE)
    {
        BindInputAttachmentDescriptors_Vulkan(inputAttachmentSet);
    }

    // Issue actual draw command via geometry
    geometry->Draw(this);
}

// ============================================
// Draw implementation methods (Phase 3B - Immediate Rendering)
// ============================================

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned vertexStart, unsigned vertexCount)
{
    if (!impl_ || vertexCount == 0)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    // CRITICAL: Verify render pass is active before recording draw commands
    if (!vkImpl->IsRenderPassActive())
    {
        URHO3D_LOGERROR("Draw_Vulkan: Cannot draw - render pass is not active!");
        return;
    }

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules (Phase 36+: Geometry Shader Support)
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;
    VkShaderModule gsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule,
                                     geometryShader_, geometryShader_ ? &gsModule : nullptr))
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to create shader modules");
        return;
    }

    // REFLECTION-BASED LAYOUT: Create descriptor set layout dynamically from reflected SPIR-V bindings
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    // Get reflected resources from compiled shaders
    const Vector<SPIRVResource>& vsResources = vertexShader_->GetReflectedResources();
    const Vector<SPIRVResource>& psResources = pixelShader_->GetReflectedResources();

    // Create layout from reflection (replaces hardcoded layout)
    descriptorSetLayout = vkImpl->CreateReflectionBasedLayout(vsResources, psResources);

    if (descriptorSetLayout)
    {
        // Create pipeline layout with reflected descriptor set layout
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;

        if (vkCreatePipelineLayout(vkImpl->GetDevice(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Draw_Vulkan: Failed to create pipeline layout from reflection");
            layout = VK_NULL_HANDLE;
        }
        else
        {
            URHO3D_LOGDEBUG("Draw_Vulkan: Created reflection-based pipeline layout");

            URHO3D_LOGINFO("[DEBUG] About to call SetCurrentDescriptorSetLayout");
            // STEP 5: Store layouts in VulkanGraphicsImpl for descriptor set allocation
            vkImpl->SetCurrentDescriptorSetLayout(descriptorSetLayout);
            URHO3D_LOGINFO("[DEBUG] About to call SetCurrentPipelineLayout");
            vkImpl->SetCurrentPipelineLayout(layout);
            URHO3D_LOGINFO("[DEBUG] Finished setting current layouts");

            URHO3D_LOGINFO("[DEBUG] About to call UploadPendingShaderParameters_Vulkan");
            // STEP 5: Upload pending shader parameters BEFORE creating descriptor sets
            // This ensures constant buffer is available for uniform buffer descriptors
            UploadPendingShaderParameters_Vulkan();
            URHO3D_LOGINFO("[DEBUG] After UploadPendingShaderParameters_Vulkan");

            // STEP 5: Descriptor binding moved to after pipeline binding (Vulkan requirement)
            // See after vkCmdBindPipeline for BindMaterialDescriptors_Vulkan() call
        }
    }

    URHO3D_LOGINFO("[DEBUG] Before fallback check, layout=" + String((unsigned long long)layout));
    // FALLBACK: Use hardcoded layout if reflection failed
    if (!layout)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Falling back to hardcoded pipeline layout");
        layout = vkImpl->GetCurrentPipelineLayout();
    }

    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Invalid pipeline layout or render pass");
        // Clean up shader modules on error
        if (vsModule)
            vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule)
            vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    URHO3D_LOGINFO("[PIPELINE] About to call GetOrCreateGraphicsPipeline");
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vertexBuffers_[0], vsModule, fsModule, gsModule);
    URHO3D_LOGINFO(String("[PIPELINE] GetOrCreateGraphicsPipeline returned: ") + String((unsigned long long)pipeline));
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        // Clean up shader modules on error
        if (vsModule)
            vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule)
            vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    URHO3D_LOGINFO(String("[PIPELINE] About to bind pipeline: ") + String((unsigned long long)pipeline));
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    URHO3D_LOGINFO("[PIPELINE] vkCmdBindPipeline completed");

    // Set viewport and scissor (must be set after pipeline binding for dynamic state)
    VkViewport viewport{};
    viewport.x = static_cast<float>(viewport_.left_);
    viewport.y = static_cast<float>(viewport_.top_);
    viewport.width = static_cast<float>(viewport_.Width());
    viewport.height = static_cast<float>(viewport_.Height());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    // Debug: Check viewport dimensions
    if (viewport.width <= 0 || viewport.height <= 0)
    {
        fprintf(stderr, "WARNING: Invalid viewport dimensions: x=%f y=%f w=%f h=%f\n",
                viewport.x, viewport.y, viewport.width, viewport.height);
        // Use default viewport if invalid
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = 1024.0f;
        viewport.height = 768.0f;
    }

    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

    URHO3D_LOGDEBUG("Draw_Vulkan(vertexDraw): Viewport: x=" + String(viewport.x) + ", y=" + String(viewport.y) +
                    ", w=" + String(viewport.width) + ", h=" + String(viewport.height) +
                    ", depth=[" + String(viewport.minDepth) + "," + String(viewport.maxDepth) + "]");

    VkRect2D scissor{};
    scissor.offset.x = viewport_.left_;
    scissor.offset.y = viewport_.top_;
    scissor.extent.width = viewport_.Width();
    scissor.extent.height = viewport_.Height();

    // CRITICAL FIX: Validate scissor dimensions (zero-sized scissor clips all fragments)
    if (scissor.extent.width == 0 || scissor.extent.height == 0)
    {
        URHO3D_LOGWARNING("Scissor rectangle has zero dimensions, using swapchain extent");
        auto* vkImpl = static_cast<VulkanGraphicsImpl*>(impl_);
        VkExtent2D swapchainExtent = vkImpl->GetSwapchainExtent();
        scissor.extent.width = swapchainExtent.width;
        scissor.extent.height = swapchainExtent.height;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
    }
    URHO3D_LOGDEBUG("Scissor rect: offset=(" + String(scissor.offset.x) + "," +
                    String(scissor.offset.y) + "), extent=" +
                    String(scissor.extent.width) + "x" + String(scissor.extent.height));

    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

    // STEP 5 FIX: Disabled - reflection-based layout binds everything in set 0
    // // Bind texture descriptors (slot 1) if any textures are set
    // BindGBufferTextureDescriptors_Vulkan();

    // Phase 36 Step 4: Upload is now done before BindMaterialDescriptors_Vulkan() (see above)
    // UploadPendingShaderParameters_Vulkan();

    // STEP 5 FIX: Bind material descriptors AFTER pipeline binding (Vulkan requirement)
    // Descriptor sets must be bound after vkCmdBindPipeline to remain valid
    if (!BindMaterialDescriptors_Vulkan(nullptr))
    {
        URHO3D_LOGDEBUG("Failed to bind reflection-based material descriptors");
    }

    // Record draw command
    URHO3D_LOGDEBUG("About to call vkCmdDraw: vertexCount=" + String(vertexCount) + ", vertexStart=" + String(vertexStart));
    vkCmdDraw(cmdBuffer, vertexCount, 1, vertexStart, 0);
    URHO3D_LOGDEBUG("vkCmdDraw completed successfully");

    // Clean up shader modules after pipeline is created (pipeline retains a reference)
    if (vsModule)
        vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule)
        vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
}

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    if (!impl_ || indexCount == 0)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    // CRITICAL: Verify render pass is active before recording draw commands
    if (!vkImpl->IsRenderPassActive())
    {
        URHO3D_LOGERROR("Draw_Vulkan(indexed): Cannot draw - render pass is not active!");
        return;
    }

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules (Phase 36+: Geometry Shader Support)
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;
    VkShaderModule gsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule,
                                     geometryShader_, geometryShader_ ? &gsModule : nullptr))
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to create shader modules");
        return;
    }

    // REFLECTION-BASED LAYOUT: Create descriptor set layout dynamically from reflected SPIR-V bindings
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    // Get reflected resources from compiled shaders
    const Vector<SPIRVResource>& vsResources = vertexShader_->GetReflectedResources();
    const Vector<SPIRVResource>& psResources = pixelShader_->GetReflectedResources();

    // Create layout from reflection (replaces hardcoded layout)
    descriptorSetLayout = vkImpl->CreateReflectionBasedLayout(vsResources, psResources);

    if (descriptorSetLayout)
    {
        // Create pipeline layout with reflected descriptor set layout
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;

        if (vkCreatePipelineLayout(vkImpl->GetDevice(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Draw_Vulkan: Failed to create pipeline layout from reflection");
            layout = VK_NULL_HANDLE;
        }
        else
        {
            URHO3D_LOGDEBUG("Draw_Vulkan: Created reflection-based pipeline layout");

            URHO3D_LOGINFO("[DEBUG] About to call SetCurrentDescriptorSetLayout");
            // STEP 5: Store layouts in VulkanGraphicsImpl for descriptor set allocation
            vkImpl->SetCurrentDescriptorSetLayout(descriptorSetLayout);
            URHO3D_LOGINFO("[DEBUG] About to call SetCurrentPipelineLayout");
            vkImpl->SetCurrentPipelineLayout(layout);
            URHO3D_LOGINFO("[DEBUG] Finished setting current layouts");

            URHO3D_LOGINFO("[DEBUG] About to call UploadPendingShaderParameters_Vulkan");
            // STEP 5: Upload pending shader parameters BEFORE creating descriptor sets
            // This ensures constant buffer is available for uniform buffer descriptors
            UploadPendingShaderParameters_Vulkan();
            URHO3D_LOGINFO("[DEBUG] After UploadPendingShaderParameters_Vulkan");

            // STEP 5: Descriptor binding moved to after pipeline binding (Vulkan requirement)
            // See after vkCmdBindPipeline for BindMaterialDescriptors_Vulkan() call
        }
    }

    URHO3D_LOGINFO("[DEBUG] Before fallback check, layout=" + String((unsigned long long)layout));
    // FALLBACK: Use hardcoded layout if reflection failed
    if (!layout)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Falling back to hardcoded pipeline layout");
        layout = vkImpl->GetCurrentPipelineLayout();
    }

    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Invalid pipeline layout or render pass");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vertexBuffers_[0], vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    URHO3D_LOGINFO(String("[PIPELINE] About to bind pipeline: ") + String((unsigned long long)pipeline));
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    URHO3D_LOGINFO("[PIPELINE] vkCmdBindPipeline completed");

    // Set viewport and scissor (must be set after pipeline binding for dynamic state)
    VkViewport viewport{};
    viewport.x = static_cast<float>(viewport_.left_);
    viewport.y = static_cast<float>(viewport_.top_);
    viewport.width = static_cast<float>(viewport_.Width());
    viewport.height = static_cast<float>(viewport_.Height());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    // Debug: Check viewport dimensions
    if (viewport.width <= 0 || viewport.height <= 0)
    {
        fprintf(stderr, "WARNING: Invalid viewport dimensions: x=%f y=%f w=%f h=%f\n",
                viewport.x, viewport.y, viewport.width, viewport.height);
        // Use default viewport if invalid
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = 1024.0f;
        viewport.height = 768.0f;
    }

    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

    URHO3D_LOGDEBUG("Draw_Vulkan(indexedDraw): Viewport: x=" + String(viewport.x) + ", y=" + String(viewport.y) +
                    ", w=" + String(viewport.width) + ", h=" + String(viewport.height) +
                    ", depth=[" + String(viewport.minDepth) + "," + String(viewport.maxDepth) + "]");

    VkRect2D scissor{};
    scissor.offset.x = viewport_.left_;
    scissor.offset.y = viewport_.top_;
    scissor.extent.width = viewport_.Width();
    scissor.extent.height = viewport_.Height();

    // CRITICAL FIX: Validate scissor dimensions (zero-sized scissor clips all fragments)
    if (scissor.extent.width == 0 || scissor.extent.height == 0)
    {
        URHO3D_LOGWARNING("Scissor rectangle has zero dimensions, using swapchain extent");
        auto* vkImpl = static_cast<VulkanGraphicsImpl*>(impl_);
        VkExtent2D swapchainExtent = vkImpl->GetSwapchainExtent();
        scissor.extent.width = swapchainExtent.width;
        scissor.extent.height = swapchainExtent.height;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
    }
    URHO3D_LOGDEBUG("Scissor rect: offset=(" + String(scissor.offset.x) + "," +
                    String(scissor.offset.y) + "), extent=" +
                    String(scissor.extent.width) + "x" + String(scissor.extent.height));

    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

    // STEP 5 FIX: Disabled - reflection-based layout binds everything in set 0
    // // Bind texture descriptors (slot 1) if any textures are set
    // BindGBufferTextureDescriptors_Vulkan();

    // Phase 36 Step 4: Upload is now done before BindMaterialDescriptors_Vulkan() (see above)
    // UploadPendingShaderParameters_Vulkan();

    // STEP 5 FIX: Bind material descriptors AFTER pipeline binding (Vulkan requirement)
    // Descriptor sets must be bound after vkCmdBindPipeline to remain valid
    if (!BindMaterialDescriptors_Vulkan(nullptr))
    {
        URHO3D_LOGDEBUG("Failed to bind reflection-based material descriptors");
    }

    // Record indexed draw command
    URHO3D_LOGDEBUG("About to call vkCmdDrawIndexed: indexCount=" + String(indexCount) + ", indexStart=" + String(indexStart) + ", minVertex=" + String(minVertex));
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, minVertex, 0);
    URHO3D_LOGDEBUG("vkCmdDrawIndexed completed successfully");

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
}

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned baseVertexIndex, unsigned minVertex, unsigned vertexCount)
{
    if (!impl_ || indexCount == 0)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    // CRITICAL: Verify render pass is active before recording draw commands
    if (!vkImpl->IsRenderPassActive())
    {
        URHO3D_LOGERROR("Draw_Vulkan(indexed+base): Cannot draw - render pass is not active!");
        return;
    }

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules (Phase 36+: Geometry Shader Support)
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;
    VkShaderModule gsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule,
                                     geometryShader_, geometryShader_ ? &gsModule : nullptr))
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to create shader modules");
        return;
    }

    // Get pipeline layout and render pass
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Invalid pipeline layout or render pass");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Get or create graphics pipeline WITH shader modules
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vertexBuffers_[0], vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    URHO3D_LOGINFO(String("[PIPELINE] About to bind pipeline: ") + String((unsigned long long)pipeline));
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    URHO3D_LOGINFO("[PIPELINE] vkCmdBindPipeline completed");

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

    // STEP 5 FIX: Bind material descriptors AFTER pipeline binding (Vulkan requirement)
    // Descriptor sets must be bound after vkCmdBindPipeline to remain valid
    if (!BindMaterialDescriptors_Vulkan(nullptr))
    {
        URHO3D_LOGDEBUG("Failed to bind reflection-based material descriptors");
    }

    // Record draw command with base vertex index
    URHO3D_LOGDEBUG("About to call vkCmdDrawIndexed: indexCount=" + String(indexCount) + ", indexStart=" + String(indexStart) + ", baseVertexIndex=" + String(baseVertexIndex));
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, baseVertexIndex, 0);
    URHO3D_LOGDEBUG("vkCmdDrawIndexed completed successfully");

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("Draw_Vulkan: indexStart=" + String(indexStart) + " indexCount=" + String(indexCount) + " baseVertexIndex=" + String(baseVertexIndex));
}

void Graphics::DrawInstanced_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount, unsigned instanceCount)
{
    if (!impl_ || indexCount == 0 || instanceCount == 0)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules (Phase 36+: Geometry Shader Support)
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;
    VkShaderModule gsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule,
                                     geometryShader_, geometryShader_ ? &gsModule : nullptr))
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to create shader modules");
        return;
    }

    // Get pipeline layout and render pass
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Invalid pipeline layout or render pass");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Get or create graphics pipeline WITH shader modules
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vertexBuffers_[0], vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    URHO3D_LOGINFO(String("[PIPELINE] About to bind pipeline: ") + String((unsigned long long)pipeline));
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    URHO3D_LOGINFO("[PIPELINE] vkCmdBindPipeline completed");

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

    // STEP 5 FIX: Bind material descriptors AFTER pipeline binding (Vulkan requirement)
    // Descriptor sets must be bound after vkCmdBindPipeline to remain valid
    if (!BindMaterialDescriptors_Vulkan(nullptr))
    {
        URHO3D_LOGDEBUG("Failed to bind reflection-based material descriptors");
    }

    // Record instanced draw command
    URHO3D_LOGDEBUG("About to call vkCmdDrawIndexed (instanced): indexCount=" + String(indexCount) + ", instanceCount=" + String(instanceCount) + ", indexStart=" + String(indexStart) + ", minVertex=" + String(minVertex));
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, minVertex, 0);
    URHO3D_LOGDEBUG("vkCmdDrawIndexed (instanced) completed successfully");

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
}

void Graphics::DrawInstanced_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned baseVertexIndex, unsigned minVertex, unsigned vertexCount, unsigned instanceCount)
{
    if (!impl_ || indexCount == 0 || instanceCount == 0)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules (Phase 36+: Geometry Shader Support)
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;
    VkShaderModule gsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule,
                                     geometryShader_, geometryShader_ ? &gsModule : nullptr))
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to create shader modules");
        return;
    }

    // Get pipeline layout and render pass
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Invalid pipeline layout or render pass");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Get or create graphics pipeline WITH shader modules
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vertexBuffers_[0], vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    URHO3D_LOGINFO(String("[PIPELINE] About to bind pipeline: ") + String((unsigned long long)pipeline));
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    URHO3D_LOGINFO("[PIPELINE] vkCmdBindPipeline completed");

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

    // STEP 5 FIX: Bind material descriptors AFTER pipeline binding (Vulkan requirement)
    // Descriptor sets must be bound after vkCmdBindPipeline to remain valid
    if (!BindMaterialDescriptors_Vulkan(nullptr))
    {
        URHO3D_LOGDEBUG("Failed to bind reflection-based material descriptors");
    }

    // Record instanced draw command with base vertex index
    URHO3D_LOGDEBUG("About to call vkCmdDrawIndexed (instanced+baseVertex): indexCount=" + String(indexCount) + ", instanceCount=" + String(instanceCount) + ", indexStart=" + String(indexStart) + ", baseVertexIndex=" + String(baseVertexIndex));
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, baseVertexIndex, 0);
    URHO3D_LOGDEBUG("vkCmdDrawIndexed (instanced+baseVertex) completed successfully");

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
}

// ============================================
// Compute Shader Support (Phase 36+)
// ============================================

void Graphics::DispatchCompute_Vulkan(unsigned groupCountX, unsigned groupCountY, unsigned groupCountZ)
{
    if (!impl_ || groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)
    {
        URHO3D_LOGWARNING("DispatchCompute_Vulkan: Invalid parameters");
        return;
    }

    if (!computeShader_)
    {
        URHO3D_LOGWARNING("DispatchCompute_Vulkan: No compute shader set");
        return;
    }

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Compile compute shader to SPIR-V
    VkShaderModule csModule = VK_NULL_HANDLE;
    Vector<uint32_t> spirvBytecode;
    String errorOutput;

    if (!VulkanShaderModule::GetOrCompileSPIRV(computeShader_, spirvBytecode, errorOutput))
    {
        URHO3D_LOGERROR("DispatchCompute_Vulkan: Failed to compile compute shader: " + errorOutput);
        return;
    }

    csModule = VulkanShaderModule::CreateShaderModule(vkImpl->GetDevice(), spirvBytecode);
    if (!csModule)
    {
        URHO3D_LOGERROR("DispatchCompute_Vulkan: Failed to create compute shader module");
        return;
    }

    // Get pipeline layout (compute pipeline uses same descriptor sets as graphics)
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
    if (!layout)
    {
        URHO3D_LOGWARNING("DispatchCompute_Vulkan: Invalid pipeline layout");
        vkDestroyShaderModule(vkImpl->GetDevice(), csModule, nullptr);
        return;
    }

    // Get or create compute pipeline
    VulkanComputePipeline* computePipeline = vkImpl->GetComputePipeline();
    if (!computePipeline)
    {
        URHO3D_LOGERROR("DispatchCompute_Vulkan: Compute pipeline manager not initialized");
        vkDestroyShaderModule(vkImpl->GetDevice(), csModule, nullptr);
        return;
    }

    VkPipeline pipeline = computePipeline->GetOrCreatePipeline(layout, csModule);
    if (!pipeline)
    {
        URHO3D_LOGERROR("DispatchCompute_Vulkan: Failed to get or create compute pipeline");
        vkDestroyShaderModule(vkImpl->GetDevice(), csModule, nullptr);
        return;
    }

    // Bind compute pipeline
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    // TODO: Bind descriptor sets for storage buffers and uniform buffers here
    // This will be implemented when descriptor management is extended for compute

    // Dispatch compute work groups
    vkCmdDispatch(cmdBuffer, groupCountX, groupCountY, groupCountZ);

    // Insert pipeline barrier to synchronize compute writes with subsequent graphics reads
    // This prevents race conditions when graphics pipeline reads from buffers written by compute
    vkImpl->InsertPipelineBarrier(
        cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,          // srcStage: wait for compute to finish
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |          // dstStage: block vertex/fragment shaders
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,                    // srcAccess: compute shader writes
        VK_ACCESS_SHADER_READ_BIT |                    // dstAccess: graphics shader reads
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
    );

    // Clean up shader module
    vkDestroyShaderModule(vkImpl->GetDevice(), csModule, nullptr);

    URHO3D_LOGDEBUG("DispatchCompute_Vulkan: groups=(" + String(groupCountX) + ", " +
                    String(groupCountY) + ", " + String(groupCountZ) + ")");
}

void Graphics::SetComputeShader_Vulkan(ShaderVariation* shader)
{
    computeShader_ = shader;
    URHO3D_LOGDEBUG("SetComputeShader_Vulkan: " + String(shader ? shader->GetFullName() : "null"));
}

void Graphics::SetStorageBuffer_Vulkan(unsigned index, VertexBuffer* buffer)
{
    if (index >= MAX_TEXTURE_UNITS)
    {
        URHO3D_LOGWARNING("SetStorageBuffer_Vulkan: Invalid index " + String(index));
        return;
    }

    storageBuffers_[index] = buffer;

    // TODO: Update descriptor set with storage buffer binding
    // This will be implemented when storage buffer descriptor management is added

    URHO3D_LOGDEBUG("SetStorageBuffer_Vulkan: index=" + String(index) +
                    " buffer=" + String(buffer ? "valid" : "null"));
}

// ============================================
// Stub implementations for state-setting methods
// ============================================

void Graphics::SetBlendMode_Vulkan(BlendMode mode, bool alphaToCoverage)
{
    blendMode_ = mode;
    alphaToCoverage_ = alphaToCoverage;
}

void Graphics::SetCullMode_Vulkan(CullMode mode)
{
    cullMode_ = mode;
}

void Graphics::SetDepthTest_Vulkan(CompareMode mode)
{
    depthTestMode_ = mode;
}

void Graphics::SetDepthWrite_Vulkan(bool enable)
{
    depthWrite_ = enable;
}

void Graphics::SetFillMode_Vulkan(FillMode mode)
{
    fillMode_ = mode;
}

void Graphics::SetStencilTest_Vulkan(bool enable, CompareMode mode, StencilOp pass, StencilOp fail, StencilOp zFail, unsigned stencilRef, unsigned compareMask, unsigned writeMask)
{
    stencilTest_ = enable;
    stencilTestMode_ = mode;
    stencilPass_ = pass;
    stencilFail_ = fail;
    stencilZFail_ = zFail;
    stencilRef_ = stencilRef;
    stencilCompareMask_ = compareMask;
    stencilWriteMask_ = writeMask;
}

void Graphics::SetClipPlane_Vulkan(bool enable, const Plane& clipPlane, const Matrix3x4& view, const Matrix4& projection)
{
    useClipPlane_ = enable;

    if (enable)
    {
        Matrix4 viewProj = projection * view;
        clipPlane_ = clipPlane.Transformed(viewProj).ToVector4();
    }
    else
    {
        // Set to (0,0,0,1) so gl_ClipDistance = 1.0 (definitely not clipped)
        // (0,0,0,0) gives gl_ClipDistance = 0 which is on the clip boundary
        clipPlane_ = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    URHO3D_LOGINFO(String("SetClipPlane_Vulkan: enable=") + String(enable) +
                   ", setting clipPlane=(" + String(clipPlane_.x_) + "," + String(clipPlane_.y_) + "," +
                   String(clipPlane_.z_) + "," + String(clipPlane_.w_) + ")");
    SetShaderParameter(VSP_CLIPPLANE, clipPlane_);
    URHO3D_LOGINFO("SetClipPlane_Vulkan: SetShaderParameter called");
}

void Graphics::SetColorWrite_Vulkan(bool enable)
{
    colorWrite_ = enable;
}

void Graphics::SetTexture_Vulkan(unsigned index, Texture* texture)
{
    // Phase 36 Step 1: Texture binding for deferred lighting G-Buffer inputs
    // Stores texture in texture unit array for later descriptor set creation
    // Critical for deferred rendering where lighting pass reads from G-Buffer textures
    //
    // Descriptor sets will be created/updated on draw calls based on current textures_[] state
    // No explicit dirty tracking needed - Vulkan descriptor manager handles caching

    URHO3D_LOGINFO("SetTexture_Vulkan ENTRY: index=" + String(index));

    if (index >= MAX_TEXTURE_UNITS)
    {
        URHO3D_LOGERROR("SetTexture_Vulkan: Texture unit index out of range: " + String(index));
        return;
    }

    // Update texture binding state
    textures_[index] = texture;

    URHO3D_LOGINFO("SetTexture_Vulkan: Bound texture to unit " + String(index) +
                    (texture ? " (texture set)" : " (null)"));
    URHO3D_LOGINFO("SetTexture_Vulkan EXIT");
}

void Graphics::SetRenderTarget_Vulkan(unsigned index, RenderSurface* renderTarget)
{
    // Phase 34 Step 2: Render target binding for deferred rendering framebuffers
    if (index >= MAX_RENDERTARGETS)
        return;

    if (renderTarget != renderTargets_[index])
    {
        renderTargets_[index] = renderTarget;

        // Mark render targets dirty to trigger framebuffer rebuild
        // This will call RebuildRenderTargetFramebuffer() at the start of rendering
        if (impl_)
        {
            VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
            vkImpl->renderTargetsDirty_ = true;
        }
    }
}

void Graphics::SetDepthStencil_Vulkan(RenderSurface* depthStencil)
{
    // Phase 34 Step 2: Depth stencil binding for deferred rendering
    if (depthStencil != depthStencil_)
    {
        depthStencil_ = depthStencil;

        // Mark render targets dirty to trigger framebuffer rebuild
        if (impl_)
        {
            VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
            vkImpl->renderTargetsDirty_ = true;
        }
    }
}

void Graphics::ResetRenderTargets_Vulkan()
{
    // CRITICAL FIX: Reset to default swapchain rendering
    // This is called by UI::Render() to switch from G-Buffer rendering back to swapchain
    // Without this, UI renders to offscreen G-Buffer and never becomes visible

    URHO3D_LOGDEBUG("ResetRenderTargets_Vulkan: Resetting all render targets to swapchain");

    // Clear all render target slots (set to nullptr = use swapchain)
    for (unsigned i = 0; i < MAX_RENDERTARGETS; ++i)
    {
        if (renderTargets_[i])
        {
            renderTargets_[i] = nullptr;
        }
    }

    // Clear depth-stencil (will use default depth buffer)
    depthStencil_ = nullptr;

    // Mark render targets dirty to trigger framebuffer rebuild
    // Next BeginRenderPass will create swapchain framebuffer instead of G-Buffer
    if (impl_)
    {
        VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
        vkImpl->renderTargetsDirty_ = true;
        URHO3D_LOGDEBUG("ResetRenderTargets_Vulkan: Marked render targets dirty for swapchain rebuild");
    }
}

// ============================================
// Phase 32: GPU State Application - Convert to Pipeline State
// ============================================

/// \brief Apply cached Graphics state to VulkanPipelineState
/// \details Converts all cached graphics state from Graphics class members
/// to VulkanPipelineState structure for pipeline creation (Phase 32 Step 2).
/// This prepares the graphics state for GetOrCreateGraphicsPipeline().
void Graphics::ApplyGraphicsState_Vulkan(VulkanPipelineState& state) const
{
    // Blend state
    state.blendMode = blendMode_;
    state.alphaToCoverage = alphaToCoverage_;

    // Depth state
    state.depthTest = depthTestMode_;
    state.depthWrite = depthWrite_;

    // Cull state
    state.cullMode = cullMode_;

    // Fill mode (solid vs wireframe)
    state.fillMode = fillMode_;

    // Stencil state (full stencil parameters for future enhancement)
    state.stencilTest = stencilTest_;
    state.stencilTestMode = stencilTestMode_;
    state.stencilPass = stencilPass_;
    state.stencilFail = stencilFail_;
    state.stencilZFail = stencilZFail_;
    state.stencilRef = stencilRef_;
    state.stencilCompareMask = stencilCompareMask_;
    state.stencilWriteMask = stencilWriteMask_;

    // DEBUG: Log critical state that might prevent rendering
    URHO3D_LOGINFO(String("[PIPELINE_STATE] blend=") + String((int)blendMode_) +
                   ", depthTest=" + String((int)depthTestMode_) + ", depthWrite=" + String(depthWrite_) +
                   ", cull=" + String((int)cullMode_) + ", stencilTest=" + String(stencilTest_));
}

// ============================================
// Phase 36: Shader Parameters for Deferred Lighting
// ============================================

void Graphics::SetShaderParameter_Vulkan(StringHash param, const Variant& value)
{
    // Phase 36 Step 3: Shader parameter binding with constant buffer integration
    // Handles shader parameters (light position, color, matrices, etc.) for deferred lighting shaders
    // Critical for passing light data from CPU to GPU
    //
    // Implementation Strategy:
    // 1. Store parameters in shaderParameters_ map for batching
    // 2. Upload to constant buffer during draw call
    // 3. Bind constant buffer descriptor before rendering
    //
    // Current Phase: Parameter storage and validation

    if (!vertexShader_ && !pixelShader_)
    {
        // No active shaders, nothing to bind parameters to
        return;
    }

    // Phase 36 Step 3.1: Store parameter for batched upload ✅ IMPLEMENTED
    // Parameters are accumulated and uploaded to GPU in batches during draw calls
    // This reduces the number of GPU uploads and improves performance
    //
    // Implementation:
    // - Parameters stored in pendingShaderParameters_ HashMap for O(1) lookup
    // - Batched upload reduces GPU synchronization overhead
    // - Constant buffer pool provides efficient memory allocation
    // - Parameters cleared after upload (during draw call)

    pendingShaderParameters_[param] = value;

    URHO3D_LOGDEBUG("SetShaderParameter_Vulkan: Stored " + param.ToString() + " = " + value.ToString() +
                    " (total pending: " + String(pendingShaderParameters_.Size()) + ")");

    // TODO Phase 36 Step 3.2: Implement batched parameter upload in draw call
    // Integration with VulkanConstantBufferPool (to be called from PrepareDraw_Vulkan or Draw_Vulkan):
    //
    // if (!pendingShaderParameters_.Empty())
    // {
    //     VulkanConstantBufferPool* cbPool = vkImpl->GetConstantBufferPool();
    //
    //     // Calculate total parameter size
    //     size_t totalSize = CalculateParameterBufferSize(pendingShaderParameters_);
    //
    //     // Allocate constant buffer
    //     VkBuffer cbBuffer;
    //     VkDeviceSize cbOffset;
    //     void* cbData = cbPool->AllocateBuffer(nullptr, totalSize, cbBuffer, cbOffset);
    //
    //     // Pack parameters into buffer
    //     PackShaderParameters(pendingShaderParameters_, cbData, totalSize);
    //
    //     // Create/update descriptor set for constant buffer (TODO Step 3.3)
    //     VkDescriptorSet cbDescriptorSet = CreateConstantBufferDescriptorSet(cbBuffer, cbOffset, totalSize);
    //
    //     // Bind descriptor set (TODO Step 3.4)
    //     vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
    //                             pipelineLayout, 2, 1, &cbDescriptorSet, 0, nullptr);
    //
    //     // Clear pending parameters after upload
    //     pendingShaderParameters_.Clear();
    // }

    // TODO Phase 36 Step 3.3: Create constant buffer descriptor sets
    // TODO Phase 36 Step 3.4: Bind constant buffers before draw calls
}

// ============================================
// Phase 36 Step 3: Constant Buffer Parameter Helpers
// ============================================

size_t Graphics::CalculateParameterBufferSize(const HashMap<StringHash, Variant>& parameters)
{
    // Phase 36 Step 3.2.1: Calculate total buffer size for shader parameters
    // Uses std140 layout rules: all types aligned to 16 bytes (vec4 alignment)
    // This ensures compatibility with GLSL uniform buffer layout

    size_t totalSize = 0;

    for (auto it = parameters.Begin(); it != parameters.End(); ++it)
    {
        const Variant& value = it->second_;

        // Calculate size based on Variant type, aligned to 16 bytes (std140 layout)
        // std140 rules: scalars and vectors use vec4 alignment (16 bytes)
        switch (value.GetType())
        {
            case VAR_FLOAT:
            case VAR_INT:
            case VAR_BOOL:
                totalSize += 16;  // Scalar in std140 (aligned to vec4)
                break;

            case VAR_VECTOR2:
                totalSize += 16;  // vec2 in std140 (aligned to vec4)
                break;

            case VAR_VECTOR3:
                totalSize += 16;  // vec3 in std140 (padded to vec4)
                break;

            case VAR_VECTOR4:
            case VAR_COLOR:
                totalSize += 16;  // vec4 in std140
                break;

            case VAR_MATRIX3:
                totalSize += 48;  // mat3 in std140 (3 vec4s, row-major)
                break;

            case VAR_MATRIX3X4:
                totalSize += 64;  // mat3x4 in std140 (3 vec4s + padding)
                break;

            case VAR_MATRIX4:
                totalSize += 64;  // mat4 in std140 (4 vec4s)
                break;

            default:
                URHO3D_LOGWARNING("CalculateParameterBufferSize: Unsupported parameter type " +
                                  String((int)value.GetType()));
                break;
        }
    }

    URHO3D_LOGDEBUG("CalculateParameterBufferSize: Total size = " + String(totalSize) +
                    " bytes for " + String(parameters.Size()) + " parameters");

    return totalSize;
}

void Graphics::PackShaderParameters(
    const HashMap<StringHash, Variant>& parameters,
    void* buffer,
    size_t bufferSize)
{
    // Phase 36 Step 3.2.2: Pack shader parameters into GPU buffer
    // Uses std140 layout rules for GLSL uniform buffer compatibility
    // All parameters aligned to 16-byte boundaries

    if (!buffer)
    {
        URHO3D_LOGERROR("PackShaderParameters: Null buffer pointer");
        return;
    }

    unsigned char* dst = (unsigned char*)buffer;
    size_t offset = 0;

    for (auto it = parameters.Begin(); it != parameters.End(); ++it)
    {
        const Variant& value = it->second_;

        // Ensure we don't overflow the buffer
        if (offset >= bufferSize)
        {
            URHO3D_LOGERROR("PackShaderParameters: Buffer overflow at offset " + String(offset));
            break;
        }

        // Pack parameter based on type with std140 alignment
        switch (value.GetType())
        {
            case VAR_FLOAT:
            {
                float f = value.GetFloat();
                memcpy(dst + offset, &f, sizeof(float));
                offset += 16;  // std140 alignment
                break;
            }

            case VAR_INT:
            {
                int i = value.GetI32();
                memcpy(dst + offset, &i, sizeof(int));
                offset += 16;  // std140 alignment
                break;
            }

            case VAR_BOOL:
            {
                int b = value.GetBool() ? 1 : 0;
                memcpy(dst + offset, &b, sizeof(int));  // bool as int in GLSL
                offset += 16;  // std140 alignment
                break;
            }

            case VAR_VECTOR2:
            {
                const Vector2& v = value.GetVector2();
                memcpy(dst + offset, &v, sizeof(Vector2));
                offset += 16;  // std140 alignment (vec2 → vec4 with padding)
                break;
            }

            case VAR_VECTOR3:
            {
                const Vector3& v = value.GetVector3();
                memcpy(dst + offset, &v, sizeof(Vector3));
                offset += 16;  // std140 alignment (vec3 → vec4 with padding)
                break;
            }

            case VAR_VECTOR4:
            {
                const Vector4& v = value.GetVector4();
                memcpy(dst + offset, &v, sizeof(Vector4));
                offset += 16;  // std140 alignment
                break;
            }

            case VAR_COLOR:
            {
                const Color& c = value.GetColor();
                memcpy(dst + offset, &c, sizeof(Color));  // Color is vec4
                offset += 16;  // std140 alignment
                break;
            }

            case VAR_MATRIX3:
            {
                const Matrix3& m = value.GetMatrix3();
                // mat3 in std140: 3 vec4s (row-major with padding)
                // Row 0
                float row0[3] = {m.m00_, m.m01_, m.m02_};
                memcpy(dst + offset, row0, sizeof(float) * 3);
                offset += 16;
                // Row 1
                float row1[3] = {m.m10_, m.m11_, m.m12_};
                memcpy(dst + offset, row1, sizeof(float) * 3);
                offset += 16;
                // Row 2
                float row2[3] = {m.m20_, m.m21_, m.m22_};
                memcpy(dst + offset, row2, sizeof(float) * 3);
                offset += 16;
                break;
            }

            case VAR_MATRIX3X4:
            {
                const Matrix3x4& m = value.GetMatrix3x4();
                // mat3x4 in std140: 3 vec4s
                memcpy(dst + offset, &m, sizeof(Matrix3x4));
                offset += 64;  // 3 vec4s + padding
                break;
            }

            case VAR_MATRIX4:
            {
                const Matrix4& m = value.GetMatrix4();
                // mat4 in std140: 4 vec4s
                memcpy(dst + offset, &m, sizeof(Matrix4));
                offset += 64;  // 4 vec4s
                break;
            }

            default:
                URHO3D_LOGWARNING("PackShaderParameters: Unsupported parameter type " +
                                  String((int)value.GetType()) + " for parameter " +
                                  it->first_.ToString());
                break;
        }
    }

    URHO3D_LOGDEBUG("PackShaderParameters: Packed " + String(parameters.Size()) +
                    " parameters, total offset = " + String(offset) + " bytes");
}

// ============================================
// Shader Binding (Phase 36+: Geometry Shader Support)
// ============================================

void Graphics::SetShaders_Vulkan(ShaderVariation* vs, ShaderVariation* ps, ShaderVariation* gs)
{
    URHO3D_LOGINFO("SetShaders_Vulkan ENTRY");
    // Store shader pointers for pipeline creation
    vertexShader_ = vs;
    pixelShader_ = ps;
    geometryShader_ = gs;

    // Pipeline will be recreated with new shader configuration on next draw call

    URHO3D_LOGINFO("SetShaders_Vulkan: VS=" + String(vs ? vs->GetFullName() : "null") +
                    " PS=" + String(ps ? ps->GetFullName() : "null") +
                    " GS=" + String(gs ? gs->GetFullName() : "null"));
    URHO3D_LOGINFO("SetShaders_Vulkan EXIT");
}

// ============================================
// Phase 36 Step 3: Constant Buffer Descriptor Management
// ============================================

VkDescriptorSet Graphics::CreateConstantBufferDescriptorSet_Vulkan(VkBuffer buffer, size_t size)
{
    // Phase 36 Step 3.3: Create descriptor set for constant buffer
    // This enables deferred lighting shaders to access light parameters via uniform buffers
    //
    // Creates a descriptor set containing a single VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    // bound to descriptor set slot 2 (slot 0: materials, slot 1: G-Buffer textures)
    //
    // Parameters:
    //   buffer - VkBuffer containing packed shader parameters (from VulkanConstantBufferPool)
    //   size   - Size of the buffer region in bytes
    //
    // Returns:
    //   VkDescriptorSet for the constant buffer, or VK_NULL_HANDLE on error
    //
    // TODO Phase 36 Step 3.3.1: Integrate with VulkanConstantBufferPool
    // TODO Phase 36 Step 3.3.2: Handle descriptor set caching/reuse

    if (!impl_)
    {
        URHO3D_LOGERROR("CreateConstantBufferDescriptorSet_Vulkan: Graphics implementation not initialized");
        return VK_NULL_HANDLE;
    }

    VulkanGraphicsImpl* vkImpl = static_cast<VulkanGraphicsImpl*>(impl_);
    VkDevice device = vkImpl->GetDevice();

    if (!device || buffer == VK_NULL_HANDLE || size == 0)
    {
        URHO3D_LOGERROR("CreateConstantBufferDescriptorSet_Vulkan: Invalid parameters");
        return VK_NULL_HANDLE;
    }

    // Phase 36 Step 3.3.1: Create descriptor set layout (cached statically)
    static VkDescriptorSetLayout constantBufferDescriptorLayout = VK_NULL_HANDLE;

    if (constantBufferDescriptorLayout == VK_NULL_HANDLE)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        binding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &constantBufferDescriptorLayout);
        if (result != VK_SUCCESS)
        {
            URHO3D_LOGERROR("CreateConstantBufferDescriptorSet_Vulkan: Failed to create descriptor set layout");
            return VK_NULL_HANDLE;
        }

        URHO3D_LOGDEBUG("CreateConstantBufferDescriptorSet_Vulkan: Created static descriptor set layout");
    }

    // Phase 36 Step 3.3.2: Allocate descriptor set from pool
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkImpl->GetDescriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &constantBufferDescriptorLayout;

    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("CreateConstantBufferDescriptorSet_Vulkan: Failed to allocate descriptor set (pool may be exhausted)");
        return VK_NULL_HANDLE;
    }

    // Phase 36 Step 3.3.3: Update descriptor set with buffer binding
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = size;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    URHO3D_LOGDEBUG("CreateConstantBufferDescriptorSet_Vulkan: Created descriptor set with " +
                    String((unsigned)size) + " byte constant buffer");

    return descriptorSet;
}

bool Graphics::BindConstantBufferDescriptors_Vulkan(VkDescriptorSet descriptorSet)
{
    // Phase 36 Step 3.4: Bind constant buffer descriptor set for light parameters
    // Binds descriptor set containing light parameter uniform buffer
    // to descriptor set slot 2 (slot 0: materials, slot 1: G-Buffer textures)
    //
    // Called before draw calls in deferred lighting pass
    //
    // TODO Phase 36 Step 3.4.1: Integrate with pipeline layout
    // TODO Phase 36 Step 3.4.2: Handle multiple descriptor sets in pipeline

    if (!impl_ || descriptorSet == VK_NULL_HANDLE)
        return false;

    VulkanGraphicsImpl* vkImpl = static_cast<VulkanGraphicsImpl*>(impl_);
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    VkPipelineLayout pipelineLayout = vkImpl->GetCurrentPipelineLayout();

    if (!cmdBuffer || !pipelineLayout)
    {
        URHO3D_LOGWARNING("BindConstantBufferDescriptors_Vulkan: Command buffer or pipeline layout not available");
        return false;
    }

    // Bind to descriptor set slot 0 (shader parameter uniforms - matches SPIR-V expectations)
    vkCmdBindDescriptorSets(
        cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,  // Set slot 0, binding 0 for shader uniforms
        1,
        &descriptorSet,
        0, nullptr
    );

    URHO3D_LOGDEBUG("BindConstantBufferDescriptors_Vulkan: Bound constant buffer descriptor set to slot 2");
    return true;
}


/// Phase 36 Step 4: Upload pending shader parameters to GPU
/// Uniform Block Parameter Mapping
/// ============================================

struct UniformBlockInfo
{
    unsigned binding;        // Descriptor binding number (0-10)
    unsigned offset;         // Byte offset within the block (std140 layout)
    unsigned blockSize;      // Total size of the uniform block in bytes
    String blockName;        // Name of the uniform block (for debugging)
};

static UniformBlockInfo GetUniformBlockInfo(const StringHash& paramHash)
{
    // Map parameter hashes to their uniform block locations
    // Based on Uniforms.glsl std140 layout

    // FrameVS (binding 0): float cDeltaTime @ 0, float cElapsedTime @ 4
    if (paramHash == VSP_ELAPSEDTIME)  // "ElapsedTime"
        return { 0, 4, 16, "FrameVS" };
    if (paramHash == StringHash("DeltaTime"))
        return { 0, 0, 16, "FrameVS" };

    // CameraVS (binding 1): cViewProj @ offset 208 (after 9 other members)
    if (paramHash == VSP_VIEWPROJ)  // "ViewProj"
        return { 1, 208, 288, "CameraVS" };
    if (paramHash == VSP_VIEW)  // "View"
        return { 1, 80, 288, "CameraVS" };
    if (paramHash == VSP_VIEWINV)  // "ViewInv"
        return { 1, 144, 288, "CameraVS" };

    // ObjectVS (binding 5): mat4 cModel @ 0
    if (paramHash == VSP_MODEL)  // "Model"
        return { 5, 0, 64, "ObjectVS" };

    // FramePS (binding 6): float cDeltaTimePS @ 0, float cElapsedTimePS @ 4
    if (paramHash == PSP_ELAPSEDTIME)  // "ElapsedTimePS"
        return { 6, 4, 16, "FramePS" };
    if (paramHash == StringHash("DeltaTimePS"))
        return { 6, 0, 16, "FramePS" };

    // MaterialPS (binding 10): vec4 cMatDiffColor @ 0, vec3 cMatEmissiveColor @ 16, vec3 cMatEnvMapColor @ 32, vec4 cMatSpecColor @ 48
    if (paramHash == PSP_MATDIFFCOLOR)  // "MatDiffColor"
        return { 10, 0, 64, "MaterialPS" };
    if (paramHash == PSP_MATEMISSIVECOLOR)  // "MatEmissiveColor"
        return { 10, 16, 64, "MaterialPS" };
    if (paramHash == PSP_MATENVMAPCOLOR)  // "MatEnvMapColor"
        return { 10, 32, 64, "MaterialPS" };
    if (paramHash == PSP_MATSPECCOLOR)  // "MatSpecColor"
        return { 10, 48, 64, "MaterialPS" };

    // Default: unknown parameter, put in binding 0
    URHO3D_LOGWARNING("Unknown shader parameter hash: " + String(paramHash.Value()) + ", using default binding 0");
    return { 0, 0, 16, "Unknown" };
}

/// ============================================
/// COMPLETE INTEGRATION with draw pipeline
/// ============================================
///
/// **Purpose**: Uploads all pending shader parameters to GPU before draw call
///
/// **Called From**: All Draw_Vulkan() variants before issuing draw commands
///
/// **Process**:
/// 1. Check if pendingShaderParameters_ has data
/// 2. Calculate total buffer size needed (std140 layout)
/// 3. Allocate staging buffer and pack parameters
/// 4. Allocate GPU buffer from VulkanConstantBufferPool
/// 5. Create descriptor set for constant buffer
/// 6. Bind descriptor set to pipeline (slot 2)
/// 7. Clear pendingShaderParameters_ for next frame
///
/// **Performance**: O(N) where N = number of parameters
/// - Batches all parameters into single upload
/// - Reuses pool buffers across frames (no allocation overhead)
/// - Single descriptor set bind per draw call
///
/// **Integration Point**: Called after pipeline binding, before vkCmdDraw*()
void Graphics::UploadPendingShaderParameters_Vulkan()
{
    if (pendingShaderParameters_.Empty())
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

    VulkanConstantBufferPool* cbPool = vkImpl->GetConstantBufferPool();
    if (!cbPool)
        return;

    // Step 1: Group parameters by uniform block binding
    HashMap<unsigned, Vector<unsigned char>> blockBuffers;  // binding -> buffer data
    HashMap<unsigned, unsigned> blockSizes;                  // binding -> size

    for (auto it = pendingShaderParameters_.Begin(); it != pendingShaderParameters_.End(); ++it)
    {
        const StringHash& paramHash = it->first_;
        const Variant& value = it->second_;

        UniformBlockInfo blockInfo = GetUniformBlockInfo(paramHash);
        unsigned binding = blockInfo.binding;
        unsigned offset = blockInfo.offset;
        unsigned blockSize = blockInfo.blockSize;

        URHO3D_LOGDEBUG("Packing parameter hash=" + String(paramHash.Value()) +
                        " -> block=" + blockInfo.blockName + " binding=" + String(binding) +
                        " offset=" + String(offset) + " size=" + String(blockSize));

        // Ensure buffer exists and is large enough
        if (!blockBuffers.Contains(binding))
        {
            blockBuffers[binding].Resize(blockSize);
            memset(&blockBuffers[binding][0], 0, blockSize);  // Zero-initialize
            blockSizes[binding] = blockSize;
        }

        // Write parameter at correct std140 offset
        unsigned char* dst = &blockBuffers[binding][offset];

        switch (value.GetType())
        {
            case VAR_FLOAT:
            {
                float f = value.GetFloat();
                memcpy(dst, &f, sizeof(float));
                break;
            }
            case VAR_VECTOR2:
            {
                const Vector2& v = value.GetVector2();
                memcpy(dst, &v, sizeof(Vector2));
                break;
            }
            case VAR_VECTOR3:
            {
                const Vector3& v = value.GetVector3();
                memcpy(dst, &v, sizeof(Vector3));
                break;
            }
            case VAR_VECTOR4:
            {
                const Vector4& v = value.GetVector4();
                memcpy(dst, &v, sizeof(Vector4));
                break;
            }
            case VAR_COLOR:
            {
                const Color& c = value.GetColor();
                memcpy(dst, &c, sizeof(Color));
                break;
            }
            case VAR_MATRIX3:
            case VAR_MATRIX3X4:
            {
                const Matrix3x4& m = value.GetMatrix3x4();
                // mat4 in std140: store as 4 vec4s (column-major with padding)
                // Matrix3x4 layout: m00-m03 (row 0), m10-m13 (row 1), m20-m23 (row 2)
                // Translation is in column 3: m03, m13, m23
                float mat4Data[16] = {
                    m.m00_, m.m01_, m.m02_, 0.0f,
                    m.m10_, m.m11_, m.m12_, 0.0f,
                    m.m20_, m.m21_, m.m22_, 0.0f,
                    m.m03_, m.m13_, m.m23_, 1.0f  // Translation column
                };
                memcpy(dst, mat4Data, 64);
                break;
            }
            case VAR_MATRIX4:
            {
                const Matrix4& m = value.GetMatrix4();
                memcpy(dst, &m, 64);
                break;
            }
            default:
                URHO3D_LOGWARNING("Unsupported parameter type: " + String((int)value.GetType()));
                break;
        }
    }

    // CRITICAL FIX: Add dummy zero-filled blocks for missing bindings
    // The shader declares all 11 uniform blocks (0-10), but C++ only provides 5.
    // Missing blocks 2,3,4,7,8,9 must be filled with zeros to prevent shader from reading garbage.
    const struct { unsigned binding; unsigned size; const char* name; } dummyBlocks[] = {
        { 2, 128, "ZoneVS" },      // vec3 + vec3 + mat4
        { 3, 512, "LightVS" },     // vec4 + vec3 + vec4 + mat4[4] (shadow matrices)
        { 4, 64, "MaterialVS" },   // vec4 + vec4 (UV offsets)
        { 7, 128, "CameraPS" },    // vec3 + vec4 + vec2 + float + float
        { 8, 128, "ZonePS" },      // vec4 + vec4 + vec3 + vec3 + vec3 (fog/ambient)
        { 9, 512, "LightPS" }      // vec4 + vec4 + vec3 + vec4 + ... + mat4[4]
    };

    for (unsigned i = 0; i < 6; ++i)
    {
        unsigned binding = dummyBlocks[i].binding;
        if (!blockBuffers.Contains(binding))
        {
            unsigned size = dummyBlocks[i].size;
            blockBuffers[binding].Resize(size);
            memset(&blockBuffers[binding][0], 0, size);
            blockSizes[binding] = size;
            URHO3D_LOGDEBUG("Added dummy zero-filled block: " + String(dummyBlocks[i].name) +
                            " (binding=" + String(binding) + ") size=" + String(size));
        }
    }

    // Step 2: Upload each block buffer and create combined descriptor set
    // For simplicity, allocate one large buffer with all blocks concatenated
    size_t totalSize = 0;
    HashMap<unsigned, size_t> blockOffsets;  // binding -> offset in combined buffer

    for (auto it = blockBuffers.Begin(); it != blockBuffers.End(); ++it)
    {
        blockOffsets[it->first_] = totalSize;
        totalSize += blockSizes[it->first_];
    }

    if (totalSize == 0)
    {
        pendingShaderParameters_.Clear();
        return;
    }

    // Allocate combined staging buffer
    Vector<unsigned char> stagingBuffer(totalSize);
    memset(&stagingBuffer[0], 0, totalSize);

    // Copy each block to its position
    URHO3D_LOGDEBUG("UploadPendingShaderParameters_Vulkan: Concatenated buffer layout:");
    for (auto it = blockBuffers.Begin(); it != blockBuffers.End(); ++it)
    {
        unsigned binding = it->first_;
        size_t offset = blockOffsets[binding];
        size_t size = blockSizes[binding];

        // Log the global offset for this block
        const char* blockName = "Unknown";
        if (binding == 0) blockName = "FrameVS";
        else if (binding == 1) blockName = "CameraVS";
        else if (binding == 2) blockName = "ZoneVS";
        else if (binding == 3) blockName = "LightVS";
        else if (binding == 4) blockName = "MaterialVS";
        else if (binding == 5) blockName = "ObjectVS";
        else if (binding == 6) blockName = "FramePS";
        else if (binding == 7) blockName = "CameraPS";
        else if (binding == 8) blockName = "ZonePS";
        else if (binding == 9) blockName = "LightPS";
        else if (binding == 10) blockName = "MaterialPS";

        URHO3D_LOGDEBUG("  Block " + String(blockName) + " (binding=" + String(binding) +
                        ") at GLOBAL offset=" + String((unsigned)offset) + " size=" + String((unsigned)size));

        // Hex dump relevant data based on block type
        if (binding == 1)  // CameraVS - dump ViewProj matrix at offset 208
        {
            if (size >= 272)  // Ensure ViewProj is present (offset 208 + 64 bytes)
            {
                String hexDump = "    ViewProj matrix (offset 208): ";
                for (unsigned i = 208; i < 272; i += 4)
                {
                    unsigned char* ptr = &it->second_[i];
                    float* floatPtr = (float*)ptr;
                    hexDump += String(*floatPtr) + " ";
                    if ((i - 208) % 16 == 12) hexDump += "\n                                    ";
                }
                URHO3D_LOGDEBUG(hexDump);
            }
        }
        else  // Other blocks - dump first 64 bytes
        {
            String hexDump = "    Data: ";
            unsigned dumpSize = size < 64 ? size : 64;
            for (unsigned i = 0; i < dumpSize; i += 4)
            {
                unsigned char* ptr = &it->second_[i];
                float* floatPtr = (float*)ptr;
                hexDump += String(*floatPtr) + " ";
            }
            URHO3D_LOGDEBUG(hexDump);
        }

        memcpy(&stagingBuffer[offset], &it->second_[0], size);
    }

    // Upload to GPU
    VkBuffer gpuBuffer = VK_NULL_HANDLE;
    VkDeviceSize bufferOffset = 0;

    if (!cbPool->AllocateBuffer(&stagingBuffer[0], (uint32_t)totalSize, gpuBuffer, bufferOffset))
    {
        URHO3D_LOGERROR("UploadPendingShaderParameters_Vulkan: Failed to allocate constant buffer from pool");
        pendingShaderParameters_.Clear();
        return;
    }

    URHO3D_LOGDEBUG("UploadPendingShaderParameters_Vulkan: Allocated buffer " + String((unsigned long long)gpuBuffer) +
                    " offset=" + String((unsigned)bufferOffset) + " size=" + String((unsigned)totalSize));

    // Store buffer and block layout for use by descriptor set creation
    // The descriptor set will be created later with correct per-block offsets
    vkImpl->SetCurrentConstantBuffer(gpuBuffer, totalSize);
    currentBlockOffsets_ = blockOffsets;  // Store for CreateReflectionBasedDescriptorSet_Vulkan()
    currentBlockSizes_ = blockSizes;

    URHO3D_LOGDEBUG("UploadPendingShaderParameters_Vulkan: Constant buffer uploaded, blocks=" + String(blockOffsets.Size()));

    pendingShaderParameters_.Clear();
}
/// Input Attachments: Create descriptor set for G-Buffer input attachments
/// ============================================
/// Vulkan-specific optimization for deferred lighting
/// ============================================
///
/// **Purpose**: Creates descriptor set for G-Buffer input attachments in lighting subpass
///
/// **Optimization**: Input attachments use tile-local memory on mobile GPUs (faster than texture sampling)
///
/// **G-Buffer Layout**:
/// - Input 0: Albedo (from color attachment 0)
/// - Input 1: Normal (from color attachment 1)
/// - Input 2: Depth (from color attachment 2)
/// - Input 3: Position (from color attachment 3)
///
/// **Usage**: Called during lighting subpass setup, binds G-Buffer for efficient reads
VkDescriptorSet Graphics::CreateInputAttachmentDescriptorSet_Vulkan()
{
    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGWARNING("CreateInputAttachmentDescriptorSet_Vulkan: Graphics implementation not initialized");
        return VK_NULL_HANDLE;
    }

    VkDevice device = vkImpl->GetDevice();
    if (!device)
        return VK_NULL_HANDLE;

    // Static cached descriptor set layout for input attachments
    static VkDescriptorSetLayout inputAttachmentLayout = VK_NULL_HANDLE;

    if (inputAttachmentLayout == VK_NULL_HANDLE)
    {
        // Create layout with 4 input attachment bindings (G-Buffer)
        VkDescriptorSetLayoutBinding bindings[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].pImmutableSamplers = nullptr;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 4;
        layoutInfo.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &inputAttachmentLayout) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("CreateInputAttachmentDescriptorSet_Vulkan: Failed to create descriptor set layout");
            return VK_NULL_HANDLE;
        }
    }

    // Allocate descriptor set from pool
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkImpl->GetDescriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &inputAttachmentLayout;

    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("CreateInputAttachmentDescriptorSet_Vulkan: Failed to allocate descriptor set");
        return VK_NULL_HANDLE;
    }

    // Get G-Buffer image views from current render targets
    Vector<VkDescriptorImageInfo> imageInfos;
    Vector<VkWriteDescriptorSet> writes;

    for (uint32_t i = 0; i < 4; ++i)
    {
        RenderSurface* rt = GetRenderTarget(i);
        if (!rt || !rt->GetParentTexture())
            continue;

        Texture* texture = rt->GetParentTexture();
        VkImageView imageView = texture->GetVkImageView();
        if (!imageView)
            continue;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = imageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.sampler = VK_NULL_HANDLE;  // Input attachments don't use samplers
        imageInfos.Push(imageInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = i;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos.Back();
        writes.Push(write);
    }

    // Update descriptor set with G-Buffer attachments
    if (!writes.Empty())
    {
        vkUpdateDescriptorSets(device, writes.Size(), &writes[0], 0, nullptr);
        URHO3D_LOGDEBUG("CreateInputAttachmentDescriptorSet_Vulkan: Bound " + String(writes.Size()) + " input attachments");
    }

    return descriptorSet;
}

bool Graphics::BindInputAttachmentDescriptors_Vulkan(VkDescriptorSet descriptorSet)
{
    if (descriptorSet == VK_NULL_HANDLE)
        return false;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return false;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    VkPipelineLayout pipelineLayout = vkImpl->GetCurrentPipelineLayout();

    if (!cmdBuffer || !pipelineLayout)
    {
        URHO3D_LOGWARNING("BindInputAttachmentDescriptors_Vulkan: Command buffer or pipeline layout not available");
        return false;
    }

    // Bind to descriptor set slot 3 (after materials at 0, G-Buffer textures at 1, light params at 2)
    vkCmdBindDescriptorSets(
        cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        3,  // Set slot 3 for input attachments
        1,
        &descriptorSet,
        0, nullptr
    );

    URHO3D_LOGDEBUG("BindInputAttachmentDescriptors_Vulkan: Input attachment descriptors bound to set 3");
    return true;
}

// ============================================
// Shader Loading (Vulkan)
// ============================================

ShaderVariation* Graphics::GetShader_Vulkan(ShaderType type, const String& name, const String& defines) const
{
    return GetShader_Vulkan(type, name.CString(), defines.CString());
}

ShaderVariation* Graphics::GetShader_Vulkan(ShaderType type, const char* name, const char* defines) const
{
    if (lastShaderName_ != name || !lastShader_)
    {
        auto* cache = GetSubsystem<ResourceCache>();

        String fullShaderName = shaderPath_ + name + shaderExtension_;

        // Try to reduce repeated error log prints because of missing shaders
        if (lastShaderName_ == name && !cache->Exists(fullShaderName))
            return nullptr;

        lastShader_ = cache->GetResource<Shader>(fullShaderName);
        lastShaderName_ = name;
    }

    return lastShader_ ? lastShader_->GetVariation(type, defines) : nullptr;
}

// ============================================
// Graphics State Queries (Vulkan)
// ============================================

bool Graphics::IsInitialized_Vulkan() const
{
    return window_ != nullptr && impl_ != nullptr;
}

bool Graphics::IsDeviceLost_Vulkan() const
{
    VulkanGraphicsImpl* impl = GetImpl_Vulkan();
    if (!impl)
    {
        URHO3D_LOGDEBUG("IsDeviceLost_Vulkan: No impl, returning false");
        return false;
    }

    bool isLost = impl->deviceLost_;

    // Debug logging at query point
    if (isLost)
    {
        URHO3D_LOGERROR("IsDeviceLost_Vulkan: Device is LOST (query point)");
    }
    else
    {
        URHO3D_LOGDEBUG("IsDeviceLost_Vulkan: Device is OK (query point)");
    }

    return isLost;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
