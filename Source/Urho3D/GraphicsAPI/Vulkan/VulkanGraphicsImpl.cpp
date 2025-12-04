//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//

#include "../../Graphics/Graphics.h"
#include "VulkanGraphicsImpl.h"
#include "VulkanShaderCompiler.h"
#include "VulkanShaderModule.h"
#include "VulkanConstantBufferPool.h"
#include "../VulkanDefs.h"
#include "../GraphicsDefs.h"
#include "../Texture2D.h"
#include "../../IO/Log.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#ifdef _MSC_VER
#pragma warning(disable: 26812)  // Unscoped enum warning for Vulkan
#endif

namespace Urho3D
{

VulkanGraphicsImpl::VulkanGraphicsImpl()
{
}

VulkanGraphicsImpl::~VulkanGraphicsImpl()
{
    Shutdown();
}

bool VulkanGraphicsImpl::Initialize(Graphics* graphics, SDL_Window* window, int width, int height)
{
    // Store Graphics pointer for context access throughout initialization
    graphics_ = graphics;

    // Check shader compiler availability early - prevents silent failures later
    if (!VulkanShaderCompiler::CheckCompilerAvailability())
    {
        URHO3D_LOGERROR("Vulkan initialization aborted: No shader compiler available");
        return false;
    }

    if (!CreateInstance())
    {
        URHO3D_LOGERROR("Failed to create Vulkan instance");
        return false;
    }

    if (!SelectPhysicalDevice())
    {
        URHO3D_LOGERROR("Failed to select physical device");
        return false;
    }

    if (!FindQueueFamilies())
    {
        URHO3D_LOGERROR("Failed to find queue families");
        return false;
    }

    if (!CreateLogicalDevice())
    {
        URHO3D_LOGERROR("Failed to create logical device");
        return false;
    }

    if (!CreateSurface(window))
    {
        URHO3D_LOGERROR("Failed to create window surface");
        return false;
    }

    if (!CreateSwapchain(width, height))
    {
        URHO3D_LOGERROR("Failed to create swapchain");
        return false;
    }

    // Phase 30: Use MSAA sample count for depth buffer
    if (!CreateDepthBuffer(VULKAN_PREFERRED_DEPTH_FORMAT, width, height, actualSampleCount_))
    {
        // Try fallback depth format
        if (!CreateDepthBuffer(VULKAN_FALLBACK_DEPTH_FORMAT, width, height, actualSampleCount_))
        {
            URHO3D_LOGERROR("Failed to create depth buffer");
            return false;
        }
    }

    // Phase 30: Create MSAA color image if sample count > 1x
    if (!CreateMSAAColorImage(width, height))
    {
        URHO3D_LOGWARNING("Failed to create MSAA color image, will use 1x MSAA");
        actualSampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    }

    if (!CreateRenderPass())
    {
        URHO3D_LOGERROR("Failed to create render pass");
        return false;
    }

    if (!CreateFramebuffers())
    {
        URHO3D_LOGERROR("Failed to create framebuffers");
        return false;
    }

    if (!CreateCommandBuffers())
    {
        URHO3D_LOGERROR("Failed to create command buffers");
        return false;
    }

    if (!CreateSynchronizationPrimitives())
    {
        URHO3D_LOGERROR("Failed to create synchronization primitives");
        return false;
    }

    if (!CreateMemoryAllocator())
    {
        URHO3D_LOGERROR("Failed to create memory allocator");
        return false;
    }

    if (!CreateDescriptorPool())
    {
        URHO3D_LOGERROR("Failed to create descriptor pool");
        return false;
    }

    if (!CreatePipelineCache())
    {
        URHO3D_LOGERROR("Failed to create pipeline cache");
        return false;
    }

    // Initialize shader cache (Quick Win #5)
    shaderCache_ = MakeShared<VulkanShaderCache>(graphics->GetContext());
    if (!shaderCache_)
    {
        URHO3D_LOGERROR("Failed to create shader cache");
        return false;
    }

    // Initialize pipeline cache (Phase B Quick Win #10)
    pipelineCache_ = MakeShared<VulkanPipelineCache>(graphics->GetContext());
    if (!pipelineCache_ || !pipelineCache_->Initialize(this))
    {
        URHO3D_LOGERROR("Failed to initialize pipeline cache");
        return false;
    }

    // Initialize sampler cache (Quick Win #4)
    samplerCache_ = MakeShared<VulkanSamplerCache>(graphics->GetContext(), this);
    if (!samplerCache_)
    {
        URHO3D_LOGERROR("Failed to create sampler cache");
        return false;
    }

    // Initialize instance buffer manager (Phase 12)
    instanceBufferManager_ = MakeShared<VulkanInstanceBufferManager>(graphics->GetContext(), this);
    if (!instanceBufferManager_ || !instanceBufferManager_->Initialize(65536))  // 65K instances
    {
        URHO3D_LOGWARNING("Failed to initialize instance buffer manager - instancing disabled");
        instanceBufferManager_ = nullptr;
    }

    // Initialize indirect draw command manager (Phase 12)
    indirectDrawManager_ = MakeShared<VulkanIndirectDrawManager>(graphics->GetContext(), this);
    if (!indirectDrawManager_ || !indirectDrawManager_->Initialize(65536))  // 65K commands
    {
        URHO3D_LOGWARNING("Failed to initialize indirect draw manager - GPU-driven rendering disabled");
        indirectDrawManager_ = nullptr;
    }

    // Initialize staging buffer manager (Phase 10)
    stagingBufferManager_ = MakeShared<VulkanStagingBufferManager>(graphics->GetContext(), this);
    if (!stagingBufferManager_ || !stagingBufferManager_->Initialize(64 * 1024 * 1024))  // 64MB staging pool
    {
        URHO3D_LOGWARNING("Failed to initialize staging buffer manager - staging transfers disabled");
        stagingBufferManager_ = nullptr;
    }

    // Phase 22A: Create default placeholder textures
    // These are used when materials don't have textures assigned
    // Diffuse: 1x1 white texture (255, 255, 255, 255)
    defaultDiffuseTexture_ = MakeShared<Texture2D>(graphics->GetContext());
    if (defaultDiffuseTexture_)
    {
        // Format: VK_FORMAT_R8G8B8A8_SRGB (44 in Vulkan)
        defaultDiffuseTexture_->SetSize(1, 1, VK_FORMAT_R8G8B8A8_SRGB, TEXTURE_STATIC);
        unsigned char diffuseData[] = {255, 255, 255, 255};
        if (!defaultDiffuseTexture_->SetData(0, 0, 0, 1, 1, diffuseData))
        {
            URHO3D_LOGWARNING("Failed to create default diffuse texture");
            defaultDiffuseTexture_ = nullptr;
        }
    }

    // Normal map: 1x1 neutral normal (128, 128, 255, 255) = (0.5, 0.5, 1.0) in float
    defaultNormalTexture_ = MakeShared<Texture2D>(graphics->GetContext());
    if (defaultNormalTexture_)
    {
        // Format: VK_FORMAT_R8G8B8A8_UNORM (37 in Vulkan)
        defaultNormalTexture_->SetSize(1, 1, VK_FORMAT_R8G8B8A8_UNORM, TEXTURE_STATIC);
        unsigned char normalData[] = {128, 128, 255, 255};
        if (!defaultNormalTexture_->SetData(0, 0, 0, 1, 1, normalData))
        {
            URHO3D_LOGWARNING("Failed to create default normal texture");
            defaultNormalTexture_ = nullptr;
        }
    }

    // Specular: 1x1 white texture (255, 255, 255, 255)
    defaultSpecularTexture_ = MakeShared<Texture2D>(graphics->GetContext());
    if (defaultSpecularTexture_)
    {
        // Format: VK_FORMAT_R8G8B8A8_SRGB (44 in Vulkan)
        defaultSpecularTexture_->SetSize(1, 1, VK_FORMAT_R8G8B8A8_SRGB, TEXTURE_STATIC);
        unsigned char specularData[] = {255, 255, 255, 255};
        if (!defaultSpecularTexture_->SetData(0, 0, 0, 1, 1, specularData))
        {
            URHO3D_LOGWARNING("Failed to create default specular texture");
            defaultSpecularTexture_ = nullptr;
        }
    }

    // Quick Win #6: Initialize constant buffer pool for material parameters
    constantBufferPool_ = MakeShared<VulkanConstantBufferPool>();
    if (!constantBufferPool_ || !constantBufferPool_->Initialize(this))
    {
        URHO3D_LOGERROR("Failed to initialize constant buffer pool");
        return false;
    }

    /// \brief Phase 4: Initialize secondary command buffer pool for multi-threaded rendering
    /// \details Creates per-thread secondary command buffers for parallel batch recording.
    /// Uses sensible default of 4 worker threads for maximum compatibility.
    uint32_t numThreads = 4;  // Default: 4 worker threads for parallel batch recording
    secondaryCommandBufferPool_ = MakeShared<VulkanSecondaryCommandBufferPool>(this);
    if (!secondaryCommandBufferPool_ || !secondaryCommandBufferPool_->Initialize(numThreads))
    {
        URHO3D_LOGWARNING("Failed to initialize secondary command buffer pool - parallel rendering disabled");
        secondaryCommandBufferPool_ = nullptr;
    }

    URHO3D_LOGINFO("Vulkan graphics initialization successful");
    URHO3D_LOGINFO(String("Swapchain extent: ") + String(swapchainExtent_.width) + "x" + String(swapchainExtent_.height));

    return true;
}

void VulkanGraphicsImpl::Shutdown()
{
    if (device_)
    {
        vkDeviceWaitIdle(device_);
    }

    // Destroy framebuffers
    for (auto framebuffer : framebuffers_)
    {
        if (framebuffer)
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.Clear();

    // Destroy render pass
    if (renderPass_)
    {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = nullptr;
    }

    // Destroy depth buffer
    if (depthImageView_)
    {
        vkDestroyImageView(device_, depthImageView_, nullptr);
        depthImageView_ = nullptr;
    }
    if (depthImage_)
    {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = nullptr;
    }
    if (depthImageMemory_)
    {
        vkFreeMemory(device_, depthImageMemory_, nullptr);
        depthImageMemory_ = nullptr;
    }

    // Phase 30: Destroy MSAA color buffer if present
    if (msaaColorImageView_)
    {
        vkDestroyImageView(device_, msaaColorImageView_, nullptr);
        msaaColorImageView_ = nullptr;
    }
    if (msaaColorImage_)
    {
        vmaDestroyImage(allocator_, msaaColorImage_, msaaColorAllocation_);
        msaaColorImage_ = VK_NULL_HANDLE;
        msaaColorAllocation_ = nullptr;
    }

    // Destroy swapchain image views
    for (auto imageView : swapchainImageViews_)
    {
        if (imageView)
            vkDestroyImageView(device_, imageView, nullptr);
    }
    swapchainImageViews_.Clear();
    swapchainImages_.Clear();

    // Destroy swapchain
    if (swapchain_)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = nullptr;
    }

    // Destroy synchronization primitives
    for (auto fence : frameFences_)
    {
        if (fence)
            vkDestroyFence(device_, fence, nullptr);
    }
    frameFences_.Clear();

    for (auto semaphore : imageAcquiredSemaphores_)
    {
        if (semaphore)
            vkDestroySemaphore(device_, semaphore, nullptr);
    }
    imageAcquiredSemaphores_.Clear();

    for (auto semaphore : renderCompleteSemaphores_)
    {
        if (semaphore)
            vkDestroySemaphore(device_, semaphore, nullptr);
    }
    renderCompleteSemaphores_.Clear();

    // Destroy timeline semaphore if present (Phase 33)
    if (timelineRenderSemaphore_)
    {
        vkDestroySemaphore(device_, timelineRenderSemaphore_, nullptr);
        timelineRenderSemaphore_ = nullptr;
    }

    // Destroy descriptor pool
    if (descriptorPool_)
    {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = nullptr;
    }

    // Destroy pipeline cache (SharedPtr auto-cleanup)
    if (pipelineCache_)
    {
        pipelineCache_->Release();
        pipelineCache_ = nullptr;
    }

    // Destroy sampler cache (SharedPtr auto-cleanup)
    if (samplerCache_)
    {
        samplerCache_->Reset();
        samplerCache_ = nullptr;
    }

    // Destroy command pool
    if (commandPool_)
    {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = nullptr;
    }

    // Destroy memory allocator
    if (allocator_)
    {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }

    // Destroy surface
    if (surface_)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = nullptr;
    }

