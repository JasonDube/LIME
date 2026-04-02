// ModelingMode_AutoRetopo.cpp - Voxel-based auto-retopology
// Generates an all-quad mesh wrapping the "live" reference surface
// Algorithm: voxelize → classify inside/outside → extract boundary quads → project → smooth

#include "ModelingMode.hpp"

#include <iostream>
#include <vector>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <cmath>

using namespace eden;

// Moller-Trumbore ray-triangle intersection
static bool rayTriangleIntersect(const glm::vec3& orig, const glm::vec3& dir,
                                  const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                  float& t) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);
    if (a > -EPSILON && a < EPSILON) return false;
    float f = 1.0f / a;
    glm::vec3 s = orig - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

void ModelingMode::autoRetopology() {
    if (!m_retopologyLiveObj) {
        std::cout << "[AutoRetopo] No live object set" << std::endl;
        return;
    }

    const int R = m_autoRetopResolution;
    const int smoothIter = m_autoRetopSmoothIter;

    std::cout << "[AutoRetopo] Starting voxel remesh (resolution=" << R
              << ", smooth=" << smoothIter << ")" << std::endl;

    // ========================================================================
    // Phase 1: Inside/Outside Voxel Classification
    // ========================================================================

    // Get triangle data from live object
    const auto& verts = m_retopologyLiveObj->getVertices();
    const auto& indices = m_retopologyLiveObj->getIndices();
    uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);

    if (triCount == 0) {
        std::cout << "[AutoRetopo] Live object has no triangles" << std::endl;
        return;
    }

    // Transform vertices to world space
    glm::mat4 worldMatrix = m_retopologyLiveObj->getTransform().getMatrix();
    std::vector<glm::vec3> triVerts(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        glm::vec4 wp = worldMatrix * glm::vec4(verts[i].position, 1.0f);
        triVerts[i] = glm::vec3(wp);
    }

    // Compute AABB from actual world-space vertex positions
    // (don't rely on getWorldBounds which requires setLocalBounds to have been called)
    glm::vec3 boundsMin(INFINITY);
    glm::vec3 boundsMax(-INFINITY);
    for (const auto& v : triVerts) {
        boundsMin = glm::min(boundsMin, v);
        boundsMax = glm::max(boundsMax, v);
    }
    glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    glm::vec3 size = boundsMax - boundsMin;

    // Pad by 10%
    glm::vec3 padding = size * 0.1f;
    glm::vec3 gridMin = boundsMin - padding;
    glm::vec3 gridMax = boundsMax + padding;
    glm::vec3 gridSize = gridMax - gridMin;
    glm::vec3 voxelSize = gridSize / float(R);

    std::cout << "[AutoRetopo] Mesh bounds: (" << boundsMin.x << "," << boundsMin.y << "," << boundsMin.z
              << ") to (" << boundsMax.x << "," << boundsMax.y << "," << boundsMax.z << ")" << std::endl;
    std::cout << "[AutoRetopo] Triangles: " << triCount << ", Vertices: " << verts.size() << std::endl;

    // Voxel grid: 0 = outside, 1 = inside
    std::vector<uint8_t> voxels(R * R * R, 0);
    auto voxelIdx = [R](int x, int y, int z) -> int { return x + y * R + z * R * R; };

    // Vote grids per axis
    std::vector<uint8_t> voteX(R * R * R, 0);
    std::vector<uint8_t> voteY(R * R * R, 0);
    std::vector<uint8_t> voteZ(R * R * R, 0);

    // Axis sweep along X: for each (y,z) column, cast ray along +X
    int totalXHits = 0;
    for (int z = 0; z < R; ++z) {
        for (int y = 0; y < R; ++y) {
            // Ray origin at left edge of grid, centered in voxel
            glm::vec3 rayOrigin(gridMin.x - voxelSize.x,
                                gridMin.y + (y + 0.5f) * voxelSize.y,
                                gridMin.z + (z + 0.5f) * voxelSize.z);
            glm::vec3 rayDir(1.0f, 0.0f, 0.0f);

            // Find all intersections
            std::vector<float> hits;
            for (uint32_t ti = 0; ti < triCount; ++ti) {
                float t;
                if (rayTriangleIntersect(rayOrigin, rayDir,
                        triVerts[indices[ti * 3]], triVerts[indices[ti * 3 + 1]], triVerts[indices[ti * 3 + 2]], t)) {
                    hits.push_back(t);
                }
            }
            totalXHits += (int)hits.size();
            std::sort(hits.begin(), hits.end());

            // Deduplicate very close hits (within half a voxel)
            float dedup = voxelSize.x * 0.3f;
            std::vector<float> uniqueHits;
            for (float h : hits) {
                if (uniqueHits.empty() || (h - uniqueHits.back()) > dedup) {
                    uniqueHits.push_back(h);
                }
            }

            // Walk column toggling inside/outside
            bool inside = false;
            int hitIdx = 0;
            for (int x = 0; x < R; ++x) {
                float voxelCenter = (gridMin.x + (x + 0.5f) * voxelSize.x) - rayOrigin.x;
                while (hitIdx < (int)uniqueHits.size() && uniqueHits[hitIdx] < voxelCenter) {
                    inside = !inside;
                    hitIdx++;
                }
                if (inside) voteX[voxelIdx(x, y, z)] = 1;
            }
        }
    }
    std::cout << "[AutoRetopo] X-sweep total ray-tri hits: " << totalXHits << std::endl;

    // Axis sweep along Y
    for (int z = 0; z < R; ++z) {
        for (int x = 0; x < R; ++x) {
            glm::vec3 rayOrigin(gridMin.x + (x + 0.5f) * voxelSize.x,
                                gridMin.y - voxelSize.y,
                                gridMin.z + (z + 0.5f) * voxelSize.z);
            glm::vec3 rayDir(0.0f, 1.0f, 0.0f);

            std::vector<float> hits;
            for (uint32_t ti = 0; ti < triCount; ++ti) {
                float t;
                if (rayTriangleIntersect(rayOrigin, rayDir,
                        triVerts[indices[ti * 3]], triVerts[indices[ti * 3 + 1]], triVerts[indices[ti * 3 + 2]], t)) {
                    hits.push_back(t);
                }
            }
            std::sort(hits.begin(), hits.end());

            float dedup = voxelSize.y * 0.3f;
            std::vector<float> uniqueHits;
            for (float h : hits) {
                if (uniqueHits.empty() || (h - uniqueHits.back()) > dedup) {
                    uniqueHits.push_back(h);
                }
            }

            bool inside = false;
            int hitIdx = 0;
            for (int y = 0; y < R; ++y) {
                float voxelCenter = (gridMin.y + (y + 0.5f) * voxelSize.y) - rayOrigin.y;
                while (hitIdx < (int)uniqueHits.size() && uniqueHits[hitIdx] < voxelCenter) {
                    inside = !inside;
                    hitIdx++;
                }
                if (inside) voteY[voxelIdx(x, y, z)] = 1;
            }
        }
    }

    // Axis sweep along Z
    for (int y = 0; y < R; ++y) {
        for (int x = 0; x < R; ++x) {
            glm::vec3 rayOrigin(gridMin.x + (x + 0.5f) * voxelSize.x,
                                gridMin.y + (y + 0.5f) * voxelSize.y,
                                gridMin.z - voxelSize.z);
            glm::vec3 rayDir(0.0f, 0.0f, 1.0f);

            std::vector<float> hits;
            for (uint32_t ti = 0; ti < triCount; ++ti) {
                float t;
                if (rayTriangleIntersect(rayOrigin, rayDir,
                        triVerts[indices[ti * 3]], triVerts[indices[ti * 3 + 1]], triVerts[indices[ti * 3 + 2]], t)) {
                    hits.push_back(t);
                }
            }
            std::sort(hits.begin(), hits.end());

            float dedup = voxelSize.z * 0.3f;
            std::vector<float> uniqueHits;
            for (float h : hits) {
                if (uniqueHits.empty() || (h - uniqueHits.back()) > dedup) {
                    uniqueHits.push_back(h);
                }
            }

            bool inside = false;
            int hitIdx = 0;
            for (int z = 0; z < R; ++z) {
                float voxelCenter = (gridMin.z + (z + 0.5f) * voxelSize.z) - rayOrigin.z;
                while (hitIdx < (int)uniqueHits.size() && uniqueHits[hitIdx] < voxelCenter) {
                    inside = !inside;
                    hitIdx++;
                }
                if (inside) voteZ[voxelIdx(x, y, z)] = 1;
            }
        }
    }

    // Majority vote: inside if 2+ of 3 axes agree
    int insideCount = 0;
    for (int i = 0; i < R * R * R; ++i) {
        int votes = voteX[i] + voteY[i] + voteZ[i];
        voxels[i] = (votes >= 2) ? 1 : 0;
        if (voxels[i]) insideCount++;
    }

    std::cout << "[AutoRetopo] Phase 1: " << insideCount << " / " << (R * R * R)
              << " voxels classified as inside" << std::endl;

    if (insideCount == 0) {
        std::cout << "[AutoRetopo] No inside voxels found — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 2: Quad Extraction
    // ========================================================================

    // Vertex deduplication: key = packed grid coords, value = vertex index
    auto packCoord = [](int x, int y, int z) -> uint64_t {
        return (uint64_t(x) & 0xFFFFF) | ((uint64_t(y) & 0xFFFFF) << 20) | ((uint64_t(z) & 0xFFFFF) << 40);
    };
    std::unordered_map<uint64_t, uint32_t> vertexMap;
    std::vector<glm::vec3> quadVerts;
    std::vector<std::array<uint32_t, 4>> quadFaces;

    auto getOrCreateVertex = [&](int gx, int gy, int gz) -> uint32_t {
        uint64_t key = packCoord(gx, gy, gz);
        auto it = vertexMap.find(key);
        if (it != vertexMap.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(quadVerts.size());
        quadVerts.push_back(gridMin + glm::vec3(gx, gy, gz) * voxelSize);
        vertexMap[key] = idx;
        return idx;
    };

    // Check adjacent voxels along each axis
    // Along X axis: between (x, y, z) and (x+1, y, z)
    for (int z = 0; z < R; ++z) {
        for (int y = 0; y < R; ++y) {
            for (int x = 0; x < R - 1; ++x) {
                uint8_t a = voxels[voxelIdx(x, y, z)];
                uint8_t b = voxels[voxelIdx(x + 1, y, z)];
                if (a == b) continue;  // Both same — no boundary

                // Quad on YZ plane at x+1
                uint32_t v0 = getOrCreateVertex(x + 1, y, z);
                uint32_t v1 = getOrCreateVertex(x + 1, y + 1, z);
                uint32_t v2 = getOrCreateVertex(x + 1, y + 1, z + 1);
                uint32_t v3 = getOrCreateVertex(x + 1, y, z + 1);

                if (a == 1) {
                    // Inside→Outside: normal points +X
                    quadFaces.push_back({v0, v3, v2, v1});
                } else {
                    // Outside→Inside: normal points -X
                    quadFaces.push_back({v0, v1, v2, v3});
                }
            }
        }
    }

    // Along Y axis: between (x, y, z) and (x, y+1, z)
    for (int z = 0; z < R; ++z) {
        for (int x = 0; x < R; ++x) {
            for (int y = 0; y < R - 1; ++y) {
                uint8_t a = voxels[voxelIdx(x, y, z)];
                uint8_t b = voxels[voxelIdx(x, y + 1, z)];
                if (a == b) continue;

                // Quad on XZ plane at y+1
                uint32_t v0 = getOrCreateVertex(x, y + 1, z);
                uint32_t v1 = getOrCreateVertex(x, y + 1, z + 1);
                uint32_t v2 = getOrCreateVertex(x + 1, y + 1, z + 1);
                uint32_t v3 = getOrCreateVertex(x + 1, y + 1, z);

                if (a == 1) {
                    // Inside→Outside: normal points +Y
                    quadFaces.push_back({v0, v3, v2, v1});
                } else {
                    quadFaces.push_back({v0, v1, v2, v3});
                }
            }
        }
    }

    // Along Z axis: between (x, y, z) and (x, y, z+1)
    for (int y = 0; y < R; ++y) {
        for (int x = 0; x < R; ++x) {
            for (int z = 0; z < R - 1; ++z) {
                uint8_t a = voxels[voxelIdx(x, y, z)];
                uint8_t b = voxels[voxelIdx(x, y, z + 1)];
                if (a == b) continue;

                // Quad on XY plane at z+1
                uint32_t v0 = getOrCreateVertex(x, y, z + 1);
                uint32_t v1 = getOrCreateVertex(x + 1, y, z + 1);
                uint32_t v2 = getOrCreateVertex(x + 1, y + 1, z + 1);
                uint32_t v3 = getOrCreateVertex(x, y + 1, z + 1);

                if (a == 1) {
                    // Inside→Outside: normal points +Z
                    quadFaces.push_back({v0, v3, v2, v1});
                } else {
                    quadFaces.push_back({v0, v1, v2, v3});
                }
            }
        }
    }

    // Also handle boundary voxels at grid edges (inside voxels adjacent to grid boundary)
    for (int z = 0; z < R; ++z) {
        for (int y = 0; y < R; ++y) {
            // Left face (x=0)
            if (voxels[voxelIdx(0, y, z)]) {
                uint32_t v0 = getOrCreateVertex(0, y, z);
                uint32_t v1 = getOrCreateVertex(0, y + 1, z);
                uint32_t v2 = getOrCreateVertex(0, y + 1, z + 1);
                uint32_t v3 = getOrCreateVertex(0, y, z + 1);
                quadFaces.push_back({v0, v1, v2, v3}); // Normal -X
            }
            // Right face (x=R-1)
            if (voxels[voxelIdx(R - 1, y, z)]) {
                uint32_t v0 = getOrCreateVertex(R, y, z);
                uint32_t v1 = getOrCreateVertex(R, y + 1, z);
                uint32_t v2 = getOrCreateVertex(R, y + 1, z + 1);
                uint32_t v3 = getOrCreateVertex(R, y, z + 1);
                quadFaces.push_back({v0, v3, v2, v1}); // Normal +X
            }
        }
    }
    for (int z = 0; z < R; ++z) {
        for (int x = 0; x < R; ++x) {
            // Bottom face (y=0)
            if (voxels[voxelIdx(x, 0, z)]) {
                uint32_t v0 = getOrCreateVertex(x, 0, z);
                uint32_t v1 = getOrCreateVertex(x, 0, z + 1);
                uint32_t v2 = getOrCreateVertex(x + 1, 0, z + 1);
                uint32_t v3 = getOrCreateVertex(x + 1, 0, z);
                quadFaces.push_back({v0, v1, v2, v3}); // Normal -Y
            }
            // Top face (y=R-1)
            if (voxels[voxelIdx(x, R - 1, z)]) {
                uint32_t v0 = getOrCreateVertex(x, R, z);
                uint32_t v1 = getOrCreateVertex(x, R, z + 1);
                uint32_t v2 = getOrCreateVertex(x + 1, R, z + 1);
                uint32_t v3 = getOrCreateVertex(x + 1, R, z);
                quadFaces.push_back({v0, v3, v2, v1}); // Normal +Y
            }
        }
    }
    for (int y = 0; y < R; ++y) {
        for (int x = 0; x < R; ++x) {
            // Front face (z=0)
            if (voxels[voxelIdx(x, y, 0)]) {
                uint32_t v0 = getOrCreateVertex(x, y, 0);
                uint32_t v1 = getOrCreateVertex(x + 1, y, 0);
                uint32_t v2 = getOrCreateVertex(x + 1, y + 1, 0);
                uint32_t v3 = getOrCreateVertex(x, y + 1, 0);
                quadFaces.push_back({v0, v1, v2, v3}); // Normal -Z
            }
            // Back face (z=R-1)
            if (voxels[voxelIdx(x, y, R - 1)]) {
                uint32_t v0 = getOrCreateVertex(x, y, R);
                uint32_t v1 = getOrCreateVertex(x + 1, y, R);
                uint32_t v2 = getOrCreateVertex(x + 1, y + 1, R);
                uint32_t v3 = getOrCreateVertex(x, y + 1, R);
                quadFaces.push_back({v0, v3, v2, v1}); // Normal +Z
            }
        }
    }

    std::cout << "[AutoRetopo] Phase 2: " << quadFaces.size() << " quads, "
              << quadVerts.size() << " vertices extracted" << std::endl;

    if (quadFaces.empty()) {
        std::cout << "[AutoRetopo] No boundary quads found — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 3: Vertex Projection to Surface
    // ========================================================================

    // Build adjacency: for each vertex, collect adjacent face normals
    std::vector<glm::vec3> vertexNormals(quadVerts.size(), glm::vec3(0.0f));
    for (const auto& face : quadFaces) {
        glm::vec3 e1 = quadVerts[face[1]] - quadVerts[face[0]];
        glm::vec3 e2 = quadVerts[face[3]] - quadVerts[face[0]];
        glm::vec3 n = glm::cross(e1, e2);
        float len = glm::length(n);
        if (len > 1e-8f) n /= len;
        for (int i = 0; i < 4; ++i) {
            vertexNormals[face[i]] += n;
        }
    }
    for (auto& n : vertexNormals) {
        float len = glm::length(n);
        if (len > 1e-8f) n /= len;
        else n = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // Project each vertex onto surface
    int projected = 0;
    for (size_t vi = 0; vi < quadVerts.size(); ++vi) {
        glm::vec3 pos = quadVerts[vi];
        glm::vec3 normal = vertexNormals[vi];

        // Try ray along negative normal (toward surface from outside)
        auto hit = m_retopologyLiveObj->raycast(pos, -normal);
        if (hit.hit) {
            quadVerts[vi] = hit.position;
            projected++;
            continue;
        }

        // Fallback: ray toward mesh center
        glm::vec3 toCenter = glm::normalize(center - pos);
        hit = m_retopologyLiveObj->raycast(pos, toCenter);
        if (hit.hit) {
            quadVerts[vi] = hit.position;
            projected++;
            continue;
        }

        // Fallback: ray away from mesh center
        hit = m_retopologyLiveObj->raycast(pos, -toCenter);
        if (hit.hit) {
            quadVerts[vi] = hit.position;
            projected++;
            continue;
        }

        // Fallback: 6 axis-aligned directions
        const glm::vec3 axisDirs[6] = {
            {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
        };
        for (const auto& dir : axisDirs) {
            hit = m_retopologyLiveObj->raycast(pos, dir);
            if (hit.hit) {
                quadVerts[vi] = hit.position;
                projected++;
                break;
            }
        }
    }

    std::cout << "[AutoRetopo] Phase 3: " << projected << " / " << quadVerts.size()
              << " vertices projected onto surface" << std::endl;

    // ========================================================================
    // Phase 4: Laplacian Smoothing
    // ========================================================================

    if (smoothIter > 0) {
        // Build vertex adjacency from quad faces
        std::vector<std::vector<uint32_t>> adjacency(quadVerts.size());
        for (const auto& face : quadFaces) {
            for (int i = 0; i < 4; ++i) {
                uint32_t a = face[i];
                uint32_t b = face[(i + 1) % 4];
                // Add b as neighbor of a (and vice versa), avoiding duplicates
                if (std::find(adjacency[a].begin(), adjacency[a].end(), b) == adjacency[a].end())
                    adjacency[a].push_back(b);
                if (std::find(adjacency[b].begin(), adjacency[b].end(), a) == adjacency[b].end())
                    adjacency[b].push_back(a);
            }
        }

        const float blendFactor = 0.5f;

        for (int iter = 0; iter < smoothIter; ++iter) {
            std::vector<glm::vec3> newPositions = quadVerts;

            for (size_t vi = 0; vi < quadVerts.size(); ++vi) {
                if (adjacency[vi].empty()) continue;

                // Compute average neighbor position
                glm::vec3 avg(0.0f);
                for (uint32_t ni : adjacency[vi]) {
                    avg += quadVerts[ni];
                }
                avg /= float(adjacency[vi].size());

                // Blend toward average
                newPositions[vi] = glm::mix(quadVerts[vi], avg, blendFactor);
            }

            // Re-project onto surface
            for (size_t vi = 0; vi < newPositions.size(); ++vi) {
                glm::vec3 pos = newPositions[vi];
                glm::vec3 toCenter = glm::normalize(center - pos);

                auto hit = m_retopologyLiveObj->raycast(pos, toCenter);
                if (hit.hit) {
                    newPositions[vi] = hit.position;
                    continue;
                }
                hit = m_retopologyLiveObj->raycast(pos, -toCenter);
                if (hit.hit) {
                    newPositions[vi] = hit.position;
                }
                // If no hit, keep the smoothed position
            }

            quadVerts = newPositions;
        }

        std::cout << "[AutoRetopo] Phase 4: " << smoothIter << " smoothing iterations done" << std::endl;
    }

    // ========================================================================
    // Phase 5: Build EditableMesh + GPU Upload
    // ========================================================================

    // Build EditableMesh — add vertices, then batch-add all faces at once
    // (addQuadFace calls rebuildEdgeMap per face which is O(n^2) for thousands of quads)
    EditableMesh retopoMesh;

    for (size_t vi = 0; vi < quadVerts.size(); ++vi) {
        HEVertex v;
        v.position = quadVerts[vi];
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        v.uv = glm::vec2(0.0f);
        v.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        retopoMesh.addVertex(v);
    }

    // Batch-add all quad faces (rebuilds edge map only once at end)
    retopoMesh.addQuadFacesBatch(quadFaces);
    retopoMesh.recalculateNormals();

    // Find or create the retopo scene object
    SceneObject* retopoObj = nullptr;
    for (auto& obj : m_ctx.sceneObjects) {
        if (obj->getName() == "retopo_mesh") {
            retopoObj = obj.get();
            break;
        }
    }
    if (!retopoObj) {
        auto newObj = std::make_unique<SceneObject>("retopo_mesh");
        newObj->setDescription("Auto-retopology mesh");
        retopoObj = newObj.get();
        m_ctx.sceneObjects.push_back(std::move(newObj));
    }

    // Triangulate for GPU
    std::vector<ModelVertex> gpuVertices;
    std::vector<uint32_t> gpuIndices;
    std::set<uint32_t> noHidden;
    retopoMesh.triangulate(gpuVertices, gpuIndices, noHidden);

    if (gpuIndices.empty()) {
        std::cout << "[AutoRetopo] Triangulation produced no geometry" << std::endl;
        return;
    }

    // Destroy old GPU model if one exists
    uint32_t oldHandle = retopoObj->getBufferHandle();
    if (oldHandle != UINT32_MAX) {
        m_ctx.modelRenderer.destroyModel(oldHandle);
    }

    // Create new GPU model
    uint32_t newHandle = m_ctx.modelRenderer.createModel(gpuVertices, gpuIndices, nullptr, 0, 0);
    retopoObj->setBufferHandle(newHandle);
    retopoObj->setIndexCount(static_cast<uint32_t>(gpuIndices.size()));
    retopoObj->setVertexCount(static_cast<uint32_t>(gpuVertices.size()));
    retopoObj->setMeshData(gpuVertices, gpuIndices);
    retopoObj->setVisible(true);

    // Store half-edge data on the scene object
    const auto& heVerts = retopoMesh.getVerticesData();
    const auto& heHalfEdges = retopoMesh.getHalfEdges();
    const auto& heFaces = retopoMesh.getFacesData();

    std::vector<SceneObject::StoredHEVertex> storedVerts;
    storedVerts.reserve(heVerts.size());
    for (const auto& v : heVerts) {
        storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
    }

    std::vector<SceneObject::StoredHalfEdge> storedHE;
    storedHE.reserve(heHalfEdges.size());
    for (const auto& he : heHalfEdges) {
        storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
    }

    std::vector<SceneObject::StoredHEFace> storedFaces;
    storedFaces.reserve(heFaces.size());
    for (const auto& f : heFaces) {
        storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
    }

    retopoObj->setEditableMeshData(storedVerts, storedHE, storedFaces);

    // Compute local bounds
    AABB bounds;
    bounds.min = glm::vec3(INFINITY);
    bounds.max = glm::vec3(-INFINITY);
    for (const auto& v : gpuVertices) {
        bounds.min = glm::min(bounds.min, v.position);
        bounds.max = glm::max(bounds.max, v.position);
    }
    retopoObj->setLocalBounds(bounds);

    // Select the retopo object and load its mesh into the editor
    m_ctx.selectedObject = retopoObj;
    m_ctx.editableMesh = retopoMesh;
    m_ctx.meshDirty = false;

    // Build faceToTriangles mapping (required for face/edge selection)
    m_ctx.faceToTriangles.clear();
    uint32_t triIndex = 0;
    for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
        uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
        uint32_t faceTriCount = (vertCount >= 3) ? (vertCount - 2) : 0;
        for (uint32_t i = 0; i < faceTriCount; ++i) {
            m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
        }
    }

    // Clear any stale selection state
    m_ctx.selectedFaces.clear();
    m_ctx.hiddenFaces.clear();

    // Exit retopo mode if active
    m_retopologyMode = false;
    m_retopologyQuads.clear();
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();
    m_retopologyObjCreated = false;

    std::cout << "[AutoRetopo] Phase 5 complete: " << retopoMesh.getFaceCount()
              << " quad faces, " << quadVerts.size() << " vertices, "
              << gpuIndices.size() / 3 << " triangles" << std::endl;
    std::cout << "[AutoRetopo] Done! Mesh is selected and ready for editing." << std::endl;
}

// =============================================================================
// Quad Blanket Retopology — view-based grid projection onto mesh surface
// Like draping graph paper onto a sculpture from the camera's viewpoint.
// =============================================================================

void ModelingMode::quadBlanketRetopology() {
    if (!m_retopologyLiveObj) {
        std::cout << "[QuadBlanket] No live object set" << std::endl;
        return;
    }

    // Guard: make sure the live object isn't a retopo blanket mesh itself
    const std::string& liveName = m_retopologyLiveObj->getName();
    if (liveName.rfind("retopo_blanket", 0) == 0) {
        std::cout << "[QuadBlanket] Live object is a retopo mesh — select the original model as live first" << std::endl;
        return;
    }

    const int resX = m_quadBlanketResX;
    const int resY = m_quadBlanketResY;
    const int smoothIter = m_quadBlanketSmoothIter;
    const bool trimPartial = m_quadBlanketTrimPartial;
    const float padding = m_quadBlanketPadding;

    std::cout << "[QuadBlanket] Starting (grid=" << resX << "x" << resY
              << ", smooth=" << smoothIter << ", trim=" << trimPartial
              << ", pad=" << padding << ")" << std::endl;

    // Get view direction from camera — we only use the DIRECTION, not position
    Camera& cam = m_ctx.splitView ? m_ctx.camera2 : m_ctx.camera;
    glm::vec3 viewDir = glm::normalize(cam.getFront());

    std::cout << "[QuadBlanket] Camera front=(" << viewDir.x << "," << viewDir.y << "," << viewDir.z
              << ") pos=(" << cam.getPosition().x << "," << cam.getPosition().y << "," << cam.getPosition().z << ")" << std::endl;

    // Build orthonormal basis from view direction
    // gridRight = perpendicular to viewDir in horizontal plane
    // gridUp = perpendicular to both
    glm::vec3 gridRight, gridUp;
    if (std::abs(glm::dot(viewDir, glm::vec3(0, 1, 0))) > 0.99f) {
        // Looking straight up/down — use Z as "up" reference
        gridRight = glm::normalize(glm::cross(viewDir, glm::vec3(0, 0, 1)));
    } else {
        gridRight = glm::normalize(glm::cross(viewDir, glm::vec3(0, 1, 0)));
    }
    gridUp = glm::normalize(glm::cross(gridRight, viewDir));

    // ========================================================================
    // Phase 1: Transform mesh to world space, compute projected AABB
    // ========================================================================

    const auto& verts = m_retopologyLiveObj->getVertices();
    const auto& indices = m_retopologyLiveObj->getIndices();
    uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);

    if (triCount == 0) {
        std::cout << "[QuadBlanket] Live object has no triangles" << std::endl;
        return;
    }

    glm::mat4 worldMatrix = m_retopologyLiveObj->getTransform().getMatrix();
    std::vector<glm::vec3> worldVerts(verts.size());
    glm::vec3 meshCenter(0.0f);
    for (size_t i = 0; i < verts.size(); ++i) {
        glm::vec4 wp = worldMatrix * glm::vec4(verts[i].position, 1.0f);
        worldVerts[i] = glm::vec3(wp);
        meshCenter += worldVerts[i];
    }
    meshCenter /= float(verts.size());

    // Project all vertices onto gridRight/gridUp plane to find 2D extents
    // Use meshCenter as the reference origin (NOT camera position)
    float minU = INFINITY, maxU = -INFINITY;
    float minV = INFINITY, maxV = -INFINITY;
    float minDepth = INFINITY, maxDepth = -INFINITY;
    for (const auto& wv : worldVerts) {
        glm::vec3 rel = wv - meshCenter;
        float u = glm::dot(rel, gridRight);
        float v = glm::dot(rel, gridUp);
        float d = glm::dot(rel, viewDir);
        minU = std::min(minU, u);  maxU = std::max(maxU, u);
        minV = std::min(minV, v);  maxV = std::max(maxV, v);
        minDepth = std::min(minDepth, d);  maxDepth = std::max(maxDepth, d);
    }

    // Add padding
    float spanU = maxU - minU;
    float spanV = maxV - minV;
    minU -= spanU * padding;  maxU += spanU * padding;
    minV -= spanV * padding;  maxV += spanV * padding;
    spanU = maxU - minU;
    spanV = maxV - minV;

    float depthRange = maxDepth - minDepth;

    std::cout << "[QuadBlanket] Phase 1: mesh center=(" << meshCenter.x << "," << meshCenter.y << "," << meshCenter.z
              << ") spanU=" << spanU << " spanV=" << spanV << " depth=" << depthRange
              << " tris=" << triCount << " verts=" << verts.size() << std::endl;

    // ========================================================================
    // Phase 2: Generate grid + raycast onto front surface
    // ========================================================================
    // For each grid cell, shoot a ray from well in front of the mesh along
    // viewDir. All rays are parallel (ortho projection from the view).
    // We use the mesh center as anchor — no camera position dependency.

    int numVertsX = resX + 1;
    int numVertsY = resY + 1;
    int totalGridVerts = numVertsX * numVertsY;

    // Ray origin plane: meshCenter - viewDir * (depthRange + offset)
    // This puts ray origins well in front of the mesh
    float rayOffset = depthRange * 0.5f + 2.0f;
    glm::vec3 rayPlaneCenter = meshCenter - viewDir * rayOffset;

    std::vector<glm::vec3> projectedVerts(totalGridVerts);
    std::vector<bool> vertHit(totalGridVerts, false);
    int hitCount = 0;

    for (int iy = 0; iy < numVertsY; ++iy) {
        for (int ix = 0; ix < numVertsX; ++ix) {
            int idx = iy * numVertsX + ix;

            float u = minU + (float(ix) / float(resX)) * spanU;
            float v = minV + (float(iy) / float(resY)) * spanV;

            // Ray origin on the plane in front of the mesh
            glm::vec3 rayOrigin = rayPlaneCenter + gridRight * u + gridUp * v;
            glm::vec3 rayDir = viewDir;

            // Test all front-facing triangles, find closest hit
            float closestT = std::numeric_limits<float>::max();
            glm::vec3 closestPos;
            bool foundHit = false;

            for (uint32_t ti = 0; ti < triCount; ++ti) {
                const glm::vec3& wv0 = worldVerts[indices[ti * 3]];
                const glm::vec3& wv1 = worldVerts[indices[ti * 3 + 1]];
                const glm::vec3& wv2 = worldVerts[indices[ti * 3 + 2]];

                glm::vec3 edge1 = wv1 - wv0;
                glm::vec3 edge2 = wv2 - wv0;

                // Front-face check: triangle normal must face against viewDir
                glm::vec3 triN = glm::cross(edge1, edge2);
                if (glm::dot(triN, viewDir) > 0.0f) continue;  // Back-facing

                // Moller-Trumbore
                glm::vec3 h = glm::cross(rayDir, edge2);
                float a = glm::dot(edge1, h);
                if (std::abs(a) < 1e-8f) continue;

                float f = 1.0f / a;
                glm::vec3 s = rayOrigin - wv0;
                float bu = f * glm::dot(s, h);
                if (bu < 0.0f || bu > 1.0f) continue;

                glm::vec3 q = glm::cross(s, edge1);
                float bv = f * glm::dot(rayDir, q);
                if (bv < 0.0f || bu + bv > 1.0f) continue;

                float t = f * glm::dot(edge2, q);
                if (t > 0.001f && t < closestT) {
                    closestT = t;
                    closestPos = rayOrigin + rayDir * t;
                    foundHit = true;
                }
            }

            if (foundHit) {
                projectedVerts[idx] = closestPos;
                vertHit[idx] = true;
                hitCount++;
            }
        }
    }

    std::cout << "[QuadBlanket] Phase 2: " << hitCount << " / " << totalGridVerts
              << " rays hit front surface" << std::endl;

    if (hitCount == 0) {
        std::cout << "[QuadBlanket] No hits — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 3: Build quad faces + cull
    // ========================================================================

    std::vector<std::array<uint32_t, 4>> quadFaces;
    for (int iy = 0; iy < resY; ++iy) {
        for (int ix = 0; ix < resX; ++ix) {
            uint32_t v00 = iy * numVertsX + ix;
            uint32_t v10 = iy * numVertsX + ix + 1;
            uint32_t v11 = (iy + 1) * numVertsX + ix + 1;
            uint32_t v01 = (iy + 1) * numVertsX + ix;

            if (trimPartial) {
                if (vertHit[v00] && vertHit[v10] && vertHit[v11] && vertHit[v01])
                    quadFaces.push_back({v00, v10, v11, v01});
            } else {
                if (vertHit[v00] || vertHit[v10] || vertHit[v11] || vertHit[v01])
                    quadFaces.push_back({v00, v10, v11, v01});
            }
        }
    }

    // Stretch cull: remove quads that bridge across gaps between body parts
    float cellSize = std::max(spanU / float(resX), spanV / float(resY));
    // Max allowed edge: either 5x the cell size or proportional to mesh depth
    float maxEdgeLen = std::max(cellSize * 5.0f, depthRange * 0.4f);

    size_t beforeCull = quadFaces.size();
    quadFaces.erase(std::remove_if(quadFaces.begin(), quadFaces.end(),
        [&](const std::array<uint32_t, 4>& face) {
            for (int i = 0; i < 4; ++i) {
                if (!vertHit[face[i]] || !vertHit[face[(i + 1) % 4]]) return true;
                float len = glm::length(projectedVerts[face[i]] - projectedVerts[face[(i + 1) % 4]]);
                if (len > maxEdgeLen) return true;
            }
            return false;
        }), quadFaces.end());

    std::cout << "[QuadBlanket] Phase 3: " << quadFaces.size() << " quads ("
              << (beforeCull - quadFaces.size()) << " culled, maxEdge=" << maxEdgeLen << ")" << std::endl;

    if (quadFaces.empty()) {
        std::cout << "[QuadBlanket] No quads survived — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 4: Compact vertices (remove unreferenced)
    // ========================================================================

    std::vector<bool> vertUsed(totalGridVerts, false);
    for (const auto& face : quadFaces) {
        for (int i = 0; i < 4; ++i) vertUsed[face[i]] = true;
    }

    std::vector<uint32_t> remap(totalGridVerts, UINT32_MAX);
    std::vector<glm::vec3> compactVerts;
    for (int i = 0; i < totalGridVerts; ++i) {
        if (vertUsed[i]) {
            remap[i] = static_cast<uint32_t>(compactVerts.size());
            compactVerts.push_back(projectedVerts[i]);
        }
    }
    for (auto& face : quadFaces) {
        for (int i = 0; i < 4; ++i) face[i] = remap[face[i]];
    }

    std::cout << "[QuadBlanket] Phase 4: " << compactVerts.size() << " vertices" << std::endl;

    // ========================================================================
    // Phase 5: Laplacian smoothing + re-projection onto surface
    // ========================================================================

    if (smoothIter > 0) {
        std::vector<std::vector<uint32_t>> adjacency(compactVerts.size());
        for (const auto& face : quadFaces) {
            for (int i = 0; i < 4; ++i) {
                uint32_t a = face[i], b = face[(i + 1) % 4];
                if (std::find(adjacency[a].begin(), adjacency[a].end(), b) == adjacency[a].end())
                    adjacency[a].push_back(b);
                if (std::find(adjacency[b].begin(), adjacency[b].end(), a) == adjacency[b].end())
                    adjacency[b].push_back(a);
            }
        }

        for (int iter = 0; iter < smoothIter; ++iter) {
            std::vector<glm::vec3> newPos = compactVerts;
            for (size_t vi = 0; vi < compactVerts.size(); ++vi) {
                if (adjacency[vi].empty()) continue;
                glm::vec3 avg(0.0f);
                for (uint32_t ni : adjacency[vi]) avg += compactVerts[ni];
                avg /= float(adjacency[vi].size());
                newPos[vi] = glm::mix(compactVerts[vi], avg, 0.5f);
            }

            // Re-project smoothed positions onto surface using viewDir rays
            for (size_t vi = 0; vi < newPos.size(); ++vi) {
                glm::vec3 pos = newPos[vi];
                // Shoot ray from offset position along viewDir
                glm::vec3 origin = pos - viewDir * (depthRange + 1.0f);
                float closestT = std::numeric_limits<float>::max();
                glm::vec3 closestPos;
                bool found = false;
                for (uint32_t ti = 0; ti < triCount; ++ti) {
                    const glm::vec3& wv0 = worldVerts[indices[ti * 3]];
                    const glm::vec3& wv1 = worldVerts[indices[ti * 3 + 1]];
                    const glm::vec3& wv2 = worldVerts[indices[ti * 3 + 2]];
                    glm::vec3 edge1 = wv1 - wv0, edge2 = wv2 - wv0;
                    glm::vec3 triN = glm::cross(edge1, edge2);
                    if (glm::dot(triN, viewDir) > 0.0f) continue;
                    glm::vec3 h = glm::cross(viewDir, edge2);
                    float a = glm::dot(edge1, h);
                    if (std::abs(a) < 1e-8f) continue;
                    float f = 1.0f / a;
                    glm::vec3 s = origin - wv0;
                    float bu = f * glm::dot(s, h);
                    if (bu < 0.0f || bu > 1.0f) continue;
                    glm::vec3 q = glm::cross(s, edge1);
                    float bv = f * glm::dot(viewDir, q);
                    if (bv < 0.0f || bu + bv > 1.0f) continue;
                    float t = f * glm::dot(edge2, q);
                    if (t > 0.001f && t < closestT) {
                        closestT = t;
                        closestPos = origin + viewDir * t;
                        found = true;
                    }
                }
                if (found) newPos[vi] = closestPos;
            }
            compactVerts = newPos;
        }
        std::cout << "[QuadBlanket] Phase 5: " << smoothIter << " smoothing iterations done" << std::endl;
    }

    // ========================================================================
    // Phase 6: Build EditableMesh + GPU Upload + Scene Object
    // ========================================================================

    EditableMesh retopoMesh;
    for (size_t vi = 0; vi < compactVerts.size(); ++vi) {
        HEVertex v;
        v.position = compactVerts[vi];
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        v.uv = glm::vec2(0.0f);
        v.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        retopoMesh.addVertex(v);
    }

    retopoMesh.addQuadFacesBatch(quadFaces);
    retopoMesh.recalculateNormals();

    // Create a new retopo scene object with a unique name
    int blanketIdx = 1;
    for (auto& obj : m_ctx.sceneObjects) {
        if (obj->getName().rfind("retopo_blanket", 0) == 0)
            blanketIdx++;
    }
    std::string retopoName = "retopo_blanket_" + std::to_string(blanketIdx);

    auto newObj = std::make_unique<SceneObject>(retopoName);
    newObj->setDescription("Quad blanket retopology mesh");
    SceneObject* retopoObj = newObj.get();
    m_ctx.sceneObjects.push_back(std::move(newObj));
    std::cout << "[QuadBlanket] Created scene object: " << retopoName << std::endl;

    // Triangulate for GPU
    std::vector<ModelVertex> gpuVertices;
    std::vector<uint32_t> gpuIndices;
    std::set<uint32_t> noHidden;
    retopoMesh.triangulate(gpuVertices, gpuIndices, noHidden);

    if (gpuIndices.empty()) {
        std::cout << "[QuadBlanket] Triangulation produced no geometry" << std::endl;
        return;
    }

    uint32_t newHandle = m_ctx.modelRenderer.createModel(gpuVertices, gpuIndices, nullptr, 0, 0);
    retopoObj->setBufferHandle(newHandle);
    retopoObj->setIndexCount(static_cast<uint32_t>(gpuIndices.size()));
    retopoObj->setVertexCount(static_cast<uint32_t>(gpuVertices.size()));
    retopoObj->setMeshData(gpuVertices, gpuIndices);
    retopoObj->setVisible(true);

    // Store half-edge data
    const auto& heVerts = retopoMesh.getVerticesData();
    const auto& heHalfEdges = retopoMesh.getHalfEdges();
    const auto& heFaces = retopoMesh.getFacesData();

    std::vector<SceneObject::StoredHEVertex> storedVerts;
    storedVerts.reserve(heVerts.size());
    for (const auto& v : heVerts)
        storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});

    std::vector<SceneObject::StoredHalfEdge> storedHE;
    storedHE.reserve(heHalfEdges.size());
    for (const auto& he : heHalfEdges)
        storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});

    std::vector<SceneObject::StoredHEFace> storedFaces;
    storedFaces.reserve(heFaces.size());
    for (const auto& f : heFaces)
        storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});

    retopoObj->setEditableMeshData(storedVerts, storedHE, storedFaces);

    // Bounds
    AABB bounds;
    bounds.min = glm::vec3(INFINITY);
    bounds.max = glm::vec3(-INFINITY);
    for (const auto& v : gpuVertices) {
        bounds.min = glm::min(bounds.min, v.position);
        bounds.max = glm::max(bounds.max, v.position);
    }
    retopoObj->setLocalBounds(bounds);

    // Select and load into editor
    m_ctx.selectedObject = retopoObj;
    m_ctx.editableMesh = retopoMesh;
    m_ctx.meshDirty = false;
    invalidateWireframeCache();

    m_ctx.faceToTriangles.clear();
    uint32_t triIdx = 0;
    for (uint32_t fi = 0; fi < m_ctx.editableMesh.getFaceCount(); ++fi) {
        uint32_t vc = m_ctx.editableMesh.getFace(fi).vertexCount;
        uint32_t ftc = (vc >= 3) ? (vc - 2) : 0;
        for (uint32_t i = 0; i < ftc; ++i)
            m_ctx.faceToTriangles[fi].push_back(triIdx++);
    }

    m_ctx.selectedFaces.clear();
    m_ctx.hiddenFaces.clear();
    m_retopologyMode = false;
    m_retopologyQuads.clear();
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();
    m_retopologyObjCreated = false;

    std::cout << "[QuadBlanket] Done! " << retopoMesh.getFaceCount() << " quads, "
              << compactVerts.size() << " verts, " << gpuIndices.size() / 3 << " tris" << std::endl;
}

