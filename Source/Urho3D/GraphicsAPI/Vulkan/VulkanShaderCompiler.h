//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan GLSL to SPIR-V shader compiler (Phase 6)

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/Vector.h"
#include "../../Container/String.h"
#include "../GraphicsDefs.h"
#include <vulkan/vulkan.h>

namespace Urho3D
{

class ShaderVariation;

/// GLSL to SPIR-V compiler for Vulkan shaders
class VulkanShaderCompiler
{
public:
    /// Compile GLSL shader source to SPIR-V bytecode
    static bool CompileGLSLToSPIRV(
        const String& source,
        const String& defines,
        ShaderType type,
        Vector<uint32_t>& spirvBytecode,
        String& compilerOutput
    );

    /// Get Vulkan shader stage from ShaderType
    static VkShaderStageFlagBits GetShaderStage(ShaderType type);

    /// Preprocess shader source (handle includes)
    static bool PreprocessShader(
        const String& source,
        String& preprocessed,
        String& compilerOutput
    );

private:
    /// Try to compile using shaderc library (preferred)
    static bool CompileWithShaderc(
        const String& source,
        const String& defines,
        ShaderType type,
        Vector<uint32_t>& spirvBytecode,
        String& compilerOutput
    );

    /// Try to compile using glslang library (fallback)
    static bool CompileWithGlslang(
        const String& source,
        const String& defines,
        ShaderType type,
        Vector<uint32_t>& spirvBytecode,
        String& compilerOutput
    );

    /// Convert compiler output to human-readable format
    static String FormatCompilerOutput(const String& rawOutput);
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
