#include "TextureManager.hpp"
#include "VulkanContext.hpp"
#include "Buffer.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <fstream>

namespace eden {

// Simple DDS header structures
#pragma pack(push, 1)
struct DDSPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DDSHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDSPixelFormat pixelFormat;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
#pragma pack(pop)

TextureManager::TextureManager(VulkanContext& context)
    : m_context(context)
{
    createDescriptorSetLayout();
    createDescriptorPool();
    allocateDescriptorSet();
    createSampler();
    createDefaultTextures();
}

TextureManager::~TextureManager() {
    VkDevice device = m_context.getDevice();

    if (m_textureArrayView) vkDestroyImageView(device, m_textureArrayView, nullptr);
    if (m_textureArray) vkDestroyImage(device, m_textureArray, nullptr);
    if (m_textureMemory) {
        Buffer::trackVramFreeHandle(m_textureMemory);
        vkFreeMemory(device, m_textureMemory, nullptr);
    }

    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    if (m_descriptorSetLayout) vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
}

void TextureManager::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;  // Single texture array
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

void TextureManager::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
}

void TextureManager::allocateDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor set");
    }
}

void TextureManager::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture sampler");
    }
}

void TextureManager::createDefaultTextures() {
    createDefaultTextureArray();
    updateDescriptorSet();
}

void TextureManager::createDefaultTextureArray() {
    // Create 4 default textures (grass, sand, rock, snow)
    const int size = 256;
    const int layerCount = 4;

    std::vector<std::vector<unsigned char>> textures(layerCount);

    // Default colors with noise
    struct TexColor { uint8_t r, g, b; };
    TexColor colors[4] = {
        {100, 150, 80},   // Grass green
        {180, 160, 120},  // Sand/dirt
        {120, 110, 100},  // Rock gray
        {240, 240, 250}   // Snow white
    };

    for (int layer = 0; layer < layerCount; layer++) {
        textures[layer].resize(size * size * 4);
        auto& pixels = textures[layer];
        auto& col = colors[layer];

        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                int i = (y * size + x) * 4;
                int noise = ((x * 17 + y * 31) % 20) - 10;
                pixels[i + 0] = static_cast<uint8_t>(std::max(0, std::min(255, col.r + noise)));
                pixels[i + 1] = static_cast<uint8_t>(std::max(0, std::min(255, col.g + noise)));
                pixels[i + 2] = static_cast<uint8_t>(std::max(0, std::min(255, col.b + noise)));
                pixels[i + 3] = 255;
            }
        }
    }

    createTextureArray(textures, size, size, layerCount);
    m_textureCount = layerCount;
}

bool TextureManager::loadDDSFile(const std::string& path, std::vector<unsigned char>& outData, int& outWidth, int& outHeight) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);

    if (fileSize < sizeof(DDSHeader)) {
        return false;
    }

    DDSHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    // Check magic number "DDS "
    if (header.magic != 0x20534444) {
        return false;
    }

    outWidth = static_cast<int>(header.width);
    outHeight = static_cast<int>(header.height);

    // Check for uncompressed RGBA format
    bool isUncompressed = (header.pixelFormat.flags & 0x40) != 0;  // DDPF_RGB
    bool hasAlpha = (header.pixelFormat.flags & 0x01) != 0;  // DDPF_ALPHAPIXELS

    if (isUncompressed && header.pixelFormat.rgbBitCount == 32) {
        // Uncompressed 32-bit RGBA
        size_t dataSize = outWidth * outHeight * 4;
        outData.resize(dataSize);
        file.read(reinterpret_cast<char*>(outData.data()), dataSize);
        return true;
    }

    // For compressed formats (BC1/BC3/BC7), fall back to stb_image
    file.close();

    // Try loading with stb_image (works for many DDS variants)
    int channels;
    unsigned char* pixels = stbi_load(path.c_str(), &outWidth, &outHeight, &channels, STBI_rgb_alpha);
    if (pixels) {
        outData.resize(outWidth * outHeight * 4);
        memcpy(outData.data(), pixels, outData.size());
        stbi_image_free(pixels);
        return true;
    }

    return false;
}