// =============================================================================
// Patch Blanket — targeted rectangle projection into manual retopo quads
// Like Quad Blanket but only within a screen-space rectangle, and results go
// into m_retopologyQuads for the manual retopo workflow.
// =============================================================================

void ModelingMode::executePatchBlanket() {
    if (!m_retopologyLiveObj) {
        std::cout << "[PatchBlanket] No live object set" << std::endl;
        return;
    }

    const int resX = m_quadBlanketResX;
    const int resY = m_quadBlanketResY;
    const int smoothIter = m_quadBlanketSmoothIter;
    const bool trimPartial = m_quadBlanketTrimPartial;

    // Normalize the screen rect (start may be > end if dragged right-to-left)
    glm::vec2 rectMin = glm::min(m_patchBlanketStart, m_patchBlanketEnd);
    glm::vec2 rectMax = glm::max(m_patchBlanketStart, m_patchBlanketEnd);

    float rectW = rectMax.x - rectMin.x;
    float rectH = rectMax.y - rectMin.y;
    if (rectW < 10.0f || rectH < 10.0f) {
        std::cout << "[PatchBlanket] Rectangle too small (" << rectW << "x" << rectH << ")" << std::endl;
        return;
    }

    std::cout << "[PatchBlanket] Starting (grid=" << resX << "x" << resY
              << ", rect=" << rectMin.x << "," << rectMin.y << " to " << rectMax.x << "," << rectMax.y
              << ", smooth=" << smoothIter << ")" << std::endl;

    // Get camera for view direction and screen projection
    Camera& cam = m_ctx.splitView ? m_ctx.camera2 : m_ctx.camera;
    glm::vec3 viewDir = glm::normalize(cam.getFront());

    // Build orthonormal basis from view direction
    glm::vec3 gridRight, gridUp;
    if (std::abs(glm::dot(viewDir, glm::vec3(0, 1, 0))) > 0.99f) {
        gridRight = glm::normalize(glm::cross(viewDir, glm::vec3(0, 0, 1)));
    } else {
        gridRight = glm::normalize(glm::cross(viewDir, glm::vec3(0, 1, 0)));
    }
    gridUp = glm::normalize(glm::cross(gridRight, viewDir));

    // ========================================================================
    // Phase 1: Transform mesh to world space, compute projected AABB
    // ========================================================================

    const auto& verts = m_retopologyLiveObj->getVertices();
    const auto& indices = m_retopologyLiveObj->getIndices();
    uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);

    if (triCount == 0) {
        std::cout << "[PatchBlanket] Live object has no triangles" << std::endl;
        return;
    }

    glm::mat4 worldMatrix = m_retopologyLiveObj->getTransform().getMatrix();
    std::vector<glm::vec3> worldVerts(verts.size());
    glm::vec3 meshCenter(0.0f);
    for (size_t i = 0; i < verts.size(); ++i) {
        glm::vec4 wp = worldMatrix * glm::vec4(verts[i].position, 1.0f);
        worldVerts[i] = glm::vec3(wp);
        meshCenter += worldVerts[i];
    }
    meshCenter /= float(verts.size());

    // Project all vertices onto gridRight/gridUp plane to find 2D extents
    float minU = INFINITY, maxU = -INFINITY;
    float minV = INFINITY, maxV = -INFINITY;
    float minDepth = INFINITY, maxDepth = -INFINITY;
    for (const auto& wv : worldVerts) {
        glm::vec3 rel = wv - meshCenter;
        float u = glm::dot(rel, gridRight);
        float v = glm::dot(rel, gridUp);
        float d = glm::dot(rel, viewDir);
        minU = std::min(minU, u);  maxU = std::max(maxU, u);
        minV = std::min(minV, v);  maxV = std::max(maxV, v);
        minDepth = std::min(minDepth, d);  maxDepth = std::max(maxDepth, d);
    }

    float padding = m_quadBlanketPadding;
    float spanU = maxU - minU;
    float spanV = maxV - minV;
    minU -= spanU * padding;  maxU += spanU * padding;
    minV -= spanV * padding;  maxV += spanV * padding;
    spanU = maxU - minU;
    spanV = maxV - minV;

    float depthRange = maxDepth - minDepth;

    // ========================================================================
    // Phase 2: Generate grid + raycast, but only for vertices inside screen rect
    // ========================================================================

    // Set up screen-space projection
    float vpW = static_cast<float>(m_ctx.window.getWidth());
    float vpH = static_cast<float>(m_ctx.window.getHeight());
    float aspect = vpW / vpH;
    glm::mat4 viewMat = cam.getViewMatrix();
    glm::mat4 projMat = cam.getProjectionMatrix(aspect);
    glm::mat4 vpMat = projMat * viewMat;

    auto worldToScreen = [&](const glm::vec3& pos) -> glm::vec2 {
        glm::vec4 clip = vpMat * glm::vec4(pos, 1.0f);
        if (clip.w <= 0.0f) return glm::vec2(-10000.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);
    };

    int numVertsX = resX + 1;
    int numVertsY = resY + 1;
    int totalGridVerts = numVertsX * numVertsY;

    float rayOffset = depthRange * 0.5f + 2.0f;
    glm::vec3 rayPlaneCenter = meshCenter - viewDir * rayOffset;

    std::vector<glm::vec3> projectedVerts(totalGridVerts);
    std::vector<bool> vertHit(totalGridVerts, false);
    int hitCount = 0;

    for (int iy = 0; iy < numVertsY; ++iy) {
        for (int ix = 0; ix < numVertsX; ++ix) {
            int idx = iy * numVertsX + ix;

            float u = minU + (float(ix) / float(resX)) * spanU;
            float v = minV + (float(iy) / float(resY)) * spanV;

            // Compute the world position this grid vertex would be at (on ray plane)
            glm::vec3 gridWorldPos = rayPlaneCenter + gridRight * u + gridUp * v;

            // Check if screen projection falls inside the user's rectangle
            glm::vec2 screenPos = worldToScreen(gridWorldPos);
            if (screenPos.x < rectMin.x || screenPos.x > rectMax.x ||
                screenPos.y < rectMin.y || screenPos.y > rectMax.y) {
                continue;  // Outside the rectangle — skip
            }

            // Ray origin on the plane in front of the mesh
            glm::vec3 rayOrigin = gridWorldPos;
            glm::vec3 rayDir = viewDir;

            // Test all front-facing triangles, find closest hit
            float closestT = std::numeric_limits<float>::max();
            glm::vec3 closestPos;
            bool foundHit = false;

            for (uint32_t ti = 0; ti < triCount; ++ti) {
                const glm::vec3& wv0 = worldVerts[indices[ti * 3]];
                const glm::vec3& wv1 = worldVerts[indices[ti * 3 + 1]];
                const glm::vec3& wv2 = worldVerts[indices[ti * 3 + 2]];

                glm::vec3 edge1 = wv1 - wv0;
                glm::vec3 edge2 = wv2 - wv0;

                glm::vec3 triN = glm::cross(edge1, edge2);
                if (glm::dot(triN, viewDir) > 0.0f) continue;  // Back-facing

                glm::vec3 h = glm::cross(rayDir, edge2);
                float a = glm::dot(edge1, h);
                if (std::abs(a) < 1e-8f) continue;

                float f = 1.0f / a;
                glm::vec3 s = rayOrigin - wv0;
                float bu = f * glm::dot(s, h);
                if (bu < 0.0f || bu > 1.0f) continue;

                glm::vec3 q = glm::cross(s, edge1);
                float bv = f * glm::dot(rayDir, q);
                if (bv < 0.0f || bu + bv > 1.0f) continue;

                float t = f * glm::dot(edge2, q);
                if (t > 0.001f && t < closestT) {
                    closestT = t;
                    closestPos = rayOrigin + rayDir * t;
                    foundHit = true;
                }
            }

            if (foundHit) {
                projectedVerts[idx] = closestPos;
                vertHit[idx] = true;
                hitCount++;
            }
        }
    }

    std::cout << "[PatchBlanket] Phase 2: " << hitCount << " / " << totalGridVerts
              << " rays hit (within rect)" << std::endl;

    if (hitCount == 0) {
        std::cout << "[PatchBlanket] No hits within rectangle — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 3: Build quad faces + cull
    // ========================================================================

    std::vector<std::array<uint32_t, 4>> quadFaces;
    for (int iy = 0; iy < resY; ++iy) {
        for (int ix = 0; ix < resX; ++ix) {
            uint32_t v00 = iy * numVertsX + ix;
            uint32_t v10 = iy * numVertsX + ix + 1;
            uint32_t v11 = (iy + 1) * numVertsX + ix + 1;
            uint32_t v01 = (iy + 1) * numVertsX + ix;

            if (trimPartial) {
                if (vertHit[v00] && vertHit[v10] && vertHit[v11] && vertHit[v01])
                    quadFaces.push_back({v00, v10, v11, v01});
            } else {
                if (vertHit[v00] || vertHit[v10] || vertHit[v11] || vertHit[v01])
                    quadFaces.push_back({v00, v10, v11, v01});
            }
        }
    }

    // Stretch cull
    float cellSize = std::max(spanU / float(resX), spanV / float(resY));
    float maxEdgeLen = std::max(cellSize * 5.0f, depthRange * 0.4f);

    size_t beforeCull = quadFaces.size();
    quadFaces.erase(std::remove_if(quadFaces.begin(), quadFaces.end(),
        [&](const std::array<uint32_t, 4>& face) {
            for (int i = 0; i < 4; ++i) {
                if (!vertHit[face[i]] || !vertHit[face[(i + 1) % 4]]) return true;
                float len = glm::length(projectedVerts[face[i]] - projectedVerts[face[(i + 1) % 4]]);
                if (len > maxEdgeLen) return true;
            }
            return false;
        }), quadFaces.end());

    std::cout << "[PatchBlanket] Phase 3: " << quadFaces.size() << " quads ("
              << (beforeCull - quadFaces.size()) << " culled)" << std::endl;

    if (quadFaces.empty()) {
        std::cout << "[PatchBlanket] No quads survived — aborting" << std::endl;
        return;
    }

    // ========================================================================
    // Phase 4: Compact vertices
    // ========================================================================

    std::vector<bool> vertUsed(totalGridVerts, false);
    for (const auto& face : quadFaces) {
        for (int i = 0; i < 4; ++i) vertUsed[face[i]] = true;
    }

    std::vector<uint32_t> remap(totalGridVerts, UINT32_MAX);
    std::vector<glm::vec3> compactVerts;
    for (int i = 0; i < totalGridVerts; ++i) {
        if (vertUsed[i]) {
            remap[i] = static_cast<uint32_t>(compactVerts.size());
            compactVerts.push_back(projectedVerts[i]);
        }
    }
    for (auto& face : quadFaces) {
        for (int i = 0; i < 4; ++i) face[i] = remap[face[i]];
    }

    // ========================================================================
    // Phase 5: Laplacian smoothing + re-projection
    // ========================================================================

    if (smoothIter > 0) {
        std::vector<std::vector<uint32_t>> adjacency(compactVerts.size());
        for (const auto& face : quadFaces) {
            for (int i = 0; i < 4; ++i) {
                uint32_t a = face[i], b = face[(i + 1) % 4];
                if (std::find(adjacency[a].begin(), adjacency[a].end(), b) == adjacency[a].end())
                    adjacency[a].push_back(b);
                if (std::find(adjacency[b].begin(), adjacency[b].end(), a) == adjacency[b].end())
                    adjacency[b].push_back(a);
            }
        }

        for (int iter = 0; iter < smoothIter; ++iter) {
            std::vector<glm::vec3> newPos = compactVerts;
            for (size_t vi = 0; vi < compactVerts.size(); ++vi) {
                if (adjacency[vi].empty()) continue;
                glm::vec3 avg(0.0f);
                for (uint32_t ni : adjacency[vi]) avg += compactVerts[ni];
                avg /= float(adjacency[vi].size());
                newPos[vi] = glm::mix(compactVerts[vi], avg, 0.5f);
            }

            // Re-project smoothed positions onto surface
            for (size_t vi = 0; vi < newPos.size(); ++vi) {
                glm::vec3 pos = newPos[vi];
                glm::vec3 origin = pos - viewDir * (depthRange + 1.0f);
                float closestT = std::numeric_limits<float>::max();
                glm::vec3 closestPos;
                bool found = false;
                for (uint32_t ti = 0; ti < triCount; ++ti) {
                    const glm::vec3& wv0 = worldVerts[indices[ti * 3]];
                    const glm::vec3& wv1 = worldVerts[indices[ti * 3 + 1]];
                    const glm::vec3& wv2 = worldVerts[indices[ti * 3 + 2]];
                    glm::vec3 edge1 = wv1 - wv0, edge2 = wv2 - wv0;
                    glm::vec3 triN = glm::cross(edge1, edge2);
                    if (glm::dot(triN, viewDir) > 0.0f) continue;
                    glm::vec3 h = glm::cross(viewDir, edge2);
                    float a = glm::dot(edge1, h);
                    if (std::abs(a) < 1e-8f) continue;
                    float f = 1.0f / a;
                    glm::vec3 s = origin - wv0;
                    float bu = f * glm::dot(s, h);
                    if (bu < 0.0f || bu > 1.0f) continue;
                    glm::vec3 q = glm::cross(s, edge1);
                    float bv = f * glm::dot(viewDir, q);
                    if (bv < 0.0f || bu + bv > 1.0f) continue;
                    float t = f * glm::dot(edge2, q);
                    if (t > 0.001f && t < closestT) {
                        closestT = t;
                        closestPos = origin + viewDir * t;
                        found = true;
                    }
                }
                if (found) newPos[vi] = closestPos;
            }
            compactVerts = newPos;
        }
        std::cout << "[PatchBlanket] Phase 5: " << smoothIter << " smoothing iterations done" << std::endl;
    }

    // ========================================================================
    // Phase 6: Append to m_retopologyQuads (NO scene object creation)
    // ========================================================================

    size_t addedQuads = 0;
    for (const auto& face : quadFaces) {
        RetopologyQuad rq;
        rq.verts[0] = compactVerts[face[0]];
        rq.verts[1] = compactVerts[face[1]];
        rq.verts[2] = compactVerts[face[2]];
        rq.verts[3] = compactVerts[face[3]];
        m_retopologyQuads.push_back(rq);
        addedQuads++;
    }

    // Ensure retopo mode is active so the overlay draws the new quads
    m_retopologyMode = true;

    std::cout << "[PatchBlanket] Done! Added " << addedQuads << " quads to retopo accumulator (total: "
              << m_retopologyQuads.size() << ")" << std::endl;
}

