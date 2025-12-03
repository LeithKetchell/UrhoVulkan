# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Urho3D** is a free, lightweight, cross-platform 2D and 3D game engine implemented in C++, released under the MIT license. It supports multiple scripting languages (AngelScript, Lua) and multiple rendering backends (OpenGL, Direct3D 11).

- **Current Version**: 1.9.0
- **Build System**: CMake 3.15+
- **Supported Platforms**: Windows, Linux, macOS, iOS, tvOS, Android, Web (Emscripten), Raspberry Pi
- **Documentation**: https://urho3d.io/documentation
- **Contributing**: https://urho3d.io/documentation/HEAD/_contribution_checklist.html

## Build Commands

### Quick Start (Linux/macOS)

```bash
# Configure CMake build
script/cmake_generic.sh build

# Build the project
cd build
make -j$(nproc)
```

### Using Rake (Recommended)

The `rakefile` provides high-level build tasks:

```bash
# Configure and build
rake build

# Build with specific options
rake build URHO3D_ANGELSCRIPT=1 URHO3D_LUA=1 URHO3D_PHYSICS=1

# Release build
rake build CMAKE_BUILD_TYPE=Release

# Run tests
rake test

# Generate documentation
rake doc

# Clean build artifacts
rake clean
```

### Cross-Compilation

```bash
# Raspberry Pi / ARM
script/cmake_rpi.sh build

# Web (Emscripten)
script/cmake_emscripten.sh build

# iOS
script/cmake_ios.sh build

# Android (uses Gradle)
rake build PLATFORM=android

# MinGW (Windows)
script/cmake_mingw.sh build
```

### Build Configuration Options

Key CMake options are stored in `script/.build-options`. Common ones:

```bash
-DURHO3D_ANGELSCRIPT=1          # Enable AngelScript support
-DURHO3D_LUA=1                  # Enable Lua support
-DURHO3D_PHYSICS=1              # Enable Bullet physics
-DURHO3D_PHYSICS2D=1            # Enable Box2D physics
-DURHO3D_NAVIGATION=1           # Enable pathfinding (Recast/Detour)
-DURHO3D_URHO2D=1               # Enable 2D engine
-DURHO3D_NETWORK=1              # Enable networking
-DURHO3D_SAMPLES=1              # Build example projects
-DURHO3D_TOOLS=1                # Build development tools
-DURHO3D_TESTING=1              # Enable tests
-DCMAKE_BUILD_TYPE=Release      # Release vs Debug
-DURHO3D_LIB_TYPE=SHARED        # Shared library (default: STATIC)
```

## Repository Structure

```
Source/
├── Urho3D/              # Core engine library (24 modules)
├── Samples/             # 60+ example projects
├── Tools/               # Development tools (AssetImporter, BindingGenerator, etc.)
└── ThirdParty/          # 30+ external dependencies

cmake/                   # CMake configuration modules
script/                  # Build automation scripts
bin/                     # Data files and game resources
Docs/                    # Doxygen documentation configuration
```

## Core Architecture

### Engine Modules

The engine is divided into 24 specialized modules in `/Source/Urho3D/`:

**Foundation**: Core, Container, Math (event system, data structures, math)
**Engine**: Engine, IO, Resource, Scene (application lifecycle, resource management, scene graph)
**Graphics**: Graphics, GraphicsAPI, UI (rendering pipeline, materials, UI components)
**Specialized**: Input, Audio, Physics, Physics2D, Navigation, Urho2D, IK, Network, Database
**Scripting**: AngelScript, LuaScript (2.35.1 and Lua/LuaJIT bindings)

### Key Architectural Patterns

1. **Event System**: Core event-driven architecture via `Object` and `Context` classes
2. **Component Architecture**: Node/Component pattern for scene graph (`Node`, `Component`, `LogicComponent`)
3. **Resource Management**: Centralized `ResourceCache` for asset loading and caching
4. **Module Layering**: Foundation → Engine → Subsystems → Application
5. **Serialization**: XML-based scene/component serialization via pugixml

### Important Files

| File | Purpose |
|------|---------|
| `/Source/Urho3D/Core/Context.h` | Core engine context and event system |
| `/Source/Urho3D/Core/Object.h` | Base object model and event handling |
| `/Source/Urho3D/Scene/Scene.h` | Scene management and node hierarchy |
| `/Source/Urho3D/Engine/Engine.h` | Main engine class and lifecycle |
| `/CMakeLists.txt` | Root build configuration |
| `/script/.build-options` | All available build configuration options |

