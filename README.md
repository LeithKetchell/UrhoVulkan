# UrhoVulkan (aka Urho3D 2.0.1)
Adds an optional Vulkan rendering backend on top of Urho3D 1.9.0, supporting all the original examples, shaders, materials and render paths, and without sacrificing legacy backends.
When enabled (via cmake URHO3D_VULKAN option), Vulkan support is injected deeply into Urho3D's existing rendering system, including parallel draw batching and many other optimizations.
The current state offers 40 to 60% better framerates when compared to OpenGL, with the greatest gains realized in the more complex scenes.
The goal is to go further, and leverage Vulkan support across many more Urho3D subsystems.

Author: Leith Ketchell (with assistance from Claude Code)
License: MIT (see licenses/urho3d)
Date: December 3, 2025
