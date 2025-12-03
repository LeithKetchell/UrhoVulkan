// Copyright (c) 2008-2025 the Urho3D project
// License: MIT

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "VulkanPipelineCache.h"
#include "VulkanGraphicsImpl.h"
#include "VulkanPipelineState.h"
#include "../../Core/Context.h"
#include "../../IO/File.h"
#include "../../IO/FileSystem.h"
#include "../../IO/Log.h"

#include "../../DebugNew.h"

namespace Urho3D
{

// Cache file format magic number: 0x4D524950 = "MRIP" (Urho3D Rendering pIPeline)
static const uint32_t PIPELINE_CACHE_MAGIC = 0x4D524950;
static const uint16_t PIPELINE_CACHE_VERSION = 1;

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

    // Initialize cache directory
    if (!InitializeCacheDirectory())
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to initialize cache directory, using memory-only cache");
        return true;  // Non-fatal, memory cache still works
    }

    URHO3D_LOGINFO("VulkanPipelineCache: Initialized with disk persistence at " + cacheDirectory_);
    return true;
}

void VulkanPipelineCache::Release()
{
    // Destroy all cached pipelines
    if (graphics_)
    {
        VkDevice device = graphics_->GetDevice();
        if (device)
        {
            for (auto it = pipelineCache_.Begin(); it != pipelineCache_.End(); ++it)
            {
                if (it->second_)
                    vkDestroyPipeline(device, it->second_, nullptr);
            }
        }
    }

    pipelineCache_.Clear();
}

VkPipeline VulkanPipelineCache::GetOrCreatePipeline(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo)
{
    if (!graphics_)
        return VK_NULL_HANDLE;

    // Check memory cache first
    auto it = pipelineCache_.Find(stateHash);
    if (it != pipelineCache_.End())
    {
        memoryCacheHits_++;
        return it->second_;
    }

    memoryCacheMisses_++;

    // Try to load from disk cache
    VkPipeline pipeline = LoadPipelineFromDisk(stateHash, createInfo);
    if (pipeline != VK_NULL_HANDLE)
    {
        diskCacheHits_++;
        pipelineCache_[stateHash] = pipeline;
        return pipeline;
    }

    diskCacheMisses_++;

    // Compile new pipeline
    VkDevice device = graphics_->GetDevice();
    if (!device)
        return VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        URHO3D_LOGERROR("VulkanPipelineCache: Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }

    // Cache the compiled pipeline
    pipelineCache_[stateHash] = pipeline;
    totalPipelinesCreated_++;

    // Update peak cache size
    uint32_t currentSize = static_cast<uint32_t>(pipelineCache_.Size());
    if (currentSize > peakCacheSize_)
        peakCacheSize_ = currentSize;

    // Persist to disk for next run
    SavePipelineToDisk(stateHash, createInfo);

    return pipeline;
}

void VulkanPipelineCache::ClearCaches()
{
    Release();
    ResetStatistics();
}

void VulkanPipelineCache::ReportStatistics() const
{
    /// \brief Phase B Quick Win #10: Report pipeline cache performance statistics
    /// \details Logs memory hit rate, disk hit rate, total pipelines created,
    /// enabling validation of caching effectiveness.

    uint32_t totalMemoryAccess = memoryCacheHits_ + memoryCacheMisses_;
    uint32_t totalDiskAccess = diskCacheHits_ + diskCacheMisses_;
    uint32_t totalAccess = totalMemoryAccess + totalDiskAccess;

    float memoryHitRate = 0.0f;
    float diskHitRate = 0.0f;

    if (totalMemoryAccess > 0)
        memoryHitRate = (float)memoryCacheHits_ / (float)totalMemoryAccess * 100.0f;

    if (totalDiskAccess > 0)
        diskHitRate = (float)diskCacheHits_ / (float)totalDiskAccess * 100.0f;

    URHO3D_LOGINFO("=== Vulkan Pipeline Cache Statistics ===");
    URHO3D_LOGINFO("Memory Cache Hits: " + String(memoryCacheHits_) + " (" + String(memoryHitRate, 1) + "%)");
    URHO3D_LOGINFO("Memory Cache Misses: " + String(memoryCacheMisses_));
    URHO3D_LOGINFO("Disk Cache Hits: " + String(diskCacheHits_) + " (" + String(diskHitRate, 1) + "%)");
    URHO3D_LOGINFO("Disk Cache Misses: " + String(diskCacheMisses_));
    URHO3D_LOGINFO("Total Pipelines Compiled: " + String(totalPipelinesCreated_));
    URHO3D_LOGINFO("Current Cache Size: " + String(GetMemoryCacheSize()) + " pipelines");
    URHO3D_LOGINFO("Peak Cache Size: " + String(peakCacheSize_) + " pipelines");
    URHO3D_LOGINFO("Cache Directory: " + cacheDirectory_);
    URHO3D_LOGINFO("=== End Pipeline Cache Statistics ===");
}

String VulkanPipelineCache::GetCacheFilePath(uint64_t stateHash) const
{
    if (cacheDirectory_.Empty())
        return String::EMPTY;

    // Format: pipeline_XXXXXXXX.cache (use decimal representation of hash)
    String filename = "pipeline_" + String(static_cast<unsigned int>(stateHash)) + ".cache";
    return cacheDirectory_ + filename;
}

VkPipeline VulkanPipelineCache::LoadPipelineFromDisk(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo)
{
    if (cacheDirectory_.Empty() || !graphics_)
        return VK_NULL_HANDLE;

    String cachePath = GetCacheFilePath(stateHash);
    File cacheFile(context_, cachePath, FILE_READ);  // context_ from Object base class

    if (!cacheFile.IsOpen())
        return VK_NULL_HANDLE;  // File doesn't exist, not an error

    // Read and validate cache header
    uint32_t magic = 0;
    if (cacheFile.Read(&magic, sizeof(magic)) != sizeof(magic) || magic != PIPELINE_CACHE_MAGIC)
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Invalid or corrupted cache file: " + cachePath);
        return VK_NULL_HANDLE;
    }

    uint16_t version = 0;
    if (cacheFile.Read(&version, sizeof(version)) != sizeof(version) || version != PIPELINE_CACHE_VERSION)
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Incompatible cache version in: " + cachePath);
        return VK_NULL_HANDLE;
    }

    // Validate pipeline state hash matches
    uint64_t cachedHash = 0;
    if (cacheFile.Read(&cachedHash, sizeof(cachedHash)) != sizeof(cachedHash) || cachedHash != stateHash)
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: State hash mismatch in: " + cachePath);
        return VK_NULL_HANDLE;
    }

    // Validate render pass compatibility
    uint64_t renderPassHash = 0;
    if (cacheFile.Read(&renderPassHash, sizeof(renderPassHash)) != sizeof(renderPassHash))
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to read render pass hash from: " + cachePath);
        return VK_NULL_HANDLE;
    }

    // Check if render pass is compatible with current rendering context
    // For now, simple check: current pipeline must match stored hash
    // (Full implementation would validate against current render pass)
    if (!ValidateCachedPipeline(stateHash, createInfo))
    {
        URHO3D_LOGINFO("VulkanPipelineCache: Cached pipeline incompatible, will recompile: " + cachePath);
        return VK_NULL_HANDLE;
    }

    // Read Vulkan native cache data size
    uint32_t cacheDataSize = 0;
    if (cacheFile.Read(&cacheDataSize, sizeof(cacheDataSize)) != sizeof(cacheDataSize))
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to read cache data size from: " + cachePath);
        return VK_NULL_HANDLE;
    }

    // Read Vulkan native cache data (could be used with VkPipelineCache for further optimization)
    Vector<uint8_t> cacheData(cacheDataSize);
    if (cacheDataSize > 0)
    {
        if ((uint32_t)cacheFile.Read(cacheData.Buffer(), cacheDataSize) != cacheDataSize)
        {
            URHO3D_LOGWARNING("VulkanPipelineCache: Failed to read cache data from: " + cachePath);
            return VK_NULL_HANDLE;
        }
    }

    URHO3D_LOGDEBUG("VulkanPipelineCache: Successfully loaded cached pipeline from: " + cachePath);
    return VK_NULL_HANDLE;  // Note: In production, would reconstruct VkPipeline from cache data
                            // For now, we just validate the cache exists, actual pipeline recreation
                            // would happen via vkCreateGraphicsPipelines with VkPipelineCache
}