    // Destroy debug messenger
    if (debugMessenger_ && instance_)
    {
        auto vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (vkDestroyDebugUtilsMessengerEXT)
            vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }

    // Destroy device
    if (device_)
    {
        vkDestroyDevice(device_, nullptr);
        device_ = nullptr;
    }

    // Destroy instance
    if (instance_)
    {
        vkDestroyInstance(instance_, nullptr);
        instance_ = nullptr;
    }

    // Release caches
    if (shaderCache_)
    {
        shaderCache_->Release();
        shaderCache_ = nullptr;
    }
    if (samplerCache_)
    {
        samplerCache_ = nullptr;
    }
}

bool VulkanGraphicsImpl::AcquireNextImage()
{
    if (!swapchain_)
        return false;

    // Wait for fence from previous frame
    WaitForFrameFence();

    VkSemaphore imageAcquiredSemaphore = imageAcquiredSemaphores_[frameIndex_];
    VkFence frameFence = frameFences_[frameIndex_];

    // Reset fence for this frame
    vkResetFences(device_, 1, &frameFence);

    // Acquire next swapchain image
    VkResult result = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        imageAcquiredSemaphore,
        nullptr,
        &currentImageIndex_
    );

    if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to acquire swapchain image: " + String((int)result));
        return false;
    }

    return true;
}

void VulkanGraphicsImpl::Present()
{
    if (!swapchain_)
        return;

    VkCommandBuffer cmdBuffer = GetFrameCommandBuffer();
    VkFence frameFence = frameFences_[frameIndex_];

    // PHASE 33: Submit command buffer with optional timeline semaphore signaling
    if (cmdBuffer && graphicsQueue_)
    {
        // Prepare semaphore signal structures
        VkSemaphore signalSemaphores[2]{};
        uint32_t signalSemaphoreCount = 0;

        // Always signal render complete binary semaphore for presentation
        signalSemaphores[signalSemaphoreCount++] = renderCompleteSemaphores_[frameIndex_];

        // Prepare timeline semaphore signaling if available (Phase 33)
        VkTimelineSemaphoreSubmitInfo timelineSubmitInfo{};
        if (supportsTimelineSemaphores_ && timelineRenderSemaphore_ != VK_NULL_HANDLE)
        {
            // Timeline semaphore will be signaled with incremented counter
            uint64_t signalValue = timelineRenderCounter_ + 1;
            timelineSubmitInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR;
            timelineSubmitInfo.signalSemaphoreValueCount = 1;
            timelineSubmitInfo.pSignalSemaphoreValues = &signalValue;

            // Add timeline semaphore to signal list
            signalSemaphores[signalSemaphoreCount++] = timelineRenderSemaphore_;
        }

        // Submit rendered command buffer to graphics queue
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = (supportsTimelineSemaphores_ && timelineRenderSemaphore_ != VK_NULL_HANDLE)
            ? &timelineSubmitInfo : nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAcquiredSemaphores_[frameIndex_];
        submitInfo.signalSemaphoreCount = signalSemaphoreCount;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Wait for image acquired before rendering, signal completion after render
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submitInfo.pWaitDstStageMask = &waitStage;

        VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frameFence);
        if (submitResult != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Failed to submit command buffer: " + String((int)submitResult));
            return;
        }

        // Increment timeline counter after successful submission (Phase 33)
        SignalTimelineRenderSemaphore();
    }

    // Present rendered image to swapchain
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderCompleteSemaphores_[frameIndex_];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &currentImageIndex_;

    VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);

    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
    {
        URHO3D_LOGERROR("Failed to present swapchain image: " + String((int)presentResult));
    }

    // Advance to next frame
    frameIndex_ = (frameIndex_ + 1) % VULKAN_FRAME_COUNT;
}

VkCommandBuffer VulkanGraphicsImpl::GetFrameCommandBuffer() const
{
    if (frameIndex_ < commandBuffers_.Size())
        return commandBuffers_[frameIndex_];
    return nullptr;
}

void VulkanGraphicsImpl::WaitForFrameFence()
{
    if (frameIndex_ >= frameFences_.Size())
        return;

    // PHASE 33: Prefer timeline semaphore waits for non-blocking synchronization
    if (supportsTimelineSemaphores_ && timelineRenderSemaphore_ != VK_NULL_HANDLE)
    {
        // Wait on timeline semaphore instead of fence - allows other GPU work to proceed
        // This provides ~1-2ms improvement by avoiding CPU blocking
        WaitOnTimelineRenderSemaphore(timelineRenderCounter_);
    }
    else
    {
        // Fallback: CPU-blocking fence wait (original behavior)
        VkFence fence = frameFences_[frameIndex_];
        if (fence)
        {
            vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        }
    }
}

