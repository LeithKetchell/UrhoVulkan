//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan constant buffer pooling implementation (Quick Win #6)

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "VulkanConstantBufferPool.h"
#include "VulkanGraphicsImpl.h"
#include "../../IO/Log.h"
#include <cstring>

#include "../../DebugNew.h"

namespace Urho3D
{

VulkanConstantBufferPool::VulkanConstantBufferPool()
{
}

VulkanConstantBufferPool::~VulkanConstantBufferPool()
{
    Release();
}

bool VulkanConstantBufferPool::Initialize(VulkanGraphicsImpl* impl, uint32_t poolSizeInBytes)
{
    if (!impl)
    {
        URHO3D_LOGERROR("Invalid graphics implementation passed to VulkanConstantBufferPool");
        return false;
    }

    impl_ = impl;
    poolSizePerBuffer_ = poolSizeInBytes;

    // Create initial pool buffer
    if (!CreatePoolBuffer(poolSizePerBuffer_))
    {
        URHO3D_LOGERROR("Failed to create initial constant buffer pool");
        return false;
    }

    URHO3D_LOGDEBUG("VulkanConstantBufferPool initialized with size: " + String(poolSizeInBytes / 1024 / 1024) + " MB");
    return true;
}

void VulkanConstantBufferPool::Release()
{
    // Destroy all pool buffers
    for (auto& buffer : poolBuffers_)
    {
        if (buffer.buffer && impl_)
        {
            vmaDestroyBuffer(impl_->GetAllocator(), buffer.buffer, buffer.allocation);
        }
    }
    poolBuffers_.Clear();
    impl_ = nullptr;

    URHO3D_LOGDEBUG("VulkanConstantBufferPool released (total allocations: " +
                   String(stats_.allocationCount) + ", peak frame size: " +
                   String(stats_.peakFrameSize / 1024) + " KB)");
}

bool VulkanConstantBufferPool::CreatePoolBuffer(uint32_t size)
{
    if (!impl_)
        return false;

    if (poolBuffers_.Size() >= MAX_POOL_BUFFERS)
    {
        URHO3D_LOGWARNING("Constant buffer pool reached maximum buffer count: " + String(MAX_POOL_BUFFERS));
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // Attempt to use memory pool for optimized allocation
    VulkanMemoryPoolManager* poolMgr = impl_->GetMemoryPoolManager();
    if (poolMgr)
    {
        VmaPool pool = poolMgr->GetPool(VulkanMemoryPoolType::ConstantBuffers);
        if (pool)
        {
            allocInfo.pool = pool;
        }
    }

    PooledConstantBuffer poolBuffer{};
    poolBuffer.size = size;

    if (vmaCreateBuffer(impl_->GetAllocator(), &bufferInfo, &allocInfo,
                       &poolBuffer.buffer, &poolBuffer.allocation, nullptr) != VK_SUCCESS)
    {
        // Fallback: attempt without pool if pool allocation failed
        if (poolMgr)
        {
            allocInfo.pool = nullptr;
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            if (vmaCreateBuffer(impl_->GetAllocator(), &bufferInfo, &allocInfo,
                               &poolBuffer.buffer, &poolBuffer.allocation, nullptr) != VK_SUCCESS)
            {
                URHO3D_LOGERROR("Failed to create constant buffer pool buffer");
                return false;
            }
        }
        else
        {
            URHO3D_LOGERROR("Failed to create constant buffer pool buffer");
            return false;
        }
    }

    // Map the buffer for persistent writing
    VmaAllocationInfo allocInfoResult;
    vmaGetAllocationInfo(impl_->GetAllocator(), poolBuffer.allocation, &allocInfoResult);
    poolBuffer.mappedData = allocInfoResult.pMappedData;

    if (!poolBuffer.mappedData)
    {
        URHO3D_LOGERROR("Failed to map constant buffer pool buffer");
        vmaDestroyBuffer(impl_->GetAllocator(), poolBuffer.buffer, poolBuffer.allocation);
        return false;
    }

    poolBuffer.currentOffset = 0;
    poolBuffer.isUsed = true;

    poolBuffers_.Push(poolBuffer);

    URHO3D_LOGDEBUG("Created constant buffer pool buffer (size: " +
                   String(size / 1024 / 1024) + " MB, total buffers: " +
                   String(poolBuffers_.Size()) + ")");

    stats_.totalPoolSize += size;
    return true;
}

PooledConstantBuffer* VulkanConstantBufferPool::FindAvailableBuffer(uint32_t requiredSize)
{
    if (!impl_ || requiredSize == 0)
        return nullptr;

    // Look for existing buffer with enough space
    for (auto& buffer : poolBuffers_)
    {
        uint32_t remainingSpace = buffer.size - buffer.currentOffset;
        if (remainingSpace >= requiredSize)
            return &buffer;
    }

    // No buffer has space, create new one if possible
    if (poolBuffers_.Size() < MAX_POOL_BUFFERS)
    {
        uint32_t newBufferSize = poolSizePerBuffer_;
        // If single allocation exceeds pool size, create larger buffer
        if (requiredSize > newBufferSize)
            newBufferSize = ((requiredSize + 65535) / 65536) * 65536;  // Align to 64KB

        if (CreatePoolBuffer(newBufferSize))
        {
            return &poolBuffers_.Back();
        }
    }

    return nullptr;
}

bool VulkanConstantBufferPool::AllocateBuffer(
    const void* data,
    uint32_t dataSize,
    VkBuffer& outBuffer,
    VkDeviceSize& outOffset)
{
    if (!impl_ || !data || dataSize == 0)
        return false;

    // Align allocation to 256-byte boundary (Vulkan requirement for dynamic offsets)
    uint32_t alignedSize = ((dataSize + 255) / 256) * 256;

    PooledConstantBuffer* buffer = FindAvailableBuffer(alignedSize);
    if (!buffer)
    {
        URHO3D_LOGERROR("Constant buffer pool exhausted: requested " + String(dataSize) +
                       " bytes, aligned to " + String(alignedSize));
        return false;
    }

    // Write data to buffer
    std::memcpy((char*)buffer->mappedData + buffer->currentOffset, data, dataSize);

    // Return buffer and offset
    outBuffer = buffer->buffer;
    outOffset = buffer->currentOffset;

    // Update tracking
    buffer->currentOffset += alignedSize;
    currentFrameSize_ += alignedSize;
    stats_.allocationCount++;

    if (currentFrameSize_ > stats_.peakFrameSize)
        stats_.peakFrameSize = currentFrameSize_;

    return true;
}

void VulkanConstantBufferPool::ResetFrameAllocations()
{
    // Reset all buffer offsets for next frame
    for (auto& buffer : poolBuffers_)
    {
        buffer.currentOffset = 0;
    }

    currentFrameSize_ = 0;

    // Phase B Quick Win #11: Evaluate pool utilization and apply dynamic sizing
    // TODO: Implement dynamic sizing algorithm for optimal memory utilization
    // if (dynamicSizingEnabled_)
    //     UpdateDynamicSizing();
}

VulkanConstantBufferPool::PoolStats VulkanConstantBufferPool::GetStatistics() const
{
    PoolStats stats = stats_;

    // Calculate current usage
    stats.allocatedBuffers = poolBuffers_.Size();
    stats.totalPoolSize = 0;
    stats.usedSize = 0;

    for (const auto& buffer : poolBuffers_)
    {
        stats.totalPoolSize += buffer.size;
        stats.usedSize += buffer.currentOffset;
    }

    stats.wastedSize = stats.totalPoolSize - stats.usedSize;

    if (stats.totalPoolSize > 0)
    {
        stats.averageFragmentation = (float)stats.wastedSize / (float)stats.totalPoolSize;
    }

    return stats;
}

uint32_t VulkanConstantBufferPool::GetRecommendedPoolSize() const
{
    /// \brief Phase B Quick Win #11: Recommend optimal buffer size based on history
    /// \details Analyzes last 10 frames to recommend 1.25x peak usage size

    if (frameHistoryLog_.Empty())
        return poolSizePerBuffer_;

    // Find peak usage in history
    uint32_t peakUsage = 0;
    for (uint32_t frameSize : frameHistoryLog_)
    {
        if (frameSize > peakUsage)
            peakUsage = frameSize;
    }

    if (peakUsage == 0)
        return poolSizePerBuffer_;

    // Recommend 1.25x peak usage (25% headroom)
    uint32_t recommendedSize = static_cast<uint32_t>(peakUsage * 1.25f);

    // Align to 1MB boundaries for cleaner sizing
    const uint32_t ALIGNMENT = 1024 * 1024;
    recommendedSize = ((recommendedSize + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

    return recommendedSize;
}

// TODO: Phase B Quick Win #11 - Dynamic sizing not yet exposed in public API
// This method was part of the implementation but is not currently called
// and not declared in the public header. Can be integrated later when needed.
/*
void VulkanConstantBufferPool::UpdateDynamicSizing()
{
    /// \brief Phase B Quick Win #11: Evaluate utilization and auto-adjust pool sizing
    /// \details Called at end of each frame to monitor trends and apply sizing changes

    if (!dynamicSizingEnabled_ || poolBuffers_.Empty())
        return;

    totalFramesProcessed_++;

    // Calculate current frame utilization
    uint32_t totalPoolSize = 0;
    uint32_t usedSize = 0;

    for (const auto& buffer : poolBuffers_)
    {
        totalPoolSize += buffer.size;
        usedSize += buffer.currentOffset;
    }

    // Track frame history (keep last 10 frames)
    if (frameHistoryLog_.Size() >= 10)
        frameHistoryLog_.Erase(frameHistoryLog_.Begin());

    frameHistoryLog_.Push(usedSize);

    if (totalPoolSize == 0)
        return;

    uint32_t utilizationPercent = (usedSize * 100) / totalPoolSize;

    // Check if we should grow the pool
    if (utilizationPercent > growthThreshold_)
    {
        consecutiveHighUtilizationFrames_++;
        consecutiveLowUtilizationFrames_ = 0;

        // Trigger growth if threshold met and enough time has passed
        uint32_t framesSinceAdjustment = totalFramesProcessed_ - lastAdjustmentFrameNumber_;
        if (consecutiveHighUtilizationFrames_ >= growthFrameWindow_ &&
            framesSinceAdjustment >= MIN_FRAMES_BETWEEN_ADJUSTMENTS)
        {
            uint32_t newSize = static_cast<uint32_t>(poolSizePerBuffer_ * MAX_GROWTH_MULTIPLIER);
            poolSizePerBuffer_ = newSize;
            lastAdjustmentFrameNumber_ = totalFramesProcessed_;
            consecutiveHighUtilizationFrames_ = 0;

            URHO3D_LOGINFO("VulkanConstantBufferPool: Auto-grown to " +
                          String(poolSizePerBuffer_ / 1024 / 1024) + "MB (utilization: " +
                          String(utilizationPercent) + "%)");
        }
    }
    // Check if we could shrink the pool
    else if (utilizationPercent < shrinkThreshold_ && poolBuffers_.Size() > 1)
    {
        consecutiveLowUtilizationFrames_++;
        consecutiveHighUtilizationFrames_ = 0;

        // Note: Actual shrinking is deferred until a new frame would fit in smaller buffers
        // This prevents constant resizing due to transient underutilization
    }
    else
    {
        // Reset counters if utilization is in normal range
        consecutiveHighUtilizationFrames_ = 0;
        consecutiveLowUtilizationFrames_ = 0;
    }
}
*/

} // namespace Urho3D

#endif  // URHO3D_VULKAN
