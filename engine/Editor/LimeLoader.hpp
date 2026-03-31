#pragma once

#include "Renderer/ModelRenderer.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>

namespace eden {

/**
 * @brief Loader for .lime model format
 *
 * LIME format stores:
 * - Half-edge mesh topology (quads preserved)
 * - Vertex colors
 * - Embedded RGBA textures as base64
 */
class LimeLoader {
public:
    struct ControlPoint {
        uint32_t vertexIndex;
        std::string name;
    };

    struct LoadedMesh {
        std::string name;
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<unsigned char> textureData;
        int textureWidth = 0;
        int textureHeight = 0;
        bool hasTexture = false;
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};  // Euler degrees (converted from quaternion)
        glm::vec3 scale{1.0f};
        std::vector<ControlPoint> controlPoints;
        std::unordered_map<std::string, std::string> metadata;
    };

    struct LoadResult {
        bool success = false;
        std::string error;
        LoadedMesh mesh;
    };

    /**
     * @brief Load a .lime file
     * @param filepath Path to the .lime file
     * @return LoadResult with mesh data or error
     */
    static LoadResult load(const std::string& filepath);

    /**
     * @brief Create a SceneObject from loaded mesh data
     * @param mesh The loaded mesh data
     * @param renderer ModelRenderer to create GPU resources
     * @return Unique pointer to SceneObject, or nullptr on failure
     */
    static std::unique_ptr<class SceneObject> createSceneObject(
        const LoadedMesh& mesh,
        ModelRenderer& renderer
    );
};

} // namespace eden
