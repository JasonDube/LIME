// ModelingMode_Snap.cpp - Snap mode functions for ModelingMode
// Split from ModelingMode.cpp for better organization

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>

#include <iostream>
#include <limits>

using namespace eden;

void ModelingMode::drawSnapVertexOverlay(float vpX, float vpY, float vpW, float vpH) {
    Camera& activeCamera = (m_ctx.splitView && vpX > 0) ? m_ctx.camera2 : m_ctx.camera;

    glm::mat4 view = activeCamera.getViewMatrix();
    float aspectRatio = vpW / vpH;
    glm::mat4 proj = activeCamera.getProjectionMatrix(aspectRatio);
    glm::mat4 vp = proj * view;

    auto worldToScreen = [&](const glm::vec3& worldPos) -> ImVec2 {
        glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    // Draw all vertices on source object (if set)
    if (m_snapSrcObj && m_snapSrcObj->hasEditableMeshData()) {
        glm::mat4 modelMatrix = m_snapSrcObj->getTransform().getMatrix();
        const auto& heVerts = m_snapSrcObj->getHEVertices();

        // Draw unselected vertices in cyan
        ImU32 vertColor = IM_COL32(0, 200, 255, 200);
        float vertRadius = 6.0f;

        for (size_t vi = 0; vi < heVerts.size(); ++vi) {
            // Skip if already selected
            bool isSelected = false;
            for (uint32_t selIdx : m_snapSrcVertIndices) {
                if (selIdx == vi) { isSelected = true; break; }
            }
            if (isSelected) continue;

            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(heVerts[vi].position, 1.0f));
            ImVec2 screenPos = worldToScreen(worldPos);
            if (screenPos.x > -500) {
                drawList->AddCircleFilled(screenPos, vertRadius, vertColor);
                drawList->AddCircle(screenPos, vertRadius, IM_COL32(0, 0, 0, 255), 0, 1.5f);
            }
        }
    }

    // Draw all vertices on target object (if set and different from source)
    if (m_snapDstObj && m_snapDstObj != m_snapSrcObj && m_snapDstObj->hasEditableMeshData()) {
        glm::mat4 modelMatrix = m_snapDstObj->getTransform().getMatrix();
        const auto& heVerts = m_snapDstObj->getHEVertices();

        // Draw unselected vertices in magenta
        ImU32 vertColor = IM_COL32(255, 100, 200, 200);
        float vertRadius = 6.0f;

        for (size_t vi = 0; vi < heVerts.size(); ++vi) {
            // Skip if already selected
            bool isSelected = false;
            for (uint32_t selIdx : m_snapDstVertIndices) {
                if (selIdx == vi) { isSelected = true; break; }
            }
            if (isSelected) continue;

            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(heVerts[vi].position, 1.0f));
            ImVec2 screenPos = worldToScreen(worldPos);
            if (screenPos.x > -500) {
                drawList->AddCircleFilled(screenPos, vertRadius, vertColor);
                drawList->AddCircle(screenPos, vertRadius, IM_COL32(0, 0, 0, 255), 0, 1.5f);
            }
        }
    }

    // Draw selected source vertices with numbers (green)
    ImU32 srcSelectedColor = IM_COL32(50, 255, 50, 255);
    float selectedRadius = 10.0f;
    for (size_t i = 0; i < m_snapSrcVerts.size(); ++i) {
        ImVec2 screenPos = worldToScreen(m_snapSrcVerts[i]);
        if (screenPos.x > -500) {
            drawList->AddCircleFilled(screenPos, selectedRadius, srcSelectedColor);
            drawList->AddCircle(screenPos, selectedRadius, IM_COL32(0, 0, 0, 255), 0, 2.0f);
            // Draw number
            char numStr[8];
            snprintf(numStr, sizeof(numStr), "%zu", i + 1);
            ImVec2 textSize = ImGui::CalcTextSize(numStr);
            drawList->AddText(ImVec2(screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f),
                             IM_COL32(0, 0, 0, 255), numStr);
        }
    }

    // Draw selected target vertices with numbers (orange)
    ImU32 dstSelectedColor = IM_COL32(255, 165, 0, 255);
    for (size_t i = 0; i < m_snapDstVerts.size(); ++i) {
        ImVec2 screenPos = worldToScreen(m_snapDstVerts[i]);
        if (screenPos.x > -500) {
            drawList->AddCircleFilled(screenPos, selectedRadius, dstSelectedColor);
            drawList->AddCircle(screenPos, selectedRadius, IM_COL32(0, 0, 0, 255), 0, 2.0f);
            // Draw number
            char numStr[8];
            snprintf(numStr, sizeof(numStr), "%zu", i + 1);
            ImVec2 textSize = ImGui::CalcTextSize(numStr);
            drawList->AddText(ImVec2(screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f),
                             IM_COL32(0, 0, 0, 255), numStr);
        }
    }

    drawList->PopClipRect();
}

