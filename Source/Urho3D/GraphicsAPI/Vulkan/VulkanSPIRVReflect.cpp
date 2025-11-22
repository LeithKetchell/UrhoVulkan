//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// SPIR-V reflection utilities for descriptor set creation (Phase 7)

#ifdef URHO3D_VULKAN

#include "VulkanSPIRVReflect.h"
#include "../../Core/Log.h"
#include <cstring>

namespace Urho3D
{

// SPIR-V Constants
static const uint32_t SPIRV_MAGIC = 0x07230203;
static const uint32_t SPIRV_VERSION = 0x00010000;

// SPIR-V Opcodes
enum SPIRVOp
{
    OpNop = 0,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
};

// SPIR-V Decorations
enum SPIRVDecoration
{
    DecorationDescriptorSet = 34,
    DecorationBinding = 33,
};

bool VulkanSPIRVReflect::ReflectShaderResources(
    const Vector<uint32_t>& spirvBytecode,
    VkShaderStageFlagBits shaderStage,
    Vector<SPIRVResource>& resources)
{
    resources.Clear();

    if (spirvBytecode.Empty())
    {
        URHO3D_LOGERROR("Empty SPIR-V bytecode");
        return false;
    }

    // Validate SPIR-V header
    if (!ValidateSPIRVHeader(spirvBytecode))
    {
        URHO3D_LOGERROR("Invalid SPIR-V header");
        return false;
    }

    // Basic reflection: Find OpVariable instructions for resources
    // This is a simplified reflection that identifies uniform buffers
    size_t idx = 5;  // Skip header (5 words)

    while (idx < spirvBytecode.Size())
    {
        uint32_t word = spirvBytecode[idx];
        uint16_t wordCount = word >> 16;
        uint16_t opcode = word & 0xFFFF;

        if (wordCount == 0)
        {
            URHO3D_LOGWARNING("Invalid SPIR-V instruction word count");
            break;
        }

        // Handle Decorate instructions to extract binding info
        if (opcode == OpDecorate && idx + 3 < spirvBytecode.Size())
        {
            uint32_t targetId = spirvBytecode[idx + 1];
            uint32_t decoration = spirvBytecode[idx + 2];

            // We parse decorations but simplified version doesn't fully decode them
            // A complete implementation would track all decorations per variable
        }

        // Handle Variable instructions (resources)
        if (opcode == OpVariable && idx + 3 < spirvBytecode.Size())
        {
            // Variable: result_type, result_id, storage_class, [initializer]
            // For now, we create default resources as a framework
            // Full reflection would require glslang or spirv-reflect library
        }

        idx += wordCount;
    }

    // Framework: Add default uniform buffer binding for now
    // Real implementation would parse the SPIR-V instructions above
    SPIRVResource uniformBuffer;
    uniformBuffer.set = 0;
    uniformBuffer.binding = 0;
    uniformBuffer.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformBuffer.descriptorCount = 1;
    uniformBuffer.stageFlags = shaderStage;

    resources.Push(uniformBuffer);

    URHO3D_LOGDEBUG("Reflected " + String(resources.Size()) + " resources from SPIR-V");
    return true;
}

bool VulkanSPIRVReflect::ValidateSPIRVHeader(const Vector<uint32_t>& spirv)
{
    if (spirv.Size() < 5)
    {
        URHO3D_LOGERROR("SPIR-V bytecode too short for header");
        return false;
    }

    // Check magic number
    if (spirv[0] != SPIRV_MAGIC)
    {
        URHO3D_LOGERROR("Invalid SPIR-V magic number");
        return false;
    }

    // Check version (we support v1.0+)
    uint32_t version = spirv[1];
    if (version < SPIRV_VERSION)
    {
        URHO3D_LOGWARNING("SPIR-V version " + String(version >> 16) + "." + String((version >> 8) & 0xFF) + " may not be fully supported");
    }

    // Header format:
    // [0] Magic number
    // [1] Version
    // [2] Generator's magic number
    // [3] Bound (all IDs < bound)
    // [4] Schema (instruction schema version)

    return true;
}

void VulkanSPIRVReflect::ExtractResourceMetadata(
    const Vector<uint32_t>& spirv,
    Vector<SPIRVResource>& resources)
{
    // This is a framework for more sophisticated reflection
    // A full implementation would:
    // 1. Parse OpVariable instructions
    // 2. Match decorations with variable IDs
    // 3. Determine descriptor types from OpTypePointer
    // 4. Build complete descriptor set layouts

    // For now, return empty - descriptor creation uses defaults
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
