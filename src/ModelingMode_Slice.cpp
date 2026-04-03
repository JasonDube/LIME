// ModelingMode_Slice.cpp - Mesh slice tool: cut a mesh into two pieces with a positioning plane

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>

#include <iostream>
#include <cmath>
#include <algorithm>
#include <map>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace eden;

// --- Plane parameter update ---

void ModelingMode::updateSlicePlaneFromParams() {
    // Start from preset axis
    glm::vec3 baseNormal(0.0f);
    switch (m_slicePresetAxis) {
        case 0: baseNormal = glm::vec3(1, 0, 0); break; // X
        case 1: baseNormal = glm::vec3(0, 1, 0); break; // Y
        case 2: baseNormal = glm::vec3(0, 0, 1); break; // Z
    }

    // Apply pitch (rotation around local X) and yaw (rotation around local Y)
    float pitchRad = glm::radians(m_slicePlaneRotationX);
    float yawRad = glm::radians(m_slicePlaneRotationY);

    // Build rotation: yaw first, then pitch
    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), yawRad, glm::vec3(0, 1, 0));
    rot = glm::rotate(rot, pitchRad, glm::vec3(1, 0, 0));

    m_slicePlaneNormal = glm::normalize(glm::vec3(rot * glm::vec4(baseNormal, 0.0f)));

    // Center from object AABB + offset along normal
    if (m_ctx.selectedObject) {
        AABB bounds = m_ctx.selectedObject->getLocalBounds();
        glm::vec3 aabbSize = bounds.getSize();
        float maxDim = glm::max(aabbSize.x, glm::max(aabbSize.y, aabbSize.z));

        // If bounds are zero, compute from editable mesh
        if (maxDim < 1e-4f && m_ctx.editableMesh.isValid()) {
            glm::vec3 bmin(INFINITY), bmax(-INFINITY);
            for (uint32_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                glm::vec3 p = m_ctx.editableMesh.getVertex(i).position;
                bmin = glm::min(bmin, p);
                bmax = glm::max(bmax, p);
            }
            if (bmin.x < INFINITY) {
                bounds.min = bmin;
                bounds.max = bmax;
                m_ctx.selectedObject->setLocalBounds(bounds);
            }
        }

        glm::vec3 aabbCenter = bounds.getCenter();
        m_slicePlaneCenter = aabbCenter + m_slicePlaneNormal * m_slicePlaneOffset;
    }
}

// --- 3D visualization ---

