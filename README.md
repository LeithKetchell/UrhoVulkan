![Urho3D logo](https://raw.githubusercontent.com/urho3d/Urho3D/master/bin/Data/Textures/LogoLarge.png)

# Urho3D 2.0.1

[![CI/CD](https://github.com/urho3d/Urho3D/workflows/CI/CD/badge.svg)](https://github.com/urho3d/Urho3D/actions?query=workflow%3ACI%2FCD)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/4954/badge.svg)](https://scan.coverity.com/projects/urho3d-urho3d)

**Urho3D** is a free lightweight, cross-platform 2D and 3D game engine implemented in C++ and released under the MIT license. Greatly inspired by OGRE and Horde3D.

Main website: [https://urho3d.io/](https://urho3d.io/)

---

## What's New in 2.0.1

### Vulkan Graphics Backend
- Full Vulkan rendering backend alongside legacy OpenGL and Direct3D 11
- All 55+ samples build and run on Vulkan
- GLSL to SPIR-V runtime shader compilation via glslang
- `URHO_VULKAN` shader define for Vulkan-specific shader paths (do not use `VULKAN` — conflicts with glslang)
- Native VkPipelineCache with on-disk persistence
- Shader module caching and redundant bind elimination
- Hybrid framebuffer support (swapchain color + custom depth RTT)
- RTT Y-flip fix — water reflections and render-to-texture samples render correctly
- Shadow mapping with projection-matrix depth bias
- TextureCube / skybox support
- Anisotropic filtering
- Render-to-texture infrastructure
- Per-frame profiler UI overlay (ProfilerUI) on all samples

### Rendering
- FXAA anti-aliasing post-process
- Bloom post-process
- Motion blur post-process with camera-based reprojection (Sample 58)
- God rays post-process with 3-phase sun glow (pale white, vivid red at sunset, fade below horizon) and cool blue moon glow
- Custom shader uniform support (bare GLSL uniforms wrapped into constant buffer blocks)
- Selection outline system (silhouette mask + Sobel edge detection)
- Height fog with min/max height range, underwater inverse fog, and atmospheric time-of-day cycling
- Underwater color dynamically tracks zone fog color per frame instead of hardcoded values
- 4-layer terrain texture blending (4 detail layers from 3 texture fetches via alpha channel packing) with division-by-zero guard on empty weight maps
- Celestial body depth — sun and moon write depth (0.9999) so terrain occludes them correctly

### Physics Enhancements
- Collision shapes integrated directly into RigidBody — no separate CollisionShape component required for common shapes (box, sphere, capsule, cylinder, cone, plane, trimesh, convex hull)
- Global primitive shape cache in PhysicsWorld — identical shapes shared across bodies (e.g. 500 boxes = 1 btBoxShape)
- Compound shape API on RigidBody (AddChildShape / ClearChildShapes) — compound children reference cached primitives
- Mesh/convex hull shape support (SetTriMeshShape, SetConvexHullShape)
- MultiBody component — btMultiBody integration for articulated rigid body chains
- Soft body physics via btSoftBody integration (Sample 59)
- Legacy CollisionShape component still supported for backward compatibility
- Rigid body activation events (E_RIGIDBODYSLEEP / E_RIGIDBODYWAKEUP)

### Particle System Enhancements
- Particle collision system — Kill, Bounce, and Stick modes for particles hitting surfaces
- Y-plane collision (fast, no physics dependency) and physics raycast collision (accurate)
- Decal spawning on particle impact — configurable material, size, and lifetime
- Full XML serialization — collision settings saved/loaded with particle effect files
- Zero cost when disabled, backward compatible

### Animal System
- **Animal base class** — LogicComponent with state machine (IDLE / WANDER / FLEE / DIE)
- Auto-scales model to target size using bounding box
- Terrain following, water avoidance, respawn, animation crossfade
- **Species**: Deer (3 anims), Fox (12 anims — attack, eating, death, gallop, walk, idle variants, hit reactions, jumps), Rabbit (11 anims)
- **SchoolFish** — flocking behaviour (cohesion, alignment, separation), 3 schools of 15

### Networking & AuthServer
- **AuthServer** — central authority server for multiplayer terrain editing (port 9090, UDP via SLikeNet)
- **PAKE authentication** — Password-Authenticated Key Exchange using libsodium (X25519 + XChaCha20-Poly1305)
- 4-message encrypted login flow with username enumeration prevention and forward secrecy
- **SQLite database** — user accounts and terrain patch ownership
- **Patch ownership** — grid-based territory system (128-unit patches), claim and query via protocol
- **Edit validation** — server validates terrain/object edits by patch ownership, broadcasts to all authenticated clients
- **Peer brokering framework** — AuthServer introduces peers with shared random tokens
- **Relay framework** — subclient-to-AuthServer message relay through subservers
- **Debug GUI** — live client list, color-coded activity log, connection status
- **LAN discovery** — broadcast beacon for auto-discovery on local network
- Build with `-DURHO3D_NETWORK=1 -DURHO3D_DATABASE_SQLITE=1`

### Bullet SDK Samples
- Faithful ports of Bullet SDK examples as standalone Urho3D samples
- Build with `-DURHO3D_BULLET_SAMPLES=1`
- Includes: BasicDemo, Chain, CollisionFilter, Constraints, Domino, ForkLift, Gyroscopic, Heightfield, HelloPhysics, Hinge2Vehicle, Kinematic, MotorDemo, Planar2D, PlanetGravity, Raycast, RollingFriction, SoftContact, SoftDemo, Stacking, TwoJoint, and more

---

## Tools

### ModelViewer
Standalone model inspection and repair tool (`build/bin/ModelViewer`).

- Load any .mdl file, auto-apply material lists, orbital camera with auto-framing
- **Mesh inspector**: per-geometry vertex/index counts, vertex element layout, LOD levels, bounding box dimensions
- **Skeleton inspector**: bone hierarchy with indentation, radius display, bone picking (click to highlight subtree)
- **Animation inspector**: track listing with channel masks (P/R/S), keyframe counts, time ranges, trigger points
- **Material inspector**: techniques, texture bindings by unit name, shader parameters, VS/PS defines
- **Morph target display** with current weights
- **Multi-animation blending**: per-layer weight sliders, Lerp/Add toggle, loop/reverse controls
- **Vertex Editor** (V key): select/move/delete vertices with ray-based picking, save edited model back to disk (Ctrl+S)
- Collapsible info panel sections, help overlay (H key) with full keybinding reference

### AssetImporter
- **`info` command** — inspect native .mdl and .ani files without Assimp: geometry counts, bounding box dimensions, skeleton hierarchy, animation tracks with channel masks and keyframe ranges. Diagnoses scale problems — shows the bounding box diagonal and warns when a model is oversized, suggesting the correct `-scale` factor
- Bone scaling bug fixed — initialPosition, offsetMatrix, radius, and boundingBox now scale correctly
- Auto material list generation — multi-material models automatically get a .txt material list
- Strips CR/LF/CRLF from filenames and asset names

### WorkboardManager
GUI dashboard for team coordination (`build/bin/WorkboardManager`).

- Workboard display, plan browser, instance status, message composer
- Bidirectional messaging with Claude Code instances via message spool directories
- Automatic liveness detection — crashed instances culled from status display
- LAN discovery beacon for multi-tool coordination
- Font/theme customization with persistent preferences
- Build with `-DURHO3D_TOOLS=1`

---

## FBX Pipeline

Urho3D's AssetImporter supports two FBX pathways using **incompatible FBX format versions**. Each pathway is one-directional.

### Import: External FBX to Urho
```
AssetImporter model input.fbx output.mdl [-scale 0.01]
```
- Reads **old FBX** (pre-7500) — the format used by Mixamo, Quaternius, and most asset stores
- Produces `.mdl` (model) and `.ani` (animation) files
- **Cannot** import FBX 7500 files

### Export: Urho to Blender
```
AssetImporter export input.mdl output.fbx [input.ani ...]
```
- Writes **FBX 7500** — the format Blender reads best
- For editing, re-rigging, or adding animations in Blender
- **Cannot** export old FBX format

### Round-Trip Workflow
```
External Asset (Mixamo, etc.)
    | Old FBX
    v
AssetImporter import  -->  .mdl / .ani  -->  Urho3D
    |
    v FBX 7500 (AssetImporter export)
Blender (edit, add anims, rekey)
    |
    v Old FBX (Blender FBX export)
AssetImporter import  -->  .mdl / .ani  -->  Urho3D
```

Blender is the format bridge. You cannot skip Blender when going from FBX 7500 back to Urho.

### Import Scale
Artists are crap at making things to scale. Mixamo models are 100x too large. Always check:
```bash
AssetImporter info model.mdl    # shows bounding box diagonal
AssetImporter model input.fbx output.mdl -scale 0.01
```
FBX unit metadata is **not** honored by Assimp — you must use `-scale` manually.

### Model Formats
The engine reads three Urho model formats:
- **UMDL** (legacy bitmask) — use this for import (`-t` flag or default)
- **UMD2** (explicit vertex elements) — crashes on load in this build, avoid
- **UMD3** (header bounding box) — reverse-engineered, read-only support

---

## Editor (Sample 60 / TerrainNode)

A lightweight scene editor built as Sample 60 with multiplayer connectivity via AuthServer — LAN discovery, encrypted login, collaborative terrain/object editing with server-authoritative validation.

### Controls
- **WASD** to move, **mouse** to look (camera mode)
- **TAB** toggles camera mode / cursor mode (needed for menus and UI)
- **ESC** peels back UI layers one at a time
- **NumPad Enter** toggles all UI on/off

### Menu Bar
- **File**: Save/Load Scene, Import Model, Export Prefab, Exit
- **Create**: From Prefab (load XML as object brush), Clear Object Brush
- **Edit**: Undo/Redo (Ctrl+Z/Y), Transform mode (Translate/Rotate/Scale), World/Local toggle
- **View**: Hierarchy window, Inspector window
- **Environment**: Terrain Tools, Time of Day slider (-12h to +12h)
- **Overlay**: Toggle Fullscreen (F11), Wireframe (Z), Debug Geometry (Space), Height Fog (H), Profiler

### Scene Hierarchy
Tree view of all nodes with expand/collapse. Nodes in white, components in green. Click to select, double-click to fly camera to node.

### Property Inspector
Editable attributes for the selected node — position, rotation, scale, component attributes. Floats, ints, strings, bools, enums, Vector3, Quaternion, Color — all editable inline. Changes apply immediately.

### Terrain Editing
- LMB = raise, RMB = lower
- Brush modes: Raise/Lower, Smooth, Flatten
- Brush shapes: circle, square, triangle, star, pentagon, hexagon, octagon
- Scroll wheel adjusts brush radius
- 32-bit heightmap precision (~4 billion height levels)
- Brush rotation slider with text input

### Day/Night Cycle
- Real-time sun/moon positioning synced to network time (Melbourne, AU)
- Date/Month/Year sliders for seasonal variation
- Cloud layer and star field rotate independently
- Height fog auto-scales with time of day
- God rays on sun and moon, reflected in water
- 1/2 keys track sun/moon with camera

### Prefab System
- Export selected node subtree as XML prefab
- Load prefab as object brush, click terrain to instance clones
- Clones orient to terrain surface normal
- Static clones settle via temporary physics then revert to zero-mass

### Underwater Fish — The Urho
The Urho is the official mascot of the Urho3D engine — a spiny, prehistoric-looking fish native to Finnish waters. 50 swim in the editor's ocean with reactive steering behaviours.

- CC0 fish model by Modanung (Urho3D community contributor) — first known UMD3 model loaded by the engine
- Vertex shader tail wiggle (FishWiggle.glsl) — sine wave displacement, adjustable via Environment menu
- Terrain-aware spawning, water column clamping
- Reactive steering: neighbour avoidance, wander, shallow water avoidance, boundary containment
- Camera interaction zones: orbit (3-15m), stare (<3m), veer-off (<1m)
- Depth-dependent behaviour — fish stay deep, rising at dawn/dusk with per-fish randomized schedule

### Minimap
Bottom-right corner, GPU-rendered orthographic RTT camera. Rotates with camera yaw, red dot shows position.

---

## IPC System (Message Spool V2)

Claude Code instances and the WorkboardManager communicate via atomic message files in spool directories. Messages cannot be lost — files persist until consumed.

```
/tmp/urho_claude/
    spool/
        to_coder/       # Messages waiting for Coder
        to_planner/     # Messages waiting for Planner
        to_manager/     # Messages waiting for Manager
    instances/          # PID/role registration
    wake_<role>         # FIFO for instant notification
```

Each message is an atomic file (`.tmp` -> rename to `.msg`) with headers and body. Sequence-numbered filenames guarantee ordering. See `Claude/IPC_PROTOCOL.md` for full details.

---

## Building

### Prerequisites (Linux)
```bash
sudo apt-get install -y cmake build-essential libsdl2-dev libvulkan-dev \
    vulkan-tools vulkan-validationlayers-dev glslang-tools
```

### Configure and Build
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
    -DURHO3D_TOOLS=1 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

make -j$(nproc)
```

**Important:** `-DURHO3D_GLSLANG=1` is **required** when using `-DURHO3D_VULKAN=1`. Without it, the Vulkan backend cannot compile shaders at runtime.

### Run a Sample
```bash
cd bin
./09_MultipleViewports
```

### Vulkan Validation Layers
```bash
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
./bin/04_StaticScene
```

### Build Flags

| Flag | Required | Purpose |
|------|----------|---------|
| `URHO3D_VULKAN=1` | — | Vulkan graphics backend |
| `URHO3D_GLSLANG=1` | With Vulkan | GLSL to SPIR-V runtime compilation |
| `URHO3D_SAMPLES=1` | — | Build 55+ example projects |
| `URHO3D_TOOLS=1` | — | Build ModelViewer, WorkboardManager, AssetImporter |
| `URHO3D_PHYSICS=1` | — | Bullet physics support |
| `URHO3D_NAVIGATION=1` | — | Recast/Detour pathfinding |
| `URHO3D_NETWORK=1` | — | SLikeNet networking + libsodium encryption |
| `URHO3D_DATABASE_SQLITE=1` | — | SQLite database (required for AuthServer) |
| `URHO3D_ANGELSCRIPT=1` | — | AngelScript scripting |
| `URHO3D_LUA=1` | — | Lua scripting |
| `URHO3D_BULLET_SAMPLES=1` | — | Bullet SDK sample ports |
| `CMAKE_BUILD_TYPE` | — | Debug / Release / RelWithDebInfo |

### Cross-Compilation
```bash
# Raspberry Pi / ARM
script/cmake_rpi.sh build

# Web (Emscripten)
script/cmake_emscripten.sh build

# iOS (with optional Vulkan via MoltenVK)
script/cmake_ios.sh build [-DURHO3D_VULKAN=1]

# Android (Vulkan native on API 24+)
rake build PLATFORM=android [URHO3D_VULKAN=1]

# MinGW (Windows)
script/cmake_mingw.sh build
```

---

## Vulkan Fixes
- Fullscreen toggle: RecreateSwapchainResources preserves device/textures/buffers
- Clear command: force EnsureRenderPassStarted when targets are dirty
- OnDeviceReset: proper reload for Texture2D, TextureCube, VertexBuffer, IndexBuffer
- Release null-safety for device/allocator teardown ordering

## Engine Changes
- AngelScript bindings for new RigidBody shape API and MultiBody
- Multi-viewport instancing fix for deferred GPU execution
- Vulkan screenshot capture (swapchain -> staging buffer -> BGRA->RGB -> PNG)
- Format string parsing fix in Str.cpp (flags, width, precision, length modifiers)
- Fixed upstream 1.9 bug in binding generator (`RemoveRefs` in `XmlAnalyzer.cpp`) — Doxygen XML nodes concatenated without whitespace, producing broken type declarations (~188 registrations affected)
- Animation text key infrastructure — AnimationTextKey, E_ANIMATIONTEXTKEY event, JSON sidecar loading. Fully wired, ready for tooling.

## Model Format
- UMD3 format support — reverse-engineered (bounding box at header, otherwise identical to UMD2)
- Oversized model rejection — `SetModel()` rejects models with bounding box exceeding 10 units unless `allowOversized = true`
- Engine reads all three Urho model formats: UMDL, UMD2, UMD3

---

## License
Licensed under the MIT license, see [LICENSE](licenses/urho3d/LICENSE) for details.

## Contributing
Before making pull requests, please read the [Contribution checklist](https://urho3d.io/documentation/HEAD/_contribution_checklist.html) and [Coding conventions](https://urho3d.io/documentation/HEAD/_coding_conventions.html).

## Credits

### Vulkan Backend & Engine Enhancements (v2.0.1)
- Leith Ketchell (https://github.com/LeithKetchell) — Vulkan backend, physics integration, soft body support, rendering enhancements, networking & AuthServer, animal system, editor
- Claude (https://github.com/anthropics) — AI pair programming assistant

### Original Engine
Urho3D is greatly inspired by OGRE (http://www.ogre3d.org) and Horde3D
(http://www.horde3d.org). Additional inspiration & research used:
- Rectangle packing by Jukka Jylanki (clb)
  http://clb.demon.fi/projects/rectangle-bin-packing
- Tangent generation from Terathon
  http://www.terathon.com/code/tangent.html
- Fast, Minimum Storage Ray/Triangle Intersection by Moller & Trumbore
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

### Third-Party Libraries
Urho3D uses 30+ external libraries in `/Source/ThirdParty/`:

AngelScript 2.35.1, Boost 1.64.0, Box2D 2.4.1+, Bullet 3.06+, Civetweb 1.7, FreeType 2.8, GLEW 1.13.0, SLikeNet, libsodium 1.0.21, libcpuid 0.4.0+, Lua 5.1, LuaJIT 2.1.0+, LZ4 1.7.5, Mustache 1.0, nanodbc 2.12.4+, Open Asset Import Library 4.1.0, pugixml 1.12+, RapidJSON 1.1.0+, Recast/Detour, SDL 2.0.10+, SQLite 3.36.0, StanHull, stb_image 2.18, stb_image_write 1.08, stb_rect_pack 0.11, stb_vorbis 1.13b, tolua++ 1.0.93, WebP, ETCPACK, Tracy 0.7.6, VMA (Vulkan Memory Allocator).

License information for all dependencies is in `/licenses/`.

### Assets
Jack and mushroom models from realXtend. Ninja model and terrain/water/smoke textures from OGRE. BlueHighway font from Larabie Fonts. Anonymous Pro font by Mark Simonson. NinjaSnowWar sounds by Veli-Pekka Tatila. PBR textures from Substance Share. IBL textures from HDRLab's sIBL Archive. Dieselpunk Moto model by allexandr007. Mutant & Kachujin models from Mixamo. Fish model by Modanung (CC0, OpenGameArt). Animal models from Quaternius (CC0). All other assets (including shaders) by Urho3D authors, licensed as the engine itself.

## Documentation
- Online API Reference: https://urho3d.io/documentation/HEAD/index.html
- Building: https://urho3d.io/documentation/HEAD/_building.html
- Using as library: https://urho3d.io/documentation/HEAD/_using_library.html
- Generate locally: `cd Docs && doxygen Doxyfile`

## History
The original change history is at https://urho3d.io/documentation/HEAD/_history.html.
The 2.0.1 change history is tracked in git commit messages.