void ModelingMode::scatterPointsOnSurface() {
    if (!m_retopologyLiveObj || !m_retopologyLiveObj->hasMeshData()) {
        std::cout << "[Scatter] No live object set" << std::endl;
        return;
    }

    float spacing = m_scatterSpacing * 0.01f;  // cm → model units (1 unit = 1m)
    if (spacing < 0.001f) spacing = 0.001f;

    const auto& verts = m_retopologyLiveObj->getVertices();
    const auto& indices = m_retopologyLiveObj->getIndices();
    glm::mat4 modelMat = m_retopologyLiveObj->getTransform().getMatrix();
    glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(modelMat)));

    // Transform all vertices and normals to world space
    struct WVert { glm::vec3 pos, nrm; };
    std::vector<WVert> worldVerts(verts.size());
    for (size_t i = 0; i < verts.size(); i++) {
        worldVerts[i].pos = glm::vec3(modelMat * glm::vec4(verts[i].position, 1.0f));
        worldVerts[i].nrm = glm::normalize(normalMat * verts[i].normal);
    }

    // Find bounding box and global center for consistent ring alignment
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    glm::vec3 globalCenter(0.0f);
    for (const auto& wv : worldVerts) {
        bmin = glm::min(bmin, wv.pos);
        bmax = glm::max(bmax, wv.pos);
        globalCenter += wv.pos;
    }
    globalCenter /= static_cast<float>(worldVerts.size());
    float minY = bmin.y, maxY = bmax.y;

    // Clear existing retopo verts and ring info (keep existing quads)
    m_scatterRings.clear();
    m_scatterEdges.clear();
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();

    int totalPoints = 0;
    int ringCount = 0;
    int targetPointCount = -1;  // Set by first valid ring, then forced on all subsequent rings

    // Slice from bottom to top at spacing intervals
    for (float sliceY = minY + spacing * 0.5f; sliceY < maxY; sliceY += spacing) {

        // ── Intersect all triangles with horizontal plane Y = sliceY ──
        struct Segment { glm::vec3 a, b; glm::vec3 na, nb; };
        std::vector<Segment> segments;

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3& p0 = worldVerts[indices[i]].pos;
            const glm::vec3& p1 = worldVerts[indices[i + 1]].pos;
            const glm::vec3& p2 = worldVerts[indices[i + 2]].pos;
            const glm::vec3& n0 = worldVerts[indices[i]].nrm;
            const glm::vec3& n1 = worldVerts[indices[i + 1]].nrm;
            const glm::vec3& n2 = worldVerts[indices[i + 2]].nrm;

            // Classify vertices relative to the plane
            float d0 = p0.y - sliceY;
            float d1 = p1.y - sliceY;
            float d2 = p2.y - sliceY;

            // Count how many are above/below (with small epsilon)
            const float eps = 1e-6f;
            int above = (d0 > eps ? 1 : 0) + (d1 > eps ? 1 : 0) + (d2 > eps ? 1 : 0);
            int below = (d0 < -eps ? 1 : 0) + (d1 < -eps ? 1 : 0) + (d2 < -eps ? 1 : 0);

            // Need vertices on both sides for an intersection
            if (above == 0 || below == 0) continue;

            // Find the two intersection points on edges that cross the plane
            glm::vec3 pts[3] = {p0, p1, p2};
            glm::vec3 nrms[3] = {n0, n1, n2};
            float dists[3] = {d0, d1, d2};
            glm::vec3 hitPos[2];
            glm::vec3 hitNrm[2];
            int hitCount = 0;

            for (int e = 0; e < 3 && hitCount < 2; e++) {
                int ea = e, eb = (e + 1) % 3;
                if ((dists[ea] > eps && dists[eb] < -eps) || (dists[ea] < -eps && dists[eb] > eps)) {
                    float t = dists[ea] / (dists[ea] - dists[eb]);
                    hitPos[hitCount] = pts[ea] + (pts[eb] - pts[ea]) * t;
                    hitNrm[hitCount] = glm::normalize(nrms[ea] + (nrms[eb] - nrms[ea]) * t);
                    hitCount++;
                }
            }

            if (hitCount == 2) {
                segments.push_back({hitPos[0], hitPos[1], hitNrm[0], hitNrm[1]});
            }
        }

        if (segments.empty()) continue;

        // ── Chain segments into contour loops ──
        // Compute average segment length for weld tolerance
        float avgSegLen = 0.0f;
        for (const auto& seg : segments) avgSegLen += glm::length(seg.b - seg.a);
        avgSegLen /= std::max(1.0f, static_cast<float>(segments.size()));
        float weldDist = avgSegLen * 1.5f;  // Generous: 1.5x average segment length

        std::vector<bool> used(segments.size(), false);

        // Collect ALL contour loops at this height
        struct ContourLoop {
            std::vector<glm::vec3> pts;
            std::vector<glm::vec3> nrms;
            float length = 0.0f;
        };
        std::vector<ContourLoop> allLoops;

        while (true) {
            int startIdx = -1;
            for (size_t s = 0; s < segments.size(); s++) {
                if (!used[s]) { startIdx = static_cast<int>(s); break; }
            }
            if (startIdx < 0) break;

            ContourLoop loop;
            used[startIdx] = true;
            loop.pts.push_back(segments[startIdx].a);
            loop.pts.push_back(segments[startIdx].b);
            loop.nrms.push_back(segments[startIdx].na);
            loop.nrms.push_back(segments[startIdx].nb);

            bool extended = true;
            while (extended) {
                extended = false;
                glm::vec3 tail = loop.pts.back();
                float bestDist = weldDist;
                int bestSeg = -1;
                bool bestFlip = false;
                for (size_t s = 0; s < segments.size(); s++) {
                    if (used[s]) continue;
                    float da = glm::length(segments[s].a - tail);
                    float db = glm::length(segments[s].b - tail);
                    if (da < bestDist) { bestDist = da; bestSeg = static_cast<int>(s); bestFlip = false; }
                    if (db < bestDist) { bestDist = db; bestSeg = static_cast<int>(s); bestFlip = true; }
                }
                if (bestSeg >= 0) {
                    used[bestSeg] = true;
                    if (bestFlip) {
                        loop.pts.push_back(segments[bestSeg].a);
                        loop.nrms.push_back(segments[bestSeg].na);
                    } else {
                        loop.pts.push_back(segments[bestSeg].b);
                        loop.nrms.push_back(segments[bestSeg].nb);
                    }
                    extended = true;
                }
            }

            // Compute loop length
            for (size_t p = 1; p < loop.pts.size(); p++)
                loop.length += glm::length(loop.pts[p] - loop.pts[p - 1]);

            if (loop.pts.size() >= 3 && loop.length > spacing)
                allLoops.push_back(std::move(loop));
        }

        if (allLoops.empty()) continue;

        // Pick the LONGEST loop — that's the outer surface shell
        size_t longestIdx = 0;
        for (size_t li = 1; li < allLoops.size(); li++) {
            if (allLoops[li].length > allLoops[longestIdx].length)
                longestIdx = li;
        }

        std::vector<glm::vec3> contourPts = std::move(allLoops[longestIdx].pts);
        std::vector<glm::vec3> contourNrms = std::move(allLoops[longestIdx].nrms);

            // ── Ensure consistent winding (counterclockwise on XZ plane viewed from +Y) ──
            // Compute signed area on XZ plane. If negative, reverse the contour.
            {
                float signedArea = 0.0f;
                for (size_t p = 0; p < contourPts.size(); p++) {
                    size_t pNext = (p + 1) % contourPts.size();
                    signedArea += (contourPts[pNext].x - contourPts[p].x) *
                                  (contourPts[pNext].z + contourPts[p].z);
                }
                if (signedArea > 0.0f) {
                    // Clockwise — reverse to counterclockwise
                    std::reverse(contourPts.begin(), contourPts.end());
                    std::reverse(contourNrms.begin(), contourNrms.end());
                }
            }

            // ── Find where contour crosses the sagittal plane ──
            // Sagittal plane: Z = 0 (world origin, model centered on origin).
            // Find crossing on the +X side (right ear area) to avoid mouth interior.
            float planeZ = 0.0f;
            int crossSeg = -1;
            float crossT = 0.0f;
            for (size_t p = 0; p < contourPts.size(); p++) {
                size_t pNext = (p + 1) % contourPts.size();
                float z0 = contourPts[p].z - planeZ;
                float z1 = contourPts[pNext].z - planeZ;
                // Look for sign change (crossing the sagittal plane)
                if ((z0 <= 0.0f && z1 > 0.0f) || (z0 >= 0.0f && z1 < 0.0f)) {
                    float t = z0 / (z0 - z1);
                    glm::vec3 crossPt = contourPts[p] + (contourPts[pNext] - contourPts[p]) * t;
                    // Must be on the +X side (right side of model)
                    if (crossPt.x >= 0.0f) {
                        crossSeg = static_cast<int>(p);
                        crossT = t;
                        break;
                    }
                }
            }
            // Fallback: if no +X crossing found, try any crossing
            if (crossSeg < 0) {
                for (size_t p = 0; p < contourPts.size(); p++) {
                    size_t pNext = (p + 1) % contourPts.size();
                    float z0 = contourPts[p].z - planeZ;
                    float z1 = contourPts[pNext].z - planeZ;
                    if ((z0 <= 0.0f && z1 > 0.0f) || (z0 >= 0.0f && z1 < 0.0f)) {
                        crossSeg = static_cast<int>(p);
                        crossT = z0 / (z0 - z1);
                        break;
                    }
                }
            }

            // Rotate contour so the crossing point segment is at the start
            // Insert the exact crossing point as the new start
            if (crossSeg >= 0) {
                size_t pNext = (crossSeg + 1) % contourPts.size();
                glm::vec3 crossPt = contourPts[crossSeg] + (contourPts[pNext] - contourPts[crossSeg]) * crossT;
                glm::vec3 crossNm = glm::normalize(contourNrms[crossSeg] * (1.0f - crossT) + contourNrms[pNext] * crossT);

                // Rebuild contour starting from the crossing point
                std::vector<glm::vec3> newPts;
                std::vector<glm::vec3> newNrms;
                newPts.push_back(crossPt);
                newNrms.push_back(crossNm);
                // Walk from the segment after the crossing
                for (size_t k = 1; k <= contourPts.size(); k++) {
                    size_t idx = (crossSeg + k) % contourPts.size();
                    newPts.push_back(contourPts[idx]);
                    newNrms.push_back(contourNrms[idx]);
                }
                // Close back to the crossing point
                newPts.push_back(crossPt);
                newNrms.push_back(crossNm);

                contourPts = newPts;
                contourNrms = newNrms;
            }

            // ── Compute total contour length ──
            float totalLen = 0.0f;
            for (size_t p = 1; p < contourPts.size(); p++) {
                totalLen += glm::length(contourPts[p] - contourPts[p - 1]);
            }
            if (totalLen < spacing) continue;  // Skip tiny contours

            // ── Determine point count for this ring (divisible by 4) ──
            int pointsOnRing;
            if (targetPointCount < 0) {
                pointsOnRing = std::max(4, static_cast<int>(std::round(totalLen / spacing)));
                // Round up to nearest multiple of 4
                if (pointsOnRing % 4 != 0) pointsOnRing += (4 - pointsOnRing % 4);
                targetPointCount = pointsOnRing;
            } else {
                pointsOnRing = targetPointCount;
            }

            // ── Distribute pointsOnRing points evenly along the contour ──
            // Point 0 is at the plane crossing (contourPts[0])
            float stepDist = totalLen / pointsOnRing;

            std::vector<float> cumDist(contourPts.size(), 0.0f);
            for (size_t p = 1; p < contourPts.size(); p++) {
                cumDist[p] = cumDist[p - 1] + glm::length(contourPts[p] - contourPts[p - 1]);
            }

            size_t ringStartIdx = m_retopologyVerts.size();

            for (int pi = 0; pi < pointsOnRing; pi++) {
                float targetDist = pi * stepDist;

                size_t seg = 0;
                for (size_t s = 1; s < contourPts.size(); s++) {
                    if (cumDist[s] >= targetDist) { seg = s - 1; break; }
                    seg = s - 1;
                }
                if (seg + 1 >= contourPts.size()) seg = contourPts.size() - 2;

                float segStart = cumDist[seg];
                float segEnd = cumDist[seg + 1];
                float segLen = segEnd - segStart;
                float t = (segLen > 1e-7f) ? (targetDist - segStart) / segLen : 0.0f;
                t = std::max(0.0f, std::min(1.0f, t));

                glm::vec3 pt = contourPts[seg] + (contourPts[seg + 1] - contourPts[seg]) * t;
                glm::vec3 nm = glm::normalize(contourNrms[seg] * (1.0f - t) + contourNrms[seg + 1] * t);

                m_retopologyVerts.push_back(pt);
                m_retopologyNormals.push_back(nm);
                m_retopologyVertMeshIdx.push_back(UINT32_MAX);
                totalPoints++;
            }

            // Record ring with its contour path for later sliding
            size_t ringPtCount = m_retopologyVerts.size() - ringStartIdx;
            if (ringPtCount >= 3) {
                ScatterRing ring;
                ring.startIdx = ringStartIdx;
                ring.count = ringPtCount;
                ring.contourPts = contourPts;
                ring.contourNrms = contourNrms;
                ring.totalLen = totalLen;
                ring.cumDist = cumDist;
                ring.slideOffset = 0.0f;
                m_scatterRings.push_back(std::move(ring));
            }

            ringCount++;
    }

    // ── Slide each ring so its start point lands on Z=0, +X side ──
    // Walk each ring's stored contour, find the distance where Z crosses 0
    // on the +X side, set that as the slideOffset, resample.
    for (size_t ri = 0; ri < m_scatterRings.size(); ri++) {
        auto& ring = m_scatterRings[ri];
        if (ring.contourPts.size() < 3) continue;

        // Walk the contour to find where Z crosses 0 on the +X side
        float bestCrossDist = -1.0f;
        for (size_t p = 0; p < ring.contourPts.size() - 1; p++) {
            float z0 = ring.contourPts[p].z;
            float z1 = ring.contourPts[p + 1].z;

            // Sign change = crossing Z=0
            if ((z0 <= 0.0f && z1 > 0.0f) || (z0 >= 0.0f && z1 < 0.0f)) {
                float t = z0 / (z0 - z1);
                glm::vec3 crossPt = ring.contourPts[p] + (ring.contourPts[p + 1] - ring.contourPts[p]) * t;

                // Must be on +X side
                if (crossPt.x >= 0.0f) {
                    float crossDist = ring.cumDist[p] + t * (ring.cumDist[p + 1] - ring.cumDist[p]);
                    bestCrossDist = crossDist;
                    break;
                }
            }
        }

        if (bestCrossDist >= 0.0f) {
            ring.slideOffset = bestCrossDist;
            resampleRing(ri);
        }
    }

    std::cout << "[Scatter] Slid " << m_scatterRings.size() << " rings to Z=0 seam" << std::endl;

    // ── Level 1 Refinement: lock quarter-points to axis crossings ──
    // Point 0 is already at Z=0 +X (right side).
    // Now lock: N/4 → X=0 +Z (front), N/2 → Z=0 -X (left), 3N/4 → X=0 -Z (back)
    // Then redistribute in-between points evenly within each quarter-arc.
    for (size_t ri = 0; ri < m_scatterRings.size(); ri++) {
        auto& ring = m_scatterRings[ri];
        if (ring.contourPts.size() < 3 || ring.totalLen < 0.001f) continue;
        int N = static_cast<int>(ring.count);
        if (N < 4 || N % 2 != 0) continue;

        int quarter = N / 4;

        // Find contour distances for the 4 axis crossings
        // We need: Z=0 +X (already have as slideOffset), X=0 +Z, Z=0 -X, X=0 -Z
        struct AxisCross { float dist; };
        float crossDists[4];
        crossDists[0] = ring.slideOffset;  // Already computed: Z=0 +X

        // Helper: find contour distance where axis crosses, starting search from a given distance
        auto findCrossing = [&](bool crossZ, float targetVal, bool positiveSide, int sideAxis, float searchStart) -> float {
            // Walk contour looking for the crossing
            // crossZ=true: look for Z=targetVal, check sideAxis for +/- filter
            // crossZ=false: look for X=targetVal
            // Find the crossing closest to 25% of total length from searchStart
            float expectedDist = ring.totalLen * 0.25f;
            float bestCross = -1.0f;
            float bestError = FLT_MAX;

            for (size_t p = 0; p < ring.contourPts.size() - 1; p++) {
                float v0, v1;
                if (crossZ) {
                    v0 = ring.contourPts[p].z - targetVal;
                    v1 = ring.contourPts[p + 1].z - targetVal;
                } else {
                    v0 = ring.contourPts[p].x - targetVal;
                    v1 = ring.contourPts[p + 1].x - targetVal;
                }

                if ((v0 <= 0.0f && v1 > 0.0f) || (v0 >= 0.0f && v1 < 0.0f)) {
                    float t = v0 / (v0 - v1);
                    glm::vec3 crossPt = ring.contourPts[p] + (ring.contourPts[p + 1] - ring.contourPts[p]) * t;
                    float crossDist = ring.cumDist[p] + t * (ring.cumDist[p + 1] - ring.cumDist[p]);

                    float sideVal = (sideAxis == 0) ? crossPt.x : crossPt.z;
                    bool correctSide = positiveSide ? (sideVal >= -0.01f) : (sideVal <= 0.01f);
                    if (!correctSide) continue;

                    // How far is this crossing from searchStart (wrapping)?
                    float wrapDist = crossDist;
                    if (wrapDist < searchStart) wrapDist += ring.totalLen;
                    float distFromStart = wrapDist - searchStart;

                    // Pick the one closest to the expected quarter distance
                    float error = std::abs(distFromStart - expectedDist);
                    if (error < bestError) {
                        bestError = error;
                        bestCross = crossDist;
                    }
                }
            }
            return bestCross;
        };

        // Quarter 1: X=0 on +Z side (front) — roughly 1/4 around from point 0
        crossDists[1] = findCrossing(false, 0.0f, true, 2, crossDists[0]);  // X=0, +Z side

        // Quarter 2: Z=0 on -X side (left) — roughly 1/2 around
        if (crossDists[1] >= 0.0f)
            crossDists[2] = findCrossing(true, 0.0f, false, 0, crossDists[1]);  // Z=0, -X side
        else
            crossDists[2] = -1.0f;

        // Quarter 3: X=0 on -Z side (back) — roughly 3/4 around
        if (crossDists[2] >= 0.0f)
            crossDists[3] = findCrossing(false, 0.0f, false, 2, crossDists[2]);  // X=0, -Z side
        else
            crossDists[3] = -1.0f;

        // Only refine if we found all 4 crossings
        if (crossDists[1] < 0.0f || crossDists[2] < 0.0f || crossDists[3] < 0.0f) continue;

        // Redistribute N points with anchors at fixed N/4 intervals.
        // N is divisible by 4, so anchors at 0, N/4, N/2, 3N/4.
        // Each quarter-arc gets exactly N/4 points (anchor + N/4-1 in-between).

        // Helper: sample contour at a given distance
        auto sampleContour = [&](float dist, glm::vec3& outPt, glm::vec3& outNm) {
            dist = std::fmod(dist, ring.totalLen);
            if (dist < 0.0f) dist += ring.totalLen;
            size_t seg = 0;
            for (size_t s = 1; s < ring.cumDist.size(); s++) {
                if (ring.cumDist[s] >= dist) { seg = s - 1; break; }
                seg = s - 1;
            }
            if (seg + 1 >= ring.contourPts.size()) seg = ring.contourPts.size() - 2;
            float segStart = ring.cumDist[seg];
            float segEnd = ring.cumDist[seg + 1];
            float segLen = segEnd - segStart;
            float t = (segLen > 1e-7f) ? (dist - segStart) / segLen : 0.0f;
            t = std::max(0.0f, std::min(1.0f, t));
            outPt = ring.contourPts[seg] + (ring.contourPts[seg + 1] - ring.contourPts[seg]) * t;
            outNm = glm::normalize(ring.contourNrms[seg] * (1.0f - t) + ring.contourNrms[seg + 1] * t);
        };

        for (int q = 0; q < 4; q++) {
            float arcStart = crossDists[q];
            float arcEnd = crossDists[(q + 1) % 4];
            float arcLen = arcEnd - arcStart;
            if (arcLen < 0.0f) arcLen += ring.totalLen;

            int startPt = q * quarter;
            for (int k = 0; k < quarter; k++) {
                float frac = static_cast<float>(k) / quarter;
                float dist = std::fmod(arcStart + frac * arcLen, ring.totalLen);

                glm::vec3 pt, nm;
                sampleContour(dist, pt, nm);
                m_retopologyVerts[ring.startIdx + startPt + k] = pt;
                m_retopologyNormals[ring.startIdx + startPt + k] = nm;
            }
        }
    }

    std::cout << "[Scatter] Level 1 refinement: locked quarter-points to axis crossings" << std::endl;

    // ── Add pole cap points at top and bottom ──
    // Find the highest and lowest points on the live mesh surface
    m_bottomPoleIdx = SIZE_MAX;
    m_topPoleIdx = SIZE_MAX;

    if (!m_scatterRings.empty() && m_retopologyLiveObj) {
        // Bottom pole: raycast straight down from mesh center to find bottom surface point
        glm::vec3 bottomRayOrigin(globalCenter.x, bmin.y - 1.0f, globalCenter.z);
        glm::vec3 upDir(0, 1, 0);
        auto bottomHit = m_retopologyLiveObj->raycast(bottomRayOrigin, upDir);
        if (bottomHit.hit) {
            m_bottomPoleIdx = m_retopologyVerts.size();
            m_retopologyVerts.push_back(bottomHit.position);
            m_retopologyNormals.push_back(bottomHit.normal);
            m_retopologyVertMeshIdx.push_back(UINT32_MAX);
            totalPoints++;
        }

        // Top pole: raycast straight down from above to find top surface point
        glm::vec3 topRayOrigin(globalCenter.x, bmax.y + 1.0f, globalCenter.z);
        glm::vec3 downDir(0, -1, 0);
        auto topHit = m_retopologyLiveObj->raycast(topRayOrigin, downDir);
        if (topHit.hit) {
            m_topPoleIdx = m_retopologyVerts.size();
            m_retopologyVerts.push_back(topHit.position);
            m_retopologyNormals.push_back(topHit.normal);
            m_retopologyVertMeshIdx.push_back(UINT32_MAX);
            totalPoints++;
        }

        std::cout << "[Scatter] Added pole caps: bottom=" << (m_bottomPoleIdx != SIZE_MAX ? "yes" : "no")
                  << " top=" << (m_topPoleIdx != SIZE_MAX ? "yes" : "no") << std::endl;
    }

    // Print summary
    float avgPerRing = ringCount > 0 ? static_cast<float>(totalPoints) / ringCount : 0;
    std::cout << "[Scatter] Avg " << avgPerRing << " pts/ring, spacing=" << spacing
              << " units, model height=" << (maxY - minY) << std::endl;

    m_retopologyMode = true;

    std::cout << "[Scatter] Placed " << totalPoints << " points in " << ringCount
              << " contour rings (" << m_scatterRings.size() << " valid) at "
              << m_scatterSpacing << "cm spacing ("
              << spacing << " model units) on "
              << m_retopologyLiveObj->getName() << std::endl;
}

