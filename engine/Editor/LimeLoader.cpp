#include "LimeLoader.hpp"
#include "SceneObject.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <glm/gtc/quaternion.hpp>

namespace eden {

// Base64 decoding
static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::vector<unsigned char> base64_decode(const std::string& encoded) {
    std::vector<unsigned char> ret;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)base64_chars[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            ret.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return ret;
}

// Internal structures for parsing
struct LimeVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 color = glm::vec4(1.0f);
    uint32_t halfEdgeIndex;
    bool selected;
};

struct LimeFace {
    uint32_t halfEdgeIndex;
    uint32_t vertexCount;
    bool selected;
    std::vector<uint32_t> vertexIndices;
};

LimeLoader::LoadResult LimeLoader::load(const std::string& filepath) {
    LoadResult result;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        result.error = "Failed to open file: " + filepath;
        return result;
    }

    result.mesh.name = std::filesystem::path(filepath).stem().string();

    std::vector<LimeVertex> limeVertices;
    std::vector<LimeFace> limeFaces;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "transform_pos:") {
            iss >> result.mesh.position.x >> result.mesh.position.y >> result.mesh.position.z;
        }
        else if (type == "transform_rot:") {
            // Stored as quaternion (w x y z), convert to euler degrees
            float w, x, y, z;
            iss >> w >> x >> y >> z;
            glm::quat q(w, x, y, z);
            result.mesh.rotation = glm::degrees(glm::eulerAngles(q));
        }
        else if (type == "transform_scale:") {
            iss >> result.mesh.scale.x >> result.mesh.scale.y >> result.mesh.scale.z;
        }
        else if (type == "tex_size:") {
            iss >> result.mesh.textureWidth >> result.mesh.textureHeight;
        }
        else if (type == "tex_data:") {
            std::string encoded;
            iss >> encoded;
            result.mesh.textureData = base64_decode(encoded);
            result.mesh.hasTexture = !result.mesh.textureData.empty() &&
                                      result.mesh.textureWidth > 0 &&
                                      result.mesh.textureHeight > 0;
        }
        else if (type == "v") {
            // Parse vertex: v idx: pos | nrm | uv | [col |] halfEdgeIdx selected
            uint32_t idx;
            char colon, pipe1, pipe2, pipe3, pipe4;
            LimeVertex v;
            int selected;

            iss >> idx >> colon
                >> v.position.x >> v.position.y >> v.position.z >> pipe1
                >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                >> v.uv.x >> v.uv.y >> pipe3;

            // Try to read color (v2.0 format)
            float r, g, b, a;
            if (iss >> r >> g >> b >> a >> pipe4) {
                v.color = glm::vec4(r, g, b, a);
                iss >> v.halfEdgeIndex >> selected;
            } else {
                // Fallback for v1.0 format (no color)
                v.color = glm::vec4(1.0f);
                iss.clear();
                iss.seekg(0);
                std::string dummy;
                iss >> dummy >> idx >> colon
                    >> v.position.x >> v.position.y >> v.position.z >> pipe1
                    >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                    >> v.uv.x >> v.uv.y >> pipe3
                    >> v.halfEdgeIndex >> selected;
            }
            v.selected = (selected != 0);

            if (idx >= limeVertices.size()) {
                limeVertices.resize(idx + 1);
            }
            limeVertices[idx] = v;
        }
        else if (type == "f") {
            // Parse face: f idx: halfEdgeIdx vertexCount selected | vertex_indices...
            uint32_t idx, heIdx, vertCount;
            int selected;
            char colon, pipe;

            iss >> idx >> colon >> heIdx >> vertCount >> selected >> pipe;

            LimeFace f;
            f.halfEdgeIndex = heIdx;
            f.vertexCount = vertCount;
            f.selected = (selected != 0);

            // Read vertex indices
            uint32_t vi;
            while (iss >> vi) {
                f.vertexIndices.push_back(vi);
            }

            if (idx >= limeFaces.size()) {
                limeFaces.resize(idx + 1);
            }
            limeFaces[idx] = f;
        }
        else if (type == "cp") {
            // Parse control point: cp idx: vertexIndex "name"
            uint32_t idx, vertIdx;
            char colon;
            iss >> idx >> colon >> vertIdx;
            // Read quoted name
            std::string cpName;
            std::getline(iss, cpName);
            // Strip leading whitespace and quotes
            size_t start = cpName.find('"');
            size_t end = cpName.rfind('"');
            if (start != std::string::npos && end != std::string::npos && end > start) {
                cpName = cpName.substr(start + 1, end - start - 1);
            } else {
                // No quotes — trim whitespace
                while (!cpName.empty() && cpName[0] == ' ') cpName.erase(0, 1);
            }
            result.mesh.controlPoints.push_back({vertIdx, cpName});
        }
        else if (type == "meta") {
            // Parse metadata: meta key: value
            std::string rest;
            std::getline(iss, rest);
            // Strip leading whitespace
            size_t start = rest.find_first_not_of(' ');
            if (start != std::string::npos) {
                rest = rest.substr(start);
            }
            // Split on first ": "
            size_t colonPos = rest.find(": ");
            if (colonPos != std::string::npos) {
                std::string key = rest.substr(0, colonPos);
                std::string value = rest.substr(colonPos + 2);
                result.mesh.metadata[key] = value;
            }
        }
        // We don't need half-edge data for rendering, skip "he" lines
    }

    file.close();

    // Convert to triangulated mesh for GPU
    // First, create ModelVertex array from lime vertices
    result.mesh.vertices.reserve(limeVertices.size());
    for (const auto& lv : limeVertices) {
        ModelVertex mv;
        mv.position = lv.position;
        mv.normal = lv.normal;
        mv.texCoord = lv.uv;
        mv.color = lv.color;
        result.mesh.vertices.push_back(mv);
    }

    // Triangulate faces (fan triangulation for quads and n-gons)
    for (const auto& face : limeFaces) {
        if (face.vertexIndices.size() < 3) continue;

        // Fan triangulation: first vertex is the hub
        for (size_t i = 1; i + 1 < face.vertexIndices.size(); ++i) {
            result.mesh.indices.push_back(face.vertexIndices[0]);
            result.mesh.indices.push_back(face.vertexIndices[i]);
            result.mesh.indices.push_back(face.vertexIndices[i + 1]);
        }
    }

    result.success = true;
    std::cout << "Loaded LIME: " << filepath << " ("
              << limeVertices.size() << " vertices, "
              << limeFaces.size() << " faces, "
              << result.mesh.indices.size() / 3 << " triangles)";
    if (result.mesh.hasTexture) {
        std::cout << " with " << result.mesh.textureWidth << "x" << result.mesh.textureHeight << " texture";
    }
    if (!result.mesh.controlPoints.empty()) {
        std::cout << ", " << result.mesh.controlPoints.size() << " control points";
    }
    if (!result.mesh.metadata.empty()) {
        std::cout << ", " << result.mesh.metadata.size() << " metadata entries";
    }
    std::cout << std::endl;

    return result;
}

