#pragma once

#include "SceneObject.hpp"
#include "Renderer/ModelRenderer.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace eden {

struct LoadedTexture {
    std::vector<unsigned char> data;
    int width = 0;
    int height = 0;
};

// Stored half-edge data for preserving quad topology
struct StoredHEData {
    std::vector<SceneObject::StoredHEVertex> vertices;
    std::vector<SceneObject::StoredHalfEdge> halfEdges;
    std::vector<SceneObject::StoredHEFace> faces;
};

struct LoadedMesh {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds;
    std::string name;
    LoadedTexture texture;  // Base color texture if available
    bool hasTexture = false;

    // Half-edge data for preserving quad topology (optional, from EDEN exports)
    std::optional<StoredHEData> halfEdgeData;
};

struct LoadResult {
    bool success = false;
    std::string error;
    std::vector<LoadedMesh> meshes;
};

class GLBLoader {
public:
    // Load a GLB/GLTF file and return mesh data
    static LoadResult load(const std::string& filepath);

    // Create a SceneObject from loaded mesh data and upload to GPU
    static std::unique_ptr<SceneObject> createSceneObject(
        const LoadedMesh& mesh,
        ModelRenderer& modelRenderer
    );

    // Export mesh data to GLB file
    static bool save(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const std::string& meshName = "mesh");

    // Export mesh data with embedded texture to GLB file
    static bool save(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const unsigned char* textureData,
                     int textureWidth,
                     int textureHeight,
                     const std::string& meshName = "mesh");

    // Export mesh data with half-edge data (preserves quad topology)
    static bool saveWithHalfEdgeData(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const StoredHEData& heData,
                     const unsigned char* textureData,
                     int textureWidth,
                     int textureHeight,
                     const std::string& meshName = "mesh");

private:
    static glm::vec3 calculateNormal(
        const glm::vec3& v0,
        const glm::vec3& v1,
        const glm::vec3& v2
    );
};

} // namespace eden
