//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan-specific graphics implementations - Minimal version

#include "../Precompiled.h"
#include "../GraphicsAPI/Vulkan/VulkanGraphicsImpl.h"
#include "../GraphicsAPI/Vulkan/VulkanMaterialDescriptorManager.h"
#include "../GraphicsAPI/Vulkan/VulkanShaderModule.h"
#include "../GraphicsAPI/Vulkan/VulkanShaderProgram.h"
#include "Graphics.h"
#include "Geometry.h"
#include "../GraphicsAPI/Texture.h"
#include "../GraphicsAPI/RenderSurface.h"
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

    // Static cached descriptor set layout for G-Buffer textures
    static VkDescriptorSetLayout textureLayout = VK_NULL_HANDLE;

    if (textureLayout == VK_NULL_HANDLE)
    {
        // Create layout with 8 combined image-sampler bindings for texture units
        VkDescriptorSetLayoutBinding bindings[MAX_TEXTURE_UNITS];
        for (unsigned i = 0; i < MAX_TEXTURE_UNITS; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].pImmutableSamplers = nullptr;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = MAX_TEXTURE_UNITS;
        layoutInfo.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &textureLayout) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("CreateGBufferTextureDescriptorSet_Vulkan: Failed to create descriptor set layout");
            return VK_NULL_HANDLE;
        }
    }

    // Allocate descriptor set from pool
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkImpl->GetDescriptorPool();
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &textureLayout;

    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("CreateGBufferTextureDescriptorSet_Vulkan: Failed to allocate descriptor set");
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

void Graphics::Draw_Vulkan(Geometry* geometry, Material* material)
{
    // Phase 27B + Phase 36: Render command recording with full descriptor binding
    // Records geometry draw command with material + texture descriptors bound
    // Integrates Phases 17-26 descriptor pipeline + Phase 36 G-Buffer textures

    if (!geometry || geometry->IsEmpty() || !material)
        return;

    // Bind material descriptors before draw call (Set 0)
    // This provides textures, samplers, and material constants to shaders
    if (!BindMaterialDescriptors_Vulkan(material))
    {
        URHO3D_LOGWARNING("Failed to bind material descriptors, draw may be incomplete");
        // Continue anyway - descriptor binding failures should not block rendering
    }

    // Bind G-Buffer texture descriptors for deferred lighting (Set 1)
    // This provides access to G-Buffer textures during lighting pass
    if (!BindGBufferTextureDescriptors_Vulkan())
    {
        URHO3D_LOGDEBUG("Failed to bind G-Buffer texture descriptors");
        // Non-fatal - forward rendering doesn't need G-Buffer textures
    }

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

    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule, gsModule);
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

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

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

    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("Draw_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

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
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(layout, renderPass, pipelineState, vsModule, fsModule, gsModule);
    if (!pipeline)
    {
        URHO3D_LOGWARNING("DrawInstanced_Vulkan: Failed to get or create graphics pipeline");
        if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);
        return;
    }

    // Bind the graphics pipeline for this draw call
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Phase 36 Step 4: Upload pending shader parameters before draw
    UploadPendingShaderParameters_Vulkan();

    // Record instanced draw command with base vertex index
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, baseVertexIndex, 0);

    // Clean up shader modules after pipeline is created
    if (vsModule) vkDestroyShaderModule(vkImpl->GetDevice(), vsModule, nullptr);
    if (fsModule) vkDestroyShaderModule(vkImpl->GetDevice(), fsModule, nullptr);

    URHO3D_LOGDEBUG("DrawInstanced_Vulkan: indexStart=" + String(indexStart) + " indexCount=" + String(indexCount) + " instances=" + String(instanceCount) + " baseVertexIndex=" + String(baseVertexIndex));
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

    if (index >= MAX_TEXTURE_UNITS)
    {
        URHO3D_LOGERROR("SetTexture_Vulkan: Texture unit index out of range: " + String(index));
        return;
    }

    // Update texture binding state
    textures_[index] = texture;

    URHO3D_LOGDEBUG("SetTexture_Vulkan: Bound texture to unit " + String(index) +
                    (texture ? " (texture set)" : " (null)"));
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
    // Phase 36A: Create multi-set pipeline layouts for deferred rendering
    // Store shader pointers for pipeline creation
    vertexShader_ = vs;
    pixelShader_ = ps;
    geometryShader_ = gs;

    URHO3D_LOGDEBUG("SetShaders_Vulkan: VS=" + String(vs ? vs->GetFullName() : "null") +
                    " PS=" + String(ps ? ps->GetFullName() : "null") +
                    " GS=" + String(gs ? gs->GetFullName() : "null"));

    // Phase 36A: Create or retrieve shader program with multi-set pipeline layout
    if (vs && ps)
    {
        VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
        if (!vkImpl)
            return;

        VkDevice device = vkImpl->GetDevice();
        if (!device)
            return;

        // Create shader program (combines VS and PS parameters)
        VulkanShaderProgram* program = new VulkanShaderProgram(vs, ps);
        if (!program)
        {
            URHO3D_LOGERROR("SetShaders_Vulkan: Failed to create shader program");
            return;
        }

        // Create multi-set pipeline layout if not already created
        if (program->GetPipelineLayout() == VK_NULL_HANDLE)
        {
            // Phase 36A Profiler Integration: Track pipeline layout creation time
            VulkanProfiler* profiler = GetVulkanProfiler();
            if (profiler)
                profiler->StartPhase("Phase36A: Pipeline Layout Creation");

            // Build descriptor set layout array for multi-set binding
            // Set 0: Material descriptors (textures, samplers, material parameters)
            // Set 1: G-Buffer texture descriptors (for deferred lighting pass)
            // Set 2: Constant buffer descriptors (light parameters)
            Vector<VkDescriptorSetLayout> layouts;

            // Always include material descriptor layout (Set 0)
            VkDescriptorSetLayout materialLayout = vkImpl->GetMaterialDescriptorLayout();
            if (materialLayout != VK_NULL_HANDLE)
            {
                layouts.Push(materialLayout);
                URHO3D_LOGDEBUG("SetShaders_Vulkan: Added material descriptor layout (Set 0)");
            }

            // Include G-Buffer texture layout for deferred rendering (Set 1)
            VkDescriptorSetLayout gbufferLayout = vkImpl->GetGBufferTextureLayout();
            if (gbufferLayout != VK_NULL_HANDLE)
            {
                layouts.Push(gbufferLayout);
                URHO3D_LOGDEBUG("SetShaders_Vulkan: Added G-Buffer texture layout (Set 1)");
            }

            // Include constant buffer layout for light parameters (Set 2)
            VkDescriptorSetLayout constantLayout = vkImpl->GetConstantBufferLayout();
            if (constantLayout != VK_NULL_HANDLE)
            {
                layouts.Push(constantLayout);
                URHO3D_LOGDEBUG("SetShaders_Vulkan: Added constant buffer layout (Set 2)");
            }

            // Create multi-set pipeline layout
            if (!layouts.Empty())
            {
                if (program->CreatePipelineLayout(device, layouts))
                {
                    URHO3D_LOGDEBUG(String("SetShaders_Vulkan: Created pipeline layout with ") + String(layouts.Size()) + " descriptor set(s)");
                }
                else
                {
                    URHO3D_LOGERROR("SetShaders_Vulkan: Failed to create pipeline layout");
                    delete program;
                    return;
                }
            }
            else
            {
                URHO3D_LOGWARNING("SetShaders_Vulkan: No descriptor set layouts available");
            }

            // Phase 36A Profiler Integration: End tracking
            if (profiler)
                profiler->EndPhase();
        }

        // Store pipeline layout in graphics impl for descriptor binding
        VkPipelineLayout pipelineLayout = program->GetPipelineLayout();
        if (pipelineLayout != VK_NULL_HANDLE)
        {
            vkImpl->SetCurrentPipelineLayout(pipelineLayout);
            URHO3D_LOGDEBUG("SetShaders_Vulkan: Pipeline layout set successfully");
        }

        // Clean up temporary shader program (layout is now cached)
        // TODO: Cache shader programs for reuse instead of recreating
        delete program;
    }

    // Pipeline will be recreated with new shader configuration on next draw call
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

    // Bind to descriptor set slot 2 (after materials at 0 and G-Buffer textures at 1)
    vkCmdBindDescriptorSets(
        cmdBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        2,  // Set slot 2 for light parameters
        1,
        &descriptorSet,
        0, nullptr
    );

    URHO3D_LOGDEBUG("BindConstantBufferDescriptors_Vulkan: Bound constant buffer descriptor set to slot 2");
    return true;
}


