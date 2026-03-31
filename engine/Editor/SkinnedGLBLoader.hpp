#pragma once

#include "Renderer/SkinnedModelRenderer.hpp"
#include "eden/Animation.hpp"
#include <string>
#include <vector>
#include <memory>

namespace eden {

struct SkinnedLoadedMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;
    std::vector<unsigned char> textureData;
    int textureWidth = 0;
    int textureHeight = 0;
    bool hasTexture = false;
};

struct SkinnedLoadResult {
    bool success = false;
    std::string error;
    std::vector<SkinnedLoadedMesh> meshes;
    std::unique_ptr<Skeleton> skeleton;
    std::vector<AnimationClip> animations;
};

class SkinnedGLBLoader {
public:
    // Load a GLB/GLTF file with skeleton and animation data
    static SkinnedLoadResult load(const std::string& filepath);

    // Check if a GLB file contains skeletal animation data
    static bool hasSkeleton(const std::string& filepath);
};

} // namespace eden