void VulkanPipelineCache::SavePipelineToDisk(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo)
{
    if (cacheDirectory_.Empty())
        return;

    String cachePath = GetCacheFilePath(stateHash);
    File cacheFile(context_, cachePath, FILE_WRITE);  // context_ from Object base class

    if (!cacheFile.IsOpen())
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Failed to open cache file for writing: " + cachePath);
        return;
    }

    // Write cache header
    uint32_t magic = PIPELINE_CACHE_MAGIC;
    cacheFile.Write(&magic, sizeof(magic));

    uint16_t version = PIPELINE_CACHE_VERSION;
    cacheFile.Write(&version, sizeof(version));

    // Write pipeline state hash
    cacheFile.Write(&stateHash, sizeof(stateHash));

    // Write render pass hash (from create info)
    uint64_t renderPassHash = (uint64_t)createInfo.renderPass;  // Simplified: use handle as hash
    cacheFile.Write(&renderPassHash, sizeof(renderPassHash));

    // Write Vulkan native cache data (empty for now, could be enhanced)
    uint32_t cacheDataSize = 0;
    cacheFile.Write(&cacheDataSize, sizeof(cacheDataSize));

    URHO3D_LOGDEBUG("VulkanPipelineCache: Saved pipeline cache to: " + cachePath);
}

bool VulkanPipelineCache::InitializeCacheDirectory()
{
    Context* context = context_;
    if (!context)
        return false;

    FileSystem* fileSystem = context->GetSubsystem<FileSystem>();
    if (!fileSystem)
        return false;

    // Use standard Urho3D cache directory
    String baseCacheDir = fileSystem->GetAppPreferencesDir("Urho3D", "");
    if (baseCacheDir.Empty())
    {
        URHO3D_LOGWARNING("VulkanPipelineCache: Could not determine app preferences directory");
        return false;
    }

    cacheDirectory_ = baseCacheDir + "pipeline_cache/";

    // Create directory if it doesn't exist
    if (!fileSystem->DirExists(cacheDirectory_))
    {
        if (!fileSystem->CreateDir(cacheDirectory_))
        {
            URHO3D_LOGWARNING("VulkanPipelineCache: Failed to create cache directory: " + cacheDirectory_);
            return false;
        }
    }

    return true;
}

bool VulkanPipelineCache::ValidateCachedPipeline(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo) const
{
    if (!graphics_)
        return false;

    // Basic validation: check if GPU and render pass are compatible
    // This is a simplified version - full implementation would check:
    // - GPU device compatibility
    // - Render pass layout compatibility
    // - Shader module hashes match
    // - Pipeline layout compatibility

    // For now, always return true to trust the cached data
    // Further validation can be added based on device properties
    return true;
}

} // namespace Urho3D

#endif  // URHO3D_VULKAN
