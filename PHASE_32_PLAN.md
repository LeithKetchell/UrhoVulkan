# Phase 32: Vulkan Deferred Rendering GPU State Application - Implementation Plan

**Objective:** Convert cached graphics state to actual Vulkan pipeline state, enabling deferred light volume rendering.

## Current Status (End of Phase 31)

**What's Working:**
- Graphics state functions route to Vulkan implementations ✅
- State is cached in Graphics class members ✅
- G-Buffer attachments created ✅
- Deferred shaders compile to SPIR-V ✅
- All 55+ samples compile ✅

**What's NOT Working:**
- Cached state is never applied to Vulkan pipelines ❌
- Blend modes not applied ❌
- Depth tests not applied ❌
- Stencil tests not applied ❌
- Deferred rendering produces no visible output ❌

## Phase 32 Implementation Strategy

### Step 1: Implement VulkanGraphicsImpl::ApplyGraphicsState()

**Location:** `VulkanGraphicsImpl.cpp` (new method)

**Signature:**
```cpp
void VulkanGraphicsImpl::ApplyGraphicsState(VulkanPipelineState& state)
{
    // Convert cached Graphics state to VulkanPipelineState struct
    // Called before creating/binding graphics pipeline
}
```

**Implementation:**
```cpp
void VulkanGraphicsImpl::ApplyGraphicsState(VulkanPipelineState& state)
{
    // Map Graphics::blendMode_ → VkPipelineColorBlendStateCreateInfo
    state.blendMode = blendMode_;
    state.alphaToCoverage = alphaToCoverage_;

    // Map Graphics::depthTestMode_ → VkPipelineDepthStencilStateCreateInfo
    state.depthTest = depthTestMode_;
    state.depthWrite = depthWrite_;

    // Map Graphics::stencilTest_ → VkPipelineDepthStencilStateCreateInfo stencil
    state.stencilTest = stencilTest_;
    state.stencilTestMode = stencilTestMode_;
    state.stencilPass = stencilPass_;
    state.stencilFail = stencilFail_;
    state.stencilZFail = stencilZFail_;
    state.stencilRef = stencilRef_;
    state.stencilCompareMask = stencilCompareMask_;
    state.stencilWriteMask = stencilWriteMask_;

    // Map Graphics::cullMode_ → VkPipelineRasterizationStateCreateInfo
    state.cullMode = cullMode_;

    // Map Graphics::fillMode_ → VkPipelineRasterizationStateCreateInfo
    state.fillMode = fillMode_;
}
```

### Step 2: Extend VulkanPipelineState Structure

**Location:** `VulkanDefs.h` (update struct)

**Current State:**
```cpp
struct VulkanPipelineState {
    // Rasterization
    VkCullModeFlags cullMode;
    VkPolygonMode fillMode;

    // Depth & Stencil
    VkCompareOp depthTest;
    bool depthWrite;

    // Blend
    BlendMode blendMode;
    bool alphaToCoverage;

    // Stencil (NEW)
    bool stencilTest;
    CompareMode stencilTestMode;
    StencilOp stencilPass;
    StencilOp stencilFail;
    StencilOp stencilZFail;
    u32 stencilRef;
    u32 stencilCompareMask;
    u32 stencilWriteMask;
};
```

### Step 3: Implement State-to-VkPipeline Conversion Functions

**Location:** `VulkanGraphicsImpl.cpp` (new helper functions)

```cpp
// Convert BlendMode enum to VkPipelineColorBlendAttachmentState
VkPipelineColorBlendAttachmentState ConvertBlendMode(BlendMode mode, bool alphaToCoverage)
{
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    switch (mode)
    {
    case BLEND_REPLACE:
        blendAttachment.blendEnable = false;
        break;

    case BLEND_ADD:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;

    case BLEND_SUBTRACT:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;  // 1 - src
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        break;

    case BLEND_MULTIPLY:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;

    // ... more blend modes
    }

    return blendAttachment;
}

// Convert CompareMode to VkCompareOp
VkCompareOp ConvertCompareMode(CompareMode mode)
{
    switch (mode)
    {
    case CMP_ALWAYS: return VK_COMPARE_OP_ALWAYS;
    case CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case CMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case CMP_LESS: return VK_COMPARE_OP_LESS;
    case CMP_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CMP_NEVER: return VK_COMPARE_OP_NEVER;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// Convert CullMode to VkCullModeFlags
VkCullModeFlags ConvertCullMode(CullMode mode)
{
    switch (mode)
    {
    case CULL_NONE: return VK_CULL_MODE_NONE;
    case CULL_CCW: return VK_CULL_MODE_BACK_BIT;
    case CULL_CW: return VK_CULL_MODE_FRONT_BIT;
    default: return VK_CULL_MODE_BACK_BIT;
    }
}

// Convert StencilOp to VkStencilOp
VkStencilOp ConvertStencilOp(StencilOp op)
{
    switch (op)
    {
    case OP_KEEP: return VK_STENCIL_OP_KEEP;
    case OP_ZERO: return VK_STENCIL_OP_ZERO;
    case OP_REF: return VK_STENCIL_OP_REPLACE;
    case OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case OP_INVERT: return VK_STENCIL_OP_INVERT;
    case OP_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case OP_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
```

### Step 4: Create/Cache Graphics Pipelines with State

**Location:** `VulkanGraphicsImpl.cpp` (update GetOrCreateGraphicsPipeline)