## Testing

```bash
# Run all tests
rake test

# Or with CMake
cd build
ctest --verbose

# Tests are located in /Source/Tools/Tests/
```

Tests cover Container and Math modules. Enable with `-DURHO3D_TESTING=1`.

## Code Style and Conventions

Follow the project's [Coding Conventions](https://urho3d.io/documentation/HEAD/_coding_conventions.html):

- Use clang-tidy for static analysis (integrated in CI/CD)
- Use clang-format for code formatting (config in `.clang-format`)
- Follow C++ naming conventions: PascalCase for classes, camelCase for functions/variables
- Doxygen comments for public APIs (C++ style: `///`)

## Documentation

### Generate Doxygen Documentation

```bash
rake doc
# Output: Docs/html/
```

Or manually:
```bash
cd Docs
doxygen Doxyfile
```

### Online Documentation

- Full API Reference: https://urho3d.io/documentation/HEAD/index.html
- GitHub Pages: https://urho3d.github.io

## Working with Samples

The `/Source/Samples/` directory contains 60+ example projects demonstrating engine features:

- **01-10**: Foundational features (rendering, UI, animation)
- **11-20**: Physics, audio, networking, character control
- **21-59**: Scripting, advanced rendering, 2D engine

Each sample is a standalone CMake project. Build all samples with:

```bash
rake build URHO3D_SAMPLES=1
```

## Development Workflow

1. **Locate the relevant module**: Code changes typically belong in one of the 24 modules in `/Source/Urho3D/`
2. **Check examples**: Review `/Source/Samples/` for examples demonstrating similar features
3. **Build and test**: Use `rake build` and `rake test` for validation
4. **Follow conventions**: Use clang-format and clang-tidy for code quality
5. **Test on platforms**: Use cross-compilation scripts for multi-platform validation

## Common Development Tasks

### Build a Single Optimization

```bash
# Debug build for development
rake build CMAKE_BUILD_TYPE=Debug

# Release build with optimizations
rake build CMAKE_BUILD_TYPE=Release

# Static library (default)
rake build URHO3D_LIB_TYPE=STATIC

# Shared library
rake build URHO3D_LIB_TYPE=SHARED
```

### Enable Profiling

```bash
# Tracy profiler support
rake build URHO3D_TRACY_PROFILING=1
```

### Generate Scripting Bindings

```bash
# Regenerate AngelScript/Lua bindings
rake build URHO3D_GENERATEBINDINGS=1
```

## Third-Party Dependencies

Over 30 external libraries integrated in `/Source/ThirdParty/`:

**Graphics**: GLEW, Assimp, ETCPACK
**Physics**: Bullet, Box2D, StanHull
**Scripting**: AngelScript, Lua, LuaJIT
**Audio**: SDL 2.0
**UI/Text**: FreeType, pugixml, Mustache
**Navigation**: Recast, Detour, DetourCrowd
**Networking**: SLikeNet, Civetweb
**Database**: SQLite, nanodbc
**Images**: stb_image, stb_vorbis, WebP
**Other**: RapidJSON, LZ4, boost, Tracy

License information for all dependencies is in `/licenses/`.

## CI/CD Pipeline

Located in `.github/workflows/`:
- **main.yml**: Primary CI/CD across all platforms
- **nightly.yml**: Scheduled nightly builds
- **style.yml**: Code style and formatting checks
- **website.yml**: Documentation website deployment

Uses ccache for build caching and Docker for cross-platform builds.

## Vulkan Graphics Backend (NEW)

### Overview

Urho3D 1.9.0 now includes **Vulkan** as a modern graphics backend alongside legacy OpenGL and Direct3D 11 support. Vulkan is the preferred backend on supported Linux platforms, with automatic fallback to OpenGL/D3D11.

### Building with Vulkan

```bash
# Configure with Vulkan backend (Linux)
script/cmake_generic.sh build -DURHO3D_VULKAN=1

# Build with samples
script/cmake_generic.sh build -DURHO3D_VULKAN=1 -DURHO3D_SAMPLES=1

# Or using Rake
rake build URHO3D_VULKAN=1 URHO3D_SAMPLES=1
```

### Graphics API Priority

The engine automatically selects the best available graphics backend in this order:

