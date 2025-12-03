// Copyright (c) 2008-2025 the Urho3D project
// License: MIT

#include "../../Precompiled.h"

#ifdef URHO3D_VULKAN

#include "VulkanMaterialDescriptorManager.h"
#include "VulkanGraphicsImpl.h"
#include "VulkanSPIRVReflect.h"
#include "../../Graphics/Material.h"
#include "../../GraphicsAPI/Texture2D.h"
#include "../../Graphics/Graphics.h"
#include "../../Math/Color.h"
#include "../../Math/Vector3.h"
#include "../../IO/Log.h"

#include "../../DebugNew.h"

namespace Urho3D
{

VulkanMaterialDescriptorManager::VulkanMaterialDescriptorManager(Context* context, VulkanGraphicsImpl* graphics) :
    Object(context),
    graphics_(graphics),
    descriptorSetLayout_(VK_NULL_HANDLE),
    descriptorPool_(VK_NULL_HANDLE)
{
}

VulkanMaterialDescriptorManager::~VulkanMaterialDescriptorManager()
{
    Reset();

    if (graphics_)
    {
        VkDevice device = graphics_->GetDevice();
        if (device)
        {
            // Destroy descriptor pool (Phase 16.1)
            if (descriptorPool_ != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptorPool_, nullptr);

            // Destroy descriptor set layout
            if (descriptorSetLayout_ != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);

            // Phase 24: Destroy cached reflection-based layouts
            for (auto it = layoutCache_.Begin(); it != layoutCache_.End(); ++it)
            {
                if (it->second_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device, it->second_, nullptr);
            }
            layoutCache_.Clear();

            // Phase 25: Clear descriptor set variant cache
            // Note: Individual descriptor sets are freed via descriptor pool
            // Only need to clear the map, not destroy individual sets
            descriptorSetVariantCache_.Clear();
            dynamicDescriptorSetCount_ = 0;
        }
    }

    graphics_ = nullptr;
}

bool VulkanMaterialDescriptorManager::Initialize()
{
    if (!graphics_)
        return false;

    VkDevice device = graphics_->GetDevice();
    if (!device)
        return false;

    // Create descriptor set layout for materials
    if (!CreateDescriptorSetLayout())
    {
        URHO3D_LOGERROR("VulkanMaterialDescriptorManager: Failed to create descriptor set layout");
        return false;
    }

    // Phase 16.1: Create descriptor pool for material descriptor sets
    // Pool size: ~100 descriptor sets with space for UBO + 3 samplers per set
    VkDescriptorPoolSize poolSizes[2];

    // Uniform buffer objects (one per material)
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 100;

    // Combined image samplers (3 per material: diffuse, normal, specular)
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 300;  // 100 materials * 3 samplers

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 100;  // Maximum 100 descriptor sets
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // Allow individual set freeing

    VkResult result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_);
    if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("VulkanMaterialDescriptorManager: Failed to create descriptor pool");
        return false;
    }

    URHO3D_LOGINFO("VulkanMaterialDescriptorManager: Created descriptor pool with capacity for 100 materials");
    URHO3D_LOGINFO("VulkanMaterialDescriptorManager: Initialization complete");
    return true;
}

