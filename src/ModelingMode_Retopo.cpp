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

    // Draw scatter edges (ring connections) as green lines
    ImU32 scatterEdgeColor = IM_COL32(0, 255, 100, 220);
    for (const auto& edge : m_scatterEdges) {
        if (edge.a >= m_retopologyVerts.size() || edge.b >= m_retopologyVerts.size()) continue;
        ImVec2 a = worldToScreen(m_retopologyVerts[edge.a]);
        ImVec2 b = worldToScreen(m_retopologyVerts[edge.b]);
        if (a.x > -500 && b.x > -500) {
            drawList->AddLine(a, b, scatterEdgeColor, 2.0f);
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

    // Draw edges between consecutive placed vertices (only in manual placement mode, not scatter)
    if (m_retopologyVerts.size() >= 2 && m_scatterEdges.empty()) {
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

    // Draw ring anchor points (0, N/4, N/2, 3N/4) — ON TOP of everything
    // Point 0 = green (right, Z=0 +X), N/4 = cyan (front, X=0 +Z),
    // N/2 = blue (left, Z=0 -X), 3N/4 = magenta (back, X=0 -Z)
    if (!m_scatterRings.empty()) {
        ImU32 anchorColors[4] = {
            IM_COL32(0, 255, 0, 255),    // 0: green (right)
            IM_COL32(0, 255, 255, 255),  // N/4: cyan (front)
            IM_COL32(80, 80, 255, 255),  // N/2: blue (left)
            IM_COL32(255, 0, 255, 255),  // 3N/4: magenta (back)
        };
        ImU32 anchorOutline = IM_COL32(0, 0, 0, 255);

        for (size_t ri = 0; ri < m_scatterRings.size(); ri++) {
            const auto& ring = m_scatterRings[ri];
            if (ring.count < 4 || ring.startIdx >= m_retopologyVerts.size()) continue;
            int N = static_cast<int>(ring.count);
            int quarter = N / 4;

            for (int a = 0; a < 4; a++) {
                int ptIdx = a * quarter;
                if (ring.startIdx + ptIdx >= m_retopologyVerts.size()) continue;

                ImVec2 sp = worldToScreen(m_retopologyVerts[ring.startIdx + ptIdx]);
                if (sp.x <= -500) continue;

                bool isSelected = (m_ringDragging && m_selectedRing == static_cast<int>(ri) && a == 0);
                ImU32 color = isSelected ? IM_COL32(255, 255, 0, 255) : anchorColors[a];
                float radius = (a == 0) ? 10.0f : 8.0f;
                if (isSelected) radius = 12.0f;

                drawList->AddCircleFilled(sp, radius, color);
                drawList->AddCircle(sp, radius, anchorOutline, 0, 2.0f);

                // Show XYZ only on the lead point (point 0)
                if (a == 0) {
                    glm::vec3 pos = m_retopologyVerts[ring.startIdx];
                    char coordBuf[64];
                    snprintf(coordBuf, sizeof(coordBuf), "%.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
                    ImVec2 textPos(sp.x + radius + 4, sp.y - 6);
                    ImU32 textBg = IM_COL32(0, 0, 0, 180);
                    ImVec2 textSize = ImGui::CalcTextSize(coordBuf);
                    drawList->AddRectFilled(
                        ImVec2(textPos.x - 2, textPos.y - 1),
                        ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1),
                        textBg, 2.0f);
                    drawList->AddText(textPos, color, coordBuf);
                }
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

    // Assign cylindrical UVs using scatter ring topology if available.
    // Seam along -X anchor points (index N/2 per ring).
    // U = position around ring starting from seam, V = ring level bottom to top.
    std::unordered_map<uint32_t, glm::vec2> vertexUVs;
    bool hasRingUVs = false;

    if (!m_scatterRings.empty()) {
        int numRings = static_cast<int>(m_scatterRings.size());
        // Check if we have pole points
        bool hasBottomPole = (m_bottomPoleIdx != SIZE_MAX && m_bottomPoleIdx < m_retopologyVerts.size());
        bool hasTopPole = (m_topPoleIdx != SIZE_MAX && m_topPoleIdx < m_retopologyVerts.size());

        for (int ri = 0; ri < numRings; ri++) {
            const auto& ring = m_scatterRings[ri];
            int N = static_cast<int>(ring.count);
            int seamIdx = N / 2;  // -X anchor point = seam

            // V = ring level (0 = bottom, 1 = top)
            // Leave room for poles at V=0 and V=1
            float v = (hasBottomPole ? 0.02f : 0.0f) +
                      (static_cast<float>(ri) / std::max(1, numRings - 1)) *
                      (1.0f - (hasBottomPole ? 0.02f : 0.0f) - (hasTopPole ? 0.02f : 0.0f));

            for (int pi = 0; pi < N; pi++) {
                // U starts from the seam (N/2), wraps around
                int offsetFromSeam = ((pi - seamIdx) + N) % N;
                float u = static_cast<float>(offsetFromSeam) / N;

                // Find which unique vertex this scatter point maps to
                glm::vec3 pos = m_retopologyVerts[ring.startIdx + pi];
                for (uint32_t vi = 0; vi < static_cast<uint32_t>(uniquePositions.size()); vi++) {
                    if (glm::length(uniquePositions[vi] - pos) < mergeThreshold) {
                        vertexUVs[vi] = glm::vec2(u, v);
                        break;
                    }
                }
            }
        }

        // Pole UVs
        if (hasBottomPole) {
            glm::vec3 polePos = m_retopologyVerts[m_bottomPoleIdx];
            for (uint32_t vi = 0; vi < static_cast<uint32_t>(uniquePositions.size()); vi++) {
                if (glm::length(uniquePositions[vi] - polePos) < mergeThreshold) {
                    vertexUVs[vi] = glm::vec2(0.5f, 0.0f);
                    break;
                }
            }
        }
        if (hasTopPole) {
            glm::vec3 polePos = m_retopologyVerts[m_topPoleIdx];
            for (uint32_t vi = 0; vi < static_cast<uint32_t>(uniquePositions.size()); vi++) {
                if (glm::length(uniquePositions[vi] - polePos) < mergeThreshold) {
                    vertexUVs[vi] = glm::vec2(0.5f, 1.0f);
                    break;
                }
            }
        }

        hasRingUVs = !vertexUVs.empty();
        if (hasRingUVs) {
            std::cout << "[Retopo] Assigned cylindrical UVs from ring topology ("
                      << vertexUVs.size() << " verts, seam at -X)" << std::endl;
        }
    }

    // Add vertices to the fresh mesh
    for (uint32_t vi = 0; vi < static_cast<uint32_t>(uniquePositions.size()); vi++) {
        HEVertex v;
        v.position = uniquePositions[vi];
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        auto uvIt = vertexUVs.find(vi);
        v.uv = (uvIt != vertexUVs.end()) ? uvIt->second : glm::vec2(0.0f);
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
