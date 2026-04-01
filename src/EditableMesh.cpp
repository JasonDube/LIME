#include "EditableMesh.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iostream>
#include <fstream>
#include <sstream>
#include <queue>
#include <map>
#include <unordered_map>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace eden {

// Base64 encoding/decoding for texture embedding
static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* data, size_t len) {
    std::string ret;
    ret.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = (unsigned int)data[i] << 16;
        if (i + 1 < len) n |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) n |= (unsigned int)data[i + 2];
        ret.push_back(base64_chars[(n >> 18) & 0x3F]);
        ret.push_back(base64_chars[(n >> 12) & 0x3F]);
        ret.push_back((i + 1 < len) ? base64_chars[(n >> 6) & 0x3F] : '=');
        ret.push_back((i + 2 < len) ? base64_chars[n & 0x3F] : '=');
    }
    return ret;
}

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

void EditableMesh::buildFromTriangles(const std::vector<ModelVertex>& vertices,
                                      const std::vector<uint32_t>& indices) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    // Convert vertices
    m_vertices.reserve(vertices.size());
    for (const auto& v : vertices) {
        HEVertex hv;
        hv.position = v.position;
        hv.normal = v.normal;
        hv.uv = v.texCoord;
        hv.color = v.color;
        hv.halfEdgeIndex = UINT32_MAX;
        hv.selected = false;
        m_vertices.push_back(hv);
    }

    // Build faces from triangles
    size_t triCount = indices.size() / 3;
    m_faces.reserve(triCount);
    m_halfEdges.reserve(triCount * 3);

    for (size_t i = 0; i < triCount; ++i) {
        std::vector<uint32_t> faceVerts = {
            indices[i * 3],
            indices[i * 3 + 1],
            indices[i * 3 + 2]
        };
        addFace(faceVerts);
    }

    // Link twin half-edges by position (O(n) hash map approach)
    linkTwinsByPosition();
    rebuildEdgeMap();
}

void EditableMesh::buildFromQuads(const std::vector<ModelVertex>& vertices,
                                   const std::vector<uint32_t>& indices) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    // Convert vertices
    m_vertices.reserve(vertices.size());
    for (const auto& v : vertices) {
        HEVertex hv;
        hv.position = v.position;
        hv.normal = v.normal;
        hv.uv = v.texCoord;
        hv.color = v.color;
        hv.halfEdgeIndex = UINT32_MAX;
        hv.selected = false;
        m_vertices.push_back(hv);
    }

    // Build faces from quads (4 indices per face)
    size_t quadCount = indices.size() / 4;
    m_faces.reserve(quadCount);
    m_halfEdges.reserve(quadCount * 4);

    for (size_t i = 0; i < quadCount; ++i) {
        std::vector<uint32_t> faceVerts = {
            indices[i * 4],
            indices[i * 4 + 1],
            indices[i * 4 + 2],
            indices[i * 4 + 3]
        };
        addFace(faceVerts);
    }

    // Link twin half-edges by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
}

void EditableMesh::setFromData(const std::vector<HEVertex>& vertices,
                               const std::vector<HalfEdge>& halfEdges,
                               const std::vector<HEFace>& faces) {
    m_vertices = vertices;
    m_halfEdges = halfEdges;
    m_faces = faces;
    m_edgeMap.clear();
    m_selectedEdges.clear();

    // Rebuild edge map from half-edges
    rebuildEdgeMap();
}

void EditableMesh::buildCube(float size) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    float h = size * 0.5f;
    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

    // 8 unique corner vertices (shared across faces for proper topology)
    // But we need 24 vertices for unique normals per face
    m_vertices = {
        // Front face (z = +h)
        {{-h, -h,  h}, { 0,  0,  1}, {0, 0}, white, UINT32_MAX, false},  // 0
        {{ h, -h,  h}, { 0,  0,  1}, {1, 0}, white, UINT32_MAX, false},  // 1
        {{ h,  h,  h}, { 0,  0,  1}, {1, 1}, white, UINT32_MAX, false},  // 2
        {{-h,  h,  h}, { 0,  0,  1}, {0, 1}, white, UINT32_MAX, false},  // 3
        // Back face (z = -h)
        {{ h, -h, -h}, { 0,  0, -1}, {0, 0}, white, UINT32_MAX, false},  // 4
        {{-h, -h, -h}, { 0,  0, -1}, {1, 0}, white, UINT32_MAX, false},  // 5
        {{-h,  h, -h}, { 0,  0, -1}, {1, 1}, white, UINT32_MAX, false},  // 6
        {{ h,  h, -h}, { 0,  0, -1}, {0, 1}, white, UINT32_MAX, false},  // 7
        // Top face (y = +h)
        {{-h,  h,  h}, { 0,  1,  0}, {0, 0}, white, UINT32_MAX, false},  // 8
        {{ h,  h,  h}, { 0,  1,  0}, {1, 0}, white, UINT32_MAX, false},  // 9
        {{ h,  h, -h}, { 0,  1,  0}, {1, 1}, white, UINT32_MAX, false},  // 10
        {{-h,  h, -h}, { 0,  1,  0}, {0, 1}, white, UINT32_MAX, false},  // 11
        // Bottom face (y = -h)
        {{-h, -h, -h}, { 0, -1,  0}, {0, 0}, white, UINT32_MAX, false},  // 12
        {{ h, -h, -h}, { 0, -1,  0}, {1, 0}, white, UINT32_MAX, false},  // 13
        {{ h, -h,  h}, { 0, -1,  0}, {1, 1}, white, UINT32_MAX, false},  // 14
        {{-h, -h,  h}, { 0, -1,  0}, {0, 1}, white, UINT32_MAX, false},  // 15
        // Right face (x = +h)
        {{ h, -h,  h}, { 1,  0,  0}, {0, 0}, white, UINT32_MAX, false},  // 16
        {{ h, -h, -h}, { 1,  0,  0}, {1, 0}, white, UINT32_MAX, false},  // 17
        {{ h,  h, -h}, { 1,  0,  0}, {1, 1}, white, UINT32_MAX, false},  // 18
        {{ h,  h,  h}, { 1,  0,  0}, {0, 1}, white, UINT32_MAX, false},  // 19
        // Left face (x = -h)
        {{-h, -h, -h}, {-1,  0,  0}, {0, 0}, white, UINT32_MAX, false},  // 20
        {{-h, -h,  h}, {-1,  0,  0}, {1, 0}, white, UINT32_MAX, false},  // 21
        {{-h,  h,  h}, {-1,  0,  0}, {1, 1}, white, UINT32_MAX, false},  // 22
        {{-h,  h, -h}, {-1,  0,  0}, {0, 1}, white, UINT32_MAX, false},  // 23
    };

    // Create 6 quad faces
    m_faces.reserve(6);
    m_halfEdges.reserve(24);

    // Quad indices for each face (CCW winding)
    std::vector<std::vector<uint32_t>> quadFaces = {
        {0, 1, 2, 3},     // Front
        {4, 5, 6, 7},     // Back
        {8, 9, 10, 11},   // Top
        {12, 13, 14, 15}, // Bottom
        {16, 17, 18, 19}, // Right
        {20, 21, 22, 23}  // Left
    };

    for (const auto& faceVerts : quadFaces) {
        addFace(faceVerts);
    }

    // Link twin half-edges by position (faces have separate vertices with same positions)
    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube with " << m_faces.size() << " quad faces, "
              << m_vertices.size() << " vertices" << std::endl;
}

void EditableMesh::buildBox(float width, float height, float depth) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    float hx = width * 0.5f;
    float hy = height * 0.5f;
    float hz = depth * 0.5f;
    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

    // 24 vertices (4 per face × 6 faces) for unique normals
    m_vertices = {
        // Front face (z = +hz)
        {{-hx, -hy,  hz}, { 0,  0,  1}, {0, 0}, white, UINT32_MAX, false},  // 0
        {{ hx, -hy,  hz}, { 0,  0,  1}, {1, 0}, white, UINT32_MAX, false},  // 1
        {{ hx,  hy,  hz}, { 0,  0,  1}, {1, 1}, white, UINT32_MAX, false},  // 2
        {{-hx,  hy,  hz}, { 0,  0,  1}, {0, 1}, white, UINT32_MAX, false},  // 3
        // Back face (z = -hz)
        {{ hx, -hy, -hz}, { 0,  0, -1}, {0, 0}, white, UINT32_MAX, false},  // 4
        {{-hx, -hy, -hz}, { 0,  0, -1}, {1, 0}, white, UINT32_MAX, false},  // 5
        {{-hx,  hy, -hz}, { 0,  0, -1}, {1, 1}, white, UINT32_MAX, false},  // 6
        {{ hx,  hy, -hz}, { 0,  0, -1}, {0, 1}, white, UINT32_MAX, false},  // 7
        // Top face (y = +hy)
        {{-hx,  hy,  hz}, { 0,  1,  0}, {0, 0}, white, UINT32_MAX, false},  // 8
        {{ hx,  hy,  hz}, { 0,  1,  0}, {1, 0}, white, UINT32_MAX, false},  // 9
        {{ hx,  hy, -hz}, { 0,  1,  0}, {1, 1}, white, UINT32_MAX, false},  // 10
        {{-hx,  hy, -hz}, { 0,  1,  0}, {0, 1}, white, UINT32_MAX, false},  // 11
        // Bottom face (y = -hy)
        {{-hx, -hy, -hz}, { 0, -1,  0}, {0, 0}, white, UINT32_MAX, false},  // 12
        {{ hx, -hy, -hz}, { 0, -1,  0}, {1, 0}, white, UINT32_MAX, false},  // 13
        {{ hx, -hy,  hz}, { 0, -1,  0}, {1, 1}, white, UINT32_MAX, false},  // 14
        {{-hx, -hy,  hz}, { 0, -1,  0}, {0, 1}, white, UINT32_MAX, false},  // 15
        // Right face (x = +hx)
        {{ hx, -hy,  hz}, { 1,  0,  0}, {0, 0}, white, UINT32_MAX, false},  // 16
        {{ hx, -hy, -hz}, { 1,  0,  0}, {1, 0}, white, UINT32_MAX, false},  // 17
        {{ hx,  hy, -hz}, { 1,  0,  0}, {1, 1}, white, UINT32_MAX, false},  // 18
        {{ hx,  hy,  hz}, { 1,  0,  0}, {0, 1}, white, UINT32_MAX, false},  // 19
        // Left face (x = -hx)
        {{-hx, -hy, -hz}, {-1,  0,  0}, {0, 0}, white, UINT32_MAX, false},  // 20
        {{-hx, -hy,  hz}, {-1,  0,  0}, {1, 0}, white, UINT32_MAX, false},  // 21
        {{-hx,  hy,  hz}, {-1,  0,  0}, {1, 1}, white, UINT32_MAX, false},  // 22
        {{-hx,  hy, -hz}, {-1,  0,  0}, {0, 1}, white, UINT32_MAX, false},  // 23
    };

    m_faces.reserve(6);
    m_halfEdges.reserve(24);

    std::vector<std::vector<uint32_t>> quadFaces = {
        {0, 1, 2, 3},     // Front
        {4, 5, 6, 7},     // Back
        {8, 9, 10, 11},   // Top
        {12, 13, 14, 15}, // Bottom
        {16, 17, 18, 19}, // Right
        {20, 21, 22, 23}  // Left
    };

    for (const auto& faceVerts : quadFaces) {
        addFace(faceVerts);
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built box (" << width << "x" << height << "x" << depth << ") with "
              << m_faces.size() << " quad faces, " << m_vertices.size() << " vertices" << std::endl;
}

void EditableMesh::buildCylinder(float radius, float height, int segments, int divisions, bool caps, int capRings) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float halfHeight = height * 0.5f;

    // Create vertices for (divisions + 1) rings (no duplicate vertices at seam)
    // Side vertices - only 'segments' columns per ring, faces will wrap using modulo
    int sideVertStart = 0;
    int vertsPerRing = segments;

    for (int ring = 0; ring <= divisions; ++ring) {
        float v = static_cast<float>(ring) / divisions;
        float y = -halfHeight + v * height;

        for (int seg = 0; seg < segments; ++seg) {
            float angle = (static_cast<float>(seg) / segments) * 2.0f * glm::pi<float>();
            float x = std::cos(angle) * radius;
            float z = std::sin(angle) * radius;
            float u = static_cast<float>(seg) / segments;

            glm::vec3 normal = glm::normalize(glm::vec3(x, 0, z));

            m_vertices.push_back({
                {x, y, z},
                normal,
                {u, v},
                white,
                UINT32_MAX,
                false
            });
        }
    }

    // Create side quad faces - wrap indices using modulo for seamless topology
    for (int ring = 0; ring < divisions; ++ring) {
        for (int seg = 0; seg < segments; ++seg) {
            int nextSeg = (seg + 1) % segments;  // Wrap around

            uint32_t bl = sideVertStart + ring * vertsPerRing + seg;
            uint32_t tl = sideVertStart + (ring + 1) * vertsPerRing + seg;
            uint32_t br = sideVertStart + ring * vertsPerRing + nextSeg;
            uint32_t tr = sideVertStart + (ring + 1) * vertsPerRing + nextSeg;

            // Quad winding: bl, tl, tr, br (CCW when viewed from outside)
            addFace({bl, tl, tr, br});
        }
    }

    // Create cap vertices and faces with quad grid topology
    if (caps) {
        // Ensure at least 1 ring for the cap
        capRings = std::max(1, capRings);

        // ========== TOP CAP ==========
        // Create center vertex first
        uint32_t topCenterIdx = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({
            {0, halfHeight, 0},
            {0, 1, 0},
            {0.5f, 0.5f},
            white,
            UINT32_MAX,
            false
        });

        // Create concentric rings from center outward
        // Ring 0 is the innermost, ring capRings is the outer rim
        std::vector<uint32_t> topCapRingStarts(capRings + 1);

        for (int ring = 0; ring <= capRings; ++ring) {
            topCapRingStarts[ring] = static_cast<uint32_t>(m_vertices.size());

            // Radius for this ring
            float ringRadius = (static_cast<float>(ring + 1) / (capRings + 1)) * radius;

            for (int seg = 0; seg < segments; ++seg) {
                float angle = (static_cast<float>(seg) / segments) * 2.0f * glm::pi<float>();
                float x = std::cos(angle) * ringRadius;
                float z = std::sin(angle) * ringRadius;

                // UV: polar coordinates mapped to 0-1 range
                float uvRadius = (static_cast<float>(ring + 1) / (capRings + 1)) * 0.5f;
                float uvX = 0.5f + std::cos(angle) * uvRadius;
                float uvY = 0.5f + std::sin(angle) * uvRadius;

                m_vertices.push_back({
                    {x, halfHeight, z},
                    {0, 1, 0},
                    {uvX, uvY},
                    white,
                    UINT32_MAX,
                    false
                });
            }
        }

        // Create triangles from center to innermost ring (top cap)
        for (int seg = 0; seg < segments; ++seg) {
            int nextSeg = (seg + 1) % segments;
            uint32_t curr = topCapRingStarts[0] + seg;
            uint32_t next = topCapRingStarts[0] + nextSeg;
            // CCW winding when viewed from above (normal pointing up)
            addFace({topCenterIdx, next, curr});
        }

        // Create quad faces between adjacent rings (top cap)
        for (int ring = 0; ring < capRings; ++ring) {
            for (int seg = 0; seg < segments; ++seg) {
                int nextSeg = (seg + 1) % segments;

                uint32_t innerCurr = topCapRingStarts[ring] + seg;
                uint32_t innerNext = topCapRingStarts[ring] + nextSeg;
                uint32_t outerCurr = topCapRingStarts[ring + 1] + seg;
                uint32_t outerNext = topCapRingStarts[ring + 1] + nextSeg;

                // Quad: CCW winding when viewed from above (normal pointing up)
                addFace({innerCurr, innerNext, outerNext, outerCurr});
            }
        }

        // ========== BOTTOM CAP ==========
        // Create center vertex first
        uint32_t bottomCenterIdx = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({
            {0, -halfHeight, 0},
            {0, -1, 0},
            {0.5f, 0.5f},
            white,
            UINT32_MAX,
            false
        });

        std::vector<uint32_t> bottomCapRingStarts(capRings + 1);

        for (int ring = 0; ring <= capRings; ++ring) {
            bottomCapRingStarts[ring] = static_cast<uint32_t>(m_vertices.size());

            float ringRadius = (static_cast<float>(ring + 1) / (capRings + 1)) * radius;

            for (int seg = 0; seg < segments; ++seg) {
                float angle = (static_cast<float>(seg) / segments) * 2.0f * glm::pi<float>();
                float x = std::cos(angle) * ringRadius;
                float z = std::sin(angle) * ringRadius;

                float uvRadius = (static_cast<float>(ring + 1) / (capRings + 1)) * 0.5f;
                float uvX = 0.5f + std::cos(angle) * uvRadius;
                float uvY = 0.5f - std::sin(angle) * uvRadius;  // Flipped for bottom

                m_vertices.push_back({
                    {x, -halfHeight, z},
                    {0, -1, 0},
                    {uvX, uvY},
                    white,
                    UINT32_MAX,
                    false
                });
            }
        }

        // Create triangles from center to innermost ring (bottom cap)
        for (int seg = 0; seg < segments; ++seg) {
            int nextSeg = (seg + 1) % segments;
            uint32_t curr = bottomCapRingStarts[0] + seg;
            uint32_t next = bottomCapRingStarts[0] + nextSeg;
            // CCW winding when viewed from below (normal pointing down)
            addFace({bottomCenterIdx, curr, next});
        }

        // Create quad faces between adjacent rings (bottom cap)
        for (int ring = 0; ring < capRings; ++ring) {
            for (int seg = 0; seg < segments; ++seg) {
                int nextSeg = (seg + 1) % segments;

                uint32_t innerCurr = bottomCapRingStarts[ring] + seg;
                uint32_t innerNext = bottomCapRingStarts[ring] + nextSeg;
                uint32_t outerCurr = bottomCapRingStarts[ring + 1] + seg;
                uint32_t outerNext = bottomCapRingStarts[ring + 1] + nextSeg;

                // Quad: CCW winding when viewed from below (normal pointing down)
                addFace({innerCurr, outerCurr, outerNext, innerNext});
            }
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cylinder with " << m_faces.size() << " faces, "
              << m_vertices.size() << " vertices" << std::endl;
}

void EditableMesh::buildSphere(float radius, int rings, int segments) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

    // Create vertices with single pole vertices (not duplicated per segment)
    // rings = number of horizontal divisions (latitude)
    // segments = number of vertical divisions (longitude)

    // Top pole - single vertex (index 0)
    uint32_t topPoleIdx = 0;
    m_vertices.push_back({
        {0, radius, 0},
        {0, 1, 0},
        {0.5f, 0.0f},
        white,
        UINT32_MAX,
        false
    });

    // Middle rings (ring 1 to rings-1) - segments vertices each
    for (int ring = 1; ring < rings; ++ring) {
        float phi = glm::pi<float>() * static_cast<float>(ring) / rings;
        float y = std::cos(phi) * radius;
        float ringRadius = std::sin(phi) * radius;
        float v = static_cast<float>(ring) / rings;

        for (int seg = 0; seg < segments; ++seg) {
            float theta = 2.0f * glm::pi<float>() * static_cast<float>(seg) / segments;
            float x = std::cos(theta) * ringRadius;
            float z = std::sin(theta) * ringRadius;
            float u = static_cast<float>(seg) / segments;

            glm::vec3 pos(x, y, z);
            glm::vec3 normal = glm::normalize(pos);

            m_vertices.push_back({
                pos,
                normal,
                {u, v},
                white,
                UINT32_MAX,
                false
            });
        }
    }

    // Bottom pole - single vertex
    uint32_t bottomPoleIdx = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back({
        {0, -radius, 0},
        {0, -1, 0},
        {0.5f, 1.0f},
        white,
        UINT32_MAX,
        false
    });

    // Helper to get vertex index for a ring/segment
    // Ring 0 = top pole, Ring rings = bottom pole, otherwise middle rings
    auto getVertexIdx = [&](int ring, int seg) -> uint32_t {
        if (ring == 0) return topPoleIdx;
        if (ring == rings) return bottomPoleIdx;
        // Middle rings start at index 1, each has 'segments' vertices
        return 1 + (ring - 1) * segments + (seg % segments);
    };

    // Create faces
    for (int ring = 0; ring < rings; ++ring) {
        for (int seg = 0; seg < segments; ++seg) {
            int nextSeg = (seg + 1) % segments;

            // Top cap triangles (ring 0 to ring 1)
            if (ring == 0) {
                uint32_t pole = getVertexIdx(0, 0);
                uint32_t bl = getVertexIdx(1, seg);
                uint32_t br = getVertexIdx(1, nextSeg);
                // Triangle: pole, br, bl (CCW from outside)
                addFace({pole, br, bl});
            }
            // Bottom cap triangles (ring rings-1 to ring rings)
            else if (ring == rings - 1) {
                uint32_t tl = getVertexIdx(ring, seg);
                uint32_t tr = getVertexIdx(ring, nextSeg);
                uint32_t pole = getVertexIdx(rings, 0);
                // Triangle: tl, tr, pole (CCW from outside)
                addFace({tl, tr, pole});
            }
            // Middle quads
            else {
                uint32_t tl = getVertexIdx(ring, seg);
                uint32_t tr = getVertexIdx(ring, nextSeg);
                uint32_t bl = getVertexIdx(ring + 1, seg);
                uint32_t br = getVertexIdx(ring + 1, nextSeg);
                // Quad: tl, tr, br, bl (CCW from outside)
                addFace({tl, tr, br, bl});
            }
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built sphere with " << m_faces.size() << " faces, "
              << m_vertices.size() << " vertices" << std::endl;
}

void EditableMesh::buildCubeRing(int segments, float innerRadius, float outerRadius, float height) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float halfHeight = height * 0.5f;
    float angleStep = 2.0f * glm::pi<float>() / segments;

    // Each segment is a deformed cube (6 faces, 24 vertices)
    for (int seg = 0; seg < segments; ++seg) {
        float angle1 = seg * angleStep;
        float angle2 = (seg + 1) * angleStep;

        // Calculate the 8 corner positions of this wedge segment
        // Inner corners
        glm::vec3 innerBot1(std::cos(angle1) * innerRadius, -halfHeight, std::sin(angle1) * innerRadius);
        glm::vec3 innerBot2(std::cos(angle2) * innerRadius, -halfHeight, std::sin(angle2) * innerRadius);
        glm::vec3 innerTop1(std::cos(angle1) * innerRadius,  halfHeight, std::sin(angle1) * innerRadius);
        glm::vec3 innerTop2(std::cos(angle2) * innerRadius,  halfHeight, std::sin(angle2) * innerRadius);
        // Outer corners
        glm::vec3 outerBot1(std::cos(angle1) * outerRadius, -halfHeight, std::sin(angle1) * outerRadius);
        glm::vec3 outerBot2(std::cos(angle2) * outerRadius, -halfHeight, std::sin(angle2) * outerRadius);
        glm::vec3 outerTop1(std::cos(angle1) * outerRadius,  halfHeight, std::sin(angle1) * outerRadius);
        glm::vec3 outerTop2(std::cos(angle2) * outerRadius,  halfHeight, std::sin(angle2) * outerRadius);

        // Calculate normals for each face
        glm::vec3 outerNormal = glm::normalize(glm::vec3(std::cos((angle1 + angle2) * 0.5f), 0, std::sin((angle1 + angle2) * 0.5f)));
        glm::vec3 innerNormal = -outerNormal;
        glm::vec3 topNormal(0, 1, 0);
        glm::vec3 bottomNormal(0, -1, 0);
        glm::vec3 side1Normal = glm::normalize(glm::vec3(-std::sin(angle1), 0, std::cos(angle1)));
        glm::vec3 side2Normal = glm::normalize(glm::vec3(std::sin(angle2), 0, -std::cos(angle2)));

        // UV coordinates - each face gets 0-1 mapping
        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Outer face (facing outward) - CCW from outside: outerBot1, outerBot2, outerTop2, outerTop1
        m_vertices.push_back({outerBot1, outerNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBot2, outerNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop2, outerNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop1, outerNormal, {0, 1}, white, UINT32_MAX, false});

        // Inner face (facing inward) - CCW from inside: innerBot2, innerBot1, innerTop1, innerTop2
        m_vertices.push_back({innerBot2, innerNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBot1, innerNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerTop1, innerNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerTop2, innerNormal, {0, 1}, white, UINT32_MAX, false});

        // Top face (facing up) - CCW from above: innerTop1, outerTop1, outerTop2, innerTop2
        m_vertices.push_back({innerTop1, topNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop1, topNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop2, topNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerTop2, topNormal, {0, 1}, white, UINT32_MAX, false});

        // Bottom face (facing down) - CCW from below: innerBot2, outerBot2, outerBot1, innerBot1
        m_vertices.push_back({innerBot2, bottomNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBot2, bottomNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBot1, bottomNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBot1, bottomNormal, {0, 1}, white, UINT32_MAX, false});

        // Side 1 (at angle1) - CCW from outside: outerBot1, innerBot1, innerTop1, outerTop1
        m_vertices.push_back({outerBot1, side1Normal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBot1, side1Normal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerTop1, side1Normal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop1, side1Normal, {0, 1}, white, UINT32_MAX, false});

        // Side 2 (at angle2) - CCW from outside: innerBot2, outerBot2, outerTop2, innerTop2
        m_vertices.push_back({innerBot2, side2Normal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBot2, side2Normal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerTop2, side2Normal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerTop2, side2Normal, {0, 1}, white, UINT32_MAX, false});

        // Create the 6 quad faces for this segment (reversed winding for correct normals)
        addFace({baseIdx + 3,  baseIdx + 2,  baseIdx + 1,  baseIdx + 0});   // Outer
        addFace({baseIdx + 7,  baseIdx + 6,  baseIdx + 5,  baseIdx + 4});   // Inner
        addFace({baseIdx + 11, baseIdx + 10, baseIdx + 9,  baseIdx + 8});   // Top
        addFace({baseIdx + 15, baseIdx + 14, baseIdx + 13, baseIdx + 12});  // Bottom
        addFace({baseIdx + 19, baseIdx + 18, baseIdx + 17, baseIdx + 16});  // Side 1
        addFace({baseIdx + 23, baseIdx + 22, baseIdx + 21, baseIdx + 20});  // Side 2
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube ring with " << segments << " segments, "
              << m_faces.size() << " faces, " << m_vertices.size() << " vertices" << std::endl;
}

void EditableMesh::buildCubeArch(int segments, float innerRadius, float outerRadius, float depth, float arcDegrees) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float halfDepth = depth * 0.5f;
    float arcRadians = glm::radians(arcDegrees);
    float startAngle = glm::pi<float>() * 0.5f - arcRadians * 0.5f;  // Center the arch
    float angleStep = arcRadians / segments;

    // Each segment is a wedge piece of the arch
    for (int seg = 0; seg < segments; ++seg) {
        float angle1 = startAngle + seg * angleStep;
        float angle2 = startAngle + (seg + 1) * angleStep;

        // Arch curves in XY plane, depth is in Z
        // Inner corners (closer to center of arch)
        glm::vec3 innerFront1(std::cos(angle1) * innerRadius, std::sin(angle1) * innerRadius, -halfDepth);
        glm::vec3 innerFront2(std::cos(angle2) * innerRadius, std::sin(angle2) * innerRadius, -halfDepth);
        glm::vec3 innerBack1(std::cos(angle1) * innerRadius, std::sin(angle1) * innerRadius, halfDepth);
        glm::vec3 innerBack2(std::cos(angle2) * innerRadius, std::sin(angle2) * innerRadius, halfDepth);
        // Outer corners
        glm::vec3 outerFront1(std::cos(angle1) * outerRadius, std::sin(angle1) * outerRadius, -halfDepth);
        glm::vec3 outerFront2(std::cos(angle2) * outerRadius, std::sin(angle2) * outerRadius, -halfDepth);
        glm::vec3 outerBack1(std::cos(angle1) * outerRadius, std::sin(angle1) * outerRadius, halfDepth);
        glm::vec3 outerBack2(std::cos(angle2) * outerRadius, std::sin(angle2) * outerRadius, halfDepth);

        // Calculate normals
        float midAngle = (angle1 + angle2) * 0.5f;
        glm::vec3 outerNormal(std::cos(midAngle), std::sin(midAngle), 0);
        glm::vec3 innerNormal = -outerNormal;
        glm::vec3 frontNormal(0, 0, -1);
        glm::vec3 backNormal(0, 0, 1);
        glm::vec3 side1Normal(-std::sin(angle1), std::cos(angle1), 0);
        glm::vec3 side2Normal(std::sin(angle2), -std::cos(angle2), 0);

        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Outer face (curved outside)
        m_vertices.push_back({outerFront1, outerNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerFront2, outerNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBack2, outerNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBack1, outerNormal, {0, 1}, white, UINT32_MAX, false});

        // Inner face (curved inside)
        m_vertices.push_back({innerFront2, innerNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerFront1, innerNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack1, innerNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack2, innerNormal, {0, 1}, white, UINT32_MAX, false});

        // Front face (Z-)
        m_vertices.push_back({outerFront1, frontNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerFront1, frontNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerFront2, frontNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({outerFront2, frontNormal, {0, 1}, white, UINT32_MAX, false});

        // Back face (Z+)
        m_vertices.push_back({outerBack2, backNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack2, backNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack1, backNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBack1, backNormal, {0, 1}, white, UINT32_MAX, false});

        // Side 1 (at angle1) - only for first segment
        m_vertices.push_back({outerFront1, side1Normal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerBack1, side1Normal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack1, side1Normal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerFront1, side1Normal, {0, 1}, white, UINT32_MAX, false});

        // Side 2 (at angle2) - only for last segment
        m_vertices.push_back({outerBack2, side2Normal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({outerFront2, side2Normal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({innerFront2, side2Normal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({innerBack2, side2Normal, {0, 1}, white, UINT32_MAX, false});

        // Add faces (original winding order)
        addFace({baseIdx + 0,  baseIdx + 1,  baseIdx + 2,  baseIdx + 3});   // Outer
        addFace({baseIdx + 4,  baseIdx + 5,  baseIdx + 6,  baseIdx + 7});   // Inner
        addFace({baseIdx + 8,  baseIdx + 9,  baseIdx + 10, baseIdx + 11});  // Front
        addFace({baseIdx + 12, baseIdx + 13, baseIdx + 14, baseIdx + 15});  // Back

        // Only add end caps for first and last segments
        if (seg == 0) {
            addFace({baseIdx + 16, baseIdx + 17, baseIdx + 18, baseIdx + 19});  // Side 1
        }
        if (seg == segments - 1) {
            addFace({baseIdx + 20, baseIdx + 21, baseIdx + 22, baseIdx + 23});  // Side 2
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube arch with " << segments << " segments, "
              << arcDegrees << " degrees, " << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::buildCubeColumn(int segments, float radius, float height) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float halfHeight = height * 0.5f;
    float angleStep = 2.0f * glm::pi<float>() / segments;

    // Center vertex indices for top and bottom caps
    uint32_t bottomCenterIdx = 0;
    uint32_t topCenterIdx = 1;

    // Add center vertices for caps
    m_vertices.push_back({{0, -halfHeight, 0}, {0, -1, 0}, {0.5f, 0.5f}, white, UINT32_MAX, false});
    m_vertices.push_back({{0, halfHeight, 0}, {0, 1, 0}, {0.5f, 0.5f}, white, UINT32_MAX, false});

    // Each segment is a wedge (like a pie slice)
    for (int seg = 0; seg < segments; ++seg) {
        float angle1 = seg * angleStep;
        float angle2 = (seg + 1) * angleStep;

        glm::vec3 bot1(std::cos(angle1) * radius, -halfHeight, std::sin(angle1) * radius);
        glm::vec3 bot2(std::cos(angle2) * radius, -halfHeight, std::sin(angle2) * radius);
        glm::vec3 top1(std::cos(angle1) * radius, halfHeight, std::sin(angle1) * radius);
        glm::vec3 top2(std::cos(angle2) * radius, halfHeight, std::sin(angle2) * radius);

        float midAngle = (angle1 + angle2) * 0.5f;
        glm::vec3 outerNormal(std::cos(midAngle), 0, std::sin(midAngle));
        glm::vec3 topNormal(0, 1, 0);
        glm::vec3 bottomNormal(0, -1, 0);

        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Outer face
        m_vertices.push_back({bot1, outerNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({bot2, outerNormal, {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({top2, outerNormal, {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({top1, outerNormal, {0, 1}, white, UINT32_MAX, false});

        // Top triangle (as quad with center)
        m_vertices.push_back({top1, topNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({top2, topNormal, {1, 0}, white, UINT32_MAX, false});
        glm::vec3 topCenter(0, halfHeight, 0);
        m_vertices.push_back({topCenter, topNormal, {0.5f, 1}, white, UINT32_MAX, false});

        // Bottom triangle (as quad with center)
        m_vertices.push_back({bot2, bottomNormal, {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({bot1, bottomNormal, {1, 0}, white, UINT32_MAX, false});
        glm::vec3 botCenter(0, -halfHeight, 0);
        m_vertices.push_back({botCenter, bottomNormal, {0.5f, 1}, white, UINT32_MAX, false});

        // Add faces
        addFace({baseIdx + 3, baseIdx + 2, baseIdx + 1, baseIdx + 0});  // Outer
        addFace({baseIdx + 6, baseIdx + 5, baseIdx + 4});  // Top triangle (topCenter, top2, top1 - normal up)
        addFace({baseIdx + 8, baseIdx + 7, baseIdx + 9});  // Bottom triangle (CCW from below, normal down)
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube column with " << segments << " segments, "
              << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::buildCubeStairs(int steps, float width, float stepHeight, float stepDepth) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float halfWidth = width * 0.5f;

    // Each step is a cube
    for (int step = 0; step < steps; ++step) {
        float y0 = step * stepHeight;
        float y1 = (step + 1) * stepHeight;
        float z0 = step * stepDepth;
        float z1 = (step + 1) * stepDepth;

        // 8 corners of this step cube
        glm::vec3 v[8] = {
            {-halfWidth, y0, z0}, {halfWidth, y0, z0},  // bottom front
            {halfWidth, y0, z1},  {-halfWidth, y0, z1}, // bottom back
            {-halfWidth, y1, z0}, {halfWidth, y1, z0},  // top front
            {halfWidth, y1, z1},  {-halfWidth, y1, z1}  // top back
        };

        glm::vec3 normals[6] = {
            {0, 0, -1},  // front
            {0, 0, 1},   // back
            {0, 1, 0},   // top
            {0, -1, 0},  // bottom
            {1, 0, 0},   // right
            {-1, 0, 0}   // left
        };

        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Front face
        m_vertices.push_back({v[0], normals[0], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[0], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[0], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[0], {0, 1}, white, UINT32_MAX, false});

        // Back face (only for last step)
        m_vertices.push_back({v[2], normals[1], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[3], normals[1], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[1], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[1], {0, 1}, white, UINT32_MAX, false});

        // Top face
        m_vertices.push_back({v[4], normals[2], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[2], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[2], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[2], {0, 1}, white, UINT32_MAX, false});

        // Bottom face (only for first step)
        m_vertices.push_back({v[3], normals[3], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[3], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[3], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[0], normals[3], {0, 1}, white, UINT32_MAX, false});

        // Right face
        m_vertices.push_back({v[1], normals[4], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[4], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[4], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[4], {0, 1}, white, UINT32_MAX, false});

        // Left face
        m_vertices.push_back({v[3], normals[5], {0, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[0], normals[5], {1, 0}, white, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[5], {1, 1}, white, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[5], {0, 1}, white, UINT32_MAX, false});

        // Add faces
        addFace({baseIdx + 3,  baseIdx + 2,  baseIdx + 1,  baseIdx + 0});   // Front (reversed)
        addFace({baseIdx + 7,  baseIdx + 6,  baseIdx + 5,  baseIdx + 4});   // Back (reversed)
        addFace({baseIdx + 11, baseIdx + 10, baseIdx + 9,  baseIdx + 8});   // Top (reversed)
        addFace({baseIdx + 15, baseIdx + 14, baseIdx + 13, baseIdx + 12});  // Bottom (reversed)
        addFace({baseIdx + 19, baseIdx + 18, baseIdx + 17, baseIdx + 16});  // Right (reversed)
        addFace({baseIdx + 23, baseIdx + 22, baseIdx + 21, baseIdx + 20});  // Left (reversed)
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube stairs with " << steps << " steps, "
              << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::buildCubeRoom(int width, int height, int depth, float cubeSize, int windowFront) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 floorColor(0.3f, 0.3f, 0.35f, 1.0f);
    glm::vec4 ceilingColor(0.25f, 0.25f, 0.3f, 1.0f);
    glm::vec4 leftColor(0.5f, 0.2f, 0.2f, 1.0f);
    glm::vec4 rightColor(0.2f, 0.5f, 0.2f, 1.0f);
    glm::vec4 backColor(0.35f, 0.35f, 0.4f, 1.0f);
    glm::vec4 frontColor(0.2f, 0.3f, 0.5f, 1.0f);

    float s = cubeSize;
    float halfW = width * s * 0.5f;
    float halfD = depth * s * 0.5f;

    // Helper lambda to add an interior cube (normals point inward)
    auto addInteriorCube = [&](float cx, float cy, float cz, const glm::vec4& color) {
        float x = cx - s * 0.5f;
        float y = cy;
        float z = cz - s * 0.5f;

        glm::vec3 v[8] = {
            {x,     y,     z},     {x + s, y,     z},
            {x + s, y,     z + s}, {x,     y,     z + s},
            {x,     y + s, z},     {x + s, y + s, z},
            {x + s, y + s, z + s}, {x,     y + s, z + s}
        };

        // Normals point INWARD for interior viewing
        glm::vec3 normals[6] = {
            {0, 0, 1},   // front face normal points +Z (inward)
            {0, 0, -1},  // back face normal points -Z (inward)
            {0, -1, 0},  // top face normal points down (inward)
            {0, 1, 0},   // bottom face normal points up (inward)
            {-1, 0, 0},  // right face normal points -X (inward)
            {1, 0, 0}    // left face normal points +X (inward)
        };

        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Front (CCW from inside = reversed from outside)
        m_vertices.push_back({v[1], normals[0], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[0], normals[0], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[0], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[0], {0, 1}, color, UINT32_MAX, false});

        // Back
        m_vertices.push_back({v[3], normals[1], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[1], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[1], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[1], {0, 1}, color, UINT32_MAX, false});

        // Top (ceiling looking down)
        m_vertices.push_back({v[7], normals[2], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[2], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[2], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[2], {0, 1}, color, UINT32_MAX, false});

        // Bottom (floor looking up)
        m_vertices.push_back({v[0], normals[3], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[3], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[3], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[3], normals[3], {0, 1}, color, UINT32_MAX, false});

        // Right
        m_vertices.push_back({v[2], normals[4], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[4], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[4], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[4], {0, 1}, color, UINT32_MAX, false});

        // Left
        m_vertices.push_back({v[0], normals[5], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[3], normals[5], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[5], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[5], {0, 1}, color, UINT32_MAX, false});

        // Add faces (CCW winding for interior)
        addFace({baseIdx + 0,  baseIdx + 1,  baseIdx + 2,  baseIdx + 3});
        addFace({baseIdx + 4,  baseIdx + 5,  baseIdx + 6,  baseIdx + 7});
        addFace({baseIdx + 8,  baseIdx + 9,  baseIdx + 10, baseIdx + 11});
        addFace({baseIdx + 12, baseIdx + 13, baseIdx + 14, baseIdx + 15});
        addFace({baseIdx + 16, baseIdx + 17, baseIdx + 18, baseIdx + 19});
        addFace({baseIdx + 20, baseIdx + 21, baseIdx + 22, baseIdx + 23});
    };

    int windowX = width / 2;
    int windowY = height / 2;

    // Floor (y = 0)
    for (int x = 0; x < width; x++) {
        for (int z = 0; z < depth; z++) {
            float px = (x - width / 2.0f + 0.5f) * s;
            float pz = (z - depth / 2.0f + 0.5f) * s;
            glm::vec4 color = ((x + z) % 2 == 0) ? floorColor : floorColor * 0.8f;
            color.a = 1.0f;
            addInteriorCube(px, 0.0f, pz, color);
        }
    }

    // Ceiling (y = height)
    for (int x = 0; x < width; x++) {
        for (int z = 0; z < depth; z++) {
            float px = (x - width / 2.0f + 0.5f) * s;
            float pz = (z - depth / 2.0f + 0.5f) * s;
            addInteriorCube(px, height * s, pz, ceilingColor);
        }
    }

    // Left wall (x = -width/2)
    for (int y = 1; y < height; y++) {
        for (int z = 0; z < depth; z++) {
            float px = (-width / 2.0f + 0.5f) * s;
            float py = y * s;
            float pz = (z - depth / 2.0f + 0.5f) * s;
            float gradient = 0.7f + 0.3f * (float(y) / height);
            glm::vec4 color = leftColor * gradient;
            color.a = 1.0f;
            addInteriorCube(px, py, pz, color);
        }
    }

    // Right wall (x = width/2)
    for (int y = 1; y < height; y++) {
        for (int z = 0; z < depth; z++) {
            float px = (width / 2.0f - 0.5f) * s;
            float py = y * s;
            float pz = (z - depth / 2.0f + 0.5f) * s;
            float gradient = 0.7f + 0.3f * (float(y) / height);
            glm::vec4 color = rightColor * gradient;
            color.a = 1.0f;
            addInteriorCube(px, py, pz, color);
        }
    }

    // Back wall (z = -depth/2)
    for (int x = 1; x < width - 1; x++) {
        for (int y = 1; y < height; y++) {
            float px = (x - width / 2.0f + 0.5f) * s;
            float py = y * s;
            float pz = (-depth / 2.0f + 0.5f) * s;
            addInteriorCube(px, py, pz, backColor);
        }
    }

    // Front wall with window (z = depth/2)
    for (int x = 1; x < width - 1; x++) {
        for (int y = 1; y < height; y++) {
            // Skip cubes in window area
            int halfWin = windowFront / 2;
            bool isWindow = (x >= windowX - halfWin && x <= windowX + halfWin) &&
                           (y >= windowY - 1 && y <= windowY);
            if (isWindow) continue;

            float px = (x - width / 2.0f + 0.5f) * s;
            float py = y * s;
            float pz = (depth / 2.0f - 0.5f) * s;
            addInteriorCube(px, py, pz, frontColor);
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube room " << width << "x" << height << "x" << depth
              << " with window, " << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::buildCubeHead(float scale) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 skinColor(0.9f, 0.75f, 0.65f, 1.0f);
    glm::vec4 eyeWhite(0.95f, 0.95f, 0.95f, 1.0f);
    glm::vec4 irisColor(0.3f, 0.5f, 0.7f, 1.0f);
    glm::vec4 pupilColor(0.1f, 0.1f, 0.1f, 1.0f);
    glm::vec4 lipColor(0.75f, 0.5f, 0.5f, 1.0f);
    glm::vec4 hairColor(0.25f, 0.15f, 0.1f, 1.0f);
    glm::vec4 browColor(0.35f, 0.25f, 0.15f, 1.0f);
    glm::vec4 noseColor(0.88f, 0.72f, 0.63f, 1.0f);
    glm::vec4 earColor(0.85f, 0.7f, 0.6f, 1.0f);
    glm::vec4 shadowColor(0.8f, 0.65f, 0.55f, 1.0f);

    float s = scale * 0.05f;  // Half the size = double the resolution

    // Helper lambda to add a cube at position (x, y, z) with given color
    auto addCube = [&](int gx, int gy, int gz, const glm::vec4& color) {
        float x = gx * s;
        float y = gy * s;
        float z = gz * s;

        glm::vec3 v[8] = {
            {x,     y,     z},     {x + s, y,     z},      // bottom front
            {x + s, y,     z + s}, {x,     y,     z + s},  // bottom back
            {x,     y + s, z},     {x + s, y + s, z},      // top front
            {x + s, y + s, z + s}, {x,     y + s, z + s}   // top back
        };

        glm::vec3 normals[6] = {
            {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}
        };

        uint32_t baseIdx = static_cast<uint32_t>(m_vertices.size());

        // Front, Back, Top, Bottom, Right, Left vertices
        m_vertices.push_back({v[0], normals[0], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[0], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[0], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[0], {0, 1}, color, UINT32_MAX, false});

        m_vertices.push_back({v[2], normals[1], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[3], normals[1], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[1], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[1], {0, 1}, color, UINT32_MAX, false});

        m_vertices.push_back({v[4], normals[2], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[2], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[2], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[2], {0, 1}, color, UINT32_MAX, false});

        m_vertices.push_back({v[3], normals[3], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[3], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[1], normals[3], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[0], normals[3], {0, 1}, color, UINT32_MAX, false});

        m_vertices.push_back({v[1], normals[4], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[2], normals[4], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[6], normals[4], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[5], normals[4], {0, 1}, color, UINT32_MAX, false});

        m_vertices.push_back({v[3], normals[5], {0, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[0], normals[5], {1, 0}, color, UINT32_MAX, false});
        m_vertices.push_back({v[4], normals[5], {1, 1}, color, UINT32_MAX, false});
        m_vertices.push_back({v[7], normals[5], {0, 1}, color, UINT32_MAX, false});

        addFace({baseIdx + 3,  baseIdx + 2,  baseIdx + 1,  baseIdx + 0});   // Front
        addFace({baseIdx + 7,  baseIdx + 6,  baseIdx + 5,  baseIdx + 4});   // Back
        addFace({baseIdx + 11, baseIdx + 10, baseIdx + 9,  baseIdx + 8});   // Top
        addFace({baseIdx + 15, baseIdx + 14, baseIdx + 13, baseIdx + 12});  // Bottom
        addFace({baseIdx + 19, baseIdx + 18, baseIdx + 17, baseIdx + 16});  // Right
        addFace({baseIdx + 23, baseIdx + 22, baseIdx + 21, baseIdx + 20});  // Left
    };

    // Anatomical proportions (doubled resolution: 16 wide, 20 tall, 16 deep)
    // Eyes at vertical midpoint, head is ~1.2x taller than wide
    // Cranium larger at back, face narrower at jaw

    // Main cranium - egg shaped, wider at top, narrower at jaw
    for (int y = 0; y < 20; y++) {
        for (int x = -8; x < 8; x++) {
            for (int z = -8; z < 8; z++) {
                float fx = (x + 0.5f) / 8.0f;
                float fy = (y - 10.0f) / 10.0f;
                float fz = (z + 0.5f) / 8.0f;

                // Skull wider at cranium (top-back), narrower at jaw
                float widthMod = 1.0f;
                if (y < 8) {
                    // Jaw area - taper inward
                    widthMod = 0.7f + 0.3f * (y / 8.0f);
                }

                // Head deeper at back (cranium bulge)
                float depthMod = 1.0f;
                if (z > 0 && y > 10) {
                    depthMod = 1.15f;  // Back of skull bulges
                }

                float dx = fx / widthMod;
                float dy = fy * 0.85f;  // Slightly taller
                float dz = fz / depthMod;
                float dist = dx*dx + dy*dy + dz*dz;

                if (dist < 1.0f) {
                    // Forehead area - slight brow ridge
                    bool isBrowRidge = (y >= 10 && y <= 12 && z < -6 && abs(x) > 2 && abs(x) < 6);

                    // Hair on top and back
                    if (y >= 14 && dist > 0.7f) {
                        addCube(x, y, z, hairColor);
                    } else if (isBrowRidge) {
                        addCube(x, y, z, browColor);
                    } else {
                        addCube(x, y, z, skinColor);
                    }
                }
            }
        }
    }

    // Eye sockets (recessed areas)
    // Eyes are 1 eye-width apart, each eye is ~2 units wide at this scale
    // Left eye socket (x = -5 to -2)
    for (int ey = 9; ey <= 11; ey++) {
        for (int ex = -5; ex <= -2; ex++) {
            addCube(ex, ey, -8, shadowColor);  // Socket shadow
        }
    }
    // Right eye socket
    for (int ey = 9; ey <= 11; ey++) {
        for (int ex = 1; ex <= 4; ex++) {
            addCube(ex, ey, -8, shadowColor);
        }
    }

    // Eyeballs - white with iris and pupil
    // Left eye
    addCube(-5, 10, -9, eyeWhite);
    addCube(-4, 10, -9, irisColor);
    addCube(-3, 10, -9, eyeWhite);
    addCube(-5, 9, -9, eyeWhite);
    addCube(-4, 9, -9, pupilColor);
    addCube(-3, 9, -9, eyeWhite);

    // Right eye
    addCube(2, 10, -9, eyeWhite);
    addCube(3, 10, -9, irisColor);
    addCube(4, 10, -9, eyeWhite);
    addCube(2, 9, -9, eyeWhite);
    addCube(3, 9, -9, pupilColor);
    addCube(4, 9, -9, eyeWhite);

    // Eyebrows - arched
    for (int ex = -6; ex <= -1; ex++) {
        int browY = (abs(ex + 3) < 2) ? 13 : 12;  // Arch in middle
        addCube(ex, browY, -8, browColor);
    }
    for (int ex = 0; ex <= 5; ex++) {
        int browY = (abs(ex - 2) < 2) ? 13 : 12;
        addCube(ex, browY, -8, browColor);
    }

    // Nose - bridge and tip
    // Bridge (narrow at top, wider at bottom)
    for (int ny = 5; ny <= 9; ny++) {
        int noseWidth = (ny > 7) ? 0 : 1;
        for (int nx = -noseWidth; nx <= noseWidth; nx++) {
            int depth = (ny < 7) ? -9 : -8;
            addCube(nx, ny, depth, noseColor);
        }
    }
    // Nose tip and nostrils
    addCube(-1, 5, -10, noseColor);
    addCube(0, 5, -10, noseColor);
    addCube(-1, 4, -10, noseColor);
    addCube(0, 4, -10, noseColor);
    // Nostrils (slight shadow)
    addCube(-2, 4, -9, shadowColor);
    addCube(1, 4, -9, shadowColor);

    // Philtrum (groove between nose and lip)
    addCube(-1, 3, -8, skinColor);
    addCube(0, 3, -8, skinColor);

    // Mouth/Lips
    // Upper lip
    for (int mx = -3; mx <= 2; mx++) {
        addCube(mx, 2, -8, lipColor);
    }
    // Lower lip (slightly fuller)
    for (int mx = -3; mx <= 2; mx++) {
        addCube(mx, 1, -8, lipColor);
    }
    // Mouth corners
    addCube(-4, 2, -8, shadowColor);
    addCube(3, 2, -8, shadowColor);

    // Chin
    for (int cy = -1; cy <= 0; cy++) {
        for (int cx = -2; cx <= 1; cx++) {
            addCube(cx, cy, -8, skinColor);
        }
    }

    // Ears - positioned from eye level to nose bottom
    // Left ear
    for (int ey = 5; ey <= 11; ey++) {
        addCube(-9, ey, -2, earColor);
        addCube(-9, ey, -1, earColor);
        if (ey >= 7 && ey <= 9) {
            addCube(-10, ey, -2, earColor);  // Ear lobe/helix
        }
    }
    // Right ear
    for (int ey = 5; ey <= 11; ey++) {
        addCube(8, ey, -2, earColor);
        addCube(8, ey, -1, earColor);
        if (ey >= 7 && ey <= 9) {
            addCube(9, ey, -2, earColor);
        }
    }

    // Cheekbones (subtle prominence)
    for (int cy = 6; cy <= 8; cy++) {
        addCube(-7, cy, -6, skinColor);
        addCube(6, cy, -6, skinColor);
    }

    // Neck - cylindrical, narrower than head
    for (int ny = -4; ny < 0; ny++) {
        for (int nx = -3; nx < 3; nx++) {
            for (int nz = -3; nz < 3; nz++) {
                float ndist = (nx + 0.5f) * (nx + 0.5f) / 9.0f + (nz + 0.5f) * (nz + 0.5f) / 9.0f;
                if (ndist < 1.0f) {
                    addCube(nx, ny, nz, skinColor);
                }
            }
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "Built cube head with " << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::clear() {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();
    m_undoStack.clear();
    m_redoStack.clear();
}

void EditableMesh::setMeshData(const std::vector<HEVertex>& verts,
                                const std::vector<HalfEdge>& halfEdges,
                                const std::vector<HEFace>& faces) {
    m_vertices = verts;
    m_halfEdges = halfEdges;
    m_faces = faces;
    rebuildEdgeMap();
}

uint32_t EditableMesh::addVertex(const HEVertex& vertex) {
    uint32_t idx = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back(vertex);
    return idx;
}

uint32_t EditableMesh::addQuadFace(const std::vector<uint32_t>& vertIndices) {
    if (vertIndices.size() != 4) {
        std::cerr << "addQuadFace: Expected 4 vertices, got " << vertIndices.size() << std::endl;
        return UINT32_MAX;
    }

    uint32_t faceIdx = static_cast<uint32_t>(m_faces.size());
    HEFace face;
    face.vertexCount = 4;
    face.selected = false;

    uint32_t firstHE = static_cast<uint32_t>(m_halfEdges.size());
    face.halfEdgeIndex = firstHE;

    // Create 4 half-edges for this quad face
    for (size_t i = 0; i < 4; ++i) {
        HalfEdge he;
        he.vertexIndex = vertIndices[(i + 1) % 4];  // Points TO next vertex
        he.faceIndex = faceIdx;
        he.nextIndex = firstHE + ((i + 1) % 4);
        he.prevIndex = firstHE + ((i + 3) % 4);
        he.twinIndex = UINT32_MAX;

        m_halfEdges.push_back(he);

        // Set vertex's outgoing half-edge if not set
        if (m_vertices[vertIndices[i]].halfEdgeIndex == UINT32_MAX) {
            m_vertices[vertIndices[i]].halfEdgeIndex = firstHE + i;
        }
    }

    m_faces.push_back(face);
    rebuildEdgeMap();

    std::cout << "Added quad face " << faceIdx << " with vertices: "
              << vertIndices[0] << ", " << vertIndices[1] << ", "
              << vertIndices[2] << ", " << vertIndices[3]
              << " (vertexCount=" << face.vertexCount << ")" << std::endl;

    return faceIdx;
}

void EditableMesh::addQuadFacesBatch(const std::vector<std::array<uint32_t, 4>>& faces) {
    for (const auto& vertIndices : faces) {
        uint32_t faceIdx = static_cast<uint32_t>(m_faces.size());
        HEFace face;
        face.vertexCount = 4;
        face.selected = false;

        uint32_t firstHE = static_cast<uint32_t>(m_halfEdges.size());
        face.halfEdgeIndex = firstHE;

        for (size_t i = 0; i < 4; ++i) {
            HalfEdge he;
            he.vertexIndex = vertIndices[(i + 1) % 4];
            he.faceIndex = faceIdx;
            he.nextIndex = firstHE + ((i + 1) % 4);
            he.prevIndex = firstHE + ((i + 3) % 4);
            he.twinIndex = UINT32_MAX;
            m_halfEdges.push_back(he);

            if (m_vertices[vertIndices[i]].halfEdgeIndex == UINT32_MAX) {
                m_vertices[vertIndices[i]].halfEdgeIndex = firstHE + i;
            }
        }

        m_faces.push_back(face);
    }

    // Rebuild once after all faces are added
    rebuildEdgeMap();
    linkTwinsByPosition();

    std::cout << "Batch-added " << faces.size() << " quad faces" << std::endl;
}

uint32_t EditableMesh::addFace(const std::vector<uint32_t>& vertIndices) {
    if (vertIndices.size() < 3) return UINT32_MAX;

    uint32_t faceIdx = static_cast<uint32_t>(m_faces.size());
    HEFace face;
    face.vertexCount = static_cast<uint32_t>(vertIndices.size());
    face.selected = false;

    uint32_t firstHE = static_cast<uint32_t>(m_halfEdges.size());
    face.halfEdgeIndex = firstHE;

    // Create half-edges for this face
    for (size_t i = 0; i < vertIndices.size(); ++i) {
        HalfEdge he;
        he.vertexIndex = vertIndices[(i + 1) % vertIndices.size()];  // Points TO next vertex
        he.faceIndex = faceIdx;
        he.nextIndex = firstHE + ((i + 1) % vertIndices.size());
        he.prevIndex = firstHE + ((i + vertIndices.size() - 1) % vertIndices.size());
        he.twinIndex = UINT32_MAX;

        m_halfEdges.push_back(he);

        // Set vertex's outgoing half-edge if not set
        if (m_vertices[vertIndices[i]].halfEdgeIndex == UINT32_MAX) {
            m_vertices[vertIndices[i]].halfEdgeIndex = firstHE + i;
        }
    }

    m_faces.push_back(face);
    return faceIdx;
}

void EditableMesh::rebuildEdgeMap() {
    m_edgeMap.clear();
    m_edgeMap.reserve(m_halfEdges.size());
    for (uint32_t i = 0; i < m_halfEdges.size(); ++i) {
        uint32_t fromVert = m_halfEdges[m_halfEdges[i].prevIndex].vertexIndex;
        uint32_t toVert = m_halfEdges[i].vertexIndex;
        uint64_t key = makeEdgeKey(fromVert, toVert);
        m_edgeMap.emplace(key, i);  // emplace won't overwrite existing
    }
}

void EditableMesh::linkTwinsByPosition() {
    // Link twin half-edges by POSITION (not index) to handle duplicate vertices
    // Uses hash map for O(n) instead of O(n²) nested loop
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    // Build hash map: edge (fromPos, toPos) -> half-edge index
    // For each unlinked half-edge, insert it keyed by (from, to) position pair.
    // If the reverse edge (to, from) is already in the map, link them as twins.
    struct PairHash {
        size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
            return std::hash<uint64_t>()(p.first) ^ (std::hash<uint64_t>()(p.second) << 32);
        }
    };
    std::unordered_map<std::pair<uint64_t, uint64_t>, uint32_t, PairHash> edgeLookup;
    edgeLookup.reserve(m_halfEdges.size());

    for (size_t i = 0; i < m_halfEdges.size(); ++i) {
        if (m_halfEdges[i].twinIndex != UINT32_MAX) continue;

        uint32_t fromVert = m_halfEdges[m_halfEdges[i].prevIndex].vertexIndex;
        uint32_t toVert = m_halfEdges[i].vertexIndex;
        uint64_t fromPos = posKey(m_vertices[fromVert].position);
        uint64_t toPos = posKey(m_vertices[toVert].position);

        // Check if the reverse edge already exists
        auto reverseKey = std::make_pair(toPos, fromPos);
        auto it = edgeLookup.find(reverseKey);
        if (it != edgeLookup.end()) {
            uint32_t j = it->second;
            m_halfEdges[i].twinIndex = j;
            m_halfEdges[j].twinIndex = static_cast<uint32_t>(i);
            edgeLookup.erase(it);
        } else {
            edgeLookup[std::make_pair(fromPos, toPos)] = static_cast<uint32_t>(i);
        }
    }
}

void EditableMesh::rebuildFromFaces() {
    // Rebuild mesh structure, removing deleted faces (vertexCount == 0)
    // and recreating half-edges properly

    // Collect all valid faces with their vertex indices
    struct FaceData {
        std::vector<uint32_t> verts;
        bool selected;
    };
    std::vector<FaceData> validFaces;

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (m_faces[faceIdx].vertexCount == 0) continue;  // Skip deleted faces

        FaceData fd;
        fd.verts = getFaceVertices(faceIdx);
        fd.selected = m_faces[faceIdx].selected;
        if (!fd.verts.empty()) {
            validFaces.push_back(fd);
        }
    }

    // Reset vertex half-edge indices
    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    // Rebuild faces and half-edges
    m_faces.clear();
    m_halfEdges.clear();

    for (const auto& fd : validFaces) {
        uint32_t newFaceIdx = static_cast<uint32_t>(m_faces.size());
        HEFace face;
        face.vertexCount = static_cast<uint32_t>(fd.verts.size());
        face.selected = fd.selected;
        face.halfEdgeIndex = static_cast<uint32_t>(m_halfEdges.size());

        for (size_t i = 0; i < fd.verts.size(); ++i) {
            HalfEdge he;
            he.vertexIndex = fd.verts[(i + 1) % fd.verts.size()];
            he.faceIndex = newFaceIdx;
            he.nextIndex = face.halfEdgeIndex + ((i + 1) % fd.verts.size());
            he.prevIndex = face.halfEdgeIndex + ((i + fd.verts.size() - 1) % fd.verts.size());
            he.twinIndex = UINT32_MAX;
            m_halfEdges.push_back(he);

            if (m_vertices[fd.verts[i]].halfEdgeIndex == UINT32_MAX) {
                m_vertices[fd.verts[i]].halfEdgeIndex = face.halfEdgeIndex + i;
            }
        }

        m_faces.push_back(face);
    }

    // Relink twins and rebuild edge map
    linkTwinsByPosition();
    rebuildEdgeMap();
    m_selectedEdges.clear();
}

void EditableMesh::mergeTrianglesToQuads(float normalThreshold) {
    // Find pairs of triangles that share an edge and have similar normals
    std::vector<bool> merged(m_faces.size(), false);
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> mergeList; // (face1, face2, sharedEdgeHE)
    mergeList.reserve(m_faces.size() / 2);

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (merged[faceIdx]) continue;
        if (m_faces[faceIdx].vertexCount != 3) continue;

        glm::vec3 normal1 = getFaceNormal(faceIdx);

        uint32_t he = m_faces[faceIdx].halfEdgeIndex;
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t twin = m_halfEdges[he].twinIndex;
            if (twin != UINT32_MAX) {
                uint32_t neighborFace = m_halfEdges[twin].faceIndex;
                if (neighborFace != UINT32_MAX && !merged[neighborFace] &&
                    m_faces[neighborFace].vertexCount == 3 && neighborFace > faceIdx) {

                    glm::vec3 normal2 = getFaceNormal(neighborFace);
                    float dotProduct = glm::dot(normal1, normal2);

                    if (dotProduct > normalThreshold) {
                        // Check UV continuity — don't merge across UV seams
                        // Compare the shared edge vertices: if the twin uses different vertex
                        // indices, check that UVs match (same index = same UV, no seam)
                        uint32_t heFrom = m_halfEdges[m_halfEdges[he].prevIndex].vertexIndex;
                        uint32_t heTo = m_halfEdges[he].vertexIndex;
                        uint32_t twinFrom = m_halfEdges[m_halfEdges[twin].prevIndex].vertexIndex;
                        uint32_t twinTo = m_halfEdges[twin].vertexIndex;

                        // Twin edge is reversed: he goes A→B, twin goes B'→A'
                        // So heTo matches twinFrom position, heFrom matches twinTo position
                        bool uvMatch = true;
                        const float uvTol = 0.001f;
                        if (heTo != twinFrom) {
                            glm::vec2 d = m_vertices[heTo].uv - m_vertices[twinFrom].uv;
                            if (std::abs(d.x) > uvTol || std::abs(d.y) > uvTol) uvMatch = false;
                        }
                        if (heFrom != twinTo) {
                            glm::vec2 d = m_vertices[heFrom].uv - m_vertices[twinTo].uv;
                            if (std::abs(d.x) > uvTol || std::abs(d.y) > uvTol) uvMatch = false;
                        }

                        if (uvMatch) {
                            mergeList.push_back({faceIdx, neighborFace, he});
                            merged[faceIdx] = true;
                            merged[neighborFace] = true;
                            break;
                        }
                    }
                }
            }
            he = m_halfEdges[he].nextIndex;
        }
    }

    if (mergeList.empty()) {
        return;
    }
    std::cout << "mergeTrianglesToQuads: " << m_faces.size() << " faces -> merged " << mergeList.size() << " pairs" << std::endl;

    // Rebuild mesh with quads
    std::vector<HEVertex> newVertices = m_vertices;
    std::vector<HEFace> newFaces;
    std::vector<HalfEdge> newHalfEdges;

    // Reset vertex half-edge indices
    for (auto& v : newVertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    // Position comparison helper (handles duplicate vertices at same position)
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    // Add merged quads
    for (const auto& [f1, f2, sharedHE] : mergeList) {
        // Get vertices of both triangles
        std::vector<uint32_t> verts1 = getFaceVertices(f1);
        std::vector<uint32_t> verts2 = getFaceVertices(f2);

        // Find the shared edge vertices (by index from f1)
        uint32_t sharedV1 = m_halfEdges[m_halfEdges[sharedHE].prevIndex].vertexIndex;
        uint32_t sharedV2 = m_halfEdges[sharedHE].vertexIndex;

        // Get POSITIONS of shared vertices for comparison (handles duplicate vertices)
        uint64_t sharedPos1 = posKey(m_vertices[sharedV1].position);
        uint64_t sharedPos2 = posKey(m_vertices[sharedV2].position);

        // Build quad vertex list: start from non-shared vertex of f1,
        // go to first shared vertex, then non-shared vertex of f2, then second shared vertex
        std::vector<uint32_t> quadVerts;

        // Find non-shared vertex in f1 (compare by POSITION, not index)
        for (uint32_t v : verts1) {
            uint64_t vPos = posKey(m_vertices[v].position);
            if (vPos != sharedPos1 && vPos != sharedPos2) {
                quadVerts.push_back(v);
                break;
            }
        }
        quadVerts.push_back(sharedV1);

        // Find non-shared vertex in f2 (compare by POSITION, not index)
        for (uint32_t v : verts2) {
            uint64_t vPos = posKey(m_vertices[v].position);
            if (vPos != sharedPos1 && vPos != sharedPos2) {
                quadVerts.push_back(v);
                break;
            }
        }
        quadVerts.push_back(sharedV2);

        // Create the quad face
        uint32_t faceIdx = static_cast<uint32_t>(newFaces.size());
        HEFace face;
        face.vertexCount = 4;
        face.selected = false;
        face.halfEdgeIndex = static_cast<uint32_t>(newHalfEdges.size());

        for (size_t i = 0; i < 4; ++i) {
            HalfEdge he;
            he.vertexIndex = quadVerts[(i + 1) % 4];
            he.faceIndex = faceIdx;
            he.nextIndex = face.halfEdgeIndex + ((i + 1) % 4);
            he.prevIndex = face.halfEdgeIndex + ((i + 3) % 4);
            he.twinIndex = UINT32_MAX;
            newHalfEdges.push_back(he);

            if (newVertices[quadVerts[i]].halfEdgeIndex == UINT32_MAX) {
                newVertices[quadVerts[i]].halfEdgeIndex = face.halfEdgeIndex + i;
            }
        }

        newFaces.push_back(face);
    }

    // Add remaining unmerged triangles
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (!merged[faceIdx]) {
            std::vector<uint32_t> verts = getFaceVertices(faceIdx);

            uint32_t newFaceIdx = static_cast<uint32_t>(newFaces.size());
            HEFace face;
            face.vertexCount = static_cast<uint32_t>(verts.size());
            face.selected = false;
            face.halfEdgeIndex = static_cast<uint32_t>(newHalfEdges.size());

            for (size_t i = 0; i < verts.size(); ++i) {
                HalfEdge he;
                he.vertexIndex = verts[(i + 1) % verts.size()];
                he.faceIndex = newFaceIdx;
                he.nextIndex = face.halfEdgeIndex + ((i + 1) % verts.size());
                he.prevIndex = face.halfEdgeIndex + ((i + verts.size() - 1) % verts.size());
                he.twinIndex = UINT32_MAX;
                newHalfEdges.push_back(he);

                if (newVertices[verts[i]].halfEdgeIndex == UINT32_MAX) {
                    newVertices[verts[i]].halfEdgeIndex = face.halfEdgeIndex + i;
                }
            }

            newFaces.push_back(face);
        }
    }

    // Replace old data
    m_vertices = std::move(newVertices);
    m_faces = std::move(newFaces);
    m_halfEdges = std::move(newHalfEdges);

    // Link twin half-edges by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
    std::cout << "mergeTrianglesToQuads: Merged to " << m_faces.size() << " faces, mergeList had " << mergeList.size() << " pairs" << std::endl;
}

void EditableMesh::triangulate(std::vector<ModelVertex>& outVerts,
                               std::vector<uint32_t>& outIndices) const {
    outVerts.clear();
    outIndices.clear();

    // Convert HEVertex to ModelVertex
    outVerts.reserve(m_vertices.size());
    for (const auto& hv : m_vertices) {
        ModelVertex mv;
        mv.position = hv.position;
        mv.normal = hv.normal;
        mv.texCoord = hv.uv;
        mv.color = hv.color;
        // Ensure alpha is fully opaque
        mv.color.a = 1.0f;
        outVerts.push_back(mv);
    }

    // Triangulate each face
    for (size_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        const auto& face = m_faces[faceIdx];
        std::vector<uint32_t> faceVerts;
        uint32_t he = face.halfEdgeIndex;
        uint32_t startHE = he;
        uint32_t loopCount = 0;
        const uint32_t maxLoops = 100; // Safety limit

        for (uint32_t i = 0; i < face.vertexCount && loopCount < maxLoops; ++i) {
            if (he >= m_halfEdges.size()) {
                std::cerr << "ERROR: Face " << faceIdx << " has invalid halfEdge " << he << std::endl;
                break;
            }
            uint32_t prevHE = m_halfEdges[he].prevIndex;
            if (prevHE >= m_halfEdges.size()) {
                std::cerr << "ERROR: Face " << faceIdx << " HE " << he << " has invalid prevIndex " << prevHE << std::endl;
                break;
            }
            uint32_t vertIdx = m_halfEdges[prevHE].vertexIndex;
            if (vertIdx >= m_vertices.size()) {
                std::cerr << "ERROR: Face " << faceIdx << " has invalid vertex " << vertIdx << std::endl;
                break;
            }
            faceVerts.push_back(vertIdx);
            he = m_halfEdges[he].nextIndex;
            loopCount++;
        }

        if (loopCount >= maxLoops) {
            std::cerr << "ERROR: Face " << faceIdx << " has infinite loop in half-edges!" << std::endl;
            continue;
        }

        // Fan triangulation for convex polygons
        for (uint32_t i = 1; i + 1 < faceVerts.size(); ++i) {
            outIndices.push_back(faceVerts[0]);
            outIndices.push_back(faceVerts[i]);
            outIndices.push_back(faceVerts[i + 1]);
        }
    }
}

void EditableMesh::triangulate(std::vector<ModelVertex>& outVerts,
                               std::vector<uint32_t>& outIndices,
                               const std::set<uint32_t>& hiddenFaces) const {
    outVerts.clear();
    outIndices.clear();

    // Convert HEVertex to ModelVertex
    outVerts.reserve(m_vertices.size());
    for (const auto& hv : m_vertices) {
        ModelVertex mv;
        mv.position = hv.position;
        mv.normal = hv.normal;
        mv.texCoord = hv.uv;
        mv.color = hv.color;
        mv.color.a = 1.0f;
        outVerts.push_back(mv);
    }

    // Triangulate each face (skip hidden faces)
    for (size_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        // Skip hidden faces
        if (hiddenFaces.find(static_cast<uint32_t>(faceIdx)) != hiddenFaces.end()) {
            continue;
        }

        const auto& face = m_faces[faceIdx];
        std::vector<uint32_t> faceVerts;
        uint32_t he = face.halfEdgeIndex;
        uint32_t loopCount = 0;
        const uint32_t maxLoops = 100;

        for (uint32_t i = 0; i < face.vertexCount && loopCount < maxLoops; ++i) {
            if (he >= m_halfEdges.size()) break;
            uint32_t prevHE = m_halfEdges[he].prevIndex;
            if (prevHE >= m_halfEdges.size()) break;
            uint32_t vertIdx = m_halfEdges[prevHE].vertexIndex;
            if (vertIdx >= m_vertices.size()) break;
            faceVerts.push_back(vertIdx);
            he = m_halfEdges[he].nextIndex;
            loopCount++;
        }

        if (loopCount >= maxLoops) continue;

        // Fan triangulation for convex polygons
        for (uint32_t i = 1; i + 1 < faceVerts.size(); ++i) {
            outIndices.push_back(faceVerts[0]);
            outIndices.push_back(faceVerts[i]);
            outIndices.push_back(faceVerts[i + 1]);
        }
    }
}

void EditableMesh::triangulateSkinned(std::vector<SkinnedVertex>& outVerts,
                                       std::vector<uint32_t>& outIndices) const {
    std::set<uint32_t> noHidden;
    triangulateSkinned(outVerts, outIndices, noHidden);
}

void EditableMesh::triangulateSkinned(std::vector<SkinnedVertex>& outVerts,
                                       std::vector<uint32_t>& outIndices,
                                       const std::set<uint32_t>& hiddenFaces) const {
    outVerts.clear();
    outIndices.clear();

    outVerts.reserve(m_vertices.size());
    for (const auto& hv : m_vertices) {
        SkinnedVertex sv;
        sv.position = hv.position;
        sv.normal = hv.normal;
        sv.texCoord = hv.uv;
        sv.color = hv.color;
        sv.color.a = 1.0f;
        sv.joints = hv.boneIndices;
        sv.weights = hv.boneWeights;
        outVerts.push_back(sv);
    }

    for (size_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (!hiddenFaces.empty() &&
            hiddenFaces.find(static_cast<uint32_t>(faceIdx)) != hiddenFaces.end()) {
            continue;
        }

        const auto& face = m_faces[faceIdx];
        std::vector<uint32_t> faceVerts;
        uint32_t he = face.halfEdgeIndex;
        uint32_t loopCount = 0;
        const uint32_t maxLoops = 100;

        for (uint32_t i = 0; i < face.vertexCount && loopCount < maxLoops; ++i) {
            if (he >= m_halfEdges.size()) break;
            uint32_t prevHE = m_halfEdges[he].prevIndex;
            if (prevHE >= m_halfEdges.size()) break;
            uint32_t vertIdx = m_halfEdges[prevHE].vertexIndex;
            if (vertIdx >= m_vertices.size()) break;
            faceVerts.push_back(vertIdx);
            he = m_halfEdges[he].nextIndex;
            loopCount++;
        }

        if (loopCount >= maxLoops) continue;

        for (uint32_t i = 1; i + 1 < faceVerts.size(); ++i) {
            outIndices.push_back(faceVerts[0]);
            outIndices.push_back(faceVerts[i]);
            outIndices.push_back(faceVerts[i + 1]);
        }
    }
}

// Helper: distance from point to line segment
static float pointToSegmentDistance(const glm::vec3& point, const glm::vec3& segA, const glm::vec3& segB) {
    glm::vec3 ab = segB - segA;
    float lenSq = glm::dot(ab, ab);
    if (lenSq < 1e-8f) {
        return glm::length(point - segA);
    }
    float t = glm::clamp(glm::dot(point - segA, ab) / lenSq, 0.0f, 1.0f);
    glm::vec3 closest = segA + t * ab;
    return glm::length(point - closest);
}

void EditableMesh::generateAutoWeights(const std::vector<glm::vec3>& boneHeadPositions) {
    if (m_skeleton.bones.empty() || boneHeadPositions.empty()) return;

    int numBones = static_cast<int>(m_skeleton.bones.size());

    // Precompute children for each bone
    std::vector<std::vector<int>> boneChildren(numBones);
    for (int b = 0; b < numBones; ++b) {
        int p = m_skeleton.bones[b].parentIndex;
        if (p >= 0 && p < numBones) {
            boneChildren[p].push_back(b);
        }
    }

    for (auto& vert : m_vertices) {
        // Each bone owns the segments toward its CHILDREN, not toward its parent
        // This way Bone.001 owns the space from 001→002, Bone.002 owns 002→003, etc.
        std::vector<std::pair<float, int>> distances; // (distance, boneIndex)
        for (int b = 0; b < numBones; ++b) {
            glm::vec3 head = boneHeadPositions[b];
            float dist;
            if (boneChildren[b].empty()) {
                // Leaf bone: just point distance
                dist = glm::length(vert.position - head);
            } else {
                // Min distance to any segment from this bone to its children
                dist = std::numeric_limits<float>::max();
                for (int child : boneChildren[b]) {
                    float d = pointToSegmentDistance(vert.position, head, boneHeadPositions[child]);
                    dist = std::min(dist, d);
                }
            }
            distances.push_back({dist, b});
        }

        // Sort by distance, take 3 closest
        std::sort(distances.begin(), distances.end());
        int count = std::min(3, static_cast<int>(distances.size()));

        glm::ivec4 indices(0);
        glm::vec4 weights(0.0f);
        float totalWeight = 0.0f;

        for (int i = 0; i < count; ++i) {
            indices[i] = distances[i].second;
            float d = distances[i].first + 0.001f;
            weights[i] = 1.0f / (d * d * d);
            totalWeight += weights[i];
        }

        // Normalize
        if (totalWeight > 0.0f) {
            weights /= totalWeight;
        } else {
            weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        }
        weights[3] = 0.0f;
        indices[3] = 0;

        vert.boneIndices = indices;
        vert.boneWeights = weights;
    }

    std::cout << "[AutoWeights] Assigned weights for " << m_vertices.size()
              << " vertices using " << numBones << " bones" << std::endl;
}

void EditableMesh::clearBoneWeights() {
    for (auto& vert : m_vertices) {
        vert.boneIndices = glm::ivec4(0);
        vert.boneWeights = glm::vec4(0.0f);
    }
}

std::vector<uint32_t> EditableMesh::getFaceVertices(uint32_t faceIdx) const {
    std::vector<uint32_t> result;
    if (faceIdx >= m_faces.size()) return result;

    const HEFace& face = m_faces[faceIdx];
    uint32_t he = face.halfEdgeIndex;

    for (uint32_t i = 0; i < face.vertexCount; ++i) {
        // Get vertex at start of this half-edge
        result.push_back(m_halfEdges[m_halfEdges[he].prevIndex].vertexIndex);
        he = m_halfEdges[he].nextIndex;
    }

    return result;
}

std::vector<uint32_t> EditableMesh::getVertexFaces(uint32_t vertIdx) const {
    std::vector<uint32_t> result;
    if (vertIdx >= m_vertices.size()) return result;

    uint32_t startHE = m_vertices[vertIdx].halfEdgeIndex;
    if (startHE == UINT32_MAX) return result;

    // Walk around vertex using twin/prev
    uint32_t he = startHE;
    do {
        if (m_halfEdges[he].faceIndex != UINT32_MAX) {
            result.push_back(m_halfEdges[he].faceIndex);
        }

        // Go to prev to get the other half-edge from this vertex
        uint32_t prev = m_halfEdges[he].prevIndex;
        // Then get twin to go to adjacent face
        uint32_t twin = m_halfEdges[prev].twinIndex;

        if (twin == UINT32_MAX) {
            // Hit boundary, try going the other way
            break;
        }
        he = twin;
    } while (he != startHE);

    // Remove duplicates
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

std::vector<uint32_t> EditableMesh::getVertexEdges(uint32_t vertIdx) const {
    std::vector<uint32_t> result;
    if (vertIdx >= m_vertices.size()) return result;

    uint32_t startHE = m_vertices[vertIdx].halfEdgeIndex;
    if (startHE == UINT32_MAX) return result;

    uint32_t he = startHE;
    do {
        result.push_back(he);

        uint32_t prev = m_halfEdges[he].prevIndex;
        uint32_t twin = m_halfEdges[prev].twinIndex;

        if (twin == UINT32_MAX) break;
        he = twin;
    } while (he != startHE);

    return result;
}

std::vector<uint32_t> EditableMesh::getVertexNeighbors(uint32_t vertIdx) const {
    std::vector<uint32_t> result;
    if (vertIdx >= m_vertices.size()) return result;

    auto edges = getVertexEdges(vertIdx);
    for (uint32_t he : edges) {
        result.push_back(m_halfEdges[he].vertexIndex);
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

std::vector<uint32_t> EditableMesh::getFaceEdges(uint32_t faceIdx) const {
    std::vector<uint32_t> result;
    if (faceIdx >= m_faces.size()) return result;

    const HEFace& face = m_faces[faceIdx];
    uint32_t he = face.halfEdgeIndex;

    for (uint32_t i = 0; i < face.vertexCount; ++i) {
        result.push_back(he);
        he = m_halfEdges[he].nextIndex;
    }

    return result;
}

std::vector<uint32_t> EditableMesh::getFaceNeighbors(uint32_t faceIdx) const {
    std::vector<uint32_t> result;
    auto edges = getFaceEdges(faceIdx);

    for (uint32_t he : edges) {
        uint32_t twin = m_halfEdges[he].twinIndex;
        if (twin != UINT32_MAX) {
            uint32_t neighborFace = m_halfEdges[twin].faceIndex;
            if (neighborFace != UINT32_MAX) {
                result.push_back(neighborFace);
            }
        }
    }

    return result;
}

std::pair<uint32_t, uint32_t> EditableMesh::getEdgeVertices(uint32_t heIdx) const {
    if (heIdx >= m_halfEdges.size()) return {UINT32_MAX, UINT32_MAX};

    uint32_t fromVert = m_halfEdges[m_halfEdges[heIdx].prevIndex].vertexIndex;
    uint32_t toVert = m_halfEdges[heIdx].vertexIndex;
    return {fromVert, toVert};
}

uint32_t EditableMesh::findNextLoopEdge(uint32_t heIdx) const {
    // For edge loop: find the edge across the quad from this edge
    uint32_t faceIdx = m_halfEdges[heIdx].faceIndex;
    if (faceIdx == UINT32_MAX) return UINT32_MAX;

    const HEFace& face = m_faces[faceIdx];
    if (face.vertexCount != 4) return UINT32_MAX;  // Only works for quads

    // In a quad, the opposite edge is 2 steps forward
    uint32_t next1 = m_halfEdges[heIdx].nextIndex;
    uint32_t next2 = m_halfEdges[next1].nextIndex;

    return next2;
}

std::vector<uint32_t> EditableMesh::getEdgeLoop(uint32_t heIdx) const {
    std::vector<uint32_t> loop;
    if (heIdx >= m_halfEdges.size()) return loop;

    std::set<uint32_t> visited;

    // Walk in one direction
    uint32_t current = heIdx;
    while (current != UINT32_MAX && visited.find(current) == visited.end()) {
        // Add the canonical edge (smaller half-edge index of pair)
        uint32_t twin = m_halfEdges[current].twinIndex;
        uint32_t canonical = (twin != UINT32_MAX && twin < current) ? twin : current;
        loop.push_back(canonical);
        visited.insert(current);
        if (twin != UINT32_MAX) visited.insert(twin);

        // Find opposite edge in this face
        uint32_t opposite = findNextLoopEdge(current);
        if (opposite == UINT32_MAX) break;

        // Cross to other face via twin
        current = m_halfEdges[opposite].twinIndex;
    }

    // Walk in opposite direction from twin
    uint32_t startTwin = m_halfEdges[heIdx].twinIndex;
    if (startTwin != UINT32_MAX && visited.find(startTwin) == visited.end()) {
        current = startTwin;
        std::vector<uint32_t> backLoop;

        while (current != UINT32_MAX && visited.find(current) == visited.end()) {
            uint32_t twin = m_halfEdges[current].twinIndex;
            uint32_t canonical = (twin != UINT32_MAX && twin < current) ? twin : current;
            backLoop.push_back(canonical);
            visited.insert(current);
            if (twin != UINT32_MAX) visited.insert(twin);

            uint32_t opposite = findNextLoopEdge(current);
            if (opposite == UINT32_MAX) break;

            current = m_halfEdges[opposite].twinIndex;
        }

        // Prepend back loop (reversed) to main loop
        std::reverse(backLoop.begin(), backLoop.end());
        backLoop.insert(backLoop.end(), loop.begin(), loop.end());
        loop = std::move(backLoop);
    }

    // Remove duplicates
    std::sort(loop.begin(), loop.end());
    loop.erase(std::unique(loop.begin(), loop.end()), loop.end());

    return loop;
}

std::vector<uint32_t> EditableMesh::getEdgeRing(uint32_t heIdx) const {
    std::vector<uint32_t> ring;
    if (heIdx >= m_halfEdges.size()) return ring;

    std::set<uint32_t> visited;

    // Walk along the edge (using next edges) in both directions
    auto walkRing = [&](uint32_t startHE) {
        uint32_t current = startHE;
        while (current != UINT32_MAX && visited.find(current) == visited.end()) {
            uint32_t twin = m_halfEdges[current].twinIndex;
            uint32_t canonical = (twin != UINT32_MAX && twin < current) ? twin : current;
            ring.push_back(canonical);
            visited.insert(current);
            if (twin != UINT32_MAX) visited.insert(twin);

            // Move to next edge in face
            uint32_t next = m_halfEdges[current].nextIndex;
            // Cross to adjacent face
            current = m_halfEdges[next].twinIndex;
            if (current == UINT32_MAX) break;
            // Move to opposite side
            current = m_halfEdges[current].nextIndex;
        }
    };

    walkRing(heIdx);

    // Walk other direction from twin
    if (m_halfEdges[heIdx].twinIndex != UINT32_MAX) {
        uint32_t twin = m_halfEdges[heIdx].twinIndex;
        uint32_t next = m_halfEdges[twin].nextIndex;
        uint32_t crossing = m_halfEdges[next].twinIndex;
        if (crossing != UINT32_MAX) {
            walkRing(m_halfEdges[crossing].nextIndex);
        }
    }

    std::sort(ring.begin(), ring.end());
    ring.erase(std::unique(ring.begin(), ring.end()), ring.end());

    return ring;
}

bool EditableMesh::isQuad(uint32_t faceIdx) const {
    if (faceIdx >= m_faces.size()) return false;
    return m_faces[faceIdx].vertexCount == 4;
}

void EditableMesh::selectVertex(uint32_t idx, bool additive) {
    if (idx >= m_vertices.size()) return;

    if (!additive) {
        clearSelection();
    }
    m_vertices[idx].selected = true;
}

void EditableMesh::selectEdge(uint32_t heIdx, bool additive) {
    if (heIdx >= m_halfEdges.size()) return;

    if (!additive) {
        clearSelection();
    }

    // Select both half-edges of the edge
    m_selectedEdges.insert(heIdx);
    uint32_t twin = m_halfEdges[heIdx].twinIndex;
    if (twin != UINT32_MAX) {
        m_selectedEdges.insert(twin);
    }
}

void EditableMesh::selectFace(uint32_t idx, bool additive) {
    if (idx >= m_faces.size()) return;

    if (!additive) {
        clearSelection();
    }
    m_faces[idx].selected = true;
}

void EditableMesh::selectEdgeLoop(uint32_t heIdx) {
    auto loop = getEdgeLoop(heIdx);
    for (uint32_t he : loop) {
        m_selectedEdges.insert(he);
        uint32_t twin = m_halfEdges[he].twinIndex;
        if (twin != UINT32_MAX) {
            m_selectedEdges.insert(twin);
        }
    }
}

void EditableMesh::selectEdgeRing(uint32_t heIdx) {
    auto ring = getEdgeRing(heIdx);
    for (uint32_t he : ring) {
        m_selectedEdges.insert(he);
        uint32_t twin = m_halfEdges[he].twinIndex;
        if (twin != UINT32_MAX) {
            m_selectedEdges.insert(twin);
        }
    }
}

void EditableMesh::selectFacesByNormal(const glm::vec3& viewDir, float maxAngleDeg,
                                        const std::set<uint32_t>& skipFaces) {
    float cosThreshold = cosf(glm::radians(maxAngleDeg));
    glm::vec3 dir = glm::normalize(viewDir);

    for (uint32_t i = 0; i < m_faces.size(); ++i) {
        if (skipFaces.count(i)) continue;
        glm::vec3 normal = getFaceNormal(i);
        // Face normal points outward; camera looks inward.
        // A front-facing face has normal opposing the view direction.
        float dot = glm::dot(normal, -dir);
        if (dot >= cosThreshold) {
            m_faces[i].selected = true;
        }
    }
}

void EditableMesh::clearSelection() {
    for (auto& v : m_vertices) {
        v.selected = false;
    }
    for (auto& f : m_faces) {
        f.selected = false;
    }
    m_selectedEdges.clear();
}

void EditableMesh::invertSelection(ModelingSelectionMode mode) {
    switch (mode) {
        case ModelingSelectionMode::Vertex:
            for (auto& v : m_vertices) {
                v.selected = !v.selected;
            }
            break;
        case ModelingSelectionMode::Edge: {
            std::set<uint32_t> allEdges;
            for (uint32_t i = 0; i < m_halfEdges.size(); ++i) {
                allEdges.insert(i);
            }
            std::set<uint32_t> newSelection;
            std::set_difference(allEdges.begin(), allEdges.end(),
                              m_selectedEdges.begin(), m_selectedEdges.end(),
                              std::inserter(newSelection, newSelection.begin()));
            m_selectedEdges = std::move(newSelection);
            break;
        }
        case ModelingSelectionMode::Face:
            for (auto& f : m_faces) {
                f.selected = !f.selected;
            }
            break;
    }
}

void EditableMesh::toggleVertexSelection(uint32_t idx) {
    if (idx >= m_vertices.size()) return;
    m_vertices[idx].selected = !m_vertices[idx].selected;
}

void EditableMesh::toggleEdgeSelection(uint32_t heIdx) {
    if (heIdx >= m_halfEdges.size()) return;

    uint32_t twin = m_halfEdges[heIdx].twinIndex;

    if (m_selectedEdges.count(heIdx) > 0) {
        m_selectedEdges.erase(heIdx);
        if (twin != UINT32_MAX) m_selectedEdges.erase(twin);
    } else {
        m_selectedEdges.insert(heIdx);
        if (twin != UINT32_MAX) m_selectedEdges.insert(twin);
    }
}

void EditableMesh::toggleFaceSelection(uint32_t idx) {
    if (idx >= m_faces.size()) return;
    m_faces[idx].selected = !m_faces[idx].selected;
}

std::vector<uint32_t> EditableMesh::getSelectedVertices() const {
    std::vector<uint32_t> result;
    for (uint32_t i = 0; i < m_vertices.size(); ++i) {
        if (m_vertices[i].selected) {
            result.push_back(i);
        }
    }
    return result;
}

std::vector<uint32_t> EditableMesh::getSelectedEdges() const {
    // Return unique edges (just one half-edge per edge)
    std::set<uint32_t> uniqueEdges;
    for (uint32_t he : m_selectedEdges) {
        uint32_t twin = m_halfEdges[he].twinIndex;
        uint32_t canonical = (twin != UINT32_MAX && twin < he) ? twin : he;
        uniqueEdges.insert(canonical);
    }
    return std::vector<uint32_t>(uniqueEdges.begin(), uniqueEdges.end());
}

std::vector<uint32_t> EditableMesh::getSelectedFaces() const {
    std::vector<uint32_t> result;
    for (uint32_t i = 0; i < m_faces.size(); ++i) {
        if (m_faces[i].selected) {
            result.push_back(i);
        }
    }
    return result;
}

bool EditableMesh::hasSelection() const {
    for (const auto& v : m_vertices) {
        if (v.selected) return true;
    }
    if (!m_selectedEdges.empty()) return true;
    for (const auto& f : m_faces) {
        if (f.selected) return true;
    }
    return false;
}

glm::vec3 EditableMesh::getFaceNormal(uint32_t faceIdx) const {
    auto verts = getFaceVertices(faceIdx);
    if (verts.size() < 3) return glm::vec3(0, 1, 0);

    glm::vec3 v0 = m_vertices[verts[0]].position;
    glm::vec3 v1 = m_vertices[verts[1]].position;
    glm::vec3 v2 = m_vertices[verts[2]].position;

    return glm::normalize(glm::cross(v1 - v0, v2 - v0));
}

glm::vec3 EditableMesh::getFaceCenter(uint32_t faceIdx) const {
    auto verts = getFaceVertices(faceIdx);
    if (verts.empty()) return glm::vec3(0);

    glm::vec3 center(0);
    for (uint32_t v : verts) {
        center += m_vertices[v].position;
    }
    return center / static_cast<float>(verts.size());
}

MeshRayHit EditableMesh::raycastVertex(const glm::vec3& origin, const glm::vec3& dir,
                                        float threshold) const {
    MeshRayHit result;
    float closestDist = std::numeric_limits<float>::max();

    for (uint32_t i = 0; i < m_vertices.size(); ++i) {
        const glm::vec3& pos = m_vertices[i].position;

        // Project vertex onto ray
        glm::vec3 toVert = pos - origin;
        float t = glm::dot(toVert, dir);
        if (t < 0) continue;  // Behind camera

        glm::vec3 closest = origin + dir * t;
        float dist = glm::length(pos - closest);

        if (dist < threshold && t < closestDist) {
            closestDist = t;
            result.hit = true;
            result.distance = t;
            result.position = pos;
            result.vertexIndex = i;
        }
    }

    return result;
}

MeshRayHit EditableMesh::raycastEdge(const glm::vec3& origin, const glm::vec3& dir,
                                      float threshold) const {
    MeshRayHit result;
    float closestDist = std::numeric_limits<float>::max();

    std::set<uint64_t> processedEdges;

    for (uint32_t i = 0; i < m_halfEdges.size(); ++i) {
        auto [v0, v1] = getEdgeVertices(i);
        uint64_t key = makeEdgeKey(v0, v1);
        if (processedEdges.count(key) > 0) continue;
        processedEdges.insert(key);

        glm::vec3 p0 = m_vertices[v0].position;
        glm::vec3 p1 = m_vertices[v1].position;
        glm::vec3 edgeDir = p1 - p0;
        float edgeLen = glm::length(edgeDir);
        if (edgeLen < 0.0001f) continue;
        edgeDir /= edgeLen;

        // Find closest points between ray and edge
        glm::vec3 w0 = origin - p0;
        float a = glm::dot(dir, dir);
        float b = glm::dot(dir, edgeDir);
        float c = glm::dot(edgeDir, edgeDir);
        float d = glm::dot(dir, w0);
        float e = glm::dot(edgeDir, w0);
        float denom = a * c - b * b;

        if (std::abs(denom) < 0.0001f) continue;  // Parallel

        float t = (b * e - c * d) / denom;
        float s = (a * e - b * d) / denom;

        if (t < 0 || s < 0 || s > edgeLen) continue;

        glm::vec3 closestOnRay = origin + dir * t;
        glm::vec3 closestOnEdge = p0 + edgeDir * s;
        float dist = glm::length(closestOnRay - closestOnEdge);

        if (dist < threshold && t < closestDist) {
            closestDist = t;
            result.hit = true;
            result.distance = t;
            result.position = closestOnEdge;
            result.edgeIndex = i;
        }
    }

    return result;
}

MeshRayHit EditableMesh::raycastFace(const glm::vec3& origin, const glm::vec3& dir) const {
    MeshRayHit result;
    float closestDist = std::numeric_limits<float>::max();

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        auto verts = getFaceVertices(faceIdx);
        if (verts.size() < 3) continue;

        // Triangulate and test each triangle
        for (size_t i = 1; i + 1 < verts.size(); ++i) {
            glm::vec3 v0 = m_vertices[verts[0]].position;
            glm::vec3 v1 = m_vertices[verts[i]].position;
            glm::vec3 v2 = m_vertices[verts[i + 1]].position;

            // Moller-Trumbore intersection
            glm::vec3 edge1 = v1 - v0;
            glm::vec3 edge2 = v2 - v0;
            glm::vec3 h = glm::cross(dir, edge2);
            float a = glm::dot(edge1, h);

            if (std::abs(a) < 0.0001f) continue;

            float f = 1.0f / a;
            glm::vec3 s = origin - v0;
            float u = f * glm::dot(s, h);

            if (u < 0.0f || u > 1.0f) continue;

            glm::vec3 q = glm::cross(s, edge1);
            float v = f * glm::dot(dir, q);

            if (v < 0.0f || u + v > 1.0f) continue;

            float t = f * glm::dot(edge2, q);

            if (t > 0.0001f && t < closestDist) {
                closestDist = t;
                result.hit = true;
                result.distance = t;
                result.position = origin + dir * t;
                result.normal = glm::normalize(glm::cross(edge1, edge2));
                result.faceIndex = faceIdx;
            }
        }
    }

    return result;
}

MeshRayHit EditableMesh::raycastFace(const glm::vec3& origin, const glm::vec3& dir,
                                     const std::set<uint32_t>& skipFaces) const {
    MeshRayHit result;
    float closestDist = std::numeric_limits<float>::max();

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        // Skip hidden faces
        if (skipFaces.find(faceIdx) != skipFaces.end()) continue;

        auto verts = getFaceVertices(faceIdx);
        if (verts.size() < 3) continue;

        // Triangulate and test each triangle
        for (size_t i = 1; i + 1 < verts.size(); ++i) {
            glm::vec3 v0 = m_vertices[verts[0]].position;
            glm::vec3 v1 = m_vertices[verts[i]].position;
            glm::vec3 v2 = m_vertices[verts[i + 1]].position;

            // Moller-Trumbore intersection
            glm::vec3 edge1 = v1 - v0;
            glm::vec3 edge2 = v2 - v0;
            glm::vec3 h = glm::cross(dir, edge2);
            float a = glm::dot(edge1, h);

            if (std::abs(a) < 0.0001f) continue;

            float f = 1.0f / a;
            glm::vec3 s = origin - v0;
            float u = f * glm::dot(s, h);

            if (u < 0.0f || u > 1.0f) continue;

            glm::vec3 q = glm::cross(s, edge1);
            float v = f * glm::dot(dir, q);

            if (v < 0.0f || u + v > 1.0f) continue;

            float t = f * glm::dot(edge2, q);

            if (t > 0.0001f && t < closestDist) {
                closestDist = t;
                result.hit = true;
                result.distance = t;
                result.position = origin + dir * t;
                result.normal = glm::normalize(glm::cross(edge1, edge2));
                result.faceIndex = faceIdx;
            }
        }
    }

    return result;
}

MeshRayHit EditableMesh::raycast(const glm::vec3& origin, const glm::vec3& dir,
                                  ModelingSelectionMode mode, float threshold) const {
    switch (mode) {
        case ModelingSelectionMode::Vertex:
            return raycastVertex(origin, dir, threshold);
        case ModelingSelectionMode::Edge:
            return raycastEdge(origin, dir, threshold);
        case ModelingSelectionMode::Face:
            return raycastFace(origin, dir);
    }
    return MeshRayHit();
}

MeshRayHit EditableMesh::raycast(const glm::vec3& origin, const glm::vec3& dir,
                                  ModelingSelectionMode mode, float threshold,
                                  const std::set<uint32_t>& skipFaces) const {
    switch (mode) {
        case ModelingSelectionMode::Vertex:
            return raycastVertex(origin, dir, threshold);
        case ModelingSelectionMode::Edge:
            return raycastEdge(origin, dir, threshold);
        case ModelingSelectionMode::Face:
            return raycastFace(origin, dir, skipFaces);
    }
    return MeshRayHit();
}

void EditableMesh::extrudeFaces(const std::vector<uint32_t>& faceIndices, float distance) {
    if (faceIndices.empty()) return;

    std::set<uint32_t> selectedFaceSet(faceIndices.begin(), faceIndices.end());

    // Get all vertices of selected faces and track which faces each vertex belongs to
    std::map<uint32_t, std::vector<uint32_t>> vertexToFaces;  // vertex -> list of selected faces it belongs to
    for (uint32_t faceIdx : faceIndices) {
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            vertexToFaces[v].push_back(faceIdx);
        }
    }

    std::vector<uint32_t> faceVerts;
    for (const auto& [v, faces] : vertexToFaces) {
        faceVerts.push_back(v);
    }

    // Calculate per-vertex extrusion direction (average of normals of selected faces containing that vertex)
    std::map<uint32_t, glm::vec3> vertexExtrudeDir;
    for (uint32_t v : faceVerts) {
        glm::vec3 dir(0);
        for (uint32_t faceIdx : vertexToFaces[v]) {
            dir += getFaceNormal(faceIdx);
        }
        vertexExtrudeDir[v] = glm::normalize(dir);
    }

    // Store original positions
    std::map<uint32_t, glm::vec3> originalPositions;
    for (uint32_t v : faceVerts) {
        originalPositions[v] = m_vertices[v].position;
    }

    // Check if connected to mesh (any edge shared with non-selected faces)
    bool hasConnectionToMesh = false;

    for (uint32_t faceIdx : faceIndices) {
        auto edges = getFaceEdges(faceIdx);
        for (uint32_t he : edges) {
            uint32_t twin = m_halfEdges[he].twinIndex;
            if (twin != UINT32_MAX) {
                uint32_t neighborFace = m_halfEdges[twin].faceIndex;
                if (neighborFace != UINT32_MAX && selectedFaceSet.find(neighborFace) == selectedFaceSet.end()) {
                    hasConnectionToMesh = true;
                    break;
                }
            }
        }
        if (hasConnectionToMesh) break;
    }

    // Create vertex mappings
    // For connected extrusion: create TWO new vertices per face vertex
    //   - bottomVert: at original position (for adjacent faces and side faces' bottom)
    //   - topVert: at extruded position (for extruded face and side faces' top)
    // For floating extrusion: create ONE new vertex per face vertex
    //   - bottomVert: at original position (for bottom face and side faces' bottom)
    //   - topVert: use original vertex moved to extruded position

    std::map<uint32_t, uint32_t> oldToBottomVert;
    std::map<uint32_t, uint32_t> oldToTopVert;

    if (hasConnectionToMesh) {
        // Connected extrusion: create all new vertices (don't modify originals)
        for (uint32_t oldVert : faceVerts) {
            glm::vec3 extrudeDir = vertexExtrudeDir[oldVert];

            // Create vertex at original position (for adjacent faces and side bottoms)
            HEVertex bottomVert = m_vertices[oldVert];
            bottomVert.halfEdgeIndex = UINT32_MAX;
            bottomVert.selected = false;
            uint32_t bottomIdx = static_cast<uint32_t>(m_vertices.size());
            oldToBottomVert[oldVert] = bottomIdx;
            m_vertices.push_back(bottomVert);

            // Create vertex at extruded position (for top face and side tops)
            HEVertex topVert = m_vertices[oldVert];
            topVert.position = originalPositions[oldVert] + extrudeDir * distance;
            topVert.normal = extrudeDir;
            topVert.halfEdgeIndex = UINT32_MAX;
            topVert.selected = false;
            uint32_t topIdx = static_cast<uint32_t>(m_vertices.size());
            oldToTopVert[oldVert] = topIdx;
            m_vertices.push_back(topVert);
        }
    } else {
        // Floating extrusion: create bottom vertices, move originals to top
        for (uint32_t oldVert : faceVerts) {
            glm::vec3 extrudeDir = vertexExtrudeDir[oldVert];

            // Create vertex at original position (for bottom face and side bottoms)
            HEVertex bottomVert = m_vertices[oldVert];
            bottomVert.halfEdgeIndex = UINT32_MAX;
            bottomVert.selected = false;
            uint32_t bottomIdx = static_cast<uint32_t>(m_vertices.size());
            oldToBottomVert[oldVert] = bottomIdx;
            m_vertices.push_back(bottomVert);

            // Move original vertex to extruded position
            m_vertices[oldVert].position = originalPositions[oldVert] + extrudeDir * distance;
            m_vertices[oldVert].normal = extrudeDir;
            oldToTopVert[oldVert] = oldVert;  // Use original vertex for top
        }
    }

    // Find boundary edges of selected faces (edges that appear only once in selection)
    std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> edgeToVerts;
    for (uint32_t faceIdx : faceIndices) {
        auto verts = getFaceVertices(faceIdx);
        for (size_t i = 0; i < verts.size(); ++i) {
            uint32_t v0 = verts[i];
            uint32_t v1 = verts[(i + 1) % verts.size()];
            uint64_t key = makeEdgeKey(v0, v1);
            edgeToVerts[key].push_back({v0, v1});
        }
    }

    // Create new faces
    std::vector<std::vector<uint32_t>> newFaces;

    // Create side quads for boundary edges
    for (const auto& [key, vertPairs] : edgeToVerts) {
        if (vertPairs.size() == 1) {
            uint32_t v0 = vertPairs[0].first;
            uint32_t v1 = vertPairs[0].second;
            uint32_t v0_bottom = oldToBottomVert[v0];
            uint32_t v1_bottom = oldToBottomVert[v1];
            uint32_t v0_top = oldToTopVert[v0];
            uint32_t v1_top = oldToTopVert[v1];

            // Side quad: bottom0, bottom1, top1, top0 (CCW when viewed from outside)
            newFaces.push_back({v0_bottom, v1_bottom, v1_top, v0_top});
        }
    }

    // Create bottom face only for floating extrusion
    if (!hasConnectionToMesh) {
        for (uint32_t faceIdx : faceIndices) {
            auto topVerts = getFaceVertices(faceIdx);
            std::vector<uint32_t> bottomVerts;
            // Reverse winding for bottom face
            for (int i = static_cast<int>(topVerts.size()) - 1; i >= 0; --i) {
                bottomVerts.push_back(oldToBottomVert[topVerts[i]]);
            }
            newFaces.push_back(bottomVerts);
        }
    }

    // Rebuild mesh with updated faces
    std::vector<std::vector<uint32_t>> allFaces;

    // Process existing faces
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        auto verts = getFaceVertices(faceIdx);

        if (selectedFaceSet.count(faceIdx) > 0) {
            // Extruded face: update vertices to use top positions
            std::vector<uint32_t> newVerts;
            for (uint32_t v : verts) {
                newVerts.push_back(oldToTopVert[v]);
            }
            allFaces.push_back(newVerts);
        } else if (hasConnectionToMesh) {
            // Adjacent face: update any shared vertices to use bottom positions
            std::vector<uint32_t> newVerts;
            for (uint32_t v : verts) {
                auto it = oldToBottomVert.find(v);
                if (it != oldToBottomVert.end()) {
                    newVerts.push_back(it->second);
                } else {
                    newVerts.push_back(v);
                }
            }
            allFaces.push_back(newVerts);
        } else {
            // Unrelated face: keep as-is
            allFaces.push_back(verts);
        }
    }

    // Add new side and bottom faces
    for (const auto& face : newFaces) {
        allFaces.push_back(face);
    }

    // Rebuild the half-edge structure
    std::vector<HEVertex> savedVerts = m_vertices;
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    // Reset vertex half-edge indices
    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    // Add all faces
    for (const auto& faceVerts : allFaces) {
        addFace(faceVerts);
    }

    // Link twin half-edges by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();

    // Select the top face (the original extruded face) - it's still at index 0
    // Actually, select all the original faces that were extruded
    for (uint32_t faceIdx : faceIndices) {
        if (faceIdx < m_faces.size()) {
            m_faces[faceIdx].selected = true;
        }
    }
}

void EditableMesh::extrudeSelectedFaces(float distance) {
    extrudeFaces(getSelectedFaces(), distance);
}

void EditableMesh::extrudeEdges(const std::vector<uint32_t>& halfEdgeIndices, float distance) {
    if (halfEdgeIndices.empty()) return;

    // Collect unique edge vertex pairs with their bordering face normal
    struct EdgeInfo {
        uint32_t v0, v1;
        glm::vec3 faceNormal; // normal of the face this edge borders
    };
    std::vector<EdgeInfo> edges;
    std::set<uint64_t> seen;
    std::set<uint32_t> allVertIndices;

    for (uint32_t heIdx : halfEdgeIndices) {
        if (heIdx >= m_halfEdges.size()) continue;
        auto [v0, v1] = getEdgeVertices(heIdx);
        uint64_t key = makeEdgeKey(v0, v1);
        if (seen.count(key)) continue;
        seen.insert(key);

        // Get the face normal from the half-edge's face (the bordering face)
        glm::vec3 fn(0, 1, 0);
        if (m_halfEdges[heIdx].faceIndex != UINT32_MAX) {
            fn = getFaceNormal(m_halfEdges[heIdx].faceIndex);
        } else {
            uint32_t twin = m_halfEdges[heIdx].twinIndex;
            if (twin != UINT32_MAX && m_halfEdges[twin].faceIndex != UINT32_MAX) {
                fn = getFaceNormal(m_halfEdges[twin].faceIndex);
            }
        }

        edges.push_back({v0, v1, fn});
        allVertIndices.insert(v0);
        allVertIndices.insert(v1);
    }

    if (edges.empty()) return;

    // Compute per-vertex extrude direction: in the face plane, perpendicular to the edge,
    // pointing away from the bordering face center
    std::map<uint32_t, glm::vec3> vertexDir;
    for (uint32_t vi : allVertIndices) {
        vertexDir[vi] = glm::vec3(0.0f);
    }

    for (const auto& e : edges) {
        glm::vec3 edgeDir = glm::normalize(m_vertices[e.v1].position - m_vertices[e.v0].position);
        // Cross edge with face normal → vector in face plane, perpendicular to edge
        glm::vec3 outward = glm::normalize(glm::cross(edgeDir, e.faceNormal));

        // Make sure it points away from the face center
        glm::vec3 edgeMid = (m_vertices[e.v0].position + m_vertices[e.v1].position) * 0.5f;

        // Find the bordering face center to determine "away" direction
        // Use the half-edge to get the face
        uint64_t key = makeEdgeKey(e.v0, e.v1);
        auto mapIt = m_edgeMap.find(key);
        if (mapIt != m_edgeMap.end()) {
            uint32_t he = mapIt->second;
            uint32_t faceIdx = m_halfEdges[he].faceIndex;
            if (faceIdx == UINT32_MAX) {
                uint32_t twin = m_halfEdges[he].twinIndex;
                if (twin != UINT32_MAX) faceIdx = m_halfEdges[twin].faceIndex;
            }
            if (faceIdx != UINT32_MAX) {
                glm::vec3 fc = getFaceCenter(faceIdx);
                // Flip if outward points toward face center
                if (glm::dot(outward, edgeMid - fc) < 0.0f) {
                    outward = -outward;
                }
            }
        }

        vertexDir[e.v0] += outward;
        vertexDir[e.v1] += outward;
    }

    // Normalize directions
    for (auto& [vi, dir] : vertexDir) {
        if (glm::length(dir) > 0.0001f) {
            dir = glm::normalize(dir);
        } else {
            dir = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Create new vertices at extruded positions
    std::map<uint32_t, uint32_t> oldToNew; // original vert index → new extruded vert index
    for (uint32_t vi : allVertIndices) {
        HEVertex newVert = m_vertices[vi];
        newVert.position = m_vertices[vi].position + vertexDir[vi] * distance;
        newVert.halfEdgeIndex = UINT32_MAX;
        newVert.selected = false;
        uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
        oldToNew[vi] = newIdx;
        m_vertices.push_back(newVert);
    }

    // Collect all existing faces
    std::vector<std::vector<uint32_t>> allFaces;
    for (uint32_t fi = 0; fi < static_cast<uint32_t>(m_faces.size()); fi++) {
        allFaces.push_back(getFaceVertices(fi));
    }

    // Create quad faces connecting original edges to extruded edges
    // Winding is chosen so the new quad's normal matches the bordering face's normal
    for (const auto& e : edges) {
        uint32_t nv0 = oldToNew[e.v0];
        uint32_t nv1 = oldToNew[e.v1];

        glm::vec3 p0 = m_vertices[e.v0].position;
        glm::vec3 p1 = m_vertices[e.v1].position;
        glm::vec3 np0 = m_vertices[nv0].position;

        // Check which winding produces a normal aligned with the bordering face
        glm::vec3 candidateNormal = glm::cross(p1 - p0, np0 - p0);

        if (glm::dot(candidateNormal, e.faceNormal) >= 0.0f) {
            allFaces.push_back({e.v0, e.v1, nv1, nv0});
        } else {
            allFaces.push_back({e.v0, nv0, nv1, e.v1});
        }
    }

    // Rebuild half-edge structure
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    for (const auto& fv : allFaces) {
        addFace(fv);
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();

    // Select the new extruded edges
    for (const auto& e : edges) {
        uint32_t nv0 = oldToNew[e.v0];
        uint32_t nv1 = oldToNew[e.v1];
        uint64_t key = makeEdgeKey(nv0, nv1);
        auto it = m_edgeMap.find(key);
        if (it != m_edgeMap.end()) {
            m_selectedEdges.insert(it->second);
            uint32_t twin = m_halfEdges[it->second].twinIndex;
            if (twin != UINT32_MAX) m_selectedEdges.insert(twin);
        }
    }
}

void EditableMesh::extrudeSelectedEdges(float distance) {
    extrudeEdges(getSelectedEdges(), distance);
}

void EditableMesh::insetSelectedFaces(float amount) {
    auto selectedFaces = getSelectedFaces();
    if (selectedFaces.empty()) return;

    // Clamp amount to valid range
    amount = std::clamp(amount, 0.01f, 0.99f);

    // Process each selected face
    std::vector<uint32_t> facesToDelete;
    std::vector<std::vector<uint32_t>> newQuadsToCreate;  // Store vertex indices for new quads

    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);

        // Only works on quads for now
        if (verts.size() != 4) {
            std::cout << "Inset: Skipping non-quad face " << faceIdx << " (has " << verts.size() << " verts)" << std::endl;
            continue;
        }

        // Get the 4 corner vertices
        uint32_t v0 = verts[0], v1 = verts[1], v2 = verts[2], v3 = verts[3];
        glm::vec3 p0 = m_vertices[v0].position;
        glm::vec3 p1 = m_vertices[v1].position;
        glm::vec3 p2 = m_vertices[v2].position;
        glm::vec3 p3 = m_vertices[v3].position;

        // Calculate face center
        glm::vec3 center = (p0 + p1 + p2 + p3) * 0.25f;

        // Create 4 new inset vertices (moved toward center by amount)
        uint32_t nv0 = static_cast<uint32_t>(m_vertices.size());
        uint32_t nv1 = nv0 + 1;
        uint32_t nv2 = nv0 + 2;
        uint32_t nv3 = nv0 + 3;

        // Calculate center UV for interpolation
        glm::vec2 centerUV = (m_vertices[v0].uv + m_vertices[v1].uv + m_vertices[v2].uv + m_vertices[v3].uv) * 0.25f;

        // Create 4 new inset vertices, copying color/normal from originals
        HEVertex newVert;
        newVert.halfEdgeIndex = UINT32_MAX;
        newVert.selected = false;

        // Vertex 0
        newVert.position = glm::mix(p0, center, amount);
        newVert.uv = glm::mix(m_vertices[v0].uv, centerUV, amount);
        newVert.normal = m_vertices[v0].normal;
        newVert.color = m_vertices[v0].color;
        m_vertices.push_back(newVert);

        // Vertex 1
        newVert.position = glm::mix(p1, center, amount);
        newVert.uv = glm::mix(m_vertices[v1].uv, centerUV, amount);
        newVert.normal = m_vertices[v1].normal;
        newVert.color = m_vertices[v1].color;
        m_vertices.push_back(newVert);

        // Vertex 2
        newVert.position = glm::mix(p2, center, amount);
        newVert.uv = glm::mix(m_vertices[v2].uv, centerUV, amount);
        newVert.normal = m_vertices[v2].normal;
        newVert.color = m_vertices[v2].color;
        m_vertices.push_back(newVert);

        // Vertex 3
        newVert.position = glm::mix(p3, center, amount);
        newVert.uv = glm::mix(m_vertices[v3].uv, centerUV, amount);
        newVert.normal = m_vertices[v3].normal;
        newVert.color = m_vertices[v3].color;
        m_vertices.push_back(newVert);

        // Mark original face for deletion
        facesToDelete.push_back(faceIdx);

        // Queue new quads to create:
        // Inner quad (the inset face) - same winding as original
        newQuadsToCreate.push_back({nv0, nv1, nv2, nv3});

        // 4 border quads connecting outer to inner
        // Each connects: outer edge to corresponding inner edge
        // Winding: outer0, outer1, inner1, inner0 (to match original face normal)
        newQuadsToCreate.push_back({v0, v1, nv1, nv0});  // Edge 0-1
        newQuadsToCreate.push_back({v1, v2, nv2, nv1});  // Edge 1-2
        newQuadsToCreate.push_back({v2, v3, nv3, nv2});  // Edge 2-3
        newQuadsToCreate.push_back({v3, v0, nv0, nv3});  // Edge 3-0
    }

    // Delete original faces first
    if (!facesToDelete.empty()) {
        // Sort descending so we delete from end first (preserves indices)
        std::sort(facesToDelete.begin(), facesToDelete.end(), std::greater<uint32_t>());

        for (uint32_t faceIdx : facesToDelete) {
            // Simple removal - mark half-edges as invalid
            if (faceIdx < m_faces.size()) {
                m_faces[faceIdx].vertexCount = 0;  // Mark as deleted
            }
        }
    }

    // Create new quads
    for (const auto& quad : newQuadsToCreate) {
        uint32_t newFaceIdx = static_cast<uint32_t>(m_faces.size());
        HEFace face;
        face.vertexCount = 4;
        face.selected = false;
        face.halfEdgeIndex = static_cast<uint32_t>(m_halfEdges.size());

        // Create half-edges for this quad (CCW winding)
        for (int i = 0; i < 4; ++i) {
            HalfEdge he;
            he.vertexIndex = quad[(i + 1) % 4];  // Points to next vertex
            he.faceIndex = newFaceIdx;
            he.nextIndex = face.halfEdgeIndex + ((i + 1) % 4);
            he.prevIndex = face.halfEdgeIndex + ((i + 3) % 4);
            he.twinIndex = UINT32_MAX;
            m_halfEdges.push_back(he);

            // Update vertex half-edge reference
            if (m_vertices[quad[i]].halfEdgeIndex == UINT32_MAX) {
                m_vertices[quad[i]].halfEdgeIndex = face.halfEdgeIndex + i;
            }
        }

        m_faces.push_back(face);
    }

    // Rebuild mesh structure (removes deleted faces, relinks twins)
    rebuildFromFaces();

    std::cout << "Inset: Created " << newQuadsToCreate.size() << " new quads from "
              << facesToDelete.size() << " selected faces" << std::endl;
}

void EditableMesh::deleteFaces(const std::vector<uint32_t>& faceIndices) {
    if (faceIndices.empty()) return;

    std::set<uint32_t> toDelete(faceIndices.begin(), faceIndices.end());

    // Mark faces for deletion
    std::vector<bool> keepFace(m_faces.size(), true);
    for (uint32_t idx : toDelete) {
        if (idx < keepFace.size()) {
            keepFace[idx] = false;
        }
    }

    // Rebuild mesh without deleted faces
    std::vector<HEVertex> newVertices = m_vertices;
    std::vector<HEFace> newFaces;
    std::vector<HalfEdge> newHalfEdges;

    // Reset vertex half-edge indices
    for (auto& v : newVertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (!keepFace[faceIdx]) continue;

        auto verts = getFaceVertices(faceIdx);

        uint32_t newFaceIdx = static_cast<uint32_t>(newFaces.size());
        HEFace face;
        face.vertexCount = static_cast<uint32_t>(verts.size());
        face.selected = false;
        face.halfEdgeIndex = static_cast<uint32_t>(newHalfEdges.size());

        for (size_t i = 0; i < verts.size(); ++i) {
            HalfEdge he;
            he.vertexIndex = verts[(i + 1) % verts.size()];
            he.faceIndex = newFaceIdx;
            he.nextIndex = face.halfEdgeIndex + ((i + 1) % verts.size());
            he.prevIndex = face.halfEdgeIndex + ((i + verts.size() - 1) % verts.size());
            he.twinIndex = UINT32_MAX;
            newHalfEdges.push_back(he);

            if (newVertices[verts[i]].halfEdgeIndex == UINT32_MAX) {
                newVertices[verts[i]].halfEdgeIndex = face.halfEdgeIndex + i;
            }
        }

        newFaces.push_back(face);
    }

    // Replace data
    m_vertices = std::move(newVertices);
    m_faces = std::move(newFaces);
    m_halfEdges = std::move(newHalfEdges);

    // Relink twins by position (handles duplicate vertices at same position)
    linkTwinsByPosition();

    // Remove orphaned vertices that no longer belong to any face
    removeOrphanedVertices();

    rebuildEdgeMap();
    clearSelection();
}

void EditableMesh::deleteSelectedFaces() {
    deleteFaces(getSelectedFaces());
}

void EditableMesh::removeOrphanedVertices() {
    if (m_vertices.empty()) return;

    // Find which vertices are actually used by half-edges
    std::vector<bool> used(m_vertices.size(), false);
    for (const auto& he : m_halfEdges) {
        if (he.vertexIndex < m_vertices.size()) {
            used[he.vertexIndex] = true;
        }
    }
    // Also mark vertices referenced as the "from" vertex of each half-edge
    // (the prev half-edge's vertexIndex is the "from" of current half-edge,
    //  but we also need to check via face vertex traversal)
    for (uint32_t fi = 0; fi < m_faces.size(); ++fi) {
        auto verts = getFaceVertices(fi);
        for (uint32_t vi : verts) {
            if (vi < m_vertices.size()) used[vi] = true;
        }
    }

    // Build remap table: old index -> new index
    std::vector<uint32_t> remap(m_vertices.size(), UINT32_MAX);
    std::vector<HEVertex> compacted;
    compacted.reserve(m_vertices.size());
    for (uint32_t i = 0; i < m_vertices.size(); ++i) {
        if (used[i]) {
            remap[i] = static_cast<uint32_t>(compacted.size());
            compacted.push_back(m_vertices[i]);
        }
    }

    if (compacted.size() == m_vertices.size()) return; // nothing to remove

    uint32_t removed = static_cast<uint32_t>(m_vertices.size() - compacted.size());

    // Remap all half-edge vertex indices
    for (auto& he : m_halfEdges) {
        if (he.vertexIndex < remap.size()) {
            he.vertexIndex = remap[he.vertexIndex];
        }
    }

    // Remap control points
    std::vector<ControlPoint> newControlPoints;
    for (auto& cp : m_controlPoints) {
        if (cp.vertexIndex < remap.size() && remap[cp.vertexIndex] != UINT32_MAX) {
            newControlPoints.push_back({remap[cp.vertexIndex], cp.name});
        }
    }
    m_controlPoints = std::move(newControlPoints);

    // Remap vertex halfEdgeIndex references (already valid since half-edge array didn't change)
    m_vertices = std::move(compacted);

    std::cout << "Removed " << removed << " orphaned vertices" << std::endl;
}

void EditableMesh::addControlPoint(uint32_t vertexIndex, const std::string& name) {
    if (vertexIndex >= m_vertices.size()) return;
    if (!isControlPoint(vertexIndex)) {
        m_controlPoints.push_back({vertexIndex, name});
        std::cout << "Added control point '" << name << "' at vertex " << vertexIndex << " (total: " << m_controlPoints.size() << ")" << std::endl;
    }
}

void EditableMesh::removeControlPoint(uint32_t vertexIndex) {
    auto it = std::find_if(m_controlPoints.begin(), m_controlPoints.end(),
        [vertexIndex](const ControlPoint& cp) { return cp.vertexIndex == vertexIndex; });
    if (it != m_controlPoints.end()) {
        std::cout << "Removed control point '" << it->name << "' at vertex " << vertexIndex << " (total: " << m_controlPoints.size() - 1 << ")" << std::endl;
        m_controlPoints.erase(it);
    }
}

void EditableMesh::clearControlPoints() {
    m_controlPoints.clear();
}

bool EditableMesh::isControlPoint(uint32_t vertexIndex) const {
    return std::find_if(m_controlPoints.begin(), m_controlPoints.end(),
        [vertexIndex](const ControlPoint& cp) { return cp.vertexIndex == vertexIndex; }) != m_controlPoints.end();
}

std::string EditableMesh::getControlPointName(uint32_t vertexIndex) const {
    auto it = std::find_if(m_controlPoints.begin(), m_controlPoints.end(),
        [vertexIndex](const ControlPoint& cp) { return cp.vertexIndex == vertexIndex; });
    return (it != m_controlPoints.end()) ? it->name : "";
}

// ── Port methods ───────────────────────────────────────

void EditableMesh::addPort(const Port& port) {
    m_ports.push_back(port);
}

void EditableMesh::removePort(size_t index) {
    if (index < m_ports.size()) {
        m_ports.erase(m_ports.begin() + index);
    }
}

void EditableMesh::clearPorts() {
    m_ports.clear();
}

// ── Mirror ─────────────────────────────────────────────

void EditableMesh::mirrorMergeX(float weldThreshold) {
    if (m_vertices.empty() || m_faces.empty()) return;

    uint32_t origVertCount = static_cast<uint32_t>(m_vertices.size());
    uint32_t origFaceCount = static_cast<uint32_t>(m_faces.size());

    // Step 1: Duplicate all vertices with X flipped
    std::vector<uint32_t> vertRemap(origVertCount); // old index -> new mirrored index
    for (uint32_t i = 0; i < origVertCount; ++i) {
        HEVertex mirrored = m_vertices[i];
        mirrored.position.x = -mirrored.position.x;
        mirrored.normal.x = -mirrored.normal.x;
        mirrored.halfEdgeIndex = UINT32_MAX; // will be set during face creation
        mirrored.selected = false;
        vertRemap[i] = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back(mirrored);
    }

    // Step 2: Duplicate all faces with reversed winding (using mirrored vertices)
    for (uint32_t fi = 0; fi < origFaceCount; ++fi) {
        auto verts = getFaceVertices(fi);
        // Reverse winding for mirrored faces
        std::reverse(verts.begin(), verts.end());

        uint32_t newFaceIdx = static_cast<uint32_t>(m_faces.size());
        HEFace face;
        face.vertexCount = static_cast<uint32_t>(verts.size());
        face.selected = false;
        face.halfEdgeIndex = static_cast<uint32_t>(m_halfEdges.size());

        for (size_t i = 0; i < verts.size(); ++i) {
            HalfEdge he;
            he.vertexIndex = vertRemap[verts[(i + 1) % verts.size()]];
            he.faceIndex = newFaceIdx;
            he.nextIndex = face.halfEdgeIndex + ((i + 1) % verts.size());
            he.prevIndex = face.halfEdgeIndex + ((i + verts.size() - 1) % verts.size());
            he.twinIndex = UINT32_MAX;
            m_halfEdges.push_back(he);

            uint32_t mirroredVert = vertRemap[verts[i]];
            if (m_vertices[mirroredVert].halfEdgeIndex == UINT32_MAX) {
                m_vertices[mirroredVert].halfEdgeIndex = face.halfEdgeIndex + i;
            }
        }

        m_faces.push_back(face);
    }

    // Step 3: Weld seam vertices along X=0
    // For each original vertex near X=0, merge it with its mirrored duplicate
    float threshold2 = weldThreshold * weldThreshold;
    std::map<uint32_t, uint32_t> weldMap; // mirrored index -> original index
    for (uint32_t i = 0; i < origVertCount; ++i) {
        // Check if vertex is on the seam (X ≈ 0)
        if (std::abs(m_vertices[i].position.x) <= weldThreshold) {
            uint32_t mirroredIdx = vertRemap[i];
            weldMap[mirroredIdx] = i;
            // Snap original vertex exactly to X=0
            m_vertices[i].position.x = 0.0f;
        }
    }

    if (!weldMap.empty()) {
        // Remap half-edges from mirrored seam verts to original seam verts
        for (auto& he : m_halfEdges) {
            auto it = weldMap.find(he.vertexIndex);
            if (it != weldMap.end()) {
                he.vertexIndex = it->second;
            }
        }
    }

    // Step 4: Relink twins and clean up
    linkTwinsByPosition();
    removeOrphanedVertices();
    rebuildEdgeMap();
    clearSelection();

    std::cout << "Mirror Merge X: " << origVertCount << " -> " << m_vertices.size()
              << " vertices, " << origFaceCount << " -> " << m_faces.size() << " faces"
              << " (welded " << weldMap.size() << " seam vertices)" << std::endl;
}

void EditableMesh::hollow(float thickness) {
    if (m_vertices.empty() || m_faces.empty()) return;
    if (thickness <= 0.0f) return;

    // Hollow operation for buildings:
    // - Original mesh becomes the OUTER shell (stays in place, normals outward)
    // - Create INNER shell by shrinking vertices INWARD (normals point inward)
    // - Connect at boundary edges
    //
    // Key: Group vertices by POSITION to handle meshes with duplicated vertices
    // (like cubes with hard normals where each face has its own vertices)

    // Step 1: Group vertices by position and calculate averaged normals per position
    const float posEpsilon = 0.0001f;
    std::map<std::tuple<int, int, int>, std::vector<uint32_t>> positionGroups;
    std::map<std::tuple<int, int, int>, glm::vec3> positionNormals;

    auto quantize = [posEpsilon](const glm::vec3& p) {
        return std::make_tuple(
            static_cast<int>(std::round(p.x / posEpsilon)),
            static_cast<int>(std::round(p.y / posEpsilon)),
            static_cast<int>(std::round(p.z / posEpsilon))
        );
    };

    // First pass: group vertices and accumulate face normals per position
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        glm::vec3 faceNormal = getFaceNormal(faceIdx);
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            auto key = quantize(m_vertices[v].position);
            positionGroups[key].push_back(v);
            positionNormals[key] += faceNormal;
        }
    }

    // Normalize the accumulated normals
    for (auto& [key, normal] : positionNormals) {
        float len = glm::length(normal);
        if (len > 0.0001f) {
            normal = normal / len;
        }
    }

    // Step 2: Collect original faces and boundary edges
    std::vector<std::vector<uint32_t>> originalFaces;
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> boundaryEdges;

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        auto verts = getFaceVertices(faceIdx);
        originalFaces.push_back(verts);

        auto edges = getFaceEdges(faceIdx);
        for (size_t i = 0; i < edges.size(); ++i) {
            uint32_t he = edges[i];
            if (m_halfEdges[he].twinIndex == UINT32_MAX) {
                uint32_t v0 = verts[i];
                uint32_t v1 = verts[(i + 1) % verts.size()];
                uint64_t key = makeEdgeKey(v0, v1);
                boundaryEdges[key] = {v0, v1};
            }
        }
    }

    // Step 3: Create INNER vertices by offsetting INWARD along averaged position normals
    uint32_t originalVertCount = static_cast<uint32_t>(m_vertices.size());
    std::map<uint32_t, uint32_t> outerToInner;  // Map outer (original) vertex -> new inner vertex

    for (uint32_t i = 0; i < originalVertCount; ++i) {
        auto key = quantize(m_vertices[i].position);
        glm::vec3 avgNormal = positionNormals[key];

        HEVertex innerVert = m_vertices[i];
        // Offset INWARD (opposite of normal direction)
        innerVert.position = m_vertices[i].position - avgNormal * thickness;
        // Inner normals will point inward (will be set by face winding)
        innerVert.normal = -avgNormal;
        innerVert.halfEdgeIndex = UINT32_MAX;
        innerVert.selected = false;

        uint32_t innerIdx = static_cast<uint32_t>(m_vertices.size());
        outerToInner[i] = innerIdx;
        m_vertices.push_back(innerVert);
    }

    // Step 4: Create OUTER faces (original faces, keep as-is, normals point outward)
    std::vector<std::vector<uint32_t>> outerFaces = originalFaces;

    // Step 5: Create INNER faces with REVERSED winding (normals point inward)
    std::vector<std::vector<uint32_t>> innerFaces;
    for (const auto& origFace : originalFaces) {
        std::vector<uint32_t> innerFace;
        // Reverse winding and use inner vertices
        for (int i = static_cast<int>(origFace.size()) - 1; i >= 0; --i) {
            innerFace.push_back(outerToInner[origFace[i]]);
        }
        innerFaces.push_back(innerFace);
    }

    // Step 6: Create connecting faces for boundary edges (the "rim" or "thickness edge")
    std::vector<std::vector<uint32_t>> connectingFaces;
    for (const auto& [key, edge] : boundaryEdges) {
        uint32_t outer0 = edge.first;
        uint32_t outer1 = edge.second;
        uint32_t inner0 = outerToInner[outer0];
        uint32_t inner1 = outerToInner[outer1];

        // Create a quad connecting outer and inner edges
        // Outer edge goes v0->v1 (CCW from outside), so rim should be: outer0, inner0, inner1, outer1
        connectingFaces.push_back({outer0, inner0, inner1, outer1});
    }

    // Step 7: Rebuild the mesh with all faces
    std::vector<std::vector<uint32_t>> allFaces;

    // Add outer faces (original - face outward)
    for (const auto& face : outerFaces) {
        allFaces.push_back(face);
    }

    // Add inner faces (shrunk shell - face inward)
    for (const auto& face : innerFaces) {
        allFaces.push_back(face);
    }

    // Add connecting rim faces
    for (const auto& face : connectingFaces) {
        allFaces.push_back(face);
    }

    // Clear and rebuild half-edge structure
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    // Reset vertex half-edge indices
    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    // Add all faces
    for (const auto& faceVerts : allFaces) {
        addFace(faceVerts);
    }

    // Link twin half-edges by position
    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();
}

void EditableMesh::booleanCut(const glm::vec3& cutterMin, const glm::vec3& cutterMax) {
    if (m_vertices.empty() || m_faces.empty()) return;

    // Boolean cut for axis-aligned boxes cutting through flat walls.
    // For each axis-aligned face that the cutter box passes through:
    // 1. Find the intersection rectangle on the face
    // 2. Subdivide the face into frame pieces around the hole
    // 3. Track cut holes for later connecting with frame faces

    const float epsilon = 0.0001f;

    // Structure to track a cut hole for frame generation
    struct CutHole {
        int axis;           // 0=X, 1=Y, 2=Z - which axis the face normal is along
        float planePos;     // Position along that axis
        bool positive;      // true if normal points in positive direction
        glm::vec2 holeMin;  // Min corner of hole in the 2D plane
        glm::vec2 holeMax;  // Max corner of hole in the 2D plane
        std::array<uint32_t, 4> holeVerts;  // The 4 vertices of the hole (for frame connection)
    };
    std::vector<CutHole> cutHoles;

    // Collect all faces and their data before modification
    struct FaceData {
        uint32_t faceIdx;
        std::vector<uint32_t> verts;
        glm::vec3 normal;
        glm::vec3 center;
    };
    std::vector<FaceData> facesToProcess;

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        FaceData fd;
        fd.faceIdx = faceIdx;
        fd.verts = getFaceVertices(faceIdx);
        fd.normal = getFaceNormal(faceIdx);
        fd.center = getFaceCenter(faceIdx);
        facesToProcess.push_back(fd);
    }

    // Determine which faces to cut and collect new faces
    std::vector<std::vector<uint32_t>> newFaces;
    std::set<uint32_t> facesToRemove;

    for (const auto& fd : facesToProcess) {
        // Check if face is axis-aligned
        int axis = -1;
        bool positive = false;
        if (std::abs(std::abs(fd.normal.x) - 1.0f) < 0.01f) {
            axis = 0;
            positive = fd.normal.x > 0;
        } else if (std::abs(std::abs(fd.normal.y) - 1.0f) < 0.01f) {
            axis = 1;
            positive = fd.normal.y > 0;
        } else if (std::abs(std::abs(fd.normal.z) - 1.0f) < 0.01f) {
            axis = 2;
            positive = fd.normal.z > 0;
        }

        if (axis == -1) continue;  // Not axis-aligned, skip

        // Get face plane position
        float planePos = (axis == 0) ? fd.center.x : (axis == 1) ? fd.center.y : fd.center.z;

        // Check if cutter box passes through this plane
        float cutMin = (axis == 0) ? cutterMin.x : (axis == 1) ? cutterMin.y : cutterMin.z;
        float cutMax = (axis == 0) ? cutterMax.x : (axis == 1) ? cutterMax.y : cutterMax.z;

        if (planePos < cutMin - epsilon || planePos > cutMax + epsilon) {
            continue;  // Plane doesn't intersect cutter
        }

        // Get the 2D bounds of the face and cutter in the plane
        // For axis=0 (X), plane coords are (Y, Z)
        // For axis=1 (Y), plane coords are (X, Z)
        // For axis=2 (Z), plane coords are (X, Y)
        auto get2D = [axis](const glm::vec3& p) -> glm::vec2 {
            if (axis == 0) return {p.y, p.z};
            if (axis == 1) return {p.x, p.z};
            return {p.x, p.y};
        };

        auto get3D = [axis, planePos](const glm::vec2& p) -> glm::vec3 {
            if (axis == 0) return {planePos, p.x, p.y};
            if (axis == 1) return {p.x, planePos, p.y};
            return {p.x, p.y, planePos};
        };

        // Get face bounds in 2D
        glm::vec2 faceMin(FLT_MAX), faceMax(-FLT_MAX);
        for (uint32_t v : fd.verts) {
            glm::vec2 p2d = get2D(m_vertices[v].position);
            faceMin = glm::min(faceMin, p2d);
            faceMax = glm::max(faceMax, p2d);
        }

        // Get cutter bounds in 2D
        glm::vec2 cutMin2D = get2D(cutterMin);
        glm::vec2 cutMax2D = get2D(cutterMax);
        // Ensure min < max
        glm::vec2 cutterMin2D = glm::min(cutMin2D, cutMax2D);
        glm::vec2 cutterMax2D = glm::max(cutMin2D, cutMax2D);

        // Compute intersection rectangle
        glm::vec2 holeMin = glm::max(faceMin, cutterMin2D);
        glm::vec2 holeMax = glm::min(faceMax, cutterMax2D);

        // Check if there's a valid intersection
        if (holeMin.x >= holeMax.x - epsilon || holeMin.y >= holeMax.y - epsilon) {
            continue;  // No intersection
        }

        // Shrink hole slightly to ensure it's inside the face
        holeMin += glm::vec2(epsilon);
        holeMax -= glm::vec2(epsilon);

        // Check if hole is meaningfully inside the face
        if (holeMin.x <= faceMin.x + epsilon || holeMax.x >= faceMax.x - epsilon ||
            holeMin.y <= faceMin.y + epsilon || holeMax.y >= faceMax.y - epsilon) {
            continue;  // Hole touches or exceeds face boundary
        }

        // Mark this face for removal
        facesToRemove.insert(fd.faceIdx);

        // Get vertex properties from original face for new vertices
        HEVertex templateVert = m_vertices[fd.verts[0]];
        templateVert.halfEdgeIndex = UINT32_MAX;
        templateVert.selected = false;

        // Create 4 new vertices for the hole corners
        uint32_t holeV[4];  // BL, BR, TR, TL
        glm::vec2 holeCorners[4] = {
            {holeMin.x, holeMin.y},  // BL
            {holeMax.x, holeMin.y},  // BR
            {holeMax.x, holeMax.y},  // TR
            {holeMin.x, holeMax.y}   // TL
        };

        for (int i = 0; i < 4; ++i) {
            HEVertex v = templateVert;
            v.position = get3D(holeCorners[i]);
            v.normal = fd.normal;
            holeV[i] = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(v);
        }

        // Get the face corner vertices (assuming it's a quad)
        // We need to identify which corner is which
        // Find BL, BR, TR, TL corners of the original face
        uint32_t faceCorners[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};  // BL, BR, TR, TL

        for (uint32_t v : fd.verts) {
            glm::vec2 p = get2D(m_vertices[v].position);
            bool isLeft = (p.x - faceMin.x) < (faceMax.x - p.x);
            bool isBottom = (p.y - faceMin.y) < (faceMax.y - p.y);

            if (isLeft && isBottom) faceCorners[0] = v;       // BL
            else if (!isLeft && isBottom) faceCorners[1] = v; // BR
            else if (!isLeft && !isBottom) faceCorners[2] = v; // TR
            else faceCorners[3] = v;                           // TL
        }

        // Verify we found all corners
        bool valid = true;
        for (int i = 0; i < 4; ++i) {
            if (faceCorners[i] == UINT32_MAX) valid = false;
        }
        if (!valid) continue;

        // Create 4 frame faces around the hole
        // The winding order depends on whether the normal is positive or negative

        if (positive) {
            // Normal points in positive direction (e.g., +X, +Y, +Z)
            // CCW winding when looking from positive side
            // Bottom: faceCorners[0], faceCorners[1], holeV[1], holeV[0]
            newFaces.push_back({faceCorners[0], faceCorners[1], holeV[1], holeV[0]});
            // Right: faceCorners[1], faceCorners[2], holeV[2], holeV[1]
            newFaces.push_back({faceCorners[1], faceCorners[2], holeV[2], holeV[1]});
            // Top: faceCorners[2], faceCorners[3], holeV[3], holeV[2]
            newFaces.push_back({faceCorners[2], faceCorners[3], holeV[3], holeV[2]});
            // Left: faceCorners[3], faceCorners[0], holeV[0], holeV[3]
            newFaces.push_back({faceCorners[3], faceCorners[0], holeV[0], holeV[3]});
        } else {
            // Normal points in negative direction
            // Reverse winding
            newFaces.push_back({faceCorners[1], faceCorners[0], holeV[0], holeV[1]});
            newFaces.push_back({faceCorners[2], faceCorners[1], holeV[1], holeV[2]});
            newFaces.push_back({faceCorners[3], faceCorners[2], holeV[2], holeV[3]});
            newFaces.push_back({faceCorners[0], faceCorners[3], holeV[3], holeV[0]});
        }

        // Record this cut hole for frame generation
        CutHole hole;
        hole.axis = axis;
        hole.planePos = planePos;
        hole.positive = positive;
        hole.holeMin = holeMin;
        hole.holeMax = holeMax;
        hole.holeVerts = {holeV[0], holeV[1], holeV[2], holeV[3]};
        cutHoles.push_back(hole);
    }

    // Find pairs of cut holes and create connecting frame faces
    for (size_t i = 0; i < cutHoles.size(); ++i) {
        for (size_t j = i + 1; j < cutHoles.size(); ++j) {
            const auto& h1 = cutHoles[i];
            const auto& h2 = cutHoles[j];

            // Must be same axis, opposite directions
            if (h1.axis != h2.axis) continue;
            if (h1.positive == h2.positive) continue;

            // Must have overlapping holes
            glm::vec2 overlapMin = glm::max(h1.holeMin, h2.holeMin);
            glm::vec2 overlapMax = glm::min(h1.holeMax, h2.holeMax);

            if (overlapMin.x >= overlapMax.x - epsilon || overlapMin.y >= overlapMax.y - epsilon) {
                continue;  // No overlap
            }

            // Create 4 connecting faces (the frame/jamb)
            // h1.holeVerts: BL, BR, TR, TL
            // h2.holeVerts: BL, BR, TR, TL
            // We need to connect corresponding edges

            const std::array<uint32_t, 4>& v1 = h1.holeVerts;  // Front hole
            const std::array<uint32_t, 4>& v2 = h2.holeVerts;  // Back hole

            // The frame faces connect front hole to back hole
            // These faces should face INTO the opening (visible from inside the doorway)
            // Winding depends on which is positive/negative
            if (h1.positive) {
                // h1 is the positive (outer) face, h2 is negative (inner)
                // Normals should face into the opening
                newFaces.push_back(std::vector<uint32_t>{v1[1], v2[1], v2[0], v1[0]});  // Bottom
                newFaces.push_back(std::vector<uint32_t>{v1[2], v2[2], v2[1], v1[1]});  // Right
                newFaces.push_back(std::vector<uint32_t>{v1[3], v2[3], v2[2], v1[2]});  // Top
                newFaces.push_back(std::vector<uint32_t>{v1[0], v2[0], v2[3], v1[3]});  // Left
            } else {
                // h1 is negative, h2 is positive
                newFaces.push_back(std::vector<uint32_t>{v2[1], v1[1], v1[0], v2[0]});  // Bottom
                newFaces.push_back(std::vector<uint32_t>{v2[2], v1[2], v1[1], v2[1]});  // Right
                newFaces.push_back(std::vector<uint32_t>{v2[3], v1[3], v1[2], v2[2]});  // Top
                newFaces.push_back(std::vector<uint32_t>{v2[0], v1[0], v1[3], v2[3]});  // Left
            }
        }
    }

    if (facesToRemove.empty()) {
        return;  // No cuts made
    }

    // Collect faces to keep
    std::vector<std::vector<uint32_t>> keptFaces;
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (facesToRemove.find(faceIdx) == facesToRemove.end()) {
            keptFaces.push_back(getFaceVertices(faceIdx));
        }
    }

    // Rebuild mesh with kept faces + new faces
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    for (const auto& face : keptFaces) {
        addFace(face);
    }
    for (const auto& face : newFaces) {
        addFace(face);
    }

    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();
}

bool EditableMesh::bridgeEdges(uint32_t heIdx1, uint32_t heIdx2, int segments) {
    if (heIdx1 >= m_halfEdges.size() || heIdx2 >= m_halfEdges.size()) return false;
    if (heIdx1 == heIdx2) return false;
    if (segments < 1) segments = 1;

    // Get the two vertices of each edge
    auto [v0a, v0b] = getEdgeVertices(heIdx1);
    auto [v1a, v1b] = getEdgeVertices(heIdx2);

    // Make sure these are different vertices
    if (v0a == v1a || v0a == v1b || v0b == v1a || v0b == v1b) {
        return false;  // Edges share a vertex, can't bridge
    }

    // Determine the best vertex pairing to minimize twisting
    // Compare distances: (v0a-v1a + v0b-v1b) vs (v0a-v1b + v0b-v1a)
    float dist1 = glm::distance(m_vertices[v0a].position, m_vertices[v1a].position) +
                  glm::distance(m_vertices[v0b].position, m_vertices[v1b].position);
    float dist2 = glm::distance(m_vertices[v0a].position, m_vertices[v1b].position) +
                  glm::distance(m_vertices[v0b].position, m_vertices[v1a].position);

    // Determine vertex pairing
    uint32_t startA, startB, endA, endB;
    if (dist1 <= dist2) {
        startA = v0a; startB = v0b;
        endA = v1a; endB = v1b;
    } else {
        startA = v0a; startB = v0b;
        endA = v1b; endB = v1a;
    }

    // Get positions for interpolation
    glm::vec3 posStartA = m_vertices[startA].position;
    glm::vec3 posStartB = m_vertices[startB].position;
    glm::vec3 posEndA = m_vertices[endA].position;
    glm::vec3 posEndB = m_vertices[endB].position;

    // Get UVs and colors for interpolation
    glm::vec2 uvStartA = m_vertices[startA].uv;
    glm::vec2 uvStartB = m_vertices[startB].uv;
    glm::vec2 uvEndA = m_vertices[endA].uv;
    glm::vec2 uvEndB = m_vertices[endB].uv;

    glm::vec4 colStartA = m_vertices[startA].color;
    glm::vec4 colStartB = m_vertices[startB].color;
    glm::vec4 colEndA = m_vertices[endA].color;
    glm::vec4 colEndB = m_vertices[endB].color;

    // Create intermediate vertices if segments > 1
    // We need (segments - 1) rows of intermediate vertex pairs
    std::vector<std::pair<uint32_t, uint32_t>> vertexRows;
    vertexRows.push_back({startA, startB});  // First row is the start edge

    for (int i = 1; i < segments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(segments);

        // Interpolate positions
        glm::vec3 posA = glm::mix(posStartA, posEndA, t);
        glm::vec3 posB = glm::mix(posStartB, posEndB, t);

        // Interpolate UVs
        glm::vec2 uvA = glm::mix(uvStartA, uvEndA, t);
        glm::vec2 uvB = glm::mix(uvStartB, uvEndB, t);

        // Interpolate colors
        glm::vec4 colA = glm::mix(colStartA, colEndA, t);
        glm::vec4 colB = glm::mix(colStartB, colEndB, t);

        // Create new vertices
        HEVertex vertA, vertB;
        vertA.position = posA;
        vertA.normal = glm::vec3(0, 1, 0);  // Will be recalculated
        vertA.uv = uvA;
        vertA.color = colA;
        vertA.halfEdgeIndex = UINT32_MAX;
        vertA.selected = false;

        vertB.position = posB;
        vertB.normal = glm::vec3(0, 1, 0);
        vertB.uv = uvB;
        vertB.color = colB;
        vertB.halfEdgeIndex = UINT32_MAX;
        vertB.selected = false;

        uint32_t idxA = addVertex(vertA);
        uint32_t idxB = addVertex(vertB);
        vertexRows.push_back({idxA, idxB});
    }

    vertexRows.push_back({endA, endB});  // Last row is the end edge

    // Create quads between each pair of rows
    // Winding order reversed to fix normals: {rowA, rowB, nextRowB, nextRowA}
    std::vector<uint32_t> newFaceIndices;
    for (size_t i = 0; i < vertexRows.size() - 1; ++i) {
        auto [currA, currB] = vertexRows[i];
        auto [nextA, nextB] = vertexRows[i + 1];

        // Reversed winding order for correct normals
        std::vector<uint32_t> quadVerts = {currA, nextA, nextB, currB};
        uint32_t newFaceIdx = addFace(quadVerts);
        newFaceIndices.push_back(newFaceIdx);
    }

    // Link all twins by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();

    // Select all new faces
    clearSelection();
    for (uint32_t faceIdx : newFaceIndices) {
        if (faceIdx < m_faces.size()) {
            m_faces[faceIdx].selected = true;
        }
    }

    return true;
}

void EditableMesh::flipSelectedNormals() {
    auto selectedFaces = getSelectedFaces();
    if (selectedFaces.empty()) return;

    // If only one face, just flip it
    if (selectedFaces.size() == 1) {
        uint32_t faceIdx = selectedFaces[0];
        auto verts = getFaceVertices(faceIdx);

        // Reverse the vertex order
        std::reverse(verts.begin(), verts.end());

        // We need to rebuild this face with reversed winding
        // Store all faces, modify this one, rebuild
        std::vector<std::vector<uint32_t>> allFaces;
        for (uint32_t i = 0; i < m_faces.size(); ++i) {
            if (i == faceIdx) {
                allFaces.push_back(verts);
            } else {
                allFaces.push_back(getFaceVertices(i));
            }
        }

        // Rebuild mesh
        m_halfEdges.clear();
        m_faces.clear();
        m_edgeMap.clear();
        m_selectedEdges.clear();

        for (auto& v : m_vertices) {
            v.halfEdgeIndex = UINT32_MAX;
        }

        for (const auto& faceVerts : allFaces) {
            addFace(faceVerts);
        }

        // Link twin half-edges by position (handles duplicate vertices at same position)
        linkTwinsByPosition();
        rebuildEdgeMap();
        recalculateNormals();

        // Re-select the face
        m_faces[faceIdx].selected = true;
        return;
    }

    // Multiple faces selected - find predominant direction and flip all to minority
    glm::vec3 avgNormal(0);
    std::vector<glm::vec3> faceNormals;

    for (uint32_t faceIdx : selectedFaces) {
        glm::vec3 normal = getFaceNormal(faceIdx);
        faceNormals.push_back(normal);
        avgNormal += normal;
    }

    if (glm::length(avgNormal) < 0.001f) {
        // Normals cancel out, just flip all
        avgNormal = faceNormals[0];
    } else {
        avgNormal = glm::normalize(avgNormal);
    }

    // Count faces aligned with average vs against
    int alignedCount = 0;
    int opposedCount = 0;

    for (const auto& normal : faceNormals) {
        if (glm::dot(normal, avgNormal) > 0) {
            alignedCount++;
        } else {
            opposedCount++;
        }
    }

    // Determine target direction (opposite of majority)
    glm::vec3 targetDir = (alignedCount >= opposedCount) ? -avgNormal : avgNormal;

    // Collect all faces, flipping selected ones that don't match target
    std::vector<std::vector<uint32_t>> allFaces;
    std::set<uint32_t> selectedSet(selectedFaces.begin(), selectedFaces.end());

    for (uint32_t i = 0; i < m_faces.size(); ++i) {
        auto verts = getFaceVertices(i);

        if (selectedSet.count(i) > 0) {
            // This face is selected - check if it needs flipping
            glm::vec3 normal = getFaceNormal(i);
            if (glm::dot(normal, targetDir) < 0) {
                // Needs flipping
                std::reverse(verts.begin(), verts.end());
            }
        }

        allFaces.push_back(verts);
    }

    // Rebuild mesh
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    for (const auto& faceVerts : allFaces) {
        addFace(faceVerts);
    }

    // Link twin half-edges by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();

    // Re-select the faces
    for (uint32_t faceIdx : selectedFaces) {
        if (faceIdx < m_faces.size()) {
            m_faces[faceIdx].selected = true;
        }
    }
}

void EditableMesh::catmullClarkSubdivide(int levels) {
    if (m_faces.empty() || m_vertices.empty()) return;
    levels = std::max(1, std::min(levels, 3));

    for (int level = 0; level < levels; ++level) {
        uint32_t origVertCount = static_cast<uint32_t>(m_vertices.size());
        uint32_t origFaceCount = static_cast<uint32_t>(m_faces.size());
        uint32_t origHECount = static_cast<uint32_t>(m_halfEdges.size());

        // === Step 1: Compute face points ===
        std::vector<uint32_t> facePointIdx(origFaceCount);
        for (uint32_t fi = 0; fi < origFaceCount; ++fi) {
            auto verts = getFaceVertices(fi);
            if (verts.empty()) continue;

            HEVertex fp{};
            fp.halfEdgeIndex = UINT32_MAX;
            float n = static_cast<float>(verts.size());
            for (uint32_t vi : verts) {
                const auto& v = m_vertices[vi];
                fp.position += v.position;
                fp.uv += v.uv;
                fp.color += v.color;
            }
            fp.position /= n;
            fp.uv /= n;
            fp.color /= n;
            facePointIdx[fi] = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(fp);
        }

        // === Step 2: Compute edge points ===
        // Use half-edge canonical index (min of he, twin) as edge key
        // This correctly handles split-vertex meshes where the same geometric edge
        // has different vertex indices on different faces.
        struct EdgeInfo {
            uint32_t he;           // one of the half-edges
            uint32_t face0, face1; // adjacent faces
            bool boundary;
        };
        std::unordered_map<uint32_t, EdgeInfo> edgeInfoMap;   // canonical he → info
        std::unordered_map<uint32_t, uint32_t> edgePointMap;  // canonical he → new vertex index

        for (uint32_t fi = 0; fi < origFaceCount; ++fi) {
            auto heEdges = getFaceEdges(fi);
            for (uint32_t he : heEdges) {
                uint32_t twin = m_halfEdges[he].twinIndex;
                uint32_t canonKey = (twin != UINT32_MAX) ? std::min(he, twin) : he;

                auto it = edgeInfoMap.find(canonKey);
                if (it == edgeInfoMap.end()) {
                    EdgeInfo ei;
                    ei.he = he;
                    ei.face0 = fi;
                    ei.face1 = UINT32_MAX;
                    ei.boundary = (twin == UINT32_MAX);
                    edgeInfoMap[canonKey] = ei;
                } else {
                    it->second.face1 = fi;
                    it->second.boundary = false;
                }
            }
        }

        for (auto& [canonKey, ei] : edgeInfoMap) {
            uint32_t fromV = m_halfEdges[m_halfEdges[ei.he].prevIndex].vertexIndex;
            uint32_t toV = m_halfEdges[ei.he].vertexIndex;
            const auto& v0 = m_vertices[fromV];
            const auto& v1 = m_vertices[toV];

            HEVertex ep{};
            ep.halfEdgeIndex = UINT32_MAX;

            if (ei.boundary || ei.face1 == UINT32_MAX) {
                ep.position = (v0.position + v1.position) * 0.5f;
                ep.uv = (v0.uv + v1.uv) * 0.5f;
                ep.color = (v0.color + v1.color) * 0.5f;
            } else {
                const auto& fp0 = m_vertices[facePointIdx[ei.face0]];
                const auto& fp1 = m_vertices[facePointIdx[ei.face1]];
                ep.position = (v0.position + v1.position + fp0.position + fp1.position) * 0.25f;
                ep.uv = (v0.uv + v1.uv + fp0.uv + fp1.uv) * 0.25f;
                ep.color = (v0.color + v1.color + fp0.color + fp1.color) * 0.25f;
            }
            edgePointMap[canonKey] = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(ep);
        }

        // === Step 3: Move original vertices ===
        std::vector<glm::vec3> newPositions(origVertCount);
        std::vector<glm::vec2> newUVs(origVertCount);
        std::vector<glm::vec4> newColors(origVertCount);

        for (uint32_t vi = 0; vi < origVertCount; ++vi) {
            auto adjFaces = getVertexFaces(vi);
            auto adjEdges = getVertexEdges(vi);
            const auto& P = m_vertices[vi];

            // Check if boundary vertex using half-edge twins
            bool isBoundary = false;
            std::vector<uint32_t> boundaryEdgeCanonKeys;
            for (uint32_t he : adjEdges) {
                if (m_halfEdges[he].twinIndex == UINT32_MAX) {
                    isBoundary = true;
                    boundaryEdgeCanonKeys.push_back(he); // no twin, he IS the canonical key
                }
            }

            if (isBoundary) {
                // Boundary vertex: average of adjacent boundary edge midpoints + original position
                // Rule: (M + P) / 2 where M = average of boundary edge midpoints
                glm::vec3 M(0);
                glm::vec2 Muv(0);
                glm::vec4 Mcolor(0);
                int count = 0;
                for (uint32_t ck : boundaryEdgeCanonKeys) {
                    uint32_t fromV = m_halfEdges[m_halfEdges[ck].prevIndex].vertexIndex;
                    uint32_t toV = m_halfEdges[ck].vertexIndex;
                    M += (m_vertices[fromV].position + m_vertices[toV].position) * 0.5f;
                    Muv += (m_vertices[fromV].uv + m_vertices[toV].uv) * 0.5f;
                    Mcolor += (m_vertices[fromV].color + m_vertices[toV].color) * 0.5f;
                    count++;
                }
                if (count >= 2) {
                    float w = 1.0f / static_cast<float>(count);
                    M *= w; Muv *= w; Mcolor *= w;
                    newPositions[vi] = (M + P.position) * 0.5f;
                    newUVs[vi] = (Muv + P.uv) * 0.5f;
                    newColors[vi] = (Mcolor + P.color) * 0.5f;
                } else {
                    newPositions[vi] = P.position;
                    newUVs[vi] = P.uv;
                    newColors[vi] = P.color;
                }
            } else {
                int n = static_cast<int>(adjFaces.size());
                if (n == 0) {
                    newPositions[vi] = P.position;
                    newUVs[vi] = P.uv;
                    newColors[vi] = P.color;
                    continue;
                }
                float fn = static_cast<float>(n);

                // Q = average of face points
                glm::vec3 Q(0);
                glm::vec2 Quv(0);
                glm::vec4 Qcolor(0);
                for (uint32_t fi : adjFaces) {
                    if (fi < origFaceCount) {
                        const auto& fp = m_vertices[facePointIdx[fi]];
                        Q += fp.position;
                        Quv += fp.uv;
                        Qcolor += fp.color;
                    }
                }
                Q /= fn;
                Quv /= fn;
                Qcolor /= fn;

                // R = average of edge midpoints (using topological neighbors)
                glm::vec3 R(0);
                glm::vec2 Ruv(0);
                glm::vec4 Rcolor(0);
                int edgeCount = 0;
                for (uint32_t he : adjEdges) {
                    uint32_t ni = m_halfEdges[he].vertexIndex;
                    if (ni < origVertCount) {
                        R += (P.position + m_vertices[ni].position) * 0.5f;
                        Ruv += (P.uv + m_vertices[ni].uv) * 0.5f;
                        Rcolor += (P.color + m_vertices[ni].color) * 0.5f;
                        edgeCount++;
                    }
                }
                if (edgeCount > 0) {
                    float ec = static_cast<float>(edgeCount);
                    R /= ec;
                    Ruv /= ec;
                    Rcolor /= ec;
                }

                // New position: (Q + 2R + (n-3)P) / n
                newPositions[vi] = (Q + 2.0f * R + (fn - 3.0f) * P.position) / fn;
                newUVs[vi] = (Quv + 2.0f * Ruv + (fn - 3.0f) * P.uv) / fn;
                newColors[vi] = (Qcolor + 2.0f * Rcolor + (fn - 3.0f) * P.color) / fn;
            }
        }

        // Apply moved positions to original vertices
        for (uint32_t vi = 0; vi < origVertCount; ++vi) {
            m_vertices[vi].position = newPositions[vi];
            m_vertices[vi].uv = newUVs[vi];
            m_vertices[vi].color = newColors[vi];
        }

        // === Step 4: Create new quads ===
        // Each original N-gon face produces N new quads
        // Use half-edge indices to look up edge points (not vertex-index pairs)
        std::vector<std::array<uint32_t, 4>> newQuads;

        for (uint32_t fi = 0; fi < origFaceCount; ++fi) {
            auto verts = getFaceVertices(fi);
            auto heEdges = getFaceEdges(fi);
            uint32_t nv = static_cast<uint32_t>(verts.size());
            uint32_t fpIdx = facePointIdx[fi];

            for (uint32_t i = 0; i < nv; ++i) {
                uint32_t curr = verts[i];
                uint32_t heNext = heEdges[i];                        // edge: curr → next
                uint32_t hePrev = heEdges[(i + nv - 1) % nv];        // edge: prev → curr

                // Canonical edge keys
                uint32_t twinNext = m_halfEdges[heNext].twinIndex;
                uint32_t canonNext = (twinNext != UINT32_MAX) ? std::min(heNext, twinNext) : heNext;

                uint32_t twinPrev = m_halfEdges[hePrev].twinIndex;
                uint32_t canonPrev = (twinPrev != UINT32_MAX) ? std::min(hePrev, twinPrev) : hePrev;

                uint32_t epNext = edgePointMap[canonNext];
                uint32_t epPrev = edgePointMap[canonPrev];

                // Quad: face_point, edge_point_prev, original_vertex, edge_point_next
                newQuads.push_back({fpIdx, epPrev, curr, epNext});
            }
        }

        // Rebuild mesh with new topology
        m_halfEdges.clear();
        m_faces.clear();
        m_edgeMap.clear();
        m_selectedEdges.clear();
        for (auto& v : m_vertices) {
            v.halfEdgeIndex = UINT32_MAX;
        }

        addQuadFacesBatch(newQuads);
        recalculateNormals();

        std::cout << "Catmull-Clark subdivision level " << (level + 1)
                  << ": " << m_vertices.size() << " verts, "
                  << m_faces.size() << " faces" << std::endl;
    }
}

void EditableMesh::mergeVertices(const std::vector<uint32_t>& vertIndices) {
    if (vertIndices.size() < 2) return;

    // Calculate average position
    glm::vec3 avgPos(0);
    glm::vec3 avgNormal(0);
    glm::vec2 avgUV(0);
    glm::vec4 avgColor(0);

    for (uint32_t v : vertIndices) {
        if (v < m_vertices.size()) {
            avgPos += m_vertices[v].position;
            avgNormal += m_vertices[v].normal;
            avgUV += m_vertices[v].uv;
            avgColor += m_vertices[v].color;
        }
    }

    float count = static_cast<float>(vertIndices.size());
    avgPos /= count;
    avgNormal = glm::normalize(avgNormal);
    avgUV /= count;
    avgColor /= count;

    // Keep the first vertex, remove others
    uint32_t keepVert = vertIndices[0];
    m_vertices[keepVert].position = avgPos;
    m_vertices[keepVert].normal = avgNormal;
    m_vertices[keepVert].uv = avgUV;
    m_vertices[keepVert].color = avgColor;

    // Create map from removed vertices to kept vertex
    std::map<uint32_t, uint32_t> remapVert;
    for (size_t i = 1; i < vertIndices.size(); ++i) {
        remapVert[vertIndices[i]] = keepVert;
    }

    // Update all half-edges to use the kept vertex
    for (auto& he : m_halfEdges) {
        auto it = remapVert.find(he.vertexIndex);
        if (it != remapVert.end()) {
            he.vertexIndex = it->second;
        }
    }

    // Remove degenerate faces (where two or more vertices are the same)
    std::vector<uint32_t> degenerateFaces;
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        auto verts = getFaceVertices(faceIdx);
        std::set<uint32_t> uniqueVerts(verts.begin(), verts.end());
        if (uniqueVerts.size() < 3) {
            degenerateFaces.push_back(faceIdx);
        }
    }

    if (!degenerateFaces.empty()) {
        deleteFaces(degenerateFaces);
    }

    rebuildEdgeMap();
}

void EditableMesh::mergeSelectedVertices() {
    mergeVertices(getSelectedVertices());
}

void EditableMesh::insertEdgeLoop(uint32_t heIdx, int count) {
    if (heIdx >= m_halfEdges.size()) return;
    if (count < 1) count = 1;

    // Helper to create a POSITION-based edge key (not index-based)
    // This handles meshes with duplicate vertices at same position
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    auto makePositionEdgeKey = [&](uint32_t v0, uint32_t v1) -> std::pair<uint64_t, uint64_t> {
        uint64_t p0 = posKey(m_vertices[v0].position);
        uint64_t p1 = posKey(m_vertices[v1].position);
        // Return ordered pair for consistent key
        if (p0 <= p1) return {p0, p1};
        return {p1, p0};
    };

    // Collect all edges to cut and faces to split by walking through quads
    // The edges to cut are the ones we traverse (entry and exit of each quad)
    struct LoopSegment {
        uint32_t faceIdx;
        uint32_t entryHE;   // Half-edge we enter the face through
        uint32_t exitHE;    // Half-edge we exit through (opposite in quad)
    };
    std::vector<LoopSegment> segments;
    std::set<uint32_t> visitedFaces;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> edgesToCut;  // Position-based edge key -> representative half-edge

    // Helper to add an edge to cut (using position-based key)
    auto addEdgeToCut = [&](uint32_t he) {
        auto [v0, v1] = getEdgeVertices(he);
        auto key = makePositionEdgeKey(v0, v1);
        if (edgesToCut.find(key) == edgesToCut.end()) {
            edgesToCut[key] = he;  // Store the half-edge for later vertex interpolation
        }
    };

    // Walk in one direction
    uint32_t current = heIdx;
    while (current != UINT32_MAX) {
        uint32_t faceIdx = m_halfEdges[current].faceIndex;
        if (faceIdx == UINT32_MAX) break;
        if (visitedFaces.count(faceIdx) > 0) break;  // Completed loop
        if (m_faces[faceIdx].vertexCount != 4) break;  // Stop at non-quads

        visitedFaces.insert(faceIdx);

        // Find opposite edge (exit edge)
        uint32_t exitHE = findNextLoopEdge(current);
        if (exitHE == UINT32_MAX) break;

        segments.push_back({faceIdx, current, exitHE});
        addEdgeToCut(current);
        addEdgeToCut(exitHE);

        // Move to next face via twin of exit edge
        current = m_halfEdges[exitHE].twinIndex;
    }

    // Walk in opposite direction from start
    uint32_t startTwin = m_halfEdges[heIdx].twinIndex;
    if (startTwin != UINT32_MAX) {
        current = startTwin;
        std::vector<LoopSegment> backSegments;

        while (current != UINT32_MAX) {
            uint32_t faceIdx = m_halfEdges[current].faceIndex;
            if (faceIdx == UINT32_MAX) break;
            if (visitedFaces.count(faceIdx) > 0) break;
            if (m_faces[faceIdx].vertexCount != 4) break;

            visitedFaces.insert(faceIdx);

            uint32_t exitHE = findNextLoopEdge(current);
            if (exitHE == UINT32_MAX) break;

            backSegments.push_back({faceIdx, current, exitHE});
            addEdgeToCut(current);
            addEdgeToCut(exitHE);

            current = m_halfEdges[exitHE].twinIndex;
        }

        // Prepend back segments
        std::reverse(backSegments.begin(), backSegments.end());
        backSegments.insert(backSegments.end(), segments.begin(), segments.end());
        segments = std::move(backSegments);
    }

    if (segments.empty()) return;

    // Create new vertices at evenly spaced positions along each edge
    // For count=1: position at 0.5
    // For count=2: positions at 0.333 and 0.666
    // For count=N: positions at 1/(N+1), 2/(N+1), ..., N/(N+1)
    // Map from position-based edge key to array of new vertex indices
    std::map<std::pair<uint64_t, uint64_t>, std::vector<uint32_t>> edgeToNewVerts;

    for (const auto& [edgeKey, he] : edgesToCut) {
        auto [v0, v1] = getEdgeVertices(he);
        std::vector<uint32_t> newVertIndices;

        for (int i = 1; i <= count; ++i) {
            float t = static_cast<float>(i) / (count + 1);

            HEVertex newVert;
            newVert.position = glm::mix(m_vertices[v0].position, m_vertices[v1].position, t);
            newVert.normal = glm::normalize(m_vertices[v0].normal + m_vertices[v1].normal);
            newVert.uv = glm::mix(m_vertices[v0].uv, m_vertices[v1].uv, t);
            newVert.color = glm::mix(m_vertices[v0].color, m_vertices[v1].color, t);
            newVert.halfEdgeIndex = UINT32_MAX;
            newVert.selected = false;

            uint32_t newVertIdx = static_cast<uint32_t>(m_vertices.size());
            newVertIndices.push_back(newVertIdx);
            m_vertices.push_back(newVert);
        }

        edgeToNewVerts[edgeKey] = newVertIndices;
    }

    // Rebuild faces - split each quad in the loop into two quads
    std::vector<std::vector<uint32_t>> allFaces;
    std::set<uint32_t> splitFaces;
    for (const auto& seg : segments) {
        splitFaces.insert(seg.faceIdx);
    }

    // Keep non-split faces as-is
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (splitFaces.count(faceIdx) == 0) {
            allFaces.push_back(getFaceVertices(faceIdx));
        }
    }

    // Split faces in the loop
    for (const auto& seg : segments) {
        auto verts = getFaceVertices(seg.faceIdx);
        if (verts.size() != 4) continue;

        // Find which edge indices correspond to entry and exit
        // Entry edge goes from verts[i] to verts[i+1] for some i
        auto [entryV0, entryV1] = getEdgeVertices(seg.entryHE);
        auto [exitV0, exitV1] = getEdgeVertices(seg.exitHE);

        // Find entry edge position in vertex list
        int entryIdx = -1;
        for (int i = 0; i < 4; ++i) {
            if ((verts[i] == entryV0 && verts[(i+1)%4] == entryV1) ||
                (verts[i] == entryV1 && verts[(i+1)%4] == entryV0)) {
                entryIdx = i;
                break;
            }
        }
        if (entryIdx == -1) continue;

        // Get the new vertices for entry and exit edges using position-based keys
        auto entryKey = makePositionEdgeKey(entryV0, entryV1);
        auto exitKey = makePositionEdgeKey(exitV0, exitV1);

        auto entryNewIt = edgeToNewVerts.find(entryKey);
        auto exitNewIt = edgeToNewVerts.find(exitKey);
        if (entryNewIt == edgeToNewVerts.end() || exitNewIt == edgeToNewVerts.end()) continue;

        const auto& entryNewVerts = entryNewIt->second;
        const auto& exitNewVerts = exitNewIt->second;
        if (entryNewVerts.size() != static_cast<size_t>(count) ||
            exitNewVerts.size() != static_cast<size_t>(count)) continue;

        // Reorder vertices so entry is at index 0
        std::vector<uint32_t> reordered(4);
        for (int i = 0; i < 4; ++i) {
            reordered[i] = verts[(entryIdx + i) % 4];
        }
        // Now entry edge is reordered[0] -> reordered[1]
        // Exit edge is reordered[2] -> reordered[3]

        // Determine if we need to reverse the order of new verts on entry/exit
        // Entry edge: new verts should go from reordered[0] towards reordered[1]
        // Exit edge: new verts should go from reordered[3] towards reordered[2]
        // We need to check if the edge key ordering matches our expected direction

        std::vector<uint32_t> entryOrdered = entryNewVerts;
        std::vector<uint32_t> exitOrdered = exitNewVerts;

        // Check if entry edge direction matches (v0 to v1)
        // If the key was created with v1 first, we need to reverse
        {
            auto [v0, v1] = getEdgeVertices(edgesToCut[entryKey]);
            glm::vec3 keyDir = m_vertices[v1].position - m_vertices[v0].position;
            glm::vec3 expectedDir = m_vertices[reordered[1]].position - m_vertices[reordered[0]].position;
            if (glm::dot(keyDir, expectedDir) < 0) {
                std::reverse(entryOrdered.begin(), entryOrdered.end());
            }
        }

        // Check exit edge direction (should go from reordered[3] to reordered[2])
        {
            auto [v0, v1] = getEdgeVertices(edgesToCut[exitKey]);
            glm::vec3 keyDir = m_vertices[v1].position - m_vertices[v0].position;
            glm::vec3 expectedDir = m_vertices[reordered[2]].position - m_vertices[reordered[3]].position;
            if (glm::dot(keyDir, expectedDir) < 0) {
                std::reverse(exitOrdered.begin(), exitOrdered.end());
            }
        }

        // Split the quad into (count + 1) quads
        // First quad: reordered[0] -> entryOrdered[0] -> exitOrdered[0] -> reordered[3]
        // Middle quads: entryOrdered[i-1] -> entryOrdered[i] -> exitOrdered[i] -> exitOrdered[i-1]
        // Last quad: entryOrdered[count-1] -> reordered[1] -> reordered[2] -> exitOrdered[count-1]

        // First quad
        allFaces.push_back({reordered[0], entryOrdered[0], exitOrdered[0], reordered[3]});

        // Middle quads
        for (int i = 1; i < count; ++i) {
            allFaces.push_back({entryOrdered[i-1], entryOrdered[i], exitOrdered[i], exitOrdered[i-1]});
        }

        // Last quad
        allFaces.push_back({entryOrdered[count-1], reordered[1], reordered[2], exitOrdered[count-1]});
    }

    // Rebuild the half-edge structure
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    for (auto& v : m_vertices) {
        v.halfEdgeIndex = UINT32_MAX;
    }

    for (const auto& faceVerts : allFaces) {
        addFace(faceVerts);
    }

    // Link twin half-edges by position (handles duplicate vertices at same position)
    linkTwinsByPosition();
    rebuildEdgeMap();
    recalculateNormals();
}

void EditableMesh::recalculateNormals() {
    // Reset vertex normals
    for (auto& v : m_vertices) {
        v.normal = glm::vec3(0);
    }

    // Accumulate face normals to vertices
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        glm::vec3 faceNormal = getFaceNormal(faceIdx);
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            m_vertices[v].normal += faceNormal;
        }
    }

    // Normalize
    for (auto& v : m_vertices) {
        if (glm::length(v.normal) > 0.0001f) {
            v.normal = glm::normalize(v.normal);
        } else {
            v.normal = glm::vec3(0, 1, 0);
        }
    }
}

void EditableMesh::setAllVertexColors(const glm::vec4& color) {
    for (auto& v : m_vertices) {
        v.color = color;
    }
}

void EditableMesh::boxProjectUVs(float scale) {
    if (m_faces.empty()) return;

    // For each face, project its vertices based on face normal direction
    // Track which vertices have been assigned UVs to avoid overwriting with worse projections
    std::vector<bool> vertexProcessed(m_vertices.size(), false);
    std::vector<float> vertexDotProduct(m_vertices.size(), 0.0f);  // Track projection quality

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        glm::vec3 normal = getFaceNormal(faceIdx);
        auto faceVerts = getFaceVertices(faceIdx);

        // Determine dominant axis based on face normal
        float absX = std::abs(normal.x);
        float absY = std::abs(normal.y);
        float absZ = std::abs(normal.z);

        // Find which axis the normal aligns with most
        int dominantAxis = 0;  // 0 = X, 1 = Y, 2 = Z
        float maxAbs = absX;
        if (absY > maxAbs) { dominantAxis = 1; maxAbs = absY; }
        if (absZ > maxAbs) { dominantAxis = 2; maxAbs = absZ; }

        // Project each vertex in this face
        for (uint32_t vertIdx : faceVerts) {
            const glm::vec3& pos = m_vertices[vertIdx].position;

            // Only update if this projection is better (normal more aligned with axis)
            // or vertex hasn't been processed yet
            if (!vertexProcessed[vertIdx] || maxAbs > vertexDotProduct[vertIdx]) {
                glm::vec2 uv;

                switch (dominantAxis) {
                    case 0:  // X-facing: project onto YZ plane
                        uv.x = pos.z * (normal.x > 0 ? 1.0f : -1.0f);
                        uv.y = pos.y;
                        break;
                    case 1:  // Y-facing: project onto XZ plane
                        uv.x = pos.x;
                        uv.y = pos.z * (normal.y > 0 ? -1.0f : 1.0f);
                        break;
                    case 2:  // Z-facing: project onto XY plane
                        uv.x = pos.x * (normal.z > 0 ? -1.0f : 1.0f);
                        uv.y = pos.y;
                        break;
                }

                m_vertices[vertIdx].uv = uv * scale;
                vertexProcessed[vertIdx] = true;
                vertexDotProduct[vertIdx] = maxAbs;
            }
        }
    }

    // Normalize UVs to 0-1 range
    glm::vec2 uvMin(FLT_MAX), uvMax(-FLT_MAX);
    for (const auto& v : m_vertices) {
        uvMin = glm::min(uvMin, v.uv);
        uvMax = glm::max(uvMax, v.uv);
    }

    glm::vec2 uvRange = uvMax - uvMin;
    if (uvRange.x > 0.0001f && uvRange.y > 0.0001f) {
        // Scale to fit in 0-1, maintaining aspect ratio
        float maxRange = std::max(uvRange.x, uvRange.y);
        for (auto& v : m_vertices) {
            v.uv = (v.uv - uvMin) / maxRange;
        }
    }

    std::cout << "Box projected UVs for " << m_vertices.size() << " vertices" << std::endl;
}

// Helper to get all vertices affected by current selection (vertices, edges, or faces)
std::set<uint32_t> EditableMesh::getAffectedVertices() const {
    std::set<uint32_t> affected;

    // Direct vertex selection
    for (uint32_t i = 0; i < m_vertices.size(); ++i) {
        if (m_vertices[i].selected) {
            affected.insert(i);
        }
    }

    // Vertices from selected edges
    for (uint32_t heIdx : m_selectedEdges) {
        auto [v0, v1] = getEdgeVertices(heIdx);
        affected.insert(v0);
        affected.insert(v1);
    }

    // Vertices from selected faces
    for (uint32_t i = 0; i < m_faces.size(); ++i) {
        if (m_faces[i].selected) {
            auto verts = getFaceVertices(i);
            for (uint32_t v : verts) {
                affected.insert(v);
            }
        }
    }

    return affected;
}

void EditableMesh::translateSelectedVertices(const glm::vec3& delta) {
    auto affected = getAffectedVertices();
    if (affected.empty()) return;

    // Collect positions of affected vertices
    std::set<uint64_t> affectedPositions;
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    for (uint32_t idx : affected) {
        affectedPositions.insert(posKey(m_vertices[idx].position));
    }

    // Move ALL vertices at those positions (handles duplicate verts for hard edges)
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        if (affectedPositions.count(posKey(m_vertices[i].position))) {
            m_vertices[i].position += delta;
        }
    }
}

void EditableMesh::scaleSelectedVertices(const glm::vec3& scale, const glm::vec3& pivot) {
    auto affected = getAffectedVertices();
    if (affected.empty()) return;

    // Collect positions of affected vertices
    std::set<uint64_t> affectedPositions;
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    for (uint32_t idx : affected) {
        affectedPositions.insert(posKey(m_vertices[idx].position));
    }

    // Scale ALL vertices at those positions
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        if (affectedPositions.count(posKey(m_vertices[i].position))) {
            m_vertices[i].position = pivot + (m_vertices[i].position - pivot) * scale;
        }
    }
}

void EditableMesh::rotateSelectedVertices(const glm::vec3& eulerDegrees, const glm::vec3& pivot) {
    glm::vec3 radians = glm::radians(eulerDegrees);
    glm::mat4 rotMat = glm::mat4(1.0f);
    rotMat = glm::rotate(rotMat, radians.x, glm::vec3(1, 0, 0));
    rotMat = glm::rotate(rotMat, radians.y, glm::vec3(0, 1, 0));
    rotMat = glm::rotate(rotMat, radians.z, glm::vec3(0, 0, 1));

    auto affected = getAffectedVertices();
    if (affected.empty()) return;

    // Collect positions of affected vertices
    std::set<uint64_t> affectedPositions;
    auto posKey = [](const glm::vec3& p) -> uint64_t {
        int32_t x = static_cast<int32_t>(p.x * 10000.0f);
        int32_t y = static_cast<int32_t>(p.y * 10000.0f);
        int32_t z = static_cast<int32_t>(p.z * 10000.0f);
        return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
               (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
               static_cast<uint64_t>(z & 0xFFFFF);
    };

    for (uint32_t idx : affected) {
        affectedPositions.insert(posKey(m_vertices[idx].position));
    }

    // Rotate ALL vertices at those positions
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        if (affectedPositions.count(posKey(m_vertices[i].position))) {
            glm::vec3 local = m_vertices[i].position - pivot;
            glm::vec3 rotated = glm::vec3(rotMat * glm::vec4(local, 1.0f));
            m_vertices[i].position = pivot + rotated;
        }
    }
}

glm::vec3 EditableMesh::getSelectionCenter() const {
    auto affected = getAffectedVertices();
    if (affected.empty()) return glm::vec3(0);

    glm::vec3 center(0);
    for (uint32_t idx : affected) {
        center += m_vertices[idx].position;
    }
    return center / static_cast<float>(affected.size());
}

void EditableMesh::flattenX() {
    auto affected = getAffectedVertices();
    if (affected.size() < 2) return;

    // Calculate average X
    float avgX = 0.0f;
    for (uint32_t idx : affected) {
        avgX += m_vertices[idx].position.x;
    }
    avgX /= static_cast<float>(affected.size());

    // Set all X to average
    for (uint32_t idx : affected) {
        m_vertices[idx].position.x = avgX;
    }

    recalculateNormals();
}

void EditableMesh::flattenY() {
    auto affected = getAffectedVertices();
    if (affected.size() < 2) return;

    // Calculate average Y
    float avgY = 0.0f;
    for (uint32_t idx : affected) {
        avgY += m_vertices[idx].position.y;
    }
    avgY /= static_cast<float>(affected.size());

    // Set all Y to average
    for (uint32_t idx : affected) {
        m_vertices[idx].position.y = avgY;
    }

    recalculateNormals();
}

void EditableMesh::flattenZ() {
    auto affected = getAffectedVertices();
    if (affected.size() < 2) return;

    // Calculate average Z
    float avgZ = 0.0f;
    for (uint32_t idx : affected) {
        avgZ += m_vertices[idx].position.z;
    }
    avgZ /= static_cast<float>(affected.size());

    // Set all Z to average
    for (uint32_t idx : affected) {
        m_vertices[idx].position.z = avgZ;
    }

    recalculateNormals();
}

void EditableMesh::makeCoplanar() {
    auto affected = getAffectedVertices();
    if (affected.size() < 3) return;  // Need at least 3 points for a plane

    // Step 1: Calculate centroid
    glm::vec3 centroid(0);
    for (uint32_t idx : affected) {
        centroid += m_vertices[idx].position;
    }
    centroid /= static_cast<float>(affected.size());

    // Step 2: Build covariance matrix
    // Cov = sum of (p - centroid) * (p - centroid)^T
    glm::mat3 cov(0.0f);
    for (uint32_t idx : affected) {
        glm::vec3 d = m_vertices[idx].position - centroid;
        cov[0][0] += d.x * d.x;
        cov[0][1] += d.x * d.y;
        cov[0][2] += d.x * d.z;
        cov[1][1] += d.y * d.y;
        cov[1][2] += d.y * d.z;
        cov[2][2] += d.z * d.z;
    }
    // Symmetric matrix
    cov[1][0] = cov[0][1];
    cov[2][0] = cov[0][2];
    cov[2][1] = cov[1][2];

    // Step 3: Find eigenvector with smallest eigenvalue using power iteration
    // We use inverse iteration: (A - shift*I)^-1 converges to smallest eigenvector
    // For simplicity, we'll use a direct approach: try each axis and pick the one
    // that results in minimum variance

    // Alternative simpler approach: compute normal using Newell's method
    // For scattered points, we use cross products of vectors from centroid
    glm::vec3 normal(0);

    // Use SVD-like approach: the plane normal is the direction of least variance
    // We'll find it by testing the eigenvectors

    // Compute eigenvalues of 3x3 symmetric matrix analytically
    float p1 = cov[0][1] * cov[0][1] + cov[0][2] * cov[0][2] + cov[1][2] * cov[1][2];

    if (p1 < 1e-10f) {
        // Covariance is diagonal - eigenvalues are diagonal elements
        float e1 = cov[0][0], e2 = cov[1][1], e3 = cov[2][2];
        if (e1 <= e2 && e1 <= e3) normal = glm::vec3(1, 0, 0);
        else if (e2 <= e1 && e2 <= e3) normal = glm::vec3(0, 1, 0);
        else normal = glm::vec3(0, 0, 1);
    } else {
        // Use power iteration to find eigenvector with smallest eigenvalue
        // First find largest eigenvector, then find smallest in remaining space
        glm::vec3 v1(1, 0, 0);  // Start with arbitrary vector

        // Power iteration for largest eigenvalue
        for (int i = 0; i < 50; ++i) {
            v1 = cov * v1;
            float len = glm::length(v1);
            if (len > 1e-10f) v1 /= len;
        }

        // Find second largest - orthogonal to first
        glm::vec3 v2 = glm::vec3(0, 1, 0);
        if (std::abs(glm::dot(v1, v2)) > 0.9f) v2 = glm::vec3(0, 0, 1);
        v2 = v2 - glm::dot(v2, v1) * v1;
        v2 = glm::normalize(v2);

        for (int i = 0; i < 50; ++i) {
            v2 = cov * v2;
            v2 = v2 - glm::dot(v2, v1) * v1;  // Keep orthogonal
            float len = glm::length(v2);
            if (len > 1e-10f) v2 /= len;
        }

        // The normal is orthogonal to both largest eigenvectors
        normal = glm::normalize(glm::cross(v1, v2));
    }

    // Step 4: Project all vertices onto the plane
    // Plane equation: dot(p - centroid, normal) = 0
    // Projection: p' = p - dot(p - centroid, normal) * normal
    for (uint32_t idx : affected) {
        glm::vec3& pos = m_vertices[idx].position;
        float dist = glm::dot(pos - centroid, normal);
        pos = pos - dist * normal;
    }

    recalculateNormals();
}

uint32_t EditableMesh::findHalfEdge(uint32_t fromVert, uint32_t toVert) const {
    uint64_t key = makeEdgeKey(fromVert, toVert);
    auto it = m_edgeMap.find(key);
    if (it == m_edgeMap.end()) return UINT32_MAX;

    // Check which direction this half-edge goes
    uint32_t he = it->second;
    auto [v0, v1] = getEdgeVertices(he);
    if (v0 == fromVert && v1 == toVert) {
        return he;
    }

    // Try twin
    uint32_t twin = m_halfEdges[he].twinIndex;
    if (twin != UINT32_MAX) {
        auto [tv0, tv1] = getEdgeVertices(twin);
        if (tv0 == fromVert && tv1 == toVert) {
            return twin;
        }
    }

    return UINT32_MAX;
}

bool EditableMesh::validateTopology() const {
    bool valid = true;

    // Check half-edge consistency
    for (uint32_t i = 0; i < m_halfEdges.size(); ++i) {
        const HalfEdge& he = m_halfEdges[i];

        // Check next/prev form a loop
        if (m_halfEdges[he.nextIndex].prevIndex != i) {
            std::cerr << "Half-edge " << i << ": next->prev != self" << std::endl;
            valid = false;
        }
        if (m_halfEdges[he.prevIndex].nextIndex != i) {
            std::cerr << "Half-edge " << i << ": prev->next != self" << std::endl;
            valid = false;
        }

        // Check twin consistency
        if (he.twinIndex != UINT32_MAX) {
            if (m_halfEdges[he.twinIndex].twinIndex != i) {
                std::cerr << "Half-edge " << i << ": twin->twin != self" << std::endl;
                valid = false;
            }
        }
    }

    // Check face half-edge loops
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        const HEFace& face = m_faces[faceIdx];
        uint32_t he = face.halfEdgeIndex;
        uint32_t count = 0;

        do {
            if (m_halfEdges[he].faceIndex != faceIdx) {
                std::cerr << "Face " << faceIdx << ": half-edge " << he
                          << " has wrong face index" << std::endl;
                valid = false;
            }
            he = m_halfEdges[he].nextIndex;
            count++;
            if (count > face.vertexCount + 1) {
                std::cerr << "Face " << faceIdx << ": infinite loop in half-edges" << std::endl;
                valid = false;
                break;
            }
        } while (he != face.halfEdgeIndex);

        if (count != face.vertexCount) {
            std::cerr << "Face " << faceIdx << ": vertex count mismatch ("
                      << count << " vs " << face.vertexCount << ")" << std::endl;
            valid = false;
        }
    }

    return valid;
}

void EditableMesh::saveState() {
    // Create snapshot of current state
    MeshState state;
    state.vertices = m_vertices;
    state.halfEdges = m_halfEdges;
    state.faces = m_faces;
    state.edgeMap = m_edgeMap;
    state.selectedEdges = m_selectedEdges;

    // Push to undo stack
    m_undoStack.push_back(std::move(state));

    // Limit undo stack size
    if (m_undoStack.size() > MAX_UNDO_LEVELS) {
        m_undoStack.erase(m_undoStack.begin());
    }

    // Clear redo stack when new action is performed
    m_redoStack.clear();
}

bool EditableMesh::undo() {
    if (m_undoStack.empty()) return false;

    // Save current state to redo stack
    MeshState redoState;
    redoState.vertices = m_vertices;
    redoState.halfEdges = m_halfEdges;
    redoState.faces = m_faces;
    redoState.edgeMap = m_edgeMap;
    redoState.selectedEdges = m_selectedEdges;
    m_redoStack.push_back(std::move(redoState));

    // Restore from undo stack
    MeshState& undoState = m_undoStack.back();
    m_vertices = std::move(undoState.vertices);
    m_halfEdges = std::move(undoState.halfEdges);
    m_faces = std::move(undoState.faces);
    m_edgeMap = std::move(undoState.edgeMap);
    m_selectedEdges = std::move(undoState.selectedEdges);
    m_undoStack.pop_back();

    return true;
}

bool EditableMesh::redo() {
    if (m_redoStack.empty()) return false;

    // Save current state to undo stack
    MeshState undoState;
    undoState.vertices = m_vertices;
    undoState.halfEdges = m_halfEdges;
    undoState.faces = m_faces;
    undoState.edgeMap = m_edgeMap;
    undoState.selectedEdges = m_selectedEdges;
    m_undoStack.push_back(std::move(undoState));

    // Restore from redo stack
    MeshState& redoState = m_redoStack.back();
    m_vertices = std::move(redoState.vertices);
    m_halfEdges = std::move(redoState.halfEdges);
    m_faces = std::move(redoState.faces);
    m_edgeMap = std::move(redoState.edgeMap);
    m_selectedEdges = std::move(redoState.selectedEdges);
    m_redoStack.pop_back();

    return true;
}

void EditableMesh::clearUndoHistory() {
    m_undoStack.clear();
    m_redoStack.clear();
}

void EditableMesh::smartProjectUVs(float angleThreshold, float islandMargin) {
    if (m_faces.empty()) return;

    // Convert angle threshold to cosine for faster comparison
    float cosThreshold = std::cos(glm::radians(angleThreshold));

    // Step 1: Group faces into islands based on angle threshold
    std::vector<int> faceIsland(m_faces.size(), -1);
    std::vector<std::vector<uint32_t>> islands;

    for (uint32_t startFace = 0; startFace < m_faces.size(); ++startFace) {
        if (faceIsland[startFace] != -1) continue;

        // Start a new island with flood fill
        int islandIdx = static_cast<int>(islands.size());
        islands.push_back({});
        std::queue<uint32_t> toProcess;
        toProcess.push(startFace);
        faceIsland[startFace] = islandIdx;

        while (!toProcess.empty()) {
            uint32_t faceIdx = toProcess.front();
            toProcess.pop();
            islands[islandIdx].push_back(faceIdx);

            glm::vec3 faceNormal = getFaceNormal(faceIdx);

            // Check all neighboring faces
            auto neighbors = getFaceNeighbors(faceIdx);
            for (uint32_t neighborIdx : neighbors) {
                if (faceIsland[neighborIdx] != -1) continue;

                glm::vec3 neighborNormal = getFaceNormal(neighborIdx);
                float dot = glm::dot(faceNormal, neighborNormal);

                // If normals are similar enough, add to same island
                if (dot >= cosThreshold) {
                    faceIsland[neighborIdx] = islandIdx;
                    toProcess.push(neighborIdx);
                }
            }
        }
    }

    std::cout << "Smart UV: Created " << islands.size() << " islands from " << m_faces.size() << " faces" << std::endl;

    // Step 2: For each island, compute projection and local UVs
    struct IslandData {
        std::vector<uint32_t> faces;
        std::set<uint32_t> vertices;
        glm::vec3 avgNormal;
        glm::vec3 tangent;
        glm::vec3 bitangent;
        glm::vec2 uvMin, uvMax;
        float width, height;
    };
    std::vector<IslandData> islandData(islands.size());

    for (size_t i = 0; i < islands.size(); ++i) {
        IslandData& data = islandData[i];
        data.faces = islands[i];

        // Collect all vertices in this island
        for (uint32_t faceIdx : data.faces) {
            auto verts = getFaceVertices(faceIdx);
            for (uint32_t v : verts) {
                data.vertices.insert(v);
            }
        }

        // Compute average normal for the island
        data.avgNormal = glm::vec3(0);
        for (uint32_t faceIdx : data.faces) {
            data.avgNormal += getFaceNormal(faceIdx);
        }
        data.avgNormal = glm::normalize(data.avgNormal);

        // Build orthonormal basis for projection
        // Find a vector not parallel to normal
        glm::vec3 up = (std::abs(data.avgNormal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        data.tangent = glm::normalize(glm::cross(up, data.avgNormal));
        data.bitangent = glm::cross(data.avgNormal, data.tangent);

        // Project vertices onto the plane and compute local UVs
        data.uvMin = glm::vec2(FLT_MAX);
        data.uvMax = glm::vec2(-FLT_MAX);

        for (uint32_t vertIdx : data.vertices) {
            const glm::vec3& pos = m_vertices[vertIdx].position;
            glm::vec2 uv;
            uv.x = glm::dot(pos, data.tangent);
            uv.y = glm::dot(pos, data.bitangent);

            // Temporarily store in vertex UV (will be adjusted during packing)
            m_vertices[vertIdx].uv = uv;

            data.uvMin = glm::min(data.uvMin, uv);
            data.uvMax = glm::max(data.uvMax, uv);
        }

        // Normalize island UVs to start at origin
        for (uint32_t vertIdx : data.vertices) {
            m_vertices[vertIdx].uv -= data.uvMin;
        }

        data.width = data.uvMax.x - data.uvMin.x;
        data.height = data.uvMax.y - data.uvMin.y;

        // Ensure minimum size to avoid degenerate islands
        if (data.width < 0.001f) data.width = 0.001f;
        if (data.height < 0.001f) data.height = 0.001f;
    }

    // Step 3: Pack islands into UV space using simple shelf packing
    // Sort islands by height (tallest first) for better packing
    std::vector<size_t> sortedIndices(islands.size());
    for (size_t i = 0; i < islands.size(); ++i) sortedIndices[i] = i;
    std::sort(sortedIndices.begin(), sortedIndices.end(), [&](size_t a, size_t b) {
        return islandData[a].height > islandData[b].height;
    });

    // Find total area and estimate scale factor to fit in 0-1 with margins
    float totalArea = 0;
    float maxDim = 0;
    for (const auto& data : islandData) {
        totalArea += (data.width + islandMargin) * (data.height + islandMargin);
        maxDim = std::max(maxDim, std::max(data.width, data.height));
    }

    // Estimate scale to fit everything in unit square (with some headroom)
    float scale = 1.0f / (std::sqrt(totalArea) * 1.2f);
    scale = std::min(scale, 0.9f / maxDim);  // Don't let any island be bigger than 90% of UV space

    // Simple shelf-based packing
    struct Shelf {
        float y;
        float height;
        float x;  // Current x position on this shelf
    };
    std::vector<Shelf> shelves;
    shelves.push_back({islandMargin, 0, islandMargin});

    std::vector<glm::vec2> islandOffsets(islands.size());

    for (size_t idx : sortedIndices) {
        IslandData& data = islandData[idx];
        float scaledWidth = data.width * scale + islandMargin;
        float scaledHeight = data.height * scale + islandMargin;

        // Find a shelf that can fit this island
        bool placed = false;
        for (auto& shelf : shelves) {
            if (shelf.x + scaledWidth <= 1.0f - islandMargin &&
                shelf.y + scaledHeight <= 1.0f - islandMargin) {
                // Place on this shelf
                islandOffsets[idx] = glm::vec2(shelf.x, shelf.y);
                shelf.x += scaledWidth;
                shelf.height = std::max(shelf.height, scaledHeight);
                placed = true;
                break;
            }
        }

        if (!placed) {
            // Create new shelf
            float newY = shelves.back().y + shelves.back().height;
            if (newY + scaledHeight <= 1.0f - islandMargin) {
                shelves.push_back({newY, scaledHeight, islandMargin + scaledWidth});
                islandOffsets[idx] = glm::vec2(islandMargin, newY);
            } else {
                // Doesn't fit - scale down and retry would be ideal, but for now just place it
                // This can cause overlap in extreme cases
                islandOffsets[idx] = glm::vec2(islandMargin, islandMargin);
                std::cerr << "Warning: Island " << idx << " may overlap (UV space full)" << std::endl;
            }
        }
    }

    // Step 4: Apply final UV coordinates
    for (size_t i = 0; i < islands.size(); ++i) {
        IslandData& data = islandData[i];
        glm::vec2 offset = islandOffsets[i];

        for (uint32_t vertIdx : data.vertices) {
            // Scale and offset the UV
            m_vertices[vertIdx].uv = m_vertices[vertIdx].uv * scale + offset;
        }
    }

    // Handle vertices that might be shared between islands (use the last assigned UV)
    // For proper seam handling, we'd need to duplicate vertices, but that changes topology
    // For now, shared vertices get the UV from whichever island processed them last

    std::cout << "Smart UV projection complete: " << islands.size() << " islands packed" << std::endl;
}

void EditableMesh::planarProjectByNormal(float normalTolerance, float islandMargin) {
    if (m_faces.empty()) return;

    // Step 1: Group faces by their normal direction
    auto normalKey = [&](const glm::vec3& n) -> std::tuple<int, int, int> {
        int precision = static_cast<int>(1.0f / normalTolerance);
        return std::make_tuple(
            static_cast<int>(std::round(n.x * precision)),
            static_cast<int>(std::round(n.y * precision)),
            static_cast<int>(std::round(n.z * precision))
        );
    };

    // Map each face to its normal group
    std::map<std::tuple<int, int, int>, std::vector<uint32_t>> normalGroups;
    std::vector<int> faceToGroup(m_faces.size());

    int groupIdx = 0;
    std::map<std::tuple<int, int, int>, int> keyToGroupIdx;

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        glm::vec3 normal = getFaceNormal(faceIdx);
        auto key = normalKey(normal);

        if (keyToGroupIdx.find(key) == keyToGroupIdx.end()) {
            keyToGroupIdx[key] = groupIdx++;
        }
        faceToGroup[faceIdx] = keyToGroupIdx[key];
        normalGroups[key].push_back(faceIdx);
    }

    std::cout << "Planar UV: Found " << normalGroups.size() << " unique normal directions from " << m_faces.size() << " faces" << std::endl;

    // Step 2: Duplicate vertices at UV seams
    // For each vertex, check which normal groups use it
    // If a vertex is used by multiple normal groups, duplicate it for each group

    // Map: (original vertex index, group index) -> new vertex index
    std::map<std::pair<uint32_t, int>, uint32_t> vertexDuplicates;

    // Track which vertices need duplication
    std::map<uint32_t, std::set<int>> vertexGroups;  // vertex -> set of groups using it

    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        int group = faceToGroup[faceIdx];
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            vertexGroups[v].insert(group);
        }
    }

    // Create duplicates for shared vertices
    for (auto& [vertIdx, groups] : vertexGroups) {
        bool first = true;
        for (int group : groups) {
            if (first) {
                // First group keeps the original vertex
                vertexDuplicates[{vertIdx, group}] = vertIdx;
                first = false;
            } else {
                // Other groups get duplicates
                uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
                m_vertices.push_back(m_vertices[vertIdx]);  // Copy vertex data
                vertexDuplicates[{vertIdx, group}] = newIdx;
            }
        }
    }

    // Step 3: Update half-edges to point to duplicated vertices
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        int group = faceToGroup[faceIdx];
        uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
        uint32_t heIdx = startHE;

        do {
            uint32_t oldVertIdx = m_halfEdges[heIdx].vertexIndex;
            uint32_t newVertIdx = vertexDuplicates[{oldVertIdx, group}];
            m_halfEdges[heIdx].vertexIndex = newVertIdx;

            // Update the vertex's halfEdgeIndex to point to one of its outgoing edges
            m_vertices[newVertIdx].halfEdgeIndex = heIdx;

            heIdx = m_halfEdges[heIdx].nextIndex;
        } while (heIdx != startHE);
    }

    // Rebuild edge map since vertex indices changed
    rebuildEdgeMap();

    // Step 4: For each group, compute planar projection
    struct IslandData {
        std::vector<uint32_t> faces;
        std::set<uint32_t> vertices;
        glm::vec3 normal;
        glm::vec3 tangent;
        glm::vec3 bitangent;
        glm::vec2 uvMin, uvMax;
        float width, height;
    };

    std::vector<IslandData> islands;

    for (auto& [key, faceList] : normalGroups) {
        IslandData island;
        island.faces = faceList;

        // Compute average normal for this group
        island.normal = glm::vec3(0);
        for (uint32_t faceIdx : island.faces) {
            island.normal += getFaceNormal(faceIdx);
        }
        island.normal = glm::normalize(island.normal);

        // Collect vertices (now they're unique per group after duplication)
        for (uint32_t faceIdx : island.faces) {
            auto verts = getFaceVertices(faceIdx);
            for (uint32_t v : verts) {
                island.vertices.insert(v);
            }
        }

        // Build orthonormal basis for projection plane
        glm::vec3 up = (std::abs(island.normal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        island.tangent = glm::normalize(glm::cross(up, island.normal));
        island.bitangent = glm::cross(island.normal, island.tangent);

        // Project each vertex onto the plane
        island.uvMin = glm::vec2(FLT_MAX);
        island.uvMax = glm::vec2(-FLT_MAX);

        for (uint32_t vertIdx : island.vertices) {
            const glm::vec3& pos = m_vertices[vertIdx].position;
            glm::vec2 uv;
            uv.x = glm::dot(pos, island.tangent);
            uv.y = glm::dot(pos, island.bitangent);
            m_vertices[vertIdx].uv = uv;
            island.uvMin = glm::min(island.uvMin, uv);
            island.uvMax = glm::max(island.uvMax, uv);
        }

        // Normalize to start at origin
        for (uint32_t vertIdx : island.vertices) {
            m_vertices[vertIdx].uv -= island.uvMin;
        }

        island.width = island.uvMax.x - island.uvMin.x;
        island.height = island.uvMax.y - island.uvMin.y;

        if (island.width < 0.001f) island.width = 0.001f;
        if (island.height < 0.001f) island.height = 0.001f;

        islands.push_back(std::move(island));
    }

    // Step 5: Pack islands into UV space using shelf packing
    std::vector<size_t> sortedIndices(islands.size());
    for (size_t i = 0; i < islands.size(); ++i) sortedIndices[i] = i;
    std::sort(sortedIndices.begin(), sortedIndices.end(), [&](size_t a, size_t b) {
        return islands[a].height > islands[b].height;
    });

    float totalArea = 0;
    float maxDim = 0;
    for (const auto& island : islands) {
        totalArea += (island.width + islandMargin) * (island.height + islandMargin);
        maxDim = std::max(maxDim, std::max(island.width, island.height));
    }

    // Calculate scale with retry if packing fails
    float availableSpace = 1.0f - 2.0f * islandMargin;
    float scale = availableSpace / (std::sqrt(totalArea) * 1.5f);  // More conservative
    scale = std::min(scale, availableSpace / maxDim * 0.9f);  // Leave headroom

    struct Shelf {
        float y;
        float height;
        float x;
    };

    std::vector<glm::vec2> islandOffsets(islands.size());
    bool packingSucceeded = false;

    // Retry with smaller scale if packing fails
    for (int attempt = 0; attempt < 5 && !packingSucceeded; ++attempt) {
        std::vector<Shelf> shelves;
        shelves.push_back({islandMargin, 0.0f, islandMargin});
        packingSucceeded = true;

        for (size_t idx : sortedIndices) {
            IslandData& island = islands[idx];
            float scaledWidth = island.width * scale + islandMargin;
            float scaledHeight = island.height * scale + islandMargin;

            bool placed = false;
            for (auto& shelf : shelves) {
                if (shelf.x + scaledWidth <= 1.0f - islandMargin &&
                    shelf.y + scaledHeight <= 1.0f - islandMargin) {
                    islandOffsets[idx] = glm::vec2(shelf.x, shelf.y);
                    shelf.x += scaledWidth;
                    shelf.height = std::max(shelf.height, scaledHeight);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                float newY = shelves.back().y + shelves.back().height;
                if (newY + scaledHeight <= 1.0f - islandMargin) {
                    shelves.push_back({newY, scaledHeight, islandMargin + scaledWidth});
                    islandOffsets[idx] = glm::vec2(islandMargin, newY);
                } else {
                    // Packing failed - reduce scale and retry
                    packingSucceeded = false;
                    scale *= 0.8f;
                    break;
                }
            }
        }
    }

    if (!packingSucceeded) {
        std::cerr << "Warning: UV packing incomplete, some islands may overlap" << std::endl;
    }

    // Step 6: Apply final scaled and offset UVs
    for (size_t i = 0; i < islands.size(); ++i) {
        IslandData& island = islands[i];
        glm::vec2 offset = islandOffsets[i];

        for (uint32_t vertIdx : island.vertices) {
            m_vertices[vertIdx].uv = m_vertices[vertIdx].uv * scale + offset;
        }
    }

    std::cout << "Planar UV projection complete: " << islands.size() << " islands, "
              << m_vertices.size() << " vertices (after seam splitting)" << std::endl;
}

void EditableMesh::projectSelectedFacesFromView(const glm::vec3& viewDir, const glm::vec3& viewUp, float scale) {
    // Get selected faces
    auto selectedFaces = getSelectedFaces();
    if (selectedFaces.empty()) {
        std::cout << "No faces selected for projection" << std::endl;
        return;
    }

    // Convert to set for fast lookup
    std::set<uint32_t> selectedFaceSet(selectedFaces.begin(), selectedFaces.end());

    // Step 1: Find vertices that are shared between selected and non-selected faces
    // These need to be duplicated so the projection creates a separate island

    // Track which vertices belong to selected faces
    std::set<uint32_t> selectedVerts;
    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            selectedVerts.insert(v);
        }
    }

    // Find boundary vertices (shared with non-selected faces)
    std::set<uint32_t> boundaryVerts;
    for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        if (selectedFaceSet.count(faceIdx) > 0) continue;  // Skip selected faces

        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            if (selectedVerts.count(v) > 0) {
                boundaryVerts.insert(v);
            }
        }
    }

    // Step 2: Duplicate boundary vertices for the selected faces
    // Map: old vertex index -> new vertex index (for selected faces only)
    std::map<uint32_t, uint32_t> vertexRemap;

    for (uint32_t oldIdx : boundaryVerts) {
        // Create a duplicate vertex
        uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back(m_vertices[oldIdx]);
        vertexRemap[oldIdx] = newIdx;
    }

    // Step 3: Update selected faces to use the new duplicated vertices
    for (uint32_t faceIdx : selectedFaces) {
        uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
        uint32_t heIdx = startHE;

        do {
            uint32_t oldVertIdx = m_halfEdges[heIdx].vertexIndex;
            if (vertexRemap.count(oldVertIdx) > 0) {
                uint32_t newVertIdx = vertexRemap[oldVertIdx];
                m_halfEdges[heIdx].vertexIndex = newVertIdx;
                m_vertices[newVertIdx].halfEdgeIndex = heIdx;
            }
            heIdx = m_halfEdges[heIdx].nextIndex;
        } while (heIdx != startHE);
    }

    // Rebuild edge map since vertex indices changed
    rebuildEdgeMap();

    // Step 4: Collect vertices from selected faces (now with duplicates)
    std::set<uint32_t> projectionVerts;
    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            projectionVerts.insert(v);
        }
    }

    // Step 5: Build orthonormal basis from view direction and project
    glm::vec3 projNormal = -glm::normalize(viewDir);
    glm::vec3 projRight = glm::normalize(glm::cross(viewUp, projNormal));
    glm::vec3 projUp = glm::cross(projNormal, projRight);

    // Find bounds of projection
    glm::vec2 uvMin(FLT_MAX);
    glm::vec2 uvMax(-FLT_MAX);

    std::map<uint32_t, glm::vec2> projectedUVs;
    for (uint32_t vertIdx : projectionVerts) {
        const glm::vec3& pos = m_vertices[vertIdx].position;

        glm::vec2 uv;
        uv.x = glm::dot(pos, projRight);
        uv.y = glm::dot(pos, projUp);

        projectedUVs[vertIdx] = uv;
        uvMin = glm::min(uvMin, uv);
        uvMax = glm::max(uvMax, uv);
    }

    // Calculate dimensions and centering
    glm::vec2 size = uvMax - uvMin;
    float maxDim = std::max(size.x, size.y);
    if (maxDim < 0.0001f) maxDim = 1.0f;

    float normalizeScale = scale / maxDim;
    glm::vec2 center = (uvMin + uvMax) * 0.5f;

    // Apply normalized UVs
    for (uint32_t vertIdx : projectionVerts) {
        glm::vec2 uv = projectedUVs[vertIdx];
        uv = (uv - center) * normalizeScale + glm::vec2(0.5f, 0.5f);
        m_vertices[vertIdx].uv = uv;
    }

    std::cout << "Projected " << selectedFaces.size() << " faces from view ("
              << boundaryVerts.size() << " boundary vertices duplicated)" << std::endl;
}

void EditableMesh::cylindricalProjectUVs(const glm::vec3& axisHint, bool usePCA) {
    // Get selected faces, or use all faces if none selected
    auto selectedFaces = getSelectedFaces();
    bool usingAllFaces = selectedFaces.empty();

    std::cout << "Cylindrical UV projection starting:" << std::endl;
    std::cout << "  - Faces with selection flag: " << selectedFaces.size() << std::endl;
    std::cout << "  - Total faces in mesh: " << m_faces.size() << std::endl;
    std::cout << "  - Using all faces: " << (usingAllFaces ? "YES" : "NO") << std::endl;

    if (usingAllFaces) {
        selectedFaces.reserve(m_faces.size());
        for (uint32_t i = 0; i < m_faces.size(); ++i) {
            selectedFaces.push_back(i);
        }
    }

    if (selectedFaces.empty()) {
        std::cout << "No faces for cylindrical projection" << std::endl;
        return;
    }

    // Convert to set for fast lookup
    std::set<uint32_t> selectedFaceSet(selectedFaces.begin(), selectedFaces.end());

    // =========================================================================
    // STEP 0: Duplicate boundary vertices (shared with non-selected faces)
    // =========================================================================
    // This ensures that modifying UVs for selected faces doesn't affect
    // non-selected faces (like cylinder caps when only body is selected).

    size_t boundaryVertsDuplicated = 0;

    if (!usingAllFaces) {
        // Collect vertices from selected faces
        std::set<uint32_t> selectedVerts;
        for (uint32_t faceIdx : selectedFaces) {
            auto verts = getFaceVertices(faceIdx);
            for (uint32_t v : verts) {
                selectedVerts.insert(v);
            }
        }

        std::cout << "  - Vertices in selected faces: " << selectedVerts.size() << std::endl;

        // Find vertices shared with non-selected faces (these need duplication)
        std::set<uint32_t> boundaryVerts;
        size_t nonSelectedFaceCount = 0;
        for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
            if (selectedFaceSet.count(faceIdx) > 0) continue;  // Skip selected faces
            nonSelectedFaceCount++;

            auto verts = getFaceVertices(faceIdx);
            for (uint32_t v : verts) {
                if (selectedVerts.count(v) > 0) {
                    boundaryVerts.insert(v);
                }
            }
        }

        std::cout << "  - Non-selected faces checked: " << nonSelectedFaceCount << std::endl;
        std::cout << "  - Boundary vertices found: " << boundaryVerts.size() << std::endl;

        // Duplicate boundary vertices for selected faces
        std::map<uint32_t, uint32_t> boundaryVertexRemap;
        for (uint32_t oldIdx : boundaryVerts) {
            uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(m_vertices[oldIdx]);
            boundaryVertexRemap[oldIdx] = newIdx;
        }

        // Update selected faces to use duplicated vertices
        for (uint32_t faceIdx : selectedFaces) {
            uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
            uint32_t heIdx = startHE;

            do {
                uint32_t oldVertIdx = m_halfEdges[heIdx].vertexIndex;
                if (boundaryVertexRemap.count(oldVertIdx) > 0) {
                    uint32_t newVertIdx = boundaryVertexRemap[oldVertIdx];
                    m_halfEdges[heIdx].vertexIndex = newVertIdx;
                    m_vertices[newVertIdx].halfEdgeIndex = heIdx;
                }
                heIdx = m_halfEdges[heIdx].nextIndex;
            } while (heIdx != startHE);
        }

        // Rebuild edge map since vertex indices changed
        if (!boundaryVerts.empty()) {
            rebuildEdgeMap();
        }

        boundaryVertsDuplicated = boundaryVerts.size();
    }

    // Collect all vertices from the target faces (now with boundary verts duplicated)
    std::set<uint32_t> targetVertSet;
    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            targetVertSet.insert(v);
        }
    }
    std::vector<uint32_t> targetVerts(targetVertSet.begin(), targetVertSet.end());

    if (targetVerts.size() < 3) {
        std::cout << "Not enough vertices for cylindrical projection" << std::endl;
        return;
    }

    // =========================================================================
    // STEP 1: Compute cylinder axis
    // =========================================================================
    // We use either PCA (more robust for arbitrary orientations) or the axis hint.
    // PCA finds the direction of maximum variance - for a cylinder, this is the
    // cylinder's main axis.

    // Compute centroid
    glm::vec3 centroid(0.0f);
    for (uint32_t vIdx : targetVerts) {
        centroid += m_vertices[vIdx].position;
    }
    centroid /= static_cast<float>(targetVerts.size());

    glm::vec3 cylinderAxis;

    if (usePCA) {
        // PCA: Build covariance matrix and find eigenvector with largest eigenvalue
        // For cylinder unwrapping, the axis is the direction of greatest extent

        // Covariance matrix (3x3, symmetric)
        float cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

        for (uint32_t vIdx : targetVerts) {
            glm::vec3 d = m_vertices[vIdx].position - centroid;
            cov[0][0] += d.x * d.x;
            cov[0][1] += d.x * d.y;
            cov[0][2] += d.x * d.z;
            cov[1][1] += d.y * d.y;
            cov[1][2] += d.y * d.z;
            cov[2][2] += d.z * d.z;
        }
        cov[1][0] = cov[0][1];
        cov[2][0] = cov[0][2];
        cov[2][1] = cov[1][2];

        // Power iteration to find dominant eigenvector (axis of max variance)
        // This is the cylinder's main axis
        glm::vec3 v = glm::normalize(axisHint);  // Start with hint
        for (int iter = 0; iter < 50; ++iter) {
            glm::vec3 vNew;
            vNew.x = cov[0][0] * v.x + cov[0][1] * v.y + cov[0][2] * v.z;
            vNew.y = cov[1][0] * v.x + cov[1][1] * v.y + cov[1][2] * v.z;
            vNew.z = cov[2][0] * v.x + cov[2][1] * v.y + cov[2][2] * v.z;
            float len = glm::length(vNew);
            if (len > 1e-8f) {
                v = vNew / len;
            }
        }
        cylinderAxis = v;
    } else {
        cylinderAxis = glm::normalize(axisHint);
    }

    // Ensure consistent axis direction (prefer positive Y component if near-vertical)
    if (std::abs(cylinderAxis.y) > 0.5f && cylinderAxis.y < 0) {
        cylinderAxis = -cylinderAxis;
    }

    // =========================================================================
    // STEP 2: Build orthonormal basis perpendicular to cylinder axis
    // =========================================================================
    // We need two vectors (tangent, bitangent) in the plane perpendicular to the
    // cylinder axis. These define the "reference frame" for computing theta.

    glm::vec3 up = (std::abs(cylinderAxis.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 tangent = glm::normalize(glm::cross(up, cylinderAxis));
    glm::vec3 bitangent = glm::cross(cylinderAxis, tangent);

    // =========================================================================
    // STEP 3: Compute cylindrical coordinates for each vertex
    // =========================================================================
    // For each vertex:
    //   - theta = atan2(dot with bitangent, dot with tangent) -> angle around axis
    //   - height = dot with axis (projection onto cylinder axis)
    //
    // theta ranges from -π to +π. We'll normalize to 0-1 for U coordinate.
    // height will be normalized to 0-1 for V coordinate.

    struct VertexCylCoord {
        float theta;   // Angle around cylinder axis (-π to +π)
        float height;  // Position along cylinder axis
    };
    std::map<uint32_t, VertexCylCoord> cylCoords;

    float minHeight = FLT_MAX, maxHeight = -FLT_MAX;

    for (uint32_t vIdx : targetVerts) {
        glm::vec3 pos = m_vertices[vIdx].position - centroid;

        // Project onto plane perpendicular to axis
        float tx = glm::dot(pos, tangent);
        float ty = glm::dot(pos, bitangent);

        // Compute angle (theta)
        float theta = std::atan2(ty, tx);  // Range: [-π, +π]

        // Height along axis
        float height = glm::dot(pos, cylinderAxis);

        cylCoords[vIdx] = {theta, height};
        minHeight = std::min(minHeight, height);
        maxHeight = std::max(maxHeight, height);
    }

    // =========================================================================
    // STEP 4: Assign UVs and handle seam face
    // =========================================================================
    // The seam is at theta = ±π. One face will span across this discontinuity.
    // Strategy: First assign all UVs, then find the seam face and fix it.

    float heightRange = maxHeight - minHeight;
    if (heightRange < 1e-6f) heightRange = 1.0f;

    // First pass: assign UVs to all vertices using basic formula
    for (uint32_t vIdx : targetVerts) {
        float theta = cylCoords[vIdx].theta;
        float height = cylCoords[vIdx].height;

        // Map theta [-π, π] to U [0, 1]
        float u = (theta + glm::pi<float>()) / (2.0f * glm::pi<float>());
        float v = (height - minHeight) / heightRange;

        m_vertices[vIdx].uv = glm::vec2(u, v);
    }

    // Second pass: find seam face (theta difference > π) and fix its UVs
    size_t boundaryVertsCount = 0;
    uint32_t seamFaceIdx = UINT32_MAX;

    // First pass: identify ALL faces that cross the seam
    std::vector<uint32_t> seamFaces;
    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);

        float minTheta = FLT_MAX, maxTheta = -FLT_MAX;
        for (uint32_t v : verts) {
            float theta = cylCoords[v].theta;
            minTheta = std::min(minTheta, theta);
            maxTheta = std::max(maxTheta, theta);
        }

        // Face crosses seam if theta range spans more than π
        if (maxTheta - minTheta > glm::pi<float>()) {
            seamFaces.push_back(faceIdx);
            if (seamFaceIdx == UINT32_MAX) seamFaceIdx = faceIdx;  // Track first one for debug
        }
    }

    std::cout << "[UV] Found " << seamFaces.size() << " seam-crossing faces" << std::endl;

    // Second pass: fix all seam faces by duplicating negative-theta vertices
    for (uint32_t faceIdx : seamFaces) {
        std::cout << "[UV] Fixing seam face " << faceIdx << ":" << std::endl;

        uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
        uint32_t heIdx = startHE;

        do {
            uint32_t vertIdx = m_halfEdges[heIdx].vertexIndex;
            float theta = cylCoords[vertIdx].theta;
            float height = cylCoords[vertIdx].height;

            if (theta < 0) {
                // Negative theta (near -π) maps to U near 0, but should be U = 1.0
                // Duplicate this vertex and place at U = 1.0
                uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
                m_vertices.push_back(m_vertices[vertIdx]);
                cylCoords[newIdx] = cylCoords[vertIdx];

                // Update half-edge to use new vertex
                m_halfEdges[heIdx].vertexIndex = newIdx;
                m_vertices[newIdx].halfEdgeIndex = heIdx;

                float v = (height - minHeight) / heightRange;
                m_vertices[newIdx].uv = glm::vec2(1.0f, v);

                std::cout << "  Duplicated vertex " << vertIdx << " -> " << newIdx
                          << " (theta=" << theta << "), UV: (1.0, " << v << ")" << std::endl;
                boundaryVertsCount++;
            } else {
                // Positive theta (near +π) already has correct U near 1.0
                glm::vec2 existingUV = m_vertices[vertIdx].uv;
                std::cout << "  Kept vertex " << vertIdx << " (theta=" << theta
                          << "), UV: (" << existingUV.x << ", " << existingUV.y << ")" << std::endl;
            }

            heIdx = m_halfEdges[heIdx].nextIndex;
        } while (heIdx != startHE);
    }

    // Rebuild edge map once after processing all seam faces
    if (!seamFaces.empty()) {
        rebuildEdgeMap();
    }

    // Re-collect target vertices (now includes seam duplicates)
    targetVertSet.clear();
    for (uint32_t faceIdx : selectedFaces) {
        auto verts = getFaceVertices(faceIdx);
        for (uint32_t v : verts) {
            targetVertSet.insert(v);
        }
    }

    // =========================================================================
    // STEP 5: Snap top/bottom row vertices to V=1/V=0
    // =========================================================================
    // After seam vertex duplication, ensure all vertices in top and bottom rows
    // are perfectly aligned. This includes both original and duplicated seam vertices.

    // Find all unique heights and sort them to identify top/bottom rows
    std::vector<std::pair<float, uint32_t>> heightsWithVerts;
    for (uint32_t vIdx : targetVertSet) {
        glm::vec3 pos = m_vertices[vIdx].position - centroid;
        float height = glm::dot(pos, cylinderAxis);
        heightsWithVerts.push_back({height, vIdx});
    }

    // Sort by height
    std::sort(heightsWithVerts.begin(), heightsWithVerts.end());

    // Find distinct height levels (group vertices within small tolerance)
    float groupTolerance = heightRange * 0.05f;  // 5% tolerance
    std::vector<std::vector<uint32_t>> heightGroups;
    std::vector<float> groupHeights;

    for (auto& [height, vIdx] : heightsWithVerts) {
        bool addedToGroup = false;
        for (size_t i = 0; i < heightGroups.size(); ++i) {
            if (std::abs(height - groupHeights[i]) < groupTolerance) {
                heightGroups[i].push_back(vIdx);
                addedToGroup = true;
                break;
            }
        }
        if (!addedToGroup) {
            heightGroups.push_back({vIdx});
            groupHeights.push_back(height);
        }
    }

    std::cout << "[UV] Found " << heightGroups.size() << " height rows" << std::endl << std::flush;

    if (heightGroups.size() >= 2) {
        // Bottom row is first group, top row is last group
        auto& bottomRowVerts = heightGroups.front();
        auto& topRowVerts = heightGroups.back();

        std::cout << "[UV] Top row: " << topRowVerts.size() << " verts, Bottom row: " << bottomRowVerts.size() << " verts" << std::endl << std::flush;

        // Snap top row to V=1
        for (uint32_t vIdx : topRowVerts) {
            glm::vec2 uv = m_vertices[vIdx].uv;
            float oldV = uv.y;
            uv.y = 1.0f;
            m_vertices[vIdx].uv = uv;
            std::cout << "  Top vertex " << vIdx << ": V " << oldV << " -> 1.0" << std::endl;
        }

        // Snap bottom row to V=0
        for (uint32_t vIdx : bottomRowVerts) {
            glm::vec2 uv = m_vertices[vIdx].uv;
            float oldV = uv.y;
            uv.y = 0.0f;
            m_vertices[vIdx].uv = uv;
            std::cout << "  Bottom vertex " << vIdx << ": V " << oldV << " -> 0.0" << std::endl;
        }
        std::cout << std::flush;
    }

    // =========================================================================
    // STEP 7: Pack island to avoid overlap with non-selected faces
    // =========================================================================
    // If we're only projecting selected faces, we need to position the
    // cylindrical UV island so it doesn't overlap with the existing UVs
    // of non-selected faces. This makes the island independently selectable
    // and transformable in the UV editor.

    if (!usingAllFaces) {
        // Find bounding box of non-selected faces' UVs
        glm::vec2 existingMin(FLT_MAX);
        glm::vec2 existingMax(-FLT_MAX);
        bool hasExistingUVs = false;

        for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
            if (selectedFaceSet.count(faceIdx) > 0) continue;  // Skip selected faces

            auto verts = getFaceVertices(faceIdx);
            for (uint32_t v : verts) {
                glm::vec2 uv = m_vertices[v].uv;
                existingMin = glm::min(existingMin, uv);
                existingMax = glm::max(existingMax, uv);
                hasExistingUVs = true;
            }
        }

        if (hasExistingUVs) {
            // Find bounding box of our cylindrical projection
            glm::vec2 cylMin(FLT_MAX);
            glm::vec2 cylMax(-FLT_MAX);
            for (uint32_t faceIdx : selectedFaces) {
                auto verts = getFaceVertices(faceIdx);
                for (uint32_t v : verts) {
                    glm::vec2 uv = m_vertices[v].uv;
                    cylMin = glm::min(cylMin, uv);
                    cylMax = glm::max(cylMax, uv);
                }
            }

            glm::vec2 cylSize = cylMax - cylMin;
            glm::vec2 existingSize = existingMax - existingMin;

            // Scale cylindrical island to fit nicely (use ~40% of available space)
            float targetWidth = 0.45f;
            float scale = targetWidth / std::max(cylSize.x, cylSize.y);
            if (scale > 1.0f) scale = 1.0f;  // Don't scale up

            glm::vec2 scaledSize = cylSize * scale;

            // Position: place to the right of existing UVs with margin
            float margin = 0.02f;
            glm::vec2 newOrigin;

            // Try to fit to the right first
            if (existingMax.x + margin + scaledSize.x <= 1.0f) {
                newOrigin.x = existingMax.x + margin;
                newOrigin.y = 0.5f - scaledSize.y * 0.5f;  // Center vertically
            }
            // Otherwise place below
            else if (existingMax.y + margin + scaledSize.y <= 1.0f) {
                newOrigin.x = 0.5f - scaledSize.x * 0.5f;  // Center horizontally
                newOrigin.y = existingMax.y + margin;
            }
            // Fallback: just offset to top-right corner area
            else {
                newOrigin.x = 1.0f - scaledSize.x - margin;
                newOrigin.y = margin;
            }

            // Apply transform to cylindrical UVs (only process each vertex once!)
            std::set<uint32_t> processedVerts;
            for (uint32_t faceIdx : selectedFaces) {
                auto verts = getFaceVertices(faceIdx);
                for (uint32_t v : verts) {
                    if (processedVerts.count(v) > 0) continue;  // Skip already processed
                    processedVerts.insert(v);

                    glm::vec2 uv = m_vertices[v].uv;
                    // Normalize to [0,1] within the cylindrical island, then scale and offset
                    uv = (uv - cylMin) / (cylMax - cylMin);  // Normalize to [0,1]
                    uv = uv * scaledSize + newOrigin;        // Scale and position
                    m_vertices[v].uv = uv;
                }
            }

            std::cout << "  - Island packed: offset to (" << newOrigin.x << ", " << newOrigin.y
                      << "), scale " << scale << std::endl;
            std::cout << "  - cylMin: (" << cylMin.x << ", " << cylMin.y << "), cylMax: (" << cylMax.x << ", " << cylMax.y << ")" << std::endl;

            // Debug: print seam face final UVs
            if (seamFaceIdx != UINT32_MAX) {
                std::cout << "  - Seam face " << seamFaceIdx << " final UVs:" << std::endl;
                auto seamVerts = getFaceVertices(seamFaceIdx);
                for (uint32_t v : seamVerts) {
                    glm::vec2 uv = m_vertices[v].uv;
                    std::cout << "    Vertex " << v << ": (" << uv.x << ", " << uv.y << ")" << std::endl;
                }
            }
        }
    }

    std::cout << "Cylindrical projection complete:" << std::endl;
    std::cout << "  - " << selectedFaces.size() << " faces processed" << std::endl;
    std::cout << "  - " << boundaryVertsDuplicated << " vertices duplicated at selection boundary" << std::endl;
    std::cout << "  - Cylinder axis: (" << cylinderAxis.x << ", " << cylinderAxis.y << ", " << cylinderAxis.z << ")" << std::endl;
    std::cout << "  - Note: Seam face at theta=±π will appear stretched (expected for basic cylindrical projection)" << std::endl;
}

void EditableMesh::perFaceProjectUVs(float margin) {
    if (m_faces.empty()) return;

    uint32_t numFaces = static_cast<uint32_t>(m_faces.size());

    std::cout << "Per-face UV projection: " << numFaces << " faces" << std::endl;

    // Step 1: Find which vertices are shared by multiple faces and need duplication
    // Map: original vertex index -> list of (faceIdx, localVertIdx in that face)
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> vertexUsage;

    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        auto faceVerts = getFaceVertices(faceIdx);
        for (uint32_t localIdx = 0; localIdx < faceVerts.size(); ++localIdx) {
            vertexUsage[faceVerts[localIdx]].push_back({faceIdx, localIdx});
        }
    }

    // Step 2: Duplicate shared vertices so each face has its own copy
    // Map: (faceIdx, original vertex) -> new vertex index for that face
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> faceVertexMap;

    for (auto& [origVert, usages] : vertexUsage) {
        if (usages.size() == 1) {
            // Only used by one face - no duplication needed
            faceVertexMap[{usages[0].first, origVert}] = origVert;
        } else {
            // Shared by multiple faces - first face keeps original, others get duplicates
            faceVertexMap[{usages[0].first, origVert}] = origVert;

            for (size_t i = 1; i < usages.size(); ++i) {
                // Create duplicate vertex
                HEVertex newVert = m_vertices[origVert];
                newVert.halfEdgeIndex = UINT32_MAX;  // Will be fixed below
                uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
                m_vertices.push_back(newVert);
                faceVertexMap[{usages[i].first, origVert}] = newIdx;
            }
        }
    }

    // Step 3: Update half-edges to point to the new (possibly duplicated) vertices
    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
        uint32_t he = startHE;
        do {
            uint32_t origVert = m_halfEdges[he].vertexIndex;
            auto it = faceVertexMap.find({faceIdx, origVert});
            if (it != faceVertexMap.end()) {
                m_halfEdges[he].vertexIndex = it->second;
                m_vertices[it->second].halfEdgeIndex = he;
            }
            he = m_halfEdges[he].nextIndex;
        } while (he != startHE);
    }

    // Step 4: Calculate grid dimensions
    uint32_t gridCols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(numFaces))));
    uint32_t gridRows = (numFaces + gridCols - 1) / gridCols;

    float cellSize = 1.0f / static_cast<float>(std::max(gridCols, gridRows));
    float innerSize = cellSize - margin * 2.0f;

    std::cout << "  Grid: " << gridCols << "x" << gridRows << ", duplicated to " << m_vertices.size() << " vertices" << std::endl;

    // Step 5: Project each face and assign UVs
    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        auto faceVerts = getFaceVertices(faceIdx);
        if (faceVerts.empty()) continue;

        glm::vec3 normal = getFaceNormal(faceIdx);
        glm::vec3 center = getFaceCenter(faceIdx);

        // Build local coordinate system
        glm::vec3 tangent;
        if (std::abs(normal.y) < 0.9f) {
            tangent = glm::normalize(glm::cross(normal, glm::vec3(0, 1, 0)));
        } else {
            tangent = glm::normalize(glm::cross(normal, glm::vec3(1, 0, 0)));
        }
        glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));

        // Project vertices
        std::vector<glm::vec2> localUVs;
        glm::vec2 minUV(FLT_MAX), maxUV(-FLT_MAX);

        for (uint32_t vIdx : faceVerts) {
            glm::vec3 relPos = m_vertices[vIdx].position - center;
            glm::vec2 localUV(glm::dot(relPos, tangent), glm::dot(relPos, bitangent));
            localUVs.push_back(localUV);
            minUV = glm::min(minUV, localUV);
            maxUV = glm::max(maxUV, localUV);
        }

        glm::vec2 range = maxUV - minUV;
        float maxRange = std::max(range.x, range.y);
        if (maxRange < 0.0001f) maxRange = 1.0f;

        // Grid cell position
        uint32_t col = faceIdx % gridCols;
        uint32_t row = faceIdx / gridCols;
        glm::vec2 cellOrigin(col * cellSize + margin, row * cellSize + margin);

        // Assign UVs
        for (size_t i = 0; i < faceVerts.size(); ++i) {
            glm::vec2 normalizedUV = (localUVs[i] - minUV) / maxRange;
            glm::vec2 offset = cellOrigin;
            offset.x += (innerSize - range.x / maxRange * innerSize) * 0.5f;
            offset.y += (innerSize - range.y / maxRange * innerSize) * 0.5f;

            m_vertices[faceVerts[i]].uv = normalizedUV * innerSize + offset;
        }
    }

    std::cout << "Per-face UV projection complete" << std::endl;
}

void EditableMesh::uniformSquareUVs(float margin) {
    if (m_faces.empty()) return;

    uint32_t numFaces = static_cast<uint32_t>(m_faces.size());

    std::cout << "Uniform Square UV projection: " << numFaces << " faces" << std::endl;

    // Step 1: Find which vertices are shared by multiple faces and need duplication
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> vertexUsage;

    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        auto faceVerts = getFaceVertices(faceIdx);
        for (uint32_t localIdx = 0; localIdx < faceVerts.size(); ++localIdx) {
            vertexUsage[faceVerts[localIdx]].push_back({faceIdx, localIdx});
        }
    }

    // Step 2: Duplicate shared vertices so each face has its own copy
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> faceVertexMap;

    for (auto& [origVert, usages] : vertexUsage) {
        if (usages.size() == 1) {
            faceVertexMap[{usages[0].first, origVert}] = origVert;
        } else {
            faceVertexMap[{usages[0].first, origVert}] = origVert;

            for (size_t i = 1; i < usages.size(); ++i) {
                HEVertex newVert = m_vertices[origVert];
                newVert.halfEdgeIndex = UINT32_MAX;
                uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
                m_vertices.push_back(newVert);
                faceVertexMap[{usages[i].first, origVert}] = newIdx;
            }
        }
    }

    // Step 3: Update half-edges to point to the new vertices
    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
        uint32_t he = startHE;
        do {
            uint32_t origVert = m_halfEdges[he].vertexIndex;
            auto it = faceVertexMap.find({faceIdx, origVert});
            if (it != faceVertexMap.end()) {
                m_halfEdges[he].vertexIndex = it->second;
                m_vertices[it->second].halfEdgeIndex = he;
            }
            he = m_halfEdges[he].nextIndex;
        } while (he != startHE);
    }

    // Step 4: Calculate grid dimensions
    uint32_t gridCols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(numFaces))));
    uint32_t gridRows = (numFaces + gridCols - 1) / gridCols;

    float cellSize = 1.0f / static_cast<float>(std::max(gridCols, gridRows));
    float innerSize = cellSize - margin * 2.0f;

    std::cout << "  Grid: " << gridCols << "x" << gridRows << ", cell size: " << cellSize << std::endl;

    // Step 5: Map each face to a PERFECT SQUARE (or right triangle for 3-vert faces)
    for (uint32_t faceIdx = 0; faceIdx < numFaces; ++faceIdx) {
        auto faceVerts = getFaceVertices(faceIdx);
        if (faceVerts.empty()) continue;

        // Grid cell position
        uint32_t col = faceIdx % gridCols;
        uint32_t row = faceIdx / gridCols;
        glm::vec2 cellOrigin(col * cellSize + margin, row * cellSize + margin);

        size_t vertCount = faceVerts.size();

        if (vertCount == 3) {
            // Triangle: map to a right triangle (half of square)
            // Vertex 0 -> bottom-left (0, 0)
            // Vertex 1 -> bottom-right (1, 0)
            // Vertex 2 -> top-left (0, 1)
            m_vertices[faceVerts[0]].uv = cellOrigin + glm::vec2(0, 0);
            m_vertices[faceVerts[1]].uv = cellOrigin + glm::vec2(innerSize, 0);
            m_vertices[faceVerts[2]].uv = cellOrigin + glm::vec2(0, innerSize);
        } else if (vertCount == 4) {
            // Quad: map to a perfect square
            // Vertex 0 -> bottom-left (0, 0)
            // Vertex 1 -> bottom-right (1, 0)
            // Vertex 2 -> top-right (1, 1)
            // Vertex 3 -> top-left (0, 1)
            m_vertices[faceVerts[0]].uv = cellOrigin + glm::vec2(0, 0);
            m_vertices[faceVerts[1]].uv = cellOrigin + glm::vec2(innerSize, 0);
            m_vertices[faceVerts[2]].uv = cellOrigin + glm::vec2(innerSize, innerSize);
            m_vertices[faceVerts[3]].uv = cellOrigin + glm::vec2(0, innerSize);
        } else {
            // N-gon: distribute vertices evenly around a square
            for (size_t i = 0; i < vertCount; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(vertCount);
                // Map to perimeter of square
                float perimeter = t * 4.0f;
                glm::vec2 uv;
                if (perimeter < 1.0f) {
                    uv = glm::vec2(perimeter, 0);  // Bottom edge
                } else if (perimeter < 2.0f) {
                    uv = glm::vec2(1.0f, perimeter - 1.0f);  // Right edge
                } else if (perimeter < 3.0f) {
                    uv = glm::vec2(3.0f - perimeter, 1.0f);  // Top edge
                } else {
                    uv = glm::vec2(0, 4.0f - perimeter);  // Left edge
                }
                m_vertices[faceVerts[i]].uv = cellOrigin + uv * innerSize;
            }
        }
    }

    std::cout << "Uniform Square UV projection complete - all faces mapped to identical squares" << std::endl;
}

int EditableMesh::sewAllUVs(const std::vector<uint32_t>& targetFaces) {
    if (m_faces.empty()) return 0;

    // Determine which faces to process
    std::set<uint32_t> facesToProcess;
    if (targetFaces.empty()) {
        // No selection - process all faces
        for (uint32_t i = 0; i < m_faces.size(); ++i) {
            facesToProcess.insert(i);
        }
        std::cout << "Sew All UVs: Processing ALL " << facesToProcess.size() << " faces" << std::endl;
    } else {
        // Use provided selection
        facesToProcess.insert(targetFaces.begin(), targetFaces.end());
        std::cout << "Sew All UVs: Processing " << facesToProcess.size() << " SELECTED faces" << std::endl;
    }

    // Step 0: For selected-faces mode, move NON-selected faces to a "parking" area
    // They stay visible (like puzzle pieces waiting to be placed) but outside 0-1 workspace
    if (!targetFaces.empty()) {
        // Find vertices used ONLY by non-selected faces
        std::set<uint32_t> nonSelectedVerts;
        for (uint32_t f = 0; f < m_faces.size(); ++f) {
            if (facesToProcess.count(f) == 0) {
                auto fv = getFaceVertices(f);
                nonSelectedVerts.insert(fv.begin(), fv.end());
            }
        }
        // Remove vertices that are also used by selected faces
        for (uint32_t f : facesToProcess) {
            auto fv = getFaceVertices(f);
            for (uint32_t v : fv) {
                nonSelectedVerts.erase(v);
            }
        }
        // Move to parking area (right of 0-1 space, visible if user pans)
        for (uint32_t v : nonSelectedVerts) {
            m_vertices[v].uv.x += 1.5f;  // Park just outside 0-1 space
        }
        std::cout << "Sew All UVs: Parked " << nonSelectedVerts.size() << " non-selected vertices outside 0-1 (puzzle pieces waiting)" << std::endl;
    }

    std::cout << "Sew All UVs: Starting with per-face projection..." << std::endl;

    // Step 1: Apply per-face UVs ONLY to target faces
    // First, duplicate shared vertices so each face has its own UV coords
    {
        // Track which vertices are used by which target faces
        std::map<uint32_t, std::vector<uint32_t>> vertexToFaces;
        for (uint32_t faceIdx : facesToProcess) {
            auto fv = getFaceVertices(faceIdx);
            for (uint32_t v : fv) {
                vertexToFaces[v].push_back(faceIdx);
            }
        }

        // Track which vertices are also used by NON-selected faces
        std::set<uint32_t> vertsUsedByNonSelected;
        for (uint32_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
            if (facesToProcess.count(faceIdx) > 0) continue;  // Skip selected faces
            auto fv = getFaceVertices(faceIdx);
            for (uint32_t v : fv) {
                if (vertexToFaces.count(v) > 0) {
                    vertsUsedByNonSelected.insert(v);
                }
            }
        }

        // Map: (faceIdx, original vertex) -> new vertex index for that face
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> faceVertexMap;

        for (auto& [origVert, faceList] : vertexToFaces) {
            bool sharedWithNonSelected = vertsUsedByNonSelected.count(origVert) > 0;

            if (faceList.size() == 1 && !sharedWithNonSelected) {
                // Only used by one target face AND not shared with non-selected - no duplication needed
                faceVertexMap[{faceList[0], origVert}] = origVert;
            } else {
                // Either shared by multiple target faces OR shared with non-selected faces
                // All target faces get their own duplicate vertex
                for (size_t i = 0; i < faceList.size(); ++i) {
                    // Create duplicate vertex for each target face
                    HEVertex newVert = m_vertices[origVert];
                    newVert.halfEdgeIndex = UINT32_MAX;
                    uint32_t newIdx = static_cast<uint32_t>(m_vertices.size());
                    m_vertices.push_back(newVert);
                    faceVertexMap[{faceList[i], origVert}] = newIdx;
                }
            }
        }

        // Update half-edges to point to new vertices
        for (uint32_t faceIdx : facesToProcess) {
            uint32_t startHE = m_faces[faceIdx].halfEdgeIndex;
            uint32_t he = startHE;
            do {
                uint32_t origVert = m_halfEdges[he].vertexIndex;
                auto it = faceVertexMap.find({faceIdx, origVert});
                if (it != faceVertexMap.end()) {
                    m_halfEdges[he].vertexIndex = it->second;
                    m_vertices[it->second].halfEdgeIndex = he;
                }
                he = m_halfEdges[he].nextIndex;
            } while (he != startHE);
        }

        std::cout << "Sew All UVs: Duplicated vertices, now have " << m_vertices.size() << " vertices" << std::endl;

        // Now project each face by normal
        uint32_t numTargetFaces = static_cast<uint32_t>(facesToProcess.size());
        uint32_t gridCols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(numTargetFaces))));
        float cellSize = 1.0f / static_cast<float>(gridCols);
        float innerSize = cellSize * 0.95f;

        uint32_t faceCount = 0;
        for (uint32_t faceIdx : facesToProcess) {
            auto faceVerts = getFaceVertices(faceIdx);
            if (faceVerts.empty()) continue;

            glm::vec3 normal = getFaceNormal(faceIdx);
            glm::vec3 center = getFaceCenter(faceIdx);

            // Build local coordinate system
            glm::vec3 tangent;
            if (std::abs(normal.y) < 0.9f) {
                tangent = glm::normalize(glm::cross(normal, glm::vec3(0, 1, 0)));
            } else {
                tangent = glm::normalize(glm::cross(normal, glm::vec3(1, 0, 0)));
            }
            glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));

            // Project vertices
            std::vector<glm::vec2> localUVs;
            glm::vec2 minUV(FLT_MAX), maxUV(-FLT_MAX);

            for (uint32_t vIdx : faceVerts) {
                glm::vec3 relPos = m_vertices[vIdx].position - center;
                glm::vec2 localUV(glm::dot(relPos, tangent), glm::dot(relPos, bitangent));
                localUVs.push_back(localUV);
                minUV = glm::min(minUV, localUV);
                maxUV = glm::max(maxUV, localUV);
            }

            glm::vec2 range = maxUV - minUV;
            float maxRange = std::max(range.x, range.y);
            if (maxRange < 0.0001f) maxRange = 1.0f;

            // Grid cell position
            uint32_t col = faceCount % gridCols;
            uint32_t row = faceCount / gridCols;
            glm::vec2 cellOrigin(col * cellSize, row * cellSize);

            // Assign UVs
            for (size_t i = 0; i < faceVerts.size(); ++i) {
                glm::vec2 normalizedUV = (localUVs[i] - minUV) / maxRange;
                m_vertices[faceVerts[i]].uv = normalizedUV * innerSize + cellOrigin;
            }

            faceCount++;
        }
    }

    // Step 2: Move target faces outside UV space (offset by +2 in U)
    {
        std::set<uint32_t> targetVerts;
        for (uint32_t faceIdx : facesToProcess) {
            auto fv = getFaceVertices(faceIdx);
            targetVerts.insert(fv.begin(), fv.end());
        }
        for (uint32_t v : targetVerts) {
            m_vertices[v].uv.x += 2.0f;
        }
    }

    std::cout << "Sew All UVs: Moved target faces outside 0-1 space" << std::endl;

    // Step 3: Build list of shared edges (only between target faces)
    std::vector<uint32_t> sharedEdges;
    for (uint32_t heIdx = 0; heIdx < m_halfEdges.size(); ++heIdx) {
        uint32_t twinIdx = m_halfEdges[heIdx].twinIndex;
        if (twinIdx != UINT32_MAX && heIdx < twinIdx) {
            // Both faces must be in target set
            uint32_t faceA = m_halfEdges[heIdx].faceIndex;
            uint32_t faceB = m_halfEdges[twinIdx].faceIndex;
            if (facesToProcess.count(faceA) && facesToProcess.count(faceB)) {
                sharedEdges.push_back(heIdx);
            }
        }
    }

    std::cout << "Sew All UVs: Found " << sharedEdges.size() << " shared edges between target faces" << std::endl;

    // Step 3.5: Find existing faces already in 0-1 UV space (from previous sew operations)
    // These need to be included in overlap checking but won't be sewn
    std::set<uint32_t> existingFacesInUV;
    for (uint32_t f = 0; f < m_faces.size(); ++f) {
        if (facesToProcess.count(f) > 0) continue;  // Skip faces we're processing

        auto verts = getFaceVertices(f);
        if (verts.empty()) continue;

        // Check if face centroid is in 0-2 range (reasonable UV space)
        glm::vec2 centroid(0.0f);
        for (uint32_t v : verts) {
            centroid += m_vertices[v].uv;
        }
        centroid /= static_cast<float>(verts.size());

        // If centroid is in reasonable UV range, consider it an existing island
        if (centroid.x >= -0.5f && centroid.x <= 1.5f &&
            centroid.y >= -0.5f && centroid.y <= 2.0f) {
            existingFacesInUV.insert(f);
        }
    }

    if (!existingFacesInUV.empty()) {
        std::cout << "Sew All UVs: Found " << existingFacesInUV.size() << " existing faces in UV space to check against" << std::endl;
    }

    // Step 4: Track which faces are "in" the 0-1 working area
    std::set<uint32_t> facesInWorkArea;

    // Helper: get all vertex indices used by a face
    auto getFaceVerts = [&](uint32_t faceIdx) -> std::set<uint32_t> {
        std::set<uint32_t> verts;
        auto fv = getFaceVertices(faceIdx);
        verts.insert(fv.begin(), fv.end());
        return verts;
    };

    // Helper: get all vertex indices used by a set of faces
    auto getFacesVerts = [&](const std::set<uint32_t>& faces) -> std::set<uint32_t> {
        std::set<uint32_t> verts;
        for (uint32_t faceIdx : faces) {
            auto fv = getFaceVertices(faceIdx);
            verts.insert(fv.begin(), fv.end());
        }
        return verts;
    };

    // Helper: check if a point is inside a triangle (2D)
    auto pointInTriangle = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) -> bool {
        auto sign = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) -> float {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };
        float d1 = sign(p, a, b);
        float d2 = sign(p, b, c);
        float d3 = sign(p, c, a);
        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(hasNeg && hasPos);
    };

    // Helper: check if two line segments intersect (2D)
    auto segmentsIntersect = [](const glm::vec2& a1, const glm::vec2& a2,
                                 const glm::vec2& b1, const glm::vec2& b2) -> bool {
        auto ccw = [](const glm::vec2& A, const glm::vec2& B, const glm::vec2& C) -> bool {
            return (C.y - A.y) * (B.x - A.x) > (B.y - A.y) * (C.x - A.x);
        };
        return ccw(a1, b1, b2) != ccw(a2, b1, b2) && ccw(a1, a2, b1) != ccw(a1, a2, b2);
    };

    // Helper: check if two triangles overlap in UV space (excluding shared edges)
    auto trianglesOverlap = [&](const glm::vec2& a0, const glm::vec2& a1, const glm::vec2& a2,
                                 const glm::vec2& b0, const glm::vec2& b1, const glm::vec2& b2,
                                 float epsilon = 0.0001f) -> bool {
        // Skip degenerate triangles
        float areaA = std::abs((a1.x - a0.x) * (a2.y - a0.y) - (a2.x - a0.x) * (a1.y - a0.y));
        float areaB = std::abs((b1.x - b0.x) * (b2.y - b0.y) - (b2.x - b0.x) * (b1.y - b0.y));
        if (areaA < epsilon || areaB < epsilon) return false;

        // Check for coincident/stacked triangles (centroids nearly identical)
        glm::vec2 centroidA = (a0 + a1 + a2) / 3.0f;
        glm::vec2 centroidB = (b0 + b1 + b2) / 3.0f;
        float centroidDist = glm::length(centroidA - centroidB);
        // If centroids are very close and triangles have similar area, they're likely stacked
        if (centroidDist < 0.001f && std::abs(areaA - areaB) < 0.001f) {
            return true;  // Coincident triangles
        }

        // Check if any vertex of one triangle is strictly inside the other
        // Use a small inset to avoid false positives at shared edges
        auto pointStrictlyInTriangle = [&](const glm::vec2& p, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2) -> bool {
            auto sign = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) -> float {
                return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
            };
            float d1 = sign(p, t0, t1);
            float d2 = sign(p, t1, t2);
            float d3 = sign(p, t2, t0);
            // Strictly inside means all same sign with margin
            bool allPos = (d1 > epsilon) && (d2 > epsilon) && (d3 > epsilon);
            bool allNeg = (d1 < -epsilon) && (d2 < -epsilon) && (d3 < -epsilon);
            return allPos || allNeg;
        };

        if (pointStrictlyInTriangle(a0, b0, b1, b2) ||
            pointStrictlyInTriangle(a1, b0, b1, b2) ||
            pointStrictlyInTriangle(a2, b0, b1, b2))
            return true;
        if (pointStrictlyInTriangle(b0, a0, a1, a2) ||
            pointStrictlyInTriangle(b1, a0, a1, a2) ||
            pointStrictlyInTriangle(b2, a0, a1, a2))
            return true;

        // Check if edges cross (not just touch)
        auto edgesCross = [&](const glm::vec2& p1, const glm::vec2& p2,
                              const glm::vec2& p3, const glm::vec2& p4) -> bool {
            // Check if segments properly intersect (not just at endpoints)
            glm::vec2 d1 = p2 - p1;
            glm::vec2 d2 = p4 - p3;
            float cross = d1.x * d2.y - d1.y * d2.x;
            if (std::abs(cross) < epsilon) return false;  // Parallel

            glm::vec2 d3 = p3 - p1;
            float t = (d3.x * d2.y - d3.y * d2.x) / cross;
            float u = (d3.x * d1.y - d3.y * d1.x) / cross;

            // Strictly between 0 and 1 (not at endpoints)
            return t > epsilon && t < 1.0f - epsilon && u > epsilon && u < 1.0f - epsilon;
        };

        glm::vec2 aEdges[3][2] = {{a0, a1}, {a1, a2}, {a2, a0}};
        glm::vec2 bEdges[3][2] = {{b0, b1}, {b1, b2}, {b2, b0}};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (edgesCross(aEdges[i][0], aEdges[i][1], bEdges[j][0], bEdges[j][1]))
                    return true;
            }
        }
        return false;
    };

    // Helper: get triangulated UVs for a face
    auto getFaceTrianglesUV = [&](uint32_t faceIdx) -> std::vector<std::array<glm::vec2, 3>> {
        std::vector<std::array<glm::vec2, 3>> tris;
        auto verts = getFaceVertices(faceIdx);
        if (verts.size() < 3) return tris;

        glm::vec2 v0 = m_vertices[verts[0]].uv;
        for (size_t i = 1; i + 1 < verts.size(); ++i) {
            tris.push_back({v0, m_vertices[verts[i]].uv, m_vertices[verts[i + 1]].uv});
        }
        return tris;
    };

    // Step 5: Bring first target face into 0-1 space, centered
    if (!facesToProcess.empty()) {
        uint32_t firstFace = *facesToProcess.begin();
        auto firstFaceVerts = getFaceVerts(firstFace);

        // Find bounds of first face
        glm::vec2 minUV(FLT_MAX), maxUV(-FLT_MAX);
        for (uint32_t v : firstFaceVerts) {
            minUV = glm::min(minUV, m_vertices[v].uv);
            maxUV = glm::max(maxUV, m_vertices[v].uv);
        }

        // Center it in 0-1 space
        glm::vec2 center = (minUV + maxUV) * 0.5f;
        glm::vec2 offset = glm::vec2(0.5f, 0.5f) - center;

        for (uint32_t v : firstFaceVerts) {
            m_vertices[v].uv += offset;
        }

        facesInWorkArea.insert(firstFace);
        std::cout << "Sew All UVs: Placed first target face (" << firstFace << ") in 0-1 space" << std::endl;
    }

    int sewnCount = 0;
    bool progress = true;
    int iterations = 0;
    const int maxIterations = static_cast<int>(m_faces.size()) * 2;  // Safety limit

    // Step 6: Iteratively sew edges where one face is in work area and one is outside
    while (progress && iterations < maxIterations) {
        progress = false;
        iterations++;

        for (uint32_t heIdx : sharedEdges) {
            uint32_t twinIdx = m_halfEdges[heIdx].twinIndex;
            if (twinIdx == UINT32_MAX) continue;

            uint32_t faceA = m_halfEdges[heIdx].faceIndex;
            uint32_t faceB = m_halfEdges[twinIdx].faceIndex;

            if (faceA == UINT32_MAX || faceB == UINT32_MAX) continue;

            // We need exactly one face in the work area and one outside
            bool aIn = facesInWorkArea.count(faceA) > 0;
            bool bIn = facesInWorkArea.count(faceB) > 0;

            if (aIn == bIn) continue;  // Both in or both out - skip

            // Make faceA the one in the work area, faceB the one to bring in
            if (!aIn) {
                std::swap(faceA, faceB);
                std::swap(heIdx, twinIdx);
            }

            // Get edge vertices
            uint32_t vertA_to = m_halfEdges[heIdx].vertexIndex;
            uint32_t vertA_from = m_halfEdges[m_halfEdges[heIdx].prevIndex].vertexIndex;
            uint32_t vertB_to = m_halfEdges[twinIdx].vertexIndex;
            uint32_t vertB_from = m_halfEdges[m_halfEdges[twinIdx].prevIndex].vertexIndex;

            // Get current UVs
            glm::vec2 uvA_from = m_vertices[vertA_from].uv;
            glm::vec2 uvA_to = m_vertices[vertA_to].uv;
            glm::vec2 uvB_from = m_vertices[vertB_from].uv;
            glm::vec2 uvB_to = m_vertices[vertB_to].uv;

            // Edge vectors (B's edge is opposite direction)
            glm::vec2 edgeA = uvA_to - uvA_from;
            glm::vec2 edgeB = uvB_from - uvB_to;

            float lenA = glm::length(edgeA);
            float lenB = glm::length(edgeB);

            if (lenA < 0.0001f || lenB < 0.0001f) continue;

            // Calculate transform: scale + rotate + translate
            float scaleFactor = lenA / lenB;
            float angleA = std::atan2(edgeA.y, edgeA.x);
            float angleB = std::atan2(edgeB.y, edgeB.x);
            float rotAngle = angleA - angleB;

            // Get vertices of face B
            auto faceBVerts = getFaceVerts(faceB);

            // Store original UVs
            std::map<uint32_t, glm::vec2> originalUVs;
            for (uint32_t v : faceBVerts) {
                originalUVs[v] = m_vertices[v].uv;
            }

            // Apply transform to face B
            glm::vec2 pivot = uvB_to;
            float cosR = std::cos(rotAngle);
            float sinR = std::sin(rotAngle);

            for (uint32_t v : faceBVerts) {
                glm::vec2 uv = m_vertices[v].uv;
                uv -= pivot;
                uv *= scaleFactor;
                glm::vec2 rotated(uv.x * cosR - uv.y * sinR, uv.x * sinR + uv.y * cosR);
                rotated += uvA_from;
                m_vertices[v].uv = rotated;
            }

            // Simple overlap check: each face gets a "number" (its centroid position)
            // No two faces can have the same number (centroid too close)

            // Calculate centroid of face B after transform
            auto faceBVertsVec = getFaceVertices(faceB);
            glm::vec2 faceBCentroid(0.0f);
            for (uint32_t v : faceBVertsVec) {
                faceBCentroid += m_vertices[v].uv;
            }
            faceBCentroid /= static_cast<float>(faceBVertsVec.size());

            // Also calculate face B's approximate size (for adaptive threshold)
            glm::vec2 minB(FLT_MAX), maxB(-FLT_MAX);
            for (uint32_t v : faceBVertsVec) {
                minB = glm::min(minB, m_vertices[v].uv);
                maxB = glm::max(maxB, m_vertices[v].uv);
            }
            float faceBSize = glm::length(maxB - minB) * 0.5f;  // Half diagonal as size estimate

            // Threshold: if centroids are closer than half the face size, it's overlap
            float overlapThreshold = std::max(0.001f, faceBSize * 0.4f);

            bool hasOverlap = false;

            // Check against faces we're sewing in this operation
            for (uint32_t existingFace : facesInWorkArea) {
                if (existingFace == faceA) continue;  // Skip the face we're sewing to

                auto existingVertsVec = getFaceVertices(existingFace);
                glm::vec2 existingCentroid(0.0f);
                for (uint32_t v : existingVertsVec) {
                    existingCentroid += m_vertices[v].uv;
                }
                existingCentroid /= static_cast<float>(existingVertsVec.size());

                float dist = glm::length(faceBCentroid - existingCentroid);
                if (dist < overlapThreshold) {
                    hasOverlap = true;
                    break;
                }
            }

            // Also check against faces from PREVIOUS sew operations (existing UV islands)
            if (!hasOverlap) {
                for (uint32_t existingFace : existingFacesInUV) {
                    auto existingVertsVec = getFaceVertices(existingFace);
                    glm::vec2 existingCentroid(0.0f);
                    for (uint32_t v : existingVertsVec) {
                        existingCentroid += m_vertices[v].uv;
                    }
                    existingCentroid /= static_cast<float>(existingVertsVec.size());

                    float dist = glm::length(faceBCentroid - existingCentroid);
                    if (dist < overlapThreshold) {
                        hasOverlap = true;
                        break;
                    }
                }
            }

            if (hasOverlap) {
                // Revert UVs
                for (auto& [v, uv] : originalUVs) {
                    m_vertices[v].uv = uv;
                }
            } else {
                // Success! Add face B to work area
                facesInWorkArea.insert(faceB);
                sewnCount++;
                progress = true;
            }
        }
    }

    std::cout << "Sew All UVs: Successfully sewn " << sewnCount << " edges, "
              << facesInWorkArea.size() << " faces in UV island" << std::endl;

    // Step 7: Pack rejected faces back into visible UV space
    // Find target faces that are still outside (not in work area)
    std::vector<uint32_t> rejectedFaces;
    for (uint32_t f : facesToProcess) {
        if (facesInWorkArea.count(f) == 0) {
            rejectedFaces.push_back(f);
        }
    }

    if (!rejectedFaces.empty()) {
        std::cout << "Sew All UVs: " << rejectedFaces.size() << " faces rejected (would overlap)" << std::endl;

        // Find bounds of the main sewn island
        glm::vec2 mainMin(FLT_MAX), mainMax(-FLT_MAX);
        for (uint32_t f : facesInWorkArea) {
            auto verts = getFaceVertices(f);
            for (uint32_t v : verts) {
                mainMin = glm::min(mainMin, m_vertices[v].uv);
                mainMax = glm::max(mainMax, m_vertices[v].uv);
            }
        }

        // Pack rejected faces in a row below the main island
        float packX = 0.0f;
        float packY = mainMax.y + 0.05f;  // Start below main island with margin
        float rowHeight = 0.0f;
        float maxRowWidth = 1.0f;  // Try to keep within 0-1 width

        for (uint32_t faceIdx : rejectedFaces) {
            auto verts = getFaceVertices(faceIdx);
            if (verts.empty()) continue;

            // Find this face's current bounds (still at +2 offset)
            glm::vec2 faceMin(FLT_MAX), faceMax(-FLT_MAX);
            for (uint32_t v : verts) {
                faceMin = glm::min(faceMin, m_vertices[v].uv);
                faceMax = glm::max(faceMax, m_vertices[v].uv);
            }

            float faceWidth = faceMax.x - faceMin.x;
            float faceHeight = faceMax.y - faceMin.y;

            // Check if we need to start a new row
            if (packX + faceWidth > maxRowWidth && packX > 0.0f) {
                packX = 0.0f;
                packY += rowHeight + 0.02f;
                rowHeight = 0.0f;
            }

            // Move face to pack position
            glm::vec2 offset = glm::vec2(packX, packY) - faceMin;
            for (uint32_t v : verts) {
                m_vertices[v].uv += offset;
            }

            packX += faceWidth + 0.02f;  // Margin between faces
            rowHeight = std::max(rowHeight, faceHeight);
        }

        std::cout << "Sew All UVs: Packed rejected faces below main island (starting at Y=" << (mainMax.y + 0.05f) << ")" << std::endl;
    }

    // Non-selected faces remain parked outside 0-1 space (puzzle pieces waiting to be placed)
    // User can pan over to see them and select them for the next sew operation

    return sewnCount;
}

bool EditableMesh::saveLime(const std::string& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for writing" << std::endl;
        return false;
    }

    file << "# LIME Model Format v1.0\n";
    file << "# Human-readable mesh debug format\n\n";

    // Vertices
    file << "# VERTICES: " << m_vertices.size() << "\n";
    file << "# idx: pos.x pos.y pos.z | nrm.x nrm.y nrm.z | uv.u uv.v | halfEdgeIdx selected\n";
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const auto& v = m_vertices[i];
        file << "v " << i << ": "
             << v.position.x << " " << v.position.y << " " << v.position.z << " | "
             << v.normal.x << " " << v.normal.y << " " << v.normal.z << " | "
             << v.uv.x << " " << v.uv.y << " | "
             << v.halfEdgeIndex << " " << (v.selected ? 1 : 0) << "\n";
    }
    file << "\n";

    // Faces
    file << "# FACES: " << m_faces.size() << "\n";
    file << "# idx: halfEdgeIdx vertexCount selected | vertex_indices...\n";
    for (size_t i = 0; i < m_faces.size(); ++i) {
        const auto& f = m_faces[i];
        auto verts = getFaceVertices(static_cast<uint32_t>(i));
        file << "f " << i << ": " << f.halfEdgeIndex << " " << f.vertexCount << " "
             << (f.selected ? 1 : 0) << " |";
        for (uint32_t v : verts) {
            file << " " << v;
        }
        file << "\n";
    }
    file << "\n";

    // Half-edges
    file << "# HALF_EDGES: " << m_halfEdges.size() << "\n";
    file << "# idx: vertexIndex faceIndex nextIndex prevIndex twinIndex\n";
    for (size_t i = 0; i < m_halfEdges.size(); ++i) {
        const auto& he = m_halfEdges[i];
        file << "he " << i << ": "
             << he.vertexIndex << " "
             << he.faceIndex << " "
             << he.nextIndex << " "
             << he.prevIndex << " "
             << he.twinIndex << "\n";
    }
    file << "\n";

    // Control points
    if (!m_controlPoints.empty()) {
        file << "# CONTROL_POINTS: " << m_controlPoints.size() << "\n";
        for (size_t i = 0; i < m_controlPoints.size(); ++i) {
            file << "cp " << i << ": " << m_controlPoints[i].vertexIndex << " \"" << m_controlPoints[i].name << "\"\n";
        }
        file << "\n";
    }

    // Connection ports
    if (!m_ports.empty()) {
        file << "# PORTS: " << m_ports.size() << "\n";
        for (size_t i = 0; i < m_ports.size(); ++i) {
            const auto& p = m_ports[i];
            file << "port " << i << ": \"" << p.name << "\" "
                 << p.position.x << " " << p.position.y << " " << p.position.z << " | "
                 << p.forward.x << " " << p.forward.y << " " << p.forward.z << " | "
                 << p.up.x << " " << p.up.y << " " << p.up.z << "\n";
        }
        file << "\n";
    }

    // Summary for quick debugging
    file << "# SUMMARY\n";
    file << "# Total vertices: " << m_vertices.size() << "\n";
    file << "# Total faces: " << m_faces.size() << "\n";
    file << "# Total half-edges: " << m_halfEdges.size() << "\n";

    // Count quads vs tris
    int quadCount = 0, triCount = 0, otherCount = 0;
    for (const auto& f : m_faces) {
        if (f.vertexCount == 4) quadCount++;
        else if (f.vertexCount == 3) triCount++;
        else otherCount++;
    }
    file << "# Quads: " << quadCount << ", Tris: " << triCount << ", Other: " << otherCount << "\n";

    // Count twins linked
    int twinsLinked = 0;
    for (const auto& he : m_halfEdges) {
        if (he.twinIndex != UINT32_MAX) twinsLinked++;
    }
    file << "# Half-edges with twins: " << twinsLinked << " / " << m_halfEdges.size() << "\n";

    file.close();
    std::cout << "Saved mesh to " << filepath << std::endl;
    return true;
}

bool EditableMesh::saveLime(const std::string& filepath, const unsigned char* textureData, int texWidth, int texHeight) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for writing" << std::endl;
        return false;
    }

    file << "# LIME Model Format v2.0\n";
    file << "# Half-edge mesh with embedded texture\n\n";

    // Texture section (if provided)
    if (textureData && texWidth > 0 && texHeight > 0) {
        size_t texSize = static_cast<size_t>(texWidth) * texHeight * 4;  // RGBA
        std::string encoded = base64_encode(textureData, texSize);
        file << "# TEXTURE\n";
        file << "tex_size: " << texWidth << " " << texHeight << "\n";
        file << "tex_data: " << encoded << "\n\n";
    }

    // Vertices
    file << "# VERTICES: " << m_vertices.size() << "\n";
    file << "# idx: pos.x pos.y pos.z | nrm.x nrm.y nrm.z | uv.u uv.v | col.r col.g col.b col.a | halfEdgeIdx selected\n";
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const auto& v = m_vertices[i];
        file << "v " << i << ": "
             << v.position.x << " " << v.position.y << " " << v.position.z << " | "
             << v.normal.x << " " << v.normal.y << " " << v.normal.z << " | "
             << v.uv.x << " " << v.uv.y << " | "
             << v.color.r << " " << v.color.g << " " << v.color.b << " " << v.color.a << " | "
             << v.halfEdgeIndex << " " << (v.selected ? 1 : 0) << "\n";
    }
    file << "\n";

    // Faces
    file << "# FACES: " << m_faces.size() << "\n";
    file << "# idx: halfEdgeIdx vertexCount selected | vertex_indices...\n";
    for (size_t i = 0; i < m_faces.size(); ++i) {
        const auto& f = m_faces[i];
        auto verts = getFaceVertices(static_cast<uint32_t>(i));
        file << "f " << i << ": " << f.halfEdgeIndex << " " << f.vertexCount << " "
             << (f.selected ? 1 : 0) << " |";
        for (uint32_t v : verts) {
            file << " " << v;
        }
        file << "\n";
    }
    file << "\n";

    // Half-edges
    file << "# HALF_EDGES: " << m_halfEdges.size() << "\n";
    file << "# idx: vertexIndex faceIndex nextIndex prevIndex twinIndex\n";
    for (size_t i = 0; i < m_halfEdges.size(); ++i) {
        const auto& he = m_halfEdges[i];
        file << "he " << i << ": "
             << he.vertexIndex << " "
             << he.faceIndex << " "
             << he.nextIndex << " "
             << he.prevIndex << " "
             << he.twinIndex << "\n";
    }
    file << "\n";

    // Control points
    if (!m_controlPoints.empty()) {
        file << "# CONTROL_POINTS: " << m_controlPoints.size() << "\n";
        for (size_t i = 0; i < m_controlPoints.size(); ++i) {
            file << "cp " << i << ": " << m_controlPoints[i].vertexIndex << " \"" << m_controlPoints[i].name << "\"\n";
        }
        file << "\n";
    }

    // Connection ports
    if (!m_ports.empty()) {
        file << "# PORTS: " << m_ports.size() << "\n";
        for (size_t i = 0; i < m_ports.size(); ++i) {
            const auto& p = m_ports[i];
            file << "port " << i << ": \"" << p.name << "\" "
                 << p.position.x << " " << p.position.y << " " << p.position.z << " | "
                 << p.forward.x << " " << p.forward.y << " " << p.forward.z << " | "
                 << p.up.x << " " << p.up.y << " " << p.up.z << "\n";
        }
        file << "\n";
    }

    // Summary
    file << "# SUMMARY\n";
    file << "# Total vertices: " << m_vertices.size() << "\n";
    file << "# Total faces: " << m_faces.size() << "\n";
    file << "# Total half-edges: " << m_halfEdges.size() << "\n";
    if (textureData && texWidth > 0 && texHeight > 0) {
        file << "# Texture: " << texWidth << "x" << texHeight << " RGBA\n";
    }

    int quadCount = 0, triCount = 0, otherCount = 0;
    for (const auto& f : m_faces) {
        if (f.vertexCount == 4) quadCount++;
        else if (f.vertexCount == 3) triCount++;
        else otherCount++;
    }
    file << "# Quads: " << quadCount << ", Tris: " << triCount << ", Other: " << otherCount << "\n";

    file.close();
    std::cout << "Saved mesh with texture to " << filepath << std::endl;
    return true;
}

bool EditableMesh::saveLime(const std::string& filepath, const unsigned char* textureData, int texWidth, int texHeight,
                            const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for writing" << std::endl;
        return false;
    }

    // Use v3.0 if skeleton data present, otherwise v2.1 for backward compat
    bool hasBones = !m_skeleton.bones.empty();
    if (hasBones) {
        file << "# LIME Model Format v3.0\n";
        file << "# Half-edge mesh with skeleton, embedded texture and transform\n\n";
    } else {
        file << "# LIME Model Format v2.1\n";
        file << "# Half-edge mesh with embedded texture and transform\n\n";
    }

    // Transform section
    file << "# TRANSFORM\n";
    file << "transform_pos: " << position.x << " " << position.y << " " << position.z << "\n";
    file << "transform_rot: " << rotation.w << " " << rotation.x << " " << rotation.y << " " << rotation.z << "\n";
    file << "transform_scale: " << scale.x << " " << scale.y << " " << scale.z << "\n\n";

    // Skeleton section (v3.0 only)
    if (hasBones) {
        file << "# SKELETON: " << m_skeleton.bones.size() << "\n";
        file << "# bone idx: parentIdx \"name\" | 16 inverseBindMatrix floats | 16 localTransform floats\n";
        for (size_t i = 0; i < m_skeleton.bones.size(); ++i) {
            const auto& bone = m_skeleton.bones[i];
            file << "bone " << i << ": " << bone.parentIndex << " \"" << bone.name << "\" |";
            const float* ibm = &bone.inverseBindMatrix[0][0];
            for (int j = 0; j < 16; ++j) file << " " << ibm[j];
            file << " |";
            const float* lt = &bone.localTransform[0][0];
            for (int j = 0; j < 16; ++j) file << " " << lt[j];
            file << "\n";
        }
        file << "\n";
    }

    // Texture section (if provided)
    if (textureData && texWidth > 0 && texHeight > 0) {
        size_t texSize = static_cast<size_t>(texWidth) * texHeight * 4;  // RGBA
        std::string encoded = base64_encode(textureData, texSize);
        file << "# TEXTURE\n";
        file << "tex_size: " << texWidth << " " << texHeight << "\n";
        file << "tex_data: " << encoded << "\n\n";
    }

    // Vertices
    file << "# VERTICES: " << m_vertices.size() << "\n";
    if (hasBones) {
        file << "# idx: pos.x pos.y pos.z | nrm.x nrm.y nrm.z | uv.u uv.v | col.r col.g col.b col.a | halfEdgeIdx selected | bi0 bi1 bi2 bi3 | bw0 bw1 bw2 bw3\n";
    } else {
        file << "# idx: pos.x pos.y pos.z | nrm.x nrm.y nrm.z | uv.u uv.v | col.r col.g col.b col.a | halfEdgeIdx selected\n";
    }
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const auto& v = m_vertices[i];
        file << "v " << i << ": "
             << v.position.x << " " << v.position.y << " " << v.position.z << " | "
             << v.normal.x << " " << v.normal.y << " " << v.normal.z << " | "
             << v.uv.x << " " << v.uv.y << " | "
             << v.color.r << " " << v.color.g << " " << v.color.b << " " << v.color.a << " | "
             << v.halfEdgeIndex << " " << (v.selected ? 1 : 0);
        if (hasBones) {
            file << " | " << v.boneIndices.x << " " << v.boneIndices.y << " " << v.boneIndices.z << " " << v.boneIndices.w
                 << " | " << v.boneWeights.x << " " << v.boneWeights.y << " " << v.boneWeights.z << " " << v.boneWeights.w;
        }
        file << "\n";
    }
    file << "\n";

    // Faces
    file << "# FACES: " << m_faces.size() << "\n";
    file << "# idx: halfEdgeIdx vertexCount selected | vertex_indices...\n";
    for (size_t i = 0; i < m_faces.size(); ++i) {
        const auto& f = m_faces[i];
        auto verts = getFaceVertices(static_cast<uint32_t>(i));
        file << "f " << i << ": " << f.halfEdgeIndex << " " << f.vertexCount << " "
             << (f.selected ? 1 : 0) << " |";
        for (uint32_t v : verts) {
            file << " " << v;
        }
        file << "\n";
    }
    file << "\n";

    // Half-edges
    file << "# HALF_EDGES: " << m_halfEdges.size() << "\n";
    file << "# idx: vertexIndex faceIndex nextIndex prevIndex twinIndex\n";
    for (size_t i = 0; i < m_halfEdges.size(); ++i) {
        const auto& he = m_halfEdges[i];
        file << "he " << i << ": "
             << he.vertexIndex << " "
             << he.faceIndex << " "
             << he.nextIndex << " "
             << he.prevIndex << " "
             << he.twinIndex << "\n";
    }
    file << "\n";

    // Control points
    if (!m_controlPoints.empty()) {
        file << "# CONTROL_POINTS: " << m_controlPoints.size() << "\n";
        for (size_t i = 0; i < m_controlPoints.size(); ++i) {
            file << "cp " << i << ": " << m_controlPoints[i].vertexIndex << " \"" << m_controlPoints[i].name << "\"\n";
        }
        file << "\n";
    }

    // Connection ports
    if (!m_ports.empty()) {
        file << "# PORTS: " << m_ports.size() << "\n";
        for (size_t i = 0; i < m_ports.size(); ++i) {
            const auto& p = m_ports[i];
            file << "port " << i << ": \"" << p.name << "\" "
                 << p.position.x << " " << p.position.y << " " << p.position.z << " | "
                 << p.forward.x << " " << p.forward.y << " " << p.forward.z << " | "
                 << p.up.x << " " << p.up.y << " " << p.up.z << "\n";
        }
        file << "\n";
    }

    // Metadata (widget properties, machine info, etc.)
    if (!m_metadata.empty()) {
        file << "# METADATA\n";
        for (const auto& [key, value] : m_metadata) {
            file << "meta " << key << ": " << value << "\n";
        }
        file << "\n";
    }

    // Summary
    file << "# SUMMARY\n";
    file << "# Total vertices: " << m_vertices.size() << "\n";
    file << "# Total faces: " << m_faces.size() << "\n";
    file << "# Total half-edges: " << m_halfEdges.size() << "\n";
    if (!m_controlPoints.empty()) {
        file << "# Control points: " << m_controlPoints.size() << "\n";
    }
    if (!m_metadata.empty()) {
        file << "# Metadata entries: " << m_metadata.size() << "\n";
    }
    if (textureData && texWidth > 0 && texHeight > 0) {
        file << "# Texture: " << texWidth << "x" << texHeight << " RGBA\n";
    }

    int quadCount = 0, triCount = 0, otherCount = 0;
    for (const auto& f : m_faces) {
        if (f.vertexCount == 4) quadCount++;
        else if (f.vertexCount == 3) triCount++;
        else otherCount++;
    }
    file << "# Quads: " << quadCount << ", Tris: " << triCount << ", Other: " << otherCount << "\n";

    file.close();
    // Debug: print UV range for save verification
    glm::vec2 uvMin(FLT_MAX), uvMax(-FLT_MAX);
    for (const auto& v : m_vertices) {
        uvMin = glm::min(uvMin, v.uv);
        uvMax = glm::max(uvMax, v.uv);
    }
    std::cout << "Saved mesh with texture and transform to " << filepath
              << " | UV range: [" << uvMin.x << "," << uvMin.y << "] - [" << uvMax.x << "," << uvMax.y << "]" << std::endl;
    return true;
}

bool EditableMesh::loadLime(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for reading" << std::endl;
        return false;
    }

    m_vertices.clear();
    m_faces.clear();
    m_halfEdges.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();
    m_controlPoints.clear();
    m_ports.clear();
    m_metadata.clear();

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "v") {
            // Parse vertex: v idx: pos.x pos.y pos.z | nrm.x nrm.y nrm.z | uv.u uv.v | halfEdgeIdx selected
            uint32_t idx;
            char colon, pipe1, pipe2, pipe3;
            HEVertex v;
            int selected;

            iss >> idx >> colon
                >> v.position.x >> v.position.y >> v.position.z >> pipe1
                >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                >> v.uv.x >> v.uv.y >> pipe3
                >> v.halfEdgeIndex >> selected;
            v.selected = (selected != 0);
            v.color = glm::vec4(1.0f);  // Default white

            if (idx >= m_vertices.size()) {
                m_vertices.resize(idx + 1);
            }
            m_vertices[idx] = v;
        }
        else if (type == "f") {
            // Parse face: f idx: halfEdgeIdx vertexCount selected | vertex_indices...
            uint32_t idx, heIdx, vertCount;
            int selected;
            char colon, pipe;

            iss >> idx >> colon >> heIdx >> vertCount >> selected >> pipe;

            HEFace f;
            f.halfEdgeIndex = heIdx;
            f.vertexCount = vertCount;
            f.selected = (selected != 0);

            if (idx >= m_faces.size()) {
                m_faces.resize(idx + 1);
            }
            m_faces[idx] = f;
        }
        else if (type == "he") {
            // Parse half-edge: he idx: vertexIndex faceIndex nextIndex prevIndex twinIndex
            uint32_t idx;
            char colon;
            HalfEdge he;

            iss >> idx >> colon
                >> he.vertexIndex >> he.faceIndex >> he.nextIndex >> he.prevIndex >> he.twinIndex;

            if (idx >= m_halfEdges.size()) {
                m_halfEdges.resize(idx + 1);
            }
            m_halfEdges[idx] = he;
        }
        else if (type == "cp") {
            uint32_t idx;
            char colon;
            uint32_t vertIdx;
            iss >> idx >> colon >> vertIdx;
            // Parse optional quoted name
            std::string cpName = "CP" + std::to_string(idx);
            std::string rest;
            std::getline(iss, rest);
            size_t q1 = rest.find('"');
            size_t q2 = rest.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                cpName = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            m_controlPoints.push_back({vertIdx, cpName});
        }
        else if (type == "port") {
            // Parse: port idx: "name" px py pz | fx fy fz | ux uy uz
            uint32_t idx;
            char colon;
            iss >> idx >> colon;
            std::string rest;
            std::getline(iss, rest);
            Port port;
            // Extract quoted name
            size_t q1 = rest.find('"');
            size_t q2 = rest.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                port.name = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            // Parse position | forward | up after the closing quote
            std::string nums = (q2 != std::string::npos) ? rest.substr(q2 + 1) : rest;
            // Replace pipes with spaces for easy parsing
            for (char& c : nums) { if (c == '|') c = ' '; }
            std::istringstream niss(nums);
            niss >> port.position.x >> port.position.y >> port.position.z
                 >> port.forward.x >> port.forward.y >> port.forward.z
                 >> port.up.x >> port.up.y >> port.up.z;
            m_ports.push_back(port);
        }
        else if (type == "meta") {
            std::string rest;
            std::getline(iss, rest);
            size_t start = rest.find_first_not_of(' ');
            if (start != std::string::npos) rest = rest.substr(start);
            size_t colonPos = rest.find(": ");
            if (colonPos != std::string::npos) {
                m_metadata[rest.substr(0, colonPos)] = rest.substr(colonPos + 2);
            }
        }
    }

    file.close();
    rebuildEdgeMap();

    std::cout << "Loaded mesh from " << filepath << ": "
              << m_vertices.size() << " vertices, "
              << m_faces.size() << " faces, "
              << m_halfEdges.size() << " half-edges"
              << (m_controlPoints.empty() ? "" : ", " + std::to_string(m_controlPoints.size()) + " control points")
              << (m_metadata.empty() ? "" : ", " + std::to_string(m_metadata.size()) + " metadata entries")
              << std::endl;

    return true;
}

bool EditableMesh::loadLime(const std::string& filepath, std::vector<unsigned char>& outTextureData, int& outTexWidth, int& outTexHeight) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for reading" << std::endl;
        return false;
    }

    m_vertices.clear();
    m_faces.clear();
    m_halfEdges.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();
    m_metadata.clear();
    outTextureData.clear();
    outTexWidth = 0;
    outTexHeight = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "tex_size:") {
            iss >> outTexWidth >> outTexHeight;
        }
        else if (type == "tex_data:") {
            std::string encoded;
            iss >> encoded;
            outTextureData = base64_decode(encoded);
        }
        else if (type == "v") {
            // Parse vertex: v idx: pos | nrm | uv | col | halfEdgeIdx selected
            uint32_t idx;
            char colon, pipe1, pipe2, pipe3, pipe4;
            HEVertex v;
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
                // Re-parse without color
                std::istringstream iss2(line);
                std::string dummy;
                iss2 >> dummy >> idx >> colon
                     >> v.position.x >> v.position.y >> v.position.z >> pipe1
                     >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                     >> v.uv.x >> v.uv.y >> pipe3
                     >> v.halfEdgeIndex >> selected;
            }
            v.selected = (selected != 0);

            if (idx >= m_vertices.size()) {
                m_vertices.resize(idx + 1);
            }
            m_vertices[idx] = v;
        }
        else if (type == "f") {
            // Parse face: f idx: halfEdgeIdx vertexCount selected | vertex_indices...
            uint32_t idx, heIdx, vertCount;
            int selected;
            char colon, pipe;

            iss >> idx >> colon >> heIdx >> vertCount >> selected >> pipe;

            HEFace f;
            f.halfEdgeIndex = heIdx;
            f.vertexCount = vertCount;
            f.selected = (selected != 0);

            if (idx >= m_faces.size()) {
                m_faces.resize(idx + 1);
            }
            m_faces[idx] = f;
        }
        else if (type == "he") {
            // Parse half-edge: he idx: vertexIndex faceIndex nextIndex prevIndex twinIndex
            uint32_t idx;
            char colon;
            HalfEdge he;

            iss >> idx >> colon
                >> he.vertexIndex >> he.faceIndex >> he.nextIndex >> he.prevIndex >> he.twinIndex;

            if (idx >= m_halfEdges.size()) {
                m_halfEdges.resize(idx + 1);
            }
            m_halfEdges[idx] = he;
        }
        else if (type == "cp") {
            uint32_t idx;
            char colon;
            uint32_t vertIdx;
            iss >> idx >> colon >> vertIdx;
            // Parse optional quoted name
            std::string cpName = "CP" + std::to_string(idx);
            std::string rest;
            std::getline(iss, rest);
            size_t q1 = rest.find('"');
            size_t q2 = rest.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                cpName = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            m_controlPoints.push_back({vertIdx, cpName});
        }
        else if (type == "port") {
            // Parse: port idx: "name" px py pz | fx fy fz | ux uy uz
            uint32_t idx;
            char colon;
            iss >> idx >> colon;
            std::string rest;
            std::getline(iss, rest);
            Port port;
            // Extract quoted name
            size_t q1 = rest.find('"');
            size_t q2 = rest.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                port.name = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            // Parse position | forward | up after the closing quote
            std::string nums = (q2 != std::string::npos) ? rest.substr(q2 + 1) : rest;
            // Replace pipes with spaces for easy parsing
            for (char& c : nums) { if (c == '|') c = ' '; }
            std::istringstream niss(nums);
            niss >> port.position.x >> port.position.y >> port.position.z
                 >> port.forward.x >> port.forward.y >> port.forward.z
                 >> port.up.x >> port.up.y >> port.up.z;
            m_ports.push_back(port);
        }
        else if (type == "meta") {
            std::string rest;
            std::getline(iss, rest);
            size_t start = rest.find_first_not_of(' ');
            if (start != std::string::npos) rest = rest.substr(start);
            size_t colonPos = rest.find(": ");
            if (colonPos != std::string::npos) {
                m_metadata[rest.substr(0, colonPos)] = rest.substr(colonPos + 2);
            }
        }
    }

    file.close();
    rebuildEdgeMap();

    std::cout << "Loaded mesh from " << filepath << ": "
              << m_vertices.size() << " vertices, "
              << m_faces.size() << " faces, "
              << m_halfEdges.size() << " half-edges";
    if (outTexWidth > 0 && outTexHeight > 0) {
        std::cout << ", texture " << outTexWidth << "x" << outTexHeight;
    }
    if (!m_controlPoints.empty()) {
        std::cout << ", " << m_controlPoints.size() << " control points";
    }
    if (!m_metadata.empty()) {
        std::cout << ", " << m_metadata.size() << " metadata entries";
    }
    std::cout << std::endl;

    return true;
}

bool EditableMesh::loadLime(const std::string& filepath, std::vector<unsigned char>& outTextureData, int& outTexWidth, int& outTexHeight,
                            glm::vec3& outPosition, glm::quat& outRotation, glm::vec3& outScale) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for reading" << std::endl;
        return false;
    }

    m_vertices.clear();
    m_faces.clear();
    m_halfEdges.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();
    m_controlPoints.clear();
    m_ports.clear();
    m_metadata.clear();
    m_skeleton.bones.clear();
    m_skeleton.boneNameToIndex.clear();
    outTextureData.clear();
    outTexWidth = 0;
    outTexHeight = 0;

    // Default transform values
    outPosition = glm::vec3(0.0f);
    outRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // Identity quaternion
    outScale = glm::vec3(1.0f);

    bool isV3 = false;

    std::string line;
    while (std::getline(file, line)) {
        // Check for v3.0 header
        if (line.find("v3.0") != std::string::npos) {
            isV3 = true;
        }
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "transform_pos:") {
            iss >> outPosition.x >> outPosition.y >> outPosition.z;
        }
        else if (type == "transform_rot:") {
            iss >> outRotation.w >> outRotation.x >> outRotation.y >> outRotation.z;
        }
        else if (type == "transform_scale:") {
            iss >> outScale.x >> outScale.y >> outScale.z;
        }
        else if (type == "tex_size:") {
            iss >> outTexWidth >> outTexHeight;
        }
        else if (type == "tex_data:") {
            std::string encoded;
            iss >> encoded;
            outTextureData = base64_decode(encoded);
        }
        else if (type == "bone") {
            // Parse bone: bone idx: parentIdx "name" | 16 inverseBindMatrix floats | 16 localTransform floats
            uint32_t idx;
            int parentIdx;
            char colon, pipe1, pipe2;
            iss >> idx >> colon >> parentIdx;

            // Parse quoted name
            std::string boneName;
            char c;
            // Skip to opening quote
            while (iss.get(c) && c != '"') {}
            while (iss.get(c) && c != '"') {
                boneName += c;
            }

            iss >> pipe1;
            Bone bone;
            bone.name = boneName;
            bone.parentIndex = parentIdx;

            // Read 16 inverse bind matrix floats
            float* ibm = &bone.inverseBindMatrix[0][0];
            for (int j = 0; j < 16; ++j) iss >> ibm[j];

            iss >> pipe2;

            // Read 16 local transform floats
            float* lt = &bone.localTransform[0][0];
            for (int j = 0; j < 16; ++j) iss >> lt[j];

            if (idx >= m_skeleton.bones.size()) {
                m_skeleton.bones.resize(idx + 1);
            }
            m_skeleton.bones[idx] = bone;
            m_skeleton.boneNameToIndex[boneName] = static_cast<int>(idx);
        }
        else if (type == "v") {
            // Parse vertex: v idx: pos | nrm | uv | col | halfEdgeIdx selected [| bi0 bi1 bi2 bi3 | bw0 bw1 bw2 bw3]
            uint32_t idx;
            char colon, pipe1, pipe2, pipe3, pipe4;
            HEVertex v;
            int selected;

            iss >> idx >> colon
                >> v.position.x >> v.position.y >> v.position.z >> pipe1
                >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                >> v.uv.x >> v.uv.y >> pipe3;

            // Try to read color (v2.0+ format)
            float r, g, b, a;
            if (iss >> r >> g >> b >> a >> pipe4) {
                v.color = glm::vec4(r, g, b, a);
                iss >> v.halfEdgeIndex >> selected;

                // Try to read bone data (v3.0 format)
                char pipe5;
                int bi0, bi1, bi2, bi3;
                if (iss >> pipe5 >> bi0 >> bi1 >> bi2 >> bi3) {
                    v.boneIndices = glm::ivec4(bi0, bi1, bi2, bi3);
                    char pipe6;
                    float bw0, bw1, bw2, bw3;
                    if (iss >> pipe6 >> bw0 >> bw1 >> bw2 >> bw3) {
                        v.boneWeights = glm::vec4(bw0, bw1, bw2, bw3);
                    }
                }
            } else {
                // Fallback for v1.0 format (no color)
                v.color = glm::vec4(1.0f);
                iss.clear();
                iss.seekg(0);
                // Re-parse without color
                std::istringstream iss2(line);
                std::string dummy;
                iss2 >> dummy >> idx >> colon
                     >> v.position.x >> v.position.y >> v.position.z >> pipe1
                     >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                     >> v.uv.x >> v.uv.y >> pipe3
                     >> v.halfEdgeIndex >> selected;
            }
            v.selected = (selected != 0);

            if (idx >= m_vertices.size()) {
                m_vertices.resize(idx + 1);
            }
            m_vertices[idx] = v;
        }
        else if (type == "f") {
            // Parse face: f idx: halfEdgeIdx vertexCount selected | vertex_indices...
            uint32_t idx, heIdx, vertCount;
            int selected;
            char colon, pipe;

            iss >> idx >> colon >> heIdx >> vertCount >> selected >> pipe;

            HEFace f;
            f.halfEdgeIndex = heIdx;
            f.vertexCount = vertCount;
            f.selected = (selected != 0);

            if (idx >= m_faces.size()) {
                m_faces.resize(idx + 1);
            }
            m_faces[idx] = f;
        }
        else if (type == "he") {
            // Parse half-edge: he idx: vertexIndex faceIndex nextIndex prevIndex twinIndex
            uint32_t idx;
            char colon;
            HalfEdge he;

            iss >> idx >> colon
                >> he.vertexIndex >> he.faceIndex >> he.nextIndex >> he.prevIndex >> he.twinIndex;

            if (idx >= m_halfEdges.size()) {
                m_halfEdges.resize(idx + 1);
            }
            m_halfEdges[idx] = he;
        }
        else if (type == "cp") {
            uint32_t idx;
            char colon;
            uint32_t vertIdx;
            iss >> idx >> colon >> vertIdx;
            // Parse optional quoted name
            std::string cpName = "CP" + std::to_string(idx);
            std::string rest;
            std::getline(iss, rest);
            size_t q1 = rest.find('"');
            size_t q2 = rest.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                cpName = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            m_controlPoints.push_back({vertIdx, cpName});
        }
        else if (type == "port") {
            // Parse: port idx: "name" px py pz | fx fy fz | ux uy uz
            uint32_t idx;
            char colon;
            iss >> idx >> colon;
            std::string rest;
            std::getline(iss, rest);
            Port port;
            // Extract quoted name
            size_t q1 = rest.find('"');
            size_t q2 = rest.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                port.name = rest.substr(q1 + 1, q2 - q1 - 1);
            }
            // Parse position | forward | up after the closing quote
            std::string nums = (q2 != std::string::npos) ? rest.substr(q2 + 1) : rest;
            // Replace pipes with spaces for easy parsing
            for (char& c : nums) { if (c == '|') c = ' '; }
            std::istringstream niss(nums);
            niss >> port.position.x >> port.position.y >> port.position.z
                 >> port.forward.x >> port.forward.y >> port.forward.z
                 >> port.up.x >> port.up.y >> port.up.z;
            m_ports.push_back(port);
        }
        else if (type == "meta") {
            std::string rest;
            std::getline(iss, rest);
            size_t start = rest.find_first_not_of(' ');
            if (start != std::string::npos) rest = rest.substr(start);
            size_t colonPos = rest.find(": ");
            if (colonPos != std::string::npos) {
                m_metadata[rest.substr(0, colonPos)] = rest.substr(colonPos + 2);
            }
        }
    }

    file.close();
    rebuildEdgeMap();

    std::cout << "Loaded mesh from " << filepath << ": "
              << m_vertices.size() << " vertices, "
              << m_faces.size() << " faces, "
              << m_halfEdges.size() << " half-edges";
    if (outTexWidth > 0 && outTexHeight > 0) {
        std::cout << ", texture " << outTexWidth << "x" << outTexHeight;
    }
    if (!m_controlPoints.empty()) {
        std::cout << ", " << m_controlPoints.size() << " control points";
    }
    if (!m_metadata.empty()) {
        std::cout << ", " << m_metadata.size() << " metadata entries";
    }
    std::cout << ", transform: pos(" << outPosition.x << "," << outPosition.y << "," << outPosition.z << ")"
              << " scale(" << outScale.x << "," << outScale.y << "," << outScale.z << ")";
    // Debug: print UV range for load verification
    glm::vec2 uvMin(FLT_MAX), uvMax(-FLT_MAX);
    for (const auto& v : m_vertices) {
        uvMin = glm::min(uvMin, v.uv);
        uvMax = glm::max(uvMax, v.uv);
    }
    std::cout << " | UV range: [" << uvMin.x << "," << uvMin.y << "] - [" << uvMax.x << "," << uvMax.y << "]" << std::endl;

    return true;
}

bool EditableMesh::saveOBJ(const std::string& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for writing" << std::endl;
        return false;
    }

    file << "# OBJ file exported from EDEN Model Editor\n";
    file << "# Vertices: " << m_vertices.size() << ", Faces: " << m_faces.size() << "\n\n";

    // Write vertices (positions)
    for (const auto& v : m_vertices) {
        file << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
    }
    file << "\n";

    // Write texture coordinates
    for (const auto& v : m_vertices) {
        file << "vt " << v.uv.x << " " << v.uv.y << "\n";
    }
    file << "\n";

    // Write normals
    for (const auto& v : m_vertices) {
        file << "vn " << v.normal.x << " " << v.normal.y << " " << v.normal.z << "\n";
    }
    file << "\n";

    // Write faces (OBJ indices are 1-based)
    // Format: f v1/vt1/vn1 v2/vt2/vn2 ...
    for (size_t faceIdx = 0; faceIdx < m_faces.size(); ++faceIdx) {
        auto verts = getFaceVertices(static_cast<uint32_t>(faceIdx));
        file << "f";
        for (uint32_t v : verts) {
            uint32_t idx = v + 1;  // 1-based indexing
            file << " " << idx << "/" << idx << "/" << idx;
        }
        file << "\n";
    }

    file.close();
    std::cout << "Saved OBJ: " << filepath << " (" << m_vertices.size() << " vertices, "
              << m_faces.size() << " faces)" << std::endl;
    return true;
}

bool EditableMesh::loadOBJ(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for reading" << std::endl;
        return false;
    }

    m_vertices.clear();
    m_faces.clear();
    m_halfEdges.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> texCoords;
    std::vector<glm::vec3> normals;
    std::vector<std::vector<uint32_t>> faceVertIndices;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "v") {
            glm::vec3 pos;
            iss >> pos.x >> pos.y >> pos.z;
            positions.push_back(pos);
        } else if (type == "vt") {
            glm::vec2 uv;
            iss >> uv.x >> uv.y;
            texCoords.push_back(uv);
        } else if (type == "vn") {
            glm::vec3 norm;
            iss >> norm.x >> norm.y >> norm.z;
            normals.push_back(norm);
        } else if (type == "f") {
            std::vector<uint32_t> faceVerts;
            std::string vertStr;
            while (iss >> vertStr) {
                // Parse v/vt/vn or v//vn or v/vt or v
                std::replace(vertStr.begin(), vertStr.end(), '/', ' ');
                std::istringstream viss(vertStr);
                int vi = 0;
                viss >> vi;
                if (vi != 0) {
                    // OBJ indices are 1-based, negative means relative to end
                    if (vi < 0) vi = static_cast<int>(positions.size()) + vi + 1;
                    faceVerts.push_back(static_cast<uint32_t>(vi - 1));  // Convert to 0-based
                }
            }
            if (faceVerts.size() >= 3) {
                faceVertIndices.push_back(faceVerts);
            }
        }
    }
    file.close();

    // Build vertices from positions (use normals/UVs if available)
    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    m_vertices.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        HEVertex v;
        v.position = positions[i];
        v.normal = (i < normals.size()) ? normals[i] : glm::vec3(0, 1, 0);
        v.uv = (i < texCoords.size()) ? texCoords[i] : glm::vec2(0, 0);
        v.color = white;
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        m_vertices.push_back(v);
    }

    // Build faces using addFace (creates half-edge structure)
    for (const auto& faceVerts : faceVertIndices) {
        addFace(faceVerts);
    }

    // Link twin half-edges
    linkTwinsByPosition();
    rebuildEdgeMap();

    // Recalculate normals if none were provided
    if (normals.empty()) {
        recalculateNormals();
    }

    std::cout << "Loaded OBJ: " << filepath << " (" << m_vertices.size() << " vertices, "
              << m_faces.size() << " faces)" << std::endl;
    return true;
}

void EditableMesh::buildCubeSphere(float radius, float cubeSize, int rings, int segments, bool interior, bool solidShell) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

    // Helper lambda to add an axis-aligned cube at a position
    // Uses the exact same vertex/face pattern as buildCube() which works correctly
    auto addCubeAt = [&](const glm::vec3& center, float size, const glm::vec4& color) {
        float h = size * 0.5f;
        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        // Exact same pattern as buildCube - 24 vertices with proper normals
        // Front face (z = +h)
        m_vertices.push_back({{center.x-h, center.y-h, center.z+h}, {0,0,1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y-h, center.z+h}, {0,0,1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z+h}, {0,0,1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y+h, center.z+h}, {0,0,1}, {0,1}, color, UINT32_MAX, false});
        // Back face (z = -h)
        m_vertices.push_back({{center.x+h, center.y-h, center.z-h}, {0,0,-1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y-h, center.z-h}, {0,0,-1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y+h, center.z-h}, {0,0,-1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z-h}, {0,0,-1}, {0,1}, color, UINT32_MAX, false});
        // Top face (y = +h)
        m_vertices.push_back({{center.x-h, center.y+h, center.z+h}, {0,1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z+h}, {0,1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z-h}, {0,1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y+h, center.z-h}, {0,1,0}, {0,1}, color, UINT32_MAX, false});
        // Bottom face (y = -h)
        m_vertices.push_back({{center.x-h, center.y-h, center.z-h}, {0,-1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y-h, center.z-h}, {0,-1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y-h, center.z+h}, {0,-1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y-h, center.z+h}, {0,-1,0}, {0,1}, color, UINT32_MAX, false});
        // Right face (x = +h)
        m_vertices.push_back({{center.x+h, center.y-h, center.z+h}, {1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y-h, center.z-h}, {1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z-h}, {1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x+h, center.y+h, center.z+h}, {1,0,0}, {0,1}, color, UINT32_MAX, false});
        // Left face (x = -h)
        m_vertices.push_back({{center.x-h, center.y-h, center.z-h}, {-1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y-h, center.z+h}, {-1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y+h, center.z+h}, {-1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{center.x-h, center.y+h, center.z-h}, {-1,0,0}, {0,1}, color, UINT32_MAX, false});

        if (interior) {
            // Reversed winding for interior viewing
            addFace({base+3, base+2, base+1, base+0});       // Front
            addFace({base+7, base+6, base+5, base+4});       // Back
            addFace({base+11, base+10, base+9, base+8});     // Top
            addFace({base+15, base+14, base+13, base+12});   // Bottom
            addFace({base+19, base+18, base+17, base+16});   // Right
            addFace({base+23, base+22, base+21, base+20});   // Left
        } else {
            // Standard winding for exterior viewing (same as buildCube)
            addFace({base+0, base+1, base+2, base+3});       // Front
            addFace({base+4, base+5, base+6, base+7});       // Back
            addFace({base+8, base+9, base+10, base+11});     // Top
            addFace({base+12, base+13, base+14, base+15});   // Bottom
            addFace({base+16, base+17, base+18, base+19});   // Right
            addFace({base+20, base+21, base+22, base+23});   // Left
        }
    };

    // Generate cubes on the sphere surface using latitude/longitude
    int totalCubes = 0;

    // For solid shell: reduce segments near poles to avoid overlap
    // Skip pole caps entirely - they don't work well with cubes

    // Latitude rings
    for (int ring = 1; ring <= rings; ++ring) {
        float phi = glm::pi<float>() * ring / (rings + 1);  // Latitude angle from top
        float y = radius * std::cos(phi);
        float ringRadius = radius * std::sin(phi);

        // Calculate number of segments for this ring based on circumference
        int ringSegments;
        if (solidShell) {
            // Scale segments by sin(phi) to reduce near poles
            ringSegments = std::max(4, static_cast<int>(segments * std::sin(phi)));
        } else {
            ringSegments = std::max(4, static_cast<int>(segments * std::sin(phi)));
        }

        // Calculate cube size for this ring
        float ringCubeSize;
        if (solidShell) {
            // Size cubes to touch horizontally (around the ring)
            float circumference = 2.0f * glm::pi<float>() * ringRadius;
            ringCubeSize = circumference / ringSegments;

            // Clamp to vertical spacing to avoid huge cubes near poles
            float latitudeStep = glm::pi<float>() * radius / (rings + 1);
            ringCubeSize = std::min(ringCubeSize, latitudeStep * 1.1f);
        } else {
            ringCubeSize = cubeSize;
        }

        for (int seg = 0; seg < ringSegments; ++seg) {
            float theta = 2.0f * glm::pi<float>() * seg / ringSegments;
            float x = ringRadius * std::cos(theta);
            float z = ringRadius * std::sin(theta);

            glm::vec3 position(x, y, z);

            // Vary color based on position
            float hue = static_cast<float>(seg) / ringSegments;
            float brightness = 0.7f + 0.3f * (static_cast<float>(ring) / (rings + 1));
            glm::vec4 color(
                0.5f + 0.3f * std::sin(hue * 6.28f),
                0.5f + 0.3f * std::sin(hue * 6.28f + 2.09f),
                0.5f + 0.3f * std::sin(hue * 6.28f + 4.18f),
                1.0f
            );
            color = color * brightness;
            color.a = 1.0f;

            addCubeAt(position, ringCubeSize, color);
            totalCubes++;
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();

    std::cout << "Built cube sphere with radius=" << radius << ", cubeSize=" << cubeSize
              << ", " << totalCubes << " cubes, " << m_faces.size() << " faces"
              << (interior ? " (interior)" : " (exterior)") << std::endl;
}

void EditableMesh::buildExtrudedSphere(float radius, float thickness, int rings, int segments, bool interior) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    float innerRadius = radius;
    float outerRadius = radius + thickness;

    // Helper to create a vertex
    auto addVertex = [&](const glm::vec3& pos, const glm::vec3& normal, const glm::vec2& uv, const glm::vec4& color) -> uint32_t {
        uint32_t idx = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({pos, normal, uv, color, UINT32_MAX, false});
        return idx;
    };

    // For each face of a UV sphere, we create an extruded block
    // The block has: inner face, outer face, and side faces connecting them

    // Helper to get sphere position at (ring, segment)
    auto getSpherePos = [&](int ring, int seg, float r) -> glm::vec3 {
        if (ring == 0) return glm::vec3(0, r, 0);  // Top pole
        if (ring == rings) return glm::vec3(0, -r, 0);  // Bottom pole

        float phi = glm::pi<float>() * static_cast<float>(ring) / rings;
        float theta = 2.0f * glm::pi<float>() * static_cast<float>(seg % segments) / segments;
        float y = std::cos(phi) * r;
        float ringR = std::sin(phi) * r;
        float x = std::cos(theta) * ringR;
        float z = std::sin(theta) * ringR;
        return glm::vec3(x, y, z);
    };

    // Get face normal (average of vertex normals)
    auto getFaceNormal = [&](const std::vector<glm::vec3>& verts) -> glm::vec3 {
        glm::vec3 center(0);
        for (const auto& v : verts) center += v;
        center /= static_cast<float>(verts.size());
        return glm::normalize(center);
    };

    // Create an extruded block from a face
    auto createBlock = [&](const std::vector<glm::vec3>& innerVerts, const std::vector<glm::vec3>& outerVerts, const glm::vec4& color) {
        size_t n = innerVerts.size();

        // Calculate face normals
        glm::vec3 innerNormal = -getFaceNormal(innerVerts);  // Points inward
        glm::vec3 outerNormal = getFaceNormal(outerVerts);   // Points outward

        if (interior) {
            // For interior viewing, swap inner/outer logic
            std::swap(innerNormal, outerNormal);
        }

        // Inner face (facing toward sphere center) - only visible from inside
        std::vector<uint32_t> innerFace;
        for (size_t i = 0; i < n; ++i) {
            innerFace.push_back(addVertex(innerVerts[i], innerNormal, {0, 0}, color));
        }
        if (interior) {
            // CCW for interior viewing
            addFace(innerFace);
        } else {
            // Reverse for exterior (we won't see this face anyway)
            std::vector<uint32_t> reversed(innerFace.rbegin(), innerFace.rend());
            addFace(reversed);
        }

        // Outer face (facing away from sphere center) - visible from outside
        std::vector<uint32_t> outerFace;
        for (size_t i = 0; i < n; ++i) {
            outerFace.push_back(addVertex(outerVerts[i], outerNormal, {0, 0}, color));
        }
        if (interior) {
            // Reverse for interior viewing
            std::vector<uint32_t> reversed(outerFace.rbegin(), outerFace.rend());
            addFace(reversed);
        } else {
            // CCW for exterior viewing
            addFace(outerFace);
        }

        // Side faces connecting inner and outer
        for (size_t i = 0; i < n; ++i) {
            size_t next = (i + 1) % n;

            glm::vec3 inner1 = innerVerts[i];
            glm::vec3 inner2 = innerVerts[next];
            glm::vec3 outer1 = outerVerts[i];
            glm::vec3 outer2 = outerVerts[next];

            // Side face normal (perpendicular to edge, pointing outward from block)
            glm::vec3 edge = inner2 - inner1;
            glm::vec3 outDir = outer1 - inner1;
            glm::vec3 sideNormal = glm::normalize(glm::cross(edge, outDir));

            uint32_t v0 = addVertex(inner1, sideNormal, {0, 0}, color);
            uint32_t v1 = addVertex(inner2, sideNormal, {1, 0}, color);
            uint32_t v2 = addVertex(outer2, sideNormal, {1, 1}, color);
            uint32_t v3 = addVertex(outer1, sideNormal, {0, 1}, color);

            if (interior) {
                addFace({v3, v2, v1, v0});  // Reversed for interior
            } else {
                addFace({v0, v1, v2, v3});  // CCW for exterior
            }
        }
    };

    // Generate faces for each cell of the UV sphere
    for (int ring = 0; ring < rings; ++ring) {
        for (int seg = 0; seg < segments; ++seg) {
            int nextSeg = (seg + 1) % segments;

            // Vary color based on position
            float hue = static_cast<float>(seg) / segments;
            float brightness = 0.6f + 0.4f * (static_cast<float>(ring) / rings);
            glm::vec4 color(
                0.5f + 0.4f * std::sin(hue * 6.28f),
                0.5f + 0.4f * std::sin(hue * 6.28f + 2.09f),
                0.5f + 0.4f * std::sin(hue * 6.28f + 4.18f),
                1.0f
            );
            color = color * brightness;
            color.a = 1.0f;

            if (ring == 0) {
                // Top cap - triangles
                glm::vec3 pole_in = getSpherePos(0, 0, innerRadius);
                glm::vec3 bl_in = getSpherePos(1, seg, innerRadius);
                glm::vec3 br_in = getSpherePos(1, nextSeg, innerRadius);

                glm::vec3 pole_out = getSpherePos(0, 0, outerRadius);
                glm::vec3 bl_out = getSpherePos(1, seg, outerRadius);
                glm::vec3 br_out = getSpherePos(1, nextSeg, outerRadius);

                createBlock({pole_in, br_in, bl_in}, {pole_out, br_out, bl_out}, color);
            }
            else if (ring == rings - 1) {
                // Bottom cap - triangles
                glm::vec3 tl_in = getSpherePos(ring, seg, innerRadius);
                glm::vec3 tr_in = getSpherePos(ring, nextSeg, innerRadius);
                glm::vec3 pole_in = getSpherePos(rings, 0, innerRadius);

                glm::vec3 tl_out = getSpherePos(ring, seg, outerRadius);
                glm::vec3 tr_out = getSpherePos(ring, nextSeg, outerRadius);
                glm::vec3 pole_out = getSpherePos(rings, 0, outerRadius);

                createBlock({tl_in, tr_in, pole_in}, {tl_out, tr_out, pole_out}, color);
            }
            else {
                // Middle - quads
                glm::vec3 tl_in = getSpherePos(ring, seg, innerRadius);
                glm::vec3 tr_in = getSpherePos(ring, nextSeg, innerRadius);
                glm::vec3 br_in = getSpherePos(ring + 1, nextSeg, innerRadius);
                glm::vec3 bl_in = getSpherePos(ring + 1, seg, innerRadius);

                glm::vec3 tl_out = getSpherePos(ring, seg, outerRadius);
                glm::vec3 tr_out = getSpherePos(ring, nextSeg, outerRadius);
                glm::vec3 br_out = getSpherePos(ring + 1, nextSeg, outerRadius);
                glm::vec3 bl_out = getSpherePos(ring + 1, seg, outerRadius);

                createBlock({tl_in, tr_in, br_in, bl_in}, {tl_out, tr_out, br_out, bl_out}, color);
            }
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();

    std::cout << "Built extruded sphere with radius=" << radius << ", thickness=" << thickness
              << ", " << m_faces.size() << " faces"
              << (interior ? " (interior)" : " (exterior)") << std::endl;
}

void EditableMesh::buildCubeBlock(int width, int height, int depth, float cubeSize) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    float h = cubeSize * 0.5f;

    // Helper to add a cube at grid position (gx, gy, gz)
    auto addCubeAt = [&](int gx, int gy, int gz, const glm::vec4& color) {
        float cx = (gx - width * 0.5f + 0.5f) * cubeSize;
        float cy = gy * cubeSize + h;  // Sit on ground
        float cz = (gz - depth * 0.5f + 0.5f) * cubeSize;

        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        // Same pattern as buildCube - 24 vertices
        // Front face (z = +h)
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {0,0,1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {0,0,1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {0,0,1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {0,0,1}, {0,1}, color, UINT32_MAX, false});
        // Back face (z = -h)
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {0,0,-1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {0,0,-1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {0,0,-1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {0,0,-1}, {0,1}, color, UINT32_MAX, false});
        // Top face (y = +h)
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {0,1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {0,1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {0,1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {0,1,0}, {0,1}, color, UINT32_MAX, false});
        // Bottom face (y = -h)
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {0,-1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {0,-1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {0,-1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {0,-1,0}, {0,1}, color, UINT32_MAX, false});
        // Right face (x = +h)
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {1,0,0}, {0,1}, color, UINT32_MAX, false});
        // Left face (x = -h)
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {-1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {-1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {-1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {-1,0,0}, {0,1}, color, UINT32_MAX, false});

        // Standard winding (same as buildCube)
        addFace({base+0, base+1, base+2, base+3});       // Front
        addFace({base+4, base+5, base+6, base+7});       // Back
        addFace({base+8, base+9, base+10, base+11});     // Top
        addFace({base+12, base+13, base+14, base+15});   // Bottom
        addFace({base+16, base+17, base+18, base+19});   // Right
        addFace({base+20, base+21, base+22, base+23});   // Left
    };

    // Fill the rectangular block with cubes
    int totalCubes = 0;
    for (int y = 0; y < height; ++y) {
        for (int z = 0; z < depth; ++z) {
            for (int x = 0; x < width; ++x) {
                // Vary color based on position
                float hue = static_cast<float>(x + z) / (width + depth);
                float brightness = 0.6f + 0.4f * (static_cast<float>(y) / height);
                glm::vec4 color(
                    0.5f + 0.4f * std::sin(hue * 6.28f),
                    0.5f + 0.4f * std::sin(hue * 6.28f + 2.09f),
                    0.5f + 0.4f * std::sin(hue * 6.28f + 4.18f),
                    1.0f
                );
                color = color * brightness;
                color.a = 1.0f;

                addCubeAt(x, y, z, color);
                totalCubes++;
            }
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();

    std::cout << "Built cube block " << width << "x" << height << "x" << depth
              << " with " << totalCubes << " cubes, " << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::buildBlockPlate(int width, int height, float cubeSize, bool beveled, float bevelAmount) {
    m_vertices.clear();
    m_halfEdges.clear();
    m_faces.clear();
    m_edgeMap.clear();
    m_selectedEdges.clear();

    float h = cubeSize * 0.5f;
    float b = beveled ? (bevelAmount * h) : 0.0f;  // Bevel inset

    // Helper to add a regular cube at grid position (gx, gy)
    auto addCubeAt = [&](int gx, int gy, const glm::vec4& color) {
        float cx = (gx - width * 0.5f + 0.5f) * cubeSize;
        float cy = gy * cubeSize + h;  // Sit on ground
        float cz = 0.0f;  // Centered on Z=0

        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        // 24 vertices for a cube
        // Front face (z = +h)
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {0,0,1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {0,0,1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {0,0,1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {0,0,1}, {0,1}, color, UINT32_MAX, false});
        // Back face (z = -h)
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {0,0,-1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {0,0,-1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {0,0,-1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {0,0,-1}, {0,1}, color, UINT32_MAX, false});
        // Top face (y = +h)
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {0,1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {0,1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {0,1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {0,1,0}, {0,1}, color, UINT32_MAX, false});
        // Bottom face (y = -h)
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {0,-1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {0,-1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {0,-1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {0,-1,0}, {0,1}, color, UINT32_MAX, false});
        // Right face (x = +h)
        m_vertices.push_back({{cx+h, cy-h, cz+h}, {1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-h, cz-h}, {1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz-h}, {1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+h, cz+h}, {1,0,0}, {0,1}, color, UINT32_MAX, false});
        // Left face (x = -h)
        m_vertices.push_back({{cx-h, cy-h, cz-h}, {-1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-h, cz+h}, {-1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz+h}, {-1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+h, cz-h}, {-1,0,0}, {0,1}, color, UINT32_MAX, false});

        addFace({base+0, base+1, base+2, base+3});       // Front
        addFace({base+4, base+5, base+6, base+7});       // Back
        addFace({base+8, base+9, base+10, base+11});     // Top
        addFace({base+12, base+13, base+14, base+15});   // Bottom
        addFace({base+16, base+17, base+18, base+19});   // Right
        addFace({base+20, base+21, base+22, base+23});   // Left
    };

    // Helper to add a beveled cube at grid position (gx, gy)
    auto addBeveledCubeAt = [&](int gx, int gy, const glm::vec4& color) {
        float cx = (gx - width * 0.5f + 0.5f) * cubeSize;
        float cy = gy * cubeSize + h;
        float cz = 0.0f;

        // A beveled cube has 26 faces:
        // - 6 main faces (smaller than original)
        // - 12 edge bevel faces (one per edge)
        // - 8 corner triangles

        // For simplicity, we'll just do edge bevels (chamfered edges)
        // Inner dimensions (the flat face parts)
        float hi = h - b;  // Inner half-size

        uint32_t base = static_cast<uint32_t>(m_vertices.size());

        // Darker color for bevel faces
        glm::vec4 bevelColor = color * 0.7f;
        bevelColor.a = 1.0f;

        // FRONT FACE (z = +h plane, but inner part at z = +h, outer corners at z = +h)
        // Main front face (inset by bevel amount on all sides)
        m_vertices.push_back({{cx-hi, cy-hi, cz+h}, {0,0,1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-hi, cz+h}, {0,0,1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz+h}, {0,0,1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz+h}, {0,0,1}, {0,1}, color, UINT32_MAX, false});
        addFace({base+0, base+1, base+2, base+3});  // Front main

        // BACK FACE (z = -h)
        uint32_t backBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy-hi, cz-h}, {0,0,-1}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-hi, cz-h}, {0,0,-1}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz-h}, {0,0,-1}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz-h}, {0,0,-1}, {0,1}, color, UINT32_MAX, false});
        addFace({backBase+0, backBase+1, backBase+2, backBase+3});  // Back main

        // TOP FACE (y = +h)
        uint32_t topBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy+h, cz+hi}, {0,1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz+hi}, {0,1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz-hi}, {0,1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz-hi}, {0,1,0}, {0,1}, color, UINT32_MAX, false});
        addFace({topBase+0, topBase+1, topBase+2, topBase+3});  // Top main

        // BOTTOM FACE (y = -h)
        uint32_t botBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy-h, cz-hi}, {0,-1,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz-hi}, {0,-1,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz+hi}, {0,-1,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-h, cz+hi}, {0,-1,0}, {0,1}, color, UINT32_MAX, false});
        addFace({botBase+0, botBase+1, botBase+2, botBase+3});  // Bottom main

        // RIGHT FACE (x = +h)
        uint32_t rightBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+h, cy-hi, cz+hi}, {1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-hi, cz-hi}, {1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz-hi}, {1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz+hi}, {1,0,0}, {0,1}, color, UINT32_MAX, false});
        addFace({rightBase+0, rightBase+1, rightBase+2, rightBase+3});  // Right main

        // LEFT FACE (x = -h)
        uint32_t leftBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-h, cy-hi, cz-hi}, {-1,0,0}, {0,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-hi, cz+hi}, {-1,0,0}, {1,0}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz+hi}, {-1,0,0}, {1,1}, color, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz-hi}, {-1,0,0}, {0,1}, color, UINT32_MAX, false});
        addFace({leftBase+0, leftBase+1, leftBase+2, leftBase+3});  // Left main

        // BEVEL FACES - 12 edge bevels
        // Each bevel connects the edge of one main face to the edge of an adjacent main face
        glm::vec3 diagNorm;

        // Front-Top bevel
        diagNorm = glm::normalize(glm::vec3(0, 1, 1));
        uint32_t ftBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy+hi, cz+h}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz+h}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz+hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz+hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({ftBase+0, ftBase+1, ftBase+2, ftBase+3});

        // Front-Bottom bevel
        diagNorm = glm::normalize(glm::vec3(0, -1, 1));
        uint32_t fbBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy-h, cz+hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz+hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-hi, cz+h}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-hi, cz+h}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({fbBase+0, fbBase+1, fbBase+2, fbBase+3});

        // Front-Right bevel
        diagNorm = glm::normalize(glm::vec3(1, 0, 1));
        uint32_t frBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy-hi, cz+h}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-hi, cz+hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz+hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz+h}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({frBase+0, frBase+1, frBase+2, frBase+3});

        // Front-Left bevel
        diagNorm = glm::normalize(glm::vec3(-1, 0, 1));
        uint32_t flBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-h, cy-hi, cz+hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-hi, cz+h}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz+h}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz+hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({flBase+0, flBase+1, flBase+2, flBase+3});

        // Back-Top bevel
        diagNorm = glm::normalize(glm::vec3(0, 1, -1));
        uint32_t btBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy+hi, cz-h}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz-h}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz-hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz-hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({btBase+0, btBase+1, btBase+2, btBase+3});

        // Back-Bottom bevel
        diagNorm = glm::normalize(glm::vec3(0, -1, -1));
        uint32_t bbBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy-h, cz-hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-h, cz-hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-hi, cz-h}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-hi, cz-h}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({bbBase+0, bbBase+1, bbBase+2, bbBase+3});

        // Back-Right bevel
        diagNorm = glm::normalize(glm::vec3(1, 0, -1));
        uint32_t brBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+h, cy-hi, cz-hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-hi, cz-h}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz-h}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz-hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({brBase+0, brBase+1, brBase+2, brBase+3});

        // Back-Left bevel
        diagNorm = glm::normalize(glm::vec3(-1, 0, -1));
        uint32_t blBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy-hi, cz-h}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-hi, cz-hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz-hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz-h}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({blBase+0, blBase+1, blBase+2, blBase+3});

        // Top-Right bevel
        diagNorm = glm::normalize(glm::vec3(1, 1, 0));
        uint32_t trBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy+h, cz+hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz-hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz-hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz+hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({trBase+0, trBase+1, trBase+2, trBase+3});

        // Top-Left bevel
        diagNorm = glm::normalize(glm::vec3(-1, 1, 0));
        uint32_t tlBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy+h, cz-hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz+hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz+hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz-hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({tlBase+0, tlBase+1, tlBase+2, tlBase+3});

        // Bottom-Right bevel
        diagNorm = glm::normalize(glm::vec3(1, -1, 0));
        uint32_t brBotBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy-h, cz-hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz+hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-hi, cz+hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-hi, cz-hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({brBotBase+0, brBotBase+1, brBotBase+2, brBotBase+3});

        // Bottom-Left bevel
        diagNorm = glm::normalize(glm::vec3(-1, -1, 0));
        uint32_t blBotBase = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy-h, cz+hi}, diagNorm, {0,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-h, cz-hi}, diagNorm, {1,0}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-hi, cz-hi}, diagNorm, {1,1}, bevelColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-hi, cz+hi}, diagNorm, {0,1}, bevelColor, UINT32_MAX, false});
        addFace({blBotBase+0, blBotBase+1, blBotBase+2, blBotBase+3});

        // 8 CORNER TRIANGLES
        glm::vec4 cornerColor = color * 0.5f;
        cornerColor.a = 1.0f;

        // Front-Top-Right corner
        diagNorm = glm::normalize(glm::vec3(1, 1, 1));
        uint32_t c1 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy+hi, cz+h}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy+hi, cz+hi}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz+hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c1+0, c1+1, c1+2});

        // Front-Top-Left corner
        diagNorm = glm::normalize(glm::vec3(-1, 1, 1));
        uint32_t c2 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-h, cy+hi, cz+hi}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+hi, cz+h}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz+hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c2+0, c2+1, c2+2});

        // Front-Bottom-Right corner
        diagNorm = glm::normalize(glm::vec3(1, -1, 1));
        uint32_t c3 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+h, cy-hi, cz+hi}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-hi, cz+h}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz+hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c3+0, c3+1, c3+2});

        // Front-Bottom-Left corner
        diagNorm = glm::normalize(glm::vec3(-1, -1, 1));
        uint32_t c4 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy-hi, cz+h}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy-hi, cz+hi}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-h, cz+hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c4+0, c4+1, c4+2});

        // Back-Top-Right corner
        diagNorm = glm::normalize(glm::vec3(1, 1, -1));
        uint32_t c5 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+h, cy+hi, cz-hi}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+hi, cz-h}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy+h, cz-hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c5+0, c5+1, c5+2});

        // Back-Top-Left corner
        diagNorm = glm::normalize(glm::vec3(-1, 1, -1));
        uint32_t c6 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-hi, cy+hi, cz-h}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-h, cy+hi, cz-hi}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy+h, cz-hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c6+0, c6+1, c6+2});

        // Back-Bottom-Right corner
        diagNorm = glm::normalize(glm::vec3(1, -1, -1));
        uint32_t c7 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx+hi, cy-hi, cz-h}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+h, cy-hi, cz-hi}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx+hi, cy-h, cz-hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c7+0, c7+1, c7+2});

        // Back-Bottom-Left corner
        diagNorm = glm::normalize(glm::vec3(-1, -1, -1));
        uint32_t c8 = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back({{cx-h, cy-hi, cz-hi}, diagNorm, {0,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-hi, cz-h}, diagNorm, {1,0}, cornerColor, UINT32_MAX, false});
        m_vertices.push_back({{cx-hi, cy-h, cz-hi}, diagNorm, {0.5f,1}, cornerColor, UINT32_MAX, false});
        addFace({c8+0, c8+1, c8+2});
    };

    // Build the wall of cubes (width x height, single layer)
    int totalCubes = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Brick-like color variation
            float hue = static_cast<float>(x) / width;
            float brightness = 0.7f + 0.3f * (static_cast<float>(y) / height);
            glm::vec4 color(
                0.8f + 0.15f * std::sin(hue * 6.28f + y * 0.5f),
                0.6f + 0.15f * std::sin(hue * 6.28f + 2.09f + y * 0.3f),
                0.5f + 0.15f * std::sin(hue * 6.28f + 4.18f + y * 0.7f),
                1.0f
            );
            color = color * brightness;
            color.a = 1.0f;

            if (beveled) {
                addBeveledCubeAt(x, y, color);
            } else {
                addCubeAt(x, y, color);
            }
            totalCubes++;
        }
    }

    linkTwinsByPosition();
    rebuildEdgeMap();

    std::cout << "Built block plate " << width << "x" << height
              << (beveled ? " (beveled)" : "")
              << " with " << totalCubes << " cubes, " << m_faces.size() << " faces" << std::endl;
}

void EditableMesh::autoUVCubes() {
    // Each cube has 24 vertices (4 per face * 6 faces)
    // We'll pack each cube as an island in UV space

    if (m_vertices.empty()) return;

    // Count cubes (24 vertices per cube)
    int numCubes = static_cast<int>(m_vertices.size()) / 24;
    if (numCubes == 0) {
        std::cout << "No cubes found for auto-UV" << std::endl;
        return;
    }

    // Calculate grid size for packing
    int gridSize = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(numCubes))));
    float cubeUVSize = 1.0f / gridSize;  // Size of each cube's island in UV space

    // Each cube's island is subdivided into a 3x2 grid for its 6 faces
    // Layout:  [Front][Back][Top]
    //          [Bottom][Right][Left]
    float faceWidth = cubeUVSize / 3.0f;
    float faceHeight = cubeUVSize / 2.0f;

    // Small padding to prevent bleeding
    float padding = 0.002f;
    float innerWidth = faceWidth - padding * 2;
    float innerHeight = faceHeight - padding * 2;

    for (int cubeIdx = 0; cubeIdx < numCubes; ++cubeIdx) {
        // Calculate this cube's position in the UV grid
        int gridX = cubeIdx % gridSize;
        int gridY = cubeIdx / gridSize;

        float baseU = gridX * cubeUVSize;
        float baseV = gridY * cubeUVSize;

        // Get vertex offset for this cube
        size_t vertOffset = cubeIdx * 24;

        // Face layout in the cube's island (3 columns x 2 rows):
        // Row 0: Front (0), Back (1), Top (2)
        // Row 1: Bottom (3), Right (4), Left (5)

        // UV offsets for each face within the cube's island
        struct FaceUV { float u, v; };
        FaceUV faceOffsets[6] = {
            {0, 0},           // Front  - column 0, row 0
            {faceWidth, 0},   // Back   - column 1, row 0
            {faceWidth*2, 0}, // Top    - column 2, row 0
            {0, faceHeight},           // Bottom - column 0, row 1
            {faceWidth, faceHeight},   // Right  - column 1, row 1
            {faceWidth*2, faceHeight}  // Left   - column 2, row 1
        };

        // Each face has 4 vertices in order: BL, BR, TR, TL (or similar CCW)
        // We map them to the face's UV region
        for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
            size_t faceVertOffset = vertOffset + faceIdx * 4;

            float faceBaseU = baseU + faceOffsets[faceIdx].u + padding;
            float faceBaseV = baseV + faceOffsets[faceIdx].v + padding;

            // Vertex 0: bottom-left of face
            if (faceVertOffset < m_vertices.size())
                m_vertices[faceVertOffset].uv = {faceBaseU, faceBaseV};
            // Vertex 1: bottom-right
            if (faceVertOffset + 1 < m_vertices.size())
                m_vertices[faceVertOffset + 1].uv = {faceBaseU + innerWidth, faceBaseV};
            // Vertex 2: top-right
            if (faceVertOffset + 2 < m_vertices.size())
                m_vertices[faceVertOffset + 2].uv = {faceBaseU + innerWidth, faceBaseV + innerHeight};
            // Vertex 3: top-left
            if (faceVertOffset + 3 < m_vertices.size())
                m_vertices[faceVertOffset + 3].uv = {faceBaseU, faceBaseV + innerHeight};
        }
    }

    std::cout << "Auto-UV applied to " << numCubes << " cubes in " << gridSize << "x" << gridSize << " grid" << std::endl;
}

} // namespace eden
