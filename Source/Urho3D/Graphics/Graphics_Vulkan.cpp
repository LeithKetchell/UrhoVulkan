//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan-specific graphics implementations

#include "../Precompiled.h"
#include "../GraphicsAPI/Vulkan/VulkanGraphicsImpl.h"
#include "../GraphicsAPI/Vulkan/VulkanShaderModule.h"
#include "../GraphicsAPI/Vulkan/VulkanPipelineState.h"
#include "../GraphicsAPI/Vulkan/VulkanConstantBuffer.h"
#include "Graphics.h"

#ifdef URHO3D_VULKAN

namespace Urho3D
{

// ============================================
// Graphics Constructor & Initialization (Phase 3)
// ============================================

void Graphics::Constructor_Vulkan()
{
    // TODO: Implement full Vulkan initialization
    // This will be filled in during Phase 3
    URHO3D_LOGINFO("Vulkan graphics constructor called");
}

// ============================================
// Frame Lifecycle Methods (Phase 3)
// ============================================

bool Graphics::BeginFrame_Vulkan()
{
    if (!impl_)
        return false;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();

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

    // Store frame data
    numPrimitives_ = 0;
    numBatches_ = 0;

    return true;
}

void Graphics::EndFrame_Vulkan()
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    // End render pass
    vkCmdEndRenderPass(cmdBuffer);

    // End command buffer recording
    if (vkEndCommandBuffer(cmdBuffer) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to end command buffer");
        return;
    }

    // Submit command buffer
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    // Wait for image acquired semaphore, signal render complete
    uint32_t frameIndex = vkImpl->GetFrameIndex();
    VkSemaphore imageAcquiredSemaphore = vkImpl->GetImpl_Vulkan()->imageAcquiredSemaphores_[frameIndex];
    VkSemaphore renderCompleteSemaphore = vkImpl->GetImpl_Vulkan()->renderCompleteSemaphores_[frameIndex];
    VkFence frameFence = vkImpl->GetImpl_Vulkan()->frameFences_[frameIndex];

    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderCompleteSemaphore;

    VkQueue graphicsQueue = vkImpl->GetGraphicsQueue();
    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, frameFence) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to submit command buffer");
        return;
    }

    // Present swapchain image
    vkImpl->Present();
}

void Graphics::Clear_Vulkan(ClearTargetFlags flags, const Color& color, float depth, unsigned stencil)
{
    if (!impl_)
        return;

    // Vulkan render pass attachment clears are configured at render pass begin
    // Clear values are set via vkCmdClearAttachments if needed during rendering
    // For now, we rely on loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR from render pass
}

void Graphics::SetViewport_Vulkan(int x, int y, int width, int height)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    VkViewport viewport{};
    viewport.x = (float)x;
    viewport.y = (float)y;
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
}

void Graphics::SetScissor_Vulkan(int x, int y, int width, int height)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    VkRect2D scissor{};
    scissor.offset = {x, y};
    scissor.extent = {(uint32_t)width, (uint32_t)height};

    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
}

// ============================================
// Vertex & Index Buffers (Phase 4)
// ============================================

void Graphics::SetVertexBuffer_Vulkan(unsigned index, VertexBuffer* buffer)
{
    if (!impl_ || !buffer)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    // Update shadow data to GPU if needed
    if (buffer->HasPendingData())
    {
        // This triggers the actual GPU upload
        buffer->OnDeviceReset();
    }

    // Get the Vulkan buffer
    VkBuffer vkBuffer = (VkBuffer)(void*)buffer->GetGPUObject();
    if (!vkBuffer)
        return;

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmdBuffer, index, 1, &vkBuffer, &offset);
}

