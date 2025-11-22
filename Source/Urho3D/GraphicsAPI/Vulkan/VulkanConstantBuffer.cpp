//
// Copyright (c) 2008-2024 the Urho3D project.
// License: MIT
//
// Vulkan constant buffer (uniform buffer) implementation (Phase 9)

#ifdef URHO3D_VULKAN

#include "VulkanConstantBuffer.h"
#include "../../Graphics/Graphics.h"
#include "../../Core/Log.h"

namespace Urho3D
{

VulkanConstantBuffer::VulkanConstantBuffer()
{
}

VulkanConstantBuffer::~VulkanConstantBuffer()
{
    Release();
}

bool VulkanConstantBuffer::Create(unsigned size)
{
    Release();

    if (!size)
        return false;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    size_ = size;

    // Create uniform buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    if (vmaCreateBuffer(impl->GetAllocator(), &bufferInfo, &allocInfo,
                       &buffer_, &allocation_, nullptr) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to create constant buffer");
        return false;
    }

    return true;
}

void VulkanConstantBuffer::Release()
{
    if (!buffer_)
        return;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (graphics)
    {
        VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
        if (impl)
        {
            vmaDestroyBuffer(impl->GetAllocator(), buffer_, allocation_);
        }
    }

    buffer_ = nullptr;
    allocation_ = nullptr;
    size_ = 0;
}

bool VulkanConstantBuffer::SetData(const void* data, unsigned size)
{
    if (!buffer_ || !data || size > size_)
        return false;

    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    VulkanGraphicsImpl* impl = graphics->GetImpl_Vulkan();
    if (!impl)
        return false;

    // Map memory and copy data
    void* mapped;
    if (vmaMapMemory(impl->GetAllocator(), allocation_, &mapped) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("Failed to map constant buffer memory");
        return false;
    }

    memcpy(mapped, data, size);
    vmaUnmapMemory(impl->GetAllocator(), allocation_);

    return true;
}

// ============================================
// Constant Buffer Manager
// ============================================

VulkanConstantBufferManager::VulkanConstantBufferManager()
{
}

VulkanConstantBufferManager::~VulkanConstantBufferManager()
{
    Release();
}

bool VulkanConstantBufferManager::Initialize()
{
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;

    impl_ = graphics->GetImpl_Vulkan();
    return impl_ != nullptr;
}

void VulkanConstantBufferManager::Release()
{
    for (auto it = buffers_.Begin(); it != buffers_.End(); ++it)
    {
        delete it->second_;
    }
    buffers_.Clear();
}

VulkanConstantBuffer* VulkanConstantBufferManager::GetOrCreateBuffer(unsigned slot, unsigned size)
{
    if (!impl_)
        return nullptr;

    VulkanConstantBuffer* buffer = buffers_[slot];
    if (!buffer)
    {
        buffer = new VulkanConstantBuffer();
        if (buffer->Create(size))
        {
            buffers_[slot] = buffer;
        }
        else
        {
            delete buffer;
            return nullptr;
        }
    }

    return buffer;
}

bool VulkanConstantBufferManager::SetBufferData(unsigned slot, const void* data, unsigned size)
{
    VulkanConstantBuffer* buffer = GetOrCreateBuffer(slot, size);
    if (!buffer)
        return false;

    return buffer->SetData(data, size);
}

bool VulkanConstantBufferManager::BindToDescriptorSet(VkDescriptorSet descriptorSet,
                                                       VkPipelineLayout pipelineLayout)
{
    if (!impl_ || !descriptorSet || !pipelineLayout)
        return false;

    // Prepare descriptor writes for all constant buffers
    Vector<VkWriteDescriptorSet> writes;
    Vector<VkDescriptorBufferInfo> bufferInfos;

    for (auto it = buffers_.Begin(); it != buffers_.End(); ++it)
    {
        VulkanConstantBuffer* buffer = it->second_;
        if (!buffer || !buffer->GetBuffer())
            continue;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffer->GetBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = buffer->GetSize();
        bufferInfos.Push(bufferInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = it->first_;  // Binding matches slot number
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfos.Back();
        writes.Push(write);
    }

    if (!writes.Empty())
    {
        vkUpdateDescriptorSets(impl_->GetDevice(), writes.Size(), writes.Data(), 0, nullptr);
    }

    return true;
}

}

#endif
