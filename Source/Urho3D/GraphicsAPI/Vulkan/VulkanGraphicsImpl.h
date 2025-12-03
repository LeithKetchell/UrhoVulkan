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
#include "VulkanSamplerCache.h"
#include "VulkanShaderCache.h"
#include "VulkanConstantBufferPool.h"
#include "VulkanMemoryPoolManager.h"
#include "VulkanSecondaryCommandBuffer.h"
#include "VulkanInstanceBufferManager.h"
#include "VulkanIndirectDrawManager.h"
#include "VulkanStagingBufferManager.h"
#include "VulkanMaterialDescriptorManager.h"
#include "VulkanPipelineCache.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

// Forward declare SDL_Window in global namespace
struct SDL_Window;

namespace Urho3D
{

class ConstantBuffer;
class Graphics;
class RenderSurface;
class ShaderProgram;
class Texture2D;

/// \brief Descriptor for render pass configuration
/// \details Allows caching multiple render pass configurations for future multi-pass support.
/// Contains all the information needed to create or retrieve a cached Vulkan render pass,
/// including attachment formats, sample counts, and subpass information.
/// Supports future MSAA and deferred rendering via configurable sample counts and subpass counts.
struct RenderPassDescriptor
{
    /// Number of attachments (color, depth, etc.)
    uint32_t attachmentCount{2};

    /// Color attachment format (typically swapchain format, default: VK_FORMAT_B8G8R8A8_SRGB)
    VkFormat colorFormat{VK_FORMAT_B8G8R8A8_SRGB};

    /// Depth attachment format (default: VK_FORMAT_D32_SFLOAT for 32-bit depth)
    VkFormat depthFormat{VK_FORMAT_D32_SFLOAT};

    /// Number of subpasses (default: 1 for single-pass rendering)
    uint32_t subpassCount{1};

    /// Sample count for MSAA support (default: VK_SAMPLE_COUNT_1_BIT, extensible for Issue #14A)
    VkSampleCountFlagBits sampleCount{VK_SAMPLE_COUNT_1_BIT};

    /// \brief Calculate hash for caching
    /// \returns Hash value computed from all descriptor fields using DJB2 algorithm
    uint32_t Hash() const
    {
        // Simple hash combining all descriptor fields
        uint32_t h = 5381;
        h = ((h << 5) + h) + attachmentCount;
        h = ((h << 5) + h) + colorFormat;
        h = ((h << 5) + h) + depthFormat;
        h = ((h << 5) + h) + subpassCount;
        h = ((h << 5) + h) + sampleCount;
        return h;
    }

