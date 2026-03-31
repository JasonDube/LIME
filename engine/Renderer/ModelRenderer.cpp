#include "ModelRenderer.hpp"
#include "VulkanContext.hpp"
#include "Buffer.hpp"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <iostream>

namespace eden {

VkVertexInputBindingDescription ModelVertex::getBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ModelVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

std::vector<VkVertexInputAttributeDescription> ModelVertex::getAttributeDescriptions() {
    std::vector<VkVertexInputAttributeDescription> attrs(4);

    // Position
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(ModelVertex, position);

    // Normal
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(ModelVertex, normal);

    // TexCoord
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = offsetof(ModelVertex, texCoord);

    // Color
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[3].offset = offsetof(ModelVertex, color);

    return attrs;
}

ModelRenderer::ModelRenderer(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent)
    : m_context(context)
{
    createDescriptorSetLayout();
    createDescriptorPool();
    createPipeline(renderPass, extent);
    createWireframePipeline(renderPass, extent);
    createSelectionPipeline(renderPass, extent);
    createDefaultTexture();

    // Create MULTIPLE line buffers for renderLines (fixes buffer overwrite when multiple renderLines calls per frame)
    VkDeviceSize lineBufferSize = MAX_LINE_VERTICES * sizeof(ModelVertex);
    for (size_t i = 0; i < NUM_LINE_BUFFERS; i++) {
        m_context.createBuffer(lineBufferSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              m_lineBuffers[i], m_lineMemories[i]);
        vkMapMemory(m_context.getDevice(), m_lineMemories[i], 0, lineBufferSize, 0, &m_lineMappedMemories[i]);
    }
    m_currentLineBuffer = 0;

    // Create MULTIPLE point buffers for renderPoints (fixes buffer overwrite when multiple renderPoints calls per frame)
    VkDeviceSize pointBufferSize = MAX_LINE_VERTICES * sizeof(ModelVertex);
    for (size_t i = 0; i < NUM_POINT_BUFFERS; i++) {
        m_context.createBuffer(pointBufferSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              m_pointBuffers[i], m_pointMemories[i]);
        vkMapMemory(m_context.getDevice(), m_pointMemories[i], 0, pointBufferSize, 0, &m_pointMappedMemories[i]);
    }
    m_currentPointBuffer = 0;

    // Create selection index buffers for batched face rendering
    VkDeviceSize selectionBufferSize = MAX_SELECTION_INDICES * sizeof(uint32_t);
    for (size_t i = 0; i < NUM_SELECTION_BUFFERS; i++) {
        m_context.createBuffer(selectionBufferSize,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              m_selectionIndexBuffers[i], m_selectionIndexMemories[i]);
        vkMapMemory(m_context.getDevice(), m_selectionIndexMemories[i], 0, selectionBufferSize, 0, &m_selectionIndexMapped[i]);
    }
    m_currentSelectionBuffer = 0;
}

ModelRenderer::~ModelRenderer() {
    VkDevice device = m_context.getDevice();

    // Destroy all models
    for (auto& [handle, data] : m_models) {
        if (data.vertexBuffer) vkDestroyBuffer(device, data.vertexBuffer, nullptr);
        if (data.vertexMemory) { Buffer::trackVramFreeHandle(data.vertexMemory); vkFreeMemory(device, data.vertexMemory, nullptr); }
        if (data.indexBuffer) vkDestroyBuffer(device, data.indexBuffer, nullptr);
        if (data.indexMemory) { Buffer::trackVramFreeHandle(data.indexMemory); vkFreeMemory(device, data.indexMemory, nullptr); }
        if (data.textureView) vkDestroyImageView(device, data.textureView, nullptr);
        if (data.textureImage) vkDestroyImage(device, data.textureImage, nullptr);
        if (data.textureMemory) { Buffer::trackVramFreeHandle(data.textureMemory); vkFreeMemory(device, data.textureMemory, nullptr); }
        if (data.textureSampler) vkDestroySampler(device, data.textureSampler, nullptr);
    }

    // Destroy default texture
    if (m_defaultTextureView) vkDestroyImageView(device, m_defaultTextureView, nullptr);
    if (m_defaultTexture) vkDestroyImage(device, m_defaultTexture, nullptr);
    if (m_defaultTextureMemory) { Buffer::trackVramFreeHandle(m_defaultTextureMemory); vkFreeMemory(device, m_defaultTextureMemory, nullptr); }
    if (m_defaultSampler) vkDestroySampler(device, m_defaultSampler, nullptr);

    if (m_descriptorPool) vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    if (m_descriptorSetLayout) vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_twoSidedPipeline) vkDestroyPipeline(device, m_twoSidedPipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_wireframePipeline) vkDestroyPipeline(device, m_wireframePipeline, nullptr);
    if (m_linePipeline) vkDestroyPipeline(device, m_linePipeline, nullptr);
    if (m_pointPipeline) vkDestroyPipeline(device, m_pointPipeline, nullptr);
    if (m_wireframePipelineLayout) vkDestroyPipelineLayout(device, m_wireframePipelineLayout, nullptr);
    if (m_selectionPipeline) vkDestroyPipeline(device, m_selectionPipeline, nullptr);
    if (m_selectionPipelineLayout) vkDestroyPipelineLayout(device, m_selectionPipelineLayout, nullptr);