void ModelingMode::cancelSnapMode() {
    m_snapMode = false;
    m_snapMergeMode = false;
    m_snapSourceObject = nullptr;
    m_snapSourceFace = -1;
}

void ModelingMode::cancelSnapVertexMode() {
    m_snapVertexMode = false;
    m_snapSrcObj = nullptr;
    m_snapDstObj = nullptr;
    m_snapSrcVerts.clear();
    m_snapDstVerts.clear();
    m_snapSrcVertIndices.clear();
    m_snapDstVertIndices.clear();
}

glm::vec3 ModelingMode::getFaceCenter(SceneObject* obj, int faceIdx) {
    if (!obj || !obj->hasEditableMeshData()) return glm::vec3(0.0f);

    const auto& heVerts = obj->getHEVertices();
    const auto& heEdges = obj->getHEHalfEdges();
    const auto& heFaces = obj->getHEFaces();

    if (faceIdx < 0 || faceIdx >= static_cast<int>(heFaces.size())) return glm::vec3(0.0f);

    // Collect face vertices by walking half-edges
    std::vector<uint32_t> faceVertIndices;
    uint32_t startHE = heFaces[faceIdx].halfEdgeIndex;
    uint32_t currHE = startHE;
    do {
        faceVertIndices.push_back(heEdges[currHE].vertexIndex);
        currHE = heEdges[currHE].nextIndex;
    } while (currHE != startHE && faceVertIndices.size() < 10);

    if (faceVertIndices.empty()) return glm::vec3(0.0f);

    glm::mat4 modelMatrix = obj->getTransform().getMatrix();
    glm::vec3 center(0.0f);

    for (uint32_t vi : faceVertIndices) {
        glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(heVerts[vi].position, 1.0f));
        center += worldPos;
    }

    return center / static_cast<float>(faceVertIndices.size());
}

glm::vec3 ModelingMode::getFaceNormal(SceneObject* obj, int faceIdx) {
    if (!obj || !obj->hasEditableMeshData()) return glm::vec3(0.0f, 1.0f, 0.0f);

    const auto& heVerts = obj->getHEVertices();
    const auto& heEdges = obj->getHEHalfEdges();
    const auto& heFaces = obj->getHEFaces();

    if (faceIdx < 0 || faceIdx >= static_cast<int>(heFaces.size())) return glm::vec3(0.0f, 1.0f, 0.0f);

    // Collect face vertices by walking half-edges
    std::vector<uint32_t> faceVertIndices;
    uint32_t startHE = heFaces[faceIdx].halfEdgeIndex;
    uint32_t currHE = startHE;
    do {
        faceVertIndices.push_back(heEdges[currHE].vertexIndex);
        currHE = heEdges[currHE].nextIndex;
    } while (currHE != startHE && faceVertIndices.size() < 10);

    if (faceVertIndices.size() < 3) return glm::vec3(0.0f, 1.0f, 0.0f);

    glm::mat4 modelMatrix = obj->getTransform().getMatrix();
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));

    // Get local positions
    glm::vec3 v0 = heVerts[faceVertIndices[0]].position;
    glm::vec3 v1 = heVerts[faceVertIndices[1]].position;
    glm::vec3 v2 = heVerts[faceVertIndices[2]].position;

    // Calculate normal in local space, then transform to world
    glm::vec3 localNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
    return glm::normalize(normalMatrix * localNormal);
}