    /// \brief Equality comparison for caching
    /// \param other The descriptor to compare with
    /// \returns True if all fields match, false otherwise
    bool operator==(const RenderPassDescriptor& other) const
    {
        return attachmentCount == other.attachmentCount &&
               colorFormat == other.colorFormat &&
               depthFormat == other.depthFormat &&
               subpassCount == other.subpassCount &&
               sampleCount == other.sampleCount;
    }
};

/// \brief Vulkan graphics implementation
/// \details Core Vulkan backend implementing the graphics API abstraction.
/// Manages Vulkan instance, physical/logical devices, swapchain, command buffers,
/// synchronization primitives, and resource caches for efficient rendering.
/// Supports frame pipelining (triple-buffering) for optimal GPU/CPU synchronization.
///
/// **Architecture Overview:**
/// VulkanGraphicsImpl is the heart of Urho3D's Vulkan rendering backend. It encapsulates
/// all low-level Vulkan operations and presents a high-level interface to the Graphics class
/// via the dispatch pattern (Graphics::Method_Vulkan() -> VulkanGraphicsImpl::Method()).
///
/// **Key Responsibilities:**
/// - **Initialization**: Creates Vulkan instance, selects GPU, creates logical device and swapchain
/// - **Frame Management**: Acquires swapchain images, records command buffers, submits work, presents frames
/// - **Memory Management**: Uses Vulkan Memory Allocator (VMA) for GPU memory allocation
/// - **Resource Caching**: Maintains caches for samplers, shaders, pipelines, and descriptor sets
/// - **Synchronization**: Implements triple-buffering with fences and semaphores for frame pipelining
/// - **Render Targets**: Supports both swapchain rendering and render-to-texture via framebuffer management
///
/// **Frame Pipelining (Triple-Buffering):**
/// Frame 0: GPU executes while CPU prepares Frame 1 and Frame 2
/// Frame 1: GPU executes while CPU prepares Frame 2 and Frame 0
/// Frame 2: GPU executes while CPU prepares Frame 0 and Frame 1
/// This prevents CPU/GPU stalls and maximizes throughput. Uses frameIndex_ (0-2) to cycle.
///
/// **Resource Caching Strategy:**
/// - VulkanSamplerCache: Caches VkSampler objects by (filter, addressMode) pair
/// - VulkanShaderCache: Caches compiled shader modules by shader source + defines
/// - VulkanPipelineCache: Two-tier caching (memory + disk) of graphics pipelines
/// - VulkanConstantBufferPool: Allocates uniform buffers from pre-allocated pools (Phase 9)
/// - VulkanMemoryPoolManager: Manages VMA pools for different allocation patterns (Phase 8)
/// - VulkanMaterialDescriptorManager: Manages descriptor sets for material parameters (Phase 27)
///
/// **Memory Allocation:**
/// All GPU allocations go through VMA (Vulkan Memory Allocator):
/// - Buffers (vertex, index, uniform) allocated via VmaAllocator
/// - Textures created with VkImage and allocated via VMA
/// - Memory type selection optimized via FindMemoryType()
/// - Supports both GPU-local and host-visible memory for staging
///
/// **Synchronization Primitives:**
/// - frameFences_: Per-frame fences ensuring CPU doesn't overwrite frame N while GPU reads it
/// - imageAcquiredSemaphores_: Signal when swapchain image is ready for rendering
/// - renderCompleteSemaphores_: Signal when render pass completes (before presentation)
/// Frame flow: AcquireNextImage() -> WaitForFrameFence() -> RecordCommands() -> Present()
///
/// **Extensibility:**
/// - renderPassCache_: HashMap supporting future multi-pass rendering (e.g., deferred rendering)
/// - RenderPassDescriptor: Extensible descriptor for render pass configurations
/// - CurrentPipelineLayout: Tracks active layout for descriptor set binding (Phase 27)
class VulkanGraphicsImpl : public RefCounted
{
public:
    /// \brief Constructor
    /// Initializes member variables to null/default states
    VulkanGraphicsImpl();

    /// \brief Destructor
    /// Automatically calls Shutdown() to release Vulkan resources
    virtual ~VulkanGraphicsImpl();

    /// \brief Initialize Vulkan instance, device, swapchain
    /// \param graphics Pointer to Graphics object for state management
    /// \param window SDL_Window for surface creation
    /// \param width Desired swapchain width in pixels
    /// \param height Desired swapchain height in pixels
    /// \returns True if initialization successful, false on error
    /// \details Performs full Vulkan initialization in sequence:
    ///   1. Create Vulkan instance with validation layers
    ///   2. Select physical device (prefers discrete GPUs)
    ///   3. Find queue families and create logical device
    ///   4. Create window surface via SDL2
    ///   5. Create swapchain with intelligent format/mode selection
    ///   6. Create depth buffer and framebuffers
    ///   7. Initialize command buffers and synchronization primitives
    ///   8. Set up memory allocator (VMA) and caches
    bool Initialize(Graphics* graphics, SDL_Window* window, int width, int height);

    /// \brief Shutdown Vulkan resources
    /// \details Safely releases all Vulkan objects in reverse creation order.
    /// Can be called multiple times safely.
    void Shutdown();

    /// \brief Acquire next swapchain image for rendering
    /// \returns True if image acquired successfully, false on out-of-date swapchain
    /// \details Waits for image availability and updates currentImageIndex_.
    /// Handles swapchain recreation on VK_ERROR_OUT_OF_DATE_KHR.
    bool AcquireNextImage();