    // Destroy all line buffers
    for (size_t i = 0; i < NUM_LINE_BUFFERS; i++) {
        if (m_lineMappedMemories[i]) vkUnmapMemory(device, m_lineMemories[i]);
        if (m_lineBuffers[i]) vkDestroyBuffer(device, m_lineBuffers[i], nullptr);
        if (m_lineMemories[i]) { Buffer::trackVramFreeHandle(m_lineMemories[i]); vkFreeMemory(device, m_lineMemories[i], nullptr); }
    }

    // Destroy all point buffers
    for (size_t i = 0; i < NUM_POINT_BUFFERS; i++) {
        if (m_pointMappedMemories[i]) vkUnmapMemory(device, m_pointMemories[i]);
        if (m_pointBuffers[i]) vkDestroyBuffer(device, m_pointBuffers[i], nullptr);
        if (m_pointMemories[i]) { Buffer::trackVramFreeHandle(m_pointMemories[i]); vkFreeMemory(device, m_pointMemories[i], nullptr); }
    }

    // Destroy selection index buffers
    for (size_t i = 0; i < NUM_SELECTION_BUFFERS; i++) {
        if (m_selectionIndexMapped[i]) vkUnmapMemory(device, m_selectionIndexMemories[i]);
        if (m_selectionIndexBuffers[i]) vkDestroyBuffer(device, m_selectionIndexBuffers[i], nullptr);
        if (m_selectionIndexMemories[i]) { Buffer::trackVramFreeHandle(m_selectionIndexMemories[i]); vkFreeMemory(device, m_selectionIndexMemories[i], nullptr); }
    }
}

void ModelRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorCount = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create model descriptor set layout");
    }
}

void ModelRenderer::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1000;  // Support up to 1000 textured models

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1000;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create model descriptor pool");
    }
}

void ModelRenderer::createPipeline(VkRenderPass renderPass, VkExtent2D extent) {
    auto vertCode = m_context.readFile("shaders/model.vert.spv");
    auto fragCode = m_context.readFile("shaders/model.frag.spv");

    VkShaderModule vertModule = m_context.createShaderModule(vertCode);
    VkShaderModule fragModule = m_context.createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    auto bindingDesc = ModelVertex::getBindingDescription();
    auto attrDescs = ModelVertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlend{};
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlend;

    // Enable dynamic viewport and scissor for split view support
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ModelPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create model pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create model pipeline");
    }

    // Create x-ray pipeline (no backface culling, no depth write, alpha blending)
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    depthStencil.depthWriteEnable = VK_FALSE;  // Don't write to depth buffer (see-through)
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_twoSidedPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create x-ray model pipeline");
    }

    vkDestroyShaderModule(m_context.getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_context.getDevice(), fragModule, nullptr);
}

void ModelRenderer::createWireframePipeline(VkRenderPass renderPass, VkExtent2D extent) {
    auto vertCode = m_context.readFile("shaders/wireframe.vert.spv");
    auto fragCode = m_context.readFile("shaders/wireframe.frag.spv");

    VkShaderModule vertModule = m_context.createShaderModule(vertCode);
    VkShaderModule fragModule = m_context.createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    auto bindingDesc = ModelVertex::getBindingDescription();
    auto attrDescs = ModelVertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_LINE;  // Wireframe mode
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // Show all edges
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;  // Don't write depth for wireframe
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // Allow drawing on same depth

    VkPipelineColorBlendAttachmentState colorBlend{};
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlend;

    // Enable dynamic viewport and scissor for split view support
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(WireframePushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 0;  // No descriptors for wireframe
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_wireframePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create wireframe pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_wireframePipelineLayout;
    pipelineInfo.renderPass = renderPass;

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_wireframePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create wireframe pipeline");
    }

    // Create line pipeline (LINE_LIST topology for renderLines)
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;  // Not wireframe mode, we're drawing actual lines
    rasterizer.lineWidth = 3.0f;  // Thicker lines for better visibility
    rasterizer.depthBiasEnable = VK_TRUE;  // Enable depth bias to prevent Z-fighting
    rasterizer.depthBiasConstantFactor = -50.0f;  // Push lines toward camera
    rasterizer.depthBiasSlopeFactor = -10.0f;
    depthStencil.depthWriteEnable = VK_TRUE;  // Write depth so lines occlude properly

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_linePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create line pipeline");
    }

    // Create point pipeline (POINT_LIST topology for renderPoints)
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pointPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create point pipeline");
    }

    vkDestroyShaderModule(m_context.getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_context.getDevice(), fragModule, nullptr);
}