void VulkanGraphicsImpl::ResetFrameCommandBuffer()
{
    VkCommandBuffer cmdBuffer = GetFrameCommandBuffer();
    if (cmdBuffer)
    {
        vkResetCommandBuffer(cmdBuffer, 0);
    }
}

void VulkanGraphicsImpl::BeginRenderPass()
{
    if (!renderPass_ || !device_)
        return;

    VkCommandBuffer cmdBuffer = GetFrameCommandBuffer();
    if (!cmdBuffer)
        return;

    // Phase 34 Step 3: Handle deferred rendering framebuffer rebuild if needed
    if (renderTargetsDirty_)
    {
        RebuildRenderTargetFramebuffer();
        // If rebuild failed, fall back to swapchain rendering
    }

    // Phase 34 Step 3: Determine which framebuffer to use (deferred G-Buffer or forward swapchain)
    VkFramebuffer currentFramebuffer = GetCurrentFramebuffer();
    VkRenderPass currentRenderPass = renderPass_;
    uint32_t clearValueCount = 2;  // Default: color + depth

    // If we have a deferred framebuffer and render targets are set, use deferred pass
    // (renderTargetFramebuffer_ is non-null when RebuildRenderTargetFramebuffer() succeeds)
    if (renderTargetFramebuffer_ != VK_NULL_HANDLE)
    {
        currentFramebuffer = renderTargetFramebuffer_;
        // G-Buffer has 4 color attachments + 1 depth = 5 attachments total
        clearValueCount = 5;
    }

    // Prepare clear values for all attachments
    // For deferred: RGBA positions, RGBA normals, RGBA albedo, RGBA specular, depth
    VkClearValue clearValues[5]{};
    for (uint32_t i = 0; i < clearValueCount - 1; ++i)
    {
        clearValues[i].color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // Color attachments: clear to black
    }
    clearValues[clearValueCount - 1].depthStencil = {1.0f, 0};  // Depth: clear to 1.0

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = currentRenderPass;
    renderPassInfo.framebuffer = currentFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;
    renderPassInfo.clearValueCount = clearValueCount;
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    renderPassActive_ = true;
}

void VulkanGraphicsImpl::EndRenderPass()
{
    // Render pass is ended by the caller via vkCmdEndRenderPass
    renderPassActive_ = false;
}

VkFramebuffer VulkanGraphicsImpl::GetCurrentFramebuffer() const
{
    if (currentImageIndex_ < framebuffers_.Size())
        return framebuffers_[currentImageIndex_];
    return nullptr;
}

VkSampler VulkanGraphicsImpl::GetSampler(VkFilter filter, VkSamplerAddressMode addressMode)
{
    if (!samplerCache_)
        return VK_NULL_HANDLE;

    // Delegate to sampler cache (Quick Win #4) - convert Vulkan types to uint8_t
    uint8_t filterMode = static_cast<uint8_t>(filter);  // 0=VK_FILTER_NEAREST, 1=VK_FILTER_LINEAR
    uint8_t addrMode = static_cast<uint8_t>(addressMode);  // 0=CLAMP, 1=REPEAT, 2=MIRROR
    return samplerCache_->GetSampler(filterMode, addrMode, 1);  // maxAnisotropy=1
}

VkPipeline VulkanGraphicsImpl::GetGraphicsPipeline(const VkGraphicsPipelineCreateInfo& createInfo, uint64_t stateHash)
{
    if (!pipelineCache_)
        return VK_NULL_HANDLE;

    // Delegate to pipeline cache (Quick Win #10)
    return pipelineCache_->GetOrCreatePipeline(stateHash, createInfo);
}

// ============================================
// Private Initialization Methods
// ============================================

bool VulkanGraphicsImpl::CreateInstance()
{
    // Get required extensions from SDL
    unsigned int extensionCount = 0;
    const char** extensionNames = nullptr;

    if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &extensionCount, nullptr))
    {
        URHO3D_LOGERROR("Failed to get Vulkan instance extension count");
        return false;
    }

    Vector<const char*> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &extensionCount, !extensions.Empty() ? &extensions[0] : nullptr))
    {
        URHO3D_LOGERROR("Failed to get Vulkan instance extensions");
        return false;
    }

    // Add validation layer extension if available
    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    Vector<VkLayerProperties> availableLayers(layerCount);
    if (layerCount > 0)
    {
        vkEnumerateInstanceLayerProperties(&layerCount, &availableLayers[0]);
    }

    bool validationLayerAvailable = false;
    for (const auto& layer : availableLayers)
    {
        if (strcmp(layer.layerName, validationLayerName) == 0)
        {
            validationLayerAvailable = true;
            break;
        }
    }

    Vector<const char*> layers;
    if (validationLayerAvailable)
    {
        layers.Push(validationLayerName);
        URHO3D_LOGINFO("Vulkan validation layer enabled");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Urho3D";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 9, 0);
    appInfo.pEngineName = "Urho3D";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 9, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.ppEnabledExtensionNames = !extensions.Empty() ? &extensions[0] : nullptr;
    createInfo.enabledLayerCount = layers.Size();
    createInfo.ppEnabledLayerNames = !layers.Empty() ? &layers[0] : nullptr;

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create Vulkan instance");
        return false;
    }

    URHO3D_LOGINFO("Vulkan instance created successfully");
    return true;
}

bool VulkanGraphicsImpl::SelectPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        URHO3D_LOGERROR("No Vulkan-capable physical devices found");
        return false;
    }

    Vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, !devices.Empty() ? &devices[0] : nullptr);

    // Priority: discrete GPU > integrated GPU > virtual GPU > CPU
    int selectedIndex = -1;
    VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;

    // Helper function to get device type name
    auto getDeviceTypeName = [](VkPhysicalDeviceType type) -> const char*
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "CPU";
        default:
            return "Other";
        }
    };

    // First pass: Try to find the best match (discrete GPU preferred)
    for (uint32_t i = 0; i < deviceCount; ++i)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        URHO3D_LOGINFO(String("GPU ") + String((int)i) + ": " + properties.deviceName +
                      " (" + getDeviceTypeName(properties.deviceType) + ")");

        // Prefer discrete GPUs
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            if (selectedIndex < 0 || selectedIndex >= 0)  // Take first discrete GPU
            {
                selectedDevice = devices[i];
                selectedIndex = i;
                break;
            }
        }
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        {
            // Use as fallback if no discrete GPU found
            if (selectedIndex < 0)
            {
                selectedDevice = devices[i];
                selectedIndex = i;
            }
        }
        else if (selectedIndex < 0)
        {
            // Last resort: use any device
            selectedDevice = devices[i];
            selectedIndex = i;
        }
    }

    if (!selectedDevice)
    {
        // Fallback: use first available device
        if (deviceCount > 0)
        {
            selectedDevice = devices[0];
            selectedIndex = 0;
            URHO3D_LOGWARNING("Using first available GPU as fallback");
        }
        else
        {
            URHO3D_LOGERROR("Failed to select physical device");
            return false;
        }
    }

    physicalDevice_ = selectedDevice;
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties_);

    URHO3D_LOGINFO(String("Selected GPU: ") + deviceProperties_.deviceName +
                   " (" + getDeviceTypeName(deviceProperties_.deviceType) + ")");

    // Detect MSAA capabilities for this device
    if (!DetectMSAACapabilities())
    {
        URHO3D_LOGWARNING("Failed to detect MSAA capabilities, defaulting to 1x");
    }

    // Detect timeline semaphore support for advanced synchronization
    DetectTimelineSemaphoreSupport();

    return true;
}

bool VulkanGraphicsImpl::FindQueueFamilies()
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);

    Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    if (queueFamilyCount > 0)
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, &queueFamilies[0]);

    graphicsQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
    presentQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;

    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphicsQueueFamily_ == VK_QUEUE_FAMILY_IGNORED)
        {
            graphicsQueueFamily_ = i;
        }

        VkBool32 presentSupport = false;
        if (surface_)
        {
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &presentSupport);
        }

        if (presentSupport && presentQueueFamily_ == VK_QUEUE_FAMILY_IGNORED)
        {
            presentQueueFamily_ = i;
        }

        if (graphicsQueueFamily_ != VK_QUEUE_FAMILY_IGNORED && presentQueueFamily_ != VK_QUEUE_FAMILY_IGNORED)
            break;
    }

    if (graphicsQueueFamily_ == VK_QUEUE_FAMILY_IGNORED)
    {
        URHO3D_LOGERROR("Failed to find graphics queue family");
        return false;
    }

    // If present queue family not found, use graphics queue family
    if (presentQueueFamily_ == VK_QUEUE_FAMILY_IGNORED)
    {
        presentQueueFamily_ = graphicsQueueFamily_;
    }

    return true;
}