void ModelingMode::drawSlicePlaneOverlay3D(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_ctx.selectedObject) return;

    updateSlicePlaneFromParams();

    // Get object AABB to size the plane — compute from editable mesh if bounds are zero
    AABB bounds = m_ctx.selectedObject->getLocalBounds();
    glm::vec3 aabbSize = bounds.getSize();
    float maxDim = glm::max(aabbSize.x, glm::max(aabbSize.y, aabbSize.z));

    // If bounds are zero/tiny, compute from editable mesh vertices
    if (maxDim < 1e-4f && m_ctx.editableMesh.isValid()) {
        glm::vec3 bmin(INFINITY), bmax(-INFINITY);
        for (uint32_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
            glm::vec3 p = m_ctx.editableMesh.getVertex(i).position;
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
        }
        if (bmin.x < INFINITY) {
            bounds.min = bmin;
            bounds.max = bmax;
            aabbSize = bounds.getSize();
            maxDim = glm::max(aabbSize.x, glm::max(aabbSize.y, aabbSize.z));
            // Store computed bounds on the object for future use
            m_ctx.selectedObject->setLocalBounds(bounds);
        }
    }

    // Fallback: if still zero, use a default size
    if (maxDim < 1e-4f) maxDim = 1.0f;

    float planeSize = maxDim * 0.75f;

    // Debug: log plane params once
    static int sliceDebugCounter = 0;
    if (++sliceDebugCounter % 300 == 1) {
        std::cout << "[Slice] plane center=(" << m_slicePlaneCenter.x << "," << m_slicePlaneCenter.y << "," << m_slicePlaneCenter.z
                  << ") normal=(" << m_slicePlaneNormal.x << "," << m_slicePlaneNormal.y << "," << m_slicePlaneNormal.z
                  << ") planeSize=" << planeSize << " maxDim=" << maxDim
                  << " aabb=[" << bounds.min.x << "," << bounds.min.y << "," << bounds.min.z
                  << " - " << bounds.max.x << "," << bounds.max.y << "," << bounds.max.z << "]"
                  << std::endl;
    }

    // Compute plane basis vectors
    glm::vec3 up(0, 1, 0);
    if (std::abs(glm::dot(m_slicePlaneNormal, up)) > 0.99f) {
        up = glm::vec3(0, 0, 1);
    }
    glm::vec3 right = glm::normalize(glm::cross(up, m_slicePlaneNormal));
    glm::vec3 forward = glm::normalize(glm::cross(m_slicePlaneNormal, right));

    // Transform to world space
    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();

    glm::vec3 center = glm::vec3(modelMatrix * glm::vec4(m_slicePlaneCenter, 1.0f));
    glm::vec3 worldRight = glm::normalize(glm::vec3(modelMatrix * glm::vec4(right, 0.0f)));
    glm::vec3 worldForward = glm::normalize(glm::vec3(modelMatrix * glm::vec4(forward, 0.0f)));
    glm::vec3 worldNormal = glm::normalize(glm::vec3(modelMatrix * glm::vec4(m_slicePlaneNormal, 0.0f)));

    // Scale for world space
    glm::vec3 scale = m_ctx.selectedObject->getTransform().getScale();
    float avgScale = (scale.x + scale.y + scale.z) / 3.0f;
    float worldPlaneSize = planeSize * avgScale;

    // Quad corners
    glm::vec3 corners[4] = {
        center - worldRight * worldPlaneSize - worldForward * worldPlaneSize,
        center + worldRight * worldPlaneSize - worldForward * worldPlaneSize,
        center + worldRight * worldPlaneSize + worldForward * worldPlaneSize,
        center - worldRight * worldPlaneSize + worldForward * worldPlaneSize,
    };

    // Cyan wireframe rectangle
    std::vector<glm::vec3> lines;
    for (int i = 0; i < 4; ++i) {
        lines.push_back(corners[i]);
        lines.push_back(corners[(i + 1) % 4]);
    }

    // Diagonal cross lines
    lines.push_back(corners[0]);
    lines.push_back(corners[2]);
    lines.push_back(corners[1]);
    lines.push_back(corners[3]);

    m_ctx.modelRenderer.renderLines(cmd, viewProj, lines, glm::vec3(0.0f, 0.8f, 1.0f));

    // Yellow normal arrow
    float arrowLen = worldPlaneSize * 0.5f;
    std::vector<glm::vec3> arrowLines;
    arrowLines.push_back(center);
    arrowLines.push_back(center + worldNormal * arrowLen);

    // Arrowhead
    glm::vec3 arrowTip = center + worldNormal * arrowLen;
    float headSize = arrowLen * 0.15f;
    arrowLines.push_back(arrowTip);
    arrowLines.push_back(arrowTip - worldNormal * headSize + worldRight * headSize);
    arrowLines.push_back(arrowTip);
    arrowLines.push_back(arrowTip - worldNormal * headSize - worldRight * headSize);

    m_ctx.modelRenderer.renderLines(cmd, viewProj, arrowLines, glm::vec3(1.0f, 1.0f, 0.0f));
}

// --- Cancel ---

void ModelingMode::cancelSliceMode() {
    m_sliceMode = false;
    m_slicePlaneOffset = 0.0f;
    m_slicePlaneRotationX = 0.0f;
    m_slicePlaneRotationY = 0.0f;
    m_slicePresetAxis = 1;
    std::cout << "[Slice] Mode cancelled" << std::endl;
}

// --- Core slice algorithm ---