void ModelRenderer::createSelectionPipeline(VkRenderPass renderPass, VkExtent2D extent) {
    auto vertCode = m_context.readFile("shaders/selection.vert.spv");
    auto fragCode = m_context.readFile("shaders/selection.frag.spv");

    VkShaderModule vertModule = m_context.createShaderModule(vertCode);
    VkShaderModule fragModule = m_context.createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    auto bindingDesc = ModelVertex::getBindingDescription();
    auto attrDescs = ModelVertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // Show both sides
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Enable alpha blending for semi-transparent selection overlay
    VkPipelineColorBlendAttachmentState colorBlend{};
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlend;

    // Enable dynamic viewport and scissor for split view support
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(SelectionPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 0;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_selectionPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create selection pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_selectionPipelineLayout;
    pipelineInfo.renderPass = renderPass;

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_selectionPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create selection pipeline");
    }

    vkDestroyShaderModule(m_context.getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_context.getDevice(), fragModule, nullptr);
}

void ModelRenderer::createDefaultTexture() {
    // Create a simple 1x1 white texture
    uint32_t white = 0xFFFFFFFF;

    createImage(1, 1, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_defaultTexture, m_defaultTextureMemory);

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    m_context.createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(m_context.getDevice(), stagingMemory, 0, 4, 0, &data);
    memcpy(data, &white, 4);
    vkUnmapMemory(m_context.getDevice(), stagingMemory);

    transitionImageLayout(m_defaultTexture, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer, m_defaultTexture, 1, 1);
    transitionImageLayout(m_defaultTexture, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(m_context.getDevice(), stagingBuffer, nullptr);
    Buffer::trackVramFreeHandle(stagingMemory);
    vkFreeMemory(m_context.getDevice(), stagingMemory, nullptr);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_defaultTexture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &m_defaultTextureView);

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_defaultSampler);

    // Create descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, &m_defaultDescriptorSet);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_defaultTextureView;
    imageInfo.sampler = m_defaultSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_defaultDescriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
}

uint32_t ModelRenderer::createModel(const std::vector<ModelVertex>& vertices,
                                     const std::vector<uint32_t>& indices,
                                     const unsigned char* textureData,
                                     int texWidth, int texHeight) {
    uint32_t handle = m_nextHandle++;
    ModelGPUData& data = m_models[handle];

    // Create vertex buffer
    VkDeviceSize vertexSize = sizeof(ModelVertex) * vertices.size();
    m_context.createBuffer(vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           data.vertexBuffer, data.vertexMemory);

    void* mapped;
    vkMapMemory(m_context.getDevice(), data.vertexMemory, 0, vertexSize, 0, &mapped);
    memcpy(mapped, vertices.data(), vertexSize);
    vkUnmapMemory(m_context.getDevice(), data.vertexMemory);

    data.vertexCount = static_cast<uint32_t>(vertices.size());

    // Create index buffer
    VkDeviceSize indexSize = sizeof(uint32_t) * indices.size();
    m_context.createBuffer(indexSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           data.indexBuffer, data.indexMemory);

    vkMapMemory(m_context.getDevice(), data.indexMemory, 0, indexSize, 0, &mapped);
    memcpy(mapped, indices.data(), indexSize);
    vkUnmapMemory(m_context.getDevice(), data.indexMemory);

    data.indexCount = static_cast<uint32_t>(indices.size());

    // Create texture if provided
    if (textureData && texWidth > 0 && texHeight > 0) {
        VkDeviceSize texSize = texWidth * texHeight * 4;

        createImage(texWidth, texHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, data.textureImage, data.textureMemory);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        m_context.createBuffer(texSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               stagingBuffer, stagingMemory);

        vkMapMemory(m_context.getDevice(), stagingMemory, 0, texSize, 0, &mapped);
        memcpy(mapped, textureData, texSize);
        vkUnmapMemory(m_context.getDevice(), stagingMemory);

        transitionImageLayout(data.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(stagingBuffer, data.textureImage, texWidth, texHeight);
        transitionImageLayout(data.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(m_context.getDevice(), stagingBuffer, nullptr);
        Buffer::trackVramFreeHandle(stagingMemory);
        vkFreeMemory(m_context.getDevice(), stagingMemory, nullptr);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = data.textureImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &data.textureView);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &data.textureSampler);

        // Create descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_descriptorSetLayout;

        vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, &data.descriptorSet);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = data.textureView;
        imageInfo.sampler = data.textureSampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = data.descriptorSet;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
        data.hasTexture = true;
        data.textureWidth = texWidth;
        data.textureHeight = texHeight;
    }

    return handle;
}

