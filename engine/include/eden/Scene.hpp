#pragma once

#include "Mesh.hpp"
#include <vector>
#include <algorithm>

namespace eden {

class Scene {
public:
    Scene() = default;

    void add(MeshPtr mesh) {
        if (mesh) {
            m_meshes.push_back(mesh);
        }
    }

    void remove(MeshPtr mesh) {
        m_meshes.erase(
            std::remove(m_meshes.begin(), m_meshes.end(), mesh),
            m_meshes.end()
        );
    }

    void clear() {
        m_meshes.clear();
    }

    const std::vector<MeshPtr>& getMeshes() const { return m_meshes; }
    size_t getMeshCount() const { return m_meshes.size(); }

private:
    std::vector<MeshPtr> m_meshes;
};

} // namespace eden