void ModelingMode::snapObjectToFace(SceneObject* srcObj, int srcFace, SceneObject* dstObj, int dstFace) {
    if (!srcObj || !dstObj) return;

    // Get face info
    glm::vec3 srcCenter = getFaceCenter(srcObj, srcFace);
    glm::vec3 srcNormal = getFaceNormal(srcObj, srcFace);
    glm::vec3 dstCenter = getFaceCenter(dstObj, dstFace);
    glm::vec3 dstNormal = getFaceNormal(dstObj, dstFace);

    // We want the source face to align with target face
    // Source normal should point opposite to target normal (faces touching)
    glm::vec3 targetNormal = -dstNormal;

    // Calculate rotation to align source normal to target normal
    glm::vec3 rotationAxis = glm::cross(srcNormal, targetNormal);
    float dotProduct = glm::dot(srcNormal, targetNormal);

    glm::quat rotation;
    if (glm::length(rotationAxis) < 0.0001f) {
        // Normals are parallel
        if (dotProduct > 0.0f) {
            // Same direction, no rotation needed
            rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        } else {
            // Opposite direction, 180 degree rotation around any perpendicular axis
            glm::vec3 perpAxis = glm::abs(srcNormal.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            perpAxis = glm::normalize(glm::cross(srcNormal, perpAxis));
            rotation = glm::angleAxis(glm::pi<float>(), perpAxis);
        }
    } else {
        rotationAxis = glm::normalize(rotationAxis);
        float angle = std::acos(glm::clamp(dotProduct, -1.0f, 1.0f));
        rotation = glm::angleAxis(angle, rotationAxis);
    }

    // Apply rotation to source object
    auto& srcTransform = srcObj->getTransform();
    glm::quat currentRot = srcTransform.getRotation();
    srcTransform.setRotation(rotation * currentRot);

    // Recalculate source face center after rotation
    glm::vec3 newSrcCenter = getFaceCenter(srcObj, srcFace);

    // Translate so face centers align
    glm::vec3 translation = dstCenter - newSrcCenter;
    srcTransform.setPosition(srcTransform.getPosition() + translation);

    std::cout << "[Snap] Snapped " << srcObj->getName() << " to " << dstObj->getName() << std::endl;
}

void ModelingMode::snapAndMergeObjects(SceneObject* srcObj, int srcFace, SceneObject* dstObj, int dstFace) {
    if (!srcObj || !dstObj) return;

    // First snap the objects together (aligns centers and normals)
    snapObjectToFace(srcObj, srcFace, dstObj, dstFace);

    // Get data from both objects
    const auto& srcHeVerts = srcObj->getHEVertices();
    const auto& srcHeEdges = srcObj->getHEHalfEdges();
    const auto& srcHeFaces = srcObj->getHEFaces();
    const auto& dstHeVerts = dstObj->getHEVertices();
    const auto& dstHeEdges = dstObj->getHEHalfEdges();
    const auto& dstHeFaces = dstObj->getHEFaces();

    // Get vertices of both snap faces
    std::vector<uint32_t> srcFaceVerts, dstFaceVerts;

    uint32_t startHE = srcHeFaces[srcFace].halfEdgeIndex;
    uint32_t currHE = startHE;
    do {
        srcFaceVerts.push_back(srcHeEdges[currHE].vertexIndex);
        currHE = srcHeEdges[currHE].nextIndex;
    } while (currHE != startHE && srcFaceVerts.size() < 10);

    startHE = dstHeFaces[dstFace].halfEdgeIndex;
    currHE = startHE;
    do {
        dstFaceVerts.push_back(dstHeEdges[currHE].vertexIndex);
        currHE = dstHeEdges[currHE].nextIndex;
    } while (currHE != startHE && dstFaceVerts.size() < 10);

    // Transform matrices
    glm::mat4 srcModelMatrix = srcObj->getTransform().getMatrix();
    glm::mat4 dstModelMatrix = dstObj->getTransform().getMatrix();
    glm::mat4 dstInvMatrix = glm::inverse(dstModelMatrix);

    // Get world positions of destination snap face vertices
    std::vector<glm::vec3> dstFaceWorldPos;
    for (uint32_t vi : dstFaceVerts) {
        dstFaceWorldPos.push_back(glm::vec3(dstModelMatrix * glm::vec4(dstHeVerts[vi].position, 1.0f)));
    }

    // Build position-based correspondence: map source snap vertex POSITIONS to destination positions
    std::vector<std::pair<glm::vec3, glm::vec3>> positionMapping;  // (srcWorldPos, dstWorldPos)

    for (size_t i = 0; i < srcFaceVerts.size(); ++i) {
        uint32_t srcVi = srcFaceVerts[i];
        glm::vec3 srcWorldPos = glm::vec3(srcModelMatrix * glm::vec4(srcHeVerts[srcVi].position, 1.0f));

        // Find closest destination vertex
        float minDist = std::numeric_limits<float>::max();
        int closestIdx = -1;
        for (size_t j = 0; j < dstFaceWorldPos.size(); ++j) {
            float dist = glm::length(srcWorldPos - dstFaceWorldPos[j]);
            if (dist < minDist) {
                minDist = dist;
                closestIdx = static_cast<int>(j);
            }
        }

        if (closestIdx >= 0) {
            positionMapping.push_back({srcWorldPos, dstFaceWorldPos[closestIdx]});
        }
    }

    // Build vertex positions in target's local space
    std::vector<glm::vec3> combinedPositions;
    std::vector<glm::vec2> combinedUVs;
    std::vector<glm::vec4> combinedColors;

    // Map from source vertex index to combined index
    std::map<uint32_t, uint32_t> srcVertexMap;

    // Start with target mesh vertices
    for (const auto& v : dstHeVerts) {
        combinedPositions.push_back(v.position);
        combinedUVs.push_back(v.uv);
        combinedColors.push_back(v.color);
    }

    // Add source mesh vertices
    // For ANY vertex at a snap face position, move it to the destination position
    const float posTolerance = 0.001f;

    for (size_t i = 0; i < srcHeVerts.size(); ++i) {
        const auto& v = srcHeVerts[i];
        uint32_t vi = static_cast<uint32_t>(i);
        glm::vec3 worldPos = glm::vec3(srcModelMatrix * glm::vec4(v.position, 1.0f));

        // Check if this vertex is at a snap face position (by position, not index)
        glm::vec3 finalWorldPos = worldPos;
        for (const auto& [srcPos, dstPos] : positionMapping) {
            if (glm::length(worldPos - srcPos) < posTolerance) {
                finalWorldPos = dstPos;  // Move to destination position
                break;
            }
        }

        glm::vec3 localPos = glm::vec3(dstInvMatrix * glm::vec4(finalWorldPos, 1.0f));

        // Add as new vertex
        srcVertexMap[vi] = static_cast<uint32_t>(combinedPositions.size());
        combinedPositions.push_back(localPos);
        combinedUVs.push_back(v.uv);
        combinedColors.push_back(v.color);
    }

    // Build faces, excluding the snap faces
    std::vector<std::vector<uint32_t>> allFaces;

    // Add target faces (excluding snap face)
    for (size_t fi = 0; fi < dstHeFaces.size(); ++fi) {
        if (static_cast<int>(fi) == dstFace) continue;  // Skip snap face

        std::vector<uint32_t> faceVerts;
        startHE = dstHeFaces[fi].halfEdgeIndex;
        currHE = startHE;
        do {
            faceVerts.push_back(dstHeEdges[currHE].vertexIndex);
            currHE = dstHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);
        allFaces.push_back(faceVerts);
    }

    // Add source faces (excluding snap face, with remapped vertex indices)
    for (size_t fi = 0; fi < srcHeFaces.size(); ++fi) {
        if (static_cast<int>(fi) == srcFace) continue;  // Skip snap face

        std::vector<uint32_t> faceVerts;
        startHE = srcHeFaces[fi].halfEdgeIndex;
        currHE = startHE;
        do {
            uint32_t srcVi = srcHeEdges[currHE].vertexIndex;
            faceVerts.push_back(srcVertexMap[srcVi]);
            currHE = srcHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);
        allFaces.push_back(faceVerts);
    }

    // Build the combined mesh
    std::vector<eden::ModelVertex> combinedVerts;
    std::vector<uint32_t> combinedIndices;

    for (size_t fi = 0; fi < allFaces.size(); ++fi) {
        const auto& faceVerts = allFaces[fi];
        if (faceVerts.size() < 3) continue;

        // Calculate face normal
        glm::vec3 v0 = combinedPositions[faceVerts[0]];
        glm::vec3 v1 = combinedPositions[faceVerts[1]];
        glm::vec3 v2 = combinedPositions[faceVerts[2]];
        glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

        uint32_t baseIdx = static_cast<uint32_t>(combinedVerts.size());

        // Add vertices for this face
        for (uint32_t vi : faceVerts) {
            eden::ModelVertex mv;
            mv.position = combinedPositions[vi];
            mv.normal = normal;
            mv.texCoord = combinedUVs[vi];
            mv.color = combinedColors[vi];
            combinedVerts.push_back(mv);
        }

        // Triangulate (fan)
        for (size_t i = 1; i + 1 < faceVerts.size(); ++i) {
            combinedIndices.push_back(baseIdx);
            combinedIndices.push_back(baseIdx + static_cast<uint32_t>(i));
            combinedIndices.push_back(baseIdx + static_cast<uint32_t>(i + 1));
        }
    }

    // Update target object with combined mesh
    uint32_t handle = m_ctx.modelRenderer.createModel(combinedVerts, combinedIndices, nullptr, 0, 0);
    dstObj->setBufferHandle(handle);
    dstObj->setIndexCount(static_cast<uint32_t>(combinedIndices.size()));
    dstObj->setVertexCount(static_cast<uint32_t>(combinedVerts.size()));
    dstObj->setMeshData(combinedVerts, combinedIndices);
    dstObj->setName(dstObj->getName() + "_merged");

    // Build quad indices for half-edge structure (4 indices per face, not triangulated)
    std::vector<uint32_t> quadIndices;
    uint32_t vertexOffset = 0;
    for (const auto& faceVerts : allFaces) {
        if (faceVerts.size() == 4) {
            // Quad face - add 4 indices
            quadIndices.push_back(vertexOffset);
            quadIndices.push_back(vertexOffset + 1);
            quadIndices.push_back(vertexOffset + 2);
            quadIndices.push_back(vertexOffset + 3);
        } else if (faceVerts.size() == 3) {
            // Triangle face - pad to 4 indices by repeating last vertex
            quadIndices.push_back(vertexOffset);
            quadIndices.push_back(vertexOffset + 1);
            quadIndices.push_back(vertexOffset + 2);
            quadIndices.push_back(vertexOffset + 2);  // Repeat last vertex
        }
        vertexOffset += static_cast<uint32_t>(faceVerts.size());
    }

    // Rebuild half-edge data using quad indices
    m_ctx.editableMesh.buildFromQuads(combinedVerts, quadIndices);

    // Convert EditableMesh data to SceneObject storage format
    const auto& heVerts = m_ctx.editableMesh.getVerticesData();
    const auto& heHalfEdges = m_ctx.editableMesh.getHalfEdges();
    const auto& heFaces = m_ctx.editableMesh.getFacesData();

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

    dstObj->setEditableMeshData(storedVerts, storedHE, storedFaces);

    // Delete source object
    m_ctx.pendingDeletions.push_back(srcObj);

    // Select the combined object
    m_ctx.selectedObject = dstObj;
    m_ctx.objectMode = true;
    buildEditableMeshFromObject();

    std::cout << "[Snap] Merged objects into " << dstObj->getName() << std::endl;
}

void ModelingMode::snapAndMergeWithVertexCorrespondence() {
    if (!m_snapSrcObj || !m_snapDstObj) return;
    if (m_snapSrcVerts.empty() || m_snapSrcVerts.size() != m_snapDstVerts.size()) return;

    std::cout << "[Snap] Merging with " << m_snapSrcVerts.size() << " vertex correspondences" << std::endl;

    // Get data from both objects
    const auto& srcHeVerts = m_snapSrcObj->getHEVertices();
    const auto& srcHeEdges = m_snapSrcObj->getHEHalfEdges();
    const auto& srcHeFaces = m_snapSrcObj->getHEFaces();
    const auto& dstHeVerts = m_snapDstObj->getHEVertices();
    const auto& dstHeEdges = m_snapDstObj->getHEHalfEdges();
    const auto& dstHeFaces = m_snapDstObj->getHEFaces();

    // Transform matrices
    glm::mat4 srcModelMatrix = m_snapSrcObj->getTransform().getMatrix();
    glm::mat4 dstModelMatrix = m_snapDstObj->getTransform().getMatrix();
    glm::mat4 dstInvMatrix = glm::inverse(dstModelMatrix);

    // Build position-based correspondence from user-selected vertices
    // Map: source world position -> destination world position
    std::vector<std::pair<glm::vec3, glm::vec3>> positionMapping;
    const float posTolerance = 0.001f;

    for (size_t i = 0; i < m_snapSrcVerts.size(); ++i) {
        positionMapping.push_back({m_snapSrcVerts[i], m_snapDstVerts[i]});
    }

    // Calculate transformation to align ENTIRE source mesh to target
    // Step 1: Calculate centroids of snap vertices
    glm::vec3 srcCentroid(0.0f);
    glm::vec3 dstCentroid(0.0f);
    for (size_t i = 0; i < m_snapSrcVerts.size(); ++i) {
        srcCentroid += m_snapSrcVerts[i];
        dstCentroid += m_snapDstVerts[i];
    }
    srcCentroid /= static_cast<float>(m_snapSrcVerts.size());
    dstCentroid /= static_cast<float>(m_snapDstVerts.size());

    // Step 2: Calculate translation to align centroids
    glm::vec3 translationOffset = dstCentroid - srcCentroid;

    std::cout << "[Snap] Source centroid: " << srcCentroid.x << ", " << srcCentroid.y << ", " << srcCentroid.z << std::endl;
    std::cout << "[Snap] Target centroid: " << dstCentroid.x << ", " << dstCentroid.y << ", " << dstCentroid.z << std::endl;
    std::cout << "[Snap] Translation offset: " << translationOffset.x << ", " << translationOffset.y << ", " << translationOffset.z << std::endl;

    // Build vertex positions in target's local space
    std::vector<glm::vec3> combinedPositions;
    std::vector<glm::vec2> combinedUVs;
    std::vector<glm::vec4> combinedColors;

    // Map from source vertex index to combined index
    std::map<uint32_t, uint32_t> srcVertexMap;

    // Start with target mesh vertices
    for (const auto& v : dstHeVerts) {
        combinedPositions.push_back(v.position);
        combinedUVs.push_back(v.uv);
        combinedColors.push_back(v.color);
    }

    // Add source mesh vertices - transform ALL vertices by the alignment offset
    for (size_t i = 0; i < srcHeVerts.size(); ++i) {
        const auto& v = srcHeVerts[i];
        uint32_t vi = static_cast<uint32_t>(i);

        // Transform to world space
        glm::vec3 worldPos = glm::vec3(srcModelMatrix * glm::vec4(v.position, 1.0f));

        // Apply alignment translation to move entire mesh to target
        glm::vec3 alignedWorldPos = worldPos + translationOffset;

        // For snap vertices specifically, use exact destination position to avoid floating point errors
        for (const auto& [srcPos, dstPos] : positionMapping) {
            if (glm::length(worldPos - srcPos) < posTolerance) {
                alignedWorldPos = dstPos;  // Use exact position for snap vertices
                break;
            }
        }

        // Transform to target's local space
        glm::vec3 localPos = glm::vec3(dstInvMatrix * glm::vec4(alignedWorldPos, 1.0f));

        // Add as new vertex
        srcVertexMap[vi] = static_cast<uint32_t>(combinedPositions.size());
        combinedPositions.push_back(localPos);
        combinedUVs.push_back(v.uv);
        combinedColors.push_back(v.color);
    }

    // Find faces that use snap vertices (these will be removed as they're the joining faces)
    std::set<int> srcSnapFaces, dstSnapFaces;

    // Find source faces that contain snap vertices
    for (size_t fi = 0; fi < srcHeFaces.size(); ++fi) {
        std::vector<uint32_t> faceVerts;
        uint32_t startHE = srcHeFaces[fi].halfEdgeIndex;
        uint32_t currHE = startHE;
        do {
            faceVerts.push_back(srcHeEdges[currHE].vertexIndex);
            currHE = srcHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);

        // Check if ALL vertices of this face are snap vertices
        bool allSnapVerts = true;
        for (uint32_t vIdx : faceVerts) {
            glm::vec3 worldPos = glm::vec3(srcModelMatrix * glm::vec4(srcHeVerts[vIdx].position, 1.0f));
            bool isSnapVert = false;
            for (const auto& [srcPos, dstPos] : positionMapping) {
                if (glm::length(worldPos - srcPos) < posTolerance) {
                    isSnapVert = true;
                    break;
                }
            }
            if (!isSnapVert) {
                allSnapVerts = false;
                break;
            }
        }
        if (allSnapVerts && faceVerts.size() == m_snapSrcVerts.size()) {
            srcSnapFaces.insert(static_cast<int>(fi));
        }
    }

    // Find destination faces that contain snap vertices
    for (size_t fi = 0; fi < dstHeFaces.size(); ++fi) {
        std::vector<uint32_t> faceVerts;
        uint32_t startHE = dstHeFaces[fi].halfEdgeIndex;
        uint32_t currHE = startHE;
        do {
            faceVerts.push_back(dstHeEdges[currHE].vertexIndex);
            currHE = dstHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);

        // Check if ALL vertices of this face are snap vertices
        bool allSnapVerts = true;
        for (uint32_t vIdx : faceVerts) {
            glm::vec3 worldPos = glm::vec3(dstModelMatrix * glm::vec4(dstHeVerts[vIdx].position, 1.0f));
            bool isSnapVert = false;
            for (const auto& [srcPos, dstPos] : positionMapping) {
                if (glm::length(worldPos - dstPos) < posTolerance) {
                    isSnapVert = true;
                    break;
                }
            }
            if (!isSnapVert) {
                allSnapVerts = false;
                break;
            }
        }
        if (allSnapVerts && faceVerts.size() == m_snapDstVerts.size()) {
            dstSnapFaces.insert(static_cast<int>(fi));
        }
    }

    std::cout << "[Snap] Removing " << srcSnapFaces.size() << " source faces and "
              << dstSnapFaces.size() << " target faces" << std::endl;

    // Build faces, excluding the snap faces
    std::vector<std::vector<uint32_t>> allFaces;

    // Add target faces (excluding snap faces)
    for (size_t fi = 0; fi < dstHeFaces.size(); ++fi) {
        if (dstSnapFaces.count(static_cast<int>(fi)) > 0) continue;

        std::vector<uint32_t> faceVerts;
        uint32_t startHE = dstHeFaces[fi].halfEdgeIndex;
        uint32_t currHE = startHE;
        do {
            faceVerts.push_back(dstHeEdges[currHE].vertexIndex);
            currHE = dstHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);
        allFaces.push_back(faceVerts);
    }

    // Add source faces (excluding snap faces, with remapped vertex indices)
    for (size_t fi = 0; fi < srcHeFaces.size(); ++fi) {
        if (srcSnapFaces.count(static_cast<int>(fi)) > 0) continue;

        std::vector<uint32_t> faceVerts;
        uint32_t startHE = srcHeFaces[fi].halfEdgeIndex;
        uint32_t currHE = startHE;
        do {
            uint32_t srcVi = srcHeEdges[currHE].vertexIndex;
            faceVerts.push_back(srcVertexMap[srcVi]);
            currHE = srcHeEdges[currHE].nextIndex;
        } while (currHE != startHE && faceVerts.size() < 10);
        allFaces.push_back(faceVerts);
    }

    // Build combined editable mesh directly (preserves quad topology)
    eden::EditableMesh tempMesh;
    tempMesh.clear();

    // Add all vertices to editable mesh
    for (size_t i = 0; i < combinedPositions.size(); ++i) {
        eden::HEVertex v;
        v.position = combinedPositions[i];
        v.normal = glm::vec3(0, 1, 0);  // Will recalculate
        v.uv = combinedUVs[i];
        v.color = combinedColors[i];
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        tempMesh.addVertex(v);
    }

    // Add all faces (preserves quads!)
    for (const auto& faceVerts : allFaces) {
        if (faceVerts.size() >= 3) {
            tempMesh.addQuadFace(faceVerts);
        }
    }

    // Recalculate normals
    tempMesh.recalculateNormals();

    // Triangulate for GPU rendering
    std::vector<eden::ModelVertex> combinedVerts;
    std::vector<uint32_t> combinedIndices;
    tempMesh.triangulate(combinedVerts, combinedIndices);

    // Upload combined mesh to GPU
    uint32_t newHandle = m_ctx.modelRenderer.createModel(combinedVerts, combinedIndices, nullptr, 0, 0);
    if (newHandle == UINT32_MAX) {
        std::cout << "[Snap] Failed to create combined mesh" << std::endl;
        return;
    }

    // Delete source object
    m_ctx.pendingDeletions.push_back(m_snapSrcObj);

    // Update destination object with combined mesh
    m_snapDstObj->setBufferHandle(newHandle);
    m_snapDstObj->setIndexCount(static_cast<uint32_t>(combinedIndices.size()));
    m_snapDstObj->setVertexCount(static_cast<uint32_t>(combinedVerts.size()));
    m_snapDstObj->setMeshData(combinedVerts, combinedIndices);
    m_snapDstObj->clearEditableMeshData();  // Force rebuild

    // Store the editable mesh data (quad topology preserved)
    std::vector<SceneObject::StoredHEVertex> storedVerts;
    std::vector<SceneObject::StoredHalfEdge> storedEdges;
    std::vector<SceneObject::StoredHEFace> storedFaces;

    for (size_t i = 0; i < tempMesh.getVertexCount(); ++i) {
        const auto& v = tempMesh.getVertex(static_cast<uint32_t>(i));
        storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
    }
    for (size_t i = 0; i < tempMesh.getHalfEdgeCount(); ++i) {
        const auto& e = tempMesh.getHalfEdge(static_cast<uint32_t>(i));
        storedEdges.push_back({e.vertexIndex, e.faceIndex, e.nextIndex, e.prevIndex, e.twinIndex});
    }
    for (size_t i = 0; i < tempMesh.getFaceCount(); ++i) {
        const auto& f = tempMesh.getFace(static_cast<uint32_t>(i));
        storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
    }
    m_snapDstObj->setEditableMeshData(storedVerts, storedEdges, storedFaces);

    // Update local bounds
    glm::vec3 minBounds(FLT_MAX), maxBounds(-FLT_MAX);
    for (const auto& v : combinedVerts) {
        minBounds = glm::min(minBounds, v.position);
        maxBounds = glm::max(maxBounds, v.position);
    }
    AABB bounds;
    bounds.min = minBounds;
    bounds.max = maxBounds;
    m_snapDstObj->setLocalBounds(bounds);

    // Select the combined object
    m_ctx.selectedObject = m_snapDstObj;
    m_ctx.selectedObjects.clear();
    m_ctx.selectedObjects.insert(m_snapDstObj);
    m_ctx.objectMode = true;
    buildEditableMeshFromObject();

    std::cout << "[Snap] Merged objects using vertex correspondence into " << m_snapDstObj->getName() << std::endl;
}
