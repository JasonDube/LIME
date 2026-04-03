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
    // Skip when scatter edges exist — scatter uses green lines instead
    if (m_scatterEdges.empty()) {
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
    }

    // Draw scatter edges (ring connections) as green lines — backface culled
    ImU32 scatterEdgeColor = IM_COL32(0, 255, 100, 220);
    for (const auto& edge : m_scatterEdges) {
        if (edge.a >= m_retopologyVerts.size() || edge.b >= m_retopologyVerts.size()) continue;
        // Cull if both endpoints face away from camera
        bool aFacing = true, bFacing = true;
        if (edge.a < m_retopologyNormals.size() && glm::length(m_retopologyNormals[edge.a]) > 0.001f)
            aFacing = glm::dot(m_retopologyNormals[edge.a], camPos - m_retopologyVerts[edge.a]) > 0.0f;
        if (edge.b < m_retopologyNormals.size() && glm::length(m_retopologyNormals[edge.b]) > 0.001f)
            bFacing = glm::dot(m_retopologyNormals[edge.b], camPos - m_retopologyVerts[edge.b]) > 0.0f;
        if (!aFacing && !bFacing) continue;

        ImVec2 a = worldToScreen(m_retopologyVerts[edge.a]);
        ImVec2 b = worldToScreen(m_retopologyVerts[edge.b]);
        if (a.x > -500 && b.x > -500) {
            drawList->AddLine(a, b, scatterEdgeColor, 2.0f);
        }
    }

    // Draw existing retopo vertices — only in manual mode (not scatter)
    if (m_scatterEdges.empty()) {
        ImU32 existingVertColor = IM_COL32(255, 255, 0, 220);
        float existingRadius = 6.0f;
        std::set<size_t> drawnVertPositions;
        for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
            if (!quadFacesCamera(m_retopologyQuads[qi])) continue;
            for (int i = 0; i < 4; ++i) {
                ImVec2 screenPos = worldToScreen(m_retopologyQuads[qi].verts[i]);
                if (screenPos.x > -500) {
                    size_t key = (size_t(screenPos.x) << 16) | size_t(screenPos.y);
                    if (drawnVertPositions.insert(key).second) {
                        drawList->AddCircleFilled(screenPos, existingRadius, existingVertColor);
                        drawList->AddCircle(screenPos, existingRadius, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                    }
                }
            }
        }
    }

    // Draw currently placed retopo vertices with numbers (red, larger)
    // Backface cull using stored normals
    ImU32 newVertColor = IM_COL32(255, 50, 50, 255);
    ImU32 existingPickedColor = IM_COL32(50, 255, 50, 255);  // Green for existing vert picks
    float selectedRadius = 10.0f;

    for (size_t i = 0; i < m_retopologyVerts.size(); ++i) {
        // Backface cull: skip points whose normal faces away from camera
        if (i < m_retopologyNormals.size() && glm::length(m_retopologyNormals[i]) > 0.001f) {
            glm::vec3 toCamera = camPos - m_retopologyVerts[i];
            if (glm::dot(m_retopologyNormals[i], toCamera) < 0.0f) continue;
        }

        ImVec2 screenPos = worldToScreen(m_retopologyVerts[i]);
        if (screenPos.x > -500) {
            bool isExisting = (i < m_retopologyVertMeshIdx.size() && m_retopologyVertMeshIdx[i] != UINT32_MAX);
            bool isSelected = m_selectedScatterPoints.count(i) > 0;
            ImU32 color = isSelected ? IM_COL32(255, 255, 0, 255) :
                          (isExisting ? existingPickedColor : newVertColor);
            float radius = isSelected ? 12.0f : selectedRadius;
            drawList->AddCircleFilled(screenPos, radius, color);
            drawList->AddCircle(screenPos, radius, IM_COL32(0, 0, 0, 255), 0, 2.0f);
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
                size_t vi = ring.startIdx + ptIdx;
                if (vi >= m_retopologyVerts.size()) continue;

                // Backface cull
                if (vi < m_retopologyNormals.size() && glm::length(m_retopologyNormals[vi]) > 0.001f) {
                    if (glm::dot(m_retopologyNormals[vi], camPos - m_retopologyVerts[vi]) < 0.0f) continue;
                }

                ImVec2 sp = worldToScreen(m_retopologyVerts[vi]);
                if (sp.x <= -500) continue;

                bool isSelected = (m_ringDragging && m_selectedRing == static_cast<int>(ri) && a == 0);
                ImU32 color = isSelected ? IM_COL32(255, 255, 0, 255) : anchorColors[a];
                float radius = (a == 0) ? 10.0f : 8.0f;
                if (isSelected) radius = 12.0f;

                drawList->AddCircleFilled(sp, radius, color);
                drawList->AddCircle(sp, radius, anchorOutline, 0, 2.0f);

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
    if (m_retopologyVerts.empty() || m_scatterEdges.empty()) {
        // Fallback: use old quad-based method if no scatter data
        if (!m_retopologyQuads.empty()) {
            std::cout << "[Retopo] Finalizing from quads (no scatter data)" << std::endl;
        } else {
            std::cout << "[Retopo] Nothing to finalize" << std::endl;
            return;
        }
    }

    EditableMesh retopoMesh;
    size_t N = m_retopologyVerts.size();

    // ── Build directly from scatter vertices and edges ──
    // Step 1: Build adjacency from scatter edges
    std::vector<std::set<size_t>> adj(N);
    for (const auto& e : m_scatterEdges) {
        if (e.a < N && e.b < N) {
            adj[e.a].insert(e.b);
            adj[e.b].insert(e.a);
        }
    }

    // Step 2: Find all quad faces by walking the edge graph
    // For each edge (A→B), try to find C and D such that A-B-C-D forms a quad
    std::set<uint64_t> usedFaces;  // Track found quads to avoid duplicates
    std::vector<std::vector<uint32_t>> faceIndices;

    auto faceKey = [](std::vector<size_t> verts) -> uint64_t {
        std::sort(verts.begin(), verts.end());
        uint64_t key = 0;
        for (size_t v : verts) key = key * 131071 + v;
        return key;
    };

    for (size_t a = 0; a < N; a++) {
        for (size_t b : adj[a]) {
            if (b <= a) continue;  // Avoid duplicate edge starts
            // Find quads: A-B-C-D where B-C, C-D, D-A are all edges
            for (size_t c : adj[b]) {
                if (c == a) continue;
                for (size_t d : adj[c]) {
                    if (d == b || d == a) continue;
                    if (adj[d].count(a) == 0) continue;  // D must connect back to A
                    // Found quad A, B, C, D
                    std::vector<size_t> fv = {a, b, c, d};
                    uint64_t key = faceKey(fv);
                    if (usedFaces.count(key)) continue;
                    usedFaces.insert(key);
                    faceIndices.push_back({static_cast<uint32_t>(a), static_cast<uint32_t>(b),
                                           static_cast<uint32_t>(c), static_cast<uint32_t>(d)});
                }
            }
        }
    }

    // Also find triangles (pole caps)
    for (size_t a = 0; a < N; a++) {
        for (size_t b : adj[a]) {
            if (b <= a) continue;
            for (size_t c : adj[b]) {
                if (c <= a || c == b) continue;
                if (adj[c].count(a) == 0) continue;
                std::vector<size_t> fv = {a, b, c};
                uint64_t key = faceKey(fv);
                if (usedFaces.count(key)) continue;
                // Only add as triangle if none of the 3 form a quad — skip if part of a quad
                bool partOfQuad = false;
                for (const auto& face : faceIndices) {
                    if (face.size() == 4) {
                        int matches = 0;
                        for (uint32_t fi : face) {
                            if (fi == a || fi == b || fi == c) matches++;
                        }
                        if (matches >= 3) { partOfQuad = true; break; }
                    }
                }
                if (!partOfQuad) {
                    usedFaces.insert(key);
                    faceIndices.push_back({static_cast<uint32_t>(a), static_cast<uint32_t>(b),
                                           static_cast<uint32_t>(c)});
                }
            }
        }
    }

    std::cout << "[Retopo] Built " << faceIndices.size() << " faces from " << N
              << " verts and " << m_scatterEdges.size() << " edges" << std::endl;

    // Step 3: Spherical UV projection from mesh center
    // U = longitude (theta), V = latitude (phi). Seam at -Z (back of head).
    std::vector<glm::vec2> vertexUVs(N, glm::vec2(0.0f));
    {
        // Compute mesh center
        glm::vec3 meshCenter(0.0f);
        for (size_t vi = 0; vi < N; vi++) meshCenter += m_retopologyVerts[vi];
        meshCenter /= static_cast<float>(N);

        // Compute mesh vertical extent for V normalization
        float minY = FLT_MAX, maxY = -FLT_MAX;
        for (size_t vi = 0; vi < N; vi++) {
            minY = std::min(minY, m_retopologyVerts[vi].y);
            maxY = std::max(maxY, m_retopologyVerts[vi].y);
        }
        float heightRange = maxY - minY;
        if (heightRange < 0.0001f) heightRange = 1.0f;

        for (size_t vi = 0; vi < N; vi++) {
            glm::vec3 dir = m_retopologyVerts[vi] - meshCenter;

            // U = longitude: atan2(x, z) gives angle on XZ plane
            // Offset so seam is at -Z (back): atan2(x, z) = 0 at +Z (front), π at -Z (back)
            float theta = std::atan2(dir.x, dir.z);  // -π to π, 0 = +Z (front)
            // Shift so seam is at -Z: theta=π and theta=-π both map to the seam
            float u = (theta + 3.14159f) / (2.0f * 3.14159f);  // 0 at -Z, wraps to 1 at -Z

            // V = latitude: normalize Y position to 0-1
            float v = (m_retopologyVerts[vi].y - minY) / heightRange;

            vertexUVs[vi] = glm::vec2(u, v);
        }
    }

    // Step 4: Fix UV seam — duplicate vertices at the seam where U wraps from ~1 to ~0
    // For each face, if any edge has a U jump > 0.5, duplicate the low-U verts with U+1
    // Then we'll have correct faces. Extra verts with U>1 get rescaled at the end.
    std::unordered_map<size_t, size_t> seamDuplicates;  // original idx → duplicate idx (U+1)

    for (auto& fi : faceIndices) {
        // Check if this face crosses the seam
        bool crossesSeam = false;
        float maxU = -FLT_MAX, minU = FLT_MAX;
        for (uint32_t idx : fi) {
            maxU = std::max(maxU, vertexUVs[idx].x);
            minU = std::min(minU, vertexUVs[idx].x);
        }
        crossesSeam = (maxU - minU) > 0.5f;

        if (crossesSeam) {
            // Duplicate low-U vertices with U+1
            for (size_t i = 0; i < fi.size(); i++) {
                uint32_t idx = fi[i];
                if (vertexUVs[idx].x < 0.5f) {
                    // Need a duplicate with U+1
                    auto dupIt = seamDuplicates.find(idx);
                    size_t dupIdx;
                    if (dupIt != seamDuplicates.end()) {
                        dupIdx = dupIt->second;
                    } else {
                        dupIdx = vertexUVs.size();
                        // Copy position/normal from original, UV with U+1
                        m_retopologyVerts.push_back(m_retopologyVerts[idx]);
                        if (idx < m_retopologyNormals.size())
                            m_retopologyNormals.push_back(m_retopologyNormals[idx]);
                        else
                            m_retopologyNormals.push_back(glm::vec3(0, 1, 0));
                        vertexUVs.push_back(glm::vec2(vertexUVs[idx].x + 1.0f, vertexUVs[idx].y));
                        seamDuplicates[idx] = dupIdx;
                    }
                    fi[i] = static_cast<uint32_t>(dupIdx);
                }
            }
        }
    }

    // Update N to include duplicates
    N = vertexUVs.size();

    // Rescale all U values to fit 0-1 (max U might be >1 from seam dups)
    float maxU = 0.0f;
    for (size_t vi = 0; vi < N; vi++) maxU = std::max(maxU, vertexUVs[vi].x);
    if (maxU > 1.0f) {
        for (size_t vi = 0; vi < N; vi++) vertexUVs[vi].x /= maxU;
    }

    // Step 4b: Add vertices to mesh
    for (size_t vi = 0; vi < N; vi++) {
        HEVertex v;
        v.position = (vi < m_retopologyVerts.size()) ? m_retopologyVerts[vi] : glm::vec3(0);
        v.normal = (vi < m_retopologyNormals.size()) ? m_retopologyNormals[vi] : glm::vec3(0, 1, 0);
        v.uv = vertexUVs[vi];
        v.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
        v.halfEdgeIndex = UINT32_MAX;
        v.selected = false;
        retopoMesh.addVertex(v);
    }

    if (!seamDuplicates.empty())
        std::cout << "[Retopo] Duplicated " << seamDuplicates.size() << " seam vertices for UV wrap" << std::endl;

    // Step 5: Fix winding then add faces
    // Compute mesh center for outward direction test
    glm::vec3 meshCenter(0.0f);
    for (size_t vi = 0; vi < N; vi++) meshCenter += m_retopologyVerts[vi];
    meshCenter /= static_cast<float>(N);

    for (auto& fi : faceIndices) {
        if (fi.size() < 3) continue;

        // Compute face center and normal from winding
        glm::vec3 faceCenter(0.0f);
        for (uint32_t idx : fi) faceCenter += m_retopologyVerts[idx];
        faceCenter /= static_cast<float>(fi.size());

        glm::vec3 e1 = m_retopologyVerts[fi[1]] - m_retopologyVerts[fi[0]];
        glm::vec3 e2 = m_retopologyVerts[fi[fi.size()-1]] - m_retopologyVerts[fi[0]];
        glm::vec3 faceNormal = glm::cross(e1, e2);

        // Should point outward: away from mesh center
        glm::vec3 outDir = faceCenter - meshCenter;
        if (glm::dot(faceNormal, outDir) < 0.0f) {
            std::reverse(fi.begin(), fi.end());
        }

        if (fi.size() == 4) {
            retopoMesh.addQuadFace(fi);
        } else {
            retopoMesh.addFace(fi);
        }
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
              << " faces, " << N << " vertices, "
              << indices.size() / 3 << " triangles" << std::endl;
}
