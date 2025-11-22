//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/HashMap.h"
#include "../GraphicsDefs.h"
#include <vulkan/vulkan.h>

namespace Urho3D
{

class ShaderVariation;

/// Vulkan shader program linking vertex and pixel shaders
class ShaderProgram_VK : public RefCounted
{
public:
    /// Constructor
    ShaderProgram_VK();
    /// Destructor
    virtual ~ShaderProgram_VK();

    /// Link shaders and create pipeline layout
    bool Link(ShaderVariation* vs, ShaderVariation* ps);

    /// Release GPU resources
    void Release();

    /// Get pipeline layout
    VkPipelineLayout GetPipelineLayout() const { return pipelineLayout_; }

    /// Get descriptor set layout
    VkDescriptorSetLayout GetDescriptorSetLayout(uint32_t set) const;

private:
    /// Reflect SPIR-V bytecode for descriptors
    bool ReflectShaders(ShaderVariation* vs, ShaderVariation* ps);

    VkPipelineLayout pipelineLayout_{};
    Vector<VkDescriptorSetLayout> descriptorSetLayouts_;
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
