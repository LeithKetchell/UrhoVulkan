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
- `URHO_VULKAN` shader define for Vulkan-specific shader paths
- Native VkPipelineCache with on-disk persistence
- Shader module caching and redundant bind elimination
- Hybrid framebuffer support (swapchain color + custom depth RTT)
- RTT Y-flip fix — water reflections and render-to-texture samples render correctly
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
- MultiBody component — btMultiBody integration for articulated rigid body chains
- Soft body physics via btSoftBody integration (Sample 59)
- Legacy CollisionShape component still supported for backward compatibility
- Rigid body activation events (E_RIGIDBODYSLEEP / E_RIGIDBODYWAKEUP) — dispatched by PhysicsWorld when bodies transition between active and sleeping states

### Particle System Enhancements
- Particle collision system — Kill, Bounce, and Stick modes for particles hitting surfaces
- Y-plane collision (fast, no physics dependency) and physics raycast collision (accurate, uses scene physics)
- Decal spawning on particle impact — configurable material, size, and lifetime
- Full XML serialization — collision settings saved/loaded with particle effect files
- Zero cost when disabled — no collision checks unless explicitly enabled per effect
- Backward compatible — existing particle effects work unchanged

### Rendering
- FXAA anti-aliasing post-process
- Bloom post-process
- Motion blur post-process with camera-based reprojection (Sample 58)
- God rays post-process with 3-phase sun glow (pale white → vivid red at sunset → fade below horizon) and cool blue moon glow
- Custom shader uniform support (bare GLSL uniforms wrapped into constant buffer blocks)
- Selection outline system (Silhouette mask + Sobel edge detection)
- Height fog with min/max height range, underwater inverse fog, and atmospheric time-of-day cycling
- Underwater color dynamically tracks zone fog color per frame instead of hardcoded values
- 4-layer terrain texture blending (4 detail layers from 3 texture fetches via alpha channel packing) with division-by-zero guard on empty weight maps
- Celestial body depth — sun and moon write depth (0.9999) so terrain occludes them correctly

### Bullet SDK Samples
- Faithful ports of Bullet SDK examples as standalone Urho3D samples
- Build with `-DURHO3D_BULLET_SAMPLES=1`
- Includes: BasicDemo, Chain, CollisionFilter, Constraints, Domino, ForkLift, Gyroscopic, Heightfield, HelloPhysics, Hinge2Vehicle, Kinematic, MotorDemo, Planar2D, PlanetGravity, Raycast, RollingFriction, SoftContact, SoftDemo, Stacking, TwoJoint, and more

### Networking & AuthServer
- **AuthServer** — central authority server for multiplayer terrain editing (port 9090, UDP via SLikeNet)
- **PAKE authentication** — Password-Authenticated Key Exchange using libsodium (X25519 + XChaCha20-Poly1305)
- 4-message encrypted login flow: key exchange, key reply, encrypted identity, encrypted auth result
- Username enumeration prevention — unknown users get random hash, same timing
- Forward secrecy — ephemeral key pairs per connection, session keys never transmitted
- **SQLite database** — user accounts (username, password hash, admin level) and terrain patch ownership
- **Patch ownership** — grid-based territory system (128-unit patches), claim and query via protocol
- **Edit validation** — server validates terrain/object edits by patch ownership, broadcasts to all authenticated clients
- **Peer brokering framework** — AuthServer introduces peers with shared random tokens for future NAT punchthrough
- **Relay framework** — subclient-to-AuthServer message relay through subservers
- **Debug GUI** — live client list, color-coded activity log, connection status
- **LAN discovery** — broadcast beacon for auto-discovery on local network
- Build with `-DURHO3D_NETWORK=1 -DURHO3D_DATABASE_SQLITE=1`

### Editor Sample (23_Water / 60_TerrainNode)

A lightweight scene editor built into Sample 23 (standalone) and forked as Sample 60 (networked). Sample 60 adds multiplayer connectivity via AuthServer — LAN discovery, encrypted login, collaborative terrain/object editing with server-authoritative validation.

**Getting Around**
- WASD to move, mouse to look (camera mode)
- TAB to toggle between camera mode and cursor mode (needed for menus and UI)
- ESC peels back UI layers one at a time (inspector, hierarchy, panels, brush, selection, then cursor mode)

**Menu Bar** (The secondary Enter key on your NumPad toggles all UI on/off)
- **File**: Save/Load Scene, Import Model, Generate Primitive (disabled), Export Prefab, Exit
- **Create**: From Prefab (load XML as object brush), Clear Object Brush, brush status label
- **Edit**: Undo/Redo (Ctrl+Z/Y), Transform mode (Translate/Rotate/Scale), World/Local toggle
- **View**: Hierarchy window, Inspector window
- **Environment**: Terrain Tools, Time of Day slider (-12h to +12h)
- **Overlay**: Toggle Fullscreen (F11), Wireframe (Z), Debug Geometry (Space), Height Fog (H), Profiler