    /// \brief Submit command buffer and present swapchain image
    /// \details Submits recorded command buffer to graphics queue and presents
    /// the swapchain image to the display. Handles synchronization with fences and semaphores.
    void Present();

    /// \brief Get current frame index (for frame pipelining)
    /// \returns Frame index (0-2 for triple buffering)
    uint32_t GetFrameIndex() const { return frameIndex_; }

    /// \brief Get current swapchain image index
    /// \returns Index of the swapchain image currently being rendered to
    uint32_t GetCurrentImageIndex() const { return currentImageIndex_; }

    /// \brief Get frame command buffer for recording commands
    /// \returns VkCommandBuffer for the current frame (triple-buffered)
    VkCommandBuffer GetFrameCommandBuffer() const;

    /// \brief Wait for frame fence (GPU-CPU synchronization)
    /// \details Blocks CPU until the current frame's rendering is complete on GPU.
    /// This prevents overwriting a buffer that the GPU is still reading from.
    void WaitForFrameFence();

    /// \brief Reset frame command buffer for new frame
    /// \details Clears the command buffer and prepares it for new command recording.
    void ResetFrameCommandBuffer();

    /// \brief Begin render pass
    /// \details Starts a new render pass on the current frame's command buffer.
    /// Must be called before any draw commands.
    void BeginRenderPass();

    /// \brief End render pass
    /// \details Finishes the current render pass. Must be called after all draw commands.
    void EndRenderPass();

    /// \name Vulkan Object Accessors
    /// \brief Access to core Vulkan objects for direct usage
    /// @{

    /// \brief Get Vulkan instance
    /// \returns VkInstance handle
    VkInstance GetInstance() const { return instance_; }

    /// \brief Get physical device
    /// \returns VkPhysicalDevice handle (selected GPU)
    VkPhysicalDevice GetPhysicalDevice() const { return physicalDevice_; }

    /// \brief Get logical device
    /// \returns VkDevice handle for command recording and resource allocation
    VkDevice GetDevice() const { return device_; }

    /// \brief Get graphics queue
    /// \returns VkQueue for graphics operations
    VkQueue GetGraphicsQueue() const { return graphicsQueue_; }

    /// \brief Get present queue
    /// \returns VkQueue for display presentation
    VkQueue GetPresentQueue() const { return presentQueue_; }

    /// \brief Get swapchain
    /// \returns VkSwapchainKHR handle
    VkSwapchainKHR GetSwapchain() const { return swapchain_; }

    /// \brief Get swapchain image format
    /// \returns VkFormat (e.g., VK_FORMAT_B8G8R8A8_SRGB)
    VkFormat GetSwapchainFormat() const { return swapchainFormat_; }

    /// \brief Get swapchain extent (resolution)
    /// \returns VkExtent2D with width and height in pixels
    VkExtent2D GetSwapchainExtent() const { return swapchainExtent_; }

    /// \brief Get current render pass
    /// \returns VkRenderPass handle
    VkRenderPass GetRenderPass() const { return renderPass_; }

    /// \brief Get current framebuffer
    /// \returns VkFramebuffer for swapchain image
    VkFramebuffer GetCurrentFramebuffer() const;

    /// \brief Get window surface
    /// \returns VkSurfaceKHR handle
    VkSurfaceKHR GetSurface() const { return surface_; }

    /// \brief Get Vulkan Memory Allocator
    /// \returns VmaAllocator for GPU memory management
    VmaAllocator GetAllocator() const { return allocator_; }

    /// \brief Get descriptor pool
    /// \returns VkDescriptorPool for descriptor set allocation
    VkDescriptorPool GetDescriptorPool() const { return descriptorPool_; }
    /// @}

    /// \brief Get physical device properties for limits validation
    /// \returns VkPhysicalDeviceProperties including device limits and capabilities
    VkPhysicalDeviceProperties GetDeviceProperties() const { return deviceProperties_; }

    /// \brief Get sampler cache
    /// \returns VulkanSamplerCache* for texture sampler management (Quick Win #4)
    VulkanSamplerCache* GetSamplerCache() const { return samplerCache_.Get(); }

