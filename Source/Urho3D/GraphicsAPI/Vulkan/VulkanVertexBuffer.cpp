//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan vertex buffer implementation (Phase 4)

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "../VertexBuffer.h"
#include "../../Graphics/Graphics.h"
#include "../../IO/Log.h"
#include "VulkanGraphicsImpl.h"

namespace Urho3D
{

void VertexBuffer::OnDeviceLost_Vulkan()
{
    // Vulkan buffers survive device loss (Vulkan device doesn't have context loss like OpenGL)
    // However, mark data as pending for recreation if needed
}

void VertexBuffer::OnDeviceReset_Vulkan()
{
    // Vulkan buffers persist, no reset needed
}

void VertexBuffer::Release_Vulkan()
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

bool VertexBuffer::Create_Vulkan()
{
    Release_Vulkan();

    if (!vertexCount_ || !vertexSize_)
        return true;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    // Calculate buffer size
    VkDeviceSize bufferSize = (VkDeviceSize)vertexCount_ * vertexSize_;

    // Create Vulkan buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = dynamic_ ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    // Attempt to use memory pool for optimized allocation
    VulkanMemoryPoolManager* poolMgr = impl->GetMemoryPoolManager();
    if (poolMgr)
    {
        VulkanMemoryPoolType poolType = dynamic_ ? VulkanMemoryPoolType::DynamicGeometry : VulkanMemoryPoolType::StaticGeometry;
        VmaPool pool = poolMgr->GetPool(poolType);
        if (pool)
        {
            allocInfo.pool = pool;
        }
    }

    VkBuffer buffer;
    VmaAllocation allocation;
    if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS)
    {
        // Fallback: attempt without pool if pool allocation failed
        if (poolMgr)
        {
            allocInfo.pool = nullptr;
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS)
            {
                URHO3D_LOGERROR("Failed to create Vulkan vertex buffer");
                return false;
            }
        }
        else
        {
            URHO3D_LOGERROR("Failed to create Vulkan vertex buffer");
            return false;
        }
    }

    object_.ptr_ = (void*)buffer;
    object_.name_ = (u32)(uintptr_t)allocation;

    dataPending_ = true;
    return true;
}

bool VertexBuffer::SetData_Vulkan(const void* data)
{
    if (!data)
        return false;

    // Copy to shadow buffer first
    if (shadowData_)
        memcpy(shadowData_.Get(), data, (size_t)vertexCount_ * vertexSize_);

    return UpdateToGPU_Vulkan();
}

bool VertexBuffer::SetDataRange_Vulkan(const void* data, i32 start, i32 count, bool discard)
{
    if (!data || start < 0 || count < 0 || start + count > vertexCount_)
        return false;

    // Copy to shadow buffer
    if (shadowData_)
    {
        memcpy(shadowData_.Get() + start * vertexSize_, data, (size_t)count * vertexSize_);
    }

    return UpdateToGPU_Vulkan();
}

bool VertexBuffer::UpdateToGPU_Vulkan()
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
            memcpy(mappedData, shadowData_.Get(), (size_t)vertexCount_ * vertexSize_);
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
            memcpy(mappedData, shadowData_.Get(), (size_t)vertexCount_ * vertexSize_);
            vmaUnmapMemory(impl->GetAllocator(), allocation);
        }
    }

    dataPending_ = false;
    return true;
}

void* VertexBuffer::Lock_Vulkan(i32 start, i32 count, bool discard)
{
    if (start < 0 || count < 0 || start + count > vertexCount_)
        return nullptr;

    lockStart_ = start;
    lockCount_ = count;

    // For shadow buffers, lock into shadow memory
    if (shadowData_)
    {
        lockState_ = LOCK_SHADOW;
        dataPending_ = true;
        return shadowData_.Get() + start * vertexSize_;
    }

    // Fallback: allocate scratch memory
    lockScratchData_ = new byte[count * vertexSize_];
    lockState_ = LOCK_SCRATCH;
    return lockScratchData_;
}

void VertexBuffer::Unlock_Vulkan()
{
    if (lockState_ == LOCK_NONE)
        return;

    if (lockState_ == LOCK_SCRATCH && lockScratchData_)
    {
        // Copy scratch data back to shadow buffer if available
        if (shadowData_)
            memcpy(shadowData_.Get() + lockStart_ * vertexSize_, lockScratchData_, lockCount_ * vertexSize_);

        delete[](byte*)lockScratchData_;
        lockScratchData_ = nullptr;
    }

    lockState_ = LOCK_NONE;
    dataPending_ = true;

    // Upload to GPU
    UpdateToGPU_Vulkan();
}

void* VertexBuffer::MapBuffer_Vulkan(i32 start, i32 count, bool discard)
{
    if (start < 0 || count < 0 || start + count > vertexCount_)
        return nullptr;

    // Vulkan doesn't support direct GPU memory mapping in the way OpenGL does
    // Fall back to shadow/scratch memory
    return Lock_Vulkan(start, count, discard);
}

void VertexBuffer::UnmapBuffer_Vulkan()
{
    Unlock_Vulkan();
}


} // namespace Urho3D

#endif  // URHO3D_VULKAN
