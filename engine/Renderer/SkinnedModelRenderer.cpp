#include "SkinnedModelRenderer.hpp"
#include "VulkanContext.hpp"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include "Buffer.hpp"

namespace eden {

VkVertexInputBindingDescription SkinnedVertex::getBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(SkinnedVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

std::vector<VkVertexInputAttributeDescription> SkinnedVertex::getAttributeDescriptions() {
    std::vector<VkVertexInputAttributeDescription> attrs(6);

    // Position
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(SkinnedVertex, position);

    // Normal
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(SkinnedVertex, normal);

    // TexCoord
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = offsetof(SkinnedVertex, texCoord);

    // Color
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[3].offset = offsetof(SkinnedVertex, color);

    // Joints (4 integer indices)
    attrs[4].binding = 0;
    attrs[4].location = 4;
    attrs[4].format = VK_FORMAT_R32G32B32A32_SINT;
    attrs[4].offset = offsetof(SkinnedVertex, joints);

    // Weights
    attrs[5].binding = 0;
    attrs[5].location = 5;
    attrs[5].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[5].offset = offsetof(SkinnedVertex, weights);

    return attrs;
}

SkinnedModelRenderer::SkinnedModelRenderer(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent)
    : m_context(context)
{
    createDescriptorSetLayout();
    createDescriptorPool();
    createPipeline(renderPass, extent);
    createDefaultTexture();
}

SkinnedModelRenderer::~SkinnedModelRenderer() {
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
        if (data.boneBuffer) vkDestroyBuffer(device, data.boneBuffer, nullptr);
        if (data.boneMemory) { Buffer::trackVramFreeHandle(data.boneMemory); vkFreeMemory(device, data.boneMemory, nullptr); }
    }

    // Destroy default texture
    if (m_defaultTextureView) vkDestroyImageView(device, m_defaultTextureView, nullptr);
    if (m_defaultTexture) vkDestroyImage(device, m_defaultTexture, nullptr);
    if (m_defaultTextureMemory) { Buffer::trackVramFreeHandle(m_defaultTextureMemory); vkFreeMemory(device, m_defaultTextureMemory, nullptr); }
    if (m_defaultSampler) vkDestroySampler(device, m_defaultSampler, nullptr);

    if (m_descriptorPool) vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    if (m_descriptorSetLayout) vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
}

void SkinnedModelRenderer::createDescriptorSetLayout() {
    std::vector<VkDescriptorSetLayoutBinding> bindings(2);

    // Binding 0: Texture sampler
    bindings[0].binding = 0;
    bindings[0].descriptorCount = 1;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: Bone matrices UBO
    bindings[1].binding = 1;
    bindings[1].descriptorCount = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create skinned model descriptor set layout");
    }
}

void SkinnedModelRenderer::createDescriptorPool() {
    std::vector<VkDescriptorPoolSize> poolSizes(2);
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 50;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 50;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 50;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create skinned model descriptor pool");
    }
}

void SkinnedModelRenderer::createPipeline(VkRenderPass renderPass, VkExtent2D extent) {
    auto vertCode = m_context.readFile("shaders/skinned_model.vert.spv");
    auto fragCode = m_context.readFile("shaders/skinned_model.frag.spv");

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

    auto bindingDesc = SkinnedVertex::getBindingDescription();
    auto attrDescs = SkinnedVertex::getAttributeDescriptions();

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

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(SkinnedModelPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create skinned model pipeline layout");
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
        throw std::runtime_error("Failed to create skinned model pipeline");
    }

    vkDestroyShaderModule(m_context.getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_context.getDevice(), fragModule, nullptr);
}

void SkinnedModelRenderer::createDefaultTexture() {
    uint32_t white = 0xFFFFFFFF;

    createImage(1, 1, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_defaultTexture, m_defaultTextureMemory);

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

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_defaultTexture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &m_defaultTextureView);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_defaultSampler);
}

