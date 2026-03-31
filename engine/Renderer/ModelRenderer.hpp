#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace eden {

class VulkanContext;
class SceneObject;

// Simpler vertex format for imported models
struct ModelVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 color;  // RGBA

    static VkVertexInputBindingDescription getBindingDescription();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();
};

struct ModelPushConstants {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 colorAdjust;  // x=hue, y=saturation, z=brightness, w=alpha (0=opaque, >0=x-ray)
};

struct WireframePushConstants {
    glm::mat4 mvp;
    glm::vec4 wireColor;
};

struct SelectionPushConstants {
    glm::mat4 mvp;
    glm::vec4 selectionColor;
};

// Stores GPU resources for a model
struct ModelGPUData {
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
};

class ModelRenderer {
public:
    ModelRenderer(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent);
    ~ModelRenderer();

    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    // Create GPU resources for a model and return a handle
    uint32_t createModel(const std::vector<ModelVertex>& vertices,
                         const std::vector<uint32_t>& indices,
                         const unsigned char* textureData = nullptr,
                         int texWidth = 0, int texHeight = 0);

    // Destroy a model's GPU resources
    void destroyModel(uint32_t handle);

    // Batch destroy — single waitIdle then free all handles
    void destroyModels(const std::vector<uint32_t>& handles);

    // Update vertex buffer with new vertex data (for freeze transform)
    void updateModelBuffer(uint32_t handle, const std::vector<ModelVertex>& vertices);

    // Render a model with optional color adjustments
    void render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                uint32_t modelHandle, const glm::mat4& modelMatrix,
                float hueShift = 0.0f, float saturation = 1.0f, float brightness = 1.0f,
                bool twoSided = false);

    // Render model wireframe with solid color
    void renderWireframe(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                         uint32_t modelHandle, const glm::mat4& modelMatrix,
                         const glm::vec3& color);

    // Render selected faces with semi-transparent overlay
    void renderSelection(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                         uint32_t modelHandle, const glm::mat4& modelMatrix,
                         const std::vector<uint32_t>& selectedFaces,
                         const glm::vec4& color);

    // Get model data
    ModelGPUData* getModelData(uint32_t handle);

    // Render lines with depth testing (for grids, guides, etc.)
    // lines: pairs of points (line0_start, line0_end, line1_start, line1_end, ...)
    void renderLines(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                     const std::vector<glm::vec3>& lines, const glm::vec3& color);
    void renderLines(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                     const std::vector<glm::vec3>& lines, const glm::vec4& color);

    // Render points with depth testing (for vertices)
    void renderPoints(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                      const std::vector<glm::vec3>& points, const glm::vec3& color, float pointSize);

    // Update model texture from CPU data (for painting)
    void updateTexture(uint32_t handle, const unsigned char* data, int width, int height);

    // Destroy texture for a model (keeps mesh data)
    void destroyTexture(uint32_t handle);

    // Update model vertex buffer (for UV regeneration)
    void updateVertices(uint32_t handle, const std::vector<ModelVertex>& vertices);

    // Recreate pipeline for swapchain resize (preserves model data)
    void recreatePipeline(VkRenderPass renderPass, VkExtent2D extent);

private:
    void createPipeline(VkRenderPass renderPass, VkExtent2D extent);
    void createWireframePipeline(VkRenderPass renderPass, VkExtent2D extent);
    void createSelectionPipeline(VkRenderPass renderPass, VkExtent2D extent);
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
    VkPipeline m_twoSidedPipeline = VK_NULL_HANDLE;  // Same as m_pipeline but no backface culling
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    // Wireframe pipeline
    VkPipelineLayout m_wireframePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_wireframePipeline = VK_NULL_HANDLE;
    VkPipeline m_linePipeline = VK_NULL_HANDLE;  // LINE_LIST topology for renderLines
    VkPipeline m_pointPipeline = VK_NULL_HANDLE;  // POINT_LIST topology for renderPoints

    // Selection pipeline (for rendering selected faces)
    VkPipelineLayout m_selectionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_selectionPipeline = VK_NULL_HANDLE;

    // Default white texture for untextured models
    VkImage m_defaultTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_defaultTextureMemory = VK_NULL_HANDLE;
    VkImageView m_defaultTextureView = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    VkDescriptorSet m_defaultDescriptorSet = VK_NULL_HANDLE;

    // Model storage
    std::unordered_map<uint32_t, ModelGPUData> m_models;
    uint32_t m_nextHandle = 1;

    // Line rendering buffers (SEPARATE buffers for each renderLines call per frame)
    static constexpr size_t NUM_LINE_BUFFERS = 32;  // Support up to 32 renderLines calls per frame (split view doubles usage: grid + axes + wireframe + selected edges per viewport)
    VkBuffer m_lineBuffers[NUM_LINE_BUFFERS] = {};
    VkDeviceMemory m_lineMemories[NUM_LINE_BUFFERS] = {};
    void* m_lineMappedMemories[NUM_LINE_BUFFERS] = {};
    size_t m_currentLineBuffer = 0;

    // Point rendering buffers (SEPARATE buffers for each renderPoints call per frame)
    static constexpr size_t NUM_POINT_BUFFERS = 4;  // Support up to 4 renderPoints calls per frame
    VkBuffer m_pointBuffers[NUM_POINT_BUFFERS] = {};
    VkDeviceMemory m_pointMemories[NUM_POINT_BUFFERS] = {};
    void* m_pointMappedMemories[NUM_POINT_BUFFERS] = {};
    size_t m_currentPointBuffer = 0;  // Cycles through buffers each call

    static constexpr size_t MAX_LINE_VERTICES = 524288;  // 512K - enough for high-poly wireframe

    // Selection batch rendering buffer (for batching renderSelection draw calls)
    static constexpr size_t NUM_SELECTION_BUFFERS = 2;
    VkBuffer m_selectionIndexBuffers[NUM_SELECTION_BUFFERS] = {};
    VkDeviceMemory m_selectionIndexMemories[NUM_SELECTION_BUFFERS] = {};
    void* m_selectionIndexMapped[NUM_SELECTION_BUFFERS] = {};
    size_t m_currentSelectionBuffer = 0;
    static constexpr size_t MAX_SELECTION_INDICES = 1048576;  // 1M indices (~333K faces)
};

} // namespace eden