void ModelRenderer::destroyModel(uint32_t handle) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    VkDevice device = m_context.getDevice();
    ModelGPUData& data = it->second;

    m_context.waitIdle();

    if (data.vertexBuffer) vkDestroyBuffer(device, data.vertexBuffer, nullptr);
    if (data.vertexMemory) { Buffer::trackVramFreeHandle(data.vertexMemory); vkFreeMemory(device, data.vertexMemory, nullptr); }
    if (data.indexBuffer) vkDestroyBuffer(device, data.indexBuffer, nullptr);
    if (data.indexMemory) { Buffer::trackVramFreeHandle(data.indexMemory); vkFreeMemory(device, data.indexMemory, nullptr); }
    if (data.textureView) vkDestroyImageView(device, data.textureView, nullptr);
    if (data.textureImage) vkDestroyImage(device, data.textureImage, nullptr);
    if (data.textureMemory) { Buffer::trackVramFreeHandle(data.textureMemory); vkFreeMemory(device, data.textureMemory, nullptr); }
    if (data.textureSampler) vkDestroySampler(device, data.textureSampler, nullptr);
    if (data.descriptorSet) vkFreeDescriptorSets(device, m_descriptorPool, 1, &data.descriptorSet);

    m_models.erase(it);
}

void ModelRenderer::destroyModels(const std::vector<uint32_t>& handles) {
    if (handles.empty()) return;

    VkDevice device = m_context.getDevice();
    m_context.waitIdle();  // Once for all

    for (uint32_t handle : handles) {
        auto it = m_models.find(handle);
        if (it == m_models.end()) continue;

        ModelGPUData& data = it->second;
        if (data.vertexBuffer) vkDestroyBuffer(device, data.vertexBuffer, nullptr);
        if (data.vertexMemory) { Buffer::trackVramFreeHandle(data.vertexMemory); vkFreeMemory(device, data.vertexMemory, nullptr); }
        if (data.indexBuffer) vkDestroyBuffer(device, data.indexBuffer, nullptr);
        if (data.indexMemory) { Buffer::trackVramFreeHandle(data.indexMemory); vkFreeMemory(device, data.indexMemory, nullptr); }
        if (data.textureView) vkDestroyImageView(device, data.textureView, nullptr);
        if (data.textureImage) vkDestroyImage(device, data.textureImage, nullptr);
        if (data.textureMemory) { Buffer::trackVramFreeHandle(data.textureMemory); vkFreeMemory(device, data.textureMemory, nullptr); }
        if (data.textureSampler) vkDestroySampler(device, data.textureSampler, nullptr);
        if (data.descriptorSet) vkFreeDescriptorSets(device, m_descriptorPool, 1, &data.descriptorSet);
        m_models.erase(it);
    }
}

void ModelRenderer::updateModelBuffer(uint32_t handle, const std::vector<ModelVertex>& vertices) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    ModelGPUData& data = it->second;

    // Verify vertex count matches
    if (vertices.size() != data.vertexCount) {
        std::cerr << "updateModelBuffer: vertex count mismatch (" << vertices.size()
                  << " vs " << data.vertexCount << ")" << std::endl;
        return;
    }

    VkDevice device = m_context.getDevice();
    VkDeviceSize bufferSize = sizeof(ModelVertex) * vertices.size();

    // Wait for GPU to finish using the buffer
    m_context.waitIdle();

    // Use staging buffer to update device-local memory
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    m_context.createBuffer(bufferSize,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           stagingBuffer, stagingMemory);

    void* stagingData;
    vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &stagingData);
    memcpy(stagingData, vertices.data(), bufferSize);
    vkUnmapMemory(device, stagingMemory);

    // Copy staging buffer to vertex buffer using a command buffer
    VkCommandBuffer cmdBuffer = m_context.beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(cmdBuffer, stagingBuffer, data.vertexBuffer, 1, &copyRegion);

    m_context.endSingleTimeCommands(cmdBuffer);

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    Buffer::trackVramFreeHandle(stagingMemory);
    vkFreeMemory(device, stagingMemory, nullptr);
}

void ModelRenderer::render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                           uint32_t modelHandle, const glm::mat4& modelMatrix,
                           float hueShift, float saturation, float brightness,
                           bool twoSided) {
    auto it = m_models.find(modelHandle);
    if (it == m_models.end()) return;

    ModelGPUData& data = it->second;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      twoSided ? m_twoSidedPipeline : m_pipeline);

    // Bind descriptor set (texture or default)
    VkDescriptorSet descSet = data.hasTexture ? data.descriptorSet : m_defaultDescriptorSet;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &descSet, 0, nullptr);

    // Push constants with color adjustments
    // For x-ray mode (twoSided), use 0.4 alpha for semi-transparency
    ModelPushConstants pc{};
    pc.mvp = viewProj * modelMatrix;
    pc.model = modelMatrix;
    pc.colorAdjust = glm::vec4(hueShift, saturation, brightness, twoSided ? 0.4f : 0.0f);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(ModelPushConstants), &pc);

    // Bind vertex/index buffers
    VkBuffer vertexBuffers[] = {data.vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, data.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(commandBuffer, data.indexCount, 1, 0, 0, 0);
}

