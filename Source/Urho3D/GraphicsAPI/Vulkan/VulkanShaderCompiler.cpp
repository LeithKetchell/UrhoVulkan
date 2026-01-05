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
#include <fstream>

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

    fprintf(stderr, "PREPROCESS: Original length=%d, Preprocessed length=%d\n",
            (int)source.Length(), (int)preprocessed.Length());
    fprintf(stderr, "PREPROCESS: First 400 chars of preprocessed: %.400s\n", preprocessed.CString());
    fprintf(stderr, "PREPROCESS: Hex dump of first 100 bytes:\n");
    const char* prep = preprocessed.CString();
    for (int i = 0; i < 100 && i < (int)preprocessed.Length(); ++i) {
        fprintf(stderr, "%02x ", (unsigned char)prep[i]);
        if ((i+1) % 20 == 0) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

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

/// \brief Add explicit layout(location=N) qualifiers to shader interface variables
/// AND layout(binding=N, set=0) to uniform samplers/buffers for Vulkan
/// \param source Shader source code
/// \returns Modified source with layout qualifiers
///
/// \details Adds layout(location=N) to all 'in' and 'out' variable declarations
/// to fix glslang auto-location mapping failures that generate invalid location 1073741823.
/// Also adds layout(binding=N, set=0) to uniform samplers to fix descriptor binding collisions.
/// Skips built-in variables (gl_Position, gl_FragColor, etc.) and already-qualified variables.
static String AddExplicitLayoutQualifiers(const String& source)
{
    String result = source;
    int inputLocation = 0;
    int outputLocation = 0;
    int uniformBlockBinding = 0;  // Uniform blocks start at binding 0
    int samplerBinding = 100;     // Samplers start at binding 100 (well separated from uniform blocks)

    // Process line by line
    Vector<String> lines = result.Split('\n');
    fprintf(stderr, "LAYOUT_DEBUG: Processing %u lines\n", lines.Size());

    for (unsigned i = 0; i < lines.Size(); ++i)
    {
        String& line = lines[i];
        String trimmed = line.Trimmed();

        // Debug: show lines that contain "attribute" or "varying"
        if (trimmed.Contains("attribute") || trimmed.Contains("varying") || trimmed.Contains("fragData"))
        {
            fprintf(stderr, "LAYOUT_DEBUG[%u]: Found candidate line: %s\n", i, trimmed.CString());
        }

        // Skip comments, empty lines, and lines with existing layout qualifiers
        if (trimmed.Empty() || trimmed.StartsWith("//") || trimmed.StartsWith("/*") ||
            trimmed.Contains("layout(location"))
        {
            if (trimmed.Contains("attribute") || trimmed.Contains("varying") || trimmed.Contains("fragData"))
                fprintf(stderr, "LAYOUT_DEBUG[%u]: SKIPPED (empty/comment/has layout)\n", i);
            continue;
        }

        // Check for 'in' or 'out' variable declarations (not in function parameters)
        // Also handle 'attribute' (input) and 'varying' (input/output depending on shader stage)
        bool isInput = (trimmed.StartsWith("in ") || trimmed.Contains(" in ") || trimmed.StartsWith("attribute "));
        bool isOutput = (trimmed.StartsWith("out ") || trimmed.Contains(" out ") || trimmed.StartsWith("varying "));

        if (trimmed.Contains("attribute") || trimmed.Contains("varying") || trimmed.Contains("fragData"))
        {
            fprintf(stderr, "LAYOUT_DEBUG[%u]: isInput=%d, isOutput=%d, hasSemicolon=%d\n",
                    i, isInput, isOutput, trimmed.Contains(";"));
        }

        if ((isInput || isOutput) && trimmed.Contains(";"))
        {
            // Skip built-in variables and uniforms (but NOT user-defined attributes/varyings)
            if (trimmed.Contains("gl_") || trimmed.Contains("uniform"))
            {
                if (trimmed.Contains("attribute") || trimmed.Contains("varying") || trimmed.Contains("fragData"))
                    fprintf(stderr, "LAYOUT_DEBUG[%u]: SKIPPED (gl_/uniform)\n", i);
                continue;
            }

            // Add layout qualifier
            if (isInput && !isOutput)
            {
                line = "layout(location = " + String(inputLocation++) + ") " + trimmed;
                fprintf(stderr, "LAYOUT: Added input location to: %s\n", line.CString());
            }
            else if (isOutput && !isInput)
            {
                line = "layout(location = " + String(outputLocation++) + ") " + trimmed;
                fprintf(stderr, "LAYOUT: Added output location to: %s\n", line.CString());
            }
        }

        // NEW: Handle uniform sampler declarations (add binding qualifiers)
        // Match: "uniform sampler2D name;" or "uniform samplerCube name;" etc.
        if (trimmed.StartsWith("uniform sampler") && trimmed.Contains(";") &&
            !trimmed.Contains("layout(binding"))
        {
            // This is a sampler without a binding qualifier
            // Add layout(binding=N, set=0) before the uniform keyword
            line = "layout(binding = " + String(samplerBinding++) + ", set = 0) " + trimmed;
            fprintf(stderr, "LAYOUT: Added sampler binding to: %s\n", line.CString());
        }

        // NEW: Handle uniform block declarations (add unique binding)
        // Match: "uniform BlockName" (opening of uniform block, not a sampler)
        // Each uniform block gets a unique binding number
        if (trimmed.StartsWith("uniform ") && !trimmed.Contains("sampler") &&
            !trimmed.Contains(";") && !trimmed.Contains("layout(binding"))
        {
            // This is a uniform block opening - assign unique binding
            line = "layout(binding = " + String(uniformBlockBinding++) + ", set = 0) " + trimmed;
            fprintf(stderr, "LAYOUT: Added uniform block binding to: %s\n", line.CString());
        }
    }

    // Reconstruct source
    result.Clear();
    for (unsigned i = 0; i < lines.Size(); ++i)
    {
        result += lines[i];
        if (i < lines.Size() - 1)
            result += "\n";
    }

    return result;
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
    fprintf(stderr, "PREPROCESS: Input source length=%d\n", (int)source.Length());
    fprintf(stderr, "PREPROCESS: First 200 chars: %.200s\n", source.CString());

    // Start with base preprocessing
    preprocessed = source;

    // Add default version if not present OR replace existing version with Vulkan-compatible one
    // Use GLSL 4.50 (Vulkan 1.0) - required for proper location auto-mapping in glslang
    size_t versionPos = preprocessed.Find("#version");
    if (versionPos != String::NPOS)
    {
        // Replace existing #version with #version 450
        size_t lineEnd = preprocessed.Find("\n", versionPos);
        if (lineEnd == String::NPOS)
            lineEnd = preprocessed.Length();
        preprocessed = "#version 450\n" + preprocessed.Substring(lineEnd + 1);
    }
    else
    {
        // No version directive, add #version 450
        preprocessed = "#version 450\n" + preprocessed;
    }

    // Process #include directives by expanding them inline
    // This allows shaders to be split across multiple files
    // NOTE: Must do this BEFORE adding layout qualifiers so we catch all in/out declarations
    String result;
    size_t pos = 0;
    size_t includePos = preprocessed.Find("#include");
    fprintf(stderr, "PREPROCESS: Looking for includes, first found at pos=%d\n", (int)includePos);

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
            URHO3D_LOGDEBUG("Processing #include directive: " + includeFilename);

            // Load include file from bin/CoreData/Shaders/GLSL/ directory
            String includePath = "bin/CoreData/Shaders/GLSL/" + includeFilename;
            fprintf(stderr, "INCLUDE: Trying to load: %s\n", includePath.CString());
            std::ifstream includeFile(includePath.CString());
            if (includeFile.is_open())
            {
                std::string includeContent((std::istreambuf_iterator<char>(includeFile)),
                                          std::istreambuf_iterator<char>());
                result += String(includeContent.c_str());
                fprintf(stderr, "INCLUDE: Loaded successfully, size=%d bytes\n", (int)includeContent.size());
                includeFile.close();
            }
            else
            {
                // Fallback: preserve #include if file not found
                fprintf(stderr, "INCLUDE: File not found, preserving directive\n");
                result += includeLine;
            }
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

    fprintf(stderr, "PREPROCESS: About to add layout qualifiers, source length=%d\n", (int)preprocessed.Length());

    // Add explicit layout qualifiers for Vulkan compatibility AFTER includes are expanded
    // This fixes the "Location 1073741823" issue where auto-mapping fails
    preprocessed = AddExplicitLayoutQualifiers(preprocessed);

    fprintf(stderr, "PREPROCESS: Layout qualifiers added, new length=%d\n", (int)preprocessed.Length());

    // DEBUG: Dump preprocessed shader with layout qualifiers to file for inspection
    static int dumpCounter = 0;
    String dumpPath = "/tmp/shader_preprocessed_" + String(dumpCounter++) + ".glsl";
    FILE* dumpFile = fopen(dumpPath.CString(), "w");
    if (dumpFile) {
        fwrite(preprocessed.CString(), 1, preprocessed.Length(), dumpFile);
        fclose(dumpFile);
        fprintf(stderr, "PREPROCESS: Dumped preprocessed shader to %s\n", dumpPath.CString());
    }

    // DEBUG: Dump preprocessed shader with layout qualifiers for inspection
    static int shaderDumpCount = 0;
    if (shaderDumpCount++ < 2) {
        char filename[256];
        snprintf(filename, sizeof(filename), "/tmp/shader_with_layout_%d.glsl", shaderDumpCount);
        std::ofstream shaderDump(filename);
        shaderDump << preprocessed.CString();
        shaderDump.close();
        fprintf(stderr, "PREPROCESS: Dumped shader to %s (%d bytes)\n", filename, (int)preprocessed.Length());
    }

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
    // DEBUG: Check if source has layout qualifiers
    fprintf(stderr, "GLSLANG_INPUT: Source length=%d, has layout qualifiers: %s\n",
            (int)source.Length(), source.Contains("layout(location") ? "YES" : "NO");
    if (source.Contains("layout(location")) {
        fprintf(stderr, "GLSLANG_INPUT: First layout qualifier found at position %d\n",
                (int)source.Find("layout(location"));
    }

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

        // Set environment FIRST (before setting strings)
        // Use Vulkan mode for proper SPIR-V generation
        // GLSL version 450 matches our #version 330 with GL3 defines (GLSL 330 -> GLSL 450 for Vulkan)
        shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

        // DISABLE auto-location mapping since we now add explicit layout(location=N) qualifiers
        shader.setAutoMapLocations(false);

        // Auto-binding control:
        // - true: Compiler assigns bindings automatically (may or may not be unique)
        // - false: Use our explicit layout(binding=N) qualifiers (but pipeline layout must match!)
        shader.setAutoMapBindings(true);  // TEST: Check what auto-binding actually produces
        // shader.setAutoMapBindings(false);  // BLOCKED: Pipeline layout doesn't match our custom bindings

        // Prepend defines directly to source instead of using setPreamble
        String sourceWithDefines = source;

        // Build defines block
        String definesBlock;

        // Add GL3 define for GLSL 330+ (enables modern GLSL features)
        definesBlock += "#define GL3\n";

        // Add USE_CBUFFERS to force uniform blocks (required for Vulkan SPIR-V)
        definesBlock += "#define USE_CBUFFERS\n";

        // Add shader type define (COMPILEVS or COMPILEPS)
        if (type == VS)
            definesBlock += "#define COMPILEVS\n";
        else if (type == PS)
            definesBlock += "#define COMPILEPS\n";

        // Add user-provided defines
        if (!defines.Empty())
        {
            Vector<String> defineList = defines.Split(' ');
            for (const String& define : defineList)
            {
                if (!define.Empty())
                {
                    definesBlock += "#define " + define + "\n";
                }
            }
        }

        // Insert defines right after #version 330 line (which PreprocessShader adds at the start)
        // The source from PreprocessShader will be: "#version 330\n<original source>"
        // We want: "#version 330\n<defines>\n<original source>"
        if (!definesBlock.Empty())
        {
            size_t versionEnd = sourceWithDefines.Find("#version");
            if (versionEnd != String::NPOS)
            {
                size_t lineEnd = sourceWithDefines.Find("\n", versionEnd);
                if (lineEnd != String::NPOS)
                {
                    sourceWithDefines = sourceWithDefines.Substring(0, lineEnd + 1) + definesBlock + sourceWithDefines.Substring(lineEnd + 1);
                }
            }
            else
            {
                // No #version found, prepend defines at the start
                sourceWithDefines = definesBlock + sourceWithDefines;
            }
            fprintf(stderr, "DEFINES: Inserted at position after #version, defines were: COMPILEVS/PS + %s\n", defines.CString());
        }

        // Set shader source strings
        const char* shaderCString = sourceWithDefines.CString();
        fprintf(stderr, "GLSLANG: Source length=%d, stage=%d\n", (int)sourceWithDefines.Length(), (int)stage);

        // DEBUG: Write shader source to file for inspection
        static int shaderCount = 0;
        String debugPath = "/tmp/shader_with_defines_" + String(shaderCount++) + ".glsl";
        std::ofstream debugFile(debugPath.CString());
        if (debugFile.is_open()) {
            debugFile << shaderCString;
            debugFile.close();
            fprintf(stderr, "GLSLANG: Wrote shader source to %s\n", debugPath.CString());
        }

        shader.setStrings(&shaderCString, 1);

        // Compile shader with relaxed rules for OpenGL-style shaders
        // EShMsgRelaxedErrors allows auto-assignment of input/output locations
        // Note: Removed EShMsgVulkanRules to allow OpenGL-style standalone uniforms (not in blocks)
        EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgSuppressWarnings | EShMsgRelaxedErrors);
        if (!shader.parse(GetDefaultResources(), 330, false, messages))
        {
            compilerOutput = String(shader.getInfoLog()) + "\n" + String(shader.getInfoDebugLog());
            URHO3D_LOGERROR("glslang shader parsing failed: " + compilerOutput);
            return false;
        }

        // Link into program
        glslang::TProgram program;
        program.addShader(&shader);

        if (!program.link(messages))
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

        // DEBUG: Dump SPIR-V for inspection (always dump first 2 shaders per run)
        static int spirvDumpCount = 0;
        if (spirvDumpCount < 2) {
            char filename[256];
            snprintf(filename, sizeof(filename), "/tmp/shader_%s_%d.spv", type == VS ? "vertex" : "fragment", spirvDumpCount);
            std::ofstream spirvDump(filename, std::ios::binary);
            spirvDump.write((const char*)spirvTemp.data(), spirvTemp.size() * 4);
            spirvDump.close();
            fprintf(stderr, "GLSLANG: Dumped SPIR-V to %s (%zu bytes)\n", filename, spirvTemp.size() * 4);
            spirvDumpCount++;
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
