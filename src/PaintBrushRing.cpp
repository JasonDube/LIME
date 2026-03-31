#include "PaintBrushRing.hpp"
#include "Renderer/VulkanContext.hpp"
#include "Renderer/PipelineBuilder.hpp"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <iostream>

namespace eden {

struct PaintBrushRingPushConstants {
    glm::mat4 mvp;
    glm::vec4 color;
};

PaintBrushRing::PaintBrushRing(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent)
    : m_context(context)
{
    std::cout << "[PaintBrushRing] Initializing..." << std::endl;
    m_vertices.resize(RING_SEGMENTS + 1);  // +1 to close the loop
    createVertexBuffer();
    createPipeline(renderPass, extent);
    std::cout << "[PaintBrushRing] Created pipeline=" << m_pipeline << " layout=" << m_pipelineLayout << std::endl;
}

PaintBrushRing::~PaintBrushRing() {
    VkDevice device = m_context.getDevice();

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    }
    if (m_mappedMemory) {
        vkUnmapMemory(device, m_vertexMemory);
    }
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_vertexBuffer, nullptr);
    }
    if (m_vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_vertexMemory, nullptr);
    }
}

void PaintBrushRing::createVertexBuffer() {
    VkDevice device = m_context.getDevice();
    VkDeviceSize bufferSize = sizeof(glm::vec3) * m_vertices.size();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create paint brush ring vertex buffer");
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, m_vertexBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_context.findMemoryType(
        memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate paint brush ring vertex buffer memory");
    }

    vkBindBufferMemory(device, m_vertexBuffer, m_vertexMemory, 0);

    // Keep buffer persistently mapped for efficient updates
    vkMapMemory(device, m_vertexMemory, 0, bufferSize, 0, &m_mappedMemory);
}

void PaintBrushRing::update(const glm::vec3& position, const glm::vec3& normal, float radius) {
    const float PI = 3.14159265f;

    // DEBUG: Create a simple ring at the given position in XY plane
    // This tests if the world-to-screen transformation works
    for (int i = 0; i <= RING_SEGMENTS; i++) {
        float t = static_cast<float>(i) / RING_SEGMENTS;
        float angle = t * 2.0f * PI;

        // Simple ring in XY plane at the given position
        float x = position.x + radius * std::cos(angle);
        float y = position.y + radius * std::sin(angle);
        float z = position.z;

        m_vertices[i] = glm::vec3(x, y, z);
    }

    updateVertexBuffer();
}

void PaintBrushRing::updateVertexBuffer() {
    if (m_mappedMemory) {
        memcpy(m_mappedMemory, m_vertices.data(), sizeof(glm::vec3) * m_vertices.size());
    }
}

void PaintBrushRing::render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj) {
    if (!m_visible || m_vertices.empty()) {
        return;
    }

    static int renderCount = 0;
    if (++renderCount % 60 == 1) {
        std::cout << "[PaintBrushRing] Rendering at pos=(" << m_vertices[0].x << "," << m_vertices[0].y << "," << m_vertices[0].z << ")" << std::endl;
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    PaintBrushRingPushConstants pc;
    pc.mvp = viewProj;
    pc.color = glm::vec4(m_color, 1.0f);

    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PaintBrushRingPushConstants), &pc);

    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdDraw(commandBuffer, static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
}

void PaintBrushRing::createPipeline(VkRenderPass renderPass, VkExtent2D extent) {
    auto result = PipelineBuilder(m_context)
        .setShaders("shaders/brush_ring.vert.spv", "shaders/brush_ring.frag.spv")
        .setVertexBinding(0, sizeof(glm::vec3))
        .addVertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
        .setCullMode(VK_CULL_MODE_NONE)
        .setDepthTest(false, false)  // Disable depth test to ensure visibility
        .setPushConstantSize(sizeof(PaintBrushRingPushConstants))
        .build(renderPass, extent);

    m_pipeline = result.pipeline;
    m_pipelineLayout = result.layout;
}

} // namespace eden
