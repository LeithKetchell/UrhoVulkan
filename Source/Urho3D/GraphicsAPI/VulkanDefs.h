//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//

#pragma once

#ifdef URHO3D_VULKAN

#include <vulkan/vulkan.h>

namespace Urho3D
{

// Vulkan-specific constants and definitions

/// Default frame pipelining depth (number of in-flight frames)
static const uint32_t VULKAN_FRAME_COUNT = 2;

/// Default descriptor pool sizes
static const uint32_t VULKAN_DESCRIPTOR_POOL_SIZE = 1000;
static const uint32_t VULKAN_DESCRIPTOR_SET_LAYOUT_POOL_SIZE = 100;

/// Default command buffer pool sizes
static const uint32_t VULKAN_COMMAND_BUFFER_PER_FRAME = 1;

/// Swapchain configuration
static const uint32_t VULKAN_DEFAULT_SWAPCHAIN_IMAGES = 2;
static const VkFormat VULKAN_PREFERRED_SURFACE_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;
static const VkFormat VULKAN_FALLBACK_SURFACE_FORMAT = VK_FORMAT_R8G8B8A8_SRGB;

/// Depth buffer configuration
static const VkFormat VULKAN_PREFERRED_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat VULKAN_FALLBACK_DEPTH_FORMAT = VK_FORMAT_D24_UNORM_S8_UINT;

/// Pipeline cache settings
static const uint64_t VULKAN_PIPELINE_CACHE_SIZE = 10 * 1024 * 1024;  // 10 MB

} // namespace Urho3D

#endif  // URHO3D_VULKAN
