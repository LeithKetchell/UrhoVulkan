![Urho3D logo](https://raw.githubusercontent.com/urho3d/Urho3D/master/bin/Data/Textures/LogoLarge.png)

# Urho3D

[![CI/CD](https://github.com/urho3d/Urho3D/workflows/CI/CD/badge.svg)](https://github.com/urho3d/Urho3D/actions?query=workflow%3ACI%2FCD)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/4954/badge.svg)](https://scan.coverity.com/projects/urho3d-urho3d)

**Urho3D** is a free lightweight, cross-platform 2D and 3D game engine implemented in C++ and released under the MIT license. Greatly inspired by OGRE and Horde3D.

Main website: [https://urho3d.io/](https://urho3d.io/)

## What's New in 2.0.1 (since 1.9)

### Vulkan Graphics Backend
- Full Vulkan rendering backend alongside legacy OpenGL and Direct3D 11
- All 55+ samples build and run on Vulkan
- GLSL to SPIR-V runtime shader compilation via glslang
- Native VkPipelineCache with on-disk persistence
- Shader module caching and redundant bind elimination
- Hybrid framebuffer support (swapchain color + custom depth RTT)
- Shadow mapping with projection-matrix depth bias
- TextureCube / skybox support
- Anisotropic filtering
- Render-to-texture infrastructure
- Per-frame profiler UI overlay (ProfilerUI) on all samples

### Physics Enhancements
- Collision shapes integrated directly into RigidBody — no separate CollisionShape component required for common shapes (box, sphere, capsule, cylinder, cone, plane, trimesh, convex hull)
- Global primitive shape cache in PhysicsWorld — identical shapes shared across bodies (e.g. 500 boxes = 1 btBoxShape), eliminating redundant memory allocation
- Compound shape API on RigidBody (AddChildShape / ClearChildShapes) — compound children reference cached primitives, so a ragdoll with 10 capsules shares the same underlying btCapsuleShape instances
- Mesh/convex hull shape support (SetTriMeshShape, SetConvexHullShape)
- Soft body physics via btSoftBody integration (Sample 59)
- Legacy CollisionShape component still supported for backward compatibility

### Rendering
- FXAA anti-aliasing post-process
- Bloom post-process
- Motion blur post-process with camera-based reprojection (Sample 58)
- Custom shader uniform support (bare GLSL uniforms wrapped into constant buffer blocks)

### Engine
- AngelScript bindings for new RigidBody shape API
- Multi-viewport instancing fix for deferred GPU execution

## License
Licensed under the MIT license, see [LICENSE](licenses/urho3d/LICENSE) for details.

## Contributing
Before making pull requests, please read the [Contribution checklist](https://urho3d.io/documentation/HEAD/_contribution_checklist.html) and [Coding conventions](https://urho3d.io/documentation/HEAD/_coding_conventions.html) pages from the documentation.

## Credits

### Vulkan Backend & Engine Enhancements (v2.0.1)
- Leith Ketchell (https://github.com/LeithKetchell) — Vulkan backend, physics integration, soft body support, rendering enhancements
- Claude (https://github.com/anthropics) — AI pair programming assistant

### Original Engine
Urho3D is greatly inspired by OGRE (http://www.ogre3d.org) and Horde3D
(http://www.horde3d.org). Additional inspiration & research used:
- Rectangle packing by Jukka Jylänki (clb)
  http://clb.demon.fi/projects/rectangle-bin-packing
- Tangent generation from Terathon
  http://www.terathon.com/code/tangent.html
- Fast, Minimum Storage Ray/Triangle Intersection by Möller & Trumbore
  http://www.graphics.cornell.edu/pubs/1997/MT97.pdf
- Linear-Speed Vertex Cache Optimisation by Tom Forsyth
  http://home.comcast.net/~tom_forsyth/papers/fast_vert_cache_opt.html
- Software rasterization of triangles based on Chris Hecker's
  Perspective Texture Mapping series in the Game Developer magazine
  http://chrishecker.com/Miscellaneous_Technical_Articles
- Networked Physics by Glenn Fiedler
  http://gafferongames.com/game-physics/networked-physics/
- Euler Angle Formulas by David Eberly
  https://www.geometrictools.com/Documentation/EulerAngles.pdf
- Red Black Trees by Julienne Walker
  http://eternallyconfuzzled.com/tuts/datastructures/jsw_tut_rbtree.aspx
- Comparison of several sorting algorithms by Juha Nieminen
  http://warp.povusers.org/SortComparison/

Urho3D uses the following third-party libraries:
- AngelScript 2.35.1 WIP (http://www.angelcode.com/angelscript)
- Boost 1.64.0 (http://www.boost.org) - only used for AngelScript generic bindings
- Box2D 2.4.1+ (https://box2d.org)
- Bullet 3.06+ (http://www.bulletphysics.org)
- Civetweb 1.7 (https://github.com/civetweb/civetweb)
- FreeType 2.8 (https://www.freetype.org)
- GLEW 1.13.0 (http://glew.sourceforge.net)
- SLikeNet (https://github.com/SLikeSoft/SLikeNet)
- libcpuid 0.4.0+ (https://github.com/anrieff/libcpuid)
- Lua 5.1 (https://www.lua.org)
- LuaJIT 2.1.0+ (http://www.luajit.org)
- LZ4 1.7.5 (https://github.com/lz4/lz4)
- Mustache 1.0 (https://mustache.github.io, https://github.com/kainjow/Mustache)
- nanodbc 2.12.4+ (https://lexicalunit.github.io/nanodbc)
- Open Asset Import Library 4.1.0 (http://assimp.sourceforge.net)
- pugixml 1.12+ (http://pugixml.org)
- RapidJSON 1.1.0+ (https://github.com/Tencent/rapidjson)
- Recast/Detour (https://github.com/recastnavigation/recastnavigation)
- SDL 2.0.10+ (https://www.libsdl.org)
- SQLite 3.36.0 (https://www.sqlite.org)
- StanHull (https://codesuppository.blogspot.com/2006/03/john-ratcliffs-code-suppository-blog.html)
- stb_image 2.18 (https://nothings.org)
- stb_image_write 1.08 (https://nothings.org)
- stb_rect_pack 0.11 (https://nothings.org)
- stb_vorbis 1.13b (https://nothings.org)
- tolua++ 1.0.93 (defunct - http://www.codenix.com/~tolua)
- WebP (https://chromium.googlesource.com/webm/libwebp)
- ETCPACK (https://github.com/Ericsson/ETCPACK)
- Tracy 0.7.6 (https://github.com/wolfpld/tracy)
- VMA (Vulkan Memory Allocator, when Vulkan rendering backend is enabled)

DXT / PVRTC decompression code based on the Squish library and the Oolong
Engine.
Jack and mushroom models from the realXtend project. (https://www.realxtend.org)
Ninja model and terrain, water, smoke, flare and status bar textures from OGRE.
BlueHighway font from Larabie Fonts.
Anonymous Pro font by Mark Simonson.
NinjaSnowWar sounds by Veli-Pekka Tätilä.
PBR textures from Substance Share. (https://share.allegorithmic.com)
IBL textures from HDRLab's sIBL Archive.
Dieselpunk Moto model by allexandr007.
Mutant & Kachujin models from Mixamo.
License / copyright information included with the assets as necessary. All other assets (including shaders) by Urho3D authors and licensed similarly as the engine itself.

## Documentation
Urho3D classes have been sparsely documented using Doxygen notation. To
generate documentation into the "Docs" subdirectory, open the Doxyfile in the
"Docs" subdirectory with doxywizard and click "Run doxygen" from the "Run" tab.
Get Doxygen from http://www.doxygen.org & Graphviz from http://www.graphviz.org.
See section "Documentation build" below on how to automate documentation
generation as part of the build process.

The documentation is also available online at
  https://urho3d.io/documentation/HEAD/index.html

Latest documentation: <https://rurho3d.github.io>

Documentation on how to build Urho3D:
  https://urho3d.io/documentation/HEAD/_building.html
Documentation on how to use Urho3D as external library
  https://urho3d.io/documentation/HEAD/_using_library.html

Replace HEAD with a specific release version in the above links to obtain the
documentation pertinent to the specified release. Alternatively, use the
document-switcher in the documentation website to do so.

## Vulkan Graphics Backend (v2.0.1)

This build includes a **modern Vulkan graphics backend** alongside the legacy OpenGL and Direct3D 11 support.

**Status**: Working - all 55+ samples build and run successfully with the Vulkan backend.

### Building with Vulkan (Linux)

**Prerequisites:**
```bash
sudo apt-get install -y cmake build-essential libsdl2-dev libvulkan-dev \
    vulkan-tools vulkan-validationlayers-dev glslang-tools
```

**Configure and build:**
```bash
mkdir -p build && cd build

cmake .. \
    -DURHO3D_VULKAN=1 \
    -DURHO3D_GLSLANG=1 \
    -DURHO3D_SAMPLES=1 \
    -DURHO3D_PHYSICS=1 \
    -DURHO3D_NAVIGATION=1 \
    -DURHO3D_NETWORK=1 \
    -DURHO3D_ANGELSCRIPT=1 \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

**Important:** `-DURHO3D_GLSLANG=1` is **required** for runtime GLSL to SPIR-V shader compilation. Without it, the Vulkan backend cannot compile shaders.

**Run a sample:**
```bash
cd bin
./09_MultipleViewports
```

**Optional: Enable Vulkan validation layers for debugging:**
```bash
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
./bin/01_HelloWorld
```

### Vulkan Build Flags Reference

| Flag | Required | Purpose |
|------|----------|---------|
| `URHO3D_VULKAN=1` | Yes | Enable Vulkan graphics backend |
| `URHO3D_GLSLANG=1` | Yes | GLSL to SPIR-V runtime compilation |
| `URHO3D_SAMPLES=1` | No | Build example projects |
| `URHO3D_PHYSICS=1` | No | Bullet physics support |
| `URHO3D_NAVIGATION=1` | No | Recast/Detour pathfinding |
| `URHO3D_NETWORK=1` | No | SLikeNet networking |
| `URHO3D_ANGELSCRIPT=1` | No | AngelScript scripting |
| `CMAKE_BUILD_TYPE=Release` | No | Optimized build (recommended) |

## History
The ORIGINAL change history is available online at
  https://urho3d.io/documentation/HEAD/_history.html
The VULKAN change history is tracked in git commit messages.
