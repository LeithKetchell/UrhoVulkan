//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// SPIR-V reflection utilities for descriptor set creation (Phase 7)

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/Vector.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Urho3D
{

/// Reflection data for a single SPIR-V resource
struct SPIRVResource
{
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uint32_t descriptorCount = 1;
    VkShaderStageFlags stageFlags = 0;
};

/// SPIR-V reflection helper for analyzing shader bytecode
class VulkanSPIRVReflect
{
public:
    /// Reflect SPIR-V bytecode to extract descriptor bindings
    static bool ReflectShaderResources(
        const Vector<uint32_t>& spirvBytecode,
        VkShaderStageFlagBits shaderStage,
        Vector<SPIRVResource>& resources);

private:
    /// Validate SPIR-V module header
    static bool ValidateSPIRVHeader(const Vector<uint32_t>& spirv);

    /// Extract OpDecorate instructions for resource metadata
    static void ExtractResourceMetadata(
        const Vector<uint32_t>& spirv,
        Vector<SPIRVResource>& resources);
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
