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

    Graphics* graphics = GetSubsystem<Graphics>();
    VulkanGraphicsImpl* impl = graphics ? graphics->GetImpl_Vulkan() : nullptr;

    VkBuffer buffer = (VkBuffer)(void*)object_.ptr_;
    VmaAllocation allocation = (VmaAllocation)object_.ptr2_;

    // Always clear handles, even if we can't destroy (shutdown order)
    object_.ptr_ = nullptr;
    object_.ptr2_ = nullptr;
    dataPending_ = false;

    // Only destroy if graphics subsystem still exists
    if (impl && buffer)
    {
        vmaDestroyBuffer(impl->GetAllocator(), buffer, allocation);
    }
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
    // ALWAYS need HOST_VISIBLE for CPU mapping (static buffers are mapped too)
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

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

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
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
    object_.ptr2_ = (void*)allocation;

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

    // If no shadow buffer, upload data directly to GPU
    if (!shadowData_)
    {
        return UploadDataToGPU_Vulkan(data, vertexCount_ * vertexSize_);
    }

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

    // DEBUG: Always log vertex buffer updates
    URHO3D_LOGDEBUG(String("SetDataRange_Vulkan: start=") + String(start) + ", count=" + String(count) +
                   ", vertexSize=" + String(vertexSize_) + ", shadowData=" + String(shadowData_ != nullptr));

    // DEBUG: Dump first vertex data to check position/color
    if (count >= 1 && vertexSize_ >= 24)
    {
        const float* floatData = (const float*)data;
        const unsigned char* byteData = (const unsigned char*)data;
        URHO3D_LOGDEBUG(String("VERTEX_DATA[0]: pos=(") + String(floatData[0]) + "," +
                       String(floatData[1]) + "," + String(floatData[2]) + "), color_bytes=(" +
                       String((int)byteData[12]) + "," + String((int)byteData[13]) + "," +
                       String((int)byteData[14]) + "," + String((int)byteData[15]) + ")");
    }

    return UpdateToGPU_Vulkan();
}

bool VertexBuffer::UploadDataToGPU_Vulkan(const void* data, size_t dataSize)
{
    if (!data || !object_.ptr_)
        return false;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    VkBuffer buffer = (VkBuffer)(void*)object_.ptr_;
    VmaAllocation allocation = (VmaAllocation)object_.ptr2_;

    // Map GPU memory and copy data directly
    void* mappedData;
    if (vmaMapMemory(impl->GetAllocator(), allocation, &mappedData) == VK_SUCCESS)
    {
        memcpy(mappedData, data, dataSize);
        // CRITICAL: Flush memory to ensure GPU visibility on non-coherent memory
        vmaFlushAllocation(impl->GetAllocator(), allocation, 0, dataSize);
        vmaUnmapMemory(impl->GetAllocator(), allocation);
        dataPending_ = false;
        return true;
    }

    URHO3D_LOGERROR("Failed to map vertex buffer memory for upload");
    return false;
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
    VmaAllocation allocation = (VmaAllocation)object_.ptr2_;

    size_t dataSize = (size_t)vertexCount_ * vertexSize_;

    // For dynamic buffers or small updates, use host memory mapping
    void* mappedData;
    VkResult mapResult = vmaMapMemory(impl->GetAllocator(), allocation, &mappedData);
    if (mapResult == VK_SUCCESS)
    {
        memcpy(mappedData, shadowData_.Get(), dataSize);
        // CRITICAL: Flush memory to ensure GPU visibility on non-coherent memory
        vmaFlushAllocation(impl->GetAllocator(), allocation, 0, dataSize);
        vmaUnmapMemory(impl->GetAllocator(), allocation);
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
        {
            memcpy(shadowData_.Get() + lockStart_ * vertexSize_, lockScratchData_, lockCount_ * vertexSize_);
        }
        else
        {
            // No shadow buffer - upload scratch data directly to GPU
            // This is needed for instancing buffers which don't use shadow data
            if (object_.ptr_)
            {
                Graphics* graphics = GetSubsystem<Graphics>();
                VulkanGraphicsImpl* impl = graphics ? graphics->GetImpl_Vulkan() : nullptr;
                if (impl)
                {
                    VmaAllocation allocation = (VmaAllocation)object_.ptr2_;
                    void* mappedData;
                    VkResult mapResult = vmaMapMemory(impl->GetAllocator(), allocation, &mappedData);
                    if (mapResult == VK_SUCCESS)
                    {
                        // Copy to the correct offset in the GPU buffer
                        size_t uploadSize = lockCount_ * vertexSize_;
                        size_t uploadOffset = lockStart_ * vertexSize_;
                        memcpy((byte*)mappedData + uploadOffset, lockScratchData_, uploadSize);

                        // CRITICAL: Flush memory to ensure GPU visibility on non-coherent memory
                        vmaFlushAllocation(impl->GetAllocator(), allocation, uploadOffset, uploadSize);

                        vmaUnmapMemory(impl->GetAllocator(), allocation);

                        // DIAGNOSTIC: Log scratch-to-GPU upload
                        static int scratchUploadLog = 0;
                        if (scratchUploadLog < 10)
                        {
                            const float* fd = reinterpret_cast<const float*>(lockScratchData_ ? lockScratchData_ : (byte*)0);
                            URHO3D_LOGWARNING("[SCRATCH_GPU] Upload OK: vtxSize=" + String(vertexSize_) +
                                ", lockCount=" + String(lockCount_) + ", uploadSize=" + String((unsigned)uploadSize) +
                                ", uploadOffset=" + String((unsigned)uploadOffset) +
                                ", vkBuf=" + String(object_.ptr_ != nullptr));
                            scratchUploadLog++;
                        }
                    }
                    else
                    {
                        URHO3D_LOGERROR("[SCRATCH_GPU] vmaMapMemory FAILED! VkResult=" + String((int)mapResult) +
                            ", vtxSize=" + String(vertexSize_) + ", lockCount=" + String(lockCount_));
                    }
                }
            }
            else
            {
                URHO3D_LOGERROR("[SCRATCH_GPU] No GPU object! Cannot upload scratch data, vtxSize=" + String(vertexSize_));
            }
        }

        delete[](byte*)lockScratchData_;
        lockScratchData_ = nullptr;
    }

    lockState_ = LOCK_NONE;
    dataPending_ = true;

    // Upload to GPU (for shadow buffer path)
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