**Current Code (Graphics_Vulkan.cpp, line 388):**
```cpp
// TODO: Get or create graphics pipeline
// vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
```

**Phase 32 Implementation:**
```cpp
VkPipeline VulkanGraphicsImpl::GetOrCreateGraphicsPipeline(
    VkPipelineLayout layout,
    VkRenderPass renderPass,
    const VulkanPipelineState& state)
{
    // Generate state hash
    uint64_t stateHash = HashGraphicsState(state);

    // Check cache
    auto it = pipelineCache_.find(stateHash);
    if (it != pipelineCache_.end())
        return it->second;

    // Create new pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.pViewportState = &viewportState_;  // Already set
    pipelineInfo.pRasterizationState = BuildRasterizationState(state);
    pipelineInfo.pColorBlendState = BuildColorBlendState(state);
    pipelineInfo.pDepthStencilState = BuildDepthStencilState(state);
    pipelineInfo.pInputAssemblyState = &inputAssemblyState_;  // Already set
    pipelineInfo.pVertexInputState = &vertexInputState_;  // Already set
    pipelineInfo.pMultisampleState = &multisampleState_;  // Already set
    pipelineInfo.pDynamicState = &dynamicState_;  // Already set

    VkPipeline pipeline;
    if (vkCreateGraphicsPipeline(device_, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }

    pipelineCache_[stateHash] = pipeline;
    return pipeline;
}
```

### Step 5: Bind Pipeline in Draw Calls

**Location:** `Graphics_Vulkan.cpp` (update Draw_Vulkan, line 376)

**Current Code:**
```cpp
void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    // ... setup code ...

    // TODO: Get or create graphics pipeline
    // vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, minVertex, 0);
}
```

**Phase 32 Implementation:**
```cpp
void Graphics::Draw_Vulkan(PrimitiveType type, unsigned indexStart, unsigned indexCount, unsigned minVertex, unsigned vertexCount)
{
    VulkanGraphicsImpl* vkImpl = GetImpl_Vulkan();
    if (!vkImpl || indexCount == 0)
        return;

    VkCommandBuffer cmdBuffer = vkImpl->GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Build pipeline state from cached graphics state
    VulkanPipelineState pipelineState;
    vkImpl->ApplyGraphicsState(pipelineState);

    // Get or create pipeline with this state
    VkPipeline pipeline = vkImpl->GetOrCreateGraphicsPipeline(
        vkImpl->GetPipelineLayout(),
        vkImpl->GetRenderPass(),
        pipelineState);

    if (pipeline == VK_NULL_HANDLE)
        return;

    // Bind pipeline and issue draw
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDrawIndexed(cmdBuffer, indexCount, 1, indexStart, minVertex, 0);
}
```

### Step 6: Handle Dynamic States (Viewport, Scissor)

Some states can be dynamic (set per-draw) rather than baked into the pipeline:

```cpp
// In GetOrCreateGraphicsPipeline, enable dynamic states
VkDynamicState dynamicStates[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS,  // For blend constant colors
};

pipelineInfo.pDynamicState = &dynamicStateInfo;

// Then in draw calls, set these dynamically:
VkViewport viewport = { ... };
vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

VkRect2D scissor = { ... };
vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

VkClearColorValue blendConstants = { 1.0f, 1.0f, 1.0f, 1.0f };
vkCmdSetBlendConstants(cmdBuffer, blendConstants.float32);
```

## Implementation Tasks

1. **Add conversion functions** (ConvertBlendMode, ConvertCompareMode, etc.)
2. **Implement ApplyGraphicsState()**
3. **Implement GetOrCreateGraphicsPipeline() with caching**
4. **Update VulkanPipelineState structure** for all required states
5. **Build pipeline creation structs** (rasterization, color blend, depth-stencil)
6. **Implement draw call state binding** (vkCmdBindPipeline)
7. **Handle dynamic states** (viewport, scissor, blend constants)
8. **Add pipeline cache cleanup** in DestroyDevice()
9. **Test with Sample 56** - deferred rendering should now be visible

## Expected Results After Phase 32

- ✅ Blend modes apply correctly (ADD for lights, SUBTRACT for negative lights)
- ✅ Depth testing enables light volume culling
- ✅ Stencil testing masks deferred lighting to only lit pixels
- ✅ Cull mode changes handle inside/outside light volume detection
- ✅ Sample 56 renders deferred lighting correctly
- ✅ 16-light deferred scene renders properly
- ✅ Performance monitored via ProfilerUI

## Risk Assessment

**Low Risk:**
- Using standard Vulkan patterns for pipeline creation
- Conversion functions are straightforward enum→enum mappings
- Pipeline caching is optional optimization (correctness works without it)

**Medium Risk:**
- Pipeline layout compatibility with shaders
- Dynamic state synchronization
- Descriptor binding to pipelines (defer to Phase 33)

## Dependencies

- Phase 31 completion (graphics state dispatch routing) ✅
- VulkanShaderCompiler from Phase 6 ✅
- G-Buffer from Phase 31 ✅

## Timeline Estimate

- **Conversion functions:** 1-2 hours
- **Pipeline creation & caching:** 2-3 hours
- **Draw call binding:** 1 hour
- **Testing & debugging:** 2-3 hours
- **Total:** ~6-9 hours

---

**Status:** Ready for implementation
**Priority:** Critical (enables deferred rendering on Vulkan)
**Depends on:** Phase 31 ✅
**Blocks:** Phase 33 (Input Attachments & Descriptors)