VkDescriptorSet VulkanMaterialDescriptorManager::GetDescriptor(Material* material)
{
    if (!material || !graphics_)
        return VK_NULL_HANDLE;

    // Calculate key for caching
    MaterialDescriptorKey key;
    key.material_ = material;
    key.textureHash_ = CalculateTextureHash(material);

    // Check if already cached
    auto it = descriptorCache_.Find(key);
    if (it != descriptorCache_.End())
        return it->second_;

    // Create new descriptor set
    VkDescriptorSet descriptorSet = CreateDescriptorSet(material);
    if (descriptorSet == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    // Cache for future access
    descriptorCache_[key] = descriptorSet;
    materialDirtyFlags_[material] = false;

    /// \brief Phase 11: Store initial state hash for dirty detection
    /// Enables automatic change detection on subsequent frames
    materialStateHashes_[material] = CalculateTextureHash(material);

    return descriptorSet;
}

bool VulkanMaterialDescriptorManager::UpdateIfDirty(Material* material)
{
    if (!material)
        return false;

    /// \brief Phase 11 (Quick Win #8): Auto-detect material state changes
    /// Compare current texture hash with stored hash to detect changes
    uint32_t currentHash = CalculateTextureHash(material);
    auto stateIt = materialStateHashes_.Find(material);

    bool stateChanged = (stateIt == materialStateHashes_.End()) || (stateIt->second_ != currentHash);
    bool explicitlyDirty = false;

    // Check if material is explicitly marked dirty
    auto dirtyIt = materialDirtyFlags_.Find(material);
    if (dirtyIt != materialDirtyFlags_.End() && dirtyIt->second_)
        explicitlyDirty = true;

    // If state changed or explicitly marked dirty, update
    if (stateChanged || explicitlyDirty)
    {
        // Force update for this material
        bool updateSuccess = ForceUpdate(material);

        // Store current state hash for next frame comparison
        if (updateSuccess)
        {
            materialStateHashes_[material] = currentHash;
            dirtyMaterialUpdateCount_++;
            if (stateChanged) dirtyMaterialsDetectedThisFrame_++;
        }

        return updateSuccess;
    }

    return false;  // Not dirty, no update needed
}

bool VulkanMaterialDescriptorManager::ForceUpdate(Material* material)
{
    if (!material || !graphics_)
        return false;

    // Find cached descriptor set for this material
    MaterialDescriptorKey key;
    key.material_ = material;
    key.textureHash_ = CalculateTextureHash(material);

    auto it = descriptorCache_.Find(key);
    if (it == descriptorCache_.End())
        return false;  // Descriptor set not cached - can't update

    VkDescriptorSet descriptorSet = it->second_;

    // Phase 14.2: Update material parameter and texture binding
    // 1. Update texture bindings first
    if (!UpdateTextureBindings(material, descriptorSet))
    {
        URHO3D_LOGWARNING("VulkanMaterialDescriptorManager: Failed to update texture bindings during ForceUpdate");
    }

    // 2. Update parameter buffer
    if (!UpdateParameterBuffer(material, descriptorSet))
    {
        URHO3D_LOGWARNING("VulkanMaterialDescriptorManager: Failed to update parameter buffer during ForceUpdate");
    }

    materialDirtyFlags_[material] = false;
    return true;
}

void VulkanMaterialDescriptorManager::Reset()
{
    // Clear cache and dirty flags (don't destroy VkDescriptorSets - they're managed by pool)
    descriptorCache_.Clear();
    materialDirtyFlags_.Clear();

    /// \brief Phase 11: Frame boundary cleanup for dirty tracking
    /// Note: Keep materialStateHashes_ for next frame comparison
    /// Only reset profiling counters for new frame
    dirtyMaterialsDetectedThisFrame_ = 0;
}

bool VulkanMaterialDescriptorManager::CreateDescriptorSetLayout()
{
    if (!graphics_)
        return false;

    VkDevice device = graphics_->GetDevice();
    if (!device)
        return false;

    // Phase 15.2: Create descriptor set layout for materials
    // Layout bindings for material properties and textures
    // Phase 24: Alternative approach - layouts can be auto-generated from shader reflection:
    // For shader programs with reflection data, bindings can be created via SPIRVResourceToBinding()
    // helper function, enabling bytecode-driven descriptor layout rather than manual specification.
    VkDescriptorSetLayoutBinding bindings[4] = {};

    // Binding 0: Material constant buffer (uniform buffer object)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    // Binding 1: Diffuse texture + sampler
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = nullptr;

    // Binding 2: Normal map texture + sampler
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].pImmutableSamplers = nullptr;

    // Binding 3: Specular/roughness texture + sampler
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout_);
    if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("VulkanMaterialDescriptorManager: Failed to create descriptor set layout");
        return false;
    }

    URHO3D_LOGINFO("VulkanMaterialDescriptorManager: Created descriptor set layout with 4 bindings");
    return true;
}

