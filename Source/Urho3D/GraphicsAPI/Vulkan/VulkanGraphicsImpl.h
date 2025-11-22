//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Container/HashMap.h"
#include "../../Container/Vector.h"
#include "../../Core/Object.h"
#include "../GraphicsDefs.h"
#include "../VulkanDefs.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Urho3D
{

class Graphics;
class ShaderProgram;

/// Vulkan graphics implementation
class VulkanGraphicsImpl : public RefCounted
{
public:
    /// Constructor
    VulkanGraphicsImpl();
    /// Destructor
    virtual ~VulkanGraphicsImpl();

    /// Initialize Vulkan instance, device, swapchain
    bool Initialize(Graphics* graphics, const WindowHandle& window, int width, int height);

    /// Shutdown Vulkan resources
    void Shutdown();

    /// Acquire next swapchain image for rendering
    bool AcquireNextImage();

    /// Submit command buffer and present swapchain image
    void Present();

    /// Get current frame index (for frame pipelining)
    uint32_t GetFrameIndex() const { return frameIndex_; }

    /// Get current swapchain image index
    uint32_t GetCurrentImageIndex() const { return currentImageIndex_; }

    /// Get frame command buffer for recording commands
    VkCommandBuffer GetFrameCommandBuffer() const;

    /// Wait for frame fence (GPU-CPU synchronization)
    void WaitForFrameFence();

    /// Reset frame command buffer for new frame
    void ResetFrameCommandBuffer();

    /// Begin render pass
    void BeginRenderPass();

    /// End render pass
    void EndRenderPass();

    // Accessor methods
    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return physicalDevice_; }
    VkDevice GetDevice() const { return device_; }
    VkQueue GetGraphicsQueue() const { return graphicsQueue_; }
    VkQueue GetPresentQueue() const { return presentQueue_; }
    VkSwapchainKHR GetSwapchain() const { return swapchain_; }
    VkFormat GetSwapchainFormat() const { return swapchainFormat_; }
    VkExtent2D GetSwapchainExtent() const { return swapchainExtent_; }
    VkRenderPass GetRenderPass() const { return renderPass_; }
    VkFramebuffer GetCurrentFramebuffer() const;
    VkSurfaceKHR GetSurface() const { return surface_; }
    VmaAllocator GetAllocator() const { return allocator_; }
    VkDescriptorPool GetDescriptorPool() const { return descriptorPool_; }
    VkPipelineCache GetPipelineCache() const { return pipelineCache_; }

    /// Get or create sampler for given filter and address modes
    VkSampler GetSampler(VkFilter filter, VkSamplerAddressMode addressMode);

    /// Get or create graphics pipeline for given state
    VkPipeline GetGraphicsPipeline(const VkGraphicsPipelineCreateInfo& createInfo, uint64_t stateHash);

private:
    /// Create Vulkan instance with required extensions
    bool CreateInstance();

    /// Select physical device
    bool SelectPhysicalDevice();

    /// Create logical device with queues
    bool CreateLogicalDevice();

    /// Create window surface
    bool CreateSurface(const WindowHandle& window);

    /// Create swapchain
    bool CreateSwapchain(int width, int height);

    /// Create depth buffer image and view
    bool CreateDepthBuffer(VkFormat format, int width, int height);

    /// Create render pass
    bool CreateRenderPass();

    /// Create framebuffers for swapchain images
    bool CreateFramebuffers();

    /// Create command pool and command buffers
    bool CreateCommandBuffers();

    /// Create synchronization primitives
    bool CreateSynchronizationPrimitives();

    /// Create memory allocator
    bool CreateMemoryAllocator();

    /// Create descriptor pool
    bool CreateDescriptorPool();

    /// Create pipeline cache
    bool CreatePipelineCache();

    /// Find suitable format for surface
    VkSurfaceFormatKHR FindSurfaceFormat();

    /// Find suitable present mode for swapchain
    VkPresentModeKHR FindPresentMode();

    /// Find memory type index
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    /// Find queue family indices
    bool FindQueueFamilies();

    // Vulkan instance and device objects
    VkInstance instance_{};
    VkPhysicalDevice physicalDevice_{};
    VkDevice device_{};
    VkSurfaceKHR surface_{};

    // Queues
    VkQueue graphicsQueue_{};
    VkQueue presentQueue_{};
    uint32_t graphicsQueueFamily_{VK_QUEUE_FAMILY_IGNORED};
    uint32_t presentQueueFamily_{VK_QUEUE_FAMILY_IGNORED};

    // Swapchain
    VkSwapchainKHR swapchain_{};
    Vector<VkImage> swapchainImages_;
    Vector<VkImageView> swapchainImageViews_;
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};

    // Depth buffer
    VkImage depthImage_{};
    VkDeviceMemory depthImageMemory_{};
    VkImageView depthImageView_{};

    // Render pass and framebuffers
    VkRenderPass renderPass_{};
    Vector<VkFramebuffer> framebuffers_;

    // Command buffers (double/triple buffering)
    VkCommandPool commandPool_{};
    Vector<VkCommandBuffer> commandBuffers_;

    // Synchronization primitives
    Vector<VkFence> frameFences_;
    Vector<VkSemaphore> imageAcquiredSemaphores_;
    Vector<VkSemaphore> renderCompleteSemaphores_;

    // Descriptor management
    VkDescriptorPool descriptorPool_{};

    // Pipeline cache
    VkPipelineCache pipelineCache_{};
    HashMap<uint64_t, VkPipeline> pipelineCacheMap_;

    // Sampler cache
    HashMap<uint64_t, VkSampler> samplerCacheMap_;

    // Memory allocator (VMA)
    VmaAllocator allocator_{};

    // Frame tracking
    uint32_t frameIndex_{0};
    uint32_t currentImageIndex_{0};
    bool renderPassActive_{false};

    // Debug callback
    VkDebugUtilsMessengerEXT debugMessenger_{};
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
