#include "SkinnedGLBLoader.hpp"
#include <tiny_gltf.h>
#include <stb_image.h>
#include <iostream>
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>

namespace eden {

// Forward declaration of image loader (shared with GLBLoader)
static bool LoadImageDataSkinned(tinygltf::Image* image, const int image_idx, std::string* err,
                                  std::string* warn, int req_width, int req_height,
                                  const unsigned char* bytes, int size, void* user_data) {
    (void)image_idx;
    (void)warn;
    (void)req_width;
    (void)req_height;
    (void)user_data;

    int w, h, comp;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);
    if (!data) {
        if (err) {
            *err = "Failed to load image with stb_image";
        }
        return false;
    }

    image->width = w;
    image->height = h;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.resize(w * h * 4);
    memcpy(image->image.data(), data, w * h * 4);

    stbi_image_free(data);
    return true;
}

// Dummy image loader for hasSkeleton() - we don't need actual textures, just structure
static bool DummyImageLoader(tinygltf::Image*, const int, std::string*, std::string*,
                             int, int, const unsigned char*, int, void*) {
    return true;  // Pretend we loaded successfully
}

bool SkinnedGLBLoader::hasSkeleton(const std::string& filepath) {
    std::cout << "[SkinnedGLBLoader::hasSkeleton] Checking: " << filepath << std::endl;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // Use dummy image loader - we only care about skeleton structure, not textures
    loader.SetImageLoader(DummyImageLoader, nullptr);

    std::string ext = std::filesystem::path(filepath).extension().string();
    bool isBinary = (ext == ".glb" || ext == ".GLB");

    bool success = isBinary
        ? loader.LoadBinaryFromFile(&model, &err, &warn, filepath)
        : loader.LoadASCIIFromFile(&model, &err, &warn, filepath);

    if (!success) {
        std::cout << "[SkinnedGLBLoader::hasSkeleton] Failed to load file!" << std::endl;
        if (!err.empty()) std::cout << "  Error: " << err << std::endl;
        return false;
    }

    std::cout << "[SkinnedGLBLoader::hasSkeleton] Skins count: " << model.skins.size() << std::endl;
    std::cout << "[SkinnedGLBLoader::hasSkeleton] Animations count: " << model.animations.size() << std::endl;

    return !model.skins.empty();
}

