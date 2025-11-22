//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan graphics pipeline state management (Phase 8)

#ifdef URHO3D_VULKAN

#include "VulkanPipelineState.h"
#include "../../Core/Log.h"

namespace Urho3D
{

unsigned VulkanPipelineState::Hash() const
{
    unsigned hash = 0;
    hash = hash * 31 + cullMode;
    hash = hash * 31 + fillMode;
    hash = hash * 31 + polygonOffsetEnable;
    hash = hash * 31 + depthTest;
    hash = hash * 31 + depthWrite;
    hash = hash * 31 + stencilTest;
    hash = hash * 31 + blendMode;
    hash = hash * 31 + alphaToCoverage;
    hash = hash * 31 + colorWrite;
    return hash;
}

bool VulkanPipelineState::operator==(const VulkanPipelineState& other) const
{
    return cullMode == other.cullMode &&
           fillMode == other.fillMode &&
           polygonOffsetEnable == other.polygonOffsetEnable &&
           depthTest == other.depthTest &&
           depthWrite == other.depthWrite &&
           stencilTest == other.stencilTest &&
           blendMode == other.blendMode &&
           alphaToCoverage == other.alphaToCoverage &&
           colorWrite == other.colorWrite;
}

VulkanPipelineCache::VulkanPipelineCache()
{
}

VulkanPipelineCache::~VulkanPipelineCache()
{
    Release();
}

bool VulkanPipelineCache::Initialize(VkDevice device, VkPipelineLayout pipelineLayout)
{
    if (!device || !pipelineLayout)
    {
        URHO3D_LOGERROR("Invalid device or pipeline layout");
        return false;
    }

    device_ = device;
    pipelineLayout_ = pipelineLayout;

    // Create pipeline cache for faster pipeline creation
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = 0;
    cacheInfo.pInitialData = nullptr;

    if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create pipeline cache");
        return false;
    }

    URHO3D_LOGDEBUG("Pipeline cache initialized");
    return true;
}

void VulkanPipelineCache::Release()
{
    // Destroy cached pipelines
    for (auto& pair : pipelineCache_)
    {
        if (pair.second_ && device_)
        {
            vkDestroyPipeline(device_, pair.second_, nullptr);
        }
    }
    pipelineCache_.Clear();

    // Destroy pipeline cache
    if (pipelineCache_ && device_)
    {
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        pipelineCache_ = nullptr;
    }

    device_ = nullptr;
    pipelineLayout_ = nullptr;
}

VkPipeline VulkanPipelineCache::GetOrCreateGraphicsPipeline(
    const VulkanPipelineState& state,
    VkRenderPass renderPass,
    VkShaderModule vertexShader,
    VkShaderModule fragmentShader,
    const VkPipelineVertexInputStateCreateInfo& vertexInputState)
{
    if (!device_ || !renderPass || !vertexShader || !fragmentShader)
    {
        URHO3D_LOGERROR("Invalid parameters for graphics pipeline creation");
        return nullptr;
    }

    // Check cache first
    StringHash stateHash(state.Hash());
    auto it = pipelineCache_.Find(stateHash);
    if (it != pipelineCache_.End())
    {
        return it->second_;
    }

    // Create new pipeline
    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};

    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertexShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragmentShader;
    shaderStages[1].pName = "main";

    // Viewport and scissor (dynamic)
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Rasterization state from pipeline state
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = (state.fillMode == FILL_WIREFRAME) ?
        VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;

    switch (state.cullMode)
    {
    case CULL_NONE:
        rasterizationState.cullMode = VK_CULL_MODE_NONE;
        break;
    case CULL_CCW:
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        break;
    case CULL_CW:
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
        break;
    default:
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        break;
    }

    rasterizationState.depthClampEnable = VK_FALSE;
    rasterizationState.rasterizerDiscardEnable = VK_FALSE;
    rasterizationState.lineWidth = 1.0f;

    // Depth/Stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = state.depthWrite ? VK_TRUE : VK_FALSE;

    // Map Urho3D compare mode to Vulkan
    switch (state.depthTest)
    {
    case CMP_ALWAYS: depthStencilState.depthCompareOp = VK_COMPARE_OP_ALWAYS; break;
    case CMP_EQUAL: depthStencilState.depthCompareOp = VK_COMPARE_OP_EQUAL; break;
    case CMP_LESS: depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS; break;
    case CMP_LESSEQUAL: depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; break;
    case CMP_GREATER: depthStencilState.depthCompareOp = VK_COMPARE_OP_GREATER; break;
    case CMP_GREATEREQUAL: depthStencilState.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
    case CMP_NOTEQUAL: depthStencilState.depthCompareOp = VK_COMPARE_OP_NOT_EQUAL; break;
    default: depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; break;
    }

    depthStencilState.stencilTestEnable = state.stencilTest ? VK_TRUE : VK_FALSE;

    // Blend state - simple alpha blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = state.colorWrite ?
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT : 0;

    // Map blend mode
    switch (state.blendMode)
    {
    case BLEND_REPLACE:
        colorBlendAttachment.blendEnable = VK_FALSE;
        break;
    case BLEND_ALPHA:
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_ADD:
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_MULTIPLY:
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        break;
    default:
        colorBlendAttachment.blendEnable = VK_FALSE;
        break;
    }

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.logicOpEnable = VK_FALSE;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &colorBlendAttachment;

    // Multisample state
    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.sampleShadingEnable = VK_FALSE;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampleState.alphaToCoverageEnable = state.alphaToCoverage ? VK_TRUE : VK_FALSE;

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = nullptr;  // Set by caller
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create graphics pipeline");
        return nullptr;
    }

    // Cache it
    pipelineCache_[stateHash] = pipeline;

    URHO3D_LOGDEBUG("Created graphics pipeline with state hash " + String(state.Hash()));
    return pipeline;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