VkDescriptorSet VulkanMaterialDescriptorManager::CreateDescriptorSet(Material* material)
{
    if (!material || !graphics_)
        return VK_NULL_HANDLE;

    VkDevice device = graphics_->GetDevice();
    if (!device || descriptorSetLayout_ == VK_NULL_HANDLE || descriptorPool_ == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    // Phase 16.2: Allocate descriptor set from pool
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;

    VkDescriptorSet descriptorSet;
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS)
    {
        URHO3D_LOGERROR("VulkanMaterialDescriptorManager: Failed to allocate descriptor set for material");
        return VK_NULL_HANDLE;
    }

    /// \brief Quick Win #7: Batch descriptor updates for efficiency
    /// Collect all descriptor writes (texture + parameter) and submit in one call
    /// instead of separate vkUpdateDescriptorSets calls. This reduces CPU/GPU sync overhead.
    Vector<VkWriteDescriptorSet> batchedWrites;
    Vector<VkDescriptorImageInfo> imageInfos;
    VkDescriptorBufferInfo bufferBindInfo{};

    // Phase 16.3: Collect material parameter buffer write
    VulkanMaterialConstants params;
    ExtractMaterialParameters(material, params);

    VulkanConstantBufferPool* constantBufferPool = graphics_->GetConstantBufferPool();
    if (constantBufferPool)
    {
        VkBuffer paramBuffer;
        VkDeviceSize paramOffset;
        if (constantBufferPool->AllocateBuffer(&params, sizeof(VulkanMaterialConstants), paramBuffer, paramOffset))
        {
            bufferBindInfo.buffer = paramBuffer;
            bufferBindInfo.offset = paramOffset;
            bufferBindInfo.range = sizeof(VulkanMaterialConstants);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = 0;  // Binding 0: material parameter UBO
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &bufferBindInfo;
            batchedWrites.Push(write);
        }
    }

    // Phase 16.4: Collect texture/sampler bindings
    VulkanSamplerCache* samplerCache = graphics_->GetSamplerCache();
    if (samplerCache)
    {
        Texture2D* diffuseTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_DIFFUSE));
        Texture2D* normalTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_NORMAL));
        Texture2D* specularTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_SPECULAR));

        // Fallback to default textures
        if (!diffuseTexture) diffuseTexture = graphics_->GetDefaultDiffuseTexture();
        if (!normalTexture) normalTexture = graphics_->GetDefaultNormalTexture();
        if (!specularTexture) specularTexture = graphics_->GetDefaultSpecularTexture();

        Texture2D* textures[] = {diffuseTexture, normalTexture, specularTexture};
        const uint32_t bindingOffsets[] = {1, 2, 3};

        for (uint32_t i = 0; i < 3; ++i)
        {
            if (!textures[i]) continue;

            VkSampler sampler = samplerCache->GetSampler(
                textures[i]->GetFilterMode(),
                textures[i]->GetAddressMode(COORD_U),
                textures[i]->GetAnisotropy()
            );

            if (!sampler) continue;

            // Create descriptor image info explicitly to avoid Push() ambiguity
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = sampler;
            imageInfo.imageView = textures[i]->GetVkImageView();
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos.Push(imageInfo);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = bindingOffsets[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfos.Back();
            batchedWrites.Push(write);
        }
    }

    // Phase 16.5: Submit all batched descriptor updates in one call
    if (!batchedWrites.Empty())
    {
        vkUpdateDescriptorSets(device, batchedWrites.Size(), batchedWrites.Buffer(), 0, nullptr);
        URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Batched " + String(batchedWrites.Size()) +
                       " descriptor writes in single update call");
    }

    URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Created descriptor set for material");
    return descriptorSet;
}