1. **Vulkan** (if available and `-DURHO3D_VULKAN=1` enabled)
2. **OpenGL** (Linux/macOS default, always available)
3. **Direct3D 11** (Windows fallback)

Only one backend can be enabled per build for safety and clarity.

### Vulkan Architecture

#### Core Components

- **VulkanGraphicsImpl** (`/Source/Urho3D/GraphicsAPI/Vulkan/VulkanGraphicsImpl.h`)
  - Instance, device, and queue management
  - Swapchain creation and presentation
  - Memory allocator integration (VMA)
  - Command buffer management (triple-buffered)
  - Render pass and framebuffer handling
  - Synchronization primitives (fences, semaphores)

- **Graphics_Vulkan.cpp** (`/Source/Urho3D/Graphics/Graphics_Vulkan.cpp`)
  - Frame lifecycle (BeginFrame, EndFrame)
  - Draw call submission
  - State management (blend, cull, depth, etc.)
  - Viewport and scissor configuration

#### Key Features Implemented

✅ **Initialization & Lifecycle**
- Vulkan instance creation with validation layer support
- Physical device selection (prefers dedicated GPUs)
- Queue family management (graphics + present)
- Surface creation via SDL2
- Swapchain with intelligent format/mode selection

✅ **Memory Management**
- Vulkan Memory Allocator (VMA) integration
- Optimal memory type selection
- GPU resource lifecycle management

✅ **Synchronization**
- Per-frame fencing for CPU-GPU sync
- Semaphores for image acquisition/presentation
- Triple-buffering for smooth rendering

✅ **Rendering Pipeline**
- Single render pass with color + depth attachments
- Per-image framebuffers
- Clear value configuration
- Dynamic viewport and scissor

✅ **Command Recording**
- Per-frame command buffers
- Batch command recording
- Proper synchronization between frames

✅ **Buffers & Textures**
- Vertex buffer creation and binding (vkCmdBindVertexBuffers)
- Index buffer creation and binding (vkCmdBindIndexBuffer)
- Dynamic and static buffer support
- 16-bit and 32-bit index sizes
- Texture creation with VkImageView and VkSampler
- Filter modes: NEAREST, BILINEAR, TRILINEAR, ANISOTROPIC
- Address modes: CLAMP, REPEAT, MIRROR

