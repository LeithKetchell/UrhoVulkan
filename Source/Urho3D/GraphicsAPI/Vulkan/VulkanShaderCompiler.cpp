//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan GLSL to SPIR-V shader compiler (Phase 6)

#include "VulkanShaderCompiler.h"
#include "../../Core/Log.h"
#include <sstream>
#include <algorithm>

#ifdef URHO3D_VULKAN

// Try to include shaderc if available (preferred compiler)
#ifdef URHO3D_SHADERC
    #include <shaderc/shaderc.hpp>
#endif

// Try to include glslang if available (fallback compiler)
#ifdef URHO3D_GLSLANG
    #include <glslang/Public/ShaderLang.h>
    #include <glslang/Public/ResourceLimits.h>
    #include <glslang/SPIRV/GlslangToSpv.h>
#endif

namespace Urho3D
{

VkShaderStageFlagBits VulkanShaderCompiler::GetShaderStage(ShaderType type)
{
    switch (type)
    {
    case VS:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case PS:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    default:
        return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

bool VulkanShaderCompiler::CompileGLSLToSPIRV(
    const String& source,
    const String& defines,
    ShaderType type,
    Vector<uint32_t>& spirvBytecode,
    String& compilerOutput)
{
    if (source.Empty())
    {
        compilerOutput = "Error: Empty shader source";
        return false;
    }

    // Preprocess shader to handle includes
    String preprocessed;
    if (!PreprocessShader(source, preprocessed, compilerOutput))
    {
        compilerOutput = "Shader preprocessing failed: " + compilerOutput;
        return false;
    }

    // Try shaderc first (preferred, faster, more modern)
#ifdef URHO3D_SHADERC
    if (CompileWithShaderc(preprocessed, defines, type, spirvBytecode, compilerOutput))
    {
        return true;
    }
    // If shaderc fails, log and continue to glslang fallback
    URHO3D_LOGDEBUG("shaderc compilation failed, trying glslang fallback");
#endif

    // Fallback to glslang
#ifdef URHO3D_GLSLANG
    if (CompileWithGlslang(preprocessed, defines, type, spirvBytecode, compilerOutput))
    {
        return true;
    }
#endif

    // Both compilers failed or not available
    if (compilerOutput.Empty())
    {
        compilerOutput = "Error: No shader compiler available (neither shaderc nor glslang found)";
    }
    return false;
}

bool VulkanShaderCompiler::PreprocessShader(
    const String& source,
    String& preprocessed,
    String& compilerOutput)
{
    // For now, implement basic preprocessing
    // Advanced preprocessing with #include would require file system access
    // and directory search paths. This is a framework for future enhancement.

    preprocessed = source;

    // Add default precision for fragment shaders
    if (preprocessed.Find("#version") == String::NPOS)
    {
        preprocessed = "#version 450\n" + preprocessed;
    }

    // TODO: Handle #include directives by loading from CoreData/Shaders/GLSL/
    // This requires ResourceCache access, which can be added later

    compilerOutput.Clear();
    return true;
}

#ifdef URHO3D_SHADERC
bool VulkanShaderCompiler::CompileWithShaderc(
    const String& source,
    const String& defines,
    ShaderType type,
    Vector<uint32_t>& spirvBytecode,
    String& compilerOutput)
{
    try
    {
        // Create shader compiler
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        // Set optimization level
        options.SetOptimizationLevel(shaderc_optimization_level_performance);

        // Add shader defines
        if (!defines.Empty())
        {
            // Parse defines string (format: "DEFINE1=value1 DEFINE2=value2")
            Vector<String> defineList = defines.Split(' ');
            for (const String& define : defineList)
            {
                if (!define.Empty())
                {
                    if (define.Contains('='))
                    {
                        Vector<String> parts = define.Split('=');
                        options.AddMacroDefinition(parts[0].CString(), parts[1].CString());
                    }
                    else
                    {
                        options.AddMacroDefinition(define.CString(), "1");
                    }
                }
            }
        }

        // Determine shader kind
        shaderc_shader_kind kind = shaderc_vertex_shader;
        if (type == PS)
            kind = shaderc_fragment_shader;

        // Compile GLSL to SPIR-V
        shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
            source.CString(),
            kind,
            "shader.glsl",
            "main",
            options
        );

        // Check for compilation errors
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            compilerOutput = String(result.GetErrorMessage().c_str());
            URHO3D_LOGERROR("shaderc compilation failed: " + compilerOutput);
            return false;
        }

        // Extract SPIR-V bytecode
        const uint32_t* spirvData = result.cbegin();
        size_t spirvSize = result.cend() - result.cbegin();

        spirvBytecode.Clear();
        spirvBytecode.Reserve(spirvSize);
        for (size_t i = 0; i < spirvSize; ++i)
        {
            spirvBytecode.Push(spirvData[i]);
        }

        if (spirvBytecode.Empty())
        {
            compilerOutput = "Error: SPIR-V compilation produced empty bytecode";
            return false;
        }

        compilerOutput = "Success: Compiled to " + String(spirvBytecode.Size() * 4) + " bytes";
        return true;
    }
    catch (const std::exception& e)
    {
        compilerOutput = "Exception in shaderc: " + String(e.what());
        URHO3D_LOGERROR(compilerOutput);
        return false;
    }
}
#else
bool VulkanShaderCompiler::CompileWithShaderc(
    const String& source,
    const String& defines,
    ShaderType type,
    Vector<uint32_t>& spirvBytecode,
    String& compilerOutput)
{
    compilerOutput = "shaderc not available (not compiled with URHO3D_SHADERC)";
    return false;
}
#endif

#ifdef URHO3D_GLSLANG
bool VulkanShaderCompiler::CompileWithGlslang(
    const String& source,
    const String& defines,
    ShaderType type,
    Vector<uint32_t>& spirvBytecode,
    String& compilerOutput)
{
    try
    {
        // Initialize glslang library
        static bool glslangInitialized = false;
        if (!glslangInitialized)
        {
            glslang::InitializeProcess();
            glslangInitialized = true;
        }

        // Determine shader stage
        EShLanguage stage = EShLangVertex;
        if (type == PS)
            stage = EShLangFragment;

        // Create shader object
        glslang::TShader shader(stage);
        const char* shaderCString = source.CString();
        shader.setStrings(&shaderCString, 1);

        // Apply defines
        if (!defines.Empty())
        {
            // Parse defines and add them to preamble
            String preamble;
            Vector<String> defineList = defines.Split(' ');
            for (const String& define : defineList)
            {
                if (!define.Empty())
                {
                    preamble += "#define " + define + "\n";
                }
            }
            shader.setPreamble(preamble.CString());
        }

        // Set environment
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

        // Compile shader
        const char* messages = "";
        if (!shader.parse(GetDefaultResources(), 100, false, EShMsgSpvRules | EShMsgVulkanRules, messages))
        {
            compilerOutput = String(shader.getInfoLog()) + "\n" + String(shader.getInfoDebugLog());
            URHO3D_LOGERROR("glslang shader parsing failed: " + compilerOutput);
            return false;
        }

        // Link into program
        glslang::TProgram program;
        program.addShader(&shader);

        if (!program.link(EShMsgSpvRules | EShMsgVulkanRules, messages))
        {
            compilerOutput = String(program.getInfoLog()) + "\n" + String(program.getInfoDebugLog());
            URHO3D_LOGERROR("glslang program linking failed: " + compilerOutput);
            return false;
        }

        // Translate to SPIR-V
        std::vector<uint32_t> spirvTemp;
        glslang::GlslangToSpv(*program.getIntermediate(stage), spirvTemp);

        if (spirvTemp.empty())
        {
            compilerOutput = "Error: SPIR-V generation produced empty bytecode";
            return false;
        }

        // Copy to Urho3D Vector
        spirvBytecode.Clear();
        spirvBytecode.Reserve(spirvTemp.size());
        for (uint32_t word : spirvTemp)
        {
            spirvBytecode.Push(word);
        }

        compilerOutput = "Success: Compiled to " + String(spirvBytecode.Size() * 4) + " bytes";
        return true;
    }
    catch (const std::exception& e)
    {
        compilerOutput = "Exception in glslang: " + String(e.what());
        URHO3D_LOGERROR(compilerOutput);
        return false;
    }
}
#else
bool VulkanShaderCompiler::CompileWithGlslang(
    const String& source,
    const String& defines,
    ShaderType type,
    Vector<uint32_t>& spirvBytecode,
    String& compilerOutput)
{
    compilerOutput = "glslang not available (not compiled with URHO3D_GLSLANG)";
    return false;
}
#endif

String VulkanShaderCompiler::FormatCompilerOutput(const String& rawOutput)
{
    if (rawOutput.Empty())
        return "No compiler output";

    // Simple formatting: capitalize first letter if it's a warning or error
    String formatted = rawOutput;
    if (!formatted.Empty() && formatted[0] >= 'a' && formatted[0] <= 'z')
    {
        formatted[0] = formatted[0] - ('a' - 'A');
    }

    return formatted;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
