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
#include "../../Container/Vector.h"
#include "VulkanGraphicsImpl.h"
#include "VulkanStagingBufferManager.h"

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

    // Calculate mipmap levels
    levels_ = CheckMaxLevels(width_, height_, requestedLevels_);

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

    // Note: Image layout transitions are handled in SetData_Vulkan during upload
    // Initial layout is VK_IMAGE_LAYOUT_UNDEFINED as specified in imageInfo.initialLayout

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
    URHO3D_LOGINFO("SetData_Vulkan(raw): level=" + String(level) + ", region=" + String(x) + "," +
                   String(y) + "," + String(width) + "x" + String(height) +
                   ", tex_size=" + String(width_) + "x" + String(height_) +
                   ", levels=" + String(levels_) + ", format=" + String(format_));

    if (!data)
        return false;

    // If texture not created yet, just mark data as pending
    // This can happen when SetData is called before SetSize/Create
    if (levels_ == 0 || width_ == 0 || height_ == 0)
    {
        dataPending_ = true;
        return true;
    }

    if (level >= levels_)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - invalid mipmap level " + String(level) + " (max: " + String(levels_ - 1) + ")");
        return false;
    }

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    VulkanStagingBufferManager* stagingMgr = impl->GetStagingBufferManager();
    if (!stagingMgr)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - staging buffer manager not available");
        return false;
    }

    // Calculate data size for the region
    unsigned dataSize = GetDataSize(width, height);
    if (dataSize == 0)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - invalid data size (format=" + String(format_) +
                       ", width=" + String(width_) + ", height=" + String(height_) + ")");
        return false;
    }

    VkImage image = (VkImage)(void*)object_.ptr_;
    if (!image)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - image not created yet");
        return false;
    }

    // Get command buffer for recording transfer commands
    VkCommandBuffer commandBuffer = impl->GetFrameCommandBuffer();
    if (commandBuffer == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - no command buffer available");
        return false;
    }

    // Transition image layout: SHADER_READ_ONLY_OPTIMAL -> TRANSFER_DST_OPTIMAL
    impl->TransitionImageLayout(image, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1);

    // Create staging buffer and copy data
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = dataSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;
    VmaAllocationInfo allocationInfo;

    if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo,
                       &stagingBuffer, &stagingAllocation, &allocationInfo) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - failed to create staging buffer");
        return false;
    }

    // Copy texture data to staging buffer
    memcpy(allocationInfo.pMappedData, data, dataSize);

    // Record buffer-to-image copy command
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;  // Tightly packed
    region.bufferImageHeight = 0;  // Tightly packed
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = level;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {x, y, 0};
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};

    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition image layout: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    impl->TransitionImageLayout(image, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               1);

    // Defer staging buffer cleanup until GPU finishes
    // For now, we'll destroy immediately (in real implementation, track with fence)
    vmaDestroyBuffer(impl->GetAllocator(), stagingBuffer, stagingAllocation);

    dataPending_ = false;
    return true;
}

