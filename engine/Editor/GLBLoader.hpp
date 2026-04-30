#pragma once

#include "SceneObject.hpp"
#include "Renderer/ModelRenderer.hpp"
#include "eden/Animation.hpp"
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

    // Export a skinned + animated mesh as glTF/GLB (translation-only bone
    // animation — LIME's rig is translation-only). Vertices are the bind-pose
    // (rest) positions. perVertexBoneIndices/Weights provide a uvec4/vec4
    // pair per vertex. animBoneWorldPosPerKey[k][i] is the world-space
    // position of bone i at keyframe k; the function converts to per-parent
    // local translations on export. Pass an empty animTimes to export a
    // skinned mesh with no animation clip.
    static bool saveSkinnedAnimated(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const std::vector<glm::ivec4>& perVertexBoneIndices,
                     const std::vector<glm::vec4>& perVertexBoneWeights,
                     const Skeleton& skeleton,
                     const std::vector<glm::vec3>& bindBoneWorldPos,
                     const std::vector<float>& animTimes,
                     const std::vector<std::vector<glm::vec3>>& animBoneWorldPosPerKey,
                     const unsigned char* textureData,
                     int textureWidth,
                     int textureHeight,
                     const std::string& meshName = "mesh",
                     const std::string& animName = "Take 001");

private:
    static glm::vec3 calculateNormal(
        const glm::vec3& v0,
        const glm::vec3& v1,
        const glm::vec3& v2
    );
};

} // namespace eden