void ModelRenderer::renderWireframe(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                     uint32_t modelHandle, const glm::mat4& modelMatrix,
                                     const glm::vec3& color) {
    auto it = m_models.find(modelHandle);
    if (it == m_models.end()) return;

    ModelGPUData& data = it->second;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);

    // Push constants with wireframe color
    WireframePushConstants pc{};
    pc.mvp = viewProj * modelMatrix;
    pc.wireColor = glm::vec4(color, 1.0f);
    vkCmdPushConstants(commandBuffer, m_wireframePipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WireframePushConstants), &pc);

    // Bind vertex/index buffers
    VkBuffer vertexBuffers[] = {data.vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, data.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(commandBuffer, data.indexCount, 1, 0, 0, 0);
}

void ModelRenderer::renderLines(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                 const std::vector<glm::vec3>& lines, const glm::vec3& color) {
    if (lines.empty()) return;

    // Bind line pipeline and push constants once
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_linePipeline);

    WireframePushConstants pc{};
    pc.mvp = viewProj;
    pc.wireColor = glm::vec4(color, 1.0f);
    vkCmdPushConstants(commandBuffer, m_wireframePipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WireframePushConstants), &pc);

    // Render in chunks of MAX_LINE_VERTICES (must be even for LINE_LIST pairs)
    size_t chunkSize = MAX_LINE_VERTICES & ~1u;  // Round down to even
    size_t offset = 0;

    while (offset < lines.size()) {
        size_t count = std::min(chunkSize, lines.size() - offset);
        count &= ~1u;  // Ensure even (complete line pairs)
        if (count == 0) break;

        // Get current buffer index and cycle
        size_t bufferIdx = m_currentLineBuffer;
        m_currentLineBuffer = (m_currentLineBuffer + 1) % NUM_LINE_BUFFERS;

        // Convert to ModelVertex format
        ModelVertex* dst = static_cast<ModelVertex*>(m_lineMappedMemories[bufferIdx]);
        for (size_t i = 0; i < count; ++i) {
            dst[i].position = lines[offset + i];
            dst[i].normal = glm::vec3(0, 1, 0);
            dst[i].texCoord = glm::vec2(0);
            dst[i].color = glm::vec4(1);
        }

        VkBuffer vertexBuffers[] = {m_lineBuffers[bufferIdx]};
        VkDeviceSize vbOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vbOffsets);
        vkCmdDraw(commandBuffer, static_cast<uint32_t>(count), 1, 0, 0);

        offset += count;
    }
}

void ModelRenderer::renderLines(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                 const std::vector<glm::vec3>& lines, const glm::vec4& color) {
    if (lines.empty()) return;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_linePipeline);

    WireframePushConstants pc{};
    pc.mvp = viewProj;
    pc.wireColor = color;
    vkCmdPushConstants(commandBuffer, m_wireframePipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WireframePushConstants), &pc);

    size_t chunkSize = MAX_LINE_VERTICES & ~1u;
    size_t offset = 0;

    while (offset < lines.size()) {
        size_t count = std::min(chunkSize, lines.size() - offset);
        count &= ~1u;
        if (count == 0) break;

        size_t bufferIdx = m_currentLineBuffer;
        m_currentLineBuffer = (m_currentLineBuffer + 1) % NUM_LINE_BUFFERS;

        ModelVertex* dst = static_cast<ModelVertex*>(m_lineMappedMemories[bufferIdx]);
        for (size_t i = 0; i < count; ++i) {
            dst[i].position = lines[offset + i];
            dst[i].normal = glm::vec3(0, 1, 0);
            dst[i].texCoord = glm::vec2(0);
            dst[i].color = glm::vec4(1);
        }

        VkBuffer vertexBuffers[] = {m_lineBuffers[bufferIdx]};
        VkDeviceSize vbOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vbOffsets);
        vkCmdDraw(commandBuffer, static_cast<uint32_t>(count), 1, 0, 0);

        offset += count;
    }
}

void ModelRenderer::renderPoints(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                  const std::vector<glm::vec3>& points, const glm::vec3& color, float pointSize) {
    if (points.empty()) return;

    // Bind point pipeline and push constants once
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pointPipeline);

    WireframePushConstants pc{};
    pc.mvp = viewProj;
    pc.wireColor = glm::vec4(color, 1.0f);
    vkCmdPushConstants(commandBuffer, m_wireframePipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WireframePushConstants), &pc);

    // Render in chunks of MAX_LINE_VERTICES
    size_t offset = 0;

    while (offset < points.size()) {
        size_t count = std::min(static_cast<size_t>(MAX_LINE_VERTICES), points.size() - offset);

        size_t bufferIdx = m_currentPointBuffer;
        m_currentPointBuffer = (m_currentPointBuffer + 1) % NUM_POINT_BUFFERS;

        // Convert to ModelVertex format directly into mapped memory
        ModelVertex* dst = static_cast<ModelVertex*>(m_pointMappedMemories[bufferIdx]);
        for (size_t i = 0; i < count; ++i) {
            dst[i].position = points[offset + i];
            dst[i].normal = glm::vec3(0, 1, 0);
            dst[i].texCoord = glm::vec2(0);
            dst[i].color = glm::vec4(1);
        }

        VkBuffer vertexBuffers[] = {m_pointBuffers[bufferIdx]};
        VkDeviceSize vbOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, vbOffsets);
        vkCmdDraw(commandBuffer, static_cast<uint32_t>(count), 1, 0, 0);

        offset += count;
    }
}