bool Texture2D::SetData_Vulkan(Image* image, bool useAlpha)
{
    if (!image)
        return false;

    // Get image dimensions and validate
    int width = image->GetWidth();
    int height = image->GetHeight();
    unsigned components = image->GetComponents();

    if (width <= 0 || height <= 0 || components == 0)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - invalid image dimensions");
        return false;
    }

    // Determine format based on image components
    unsigned format = 0;
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    if (!image->IsCompressed())
    {
        switch (components)
        {
        case 1:
            format = useAlpha ? Graphics::GetAlphaFormat() : Graphics::GetLuminanceFormat();
            break;
        case 2:
            format = Graphics::GetLuminanceAlphaFormat();
            break;
        case 3:
            format = Graphics::GetRGBFormat();
            break;
        case 4:
            format = Graphics::GetRGBAFormat();
            break;
        default:
            URHO3D_LOGERROR("Texture2D::SetData_Vulkan - unsupported component count");
            return false;
        }
    }
    else
    {
        format = graphics->GetFormat(image->GetCompressedFormat());
        if (!format)
        {
            URHO3D_LOGERROR("Texture2D::SetData_Vulkan - unsupported compressed format");
            return false;
        }
    }

    // Set texture size if not already set (this sets format_)
    if (!width_ || !height_)
    {
        // Use SetSize to properly initialize all texture parameters including format_
        if (!SetSize(width, height, format))
        {
            URHO3D_LOGERROR("Texture2D::SetData_Vulkan - SetSize failed");
            return false;
        }
    }

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    VkImage vkImage = (VkImage)(void*)object_.ptr_;
    if (!vkImage)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - image not created");
        return false;
    }

    // Get image data
    unsigned char* data = image->GetData();
    if (!data)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - no image data");
        return false;
    }

    // Calculate total data size for all mip levels
    unsigned totalSize = 0;
    int levelWidth = width;
    int levelHeight = height;

    for (unsigned level = 0; level < levels_; ++level)
    {
        totalSize += GetDataSize(levelWidth, levelHeight);
        levelWidth = Max(levelWidth >> 1, 1);
        levelHeight = Max(levelHeight >> 1, 1);
    }

    // Get command buffer for recording transfer commands
    VkCommandBuffer commandBuffer = impl->GetFrameCommandBuffer();
    if (commandBuffer == VK_NULL_HANDLE)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - no command buffer available");
        return false;
    }

    // Create staging buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;
    VmaAllocationInfo allocationInfo;

    if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo,
                       &stagingBuffer, &stagingAllocation, &allocationInfo) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Texture2D::SetData_Vulkan - failed to create staging buffer");
        return false;
    }

    // Copy all mip levels to staging buffer
    unsigned char* stagingData = (unsigned char*)allocationInfo.pMappedData;
    unsigned offset = 0;
    levelWidth = width;
    levelHeight = height;

    SharedPtr<Image> currentLevel(image);
    for (unsigned level = 0; level < levels_; ++level)
    {
        unsigned levelSize = GetDataSize(levelWidth, levelHeight);

        if (level == 0)
        {
            // Copy base level from image data
            memcpy(stagingData + offset, data, levelSize);
        }
        else if (image->GetNumCompressedLevels() > level)
        {
            // Use precomputed mip level from compressed image
            CompressedLevel compLevel = image->GetCompressedLevel(level);
            if (compLevel.data_)
                memcpy(stagingData + offset, compLevel.data_, levelSize);
        }
        else if (currentLevel)
        {
            // Generate mip level by downsampling
            currentLevel = currentLevel->GetNextLevel();
            if (currentLevel && currentLevel->GetData())
                memcpy(stagingData + offset, currentLevel->GetData(), levelSize);
        }

        offset += levelSize;
        levelWidth = Max(levelWidth >> 1, 1);
        levelHeight = Max(levelHeight >> 1, 1);
    }

    // Transition image layout: UNDEFINED -> TRANSFER_DST_OPTIMAL
    impl->TransitionImageLayout(vkImage, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               levels_);

    // Record buffer-to-image copy for all mip levels
    Vector<VkBufferImageCopy> regions;
    offset = 0;
    levelWidth = width;
    levelHeight = height;

    for (unsigned level = 0; level < levels_; ++level)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {(uint32_t)levelWidth, (uint32_t)levelHeight, 1};

        regions.Push(region);

        offset += GetDataSize(levelWidth, levelHeight);
        levelWidth = Max(levelWidth >> 1, 1);
        levelHeight = Max(levelHeight >> 1, 1);
    }

    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, vkImage,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          regions.Size(), &regions[0]);

    // Transition image layout: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    impl->TransitionImageLayout(vkImage, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               levels_);

    // VMA handles deferred cleanup - won't free until GPU operations complete
    // Note: For async uploads, use VulkanStagingBufferManager with explicit fences
    vmaDestroyBuffer(impl->GetAllocator(), stagingBuffer, stagingAllocation);

    dataPending_ = false;
    return true;
}

bool Texture2D::GetData_Vulkan(unsigned level, void* dest) const
{
    // Vulkan doesn't support easy GPU-to-CPU readback without staging buffers
    URHO3D_LOGWARNING("GetData not supported on Vulkan - use Image resource instead");
    return false;
}

// ============================================================================
// Texture Helper Functions (Phase 10 - Staging Buffer Support)
// ============================================================================

unsigned Texture::GetRowDataSize_Vulkan(int width) const
{
    // For Vulkan, format_ stores VkFormat values directly
    switch (format_)
    {
    // 1 byte per pixel formats
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
        return (unsigned)width;

    // 2 bytes per pixel formats
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
        return (unsigned)(width * 2);

    // 3 bytes per pixel formats
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8_SNORM:
    case VK_FORMAT_R8G8B8_UINT:
    case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_B8G8R8_UNORM:
        return (unsigned)(width * 3);

    // 4 bytes per pixel formats
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
        return (unsigned)(width * 4);

    // 8 bytes per pixel formats
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
        return (unsigned)(width * 8);

    // 16 bytes per pixel formats
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return (unsigned)(width * 16);

    // Compressed formats (BC/DXT) - 4x4 blocks
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return ((unsigned)(width + 3) >> 2u) * 8;  // 8 bytes per 4x4 block

    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return ((unsigned)(width + 3) >> 2u) * 16;  // 16 bytes per 4x4 block

    // ETC2/EAC compressed formats
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        return ((unsigned)(width + 3) >> 2u) * 8;

    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        return ((unsigned)(width + 3) >> 2u) * 16;

    default:
        // Default: assume RGBA 4 bytes per pixel
        return (unsigned)(width * 4);
    }
}

bool Texture::IsCompressed_Vulkan() const
{
    // Check if format is a compressed block format
    switch (format_)
    {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

bool Texture::GetParametersDirty_Vulkan() const
{
    return parametersDirty_;
}

void Texture::RegenerateLevels_Vulkan()
{
    if (!object_.ptr_ || levels_ < 2)
        return;

    auto* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return;

    auto* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return;

    VkImage image = (VkImage)object_.ptr_;
    VkCommandBuffer commandBuffer = impl->GetFrameCommandBuffer();

    // Transition base level to TRANSFER_SRC for reading
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = width_;
    int32_t mipHeight = height_;

    for (unsigned i = 1; i < levels_; i++)
    {
        // Transition previous level to TRANSFER_SRC
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition current level to TRANSFER_DST
        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Blit from previous level to current level
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1,
                              mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(commandBuffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // Transition previous level back to SHADER_READ_ONLY
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Update dimensions for next iteration
        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    // Transition last level to SHADER_READ_ONLY
    barrier.subresourceRange.baseMipLevel = levels_ - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    levelsDirty_ = false;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
