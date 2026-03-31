// ModelingMode_Retopo.cpp - Retopology tools for ModelingMode
// Split from ModelingMode.cpp for better organization

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>

#include <iostream>
#include <limits>

using namespace eden;

void ModelingMode::drawRetopologyOverlay(float vpX, float vpY, float vpW, float vpH) {
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

    // Camera position for backface culling
    glm::vec3 camPos = activeCamera.getPosition();

    // Backface cull helper: compute quad face normal and check if it faces the camera
    auto quadFacesCamera = [&](const RetopologyQuad& quad) -> bool {
        glm::vec3 edge1 = quad.verts[1] - quad.verts[0];
        glm::vec3 edge2 = quad.verts[3] - quad.verts[0];
        glm::vec3 faceNormal = glm::cross(edge1, edge2);
        glm::vec3 center = (quad.verts[0] + quad.verts[1] + quad.verts[2] + quad.verts[3]) * 0.25f;
        glm::vec3 toCamera = camPos - center;
        return glm::dot(faceNormal, toCamera) > 0.0f;
    };

    // Draw previously created retopo quads as yellow wireframe (backface culled)
    ImU32 quadEdgeColor = IM_COL32(255, 255, 0, 200);
    for (const auto& quad : m_retopologyQuads) {
        if (!quadFacesCamera(quad)) continue;
        for (int i = 0; i < 4; ++i) {
            ImVec2 a = worldToScreen(quad.verts[i]);
            ImVec2 b = worldToScreen(quad.verts[(i + 1) % 4]);
            if (a.x > -500 && b.x > -500) {
                drawList->AddLine(a, b, quadEdgeColor, 2.0f);
            }
        }
    }

    // Draw existing retopo vertices — only if at least one adjacent quad faces the camera
    ImU32 existingVertColor = IM_COL32(255, 255, 0, 220);
    float existingRadius = 6.0f;
    std::set<size_t> drawnVertPositions;  // Avoid drawing duplicate dots at same screen pos
    for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
        if (!quadFacesCamera(m_retopologyQuads[qi])) continue;
        for (int i = 0; i < 4; ++i) {
            ImVec2 screenPos = worldToScreen(m_retopologyQuads[qi].verts[i]);
            if (screenPos.x > -500) {
                // Simple dedup by screen pixel
                size_t key = (size_t(screenPos.x) << 16) | size_t(screenPos.y);
                if (drawnVertPositions.insert(key).second) {
                    drawList->AddCircleFilled(screenPos, existingRadius, existingVertColor);
                    drawList->AddCircle(screenPos, existingRadius, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                }
            }
        }
    }

    // Draw currently placed retopo vertices with numbers (red, larger)
    ImU32 newVertColor = IM_COL32(255, 50, 50, 255);
    ImU32 existingPickedColor = IM_COL32(50, 255, 50, 255);  // Green for existing vert picks
    float selectedRadius = 10.0f;

    for (size_t i = 0; i < m_retopologyVerts.size(); ++i) {
        ImVec2 screenPos = worldToScreen(m_retopologyVerts[i]);
        if (screenPos.x > -500) {
            bool isExisting = (i < m_retopologyVertMeshIdx.size() && m_retopologyVertMeshIdx[i] != UINT32_MAX);
            ImU32 color = isExisting ? existingPickedColor : newVertColor;
            drawList->AddCircleFilled(screenPos, selectedRadius, color);
            drawList->AddCircle(screenPos, selectedRadius, IM_COL32(0, 0, 0, 255), 0, 2.0f);
            // Draw number (1-based)
            char numStr[8];
            snprintf(numStr, sizeof(numStr), "%zu", i + 1);
            ImVec2 textSize = ImGui::CalcTextSize(numStr);
            drawList->AddText(ImVec2(screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f),
                             IM_COL32(255, 255, 255, 255), numStr);
        }
    }

    // Draw edges between consecutive placed vertices
    if (m_retopologyVerts.size() >= 2) {
        ImU32 edgeColor = IM_COL32(255, 100, 100, 200);
        for (size_t i = 0; i < m_retopologyVerts.size() - 1; ++i) {
            ImVec2 a = worldToScreen(m_retopologyVerts[i]);
            ImVec2 b = worldToScreen(m_retopologyVerts[i + 1]);
            if (a.x > -500 && b.x > -500) {
                drawList->AddLine(a, b, edgeColor, 2.0f);
            }
        }
        // Close the loop if we have 4 vertices (show the quad outline)
        if (m_retopologyVerts.size() == 4) {
            ImVec2 a = worldToScreen(m_retopologyVerts[3]);
            ImVec2 b = worldToScreen(m_retopologyVerts[0]);
            if (a.x > -500 && b.x > -500) {
                drawList->AddLine(a, b, edgeColor, 2.0f);
            }
        }
    }

    drawList->PopClipRect();
}

void ModelingMode::cancelRetopologyMode() {
    m_retopologyMode = false;
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();
    std::cout << "[Retopo] Mode cancelled" << std::endl;
}