void TextureManager::loadTerrainTexturesFromFolder(const std::string& folderPath) {
    VkDevice device = m_context.getDevice();

    // Clean up existing texture array
    if (m_textureArrayView) {
        vkDestroyImageView(device, m_textureArrayView, nullptr);
        m_textureArrayView = VK_NULL_HANDLE;
    }
    if (m_textureArray) {
        vkDestroyImage(device, m_textureArray, nullptr);
        m_textureArray = VK_NULL_HANDLE;
    }
    if (m_textureMemory) {
        Buffer::trackVramFreeHandle(m_textureMemory);
        vkFreeMemory(device, m_textureMemory, nullptr);
        m_textureMemory = VK_NULL_HANDLE;
    }

    // Find all DDS and PNG files in folder
    std::vector<std::string> texturePaths;

    if (std::filesystem::exists(folderPath) && std::filesystem::is_directory(folderPath)) {
        for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
                    texturePaths.push_back(entry.path().string());
                }
            }
        }
    }

    // Sort alphabetically for consistent ordering
    std::sort(texturePaths.begin(), texturePaths.end());

    if (texturePaths.empty()) {
        // No textures found, use defaults
        createDefaultTextureArray();
        updateDescriptorSet();
        return;
    }

    // Limit to max textures
    if (texturePaths.size() > MAX_TERRAIN_TEXTURES) {
        texturePaths.resize(MAX_TERRAIN_TEXTURES);
    }

    // Load all textures
    std::vector<std::vector<unsigned char>> textureData;
    int expectedWidth = 0, expectedHeight = 0;

    for (const auto& path : texturePaths) {
        std::vector<unsigned char> data;
        int width, height;

        bool loaded = false;

        // Try DDS first
        auto ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".dds") {
            loaded = loadDDSFile(path, data, width, height);
        }

        // Fall back to stb_image for PNG/JPG
        if (!loaded) {
            int channels;
            unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels) {
                data.resize(width * height * 4);
                memcpy(data.data(), pixels, data.size());
                stbi_image_free(pixels);
                loaded = true;
            }
        }

        if (!loaded) {
            continue;
        }

        // First texture sets the expected size
        if (textureData.empty()) {
            expectedWidth = width;
            expectedHeight = height;
        }

        // All textures must be the same size for texture array
        if (width != expectedWidth || height != expectedHeight) {
            continue;
        }

        textureData.push_back(std::move(data));
    }

    if (textureData.empty()) {
        createDefaultTextureArray();
        updateDescriptorSet();
        return;
    }

    createTextureArray(textureData, expectedWidth, expectedHeight, static_cast<int>(textureData.size()));
    m_textureCount = static_cast<int>(textureData.size());
    updateDescriptorSet();
}

void TextureManager::createTextureArray(const std::vector<std::vector<unsigned char>>& textureData, int width, int height, int layerCount) {
    VkDevice device = m_context.getDevice();
    VkDeviceSize layerSize = width * height * 4;
    VkDeviceSize totalSize = layerSize * layerCount;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    Buffer::trackVramAllocHandle(stagingMemory, static_cast<int64_t>(memReqs.size));
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    // Copy all texture data to staging buffer
    void* data;
    vkMapMemory(device, stagingMemory, 0, totalSize, 0, &data);
    for (int i = 0; i < layerCount; i++) {
        memcpy(static_cast<char*>(data) + i * layerSize, textureData[i].data(), layerSize);
    }
    vkUnmapMemory(device, stagingMemory);

    // Create 2D array image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layerCount;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    vkCreateImage(device, &imageInfo, nullptr, &m_textureArray);

    vkGetImageMemoryRequirements(device, m_textureArray, &memReqs);

    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &m_textureMemory);
    Buffer::trackVramAllocHandle(m_textureMemory, static_cast<int64_t>(memReqs.size));
    vkBindImageMemory(device, m_textureArray, m_textureMemory, 0);

    // Transition and copy
    transitionImageLayout(m_textureArray, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount);
    copyBufferToImageArray(stagingBuffer, m_textureArray, width, height, layerCount);
    transitionImageLayout(m_textureArray, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layerCount);

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    Buffer::trackVramFreeHandle(stagingMemory);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view for 2D array
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_textureArray;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layerCount;

    vkCreateImageView(device, &viewInfo, nullptr, &m_textureArrayView);
}

void TextureManager::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t layerCount) {
    VkCommandBuffer commandBuffer = m_context.beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::runtime_error("Unsupported layout transition");
    }

    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_context.endSingleTimeCommands(commandBuffer);
}

void TextureManager::copyBufferToImageArray(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layerCount) {
    VkCommandBuffer commandBuffer = m_context.beginSingleTimeCommands();

    std::vector<VkBufferImageCopy> regions(layerCount);
    VkDeviceSize layerSize = width * height * 4;

    for (uint32_t i = 0; i < layerCount; i++) {
        regions[i].bufferOffset = i * layerSize;
        regions[i].bufferRowLength = 0;
        regions[i].bufferImageHeight = 0;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = i;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageOffset = {0, 0, 0};
        regions[i].imageExtent = {width, height, 1};
    }

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    m_context.endSingleTimeCommands(commandBuffer);
}

void TextureManager::updateDescriptorSet() {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_textureArrayView;
    imageInfo.sampler = m_sampler;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_context.getDevice(), 1, &descriptorWrite, 0, nullptr);
}

} // namespace eden
