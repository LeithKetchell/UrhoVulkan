//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan GLSL to SPIR-V shader compiler (Phase 6)

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "VulkanShaderCompiler.h"
#include "../../IO/Log.h"
#include <sstream>
#include <algorithm>

#include "../../DebugNew.h"

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

/// \brief Convert Urho3D shader type to Vulkan shader stage
/// \param type Urho3D ShaderType enum (VS, PS, etc.)
/// \returns Corresponding VkShaderStageFlagBits for use in VkPipelineShaderStageCreateInfo
///
/// \details Maps Urho3D's shader type naming (VS/PS) to Vulkan's stage naming.
/// Currently supports vertex (VS) and pixel/fragment (PS) shaders.
/// Geometry, tessellation, and compute shaders return vertex stage as placeholder.
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

/// \brief Main entry point: compile GLSL shader source to SPIR-V bytecode
/// \param source GLSL shader source code as UTF-8 string
/// \param defines Preprocessor defines string (format: "DEFINE1 DEFINE2=value")
/// \param type Shader type (VS, PS, etc.)
/// \param spirvBytecode Output: compiled SPIR-V bytecode as uint32_t vector
/// \param compilerOutput Output: error/warning messages from compiler
/// \returns true if compilation successful, false on any error
///
/// \details Implements two-tier compilation strategy:
///   1. Validate shader source (reject if empty)
///   2. Preprocess shader (handle #version, #include directives)
///   3. Try shaderc compiler (preferred, faster, more modern)
///   4. Fall back to glslang if shaderc fails or unavailable
///   5. Return error if both compilers fail or neither available
///
/// **Preprocessing:**
/// - Adds #version 450 if not present (GLSL 4.50, compatible with Vulkan)
/// - Processes #include directives (extracts filenames for later expansion)
/// - Handles both #include "file" and #include <file> syntax
///
/// **Compiler Selection:**
/// - Preferred: shaderc (Google's compiler, optimized, faster)
/// - Fallback: glslang (Khronos reference compiler, always available if enabled)
/// - If both fail, returns detailed error message
///
/// **Output Handling:**
/// - spirvBytecode: Contains SPIR-V bytecode ready for vkCreateShaderModule()
/// - compilerOutput: Contains human-readable diagnostics (errors and warnings)
///
/// **Typical Error Cases:**
/// - Empty source -> "Error: Empty shader source"
/// - Preprocessing failure -> "Shader preprocessing failed: <details>"
/// - Compilation failure -> Compiler-specific error message
/// - No compiler available -> "Error: No shader compiler available..."
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