void ModelingMode::createRetopologyQuad() {
    if (m_retopologyVerts.size() != 4) {
        std::cout << "[Retopo] Need exactly 4 vertices, have " << m_retopologyVerts.size() << std::endl;
        return;
    }

    // Store quad in world space, then fix winding so normal faces away from live surface
    RetopologyQuad quad;
    quad.verts[0] = m_retopologyVerts[0];
    quad.verts[1] = m_retopologyVerts[1];
    quad.verts[2] = m_retopologyVerts[2];
    quad.verts[3] = m_retopologyVerts[3];

    // Compute quad face normal from cross product
    glm::vec3 edge1 = quad.verts[1] - quad.verts[0];
    glm::vec3 edge2 = quad.verts[3] - quad.verts[0];
    glm::vec3 faceNormal = glm::cross(edge1, edge2);
    float faceNormalLen = glm::length(faceNormal);
    if (faceNormalLen > 0.0001f) faceNormal /= faceNormalLen;

    // Use the stored surface normals from raycast hits to determine correct winding.
    // Average the non-zero normals (new verts have surface normals, picked existing verts have zero).
    glm::vec3 avgSurfaceNormal(0.0f);
    int normalCount = 0;
    for (size_t i = 0; i < m_retopologyNormals.size() && i < 4; ++i) {
        if (glm::length(m_retopologyNormals[i]) > 0.001f) {
            avgSurfaceNormal += m_retopologyNormals[i];
            normalCount++;
        }
    }

    // If we have surface normals, use them; otherwise fall back to raycast from center
    bool needsFlip = false;
    if (normalCount > 0) {
        avgSurfaceNormal = glm::normalize(avgSurfaceNormal);
        // Quad normal should point same direction as surface normal (outward)
        if (glm::dot(faceNormal, avgSurfaceNormal) < 0.0f) {
            needsFlip = true;
        }
    } else if (m_retopologyLiveObj) {
        // Fallback: raycast from quad center
        glm::vec3 center = (quad.verts[0] + quad.verts[1] + quad.verts[2] + quad.verts[3]) * 0.25f;
        auto hitAway = m_retopologyLiveObj->raycast(center, -faceNormal);
        auto hitToward = m_retopologyLiveObj->raycast(center, faceNormal);
        if (hitAway.hit && !hitToward.hit) {
            needsFlip = true;
        } else if (hitAway.hit && hitToward.hit) {
            if (hitAway.distance < hitToward.distance) {
                needsFlip = true;
            }
        }
    }

    if (needsFlip) {
        std::swap(quad.verts[1], quad.verts[3]);
    }

    m_retopologyQuads.push_back(quad);

    std::cout << "[Retopo] Stored quad " << m_retopologyQuads.size()
              << " (world positions only, no mesh built yet)" << std::endl;

    // Clear placed vertices for next quad (but keep quads list for overlay)
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();
}

void ModelingMode::finalizeRetopologyMesh() {
    if (m_retopologyQuads.empty()) {
        std::cout << "[Retopo] No quads to finalize" << std::endl;
        return;
    }

    // Build a fresh editable mesh from ONLY the retopo quads (not whatever is in m_ctx.editableMesh)
    EditableMesh retopoMesh;

    // Collect unique vertices and build quad index lists
    std::vector<glm::vec3> uniquePositions;
    std::vector<std::vector<uint32_t>> quadIndices;
    const float mergeThreshold = 0.001f;

    for (const auto& quad : m_retopologyQuads) {
        std::vector<uint32_t> faceIndices;
        for (int i = 0; i < 4; ++i) {
            // Check if this position already exists
            uint32_t foundIdx = UINT32_MAX;
            for (size_t vi = 0; vi < uniquePositions.size(); ++vi) {
                if (glm::length(uniquePositions[vi] - quad.verts[i]) < mergeThreshold) {
                    foundIdx = static_cast<uint32_t>(vi);
                    break;
                }
            }
            if (foundIdx == UINT32_MAX) {
                foundIdx = static_cast<uint32_t>(uniquePositions.size());
                uniquePositions.push_back(quad.verts[i]);
            }
            faceIndices.push_back(foundIdx);
        }
        quadIndices.push_back(faceIndices);
    }

    // Add vertices to the fresh mesh
    for (const auto& pos : uniquePositions) {
        HEVertex v;
        v.position = pos;
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        v.uv = glm::vec2(0.0f);
        v.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        retopoMesh.addVertex(v);
    }

    // Add quad faces
    for (const auto& fi : quadIndices) {
        retopoMesh.addQuadFace(fi);
    }

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
        newObj->setDescription("Retopology mesh");
        retopoObj = newObj.get();
        m_ctx.sceneObjects.push_back(std::move(newObj));
    }

    // Triangulate for GPU
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::set<uint32_t> noHidden;
    retopoMesh.triangulate(vertices, indices, noHidden);

    if (indices.empty()) {
        std::cout << "[Retopo] Triangulation produced no geometry" << std::endl;
        return;
    }

    // Destroy old GPU model if one exists
    uint32_t oldHandle = retopoObj->getBufferHandle();
    if (oldHandle != UINT32_MAX) {
        m_ctx.modelRenderer.destroyModel(oldHandle);
    }

    // Create new GPU model
    uint32_t newHandle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
    retopoObj->setBufferHandle(newHandle);
    retopoObj->setIndexCount(static_cast<uint32_t>(indices.size()));
    retopoObj->setVertexCount(static_cast<uint32_t>(vertices.size()));
    retopoObj->setMeshData(vertices, indices);
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
    for (const auto& v : vertices) {
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
        uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
        for (uint32_t i = 0; i < triCount; ++i) {
            m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
        }
    }

    // Clear any stale selection state
    m_ctx.selectedFaces.clear();
    m_ctx.hiddenFaces.clear();

    // Exit retopo mode
    m_retopologyMode = false;
    m_retopologyQuads.clear();
    m_retopologyVerts.clear();
    m_retopologyNormals.clear();
    m_retopologyVertMeshIdx.clear();
    m_retopologyObjCreated = false;
    std::cout << "[Retopo] Finalized retopo mesh: " << retopoMesh.getFaceCount()
              << " faces, " << uniquePositions.size() << " vertices, "
              << indices.size() / 3 << " triangles" << std::endl;
}
