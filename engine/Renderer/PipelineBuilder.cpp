#include "PipelineBuilder.hpp"
#include "VulkanContext.hpp"

#include <fstream>
#include <stdexcept>

namespace eden {

PipelineBuilder::PipelineBuilder(VulkanContext& context)
    : m_context(context)
{
}

PipelineBuilder::~PipelineBuilder() = default;

PipelineBuilder& PipelineBuilder::setShaders(const std::string& vertPath, const std::string& fragPath) {
    m_vertShaderPath = vertPath;
    m_fragShaderPath = fragPath;
    return *this;
}

PipelineBuilder& PipelineBuilder::setVertexBinding(uint32_t binding, uint32_t stride,
                                                   VkVertexInputRate inputRate) {
    // Check if binding already exists and update it
    for (auto& b : m_vertexBindings) {
        if (b.binding == binding) {
            b.stride = stride;
            b.inputRate = inputRate;
            return *this;
        }
    }

    VkVertexInputBindingDescription desc{};
    desc.binding = binding;
    desc.stride = stride;
    desc.inputRate = inputRate;
    m_vertexBindings.push_back(desc);
    return *this;
}

PipelineBuilder& PipelineBuilder::addVertexAttribute(uint32_t binding, uint32_t location,
                                                     VkFormat format, uint32_t offset) {
    VkVertexInputAttributeDescription attr{};
    attr.binding = binding;
    attr.location = location;
    attr.format = format;
    attr.offset = offset;
    m_vertexAttributes.push_back(attr);
    return *this;
}

PipelineBuilder& PipelineBuilder::clearVertexInput() {
    m_vertexBindings.clear();
    m_vertexAttributes.clear();
    return *this;
}

PipelineBuilder& PipelineBuilder::setPrimitiveTopology(VkPrimitiveTopology topology) {
    m_topology = topology;
    return *this;
}

PipelineBuilder& PipelineBuilder::setPolygonMode(VkPolygonMode mode) {
    m_polygonMode = mode;
    return *this;
}

PipelineBuilder& PipelineBuilder::setCullMode(VkCullModeFlags mode) {
    m_cullMode = mode;
    return *this;
}

PipelineBuilder& PipelineBuilder::setFrontFace(VkFrontFace frontFace) {
    m_frontFace = frontFace;
    return *this;
}

PipelineBuilder& PipelineBuilder::setLineWidth(float width) {
    m_lineWidth = width;
    return *this;
}

PipelineBuilder& PipelineBuilder::setDepthBias(float constantFactor, float slopeFactor) {
    m_depthBiasEnable = true;
    m_depthBiasConstant = constantFactor;
    m_depthBiasSlope = slopeFactor;
    return *this;
}

PipelineBuilder& PipelineBuilder::disableDepthBias() {
    m_depthBiasEnable = false;
    m_depthBiasConstant = 0.0f;
    m_depthBiasSlope = 0.0f;
    return *this;
}

PipelineBuilder& PipelineBuilder::setDepthTest(bool enable, bool writeEnable) {
    m_depthTestEnable = enable;
    m_depthWriteEnable = writeEnable;
    return *this;
}

PipelineBuilder& PipelineBuilder::setDepthCompareOp(VkCompareOp op) {
    m_depthCompareOp = op;
    return *this;
}

PipelineBuilder& PipelineBuilder::enableBlending(VkBlendFactor srcColor, VkBlendFactor dstColor,
                                                 VkBlendOp colorOp) {
    m_blendEnable = true;
    m_srcColorFactor = srcColor;
    m_dstColorFactor = dstColor;
    m_colorBlendOp = colorOp;
    return *this;
}

PipelineBuilder& PipelineBuilder::enableAlphaBlending() {
    m_blendEnable = true;
    m_srcColorFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    m_dstColorFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_colorBlendOp = VK_BLEND_OP_ADD;
    m_srcAlphaFactor = VK_BLEND_FACTOR_ONE;
    m_dstAlphaFactor = VK_BLEND_FACTOR_ZERO;
    m_alphaBlendOp = VK_BLEND_OP_ADD;
    return *this;
}

PipelineBuilder& PipelineBuilder::disableBlending() {
    m_blendEnable = false;
    return *this;
}

PipelineBuilder& PipelineBuilder::setPushConstantSize(uint32_t size, VkShaderStageFlags stages) {
    m_pushConstantSize = size;
    m_pushConstantStages = stages;
    return *this;
}

PipelineBuilder& PipelineBuilder::addDescriptorSetLayout(VkDescriptorSetLayout layout) {
    m_descriptorSetLayouts.push_back(layout);
    return *this;
}

PipelineBuilder& PipelineBuilder::clearDescriptorSetLayouts() {
    m_descriptorSetLayouts.clear();
    return *this;
}

VkShaderModule PipelineBuilder::loadShaderModule(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + path);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_context.getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module: " + path);
    }

    return shaderModule;
}

PipelineBuilder::Result PipelineBuilder::build(VkRenderPass renderPass, VkExtent2D extent) {
    if (m_vertShaderPath.empty() || m_fragShaderPath.empty()) {
        throw std::runtime_error("Shaders must be set before building pipeline");
    }

    VkDevice device = m_context.getDevice();

    // Load shaders
    VkShaderModule vertShaderModule = loadShaderModule(m_vertShaderPath);
    VkShaderModule fragShaderModule = loadShaderModule(m_fragShaderPath);

    // Shader stages
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    // Vertex input
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(m_vertexBindings.size());
    vertexInputInfo.pVertexBindingDescriptions = m_vertexBindings.empty() ? nullptr : m_vertexBindings.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_vertexAttributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = m_vertexAttributes.empty() ? nullptr : m_vertexAttributes.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = m_topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = m_polygonMode;
    rasterizer.lineWidth = m_lineWidth;
    rasterizer.cullMode = m_cullMode;
    rasterizer.frontFace = m_frontFace;
    rasterizer.depthBiasEnable = m_depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = m_depthBiasConstant;
    rasterizer.depthBiasSlopeFactor = m_depthBiasSlope;
    rasterizer.depthBiasClamp = 0.0f;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = m_depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = m_depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = m_depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = m_blendEnable ? VK_TRUE : VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = m_srcColorFactor;
    colorBlendAttachment.dstColorBlendFactor = m_dstColorFactor;
    colorBlendAttachment.colorBlendOp = m_colorBlendOp;
    colorBlendAttachment.srcAlphaBlendFactor = m_srcAlphaFactor;
    colorBlendAttachment.dstAlphaBlendFactor = m_dstAlphaFactor;
    colorBlendAttachment.alphaBlendOp = m_alphaBlendOp;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Pipeline layout
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(m_descriptorSetLayouts.size());
    layoutInfo.pSetLayouts = m_descriptorSetLayouts.empty() ? nullptr : m_descriptorSetLayouts.data();

    VkPushConstantRange pushConstantRange{};
    if (m_pushConstantSize > 0) {
        pushConstantRange.stageFlags = m_pushConstantStages;
        pushConstantRange.offset = 0;
        pushConstantRange.size = m_pushConstantSize;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;
    }

    VkPipelineLayout pipelineLayout;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        throw std::runtime_error("Failed to create pipeline layout");
    }

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = nullptr;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);

    if (result != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    return {pipeline, pipelineLayout};
}

} // namespace eden
