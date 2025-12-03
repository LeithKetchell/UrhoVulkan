# UrhoVulkan (aka Urho3D 2.0.1)
This project is a Fork based on Urho3D 1.9.0 (the final English version).
Urho3D 2.9.1 provides optional Vulkan integration at buildtime, while retaining support for all legacy backends/platforms (it's non-invasive, and can be easily enabled/disabled via cmake).
When enabled (via cmake URHO3D_VULKAN option), Vulkan support is injected deeply into Urho3D's existing rendering system at buildtime, providing parallel draw batching and many other optimizations, which together aim to unlock your machine's true graphics potential, mostly by eliminating or reducing cpu-gpu bottlenecks found in older renderers, leading to higher general performance.
For example, the current Vulkan state offers 40 to 60% better framerates when compared to OpenGL, noting that the greatest gains are only fully realized in more complex scenes (it scales well).
The goal is to go further, and leverage Vulkan support across many more Urho3D subsystems, unlocking even more performance potential.

Author: Leith Ketchell (with assistance from Claude Code)
License: MIT (see licenses/urho3d)
Date: December 3, 2025
