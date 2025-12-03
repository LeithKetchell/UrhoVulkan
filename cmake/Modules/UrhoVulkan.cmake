# Copyright (c) 2008-2024 the Urho3D project
# License: MIT
#
# Vulkan SDK detection and configuration for Urho3D

# Find Vulkan SDK
find_package(Vulkan REQUIRED)

if(NOT Vulkan_FOUND)
    message(FATAL_ERROR "Vulkan SDK not found. Please install Vulkan SDK or set VULKAN_SDK environment variable.")
endif()

message(STATUS "Vulkan found: ${Vulkan_LIBRARIES}")
message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")

# Validate Vulkan SDK version (require 1.0+)
if(Vulkan_VERSION)
    message(STATUS "Vulkan SDK version: ${Vulkan_VERSION}")
    if(Vulkan_VERSION VERSION_LESS "1.0.0")
        message(FATAL_ERROR "Vulkan SDK version ${Vulkan_VERSION} is too old. Require 1.0.0 or later.")
    endif()
else()
    message(WARNING "Could not determine Vulkan SDK version. Proceeding anyway.")
endif()

# Add Vulkan include directories
include_directories(${Vulkan_INCLUDE_DIRS})

# Find and configure VMA (Vulkan Memory Allocator)
set(VMA_PATH "${CMAKE_SOURCE_DIR}/Source/ThirdParty/VMA")
if(EXISTS "${VMA_PATH}/include/vk_mem_alloc.h")
    message(STATUS "Found VMA (Vulkan Memory Allocator) at: ${VMA_PATH}")
    include_directories(${VMA_PATH}/include)
    set(VMA_FOUND_MSG "FOUND")
    set(VMA_FOUND TRUE)
    # VMA should be built as part of the main Urho3D library CMakeLists.txt
else()
    message(WARNING "VMA (Vulkan Memory Allocator) not found at ${VMA_PATH}")
    set(VMA_FOUND_MSG "NOT FOUND")
    set(VMA_FOUND FALSE)
endif()

# Link Vulkan library
link_libraries(${Vulkan_LIBRARIES})

# Find shader compilation tools (shaderc or glslang)
# These are optional but strongly recommended for runtime shader compilation
find_package(ShaderC QUIET)

if(ShaderC_FOUND)
    message(STATUS "Found shaderc: ${ShaderC_LIBRARY}")
    set(URHO3D_SHADERC TRUE)
    set(SHADER_COMPILER_FOUND TRUE)
    add_definitions(-DURHO3D_SHADERC)
else()
    # Try to find glslang as fallback
    find_package(glslang QUIET)
    if(glslang_FOUND)
        message(STATUS "Found glslang for shader compilation (fallback)")
        set(URHO3D_GLSLANG TRUE)
        set(SHADER_COMPILER_FOUND TRUE)
        add_definitions(-DURHO3D_GLSLANG)
    else()
        message(WARNING "Neither shaderc nor glslang found. Runtime shader compilation will be disabled.")
        message(WARNING "Install Vulkan SDK (includes shaderc) or glslang library for shader compilation support.")
        set(SHADER_COMPILER_FOUND FALSE)
    endif()
endif()

# Link shader compiler libraries if found
if(ShaderC_FOUND)
    list(APPEND LIBS ${ShaderC_LIBRARY})
    if(ShaderC_INCLUDE_DIR)
        include_directories(${ShaderC_INCLUDE_DIR})
    endif()
endif()

if(glslang_FOUND)
    list(APPEND LIBS glslang SPIRV)
    if(glslang_INCLUDE_DIR)
        include_directories(${glslang_INCLUDE_DIR})
    endif()
endif()

# Define Vulkan-specific compiler flags based on platform
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Unix")
    set(VULKAN_PLATFORM_DEFINES
        VK_USE_PLATFORM_XCLIB_KHR
        VK_USE_PLATFORM_WAYLAND_KHR
    )
elseif(WIN32)
    set(VULKAN_PLATFORM_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
    set(VULKAN_PLATFORM_DEFINES VK_USE_PLATFORM_METAL_EXT)
endif()

# Add Vulkan compiler definitions with proper -D prefix
foreach(_define ${VULKAN_PLATFORM_DEFINES})
    add_definitions(-D${_define})
endforeach()

# Function to compile GLSL shaders to SPIR-V
function(compile_glsl_to_spirv GLSL_SOURCE SPIRV_OUTPUT)
    if(ShaderC_FOUND)
        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${ShaderC_GLSLC} ${GLSL_SOURCE} -o ${SPIRV_OUTPUT}
            DEPENDS ${GLSL_SOURCE}
            COMMENT "Compiling ${GLSL_SOURCE} to SPIR-V"
        )
    elseif(glslang_FOUND)
        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${glslang_VALIDATOR} -V ${GLSL_SOURCE} -o ${SPIRV_OUTPUT}
            DEPENDS ${GLSL_SOURCE}
            COMMENT "Compiling ${GLSL_SOURCE} to SPIR-V (glslang)"
        )
    else()
        message(WARNING "Cannot compile shaders: shader compiler not found")
    endif()
endfunction()

# ============================================================================
# Vulkan Configuration Validation Summary
# ============================================================================
message(STATUS "")
message(STATUS "========== Vulkan Configuration Summary ==========")
message(STATUS "Vulkan SDK:              FOUND (${Vulkan_LIBRARIES})")
message(STATUS "VMA (Memory Allocator):  ${VMA_FOUND_MSG}")
message(STATUS "Shader Compiler:         ${SHADER_COMPILER_FOUND}")
if(ShaderC_FOUND)
    message(STATUS "  - Primary: shaderc (Google's GLSL compiler)")
elseif(glslang_FOUND)
    message(STATUS "  - Primary: glslang (Khronos reference compiler)")
endif()
message(STATUS "Platform Definitions:    ${VULKAN_PLATFORM_DEFINES}")
message(STATUS "==================================================")
