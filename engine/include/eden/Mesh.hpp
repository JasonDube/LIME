#pragma once

#include "Transform.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <cstdint>

namespace eden {

struct Vertex {
    glm::vec2 position;
    glm::vec3 color;
};

struct MeshDescriptor {
    std::vector<glm::vec2> vertices;
    std::vector<glm::vec3> colors;
    std::vector<uint32_t> indices;  // Optional, empty = non-indexed
};

class Mesh {
public:
    Mesh() = default;
    explicit Mesh(const MeshDescriptor& desc);

    // Transform shortcuts
    void setPosition(float x, float y, float z = 0.0f) { m_transform.setPosition(x, y, z); }
    void setPosition(const glm::vec3& pos) { m_transform.setPosition(pos); }
    void rotate(float degrees, const glm::vec3& axis = {0, 0, 1}) { m_transform.rotate(degrees, axis); }
    void setRotation(float degrees, const glm::vec3& axis = {0, 0, 1}) { m_transform.setRotation(degrees, axis); }
    void setScale(float uniform) { m_transform.setScale(uniform); }
    void setScale(const glm::vec3& scale) { m_transform.setScale(scale); }

    Transform& getTransform() { return m_transform; }
    const Transform& getTransform() const { return m_transform; }
    glm::mat4 getModelMatrix() { return m_transform.getMatrix(); }

    const std::vector<Vertex>& getVertices() const { return m_vertices; }
    const std::vector<uint32_t>& getIndices() const { return m_indices; }
    bool isIndexed() const { return !m_indices.empty(); }
    uint32_t getVertexCount() const { return static_cast<uint32_t>(m_vertices.size()); }
    uint32_t getIndexCount() const { return static_cast<uint32_t>(m_indices.size()); }

    // Internal use - buffer handles
    void setBufferHandle(uint32_t handle) { m_bufferHandle = handle; }
    uint32_t getBufferHandle() const { return m_bufferHandle; }
    bool needsUpload() const { return m_needsUpload; }
    void markUploaded() { m_needsUpload = false; }

private:
    Transform m_transform;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    uint32_t m_bufferHandle = UINT32_MAX;
    bool m_needsUpload = true;
};

using MeshPtr = std::shared_ptr<Mesh>;

} // namespace eden
