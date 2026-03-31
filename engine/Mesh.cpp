#include <eden/Mesh.hpp>

namespace eden {

Mesh::Mesh(const MeshDescriptor& desc) {
    size_t vertexCount = desc.vertices.size();
    m_vertices.resize(vertexCount);

    for (size_t i = 0; i < vertexCount; ++i) {
        m_vertices[i].position = desc.vertices[i];
        m_vertices[i].color = (i < desc.colors.size()) ? desc.colors[i] : glm::vec3(1.0f);
    }

    m_indices = desc.indices;
    m_needsUpload = true;
}

} // namespace eden
