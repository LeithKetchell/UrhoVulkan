//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan-specific graphics implementations - Minimal version

#include "../Precompiled.h"
#include "../GraphicsAPI/Vulkan/VulkanGraphicsImpl.h"
#include "../GraphicsAPI/Vulkan/VulkanMaterialDescriptorManager.h"
#include "Graphics.h"
#include "Geometry.h"
#include "../IO/Log.h"

#ifdef URHO3D_VULKAN

namespace Urho3D
{

// ============================================
// Graphics Constructor & Initialization (Phase 3)
// ============================================

void Graphics::Constructor_Vulkan()
{
    URHO3D_LOGINFO("Vulkan graphics constructor called");
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
    if (!impl_)
        return false;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return false;

    // Apply user-requested MSAA setting from screen parameters
    vkImpl->SetRequestedSampleCount(screenParams_.multiSample_);

    // Acquire next swapchain image
    if (!vkImpl->AcquireNextImage())
    {
        URHO3D_LOGERROR("Failed to acquire next swapchain image");
        return false;
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
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return;

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

    VkViewport viewport{};
    viewport.x = static_cast<float>(x);
    viewport.y = static_cast<float>(y);
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
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

bool Graphics::BindMaterialDescriptors_Vulkan(Material* material) const
{
    // Phase 27A.1: Material descriptor binding for GPU access
    // Binds material descriptor sets (textures, samplers, parameters) before draw calls
    // Returns false if descriptors unavailable, true on success

    if (!impl_ || !material)
        return false;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
        return false;

    // Get command buffer for recording descriptor binding commands
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return false;

    // Get material descriptor manager
    VulkanMaterialDescriptorManager* descriptorManager = vkImpl->GetMaterialDescriptorManager();
    if (!descriptorManager || !descriptorManager->IsInitialized())
    {
        URHO3D_LOGDEBUG("Material descriptor manager not initialized");
        return false;
    }

    // Get or create descriptor set for this material
    VkDescriptorSet descriptorSet = descriptorManager->GetDescriptor(material);
    if (descriptorSet == VK_NULL_HANDLE)
    {
        URHO3D_LOGWARNING("Failed to get descriptor set for material");
        return false;
    }

    // Phase 27A.2: Bind descriptor set for fragment shader textures and samplers
    // Descriptor set 0: Material textures, samplers, and material parameters
    // Requires pipeline layout to be set by graphics pipeline setup
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

    URHO3D_LOGDEBUG("Material descriptors bound successfully");
    return true;
}

void Graphics::Draw_Vulkan(Geometry* geometry, Material* material)
{
    // Phase 27B: Render command recording with descriptor binding integration
    // Records geometry draw command with material descriptors bound for proper GPU access
    // Integrates Phases 17-26 descriptor pipeline into actual rendering

    if (!geometry || geometry->IsEmpty() || !material)
        return;

    // Bind material descriptors before draw call
    // This provides textures, samplers, and material constants to shaders
    if (!BindMaterialDescriptors_Vulkan(material))
    {
        URHO3D_LOGWARNING("Failed to bind material descriptors, draw may be incomplete");
        // Continue anyway - descriptor binding failures should not block rendering
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

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 32 Step 3: Apply graphics state
    VulkanPipelineState pipelineState;
    ApplyGraphicsState_Vulkan(pipelineState);

    // Phase 33 Step 2: Compile and get shader modules
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule))
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to create shader modules");
        return;
    }

    // Get or create graphics pipeline from cached state and shader modules
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
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

    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule);
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
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Phase 33 Step 3: Descriptor binding infrastructure ready
    // Material descriptor sets will be bound during geometry rendering
    // when material parameters are applied via BindMaterialDescriptors()

    // Record draw command
    vkCmdDraw(cmdBuffer, vertexCount, 1, vertexStart, 0);

    // Clean up shader modules after pipeline is created (pipeline retains a reference)
    if (vsModule)
        vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule)
        vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("Draw_Vulkan: vertexStart=" + String(vertexStart) + " vertexCount=" + String(vertexCount));
}

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    if (!impl_ || indexCount == 0)
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

    // Phase 33 Step 2: Compile and get shader modules
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule))
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to create shader modules");
        return;
    }

    // Get or create graphics pipeline from cached state and shader modules
    VkPipelineLayout layout = vkImpl->GetCurrentPipelineLayout();
    VkRenderPass renderPass = vkImpl->GetRenderPass();

    if (!layout || !renderPass)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Invalid pipeline layout or render pass");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Record indexed draw command
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, minVertex, 0);

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("Draw_Vulkan: indexStart=" + String(indexStart) + " indexCount=" + String(indexCount));
}

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned baseVertexIndex, unsigned minVertex, unsigned vertexCount)
{
    if (!impl_ || indexCount == 0)
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

    // Phase 33 Step 2: Compile and get shader modules
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule))
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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Record draw command with base vertex index
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, baseVertexIndex, 0);

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

    // Phase 33 Step 2: Compile and get shader modules
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule))
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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Record instanced draw command
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, minVertex, 0);

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("DrawInstanced_Vulkan: indexStart=" + String(indexStart) + " indexCount=" + String(indexCount) + " instances=" + String(instanceCount));
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

    // Phase 33 Step 2: Compile and get shader modules
    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule fsModule = VK_NULL_HANDLE;

    if (!vkImpl->CreateShaderModules(vertexShader_, pixelShader_, vsModule, fsModule))
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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Record instanced draw command with base vertex index
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, baseVertexIndex, 0);

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("DrawInstanced_Vulkan: indexStart=" + String(indexStart) + " indexCount=" + String(indexCount) + " instances=" + String(instanceCount) + " baseVertexIndex=" + String(baseVertexIndex));
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

void Graphics::SetColorWrite_Vulkan(bool enable)
{
    colorWrite_ = enable;
}

void Graphics::SetTexture_Vulkan(unsigned index, Texture* texture)
{
    // Texture binding not yet implemented
}

void Graphics::SetRenderTarget_Vulkan(unsigned index, RenderSurface* renderTarget)
{
    // Render target binding not yet implemented
}

void Graphics::SetDepthStencil_Vulkan(RenderSurface* depthStencil)
{
    // Depth stencil binding not yet implemented
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
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