void VulkanMaterialDescriptorManager::ExtractMaterialParameters(Material* material, VulkanMaterialConstants& outParams)
{
    if (!material)
        return;

    // Phase 15.1: Extract material parameters from Urho3D Material for GPU binding
    // Initialize with reasonable defaults
    outParams.diffuseColor_[0] = 1.0f;
    outParams.diffuseColor_[1] = 1.0f;
    outParams.diffuseColor_[2] = 1.0f;
    outParams.diffuseColor_[3] = 1.0f;

    outParams.specularColor_[0] = 1.0f;
    outParams.specularColor_[1] = 1.0f;
    outParams.specularColor_[2] = 1.0f;
    outParams.specularColor_[3] = 1.0f;

    outParams.ambientColor_[0] = 0.1f;
    outParams.ambientColor_[1] = 0.1f;
    outParams.ambientColor_[2] = 0.1f;
    outParams.ambientColor_[3] = 1.0f;

    outParams.metallic_ = 0.0f;
    outParams.roughness_ = 0.5f;
    outParams.shininess_ = 16.0f;

    // Extract shader parameters from material
    const HashMap<StringHash, MaterialShaderParameter>& shaderParams = material->GetShaderParameters();

    // Extract metallic if available
    if (shaderParams.Contains(StringHash("Metallic")))
    {
        const Variant& metallic = material->GetShaderParameter("Metallic");
        if (metallic.GetType() == VAR_FLOAT)
            outParams.metallic_ = metallic.GetFloat();
    }

    // Extract roughness if available
    if (shaderParams.Contains(StringHash("Roughness")))
    {
        const Variant& roughness = material->GetShaderParameter("Roughness");
        if (roughness.GetType() == VAR_FLOAT)
            outParams.roughness_ = roughness.GetFloat();
    }

    // Extract specular (shininess) if available
    if (shaderParams.Contains(StringHash("SpecularExponent")))
    {
        const Variant& shininess = material->GetShaderParameter("SpecularExponent");
        if (shininess.GetType() == VAR_FLOAT)
            outParams.shininess_ = shininess.GetFloat();
    }

    // Clamp values to valid ranges
    outParams.metallic_ = Clamp(outParams.metallic_, 0.0f, 1.0f);
    outParams.roughness_ = Clamp(outParams.roughness_, 0.0f, 1.0f);
    outParams.shininess_ = Max(outParams.shininess_, 1.0f);  // Shininess must be >= 1
}

uint32_t VulkanMaterialDescriptorManager::CalculateTextureHash(Material* material)
{
    if (!material)
        return 0;

    // Phase 15.1: Calculate texture state hash for dirty detection
    // Hash changes when textures are modified, invalidating cached descriptors
    uint32_t hash = (uint32_t)(size_t)material;

    // TODO: Phase 15.2.5 - Include texture pointers in hash
    // Get textures from material and include in hash:
    // - Diffuse texture pointer
    // - Normal texture pointer
    // - Specular texture pointer
    // - Other textures as applicable

    // Include material parameter hash for robustness
    hash ^= material->GetShaderParameterHash();

    return hash;
}

/// Phase 19.1: Helper function to retrieve VkImageView from texture
/// Safely extracts the VkImageView handle from a Texture2D for descriptor binding.
/// Returns VK_NULL_HANDLE if texture is null or not initialized.
static inline VkImageView GetTextureImageView(Texture2D* texture)
{
    if (!texture)
        return VK_NULL_HANDLE;

    return texture->GetVkImageView();
}

/// Phase 24.1: Helper function to generate VkDescriptorSetLayoutBinding from SPIRVResource
/// Converts automatic reflection data from shader bytecode into Vulkan descriptor bindings.
/// Simplifies descriptor layout generation by enabling bytecode-driven rather than manual specification.
static inline VkDescriptorSetLayoutBinding SPIRVResourceToBinding(const SPIRVResource& resource)
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = resource.binding;
    binding.descriptorType = resource.descriptorType;
    binding.descriptorCount = resource.descriptorCount;
    binding.stageFlags = resource.stageFlags;
    binding.pImmutableSamplers = nullptr;

    return binding;
}

/// Phase 25.1: Helper function for dynamic descriptor set allocation
/// Allocates descriptor sets using specified layout from descriptor pool.
/// Enables per-material variant descriptor set generation for shader-specific layouts.
/// Returns VK_NULL_HANDLE on allocation failure (e.g., pool exhausted).
static inline VkDescriptorSet AllocateDynamicDescriptorSet(
    VkDevice device,
    VkDescriptorPool pool,
    VkDescriptorSetLayout layout)
{
    if (!device || pool == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet descriptorSet;
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);

    if (result != VK_SUCCESS)
    {
        URHO3D_LOGWARNING("VulkanMaterialDescriptorManager: Failed to allocate dynamic descriptor set (result=" +
                         String((int)result) + ")");
        return VK_NULL_HANDLE;
    }

    return descriptorSet;
}