std::unique_ptr<SceneObject> LimeLoader::createSceneObject(
    const LoadedMesh& mesh,
    ModelRenderer& renderer
) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return nullptr;
    }

    auto obj = std::make_unique<SceneObject>(mesh.name);

    // Create GPU resources
    uint32_t handle = renderer.createModel(
        mesh.vertices,
        mesh.indices,
        mesh.hasTexture ? mesh.textureData.data() : nullptr,
        mesh.textureWidth,
        mesh.textureHeight
    );

    obj->setBufferHandle(handle);
    obj->setIndexCount(static_cast<uint32_t>(mesh.indices.size()));
    obj->setVertexCount(static_cast<uint32_t>(mesh.vertices.size()));
    obj->setMeshData(mesh.vertices, mesh.indices);

    // Compute local bounds from vertex positions
    AABB bounds;
    bounds.min = glm::vec3(INFINITY);
    bounds.max = glm::vec3(-INFINITY);
    for (const auto& v : mesh.vertices) {
        bounds.min = glm::min(bounds.min, v.position);
        bounds.max = glm::max(bounds.max, v.position);
    }
    obj->setLocalBounds(bounds);

    if (mesh.hasTexture) {
        obj->setTextureData(mesh.textureData, mesh.textureWidth, mesh.textureHeight);
    }

    // Apply saved transform (scale is critical for preserving model dimensions)
    obj->getTransform().setScale(mesh.scale);
    obj->setEulerRotation(mesh.rotation);

    // Transfer control points
    if (!mesh.controlPoints.empty()) {
        std::vector<SceneObject::StoredControlPoint> storedCPs;
        for (const auto& cp : mesh.controlPoints) {
            storedCPs.push_back({cp.vertexIndex, cp.name});
        }
        obj->setControlPoints(storedCPs);
    }

    return obj;
}

} // namespace eden