SkinnedLoadResult SkinnedGLBLoader::load(const std::string& filepath) {
    SkinnedLoadResult result;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    loader.SetImageLoader(LoadImageDataSkinned, nullptr);

    std::string ext = std::filesystem::path(filepath).extension().string();
    bool isBinary = (ext == ".glb" || ext == ".GLB");

    bool success = isBinary
        ? loader.LoadBinaryFromFile(&model, &err, &warn, filepath)
        : loader.LoadASCIIFromFile(&model, &err, &warn, filepath);

    if (!warn.empty()) {
        std::cout << "GLB Warning: " << warn << std::endl;
    }

    if (!success) {
        result.error = err.empty() ? "Unknown error loading GLB file" : err;
        return result;
    }

    std::string baseName = std::filesystem::path(filepath).stem().string();

    // === Parse Skeleton ===
    if (!model.skins.empty()) {
        const auto& skin = model.skins[0];  // Use first skin
        result.skeleton = std::make_unique<Skeleton>();

        // Get inverse bind matrices
        std::vector<glm::mat4> inverseBindMatrices;
        if (skin.inverseBindMatrices >= 0) {
            const auto& accessor = model.accessors[skin.inverseBindMatrices];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];

            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

            for (size_t i = 0; i < accessor.count; i++) {
                glm::mat4 mat;
                memcpy(glm::value_ptr(mat), data + i * 16, sizeof(glm::mat4));
                inverseBindMatrices.push_back(mat);
            }
        }

        // Build bone hierarchy
        result.skeleton->bones.resize(skin.joints.size());

        for (size_t i = 0; i < skin.joints.size(); i++) {
            int nodeIdx = skin.joints[i];
            const auto& node = model.nodes[nodeIdx];

            Bone& bone = result.skeleton->bones[i];
            bone.name = node.name.empty() ? "bone_" + std::to_string(i) : node.name;

            if (i < inverseBindMatrices.size()) {
                bone.inverseBindMatrix = inverseBindMatrices[i];
            }

            // Compute local transform from node TRS
            glm::mat4 T = glm::mat4(1.0f);
            glm::mat4 R = glm::mat4(1.0f);
            glm::mat4 S = glm::mat4(1.0f);

            if (!node.translation.empty()) {
                T = glm::translate(glm::mat4(1.0f), glm::vec3(
                    node.translation[0], node.translation[1], node.translation[2]));
            }
            if (!node.rotation.empty()) {
                glm::quat q(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
                R = glm::mat4_cast(q);
            }
            if (!node.scale.empty()) {
                S = glm::scale(glm::mat4(1.0f), glm::vec3(
                    node.scale[0], node.scale[1], node.scale[2]));
            }

            bone.localTransform = T * R * S;

            // Find parent
            bone.parentIndex = -1;
            for (size_t j = 0; j < skin.joints.size(); j++) {
                if (i == j) continue;
                int parentNodeIdx = skin.joints[j];
                const auto& parentNode = model.nodes[parentNodeIdx];
                for (int child : parentNode.children) {
                    if (child == nodeIdx) {
                        bone.parentIndex = static_cast<int>(j);
                        break;
                    }
                }
                if (bone.parentIndex >= 0) break;
            }

            result.skeleton->boneNameToIndex[bone.name] = static_cast<int>(i);
        }

        std::cout << "Loaded skeleton with " << result.skeleton->bones.size() << " bones" << std::endl;
    }

    // === Parse Animations ===
    for (const auto& anim : model.animations) {
        AnimationClip clip;
        clip.name = anim.name.empty() ? "animation_" + std::to_string(result.animations.size()) : anim.name;
        clip.duration = 0.0f;

        for (const auto& channel : anim.channels) {
            if (channel.target_node < 0) continue;

            // Find bone index for this node
            int boneIndex = -1;
            if (result.skeleton) {
                for (size_t i = 0; i < model.skins[0].joints.size(); i++) {
                    if (model.skins[0].joints[i] == channel.target_node) {
                        boneIndex = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (boneIndex < 0) continue;

            // Get or create channel for this bone
            AnimationChannel* animChannel = nullptr;
            for (auto& ch : clip.channels) {
                if (ch.boneIndex == boneIndex) {
                    animChannel = &ch;
                    break;
                }
            }
            if (!animChannel) {
                clip.channels.push_back({});
                animChannel = &clip.channels.back();
                animChannel->boneIndex = boneIndex;
            }

            const auto& sampler = anim.samplers[channel.sampler];

            // Get input (time) data
            const auto& inputAccessor = model.accessors[sampler.input];
            const auto& inputBufferView = model.bufferViews[inputAccessor.bufferView];
            const auto& inputBuffer = model.buffers[inputBufferView.buffer];
            const float* times = reinterpret_cast<const float*>(
                inputBuffer.data.data() + inputBufferView.byteOffset + inputAccessor.byteOffset);

            // Get output data
            const auto& outputAccessor = model.accessors[sampler.output];
            const auto& outputBufferView = model.bufferViews[outputAccessor.bufferView];
            const auto& outputBuffer = model.buffers[outputBufferView.buffer];
            const float* values = reinterpret_cast<const float*>(
                outputBuffer.data.data() + outputBufferView.byteOffset + outputAccessor.byteOffset);

            // Update clip duration
            if (inputAccessor.count > 0) {
                float maxTime = times[inputAccessor.count - 1];
                clip.duration = std::max(clip.duration, maxTime);
            }

            // Parse based on target path
            if (channel.target_path == "translation") {
                for (size_t i = 0; i < inputAccessor.count; i++) {
                    animChannel->positionTimes.push_back(times[i]);
                    animChannel->positions.push_back(glm::vec3(
                        values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2]));
                }
            } else if (channel.target_path == "rotation") {
                for (size_t i = 0; i < inputAccessor.count; i++) {
                    animChannel->rotationTimes.push_back(times[i]);
                    // glTF quaternions are (x, y, z, w), GLM is (w, x, y, z)
                    animChannel->rotations.push_back(glm::quat(
                        values[i * 4 + 3], values[i * 4 + 0],
                        values[i * 4 + 1], values[i * 4 + 2]));
                }
            } else if (channel.target_path == "scale") {
                for (size_t i = 0; i < inputAccessor.count; i++) {
                    animChannel->scaleTimes.push_back(times[i]);
                    animChannel->scales.push_back(glm::vec3(
                        values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2]));
                }
            }
        }

        if (!clip.channels.empty()) {
            std::cout << "Loaded animation '" << clip.name << "' duration: " << clip.duration << "s" << std::endl;
            result.animations.push_back(std::move(clip));
        }
    }

    // === Parse Meshes with Skin Data ===
    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); meshIdx++) {
        const auto& gltfMesh = model.meshes[meshIdx];

        for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); primIdx++) {
            const auto& primitive = gltfMesh.primitives[primIdx];

            if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) {
                continue;
            }

            SkinnedLoadedMesh loadedMesh;
            // Use filename as the base name (not internal mesh names like "Ch36")
            // Append mesh index only if there are multiple meshes
            loadedMesh.name = (model.meshes.size() == 1)
                ? baseName
                : baseName + "_" + std::to_string(meshIdx);

            // Get position accessor
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) continue;

            const auto& posAccessor = model.accessors[posIt->second];
            const auto& posBufferView = model.bufferViews[posAccessor.bufferView];
            const auto& posBuffer = model.buffers[posBufferView.buffer];
            const uint8_t* posData = posBuffer.data.data() + posBufferView.byteOffset + posAccessor.byteOffset;
            size_t posStride = posBufferView.byteStride ? posBufferView.byteStride : sizeof(float) * 3;

            // Normal
            const uint8_t* normData = nullptr;
            size_t normStride = 0;
            auto normIt = primitive.attributes.find("NORMAL");
            if (normIt != primitive.attributes.end()) {
                const auto& normAccessor = model.accessors[normIt->second];
                const auto& normBufferView = model.bufferViews[normAccessor.bufferView];
                const auto& normBuffer = model.buffers[normBufferView.buffer];
                normData = normBuffer.data.data() + normBufferView.byteOffset + normAccessor.byteOffset;
                normStride = normBufferView.byteStride ? normBufferView.byteStride : sizeof(float) * 3;
            }

            // UV
            const uint8_t* uvData = nullptr;
            size_t uvStride = 0;
            auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end()) {
                const auto& uvAccessor = model.accessors[uvIt->second];
                const auto& uvBufferView = model.bufferViews[uvAccessor.bufferView];
                const auto& uvBuffer = model.buffers[uvBufferView.buffer];
                uvData = uvBuffer.data.data() + uvBufferView.byteOffset + uvAccessor.byteOffset;
                uvStride = uvBufferView.byteStride ? uvBufferView.byteStride : sizeof(float) * 2;
            }

            // Joints (bone indices)
            const uint8_t* jointsData = nullptr;
            size_t jointsStride = 0;
            int jointsComponentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
            auto jointsIt = primitive.attributes.find("JOINTS_0");
            if (jointsIt != primitive.attributes.end()) {
                const auto& jointsAccessor = model.accessors[jointsIt->second];
                const auto& jointsBufferView = model.bufferViews[jointsAccessor.bufferView];
                const auto& jointsBuffer = model.buffers[jointsBufferView.buffer];
                jointsData = jointsBuffer.data.data() + jointsBufferView.byteOffset + jointsAccessor.byteOffset;
                jointsComponentType = jointsAccessor.componentType;

                size_t componentSize = 2;  // Default UNSIGNED_SHORT
                if (jointsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) componentSize = 1;

                jointsStride = jointsBufferView.byteStride ? jointsBufferView.byteStride : componentSize * 4;
            }

            // Weights
            const uint8_t* weightsData = nullptr;
            size_t weightsStride = 0;
            auto weightsIt = primitive.attributes.find("WEIGHTS_0");
            if (weightsIt != primitive.attributes.end()) {
                const auto& weightsAccessor = model.accessors[weightsIt->second];
                const auto& weightsBufferView = model.bufferViews[weightsAccessor.bufferView];
                const auto& weightsBuffer = model.buffers[weightsBufferView.buffer];
                weightsData = weightsBuffer.data.data() + weightsBufferView.byteOffset + weightsAccessor.byteOffset;
                weightsStride = weightsBufferView.byteStride ? weightsBufferView.byteStride : sizeof(float) * 4;
            }

            // Build vertices
            size_t vertexCount = posAccessor.count;
            loadedMesh.vertices.reserve(vertexCount);

            for (size_t i = 0; i < vertexCount; i++) {
                SkinnedVertex vertex{};

                // Position
                const float* pos = reinterpret_cast<const float*>(posData + i * posStride);
                vertex.position = glm::vec3(pos[0], pos[1], pos[2]);

                // Normal
                if (normData) {
                    const float* norm = reinterpret_cast<const float*>(normData + i * normStride);
                    vertex.normal = glm::vec3(norm[0], norm[1], norm[2]);
                } else {
                    vertex.normal = glm::vec3(0, 1, 0);
                }

                // UV
                if (uvData) {
                    const float* uv = reinterpret_cast<const float*>(uvData + i * uvStride);
                    vertex.texCoord = glm::vec2(uv[0], uv[1]);
                }

                // Default color
                vertex.color = glm::vec4(1.0f);

                // Joints
                if (jointsData) {
                    if (jointsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        const uint8_t* j = jointsData + i * jointsStride;
                        vertex.joints = glm::ivec4(j[0], j[1], j[2], j[3]);
                    } else {  // UNSIGNED_SHORT
                        const uint16_t* j = reinterpret_cast<const uint16_t*>(jointsData + i * jointsStride);
                        vertex.joints = glm::ivec4(j[0], j[1], j[2], j[3]);
                    }
                } else {
                    vertex.joints = glm::ivec4(0);
                }

                // Weights
                if (weightsData) {
                    const float* w = reinterpret_cast<const float*>(weightsData + i * weightsStride);
                    vertex.weights = glm::vec4(w[0], w[1], w[2], w[3]);
                } else {
                    vertex.weights = glm::vec4(1, 0, 0, 0);  // All weight on first bone
                }

                loadedMesh.vertices.push_back(vertex);
            }

            // Indices
            if (primitive.indices >= 0) {
                const auto& indexAccessor = model.accessors[primitive.indices];
                const auto& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                const auto& indexBuffer = model.buffers[indexBufferView.buffer];
                const uint8_t* indexData = indexBuffer.data.data() +
                    indexBufferView.byteOffset + indexAccessor.byteOffset;

                size_t indexCount = indexAccessor.count;
                loadedMesh.indices.reserve(indexCount);

                switch (indexAccessor.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        for (size_t i = 0; i < indexCount; i++) {
                            loadedMesh.indices.push_back(indexData[i]);
                        }
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                        const uint16_t* indices16 = reinterpret_cast<const uint16_t*>(indexData);
                        for (size_t i = 0; i < indexCount; i++) {
                            loadedMesh.indices.push_back(indices16[i]);
                        }
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                        const uint32_t* indices32 = reinterpret_cast<const uint32_t*>(indexData);
                        for (size_t i = 0; i < indexCount; i++) {
                            loadedMesh.indices.push_back(indices32[i]);
                        }
                        break;
                    }
                }
            } else {
                for (size_t i = 0; i < vertexCount; i++) {
                    loadedMesh.indices.push_back(static_cast<uint32_t>(i));
                }
            }

            // Texture
            if (primitive.material >= 0 && primitive.material < static_cast<int>(model.materials.size())) {
                const auto& material = model.materials[primitive.material];
                int texIndex = material.pbrMetallicRoughness.baseColorTexture.index;
                if (texIndex >= 0 && texIndex < static_cast<int>(model.textures.size())) {
                    const auto& texture = model.textures[texIndex];
                    if (texture.source >= 0 && texture.source < static_cast<int>(model.images.size())) {
                        const auto& image = model.images[texture.source];
                        if (!image.image.empty()) {
                            loadedMesh.textureData = image.image;
                            loadedMesh.textureWidth = image.width;
                            loadedMesh.textureHeight = image.height;
                            loadedMesh.hasTexture = true;
                        }
                    }
                }
            }

            if (!loadedMesh.vertices.empty()) {
                result.meshes.push_back(std::move(loadedMesh));
            }
        }
    }

    // Success if we have meshes OR animations (animation-only files are valid)
    result.success = !result.meshes.empty() || !result.animations.empty();
    if (!result.success && result.error.empty()) {
        result.error = "No meshes or animations found in file";
    }

    return result;
}

} // namespace eden