void ModelRenderer::renderSelection(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                     uint32_t modelHandle, const glm::mat4& modelMatrix,
                                     const std::vector<uint32_t>& selectedFaces,
                                     const glm::vec4& color) {
    if (selectedFaces.empty()) return;

    auto it = m_models.find(modelHandle);
    if (it == m_models.end()) return;

    ModelGPUData& data = it->second;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionPipeline);

    // Push constants with selection color
    SelectionPushConstants pc{};
    pc.mvp = viewProj * modelMatrix;
    pc.selectionColor = color;
    vkCmdPushConstants(commandBuffer, m_selectionPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(SelectionPushConstants), &pc);

    // Bind vertex buffer (shared across all selected faces)
    VkBuffer vertexBuffers[] = {data.vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    // Batch all selected face indices into a single draw call
    size_t bufferIdx = m_currentSelectionBuffer;
    m_currentSelectionBuffer = (m_currentSelectionBuffer + 1) % NUM_SELECTION_BUFFERS;

    // Build flat index list: for each selected face, add its 3 index offsets
    // We write raw index values (faceIdx*3 + 0/1/2) which reference into the original index buffer
    // But we need to use the selection index buffer as a separate index buffer,
    // so we copy the actual index values from the model's index buffer concept.
    // Actually, the original code used firstIndex offset into the existing index buffer.
    // For batching, we collect firstIndex values and draw with the model's own index buffer.
    // The simplest approach: use vkCmdDrawIndexed with the model's index buffer, batching via
    // a temporary index buffer that references vertex positions directly.

    // We need to read the original indices. Since the model index buffer is HOST_VISIBLE,
    // we can build our batch referencing firstIndex offsets.
    // Actually simplest: just collect all firstIndex values and emit one draw per chunk
    // OR: build a new index buffer with the subset of indices we want to draw.

    // Build selection indices: for each face, the 3 sequential indices starting at faceIdx*3
    uint32_t* dst = static_cast<uint32_t*>(m_selectionIndexMapped[bufferIdx]);
    uint32_t totalIndices = 0;
    for (uint32_t faceIdx : selectedFaces) {
        uint32_t firstIndex = faceIdx * 3;
        if (firstIndex + 3 <= data.indexCount && totalIndices + 3 <= MAX_SELECTION_INDICES) {
            dst[totalIndices]     = firstIndex;
            dst[totalIndices + 1] = firstIndex + 1;
            dst[totalIndices + 2] = firstIndex + 2;
            totalIndices += 3;
        }
    }

    if (totalIndices == 0) return;

    // Bind selection index buffer and draw all selected faces in one call
    // Note: these are indices-of-indices, so we use drawIndirect or simply
    // reference the original index buffer with offsets. Since Vulkan doesn't
    // support index-of-index, we use the model's index buffer with multiple
    // firstIndex offsets via a single indexed draw won't work directly.
    //
    // Better approach: bind the model's index buffer and use the selection buffer
    // as a way to batch. But since we can't do index-of-index in Vulkan,
    // we'll use the original index buffer and emit one draw call per face.
    //
    // ACTUALLY: The right approach is to bind the model's index buffer and draw
    // with contiguous ranges. Let's instead sort and merge contiguous ranges,
    // or just use the model's index buffer with the original per-face approach
    // but with a much more efficient method: copy the actual index VALUES into
    // our selection buffer.

    // Re-read the model's index data (it's HOST_VISIBLE) and copy selected indices
    void* indexData;
    vkMapMemory(m_context.getDevice(), data.indexMemory, 0, data.indexCount * sizeof(uint32_t), 0, &indexData);
    const uint32_t* srcIndices = static_cast<const uint32_t*>(indexData);

    totalIndices = 0;
    for (uint32_t faceIdx : selectedFaces) {
        uint32_t firstIndex = faceIdx * 3;
        if (firstIndex + 3 <= data.indexCount && totalIndices + 3 <= MAX_SELECTION_INDICES) {
            dst[totalIndices]     = srcIndices[firstIndex];
            dst[totalIndices + 1] = srcIndices[firstIndex + 1];
            dst[totalIndices + 2] = srcIndices[firstIndex + 2];
            totalIndices += 3;
        }
    }

    vkUnmapMemory(m_context.getDevice(), data.indexMemory);

    if (totalIndices == 0) return;

    // Single draw call with our batched selection index buffer
    vkCmdBindIndexBuffer(commandBuffer, m_selectionIndexBuffers[bufferIdx], 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, totalIndices, 1, 0, 0, 0);
}

ModelGPUData* ModelRenderer::getModelData(uint32_t handle) {
    auto it = m_models.find(handle);
    return it != m_models.end() ? &it->second : nullptr;
}

