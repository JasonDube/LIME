// tinygltf implementation - but don't redefine STB implementations
// as they're already included in TextureManager.cpp
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "GLBLoader.hpp"
#include <tiny_gltf.h>
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <iostream>
#include <filesystem>

// Required image callbacks when TINYGLTF_NO_STB_IMAGE is defined
// These must be in the tinygltf namespace
namespace tinygltf {

bool LoadImageData(Image* image, const int image_idx, std::string* err,
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

bool WriteImageData(const std::string* basepath, const std::string* filename,
                    const Image* image, bool embedImages,
                    const URICallbacks* uri_cb, std::string* out_uri, void* user_data) {
    (void)basepath;
    (void)filename;
    (void)uri_cb;
    (void)out_uri;
    (void)user_data;

    // For embedded images, we don't need to write to disk
    if (embedImages) {
        return true;
    }

    // For external images, write to file
    if (filename && !filename->empty() && basepath && !basepath->empty()) {
        std::string fullpath = *basepath + "/" + *filename;
        int result = stbi_write_png(fullpath.c_str(), image->width, image->height,
                                    image->component, image->image.data(),
                                    image->width * image->component);
        return result != 0;
    }

    return true;
}

} // namespace tinygltf

// Local helper for setting up loader callback
static bool LocalLoadImageData(tinygltf::Image* image, const int image_idx, std::string* err,
                               std::string* warn, int req_width, int req_height,
                               const unsigned char* bytes, int size, void* user_data) {
    return tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, user_data);
}

namespace eden {

// Base64 encoding/decoding for storing binary data in JSON extras
static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* data, size_t len) {
    std::string ret;
    ret.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = (unsigned int)data[i] << 16;
        if (i + 1 < len) n |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) n |= (unsigned int)data[i + 2];
        ret += base64_chars[(n >> 18) & 0x3F];
        ret += base64_chars[(n >> 12) & 0x3F];
        ret += (i + 1 < len) ? base64_chars[(n >> 6) & 0x3F] : '=';
        ret += (i + 2 < len) ? base64_chars[n & 0x3F] : '=';
    }
    return ret;
}

static std::vector<unsigned char> base64_decode(const std::string& encoded) {
    std::vector<unsigned char> ret;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            ret.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return ret;
}