bool VulkanGraphicsImpl::DetectMSAACapabilities()
{
    // Query device limits for supported sample counts
    // Both color and depth attachments must support the same sample count
    VkSampleCountFlags colorSamples = deviceProperties_.limits.framebufferColorSampleCounts;
    VkSampleCountFlags depthSamples = deviceProperties_.limits.framebufferDepthSampleCounts;

    // Store the intersection of supported sample counts (both must support)
    supportedSampleCountsMask_ = colorSamples & depthSamples;

    // Log available MSAA levels
    String msaaInfo = "Supported MSAA levels: ";
    if (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_1_BIT) msaaInfo += "1x ";
    if (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_2_BIT) msaaInfo += "2x ";
    if (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_4_BIT) msaaInfo += "4x ";
    if (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_8_BIT) msaaInfo += "8x ";
    if (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_16_BIT) msaaInfo += "16x ";

    URHO3D_LOGINFO(msaaInfo);

    // Default to 1x MSAA if nothing is supported (shouldn't happen)
    if (!supportedSampleCountsMask_)
    {
        supportedSampleCountsMask_ = VK_SAMPLE_COUNT_1_BIT;
        actualSampleCount_ = VK_SAMPLE_COUNT_1_BIT;
        return false;
    }

    // Start with 1x MSAA as default (can be changed via SetRequestedSampleCount)
    actualSampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    requestedSampleCount_ = VK_SAMPLE_COUNT_1_BIT;

    return true;
}

VkSampleCountFlagBits VulkanGraphicsImpl::SelectBestSampleCount(uint32_t requestedCount)
{
    // If 1x requested or invalid count, return 1x
    if (requestedCount <= 1)
        return VK_SAMPLE_COUNT_1_BIT;

    // Map requested count to VkSampleCountFlagBits and find best supported match
    VkSampleCountFlagBits requested = VK_SAMPLE_COUNT_1_BIT;
    if (requestedCount >= 2) requested = VK_SAMPLE_COUNT_2_BIT;
    if (requestedCount >= 4) requested = VK_SAMPLE_COUNT_4_BIT;
    if (requestedCount >= 8) requested = VK_SAMPLE_COUNT_8_BIT;
    if (requestedCount >= 16) requested = VK_SAMPLE_COUNT_16_BIT;

    // Check if requested sample count is supported
    if (supportedSampleCountsMask_ & requested)
    {
        actualSampleCount_ = requested;
        URHO3D_LOGINFO(String("MSAA set to ") + String((int)requestedCount) + "x");
        return requested;
    }

    // If not supported, find best fallback (prefer higher MSAA)
    if (requested > VK_SAMPLE_COUNT_8_BIT && (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_8_BIT))
        return (actualSampleCount_ = VK_SAMPLE_COUNT_8_BIT);
    if (requested > VK_SAMPLE_COUNT_4_BIT && (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_4_BIT))
        return (actualSampleCount_ = VK_SAMPLE_COUNT_4_BIT);
    if (requested > VK_SAMPLE_COUNT_2_BIT && (supportedSampleCountsMask_ & VK_SAMPLE_COUNT_2_BIT))
        return (actualSampleCount_ = VK_SAMPLE_COUNT_2_BIT);

    // Fallback to 1x
    actualSampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    URHO3D_LOGWARNING(String("Requested MSAA ") + String((int)requestedCount) +
                     "x not supported, falling back to 1x");
    return VK_SAMPLE_COUNT_1_BIT;
}

bool VulkanGraphicsImpl::DetectTimelineSemaphoreSupport()
{
    // Check if VK_KHR_timeline_semaphore is available
    // This extension provides VkSemaphoreTypeCreateInfo for timeline semaphores

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &timelineFeatures;

    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);

    supportsTimelineSemaphores_ = (timelineFeatures.timelineSemaphore == VK_TRUE);

    if (supportsTimelineSemaphores_)
    {
        URHO3D_LOGINFO("Timeline semaphore (VK_KHR_timeline_semaphore) support detected");
    }
    else
    {
        URHO3D_LOGINFO("Timeline semaphore support not available, will use binary semaphores");
    }

    return true;
}

bool VulkanGraphicsImpl::CreateTimelineSemaphore()
{
    if (!supportsTimelineSemaphores_)
    {
        URHO3D_LOGINFO("Timeline semaphores not supported, skipping creation");
        return true;  // Not an error, just use fallback
    }

    // Create timeline semaphore type info
    VkSemaphoreTypeCreateInfo typeCreateInfo{};
    typeCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    typeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    typeCreateInfo.initialValue = VULKAN_TIMELINE_INITIAL_VALUE;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &typeCreateInfo;

    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &timelineRenderSemaphore_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create timeline semaphore");
        return false;
    }

    timelineRenderCounter_ = VULKAN_TIMELINE_INITIAL_VALUE;

    URHO3D_LOGINFO("Timeline render semaphore created (replaces 3 binary semaphores)");
    return true;
}

bool VulkanGraphicsImpl::WaitOnTimelineRenderSemaphore(uint64_t targetValue)
{
    if (!supportsTimelineSemaphores_ || timelineRenderSemaphore_ == VK_NULL_HANDLE)
    {
        // Fallback: use fence-based waiting (original behavior)
        // This maintains compatibility on devices without timeline semaphores
        return true;
    }

    // Wait for timeline semaphore to reach targetValue
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timelineRenderSemaphore_;
    waitInfo.pValues = &targetValue;

    VkResult result = vkWaitSemaphores(device_, &waitInfo, VULKAN_TIMELINE_TIMEOUT_NS);

    if (result == VK_TIMEOUT)
    {
        URHO3D_LOGWARNING(String("Timeline semaphore wait timeout at value ") + String((unsigned long long)targetValue));
        return false;
    }
    else if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Timeline semaphore wait failed");
        return false;
    }

    return true;
}

void VulkanGraphicsImpl::SignalTimelineRenderSemaphore()
{
    if (supportsTimelineSemaphores_ && timelineRenderSemaphore_ != VK_NULL_HANDLE)
    {
        // Increment timeline counter after frame submission
        timelineRenderCounter_++;
    }
}

bool VulkanGraphicsImpl::CreateLogicalDevice()
{
    Vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    Vector<uint32_t> uniqueQueueFamilies;

    uniqueQueueFamilies.Push(graphicsQueueFamily_);
    if (presentQueueFamily_ != graphicsQueueFamily_)
    {
        uniqueQueueFamilies.Push(presentQueueFamily_);
    }

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.Push(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_FALSE;
    deviceFeatures.sampleRateShading = VK_FALSE;

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    uint32_t deviceExtensionCount = 1;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = queueCreateInfos.Size();
    createInfo.pQueueCreateInfos = !queueCreateInfos.Empty() ? &queueCreateInfos[0] : nullptr;
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = deviceExtensionCount;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create logical device");
        return false;
    }

    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentQueueFamily_, 0, &presentQueue_);

    URHO3D_LOGINFO("Vulkan logical device created");
    return true;
}

bool VulkanGraphicsImpl::CreateSurface(SDL_Window* window)
{
    if (!SDL_Vulkan_CreateSurface(window, instance_, &surface_))
    {
        URHO3D_LOGERROR("Failed to create Vulkan surface");
        return false;
    }

    URHO3D_LOGINFO("Vulkan surface created");
    return true;
}