void ModelingMode::resampleRing(size_t ringIdx) {
    if (ringIdx >= m_scatterRings.size()) return;
    auto& ring = m_scatterRings[ringIdx];
    if (ring.contourPts.size() < 3 || ring.totalLen < 0.001f) return;

    int N = static_cast<int>(ring.count);
    float stepDist = ring.totalLen / N;

    // Wrap slideOffset to [0, totalLen)
    float offset = std::fmod(ring.slideOffset, ring.totalLen);
    if (offset < 0.0f) offset += ring.totalLen;

    for (int pi = 0; pi < N; pi++) {
        float targetDist = std::fmod(offset + pi * stepDist, ring.totalLen);

        // Find which contour segment this distance falls on
        size_t seg = 0;
        for (size_t s = 1; s < ring.cumDist.size(); s++) {
            if (ring.cumDist[s] >= targetDist) { seg = s - 1; break; }
            seg = s - 1;
        }
        if (seg + 1 >= ring.contourPts.size()) seg = ring.contourPts.size() - 2;

        float segStart = ring.cumDist[seg];
        float segEnd = ring.cumDist[seg + 1];
        float segLen = segEnd - segStart;
        float t = (segLen > 1e-7f) ? (targetDist - segStart) / segLen : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));

        glm::vec3 pt = ring.contourPts[seg] + (ring.contourPts[seg + 1] - ring.contourPts[seg]) * t;
        glm::vec3 nm = glm::normalize(ring.contourNrms[seg] * (1.0f - t) + ring.contourNrms[seg + 1] * t);

        m_retopologyVerts[ring.startIdx + pi] = pt;
        m_retopologyNormals[ring.startIdx + pi] = nm;
    }
}