LoadResult GLBLoader::load(const std::string& filepath) {
    LoadResult result;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    // Set custom image loader callback
    loader.SetImageLoader(LocalLoadImageData, nullptr);

    // Determine if binary or ASCII
    std::string ext = std::filesystem::path(filepath).extension().string();
    bool isBinary = (ext == ".glb" || ext == ".GLB");

    bool success = false;
    if (isBinary) {
        success = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }

    if (!warn.empty()) {
        std::cout << "GLB Warning: " << warn << std::endl;
    }

    if (!success) {
        result.error = err.empty() ? "Unknown error loading GLB file" : err;
        return result;
    }

    // Extract filename for naming
    std::string baseName = std::filesystem::path(filepath).stem().string();

    // Process all meshes in the model
    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); meshIdx++) {
        const auto& gltfMesh = model.meshes[meshIdx];

        for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); primIdx++) {
            const auto& primitive = gltfMesh.primitives[primIdx];

            // Only support triangles
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) {
                continue;
            }

            LoadedMesh loadedMesh;
            // Use filename as the base name (not internal mesh names like "Ch36")
            // Append mesh index only if there are multiple meshes
            loadedMesh.name = (model.meshes.size() == 1)
                ? baseName
                : baseName + "_" + std::to_string(meshIdx);

            loadedMesh.bounds.min = glm::vec3(INFINITY);
            loadedMesh.bounds.max = glm::vec3(-INFINITY);

            // Get position accessor
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) {
                continue;
            }

            const auto& posAccessor = model.accessors[posIt->second];
            const auto& posBufferView = model.bufferViews[posAccessor.bufferView];
            const auto& posBuffer = model.buffers[posBufferView.buffer];

            const uint8_t* posData = posBuffer.data.data() + posBufferView.byteOffset + posAccessor.byteOffset;
            size_t posStride = posBufferView.byteStride ? posBufferView.byteStride : sizeof(float) * 3;

            // Get normal accessor (optional)
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

            // Get UV accessor (optional)
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

            // Get color accessor (optional)
            const uint8_t* colorData = nullptr;
            size_t colorStride = 0;
            int colorComponents = 0;
            auto colorIt = primitive.attributes.find("COLOR_0");
            if (colorIt != primitive.attributes.end()) {
                const auto& colorAccessor = model.accessors[colorIt->second];
                const auto& colorBufferView = model.bufferViews[colorAccessor.bufferView];
                const auto& colorBuffer = model.buffers[colorBufferView.buffer];
                colorData = colorBuffer.data.data() + colorBufferView.byteOffset + colorAccessor.byteOffset;
                colorComponents = (colorAccessor.type == TINYGLTF_TYPE_VEC3) ? 3 : 4;
                colorStride = colorBufferView.byteStride ? colorBufferView.byteStride : sizeof(float) * colorComponents;
            }

            // Build vertices using ModelVertex
            size_t vertexCount = posAccessor.count;
            loadedMesh.vertices.reserve(vertexCount);

            for (size_t i = 0; i < vertexCount; i++) {
                ModelVertex vertex{};

                // Position (with proper stride)
                const float* pos = reinterpret_cast<const float*>(posData + i * posStride);
                vertex.position = glm::vec3(pos[0], pos[1], pos[2]);

                // Update bounds
                loadedMesh.bounds.min = glm::min(loadedMesh.bounds.min, vertex.position);
                loadedMesh.bounds.max = glm::max(loadedMesh.bounds.max, vertex.position);

                // Normal (with proper stride)
                if (normData) {
                    const float* norm = reinterpret_cast<const float*>(normData + i * normStride);
                    vertex.normal = glm::vec3(norm[0], norm[1], norm[2]);
                } else {
                    vertex.normal = glm::vec3(0, 1, 0);
                }

                // UV (with proper stride)
                if (uvData) {
                    const float* uv = reinterpret_cast<const float*>(uvData + i * uvStride);
                    vertex.texCoord = glm::vec2(uv[0], uv[1]);
                } else {
                    vertex.texCoord = glm::vec2(0);
                }

                // Color (with proper stride)
                if (colorData) {
                    const float* col = reinterpret_cast<const float*>(colorData + i * colorStride);
                    vertex.color = glm::vec4(
                        col[0], col[1], col[2],
                        colorComponents > 3 ? col[3] : 1.0f
                    );
                } else {
                    vertex.color = glm::vec4(1.0f);  // Default white
                }

                loadedMesh.vertices.push_back(vertex);
            }

            // Get indices
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
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        {
                            const uint16_t* indices16 = reinterpret_cast<const uint16_t*>(indexData);
                            for (size_t i = 0; i < indexCount; i++) {
                                loadedMesh.indices.push_back(indices16[i]);
                            }
                        }
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        {
                            const uint32_t* indices32 = reinterpret_cast<const uint32_t*>(indexData);
                            for (size_t i = 0; i < indexCount; i++) {
                                loadedMesh.indices.push_back(indices32[i]);
                            }
                        }
                        break;
                }
            } else {
                for (size_t i = 0; i < vertexCount; i++) {
                    loadedMesh.indices.push_back(static_cast<uint32_t>(i));
                }
            }

            // Generate normals if not provided
            if (!normData && loadedMesh.indices.size() >= 3) {
                for (size_t i = 0; i + 2 < loadedMesh.indices.size(); i += 3) {
                    uint32_t i0 = loadedMesh.indices[i];
                    uint32_t i1 = loadedMesh.indices[i + 1];
                    uint32_t i2 = loadedMesh.indices[i + 2];

                    glm::vec3 normal = calculateNormal(
                        loadedMesh.vertices[i0].position,
                        loadedMesh.vertices[i1].position,
                        loadedMesh.vertices[i2].position
                    );

                    loadedMesh.vertices[i0].normal = normal;
                    loadedMesh.vertices[i1].normal = normal;
                    loadedMesh.vertices[i2].normal = normal;
                }
            }

            // Extract texture from material
            if (primitive.material >= 0 && primitive.material < static_cast<int>(model.materials.size())) {
                const auto& material = model.materials[primitive.material];

                // Get base color texture
                int texIndex = material.pbrMetallicRoughness.baseColorTexture.index;
                if (texIndex >= 0 && texIndex < static_cast<int>(model.textures.size())) {
                    const auto& texture = model.textures[texIndex];
                    if (texture.source >= 0 && texture.source < static_cast<int>(model.images.size())) {
                        const auto& image = model.images[texture.source];
                        if (!image.image.empty()) {
                            loadedMesh.texture.data = image.image;
                            loadedMesh.texture.width = image.width;
                            loadedMesh.texture.height = image.height;
                            loadedMesh.hasTexture = true;
                            std::cout << "  Loaded texture: " << image.width << "x" << image.height << std::endl;
                        }
                    }
                }
            }

            // Check for EDEN half-edge data in mesh extras
            if (gltfMesh.extras.IsObject()) {
                const auto& extras = gltfMesh.extras.Get<tinygltf::Value::Object>();
                auto versionIt = extras.find("eden_he_version");
                if (versionIt != extras.end() && versionIt->second.IsInt()) {
                    // Found EDEN half-edge data
                    auto vertCountIt = extras.find("he_vert_count");
                    auto edgeCountIt = extras.find("he_edge_count");
                    auto faceCountIt = extras.find("he_face_count");
                    auto vertsIt = extras.find("he_vertices");
                    auto edgesIt = extras.find("he_edges");
                    auto facesIt = extras.find("he_faces");

                    if (vertCountIt != extras.end() && edgeCountIt != extras.end() &&
                        faceCountIt != extras.end() && vertsIt != extras.end() &&
                        edgesIt != extras.end() && facesIt != extras.end()) {

                        int vertCount = vertCountIt->second.GetNumberAsInt();
                        int edgeCount = edgeCountIt->second.GetNumberAsInt();
                        int faceCount = faceCountIt->second.GetNumberAsInt();

                        auto vertData = base64_decode(vertsIt->second.Get<std::string>());
                        auto edgeData = base64_decode(edgesIt->second.Get<std::string>());
                        auto faceData = base64_decode(facesIt->second.Get<std::string>());

                        StoredHEData heData;
                        heData.vertices.resize(vertCount);
                        heData.halfEdges.resize(edgeCount);
                        heData.faces.resize(faceCount);

                        if (vertData.size() >= vertCount * sizeof(SceneObject::StoredHEVertex))
                            memcpy(heData.vertices.data(), vertData.data(), vertCount * sizeof(SceneObject::StoredHEVertex));
                        if (edgeData.size() >= edgeCount * sizeof(SceneObject::StoredHalfEdge))
                            memcpy(heData.halfEdges.data(), edgeData.data(), edgeCount * sizeof(SceneObject::StoredHalfEdge));
                        if (faceData.size() >= faceCount * sizeof(SceneObject::StoredHEFace))
                            memcpy(heData.faces.data(), faceData.data(), faceCount * sizeof(SceneObject::StoredHEFace));

                        loadedMesh.halfEdgeData = std::move(heData);
                        std::cout << "  Loaded EDEN half-edge data: " << faceCount << " faces" << std::endl;
                    }
                }
            }

            if (!loadedMesh.vertices.empty()) {
                result.meshes.push_back(std::move(loadedMesh));
            }
        }
    }

    result.success = !result.meshes.empty();
    if (!result.success && result.error.empty()) {
        result.error = "No valid meshes found in file";
    }

    return result;
}