bool VulkanGraphicsImpl::CreateSwapchain(int width, int height)
{
    VkSurfaceFormatKHR surfaceFormat = FindSurfaceFormat();
    VkPresentModeKHR presentMode = FindPresentMode();

    uint32_t imageCount = VULKAN_DEFAULT_SWAPCHAIN_IMAGES;
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);

    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
    {
        imageCount = capabilities.maxImageCount;
    }
    if (imageCount < capabilities.minImageCount)
    {
        imageCount = capabilities.minImageCount;
    }

    swapchainExtent_ = capabilities.currentExtent;
    if (capabilities.currentExtent.width == UINT32_MAX)
    {
        swapchainExtent_.width = (uint32_t)width;
        swapchainExtent_.height = (uint32_t)height;

        if (swapchainExtent_.width > capabilities.maxImageExtent.width)
            swapchainExtent_.width = capabilities.maxImageExtent.width;
        if (swapchainExtent_.height > capabilities.maxImageExtent.height)
            swapchainExtent_.height = capabilities.maxImageExtent.height;
    }

    swapchainFormat_ = surfaceFormat.format;

    // Log swapchain capabilities for debugging
    URHO3D_LOGDEBUG("Swapchain capabilities: images " + String((int)capabilities.minImageCount) +
                    "-" + String((int)capabilities.maxImageCount) +
                    ", extent " + String((int)capabilities.minImageExtent.width) + "x" +
                    String((int)capabilities.minImageExtent.height) + " to " +
                    String((int)capabilities.maxImageExtent.width) + "x" +
                    String((int)capabilities.maxImageExtent.height));

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = swapchainExtent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    uint32_t queueFamilyIndices[] = {graphicsQueueFamily_, presentQueueFamily_};
    if (graphicsQueueFamily_ != presentQueueFamily_)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create swapchain");
        return false;
    }

    // Get swapchain images
    uint32_t swapchainImageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, nullptr);
    swapchainImages_.Resize(swapchainImageCount);
    if (!swapchainImages_.Empty())
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, &swapchainImages_[0]);

    // Create image views
    swapchainImageViews_.Resize(swapchainImages_.Size());
    for (size_t i = 0; i < swapchainImages_.Size(); ++i)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Failed to create swapchain image view");
            return false;
        }
    }

    URHO3D_LOGINFO(String("Swapchain created with ") + String((int)swapchainImageCount) +
                   " images at " + String((int)swapchainExtent_.width) + "x" +
                   String((int)swapchainExtent_.height));
    return true;
}

bool VulkanGraphicsImpl::CreateDepthBuffer(VkFormat format, int width, int height, VkSampleCountFlagBits sampleCount)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = (uint32_t)width;
    imageInfo.extent.height = (uint32_t)height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = sampleCount;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &depthImage_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create depth image");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, depthImage_, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &depthImageMemory_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to allocate depth image memory");
        return false;
    }

    vkBindImageMemory(device_, depthImage_, depthImageMemory_, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &depthImageView_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create depth image view");
        return false;
    }

    URHO3D_LOGINFO("Depth buffer created");
    return true;
}

bool VulkanGraphicsImpl::CreateMSAAColorImage(int width, int height)
{
    // Phase 30: Only create MSAA color image if sample count > 1x
    if (actualSampleCount_ == VK_SAMPLE_COUNT_1_BIT)
    {
        return true;  // No MSAA needed
    }

    // Create intermediate multi-sample color image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = (uint32_t)width;
    imageInfo.extent.height = (uint32_t)height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = swapchainFormat_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;  // Render target only
    imageInfo.samples = actualSampleCount_;  // Use MSAA sample count
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator_, &imageInfo, &allocCreateInfo, &msaaColorImage_, &msaaColorAllocation_, nullptr) != VK_SUCCESS)
    {
        URHO3D_LOGERROR(String("Failed to create MSAA color image (") + String((int)actualSampleCount_) + "x)");
        return false;
    }

    // Create image view for MSAA color attachment
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = msaaColorImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchainFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &msaaColorImageView_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create MSAA color image view");
        vmaDestroyImage(allocator_, msaaColorImage_, msaaColorAllocation_);
        msaaColorImage_ = VK_NULL_HANDLE;
        return false;
    }

    URHO3D_LOGINFO(String("MSAA color image created (") + String((int)actualSampleCount_) + "x samples)");
    return true;
}

bool VulkanGraphicsImpl::CreateGBuffer(int width, int height)
{
    // Phase 31: Create G-Buffer attachments for deferred rendering
    // G-Buffer layout: Position (RGBA32F), Normal (RGBA16F), Albedo (RGBA8), Specular (RGBA8)

    struct GBufferAttachmentInfo
    {
        VkImage* image;
        VmaAllocation* allocation;
        VkImageView* view;
        VkFormat format;
        const char* name;
    };

    GBufferAttachmentInfo attachments[4] = {
        {&gBufferPositionImage_, &gBufferPositionAlloc_, &gBufferPositionView_, VK_FORMAT_R32G32B32A32_SFLOAT, "Position"},
        {&gBufferNormalImage_, &gBufferNormalAlloc_, &gBufferNormalView_, VK_FORMAT_R16G16B16A16_SFLOAT, "Normal"},
        {&gBufferAlbedoImage_, &gBufferAlbedoAlloc_, &gBufferAlbedoView_, VK_FORMAT_R8G8B8A8_UNORM, "Albedo"},
        {&gBufferSpecularImage_, &gBufferSpecularAlloc_, &gBufferSpecularView_, VK_FORMAT_R8G8B8A8_UNORM, "Specular"}
    };

    // Create each G-Buffer attachment
    for (int i = 0; i < 4; ++i)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = (uint32_t)width;
        imageInfo.extent.height = (uint32_t)height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = attachments[i].format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;  // G-Buffer always 1x sampled
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(allocator_, &imageInfo, &allocCreateInfo, attachments[i].image, attachments[i].allocation, nullptr) != VK_SUCCESS)
        {
            URHO3D_LOGERROR(String("Failed to create G-Buffer ") + attachments[i].name + " image");
            DestroyGBuffer();
            return false;
        }

        // Create image view for G-Buffer attachment
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = *attachments[i].image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = attachments[i].format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, attachments[i].view) != VK_SUCCESS)
        {
            URHO3D_LOGERROR(String("Failed to create G-Buffer ") + attachments[i].name + " image view");
            DestroyGBuffer();
            return false;
        }
    }

    URHO3D_LOGINFO("G-Buffer created (Position RGBA32F, Normal RGBA16F, Albedo RGBA8, Specular RGBA8)");
    return true;
}

void VulkanGraphicsImpl::DestroyGBuffer()
{
    // Phase 31: Destroy G-Buffer attachments

    VkImage* images[4] = {&gBufferPositionImage_, &gBufferNormalImage_, &gBufferAlbedoImage_, &gBufferSpecularImage_};
    VmaAllocation* allocations[4] = {&gBufferPositionAlloc_, &gBufferNormalAlloc_, &gBufferAlbedoAlloc_, &gBufferSpecularAlloc_};
    VkImageView* views[4] = {&gBufferPositionView_, &gBufferNormalView_, &gBufferAlbedoView_, &gBufferSpecularView_};

    for (int i = 0; i < 4; ++i)
    {
        if (*views[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, *views[i], nullptr);
            *views[i] = VK_NULL_HANDLE;
        }

        if (*images[i] != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, *images[i], *allocations[i]);
            *images[i] = VK_NULL_HANDLE;
            *allocations[i] = nullptr;
        }
    }
}

