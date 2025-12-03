// Copyright (c) 2008-2025 the Urho3D project
// License: MIT
//
// Vulkan graphics pipeline disk caching (Phase B Quick Win #10)

#pragma once

#ifdef URHO3D_VULKAN

#include "../../Core/Object.h"
#include "../../Container/HashMap.h"
#include "../../Container/Vector.h"
#include "VulkanPipelineState.h"
#include <vulkan/vulkan.h>

namespace Urho3D
{

class VulkanGraphicsImpl;

/// \brief Manages disk persistence of compiled Vulkan graphics pipelines
/// \details Implements two-tier caching: fast in-memory HashMap for frame access,
/// plus disk persistence to ~/.urho3d/pipeline_cache/ to avoid recompilation across
/// application restarts. Uses same pattern as VulkanShaderCache for consistency.
///
/// **Cache Strategy:**
/// - Memory cache: HashMap<uint64_t, VkPipeline> for O(1) per-frame lookups
/// - Disk cache: Serialized pipeline state + Vulkan cache data
/// - Validation: RenderPass hash + Shader hashes to detect incompatibilities
/// - Statistics: Track memory/disk hits/misses for optimization profiling
///
/// **Typical Usage:**
/// ```cpp
/// pipelineCache = MakeShared<VulkanPipelineCache>(context);
/// pipelineCache->Initialize(graphics);
/// VkPipeline pipeline = pipelineCache->GetOrCreatePipeline(stateHash, createInfo);
/// pipelineCache->ReportStatistics();
/// ```
class VulkanPipelineCache : public Object
{
    URHO3D_OBJECT(VulkanPipelineCache, Object);

public:
    /// \brief Constructor
    /// \param context Urho3D context for file operations
    explicit VulkanPipelineCache(Context* context);

    /// \brief Destructor - flushes pending cache writes
    ~VulkanPipelineCache();

    /// \brief Initialize pipeline cache
    /// \param graphics VulkanGraphicsImpl for device access
    /// \return True if initialization successful
    /// \details Loads existing disk cache files and sets up directories.
    /// Non-fatal if disk cache load fails (falls back to memory-only).
    bool Initialize(VulkanGraphicsImpl* graphics);

    /// \brief Release all cached pipelines
    /// \details Destroys all VkPipeline objects via vkDestroyPipeline.
    /// Called during graphics shutdown.
    void Release();

    /// \brief Get or create pipeline with disk cache support
    /// \param stateHash Hash of VulkanPipelineState (9 graphics state fields)
    /// \param createInfo Vulkan graphics pipeline creation parameters
    /// \return VkPipeline handle, or VK_NULL_HANDLE on failure
    /// \details Checks memory cache first, then disk cache, then compiles new pipeline.
    /// Automatically persists newly compiled pipelines to disk for next run.
    VkPipeline GetOrCreatePipeline(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo);

    /// \brief Clear all caches and statistics
    /// \details Destroys pipelines and resets profiling counters.
    /// Used between scenes or for memory cleanup.
    void ClearCaches();

    /// \brief Report cache statistics for performance profiling
    /// \details Logs memory/disk hit rates, pipeline count, cache efficiency metrics
    void ReportStatistics() const;

    /// \brief Get number of cached pipelines in memory
    uint32_t GetMemoryCacheSize() const { return static_cast<uint32_t>(pipelineCache_.Size()); }

    /// \brief Get disk cache hit count
    uint32_t GetDiskCacheHits() const { return diskCacheHits_; }

    /// \brief Get disk cache miss count
    uint32_t GetDiskCacheMisses() const { return diskCacheMisses_; }

    /// \brief Get memory cache hit count
    uint32_t GetMemoryCacheHits() const { return memoryCacheHits_; }

    /// \brief Get memory cache miss count
    uint32_t GetMemoryCacheMisses() const { return memoryCacheMisses_; }

    /// \brief Reset statistics for new profiling session
    void ResetStatistics()
    {
        diskCacheHits_ = 0;
        diskCacheMisses_ = 0;
        memoryCacheHits_ = 0;
        memoryCacheMisses_ = 0;
    }

private:
    /// \brief Get disk cache file path for pipeline state hash
    /// \param stateHash Pipeline state hash
    /// \return Absolute file path (~/.urho3d/pipeline_cache/pipeline_HASH.cache)
    String GetCacheFilePath(uint64_t stateHash) const;

    /// \brief Load pipeline from disk cache
    /// \param stateHash Pipeline state hash
    /// \param createInfo Pipeline creation parameters for validation
    /// \return VkPipeline handle if disk cache valid, VK_NULL_HANDLE otherwise
    /// \details Checks render pass and shader compatibility before loading.
    /// Returns null if cache invalid/corrupted (triggers recompilation).
    VkPipeline LoadPipelineFromDisk(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo);

    /// \brief Save compiled pipeline to disk cache
    /// \param stateHash Pipeline state hash
    /// \param createInfo Pipeline creation parameters (for embedded metadata)
    /// \details Writes pipeline state + Vulkan cache data synchronously.
    /// Non-fatal if write fails (pipeline remains in memory only).
    void SavePipelineToDisk(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo);

    /// \brief Initialize cache directory structure
    /// \return True if directory exists or created successfully
    /// \details Creates ~/.urho3d/pipeline_cache/ if needed.
    bool InitializeCacheDirectory();

    /// \brief Validate cached pipeline against current GPU state
    /// \param stateHash Cached state identifier
    /// \param createInfo Current pipeline creation parameters
    /// \return True if cache is valid and can be used
    /// \details Checks render pass hash, shader module hashes, etc.
    bool ValidateCachedPipeline(uint64_t stateHash, const VkGraphicsPipelineCreateInfo& createInfo) const;

    /// Vulkan graphics implementation
    VulkanGraphicsImpl* graphics_{nullptr};

    /// In-memory pipeline cache: state hash -> VkPipeline
    HashMap<uint64_t, VkPipeline> pipelineCache_;

    /// Disk cache hit count
    uint32_t diskCacheHits_{0};

    /// Disk cache miss count
    uint32_t diskCacheMisses_{0};

    /// Memory cache hit count
    uint32_t memoryCacheHits_{0};

    /// Memory cache miss count
    uint32_t memoryCacheMisses_{0};

    /// Total pipelines created from scratch (cache misses)
    uint32_t totalPipelinesCreated_{0};

    /// Peak pipeline cache size
    uint32_t peakCacheSize_{0};

    /// Cache directory path
    String cacheDirectory_;
};

} // namespace Urho3D

#endif  // URHO3D_VULKAN
