//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan shader program implementation (Phase 6)

#ifdef URHO3D_VULKAN

#include "VulkanShaderProgram.h"
#include "VulkanGraphicsImpl.h"
#include "VulkanShaderCompiler.h"
#include "../Shader.h"
#include "../ShaderVariation.h"
#include "../../Core/Log.h"

namespace Urho3D
{

ShaderProgram_VK::ShaderProgram_VK()
{
}

ShaderProgram_VK::~ShaderProgram_VK()
{
    Release();
}

void ShaderProgram_VK::Release()
{
    if (pipelineLayout_)
    {
        Graphics* graphics = GetSubsystem<Graphics>();
        if (graphics)
        {
            VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
            if (impl)
            {
                vkDestroyPipelineLayout(impl->GetDevice(), pipelineLayout_, nullptr);
            }
        }
        pipelineLayout_ = nullptr;
    }

    for (VkDescriptorSetLayout layout : descriptorSetLayouts_)
    {
        if (layout)
        {
            Graphics* graphics = GetSubsystem<Graphics>();
            if (graphics)
            {
                VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
                if (impl)
                {
                    vkDestroyDescriptorSetLayout(impl->GetDevice(), layout, nullptr);
                }
            }
        }
    }
    descriptorSetLayouts_.Clear();
}

bool ShaderProgram_VK::Link(ShaderVariation* vs, ShaderVariation* ps)
{
    Release();

    if (!vs || !ps)
    {
        URHO3D_LOGERROR("Invalid shader variations for linking");
        return false;
    }

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
    {
        URHO3D_LOGERROR("Graphics subsystem not available");
        return false;
    }

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
    {
        URHO3D_LOGERROR("Vulkan graphics implementation not available");
        return false;
    }

    // For now, create a minimal pipeline layout without descriptor sets
    // Full implementation would reflect SPIR-V and create descriptor layouts
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 0;
    layoutInfo.pSetLayouts = nullptr;
    layoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(impl->GetDevice(), &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create pipeline layout for shader program");
        return false;
    }

    URHO3D_LOGDEBUG("Shader program linked successfully");
    return true;
}

VkDescriptorSetLayout ShaderProgram_VK::GetDescriptorSetLayout(uint32_t set) const
{
    if (set < descriptorSetLayouts_.Size())
        return descriptorSetLayouts_[set];
    return nullptr;
}

bool ShaderProgram_VK::ReflectShaders(ShaderVariation* vs, ShaderVariation* ps)
{
    // TODO: Implement SPIR-V reflection (Phase 7)
    // This would involve:
    // 1. Getting SPIR-V bytecode from both shader variations
    // 2. Using spirv-reflect or glslang reflection to analyze descriptor bindings
    // 3. Creating VkDescriptorSetLayout objects for each set
    // 4. Updating the pipeline layout with descriptor set layouts
    return false;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