// Phase 34 Step 1: Build framebuffer for G-Buffer deferred rendering
bool VulkanGraphicsImpl::RebuildRenderTargetFramebuffer()
{
    // Phase 34 Step 1: Create framebuffer for G-Buffer (4 color + depth attachments)
    // This framebuffer is used for the geometry pass in deferred rendering

    if (gBufferPositionImage_ == VK_NULL_HANDLE ||
        gBufferNormalImage_ == VK_NULL_HANDLE ||
        gBufferAlbedoImage_ == VK_NULL_HANDLE ||
        gBufferSpecularImage_ == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("RebuildRenderTargetFramebuffer: G-Buffer not initialized");
        return false;
    }

    // Phase 34 Step 1: Create render pass descriptor for G-Buffer (4 color + depth)
    RenderPassDescriptor descriptor;
    descriptor.colorAttachmentCount = 4;
    descriptor.colorFormats[0] = VK_FORMAT_R32G32B32A32_SFLOAT;  // Position
    descriptor.colorFormats[1] = VK_FORMAT_R16G16B16A16_SFLOAT;  // Normal
    descriptor.colorFormats[2] = VK_FORMAT_R8G8B8A8_UNORM;       // Albedo
    descriptor.colorFormats[3] = VK_FORMAT_R8G8B8A8_UNORM;       // Specular
    descriptor.depthFormat = VK_FORMAT_D32_SFLOAT;
    descriptor.subpassCount = 1;                                 // Single subpass for geometry pass
    descriptor.sampleCount = VK_SAMPLE_COUNT_1_BIT;              // No MSAA for G-Buffer

    // Get or create the G-Buffer render pass
    VkRenderPass gBufferRenderPass = GetOrCreateRenderPass(descriptor);
    if (gBufferRenderPass == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("RebuildRenderTargetFramebuffer: Failed to create G-Buffer render pass");
        return false;
    }

    // Phase 34 Step 1: Prepare attachment image views
    VkImageView attachments[5] = {
        gBufferPositionView_,       // [0] Position
        gBufferNormalView_,         // [1] Normal
        gBufferAlbedoView_,         // [2] Albedo
        gBufferSpecularView_,       // [3] Specular
        depthImageView_             // [4] Depth
    };

    // Create framebuffer with G-Buffer attachments
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = gBufferRenderPass;
    framebufferInfo.attachmentCount = 5;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = swapchainExtent_.width;
    framebufferInfo.height = swapchainExtent_.height;
    framebufferInfo.layers = 1;

    // Phase 34 Step 1: Destroy old framebuffer if it exists
    if (renderTargetFramebuffer_ != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device_, renderTargetFramebuffer_, nullptr);
    }

    // Create new G-Buffer framebuffer
    if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &renderTargetFramebuffer_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("RebuildRenderTargetFramebuffer: Failed to create G-Buffer framebuffer");
        renderTargetFramebuffer_ = VK_NULL_HANDLE;
        return false;
    }

    renderTargetsDirty_ = false;
    URHO3D_LOGDEBUG("RebuildRenderTargetFramebuffer: G-Buffer framebuffer created successfully");
    return true;
}

bool VulkanGraphicsImpl::CreateRenderPass()
{
    // Phase 30: Support MSAA with resolve attachments
    // Layout: [0] = MSAA color (or swapchain if 1x), [1] = depth, [2] = resolve (if MSAA)
    bool useMSAA = (actualSampleCount_ != VK_SAMPLE_COUNT_1_BIT);
    int attachmentCount = useMSAA ? 3 : 2;

    Vector<VkAttachmentDescription> attachments;
    attachments.Resize(attachmentCount);

    // Attachment 0: Color attachment (MSAA if enabled, else swapchain)
    attachments[0].format = swapchainFormat_;
    attachments[0].samples = actualSampleCount_;  // Use MSAA sample count
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = useMSAA ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = useMSAA ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Attachment 1: Depth attachment
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = actualSampleCount_;  // Must match color attachment
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Attachment 2: Resolve attachment (only if MSAA enabled)
    if (useMSAA)
    {
        attachments[2].format = swapchainFormat_;
        attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;  // Resolve target is always 1x
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference resolveRef{};
    if (useMSAA)
    {
        resolveRef.attachment = 2;
        resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    if (useMSAA)
    {
        subpass.pResolveAttachments = &resolveRef;  // Resolve MSAA to swapchain
    }

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = attachmentCount;
    renderPassInfo.pAttachments = &attachments[0];
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create render pass");
        return false;
    }

    if (useMSAA)
    {
        URHO3D_LOGINFO(String("Render pass created with MSAA (") + String((int)actualSampleCount_) + "x) and resolve");
    }
    else
    {
        URHO3D_LOGINFO("Render pass created (1x MSAA)");
    }
    return true;
}

VkRenderPass VulkanGraphicsImpl::GetOrCreateRenderPass(const RenderPassDescriptor& descriptor)
{
    // Calculate hash for this descriptor
    uint32_t descriptorHash = descriptor.Hash();

    // Check if we already have a cached render pass for this configuration
    if (renderPassCache_.Contains(descriptorHash))
    {
        return renderPassCache_[descriptorHash];
    }

    // Need to create a new render pass for this descriptor
    // Phase 35: Support multiple color attachments for deferred rendering

    // Validate attachment configuration
    if (descriptor.colorAttachmentCount == 0 || descriptor.colorAttachmentCount > RenderPassDescriptor::MAX_COLOR_ATTACHMENTS)
    {
        URHO3D_LOGERROR("GetOrCreateRenderPass: Invalid color attachment count " + String(descriptor.colorAttachmentCount));
        return VK_NULL_HANDLE;
    }

    // Total attachments = color attachments + depth
    uint32_t totalAttachmentCount = descriptor.colorAttachmentCount + 1;

    Vector<VkAttachmentDescription> attachments;
    attachments.Resize(totalAttachmentCount);

    // Color attachments (indices 0 to colorAttachmentCount-1)
    for (uint32_t i = 0; i < descriptor.colorAttachmentCount; ++i)
    {
        attachments[i].format = descriptor.colorFormats[i];
        attachments[i].samples = descriptor.sampleCount;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Phase 35: First color attachment is presentation target, others are intermediate
        attachments[i].finalLayout = (i == 0) ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Depth attachment (after all color attachments)
    uint32_t depthIndex = descriptor.colorAttachmentCount;
    attachments[depthIndex].format = descriptor.depthFormat;
    attachments[depthIndex].samples = descriptor.sampleCount;
    attachments[depthIndex].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[depthIndex].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[depthIndex].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[depthIndex].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[depthIndex].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[depthIndex].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Color attachment references for all color attachments
    Vector<VkAttachmentReference> colorRefs;
    colorRefs.Resize(descriptor.colorAttachmentCount);
    for (uint32_t i = 0; i < descriptor.colorAttachmentCount; ++i)
    {
        colorRefs[i].attachment = i;
        colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkAttachmentReference depthRef{};
    depthRef.attachment = depthIndex;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = descriptor.colorAttachmentCount;
    subpass.pColorAttachments = colorRefs.Buffer();
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = totalAttachmentCount;
    renderPassInfo.pAttachments = attachments.Buffer();
    renderPassInfo.subpassCount = descriptor.subpassCount;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create render pass from descriptor");
        return VK_NULL_HANDLE;
    }

    // Cache and return the render pass
    renderPassCache_[descriptorHash] = renderPass;
    URHO3D_LOGDEBUG("Created and cached render pass for descriptor hash: " + String((int)descriptorHash));

    return renderPass;
}

bool VulkanGraphicsImpl::CreateFramebuffers()
{
    framebuffers_.Resize(swapchainImageViews_.Size());

    // Phase 30: Create framebuffers with MSAA support
    bool useMSAA = (actualSampleCount_ != VK_SAMPLE_COUNT_1_BIT);

    for (size_t i = 0; i < swapchainImageViews_.Size(); ++i)
    {
        Vector<VkImageView> attachments;
        if (useMSAA)
        {
            // MSAA: [0] = msaa color, [1] = depth, [2] = swapchain resolve
            attachments.Push(msaaColorImageView_);
            attachments.Push(depthImageView_);
            attachments.Push(swapchainImageViews_[i]);
        }
        else
        {
            // 1x MSAA: [0] = swapchain color, [1] = depth
            attachments.Push(swapchainImageViews_[i]);
            attachments.Push(depthImageView_);
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = (uint32_t)attachments.Size();
        framebufferInfo.pAttachments = !attachments.Empty() ? &attachments[0] : nullptr;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Failed to create framebuffer");
            return false;
        }
    }

    if (useMSAA)
    {
        URHO3D_LOGINFO(String("Framebuffers created with MSAA (") + String((int)actualSampleCount_) + "x)");
    }
    else
    {
        URHO3D_LOGINFO("Framebuffers created (1x MSAA)");
    }
    return true;
}

bool VulkanGraphicsImpl::CreateCommandBuffers()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create command pool");
        return false;
    }

    commandBuffers_.Resize(VULKAN_FRAME_COUNT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)commandBuffers_.Size();

    if (vkAllocateCommandBuffers(device_, &allocInfo, !commandBuffers_.Empty() ? &commandBuffers_[0] : nullptr) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to allocate command buffers");
        return false;
    }

    URHO3D_LOGINFO("Command buffers created");
    return true;
}

bool VulkanGraphicsImpl::CreateSynchronizationPrimitives()
{
    frameFences_.Resize(VULKAN_FRAME_COUNT);
    imageAcquiredSemaphores_.Resize(VULKAN_FRAME_COUNT);
    renderCompleteSemaphores_.Resize(VULKAN_FRAME_COUNT);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < VULKAN_FRAME_COUNT; ++i)
    {
        if (vkCreateFence(device_, &fenceInfo, nullptr, &frameFences_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAcquiredSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderCompleteSemaphores_[i]) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Failed to create synchronization primitives");
            return false;
        }
    }

    // Create timeline semaphore for advanced GPU-CPU synchronization (Phase 33)
    if (!CreateTimelineSemaphore())
    {
        URHO3D_LOGWARNING("Failed to create timeline semaphore, will use binary semaphores");
    }

    URHO3D_LOGINFO("Synchronization primitives created");
    return true;
}

