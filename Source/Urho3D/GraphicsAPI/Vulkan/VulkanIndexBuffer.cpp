//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan index buffer implementation (Phase 4)

#ifdef URHO3D_VULKAN

#include "../IndexBuffer.h"
#include "../../Graphics/Graphics.h"
#include "../../Core/Log.h"
#include "VulkanGraphicsImpl.h"

namespace Urho3D
{

void IndexBuffer::OnDeviceLost_Vulkan()
{
    // Vulkan buffers survive device loss (Vulkan device doesn't have context loss like OpenGL)
}

void IndexBuffer::OnDeviceReset_Vulkan()
{
    // Vulkan buffers persist, no reset needed
}

void IndexBuffer::Release_Vulkan()
{
    if (!object_.ptr_)
        return;

    VulkanGraphicsImpl* impl = GetSubsystem<Graphics>()->GetImpl_Vulkan();
    if (!impl)
        return;

    VkBuffer buffer = (VkBuffer)(void*)object_.ptr_;
    VmaAllocation allocation = (VmaAllocation)object_.name_;

    if (buffer)
    {
        vmaDestroyBuffer(impl->GetAllocator(), buffer, allocation);
        object_.ptr_ = nullptr;
        object_.name_ = 0;
    }

    dataPending_ = false;
}

bool IndexBuffer::Create_Vulkan()
{
    Release_Vulkan();

    if (!indexCount_ || !indexSize_)
        return true;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    // Calculate buffer size
    VkDeviceSize bufferSize = (VkDeviceSize)indexCount_ * indexSize_;

    // Create Vulkan buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = dynamic_ ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    VkBuffer buffer;
    VmaAllocation allocation;
    if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create Vulkan index buffer");
        return false;
    }

    object_.ptr_ = (void*)buffer;
    object_.name_ = (u32)(uintptr_t)allocation;

    dataPending_ = true;
    return true;
}

bool IndexBuffer::SetData_Vulkan(const void* data)
{
    if (!data)
        return false;

    // Copy to shadow buffer first
    if (shadowData_)
        memcpy(shadowData_.Get(), data, (size_t)indexCount_ * indexSize_);

    return UpdateToGPU_Vulkan();
}

bool IndexBuffer::SetDataRange_Vulkan(const void* data, i32 start, i32 count, bool discard)
{
    if (!data || start < 0 || count < 0 || start + count > indexCount_)
        return false;

    // Copy to shadow buffer
    if (shadowData_)
    {
        memcpy(shadowData_.Get() + start * indexSize_, data, (size_t)count * indexSize_);
    }

    return UpdateToGPU_Vulkan();
}

bool IndexBuffer::UpdateToGPU_Vulkan()
{
    if (!shadowData_ || !object_.ptr_)
        return false;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    VkBuffer buffer = (VkBuffer)(void*)object_.ptr_;
    VmaAllocation allocation = (VmaAllocation)object_.name_;

    // For dynamic buffers or small updates, use host memory mapping
    if (dynamic_)
    {
        void* mappedData;
        if (vmaMapMemory(impl->GetAllocator(), allocation, &mappedData) == VK_SUCCESS)
        {
            memcpy(mappedData, shadowData_.Get(), (size_t)indexCount_ * indexSize_);
            vmaUnmapMemory(impl->GetAllocator(), allocation);
        }
    }
    else
    {
        // For static buffers, would need staging buffer transfer (to be implemented with better resource management)
        // For now, use host-accessible memory for static buffers too
        void* mappedData;
        if (vmaMapMemory(impl->GetAllocator(), allocation, &mappedData) == VK_SUCCESS)
        {
            memcpy(mappedData, shadowData_.Get(), (size_t)indexCount_ * indexSize_);
            vmaUnmapMemory(impl->GetAllocator(), allocation);
        }
    }

    dataPending_ = false;
    return true;
}

void* IndexBuffer::Lock_Vulkan(i32 start, i32 count, bool discard)
{
    if (start < 0 || count < 0 || start + count > indexCount_)
        return nullptr;

    lockStart_ = start;
    lockCount_ = count;

    // For shadow buffers, lock into shadow memory
    if (shadowData_)
    {
        lockState_ = LOCK_SHADOW;
        dataPending_ = true;
        return shadowData_.Get() + start * indexSize_;
    }

    // Fallback: allocate scratch memory
    lockScratchData_ = new byte[count * indexSize_];
    lockState_ = LOCK_SCRATCH;
    return lockScratchData_;
}

void IndexBuffer::Unlock_Vulkan()
{
    if (lockState_ == LOCK_NONE)
        return;

    if (lockState_ == LOCK_SCRATCH && lockScratchData_)
    {
        // Copy scratch data back to shadow buffer if available
        if (shadowData_)
            memcpy(shadowData_.Get() + lockStart_ * indexSize_, lockScratchData_, lockCount_ * indexSize_);

        delete[](byte*)lockScratchData_;
        lockScratchData_ = nullptr;
    }

    lockState_ = LOCK_NONE;
    dataPending_ = true;

    // Upload to GPU
    UpdateToGPU_Vulkan();
}

void* IndexBuffer::MapBuffer_Vulkan(i32 start, i32 count, bool discard)
{
    if (start < 0 || count < 0 || start + count > indexCount_)
        return nullptr;

    // Vulkan doesn't support direct GPU memory mapping in the way OpenGL does
    // Fall back to shadow/scratch memory
    return Lock_Vulkan(start, count, discard);
}

void IndexBuffer::UnmapBuffer_Vulkan()
{
    Unlock_Vulkan();
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