bool GLBLoader::save(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const std::string& meshName) {
    if (vertices.empty() || indices.empty()) {
        std::cerr << "GLBLoader::save - Empty mesh data" << std::endl;
        return false;
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF gltf;

    // Create a single buffer to hold all data
    tinygltf::Buffer buffer;

    // Calculate sizes
    size_t positionByteLength = vertices.size() * sizeof(float) * 3;
    size_t normalByteLength = vertices.size() * sizeof(float) * 3;
    size_t texCoordByteLength = vertices.size() * sizeof(float) * 2;
    size_t colorByteLength = vertices.size() * sizeof(float) * 4;
    size_t indexByteLength = indices.size() * sizeof(uint32_t);

    // Calculate offsets (aligned to 4 bytes)
    size_t positionOffset = 0;
    size_t normalOffset = positionOffset + positionByteLength;
    size_t texCoordOffset = normalOffset + normalByteLength;
    size_t colorOffset = texCoordOffset + texCoordByteLength;
    size_t indexOffset = colorOffset + colorByteLength;
    size_t totalSize = indexOffset + indexByteLength;

    buffer.data.resize(totalSize);

    // Copy position data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + positionOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].position.x;
        dst[1] = vertices[i].position.y;
        dst[2] = vertices[i].position.z;
    }

    // Copy normal data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + normalOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].normal.x;
        dst[1] = vertices[i].normal.y;
        dst[2] = vertices[i].normal.z;
    }

    // Copy texcoord data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + texCoordOffset + i * sizeof(float) * 2);
        dst[0] = vertices[i].texCoord.x;
        dst[1] = vertices[i].texCoord.y;
    }

    // Copy color data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + colorOffset + i * sizeof(float) * 4);
        dst[0] = vertices[i].color.r;
        dst[1] = vertices[i].color.g;
        dst[2] = vertices[i].color.b;
        dst[3] = vertices[i].color.a;
    }

    // Copy index data
    memcpy(buffer.data.data() + indexOffset, indices.data(), indexByteLength);

    model.buffers.push_back(buffer);

    // Create buffer views
    // Position buffer view
    tinygltf::BufferView positionView;
    positionView.buffer = 0;
    positionView.byteOffset = positionOffset;
    positionView.byteLength = positionByteLength;
    positionView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(positionView);

    // Normal buffer view
    tinygltf::BufferView normalView;
    normalView.buffer = 0;
    normalView.byteOffset = normalOffset;
    normalView.byteLength = normalByteLength;
    normalView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(normalView);

    // TexCoord buffer view
    tinygltf::BufferView texCoordView;
    texCoordView.buffer = 0;
    texCoordView.byteOffset = texCoordOffset;
    texCoordView.byteLength = texCoordByteLength;
    texCoordView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(texCoordView);

    // Color buffer view
    tinygltf::BufferView colorView;
    colorView.buffer = 0;
    colorView.byteOffset = colorOffset;
    colorView.byteLength = colorByteLength;
    colorView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(colorView);

    // Index buffer view
    tinygltf::BufferView indexView;
    indexView.buffer = 0;
    indexView.byteOffset = indexOffset;
    indexView.byteLength = indexByteLength;
    indexView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    model.bufferViews.push_back(indexView);

    // Calculate bounds for position accessor
    glm::vec3 minPos(INFINITY), maxPos(-INFINITY);
    for (const auto& v : vertices) {
        minPos = glm::min(minPos, v.position);
        maxPos = glm::max(maxPos, v.position);
    }

    // Create accessors
    // Position accessor
    tinygltf::Accessor posAccessor;
    posAccessor.bufferView = 0;
    posAccessor.byteOffset = 0;
    posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    posAccessor.count = vertices.size();
    posAccessor.type = TINYGLTF_TYPE_VEC3;
    posAccessor.minValues = {minPos.x, minPos.y, minPos.z};
    posAccessor.maxValues = {maxPos.x, maxPos.y, maxPos.z};
    model.accessors.push_back(posAccessor);

    // Normal accessor
    tinygltf::Accessor normAccessor;
    normAccessor.bufferView = 1;
    normAccessor.byteOffset = 0;
    normAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    normAccessor.count = vertices.size();
    normAccessor.type = TINYGLTF_TYPE_VEC3;
    model.accessors.push_back(normAccessor);

    // TexCoord accessor
    tinygltf::Accessor texAccessor;
    texAccessor.bufferView = 2;
    texAccessor.byteOffset = 0;
    texAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    texAccessor.count = vertices.size();
    texAccessor.type = TINYGLTF_TYPE_VEC2;
    model.accessors.push_back(texAccessor);

    // Color accessor
    tinygltf::Accessor colorAccessor;
    colorAccessor.bufferView = 3;
    colorAccessor.byteOffset = 0;
    colorAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    colorAccessor.count = vertices.size();
    colorAccessor.type = TINYGLTF_TYPE_VEC4;
    model.accessors.push_back(colorAccessor);

    // Index accessor
    tinygltf::Accessor indexAccessor;
    indexAccessor.bufferView = 4;
    indexAccessor.byteOffset = 0;
    indexAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    indexAccessor.count = indices.size();
    indexAccessor.type = TINYGLTF_TYPE_SCALAR;
    model.accessors.push_back(indexAccessor);

    // Create mesh
    tinygltf::Mesh mesh;
    mesh.name = meshName;

    tinygltf::Primitive primitive;
    primitive.attributes["POSITION"] = 0;
    primitive.attributes["NORMAL"] = 1;
    primitive.attributes["TEXCOORD_0"] = 2;
    primitive.attributes["COLOR_0"] = 3;
    primitive.indices = 4;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;

    mesh.primitives.push_back(primitive);
    model.meshes.push_back(mesh);

    // Create node
    tinygltf::Node node;
    node.mesh = 0;
    node.name = meshName;
    model.nodes.push_back(node);

    // Create scene
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    // Set asset info
    model.asset.version = "2.0";
    model.asset.generator = "EDEN Model Editor";

    // Write GLB
    std::string err;
    std::string warn;
    bool success = gltf.WriteGltfSceneToFile(&model, filepath, true, true, true, true);

    if (!success) {
        std::cerr << "GLBLoader::save - Failed to write GLB file: " << filepath << std::endl;
        return false;
    }

    std::cout << "Saved GLB: " << filepath << " (" << vertices.size() << " vertices, "
              << indices.size() / 3 << " triangles)" << std::endl;
    return true;
}

