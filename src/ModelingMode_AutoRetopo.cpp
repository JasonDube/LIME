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