**Selecting Objects**
- In cursor mode: click an object in the viewport to select it (yellow outline)
- In camera mode: crosshair pick (center of screen)
- BACKSPACE or DELETE removes the selected object
- Click empty space to deselect

**Scene Hierarchy** (View > Hierarchy)
- Tree view of all nodes in the scene with expand/collapse
- Nodes shown in white, components in green
- Click a node to select it (syncs with viewport outline)
- Double-click to fly the camera to that node

**Property Inspector** (View > Inspector)
- Shows the selected node's name, position, rotation, and scale — all editable
- Lists each component with its registered attributes
- Floats, ints, strings: text fields. Bools: checkboxes. Enums: dropdowns
- Vector3/Quaternion/Color: per-axis fields with color-coded labels (R/G/B = X/Y/Z)
- Changes apply immediately

**Parenting Objects**
- Select a node in the hierarchy or viewport, then use Create > Import Model or Generate Mesh
- The new object becomes a child of the selected node
- If nothing is selected, the new object is created at the scene root
- Child nodes inherit their parent's transform (move the parent, children follow)

**Terrain Editing** (Environment > Terrain Tools)
- LMB = raise, RMB = lower (in raise/lower mode)
- Brush modes: Raise/Lower, Smooth, Flatten
- Brush shapes: circle, square, triangle, star, pentagon, hexagon, octagon
- Scroll wheel adjusts brush radius
- 32-bit heightmap precision (R + G/256 + B/65536 + A/16M, ~4 billion height levels)
- Brush rotation slider (-180 to +180 degrees) with text input for precise angles

**Day/Night Cycle & Seasonal Environment**
- Real-time sun/moon positioning synced to network time (Melbourne, AU)
- Time of Day slider in Environment menu for scrubbing +/-12 hours
- Date/Month/Year sliders for seasonal variation — sun arc, day length, sky color, and fog respond to calendar date
- Cloud layer and star field rotate independently — clouds drift slowly, stars track celestial time
- Atmospheric rendering: sky tint, fog color, ambient light tied to sun altitude
- Height fog auto-scales with time of day (thickens at dusk/night)
- God rays on sun and moon, reflected in water surface
- 1/2 keys track sun/moon with camera
- An OOFO fleet has been spotted hiding behind clouds — they camouflage against cloud cover and zip between positions, only visible during daylight

**Undo/Redo**
- Ctrl+Z / Ctrl+Y (also in Edit menu)
- Tracks object creation, deletion, and terrain strokes

**Prefab System**
- Export selected node subtree as XML prefab (File > Export Prefab)
- Load prefab as object brush (Create > From Prefab), click terrain to instance clones
- Clones orient to terrain surface normal
- Static (zero-mass) clones receive temporary mass to settle into a safe resting state via physics, then revert to static once the body sleeps

**Underwater Fish — The Urho**
- The Urho is the official mascot of the Urho3D engine — a spiny, prehistoric-looking fish native to Finnish waters. 50 of them now swim in the editor's ocean with reactive steering behaviours
- CC0 fish model by Modanung (Urho3D community contributor) via OpenGameArt — first known UMD3 model loaded by the engine
- Auto-scaled from Blender centimetres to engine metres via oversized model gate
- The fish model shipped with 0 of its 29 bones intact (lost during stale AssetImporter conversion), so we gave it a vertex shader tail wiggle instead (FishWiggle.glsl) — sine wave displacement ramping from body midpoint to tail, amplitude and frequency adjustable via Environment menu sliders
- Terrain-aware spawning — fish only placed where water depth is sufficient
- Reactive steering behaviours:
  - Nearest-neighbour avoidance (lone wolf fish, not schooling)
  - Random wander with vertical pitch changes
  - Shallow water avoidance — probe ahead, turn back to deeper water
  - Boundary containment — steer toward center when too far out
  - Camera interaction — three zones: orbit (3-15m, agitated wiggle, circle the diver), stare (<3m, slow wiggle, face the camera), veer-off (<1m, slide away naturally based on current heading)
  - Speed fades smoothly from full at 15m to 15% near the camera
  - Per-zone material swap (3 pre-built materials, zero runtime allocation) for wiggle variation
- Water column clamping — fish stay between terrain floor and water surface at all times
- Depth-dependent behaviour — fish stay deep by default, rising to the surface only at dawn and dusk with per-fish randomized schedule, depth preference, and eagerness

**Minimap**
- Bottom-right corner, GPU-rendered top-down orthographic RTT camera
- Rotates to match camera yaw (forward is always up)
- Red dot shows camera position