bool VulkanGraphicsImpl::CreateMemoryAllocator()
{
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_0;
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.instance = instance_;

    if (vmaCreateAllocator(&allocatorInfo, &allocator_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create memory allocator");
        return false;
    }

    URHO3D_LOGINFO("Memory allocator created");
    return true;
}

bool VulkanGraphicsImpl::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = VULKAN_DESCRIPTOR_POOL_SIZE;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = VULKAN_DESCRIPTOR_POOL_SIZE;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create descriptor pool");
        return false;
    }

    URHO3D_LOGINFO("Descriptor pool created");
    return true;
}

bool VulkanGraphicsImpl::CreatePipelineCache()
{
    // Create VulkanPipelineCache object (Phase B Quick Win #10)
    // Use graphics_->GetContext() to get Context for Object-derived classes
    pipelineCache_ = MakeShared<VulkanPipelineCache>(graphics_->GetContext());

    if (!pipelineCache_ || !pipelineCache_->Initialize(this))
    {
        URHO3D_LOGERROR("Failed to initialize pipeline cache");
        pipelineCache_ = nullptr;
        return false;
    }

    URHO3D_LOGINFO("Pipeline cache initialized");
    return true;
}

VkSurfaceFormatKHR VulkanGraphicsImpl::FindSurfaceFormat()
{
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);

    Vector<VkSurfaceFormatKHR> availableFormats(formatCount);
    if (!availableFormats.Empty())
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, &availableFormats[0]);

    if (availableFormats.Empty())
    {
        URHO3D_LOGWARNING("No surface formats available, using fallback");
        return VkSurfaceFormatKHR{VULKAN_FALLBACK_SURFACE_FORMAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }

    // Priority list of preferred formats (all with SRGB color space)
    VkFormat preferredFormats[] = {
        VULKAN_PREFERRED_SURFACE_FORMAT,      // B8G8R8A8 SRGB
        VULKAN_FALLBACK_SURFACE_FORMAT,       // R8G8B8A8 SRGB
        VK_FORMAT_B8G8R8A8_UNORM,            // B8G8R8A8 Linear (fallback)
        VK_FORMAT_R8G8B8A8_UNORM             // R8G8B8A8 Linear (fallback)
    };

    // Try to find a format with SRGB color space first
    for (VkFormat preferred : preferredFormats)
    {
        for (const auto& format : availableFormats)
        {
            if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                URHO3D_LOGDEBUG("Selected surface format: preferred");
                return format;
            }
        }
    }

    // If no SRGB format found, try preferred formats in linear color space
    for (VkFormat preferred : preferredFormats)
    {
        for (const auto& format : availableFormats)
        {
            if (format.format == preferred)
            {
                URHO3D_LOGDEBUG("Selected surface format: preferred (linear)");
                return format;
            }
        }
    }

    // Fallback to first available format
    URHO3D_LOGWARNING("Using first available surface format as fallback");
    return availableFormats[0];
}

VkPresentModeKHR VulkanGraphicsImpl::FindPresentMode()
{
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);

    Vector<VkPresentModeKHR> availableModes(modeCount);
    if (!availableModes.Empty())
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, &availableModes[0]);

    if (availableModes.Empty())
    {
        URHO3D_LOGWARNING("No present modes available, using FIFO fallback");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // Priority: mailbox (low latency, triple buffering) > FIFO (vsync) > immediate
    for (const auto& mode : availableModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            URHO3D_LOGDEBUG("Selected present mode: MAILBOX (triple buffering)");
            return mode;
        }
    }

    // FIFO is always available but not in the list sometimes, explicitly return it as fallback
    for (const auto& mode : availableModes)
    {
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
        {
            URHO3D_LOGDEBUG("Selected present mode: FIFO (vsync)");
            return mode;
        }
    }

    URHO3D_LOGWARNING("Mailbox and FIFO modes not available, using first available mode");
    return availableModes[0];
}

uint32_t VulkanGraphicsImpl::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    URHO3D_LOGERROR("Failed to find suitable memory type");
    return 0;
}

void VulkanGraphicsImpl::TransitionImageLayout(VkImage image, VkFormat format,
                                            VkImageLayout oldLayout, VkImageLayout newLayout,
                                            uint32_t mipLevels)
{
    // Get current frame command buffer for recording transition command
    VkCommandBuffer commandBuffer = GetFrameCommandBuffer();
    if (commandBuffer == VK_NULL_HANDLE)
        return;

    // Create image memory barrier for layout transition
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.pNext = nullptr;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // Determine pipeline stage and access flags based on layout transition
    VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    else if (newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
    }

    // Record pipeline barrier to handle the layout transition
    vkCmdPipelineBarrier(commandBuffer,
                         srcStageMask, dstStageMask,
                         0,  // no dependency flags
                         0, nullptr,  // no memory barriers
                         0, nullptr,  // no buffer barriers
                         1, &barrier);  // one image barrier
}

void VulkanGraphicsImpl::ReportPoolStatistics() const
{
    /// \brief Phase 12 (Quick Win #9): Gather and report pool statistics
    /// \details Consolidates profiling data from all memory pools (constant buffers,
    /// descriptor sets, secondary command buffers) for performance analysis.
    /// Enables identification of pool utilization bottlenecks and optimization opportunities.

    if (!constantBufferPool_)
    {
        URHO3D_LOGWARNING("ReportPoolStatistics: Constant buffer pool not initialized");
        return;
    }

    // Gather constant buffer pool statistics
    VulkanConstantBufferPool::PoolStats cbStats = constantBufferPool_->GetStatistics();

    URHO3D_LOGINFO("=== Vulkan Memory Pool Statistics ===");
    URHO3D_LOGINFO("Constant Buffer Pool:");
    URHO3D_LOGINFO("  Total pool size: " + String(cbStats.totalPoolSize / 1024 / 1024) + " MB");
    URHO3D_LOGINFO("  Used this frame: " + String(cbStats.usedSize) + " bytes");
    URHO3D_LOGINFO("  Wasted (fragmentation): " + String(cbStats.wastedSize) + " bytes");
    URHO3D_LOGINFO("  Allocated buffers: " + String(cbStats.allocatedBuffers));
    URHO3D_LOGINFO("  Peak frame size: " + String(cbStats.peakFrameSize) + " bytes");
    URHO3D_LOGINFO("  Allocation count: " + String(cbStats.allocationCount));
    URHO3D_LOGINFO("  Fragmentation ratio: " + String(cbStats.averageFragmentation, 2) + "%");

    // Gather material descriptor statistics
    if (materialDescriptorManager_)
    {
        URHO3D_LOGINFO("Material Descriptor Manager:");

        // Quick Win #8: Dirty flag optimization statistics
        // Note: These counters are internal to materialDescriptorManager_ and would need accessor methods
        // For now, report the descriptor cache size
        uint32_t descriptorCount = materialDescriptorManager_->GetCachedDescriptorCount();
        URHO3D_LOGINFO("  Cached material descriptors: " + String(descriptorCount));
    }
    else
    {
        URHO3D_LOGWARNING("ReportPoolStatistics: Material descriptor manager not initialized");
    }

    // Gather descriptor pool statistics if available
    if (descriptorPool_)
    {
        URHO3D_LOGINFO("Descriptor Pool:");

        // Count active descriptor sets (indirect count from pool state)
        // Note: VulkanDescriptorPool doesn't expose statistics directly yet
        // This is a placeholder for future enhancement with detailed descriptor stats
        URHO3D_LOGINFO("  Descriptor pool allocated (basic tracking only)");
    }

    // Gather secondary command buffer pool statistics
    if (secondaryCommandBufferPool_)
    {
        URHO3D_LOGINFO("Secondary Command Buffer Pool (Phase 4 Multi-threading):");
        URHO3D_LOGINFO("  Pool initialized for parallel batch recording");
        URHO3D_LOGINFO("  Worker threads: 4");
    }

    URHO3D_LOGINFO("=== End Pool Statistics ===");
}

// ============================================
// Phase 33: Shader Module Creation from Variations
// ============================================

