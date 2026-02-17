// Copyright (c) 2008-2025 the Urho3D project
// License: MIT

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "VulkanPipelineCache.h"
#include "VulkanGraphicsImpl.h"
#include "../../Core/Context.h"
#include "../../IO/File.h"
#include "../../IO/FileSystem.h"
#include "../../IO/Log.h"

#include "../../DebugNew.h"

namespace Urho3D
{

static const char* CACHE_FILENAME = "pipeline_cache.bin";

VulkanPipelineCache::VulkanPipelineCache(Context* context) :
    Object(context)
{
}

VulkanPipelineCache::~VulkanPipelineCache()
{
    Release();
}

bool VulkanPipelineCache::Initialize(VulkanGraphicsImpl* graphics)
{
    if (!graphics)
        return false;

    graphics_ = graphics;
    VkDevice device = graphics_->GetDevice();
    if (!device)
        return false;

    InitializeCacheDirectory();

    // Load prior cache data from disk (empty vector if no file exists)
    Vector<uint8_t> initialData = LoadCacheDataFromDisk();

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = initialData.Size();
    cacheInfo.pInitialData = initialData.Size() > 0 ? initialData.Buffer() : nullptr;

    if (vkCreatePipelineCache(device, &cacheInfo, nullptr, &vkCache_) != VK_SUCCESS)
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to create VkPipelineCache, using uncached pipelines");
        vkCache_ = VK_NULL_HANDLE;
        return true;  // Non-fatal
    }

    if (initialData.Size() > 0)
        URHO3D_LOGINFO("VulkanPipelineCache: Loaded " + String(initialData.Size()) + " bytes from disk");
    else
        URHO3D_LOGINFO("VulkanPipelineCache: Created empty (first run)");

    return true;
}

void VulkanPipelineCache::Release()
{
    VkDevice device = graphics_ ? graphics_->GetDevice() : nullptr;

    // Save cache data to disk before destroying
    if (device && vkCache_)
        SaveCacheDataToDisk();

    // Destroy all cached VkPipeline objects
    if (device)
    {
        for (auto it = pipelineMap_.Begin(); it != pipelineMap_.End(); ++it)
        {
            if (it->second_)
                vkDestroyPipeline(device, it->second_, nullptr);
        }
    }
    pipelineMap_.Clear();

    // Destroy the VkPipelineCache object
    if (device && vkCache_)
    {
        vkDestroyPipelineCache(device, vkCache_, nullptr);
        vkCache_ = VK_NULL_HANDLE;
    }
}

VkPipeline VulkanPipelineCache::FindCachedPipeline(uint64_t stateHash) const
{
    auto it = pipelineMap_.Find(stateHash);
    if (it != pipelineMap_.End())
        return it->second_;
    return VK_NULL_HANDLE;
}