/// Phase 36 Step 4: Upload pending shader parameters to GPU
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
    // Early exit if no parameters to upload
    if (pendingShaderParameters_.Empty())
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl)
    {
        URHO3D_LOGWARNING("UploadPendingShaderParameters_Vulkan: Graphics implementation not initialized");
        return;
    }

    VulkanConstantBufferPool* cbPool = vkImpl->GetConstantBufferPool();
    if (!cbPool)
    {
        URHO3D_LOGWARNING("UploadPendingShaderParameters_Vulkan: Constant buffer pool not available");
        return;
    }

    // Step 1: Calculate buffer size with std140 layout
    size_t totalSize = CalculateParameterBufferSize(pendingShaderParameters_);
    if (totalSize == 0)
    {
        URHO3D_LOGWARNING("UploadPendingShaderParameters_Vulkan: No valid parameters to upload");
        pendingShaderParameters_.Clear();
        return;
    }

    // Step 2: Allocate staging buffer and pack parameters
    Vector<unsigned char> stagingBuffer(totalSize);
    PackShaderParameters(pendingShaderParameters_, &stagingBuffer[0], totalSize);

    // Step 3: Allocate GPU buffer from pool
    VkBuffer gpuBuffer = VK_NULL_HANDLE;
    VkDeviceSize bufferOffset = 0;

    if (!cbPool->AllocateBuffer(&stagingBuffer[0], (uint32_t)totalSize, gpuBuffer, bufferOffset))
    {
        URHO3D_LOGERROR("UploadPendingShaderParameters_Vulkan: Failed to allocate constant buffer from pool");
        pendingShaderParameters_.Clear();
        return;
    }

    // Step 4: Create descriptor set for constant buffer
    VkDescriptorSet descriptorSet = CreateConstantBufferDescriptorSet_Vulkan(gpuBuffer, totalSize);
    if (descriptorSet == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("UploadPendingShaderParameters_Vulkan: Failed to create constant buffer descriptor set");
        pendingShaderParameters_.Clear();
        return;
    }

    // Step 5: Bind descriptor set to pipeline (slot 2 for light parameters)
    if (!BindConstantBufferDescriptors_Vulkan(descriptorSet))
    {
        URHO3D_LOGWARNING("UploadPendingShaderParameters_Vulkan: Failed to bind constant buffer descriptors");
        // Continue anyway - descriptor binding failures should not block rendering
    }

    // Step 6: Clear pending parameters for next frame
    pendingShaderParameters_.Clear();

    URHO3D_LOGDEBUG("UploadPendingShaderParameters_Vulkan: Uploaded " + String(totalSize) +
                    " bytes of shader parameters to GPU");
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

} // namespace Urho3D

#endif  // URHO3D_VULKAN
