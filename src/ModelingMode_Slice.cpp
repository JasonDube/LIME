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