    /// \brief Get shader cache
    /// \returns VulkanShaderCache* for compiled shader caching (Quick Win #5)
    VulkanShaderCache* GetShaderCache() const { return shaderCache_.Get(); }

    /// \brief Get pipeline cache
    /// \returns VulkanPipelineCache* for compiled pipeline disk persistence (Phase B Quick Win #10)
    VulkanPipelineCache* GetPipelineCache() const { return pipelineCache_.Get(); }

    /// \brief Get constant buffer pool
    /// \returns VulkanConstantBufferPool* for efficient uniform buffer management (Quick Win #6)
    VulkanConstantBufferPool* GetConstantBufferPool() const { return constantBufferPool_.Get(); }

    /// \brief Get memory pool manager for optimized buffer allocations
    /// \returns VulkanMemoryPoolManager* for GPU buffer pooling (Quick Win #8)
    VulkanMemoryPoolManager* GetMemoryPoolManager() const { return memoryPoolManager_.Get(); }

    /// \brief Get instance buffer manager for GPU vertex stream instancing
    /// \returns VulkanInstanceBufferManager* for per-instance data streaming (Phase 12)
    VulkanInstanceBufferManager* GetInstanceBufferManager() const { return instanceBufferManager_.Get(); }

    /// \brief Get indirect draw command buffer manager
    /// \returns VulkanIndirectDrawManager* for GPU-driven rendering (Phase 12)
    VulkanIndirectDrawManager* GetIndirectDrawManager() const { return indirectDrawManager_.Get(); }

    /// \brief Get staging buffer manager for GPU uploads
    /// \returns VulkanStagingBufferManager* for asynchronous transfers (Phase 10)
    VulkanStagingBufferManager* GetStagingBufferManager() const { return stagingBufferManager_.Get(); }

    /// \brief Get or create sampler for given filter and address modes
    /// \param filter VkFilter mode (NEAREST, LINEAR)
    /// \param addressMode VkSamplerAddressMode (CLAMP, REPEAT, MIRROR)
    /// \returns Cached or newly created VkSampler
    VkSampler GetSampler(VkFilter filter, VkSamplerAddressMode addressMode);

    /// \brief Get or create graphics pipeline for given state
    /// \param createInfo VkGraphicsPipelineCreateInfo with complete pipeline configuration
    /// \param stateHash Hash of the pipeline state for caching
    /// \returns Cached or newly created VkPipeline
    VkPipeline GetGraphicsPipeline(const VkGraphicsPipelineCreateInfo& createInfo, uint64_t stateHash);

    /// \brief Get secondary command buffer pool for parallel batch recording
    /// \returns VulkanSecondaryCommandBufferPool* for multi-threaded rendering
    VulkanSecondaryCommandBufferPool* GetSecondaryCommandBufferPool() { return secondaryCommandBufferPool_.Get(); }

    /// \brief Phase 12 (Quick Win #9): Report all pool statistics for profiling
    /// \details Gathers statistics from all memory pools (constant buffers, descriptors, etc.)
    /// Enables profiling of pool utilization and optimization effectiveness
    void ReportPoolStatistics() const;

    /// \brief Get default white diffuse texture (1x1)
    /// \returns Pointer to default diffuse texture, or nullptr if not initialized
    /// Phase 22A: Default Texture Creation
    Texture2D* GetDefaultDiffuseTexture() const { return defaultDiffuseTexture_.Get(); }

    /// \brief Get default neutral normal map texture (1x1)
    /// \returns Pointer to default normal map (0.5, 0.5, 1.0), or nullptr if not initialized
    /// Phase 22A: Default Texture Creation
    Texture2D* GetDefaultNormalTexture() const { return defaultNormalTexture_.Get(); }

    /// \brief Get default white specular texture (1x1)
    /// \returns Pointer to default specular map, or nullptr if not initialized
    /// Phase 22A: Default Texture Creation
    Texture2D* GetDefaultSpecularTexture() const { return defaultSpecularTexture_.Get(); }

    /// \brief Get material descriptor manager for GPU binding
    /// \returns VulkanMaterialDescriptorManager* for material descriptor set management (Phase 27)
    class VulkanMaterialDescriptorManager* GetMaterialDescriptorManager() const { return materialDescriptorManager_.Get(); }