bool VulkanMaterialDescriptorManager::UpdateTextureBindings(Material* material, VkDescriptorSet descriptorSet)
{
    if (!material || !descriptorSet || !graphics_)
        return false;

    VkDevice device = graphics_->GetDevice();
    if (!device)
        return false;

    // Phase 18.2-18.3: Update texture bindings in descriptor set
    // Bind material textures to descriptor set bindings 1-3

    // Phase 18.2.1: Get sampler cache from graphics
    // VulkanSamplerCache provides efficient sampler pooling
    // Note: samplerCache integration point - retrieves cached VkSampler objects

    // Phase 18.2.2: Prepare descriptor image writes
    // Array to hold descriptor writes for all texture bindings
    VkWriteDescriptorSet textureWrites[3];
    VkDescriptorImageInfo imageInfos[3];
    uint32_t writeCount = 0;

    // Phase 20: Material Texture Extraction Implementation
    // Phase 21: Default Texture Fallback System
    // Extract textures from material and bind to descriptor set
    // The framework supports three key material textures:
    // - Binding 1: Diffuse/Color texture (TU_DIFFUSE)
    // - Binding 2: Normal map texture (TU_NORMAL)
    // - Binding 3: Specular/Roughness texture (TU_SPECULAR)

    // Phase 22B: Automatic Binding with Default Textures
    // When materials don't have textures, use default 1x1 placeholder textures
    // This eliminates shader complexity and provides consistent behavior

    // Phase 20.1 - Extract textures from material
    // Cast Texture* to Texture2D* since GetTexture returns base Texture class
    Texture2D* diffuseTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_DIFFUSE));
    Texture2D* normalTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_NORMAL));
    Texture2D* specularTexture = dynamic_cast<Texture2D*>(material->GetTexture(TU_SPECULAR));

    // Phase 22.1 - Fallback to default textures if material textures are null
    // Use white 1x1 diffuse texture as fallback
    if (!diffuseTexture)
    {
        diffuseTexture = graphics_->GetDefaultDiffuseTexture();
        if (diffuseTexture)
            URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Using default diffuse texture for material without diffuse map");
    }

    // Use neutral normal map (0.5, 0.5, 1.0) as fallback
    if (!normalTexture)
    {
        normalTexture = graphics_->GetDefaultNormalTexture();
        if (normalTexture)
            URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Using default normal texture for material without normal map");
    }

    // Use white 1x1 specular texture as fallback
    if (!specularTexture)
    {
        specularTexture = graphics_->GetDefaultSpecularTexture();
        if (specularTexture)
            URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Using default specular texture for material without specular map");
    }

    // Phase 20.2 - Get sampler cache for texture sampling
    VulkanSamplerCache* samplerCache = graphics_->GetSamplerCache();
    if (!samplerCache)
        return false;

    // Phase 22B.1 - Bind textures to descriptor set
    // Process each texture type (diffuse, normal, specular)
    // All textures now have values (either from material or fallback defaults)
    const TextureUnit textureUnits[] = {TU_DIFFUSE, TU_NORMAL, TU_SPECULAR};
    Texture2D* textures[] = {diffuseTexture, normalTexture, specularTexture};
    const uint32_t bindingOffsets[] = {1, 2, 3};  // Descriptor bindings 1-3

    for (uint32_t i = 0; i < 3; ++i)
    {
        // Phase 22B: With fallback textures, we always bind (no null check needed)
        if (!textures[i])
            continue;  // Safety check in case fallback texture creation failed

        // Phase 20.3.1 - Get sampler for this texture
        // Use texture's filter and address modes for sampler cache lookup
        VkSampler sampler = samplerCache->GetSampler(
            textures[i]->GetFilterMode(),
            textures[i]->GetAddressMode(COORD_U),
            textures[i]->GetAnisotropy()
        );

        if (!sampler)
            continue;  // Skip if sampler creation failed

        // Phase 20.3.2 - Fill descriptor image info
        imageInfos[writeCount].sampler = sampler;
        imageInfos[writeCount].imageView = GetTextureImageView(textures[i]);
        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Phase 20.3.3 - Prepare descriptor write for this texture
        textureWrites[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        textureWrites[writeCount].dstSet = descriptorSet;
        textureWrites[writeCount].dstBinding = bindingOffsets[i];  // Bindings 1-3
        textureWrites[writeCount].descriptorCount = 1;
        textureWrites[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureWrites[writeCount].pImageInfo = &imageInfos[writeCount];

        writeCount++;
    }

    // Phase 20.3.4 - Submit descriptor updates to GPU
    if (writeCount > 0)
    {
        vkUpdateDescriptorSets(device, writeCount, textureWrites, 0, nullptr);
        URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Bound " + String(writeCount) + " textures to material descriptor");
    }
    else
    {
        URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Material has no textures bound");
    }

    return true;
}

bool VulkanMaterialDescriptorManager::UpdateParameterBuffer(Material* material, VkDescriptorSet descriptorSet)
{
    if (!material || !descriptorSet || !graphics_)
        return false;

    VkDevice device = graphics_->GetDevice();
    if (!device)
        return false;

    // Phase 16.3: Update material parameter constant buffer
    // Extract parameters and prepare for GPU binding
    VulkanMaterialConstants params;
    ExtractMaterialParameters(material, params);

    // Phase 16.3.1: Allocate space from constant buffer pool (Quick Win #6)
    // Using VulkanConstantBufferPool for efficient memory management
    // instead of creating individual buffers per material
    VulkanConstantBufferPool* constantBufferPool = graphics_->GetConstantBufferPool();
    if (!constantBufferPool)
    {
        URHO3D_LOGWARNING("VulkanMaterialDescriptorManager: Constant buffer pool not available");
        return false;
    }

    // Allocate space for material parameters from the pool
    VkBuffer paramBuffer;
    VkDeviceSize paramOffset;
    if (!constantBufferPool->AllocateBuffer(&params, sizeof(VulkanMaterialConstants), paramBuffer, paramOffset))
    {
        URHO3D_LOGWARNING("VulkanMaterialDescriptorManager: Failed to allocate parameter buffer from pool");
        return false;
    }

    // Phase 16.3.2: Update descriptor binding with buffer info
    VkDescriptorBufferInfo bufferBindInfo{};
    bufferBindInfo.buffer = paramBuffer;
    bufferBindInfo.offset = paramOffset;
    bufferBindInfo.range = sizeof(VulkanMaterialConstants);

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 0;  // Binding 0: material parameter UBO
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferBindInfo;

    vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

    URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Updated parameter buffer for material (offset=" +
                   String((int)paramOffset) + ")");
    return true;
}

void VulkanMaterialDescriptorManager::InvalidateVariantCacheForShader(uint64_t shaderHash)
{
    // Phase 26: Invalidate all cached descriptor sets for a shader
    // Called when shader is recompiled or changed to clear stale descriptor sets
    uint32_t invalidatedCount = 0;

    // Iterate through variant cache and remove entries matching shader
    auto it = descriptorSetVariantCache_.Begin();
    while (it != descriptorSetVariantCache_.End())
    {
        // Key format: (shader_hash << 32) | material_hash
        uint64_t cachedShaderHash = it->first_ >> 32;

        if (cachedShaderHash == shaderHash)
        {
            // Match found: remove this descriptor set from cache
            it = descriptorSetVariantCache_.Erase(it);
            invalidatedCount++;
        }
        else
        {
            ++it;
        }
    }

    // Update metrics
    cacheInvalidationCount_++;

    // Log invalidation event
    if (invalidatedCount > 0)
    {
        URHO3D_LOGDEBUG("VulkanMaterialDescriptorManager: Invalidated " + String(invalidatedCount) +
                       " descriptor sets for shader (invalidation #" + String(cacheInvalidationCount_) + ")");
    }
}

}

#endif  // URHO3D_VULKAN