### Vulkan Fixes
- Fullscreen toggle: RecreateSwapchainResources preserves device/textures/buffers
- Clear command: force EnsureRenderPassStarted when targets are dirty (fixes stale RT data)
- OnDeviceReset: proper reload for Texture2D, TextureCube, VertexBuffer, IndexBuffer
- Release null-safety for device/allocator teardown ordering

### Tools
- **ModelViewer** — standalone model inspection and repair tool (`build/bin/ModelViewer`)
  - Load any .mdl file, auto-apply material lists, orbital camera with auto-framing
  - Mesh inspector: per-geometry vertex/index counts, vertex element layout, LOD levels, bounding box dimensions
  - Skeleton inspector: bone hierarchy with indentation, radius display, bone picking (click to highlight subtree)
  - Animation inspector: track listing with channel masks (P/R/S), keyframe counts, time ranges, trigger points
  - Material inspector: techniques, texture bindings by unit name, shader parameters, VS/PS defines
  - Morph target display with current weights
  - Multi-animation blending: per-layer weight sliders, Lerp/Add toggle, loop/reverse controls
  - **Vertex Editor** — press V to enter vertex edit mode on any loaded model
    - All vertices drawn as colored crosses (red = unselected, yellow = selected), scaled by camera distance
    - Click to select nearest vertex (ray-based picking with distance-scaled threshold)
    - Drag to move vertex (constrained to camera-facing plane)
    - DEL to delete vertex — removes vertex, culls all referencing triangles, remaps surviving indices, recalculates bounding box
    - Ctrl+S to save edited model back to disk (.mdl binary format)
    - Clone-on-edit: original model untouched until explicit save
    - Automatic 16/32-bit index type selection based on vertex count
  - Collapsible info panel sections with visual separators between Model, Materials, and Animation groups
  - Help overlay (H key) with full keybinding reference
- **AssetImporter** improvements
  - `info` command — inspect native .mdl and .ani files without Assimp: geometry counts, bounding box dimensions, skeleton hierarchy, animation tracks with channel masks and keyframe ranges. Most useful for diagnosing scale — artists are crap at making things to scale, so `info` shows the bounding box diagonal and warns when a model is oversized, suggesting the correct `-scale` factor to bring it into range
  - Bone scaling bug fixed — initialPosition, offsetMatrix translation, radius, and boundingBox now scale correctly with `-scale` flag
  - Auto material list generation — multi-material models automatically get a .txt material list file alongside the .mdl

### Model Format
- UMD3 model format support — reverse-engineered and documented (bounding box at header, otherwise identical to UMD2)
- Oversized model rejection — `SetModel()` rejects models with bounding box exceeding 10 units unless `allowOversized = true`, preventing accidental stadium-sized geometry from Blender centimetre exports
- Engine now reads all three Urho model formats: UMDL (legacy bitmask), UMD2 (explicit vertex elements), UMD3 (header bounding box)

### Engine
- AngelScript bindings for new RigidBody shape API and MultiBody
- Multi-viewport instancing fix for deferred GPU execution
- Vulkan screenshot capture (swapchain → staging buffer → BGRA→RGB → PNG)
- Format string parsing fix in Str.cpp (flags, width, precision, length modifiers)
- Fixed upstream 1.9 bug in binding generator (`RemoveRefs` in `XmlAnalyzer.cpp`) — Doxygen XML nodes were concatenated without whitespace, producing broken AngelScript type declarations like `"constint"`, `"constfloat"`, `"constString"` etc. (~188 global property registrations affected)

## License
Licensed under the MIT license, see [LICENSE](licenses/urho3d/LICENSE) for details.

## Contributing
Before making pull requests, please read the [Contribution checklist](https://urho3d.io/documentation/HEAD/_contribution_checklist.html) and [Coding conventions](https://urho3d.io/documentation/HEAD/_coding_conventions.html) pages from the documentation.

## Credits

### Vulkan Backend & Engine Enhancements (v2.0.1)
- Leith Ketchell (https://github.com/LeithKetchell) — Vulkan backend, physics integration, soft body support, rendering enhancements, networking & AuthServer
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
- libsodium 1.0.21 (https://libsodium.gitbook.io) - X25519 key exchange, XChaCha20-Poly1305 AEAD encryption, BLAKE2b hashing
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
| `URHO3D_NETWORK=1` | No | SLikeNet networking + libsodium encryption |
| `URHO3D_DATABASE_SQLITE=1` | No | SQLite database (required for AuthServer) |
| `URHO3D_ANGELSCRIPT=1` | No | AngelScript scripting |
| `CMAKE_BUILD_TYPE=Release` | No | Optimized build (recommended) |

## History
The ORIGINAL change history is available online at
  https://urho3d.io/documentation/HEAD/_history.html
The VULKAN change history is tracked in git commit messages.