    /// \brief Get current pipeline layout for descriptor binding
    /// \returns VkPipelineLayout for descriptor set binding commands (Phase 27)
    VkPipelineLayout GetCurrentPipelineLayout() const { return currentPipelineLayout_; }

    /// \brief Transition image layout for texture operations
    /// \param image VkImage to transition
    /// \param format VkFormat of the image
    /// \param oldLayout Previous VkImageLayout
    /// \param newLayout Desired VkImageLayout
    /// \param mipLevels Number of mip levels to transition
    void TransitionImageLayout(VkImage image, VkFormat format,
                              VkImageLayout oldLayout, VkImageLayout newLayout,
                              uint32_t mipLevels);

    /// \brief Get framebuffer for current render targets (or swapchain if none set)
    /// \returns VkFramebuffer for render-to-texture operations
    VkFramebuffer GetCurrentFramebufferRT();

    /// \brief Rebuild framebuffer for render targets
    /// \returns True if rebuild successful, false on error
    /// \details Called when render targets change to update the framebuffer attachments
    bool RebuildRenderTargetFramebuffer();

private:
    /// \brief Create Vulkan instance with required extensions
    /// \returns True if instance created successfully, false on error
    /// \details Creates VkInstance with validation layer support (if available).
    /// Queries required platform extensions via SDL2 and loads instance-level functions.
    /// Critical first step in Vulkan initialization.
    bool CreateInstance();

    /// \brief Select physical device (GPU) for rendering
    /// \returns True if suitable device found, false if no compatible GPU available
    /// \details Enumerates all available GPUs and selects the most suitable one.
    /// Preference order: discrete GPU > integrated GPU > virtual GPU.
    /// Validates device support for graphics queue and surface presentation.
    bool SelectPhysicalDevice();

    /// \brief Create logical device and extract queue handles
    /// \returns True if device created successfully, false on error
    /// \details Creates VkDevice from selected physical device.
    /// Requests graphics and presentation queues (may be same queue family).
    /// Loads device-level Vulkan functions for command recording and resource management.
    bool CreateLogicalDevice();

    /// \brief Create window surface via SDL2
    /// \param window SDL_Window pointer from application
    /// \returns True if surface created successfully, false on error
    /// \details Creates VkSurfaceKHR for window. Platform-specific implementation
    /// handled transparently by vkCreateSurfaceKHR() and SDL2 integration.
    bool CreateSurface(SDL_Window* window);

    /// \brief Create swapchain for display presentation
    /// \param width Desired swapchain width in pixels
    /// \param height Desired swapchain height in pixels
    /// \returns True if swapchain created successfully, false on error
    /// \details Creates VkSwapchainKHR with intelligent format/mode selection.
    /// Allocates VkImage handles and creates corresponding VkImageView objects.
    /// Format chosen via FindSurfaceFormat(), present mode via FindPresentMode().
    bool CreateSwapchain(int width, int height);

    /// \brief Create depth buffer for depth testing
    /// \param format VkFormat for depth buffer (typically VK_FORMAT_D32_SFLOAT)
    /// \param width Depth buffer width in pixels (matches swapchain)
    /// \param height Depth buffer height in pixels (matches swapchain)
    /// \returns True if depth buffer created successfully, false on error
    /// \details Allocates VkImage and VkImageView for depth attachment.
    /// Memory allocated via VMA for optimal performance.
    /// Layout initialized to VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
    bool CreateDepthBuffer(VkFormat format, int width, int height);

    /// \brief Create default render pass for single-pass rendering
    /// \returns True if render pass created successfully, false on error
    /// \details Creates VkRenderPass with standard color and depth attachments.
    /// Subpass: load color, clear depth, output color. Caches in renderPassCache_.
    /// Future enhancement: GetOrCreateRenderPass() supports multi-pass rendering.
    bool CreateRenderPass();