void Graphics::SetIndexBuffer_Vulkan(IndexBuffer* buffer)
{
    if (!impl_ || !buffer)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    // Update shadow data to GPU if needed
    if (buffer->HasPendingData())
    {
        // This triggers the actual GPU upload
        buffer->OnDeviceReset();
    }

    // Get the Vulkan buffer
    VkBuffer vkBuffer = (VkBuffer)(void*)buffer->GetGPUObject();
    if (!vkBuffer)
        return;

    // Determine index type
    VkIndexType indexType = buffer->GetIndexSize() == sizeof(u32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    vkCmdBindIndexBuffer(cmdBuffer, vkBuffer, 0, indexType);
}

// ============================================
// Rendering State (Phase 4)
// ============================================

void Graphics::SetBlendMode_Vulkan(BlendMode mode, bool alphaToCoverage)
{
    // State changes are deferred until pipeline creation in PrepareDraw
    lastBlendMode_ = mode;
    lastAlphaToCoverage_ = alphaToCoverage;
}

void Graphics::SetCullMode_Vulkan(CullMode mode)
{
    lastCullMode_ = mode;
}

void Graphics::SetDepthTest_Vulkan(CompareMode mode)
{
    lastDepthTest_ = mode;
}

void Graphics::SetDepthWrite_Vulkan(bool enable)
{
    lastDepthWrite_ = enable;
}

void Graphics::SetFillMode_Vulkan(FillMode mode)
{
    lastFillMode_ = mode;
}

void Graphics::SetStencilTest_Vulkan(bool enable, CompareMode mode, StencilOp pass, StencilOp fail, StencilOp zFail, unsigned stencilRef, unsigned compareMask, unsigned writeMask)
{
    // Stencil state deferred to pipeline creation
    lastStencilTest_ = enable;
}

void Graphics::SetColorWrite_Vulkan(bool enable)
{
    lastColorWrite_ = enable;
}

// ============================================
// Draw Calls (Phase 4)
// ============================================

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned vertexStart, unsigned vertexCount)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    PrepareDraw_Vulkan();

    // Draw without index buffer
    vkCmdDraw(cmdBuffer, vertexCount, 1, vertexStart, 0);

    numPrimitives_ += vertexCount / 3;
    numBatches_++;
}

void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    PrepareDraw_Vulkan();

    // Draw with index buffer
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, minVertex, 0);

    numPrimitives_ += indexCount / 3;
    numBatches_++;
}

void Graphics::DrawInstanced_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount, unsigned instanceCount)
{
    if (!impl_)
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();

    if (!cmdBuffer)
        return;

    PrepareDraw_Vulkan();

    // Draw instanced
    vkCmdDrawIndexed(cmdBuffer, indexCount, instanceCount, indexStart, minVertex, 0);

    numPrimitives_ += (indexCount / 3) * instanceCount;
    numBatches_++;
}