void ModelingMode::performSlice() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) {
        std::cout << "[Slice] No valid mesh selected" << std::endl;
        return;
    }

    updateSlicePlaneFromParams();

    EditableMesh& srcMesh = m_ctx.editableMesh;
    uint32_t vertCount = static_cast<uint32_t>(srcMesh.getVertexCount());
    uint32_t faceCount = static_cast<uint32_t>(srcMesh.getFaceCount());

    glm::vec3 planeCenter = m_slicePlaneCenter;
    glm::vec3 planeNormal = m_slicePlaneNormal;
    const float epsilon = 1e-5f;

    // 1. Classify vertices: +1 = positive side, -1 = negative side, 0 = on plane
    std::vector<int> vertSide(vertCount);
    std::vector<float> vertDist(vertCount);

    int posCount = 0, negCount = 0;
    for (uint32_t i = 0; i < vertCount; ++i) {
        float d = glm::dot(planeNormal, srcMesh.getVertex(i).position - planeCenter);
        vertDist[i] = d;
        if (d > epsilon) {
            vertSide[i] = 1;
            posCount++;
        } else if (d < -epsilon) {
            vertSide[i] = -1;
            negCount++;
        } else {
            vertSide[i] = 0;
        }
    }

    if (posCount == 0 || negCount == 0) {
        std::cout << "[Slice] Plane does not intersect mesh (all vertices on one side)" << std::endl;
        return;
    }

    // 2. Build two output meshes
    EditableMesh posMesh, negMesh;

    // Vertex mapping: old index → new index in each mesh
    std::vector<uint32_t> posVertMap(vertCount, UINT32_MAX);
    std::vector<uint32_t> negVertMap(vertCount, UINT32_MAX);

    auto getOrAddVertex = [&](EditableMesh& mesh, std::vector<uint32_t>& vertMap,
                              uint32_t oldIdx) -> uint32_t {
        if (vertMap[oldIdx] != UINT32_MAX) return vertMap[oldIdx];
        HEVertex v = srcMesh.getVertex(oldIdx);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        uint32_t newIdx = mesh.addVertex(v);
        vertMap[oldIdx] = newIdx;
        return newIdx;
    };

    // Edge intersection cache: key = (min(v0,v1), max(v0,v1)) → intersection vertex in each mesh
    using EdgeKey = std::pair<uint32_t, uint32_t>;
    std::map<EdgeKey, uint32_t> posEdgeIntersections;
    std::map<EdgeKey, uint32_t> negEdgeIntersections;

    auto makeEdgeKey = [](uint32_t a, uint32_t b) -> EdgeKey {
        return {std::min(a, b), std::max(a, b)};
    };

    auto interpolateVertex = [&](uint32_t v0, uint32_t v1) -> HEVertex {
        float d0 = vertDist[v0];
        float d1 = vertDist[v1];
        float t = d0 / (d0 - d1);  // lerp factor: 0 at v0, 1 at v1

        const HEVertex& a = srcMesh.getVertex(v0);
        const HEVertex& b = srcMesh.getVertex(v1);

        HEVertex result;
        result.position = glm::mix(a.position, b.position, t);
        result.normal = glm::normalize(glm::mix(a.normal, b.normal, t));
        result.uv = glm::mix(a.uv, b.uv, t);
        result.color = glm::mix(a.color, b.color, t);
        result.halfEdgeIndex = UINT32_MAX;
        result.selected = false;
        return result;
    };

    auto getOrAddIntersection = [&](EditableMesh& mesh, std::map<EdgeKey, uint32_t>& cache,
                                     uint32_t v0, uint32_t v1) -> uint32_t {
        EdgeKey key = makeEdgeKey(v0, v1);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        HEVertex iv = interpolateVertex(v0, v1);
        uint32_t newIdx = mesh.addVertex(iv);
        cache[key] = newIdx;
        return newIdx;
    };

    // 3. Process each face
    for (uint32_t fi = 0; fi < faceCount; ++fi) {
        const HEFace& face = srcMesh.getFace(fi);
        if (face.vertexCount < 3) continue;

        std::vector<uint32_t> faceVerts = srcMesh.getFaceVertices(fi);
        uint32_t n = static_cast<uint32_t>(faceVerts.size());

        // Check if all vertices are on the same side
        bool allPos = true, allNeg = true;
        for (uint32_t vi : faceVerts) {
            if (vertSide[vi] != 1) allPos = false;
            if (vertSide[vi] != -1) allNeg = false;
        }

        // On-plane vertices count as both sides
        bool anyPos = false, anyNeg = false;
        for (uint32_t vi : faceVerts) {
            if (vertSide[vi] >= 0) anyPos = true;  // positive or on-plane
            if (vertSide[vi] <= 0) anyNeg = true;  // negative or on-plane
        }

        if (allPos || (!anyNeg)) {
            // Entire face on positive side
            std::vector<uint32_t> newVerts;
            for (uint32_t vi : faceVerts) {
                newVerts.push_back(getOrAddVertex(posMesh, posVertMap, vi));
            }
            posMesh.addFace(newVerts);
            continue;
        }

        if (allNeg || (!anyPos)) {
            // Entire face on negative side
            std::vector<uint32_t> newVerts;
            for (uint32_t vi : faceVerts) {
                newVerts.push_back(getOrAddVertex(negMesh, negVertMap, vi));
            }
            negMesh.addFace(newVerts);
            continue;
        }

        // Face straddles the plane — split it
        std::vector<uint32_t> posFaceVerts;
        std::vector<uint32_t> negFaceVerts;

        for (uint32_t i = 0; i < n; ++i) {
            uint32_t curr = faceVerts[i];
            uint32_t next = faceVerts[(i + 1) % n];
            int currSide = vertSide[curr];
            int nextSide = vertSide[next];

            // Add current vertex to its side(s)
            if (currSide >= 0) {  // positive or on-plane
                posFaceVerts.push_back(getOrAddVertex(posMesh, posVertMap, curr));
            }
            if (currSide <= 0) {  // negative or on-plane
                negFaceVerts.push_back(getOrAddVertex(negMesh, negVertMap, curr));
            }

            // Check if edge crosses the plane
            if ((currSide > 0 && nextSide < 0) || (currSide < 0 && nextSide > 0)) {
                // Edge crosses — add intersection point to both sides
                uint32_t posIntersect = getOrAddIntersection(posMesh, posEdgeIntersections, curr, next);
                uint32_t negIntersect = getOrAddIntersection(negMesh, negEdgeIntersections, curr, next);
                posFaceVerts.push_back(posIntersect);
                negFaceVerts.push_back(negIntersect);
            }
        }

        // Add sub-faces (skip degenerate)
        if (posFaceVerts.size() >= 3) {
            posMesh.addFace(posFaceVerts);
        }
        if (negFaceVerts.size() >= 3) {
            negMesh.addFace(negFaceVerts);
        }
    }

    // 3b. Cap holes: collect boundary vertices (intersection points) and add a cap face
    if (m_sliceCapHoles && (!posEdgeIntersections.empty())) {
        // The intersection vertices form the boundary ring of the cut.
        // Collect them and order into a loop by chaining adjacent edges.

        auto capMesh = [&](EditableMesh& mesh, std::map<EdgeKey, uint32_t>& edgeIntersections, bool flipWinding) {
            if (edgeIntersections.empty()) return;

            // Collect all intersection vertex indices
            std::set<uint32_t> capVertSet;
            for (auto& [key, vidx] : edgeIntersections) {
                capVertSet.insert(vidx);
            }
            std::vector<uint32_t> capVerts(capVertSet.begin(), capVertSet.end());

            if (capVerts.size() < 3) return;

            // Sort by angle around their centroid (projected onto the slice plane)
            glm::vec3 centroid(0.0f);
            for (uint32_t vi : capVerts) centroid += mesh.getVertex(vi).position;
            centroid /= static_cast<float>(capVerts.size());

            // Build local 2D basis on the slice plane
            glm::vec3 up = glm::vec3(0, 1, 0);
            if (std::abs(glm::dot(planeNormal, up)) > 0.9f) up = glm::vec3(1, 0, 0);
            glm::vec3 localX = glm::normalize(glm::cross(up, planeNormal));
            glm::vec3 localY = glm::normalize(glm::cross(planeNormal, localX));

            std::sort(capVerts.begin(), capVerts.end(), [&](uint32_t a, uint32_t b) {
                glm::vec3 da = mesh.getVertex(a).position - centroid;
                glm::vec3 db = mesh.getVertex(b).position - centroid;
                float angleA = std::atan2(glm::dot(da, localY), glm::dot(da, localX));
                float angleB = std::atan2(glm::dot(db, localY), glm::dot(db, localX));
                return angleA < angleB;
            });

            // Flip winding for one side so both caps face outward
            if (flipWinding) {
                std::reverse(capVerts.begin(), capVerts.end());
            }

            // Add cap face — if too many verts for one n-gon, use triangle fan
            std::cout << "[Slice] Cap face: " << capVerts.size() << " boundary verts" << std::endl;
            if (capVerts.size() <= 50) {
                mesh.addFace(capVerts);
            } else {
                // Triangle fan from centroid for large caps
                HEVertex centerVert;
                centerVert.position = centroid;
                centerVert.normal = planeNormal;
                centerVert.uv = glm::vec2(0.5f);
                centerVert.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
                centerVert.halfEdgeIndex = UINT32_MAX;
                centerVert.selected = false;
                uint32_t centerIdx = mesh.addVertex(centerVert);

                for (size_t i = 0; i < capVerts.size(); i++) {
                    size_t j = (i + 1) % capVerts.size();
                    std::vector<uint32_t> tri = {capVerts[static_cast<uint32_t>(i)],
                                                  capVerts[static_cast<uint32_t>(j)],
                                                  centerIdx};
                    mesh.addFace(tri);
                }
            }
        };

        capMesh(posMesh, posEdgeIntersections, true);   // Pos side: flip to face outward
        capMesh(negMesh, negEdgeIntersections, false);  // Neg side: natural winding faces outward

        std::cout << "[Slice] Cap: " << posEdgeIntersections.size()
                  << " pos intersections, " << negEdgeIntersections.size()
                  << " neg intersections" << std::endl;
    }

    // 4. Check results
    bool hasPos = posMesh.getFaceCount() > 0;
    bool hasNeg = negMesh.getFaceCount() > 0;

    if (!hasPos && !hasNeg) {
        std::cout << "[Slice] No geometry produced" << std::endl;
        return;
    }

    // 5. Rebuild topology
    if (hasPos) {
        posMesh.rebuildEdgeMap();
        posMesh.linkTwinsByPosition();
    }
    if (hasNeg) {
        negMesh.rebuildEdgeMap();
        negMesh.linkTwinsByPosition();
    }

    // 6. Create scene objects — follow generatePathTubeMesh() pattern
    Transform originalTransform = m_ctx.selectedObject->getTransform();
    std::string originalName = m_ctx.selectedObject->getName();

    // Capture texture data from original object before we change selection
    const unsigned char* texData = nullptr;
    int texW = 0, texH = 0;
    std::vector<unsigned char> texDataCopy;
    if (m_ctx.selectedObject->hasTextureData()) {
        texDataCopy = m_ctx.selectedObject->getTextureData();
        texW = m_ctx.selectedObject->getTextureWidth();
        texH = m_ctx.selectedObject->getTextureHeight();
        texData = texDataCopy.data();
    }

    auto createSlicedObject = [&](EditableMesh& mesh, const std::string& suffix) -> SceneObject* {
        std::string name = originalName + suffix;
        auto newObj = std::make_unique<SceneObject>(name);
        newObj->setDescription("Sliced mesh piece");
        SceneObject* obj = newObj.get();
        m_ctx.sceneObjects.push_back(std::move(newObj));

        // Copy transform from original
        obj->getTransform().setPosition(originalTransform.getPosition());
        obj->getTransform().setRotation(originalTransform.getRotation());
        obj->getTransform().setScale(originalTransform.getScale());

        // Triangulate for GPU
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        std::set<uint32_t> noHidden;
        mesh.triangulate(vertices, indices, noHidden);

        if (indices.empty()) {
            std::cout << "[Slice] " << name << ": triangulation produced no geometry" << std::endl;
            return nullptr;
        }

        // Create GPU model (with texture if original had one)
        uint32_t handle = m_ctx.modelRenderer.createModel(vertices, indices, texData, texW, texH);

        // Copy texture data to the new scene object so it's available for painting/export
        if (texData && texW > 0 && texH > 0) {
            obj->setTextureData(texDataCopy, texW, texH);
        }
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setVisible(true);

        // Store half-edge data
        const auto& heVerts = mesh.getVerticesData();
        const auto& heHalfEdges = mesh.getHalfEdges();
        const auto& heFaces = mesh.getFacesData();

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

        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Compute local bounds
        AABB bounds;
        bounds.min = glm::vec3(INFINITY);
        bounds.max = glm::vec3(-INFINITY);
        for (const auto& v : vertices) {
            bounds.min = glm::min(bounds.min, v.position);
            bounds.max = glm::max(bounds.max, v.position);
        }
        obj->setLocalBounds(bounds);

        return obj;
    };

    SceneObject* posObj = hasPos ? createSlicedObject(posMesh, "_slice_pos") : nullptr;
    SceneObject* negObj = hasNeg ? createSlicedObject(negMesh, "_slice_neg") : nullptr;

    // 7. Hide original object
    m_ctx.selectedObject->setVisible(false);

    // 8. Select the positive-side piece for editing
    SceneObject* selectObj = posObj ? posObj : negObj;
    EditableMesh& selectMesh = posObj ? posMesh : negMesh;

    if (selectObj) {
        m_ctx.selectedObject = selectObj;
        m_ctx.editableMesh = selectMesh;
        m_ctx.meshDirty = false;

        // Build faceToTriangles mapping
        m_ctx.faceToTriangles.clear();
        uint32_t triIndex = 0;
        for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
            uint32_t vc = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
            uint32_t triCount = (vc >= 3) ? (vc - 2) : 0;
            for (uint32_t ti = 0; ti < triCount; ++ti) {
                m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
            }
        }

        m_ctx.selectedFaces.clear();
        m_ctx.hiddenFaces.clear();
        invalidateWireframeCache();
    }

    // Exit slice mode
    m_sliceMode = false;
    m_slicePlaneOffset = 0.0f;
    m_slicePlaneRotationX = 0.0f;
    m_slicePlaneRotationY = 0.0f;

    std::cout << "[Slice] Complete: "
              << (hasPos ? std::to_string(posMesh.getFaceCount()) + " faces (+)" : "empty (+)")
              << " | "
              << (hasNeg ? std::to_string(negMesh.getFaceCount()) + " faces (-)" : "empty (-)")
              << std::endl;
}