void ModelRenderer::updateTexture(uint32_t handle, const unsigned char* data, int width, int height) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    ModelGPUData& modelData = it->second;
    VkDevice device = m_context.getDevice();

    // Check if we need to recreate the texture (size changed or doesn't exist)
    bool needRecreate = !modelData.hasTexture ||
                        modelData.textureWidth != width ||
                        modelData.textureHeight != height;

    if (needRecreate) {
        // Wait for GPU to be idle before destroying old resources
        // This ensures no in-flight commands are using the texture
        vkDeviceWaitIdle(device);

        // Destroy old texture resources if they exist
        if (modelData.textureView) {
            vkDestroyImageView(device, modelData.textureView, nullptr);
            modelData.textureView = VK_NULL_HANDLE;
        }
        if (modelData.textureImage) {
            vkDestroyImage(device, modelData.textureImage, nullptr);
            modelData.textureImage = VK_NULL_HANDLE;
        }
        if (modelData.textureMemory) {
            Buffer::trackVramFreeHandle(modelData.textureMemory);
            vkFreeMemory(device, modelData.textureMemory, nullptr);
            modelData.textureMemory = VK_NULL_HANDLE;
        }
        if (modelData.textureSampler) {
            vkDestroySampler(device, modelData.textureSampler, nullptr);
            modelData.textureSampler = VK_NULL_HANDLE;
        }
        if (modelData.descriptorSet) {
            vkFreeDescriptorSets(device, m_descriptorPool, 1, &modelData.descriptorSet);
            modelData.descriptorSet = VK_NULL_HANDLE;
        }

        // Reset texture state in case creation fails
        modelData.hasTexture = false;
        modelData.textureWidth = 0;
        modelData.textureHeight = 0;

        // Create new texture with correct size
        try {
            createImage(width, height, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, modelData.textureImage, modelData.textureMemory);
        } catch (const std::exception& e) {
            std::cerr << "Failed to create texture image: " << e.what() << std::endl;
            return;
        }

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = modelData.textureImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &modelData.textureView);
        if (result != VK_SUCCESS) {
            std::cerr << "Failed to create texture image view: " << result << std::endl;
            vkDestroyImage(device, modelData.textureImage, nullptr);
            Buffer::trackVramFreeHandle(modelData.textureMemory);
            vkFreeMemory(device, modelData.textureMemory, nullptr);
            modelData.textureImage = VK_NULL_HANDLE;
            modelData.textureMemory = VK_NULL_HANDLE;
            return;
        }

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        result = vkCreateSampler(device, &samplerInfo, nullptr, &modelData.textureSampler);
        if (result != VK_SUCCESS) {
            std::cerr << "Failed to create texture sampler: " << result << std::endl;
            vkDestroyImageView(device, modelData.textureView, nullptr);
            vkDestroyImage(device, modelData.textureImage, nullptr);
            Buffer::trackVramFreeHandle(modelData.textureMemory);
            vkFreeMemory(device, modelData.textureMemory, nullptr);
            modelData.textureView = VK_NULL_HANDLE;
            modelData.textureImage = VK_NULL_HANDLE;
            modelData.textureMemory = VK_NULL_HANDLE;
            return;
        }

        // Create descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_descriptorSetLayout;

        result = vkAllocateDescriptorSets(device, &allocInfo, &modelData.descriptorSet);
        if (result != VK_SUCCESS) {
            std::cerr << "Failed to allocate descriptor set: " << result << std::endl;
            vkDestroySampler(device, modelData.textureSampler, nullptr);
            vkDestroyImageView(device, modelData.textureView, nullptr);
            vkDestroyImage(device, modelData.textureImage, nullptr);
            Buffer::trackVramFreeHandle(modelData.textureMemory);
            vkFreeMemory(device, modelData.textureMemory, nullptr);
            modelData.textureSampler = VK_NULL_HANDLE;
            modelData.textureView = VK_NULL_HANDLE;
            modelData.textureImage = VK_NULL_HANDLE;
            modelData.textureMemory = VK_NULL_HANDLE;
            return;
        }

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = modelData.textureView;
        imageInfo.sampler = modelData.textureSampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = modelData.descriptorSet;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        modelData.hasTexture = true;
        modelData.textureWidth = width;
        modelData.textureHeight = height;

        // For newly created texture, transition from UNDEFINED
        transitionImageLayout(modelData.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    } else {
        // For existing texture, transition from SHADER_READ_ONLY
        transitionImageLayout(modelData.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    // Upload new texture data via staging buffer
    VkDeviceSize texSize = width * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    m_context.createBuffer(texSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           stagingBuffer, stagingMemory);

    void* mapped;
    vkMapMemory(m_context.getDevice(), stagingMemory, 0, texSize, 0, &mapped);
    memcpy(mapped, data, texSize);
    vkUnmapMemory(m_context.getDevice(), stagingMemory);

    copyBufferToImage(stagingBuffer, modelData.textureImage, width, height);
    transitionImageLayout(modelData.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(m_context.getDevice(), stagingBuffer, nullptr);
    Buffer::trackVramFreeHandle(stagingMemory);
    vkFreeMemory(m_context.getDevice(), stagingMemory, nullptr);
}

void ModelRenderer::destroyTexture(uint32_t handle) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    ModelGPUData& modelData = it->second;
    VkDevice device = m_context.getDevice();

    // Wait for GPU to be idle before destroying resources
    vkDeviceWaitIdle(device);

    // Destroy texture resources
    if (modelData.textureView) {
        vkDestroyImageView(device, modelData.textureView, nullptr);
        modelData.textureView = VK_NULL_HANDLE;
    }
    if (modelData.textureImage) {
        vkDestroyImage(device, modelData.textureImage, nullptr);
        modelData.textureImage = VK_NULL_HANDLE;
    }
    if (modelData.textureMemory) {
        Buffer::trackVramFreeHandle(modelData.textureMemory);
        vkFreeMemory(device, modelData.textureMemory, nullptr);
        modelData.textureMemory = VK_NULL_HANDLE;
    }
    if (modelData.textureSampler) {
        vkDestroySampler(device, modelData.textureSampler, nullptr);
        modelData.textureSampler = VK_NULL_HANDLE;
    }
    if (modelData.descriptorSet) {
        vkFreeDescriptorSets(device, m_descriptorPool, 1, &modelData.descriptorSet);
        modelData.descriptorSet = VK_NULL_HANDLE;
    }

    modelData.hasTexture = false;
    modelData.textureWidth = 0;
    modelData.textureHeight = 0;

    std::cout << "Destroyed texture for model " << handle << std::endl;
}

void ModelRenderer::updateVertices(uint32_t handle, const std::vector<ModelVertex>& vertices) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    ModelGPUData& data = it->second;

    // Update vertex buffer with new data
    VkDeviceSize vertexSize = sizeof(ModelVertex) * vertices.size();

    void* mapped;
    vkMapMemory(m_context.getDevice(), data.vertexMemory, 0, vertexSize, 0, &mapped);
    memcpy(mapped, vertices.data(), vertexSize);
    vkUnmapMemory(m_context.getDevice(), data.vertexMemory);
}

