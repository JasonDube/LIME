#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "eden/Animation.hpp"

namespace eden {

class VulkanContext;

// Skinned vertex format - includes bone influences
struct SkinnedVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 color;
    glm::ivec4 joints;   // Indices of up to 4 bones
    glm::vec4 weights;   // Weights for each bone (must sum to 1.0)

    static VkVertexInputBindingDescription getBindingDescription();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();
};

struct SkinnedModelPushConstants {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 colorAdjust;  // x=hue, y=saturation, z=brightness, w=unused
};

// Stores GPU resources for a skinned model
struct SkinnedModelGPUData {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;

    // Texture
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory = VK_NULL_HANDLE;
    VkImageView textureView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool hasTexture = false;
    int textureWidth = 0;
    int textureHeight = 0;

    // Bone matrices UBO
    VkBuffer boneBuffer = VK_NULL_HANDLE;
    VkDeviceMemory boneMemory = VK_NULL_HANDLE;
    void* boneMappedMemory = nullptr;  // Persistently mapped

    // Skeleton and animations
    std::unique_ptr<Skeleton> skeleton;
    std::vector<AnimationClip> animations;
    AnimationPlayer animPlayer;

};

class SkinnedModelRenderer {
public:
    SkinnedModelRenderer(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent);
    ~SkinnedModelRenderer();

    SkinnedModelRenderer(const SkinnedModelRenderer&) = delete;
    SkinnedModelRenderer& operator=(const SkinnedModelRenderer&) = delete;

    // Create GPU resources for a skinned model and return a handle
    uint32_t createModel(const std::vector<SkinnedVertex>& vertices,
                         const std::vector<uint32_t>& indices,
                         std::unique_ptr<Skeleton> skeleton,
                         std::vector<AnimationClip> animations,
                         const unsigned char* textureData = nullptr,
                         int texWidth = 0, int texHeight = 0);

    // Destroy a model's GPU resources
    void destroyModel(uint32_t handle);

    // Update animation for a model
    void updateAnimation(uint32_t handle, float deltaTime);

    // Play an animation by name
    void playAnimation(uint32_t handle, const std::string& animName, bool loop = true);

    // Stop animation
    void stopAnimation(uint32_t handle);

    // Add animation to an existing model
    void addAnimation(uint32_t handle, const AnimationClip& clip);

    // Get animation names for a model
    std::vector<std::string> getAnimationNames(uint32_t handle) const;

    // Render a skinned model
    void render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                uint32_t modelHandle, const glm::mat4& modelMatrix,
                float hueShift = 0.0f, float saturation = 1.0f, float brightness = 1.0f);

    // Get model data
    SkinnedModelGPUData* getModelData(uint32_t handle);

    // Recreate pipeline for swapchain resize
    void recreatePipeline(VkRenderPass renderPass, VkExtent2D extent);

private:
    void createPipeline(VkRenderPass renderPass, VkExtent2D extent);
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDefaultTexture();

    void createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties, VkImage& image,
                     VkDeviceMemory& memory);
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

    VulkanContext& m_context;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    // Default white texture
    VkImage m_defaultTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_defaultTextureMemory = VK_NULL_HANDLE;
    VkImageView m_defaultTextureView = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;

    // Model storage
    std::unordered_map<uint32_t, SkinnedModelGPUData> m_models;
    uint32_t m_nextHandle = 1;
};

} // namespace eden