void Graphics::PrepareDraw_Vulkan()
{
    if (!impl_ || !lastShaders_[VS] || !lastShaders_[PS])
        return;

    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 8: Full pipeline binding implementation
    // This prepares all rendering state for the draw call

    // 1. Validate shader programs and create shader modules
    String compileError;

    Vector<uint32_t> vertexSPIRV, pixelSPIRV;
    if (!VulkanShaderModule::GetOrCompileSPIRV(lastShaders_[VS], vertexSPIRV, compileError))
    {
        URHO3D_LOGERROR("Vertex shader compilation failed: " + compileError);
        return;
    }

    if (!VulkanShaderModule::GetOrCompileSPIRV(lastShaders_[PS], pixelSPIRV, compileError))
    {
        URHO3D_LOGERROR("Pixel shader compilation failed: " + compileError);
        return;
    }

    // Create shader modules from SPIR-V bytecode
    VkShaderModule vertexModule = VulkanShaderModule::CreateShaderModule(vkImpl->GetDevice(), vertexSPIRV);
    VkShaderModule pixelModule = VulkanShaderModule::CreateShaderModule(vkImpl->GetDevice(), pixelSPIRV);

    if (!vertexModule || !pixelModule)
    {
        URHO3D_LOGERROR("Failed to create shader modules");
        if (vertexModule) vkDestroyShaderModule(vkImpl->GetDevice(), vertexModule, nullptr);
        if (pixelModule) vkDestroyShaderModule(vkImpl->GetDevice(), pixelModule, nullptr);
        return;
    }

    // 2. Create graphics pipeline (or retrieve from cache)
    VulkanPipelineState pipelineState;
    pipelineState.cullMode = lastCullMode_;
    pipelineState.fillMode = lastFillMode_;
    pipelineState.depthTest = lastDepthTest_;
    pipelineState.depthWrite = lastDepthWrite_;
    pipelineState.blendMode = lastBlendMode_;
    pipelineState.stencilTest = lastStencilTest_;
    pipelineState.alphaToCoverage = lastAlphaToCoverage_;
    pipelineState.colorWrite = lastColorWrite_;

    // Build vertex input state (basic: position, normal, texcoord)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(float) * 8;  // pos(3) + normal(3) + uv(2)
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc[3]{};
    // Position
    attrDesc[0].binding = 0;
    attrDesc[0].location = 0;
    attrDesc[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc[0].offset = 0;
    // Normal
    attrDesc[1].binding = 0;
    attrDesc[1].location = 1;
    attrDesc[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc[1].offset = 12;
    // TexCoord
    attrDesc[2].binding = 0;
    attrDesc[2].location = 2;
    attrDesc[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrDesc[2].offset = 24;

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &bindingDesc;
    vertexInputState.vertexAttributeDescriptionCount = 3;
    vertexInputState.pVertexAttributeDescriptions = attrDesc;

    // Input assembly state
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyState.primitiveRestartEnable = VK_FALSE;

    // 3. Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertexModule;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = pixelModule;
    shaderStages[1].pName = "main";

    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;

    // Vertex input state
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;

    // Viewport & scissor (dynamic)
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    pipelineInfo.pDynamicState = &dynamicState;

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = (pipelineState.fillMode == FILL_WIREFRAME) ?
        VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = (pipelineState.cullMode == CULL_NONE) ?
        VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rasterizationState.lineWidth = 1.0f;
    pipelineInfo.pRasterizationState = &rasterizationState;

    // Multisample state
    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    pipelineInfo.pMultisampleState = &multisampleState;

    // Depth/Stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = pipelineState.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineInfo.pDepthStencilState = &depthStencilState;

    // Color blend state
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = (pipelineState.blendMode != BLEND_REPLACE) ? VK_TRUE : VK_FALSE;

    if (pipelineState.blendMode == BLEND_ALPHA)
    {
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &colorBlendAttachment;
    pipelineInfo.pColorBlendState = &colorBlendState;

    // Pipeline layout (from shader program)
    if (lastShaderProgram_)
    {
        pipelineInfo.layout = lastShaderProgram_->GetPipelineLayout();
    }

    pipelineInfo.renderPass = vkImpl->GetRenderPass();
    pipelineInfo.subpass = 0;

    // Create pipeline
    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(vkImpl->GetDevice(), vkImpl->GetPipelineCache(), 1,
                                  &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create graphics pipeline");
        vkDestroyShaderModule(vkImpl->GetDevice(), vertexModule, nullptr);
        vkDestroyShaderModule(vkImpl->GetDevice(), pixelModule, nullptr);
        return;
    }

    // 4. Bind pipeline
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // 5. Allocate and bind descriptor sets
    // Determine which descriptor bindings are active based on shader reflection
    Vector<VkDescriptorSetLayoutBinding> layoutBindings;

    // TODO: Phase 9 - Replace hardcoded bindings with shader reflection
    // For now, assume common layout: texture at binding 0, uniform buffer at binding 1

    // Texture descriptor binding
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    layoutBindings.Push(textureBinding);

    // Uniform buffer descriptor binding (for Phase 9 constant buffers)
    VkDescriptorSetLayoutBinding uniformBinding{};
    uniformBinding.binding = 1;
    uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformBinding.descriptorCount = 1;
    uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layoutBindings.Push(uniformBinding);

    // Get or create descriptor set layout with these bindings
    VkDescriptorSetLayout descriptorSetLayout = vkImpl->GetDescriptorPool()->GetOrCreateDescriptorSetLayout(
        layoutBindings);

    if (!descriptorSetLayout)
    {
        URHO3D_LOGERROR("Failed to get descriptor set layout");
        vkDestroyShaderModule(vkImpl->GetDevice(), vertexModule, nullptr);
        vkDestroyShaderModule(vkImpl->GetDevice(), pixelModule, nullptr);
        return;
    }

    // Allocate descriptor set from the pool
    VkDescriptorSet descriptorSet = vkImpl->GetDescriptorPool()->AllocateDescriptorSet(descriptorSetLayout);

    if (!descriptorSet)
    {
        URHO3D_LOGERROR("Failed to allocate descriptor set");
        vkDestroyShaderModule(vkImpl->GetDevice(), vertexModule, nullptr);
        vkDestroyShaderModule(vkImpl->GetDevice(), pixelModule, nullptr);
        return;
    }

    // Write descriptor set updates
    Vector<VkWriteDescriptorSet> writes;

    // Bind textures if available
    if (!textures_.Empty() && textures_[0])
    {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = (VkSampler)textures_[0]->GetSampler_Vulkan();
        imageInfo.imageView = (VkImageView)textures_[0]->GetImageView_Vulkan();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        writes.Push(write);
    }

    // Phase 9: Bind constant buffers (uniform buffers)
    // Update constant buffers with shader parameters
    // TODO: Implement full shader parameter binding for all parameters
    // For now, we bind hardcoded camera and transform data

    // Create a simple constant buffer for camera/transform data
    // Structure: camera matrix (64 bytes) + transform matrix (64 bytes) = 128 bytes
    struct CameraData
    {
        Matrix4 viewProj;  // 64 bytes
        Matrix4 model;     // 64 bytes
    };

    CameraData cameraData;
    cameraData.viewProj = viewMatrix_ * projMatrix_;
    cameraData.model = Matrix4::IDENTITY;  // TODO: Use actual model matrix from graphics state

    // Get or create constant buffer and update it
    VulkanConstantBuffer* constantBuffer = nullptr;
    static VulkanConstantBuffer cbuffer;
    static bool cbInitialized = false;

    if (!cbInitialized)
    {
        if (cbuffer.Create(sizeof(CameraData)))
        {
            cbInitialized = true;
        }
    }

    if (cbInitialized)
    {
        if (cbuffer.SetData(&cameraData, sizeof(CameraData)))
        {
            // Add constant buffer descriptor write
            VkDescriptorBufferInfo uniformInfo{};
            uniformInfo.buffer = cbuffer.GetBuffer();
            uniformInfo.offset = 0;
            uniformInfo.range = sizeof(CameraData);

            VkWriteDescriptorSet uniformWrite{};
            uniformWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            uniformWrite.dstSet = descriptorSet;
            uniformWrite.dstBinding = 1;  // Uniform buffer binding
            uniformWrite.dstArrayElement = 0;
            uniformWrite.descriptorCount = 1;
            uniformWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniformWrite.pBufferInfo = &uniformInfo;
            writes.Push(uniformWrite);
        }
    }

    // Update descriptor set with all writes
    if (!writes.Empty())
    {
        vkUpdateDescriptorSets(vkImpl->GetDevice(), writes.Size(), writes.Data(), 0, nullptr);
    }

    // 6. Bind descriptor sets to the pipeline
    if (lastShaderProgram_)
    {
        VkPipelineLayout pipelineLayout = lastShaderProgram_->GetPipelineLayout();
        vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                               0,  // firstSet
                               1,  // descriptorSetCount
                               &descriptorSet,
                               0, nullptr);  // dynamic offsets
    }

    // Cleanup shader modules (they're now part of the pipeline)
    vkDestroyShaderModule(vkImpl->GetDevice(), vertexModule, nullptr);
    vkDestroyShaderModule(vkImpl->GetDevice(), pixelModule, nullptr);
}

// ============================================
// Textures (Phase 5)
// ============================================

void Graphics::SetTexture_Vulkan(unsigned index, Texture* texture)
{
    if (!impl_ || index >= MAX_TEXTURE_UNITS)
        return;

    // Store texture for later descriptor set binding
    // Full implementation would create descriptor sets and bind immediately
    lastTextures_[index] = texture;

    if (!texture)
        return;

    // For now, ensure texture has valid sampler and image view
    VkSampler sampler = (VkSampler)texture->GetSampler();
    VkImageView imageView = (VkImageView)texture->GetShaderResourceView();

    if (!sampler || !imageView)
    {
        URHO3D_LOGDEBUG("Texture missing sampler or image view");
    }
}

// ============================================
// Shader Parameters (Phase 5)
// ============================================

void Graphics::SetShaderParameter_Vulkan(StringHash param, const Variant& value)
{
    // TODO: Implement shader parameter setting (Phase 5)
}

bool Graphics::NeedParameterUpdate_Vulkan(ShaderParameterGroup group, const void* source)
{
    // TODO: Implement parameter update check (Phase 5)
    return false;
}

void Graphics::SetShaders_Vulkan(ShaderVariation* vs, ShaderVariation* ps)
{
    // TODO: Implement shader binding (Phase 6)
}

// ============================================
// Render Targets (Phase 5)
// ============================================

void Graphics::SetRenderTarget_Vulkan(unsigned index, RenderSurface* renderTarget)
{
    // TODO: Implement render target setting (Phase 5)
}

void Graphics::SetDepthStencil_Vulkan(RenderSurface* depthStencil)
{
    // TODO: Implement depth stencil setting (Phase 5)
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
