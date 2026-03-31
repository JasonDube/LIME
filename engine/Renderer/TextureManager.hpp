#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace eden {

class VulkanContext;

class TextureManager {
public:
    static constexpr int MAX_TERRAIN_TEXTURES = 32;  // Maximum textures in array

    TextureManager(VulkanContext& context);
    ~TextureManager();

    // Load all DDS textures from a planet folder (e.g., "textures/earth/")
    // All textures must be 1024x1024 DDS files
    void loadTerrainTexturesFromFolder(const std::string& folderPath);

    // Create default textures if none loaded
    void createDefaultTextures();

    // Get number of loaded textures
    int getTextureCount() const { return m_textureCount; }

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }

private:
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void allocateDescriptorSet();
    void createSampler();
    void createTextureArray(const std::vector<std::vector<unsigned char>>& textureData, int width, int height, int layerCount);
    void createDefaultTextureArray();
    void updateDescriptorSet();

    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t layerCount);
    void copyBufferToImageArray(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layerCount);

    // DDS loading helper
    bool loadDDSFile(const std::string& path, std::vector<unsigned char>& outData, int& outWidth, int& outHeight);

    VulkanContext& m_context;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    // Single texture array for all terrain textures
    VkImage m_textureArray = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureArrayView = VK_NULL_HANDLE;

    int m_textureCount = 0;
    VkDeviceSize m_textureMemorySize = 0;  // For VRAM tracking
};

} // namespace eden