    /// \brief Get or create render pass from descriptor (extensible for multi-pass)
    /// \param descriptor RenderPassDescriptor specifying render pass configuration
    /// \returns VkRenderPass handle if created/found, nullptr if creation fails
    /// \details Checks renderPassCache_ for existing render pass matching descriptor.
    /// If not found, creates new render pass and caches for future use.
    /// Supports MSAA sample counts and multiple subpasses for deferred rendering.
    VkRenderPass GetOrCreateRenderPass(const RenderPassDescriptor& descriptor);

    /// \brief Create framebuffers for all swapchain images
    /// \returns True if all framebuffers created successfully, false on error
    /// \details Creates VkFramebuffer for each swapchain image.
    /// Each framebuffer has color attachment (swapchain image) + depth attachment.
    /// Stored in framebuffers_ vector (indexed by swapchain image index).
    bool CreateFramebuffers();

    /// \brief Create command pool and frame command buffers
    /// \returns True if buffers created successfully, false on error
    /// \details Creates VkCommandPool and allocates triple-buffered command buffers.
    /// commandBuffers_[frameIndex_] gives current frame's command buffer.
    /// Used for recording render commands via vkCmdBindPipeline, vkCmdDraw, etc.
    bool CreateCommandBuffers();

    /// \brief Create synchronization primitives for frame pipelining
    /// \returns True if primitives created successfully, false on error
    /// \details Allocates triple-buffered fences and semaphores:
    /// - frameFences_: CPU waits for GPU completion before reusing buffer
    /// - imageAcquiredSemaphores_: GPU waits for swapchain image availability
    /// - renderCompleteSemaphores_: GPU signals render completion before present
    bool CreateSynchronizationPrimitives();

    /// \brief Create Vulkan Memory Allocator instance
    /// \returns True if allocator created successfully, false on error
    /// \details Initializes VMA with device and queue family information.
    /// Used for all GPU buffer and image allocations (via VmaAllocator).
    /// Manages memory pools for optimal allocation patterns.
    bool CreateMemoryAllocator();

    /// \brief Create descriptor pool for descriptor set allocation
    /// \returns True if pool created successfully, false on error
    /// \details Allocates VkDescriptorPool with support for multiple descriptor types:
    /// - Uniform buffers (shader parameters)
    /// - Sampled images and samplers (textures)
    /// - Storage images (compute/UAV operations)
    /// Pool size determined by expected descriptor set count.
    bool CreateDescriptorPool();

    /// \brief Create pipeline cache for persistent pipeline storage
    /// \returns True if cache created successfully, false on error
    /// \details Initializes VulkanPipelineCache with optional disk persistence.
    /// Caches compiled graphics pipelines by state hash for fast retrieval.
    /// Reduces compilation time on subsequent runs if disk cache available.
    bool CreatePipelineCache();

    /// \brief Find optimal surface format for swapchain
    /// \returns VkSurfaceFormatKHR with preferred color space and format
    /// \details Queries device surface capabilities and selects best format.
    /// Preference: VK_FORMAT_B8G8R8A8_SRGB (typical desktop) with SRGB color space.
    /// Falls back to first available format if preferred format unavailable.
    VkSurfaceFormatKHR FindSurfaceFormat();

    /// \brief Find optimal present mode for swapchain
    /// \returns VkPresentModeKHR for swapchain presentation
    /// \details Queries available present modes and selects best for latency/tearing tradeoff.
    /// Preference: VK_PRESENT_MODE_MAILBOX_KHR (triple-buffered, low latency).
    /// Falls back to VK_PRESENT_MODE_FIFO_KHR (guaranteed available, may vsync).
    VkPresentModeKHR FindPresentMode();

    /// \brief Find memory type supporting required properties
    /// \param typeFilter Bitmask of allowed memory type indices (from device limits)
    /// \param properties Required VkMemoryPropertyFlags (GPU-local, host-visible, etc.)
    /// \returns Memory type index suitable for allocation
    /// \details Called during buffer/image allocation to select optimal memory heap.
    /// Raises error if no suitable memory type available for given constraints.
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    /// \brief Find queue family indices supporting graphics and presentation
    /// \returns True if suitable queue families found, false if device incompatible
    /// \details Sets graphicsQueueFamily_ and presentQueueFamily_.
    /// These may be the same queue family or different (depends on device).
    /// All rendering operations submitted to graphics queue.
    bool FindQueueFamilies();