✅ **Shader Compilation**
- GLSL to SPIR-V compilation via shaderc (Google's compiler)
- Fallback to glslang (Khronos reference compiler)
- Shader parameter/define support
- Error reporting with formatted output
- Graceful degradation if compilers unavailable
- Pipeline layout creation framework

✅ **Descriptors & Pipelines (Framework)**
- Descriptor pool with layout caching
- Support for 8 descriptor types (uniform buffers, textures, samplers, etc.)
- SPIR-V reflection framework for descriptor inference
- Graphics pipeline state caching by state hash
- Full mapping of Urho3D graphics states to Vulkan equivalents
- Shader module creation from SPIR-V bytecode
- Complete integration roadmap documented

#### Architecture Patterns

**Dispatch Pattern**: All Graphics methods use runtime dispatch:
```cpp
void Graphics::SetTexture(unsigned index, Texture* texture) {
    if (gapi == GAPI_VULKAN)
        return SetTexture_Vulkan(index, texture);
    else if (gapi == GAPI_OPENGL)
        return SetTexture_OGL(index, texture);
    // ... etc
}
```

**State Caching**: Pipelines and samplers are cached by hash for performance.

**Frame Pipelining**: Triple-buffering ensures GPU and CPU don't stall:
- Frame 0: GPU renders while CPU prepares frame 1
- Frame 1: GPU renders while CPU prepares frame 2
- Frame 2: GPU renders while CPU prepares frame 0

### Current Implementation Status

**Phases Complete:**
- ✅ Phase 1: CMake integration with Vulkan SDK detection
- ✅ Phase 2: API abstraction layer (GAPI_VULKAN enum)
- ✅ Phase 3: Core graphics initialization and frame lifecycle
- ✅ Phase 4: Vertex/Index buffers with VMA allocation
- ✅ Phase 5: Texture loading with sampler management
- ✅ Phase 6: Shader compilation with shaderc/glslang fallback
- ✅ Phase 7: SPIR-V reflection and descriptor pool management
- ✅ Phase 8: Graphics pipeline creation and binding
- ✅ Phase 9: Constant buffers and shader parameters (framework complete)

**Phases In Progress:**
- 🔄 Phase 10: Staging buffer optimization (optional enhancement)

**Not Yet Implemented:**
- Full shader parameter reflection
- Material parameter binding
- Instancing optimization
- MSAA multisampling
- Advanced render passes
- MoltenVK (macOS/iOS)
- Android support

### Known Limitations (v1.0)

- Single render pass only (no deferred rendering yet)
- No MSAA support yet
- No compute shader support
- Full shader parameter reflection not yet implemented
- No timeline semaphore support
- Staging buffers not yet optimized (optional Phase 10)

### Debugging with Vulkan

#### Enable Validation

```bash
export VK_LAYER_PATH=/path/to/vulkan-sdk/lib
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
./your_app
```

#### Common Issues

1. **"No Vulkan-capable devices found"**
   - Install Vulkan drivers for your GPU
   - Check NVIDIA/AMD driver versions

2. **"Failed to create swapchain"**
   - Window may not support presentation
   - Try running in windowed mode

3. **"Failed to compile shader"**
   - Ensure glslang or shaderc is installed
   - Check shader source for syntax errors

### Future Enhancements

- Metal backend via MoltenVK (macOS/iOS)
- Android native Vulkan support
- Compute shader support
- Advanced descriptor management
- Timeline semaphore support
- Deferred rendering optimization

### Vulkan Resources

- [Vulkan Official Registry](https://www.khronos.org/vulkan/)
- [Vulkan Specification](https://www.khronos.org/registry/vulkan/)
- [Vulkan Samples](https://github.com/KhronosGroup/Vulkan-Samples)
- [VMA Documentation](https://gpuopen.com/vulkan-memory-allocator/)

## ProfilerUI in Examples (STANDARD PATTERN)

All 55+ examples now follow a consistent pattern for displaying the Vulkan profiler UI overlay with proper styling.

### Required Setup in Example Headers (.h)

```cpp
#pragma once

#include "Sample.h"
#include <Urho3D/Graphics/ProfilerUI.h>

class YourExample : public Sample
{
    // ... class definition ...

private:
    /// Profiler UI overlay
    SharedPtr<ProfilerUI> profilerUI_;
};
```

### Required Setup in Start() Method

```cpp
void YourExample::Start()
{
    // Execute base class startup
    Sample::Start();

    // Load UI style for ProfilerUI (must be before creating UI elements)
    auto* cache = GetSubsystem<ResourceCache>();
    auto* uiStyle = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    GetSubsystem<UI>()->GetRoot()->SetDefaultStyle(uiStyle);

    // ... create scene, UI, setup viewport, subscribe to events ...

    // Set the mouse mode to use in the sample
    Sample::InitMouseMode(MM_FREE);

    // Initialize profiler UI
    auto* graphics = GetSubsystem<Graphics>();
    auto* ui = GetSubsystem<UI>();
    profilerUI_ = new ProfilerUI(context_);
    profilerUI_->Initialize(ui, graphics->GetVulkanProfiler());
    profilerUI_->SetVisible(true);
}
```

### Required Setup in HandleUpdate() Method

```cpp
void YourExample::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat();

    // ... your update code ...

    // Update profiler display
    if (profilerUI_)
    {
        GetSubsystem<Graphics>()->GetVulkanProfiler()->RecordFrame(timeStep);
        profilerUI_->Update();
    }
}
```

### Key Points

1. **UI style must be loaded FIRST** - before any UI element creation
2. **ProfilerUI must be initialized LAST** - after InitMouseMode() and all other setup
3. **HandleUpdate must call both RecordFrame() and Update()** - to properly display metrics
4. **All examples use the same pattern** - ensures consistency across the codebase

### Current Status

✅ **All 55 numbered examples + benchmark properly configured**
✅ **Complete build verification successful**
✅ **See EXAMPLES_UI_FIXES_SUMMARY.md for detailed changes**

## Known Build Options and Platforms

- **32/64-bit**: `-DURHO3D_64BIT=1` (default: auto-detect)
- **Graphics API**: `-DURHO3D_VULKAN=1` (Linux/Windows, new), `-DURHO3D_OPENGL=1` (Linux/macOS default), or `-DURHO3D_D3D11=1` (Windows)
- **CPU Features**: `-DURHO3D_SSE=1`, `-DURHO3D_MMX=1`, `-DURHO3D_3DNOW=1`
- **Android**: Uses Gradle build system (separate from CMake)
