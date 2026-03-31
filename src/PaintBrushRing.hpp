#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

namespace eden {

class VulkanContext;

class PaintBrushRing {
public:
    PaintBrushRing(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent);
    ~PaintBrushRing();

    PaintBrushRing(const PaintBrushRing&) = delete;
    PaintBrushRing& operator=(const PaintBrushRing&) = delete;

    // Update ring geometry based on hit position, normal, and radius
    void update(const glm::vec3& position, const glm::vec3& normal, float radius);

    // Render the ring
    void render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj);

    // Set ring color
    void setColor(const glm::vec3& color) { m_color = color; }

    // Enable/disable ring visibility
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const { return m_visible; }

private:
    void createPipeline(VkRenderPass renderPass, VkExtent2D extent);
    void createVertexBuffer();
    void updateVertexBuffer();

    VulkanContext& m_context;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // Vertex buffer
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    void* m_mappedMemory = nullptr;

    // Ring properties
    glm::vec3 m_color{1.0f, 1.0f, 1.0f};  // White by default
    bool m_visible = true;

    // Ring geometry
    std::vector<glm::vec3> m_vertices;
    static constexpr int RING_SEGMENTS = 48;
    static constexpr float OFFSET = 0.05f;  // Offset from surface to avoid z-fighting
};

} // namespace eden