uint32_t SkinnedModelRenderer::createModel(const std::vector<SkinnedVertex>& vertices,
                                            const std::vector<uint32_t>& indices,
                                            std::unique_ptr<Skeleton> skeleton,
                                            std::vector<AnimationClip> animations,
                                            const unsigned char* textureData,
                                            int texWidth, int texHeight) {
    uint32_t handle = m_nextHandle++;
    SkinnedModelGPUData& data = m_models[handle];

    // Store skeleton and animations
    data.skeleton = std::move(skeleton);
    data.animations = std::move(animations);
    data.animPlayer.setSkeleton(data.skeleton.get());

    // Create vertex buffer
    VkDeviceSize vertexSize = sizeof(SkinnedVertex) * vertices.size();
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

    // Create bone matrix UBO (persistently mapped)
    VkDeviceSize boneBufferSize = sizeof(glm::mat4) * MAX_BONES;
    m_context.createBuffer(boneBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           data.boneBuffer, data.boneMemory);

    vkMapMemory(m_context.getDevice(), data.boneMemory, 0, boneBufferSize, 0, &data.boneMappedMemory);

    // Initialize bone matrices to identity
    std::vector<glm::mat4> identityBones(MAX_BONES, glm::mat4(1.0f));
    memcpy(data.boneMappedMemory, identityBones.data(), boneBufferSize);

    // Create texture if provided
    VkImageView texView = m_defaultTextureView;
    VkSampler texSampler = m_defaultSampler;

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

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = data.textureImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &data.textureView);

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

        texView = data.textureView;
        texSampler = data.textureSampler;
        data.hasTexture = true;
        data.textureWidth = texWidth;
        data.textureHeight = texHeight;
    }

    // Create descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, &data.descriptorSet);

    // Update descriptor set
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texView;
    imageInfo.sampler = texSampler;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = data.boneBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(glm::mat4) * MAX_BONES;

    std::vector<VkWriteDescriptorSet> writes(2);

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = data.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = data.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(m_context.getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    return handle;
}

void SkinnedModelRenderer::destroyModel(uint32_t handle) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    VkDevice device = m_context.getDevice();
    SkinnedModelGPUData& data = it->second;

    m_context.waitIdle();

    if (data.boneMappedMemory) {
        vkUnmapMemory(device, data.boneMemory);
    }

    if (data.vertexBuffer) vkDestroyBuffer(device, data.vertexBuffer, nullptr);
    if (data.vertexMemory) { Buffer::trackVramFreeHandle(data.vertexMemory); vkFreeMemory(device, data.vertexMemory, nullptr); }
    if (data.indexBuffer) vkDestroyBuffer(device, data.indexBuffer, nullptr);
    if (data.indexMemory) { Buffer::trackVramFreeHandle(data.indexMemory); vkFreeMemory(device, data.indexMemory, nullptr); }
    if (data.textureView) vkDestroyImageView(device, data.textureView, nullptr);
    if (data.textureImage) vkDestroyImage(device, data.textureImage, nullptr);
    if (data.textureMemory) { Buffer::trackVramFreeHandle(data.textureMemory); vkFreeMemory(device, data.textureMemory, nullptr); }
    if (data.textureSampler) vkDestroySampler(device, data.textureSampler, nullptr);
    if (data.boneBuffer) vkDestroyBuffer(device, data.boneBuffer, nullptr);
    if (data.boneMemory) { Buffer::trackVramFreeHandle(data.boneMemory); vkFreeMemory(device, data.boneMemory, nullptr); }
    if (data.descriptorSet) vkFreeDescriptorSets(device, m_descriptorPool, 1, &data.descriptorSet);

    m_models.erase(it);
}