void ModelRenderer::recreatePipeline(VkRenderPass renderPass, VkExtent2D extent) {
    VkDevice device = m_context.getDevice();

    // Wait for GPU to be idle before destroying old pipeline
    m_context.waitIdle();

    // Destroy old pipeline and layout
    if (m_pipeline) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_twoSidedPipeline) {
        vkDestroyPipeline(device, m_twoSidedPipeline, nullptr);
        m_twoSidedPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy old wireframe pipeline and layout
    if (m_wireframePipeline) {
        vkDestroyPipeline(device, m_wireframePipeline, nullptr);
        m_wireframePipeline = VK_NULL_HANDLE;
    }
    if (m_linePipeline) {
        vkDestroyPipeline(device, m_linePipeline, nullptr);
        m_linePipeline = VK_NULL_HANDLE;
    }
    if (m_pointPipeline) {
        vkDestroyPipeline(device, m_pointPipeline, nullptr);
        m_pointPipeline = VK_NULL_HANDLE;
    }
    if (m_wireframePipelineLayout) {
        vkDestroyPipelineLayout(device, m_wireframePipelineLayout, nullptr);
        m_wireframePipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy old selection pipeline and layout
    if (m_selectionPipeline) {
        vkDestroyPipeline(device, m_selectionPipeline, nullptr);
        m_selectionPipeline = VK_NULL_HANDLE;
    }
    if (m_selectionPipelineLayout) {
        vkDestroyPipelineLayout(device, m_selectionPipelineLayout, nullptr);
        m_selectionPipelineLayout = VK_NULL_HANDLE;
    }

    // Create new pipelines with new extent
    createPipeline(renderPass, extent);
    createWireframePipeline(renderPass, extent);
    createSelectionPipeline(renderPass, extent);
}

void ModelRenderer::createImage(uint32_t width, uint32_t height, VkFormat format,
                                 VkImageTiling tiling, VkImageUsageFlags usage,
                                 VkMemoryPropertyFlags properties, VkImage& image,
                                 VkDeviceMemory& memory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    vkCreateImage(m_context.getDevice(), &imageInfo, nullptr, &image);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_context.getDevice(), image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(memReqs.memoryTypeBits, properties);

    vkAllocateMemory(m_context.getDevice(), &allocInfo, nullptr, &memory);
    Buffer::trackVramAllocHandle(memory, static_cast<int64_t>(memReqs.size));
    vkBindImageMemory(m_context.getDevice(), image, memory, 0);
}

void ModelRenderer::transitionImageLayout(VkImage image, VkFormat format,
                                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

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
    barrier.subresourceRange.layerCount = 1;

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
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // For texture updates (painting)
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        throw std::runtime_error("Unsupported layout transition");
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_context.endSingleTimeCommands(cmd);
}

void ModelRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_context.endSingleTimeCommands(cmd);
}

} // namespace eden
