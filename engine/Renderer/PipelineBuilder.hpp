#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace eden {

class VulkanContext;

/**
 * Fluent builder for Vulkan graphics pipelines.
 * Handles common boilerplate and provides sensible defaults.
 *
 * Usage:
 *   auto [pipeline, layout] = PipelineBuilder(context)
 *       .setShaders("shaders/model.vert.spv", "shaders/model.frag.spv")
 *       .addVertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position))
 *       .addVertexAttribute(0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal))
 *       .setVertexBinding(0, sizeof(Vertex))
 *       .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
 *       .setCullMode(VK_CULL_MODE_BACK_BIT)
 *       .setDepthTest(true, true)
 *       .setPushConstantSize(sizeof(PushConstants))
 *       .build(renderPass, extent);
 */
class PipelineBuilder {
public:
    struct Result {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
    };

    explicit PipelineBuilder(VulkanContext& context);
    ~PipelineBuilder();

    // Non-copyable
    PipelineBuilder(const PipelineBuilder&) = delete;
    PipelineBuilder& operator=(const PipelineBuilder&) = delete;

    // Shader configuration
    PipelineBuilder& setShaders(const std::string& vertPath, const std::string& fragPath);

    // Vertex input configuration
    PipelineBuilder& setVertexBinding(uint32_t binding, uint32_t stride,
                                      VkVertexInputRate inputRate = VK_VERTEX_INPUT_RATE_VERTEX);
    PipelineBuilder& addVertexAttribute(uint32_t binding, uint32_t location,
                                        VkFormat format, uint32_t offset);
    PipelineBuilder& clearVertexInput();  // Clear all bindings and attributes

    // Input assembly
    PipelineBuilder& setPrimitiveTopology(VkPrimitiveTopology topology);

    // Rasterization
    PipelineBuilder& setPolygonMode(VkPolygonMode mode);
    PipelineBuilder& setCullMode(VkCullModeFlags mode);
    PipelineBuilder& setFrontFace(VkFrontFace frontFace);
    PipelineBuilder& setLineWidth(float width);
    PipelineBuilder& setDepthBias(float constantFactor, float slopeFactor);
    PipelineBuilder& disableDepthBias();

    // Depth/stencil
    PipelineBuilder& setDepthTest(bool enable, bool writeEnable);
    PipelineBuilder& setDepthCompareOp(VkCompareOp op);

    // Color blending
    PipelineBuilder& enableBlending(VkBlendFactor srcColor, VkBlendFactor dstColor,
                                    VkBlendOp colorOp = VK_BLEND_OP_ADD);
    PipelineBuilder& enableAlphaBlending();  // Standard alpha blending preset
    PipelineBuilder& disableBlending();

    // Pipeline layout configuration
    PipelineBuilder& setPushConstantSize(uint32_t size,
                                         VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    PipelineBuilder& addDescriptorSetLayout(VkDescriptorSetLayout layout);
    PipelineBuilder& clearDescriptorSetLayouts();

    // Build the pipeline
    Result build(VkRenderPass renderPass, VkExtent2D extent);

private:
    VkShaderModule loadShaderModule(const std::string& path);

    VulkanContext& m_context;

    // Shader state
    std::string m_vertShaderPath;
    std::string m_fragShaderPath;

    // Vertex input state
    std::vector<VkVertexInputBindingDescription> m_vertexBindings;
    std::vector<VkVertexInputAttributeDescription> m_vertexAttributes;

    // Input assembly
    VkPrimitiveTopology m_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Rasterization
    VkPolygonMode m_polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags m_cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace m_frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    float m_lineWidth = 1.0f;
    bool m_depthBiasEnable = false;
    float m_depthBiasConstant = 0.0f;
    float m_depthBiasSlope = 0.0f;

    // Depth/stencil
    bool m_depthTestEnable = true;
    bool m_depthWriteEnable = true;
    VkCompareOp m_depthCompareOp = VK_COMPARE_OP_LESS;

    // Color blending
    bool m_blendEnable = false;
    VkBlendFactor m_srcColorFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor m_dstColorFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp m_colorBlendOp = VK_BLEND_OP_ADD;
    VkBlendFactor m_srcAlphaFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor m_dstAlphaFactor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp m_alphaBlendOp = VK_BLEND_OP_ADD;

    // Pipeline layout
    uint32_t m_pushConstantSize = 0;
    VkShaderStageFlags m_pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::vector<VkDescriptorSetLayout> m_descriptorSetLayouts;
};

} // namespace eden