void SkinnedModelRenderer::updateAnimation(uint32_t handle, float deltaTime) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    SkinnedModelGPUData& data = it->second;
    data.animPlayer.update(deltaTime);

    // Upload bone matrices to GPU
    const auto& boneMatrices = data.animPlayer.getBoneMatrices();
    if (!boneMatrices.empty() && data.boneMappedMemory) {
        size_t copySize = std::min(boneMatrices.size(), MAX_BONES) * sizeof(glm::mat4);
        memcpy(data.boneMappedMemory, boneMatrices.data(), copySize);
    }
}

void SkinnedModelRenderer::playAnimation(uint32_t handle, const std::string& animName, bool loop) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    SkinnedModelGPUData& data = it->second;

    // Find animation by name
    for (const auto& anim : data.animations) {
        if (anim.name == animName) {
            data.animPlayer.play(&anim, loop);
            return;
        }
    }

    std::cerr << "Animation not found: " << animName << std::endl;
}

void SkinnedModelRenderer::stopAnimation(uint32_t handle) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    it->second.animPlayer.stop();
}

void SkinnedModelRenderer::addAnimation(uint32_t handle, const AnimationClip& clip) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;

    SkinnedModelGPUData& data = it->second;

    // Save current animation state before modifying vector
    // (push_back can invalidate the pointer held by AnimationPlayer)
    std::string currentAnimName;
    float currentTime = 0.0f;
    bool wasPlaying = data.animPlayer.isPlaying();
    if (wasPlaying && data.animPlayer.getCurrentClip()) {
        currentAnimName = data.animPlayer.getCurrentClip()->name;
        currentTime = data.animPlayer.getCurrentTime();
    }

    // Stop animation before vector modification
    data.animPlayer.stop();

    // Add the new animation
    data.animations.push_back(clip);

    // Restart the animation that was playing (with fresh pointer)
    if (wasPlaying && !currentAnimName.empty()) {
        for (const auto& anim : data.animations) {
            if (anim.name == currentAnimName) {
                data.animPlayer.play(&anim, true);
                // Note: We lose the exact time position, but this is safer
                break;
            }
        }
    }
}

std::vector<std::string> SkinnedModelRenderer::getAnimationNames(uint32_t handle) const {
    std::vector<std::string> names;

    auto it = m_models.find(handle);
    if (it != m_models.end()) {
        for (const auto& anim : it->second.animations) {
            names.push_back(anim.name);
        }
    }

    return names;
}

void SkinnedModelRenderer::render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj,
                                   uint32_t modelHandle, const glm::mat4& modelMatrix,
                                   float hueShift, float saturation, float brightness) {
    auto it = m_models.find(modelHandle);
    if (it == m_models.end()) return;

    SkinnedModelGPUData& data = it->second;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &data.descriptorSet, 0, nullptr);

    SkinnedModelPushConstants pc{};
    pc.mvp = viewProj * modelMatrix;
    pc.model = modelMatrix;
    pc.colorAdjust = glm::vec4(hueShift, saturation, brightness, 0.0f);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(SkinnedModelPushConstants), &pc);

    VkBuffer vertexBuffers[] = {data.vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, data.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(commandBuffer, data.indexCount, 1, 0, 0, 0);
}

SkinnedModelGPUData* SkinnedModelRenderer::getModelData(uint32_t handle) {
    auto it = m_models.find(handle);
    return it != m_models.end() ? &it->second : nullptr;
}

void SkinnedModelRenderer::recreatePipeline(VkRenderPass renderPass, VkExtent2D extent) {
    VkDevice device = m_context.getDevice();
    m_context.waitIdle();

    if (m_pipeline) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    createPipeline(renderPass, extent);
}

void SkinnedModelRenderer::createImage(uint32_t width, uint32_t height, VkFormat format,
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

void SkinnedModelRenderer::transitionImageLayout(VkImage image, VkFormat format,
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
    } else {
        throw std::runtime_error("Unsupported layout transition");
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_context.endSingleTimeCommands(cmd);
}

void SkinnedModelRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_context.endSingleTimeCommands(cmd);
}

} // namespace eden
