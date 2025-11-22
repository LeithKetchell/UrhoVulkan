//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan descriptor pool management (Phase 7)

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/Vector.h"
#include "../../Container/HashMap.h"
#include "../../Math/StringHash.h"
#include <vulkan/vulkan.h>

namespace Urho3D
{

class Graphics;

/// Descriptor set layout hash for caching
struct DescriptorSetLayoutKey
{
    Vector<VkDescriptorSetLayoutBinding> bindings;

    unsigned Hash() const;
    bool operator==(const DescriptorSetLayoutKey& other) const;
};

/// Manages descriptor pool and descriptor set allocations
class VulkanDescriptorPool
{
public:
    /// Constructor
    VulkanDescriptorPool();
    /// Destructor
    ~VulkanDescriptorPool();

    /// Initialize the descriptor pool
    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice);

    /// Release resources
    void Release();

    /// Get or create descriptor set layout
    VkDescriptorSetLayout GetOrCreateDescriptorSetLayout(
        const Vector<VkDescriptorSetLayoutBinding>& bindings);

    /// Allocate a descriptor set
    VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);

    /// Get descriptor pool
    VkDescriptorPool GetPool() const { return descriptorPool_; }

    /// Get device
    VkDevice GetDevice() const { return device_; }

private:
    VkDevice device_{};
    VkDescriptorPool descriptorPool_{};

    // Cached descriptor set layouts by binding configuration
    HashMap<StringHash, VkDescriptorSetLayout> layoutCache_;
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