// Helper callback for stbi_write_png_to_func
static void pngWriteCallback(void* context, void* data, int size) {
    std::vector<unsigned char>* buffer = static_cast<std::vector<unsigned char>*>(context);
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

bool GLBLoader::save(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const unsigned char* textureData,
                     int textureWidth,
                     int textureHeight,
                     const std::string& meshName) {
    if (vertices.empty() || indices.empty()) {
        std::cerr << "GLBLoader::save - Empty mesh data" << std::endl;
        return false;
    }

    // If no texture, use the simple version
    if (!textureData || textureWidth <= 0 || textureHeight <= 0) {
        return save(filepath, vertices, indices, meshName);
    }

    // Encode texture as PNG to memory
    std::vector<unsigned char> pngData;
    int pngResult = stbi_write_png_to_func(pngWriteCallback, &pngData,
                                            textureWidth, textureHeight, 4,
                                            textureData, textureWidth * 4);
    if (pngResult == 0 || pngData.empty()) {
        std::cerr << "GLBLoader::save - Failed to encode texture as PNG" << std::endl;
        return save(filepath, vertices, indices, meshName);  // Fall back to no texture
    }

    std::cout << "Encoded texture as PNG: " << pngData.size() << " bytes" << std::endl;

    tinygltf::Model model;
    tinygltf::TinyGLTF gltf;

    // Create buffer to hold all data
    tinygltf::Buffer buffer;

    // Calculate sizes
    size_t positionByteLength = vertices.size() * sizeof(float) * 3;
    size_t normalByteLength = vertices.size() * sizeof(float) * 3;
    size_t texCoordByteLength = vertices.size() * sizeof(float) * 2;
    size_t colorByteLength = vertices.size() * sizeof(float) * 4;
    size_t indexByteLength = indices.size() * sizeof(uint32_t);
    size_t pngByteLength = pngData.size();

    // Calculate offsets (aligned to 4 bytes)
    size_t positionOffset = 0;
    size_t normalOffset = positionOffset + positionByteLength;
    size_t texCoordOffset = normalOffset + normalByteLength;
    size_t colorOffset = texCoordOffset + texCoordByteLength;
    size_t indexOffset = colorOffset + colorByteLength;
    size_t pngOffset = indexOffset + indexByteLength;
    // Align PNG to 4 bytes
    pngOffset = (pngOffset + 3) & ~3;
    size_t totalSize = pngOffset + pngByteLength;

    buffer.data.resize(totalSize);

    // Copy position data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + positionOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].position.x;
        dst[1] = vertices[i].position.y;
        dst[2] = vertices[i].position.z;
    }

    // Copy normal data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + normalOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].normal.x;
        dst[1] = vertices[i].normal.y;
        dst[2] = vertices[i].normal.z;
    }

    // Copy texcoord data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + texCoordOffset + i * sizeof(float) * 2);
        dst[0] = vertices[i].texCoord.x;
        dst[1] = vertices[i].texCoord.y;
    }

    // Copy color data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + colorOffset + i * sizeof(float) * 4);
        dst[0] = vertices[i].color.r;
        dst[1] = vertices[i].color.g;
        dst[2] = vertices[i].color.b;
        dst[3] = vertices[i].color.a;
    }

    // Copy index data
    memcpy(buffer.data.data() + indexOffset, indices.data(), indexByteLength);

    // Copy PNG data
    memcpy(buffer.data.data() + pngOffset, pngData.data(), pngByteLength);

    model.buffers.push_back(buffer);

    // Create buffer views
    // 0: Position
    tinygltf::BufferView positionView;
    positionView.buffer = 0;
    positionView.byteOffset = positionOffset;
    positionView.byteLength = positionByteLength;
    positionView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(positionView);

    // 1: Normal
    tinygltf::BufferView normalView;
    normalView.buffer = 0;
    normalView.byteOffset = normalOffset;
    normalView.byteLength = normalByteLength;
    normalView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(normalView);

    // 2: TexCoord
    tinygltf::BufferView texCoordView;
    texCoordView.buffer = 0;
    texCoordView.byteOffset = texCoordOffset;
    texCoordView.byteLength = texCoordByteLength;
    texCoordView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(texCoordView);

    // 3: Color
    tinygltf::BufferView colorView;
    colorView.buffer = 0;
    colorView.byteOffset = colorOffset;
    colorView.byteLength = colorByteLength;
    colorView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(colorView);

    // 4: Index
    tinygltf::BufferView indexView;
    indexView.buffer = 0;
    indexView.byteOffset = indexOffset;
    indexView.byteLength = indexByteLength;
    indexView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    model.bufferViews.push_back(indexView);

    // 5: Image (PNG)
    tinygltf::BufferView imageView;
    imageView.buffer = 0;
    imageView.byteOffset = pngOffset;
    imageView.byteLength = pngByteLength;
    // Note: No target for image buffer views
    model.bufferViews.push_back(imageView);

    // Calculate bounds for position accessor
    glm::vec3 minPos(INFINITY), maxPos(-INFINITY);
    for (const auto& v : vertices) {
        minPos = glm::min(minPos, v.position);
        maxPos = glm::max(maxPos, v.position);
    }

    // Create accessors
    // 0: Position
    tinygltf::Accessor posAccessor;
    posAccessor.bufferView = 0;
    posAccessor.byteOffset = 0;
    posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    posAccessor.count = vertices.size();
    posAccessor.type = TINYGLTF_TYPE_VEC3;
    posAccessor.minValues = {minPos.x, minPos.y, minPos.z};
    posAccessor.maxValues = {maxPos.x, maxPos.y, maxPos.z};
    model.accessors.push_back(posAccessor);

    // 1: Normal
    tinygltf::Accessor normAccessor;
    normAccessor.bufferView = 1;
    normAccessor.byteOffset = 0;
    normAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    normAccessor.count = vertices.size();
    normAccessor.type = TINYGLTF_TYPE_VEC3;
    model.accessors.push_back(normAccessor);

    // 2: TexCoord
    tinygltf::Accessor texAccessor;
    texAccessor.bufferView = 2;
    texAccessor.byteOffset = 0;
    texAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    texAccessor.count = vertices.size();
    texAccessor.type = TINYGLTF_TYPE_VEC2;
    model.accessors.push_back(texAccessor);

    // 3: Color
    tinygltf::Accessor colorAccessor;
    colorAccessor.bufferView = 3;
    colorAccessor.byteOffset = 0;
    colorAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    colorAccessor.count = vertices.size();
    colorAccessor.type = TINYGLTF_TYPE_VEC4;
    model.accessors.push_back(colorAccessor);

    // 4: Index
    tinygltf::Accessor indexAccessor;
    indexAccessor.bufferView = 4;
    indexAccessor.byteOffset = 0;
    indexAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    indexAccessor.count = indices.size();
    indexAccessor.type = TINYGLTF_TYPE_SCALAR;
    model.accessors.push_back(indexAccessor);

    // Create image
    tinygltf::Image image;
    image.bufferView = 5;
    image.mimeType = "image/png";
    image.name = "texture";
    model.images.push_back(image);

    // Create sampler
    tinygltf::Sampler sampler;
    sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
    sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    model.samplers.push_back(sampler);

    // Create texture
    tinygltf::Texture texture;
    texture.source = 0;
    texture.sampler = 0;
    texture.name = "baseColorTexture";
    model.textures.push_back(texture);

    // Create material with texture
    tinygltf::Material material;
    material.name = "paintedMaterial";
    material.pbrMetallicRoughness.baseColorTexture.index = 0;
    material.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = 1.0;
    model.materials.push_back(material);

    // Create mesh with material
    tinygltf::Mesh mesh;
    mesh.name = meshName;

    tinygltf::Primitive primitive;
    primitive.attributes["POSITION"] = 0;
    primitive.attributes["NORMAL"] = 1;
    primitive.attributes["TEXCOORD_0"] = 2;
    primitive.attributes["COLOR_0"] = 3;
    primitive.indices = 4;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.material = 0;  // Reference the material with texture

    mesh.primitives.push_back(primitive);
    model.meshes.push_back(mesh);

    // Create node
    tinygltf::Node node;
    node.mesh = 0;
    node.name = meshName;
    model.nodes.push_back(node);

    // Create scene
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    // Set asset info
    model.asset.version = "2.0";
    model.asset.generator = "EDEN Model Editor";

    // Write GLB
    std::string err;
    std::string warn;
    bool success = gltf.WriteGltfSceneToFile(&model, filepath, true, true, true, true);

    if (!success) {
        std::cerr << "GLBLoader::save - Failed to write GLB file: " << filepath << std::endl;
        return false;
    }

    std::cout << "Saved GLB with texture: " << filepath << " (" << vertices.size() << " vertices, "
              << indices.size() / 3 << " triangles, " << textureWidth << "x" << textureHeight << " texture)" << std::endl;
    return true;
}