void ModelingMode::connectScatterRings() {
    if (m_scatterRings.size() < 1) {
        std::cout << "[ConnectRings] No scatter rings" << std::endl;
        return;
    }

    m_scatterEdges.clear();
    int edgeCount = 0;

    // Horizontal only: connect each ring into a closed loop
    for (const auto& ring : m_scatterRings) {
        if (ring.count < 3) continue;
        for (size_t i = 0; i < ring.count; i++) {
            size_t cur  = ring.startIdx + i;
            size_t next = ring.startIdx + ((i + 1) % ring.count);
            m_scatterEdges.push_back({cur, next});
            edgeCount++;
        }
    }

    m_retopologyMode = true;
    std::cout << "[ConnectRings] " << m_scatterRings.size() << " rings, "
              << edgeCount << " horizontal edges" << std::endl;
}

void ModelingMode::connectScatterVertical() {
    if (m_scatterRings.size() < 2) {
        std::cout << "[ConnectVert] Need at least 2 rings" << std::endl;
        return;
    }

    int vertEdges = 0;
    int quadsCreated = 0;

    for (size_t ri = 0; ri + 1 < m_scatterRings.size(); ri++) {
        const auto& ringA = m_scatterRings[ri];
        const auto& ringB = m_scatterRings[ri + 1];
        if (ringA.count != ringB.count) continue;

        // Vertical edges: index-to-index
        for (size_t i = 0; i < ringA.count; i++) {
            size_t a = ringA.startIdx + i;
            size_t b = ringB.startIdx + i;
            m_scatterEdges.push_back({a, b});
            vertEdges++;
        }

        // Quads from the grid
        for (size_t i = 0; i < ringA.count; i++) {
            size_t i1 = (i + 1) % ringA.count;
            size_t a0 = ringA.startIdx + i;
            size_t a1 = ringA.startIdx + i1;
            size_t b0 = ringB.startIdx + i;
            size_t b1 = ringB.startIdx + i1;

            RetopologyQuad quad;
            quad.verts[0] = m_retopologyVerts[a0];
            quad.verts[1] = m_retopologyVerts[a1];
            quad.verts[2] = m_retopologyVerts[b1];
            quad.verts[3] = m_retopologyVerts[b0];
            m_retopologyQuads.push_back(quad);
            quadsCreated++;
        }
    }

    // ── Pole caps: triangle fans from pole point to nearest ring ──
    int trisCreated = 0;

    // Bottom pole → first ring
    if (m_bottomPoleIdx != SIZE_MAX && !m_scatterRings.empty()) {
        const auto& firstRing = m_scatterRings[0];
        glm::vec3 pole = m_retopologyVerts[m_bottomPoleIdx];
        for (size_t i = 0; i < firstRing.count; i++) {
            size_t i1 = (i + 1) % firstRing.count;
            size_t a = firstRing.startIdx + i;
            size_t b = firstRing.startIdx + i1;
            // Triangle: pole, a, b (degenerate quad with pole repeated)
            RetopologyQuad quad;
            quad.verts[0] = pole;
            quad.verts[1] = m_retopologyVerts[a];
            quad.verts[2] = m_retopologyVerts[b];
            quad.verts[3] = pole;
            m_retopologyQuads.push_back(quad);
            // Edge from pole to each ring point
            m_scatterEdges.push_back({m_bottomPoleIdx, a});
            trisCreated++;
        }
    }

    // Top pole → last ring
    if (m_topPoleIdx != SIZE_MAX && !m_scatterRings.empty()) {
        const auto& lastRing = m_scatterRings.back();
        glm::vec3 pole = m_retopologyVerts[m_topPoleIdx];
        for (size_t i = 0; i < lastRing.count; i++) {
            size_t i1 = (i + 1) % lastRing.count;
            size_t a = lastRing.startIdx + i;
            size_t b = lastRing.startIdx + i1;
            RetopologyQuad quad;
            quad.verts[0] = m_retopologyVerts[a];
            quad.verts[1] = m_retopologyVerts[b];
            quad.verts[2] = pole;
            quad.verts[3] = pole;
            m_retopologyQuads.push_back(quad);
            m_scatterEdges.push_back({m_topPoleIdx, a});
            trisCreated++;
        }
    }

    m_retopologyMode = true;
    std::cout << "[ConnectVert] " << vertEdges << " vertical edges, "
              << quadsCreated << " quads, " << trisCreated << " cap tris" << std::endl;
}
