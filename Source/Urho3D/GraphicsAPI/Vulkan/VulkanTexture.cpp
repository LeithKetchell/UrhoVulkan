//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan 2D texture implementation (Phase 5)

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "../Texture2D.h"
#include "../../Graphics/Graphics.h"
#include "../../IO/Log.h"
#include "../../Resource/Image.h"
#include "VulkanGraphicsImpl.h"

namespace Urho3D
{

void Texture2D::OnDeviceLost_Vulkan()
{
    // Vulkan textures survive device loss
}

void Texture2D::OnDeviceReset_Vulkan()
{
    // Vulkan textures persist
}

void Texture2D::Release_Vulkan()
{
    if (!object_.ptr_)
        return;

    VulkanGraphicsImpl* impl = GetSubsystem<Graphics>()->GetImpl_Vulkan();
    if (!impl)
        return;

    // Release image view
    if (shaderResourceView_)
    {
        vkDestroyImageView(impl->GetDevice(), (VkImageView)shaderResourceView_, nullptr);
        shaderResourceView_ = nullptr;
    }

    // Release sampler
    if (sampler_)
    {
        vkDestroySampler(impl->GetDevice(), (VkSampler)sampler_, nullptr);
        sampler_ = nullptr;
    }

    // Release image
    VkImage image = (VkImage)(void*)object_.ptr_;
    VmaAllocation allocation = (VmaAllocation)object_.name_;

    if (image)
    {
        vmaDestroyImage(impl->GetAllocator(), image, allocation);
        object_.ptr_ = nullptr;
        object_.name_ = 0;
    }
}

bool Texture2D::Create_Vulkan()
{
    Release_Vulkan();

    if (!width_ || !height_)
        return false;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    // Map Urho3D format to Vulkan format (simplified - supports common formats)
    VkFormat vkFormat = VK_FORMAT_R8G8B8A8_SRGB;  // Default RGBA SRGB

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vkFormat;
    imageInfo.extent = {(uint32_t)width_, (uint32_t)height_, 1};
    imageInfo.mipLevels = levels_;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    // Attempt to use memory pool for optimized allocation
    VulkanMemoryPoolManager* poolMgr = impl->GetMemoryPoolManager();
    if (poolMgr)
    {
        VmaPool pool = poolMgr->GetPool(VulkanMemoryPoolType::Textures);
        if (pool)
        {
            allocInfo.pool = pool;
        }
    }

    VkImage image;
    VmaAllocation allocation;
    if (vmaCreateImage(impl->GetAllocator(), &imageInfo, &allocInfo, &image, &allocation, nullptr) != VK_SUCCESS)
    {
        // Fallback: attempt without pool if pool allocation failed
        if (poolMgr)
        {
            allocInfo.pool = nullptr;
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            if (vmaCreateImage(impl->GetAllocator(), &imageInfo, &allocInfo, &image, &allocation, nullptr) != VK_SUCCESS)
            {
                URHO3D_LOGERROR("Failed to create Vulkan texture image");
                return false;
            }
        }
        else
        {
            URHO3D_LOGERROR("Failed to create Vulkan texture image");
            return false;
        }
    }

    object_.ptr_ = (void*)image;
    object_.name_ = (u32)(uintptr_t)allocation;

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkFormat;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = levels_;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(impl->GetDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create Vulkan image view");
        vmaDestroyImage(impl->GetAllocator(), image, allocation);
        object_.ptr_ = nullptr;
        return false;
    }

    shaderResourceView_ = imageView;

    // Transition image layout from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    impl->TransitionImageLayout(image, vkFormat,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               levels_);

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

    // Map filter mode
    switch (filterMode_)
    {
    case FILTER_NEAREST:
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        break;
    case FILTER_BILINEAR:
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        break;
    case FILTER_TRILINEAR:
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        break;
    case FILTER_ANISOTROPIC:
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = anisotropy_ ? anisotropy_ : 1.0f;
        break;
    default:
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }

    // Map address modes
    VkSamplerAddressMode addressModes[] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
    };

    samplerInfo.addressModeU = addressModes[addressModes_[COORD_U]];
    samplerInfo.addressModeV = addressModes[addressModes_[COORD_V]];
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = (float)levels_;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    VkSampler vkSampler;
    if (vkCreateSampler(impl->GetDevice(), &samplerInfo, nullptr, &vkSampler) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create Vulkan sampler");
        vkDestroyImageView(impl->GetDevice(), imageView, nullptr);
        vmaDestroyImage(impl->GetAllocator(), image, allocation);
        object_.ptr_ = nullptr;
        return false;
    }

    sampler_ = vkSampler;
    dataPending_ = true;

    return true;
}

bool Texture2D::SetData_Vulkan(unsigned level, int x, int y, int width, int height, const void* data)
{
    if (!data)
        return false;

    // For now, just mark data as pending
    // Full implementation would create staging buffer and copy data
    dataPending_ = true;
    return true;
}

bool Texture2D::SetData_Vulkan(Image* image, bool useAlpha)
{
    if (!image)
        return false;

    // For now, just accept the image and mark data as pending
    // Full implementation would handle format conversion and staging buffer upload
    dataPending_ = true;
    return true;
}

bool Texture2D::GetData_Vulkan(unsigned level, void* dest) const
{
    // Vulkan doesn't support easy GPU-to-CPU readback without staging buffers
    URHO3D_LOGWARNING("GetData not supported on Vulkan - use Image resource instead");
    return false;
}


} // namespace Urho3D

#endif  // URHO3D_VULKAN