bool GLBLoader::saveWithHalfEdgeData(const std::string& filepath,
                     const std::vector<ModelVertex>& vertices,
                     const std::vector<uint32_t>& indices,
                     const StoredHEData& heData,
                     const unsigned char* textureData,
                     int textureWidth,
                     int textureHeight,
                     const std::string& meshName) {
    if (vertices.empty() || indices.empty()) {
        std::cerr << "GLBLoader::saveWithHalfEdgeData - Empty mesh data" << std::endl;
        return false;
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF gltf;

    // Serialize half-edge data to binary
    std::vector<unsigned char> heVertData(heData.vertices.size() * sizeof(SceneObject::StoredHEVertex));
    std::vector<unsigned char> heEdgeData(heData.halfEdges.size() * sizeof(SceneObject::StoredHalfEdge));
    std::vector<unsigned char> heFaceData(heData.faces.size() * sizeof(SceneObject::StoredHEFace));

    if (!heData.vertices.empty())
        memcpy(heVertData.data(), heData.vertices.data(), heVertData.size());
    if (!heData.halfEdges.empty())
        memcpy(heEdgeData.data(), heData.halfEdges.data(), heEdgeData.size());
    if (!heData.faces.empty())
        memcpy(heFaceData.data(), heData.faces.data(), heFaceData.size());

    // Create buffer and buffer views (same as regular save)
    tinygltf::Buffer buffer;

    size_t positionByteLength = vertices.size() * sizeof(float) * 3;
    size_t normalByteLength = vertices.size() * sizeof(float) * 3;
    size_t texCoordByteLength = vertices.size() * sizeof(float) * 2;
    size_t colorByteLength = vertices.size() * sizeof(float) * 4;
    size_t indexByteLength = indices.size() * sizeof(uint32_t);

    size_t positionOffset = 0;
    size_t normalOffset = positionOffset + positionByteLength;
    size_t texCoordOffset = normalOffset + normalByteLength;
    size_t colorOffset = texCoordOffset + texCoordByteLength;
    size_t indexOffset = colorOffset + colorByteLength;
    size_t totalSize = indexOffset + indexByteLength;

    buffer.data.resize(totalSize);

    // Copy vertex data
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + positionOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].position.x;
        dst[1] = vertices[i].position.y;
        dst[2] = vertices[i].position.z;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + normalOffset + i * sizeof(float) * 3);
        dst[0] = vertices[i].normal.x;
        dst[1] = vertices[i].normal.y;
        dst[2] = vertices[i].normal.z;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + texCoordOffset + i * sizeof(float) * 2);
        dst[0] = vertices[i].texCoord.x;
        dst[1] = vertices[i].texCoord.y;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        float* dst = reinterpret_cast<float*>(buffer.data.data() + colorOffset + i * sizeof(float) * 4);
        dst[0] = vertices[i].color.r;
        dst[1] = vertices[i].color.g;
        dst[2] = vertices[i].color.b;
        dst[3] = vertices[i].color.a;
    }
    memcpy(buffer.data.data() + indexOffset, indices.data(), indexByteLength);

    model.buffers.push_back(buffer);

    // Create buffer views
    tinygltf::BufferView posView, normView, texView, colorView, indexView;
    posView.buffer = 0; posView.byteOffset = positionOffset; posView.byteLength = positionByteLength;
    posView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    normView.buffer = 0; normView.byteOffset = normalOffset; normView.byteLength = normalByteLength;
    normView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    texView.buffer = 0; texView.byteOffset = texCoordOffset; texView.byteLength = texCoordByteLength;
    texView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    colorView.buffer = 0; colorView.byteOffset = colorOffset; colorView.byteLength = colorByteLength;
    colorView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    indexView.buffer = 0; indexView.byteOffset = indexOffset; indexView.byteLength = indexByteLength;
    indexView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;

    model.bufferViews.push_back(posView);
    model.bufferViews.push_back(normView);
    model.bufferViews.push_back(texView);
    model.bufferViews.push_back(colorView);
    model.bufferViews.push_back(indexView);

    // Calculate bounds
    glm::vec3 minPos(INFINITY), maxPos(-INFINITY);
    for (const auto& v : vertices) {
        minPos = glm::min(minPos, v.position);
        maxPos = glm::max(maxPos, v.position);
    }

    // Create accessors
    tinygltf::Accessor posAcc, normAcc, texAcc, colorAcc, indexAcc;
    posAcc.bufferView = 0; posAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    posAcc.count = vertices.size(); posAcc.type = TINYGLTF_TYPE_VEC3;
    posAcc.minValues = {minPos.x, minPos.y, minPos.z};
    posAcc.maxValues = {maxPos.x, maxPos.y, maxPos.z};

    normAcc.bufferView = 1; normAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    normAcc.count = vertices.size(); normAcc.type = TINYGLTF_TYPE_VEC3;

    texAcc.bufferView = 2; texAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    texAcc.count = vertices.size(); texAcc.type = TINYGLTF_TYPE_VEC2;

    colorAcc.bufferView = 3; colorAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    colorAcc.count = vertices.size(); colorAcc.type = TINYGLTF_TYPE_VEC4;

    indexAcc.bufferView = 4; indexAcc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    indexAcc.count = indices.size(); indexAcc.type = TINYGLTF_TYPE_SCALAR;

    model.accessors.push_back(posAcc);
    model.accessors.push_back(normAcc);
    model.accessors.push_back(texAcc);
    model.accessors.push_back(colorAcc);
    model.accessors.push_back(indexAcc);

    // Create mesh with extras containing half-edge data
    tinygltf::Mesh mesh;
    mesh.name = meshName;

    tinygltf::Primitive primitive;
    primitive.attributes["POSITION"] = 0;
    primitive.attributes["NORMAL"] = 1;
    primitive.attributes["TEXCOORD_0"] = 2;
    primitive.attributes["COLOR_0"] = 3;
    primitive.indices = 4;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;

    mesh.primitives.push_back(primitive);

    // Store half-edge data in mesh extras as base64
    tinygltf::Value::Object heExtras;
    heExtras["eden_he_version"] = tinygltf::Value(1);
    heExtras["he_vert_count"] = tinygltf::Value((int)heData.vertices.size());
    heExtras["he_edge_count"] = tinygltf::Value((int)heData.halfEdges.size());
    heExtras["he_face_count"] = tinygltf::Value((int)heData.faces.size());
    heExtras["he_vertices"] = tinygltf::Value(base64_encode(heVertData.data(), heVertData.size()));
    heExtras["he_edges"] = tinygltf::Value(base64_encode(heEdgeData.data(), heEdgeData.size()));
    heExtras["he_faces"] = tinygltf::Value(base64_encode(heFaceData.data(), heFaceData.size()));
    mesh.extras = tinygltf::Value(heExtras);

    model.meshes.push_back(mesh);

    // Handle texture if provided
    if (textureData && textureWidth > 0 && textureHeight > 0) {
        // Encode texture as PNG
        std::vector<unsigned char> pngData;
        stbi_write_png_to_func(
            [](void* ctx, void* data, int size) {
                auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
                vec->insert(vec->end(), (unsigned char*)data, (unsigned char*)data + size);
            },
            &pngData, textureWidth, textureHeight, 4, textureData, textureWidth * 4);

        // Add texture buffer
        tinygltf::Buffer texBuffer;
        texBuffer.data = pngData;
        model.buffers.push_back(texBuffer);

        tinygltf::BufferView texBufView;
        texBufView.buffer = 1;
        texBufView.byteOffset = 0;
        texBufView.byteLength = pngData.size();
        model.bufferViews.push_back(texBufView);

        tinygltf::Image image;
        image.bufferView = 5;
        image.mimeType = "image/png";
        image.width = textureWidth;
        image.height = textureHeight;
        model.images.push_back(image);

        tinygltf::Texture texture;
        texture.source = 0;
        model.textures.push_back(texture);

        tinygltf::Material material;
        material.pbrMetallicRoughness.baseColorTexture.index = 0;
        material.pbrMetallicRoughness.metallicFactor = 0.0;
        material.pbrMetallicRoughness.roughnessFactor = 1.0;
        model.materials.push_back(material);

        model.meshes[0].primitives[0].material = 0;
    }

    // Create node and scene
    tinygltf::Node node;
    node.mesh = 0;
    node.name = meshName;
    model.nodes.push_back(node);

    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    model.asset.version = "2.0";
    model.asset.generator = "EDEN Model Editor";

    bool success = gltf.WriteGltfSceneToFile(&model, filepath, true, true, true, true);
    if (!success) {
        std::cerr << "GLBLoader::saveWithHalfEdgeData - Failed to write GLB" << std::endl;
        return false;
    }

    std::cout << "Saved GLB with half-edge data: " << filepath << " ("
              << heData.faces.size() << " HE faces preserved)" << std::endl;
    return true;
}