/// \brief Preprocess GLSL shader source before compilation
/// \param source Original GLSL shader source code
/// \param preprocessed Output: preprocessed source after expansion
/// \param compilerOutput Output: preprocessing diagnostics (usually empty on success)
/// \returns true if preprocessing successful, false on error (currently always true)
///
/// \details Handles GLSL preprocessing tasks:
///   1. Adds #version 450 directive if not present
///      - Vulkan requires explicit GLSL version >= 4.50
///      - Allows older code without version directive to work
///   2. Processes #include directives
///      - Extracts filenames from #include "file" and #include <file> syntax
///      - Preserves #include statements for compiler handling
///      - Logs include directives for debugging
///   3. Preserves shader layout and comments
///
/// **#version Directive:**
/// Vulkan shaders MUST have #version >= 450 for SPIR-V compilation.
/// If source lacks version directive, "#version 450" is prepended automatically.
/// This allows legacy GLSL code to work without modification.
///
/// **#include Handling:**
/// Currently preserves #include directives as-is for compiler handling.
/// Future enhancement: Load included files via ResourceCache for inline expansion.
/// Handles both quoted and angled bracket syntax:
///   - #include "common.glsl"  -> extracted as "common.glsl"
///   - #include <math>         -> extracted as "math"
///
/// **Current Status:**
/// Always returns true (no fatal preprocessing errors detected).
/// compilerOutput only populated if errors occur (none currently defined).
bool VulkanShaderCompiler::PreprocessShader(
    const String& source,
    String& preprocessed,
    String& compilerOutput)
{
    // Start with base preprocessing
    preprocessed = source;

    // Add default version if not present
    if (preprocessed.Find("#version") == String::NPOS)
    {
        preprocessed = "#version 450\n" + preprocessed;
    }

    // Process #include directives by expanding them inline
    // This allows shaders to be split across multiple files
    String result;
    size_t pos = 0;
    size_t includePos = preprocessed.Find("#include");

    while (includePos != String::NPOS)
    {
        // Add everything before the #include
        result += preprocessed.Substring(pos, includePos - pos);

        // Find the end of the #include line
        size_t lineEnd = preprocessed.Find('\n', includePos);
        if (lineEnd == String::NPOS)
            lineEnd = preprocessed.Length();

        // Extract the #include statement
        String includeLine = preprocessed.Substring(includePos, lineEnd - includePos);

        // Parse the filename from #include "filename" or #include <filename>
        size_t quoteStart = includeLine.Find('"');
        size_t quoteEnd = (quoteStart != String::NPOS) ? includeLine.Find('"', quoteStart + 1) : String::NPOS;

        if (quoteStart == String::NPOS)
        {
            quoteStart = includeLine.Find('<');
            quoteEnd = (quoteStart != String::NPOS) ? includeLine.Find('>', quoteStart + 1) : String::NPOS;
        }

        if (quoteStart != String::NPOS && quoteEnd != String::NPOS)
        {
            String includeFilename = includeLine.Substring(quoteStart + 1, quoteEnd - quoteStart - 1);

            // Log that we're processing an include (actual file loading would require ResourceCache)
            URHO3D_LOGDEBUG("Processing #include directive: " + includeFilename);

            // Note: Actual include file loading would require ResourceCache access:
            // result += resourceCache->GetResource<ShaderInclude>(includeFilename)->GetContent();
            // For now, we preserve the #include for compiler handling or user-provided includes
            result += includeLine;
        }
        else
        {
            // Malformed #include, keep as-is
            result += includeLine;
        }

        // Move to next include
        pos = lineEnd;
        includePos = preprocessed.Find("#include", pos);
    }

    // Add remaining source after last include
    result += preprocessed.Substring(pos);
    preprocessed = result;

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

bool VulkanShaderCompiler::CheckCompilerAvailability()
{
    String available = GetAvailableCompilers();

    if (available.Empty())
    {
        URHO3D_LOGERROR("========================================");
        URHO3D_LOGERROR("CRITICAL: No shader compiler available!");
        URHO3D_LOGERROR("========================================");
        URHO3D_LOGERROR("Shader compilation will fail. Install one of:");
        URHO3D_LOGERROR("  - shaderc (Google's GLSL compiler)");
        URHO3D_LOGERROR("  - glslang (Khronos reference compiler)");
        URHO3D_LOGERROR("");
        URHO3D_LOGERROR("Ubuntu/Debian: sudo apt-get install glslang-tools");
        URHO3D_LOGERROR("Or build from source: https://github.com/KhronosGroup/glslang");
        URHO3D_LOGERROR("========================================");
        return false;
    }

    URHO3D_LOGINFO("Shader compiler available: " + available);
    return true;
}

String VulkanShaderCompiler::GetAvailableCompilers()
{
    String result;
    bool hasCompiler = false;

#ifdef URHO3D_SHADERC
    if (!result.Empty()) result += " + ";
    result += "shaderc (primary)";
    hasCompiler = true;
#endif

#ifdef URHO3D_GLSLANG
    if (!result.Empty()) result += " + ";
    result += "glslang (fallback)";
    hasCompiler = true;
#endif

    if (!hasCompiler)
    {
        return "";  // No compilers available
    }

    return result;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