VkPipeline VulkanPipelineCache::GetOrCreatePipeline(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo)
{
    if (!graphics_)
        return VK_NULL_HANDLE;

    // Check in-memory map first
    auto it = pipelineMap_.Find(stateHash);
    if (it != pipelineMap_.End())
    {
        memoryCacheHits_++;
        return it->second_;
    }

    memoryCacheMisses_++;

    VkDevice device = graphics_->GetDevice();
    if (!device)
        return VK_NULL_HANDLE;

    // Create pipeline with native VkPipelineCache — the driver will use
    // cached compiled ISA from previous runs if the cache blob matches.
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, vkCache_, 1, &createInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("VulkanPipelineCache: Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }

    pipelineMap_[stateHash] = pipeline;
    totalPipelinesCreated_++;

    uint32_t currentSize = static_cast<uint32_t>(pipelineMap_.Size());
    if (currentSize > peakCacheSize_)
        peakCacheSize_ = currentSize;

    return pipeline;
}

void VulkanPipelineCache::ClearCaches()
{
    VkDevice device = graphics_ ? graphics_->GetDevice() : nullptr;
    if (device)
    {
        for (auto it = pipelineMap_.Begin(); it != pipelineMap_.End(); ++it)
        {
            if (it->second_)
                vkDestroyPipeline(device, it->second_, nullptr);
        }
    }
    pipelineMap_.Clear();
    ResetStatistics();
}

void VulkanPipelineCache::ReportStatistics() const
{
    uint32_t totalAccess = memoryCacheHits_ + memoryCacheMisses_;
    float hitRate = totalAccess > 0 ? (float)memoryCacheHits_ / (float)totalAccess * 100.0f : 0.0f;

    URHO3D_LOGINFO("=== Vulkan Pipeline Cache Statistics ===");
    URHO3D_LOGINFO("Memory Hits: " + String(memoryCacheHits_) + " (" + String(hitRate, 1) + "%)");
    URHO3D_LOGINFO("Memory Misses: " + String(memoryCacheMisses_));
    URHO3D_LOGINFO("Pipelines Compiled: " + String(totalPipelinesCreated_));
    URHO3D_LOGINFO("Current Size: " + String(GetMemoryCacheSize()));
    URHO3D_LOGINFO("Peak Size: " + String(peakCacheSize_));
    URHO3D_LOGINFO("VkPipelineCache: " + String(vkCache_ ? "active" : "none"));
    URHO3D_LOGINFO("========================================");
}

String VulkanPipelineCache::GetCacheFilePath() const
{
    if (cacheDirectory_.Empty())
        return String::EMPTY;
    return cacheDirectory_ + CACHE_FILENAME;
}

Vector<uint8_t> VulkanPipelineCache::LoadCacheDataFromDisk()
{
    Vector<uint8_t> data;
    String path = GetCacheFilePath();
    if (path.Empty())
        return data;

    auto* fileSystem = GetSubsystem<FileSystem>();
    if (!fileSystem || !fileSystem->FileExists(path))
        return data;

    File file(context_, path, FILE_READ);
    if (!file.IsOpen())
        return data;

    unsigned size = file.GetSize();
    if (size == 0)
        return data;

    data.Resize(size);
    if (file.Read(data.Buffer(), size) != size)
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Partial read of cache file, starting fresh");
        data.Clear();
    }

    return data;
}

void VulkanPipelineCache::SaveCacheDataToDisk()
{
    if (!graphics_ || !vkCache_)
        return;

    String path = GetCacheFilePath();
    if (path.Empty())
        return;

    VkDevice device = graphics_->GetDevice();

    // Query how much data the driver wants to write
    size_t dataSize = 0;
    if (vkGetPipelineCacheData(device, vkCache_, &dataSize, nullptr) != VK_SUCCESS || dataSize == 0)
        return;

    Vector<uint8_t> data(dataSize);
    if (vkGetPipelineCacheData(device, vkCache_, &dataSize, data.Buffer()) != VK_SUCCESS)
        return;

    File file(context_, path, FILE_WRITE);
    if (!file.IsOpen())
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to write cache file: " + path);
        return;
    }

    file.Write(data.Buffer(), dataSize);
    URHO3D_LOGINFO("VulkanPipelineCache: Saved " + String((unsigned)dataSize) + " bytes to disk");
}

bool VulkanPipelineCache::InitializeCacheDirectory()
{
    auto* fileSystem = GetSubsystem<FileSystem>();
    if (!fileSystem)
        return false;

    String baseCacheDir = fileSystem->GetAppPreferencesDir("Urho3D", "");
    if (baseCacheDir.Empty())
        return false;

    cacheDirectory_ = baseCacheDir + "pipeline_cache/";

    if (!fileSystem->DirExists(cacheDirectory_))
    {
        if (!fileSystem->CreateDir(cacheDirectory_))
        {
            URHO3D_LOGWARNING("VulkanPipelineCache: Failed to create cache directory: " + cacheDirectory_);
            cacheDirectory_.Clear();
            return false;
        }
    }

    return true;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