void ModelingMode::performExtract() {
    if (!m_retopologyLiveObj || m_retopologyQuads.empty()) {
        std::cout << "[Extract] Need live object and retopo quads" << std::endl;
        return;
    }

    // Collect all unique retopo vertex positions from the quads
    std::vector<glm::vec3> gridPoints;
    {
        const float dedupThresh = 0.001f;
        for (const auto& quad : m_retopologyQuads) {
            for (int i = 0; i < 4; i++) {
                bool exists = false;
                for (const auto& gp : gridPoints) {
                    if (glm::length(gp - quad.verts[i]) < dedupThresh) { exists = true; break; }
                }
                if (!exists) gridPoints.push_back(quad.verts[i]);
            }
        }
        std::cout << "[Extract] Grid has " << gridPoints.size() << " unique vertices from "
                  << m_retopologyQuads.size() << " quads" << std::endl;
    }

    if (!m_retopologyLiveObj->hasMeshData()) {
        std::cout << "[Extract] Live object has no mesh data" << std::endl;
        return;
    }

    // Work with the live object's triangle mesh directly (not m_ctx.editableMesh)
    const auto& srcVerts = m_retopologyLiveObj->getVertices();
    const auto& srcIndices = m_retopologyLiveObj->getIndices();
    glm::mat4 modelMatrix = m_retopologyLiveObj->getTransform().getMatrix();

    // Build editable mesh from the live object for proper face-level operations
    EditableMesh srcMesh;
    if (m_retopologyLiveObj->hasEditableMeshData()) {
        // Restore from stored half-edge data
        const auto& storedVerts = m_retopologyLiveObj->getHEVertices();
        const auto& storedHE = m_retopologyLiveObj->getHEHalfEdges();
        const auto& storedFaces = m_retopologyLiveObj->getHEFaces();
        std::vector<eden::HEVertex> heVerts;
        for (const auto& sv : storedVerts) {
            eden::HEVertex v;
            v.position = sv.position; v.normal = sv.normal; v.uv = sv.uv;
            v.color = sv.color; v.halfEdgeIndex = sv.halfEdgeIndex; v.selected = sv.selected;
            heVerts.push_back(v);
        }
        std::vector<eden::HalfEdge> heHE;
        for (const auto& sh : storedHE) {
            eden::HalfEdge h;
            h.vertexIndex = sh.vertexIndex; h.faceIndex = sh.faceIndex;
            h.nextIndex = sh.nextIndex; h.prevIndex = sh.prevIndex; h.twinIndex = sh.twinIndex;
            heHE.push_back(h);
        }
        std::vector<eden::HEFace> heFaces;
        for (const auto& sf : storedFaces) {
            eden::HEFace f;
            f.halfEdgeIndex = sf.halfEdgeIndex; f.vertexCount = sf.vertexCount; f.selected = sf.selected;
            heFaces.push_back(f);
        }
        srcMesh.setFromData(heVerts, heHE, heFaces);
    } else {
        // Build from triangles
        srcMesh.buildFromTriangles(srcVerts, srcIndices);
    }

    uint32_t faceCount = static_cast<uint32_t>(srcMesh.getFaceCount());
    float maxRayDist = m_scatterSpacing * 0.01f * 3.0f;

    // Ray-triangle intersection (Möller–Trumbore)
    auto rayTriHit = [](const glm::vec3& org, const glm::vec3& dir,
                        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                        float maxD) -> bool {
        glm::vec3 e1 = v1 - v0, e2 = v2 - v0;
        glm::vec3 h = glm::cross(dir, e2);
        float a = glm::dot(e1, h);
        if (std::abs(a) < 1e-7f) return false;
        float f = 1.0f / a;
        glm::vec3 s = org - v0;
        float u = f * glm::dot(s, h);
        if (u < 0.0f || u > 1.0f) return false;
        glm::vec3 q = glm::cross(s, e1);
        float v = f * glm::dot(dir, q);
        if (v < 0.0f || u + v > 1.0f) return false;
        float t = f * glm::dot(e2, q);
        return (t > 0.001f && t < maxD);
    };

    // Check if a ray from a point hits any retopo quad
    auto rayHitsGrid = [&](const glm::vec3& org, const glm::vec3& dir) -> bool {
        for (const auto& quad : m_retopologyQuads) {
            if (rayTriHit(org, dir, quad.verts[0], quad.verts[1], quad.verts[2], maxRayDist)) return true;
            if (rayTriHit(org, dir, quad.verts[0], quad.verts[2], quad.verts[3], maxRayDist)) return true;
        }
        return false;
    };

    // Selection mask approach: each retopo quad defines a mask region.
    // A mesh face is "under the grid" if its center projects inside any retopo quad.
    float maxProjectDist = m_scatterSpacing * 0.01f * 2.0f;

    // Helper: check if point P projects inside quad (v0,v1,v2,v3) within maxDist
    auto pointInQuadProjection = [&](const glm::vec3& P, const glm::vec3& v0, const glm::vec3& v1,
                                      const glm::vec3& v2, const glm::vec3& v3) -> bool {
        // Compute quad plane
        glm::vec3 e1 = v1 - v0, e2 = v3 - v0;
        glm::vec3 quadNormal = glm::cross(e1, e2);
        float quadNormalLen = glm::length(quadNormal);
        if (quadNormalLen < 1e-7f) return false;
        quadNormal /= quadNormalLen;

        // Distance from P to quad plane
        float dist = std::abs(glm::dot(P - v0, quadNormal));
        if (dist > maxProjectDist) return false;

        // Project P onto the quad plane
        glm::vec3 projected = P - quadNormal * glm::dot(P - v0, quadNormal);

        // Check if projected point is inside the quad using cross product test
        // Point inside convex quad if it's on the same side of all 4 edges
        glm::vec3 edges[4] = {v1 - v0, v2 - v1, v3 - v2, v0 - v3};
        glm::vec3 corners[4] = {v0, v1, v2, v3};

        int positive = 0, negative = 0;
        for (int i = 0; i < 4; i++) {
            glm::vec3 toPoint = projected - corners[i];
            float cross = glm::dot(quadNormal, glm::cross(edges[i], toPoint));
            if (cross > 0) positive++;
            else if (cross < 0) negative++;
        }
        // Inside if all same sign (all positive or all negative)
        return (positive == 4 || negative == 4);
    };

    // Pre-compute retopo quad bounding boxes for fast rejection
    struct QuadBBox { glm::vec3 min, max; };
    std::vector<QuadBBox> quadBBoxes(m_retopologyQuads.size());
    for (size_t qi = 0; qi < m_retopologyQuads.size(); qi++) {
        glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
        for (int i = 0; i < 4; i++) {
            bmin = glm::min(bmin, m_retopologyQuads[qi].verts[i]);
            bmax = glm::max(bmax, m_retopologyQuads[qi].verts[i]);
        }
        // Expand by max project distance
        quadBBoxes[qi] = {bmin - glm::vec3(maxProjectDist), bmax + glm::vec3(maxProjectDist)};
    }

    std::vector<bool> faceUnderGrid(faceCount, false);
    int underCount = 0;

    for (uint32_t fi = 0; fi < faceCount; fi++) {
        auto faceVerts = srcMesh.getFaceVertices(fi);
        if (faceVerts.empty()) continue;

        glm::vec3 faceCenter(0.0f);
        for (uint32_t vi : faceVerts)
            faceCenter += glm::vec3(modelMatrix * glm::vec4(srcMesh.getVertex(vi).position, 1.0f));
        faceCenter /= static_cast<float>(faceVerts.size());

        // Check if face center projects inside any retopo quad
        for (size_t qi = 0; qi < m_retopologyQuads.size(); qi++) {
            // Fast AABB rejection
            if (faceCenter.x < quadBBoxes[qi].min.x || faceCenter.x > quadBBoxes[qi].max.x ||
                faceCenter.y < quadBBoxes[qi].min.y || faceCenter.y > quadBBoxes[qi].max.y ||
                faceCenter.z < quadBBoxes[qi].min.z || faceCenter.z > quadBBoxes[qi].max.z)
                continue;

            const auto& q = m_retopologyQuads[qi];
            if (pointInQuadProjection(faceCenter, q.verts[0], q.verts[1], q.verts[2], q.verts[3])) {
                faceUnderGrid[fi] = true;
                underCount++;
                break;
            }
        }
    }

    if (underCount == 0) {
        std::cout << "[Extract] No faces found under retopo grid" << std::endl;
        return;
    }

    std::cout << "[Extract] Found " << underCount << " / " << faceCount << " faces under grid" << std::endl;

    // Build two meshes: extracted (under grid) and remaining
    EditableMesh extractedMesh, remainingMesh;
    std::vector<uint32_t> extractVertMap(srcMesh.getVertexCount(), UINT32_MAX);
    std::vector<uint32_t> remainVertMap(srcMesh.getVertexCount(), UINT32_MAX);

    auto getOrAddVert = [&](EditableMesh& mesh, std::vector<uint32_t>& vertMap, uint32_t oldIdx) -> uint32_t {
        if (vertMap[oldIdx] != UINT32_MAX) return vertMap[oldIdx];
        HEVertex v = srcMesh.getVertex(oldIdx);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        uint32_t newIdx = mesh.addVertex(v);
        vertMap[oldIdx] = newIdx;
        return newIdx;
    };

    for (uint32_t fi = 0; fi < faceCount; fi++) {
        auto faceVerts = srcMesh.getFaceVertices(fi);
        if (faceVerts.empty()) continue;

        EditableMesh& targetMesh = faceUnderGrid[fi] ? extractedMesh : remainingMesh;
        auto& targetMap = faceUnderGrid[fi] ? extractVertMap : remainVertMap;

        std::vector<uint32_t> newFaceVerts;
        for (uint32_t vi : faceVerts) {
            newFaceVerts.push_back(getOrAddVert(targetMesh, targetMap, vi));
        }
        targetMesh.addFace(newFaceVerts);
    }

    // Rebuild topology
    if (extractedMesh.getFaceCount() > 0) {
        extractedMesh.rebuildEdgeMap();
        extractedMesh.linkTwinsByPosition();
        extractedMesh.recalculateNormals();
    }
    if (remainingMesh.getFaceCount() > 0) {
        remainingMesh.rebuildEdgeMap();
        remainingMesh.linkTwinsByPosition();
        remainingMesh.recalculateNormals();
    }

    // Create scene objects using the same pattern as slice
    Transform originalTransform = m_retopologyLiveObj->getTransform();
    std::string originalName = m_retopologyLiveObj->getName();

    // Get texture from original
    const unsigned char* texData = nullptr;
    int texW = 0, texH = 0;
    std::vector<unsigned char> texDataCopy;
    if (m_retopologyLiveObj->hasTextureData()) {
        texDataCopy = m_retopologyLiveObj->getTextureData();
        texW = m_retopologyLiveObj->getTextureWidth();
        texH = m_retopologyLiveObj->getTextureHeight();
        texData = texDataCopy.data();
    }

    auto createExtractObject = [&](EditableMesh& mesh, const std::string& suffix) -> SceneObject* {
        if (mesh.getFaceCount() == 0) return nullptr;

        auto newObj = std::make_unique<SceneObject>(originalName + suffix);
        SceneObject* obj = newObj.get();
        m_ctx.sceneObjects.push_back(std::move(newObj));

        obj->getTransform().setPosition(originalTransform.getPosition());
        obj->getTransform().setRotation(originalTransform.getRotation());
        obj->getTransform().setScale(originalTransform.getScale());

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        std::set<uint32_t> noHidden;
        mesh.triangulate(vertices, indices, noHidden);

        if (indices.empty()) return nullptr;

        uint32_t handle = m_ctx.modelRenderer.createModel(vertices, indices, texData, texW, texH);
        if (texData && texW > 0) obj->setTextureData(texDataCopy, texW, texH);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setVisible(true);

        // Store half-edge data
        const auto& heVerts = mesh.getVerticesData();
        const auto& heHE = mesh.getHalfEdges();
        const auto& heFaces = mesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> sv;
        for (const auto& v : heVerts) sv.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> she;
        for (const auto& h : heHE) she.push_back({h.vertexIndex, h.faceIndex, h.nextIndex, h.prevIndex, h.twinIndex});
        std::vector<SceneObject::StoredHEFace> sf;
        for (const auto& f : heFaces) sf.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(sv, she, sf);

        AABB bounds;
        bounds.min = glm::vec3(INFINITY);
        bounds.max = glm::vec3(-INFINITY);
        for (const auto& v : vertices) {
            bounds.min = glm::min(bounds.min, v.position);
            bounds.max = glm::max(bounds.max, v.position);
        }
        obj->setLocalBounds(bounds);

        return obj;
    };

    SceneObject* extractedObj = createExtractObject(extractedMesh, "_extracted");
    SceneObject* remainingObj = createExtractObject(remainingMesh, "_remaining");

    // Hide original
    m_retopologyLiveObj->setVisible(false);

    // Select the extracted piece
    if (extractedObj) {
        m_ctx.selectedObject = extractedObj;
        m_ctx.editableMesh = extractedMesh;
        m_ctx.meshDirty = false;

        m_ctx.faceToTriangles.clear();
        uint32_t triIndex = 0;
        for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
            uint32_t vc = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
            uint32_t triCount = (vc >= 3) ? (vc - 2) : 0;
            for (uint32_t ti = 0; ti < triCount; ++ti) {
                m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
            }
        }
        m_ctx.selectedFaces.clear();
        m_ctx.hiddenFaces.clear();
        invalidateWireframeCache();
    }

    std::cout << "[Extract] Done: " << underCount << " faces extracted, "
              << (faceCount - underCount) << " remaining" << std::endl;
}