    // Vulkan instance and device objects
    VkInstance instance_{};
    VkPhysicalDevice physicalDevice_{};
    VkPhysicalDeviceProperties deviceProperties_{};
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

    // Render pass cache - supports future multi-pass rendering (Issue #2)
    HashMap<uint32_t, VkRenderPass> renderPassCache_;

    // Command buffers (double/triple buffering)
    VkCommandPool commandPool_{};
    Vector<VkCommandBuffer> commandBuffers_;

    // Synchronization primitives
    Vector<VkFence> frameFences_;
    Vector<VkSemaphore> imageAcquiredSemaphores_;
    Vector<VkSemaphore> renderCompleteSemaphores_;

    // Descriptor management
    VkDescriptorPool descriptorPool_{};

    // Enhanced sampler cache with expanded configurations (Quick Win #4)
    SharedPtr<VulkanSamplerCache> samplerCache_;

    // Shader compilation result cache (Quick Win #5)
    SharedPtr<VulkanShaderCache> shaderCache_;

    // Pipeline disk persistence cache (Phase B Quick Win #10)
    SharedPtr<VulkanPipelineCache> pipelineCache_;

    // Constant buffer pooling (Quick Win #6)
    SharedPtr<VulkanConstantBufferPool> constantBufferPool_;

    // Memory pool manager for optimized buffer allocations (Quick Win #8)
    SharedPtr<VulkanMemoryPoolManager> memoryPoolManager_;

    // GPU instance buffer for vertex stream instancing (Phase 12)
    SharedPtr<VulkanInstanceBufferManager> instanceBufferManager_;

    // Indirect draw command buffer manager (Phase 12)
    SharedPtr<VulkanIndirectDrawManager> indirectDrawManager_;

    // Staging buffer manager for GPU uploads (Phase 10)
    SharedPtr<VulkanStagingBufferManager> stagingBufferManager_;

    // Memory allocator (VMA)
    VmaAllocator allocator_{};

    // Frame tracking
    uint32_t frameIndex_{0};
    uint32_t currentImageIndex_{0};
    bool renderPassActive_{false};

    // Debug callback
    VkDebugUtilsMessengerEXT debugMessenger_{};

    // Secondary command buffer pool for parallel batch recording (Vulkan only)
    SharedPtr<VulkanSecondaryCommandBufferPool> secondaryCommandBufferPool_;

    // Shader parameter tracking (Phase 9)
    ShaderProgram* currentShaderProgram_{};
    Vector<ConstantBuffer*> dirtyConstantBuffers_;

    // Render target tracking (Phase 5)
    RenderSurface* renderTargets_[MAX_RENDERTARGETS]{};
    RenderSurface* depthStencil_{};
    bool renderTargetsDirty_{true};  // Flag to rebuild framebuffer when targets change
    VkFramebuffer renderTargetFramebuffer_{};  // Framebuffer for render-to-texture
    Vector<VkImageView> renderTargetViews_;    // Image views for render target attachments

    // Default placeholder textures (Phase 22A)
    /// Default 1x1 white diffuse texture for materials without diffuse maps
    SharedPtr<Texture2D> defaultDiffuseTexture_;
    /// Default 1x1 neutral normal map (0.5, 0.5, 1.0) for materials without normal maps
    SharedPtr<Texture2D> defaultNormalTexture_;
    /// Default 1x1 white specular texture for materials without specular maps
    SharedPtr<Texture2D> defaultSpecularTexture_;

    // Material descriptor management (Phase 27)
    /// Material descriptor manager for GPU binding (textures, samplers, parameters)
    SharedPtr<class VulkanMaterialDescriptorManager> materialDescriptorManager_;
    /// Current pipeline layout for descriptor set binding
    VkPipelineLayout currentPipelineLayout_{};

    // Graphics context for resource management
    Graphics* graphics_{nullptr};

    friend class Graphics;
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
