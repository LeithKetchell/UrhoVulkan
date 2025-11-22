//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan shader module management (Phase 8)

#ifdef URHO3D_VULKAN

#include "VulkanShaderModule.h"
#include "VulkanShaderCompiler.h"
#include "../ShaderVariation.h"
#include "../../Core/Log.h"

namespace Urho3D
{

VkShaderModule VulkanShaderModule::CreateShaderModule(
    VkDevice device,
    const Vector<uint32_t>& spirvBytecode)
{
    if (!device || spirvBytecode.Empty())
    {
        URHO3D_LOGERROR("Invalid device or empty SPIR-V bytecode");
        return nullptr;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirvBytecode.Size() * sizeof(uint32_t);
    createInfo.pCode = spirvBytecode.Data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create shader module");
        return nullptr;
    }

    URHO3D_LOGDEBUG("Shader module created, size: " + String(createInfo.codeSize) + " bytes");
    return shaderModule;
}

void VulkanShaderModule::DestroyShaderModule(VkDevice device, VkShaderModule module)
{
    if (device && module)
    {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

bool VulkanShaderModule::GetOrCompileSPIRV(
    ShaderVariation* variation,
    Vector<uint32_t>& spirvBytecode,
    String& errorOutput)
{
    if (!variation)
    {
        errorOutput = "Invalid shader variation";
        return false;
    }

    // TODO: Phase 8
    // 1. Check if variation has cached SPIR-V bytecode
    // 2. If not, compile from GLSL source using VulkanShaderCompiler
    // 3. Cache the compiled bytecode in ShaderVariation
    // 4. Return the SPIR-V bytecode

    // For now, framework only
    errorOutput = "SPIR-V compilation not yet integrated";
    return false;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