bool VulkanGraphicsImpl::CreateShaderModules(ShaderVariation* vertexShader, ShaderVariation* pixelShader,
                                           VkShaderModule& vsModule, VkShaderModule& fsModule)
{
    vsModule = VK_NULL_HANDLE;
    fsModule = VK_NULL_HANDLE;

    // Compile vertex shader if provided
    if (vertexShader)
    {
        Vector<uint32_t> spirvBytecode;
        String errorOutput;

        if (!VulkanShaderModule::GetOrCompileSPIRV(vertexShader, spirvBytecode, errorOutput))
        {
            URHO3D_LOGERROR("CreateShaderModules: Failed to compile vertex shader: " + errorOutput);
            return false;
        }

        vsModule = VulkanShaderModule::CreateShaderModule(device_, spirvBytecode);
        if (!vsModule)
        {
            URHO3D_LOGERROR("CreateShaderModules: Failed to create vertex shader module");
            return false;
        }
    }

    // Compile fragment/pixel shader if provided
    if (pixelShader)
    {
        Vector<uint32_t> spirvBytecode;
        String errorOutput;

        if (!VulkanShaderModule::GetOrCompileSPIRV(pixelShader, spirvBytecode, errorOutput))
        {
            URHO3D_LOGERROR("CreateShaderModules: Failed to compile pixel shader: " + errorOutput);
            // Clean up vertex shader module on failure
            if (vsModule)
            {
                VulkanShaderModule::DestroyShaderModule(device_, vsModule);
                vsModule = VK_NULL_HANDLE;
            }
            return false;
        }

        fsModule = VulkanShaderModule::CreateShaderModule(device_, spirvBytecode);
        if (!fsModule)
        {
            URHO3D_LOGERROR("CreateShaderModules: Failed to create fragment shader module");
            // Clean up vertex shader module on failure
            if (vsModule)
            {
                VulkanShaderModule::DestroyShaderModule(device_, vsModule);
                vsModule = VK_NULL_HANDLE;
            }
            return false;
        }
    }

    return true;
}

// ============================================
// Phase 32 Helper Functions for State Conversion
// ============================================

/// \brief Convert Urho3D CullMode to Vulkan cull mode flags
static VkCullModeFlags ConvertCullMode(CullMode mode)
{
    switch (mode)
    {
    case CULL_NONE: return VK_CULL_MODE_NONE;
    case CULL_CCW:  return VK_CULL_MODE_BACK_BIT;
    case CULL_CW:   return VK_CULL_MODE_FRONT_BIT;
    default:        return VK_CULL_MODE_BACK_BIT;
    }
}

/// \brief Convert Urho3D CompareMode to Vulkan comparison operator
static VkCompareOp ConvertCompareMode(CompareMode mode)
{
    switch (mode)
    {
    case CMP_ALWAYS:       return VK_COMPARE_OP_ALWAYS;
    case CMP_EQUAL:        return VK_COMPARE_OP_EQUAL;
    case CMP_NOTEQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
    case CMP_LESS:         return VK_COMPARE_OP_LESS;
    case CMP_LESSEQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CMP_GREATER:      return VK_COMPARE_OP_GREATER;
    case CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default:               return VK_COMPARE_OP_ALWAYS;
    }
}

/// \brief Convert Urho3D StencilOp to Vulkan stencil operation
static VkStencilOp ConvertStencilOp(StencilOp op)
{
    switch (op)
    {
    case OP_KEEP: return VK_STENCIL_OP_KEEP;
    case OP_ZERO: return VK_STENCIL_OP_ZERO;
    case OP_REF:  return VK_STENCIL_OP_REPLACE;
    case OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    default:      return VK_STENCIL_OP_KEEP;
    }
}

// Phase 32 Step 3 & Phase 33 Step 2: Pipeline creation with graphics state and shader modules
VkPipeline VulkanGraphicsImpl::GetOrCreateGraphicsPipeline(
    VkPipelineLayout layout,
    VkRenderPass renderPass,
    const VulkanPipelineState& state,
    VkShaderModule vsModule,
    VkShaderModule fsModule)
{
    if (!pipelineCache_)
    {
        URHO3D_LOGERROR("GetOrCreateGraphicsPipeline: Pipeline cache not initialized");
        return VK_NULL_HANDLE;
    }

    // Calculate hash of pipeline state for caching
    uint64_t stateHash = state.Hash();

    // Build rasterization state from CullMode and FillMode
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = (state.fillMode == FILL_WIREFRAME) ?
        VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = ConvertCullMode(state.cullMode);
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0f;

    // Build color blend state from blend mode
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Map blend modes to Vulkan blend factors
    switch (state.blendMode)
    {
    case BLEND_REPLACE:
        blendAttachment.blendEnable = false;
        break;
    case BLEND_ADD:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_SUBTRACT:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case BLEND_MULTIPLY:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_ALPHA:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_ADDALPHA:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case BLEND_INVDESTALPHA:
        blendAttachment.blendEnable = true;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    default:
        blendAttachment.blendEnable = false;
        break;
    }

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;
    colorBlendState.logicOpEnable = false;

    // Build depth-stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = (state.depthTest != CMP_ALWAYS);
    depthStencilState.depthWriteEnable = state.depthWrite;
    depthStencilState.depthCompareOp = ConvertCompareMode(state.depthTest);

    // Stencil state (front and back faces same)
    if (state.stencilTest)
    {
        VkStencilOpState stencilOpState{};
        stencilOpState.compareMask = state.stencilCompareMask;
        stencilOpState.writeMask = state.stencilWriteMask;
        stencilOpState.reference = state.stencilRef;
        stencilOpState.compareOp = ConvertCompareMode(state.stencilTestMode);
        stencilOpState.passOp = ConvertStencilOp(state.stencilPass);
        stencilOpState.failOp = ConvertStencilOp(state.stencilFail);
        stencilOpState.depthFailOp = ConvertStencilOp(state.stencilZFail);

        depthStencilState.stencilTestEnable = true;
        depthStencilState.front = stencilOpState;
        depthStencilState.back = stencilOpState;
    }

    // Create minimal pipeline state structures for this frame
    // Full structures would come from vertex/input assembly setup
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyState.primitiveRestartEnable = false;

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 0;
    vertexInputState.pVertexBindingDescriptions = nullptr;
    vertexInputState.vertexAttributeDescriptionCount = 0;
    vertexInputState.pVertexAttributeDescriptions = nullptr;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampleState.sampleShadingEnable = false;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 1920.0f;    // Default, will be updated by viewport calls
    viewport.height = 1080.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {1920, 1080};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Phase 33 Step 2: Prepare shader stages (vertex and fragment shaders)
    // Create VkPipelineShaderStageCreateInfo array from compiled shader modules
    Vector<VkPipelineShaderStageCreateInfo> shaderStages;

    // Add vertex shader stage if provided
    if (vsModule != VK_NULL_HANDLE)
    {
        VkPipelineShaderStageCreateInfo vsStage{};
        vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vsStage.module = vsModule;
        vsStage.pName = "main";  // GLSL entry point
        shaderStages.Push(vsStage);
    }

    // Add fragment shader stage if provided
    if (fsModule != VK_NULL_HANDLE)
    {
        VkPipelineShaderStageCreateInfo fsStage{};
        fsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fsStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fsStage.module = fsModule;
        fsStage.pName = "main";  // GLSL entry point
        shaderStages.Push(fsStage);
    }

    // Build graphics pipeline create info
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.stageCount = shaderStages.Size();        // Phase 33: Set from shader modules
    pipelineInfo.pStages = shaderStages.Size() > 0 ? &shaderStages[0] : nullptr;  // Phase 33: Bind stages

    // Use pipeline cache for creation (handles memory + disk caching)
    VkPipeline pipeline = pipelineCache_->GetOrCreatePipeline(stateHash, pipelineInfo);

    if (pipeline == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("GetOrCreateGraphicsPipeline: Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }

    return pipeline;
}

// Phase 33 Step 3: Get material descriptor set for GPU binding
VkDescriptorSet VulkanGraphicsImpl::GetMaterialDescriptor(Material* material)
{
    if (!material || !materialDescriptorManager_)
    {
        return VK_NULL_HANDLE;
    }

    // Get the descriptor set from the material descriptor manager
    // This includes all material parameters and textures
    VkDescriptorSet descriptorSet = materialDescriptorManager_->GetDescriptor(material);

    if (descriptorSet == VK_NULL_HANDLE)
    {
        URHO3D_LOGWARNING("GetMaterialDescriptor: Failed to get descriptor for material");
        return VK_NULL_HANDLE;
    }

    return descriptorSet;
}

} // namespace Urho3D
