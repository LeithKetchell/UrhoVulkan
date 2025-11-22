//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan graphics pipeline state management (Phase 8)

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/Vector.h"
#include "../../Container/HashMap.h"
#include "../../Math/StringHash.h"
#include "../GraphicsDefs.h"
#include <vulkan/vulkan.h>
#include <functional>

namespace Urho3D
{

/// Pipeline state for caching and reuse
struct VulkanPipelineState
{
    // Rasterization state
    CullMode cullMode = CULL_CCW;
    FillMode fillMode = FILL_SOLID;
    bool polygonOffsetEnable = false;

    // Depth/Stencil state
    CompareMode depthTest = CMP_LESSEQUAL;
    bool depthWrite = true;
    bool stencilTest = false;

    // Blend state
    BlendMode blendMode = BLEND_REPLACE;
    bool alphaToCoverage = false;

    // Color write
    bool colorWrite = true;

    // Hash for caching
    unsigned Hash() const;
    bool operator==(const VulkanPipelineState& other) const;
};

/// Manages graphics pipeline creation and caching
class VulkanPipelineCache
{
public:
    /// Constructor
    VulkanPipelineCache();
    /// Destructor
    ~VulkanPipelineCache();

    /// Initialize pipeline cache
    bool Initialize(VkDevice device, VkPipelineLayout pipelineLayout);

    /// Release resources
    void Release();

    /// Get or create graphics pipeline
    VkPipeline GetOrCreateGraphicsPipeline(
        const VulkanPipelineState& state,
        VkRenderPass renderPass,
        VkShaderModule vertexShader,
        VkShaderModule fragmentShader,
        const VkPipelineVertexInputStateCreateInfo& vertexInputState);

    /// Get device
    VkDevice GetDevice() const { return device_; }

private:
    VkDevice device_{};
    VkPipelineLayout pipelineLayout_{};
    VkPipelineCache pipelineCache_{};

    // Cached pipelines by state hash
    HashMap<StringHash, VkPipeline> pipelineCache_;
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
