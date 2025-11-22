//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//

#include "VulkanGraphicsImpl.h"
#include "../GraphicsDefs.h"
#include "../../Core/Log.h"
#include <SDL2/SDL.h>

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

bool VulkanGraphicsImpl::Initialize(Graphics* graphics, const WindowHandle& window, int width, int height)
{
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

    if (!CreateDepthBuffer(VULKAN_PREFERRED_DEPTH_FORMAT, width, height))
    {
        // Try fallback depth format
        if (!CreateDepthBuffer(VULKAN_FALLBACK_DEPTH_FORMAT, width, height))
        {
            URHO3D_LOGERROR("Failed to create depth buffer");
            return false;
        }
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

    // Destroy descriptor pool
    if (descriptorPool_)
    {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = nullptr;
    }

    // Destroy pipeline cache
    if (pipelineCache_)
    {
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        pipelineCache_ = nullptr;
    }

    // Destroy pipelines
    for (auto& pair : pipelineCacheMap_)
    {
        if (pair.second_)
            vkDestroyPipeline(device_, pair.second_, nullptr);
    }
    pipelineCacheMap_.Clear();

    // Destroy samplers
    for (auto& pair : samplerCacheMap_)
    {
        if (pair.second_)
            vkDestroySampler(device_, pair.second_, nullptr);
    }
    samplerCacheMap_.Clear();

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

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderCompleteSemaphores_[frameIndex_];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &currentImageIndex_;

    VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        URHO3D_LOGERROR("Failed to present swapchain image: " + String((int)result));
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

    VkFence fence = frameFences_[frameIndex_];
    if (fence)
    {
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
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

    VkClearValue clearValues[2]{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // Color: black
    clearValues[1].depthStencil = {1.0f, 0};              // Depth: 1.0

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = GetCurrentFramebuffer();
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;
    renderPassInfo.clearValueCount = 2;
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
    uint64_t hash = ((uint64_t)filter << 32) | addressMode;

    auto it = samplerCacheMap_.Find(hash);
    if (it != samplerCacheMap_.End())
        return it->second_;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    VkSampler sampler{};
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create sampler");
        return nullptr;
    }

    samplerCacheMap_[hash] = sampler;
    return sampler;
}

VkPipeline VulkanGraphicsImpl::GetGraphicsPipeline(const VkGraphicsPipelineCreateInfo& createInfo, uint64_t stateHash)
{
    auto it = pipelineCacheMap_.Find(stateHash);
    if (it != pipelineCacheMap_.End())
        return it->second_;

    VkPipeline pipeline{};
    if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &createInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create graphics pipeline");
        return nullptr;
    }

    pipelineCacheMap_[stateHash] = pipeline;
    return pipeline;
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
    if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &extensionCount, extensions.Data()))
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
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.Data());
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
    createInfo.ppEnabledExtensionNames = extensions.Data();
    createInfo.enabledLayerCount = layers.Size();
    createInfo.ppEnabledLayerNames = layers.Data();

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
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.Data());

    // Prefer dedicated GPUs, but accept integrated GPUs
    int selectedIndex = -1;
    VkPhysicalDeviceType preferredType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    for (uint32_t i = 0; i < deviceCount; ++i)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        URHO3D_LOGINFO(String("Found GPU: ") + properties.deviceName);

        if (properties.deviceType == preferredType)
        {
            physicalDevice_ = devices[i];
            selectedIndex = i;
            break;
        }
        else if (selectedIndex < 0)
        {
            physicalDevice_ = devices[i];
            selectedIndex = i;
        }
    }

    if (!physicalDevice_)
    {
        URHO3D_LOGERROR("Failed to select physical device");
        return false;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    URHO3D_LOGINFO(String("Selected GPU: ") + properties.deviceName);

    return true;
}

bool VulkanGraphicsImpl::FindQueueFamilies()
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);

    Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.Data());

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
    createInfo.pQueueCreateInfos = queueCreateInfos.Data();
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

bool VulkanGraphicsImpl::CreateSurface(const WindowHandle& window)
{
    SDL_Window* sdlWindow = (SDL_Window*)window;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, instance_, &surface_))
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
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, swapchainImages_.Data());

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

    URHO3D_LOGINFO(String("Swapchain created with ") + String((int)swapchainImageCount) + " images");
    return true;
}

bool VulkanGraphicsImpl::CreateDepthBuffer(VkFormat format, int width, int height)
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
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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

bool VulkanGraphicsImpl::CreateRenderPass()
{
    VkAttachmentDescription attachments[2]{};

    // Color attachment
    attachments[0].format = swapchainFormat_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth attachment
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create render pass");
        return false;
    }

    URHO3D_LOGINFO("Render pass created");
    return true;
}

bool VulkanGraphicsImpl::CreateFramebuffers()
{
    framebuffers_.Resize(swapchainImageViews_.Size());

    for (size_t i = 0; i < swapchainImageViews_.Size(); ++i)
    {
        VkImageView attachments[] = {
            swapchainImageViews_[i],
            depthImageView_
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS)
        {
            URHO3D_LOGERROR("Failed to create framebuffer");
            return false;
        }
    }

    URHO3D_LOGINFO("Framebuffers created");
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

    if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.Data()) != VK_SUCCESS)
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
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = 0;
    cacheInfo.pInitialData = nullptr;

    if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create pipeline cache");
        return false;
    }

    URHO3D_LOGINFO("Pipeline cache created");
    return true;
}

VkSurfaceFormatKHR VulkanGraphicsImpl::FindSurfaceFormat()
{
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);

    Vector<VkSurfaceFormatKHR> availableFormats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, availableFormats.Data());

    // Prefer SRGB color space for gamma-correct rendering
    for (const auto& format : availableFormats)
    {
        if (format.format == VULKAN_PREFERRED_SURFACE_FORMAT &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return format;
        }
    }

    // Fallback to first available format
    if (availableFormats.Size() > 0)
    {
        return availableFormats[0];
    }

    // Last resort fallback
    return VkSurfaceFormatKHR{VULKAN_FALLBACK_SURFACE_FORMAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}

VkPresentModeKHR VulkanGraphicsImpl::FindPresentMode()
{
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);

    Vector<VkPresentModeKHR> availableModes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, availableModes.Data());

    // Prefer mailbox (triple buffering), fallback to FIFO (vsync)
    for (const auto& mode : availableModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return mode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;  // Always available
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

} // namespace Urho3D