std::unique_ptr<SceneObject> GLBLoader::createSceneObject(
    const LoadedMesh& mesh,
    ModelRenderer& modelRenderer
) {
    auto obj = std::make_unique<SceneObject>(mesh.name);

    // Upload mesh to GPU with optional texture
    uint32_t handle;
    if (mesh.hasTexture) {
        handle = modelRenderer.createModel(
            mesh.vertices,
            mesh.indices,
            mesh.texture.data.data(),
            mesh.texture.width,
            mesh.texture.height
        );
    } else {
        handle = modelRenderer.createModel(
            mesh.vertices,
            mesh.indices,
            nullptr, 0, 0
        );
    }

    obj->setBufferHandle(handle);
    obj->setIndexCount(static_cast<uint32_t>(mesh.indices.size()));
    obj->setVertexCount(static_cast<uint32_t>(mesh.vertices.size()));
    obj->setLocalBounds(mesh.bounds);

    // Store mesh data for raycasting
    obj->setMeshData(mesh.vertices, mesh.indices);

    // Store texture data for painting (or create a default white texture)
    if (mesh.hasTexture) {
        obj->setTextureData(mesh.texture.data, mesh.texture.width, mesh.texture.height);
    } else {
        // Create a default white texture for painting (256x256)
        std::vector<unsigned char> defaultTex(256 * 256 * 4, 255);  // White RGBA
        obj->setTextureData(defaultTex, 256, 256);
    }

    return obj;
}

glm::vec3 GLBLoader::calculateNormal(
    const glm::vec3& v0,
    const glm::vec3& v1,
    const glm::vec3& v2
) {
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 normal = glm::cross(edge1, edge2);

    float len = glm::length(normal);
    if (len > 1e-6f) {
        return normal / len;
    }
    return glm::vec3(0, 1, 0);
}

} // namespace eden
