#include "ModelingMode.hpp"
#include "Editor/GLBLoader.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>
#include <nfd.h>
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <iostream>
#include <fstream>
#include <queue>
#include <set>
#include <map>
#include <filesystem>
#include <limits>
#include <random>
#include <unordered_set>
#include <unordered_map>

using namespace eden;

// Base64 encoding/decoding for .limes texture embedding
static const char* limes_b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string limes_base64_encode(const unsigned char* data, size_t len) {
    std::string ret;
    ret.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = (unsigned int)data[i] << 16;
        if (i + 1 < len) n |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) n |= (unsigned int)data[i + 2];
        ret.push_back(limes_b64_chars[(n >> 18) & 0x3F]);
        ret.push_back(limes_b64_chars[(n >> 12) & 0x3F]);
        ret.push_back((i + 1 < len) ? limes_b64_chars[(n >> 6) & 0x3F] : '=');
        ret.push_back((i + 2 < len) ? limes_b64_chars[n & 0x3F] : '=');
    }
    return ret;
}

static std::vector<unsigned char> limes_base64_decode(const std::string& encoded) {
    std::vector<unsigned char> ret;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)limes_b64_chars[i]] = i;
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


// Debug flag for wireframe rendering (reset when mesh is rebuilt)
static bool g_wireframeDebugPrinted = false;

// Bridge edges segment count (shared between UI and keyboard shortcut)
static int g_bridgeSegments = 1;

// Helper to check if a vertex is on a UV seam (boundary edge)
static bool isSeamVertex(const EditableMesh& mesh, uint32_t vertIdx) {
    // Get all half-edges emanating from this vertex
    auto edges = mesh.getVertexEdges(vertIdx);
    for (uint32_t heIdx : edges) {
        const HalfEdge& he = mesh.getHalfEdge(heIdx);
        // If this half-edge has no twin, it's a boundary edge
        if (he.twinIndex == UINT32_MAX) {
            return true;
        }
    }
    return false;
}

ModelingMode::ModelingMode(EditorContext& ctx)
    : IEditorMode(ctx)
{
}

void ModelingMode::onActivate() {
    // Build editable mesh if we have a selected object
    if (m_ctx.selectedObject && m_ctx.selectedObject->hasMeshData()) {
        buildEditableMeshFromObject();
    }
}

void ModelingMode::onDeactivate() {
    // Nothing special needed
}

void ModelingMode::processInput(float deltaTime) {
    bool gizmoConsumedInput = processGizmoInput();
    processModelingInput(deltaTime, gizmoConsumedInput);
}

void ModelingMode::update(float deltaTime) {
    // Update mode notification timer
    if (m_modeNotificationTimer > 0.0f) {
        m_modeNotificationTimer -= deltaTime;
    }
    if (m_saveNotificationTimer > 0.0f) {
        m_saveNotificationTimer -= deltaTime;
    }

    // Process deferred mesh updates (must happen before rendering, not during)
    if (m_ctx.meshDirty) {
        invalidateWireframeCache();
        updateMeshFromEditable();
    }

    // Process deferred clone image deletions (must happen before ImGui rendering)
    if (m_pendingCloneImageDelete >= 0 && m_pendingCloneImageDelete < static_cast<int>(m_ctx.cloneSourceImages.size())) {
        int idx = m_pendingCloneImageDelete;
        m_pendingCloneImageDelete = -1;  // Clear before deletion

        // Destroy Vulkan texture
        if (m_ctx.destroyCloneImageTextureCallback) {
            m_ctx.destroyCloneImageTextureCallback(m_ctx.cloneSourceImages[idx]);
        }
        m_ctx.cloneSourceImages.erase(m_ctx.cloneSourceImages.begin() + idx);

        // Adjust selected index
        if (m_ctx.imageRefSelectedIndex >= static_cast<int>(m_ctx.cloneSourceImages.size())) {
            m_ctx.imageRefSelectedIndex = static_cast<int>(m_ctx.cloneSourceImages.size()) - 1;
        }
    }

    // Process deferred stamp preview update (must happen before ImGui rendering)
    if (m_pendingStampPreviewUpdate && !m_ctx.stampData.empty()) {
        m_pendingStampPreviewUpdate = false;
        if (m_ctx.updateStampPreviewCallback) {
            m_ctx.updateStampPreviewCallback(m_ctx.stampData.data(), m_ctx.stampWidth, m_ctx.stampHeight);
        }
    }
}

void ModelingMode::renderUI() {
    // Display mode notification overlay
    if (m_modeNotificationTimer > 0.0f) {
        const char* modeText = m_ctx.objectMode ? "OBJECT MODE" : "COMPONENT MODE";

        // Center the notification horizontally, near top of screen
        ImVec2 windowSize = ImGui::GetIO().DisplaySize;
        ImVec2 textSize = ImGui::CalcTextSize(modeText);
        ImVec2 pos((windowSize.x - textSize.x * 2.0f) * 0.5f, 50.0f);

        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowBgAlpha(0.7f * m_modeNotificationTimer);  // Fade out
        ImGui::Begin("##ModeNotification", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);

        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, m_modeNotificationTimer), "%s", modeText);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::End();
    }

    // Display save notification overlay
    if (m_saveNotificationTimer > 0.0f) {
        const char* saveText = "FILE SAVED";

        ImVec2 windowSize = ImGui::GetIO().DisplaySize;
        ImVec2 textSize = ImGui::CalcTextSize(saveText);
        ImVec2 pos((windowSize.x - textSize.x * 2.0f) * 0.5f, 50.0f);

        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowBgAlpha(0.7f * m_saveNotificationTimer);
        ImGui::Begin("##SaveNotification", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);

        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, m_saveNotificationTimer), "%s", saveText);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::End();
    }

    renderModelingEditorUI();

    // WYSIWYG stamp preview - actually render the stamp on the texture
    if (m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
        bool shouldShowPreview = m_ctx.isPainting && m_ctx.useStamp && !m_ctx.stampData.empty();
        bool mouseOverImGui = ImGui::GetIO().WantCaptureMouse;

        if (shouldShowPreview && !mouseOverImGui) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
            if (hit.hit) {
                if (m_ctx.stampProjectFromView) {
                    // TRUE project from view: raycast each stamp pixel from camera
                    Camera& cam = m_ctx.getActiveCamera();
                    glm::vec3 camPos = cam.getPosition();
                    glm::vec3 camRight = cam.getRight();
                    glm::vec3 camUp = cam.getUp();
                    float worldSizeH = m_ctx.stampScale * m_ctx.stampScaleH * 0.5f;  // World-space size H
                    float worldSizeV = m_ctx.stampScale * m_ctx.stampScaleV * 0.5f;  // World-space size V

                    m_ctx.selectedObject->stampProjectedFromViewPreview(
                        hit.position, camPos, camRight, camUp,
                        m_ctx.stampData.data(), m_ctx.stampWidth, m_ctx.stampHeight,
                        worldSizeH, worldSizeV, m_ctx.stampRotation, m_ctx.stampOpacity, m_ctx.stampFlipH, m_ctx.stampFlipV);
                } else {
                    // Normal mode: apply UV density correction
                    m_ctx.selectedObject->stampPreviewAt(hit.uv, hit.triangleIndex, m_ctx.stampData.data(),
                                                          m_ctx.stampWidth, m_ctx.stampHeight,
                                                          m_ctx.stampScale * m_ctx.stampScaleH, m_ctx.stampScale * m_ctx.stampScaleV,
                                                          m_ctx.stampRotation, m_ctx.stampOpacity, m_ctx.stampFlipH, m_ctx.stampFlipV);
                }

                // Upload modified texture to GPU for preview
                uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                auto& texData = m_ctx.selectedObject->getTextureData();
                int w = m_ctx.selectedObject->getTextureWidth();
                int h = m_ctx.selectedObject->getTextureHeight();
                m_ctx.modelRenderer.updateTexture(handle, texData.data(), w, h);
                m_ctx.selectedObject->clearTextureModified();
            } else {
                // Not hovering over model - clear preview
                if (m_ctx.selectedObject->hasStampPreview()) {
                    m_ctx.selectedObject->clearStampPreview();
                    // Upload restored texture
                    uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                    auto& texData = m_ctx.selectedObject->getTextureData();
                    int w = m_ctx.selectedObject->getTextureWidth();
                    int h = m_ctx.selectedObject->getTextureHeight();
                    m_ctx.modelRenderer.updateTexture(handle, texData.data(), w, h);
                    m_ctx.selectedObject->clearTextureModified();
                }
            }
        } else {
            // Not in stamp mode - clear any existing preview
            if (m_ctx.selectedObject->hasStampPreview()) {
                m_ctx.selectedObject->clearStampPreview();
                // Upload restored texture
                uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                auto& texData = m_ctx.selectedObject->getTextureData();
                int w = m_ctx.selectedObject->getTextureWidth();
                int h = m_ctx.selectedObject->getTextureHeight();
                m_ctx.modelRenderer.updateTexture(handle, texData.data(), w, h);
                m_ctx.selectedObject->clearTextureModified();
            }
        }
    }

    // Check if Alt is held for eyedropper mode
    bool altHeld = Input::isKeyDown(Input::KEY_LEFT_ALT) || Input::isKeyDown(Input::KEY_RIGHT_ALT);
    bool ctrlHeld = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);
    bool eyedropperActive = altHeld && m_ctx.isPainting;
    bool cloneSourceMode = ctrlHeld && m_ctx.isPainting;

    // Draw vertex paint brush cursor
    if (m_vertexPaintMode && m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
        bool mouseOverImGui = ImGui::GetIO().WantCaptureMouse;
        if (!mouseOverImGui) {
            ImVec2 mousePos = ImGui::GetMousePos();

            // Raycast to find hit point
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
            glm::mat4 invModel = glm::inverse(modelMatrix);
            glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

            auto hit = m_ctx.editableMesh.raycastFace(localRayOrigin, localRayDir);

            float screenRadius = 20.0f;  // Default fallback

            if (hit.hit) {
                // Transform hit position to world space for screen projection
                glm::vec3 worldHitPos = glm::vec3(modelMatrix * glm::vec4(hit.position, 1.0f));

                // Get world-space brush radius (apply object scale)
                glm::vec3 scale = m_ctx.selectedObject->getTransform().getScale();
                float avgScale = (scale.x + scale.y + scale.z) / 3.0f;
                float worldRadius = m_vertexPaintRadius * avgScale;

                // Project to screen space
                Camera& cam = m_ctx.getActiveCamera();
                float screenWidth = static_cast<float>(m_ctx.window.getWidth());
                float screenHeight = static_cast<float>(m_ctx.window.getHeight());
                float aspect = screenWidth / screenHeight;
                glm::mat4 viewProj = cam.getProjectionMatrix(aspect) * cam.getViewMatrix();

                glm::vec4 clipCenter = viewProj * glm::vec4(worldHitPos, 1.0f);
                glm::vec4 clipOffset = viewProj * glm::vec4(worldHitPos + cam.getRight() * worldRadius, 1.0f);

                if (clipCenter.w > 0.001f && clipOffset.w > 0.001f) {
                    glm::vec2 screenCenter = glm::vec2(clipCenter.x / clipCenter.w, clipCenter.y / clipCenter.w);
                    glm::vec2 screenOffset = glm::vec2(clipOffset.x / clipOffset.w, clipOffset.y / clipOffset.w);

                    screenCenter = (screenCenter * 0.5f + 0.5f) * glm::vec2(screenWidth, screenHeight);
                    screenOffset = (screenOffset * 0.5f + 0.5f) * glm::vec2(screenWidth, screenHeight);

                    screenRadius = glm::length(screenOffset - screenCenter);
                }
            }

            if (screenRadius < 5.0f) screenRadius = 5.0f;

            // Draw green circle for vertex paint brush
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            ImU32 circleColor = IM_COL32(
                static_cast<int>(m_vertexPaintColor.r * 255),
                static_cast<int>(m_vertexPaintColor.g * 255),
                static_cast<int>(m_vertexPaintColor.b * 255),
                200
            );
            drawList->AddCircle(mousePos, screenRadius, circleColor, 32, 2.0f);
            // Add white outline for visibility
            drawList->AddCircle(mousePos, screenRadius + 1, IM_COL32(255, 255, 255, 150), 32, 1.0f);
        }
    }

    // Draw clone source cursor (Ctrl held in paint mode)
    if (cloneSourceMode) {
        bool mouseOverImGui = ImGui::GetIO().WantCaptureMouse;
        if (!mouseOverImGui) {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImDrawList* drawList = ImGui::GetForegroundDrawList();

            // Draw crosshair with "+" symbol for clone source
            float size = 15.0f;
            ImU32 cyan = IM_COL32(100, 200, 255, 255);
            ImU32 black = IM_COL32(0, 0, 0, 255);

            // Crosshair with outline
            drawList->AddLine(ImVec2(mousePos.x - size, mousePos.y), ImVec2(mousePos.x + size, mousePos.y), black, 3.0f);
            drawList->AddLine(ImVec2(mousePos.x, mousePos.y - size), ImVec2(mousePos.x, mousePos.y + size), black, 3.0f);
            drawList->AddLine(ImVec2(mousePos.x - size, mousePos.y), ImVec2(mousePos.x + size, mousePos.y), cyan, 2.0f);
            drawList->AddLine(ImVec2(mousePos.x, mousePos.y - size), ImVec2(mousePos.x, mousePos.y + size), cyan, 2.0f);

            // Circle around crosshair
            drawList->AddCircle(mousePos, size + 3, black, 16, 3.0f);
            drawList->AddCircle(mousePos, size + 3, cyan, 16, 1.5f);
        }
    }

    // Draw eyedropper cursor indicator (Alt held in paint mode)
    if (eyedropperActive) {
        bool mouseOverImGui = ImGui::GetIO().WantCaptureMouse;
        if (!mouseOverImGui) {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImDrawList* drawList = ImGui::GetForegroundDrawList();

            // Draw eyedropper icon (a small crosshair with color preview)
            float size = 12.0f;
            ImU32 white = IM_COL32(255, 255, 255, 255);
            ImU32 black = IM_COL32(0, 0, 0, 255);

            // Crosshair with outline
            drawList->AddLine(ImVec2(mousePos.x - size, mousePos.y), ImVec2(mousePos.x + size, mousePos.y), black, 3.0f);
            drawList->AddLine(ImVec2(mousePos.x, mousePos.y - size), ImVec2(mousePos.x, mousePos.y + size), black, 3.0f);
            drawList->AddLine(ImVec2(mousePos.x - size, mousePos.y), ImVec2(mousePos.x + size, mousePos.y), white, 1.0f);
            drawList->AddLine(ImVec2(mousePos.x, mousePos.y - size), ImVec2(mousePos.x, mousePos.y + size), white, 1.0f);

            // Show current color preview
            ImU32 previewColor = IM_COL32(
                static_cast<int>(m_ctx.paintColor.r * 255),
                static_cast<int>(m_ctx.paintColor.g * 255),
                static_cast<int>(m_ctx.paintColor.b * 255),
                255
            );
            drawList->AddRectFilled(
                ImVec2(mousePos.x + size + 4, mousePos.y - size),
                ImVec2(mousePos.x + size + 24, mousePos.y + size),
                previewColor
            );
            drawList->AddRect(
                ImVec2(mousePos.x + size + 4, mousePos.y - size),
                ImVec2(mousePos.x + size + 24, mousePos.y + size),
                white, 0.0f, 0, 1.0f
            );
        }
    }
}

void ModelingMode::renderSceneOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    // Render grid if enabled
    if (m_ctx.showGrid) {
        renderGrid3D(cmd, viewProj);
    }

    // Debug: check what objects we're rendering
    static int renderDebugCounter = 0;
    if (++renderDebugCounter % 300 == 1) {
        std::cout << "[Render] sceneObjects count: " << m_ctx.sceneObjects.size() << std::endl;
        for (size_t i = 0; i < m_ctx.sceneObjects.size(); i++) {
            auto& obj = m_ctx.sceneObjects[i];
            glm::mat4 m = obj->getTransform().getMatrix();
            glm::vec3 pos(m[3]);
            std::cout << "[Render] obj[" << i << "] name=" << obj->getName()
                      << " pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
                      << " selected=" << (obj.get() == m_ctx.selectedObject) << std::endl;
        }
    }

    // Render all scene objects
    for (auto& obj : m_ctx.sceneObjects) {
        if (!obj->isVisible()) continue;
        glm::mat4 modelMatrix = obj->getTransform().getMatrix();
        // X-ray mode disables backface culling for this object
        bool twoSided = obj->isXRay();
        m_ctx.modelRenderer.render(cmd, viewProj, obj->getBufferHandle(), modelMatrix, 0.0f, 1.0f, 1.0f, twoSided);
    }

    // Render modeling overlay (selection highlighting)
    renderModelingOverlay(cmd, viewProj);

    // Render wireframe with depth testing
    if (m_ctx.showModelingWireframe) {
        renderWireframeOverlay3D(cmd, viewProj);
    }

    // Render gizmo
    renderGizmo(cmd, viewProj);

    // Render port gizmos (3D arrows for connection ports)
    renderPortGizmos3D(cmd, viewProj);

    // Render origin crosshair for each object
    if (m_showOrigin) {
        float originSize = 0.15f;
        for (auto& obj : m_ctx.sceneObjects) {
            if (!obj->isVisible()) continue;
            glm::vec3 pos = obj->getTransform().getPosition();
            std::vector<glm::vec3> xLines = { pos - glm::vec3(originSize, 0, 0), pos + glm::vec3(originSize, 0, 0) };
            std::vector<glm::vec3> yLines = { pos - glm::vec3(0, originSize, 0), pos + glm::vec3(0, originSize, 0) };
            std::vector<glm::vec3> zLines = { pos - glm::vec3(0, 0, originSize), pos + glm::vec3(0, 0, originSize) };
            m_ctx.modelRenderer.renderLines(cmd, viewProj, xLines, glm::vec3(1.0f, 0.3f, 0.3f));
            m_ctx.modelRenderer.renderLines(cmd, viewProj, yLines, glm::vec3(0.3f, 1.0f, 0.3f));
            m_ctx.modelRenderer.renderLines(cmd, viewProj, zLines, glm::vec3(0.3f, 0.3f, 1.0f));
        }
    }

    // Render snap source face highlight (red)
    if (m_snapMode && m_snapSourceObject && m_snapSourceFace >= 0) {
        if (m_snapSourceObject->hasEditableMeshData()) {
            const auto& heVerts = m_snapSourceObject->getHEVertices();
            const auto& heEdges = m_snapSourceObject->getHEHalfEdges();
            const auto& heFaces = m_snapSourceObject->getHEFaces();

            if (m_snapSourceFace < static_cast<int>(heFaces.size())) {
                // Collect face vertices by walking half-edges
                std::vector<uint32_t> faceVertIndices;
                uint32_t startHE = heFaces[m_snapSourceFace].halfEdgeIndex;
                uint32_t currHE = startHE;
                do {
                    faceVertIndices.push_back(heEdges[currHE].vertexIndex);
                    currHE = heEdges[currHE].nextIndex;
                } while (currHE != startHE && faceVertIndices.size() < 10);

                glm::mat4 srcModelMatrix = m_snapSourceObject->getTransform().getMatrix();

                // Draw face edges in red
                std::vector<glm::vec3> faceEdges;
                for (size_t i = 0; i < faceVertIndices.size(); ++i) {
                    uint32_t vi0 = faceVertIndices[i];
                    uint32_t vi1 = faceVertIndices[(i + 1) % faceVertIndices.size()];
                    glm::vec3 p0 = glm::vec3(srcModelMatrix * glm::vec4(heVerts[vi0].position, 1.0f));
                    glm::vec3 p1 = glm::vec3(srcModelMatrix * glm::vec4(heVerts[vi1].position, 1.0f));
                    faceEdges.push_back(p0);
                    faceEdges.push_back(p1);
                }
                if (!faceEdges.empty()) {
                    m_ctx.modelRenderer.renderLines(cmd, viewProj, faceEdges, glm::vec3(1.0f, 0.0f, 0.0f));
                }
            }
        }
    }

    // Path tube 3D preview (spline + wireframe tube)
    if (m_pathTubeMode) {
        renderPathTubePreview3D(cmd, viewProj);
    }

    // Slice plane 3D overlay
    if (m_sliceMode) {
        drawSlicePlaneOverlay3D(cmd, viewProj);
    }
}

void ModelingMode::drawOverlays(float vpX, float vpY, float vpW, float vpH) {
    // Wireframe and vertices are now rendered with Vulkan (depth-tested) in renderSceneOverlay
    // ImGui overlays are only used for face normals and reference images now

    // Determine which camera to use based on viewport
    // In split view, right viewport (vpX > 0) uses camera2 (ortho), left uses camera (perspective)
    Camera& activeCamera = (m_ctx.splitView && vpX > 0) ? m_ctx.camera2 : m_ctx.camera;

    // Draw face normals
    if (m_ctx.showFaceNormals) {
        drawFaceNormalsOverlay(activeCamera, vpX, vpY, vpW, vpH);
    }

    // Draw reference images for ortho views
    drawReferenceImages(activeCamera, vpX, vpY, vpW, vpH);

    // Draw rectangle selection overlay
    if (m_ctx.isRectSelecting) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImU32 fillColor = IM_COL32(100, 150, 255, 50);
        ImU32 borderColor = IM_COL32(100, 150, 255, 200);

        float minX = std::min(m_ctx.rectSelectStart.x, m_ctx.rectSelectEnd.x);
        float maxX = std::max(m_ctx.rectSelectStart.x, m_ctx.rectSelectEnd.x);
        float minY = std::min(m_ctx.rectSelectStart.y, m_ctx.rectSelectEnd.y);
        float maxY = std::max(m_ctx.rectSelectStart.y, m_ctx.rectSelectEnd.y);

        drawList->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY), fillColor);
        drawList->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY), borderColor, 0.0f, 0, 2.0f);
    }

    // Draw paint select brush cursor
    if (m_ctx.selectionTool == SelectionTool::Paint && !m_ctx.isPainting) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImU32 brushColor = IM_COL32(255, 200, 100, 150);
        glm::vec2 mousePos = Input::getMousePosition();
        drawList->AddCircle(ImVec2(mousePos.x, mousePos.y), m_ctx.paintSelectRadius, brushColor, 32, 2.0f);
    }

    // Draw face direction arrow (when a single face is selected in face mode)
    if (!m_retopologyMode && m_ctx.modelingSelectionMode == eden::ModelingSelectionMode::Face &&
        m_ctx.editableMesh.isValid() && m_ctx.selectedObject) {
        auto selFaces = m_ctx.editableMesh.getSelectedFaces();
        int selectedFaceIdx = (selFaces.size() == 1) ? static_cast<int>(selFaces[0]) : -1;
        if (selectedFaceIdx >= 0 && selectedFaceIdx < static_cast<int>(m_ctx.editableMesh.getFaceCount())) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

        glm::mat4 view = activeCamera.getViewMatrix();
        float ar = vpW / vpH;
        glm::mat4 proj = activeCamera.getProjectionMatrix(ar);
        glm::mat4 vp = proj * view;
        glm::mat4 modelMat = m_ctx.selectedObject->getTransform().getMatrix();

        auto w2s = [&](const glm::vec3& wp) -> ImVec2 {
            glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
            if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
        };

        auto faceVertIds = m_ctx.editableMesh.getFaceVertices(static_cast<uint32_t>(selectedFaceIdx));
        std::vector<glm::vec3> fverts;
        for (uint32_t vi : faceVertIds) {
            fverts.push_back(glm::vec3(modelMat * glm::vec4(m_ctx.editableMesh.getVertex(vi).position, 1.0f)));
        }

        if (fverts.size() >= 3) {
            int numV = static_cast<int>(fverts.size());
            int dirEdge = m_faceDirectionEdge % numV;
            int dirEdge1 = (dirEdge + 1) % numV;
            ImU32 edgeColor = IM_COL32(255, 255, 100, 200);
            ImU32 greenColor = IM_COL32(0, 255, 0, 255);

            // Draw face edges
            for (int i = 0; i < numV; i++) {
                int j = (i + 1) % numV;
                ImVec2 a = w2s(fverts[i]);
                ImVec2 b = w2s(fverts[j]);
                if (a.x > -500 && b.x > -500) {
                    drawList->AddLine(a, b, (i == dirEdge) ? greenColor : edgeColor,
                                      (i == dirEdge) ? 4.0f : 2.0f);
                }
            }

            // Draw arrow showing extrude direction
            glm::vec3 edgeMid = (fverts[dirEdge] + fverts[dirEdge1]) * 0.5f;
            glm::vec3 fc(0); for (const auto& v : fverts) fc += v;
            fc /= static_cast<float>(numV);

            glm::vec3 fn = m_ctx.editableMesh.getFaceNormal(static_cast<uint32_t>(selectedFaceIdx));
            fn = glm::normalize(glm::vec3(modelMat * glm::vec4(fn, 0.0f)));
            glm::vec3 edgeDir = glm::normalize(fverts[dirEdge1] - fverts[dirEdge]);
            glm::vec3 extDir = glm::normalize(glm::cross(edgeDir, fn));
            if (glm::dot(extDir, edgeMid - fc) < 0.0f) extDir = -extDir;

            float arrowLen = glm::length(fverts[dirEdge1] - fverts[dirEdge]) * 0.4f;
            glm::vec3 arrowTip = edgeMid + extDir * arrowLen;
            glm::vec3 arrowLeft = arrowTip - extDir * arrowLen * 0.3f + edgeDir * arrowLen * 0.15f;
            glm::vec3 arrowRight = arrowTip - extDir * arrowLen * 0.3f - edgeDir * arrowLen * 0.15f;

            ImVec2 tipS = w2s(arrowTip);
            ImVec2 midS = w2s(edgeMid);
            ImVec2 leftS = w2s(arrowLeft);
            ImVec2 rightS = w2s(arrowRight);
            if (tipS.x > -500 && midS.x > -500) {
                drawList->AddLine(midS, tipS, greenColor, 3.0f);
                drawList->AddLine(tipS, leftS, greenColor, 3.0f);
                drawList->AddLine(tipS, rightS, greenColor, 3.0f);
            }
        }

        drawList->PopClipRect();
        }
    }

    // Draw selection outline for all selected objects (in object mode)
    if (m_ctx.objectMode && !m_ctx.selectedObjects.empty()) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

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

        // Selection outline disabled — user prefers clean viewport

        drawList->PopClipRect();
    }

    // Draw control point diamonds on ALL objects that have CPs
    if (m_showControlPoints) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

        glm::mat4 view = activeCamera.getViewMatrix();
        float aspectRatio = vpW / vpH;
        glm::mat4 proj = activeCamera.getProjectionMatrix(aspectRatio);
        glm::mat4 vp = proj * view;

        ImU32 cpColor = IM_COL32(255, 0, 255, 255);
        ImU32 cpOutline = IM_COL32(0, 0, 0, 220);
        ImU32 cpTextColor = IM_COL32(255, 200, 255, 255);
        ImU32 cpTextBg = IM_COL32(0, 0, 0, 180);
        float cpSize = 8.0f;

        for (auto& obj : m_ctx.sceneObjects) {
            if (!obj->hasControlPoints() || !obj->isVisible()) continue;
            if (!obj->hasEditableMeshData()) continue;

            const auto& cps = obj->getControlPoints();
            const auto& heVerts = obj->getHEVertices();
            glm::mat4 modelMatrix = obj->getTransform().getMatrix();

            for (const auto& cp : cps) {
                if (cp.vertexIndex >= heVerts.size()) continue;
                glm::vec3 localPos = heVerts[cp.vertexIndex].position;
                glm::vec4 worldPos = modelMatrix * glm::vec4(localPos, 1.0f);
                glm::vec4 clip = vp * worldPos;
                if (clip.w <= 0.0f) continue;
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                ImVec2 sp(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);

                // Diamond shape
                ImVec2 top(sp.x, sp.y - cpSize);
                ImVec2 right(sp.x + cpSize, sp.y);
                ImVec2 bottom(sp.x, sp.y + cpSize);
                ImVec2 left(sp.x - cpSize, sp.y);
                drawList->AddQuadFilled(top, right, bottom, left, cpColor);
                drawList->AddQuad(top, right, bottom, left, cpOutline, 2.0f);

                // Name label
                if (!cp.name.empty()) {
                    ImVec2 textSize = ImGui::CalcTextSize(cp.name.c_str());
                    ImVec2 textPos(sp.x - textSize.x * 0.5f, sp.y - cpSize - textSize.y - 2);
                    drawList->AddRectFilled(
                        ImVec2(textPos.x - 2, textPos.y - 1),
                        ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1),
                        cpTextBg, 2.0f);
                    drawList->AddText(textPos, cpTextColor, cp.name.c_str());
                }
            }
        }

        drawList->PopClipRect();
    }

    // Draw port name labels
    renderPortOverlay(vpX, vpY, vpW, vpH);

    // Draw snap vertex mode overlay (numbered vertices)
    if (m_snapVertexMode) {
        drawSnapVertexOverlay(vpX, vpY, vpW, vpH);
    }

    // Draw patch blanket selection rectangle
    if (m_patchBlanketMode && m_patchBlanketDragging) {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        glm::vec2 rMin = glm::min(m_patchBlanketStart, m_patchBlanketEnd);
        glm::vec2 rMax = glm::max(m_patchBlanketStart, m_patchBlanketEnd);
        ImU32 rectColor = IM_COL32(255, 160, 50, 200);
        ImU32 fillColor = IM_COL32(255, 160, 50, 30);
        drawList->AddRectFilled(ImVec2(rMin.x, rMin.y), ImVec2(rMax.x, rMax.y), fillColor);
        drawList->AddRect(ImVec2(rMin.x, rMin.y), ImVec2(rMax.x, rMax.y), rectColor, 0.0f, 0, 2.0f);
    }

    // Draw retopology overlay (numbered vertices, quad wireframes, existing vert dots)
    if (m_retopologyMode && (!m_retopologyVerts.empty() || !m_retopologyQuads.empty())) {
        drawRetopologyOverlay(vpX, vpY, vpW, vpH);
    }

    // Draw path tube overlay (node circles, connecting lines)
    if (m_pathTubeMode && !m_pathNodes.empty()) {
        drawPathTubeOverlay(vpX, vpY, vpW, vpH);
    }

    // Draw skeleton overlay (bone lines + joint markers)
    if (m_riggingMode && m_showSkeleton) {
        drawSkeletonOverlay(vpX, vpY, vpW, vpH);
    }
}

void ModelingMode::renderModelingEditorUI() {
    // Scene window
    ImGui::SetNextWindowPos(ImVec2(0, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250, 350), ImGuiCond_FirstUseEver);

    if (m_ctx.showSceneWindow) {
    if (ImGui::Begin("Scene", &m_ctx.showSceneWindow)) {
        // Object list
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Objects");
        ImGui::Separator();

        static int lastClickedIndex = -1;  // For shift-click range selection

        for (size_t i = 0; i < m_ctx.sceneObjects.size(); ++i) {
            auto& obj = m_ctx.sceneObjects[i];
            ImGui::PushID(static_cast<int>(i));

            // Check if in multi-selection set or is primary selection
            bool isInMultiSelect = m_ctx.selectedObjects.count(obj.get()) > 0;
            bool isPrimary = (obj.get() == m_ctx.selectedObject);
            bool isSelected = isInMultiSelect || isPrimary;

            // Visibility toggle
            bool visible = obj->isVisible();
            if (ImGui::Checkbox("##vis", &visible)) {
                obj->setVisible(visible);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visible");
            ImGui::SameLine();

            // X-Ray toggle
            bool xray = obj->isXRay();
            if (ImGui::Checkbox("##xray", &xray)) {
                obj->setXRay(xray);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("X-Ray");
            ImGui::SameLine();

            // Object name (selectable)
            if (m_ctx.renamingObjectIndex == static_cast<int>(i)) {
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", m_ctx.renameBuffer, m_ctx.renameBufferSize,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    obj->setName(m_ctx.renameBuffer);
                    m_ctx.renamingObjectIndex = -1;
                }
                if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
                    m_ctx.renamingObjectIndex = -1;
                }
            } else {
                if (ImGui::Selectable(obj->getName().c_str(), isSelected)) {
                    bool ctrlHeld = ImGui::GetIO().KeyCtrl;
                    bool shiftHeld = ImGui::GetIO().KeyShift;

                    if (ctrlHeld) {
                        // Ctrl+Click: Toggle individual selection
                        if (isInMultiSelect) {
                            m_ctx.selectedObjects.erase(obj.get());
                        } else {
                            m_ctx.selectedObjects.insert(obj.get());
                        }
                        // Update primary selection
                        if (m_ctx.selectedObject != obj.get()) {
                            m_ctx.selectedObject = obj.get();
                            buildEditableMeshFromObject();
                        }
                        lastClickedIndex = static_cast<int>(i);
                    } else if (shiftHeld && lastClickedIndex >= 0) {
                        // Shift+Click: Range selection
                        int start = std::min(lastClickedIndex, static_cast<int>(i));
                        int end = std::max(lastClickedIndex, static_cast<int>(i));
                        for (int j = start; j <= end; ++j) {
                            m_ctx.selectedObjects.insert(m_ctx.sceneObjects[j].get());
                        }
                        // Update primary selection
                        if (m_ctx.selectedObject != obj.get()) {
                            m_ctx.selectedObject = obj.get();
                            buildEditableMeshFromObject();
                        }
                    } else {
                        // Normal click: Select single, clear others
                        m_ctx.selectedObjects.clear();
                        m_ctx.selectedObjects.insert(obj.get());
                        if (m_ctx.selectedObject != obj.get()) {
                            m_ctx.selectedObject = obj.get();
                            buildEditableMeshFromObject();
                        }
                        lastClickedIndex = static_cast<int>(i);
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    m_ctx.renamingObjectIndex = static_cast<int>(i);
                    strncpy(m_ctx.renameBuffer, obj->getName().c_str(), m_ctx.renameBufferSize - 1);
                }
            }

            // Show control points for this object
            if (obj->hasControlPoints()) {
                const auto& objCPs = obj->getControlPoints();
                for (size_t ci = 0; ci < objCPs.size(); ++ci) {
                    ImGui::PushID(static_cast<int>(1000 + i * 100 + ci));
                    ImGui::Indent(20.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), "%s", objCPs[ci].name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(v%u)", objCPs[ci].vertexIndex);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        if (isSelected) {
                            // Selected object: remove from editableMesh and sync
                            m_ctx.editableMesh.removeControlPoint(objCPs[ci].vertexIndex);
                            std::vector<SceneObject::StoredControlPoint> updatedCPs;
                            for (const auto& cp : m_ctx.editableMesh.getControlPoints()) {
                                updatedCPs.push_back({cp.vertexIndex, cp.name});
                            }
                            obj->setControlPoints(updatedCPs);
                        } else {
                            // Non-selected object: remove directly from SceneObject
                            auto cps = obj->getControlPoints();
                            cps.erase(cps.begin() + ci);
                            obj->setControlPoints(cps);
                        }
                        ImGui::Unindent(20.0f);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::Unindent(20.0f);
                    ImGui::PopID();
                }
            }

            ImGui::PopID();
        }

        if (m_ctx.sceneObjects.empty()) {
            ImGui::TextDisabled("No objects in scene");
        }

        ImGui::Spacing();

        // Object mode toggle and object operations
        ImGui::Checkbox("Object Mode", &m_ctx.objectMode);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled, gizmo moves entire object instead of components");
        }

        // Mesh stats
        if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
            int quads = 0, tris = 0, ngons = 0;
            for (uint32_t fi = 0; fi < m_ctx.editableMesh.getFaceCount(); fi++) {
                uint32_t vc = m_ctx.editableMesh.getFace(fi).vertexCount;
                if (vc == 4) quads++;
                else if (vc == 3) tris++;
                else ngons++;
            }
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                "V:%zu  Q:%d  T:%d  F:%zu",
                m_ctx.editableMesh.getVertexCount(), quads, tris,
                m_ctx.editableMesh.getFaceCount());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Vertices: %zu\nQuads: %d\nTriangles: %d\nTotal faces: %zu",
                    m_ctx.editableMesh.getVertexCount(), quads, tris,
                    m_ctx.editableMesh.getFaceCount());
            }
        }

        if (m_ctx.selectedObject) {
            ImGui::SameLine();
            if (ImGui::Button("Duplicate")) {
                duplicateSelectedObject();
            }

            // Mirror buttons - duplicate, flip geometry, and move to mirrored world position
            ImGui::SameLine();
            if (ImGui::Button("Mirror X")) {
                if (m_ctx.meshDirty) {
                    updateMeshFromEditable();
                }

                auto& srcObj = m_ctx.selectedObject;
                auto newObj = std::make_unique<SceneObject>(srcObj->getName() + "_mirrorX");

                // Copy and flip mesh data on X axis
                auto srcVerts = srcObj->getVertices();
                const auto& srcIndices = srcObj->getIndices();

                // Flip X coordinate of all vertices
                for (auto& v : srcVerts) {
                    v.position.x = -v.position.x;
                    v.normal.x = -v.normal.x;
                }

                // Reverse triangle winding order (mirroring inverts winding)
                auto newIndices = srcIndices;
                for (size_t i = 0; i + 2 < newIndices.size(); i += 3) {
                    std::swap(newIndices[i], newIndices[i + 2]);
                }

                if (!srcVerts.empty() && !newIndices.empty()) {
                    uint32_t handle;
                    if (srcObj->hasTextureData()) {
                        const auto& texData = srcObj->getTextureData();
                        int texW = srcObj->getTextureWidth();
                        int texH = srcObj->getTextureHeight();
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, texData.data(), texW, texH);
                        newObj->setTextureData(texData, texW, texH);
                    } else {
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, nullptr, 0, 0);
                    }
                    newObj->setBufferHandle(handle);
                    newObj->setIndexCount(static_cast<uint32_t>(newIndices.size()));
                    newObj->setVertexCount(static_cast<uint32_t>(srcVerts.size()));
                    newObj->setMeshData(srcVerts, newIndices);

                    // Copy and flip EditableMesh data
                    if (srcObj->hasEditableMeshData()) {
                        auto heVerts = srcObj->getHEVertices();
                        for (auto& v : heVerts) {
                            v.position.x = -v.position.x;
                        }
                        newObj->setEditableMeshData(heVerts, srcObj->getHEHalfEdges(), srcObj->getHEFaces());
                    }

                    // Mirror position around world origin
                    auto& srcTransform = srcObj->getTransform();
                    auto& transform = newObj->getTransform();
                    transform.setScale(srcTransform.getScale());
                    transform.setRotation(srcTransform.getRotation());
                    glm::vec3 pos = srcTransform.getPosition();
                    pos.x = -pos.x;
                    transform.setPosition(pos);
                }
                m_ctx.sceneObjects.push_back(std::move(newObj));
            }

            ImGui::SameLine();
            if (ImGui::Button("Mirror Y")) {
                if (m_ctx.meshDirty) {
                    updateMeshFromEditable();
                }

                auto& srcObj = m_ctx.selectedObject;
                auto newObj = std::make_unique<SceneObject>(srcObj->getName() + "_mirrorY");

                auto srcVerts = srcObj->getVertices();
                const auto& srcIndices = srcObj->getIndices();

                for (auto& v : srcVerts) {
                    v.position.y = -v.position.y;
                    v.normal.y = -v.normal.y;
                }

                // Reverse triangle winding order (mirroring inverts winding)
                auto newIndices = srcIndices;
                for (size_t i = 0; i + 2 < newIndices.size(); i += 3) {
                    std::swap(newIndices[i], newIndices[i + 2]);
                }

                if (!srcVerts.empty() && !newIndices.empty()) {
                    uint32_t handle;
                    if (srcObj->hasTextureData()) {
                        const auto& texData = srcObj->getTextureData();
                        int texW = srcObj->getTextureWidth();
                        int texH = srcObj->getTextureHeight();
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, texData.data(), texW, texH);
                        newObj->setTextureData(texData, texW, texH);
                    } else {
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, nullptr, 0, 0);
                    }
                    newObj->setBufferHandle(handle);
                    newObj->setIndexCount(static_cast<uint32_t>(newIndices.size()));
                    newObj->setVertexCount(static_cast<uint32_t>(srcVerts.size()));
                    newObj->setMeshData(srcVerts, newIndices);

                    if (srcObj->hasEditableMeshData()) {
                        auto heVerts = srcObj->getHEVertices();
                        for (auto& v : heVerts) {
                            v.position.y = -v.position.y;
                        }
                        newObj->setEditableMeshData(heVerts, srcObj->getHEHalfEdges(), srcObj->getHEFaces());
                    }

                    auto& srcTransform = srcObj->getTransform();
                    auto& transform = newObj->getTransform();
                    transform.setScale(srcTransform.getScale());
                    transform.setRotation(srcTransform.getRotation());
                    glm::vec3 pos = srcTransform.getPosition();
                    pos.y = -pos.y;
                    transform.setPosition(pos);
                }
                m_ctx.sceneObjects.push_back(std::move(newObj));
            }

            ImGui::SameLine();
            if (ImGui::Button("Mirror Z")) {
                if (m_ctx.meshDirty) {
                    updateMeshFromEditable();
                }

                auto& srcObj = m_ctx.selectedObject;
                auto newObj = std::make_unique<SceneObject>(srcObj->getName() + "_mirrorZ");

                auto srcVerts = srcObj->getVertices();
                const auto& srcIndices = srcObj->getIndices();

                for (auto& v : srcVerts) {
                    v.position.z = -v.position.z;
                    v.normal.z = -v.normal.z;
                }

                // Reverse triangle winding order (mirroring inverts winding)
                auto newIndices = srcIndices;
                for (size_t i = 0; i + 2 < newIndices.size(); i += 3) {
                    std::swap(newIndices[i], newIndices[i + 2]);
                }

                if (!srcVerts.empty() && !newIndices.empty()) {
                    uint32_t handle;
                    if (srcObj->hasTextureData()) {
                        const auto& texData = srcObj->getTextureData();
                        int texW = srcObj->getTextureWidth();
                        int texH = srcObj->getTextureHeight();
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, texData.data(), texW, texH);
                        newObj->setTextureData(texData, texW, texH);
                    } else {
                        handle = m_ctx.modelRenderer.createModel(srcVerts, newIndices, nullptr, 0, 0);
                    }
                    newObj->setBufferHandle(handle);
                    newObj->setIndexCount(static_cast<uint32_t>(newIndices.size()));
                    newObj->setVertexCount(static_cast<uint32_t>(srcVerts.size()));
                    newObj->setMeshData(srcVerts, newIndices);

                    if (srcObj->hasEditableMeshData()) {
                        auto heVerts = srcObj->getHEVertices();
                        for (auto& v : heVerts) {
                            v.position.z = -v.position.z;
                        }
                        newObj->setEditableMeshData(heVerts, srcObj->getHEHalfEdges(), srcObj->getHEFaces());
                    }

                    auto& srcTransform = srcObj->getTransform();
                    auto& transform = newObj->getTransform();
                    transform.setScale(srcTransform.getScale());
                    transform.setRotation(srcTransform.getRotation());
                    glm::vec3 pos = srcTransform.getPosition();
                    pos.z = -pos.z;
                    transform.setPosition(pos);
                }
                m_ctx.sceneObjects.push_back(std::move(newObj));
            }

            if (ImGui::Button("Center on Origin")) {
                if (m_ctx.editableMesh.isValid() && m_ctx.editableMesh.getVertexCount() > 0) {

                    // Calculate centroid of all vertices in local space
                    glm::vec3 centroid(0.0f);
                    size_t vertCount = m_ctx.editableMesh.getVertexCount();
                    for (size_t vi = 0; vi < vertCount; ++vi) {
                        centroid += m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi)).position;
                    }
                    centroid /= static_cast<float>(vertCount);

                    // Shift all vertices so centroid becomes origin
                    for (size_t vi = 0; vi < vertCount; ++vi) {
                        m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi)).position -= centroid;
                    }

                    // Shift port positions too (they're in local space)
                    auto& ports = const_cast<std::vector<EditableMesh::Port>&>(m_ctx.editableMesh.getPorts());
                    for (auto& p : ports) {
                        p.position -= centroid;
                    }

                    // Compensate transform so object stays in same world position
                    auto& transform = m_ctx.selectedObject->getTransform();
                    glm::mat4 modelMat = transform.getMatrix();
                    glm::vec3 worldCentroid = glm::vec3(modelMat * glm::vec4(centroid, 0.0f));
                    transform.setPosition(transform.getPosition() + worldCentroid);

                    // Defer GPU sync to next frame (avoids Vulkan deadlock)
                    syncPortsToSceneObject();
                    m_ctx.meshDirty = true;
                    invalidateWireframeCache();

                    std::cout << "[Center] Recentered mesh by (" << centroid.x << "," << centroid.y << "," << centroid.z << ")" << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move mesh vertices so the centroid is at the origin.\nAdjusts transform to keep world position unchanged.\nFixes hotbar/physics issues from off-center models.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Origin")) {
                if (m_ctx.editableMesh.isValid() && m_ctx.editableMesh.getVertexCount() > 0) {
                    m_ctx.editableMesh.saveState();

                    // Bake the current transform into vertex positions, then zero the transform
                    auto& transform = m_ctx.selectedObject->getTransform();
                    glm::mat4 modelMat = transform.getMatrix();
                    glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(modelMat)));

                    // Transform all vertices from local to world, then recenter on origin
                    size_t vertCount = m_ctx.editableMesh.getVertexCount();
                    std::cout << "[Reset Origin] Processing " << vertCount << " vertices..." << std::endl;
                    glm::vec3 centroid(0.0f);
                    for (size_t vi = 0; vi < vertCount; ++vi) {
                        auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                        v.position = glm::vec3(modelMat * glm::vec4(v.position, 1.0f));
                        glm::vec3 transformedNormal = normalMat * v.normal;
                        float len = glm::length(transformedNormal);
                        v.normal = (len > 0.0001f) ? transformedNormal / len : glm::vec3(0, 1, 0);
                        centroid += v.position;
                    }
                    std::cout << "[Reset Origin] Vertices transformed, updating mesh..." << std::endl;
                    centroid /= static_cast<float>(vertCount);

                    // Center on world origin
                    for (size_t vi = 0; vi < vertCount; ++vi) {
                        m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi)).position -= centroid;
                    }

                    // Transform ports: bake transform then recenter
                    auto& ports = const_cast<std::vector<EditableMesh::Port>&>(m_ctx.editableMesh.getPorts());
                    for (auto& p : ports) {
                        p.position = glm::vec3(modelMat * glm::vec4(p.position, 1.0f)) - centroid;
                        p.forward = glm::normalize(glm::vec3(modelMat * glm::vec4(p.forward, 0.0f)));
                        p.up = glm::normalize(glm::vec3(modelMat * glm::vec4(p.up, 0.0f)));
                    }

                    // Zero all transforms
                    transform.setPosition(glm::vec3(0.0f));
                    transform.setRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                    transform.setScale(glm::vec3(1.0f));

                    syncPortsToSceneObject();
                    m_ctx.meshDirty = true;
                    invalidateWireframeCache();

                    std::cout << "[Reset Origin] Model centered at world origin, transforms zeroed" << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Bake all transforms into vertices, center model\nat world origin, and reset position/rotation/scale to zero.\nThe model will sit exactly at the LIME origin.");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Origin", &m_showOrigin);

            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                // Queue object for deferred deletion (processed at start of next frame)
                m_ctx.pendingDeletions.push_back(m_ctx.selectedObject);

                // Reset gizmo state
                m_ctx.gizmoDragging = false;
                m_ctx.gizmoActiveAxis = GizmoAxis::None;
                m_ctx.gizmoHoveredAxis = GizmoAxis::None;

                // Clear selection and mesh state immediately
                m_ctx.selectedObject = nullptr;
                m_ctx.selectedObjects.clear();
                m_ctx.editableMesh.clear();
                m_ctx.meshDirty = false;
            }

            // Control point creation (requires vertex mode with 1 vertex selected)
            auto cpSelectedVerts = m_ctx.editableMesh.getSelectedVertices();
            static char cpNameBuf[64] = "";
            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##cpname", "CP name...", cpNameBuf, sizeof(cpNameBuf));
            ImGui::SameLine();
            bool cpDisabled = (cpSelectedVerts.size() != 1 || strlen(cpNameBuf) == 0);
            if (cpDisabled) ImGui::BeginDisabled();
            if (ImGui::Button("Create CP")) {
                if (cpSelectedVerts.size() == 1 && strlen(cpNameBuf) > 0) {
                    uint32_t vi = *cpSelectedVerts.begin();
                    if (m_ctx.editableMesh.isControlPoint(vi)) {
                        std::string existingName = m_ctx.editableMesh.getControlPointName(vi);
                        std::cout << "[CP] Vertex " << vi << " already has CP '" << existingName
                                  << "' — select a different vertex for '" << cpNameBuf << "'" << std::endl;
                    } else {
                        m_ctx.editableMesh.addControlPoint(vi, std::string(cpNameBuf));
                        // Save CPs to SceneObject immediately
                        std::vector<SceneObject::StoredControlPoint> storedCPs;
                        for (const auto& cp : m_ctx.editableMesh.getControlPoints()) {
                            storedCPs.push_back({cp.vertexIndex, cp.name});
                        }
                        m_ctx.selectedObject->setControlPoints(storedCPs);
                        std::cout << "[CP] Created '" << cpNameBuf << "' at v" << vi
                                  << " on '" << m_ctx.selectedObject->getName()
                                  << "' (total: " << storedCPs.size() << ")" << std::endl;
                        cpNameBuf[0] = '\0';
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Select 1 vertex, type a name, click Create CP.\nParts connect by matching CP names across objects.");
            }
            if (cpDisabled) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox("Show CPs", &m_showControlPoints);
        }

        // Port authoring
        renderPortUI();

        // Metadata editor
        renderMetadataUI();

        // Snap tool - requires at least 2 objects
        if (m_ctx.sceneObjects.size() >= 2) {
            if (m_snapVertexMode) {
                // Vertex selection mode for Snap & Merge
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "SNAP & MERGE - VERTEX MODE");
                ImGui::Separator();

                // Show current state
                if (m_snapSrcObj && m_snapDstObj) {
                    ImGui::Text("Source: %s (%zu verts)", m_snapSrcObj->getName().c_str(), m_snapSrcVerts.size());
                    ImGui::Text("Target: %s (%zu verts)", m_snapDstObj->getName().c_str(), m_snapDstVerts.size());
                } else if (m_snapSrcObj) {
                    ImGui::Text("Source: %s (%zu verts)", m_snapSrcObj->getName().c_str(), m_snapSrcVerts.size());
                    ImGui::Text("Target: (click target object)");
                } else {
                    ImGui::Text("Click vertices on SOURCE object");
                    ImGui::Text("(in order around the face)");
                }

                ImGui::Spacing();
                if (!m_snapSrcVerts.empty() || !m_snapDstVerts.empty()) {
                    ImGui::Text("Source verts: %zu", m_snapSrcVerts.size());
                    ImGui::Text("Target verts: %zu", m_snapDstVerts.size());
                }

                // Confirm button (only if we have matching vertices)
                bool canMerge = !m_snapSrcVerts.empty() &&
                               m_snapSrcVerts.size() == m_snapDstVerts.size() &&
                               m_snapSrcObj && m_snapDstObj;
                if (canMerge) {
                    ImGui::Spacing();
                    if (ImGui::Button("Confirm Merge")) {
                        snapAndMergeWithVertexCorrespondence();
                        cancelSnapVertexMode();
                    }
                }

                ImGui::Spacing();
                if (ImGui::Button("Clear Source Verts")) {
                    m_snapSrcVerts.clear();
                    m_snapSrcVertIndices.clear();
                    m_snapSrcObj = nullptr;
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Target Verts")) {
                    m_snapDstVerts.clear();
                    m_snapDstVertIndices.clear();
                    m_snapDstObj = nullptr;
                }

                if (ImGui::Button("Cancel (ESC)")) {
                    cancelSnapVertexMode();
                }
            } else if (m_snapMode) {
                if (m_snapMergeMode) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "SNAP & MERGE MODE");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "SNAP MODE");
                }
                if (m_snapSourceFace == -1) {
                    ImGui::Text("Select source face...");
                } else {
                    ImGui::Text("Select target face...");
                }
                if (ImGui::Button("Cancel (ESC)")) {
                    cancelSnapMode();
                }
            } else {
                if (ImGui::Button("Snap Faces")) {
                    m_snapMode = true;
                    m_snapMergeMode = false;
                    m_snapSourceObject = nullptr;
                    m_snapSourceFace = -1;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Snap one object's face to another (keeps separate)");
                }
                ImGui::SameLine();
                if (ImGui::Button("Snap & Merge")) {
                    // Enter vertex selection mode
                    m_snapVertexMode = true;
                    m_snapSrcObj = nullptr;
                    m_snapDstObj = nullptr;
                    m_snapSrcVerts.clear();
                    m_snapDstVerts.clear();
                    m_snapSrcVertIndices.clear();
                    m_snapDstVertIndices.clear();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click vertices in order on source, then target object");
                }
            }
        }

        // Retopology tools
        {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.8f, 1.0f), "Retopology");
            ImGui::Separator();

            // Make Live / Unlive toggle
            if (m_retopologyLiveObj) {
                ImGui::Text("Live: %s", m_retopologyLiveObj->getName().c_str());
                if (ImGui::Button("Unlive")) {
                    m_retopologyLiveObj = nullptr;
                    if (m_retopologyMode) {
                        cancelRetopologyMode();
                    }
                }
            } else {
                if (m_ctx.selectedObject) {
                    if (ImGui::Button("Make Live")) {
                        m_retopologyLiveObj = m_ctx.selectedObject;
                        std::cout << "[Retopo] Made '" << m_retopologyLiveObj->getName() << "' live" << std::endl;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Set selected object as retopo reference surface");
                    }
                } else {
                    ImGui::TextDisabled("Select object to make live");
                }
            }

            // Smooth normals for selected object (useful for imported meshes)
            if (m_ctx.selectedObject && m_ctx.selectedObject->hasMeshData()) {
                if (ImGui::Button("Smooth Normals")) {
                    auto& verts = const_cast<std::vector<ModelVertex>&>(m_ctx.selectedObject->getVertices());
                    const auto& indices = m_ctx.selectedObject->getIndices();

                    // Accumulate face normals per vertex position
                    const float posTol = 0.0001f;
                    struct NormalAccum { glm::vec3 pos; glm::vec3 normal; int count; };
                    std::vector<NormalAccum> accum;

                    // First compute face normals and accumulate to vertex positions
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        const glm::vec3& v0 = verts[indices[i]].position;
                        const glm::vec3& v1 = verts[indices[i+1]].position;
                        const glm::vec3& v2 = verts[indices[i+2]].position;
                        glm::vec3 faceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

                        for (int j = 0; j < 3; ++j) {
                            const glm::vec3& pos = verts[indices[i+j]].position;
                            bool found = false;
                            for (auto& a : accum) {
                                if (glm::length(a.pos - pos) < posTol) {
                                    a.normal += faceNormal;
                                    a.count++;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                accum.push_back({pos, faceNormal, 1});
                            }
                        }
                    }

                    // Normalize accumulated normals
                    for (auto& a : accum) {
                        float len = glm::length(a.normal);
                        if (len > 0.0001f) a.normal /= len;
                    }

                    // Apply smooth normals back to vertices
                    for (auto& v : verts) {
                        for (const auto& a : accum) {
                            if (glm::length(v.position - a.pos) < posTol) {
                                v.normal = a.normal;
                                break;
                            }
                        }
                    }

                    // Update GPU buffer and stored mesh data
                    uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                    if (handle != UINT32_MAX) {
                        m_ctx.modelRenderer.updateModelBuffer(handle, verts);
                    }
                    m_ctx.selectedObject->setMeshData(verts, indices);
                    std::cout << "[Retopo] Smooth normals applied (" << accum.size() << " unique positions)" << std::endl;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Recalculate smooth normals (fixes flat shading on imported models)");
                }
            }

            // Place Vertex tool (only available when a live object is set)
            if (m_retopologyLiveObj) {
                if (m_retopologyMode) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "PLACE VERTEX MODE");
                    ImGui::Text("Vertices: %zu / 4", m_retopologyVerts.size());
                    if (!m_retopologyQuads.empty()) {
                        ImGui::Text("Quads: %zu", m_retopologyQuads.size());
                        ImGui::Text("Click existing verts (green) or surface");
                    } else {
                        ImGui::Text("Click on live surface to place");
                    }
                    if (m_retopologyVerts.size() == 4) {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Press ENTER to create quad");
                    }

                    ImGui::Spacing();
                    if (!m_retopologyVerts.empty()) {
                        if (ImGui::Button("Undo Last Vertex")) {
                            m_retopologyVerts.pop_back();
                            m_retopologyNormals.pop_back();
                            if (!m_retopologyVertMeshIdx.empty())
                                m_retopologyVertMeshIdx.pop_back();
                        }
                    }
                    if (ImGui::Button("Clear Vertices")) {
                        m_retopologyVerts.clear();
                        m_retopologyNormals.clear();
                        m_retopologyVertMeshIdx.clear();
                    }

                    // Finalize button — builds the GPU mesh from all accumulated quads
                    if (!m_retopologyQuads.empty()) {
                        ImGui::Spacing();
                        if (ImGui::Button("Finalize Mesh")) {
                            finalizeRetopologyMesh();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Build GPU mesh from all retopo quads");
                        }
                    }

                    if (ImGui::Button("Cancel (ESC)")) {
                        cancelRetopologyMode();
                    }
                } else {
                    if (ImGui::Button("Place Vertex")) {
                        m_retopologyMode = true;
                        m_retopologyVerts.clear();
                        m_retopologyNormals.clear();
                        m_retopologyVertMeshIdx.clear();
                        std::cout << "[Retopo] Place Vertex mode enabled" << std::endl;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Click to place vertices on live surface, Enter to create quad");
                    }
                }

                // Auto-retopology controls
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Auto Retopo");
                ImGui::Separator();
                ImGui::SliderInt("Resolution", &m_autoRetopResolution, 8, 64);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Voxel grid density (higher = more detail, slower)");
                }
                ImGui::SliderInt("Smooth Iterations", &m_autoRetopSmoothIter, 0, 10);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Laplacian smoothing passes to reduce blockiness");
                }
                if (ImGui::Button("Auto Retopo")) {
                    autoRetopology();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Generate all-quad mesh from live surface using voxel remeshing");
                }

                // Quad Blanket retopology controls
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quad Blanket");
                ImGui::Separator();
                ImGui::SliderInt("Grid X", &m_quadBlanketResX, 4, 128);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Horizontal grid resolution");
                }
                ImGui::SliderInt("Grid Y", &m_quadBlanketResY, 4, 128);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Vertical grid resolution");
                }
                ImGui::SliderInt("Smooth Iters##qb", &m_quadBlanketSmoothIter, 0, 10);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Laplacian smoothing passes after projection");
                }
                ImGui::Checkbox("Trim Partial", &m_quadBlanketTrimPartial);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Only keep quads where all 4 vertices hit the surface");
                }
                ImGui::SliderFloat("Padding##qb", &m_quadBlanketPadding, 0.0f, 0.3f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Extra padding around mesh bounds (fraction of extent)");
                }
                if (ImGui::Button("Quad Blanket")) {
                    quadBlanketRetopology();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Project a quad grid from current view onto the live surface");
                }

                // Scatter Points — evenly spaced retopo vertices on surface
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Scatter Points");
                ImGui::Separator();
                ImGui::SliderFloat("Spacing (cm)", &m_scatterSpacing, 1.0f, 50.0f, "%.1f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Distance between points in centimeters.\nSmaller = more points, denser coverage.");
                }
                if (ImGui::Button("Scatter Contour")) {
                    scatterPointsOnSurface();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Horizontal contour rings on the live surface");
                }
                ImGui::SameLine();
                if (ImGui::Button("Scatter Spherical")) {
                    scatterSpherical();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Project a UV sphere inward onto the live surface (balloon shrinkwrap)");
                }

                if (m_scatterRings.empty()) ImGui::BeginDisabled();
                if (ImGui::Button("Connect Rings")) {
                    connectScatterRings();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Horizontal: connect points within each ring loop");
                }
                if (m_scatterRings.empty()) ImGui::EndDisabled();

                if (m_scatterRings.size() < 2) ImGui::BeginDisabled();
                if (ImGui::Button("Connect Vertical")) {
                    connectScatterVertical();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Vertical: connect matching indices between rings + create quads");
                }
                if (m_scatterRings.size() < 2) ImGui::EndDisabled();

                if (m_scatterRings.empty()) ImGui::BeginDisabled();
                if (ImGui::Button("Align Anchors")) {
                    alignScatterAnchors();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Re-align anchor points to axis crossings after sliding rings");
                }
                if (m_scatterRings.empty()) ImGui::EndDisabled();

                if (m_selectedScatterPoints.size() != 4) ImGui::BeginDisabled();
                if (ImGui::Button("Subdivide Quad")) {
                    subdivideSelectedQuad();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (m_selectedScatterPoints.size() != 4)
                        ImGui::SetTooltip("Click 4 points to select them, then subdivide (%zu/4 selected)",
                                          m_selectedScatterPoints.size());
                    else
                        ImGui::SetTooltip("Split the selected quad into 4 sub-quads (9 points)");
                }
                if (m_selectedScatterPoints.size() != 4) ImGui::EndDisabled();

                ImGui::Checkbox("Symmetry X", &m_scatterSymmetryX);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Mirror point moves across X=0.\nPoints on the X=0 line move alone.");
                }

                // Extract tool
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Extract");
                ImGui::Separator();
                const char* extractModes[] = {"Include Partial", "Exclude Partial"};
                int extractIdx = static_cast<int>(m_extractMode);
                if (ImGui::Combo("Mode##extract", &extractIdx, extractModes, 2)) {
                    m_extractMode = static_cast<ExtractMode>(extractIdx);
                }

                bool canExtract = m_retopologyLiveObj && !m_retopologyQuads.empty();
                if (!canExtract) ImGui::BeginDisabled();
                if (ImGui::Button("Extract Under Grid")) {
                    performExtract();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Pull faces under the retopo grid off the original mesh");
                }
                if (!canExtract) ImGui::EndDisabled();

                // Patch Blanket — targeted rectangle into retopo quads
                ImGui::Spacing();
                if (m_patchBlanketMode) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "PATCH BLANKET MODE");
                    ImGui::Text("Alt+drag rectangle on viewport");
                    ImGui::Text("ESC to cancel");
                    if (ImGui::Button("Cancel Patch Blanket")) {
                        m_patchBlanketMode = false;
                        m_patchBlanketDragging = false;
                    }
                } else {
                    if (ImGui::Button("Patch Blanket")) {
                        m_patchBlanketMode = true;
                        m_patchBlanketDragging = false;
                        std::cout << "[PatchBlanket] Mode enabled — Alt+drag a rectangle" << std::endl;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Alt+drag a rectangle, blanket only that region into retopo quads");
                    }
                }
            }
        }

        // Path Tube tools
        {
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Path Tube");
            ImGui::Separator();

            if (m_pathTubeMode) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "PATH TUBE MODE");
                ImGui::Text("Nodes: %zu", m_pathNodes.size());
                if (m_pathTubeAttached && m_retopologyLiveObj) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "Attached to: %s", m_retopologyLiveObj->getName().c_str());
                } else if (m_retopologyLiveObj && m_pathNodes.empty()) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Live: %s (click to attach)", m_retopologyLiveObj->getName().c_str());
                }
                ImGui::Text("LMB: place/select node");
                ImGui::Text("G: grab node | Del: delete");
                ImGui::Text("Ctrl+Z: undo | Enter: generate");
                ImGui::Text("ESC: cancel");

                ImGui::Spacing();
                ImGui::SliderFloat("Radius", &m_pathTubeRadius, 0.001f, 1.0f, "%.3f");
                int prevSegments = m_pathTubeSegments;
                ImGui::SliderInt("Segments", &m_pathTubeSegments, 3, 32);
                if (m_pathTubeSegments != prevSegments) {
                    resetPathTubeProfile();
                }
                ImGui::SliderInt("Samples/Span", &m_pathTubeSamplesPerSpan, 2, 32);
                ImGui::SliderFloat("Taper Start", &m_pathTubeRadiusStart, 0.0f, 4.0f, "%.2f");
                ImGui::SliderFloat("Taper End", &m_pathTubeRadiusEnd, 0.0f, 4.0f, "%.2f");

                ImGui::Spacing();
                drawProfileEditor();
                float profilePct = m_pathTubeProfileExtent * 100.0f;
                if (ImGui::SliderFloat("Profile %", &profilePct, 0.0f, 100.0f, "%.0f%%")) {
                    m_pathTubeProfileExtent = profilePct / 100.0f;
                }

                ImGui::Spacing();
                if (ImGui::Button("Generate (Enter)") && m_pathNodes.size() >= 2) {
                    generatePathTubeMesh();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel (ESC)")) {
                    cancelPathTubeMode();
                }
            } else {
                if (ImGui::Button("Path Tube")) {
                    m_pathTubeMode = true;
                    m_pathNodes.clear();
                    m_pathSelectedNode = -1;
                    m_pathTubeAttached = false;
                    resetPathTubeProfile();
                    // Disable conflicting modes
                    if (m_retopologyMode) cancelRetopologyMode();
                    if (m_snapMode) cancelSnapMode();
                    if (m_vertexPaintMode) m_vertexPaintMode = false;
                    std::cout << "[PathTube] Mode enabled" << std::endl;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to place spline nodes, Enter to generate tube mesh");
                }
            }
        }

        // Mesh Slice tool
        {
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Mesh Slice");
            ImGui::Separator();

            if (m_sliceMode) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "SLICE MODE");
                ImGui::Text("Position the cutting plane");
                ImGui::Text("ESC: cancel | Enter: slice");

                ImGui::Spacing();

                const char* axisNames[] = {"X", "Y", "Z"};
                if (ImGui::Combo("Align to Axis", &m_slicePresetAxis, axisNames, 3)) {
                    updateSlicePlaneFromParams();
                }

                if (ImGui::SliderFloat("Offset", &m_slicePlaneOffset, -2.0f, 2.0f, "%.3f")) {
                    updateSlicePlaneFromParams();
                }
                if (ImGui::SliderFloat("Pitch", &m_slicePlaneRotationX, -90.0f, 90.0f, "%.1f deg")) {
                    updateSlicePlaneFromParams();
                }
                if (ImGui::SliderFloat("Yaw", &m_slicePlaneRotationY, -180.0f, 180.0f, "%.1f deg")) {
                    updateSlicePlaneFromParams();
                }

                ImGui::Checkbox("Cap Holes", &m_sliceCapHoles);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fill the open holes after slicing with a cap face");
                }

                ImGui::Spacing();
                if (ImGui::Button("Slice (Enter)")) {
                    performSlice();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel (ESC)")) {
                    cancelSliceMode();
                }
            } else {
                if (ImGui::Button("Slice Mesh")) {
                    if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
                        m_sliceMode = true;
                        m_slicePlaneOffset = 0.0f;
                        m_slicePlaneRotationX = 0.0f;
                        m_slicePlaneRotationY = 0.0f;
                        m_slicePresetAxis = 1;
                        // Disable conflicting modes
                        if (m_pathTubeMode) cancelPathTubeMode();
                        if (m_retopologyMode) cancelRetopologyMode();
                        if (m_snapMode) cancelSnapMode();
                        if (m_vertexPaintMode) m_vertexPaintMode = false;
                        updateSlicePlaneFromParams();
                        std::cout << "[Slice] Mode enabled" << std::endl;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Cut the selected mesh into two pieces with a plane");
                }
            }
        }

        // Rigging / Skeleton tools
        if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Rigging");
            ImGui::Separator();

            if (m_riggingMode) {
                auto& skel = m_ctx.editableMesh.getSkeleton();
                int numBones = static_cast<int>(skel.bones.size());

                ImGui::Checkbox("Show Skeleton", &m_showSkeleton);
                ImGui::Checkbox("Show Bone Names", &m_showBoneNames);

                // Add bone
                ImGui::InputText("##bonename", m_newBoneName, sizeof(m_newBoneName));
                ImGui::SameLine();
                if (ImGui::Button("Add Bone")) {
                    Bone newBone;
                    newBone.name = m_newBoneName;
                    newBone.parentIndex = m_selectedBone;  // parent = currently selected bone
                    int newIdx = static_cast<int>(skel.bones.size());
                    skel.bones.push_back(newBone);
                    skel.boneNameToIndex[newBone.name] = newIdx;
                    // Add default position at origin (or near parent)
                    glm::vec3 pos(0.0f);
                    if (m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bonePositions.size())) {
                        pos = m_bonePositions[m_selectedBone] + glm::vec3(0.0f, 0.5f, 0.0f);
                    }
                    m_bonePositions.push_back(pos);
                    m_selectedBone = newIdx;
                    // Increment name
                    snprintf(m_newBoneName, sizeof(m_newBoneName), "Bone.%03d", newIdx + 1);
                    m_ctx.meshDirty = true;
                    invalidateWireframeCache();
                }

                // Bone list with inline parent dropdowns
                if (numBones > 0) {
                    ImGui::Text("Bones: %d", numBones);
                    float listHeight = std::min(numBones * 26.0f + 4.0f, 200.0f);
                    ImGui::BeginChild("##bonelist", ImVec2(0, listHeight), true);
                    for (int i = 0; i < numBones; ++i) {
                        ImGui::PushID(i);
                        bool isSelected = (m_selectedBone == i);

                        // Selectable bone name (left side)
                        std::string boneName = std::to_string(i) + ": " + skel.bones[i].name;
                        if (ImGui::Selectable(boneName.c_str(), isSelected, 0, ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0))) {
                            m_selectedBone = i;
                        }

                        // Parent combo (right side, inline)
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        int parentIdx = skel.bones[i].parentIndex;
                        std::string parentLabel = (parentIdx >= 0 && parentIdx < numBones)
                            ? skel.bones[parentIdx].name : "(root)";
                        if (ImGui::BeginCombo("##parent", parentLabel.c_str())) {
                            if (ImGui::Selectable("(root)", parentIdx == -1)) {
                                skel.bones[i].parentIndex = -1;
                            }
                            for (int j = 0; j < numBones; ++j) {
                                if (j == i) continue;
                                bool sel = (parentIdx == j);
                                if (ImGui::Selectable(skel.bones[j].name.c_str(), sel)) {
                                    skel.bones[i].parentIndex = j;
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    // Selected bone properties
                    if (m_selectedBone >= 0 && m_selectedBone < numBones) {
                        ImGui::Text("Selected: %s", skel.bones[m_selectedBone].name.c_str());

                        // Position
                        if (m_selectedBone < static_cast<int>(m_bonePositions.size())) {
                            if (ImGui::DragFloat3("Position", &m_bonePositions[m_selectedBone].x, 0.01f)) {
                                m_ctx.meshDirty = true;
                            }
                        }

                        // Place bone mode
                        if (m_placingBone) {
                            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Click mesh to place bone...");
                            if (ImGui::Button("Cancel Place")) {
                                m_placingBone = false;
                            }
                        } else {
                            if (ImGui::Button("Place on Mesh")) {
                                m_placingBone = true;
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled("W/E/R = gizmo");
                        }
                        ImGui::SliderFloat("Inset Depth", &m_boneInsetDepth, 0.0f, 1.0f, "%.2f");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("How far to push bone inward from surface (along normal)");
                        }

                        // Delete bone
                        ImGui::SameLine();
                        if (ImGui::Button("Delete Bone")) {
                            // Remove bone and fix up indices
                            skel.boneNameToIndex.erase(skel.bones[m_selectedBone].name);
                            skel.bones.erase(skel.bones.begin() + m_selectedBone);
                            m_bonePositions.erase(m_bonePositions.begin() + m_selectedBone);
                            // Fix parent indices
                            for (auto& b : skel.bones) {
                                if (b.parentIndex == m_selectedBone) b.parentIndex = -1;
                                else if (b.parentIndex > m_selectedBone) b.parentIndex--;
                            }
                            // Rebuild name map
                            skel.boneNameToIndex.clear();
                            for (int i = 0; i < static_cast<int>(skel.bones.size()); ++i) {
                                skel.boneNameToIndex[skel.bones[i].name] = i;
                            }
                            m_selectedBone = std::min(m_selectedBone, static_cast<int>(skel.bones.size()) - 1);
                            m_ctx.meshDirty = true;
                            invalidateWireframeCache();
                        }
                    }

                    ImGui::Separator();

                    // Auto-weights
                    if (ImGui::Button("Auto Weights")) {
                        m_ctx.editableMesh.saveState();
                        // Build inverse bind matrices from bone positions
                        for (int i = 0; i < numBones; ++i) {
                            skel.bones[i].inverseBindMatrix = glm::inverse(
                                glm::translate(glm::mat4(1.0f), m_bonePositions[i]));
                            skel.bones[i].localTransform = glm::translate(glm::mat4(1.0f), m_bonePositions[i]);
                        }
                        m_ctx.editableMesh.generateAutoWeights(m_bonePositions);
                        m_ctx.meshDirty = true;
                        invalidateWireframeCache();
                        std::cout << "[Rigging] Auto weights generated" << std::endl;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Assign bone weights to each vertex based on distance to nearest bones");
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Clear Weights")) {
                        m_ctx.editableMesh.saveState();
                        m_ctx.editableMesh.clearBoneWeights();
                        m_ctx.meshDirty = true;
                        invalidateWireframeCache();
                    }

                    ImGui::Separator();

                    // Save skeleton to .limesk
                    if (ImGui::Button("Save Skeleton")) {
                        nfdchar_t* outPath = nullptr;
                        nfdfilteritem_t filters[1] = {{"Skeleton", "limesk"}};
                        if (NFD_SaveDialog(&outPath, filters, 1, nullptr, "skeleton.limesk") == NFD_OKAY) {
                            std::ofstream file(outPath);
                            if (file.is_open()) {
                                file << "# LIMESK v1.0\n";
                                file << "# Skeleton file for LIME editor\n";
                                file << "bones: " << numBones << "\n";
                                for (int i = 0; i < numBones; ++i) {
                                    file << "bone " << i << ": " << skel.bones[i].parentIndex
                                         << " \"" << skel.bones[i].name << "\" | ";
                                    // Position
                                    if (i < static_cast<int>(m_bonePositions.size())) {
                                        file << m_bonePositions[i].x << " "
                                             << m_bonePositions[i].y << " "
                                             << m_bonePositions[i].z;
                                    } else {
                                        file << "0 0 0";
                                    }
                                    file << "\n";
                                }
                                file.close();
                                std::cout << "[Rigging] Saved skeleton to " << outPath << std::endl;
                            }
                            NFD_FreePath(outPath);
                        }
                    }

                    ImGui::SameLine();

                    // Load skeleton from .limesk
                    if (ImGui::Button("Load Skeleton")) {
                        nfdchar_t* outPath = nullptr;
                        nfdfilteritem_t filters[1] = {{"Skeleton", "limesk"}};
                        if (NFD_OpenDialog(&outPath, filters, 1, nullptr) == NFD_OKAY) {
                            std::ifstream file(outPath);
                            if (file.is_open()) {
                                std::string line;
                                std::getline(file, line); // header
                                std::getline(file, line); // comment
                                int boneCount = 0;
                                if (std::getline(file, line)) {
                                    sscanf(line.c_str(), "bones: %d", &boneCount);
                                }

                                Skeleton newSkel;
                                std::vector<glm::vec3> newPositions;

                                for (int i = 0; i < boneCount; ++i) {
                                    if (!std::getline(file, line)) break;
                                    Bone bone;
                                    int idx, parentIdx;
                                    char nameBuffer[256] = {};
                                    float px, py, pz;

                                    // Parse: bone 0: -1 "Bone.000" | 0.0 1.0 2.0
                                    const char* nameStart = strchr(line.c_str(), '"');
                                    const char* nameEnd = nameStart ? strchr(nameStart + 1, '"') : nullptr;
                                    if (nameStart && nameEnd) {
                                        size_t nameLen = std::min<size_t>(nameEnd - nameStart - 1, 255);
                                        strncpy(nameBuffer, nameStart + 1, nameLen);
                                        nameBuffer[nameLen] = '\0';
                                    }

                                    sscanf(line.c_str(), "bone %d: %d", &idx, &parentIdx);

                                    const char* pipePos = strchr(line.c_str(), '|');
                                    px = py = pz = 0.0f;
                                    if (pipePos) {
                                        sscanf(pipePos + 1, " %f %f %f", &px, &py, &pz);
                                    }

                                    bone.name = nameBuffer;
                                    bone.parentIndex = parentIdx;
                                    bone.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(px, py, pz));
                                    bone.inverseBindMatrix = glm::inverse(bone.localTransform);
                                    newSkel.bones.push_back(bone);
                                    newSkel.boneNameToIndex[bone.name] = i;
                                    newPositions.push_back(glm::vec3(px, py, pz));
                                }
                                file.close();

                                m_ctx.editableMesh.setSkeleton(newSkel);
                                m_bonePositions = newPositions;
                                m_selectedBone = newPositions.empty() ? -1 : 0;
                                snprintf(m_newBoneName, sizeof(m_newBoneName), "Bone.%03d", boneCount);
                                m_ctx.meshDirty = true;
                                invalidateWireframeCache();
                                std::cout << "[Rigging] Loaded skeleton (" << boneCount << " bones) from " << outPath << std::endl;
                            }
                            NFD_FreePath(outPath);
                        }
                    }
                }

                // Delete entire skeleton
                if (numBones > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                    if (ImGui::Button("Delete Skeleton")) {
                        m_ctx.editableMesh.saveState();
                        m_ctx.editableMesh.clearSkeleton();
                        m_ctx.editableMesh.clearBoneWeights();
                        m_bonePositions.clear();
                        m_selectedBone = -1;
                        m_placingBone = false;
                        snprintf(m_newBoneName, sizeof(m_newBoneName), "Bone.000");
                        m_ctx.meshDirty = true;
                        invalidateWireframeCache();
                        std::cout << "[Rigging] Skeleton deleted" << std::endl;
                    }
                    ImGui::PopStyleColor();
                }

                // === Weight Editor ===
                if (numBones > 0) {
                    // Gather all selected vertices (from vert/edge/face selection)
                    std::set<uint32_t> selectedVertSet;
                    auto selVerts = m_ctx.editableMesh.getSelectedVertices();
                    for (uint32_t vi : selVerts) selectedVertSet.insert(vi);
                    auto selEdges = m_ctx.editableMesh.getSelectedEdges();
                    for (uint32_t ei : selEdges) {
                        auto ep = m_ctx.editableMesh.getEdgeVertices(ei);
                        selectedVertSet.insert(ep.first);
                        selectedVertSet.insert(ep.second);
                    }
                    auto selFaces = m_ctx.editableMesh.getSelectedFaces();
                    for (uint32_t fi : selFaces) {
                        auto fv = m_ctx.editableMesh.getFaceVertices(fi);
                        for (uint32_t vi : fv) selectedVertSet.insert(vi);
                    }

                    if (!selectedVertSet.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Weight Editor (%d verts)", (int)selectedVertSet.size());

                        // Find all bones that influence the selection + compute average weights
                        std::map<int, float> boneWeightSums;  // boneIdx -> sum of weights
                        std::map<int, int> boneWeightCounts;   // boneIdx -> count of verts with nonzero weight
                        for (uint32_t vi : selectedVertSet) {
                            const auto& v = m_ctx.editableMesh.getVertex(vi);
                            for (int j = 0; j < 3; ++j) {
                                if (v.boneWeights[j] > 0.0f) {
                                    boneWeightSums[v.boneIndices[j]] += v.boneWeights[j];
                                    boneWeightCounts[v.boneIndices[j]]++;
                                }
                            }
                        }

                        // Bone influence summary with sliders
                        int numSelected = static_cast<int>(selectedVertSet.size());
                        for (auto& [boneIdx, weightSum] : boneWeightSums) {
                            if (boneIdx < 0 || boneIdx >= numBones) continue;
                            float avgWeight = weightSum / numSelected;
                            ImGui::PushID(boneIdx + 10000);
                            ImGui::Text("%s", skel.bones[boneIdx].name.c_str());
                            ImGui::SameLine(100);
                            float newWeight = avgWeight;
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                            if (ImGui::SliderFloat("##weight", &newWeight, 0.0f, 1.0f, "%.3f")) {
                                // Apply weight change to all selected vertices
                                float delta = newWeight - avgWeight;
                                for (uint32_t vi : selectedVertSet) {
                                    auto& v = m_ctx.editableMesh.getVertex(vi);
                                    // Find if this bone already influences this vertex
                                    int slot = -1;
                                    for (int j = 0; j < 3; ++j) {
                                        if (v.boneIndices[j] == boneIdx && v.boneWeights[j] > 0.0f) {
                                            slot = j;
                                            break;
                                        }
                                    }
                                    if (slot >= 0) {
                                        v.boneWeights[slot] = glm::clamp(v.boneWeights[slot] + delta, 0.0f, 1.0f);
                                    } else if (delta > 0.0f) {
                                        // Find an empty slot or the smallest weight slot
                                        int minSlot = 0;
                                        for (int j = 1; j < 3; ++j) {
                                            if (v.boneWeights[j] < v.boneWeights[minSlot]) minSlot = j;
                                        }
                                        if (v.boneWeights[minSlot] == 0.0f) {
                                            v.boneIndices[minSlot] = boneIdx;
                                            v.boneWeights[minSlot] = delta;
                                        }
                                    }
                                    // Normalize weights to sum to 1.0
                                    float total = v.boneWeights[0] + v.boneWeights[1] + v.boneWeights[2];
                                    if (total > 0.001f) {
                                        v.boneWeights[0] /= total;
                                        v.boneWeights[1] /= total;
                                        v.boneWeights[2] /= total;
                                    }
                                }
                                m_ctx.meshDirty = true;
                                invalidateWireframeCache();
                            }
                            ImGui::PopID();
                        }

                        // Per-vertex detail (collapsible)
                        if (numSelected <= 100 && ImGui::TreeNode("Per-Vertex Weights")) {
                            ImGui::BeginChild("##vertweights", ImVec2(0, std::min((float)numSelected * 24.0f + 4.0f, 200.0f)), true);
                            for (uint32_t vi : selectedVertSet) {
                                auto& v = m_ctx.editableMesh.getVertex(vi);
                                ImGui::PushID(static_cast<int>(vi));
                                ImGui::Text("v%d:", vi);
                                bool changed = false;
                                for (int j = 0; j < 3; ++j) {
                                    if (v.boneWeights[j] <= 0.0f && v.boneIndices[j] == 0) {
                                        // Empty slot — offer bone assignment
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(60);
                                        std::string comboId = "##b" + std::to_string(j);
                                        if (ImGui::BeginCombo(comboId.c_str(), "--")) {
                                            for (int b = 0; b < numBones; ++b) {
                                                if (ImGui::Selectable(skel.bones[b].name.c_str())) {
                                                    v.boneIndices[j] = b;
                                                    v.boneWeights[j] = 0.01f;
                                                    changed = true;
                                                }
                                            }
                                            ImGui::EndCombo();
                                        }
                                        continue;
                                    }
                                    ImGui::SameLine();
                                    std::string bName = (v.boneIndices[j] >= 0 && v.boneIndices[j] < numBones)
                                        ? skel.bones[v.boneIndices[j]].name : "?";
                                    ImGui::SetNextItemWidth(60);
                                    std::string fieldId = bName + "##w" + std::to_string(j);
                                    if (ImGui::DragFloat(fieldId.c_str(), &v.boneWeights[j], 0.01f, 0.0f, 1.0f, "%.2f")) {
                                        changed = true;
                                    }
                                }
                                if (changed) {
                                    // Normalize
                                    float total = v.boneWeights[0] + v.boneWeights[1] + v.boneWeights[2];
                                    if (total > 0.001f) {
                                        v.boneWeights[0] /= total;
                                        v.boneWeights[1] /= total;
                                        v.boneWeights[2] /= total;
                                    }
                                    m_ctx.meshDirty = true;
                                    invalidateWireframeCache();
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndChild();
                            ImGui::TreePop();
                        }
                    }
                }

                if (ImGui::Button("Exit Rigging")) {
                    cancelRiggingMode();
                }
            } else {
                if (ImGui::Button("Enter Rigging Mode")) {
                    m_riggingMode = true;
                    m_selectedBone = -1;
                    m_placingBone = false;
                    m_showSkeleton = true;
                    // Switch to component mode with vertex selection + wireframe visible
                    m_ctx.objectMode = false;
                    m_ctx.modelingSelectionMode = ModelingSelectionMode::Vertex;
                    m_ctx.showModelingWireframe = true;
                    // Sync bone positions from existing skeleton
                    auto& skel = m_ctx.editableMesh.getSkeleton();
                    m_bonePositions.clear();
                    for (const auto& bone : skel.bones) {
                        // Extract position from localTransform
                        m_bonePositions.push_back(glm::vec3(bone.localTransform[3]));
                    }
                    // Disable conflicting modes
                    if (m_pathTubeMode) cancelPathTubeMode();
                    if (m_retopologyMode) cancelRetopologyMode();
                    if (m_sliceMode) cancelSliceMode();
                    if (m_vertexPaintMode) m_vertexPaintMode = false;
                    snprintf(m_newBoneName, sizeof(m_newBoneName), "Bone.%03d",
                             static_cast<int>(skel.bones.size()));
                }
            }
        }

        // Vertex color for solid-colored objects
        if (m_ctx.selectedObject && m_ctx.selectedObject->hasMeshData()) {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Vertex Color");
            ImGui::Separator();

            static glm::vec3 solidColor(0.7f, 0.7f, 0.7f);
            ImGui::ColorEdit3("Color", &solidColor.x);

            if (ImGui::Button("Apply to Object")) {
                // Sync current edits first
                if (m_ctx.meshDirty) {
                    updateMeshFromEditable();
                }

                // Get current mesh data
                auto verts = m_ctx.selectedObject->getVertices();  // Copy
                const auto& indices = m_ctx.selectedObject->getIndices();

                // Apply color to all vertices
                for (auto& v : verts) {
                    v.color = glm::vec4(solidColor, 1.0f);
                }

                // Update GPU mesh (preserve texture)
                uint32_t oldHandle = m_ctx.selectedObject->getBufferHandle();
                uint32_t newHandle;
                if (m_ctx.selectedObject->hasTextureData()) {
                    const auto& texData = m_ctx.selectedObject->getTextureData();
                    newHandle = m_ctx.modelRenderer.createModel(verts, indices, texData.data(),
                        m_ctx.selectedObject->getTextureWidth(), m_ctx.selectedObject->getTextureHeight());
                } else {
                    newHandle = m_ctx.modelRenderer.createModel(verts, indices, nullptr, 0, 0);
                }
                m_ctx.selectedObject->setBufferHandle(newHandle);
                m_ctx.selectedObject->setMeshData(verts, indices);

                // Update editable mesh vertex colors too
                if (m_ctx.editableMesh.isValid()) {
                    for (size_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                        m_ctx.editableMesh.getVertex(i).color = glm::vec4(solidColor, 1.0f);
                    }
                }

                std::cout << "[Color] Applied solid color (" << solidColor.r << ", "
                          << solidColor.g << ", " << solidColor.b << ") to "
                          << verts.size() << " vertices" << std::endl;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set all vertices to this solid color.\nUseful for detail pieces before combining.");
            }
            ImGui::Spacing();
        }

        // Combine selected objects button
        if (m_ctx.selectedObjects.size() >= 2) {
            if (ImGui::Button("Combine Selected")) {
                // Sync current edits before combining
                if (m_ctx.meshDirty && m_ctx.selectedObject) {
                    updateMeshFromEditable();
                }

                // Merge selected objects into one
                std::vector<ModelVertex> combinedVerts;
                std::vector<uint32_t> combinedIndices;

                // Also combine half-edge data to preserve quad topology
                std::vector<SceneObject::StoredHEVertex> combinedHEVerts;
                std::vector<SceneObject::StoredHalfEdge> combinedHE;
                std::vector<SceneObject::StoredHEFace> combinedHEFaces;
                bool allHaveHEData = true;

                for (auto& obj : m_ctx.sceneObjects) {
                    if (m_ctx.selectedObjects.count(obj.get()) == 0) continue;
                    if (!obj->hasMeshData()) continue;

                    const auto& verts = obj->getVertices();
                    const auto& indices = obj->getIndices();
                    const auto& transform = obj->getTransform();
                    glm::mat4 modelMatrix = transform.getMatrix();
                    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));

                    uint32_t indexOffset = static_cast<uint32_t>(combinedVerts.size());

                    for (const auto& v : verts) {
                        ModelVertex newVert = v;
                        glm::vec4 worldPos = modelMatrix * glm::vec4(v.position, 1.0f);
                        newVert.position = glm::vec3(worldPos);
                        newVert.normal = glm::normalize(normalMatrix * v.normal);
                        combinedVerts.push_back(newVert);
                    }

                    for (uint32_t idx : indices) {
                        combinedIndices.push_back(idx + indexOffset);
                    }

                    // Combine half-edge data if available
                    if (obj->hasEditableMeshData()) {
                        uint32_t heVertOffset = static_cast<uint32_t>(combinedHEVerts.size());
                        uint32_t heOffset = static_cast<uint32_t>(combinedHE.size());
                        uint32_t heFaceOffset = static_cast<uint32_t>(combinedHEFaces.size());

                        for (const auto& v : obj->getHEVertices()) {
                            SceneObject::StoredHEVertex newV = v;
                            glm::vec4 worldPos = modelMatrix * glm::vec4(v.position, 1.0f);
                            newV.position = glm::vec3(worldPos);
                            newV.normal = glm::normalize(normalMatrix * v.normal);
                            if (newV.halfEdgeIndex != UINT32_MAX) newV.halfEdgeIndex += heOffset;
                            combinedHEVerts.push_back(newV);
                        }

                        for (const auto& he : obj->getHEHalfEdges()) {
                            SceneObject::StoredHalfEdge newHE = he;
                            if (newHE.vertexIndex != UINT32_MAX) newHE.vertexIndex += heVertOffset;
                            if (newHE.faceIndex != UINT32_MAX) newHE.faceIndex += heFaceOffset;
                            if (newHE.nextIndex != UINT32_MAX) newHE.nextIndex += heOffset;
                            if (newHE.prevIndex != UINT32_MAX) newHE.prevIndex += heOffset;
                            if (newHE.twinIndex != UINT32_MAX) newHE.twinIndex += heOffset;
                            combinedHE.push_back(newHE);
                        }

                        for (const auto& f : obj->getHEFaces()) {
                            SceneObject::StoredHEFace newF = f;
                            if (newF.halfEdgeIndex != UINT32_MAX) newF.halfEdgeIndex += heOffset;
                            combinedHEFaces.push_back(newF);
                        }
                    } else {
                        allHaveHEData = false;
                    }
                }

                if (!combinedVerts.empty()) {
                    auto combinedObj = std::make_unique<SceneObject>("Combined");
                    uint32_t handle = m_ctx.modelRenderer.createModel(combinedVerts, combinedIndices, nullptr, 0, 0);
                    combinedObj->setBufferHandle(handle);
                    combinedObj->setIndexCount(static_cast<uint32_t>(combinedIndices.size()));
                    combinedObj->setVertexCount(static_cast<uint32_t>(combinedVerts.size()));
                    combinedObj->setMeshData(combinedVerts, combinedIndices);

                    // Store combined half-edge data if all sources had it
                    if (allHaveHEData && !combinedHEVerts.empty()) {
                        combinedObj->setEditableMeshData(combinedHEVerts, combinedHE, combinedHEFaces);
                    }

                    // Queue selected objects for deletion
                    for (SceneObject* obj : m_ctx.selectedObjects) {
                        m_ctx.pendingDeletions.push_back(obj);
                    }

                    // Clear selection
                    m_ctx.selectedObject = nullptr;
                    m_ctx.selectedObjects.clear();
                    m_ctx.editableMesh.clear();
                    m_ctx.meshDirty = false;

                    m_ctx.sceneObjects.push_back(std::move(combinedObj));

                    std::cout << "[Combine Selected] Created combined mesh with " << combinedVerts.size()
                              << " vertices, " << combinedIndices.size() / 3 << " triangles" << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Merge selected objects into one combined mesh.\nCtrl+Click or Shift+Click to multi-select in list.");
            }
            ImGui::SameLine();

            // Connect CPs button - enabled when exactly 2 objects selected with matching CPs
            {
                bool canConnect = false;
                if (m_ctx.selectedObjects.size() == 2) {
                    // Check for matching CP names between the two objects
                    std::vector<SceneObject*> selVec(m_ctx.selectedObjects.begin(), m_ctx.selectedObjects.end());
                    auto& cps1 = selVec[0]->getControlPoints();
                    auto& cps2 = selVec[1]->getControlPoints();
                    int matchCount = 0;
                    for (auto& cp1 : cps1)
                        for (auto& cp2 : cps2)
                            if (cp1.name == cp2.name) matchCount++;
                    canConnect = (matchCount >= 2);
                }
                if (!canConnect) ImGui::BeginDisabled();
                if (ImGui::Button("Connect CPs")) {
                    // Clear all selections before connecting
                    m_ctx.editableMesh.clearSelection();
                    m_ctx.selectedFaces.clear();
                    m_ctx.hiddenFaces.clear();
                    connectByControlPoints();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Connect two objects by matching control points.\nAligns obj2 to obj1, merges boundary rings, removes caps.\nRequires 2 selected objects with 2+ matching CP names.");
                }
                if (!canConnect) ImGui::EndDisabled();
                if (m_connectCPsBackup.valid) {
                    ImGui::SameLine();
                    if (ImGui::Button("Undo Connect")) {
                        undoConnectCPs();
                    }
                }
            }
        }

        // Combine all objects button
        if (m_ctx.sceneObjects.size() >= 2) {
            if (ImGui::Button("Combine All")) {
                // Sync current edits before combining
                if (m_ctx.meshDirty && m_ctx.selectedObject) {
                    updateMeshFromEditable();
                }

                // Merge all objects into one
                std::vector<ModelVertex> combinedVerts;
                std::vector<uint32_t> combinedIndices;

                // Also combine half-edge data to preserve quad topology
                std::vector<SceneObject::StoredHEVertex> combinedHEVerts;
                std::vector<SceneObject::StoredHalfEdge> combinedHE;
                std::vector<SceneObject::StoredHEFace> combinedHEFaces;
                bool allHaveHEData = true;

                // Find the first textured object to preserve its texture
                SceneObject* texturedSource = nullptr;
                for (auto& obj : m_ctx.sceneObjects) {
                    if (obj->hasTextureData()) {
                        texturedSource = obj.get();
                        break;
                    }
                }

                for (auto& obj : m_ctx.sceneObjects) {
                    if (!obj->hasMeshData()) continue;

                    const auto& verts = obj->getVertices();
                    const auto& indices = obj->getIndices();
                    const auto& transform = obj->getTransform();
                    glm::mat4 modelMatrix = transform.getMatrix();
                    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));

                    uint32_t indexOffset = static_cast<uint32_t>(combinedVerts.size());

                    // Add vertices with transform applied
                    for (const auto& v : verts) {
                        ModelVertex newVert = v;
                        glm::vec4 worldPos = modelMatrix * glm::vec4(v.position, 1.0f);
                        newVert.position = glm::vec3(worldPos);
                        newVert.normal = glm::normalize(normalMatrix * v.normal);
                        // Offset UVs of non-textured objects outside 0-1 to avoid texture clash
                        if (texturedSource && obj.get() != texturedSource) {
                            newVert.texCoord.x += 1.0f;
                        }
                        combinedVerts.push_back(newVert);
                    }

                    // Add indices with offset
                    for (uint32_t idx : indices) {
                        combinedIndices.push_back(idx + indexOffset);
                    }

                    // Combine half-edge data if available
                    if (obj->hasEditableMeshData()) {
                        uint32_t heVertOffset = static_cast<uint32_t>(combinedHEVerts.size());
                        uint32_t heOffset = static_cast<uint32_t>(combinedHE.size());
                        uint32_t heFaceOffset = static_cast<uint32_t>(combinedHEFaces.size());

                        for (const auto& v : obj->getHEVertices()) {
                            SceneObject::StoredHEVertex newV = v;
                            glm::vec4 worldPos = modelMatrix * glm::vec4(v.position, 1.0f);
                            newV.position = glm::vec3(worldPos);
                            newV.normal = glm::normalize(normalMatrix * v.normal);
                            if (newV.halfEdgeIndex != UINT32_MAX) newV.halfEdgeIndex += heOffset;
                            // Offset UVs of non-textured objects outside 0-1
                            if (texturedSource && obj.get() != texturedSource) {
                                newV.uv.x += 1.0f;
                            }
                            combinedHEVerts.push_back(newV);
                        }

                        for (const auto& he : obj->getHEHalfEdges()) {
                            SceneObject::StoredHalfEdge newHE = he;
                            if (newHE.vertexIndex != UINT32_MAX) newHE.vertexIndex += heVertOffset;
                            if (newHE.faceIndex != UINT32_MAX) newHE.faceIndex += heFaceOffset;
                            if (newHE.nextIndex != UINT32_MAX) newHE.nextIndex += heOffset;
                            if (newHE.prevIndex != UINT32_MAX) newHE.prevIndex += heOffset;
                            if (newHE.twinIndex != UINT32_MAX) newHE.twinIndex += heOffset;
                            combinedHE.push_back(newHE);
                        }

                        for (const auto& f : obj->getHEFaces()) {
                            SceneObject::StoredHEFace newF = f;
                            if (newF.halfEdgeIndex != UINT32_MAX) newF.halfEdgeIndex += heOffset;
                            combinedHEFaces.push_back(newF);
                        }
                    } else {
                        allHaveHEData = false;
                    }
                }

                if (!combinedVerts.empty()) {
                    // Create new combined object
                    auto combinedObj = std::make_unique<SceneObject>("Combined");

                    // Pass texture from the textured source object
                    const unsigned char* texPtr = nullptr;
                    int texW = 0, texH = 0;
                    if (texturedSource && texturedSource->hasTextureData()) {
                        texPtr = texturedSource->getTextureData().data();
                        texW = texturedSource->getTextureWidth();
                        texH = texturedSource->getTextureHeight();
                    }
                    uint32_t handle = m_ctx.modelRenderer.createModel(combinedVerts, combinedIndices, texPtr, texW, texH);
                    combinedObj->setBufferHandle(handle);
                    combinedObj->setIndexCount(static_cast<uint32_t>(combinedIndices.size()));
                    combinedObj->setVertexCount(static_cast<uint32_t>(combinedVerts.size()));
                    combinedObj->setMeshData(combinedVerts, combinedIndices);

                    // Store combined half-edge data if all sources had it
                    if (allHaveHEData && !combinedHEVerts.empty()) {
                        combinedObj->setEditableMeshData(combinedHEVerts, combinedHE, combinedHEFaces);
                    }

                    // Copy texture data to combined object so it persists
                    if (texturedSource && texturedSource->hasTextureData()) {
                        combinedObj->setTextureData(texturedSource->getTextureData(),
                                                    texturedSource->getTextureWidth(),
                                                    texturedSource->getTextureHeight());
                    }

                    // Queue all existing objects for deletion
                    for (auto& obj : m_ctx.sceneObjects) {
                        m_ctx.pendingDeletions.push_back(obj.get());
                    }

                    // Clear selection
                    m_ctx.selectedObject = nullptr;
                    m_ctx.selectedObjects.clear();
                    m_ctx.editableMesh.clear();
                    m_ctx.meshDirty = false;

                    // Add combined object
                    m_ctx.sceneObjects.push_back(std::move(combinedObj));

                    std::cout << "[Combine] Created combined mesh with " << combinedVerts.size()
                              << " vertices, " << combinedIndices.size() / 3 << " triangles" << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Merge all objects into one combined mesh.\nTransforms are baked in, UVs preserved.");
            }
        }

        ImGui::Spacing();

        // Transform controls for selected object
        if (m_ctx.selectedObject) {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Transform");
            ImGui::Separator();

            auto& transform = m_ctx.selectedObject->getTransform();
            glm::vec3 pos = transform.getPosition();
            glm::vec3 rot = glm::degrees(glm::eulerAngles(transform.getRotation()));
            glm::vec3 scale = transform.getScale();

            // Position - directly editable input fields
            ImGui::Text("Position");
            ImGui::PushItemWidth(-1);
            if (ImGui::InputFloat3("##pos", &pos.x, "%.3f")) {
                transform.setPosition(pos);
            }
            ImGui::PopItemWidth();

            // Rotation - directly editable input fields
            ImGui::Text("Rotation");
            ImGui::PushItemWidth(-1);
            if (ImGui::InputFloat3("##rot", &rot.x, "%.1f")) {
                transform.setRotation(rot);
            }
            ImGui::PopItemWidth();

            // Scale - directly editable input fields
            ImGui::Text("Scale");
            ImGui::PushItemWidth(-1);
            if (ImGui::InputFloat3("##scale", &scale.x, "%.3f")) {
                transform.setScale(scale);
            }
            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1), "Snap Settings");
            ImGui::Separator();

            ImGui::Checkbox("Enable Snap", &m_ctx.snapEnabled);

            if (m_ctx.snapEnabled) {
                ImGui::PushItemWidth(80);
                ImGui::InputFloat("Move", &m_ctx.moveSnapIncrement, 0.0f, 0.0f, "%.2f");
                if (m_ctx.moveSnapIncrement < 0.01f) m_ctx.moveSnapIncrement = 0.01f;

                ImGui::InputFloat("Rotate", &m_ctx.rotateSnapIncrement, 0.0f, 0.0f, "%.0f");
                if (m_ctx.rotateSnapIncrement < 1.0f) m_ctx.rotateSnapIncrement = 1.0f;
                ImGui::PopItemWidth();

                // Quick preset buttons for rotation
                ImGui::SameLine();
                if (ImGui::SmallButton("15")) m_ctx.rotateSnapIncrement = 15.0f;
                ImGui::SameLine();
                if (ImGui::SmallButton("45")) m_ctx.rotateSnapIncrement = 45.0f;
                ImGui::SameLine();
                if (ImGui::SmallButton("90")) m_ctx.rotateSnapIncrement = 90.0f;
            }
        }
    }
    ImGui::End();
    }

    // Tools window
    ImGui::SetNextWindowPos(ImVec2(0, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250, 400), ImGuiCond_FirstUseEver);

    if (m_ctx.showToolsWindow) {
    if (ImGui::Begin("Tools", &m_ctx.showToolsWindow)) {
        // Selection mode section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.15f, 0.15f, 0.5f));
        ImGui::BeginChild("SelectionSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.9f, 1), "Selection Mode");
        ImGui::Separator();

        if (ImGui::RadioButton("Vertex (A)", m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex)) {
            m_ctx.modelingSelectionMode = ModelingSelectionMode::Vertex;
            m_ctx.editableMesh.clearSelection();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Edge (S)", m_ctx.modelingSelectionMode == ModelingSelectionMode::Edge)) {
            m_ctx.modelingSelectionMode = ModelingSelectionMode::Edge;
            m_ctx.editableMesh.clearSelection();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Face (D)", m_ctx.modelingSelectionMode == ModelingSelectionMode::Face)) {
            m_ctx.modelingSelectionMode = ModelingSelectionMode::Face;
            m_ctx.editableMesh.clearSelection();
        }

        // Selection tool
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "Selection Tool:");
        if (ImGui::RadioButton("Normal", m_ctx.selectionTool == SelectionTool::Normal)) {
            m_ctx.selectionTool = SelectionTool::Normal;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to select, drag for rectangle select");
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Paint", m_ctx.selectionTool == SelectionTool::Paint)) {
            m_ctx.selectionTool = SelectionTool::Paint;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Paint to select elements under cursor");
        }

        if (m_ctx.selectionTool == SelectionTool::Paint) {
            ImGui::SliderFloat("Brush Radius", &m_ctx.paintSelectRadius, 5.0f, 100.0f, "%.0f px");
        }

        // Select Facing — mass select front-facing faces
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Select by Normal:");
        static float selectFacingAngle = 45.0f;
        ImGui::SliderFloat("Max Angle", &selectFacingAngle, 5.0f, 90.0f, "%.0f deg");
        if (ImGui::Button("Select Facing Camera")) {
            Camera& cam = m_ctx.getActiveCamera();
            glm::vec3 viewDir = cam.getFront();
            // Transform view direction into model local space (inverse model rotation)
            if (m_ctx.selectedObject) {
                glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                viewDir = glm::normalize(glm::vec3(invModel * glm::vec4(viewDir, 0.0f)));
            }
            m_ctx.editableMesh.selectFacesByNormal(viewDir, selectFacingAngle, m_ctx.hiddenFaces);
        }

        // Face visibility controls
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "Visibility:");
        if (ImGui::Button("Hide Selected")) {
            // Hide all selected faces
            for (uint32_t faceIdx : m_ctx.editableMesh.getSelectedFaces()) {
                m_ctx.hiddenFaces.insert(faceIdx);
            }
            m_ctx.editableMesh.clearSelection();
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide Unselected")) {
            // Hide all faces that are NOT selected
            std::vector<uint32_t> selected = m_ctx.editableMesh.getSelectedFaces();
            std::set<uint32_t> selectedSet(selected.begin(), selected.end());
            for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
                if (selectedSet.find(faceIdx) == selectedSet.end()) {
                    m_ctx.hiddenFaces.insert(faceIdx);
                }
            }
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Show All")) {
            m_ctx.hiddenFaces.clear();
            m_ctx.meshDirty = true;
        }
        if (ImGui::Button("Invert")) {
            // Swap visible and hidden faces
            std::set<uint32_t> newHidden;
            for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
                if (m_ctx.hiddenFaces.find(faceIdx) == m_ctx.hiddenFaces.end()) {
                    newHidden.insert(faceIdx);
                }
            }
            m_ctx.hiddenFaces = newHidden;
            m_ctx.meshDirty = true;
        }
        if (!m_ctx.hiddenFaces.empty()) {
            ImGui::TextDisabled("%zu faces hidden", m_ctx.hiddenFaces.size());
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Transform / Gizmo section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.12f, 0.0f, 0.5f));
        ImGui::BeginChild("TransformSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1), "Transform");
        ImGui::Separator();

        // Gizmo mode toggle
        bool selectActive = (m_ctx.gizmoMode == GizmoMode::None);
        if (ImGui::Checkbox("Select (Q)", &selectActive)) {
            m_ctx.gizmoMode = GizmoMode::None;
        }
        ImGui::SameLine();
        bool moveActive = (m_ctx.gizmoMode == GizmoMode::Move);
        if (ImGui::Checkbox("Move (W)", &moveActive)) {
            m_ctx.gizmoMode = GizmoMode::Move;
        }
        ImGui::SameLine();
        bool rotateActive = (m_ctx.gizmoMode == GizmoMode::Rotate);
        if (ImGui::Checkbox("Rotate (E)", &rotateActive)) {
            m_ctx.gizmoMode = GizmoMode::Rotate;
        }
        ImGui::SameLine();
        bool scaleActive = (m_ctx.gizmoMode == GizmoMode::Scale);
        if (ImGui::Checkbox("Scale (R)", &scaleActive)) {
            m_ctx.gizmoMode = GizmoMode::Scale;
        }

        if (m_ctx.gizmoMode != GizmoMode::None) {
            ImGui::SliderFloat("Gizmo Size", &m_ctx.gizmoSize, 0.1f, 3.0f, "%.1f");
            ImGui::SliderFloat3("Gizmo Offset", &m_ctx.gizmoOffset.x, -2.0f, 2.0f, "%.2f");
            if (m_ctx.gizmoMode == GizmoMode::Move) {
                ImGui::Checkbox("Local Space (Face Normal)", &m_ctx.gizmoLocalSpace);
            }
            // Reset custom gizmo pivot (shown when pivot is overridden, e.g. after snap)
            if (m_useCustomGizmoPivot) {
                if (ImGui::Button("Reset Gizmo")) {
                    m_useCustomGizmoPivot = false;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Reset gizmo to object center");
                }
            }
        }

        // Numeric translation input
        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
        bool hasVertSelection = !selectedVerts.empty();

        if (hasVertSelection) {
            ImGui::Spacing();
            ImGui::Text("Move Selection:");

            static glm::vec3 translateAmount(0.0f);

            ImGui::PushItemWidth(60);
            bool changed = false;

            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "X:");
            ImGui::SameLine();
            if (ImGui::DragFloat("##tx", &translateAmount.x, 0.01f, -100.0f, 100.0f, "%.3f")) {
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "Y:");
            ImGui::SameLine();
            if (ImGui::DragFloat("##ty", &translateAmount.y, 0.01f, -100.0f, 100.0f, "%.3f")) {
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.3f, 1, 1), "Z:");
            ImGui::SameLine();
            if (ImGui::DragFloat("##tz", &translateAmount.z, 0.01f, -100.0f, 100.0f, "%.3f")) {
                changed = true;
            }

            ImGui::PopItemWidth();

            if (changed && m_ctx.selectedObject) {
                // Apply translation using position-matched vertex movement
                m_ctx.editableMesh.saveState();
                glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                glm::vec3 localDelta = glm::vec3(invModel * glm::vec4(translateAmount, 0.0f));
                m_ctx.editableMesh.translateSelectedVertices(localDelta);
                m_ctx.meshDirty = true;
                translateAmount = glm::vec3(0.0f);  // Reset after applying
            }

            // Quick translate buttons (use translateSelectedVertices for position-matched movement)
            ImGui::PushItemWidth(40);
            if (ImGui::Button("+X")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(0.1f, 0, 0));
                m_ctx.meshDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("-X")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(-0.1f, 0, 0));
                m_ctx.meshDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("+Y")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(0, 0.1f, 0));
                m_ctx.meshDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("-Y")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(0, -0.1f, 0));
                m_ctx.meshDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("+Z")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(0, 0, 0.1f));
                m_ctx.meshDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("-Z")) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.translateSelectedVertices(glm::vec3(0, 0, -0.1f));
                m_ctx.meshDirty = true;
            }
            ImGui::PopItemWidth();
        } else {
            ImGui::TextDisabled("Select vertices to transform");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Operations section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.08f, 0.0f, 0.5f));
        ImGui::BeginChild("OperationsSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1), "Operations");
        ImGui::Separator();

        bool hasSelection = !m_ctx.editableMesh.getSelectedFaces().empty() ||
                           !m_ctx.editableMesh.getSelectedEdges().empty() ||
                           !selectedVerts.empty();

        if (!hasSelection) ImGui::BeginDisabled();

        if (ImGui::Button("Extrude (Shift+E)")) {
            if (!m_ctx.editableMesh.getSelectedEdges().empty()) {
                m_ctx.editableMesh.saveState();
                int count = std::max(1, m_ctx.extrudeCount);
                float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
                for (int i = 0; i < count; ++i) {
                    m_ctx.editableMesh.extrudeSelectedEdges(stepDist);
                }
                m_ctx.meshDirty = true;
                m_ctx.gizmoMode = GizmoMode::Move;
            } else if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
                m_ctx.editableMesh.saveState();
                int count = std::max(1, m_ctx.extrudeCount);
                float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
                for (int i = 0; i < count; ++i) {
                    m_ctx.editableMesh.extrudeSelectedFaces(stepDist);
                }
                m_ctx.meshDirty = true;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat("##dist", &m_ctx.extrudeDistance, 0.0f, 0.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40);
        ImGui::InputInt("##count", &m_ctx.extrudeCount, 0, 0);
        if (m_ctx.extrudeCount < 1) m_ctx.extrudeCount = 1;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of extrusion segments");
        }

        if (ImGui::Button("Delete (Del)")) {
            if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.deleteSelectedFaces();
                m_ctx.meshDirty = true;
            }
        }

        if (ImGui::Button("Flip Normals (N)")) {
            if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.flipSelectedNormals();
                m_ctx.meshDirty = true;
            }
        }

        if (ImGui::Button("Make Normals Consistent")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.makeNormalsConsistent();
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Flood-fill to make all faces wind the same way.\nPress once = consistent. Press again = all flipped.");
        }

        if (ImGui::Button("Inset (I)")) {
            if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.insetSelectedFaces(m_ctx.insetAmount);
                m_ctx.meshDirty = true;
            }
        }
        ImGui::SameLine();
        ImGui::SliderFloat("##inset", &m_ctx.insetAmount, 0.05f, 0.95f, "%.2f");

        if (!hasSelection) ImGui::EndDisabled();

        // Edge-specific operations
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        bool hasEdgeSelection = !selectedEdges.empty();

        if (!hasEdgeSelection) ImGui::BeginDisabled();

        static int edgeLoopCount = 1;
        ImGui::SliderInt("Loop Count", &edgeLoopCount, 1, 10);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of edge loops to insert.\n1 = split in half\n2 = split into thirds\netc.");
        }

        if (ImGui::Button("Insert Edge Loop (Ctrl+R)")) {
            if (!selectedEdges.empty()) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.insertEdgeLoop(selectedEdges[0], edgeLoopCount);
                m_ctx.meshDirty = true;
            }
        }

        ImGui::SliderInt("Bridge Segments", &g_bridgeSegments, 1, 10);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of face rows in the bridge");
        }

        if (selectedEdges.size() != 2) ImGui::BeginDisabled();
        if (ImGui::Button("Bridge Edges (B)")) {
            if (selectedEdges.size() == 2) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.bridgeEdges(selectedEdges[0], selectedEdges[1], g_bridgeSegments);
                m_ctx.meshDirty = true;
            }
        }
        if (selectedEdges.size() != 2) ImGui::EndDisabled();

        // Edge path extrusion - create box tube along selected edges
        ImGui::Separator();
        ImGui::Text("Edge Path Extrusion:");
        static float edgePathBoxSize = 0.1f;
        static float edgePathTaper = 1.0f;  // 1.0 = no taper, 0.0 = taper to point
        static bool edgePathAutoUV = true;  // Auto-generate cylindrical UVs
        ImGui::SliderFloat("Box Size", &edgePathBoxSize, 0.01f, 1.0f, "%.3f");
        ImGui::SliderFloat("Taper", &edgePathTaper, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("1.0 = uniform size\n0.0 = taper to point\n0.5 = end is half the start size");
        }
        ImGui::Checkbox("Auto UV", &edgePathAutoUV);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Generate cylindrical UVs:\nU = around tube (0-1)\nV = along path (0-1)");
        }

        if (ImGui::Button("Extrude Box Along Path")) {
            extrudeBoxAlongSelectedEdges(edgePathBoxSize, edgePathTaper, edgePathAutoUV);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select connected edges to form a path,\nthen create a box tube along that path");
        }

        // Pipe Network extrusion (tubes pass through at junctions)
        ImGui::Spacing();
        ImGui::Text("Pipe Network:");
        static float pipeBoxSize = 0.1f;  // Tube cross-section size
        ImGui::SliderFloat("Pipe Size##pipe", &pipeBoxSize, 0.01f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Tube cross-section size");
        }

        if (ImGui::Button("Extrude Pipe Network")) {
            extrudePipeNetwork(pipeBoxSize, 1.0f, edgePathAutoUV);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create pipe network from selected edges.\nTubes pass through each other at junctions.\nUses mesh color from Display Options.");
        }
        ImGui::Separator();

        if (!hasEdgeSelection) ImGui::EndDisabled();

        // Vertex-specific operations
        // (selectedVerts already declared above in Transform section)
        if (selectedVerts.size() < 2) ImGui::BeginDisabled();
        if (ImGui::Button("Merge Vertices (Alt+M)")) {
            if (selectedVerts.size() >= 2) {
                m_ctx.editableMesh.saveState();
                m_ctx.editableMesh.mergeSelectedVertices();
                m_ctx.meshDirty = true;
            }
        }
        if (selectedVerts.size() < 2) ImGui::EndDisabled();

        if (selectedVerts.empty()) ImGui::BeginDisabled();
        if (ImGui::Button("Snap to X=0")) {
            m_ctx.editableMesh.saveState();
            for (uint32_t vi : selectedVerts) {
                m_ctx.editableMesh.setVertexPosition(vi,
                    glm::vec3(0.0f,
                              m_ctx.editableMesh.getVertex(vi).position.y,
                              m_ctx.editableMesh.getVertex(vi).position.z));
            }
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Set X=0 on selected vertices.\nUse before Mirror Merge X to align the center seam.");
        }
        if (selectedVerts.empty()) ImGui::EndDisabled();

        // Measurement display (when exactly 2 vertices selected)
        if (selectedVerts.size() == 2 && m_ctx.selectedObject) {
            auto it = selectedVerts.begin();
            uint32_t v1 = *it++;
            uint32_t v2 = *it;
            const auto& vert1 = m_ctx.editableMesh.getVertex(v1);
            const auto& vert2 = m_ctx.editableMesh.getVertex(v2);

            // Apply object transform to get world-space positions
            glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
            glm::vec3 worldPos1 = glm::vec3(modelMatrix * glm::vec4(vert1.position, 1.0f));
            glm::vec3 worldPos2 = glm::vec3(modelMatrix * glm::vec4(vert2.position, 1.0f));

            glm::vec3 delta = worldPos2 - worldPos1;
            float distance = glm::length(delta);

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Measure (2 verts)");
            ImGui::Separator();

            // Total distance in multiple units
            float distanceCm = distance * 100.0f;
            float distanceFeet = distance * 3.28084f;
            float distanceInches = distance * 39.3701f;

            ImGui::Text("Total: %.3f m", distance);
            ImGui::Text("       %.1f cm  |  %.2f ft  |  %.1f in", distanceCm, distanceFeet, distanceInches);

            // X/Y/Z deltas
            ImGui::Spacing();
            ImGui::TextDisabled("Deltas:");
            ImGui::Text("  X: %+.3f m (%+.1f cm)", delta.x, delta.x * 100.0f);
            ImGui::Text("  Y: %+.3f m (%+.1f cm)", delta.y, delta.y * 100.0f);
            ImGui::Text("  Z: %+.3f m (%+.1f cm)", delta.z, delta.z * 100.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Mesh-wide Operations section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.05f, 0.0f, 0.5f));
        ImGui::BeginChild("MeshOpsSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Mesh Operations");
        ImGui::Separator();

        if (ImGui::Button("Hollow (H)")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.hollow(m_ctx.hollowThickness);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create interior walls with inward-facing normals.\nUseful for buildings and hollow objects.");
        }
        ImGui::SameLine();
        ImGui::SliderFloat("##hollow", &m_ctx.hollowThickness, 0.01f, 1.0f, "%.3f");

        if (ImGui::Button("Clean Orphaned Vertices")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.removeOrphanedVertices();
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove vertices not used by any face.\nFixes floating vertex dots after face deletion.");
        }

        if (ImGui::Button("Mirror Merge X")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.mirrorMergeX(0.001f);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mirror mesh across X=0 and weld seam vertices.\nModel one half, mirror to get a symmetric mesh.");
        }

        // Boolean Cut - use another object as a cutter
        ImGui::Spacing();
        ImGui::Text("Boolean Cut:");

        // Find potential cutter objects (any object that isn't selected)
        static int cutterObjectIndex = -1;
        std::vector<std::pair<int, std::string>> cutterOptions;
        for (size_t i = 0; i < m_ctx.sceneObjects.size(); ++i) {
            if (m_ctx.sceneObjects[i].get() != m_ctx.selectedObject) {
                cutterOptions.push_back({static_cast<int>(i), m_ctx.sceneObjects[i]->getName()});
            }
        }

        if (cutterOptions.empty()) {
            ImGui::TextDisabled("Add another object as cutter");
        } else {
            // Combo to select cutter
            std::string currentCutter = (cutterObjectIndex >= 0 && cutterObjectIndex < static_cast<int>(m_ctx.sceneObjects.size()))
                ? m_ctx.sceneObjects[cutterObjectIndex]->getName() : "Select cutter...";

            if (ImGui::BeginCombo("Cutter", currentCutter.c_str())) {
                for (const auto& [idx, name] : cutterOptions) {
                    bool selected = (cutterObjectIndex == idx);
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        cutterObjectIndex = idx;
                    }
                }
                ImGui::EndCombo();
            }

            bool hasCutter = cutterObjectIndex >= 0 && cutterObjectIndex < static_cast<int>(m_ctx.sceneObjects.size())
                && m_ctx.sceneObjects[cutterObjectIndex].get() != m_ctx.selectedObject;

            if (!hasCutter) ImGui::BeginDisabled();
            if (ImGui::Button("Cut Boolean")) {
                if (hasCutter && m_ctx.selectedObject) {
                    m_ctx.editableMesh.saveState();

                    // Get cutter object's world-space bounding box
                    SceneObject* cutter = m_ctx.sceneObjects[cutterObjectIndex].get();
                    const auto& cutterVerts = cutter->getVertices();
                    glm::mat4 cutterMatrix = cutter->getTransform().getMatrix();

                    // Transform to main mesh's local space
                    glm::mat4 mainInverse = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                    glm::mat4 toLocal = mainInverse * cutterMatrix;

                    glm::vec3 cutterMin(FLT_MAX), cutterMax(-FLT_MAX);
                    for (const auto& v : cutterVerts) {
                        glm::vec3 worldPos = glm::vec3(toLocal * glm::vec4(v.position, 1.0f));
                        cutterMin = glm::min(cutterMin, worldPos);
                        cutterMax = glm::max(cutterMax, worldPos);
                    }

                    m_ctx.editableMesh.booleanCut(cutterMin, cutterMax);
                    m_ctx.meshDirty = true;
                }
            }
            if (!hasCutter) ImGui::EndDisabled();

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cut a hole through the selected mesh\nusing the cutter object's bounding box.\nBest for axis-aligned doors/windows.");
            }
        }

        // Catmull-Clark Subdivision with level navigation
        ImGui::Spacing();
        ImGui::Text("Subdivision:");
        static int currentSubdivLevel = 0;
        static std::vector<HEVertex> subdivBaseVerts;
        static std::vector<HalfEdge> subdivBaseHE;
        static std::vector<HEFace> subdivBaseFaces;
        static bool hasSubdivBase = false;

        int newLevel = currentSubdivLevel;
        ImGui::SliderInt("##subdivLevel", &newLevel, 0, 4, "Level %d");
        if (newLevel != currentSubdivLevel) {
            m_ctx.editableMesh.saveState();

            // Store base mesh when first leaving level 0
            if (!hasSubdivBase && currentSubdivLevel == 0 && newLevel > 0) {
                subdivBaseVerts = m_ctx.editableMesh.getVerticesData();
                subdivBaseHE = m_ctx.editableMesh.getHalfEdges();
                subdivBaseFaces = m_ctx.editableMesh.getFacesData();
                hasSubdivBase = true;
            }

            if (newLevel == 0 && hasSubdivBase) {
                // Restore base mesh
                m_ctx.editableMesh.setFromData(subdivBaseVerts, subdivBaseHE, subdivBaseFaces);
                hasSubdivBase = false;
            } else if (newLevel > 0 && hasSubdivBase) {
                // Restore base, then subdivide to target level
                m_ctx.editableMesh.setFromData(subdivBaseVerts, subdivBaseHE, subdivBaseFaces);
                m_ctx.editableMesh.catmullClarkSubdivide(newLevel);
            } else if (newLevel > currentSubdivLevel) {
                // No base stored (shouldn't happen), just subdivide the difference
                m_ctx.editableMesh.catmullClarkSubdivide(newLevel - currentSubdivLevel);
            }

            currentSubdivLevel = newLevel;
            m_ctx.meshDirty = true;
            invalidateWireframeCache();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Catmull-Clark subdivision level.\nDrag to change level. 0 = base mesh.\nGoing down restores the base mesh.\nEdit at level 0, then raise level to re-smooth.");
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // UV Operations section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.05f, 0.15f, 0.5f));
        ImGui::BeginChild("UVProjectionSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1), "UV Projection");
        ImGui::Separator();

        // Planar projection from camera view (selected faces only)
        bool hasFaceSelection = !m_ctx.editableMesh.getSelectedFaces().empty();
        if (!hasFaceSelection) ImGui::BeginDisabled();
        if (ImGui::Button("Planar (View)")) {
            m_ctx.editableMesh.saveState();
            // Use active camera (works for both perspective and ortho)
            Camera& cam = m_ctx.getActiveCamera();
            glm::vec3 viewDir = cam.getFront();
            glm::vec3 viewUp = cam.getUp();
            m_ctx.editableMesh.projectSelectedFacesFromView(viewDir, viewUp, m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Project selected faces from current camera view");
        }
        if (!hasFaceSelection) ImGui::EndDisabled();

        // Ortho view planar projections
        if (!hasFaceSelection) ImGui::BeginDisabled();
        ImGui::Text("Planar from:");
        if (ImGui::Button("Front")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(0, 0, -1), glm::vec3(0, 1, 0), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Back")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(0, 0, 1), glm::vec3(0, 1, 0), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Left")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Right")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        if (ImGui::Button("Top")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(0, -1, 0), glm::vec3(0, 0, -1), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Bottom")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.projectSelectedFacesFromView(glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        if (!hasFaceSelection) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Box")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.boxProjectUVs(m_ctx.uvProjectionScale);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Project all UVs based on face normal (6-sided box)");
        }

        ImGui::SameLine();
        if (ImGui::Button("By Normal")) {
            m_ctx.editableMesh.saveState();
            // Use small tolerance (0.001) to group only faces with nearly identical normals
            // For a box, this creates 6 islands (one per face direction)
            m_ctx.editableMesh.planarProjectByNormal(0.001f, m_ctx.uvIslandMargin);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Group faces by identical normals into separate UV islands");
        }

        ImGui::SameLine();
        if (ImGui::Button("Uniform")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.uniformSquareUVs(m_ctx.uvIslandMargin);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Every face gets identical square UV - stamps look the same on all faces");
        }

        // Cylindrical projection
        if (ImGui::Button("Cylindrical")) {
            m_ctx.editableMesh.saveState();
            // Axis hint based on combo selection, usePCA enabled by default
            glm::vec3 axisHint = m_ctx.cylinderAxisHint;
            m_ctx.editableMesh.cylindricalProjectUVs(axisHint, m_ctx.cylinderUsePCA);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cylindrical UV projection (uses selected faces or all if none selected).\nSeam placed at 'back' of cylinder where theta wraps.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        const char* axisItems[] = { "Y", "X", "Z" };
        if (ImGui::Combo("##CylAxis", &m_ctx.cylinderAxisIndex, axisItems, 3)) {
            // Update axis hint based on selection
            switch (m_ctx.cylinderAxisIndex) {
                case 0: m_ctx.cylinderAxisHint = glm::vec3(0, 1, 0); break;
                case 1: m_ctx.cylinderAxisHint = glm::vec3(1, 0, 0); break;
                case 2: m_ctx.cylinderAxisHint = glm::vec3(0, 0, 1); break;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cylinder axis hint (PCA will refine if enabled)");
        }
        ImGui::SameLine();
        ImGui::Checkbox("PCA", &m_ctx.cylinderUsePCA);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Use PCA to auto-detect cylinder axis from vertex positions");
        }

        // Per-face projection (each face = own island)
        if (ImGui::Button("Per-Face")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.perFaceProjectUVs(m_ctx.uvIslandMargin);
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Each face becomes its own UV island, packed in a grid.\nIdeal for manual sewing workflow.");
        }

        ImGui::SameLine();
        {
            // Check if we have selected faces
            auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
            bool hasSelection = !selectedFaces.empty();

            std::string buttonLabel = hasSelection ?
                "Sew Selected (Exp)##sewexp" : "Sew All (Exp)##sewexp";

            if (ImGui::Button(buttonLabel.c_str())) {
                m_ctx.editableMesh.saveState();
                int sewnEdges = m_ctx.editableMesh.sewAllUVs(selectedFaces);
                m_ctx.meshDirty = true;
                std::cout << "Sew All: " << sewnEdges << " edges sewn" << std::endl;
            }
            if (ImGui::IsItemHovered()) {
                if (hasSelection) {
                    ImGui::SetTooltip("EXPERIMENTAL: Applies per-face UVs to SELECTED faces,\nthen sews shared edges. Skips edges that would overlap.");
                } else {
                    ImGui::SetTooltip("EXPERIMENTAL: First applies per-face UVs, then attempts to sew\nall shared edges together. Skips edges that would cause UV overlap.");
                }
            }
        }

        ImGui::SliderFloat("UV Scale", &m_ctx.uvProjectionScale, 0.1f, 10.0f, "%.1f");

        ImGui::Separator();
        if (ImGui::Button("Auto-UV Cubes (U)")) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.autoUVCubes();
            m_ctx.meshDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Auto-UV for cube-based meshes.\nEach cube (24 verts) becomes a packed UV island with 6 faces.");
        }

        ImGui::Separator();

        // Seam Buster section
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "Seam Buster");
        bool canSeamBust = m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData();
        if (!canSeamBust) ImGui::BeginDisabled();

        ImGui::SliderInt("Pixels", &m_ctx.seamBusterPixels, 1, 16);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of pixels to extend beyond UV island edges");
        }

        if (ImGui::Button("Apply Seam Buster")) {
            if (canSeamBust) {
                m_ctx.selectedObject->saveTextureState();
                m_ctx.selectedObject->applySeamBuster(m_ctx.seamBusterPixels);
                m_ctx.selectedObject->markTextureModified();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Extend edge pixels outward to prevent texture seams.\nSamples colors from UV island edges and paints them\nbeyond the boundary to eliminate mipmap artifacts.");
        }

        if (!canSeamBust) ImGui::EndDisabled();

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Undo/Redo section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.1f, 0.15f, 0.5f));
        ImGui::BeginChild("HistorySection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1), "History");
        ImGui::Separator();

        if (ImGui::Button("Undo (Ctrl+Z)")) {
            if (m_ctx.editableMesh.undo()) {
                m_ctx.meshDirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo (Ctrl+Shift+Z)")) {
            if (m_ctx.editableMesh.redo()) {
                m_ctx.meshDirty = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Display settings section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.12f, 0.05f, 0.5f));
        ImGui::BeginChild("DisplaySection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "Display");
        ImGui::Separator();

        ImGui::Checkbox("Wireframe", &m_ctx.showModelingWireframe);
        ImGui::Checkbox("Face Normals", &m_ctx.showFaceNormals);
        ImGui::Checkbox("Grid", &m_ctx.showGrid);

        if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
            ImGui::SliderFloat("Vertex Size", &m_ctx.vertexDisplaySize, 0.01f, 0.2f, "%.2f");
        }

        ImGui::Spacing();
        ImGui::Text("Colors");

        ImGui::ColorEdit3("Background", &m_ctx.backgroundColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Mesh", &m_ctx.defaultMeshColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("Random", &m_ctx.randomMeshColors);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Each new primitive gets a random color");
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Texture Painting section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.05f, 0.1f, 0.5f));
        ImGui::BeginChild("TexturePaintSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.7f, 1), "Texture Painting");
        ImGui::Separator();

        bool hasTexture = m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData();

        // New texture button
        if (ImGui::Button("New Texture")) {
            ImGui::OpenPopup("NewTexturePopup");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create a blank texture to paint on");
        }

        // New texture popup
        if (ImGui::BeginPopup("NewTexturePopup")) {
            static int texSize = 512;
            ImGui::Text("Texture Size:");
            ImGui::RadioButton("256", &texSize, 256);
            ImGui::SameLine();
            ImGui::RadioButton("512", &texSize, 512);
            ImGui::SameLine();
            ImGui::RadioButton("1024", &texSize, 1024);
            ImGui::SameLine();
            ImGui::RadioButton("2048", &texSize, 2048);

            if (ImGui::Button("Create")) {
                if (m_ctx.selectedObject) {
                    // Create blank white texture
                    std::vector<unsigned char> texData(texSize * texSize * 4, 255);
                    m_ctx.selectedObject->setTextureData(texData, texSize, texSize);

                    // Upload to GPU
                    uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                    m_ctx.modelRenderer.updateTexture(handle, texData.data(), texSize, texSize);

                    // Reset vertex colors to white so they don't bleed through the texture
                    m_ctx.editableMesh.setAllVertexColors(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                    m_ctx.meshDirty = true;

                    std::cout << "Created " << texSize << "x" << texSize << " blank texture" << std::endl;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Delete texture button (only if texture exists)
        ImGui::SameLine();
        if (!hasTexture) ImGui::BeginDisabled();
        if (ImGui::Button("Delete Texture")) {
            if (m_ctx.selectedObject) {
                // Turn off paint mode first
                m_ctx.isPainting = false;
                // Defer texture destruction to start of next frame
                // (calling destroyTexture here would hang — vkDeviceWaitIdle during active render pass)
                m_ctx.pendingTextureDelete = true;
            }
        }
        if (!hasTexture) ImGui::EndDisabled();

        // Display texture size
        if (hasTexture) {
            ImGui::SameLine();
            ImGui::TextDisabled("%dx%d", m_ctx.selectedObject->getTextureWidth(),
                               m_ctx.selectedObject->getTextureHeight());
        }

        // Paint controls (only if texture exists)
        if (!hasTexture) {
            ImGui::BeginDisabled();
            // Force paint mode off if object has no texture
            m_ctx.isPainting = false;
        }

        ImGui::Checkbox("Paint Mode (P)", &m_ctx.isPainting);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hold Alt + Click to sample colors (eyedropper)");
        }

        // Brush mode selection
        if (ImGui::Checkbox("Use Stamp", &m_ctx.useStamp)) {
            if (m_ctx.useStamp) { m_ctx.useSmear = false; }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to stamp an image onto the texture");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Use Smear", &m_ctx.useSmear)) {
            if (m_ctx.useSmear) { m_ctx.useStamp = false; }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag to smear/smudge colors like finger painting");
        }
        if (m_ctx.useStamp) {
            // Stamp controls
            if (ImGui::Button("Load Stamp...")) {
                nfdchar_t* outPath = nullptr;
                nfdfilteritem_t filters[1] = {{"Image", "png,jpg,jpeg,bmp,tga"}};
                if (NFD_OpenDialog(&outPath, filters, 1, nullptr) == NFD_OKAY) {
                    int w, h, channels;
                    unsigned char* data = stbi_load(outPath, &w, &h, &channels, 4);
                    if (data) {
                        m_ctx.stampData.assign(data, data + w * h * 4);
                        m_ctx.stampWidth = w;
                        m_ctx.stampHeight = h;
                        // Create preview texture
                        if (m_ctx.updateStampPreviewCallback) {
                            m_ctx.updateStampPreviewCallback(data, w, h);
                        }
                        stbi_image_free(data);
                        std::cout << "Loaded stamp: " << w << "x" << h << std::endl;
                    }
                    NFD_FreePath(outPath);
                }
            }
            if (!m_ctx.stampData.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%dx%d", m_ctx.stampWidth, m_ctx.stampHeight);
            }
            ImGui::SliderFloat("Stamp Scale", &m_ctx.stampScale, 0.01f, 3.0f, "%.3f");
            ImGui::SliderFloat("Scale H", &m_ctx.stampScaleH, 0.01f, 3.0f, "%.3f");
            ImGui::SliderFloat("Scale V", &m_ctx.stampScaleV, 0.01f, 3.0f, "%.3f");
            ImGui::SliderFloat("Rotation", &m_ctx.stampRotation, -180.0f, 180.0f, "%.2f deg");
            // Fine rotation control with drag
            ImGui::SameLine();
            ImGui::PushItemWidth(60);
            if (ImGui::DragFloat("##RotFine", &m_ctx.stampRotation, 0.1f, -180.0f, 180.0f, "%.2f")) {
                // Wrap rotation to -180 to 180
                while (m_ctx.stampRotation > 180.0f) m_ctx.stampRotation -= 360.0f;
                while (m_ctx.stampRotation < -180.0f) m_ctx.stampRotation += 360.0f;
            }
            ImGui::PopItemWidth();
            ImGui::SliderFloat("Opacity", &m_ctx.stampOpacity, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Project from View", &m_ctx.stampProjectFromView);

            // Flip buttons
            if (ImGui::Button("Flip H")) {
                m_ctx.stampFlipH = !m_ctx.stampFlipH;
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip V")) {
                m_ctx.stampFlipV = !m_ctx.stampFlipV;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", m_ctx.stampFlipH ? "H " : "", m_ctx.stampFlipV ? "V" : "");

            // Fit to Face mode toggle
            ImGui::Separator();
            ImGui::Checkbox("Fit to Face Mode", &m_ctx.stampFitToFace);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, clicking a face will fit the stamp\nexactly to that face's UV coordinates");
            }
            if (m_ctx.stampFitToFace) {
                // Rotation control for corner alignment
                ImGui::SameLine();
                if (ImGui::Button("Rotate##fitcorner")) {
                    m_ctx.stampFitRotation = (m_ctx.stampFitRotation + 1) % 4;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Rotate stamp corners (current: %d x 90°)", m_ctx.stampFitRotation);
                }
            }

            // Show stamp preview thumbnail
            if (m_ctx.stampPreviewDescriptor != VK_NULL_HANDLE && !m_ctx.stampData.empty()) {
                ImGui::Separator();
                ImGui::Text("Preview:");
                // Calculate preview size maintaining aspect ratio
                float maxPreviewSize = 100.0f;
                float aspect = static_cast<float>(m_ctx.stampWidth) / m_ctx.stampHeight;
                float previewW = aspect >= 1.0f ? maxPreviewSize : maxPreviewSize * aspect;
                float previewH = aspect >= 1.0f ? maxPreviewSize / aspect : maxPreviewSize;
                // Apply flip to preview UV coordinates (match actual stamp behavior)
                ImVec2 uv0(m_ctx.stampFlipH ? 1.0f : 0.0f, m_ctx.stampFlipV ? 0.0f : 1.0f);
                ImVec2 uv1(m_ctx.stampFlipH ? 0.0f : 1.0f, m_ctx.stampFlipV ? 1.0f : 0.0f);
                ImGui::Image((ImTextureID)m_ctx.stampPreviewDescriptor, ImVec2(previewW, previewH), uv0, uv1);
            }
        } else if (m_ctx.useSmear) {
            // Smear controls
            ImGui::SliderFloat("Radius", &m_ctx.paintRadius, 0.0001f, 0.2f, "%.5f");
            ImGui::SliderFloat("Strength", &m_ctx.smearStrength, 0.1f, 1.0f, "%.2f");
            ImGui::SliderFloat("Pickup", &m_ctx.smearPickup, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How much new color to pick up while smearing (0=pure carry, 1=pure sample)");
            }
            ImGui::TextDisabled("Drag to smear colors");
        } else {
            // Regular brush controls
            ImGui::ColorEdit3("Color", &m_ctx.paintColor.x);
            ImGui::SliderFloat("Radius", &m_ctx.paintRadius, 0.0001f, 0.2f, "%.5f");
            ImGui::SliderFloat("Strength", &m_ctx.paintStrength, 0.1f, 1.0f, "%.2f");
            ImGui::Checkbox("Square Brush (Pixel Art)", &m_ctx.squareBrush);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Square brush with no falloff for pixel art style.\nUnchecked = circular brush with soft edges.");
            }

            ImGui::Checkbox("Fill Paint to Face", &m_ctx.fillPaintToFace);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click a face to fill it entirely with the current color.\nDisables brush painting while active.");
            }

            if (!m_ctx.fillPaintToFace) {
                ImGui::TextDisabled("Shift+Click: draw line");
            }

            // Edge Stroke button - paint along selected edges
            auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
            if (selectedEdges.empty()) ImGui::BeginDisabled();
            if (ImGui::Button("Edge Stroke")) {
                if (m_ctx.selectedObject && m_ctx.editableMesh.isValid() && !selectedEdges.empty()) {
                    // Save texture state for undo
                    m_ctx.selectedObject->saveTextureState();

                    int texWidth = m_ctx.selectedObject->getTextureWidth();
                    int texHeight = m_ctx.selectedObject->getTextureHeight();
                    float stepSize = m_ctx.paintRadius * 0.5f;

                    // Build adjacency map: vertex position -> list of connected edges
                    // Use position-based matching to handle UV seams (same 3D pos, different UV)
                    std::map<std::tuple<int,int,int>, std::vector<uint32_t>> posToEdges;
                    auto posKey = [](const glm::vec3& p) {
                        return std::make_tuple(
                            static_cast<int>(p.x * 10000),
                            static_cast<int>(p.y * 10000),
                            static_cast<int>(p.z * 10000)
                        );
                    };

                    for (uint32_t edgeIdx : selectedEdges) {
                        auto edgeVerts = m_ctx.editableMesh.getEdgeVertices(edgeIdx);
                        glm::vec3 p0 = m_ctx.editableMesh.getVertex(edgeVerts.first).position;
                        glm::vec3 p1 = m_ctx.editableMesh.getVertex(edgeVerts.second).position;
                        posToEdges[posKey(p0)].push_back(edgeIdx);
                        posToEdges[posKey(p1)].push_back(edgeIdx);
                    }

                    // Chain edges into paths and paint each path
                    std::set<uint32_t> processedEdges;

                    for (uint32_t startEdge : selectedEdges) {
                        if (processedEdges.count(startEdge)) continue;

                        // Build a path starting from this edge
                        std::vector<glm::vec2> pathUVs;
                        std::set<uint32_t> pathEdges;

                        // Start with this edge
                        auto edgeVerts = m_ctx.editableMesh.getEdgeVertices(startEdge);
                        uint32_t currentVert = edgeVerts.first;
                        uint32_t nextVert = edgeVerts.second;
                        pathUVs.push_back(m_ctx.editableMesh.getVertex(currentVert).uv);
                        pathUVs.push_back(m_ctx.editableMesh.getVertex(nextVert).uv);
                        pathEdges.insert(startEdge);

                        // Extend path forward
                        while (true) {
                            glm::vec3 pos = m_ctx.editableMesh.getVertex(nextVert).position;
                            auto& connectedEdges = posToEdges[posKey(pos)];
                            uint32_t foundEdge = UINT32_MAX;
                            uint32_t foundNextVert = UINT32_MAX;

                            for (uint32_t e : connectedEdges) {
                                if (pathEdges.count(e) || processedEdges.count(e)) continue;
                                auto ev = m_ctx.editableMesh.getEdgeVertices(e);
                                glm::vec3 p0 = m_ctx.editableMesh.getVertex(ev.first).position;
                                glm::vec3 p1 = m_ctx.editableMesh.getVertex(ev.second).position;
                                if (glm::length(p0 - pos) < 0.0001f) {
                                    foundEdge = e;
                                    foundNextVert = ev.second;
                                    break;
                                } else if (glm::length(p1 - pos) < 0.0001f) {
                                    foundEdge = e;
                                    foundNextVert = ev.first;
                                    break;
                                }
                            }

                            if (foundEdge == UINT32_MAX) break;
                            pathEdges.insert(foundEdge);
                            pathUVs.push_back(m_ctx.editableMesh.getVertex(foundNextVert).uv);
                            nextVert = foundNextVert;
                        }

                        // Mark all path edges as processed
                        for (uint32_t e : pathEdges) processedEdges.insert(e);

                        // Paint along the path
                        for (size_t i = 0; i + 1 < pathUVs.size(); i++) {
                            glm::vec2 startUV = pathUVs[i];
                            glm::vec2 endUV = pathUVs[i + 1];

                            float distance = glm::length(endUV - startUV);
                            int steps = static_cast<int>(distance / stepSize) + 1;
                            if (steps < 2) steps = 2;

                            // Don't paint end point except for last segment (avoid double-paint at joints)
                            int endStep = (i + 2 < pathUVs.size()) ? steps - 1 : steps;
                            for (int s = 0; s <= endStep; s++) {
                                float t = static_cast<float>(s) / static_cast<float>(steps);
                                glm::vec2 uv = glm::mix(startUV, endUV, t);
                                m_ctx.selectedObject->paintAt(uv, m_ctx.paintColor, m_ctx.paintRadius, m_ctx.paintStrength, m_ctx.squareBrush);
                            }
                        }
                    }

                    m_ctx.selectedObject->markTextureModified();
                }
            }
            if (selectedEdges.empty()) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu edges)", selectedEdges.size());
            if (ImGui::IsItemHovered() && !selectedEdges.empty()) {
                ImGui::SetTooltip("Paint stroke along all selected edges using current brush settings");
            }

            // Fill Selected Faces button
            auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
            if (selectedFaces.empty()) ImGui::BeginDisabled();
            if (ImGui::Button("Fill Faces")) {
                if (m_ctx.selectedObject && m_ctx.editableMesh.isValid() && !selectedFaces.empty()) {
                    m_ctx.selectedObject->saveTextureState();

                    int texWidth = m_ctx.selectedObject->getTextureWidth();
                    int texHeight = m_ctx.selectedObject->getTextureHeight();
                    auto& texData = m_ctx.selectedObject->getTextureData();

                    // Point-in-polygon test using crossing number
                    auto pointInPolygon = [](const glm::vec2& p, const std::vector<glm::vec2>& poly) {
                        int n = poly.size();
                        int crossings = 0;
                        for (int i = 0; i < n; i++) {
                            int j = (i + 1) % n;
                            if ((poly[i].y <= p.y && poly[j].y > p.y) || (poly[j].y <= p.y && poly[i].y > p.y)) {
                                float t = (p.y - poly[i].y) / (poly[j].y - poly[i].y);
                                if (p.x < poly[i].x + t * (poly[j].x - poly[i].x)) {
                                    crossings++;
                                }
                            }
                        }
                        return (crossings % 2) == 1;
                    };

                    // Fill each selected face
                    for (uint32_t faceIdx : selectedFaces) {
                        auto faceVerts = m_ctx.editableMesh.getFaceVertices(faceIdx);
                        std::vector<glm::vec2> uvPoly;
                        glm::vec2 uvMin(1e9f), uvMax(-1e9f);

                        for (uint32_t vi : faceVerts) {
                            glm::vec2 uv = m_ctx.editableMesh.getVertex(vi).uv;
                            uvPoly.push_back(uv);
                            uvMin = glm::min(uvMin, uv);
                            uvMax = glm::max(uvMax, uv);
                        }

                        // Convert UV bounds to pixel bounds
                        int minPX = std::max(0, static_cast<int>(uvMin.x * texWidth) - 1);
                        int maxPX = std::min(texWidth - 1, static_cast<int>(uvMax.x * texWidth) + 1);
                        int minPY = std::max(0, static_cast<int>(uvMin.y * texHeight) - 1);
                        int maxPY = std::min(texHeight - 1, static_cast<int>(uvMax.y * texHeight) + 1);

                        // Fill pixels inside the face polygon
                        for (int py = minPY; py <= maxPY; py++) {
                            for (int px = minPX; px <= maxPX; px++) {
                                glm::vec2 pixelUV((px + 0.5f) / texWidth, (py + 0.5f) / texHeight);
                                if (pointInPolygon(pixelUV, uvPoly)) {
                                    size_t idx = (py * texWidth + px) * 4;
                                    if (idx + 3 < texData.size()) {
                                        texData[idx] = static_cast<unsigned char>(m_ctx.paintColor.r * 255);
                                        texData[idx + 1] = static_cast<unsigned char>(m_ctx.paintColor.g * 255);
                                        texData[idx + 2] = static_cast<unsigned char>(m_ctx.paintColor.b * 255);
                                        // Keep alpha as is, or set to 255
                                    }
                                }
                            }
                        }
                    }

                    m_ctx.selectedObject->markTextureModified();
                }
            }
            if (selectedFaces.empty()) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu faces)", selectedFaces.size());
            if (ImGui::IsItemHovered() && !selectedFaces.empty()) {
                ImGui::SetTooltip("Fill all selected faces with current paint color");
            }
        }

        if (!hasTexture) ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // UV Re-projection section
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.05f, 0.12f, 0.5f));
        ImGui::BeginChild("UVReprojSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 1.0f, 1), "UV Pack & Re-project");
        ImGui::Separator();
        ImGui::TextDisabled("Shrink existing UVs to a corner, re-project texture");

        {
            bool canPack = m_ctx.selectedObject && m_ctx.editableMesh.isValid()
                           && m_ctx.selectedObject->hasTextureData();

            // Pack scale
            ImGui::SetNextItemWidth(100);
            const char* scaleLabels[] = { "25%", "33%", "50%", "75%" };
            const float scaleValues[] = { 0.25f, 0.33f, 0.5f, 0.75f };
            int scaleIdx = 2;  // default 50%
            for (int i = 0; i < 4; ++i) {
                if (std::abs(m_packScale - scaleValues[i]) < 0.01f) { scaleIdx = i; break; }
            }
            if (ImGui::Combo("UV Scale##pack", &scaleIdx, scaleLabels, 4)) {
                m_packScale = scaleValues[scaleIdx];
            }

            // Corner selection
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            const char* cornerLabels[] = { "Bottom-Left", "Bottom-Right", "Top-Left", "Top-Right" };
            ImGui::Combo("Corner##pack", &m_packCorner, cornerLabels, 4);

            // Texture size
            ImGui::SetNextItemWidth(100);
            const char* sizes[] = { "512", "1024", "2048", "4096" };
            int sizeIdx = (m_reprojectTexSize == 512) ? 0 : (m_reprojectTexSize == 2048) ? 2 : (m_reprojectTexSize == 4096) ? 3 : 1;
            if (ImGui::Combo("Texture Size##pack", &sizeIdx, sizes, 4)) {
                const int vals[] = { 512, 1024, 2048, 4096 };
                m_reprojectTexSize = vals[sizeIdx];
            }

            // One-click button
            if (!canPack) ImGui::BeginDisabled();
            if (ImGui::Button("Pack & Re-project", ImVec2(-1, 30))) {
                packAndReprojectUVs();
            }
            if (!canPack) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("One click: shrinks all UVs to chosen corner,\n"
                                  "re-projects texture onto new UV layout.\n"
                                  "Frees up UV space for new projections.");
            }

            // Manual re-project (for after manual UV edits)
            if (m_hasStoredUVs) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Old UVs stored (%dx%d tex)",
                                   m_storedOldTexW, m_storedOldTexH);
                if (ImGui::Button("Re-project Only")) {
                    reprojectTexture(m_reprojectTexSize);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Re-project using stored UVs onto current UV layout.\n"
                                      "Use after manually editing UVs.");
                }
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Vertex Color Painting section (no UVs needed)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.12f, 0.05f, 0.5f));
        ImGui::BeginChild("VertexColorSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "Vertex Color Painting");
        ImGui::Separator();
        ImGui::TextDisabled("Paint directly on vertices (no UVs needed)");

        // Bake texture to vertex colors button
        {
            bool canBake = m_ctx.selectedObject && m_ctx.editableMesh.isValid()
                           && m_ctx.selectedObject->hasTextureData();
            if (!canBake) ImGui::BeginDisabled();
            if (ImGui::Button("Bake Texture to Vertex Colors")) {
                bakeTextureToVertexColors();
            }
            if (!canBake) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Sample texture at each vertex UV and store as vertex color.\n"
                                  "Removes the texture — mesh renders with vertex colors only.\n"
                                  "Works best on high-poly meshes (10k+ faces).");
            }
        }
        ImGui::Spacing();

        ImGui::Checkbox("Vertex Paint Mode", &m_vertexPaintMode);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Paint colors directly onto mesh vertices.\nNo UV mapping required.\nResolution depends on vertex density.");
        }

        if (!m_vertexPaintMode) ImGui::BeginDisabled();

        ImGui::ColorEdit3("Paint Color##vtx", &m_vertexPaintColor.x);
        ImGui::SliderFloat("Radius##vtx", &m_vertexPaintRadius, 0.01f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Brush radius in local mesh units");
        }
        ImGui::SliderFloat("Strength##vtx", &m_vertexPaintStrength, 0.1f, 1.0f, "%.2f");

        // Fill all vertices with current color
        if (ImGui::Button("Fill All Vertices")) {
            if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
                m_ctx.editableMesh.saveState();
                glm::vec4 color(m_vertexPaintColor, 1.0f);
                for (uint32_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                    m_ctx.editableMesh.getVertex(i).color = color;
                }
                m_ctx.meshDirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Fill Selected")) {
            if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
                m_ctx.editableMesh.saveState();
                glm::vec4 color(m_vertexPaintColor, 1.0f);
                // Get vertices affected by current selection
                auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
                auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();

                std::set<uint32_t> vertsToFill;
                for (uint32_t v : selectedVerts) vertsToFill.insert(v);
                for (uint32_t f : selectedFaces) {
                    auto faceVerts = m_ctx.editableMesh.getFaceVertices(f);
                    for (uint32_t v : faceVerts) vertsToFill.insert(v);
                }

                for (uint32_t v : vertsToFill) {
                    m_ctx.editableMesh.getVertex(v).color = color;
                }
                m_ctx.meshDirty = true;
            }
        }

        if (!m_vertexPaintMode) ImGui::EndDisabled();

        // Handle vertex painting input
        if (m_vertexPaintMode && m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
            bool imguiCapture = ImGui::GetIO().WantCaptureMouse;
            if (Input::isMouseButtonDown(0) && !imguiCapture) {
                glm::vec3 rayOrigin, rayDir;
                m_ctx.getMouseRay(rayOrigin, rayDir);

                // Transform ray to local space
                glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
                glm::mat4 invModel = glm::inverse(modelMatrix);
                glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
                glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

                // Raycast to find hit point
                auto hit = m_ctx.editableMesh.raycastFace(localRayOrigin, localRayDir);

                if (hit.hit) {
                    // Save state only on mouse press (not during drag)
                    if (!m_vertexPaintingActive) {
                        m_ctx.editableMesh.saveState();
                        m_vertexPaintingActive = true;
                    }

                    glm::vec3 hitPos = hit.position;
                    glm::vec4 paintCol(m_vertexPaintColor, 1.0f);

                    // Find all vertices within radius and paint them
                    for (uint32_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                        HEVertex& v = m_ctx.editableMesh.getVertex(i);
                        float dist = glm::length(v.position - hitPos);

                        if (dist < m_vertexPaintRadius) {
                            // Calculate falloff (1 at center, 0 at edge)
                            float falloff = 1.0f - (dist / m_vertexPaintRadius);
                            falloff = falloff * falloff;  // Quadratic falloff for smoother edges
                            float blend = falloff * m_vertexPaintStrength;

                            // Blend color
                            v.color = glm::mix(v.color, paintCol, blend);
                        }
                    }

                    m_ctx.meshDirty = true;
                }
            } else {
                // Mouse released, reset painting state for next stroke
                m_vertexPaintingActive = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Button to open AI Generate window
    if (ImGui::Button("AI Generate (Hunyuan3D)", ImVec2(-1, 0))) {
        m_ctx.showAIGenerateWindow = true;
    }

    ImGui::End();
    }

    // ===== AI Generate Window (standalone) =====
    if (m_ctx.showAIGenerateWindow) {
    ImGui::SetNextWindowPos(ImVec2(400, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("AI Generate (Hunyuan3D)##window", &m_ctx.showAIGenerateWindow)) {

        // -- Server control row --
        bool serverOn = m_ctx.aiServerRunning;
        if (ImGui::Checkbox("Server##hunyuan", &serverOn)) {
            if (m_ctx.toggleServerCallback) {
                m_ctx.toggleServerCallback(m_generateLowVRAM, false);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(serverOn ? "Click to stop Hunyuan3D server" : "Click to start Hunyuan3D server");
        }
        ImGui::SameLine();
        if (serverOn && m_ctx.aiServerReady) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Ready");
        } else if (serverOn) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Starting...");
        } else {
            ImGui::TextDisabled("Stopped");
        }
        ImGui::SameLine(0, 20);
        ImGui::Checkbox("Low VRAM##srv", &m_generateLowVRAM);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mini model + CPU offload for texture (recommended for 12GB).\nStop and restart server after changing.");
        }

        ImGui::Separator();

        // -- Prompt --
        ImGui::Text("Prompt:");
        ImGui::InputTextMultiline("##prompt", m_generatePrompt, sizeof(m_generatePrompt),
            ImVec2(-1, 40), ImGuiInputTextFlags_None);

        // -- Image input mode tabs --
        if (ImGui::BeginTabBar("##imgmode")) {
            if (ImGui::BeginTabItem("Single Image")) {
                m_generateMultiView = false;
                if (ImGui::Button("Browse##single")) {
                    nfdchar_t* outPath = nullptr;
                    nfdfilteritem_t filters[1] = {{"Images", "png,jpg,jpeg,bmp"}};
                    if (NFD_OpenDialog(&outPath, filters, 1, nullptr) == NFD_OKAY) {
                        m_generateImagePath = outPath;
                        NFD_FreePath(outPath);
                    }
                }
                ImGui::SameLine();
                if (!m_generateImagePath.empty()) {
                    std::string fn = m_generateImagePath.substr(m_generateImagePath.find_last_of("/\\") + 1);
                    ImGui::Text("%s", fn.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##clrsingle")) m_generateImagePath.clear();
                } else {
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No image (required)");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Multi-View")) {
                m_generateMultiView = true;

                // Helper lambda for browse + display
                auto browseSlot = [](const char* label, const char* btnId, const char* clrId,
                                     std::string& path, bool required) {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(80);
                    std::string browseLabel = std::string("Browse##") + btnId;
                    if (ImGui::SmallButton(browseLabel.c_str())) {
                        nfdchar_t* outPath = nullptr;
                        nfdfilteritem_t filters[1] = {{"Images", "png,jpg,jpeg,bmp"}};
                        if (NFD_OpenDialog(&outPath, filters, 1, nullptr) == NFD_OKAY) {
                            path = outPath;
                            NFD_FreePath(outPath);
                        }
                    }
                    ImGui::SameLine();
                    if (!path.empty()) {
                        std::string fn = path.substr(path.find_last_of("/\\") + 1);
                        ImGui::Text("%s", fn.c_str());
                        ImGui::SameLine();
                        std::string clrLabel = std::string("X##") + clrId;
                        if (ImGui::SmallButton(clrLabel.c_str())) path.clear();
                    } else if (required) {
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(required)");
                    } else {
                        ImGui::TextDisabled("(uses front)");
                    }
                };

                browseSlot("Front:",  "mvfront",  "clrfront",  m_generateFrontPath, true);
                browseSlot("Left:",   "mvleft",   "clrleft",   m_generateLeftPath,  false);
                browseSlot("Right:",  "mvright",  "clrright",  m_generateRightPath, false);
                browseSlot("Back:",   "mvback",   "clrback",   m_generateBackPath,  false);

                if (ImGui::SmallButton("Clear All##mvclr")) {
                    m_generateFrontPath.clear();
                    m_generateLeftPath.clear();
                    m_generateRightPath.clear();
                    m_generateBackPath.clear();
                }

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // -- Key settings (always visible) --
        ImGui::SliderInt("Max Faces", &m_generateMaxFaces, 1000, 100000);

        ImGui::Checkbox("Texture", &m_generateTexture);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Generate texture (loaded on-demand, adds ~30-60s)");
        }
        if (m_generateTexture) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            const char* texSizes[] = {"512", "1024", "2048"};
            int texIdx = (m_generateTexSize == 512) ? 0 : (m_generateTexSize == 2048) ? 2 : 1;
            if (ImGui::Combo("##texsize", &texIdx, texSizes, 3)) {
                m_generateTexSize = (texIdx == 0) ? 512 : (texIdx == 2) ? 2048 : 1024;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Texture resolution (higher = better quality, more VRAM)");
            }
        }

        ImGui::Checkbox("Remove Background", &m_generateRemBG);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Auto-remove background from input image.\nDisable if your image already has a transparent/clean background.");
        }

        // -- Advanced settings --
        if (ImGui::TreeNode("Advanced##aigen")) {
            ImGui::SliderInt("Steps", &m_generateSteps, 1, 50);
            ImGui::SliderInt("Octree Res", &m_generateOctreeRes, 64, 512);
            ImGui::SliderFloat("Guidance", &m_generateGuidance, 1.0f, 20.0f, "%.1f");
            ImGui::InputInt("Seed", &m_generateSeed);
            ImGui::TreePop();
        }

        // -- Generate / Cancel --
        bool isGenerating = m_ctx.aiGenerating;
        bool hasImage = m_generateMultiView ? !m_generateFrontPath.empty() : !m_generateImagePath.empty();
        bool canGenerate = m_ctx.aiServerReady && !isGenerating && hasImage;

        if (!canGenerate) ImGui::BeginDisabled();
        if (ImGui::Button("Generate", ImVec2(ImGui::GetContentRegionAvail().x * 0.65f, 32))) {
            if (m_ctx.generateModelCallback) {
                // Pass front image path for both single and multi-view (server needs at least front)
                std::string primaryImage = m_generateMultiView ? m_generateFrontPath : m_generateImagePath;
                m_ctx.generateModelCallback(std::string(m_generatePrompt), primaryImage);
            }
        }
        if (!canGenerate) {
            ImGui::EndDisabled();
            if (!hasImage && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(m_generateMultiView ?
                    "Front image is required for multi-view generation" :
                    "Browse an image first — image input required");
            }
        }

        if (isGenerating) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(-1, 32))) {
                if (m_ctx.cancelGenerationCallback) {
                    m_ctx.cancelGenerationCallback();
                }
            }
        }

        // -- Status --
        if (!m_ctx.aiGenerateStatus.empty()) {
            ImVec4 statusColor = isGenerating ? ImVec4(1, 1, 0, 1) : ImVec4(0.5f, 1, 0.5f, 1);
            ImGui::TextColored(statusColor, "%s", m_ctx.aiGenerateStatus.c_str());
        }

        ImGui::Separator();

        // -- Server Output Log --
        ImGui::Text("Server Output:");
        float logHeight = ImGui::GetContentRegionAvail().y - 4;
        if (logHeight < 80) logHeight = 80;
        ImGui::BeginChild("##serverlog", ImVec2(-1, logHeight), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));

        if (m_ctx.aiLogLines.empty()) {
            ImGui::TextDisabled("No output yet. Start server and generate to see progress.");
        } else {
            for (const auto& line : m_ctx.aiLogLines) {
                // Color code lines
                if (line.find("ERROR") != std::string::npos || line.find("error") != std::string::npos ||
                    line.find("OOM") != std::string::npos || line.find("Failed") != std::string::npos) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    ImGui::TextWrapped("%s", line.c_str());
                    ImGui::PopStyleColor();
                } else if (line.find("DONE") != std::string::npos) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    ImGui::TextWrapped("%s", line.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextWrapped("%s", line.c_str());
                }
            }
            // Auto-scroll to bottom
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
                ImGui::SetScrollHereY(1.0f);
            }
        }

        ImGui::PopStyleColor();
        ImGui::EndChild();

    }
    ImGui::End();
    }

    // Camera window
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(m_ctx.window.getWidth()) - 220.0f, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 280), ImGuiCond_FirstUseEver);

    if (m_ctx.showCameraWindow) {
    if (ImGui::Begin("Camera", &m_ctx.showCameraWindow)) {
        bool isPerspective = m_ctx.camera.getProjectionMode() == ProjectionMode::Perspective;

        // View presets - ortho buttons affect right viewport (camera2), persp affects left (camera)
        if (ImGui::CollapsingHeader("View Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
            glm::vec3 viewCenter = m_ctx.selectedObject ? m_ctx.selectedObject->getTransform().getPosition() : glm::vec3(0);

            ImGui::TextDisabled("Left: Perspective | Right: Ortho");

            float btnWidth = 45.0f;
            // Row 1: Top, Front, Right
            if (ImGui::Button("Top", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Top;
                m_ctx.camera2.setViewPreset(ViewPreset::Top, viewCenter);
            }
            ImGui::SameLine();
            if (ImGui::Button("Front", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Front;
                m_ctx.camera2.setViewPreset(ViewPreset::Front, viewCenter);
            }
            ImGui::SameLine();
            if (ImGui::Button("Right", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Right;
                m_ctx.camera2.setViewPreset(ViewPreset::Right, viewCenter);
            }
            ImGui::SameLine();
            if (ImGui::Button("Persp", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = false;
                m_ctx.camera.setProjectionMode(ProjectionMode::Perspective);
                m_ctx.camera.setPosition(viewCenter + glm::vec3(3, 2, 5));
                m_ctx.camera.setYaw(-120.0f);
                m_ctx.camera.setPitch(-15.0f);
            }

            // Row 2: Bottom, Back, Left
            if (ImGui::Button("Botm", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Bottom;
                m_ctx.camera2.setViewPreset(ViewPreset::Bottom, viewCenter);
            }
            ImGui::SameLine();
            if (ImGui::Button("Back", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Back;
                m_ctx.camera2.setViewPreset(ViewPreset::Back, viewCenter);
            }
            ImGui::SameLine();
            if (ImGui::Button("Left", ImVec2(btnWidth, 0))) {
                m_ctx.splitView = true;
                m_ctx.splitOrthoPreset = ViewPreset::Left;
                m_ctx.camera2.setViewPreset(ViewPreset::Left, viewCenter);
            }

            ImGui::Separator();
        }

        // Split view controls
        if (ImGui::Checkbox("Split View", &m_ctx.splitView)) {
            if (m_ctx.splitView) {
                glm::vec3 viewCenter = m_ctx.selectedObject ? m_ctx.selectedObject->getTransform().getPosition() : glm::vec3(0);
                m_ctx.camera2.setViewPreset(m_ctx.splitOrthoPreset, viewCenter);
                // Ensure left camera stays perspective
                m_ctx.camera.setProjectionMode(ProjectionMode::Perspective);
            }
        }

        if (m_ctx.splitView) {
            ImGui::SameLine();
            ImGui::TextDisabled(m_ctx.activeViewportLeft ? "(Left active)" : "(Right active)");

            const char* presetNames[] = { "Top", "Bottom", "Front", "Back", "Right", "Left" };
            int presetIndex = static_cast<int>(m_ctx.splitOrthoPreset) - 1;
            if (presetIndex < 0) presetIndex = 0;
            if (ImGui::Combo("Right View", &presetIndex, presetNames, 6)) {
                m_ctx.splitOrthoPreset = static_cast<ViewPreset>(presetIndex + 1);
                glm::vec3 viewCenter = m_ctx.selectedObject ? m_ctx.selectedObject->getTransform().getPosition() : glm::vec3(0);
                m_ctx.camera2.setViewPreset(m_ctx.splitOrthoPreset, viewCenter);
            }
        }

        ImGui::Separator();
        ImGui::SliderFloat("Speed", &m_ctx.cameraSpeed, 0.01f, 0.2f, "%.3f");

        // Tumble style toggle
        ImGui::Checkbox("Mouse-Look Tumble", &m_ctx.mouseLookMode);

        // Reference Images section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Reference Images")) {
            const char* viewNames[] = {"Top", "Bottom", "Front", "Back", "Right", "Left"};

            for (int i = 0; i < 6; i++) {
                auto& ref = m_ctx.referenceImages[i];
                ImGui::PushID(i);

                // View name and load button
                ImGui::Text("%s:", viewNames[i]);
                ImGui::SameLine(70);

                if (ref.loaded) {
                    ImGui::Checkbox("##vis", &ref.visible);
                    ImGui::SameLine();
                    ImGui::Text("%s", ref.name.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        if (m_ctx.clearReferenceImageCallback) {
                            m_ctx.clearReferenceImageCallback(i);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove reference image");
                    }

                    // Controls for loaded image
                    ImGui::SliderFloat("Opacity", &ref.opacity, 0.0f, 1.0f, "%.2f");
                    ImGui::DragFloat2("Offset", &ref.offset.x, 0.1f);
                    ImGui::DragFloat2("Size", &ref.size.x, 0.1f, 0.1f, 50.0f);
                } else {
                    if (ImGui::Button("Load...")) {
                        loadReferenceImage(i);
                    }
                }

                ImGui::PopID();
                if (i < 5) ImGui::Separator();
            }
        }
    }
    ImGui::End();
    }

    // UV Window
    if (m_ctx.showUVWindow) {
        renderModelingUVWindow();
    }

    // Image Reference Window (for color sampling and stamps)
    if (m_ctx.showImageRefWindow) {
        renderImageRefWindow();
    }

    // Draw split view divider
    if (m_ctx.splitView) {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        float screenWidth = static_cast<float>(m_ctx.window.getWidth());
        float screenHeight = static_cast<float>(m_ctx.window.getHeight());
        float centerX = screenWidth / 2.0f;

        drawList->AddLine(ImVec2(centerX, 0), ImVec2(centerX, screenHeight),
                          IM_COL32(100, 100, 100, 255), 2.0f);

        const char* presetNames[] = { "Custom", "Top", "Bottom", "Front", "Back", "Right", "Left" };
        int presetIdx = static_cast<int>(m_ctx.splitOrthoPreset);
        const char* rightLabel = (presetIdx >= 0 && presetIdx < 7) ? presetNames[presetIdx] : "Unknown";

        drawList->AddText(ImVec2(10, 25), IM_COL32(200, 200, 200, 200), "Perspective");
        if (!m_ctx.activeViewportLeft) {
            drawList->AddText(ImVec2(10, 40), IM_COL32(150, 150, 150, 150), "(inactive)");
        }

        drawList->AddText(ImVec2(centerX + 10, 25), IM_COL32(200, 200, 200, 200), rightLabel);
        if (m_ctx.activeViewportLeft) {
            drawList->AddText(ImVec2(centerX + 10, 40), IM_COL32(150, 150, 150, 150), "(inactive)");
        }
    }
}


void ModelingMode::processModelingInput(float deltaTime, bool gizmoActive) {
    // Use IsWindowHovered for mouse-over detection
    bool mouseOverImGui = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);

    // Tab key ALWAYS toggles between object mode and component mode
    // Handle this BEFORE WantCaptureKeyboard check so Tab never goes to ImGui
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);  // Clear Tab from ImGui first
    if (Input::isKeyPressed(Input::KEY_TAB)) {
        m_ctx.objectMode = !m_ctx.objectMode;
        m_modeNotificationTimer = 1.0f;  // Show notification for 1 second
        // Clear selections when switching modes
        if (m_ctx.objectMode) {
            m_ctx.editableMesh.clearSelection();
        }
    }

    // F5 - Quick save to current file
    if (Input::isKeyPressed(Input::KEY_F5)) {
        quickSave();
    }

    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // ESC cancels snap mode
    if (Input::isKeyPressed(Input::KEY_ESCAPE) && m_snapMode) {
        cancelSnapMode();
        return;
    }

    // Select mode (Q key) - no gizmo
    if (Input::isKeyPressed(Input::KEY_Q)) {
        m_ctx.gizmoMode = GizmoMode::None;
    }

    // Move gizmo toggle (W key)
    if (Input::isKeyPressed(Input::KEY_W)) {
        m_ctx.gizmoMode = GizmoMode::Move;
    }

    // Rotate gizmo toggle (E key)
    if (Input::isKeyPressed(Input::KEY_E) && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {
        m_ctx.gizmoMode = GizmoMode::Rotate;
    }

    // Scale gizmo toggle (R key) - only when not Ctrl (Ctrl+R is edge loop)
    if (Input::isKeyPressed(Input::KEY_R) && !Input::isKeyDown(Input::KEY_LEFT_CONTROL)) {
        m_ctx.gizmoMode = GizmoMode::Scale;
    }

    // Delete key in object mode - delete selected objects (protect live retopo surface)
    if (Input::isKeyPressed(Input::KEY_DELETE) && m_ctx.objectMode && !m_retopologyMode) {
        // Check if any selected object is the live retopo surface
        bool blockedByLive = false;
        if (m_retopologyLiveObj) {
            for (SceneObject* obj : m_ctx.selectedObjects) {
                if (obj == m_retopologyLiveObj) { blockedByLive = true; break; }
            }
            if (!blockedByLive && m_ctx.selectedObject == m_retopologyLiveObj) {
                blockedByLive = true;
            }
        }
        if (blockedByLive) {
            std::cout << "[Delete] Cannot delete live retopo surface — unlive it first" << std::endl;
        } else if (!m_ctx.selectedObjects.empty()) {
            // Queue selected objects for deletion
            for (SceneObject* obj : m_ctx.selectedObjects) {
                m_ctx.pendingDeletions.push_back(obj);
            }
            // Clear selection and wireframe cache
            m_ctx.selectedObject = nullptr;
            m_ctx.selectedObjects.clear();
            m_ctx.editableMesh.clear();
            m_ctx.meshDirty = false;
            m_cachedWireLines.clear();
            m_cachedSelectedLines.clear();
            m_cachedNormalVerts.clear();
            m_cachedSelectedVerts.clear();
            m_cachedHoveredVerts.clear();
            std::cout << "[Delete] Queued " << m_ctx.pendingDeletions.size() << " object(s) for deletion" << std::endl;
            return;  // Don't process further input after deletion
        } else if (m_ctx.selectedObject) {
            // Delete single selected object
            m_ctx.pendingDeletions.push_back(m_ctx.selectedObject);
            m_ctx.selectedObject = nullptr;
            m_ctx.selectedObjects.clear();
            m_ctx.editableMesh.clear();
            m_ctx.meshDirty = false;
            m_cachedWireLines.clear();
            m_cachedSelectedLines.clear();
            m_cachedNormalVerts.clear();
            m_cachedSelectedVerts.clear();
            m_cachedHoveredVerts.clear();
            std::cout << "[Delete] Queued 1 object for deletion" << std::endl;
            return;  // Don't process further input after deletion
        }
    }

    // Paint mode toggle (P key)
    if (Input::isKeyPressed(Input::KEY_P)) {
        if (m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
            m_ctx.isPainting = !m_ctx.isPainting;
        }
    }

    // Stamp fine-tuning with arrow keys (when in paint mode with stamp)
    if (m_ctx.isPainting && m_ctx.useStamp && !m_ctx.stampData.empty()) {
        bool ctrlHeld = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);
        bool shiftHeld = Input::isKeyDown(Input::KEY_LEFT_SHIFT) || Input::isKeyDown(Input::KEY_RIGHT_SHIFT);

        // Scale increment (smaller with shift for finer control)
        float scaleIncrement = shiftHeld ? 0.001f : 0.01f;
        // Rotation increment (smaller with shift for finer control)
        float rotIncrement = shiftHeld ? 0.1f : 1.0f;

        if (ctrlHeld) {
            // Ctrl + Left/Right: rotate stamp
            if (Input::isKeyPressed(Input::KEY_LEFT)) {
                m_ctx.stampRotation -= rotIncrement;
                if (m_ctx.stampRotation < -180.0f) m_ctx.stampRotation += 360.0f;
            }
            if (Input::isKeyPressed(Input::KEY_RIGHT)) {
                m_ctx.stampRotation += rotIncrement;
                if (m_ctx.stampRotation > 180.0f) m_ctx.stampRotation -= 360.0f;
            }
        } else {
            // Left/Right: adjust horizontal scale
            if (Input::isKeyPressed(Input::KEY_LEFT)) {
                m_ctx.stampScaleH = std::max(0.001f, m_ctx.stampScaleH - scaleIncrement);
            }
            if (Input::isKeyPressed(Input::KEY_RIGHT)) {
                m_ctx.stampScaleH = std::min(5.0f, m_ctx.stampScaleH + scaleIncrement);
            }
            // Up/Down: adjust vertical scale
            if (Input::isKeyPressed(Input::KEY_UP)) {
                m_ctx.stampScaleV = std::min(5.0f, m_ctx.stampScaleV + scaleIncrement);
            }
            if (Input::isKeyPressed(Input::KEY_DOWN)) {
                m_ctx.stampScaleV = std::max(0.001f, m_ctx.stampScaleV - scaleIncrement);
            }
        }
    }

    // Place selected objects flat on grid (V key)
    bool ctrlHeldV = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);
    if (Input::isKeyPressed(Input::KEY_V) && !ctrlHeldV) {
        if (m_ctx.objectMode && !m_ctx.selectedObjects.empty()) {
            // Place all selected objects on the grid
            for (SceneObject* obj : m_ctx.selectedObjects) {
                if (!obj) continue;

                // Get the object's mesh data to find the lowest Y point
                const auto& vertices = obj->getVertices();
                if (vertices.empty()) continue;

                // Find the lowest Y in local space
                float minLocalY = std::numeric_limits<float>::max();
                for (const auto& v : vertices) {
                    if (v.position.y < minLocalY) {
                        minLocalY = v.position.y;
                    }
                }

                // Account for scale
                glm::vec3 scale = obj->getTransform().getScale();
                float scaledMinY = minLocalY * scale.y;

                // Current position
                glm::vec3 pos = obj->getTransform().getPosition();

                // Move so the lowest point sits at Y=0
                pos.y = -scaledMinY;
                obj->getTransform().setPosition(pos);
            }
        } else if (m_ctx.selectedObject) {
            // Single object mode
            SceneObject* obj = m_ctx.selectedObject;
            const auto& vertices = obj->getVertices();
            if (!vertices.empty()) {
                float minLocalY = std::numeric_limits<float>::max();
                for (const auto& v : vertices) {
                    if (v.position.y < minLocalY) {
                        minLocalY = v.position.y;
                    }
                }

                glm::vec3 scale = obj->getTransform().getScale();
                float scaledMinY = minLocalY * scale.y;
                glm::vec3 pos = obj->getTransform().getPosition();
                pos.y = -scaledMinY;
                obj->getTransform().setPosition(pos);
            }
        }
    }

    // Snap selected objects to top of objects beneath them (C key)
    bool ctrlHeldC = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);
    if (Input::isKeyPressed(Input::KEY_C) && !ctrlHeldC) {
        // Gather selected objects (either from multi-selection or single selection)
        std::vector<SceneObject*> objectsToSnap;
        if (m_ctx.objectMode && !m_ctx.selectedObjects.empty()) {
            for (SceneObject* obj : m_ctx.selectedObjects) {
                if (obj) objectsToSnap.push_back(obj);
            }
        } else if (m_ctx.selectedObject) {
            objectsToSnap.push_back(m_ctx.selectedObject);
        }

        for (SceneObject* obj : objectsToSnap) {
            const auto& vertices = obj->getVertices();
            if (vertices.empty()) continue;

            glm::vec3 objPos = obj->getTransform().getPosition();
            glm::vec3 objScale = obj->getTransform().getScale();

            // Find min/max bounds of selected object in world space
            float objMinLocalY = std::numeric_limits<float>::max();
            float objMinX = std::numeric_limits<float>::max();
            float objMaxX = std::numeric_limits<float>::lowest();
            float objMinZ = std::numeric_limits<float>::max();
            float objMaxZ = std::numeric_limits<float>::lowest();

            for (const auto& v : vertices) {
                float worldX = objPos.x + v.position.x * objScale.x;
                float worldZ = objPos.z + v.position.z * objScale.z;
                objMinX = std::min(objMinX, worldX);
                objMaxX = std::max(objMaxX, worldX);
                objMinZ = std::min(objMinZ, worldZ);
                objMaxZ = std::max(objMaxZ, worldZ);
                if (v.position.y < objMinLocalY) {
                    objMinLocalY = v.position.y;
                }
            }

            // Find the highest Y point among all objects beneath this one
            float highestYBeneath = 0.0f; // Default to ground level
            bool foundObjectBeneath = false;

            for (auto& otherObjPtr : m_ctx.sceneObjects) {
                SceneObject* other = otherObjPtr.get();
                if (other == obj) continue;
                // Skip if other is also selected (don't snap onto other selected objects)
                if (m_ctx.selectedObjects.count(other) > 0) continue;

                const auto& otherVerts = other->getVertices();
                if (otherVerts.empty()) continue;

                glm::vec3 otherPos = other->getTransform().getPosition();
                glm::vec3 otherScale = other->getTransform().getScale();

                // Check XZ overlap and find max Y
                float otherMaxY = std::numeric_limits<float>::lowest();
                bool hasOverlap = false;

                for (const auto& v : otherVerts) {
                    float worldX = otherPos.x + v.position.x * otherScale.x;
                    float worldY = otherPos.y + v.position.y * otherScale.y;
                    float worldZ = otherPos.z + v.position.z * otherScale.z;

                    // Check if this vertex is within XZ bounds of selected object
                    if (worldX >= objMinX && worldX <= objMaxX &&
                        worldZ >= objMinZ && worldZ <= objMaxZ) {
                        hasOverlap = true;
                        if (worldY > otherMaxY) {
                            otherMaxY = worldY;
                        }
                    }
                }

                if (hasOverlap && otherMaxY > highestYBeneath) {
                    highestYBeneath = otherMaxY;
                    foundObjectBeneath = true;
                }
            }

            // Position the object so its lowest point sits on top of the highest point found
            float scaledMinY = objMinLocalY * objScale.y;
            glm::vec3 newPos = objPos;
            newPos.y = highestYBeneath - scaledMinY;
            obj->getTransform().setPosition(newPos);
        }
    }

    // Eyedropper: Alt + Click to sample color from model or reference image (works in paint mode)
    bool altHeld = Input::isKeyDown(Input::KEY_LEFT_ALT) || Input::isKeyDown(Input::KEY_RIGHT_ALT);
    m_ctx.useEyedropper = altHeld && m_ctx.isPainting;  // Eyedropper active when Alt held in paint mode

    if (m_ctx.useEyedropper && Input::isMouseButtonPressed(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
        bool sampled = false;

        // First, try to sample from reference image in ortho view
        Camera& activeCamera = m_ctx.getActiveCamera();
        if (activeCamera.getProjectionMode() == ProjectionMode::Orthographic) {
            ViewPreset preset = activeCamera.getViewPreset();
            if (preset != ViewPreset::Custom) {
                int viewIndex = static_cast<int>(preset) - 1;
                if (viewIndex >= 0 && viewIndex < 6) {
                    auto& ref = m_ctx.referenceImages[viewIndex];
                    if (ref.visible && ref.loaded && !ref.pixelData.empty()) {
                        // Get viewport dimensions
                        float screenWidth = static_cast<float>(m_ctx.window.getWidth());
                        float screenHeight = static_cast<float>(m_ctx.window.getHeight());
                        float vpX = 0, vpY = 0, vpW = screenWidth, vpH = screenHeight;
                        if (m_ctx.splitView) {
                            if (m_ctx.activeViewportLeft) {
                                vpW = screenWidth / 2.0f;
                            } else {
                                vpX = screenWidth / 2.0f;
                                vpW = screenWidth / 2.0f;
                            }
                        }

                        // Calculate reference image screen bounds (same logic as drawReferenceImages)
                        float orthoSize = activeCamera.getOrthoSize();
                        float aspect = vpW / vpH;
                        glm::mat4 view = activeCamera.getViewMatrix();
                        glm::mat4 proj = glm::ortho(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, -1000.0f, 1000.0f);
                        glm::mat4 viewProj = proj * view;

                        glm::vec3 right, up, depthOffset;
                        float gridEdge = 10.0f;
                        switch (preset) {
                            case ViewPreset::Top:    right = glm::vec3(1,0,0); up = glm::vec3(0,0,-1); depthOffset = glm::vec3(0,-0.1f,0); break;
                            case ViewPreset::Bottom: right = glm::vec3(1,0,0); up = glm::vec3(0,0,1);  depthOffset = glm::vec3(0,0.1f,0); break;
                            case ViewPreset::Front:  right = glm::vec3(1,0,0); up = glm::vec3(0,1,0);  depthOffset = glm::vec3(0,0,-gridEdge); break;
                            case ViewPreset::Back:   right = glm::vec3(-1,0,0); up = glm::vec3(0,1,0); depthOffset = glm::vec3(0,0,gridEdge); break;
                            case ViewPreset::Right:  right = glm::vec3(0,0,-1); up = glm::vec3(0,1,0); depthOffset = glm::vec3(-gridEdge,0,0); break;
                            case ViewPreset::Left:   right = glm::vec3(0,0,1); up = glm::vec3(0,1,0);  depthOffset = glm::vec3(gridEdge,0,0); break;
                            default: break;
                        }

                        glm::vec3 center = depthOffset + right * ref.offset.x + up * ref.offset.y;
                        glm::vec3 corners[4] = {
                            center - right * (ref.size.x * 0.5f) - up * (ref.size.y * 0.5f),
                            center + right * (ref.size.x * 0.5f) - up * (ref.size.y * 0.5f),
                            center + right * (ref.size.x * 0.5f) + up * (ref.size.y * 0.5f),
                            center - right * (ref.size.x * 0.5f) + up * (ref.size.y * 0.5f)
                        };

                        // Project corners to screen
                        ImVec2 screenCorners[4];
                        for (int i = 0; i < 4; i++) {
                            glm::vec4 clip = viewProj * glm::vec4(corners[i], 1.0f);
                            glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
                            screenCorners[i].x = vpX + (ndc.x * 0.5f + 0.5f) * vpW;
                            screenCorners[i].y = vpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * vpH;
                        }

                        // Check if mouse is inside the reference image quad
                        ImVec2 imMousePos = ImGui::GetMousePos();
                        float mx = imMousePos.x;
                        float my = imMousePos.y;

                        // Calculate UV within reference image (0-1 range)
                        float minX = std::min({screenCorners[0].x, screenCorners[1].x, screenCorners[2].x, screenCorners[3].x});
                        float maxX = std::max({screenCorners[0].x, screenCorners[1].x, screenCorners[2].x, screenCorners[3].x});
                        float minY = std::min({screenCorners[0].y, screenCorners[1].y, screenCorners[2].y, screenCorners[3].y});
                        float maxY = std::max({screenCorners[0].y, screenCorners[1].y, screenCorners[2].y, screenCorners[3].y});

                        if (mx >= minX && mx <= maxX && my >= minY && my <= maxY) {
                            // Calculate normalized position within image
                            float u = (static_cast<float>(mx) - minX) / (maxX - minX);
                            float v = (static_cast<float>(my) - minY) / (maxY - minY);

                            // Convert to pixel coordinates
                            int px = static_cast<int>(u * ref.imageWidth);
                            int py = static_cast<int>(v * ref.imageHeight);
                            px = std::clamp(px, 0, ref.imageWidth - 1);
                            py = std::clamp(py, 0, ref.imageHeight - 1);

                            // Sample color from reference image
                            size_t idx = (py * ref.imageWidth + px) * 4;
                            if (idx + 2 < ref.pixelData.size()) {
                                m_ctx.paintColor = glm::vec3(
                                    ref.pixelData[idx] / 255.0f,
                                    ref.pixelData[idx + 1] / 255.0f,
                                    ref.pixelData[idx + 2] / 255.0f
                                );
                                sampled = true;
                                std::cout << "Sampled from reference: RGB("
                                          << static_cast<int>(ref.pixelData[idx]) << ", "
                                          << static_cast<int>(ref.pixelData[idx + 1]) << ", "
                                          << static_cast<int>(ref.pixelData[idx + 2]) << ")" << std::endl;
                            }
                        }
                    }
                }
            }
        }

        // If not sampled from reference, try model texture
        if (!sampled && m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
            if (hit.hit) {
                auto& texData = m_ctx.selectedObject->getTextureData();
                int texW = m_ctx.selectedObject->getTextureWidth();
                int texH = m_ctx.selectedObject->getTextureHeight();

                float uvX = hit.uv.x - std::floor(hit.uv.x);
                float uvY = hit.uv.y - std::floor(hit.uv.y);
                int px = static_cast<int>(uvX * texW);
                int py = static_cast<int>(uvY * texH);
                px = std::clamp(px, 0, texW - 1);
                py = std::clamp(py, 0, texH - 1);

                size_t idx = (py * texW + px) * 4;
                if (idx + 2 < texData.size()) {
                    m_ctx.paintColor = glm::vec3(
                        texData[idx] / 255.0f,
                        texData[idx + 1] / 255.0f,
                        texData[idx + 2] / 255.0f
                    );
                    sampled = true;
                    std::cout << "Sampled from model: RGB("
                              << static_cast<int>(texData[idx]) << ", "
                              << static_cast<int>(texData[idx + 1]) << ", "
                              << static_cast<int>(texData[idx + 2]) << ")" << std::endl;
                }
            }
        }
    }

    // Color sampling from model texture: Alt + Click on model
    if (altHeld && m_ctx.isPainting && m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData() &&
        Input::isMouseButtonPressed(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
        if (hit.hit) {
            // Sample color from model texture at hit UV
            int texW = m_ctx.selectedObject->getTextureWidth();
            int texH = m_ctx.selectedObject->getTextureHeight();
            auto& texData = m_ctx.selectedObject->getTextureData();

            float uvX = hit.uv.x - std::floor(hit.uv.x);
            float uvY = hit.uv.y - std::floor(hit.uv.y);
            int px = static_cast<int>(uvX * texW);
            int py = static_cast<int>(uvY * texH);
            px = std::clamp(px, 0, texW - 1);
            py = std::clamp(py, 0, texH - 1);

            size_t pixelIdx = (py * texW + px) * 4;
            if (pixelIdx + 2 < texData.size()) {
                m_ctx.paintColor.r = texData[pixelIdx] / 255.0f;
                m_ctx.paintColor.g = texData[pixelIdx + 1] / 255.0f;
                m_ctx.paintColor.b = texData[pixelIdx + 2] / 255.0f;

                std::cout << "Sampled color from model at UV (" << uvX << ", " << uvY << "): RGB("
                          << static_cast<int>(m_ctx.paintColor.r * 255) << ", "
                          << static_cast<int>(m_ctx.paintColor.g * 255) << ", "
                          << static_cast<int>(m_ctx.paintColor.b * 255) << ")" << std::endl;
            }
        }
    }

    // Handle painting when in paint mode
    bool paintedThisFrame = false;
    static bool wasPaintingLastFrame = false;
    if (m_ctx.isPainting && m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
        // Stamp mode: single click to stamp
        if (m_ctx.useStamp && !m_ctx.stampData.empty()) {
            // Handle click to stamp
            if (Input::isMouseButtonPressed(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
                glm::vec3 rayOrigin, rayDir;
                m_ctx.getMouseRay(rayOrigin, rayDir);

                auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
                if (hit.hit) {
                    // Clear any preview first (restore original texture)
                    m_ctx.selectedObject->clearStampPreview();
                    // Save state before stamping (for undo)
                    m_ctx.selectedObject->saveTextureState();

                    if (m_ctx.stampFitToFace) {
                        // Fit to Face mode: find the quad face and fit stamp to its UVs
                        // Transform ray to local space for EditableMesh raycast
                        glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                        glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
                        glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

                        auto meshHit = m_ctx.editableMesh.raycast(localRayOrigin, localRayDir, ModelingSelectionMode::Face);
                        if (meshHit.hit && meshHit.faceIndex != UINT32_MAX) {
                            auto vertIndices = m_ctx.editableMesh.getFaceVertices(meshHit.faceIndex);
                            if (vertIndices.size() >= 3) {
                                // Get UVs and rotate based on stampFitRotation
                                std::vector<glm::vec2> uvs;
                                for (uint32_t vi : vertIndices) {
                                    uvs.push_back(m_ctx.editableMesh.getVertex(vi).uv);
                                }

                                // Debug output
                                std::cout << "Fit to Face: " << uvs.size() << " vertices, UVs: ";
                                for (size_t i = 0; i < uvs.size(); i++) {
                                    std::cout << "(" << uvs[i].x << "," << uvs[i].y << ") ";
                                }
                                std::cout << std::endl;

                                // Rotate corners by stampFitRotation * 90 degrees
                                int rot = m_ctx.stampFitRotation % static_cast<int>(uvs.size());
                                std::rotate(uvs.begin(), uvs.begin() + rot, uvs.end());

                                if (uvs.size() >= 4) {
                                    m_ctx.selectedObject->stampToQuad(uvs[0], uvs[1], uvs[2], uvs[3],
                                        m_ctx.stampData.data(), m_ctx.stampWidth, m_ctx.stampHeight,
                                        m_ctx.stampOpacity);
                                } else if (uvs.size() == 3) {
                                    // Triangle: treat as degenerate quad
                                    m_ctx.selectedObject->stampToQuad(uvs[0], uvs[1], uvs[2], uvs[2],
                                        m_ctx.stampData.data(), m_ctx.stampWidth, m_ctx.stampHeight,
                                        m_ctx.stampOpacity);
                                }
                            }
                        }
                    } else if (m_ctx.stampProjectFromView) {
                        // TRUE project from view: raycast each stamp pixel from camera
                        Camera& cam = m_ctx.getActiveCamera();
                        glm::vec3 camPos = cam.getPosition();
                        glm::vec3 camRight = cam.getRight();
                        glm::vec3 camUp = cam.getUp();
                        float worldSizeH = m_ctx.stampScale * m_ctx.stampScaleH * 0.5f;
                        float worldSizeV = m_ctx.stampScale * m_ctx.stampScaleV * 0.5f;

                        m_ctx.selectedObject->stampProjectedFromView(
                            hit.position, camPos, camRight, camUp,
                            m_ctx.stampData.data(), m_ctx.stampWidth, m_ctx.stampHeight,
                            worldSizeH, worldSizeV, m_ctx.stampRotation, m_ctx.stampOpacity, m_ctx.stampFlipH, m_ctx.stampFlipV);
                    } else {
                        // Normal mode: apply UV density correction
                        m_ctx.selectedObject->stampAt(hit.uv, hit.triangleIndex, m_ctx.stampData.data(),
                                                       m_ctx.stampWidth, m_ctx.stampHeight,
                                                       m_ctx.stampScale * m_ctx.stampScaleH, m_ctx.stampScale * m_ctx.stampScaleV,
                                                       m_ctx.stampRotation, m_ctx.stampOpacity, m_ctx.stampFlipH, m_ctx.stampFlipV);
                    }
                    m_ctx.selectedObject->markTextureModified();
                    paintedThisFrame = true;
                }
            }
        }
        // Smear mode: drag to smear colors
        else if (m_ctx.useSmear && !altHeld && Input::isMouseButtonDown(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
            if (hit.hit) {
                // Save texture state at start of stroke
                if (!m_ctx.isSmearing) {
                    m_ctx.selectedObject->saveTextureState();
                    // Sample initial color at starting point
                    size_t texW = m_ctx.selectedObject->getTextureWidth();
                    size_t texH = m_ctx.selectedObject->getTextureHeight();
                    auto& texData = m_ctx.selectedObject->getTextureData();
                    float uvX = hit.uv.x - std::floor(hit.uv.x);
                    float uvY = hit.uv.y - std::floor(hit.uv.y);
                    int px = static_cast<int>(uvX * texW);
                    int py = static_cast<int>(uvY * texH);
                    px = std::clamp(px, 0, static_cast<int>(texW - 1));
                    py = std::clamp(py, 0, static_cast<int>(texH - 1));
                    size_t idx = (py * texW + px) * 4;
                    if (idx + 2 < texData.size()) {
                        m_ctx.smearCarriedColor = glm::vec3(
                            texData[idx] / 255.0f,
                            texData[idx + 1] / 255.0f,
                            texData[idx + 2] / 255.0f
                        );
                    }
                    m_ctx.isSmearing = true;
                }

                // Smear and update carried color
                m_ctx.smearCarriedColor = m_ctx.selectedObject->smearAt(
                    hit.uv, m_ctx.smearCarriedColor, m_ctx.paintRadius, m_ctx.smearStrength, m_ctx.smearPickup);
                m_ctx.selectedObject->markTextureModified();
                paintedThisFrame = true;
            }
        }
        // Fill Paint to Face mode: click a face to fill it with current color
        else if (m_ctx.fillPaintToFace && !m_ctx.useSmear && !altHeld &&
                 Input::isMouseButtonPressed(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            // Raycast against EditableMesh to find face
            glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
            glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

            auto meshHit = m_ctx.editableMesh.raycast(localRayOrigin, localRayDir, ModelingSelectionMode::Face);
            if (meshHit.hit && meshHit.faceIndex != UINT32_MAX) {
                m_ctx.selectedObject->saveTextureState();

                int texWidth = m_ctx.selectedObject->getTextureWidth();
                int texHeight = m_ctx.selectedObject->getTextureHeight();
                auto& texData = m_ctx.selectedObject->getTextureData();

                auto faceVerts = m_ctx.editableMesh.getFaceVertices(meshHit.faceIndex);
                std::vector<glm::vec2> uvPoly;
                glm::vec2 uvMin(1e9f), uvMax(-1e9f);

                for (uint32_t vi : faceVerts) {
                    glm::vec2 uv = m_ctx.editableMesh.getVertex(vi).uv;
                    uvPoly.push_back(uv);
                    uvMin = glm::min(uvMin, uv);
                    uvMax = glm::max(uvMax, uv);
                }

                // Point-in-polygon test
                auto pointInPoly = [](const glm::vec2& p, const std::vector<glm::vec2>& poly) {
                    int n = poly.size(), crossings = 0;
                    for (int i = 0; i < n; i++) {
                        int j = (i + 1) % n;
                        if ((poly[i].y <= p.y && poly[j].y > p.y) || (poly[j].y <= p.y && poly[i].y > p.y)) {
                            float t = (p.y - poly[i].y) / (poly[j].y - poly[i].y);
                            if (p.x < poly[i].x + t * (poly[j].x - poly[i].x)) crossings++;
                        }
                    }
                    return (crossings % 2) == 1;
                };

                int minPX = std::max(0, static_cast<int>(uvMin.x * texWidth) - 1);
                int maxPX = std::min(texWidth - 1, static_cast<int>(uvMax.x * texWidth) + 1);
                int minPY = std::max(0, static_cast<int>(uvMin.y * texHeight) - 1);
                int maxPY = std::min(texHeight - 1, static_cast<int>(uvMax.y * texHeight) + 1);

                for (int py = minPY; py <= maxPY; py++) {
                    for (int px = minPX; px <= maxPX; px++) {
                        glm::vec2 pixelUV((px + 0.5f) / texWidth, (py + 0.5f) / texHeight);
                        if (pointInPoly(pixelUV, uvPoly)) {
                            size_t idx = (py * texWidth + px) * 4;
                            if (idx + 3 < texData.size()) {
                                texData[idx] = static_cast<unsigned char>(m_ctx.paintColor.r * 255);
                                texData[idx + 1] = static_cast<unsigned char>(m_ctx.paintColor.g * 255);
                                texData[idx + 2] = static_cast<unsigned char>(m_ctx.paintColor.b * 255);
                            }
                        }
                    }
                }

                m_ctx.selectedObject->markTextureModified();
                paintedThisFrame = true;
            }
        }
        // Brush mode: continuous painting while dragging
        // Skip painting when Alt is held (Alt+click is for color sampling)
        else if (!m_ctx.useSmear && !m_ctx.fillPaintToFace && !altHeld && Input::isMouseButtonDown(Input::MOUSE_LEFT) && !mouseOverImGui && !gizmoActive) {
            // Get mouse ray
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            // Raycast against selected object
            auto hit = m_ctx.selectedObject->raycast(rayOrigin, rayDir);
            if (hit.hit) {
                // Save texture state at start of stroke (first paint of this mouse press)
                if (!wasPaintingLastFrame) {
                    m_ctx.selectedObject->saveTextureState();
                }
                wasPaintingLastFrame = true;

                bool shiftHeld = Input::isKeyDown(Input::KEY_LEFT_SHIFT) || Input::isKeyDown(Input::KEY_RIGHT_SHIFT);

                // Line tool: Shift+Click draws line from last position
                if (shiftHeld && m_ctx.hasLastPaintPosition && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
                    // Draw line from last paint position to current position
                    glm::vec2 startUV = m_ctx.lastPaintUV;
                    glm::vec2 endUV = hit.uv;

                    // Calculate distance in UV space and determine number of steps
                    float distance = glm::length(endUV - startUV);
                    int texSize = std::max(m_ctx.selectedObject->getTextureWidth(), m_ctx.selectedObject->getTextureHeight());
                    float stepSize = m_ctx.paintRadius * 0.5f;  // Paint every half-radius for smooth coverage
                    int steps = static_cast<int>(distance / stepSize) + 1;
                    if (steps < 2) steps = 2;

                    // Interpolate and paint along the line
                    for (int i = 0; i <= steps; i++) {
                        float t = static_cast<float>(i) / static_cast<float>(steps);
                        glm::vec2 uv = glm::mix(startUV, endUV, t);
                        m_ctx.selectedObject->paintAt(uv, m_ctx.paintColor, m_ctx.paintRadius, m_ctx.paintStrength, m_ctx.squareBrush);
                    }
                } else {
                    // Normal paint at UV coordinate
                    m_ctx.selectedObject->paintAt(hit.uv, m_ctx.paintColor, m_ctx.paintRadius, m_ctx.paintStrength, m_ctx.squareBrush);
                }

                // Store this position for line tool
                m_ctx.lastPaintUV = hit.uv;
                m_ctx.hasLastPaintPosition = true;

                m_ctx.selectedObject->markTextureModified();
                paintedThisFrame = true;  // Only block other input if we actually painted
            }
        } else {
            wasPaintingLastFrame = false;
            m_ctx.isSmearing = false;  // Reset smear state when not dragging
            m_ctx.clonePaintingActive = false;  // Reset clone tracking
        }

        // Upload modified texture to GPU (every frame while painting for live feedback)
        if (m_ctx.selectedObject->isTextureModified()) {
            uint32_t handle = m_ctx.selectedObject->getBufferHandle();
            auto& texData = m_ctx.selectedObject->getTextureData();
            int w = m_ctx.selectedObject->getTextureWidth();
            int h = m_ctx.selectedObject->getTextureHeight();
            m_ctx.modelRenderer.updateTexture(handle, texData.data(), w, h);
            m_ctx.selectedObject->clearTextureModified();
        }

        // Only block selection input if we actually painted on the model
        // This allows camera tumble when clicking on empty space
        if (paintedThisFrame) {
            return;
        }
    }

    // Mode switching (synchronized with UV editor)
    if (Input::isKeyPressed(Input::KEY_A) && !Input::isKeyDown(Input::KEY_LEFT_CONTROL)) {
        m_ctx.modelingSelectionMode = ModelingSelectionMode::Vertex;
        m_ctx.uvSelectionMode = 3;  // Sync UV editor to vertex mode
        m_ctx.uvEdgeSelectionMode = false;
        m_ctx.editableMesh.clearSelection();
        m_ctx.uvSelectedFaces.clear();
        clearUVEdgeSelection();
    }
    if (Input::isKeyPressed(Input::KEY_S) && !Input::isKeyDown(Input::KEY_LEFT_CONTROL)) {
        m_ctx.modelingSelectionMode = ModelingSelectionMode::Edge;
        m_ctx.uvSelectionMode = 2;  // Sync UV editor to edge mode
        m_ctx.uvEdgeSelectionMode = true;
        m_ctx.editableMesh.clearSelection();
        m_ctx.uvSelectedFaces.clear();
        m_ctx.uvSelectedVertices.clear();
    }
    if (Input::isKeyPressed(Input::KEY_D)) {
        m_ctx.modelingSelectionMode = ModelingSelectionMode::Face;
        m_ctx.uvSelectionMode = 1;  // Sync UV editor to face mode
        m_ctx.uvEdgeSelectionMode = false;
        m_ctx.editableMesh.clearSelection();
        clearUVEdgeSelection();
        m_ctx.uvSelectedVertices.clear();
    }

    bool ctrlDown = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);
    bool shiftDown = Input::isKeyDown(Input::KEY_LEFT_SHIFT) || Input::isKeyDown(Input::KEY_RIGHT_SHIFT);

    // Undo (Ctrl+Z)
    if (Input::isKeyPressed(Input::KEY_Z) && ctrlDown && !shiftDown) {
        // In paint mode, try texture undo first
        if (m_ctx.isPainting && m_ctx.selectedObject && m_ctx.selectedObject->canUndoTexture()) {
            if (m_ctx.selectedObject->undoTexture()) {
                // Upload restored texture to GPU
                uint32_t handle = m_ctx.selectedObject->getBufferHandle();
                auto& texData = m_ctx.selectedObject->getTextureData();
                int w = m_ctx.selectedObject->getTextureWidth();
                int h = m_ctx.selectedObject->getTextureHeight();
                m_ctx.modelRenderer.updateTexture(handle, texData.data(), w, h);
                m_ctx.selectedObject->clearTextureModified();
            }
        } else if (m_connectCPsBackup.valid) {
            // Undo Connect CPs operation (scene-level)
            undoConnectCPs();
        } else if (m_ctx.editableMesh.undo()) {
            m_ctx.meshDirty = true;
        }
    }

    // Redo (Ctrl+Shift+Z)
    if (Input::isKeyPressed(Input::KEY_Z) && ctrlDown && shiftDown) {
        if (m_ctx.editableMesh.redo()) {
            m_ctx.meshDirty = true;
        }
    }

    // Save (Ctrl+S)
    if (Input::isKeyPressed(Input::KEY_S) && ctrlDown) {
        saveEditableMeshAsLime();
    }

    // Duplicate (Ctrl+D)
    if (Input::isKeyPressed(Input::KEY_D) && ctrlDown) {
        duplicateSelectedObject();
    }

    // Toggle wireframe (1 key)
    if (Input::isKeyPressed(Input::KEY_1) && !Input::isKeyDown(Input::KEY_LEFT_CONTROL)
        && !ImGui::GetIO().WantTextInput) {
        m_ctx.showModelingWireframe = !m_ctx.showModelingWireframe;
    }

    // Toggle grid (2 key)
    if (Input::isKeyPressed(Input::KEY_2) && !Input::isKeyDown(Input::KEY_LEFT_CONTROL)
        && !ImGui::GetIO().WantTextInput) {
        m_ctx.showGrid = !m_ctx.showGrid;
    }

    // Delete selected faces (not during retopo mode)
    if (!m_retopologyMode && Input::isKeyPressed(Input::KEY_DELETE)) {
        if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.deleteSelectedFaces();
            m_ctx.meshDirty = true;
        }
    }

    // Extrude (Shift+E) — faces or edges depending on selection
    if (Input::isKeyPressed(Input::KEY_E) && Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {
        if (!m_ctx.editableMesh.getSelectedEdges().empty()) {
            m_ctx.editableMesh.saveState();
            int count = std::max(1, m_ctx.extrudeCount);
            float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
            for (int i = 0; i < count; ++i) {
                m_ctx.editableMesh.extrudeSelectedEdges(stepDist);
            }
            m_ctx.meshDirty = true;
            m_ctx.gizmoMode = GizmoMode::Move;
            m_lastOp = LastOp::ExtrudeEdge;
        } else if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
            m_ctx.editableMesh.saveState();
            int count = std::max(1, m_ctx.extrudeCount);
            float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
            for (int i = 0; i < count; ++i) {
                m_ctx.editableMesh.extrudeSelectedFaces(stepDist);
            }
            m_ctx.meshDirty = true;
            m_lastOp = LastOp::ExtrudeFace;
        }
    }

    // Repeat last operation / Face direction extrude (X)
    if (Input::isKeyPressed(Input::KEY_X) && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)
        && !Input::isKeyDown(Input::KEY_LEFT_CONTROL) && !ImGui::GetIO().WantTextInput) {
        // Face mode with selection: extrude new quad off direction edge
        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
        if (m_ctx.modelingSelectionMode == eden::ModelingSelectionMode::Face
            && selectedFaces.size() == 1 && m_ctx.editableMesh.isValid()) {
            uint32_t fi = selectedFaces[0];
            auto faceVerts = m_ctx.editableMesh.getFaceVertices(fi);
            if (faceVerts.size() >= 3) {
                m_ctx.editableMesh.saveState();
                int numV = static_cast<int>(faceVerts.size());
                int e = m_faceDirectionEdge % numV;
                int e1 = (e + 1) % numV;

                glm::vec3 edgeA = m_ctx.editableMesh.getVertex(faceVerts[e]).position;
                glm::vec3 edgeB = m_ctx.editableMesh.getVertex(faceVerts[e1]).position;
                glm::vec3 fn = m_ctx.editableMesh.getFaceNormal(fi);
                glm::vec3 edgeDir = glm::normalize(edgeB - edgeA);
                glm::vec3 extrudeDir = glm::normalize(glm::cross(edgeDir, fn));
                glm::vec3 faceCenter = m_ctx.editableMesh.getFaceCenter(fi);
                glm::vec3 edgeMid = (edgeA + edgeB) * 0.5f;
                if (glm::dot(extrudeDir, edgeMid - faceCenter) < 0.0f)
                    extrudeDir = -extrudeDir;

                float avgEdge = 0.0f;
                for (int i = 0; i < numV; i++) {
                    int j = (i + 1) % numV;
                    avgEdge += glm::length(m_ctx.editableMesh.getVertex(faceVerts[j]).position -
                                           m_ctx.editableMesh.getVertex(faceVerts[i]).position);
                }
                avgEdge /= static_cast<float>(numV);

                glm::vec3 newC = edgeB + extrudeDir * avgEdge;
                glm::vec3 newD = edgeA + extrudeDir * avgEdge;

                eden::HEVertex vc, vd;
                vc.position = newC; vc.normal = fn; vc.uv = glm::vec2(0);
                vc.color = glm::vec4(0.7f); vc.halfEdgeIndex = UINT32_MAX; vc.selected = false;
                vd.position = newD; vd.normal = fn; vd.uv = glm::vec2(0);
                vd.color = glm::vec4(0.7f); vd.halfEdgeIndex = UINT32_MAX; vd.selected = false;

                uint32_t idxC = m_ctx.editableMesh.addVertex(vc);
                uint32_t idxD = m_ctx.editableMesh.addVertex(vd);

                glm::vec3 newNormal = glm::cross(edgeB - edgeA, newD - edgeA);
                std::vector<uint32_t> newFace;
                if (glm::dot(newNormal, fn) >= 0.0f)
                    newFace = {faceVerts[e], faceVerts[e1], idxC, idxD};
                else
                    newFace = {faceVerts[e], idxD, idxC, faceVerts[e1]};

                // Deselect old face, add and select new face
                m_ctx.editableMesh.getFace(fi).selected = false;
                m_ctx.editableMesh.addFace(newFace);
                uint32_t newFi = static_cast<uint32_t>(m_ctx.editableMesh.getFaceCount()) - 1;
                m_ctx.editableMesh.getFace(newFi).selected = true;
                m_faceDirectionEdge = 2;  // Opposite edge = continue forward

                m_ctx.meshDirty = true;
                invalidateWireframeCache();
            }
        } else if (m_lastOp == LastOp::ExtrudeEdge && !m_ctx.editableMesh.getSelectedEdges().empty()) {
            m_ctx.editableMesh.saveState();
            int count = std::max(1, m_ctx.extrudeCount);
            float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
            for (int i = 0; i < count; ++i) {
                m_ctx.editableMesh.extrudeSelectedEdges(stepDist);
            }
            m_ctx.meshDirty = true;
            m_ctx.gizmoMode = GizmoMode::Move;
        } else if (m_lastOp == LastOp::ExtrudeFace && !m_ctx.editableMesh.getSelectedFaces().empty()) {
            m_ctx.editableMesh.saveState();
            int count = std::max(1, m_ctx.extrudeCount);
            float stepDist = m_ctx.extrudeDistance / static_cast<float>(count);
            for (int i = 0; i < count; ++i) {
                m_ctx.editableMesh.extrudeSelectedFaces(stepDist);
            }
            m_ctx.meshDirty = true;
        }
    }

    // Merge vertices (Alt+M)
    if (Input::isKeyPressed(Input::KEY_M) && Input::isKeyDown(Input::KEY_LEFT_ALT)) {
        if (m_ctx.editableMesh.getSelectedVertices().size() >= 2) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.mergeSelectedVertices();
            m_ctx.meshDirty = true;
        }
    }

    // Insert edge loop (Ctrl+R)
    if (Input::isKeyPressed(Input::KEY_R) && ctrlDown) {
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        if (!selectedEdges.empty()) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.insertEdgeLoop(selectedEdges[0]);
            m_ctx.meshDirty = true;
        }
    }

    // Bridge edges (B)
    if (Input::isKeyPressed(Input::KEY_B) && !ctrlDown && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        if (selectedEdges.size() == 2) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.bridgeEdges(selectedEdges[0], selectedEdges[1], g_bridgeSegments);
            m_ctx.meshDirty = true;
        }
    }

    // Flip normals (N)
    if (Input::isKeyPressed(Input::KEY_N) && !ctrlDown && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {
        if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.flipSelectedNormals();
            m_ctx.meshDirty = true;
        }
    }

    // Inset faces (I)
    if (Input::isKeyPressed(Input::KEY_I) && !ctrlDown && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {
        if (!m_ctx.editableMesh.getSelectedFaces().empty()) {
            m_ctx.editableMesh.saveState();
            m_ctx.editableMesh.insetSelectedFaces(m_ctx.insetAmount);
            m_ctx.meshDirty = true;
        }
    }

    // Hollow mesh (H)
    if (Input::isKeyPressed(72) && !ctrlDown && !Input::isKeyDown(Input::KEY_LEFT_SHIFT)) {  // 72 = 'H'
        m_ctx.editableMesh.saveState();
        m_ctx.editableMesh.hollow(m_ctx.hollowThickness);
        m_ctx.meshDirty = true;
    }

    // Snap vertex mode - click to select vertices in order
    if (m_snapVertexMode && !mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        // Find closest vertex across all objects
        SceneObject* hitObject = nullptr;
        uint32_t hitVertexIdx = UINT32_MAX;
        glm::vec3 hitVertexWorldPos;
        float closestDist = std::numeric_limits<float>::max();
        const float vertexThreshold = 0.15f;  // Click radius for vertex selection

        for (auto& objPtr : m_ctx.sceneObjects) {
            SceneObject* obj = objPtr.get();
            if (!obj || !obj->hasEditableMeshData() || !obj->isVisible()) continue;

            glm::mat4 modelMatrix = obj->getTransform().getMatrix();
            const auto& heVerts = obj->getHEVertices();

            for (size_t vi = 0; vi < heVerts.size(); ++vi) {
                glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(heVerts[vi].position, 1.0f));

                // Project vertex to get distance from ray
                glm::vec3 toVert = worldPos - rayOrigin;
                float alongRay = glm::dot(toVert, rayDir);
                if (alongRay < 0) continue;  // Behind camera

                glm::vec3 closestOnRay = rayOrigin + rayDir * alongRay;
                float dist = glm::length(worldPos - closestOnRay);

                if (dist < vertexThreshold && alongRay < closestDist) {
                    closestDist = alongRay;
                    hitObject = obj;
                    hitVertexIdx = static_cast<uint32_t>(vi);
                    hitVertexWorldPos = worldPos;
                }
            }
        }

        if (hitObject && hitVertexIdx != UINT32_MAX) {
            // Determine if this is source or target based on current state
            bool isSourceSelection = (m_snapSrcObj == nullptr) ||
                                    (m_snapSrcObj == hitObject && m_snapDstObj == nullptr);

            // If we already have source verts and click on a different object, it's target
            if (m_snapSrcObj && hitObject != m_snapSrcObj) {
                isSourceSelection = false;
            }

            if (isSourceSelection) {
                // Adding to source
                if (m_snapSrcObj == nullptr) {
                    m_snapSrcObj = hitObject;
                }
                if (hitObject == m_snapSrcObj) {
                    // Check if vertex already selected
                    bool alreadySelected = false;
                    for (size_t i = 0; i < m_snapSrcVertIndices.size(); ++i) {
                        if (m_snapSrcVertIndices[i] == hitVertexIdx) {
                            alreadySelected = true;
                            break;
                        }
                    }
                    if (!alreadySelected) {
                        m_snapSrcVerts.push_back(hitVertexWorldPos);
                        m_snapSrcVertIndices.push_back(hitVertexIdx);
                        std::cout << "[Snap] Source vertex " << m_snapSrcVerts.size() << " selected" << std::endl;
                    }
                }
            } else {
                // Adding to target
                if (m_snapDstObj == nullptr) {
                    m_snapDstObj = hitObject;
                }
                if (hitObject == m_snapDstObj) {
                    // Check if vertex already selected
                    bool alreadySelected = false;
                    for (size_t i = 0; i < m_snapDstVertIndices.size(); ++i) {
                        if (m_snapDstVertIndices[i] == hitVertexIdx) {
                            alreadySelected = true;
                            break;
                        }
                    }
                    if (!alreadySelected) {
                        m_snapDstVerts.push_back(hitVertexWorldPos);
                        m_snapDstVertIndices.push_back(hitVertexIdx);
                        std::cout << "[Snap] Target vertex " << m_snapDstVerts.size() << " selected" << std::endl;
                    }
                }
            }
        }
    }

    // ESC cancels snap vertex mode
    if (m_snapVertexMode && Input::isKeyPressed(Input::KEY_ESCAPE)) {
        cancelSnapVertexMode();
    }

    // Rigging: click to place bone on mesh surface
    if (m_riggingMode && m_placingBone && m_selectedBone >= 0 && !mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        // Transform ray into model space
        if (m_ctx.selectedObject) {
            glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
            glm::mat4 invModel = glm::inverse(modelMatrix);
            rayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
            rayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));
        }

        auto hit = m_ctx.editableMesh.raycastFace(rayOrigin, rayDir);
        if (hit.hit && m_selectedBone < static_cast<int>(m_bonePositions.size())) {
            // Inset along surface normal so bone ends up inside the mesh
            glm::vec3 placedPos = hit.position - hit.normal * m_boneInsetDepth;
            m_bonePositions[m_selectedBone] = placedPos;
            m_placingBone = false;
            m_ctx.meshDirty = true;
            invalidateWireframeCache();
            std::cout << "[Rigging] Placed bone " << m_ctx.editableMesh.getSkeleton().bones[m_selectedBone].name
                      << " at (" << placedPos.x << ", " << placedPos.y << ", " << placedPos.z
                      << ") inset=" << m_boneInsetDepth << std::endl;
        }
    }

    // ESC cancels rigging bone placement
    if (m_riggingMode && m_placingBone && Input::isKeyPressed(Input::KEY_ESCAPE)) {
        m_placingBone = false;
    }

    // Rigging: click to select bone in viewport (when not placing and gizmo not active)
    if (m_riggingMode && !m_placingBone && !m_ctx.gizmoDragging && !mouseOverImGui &&
        Input::isMouseButtonPressed(Input::MOUSE_LEFT) &&
        m_ctx.gizmoHoveredAxis == GizmoAxis::None) {
        glm::vec2 mousePos = Input::getMousePosition();
        int picked = pickBoneAtScreenPos(mousePos);
        if (picked >= 0) {
            m_selectedBone = picked;
        }
    }

    // Patch Blanket: Shift+drag rectangle to define patch region
    if (m_patchBlanketMode && !mouseOverImGui) {
        bool altDown = Input::isKeyDown(Input::KEY_LEFT_ALT) || Input::isKeyDown(Input::KEY_RIGHT_ALT);

        if (!m_patchBlanketDragging && altDown && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
            // Start drag
            glm::vec2 mousePos = Input::getMousePosition();
            m_patchBlanketStart = mousePos;
            m_patchBlanketEnd = mousePos;
            m_patchBlanketDragging = true;
        }

        if (m_patchBlanketDragging) {
            m_patchBlanketEnd = Input::getMousePosition();

            if (!Input::isMouseButtonDown(Input::MOUSE_LEFT)) {
                // Mouse released — execute
                m_patchBlanketDragging = false;
                m_patchBlanketMode = false;
                executePatchBlanket();
            }
        }

        if (Input::isKeyPressed(Input::KEY_ESCAPE)) {
            m_patchBlanketMode = false;
            m_patchBlanketDragging = false;
            std::cout << "[PatchBlanket] Cancelled" << std::endl;
        }
    }

    // Retopology: click to place vertex on live surface or pick existing retopo vertex (not while G-grab active)
    if (m_retopologyMode && !m_retopologyDragging && m_retopologyLiveObj && !mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        if (m_retopologyVerts.size() < 4) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            // First: check if clicking near an existing retopo vertex (screen-space pixel distance)
            bool pickedExisting = false;
            if (!m_retopologyQuads.empty()) {
                Camera& activeCam = m_ctx.getActiveCamera();
                glm::mat4 viewMat = activeCam.getViewMatrix();
                float aspect = static_cast<float>(m_ctx.window.getWidth()) / static_cast<float>(m_ctx.window.getHeight());
                glm::mat4 projMat = activeCam.getProjectionMatrix(aspect);
                glm::mat4 vpMat = projMat * viewMat;
                float vpW = static_cast<float>(m_ctx.window.getWidth());
                float vpH = static_cast<float>(m_ctx.window.getHeight());
                ImVec2 mousePos = ImGui::GetMousePos();

                auto worldToScreen = [&](const glm::vec3& pos) -> glm::vec2 {
                    glm::vec4 clip = vpMat * glm::vec4(pos, 1.0f);
                    if (clip.w <= 0.0f) return glm::vec2(-10000.0f);
                    glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    return glm::vec2((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);
                };

                float closestPixelDist = std::numeric_limits<float>::max();
                glm::vec3 closestPos;
                const float pixelThreshold = 15.0f;  // 15 pixels on screen

                // Search quad world positions — only front-facing quads
                for (const auto& quad : m_retopologyQuads) {
                    glm::vec3 e1 = quad.verts[1] - quad.verts[0];
                    glm::vec3 e2 = quad.verts[3] - quad.verts[0];
                    glm::vec3 fNorm = glm::cross(e1, e2);
                    glm::vec3 qCenter = (quad.verts[0] + quad.verts[1] + quad.verts[2] + quad.verts[3]) * 0.25f;
                    if (glm::dot(fNorm, rayOrigin - qCenter) <= 0.0f) continue;

                    for (int vi = 0; vi < 4; ++vi) {
                        glm::vec2 screenPos = worldToScreen(quad.verts[vi]);
                        float pixelDist = glm::length(screenPos - glm::vec2(mousePos.x, mousePos.y));

                        if (pixelDist < pixelThreshold && pixelDist < closestPixelDist) {
                            closestPixelDist = pixelDist;
                            closestPos = quad.verts[vi];
                        }
                    }
                }

                if (closestPixelDist < pixelThreshold) {
                    m_retopologyVerts.push_back(closestPos);
                    m_retopologyNormals.push_back(glm::vec3(0.0f));
                    m_retopologyVertMeshIdx.push_back(UINT32_MAX);
                    pickedExisting = true;
                    std::cout << "[Retopo] Picked existing vertex " << m_retopologyVerts.size() << std::endl;
                }
            }

            // If no existing vertex was picked, raycast against live surface for new vertex
            if (!pickedExisting) {
                auto hit = m_retopologyLiveObj->raycast(rayOrigin, rayDir);
                if (hit.hit) {
                    m_retopologyVerts.push_back(hit.position);
                    m_retopologyNormals.push_back(hit.normal);
                    m_retopologyVertMeshIdx.push_back(UINT32_MAX);
                    std::cout << "[Retopo] Placed new vertex " << m_retopologyVerts.size()
                              << " at (" << hit.position.x << ", " << hit.position.y << ", " << hit.position.z << ")" << std::endl;
                }
            }
        }

        // Auto-create quad as soon as 4th vertex is placed
        if (m_retopologyVerts.size() == 4) {
            createRetopologyQuad();
        }
    }

    // Face direction edge: arrow keys rotate which edge is the extrude direction
    // Works whenever faces are selected in face mode
    if (!m_retopologyMode && m_ctx.modelingSelectionMode == eden::ModelingSelectionMode::Face
        && !ImGui::GetIO().WantTextInput && !mouseOverImGui) {
        if (Input::isKeyPressed(Input::KEY_RIGHT)) {
            m_faceDirectionEdge = (m_faceDirectionEdge + 1) % 4;
        }
        if (Input::isKeyPressed(Input::KEY_LEFT)) {
            m_faceDirectionEdge = (m_faceDirectionEdge + 3) % 4;
        }
    }

    // Scatter point selection: LMB click to toggle select (up to 4) for subdivide
    if (m_retopologyMode && !m_scatterRings.empty() && !m_ringDragging && !m_retopologyDragging
        && !mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        Camera& selCam = m_ctx.getActiveCamera();
        glm::mat4 selVP = selCam.getProjectionMatrix(
            static_cast<float>(m_ctx.window.getWidth()) / static_cast<float>(m_ctx.window.getHeight()))
            * selCam.getViewMatrix();
        float sVpW = static_cast<float>(m_ctx.window.getWidth());
        float sVpH = static_cast<float>(m_ctx.window.getHeight());
        ImVec2 sMouse = ImGui::GetMousePos();
        glm::vec3 sCamPos = selCam.getPosition();

        float closestDist = 15.0f;
        int closestIdx = -1;
        for (size_t i = 0; i < m_retopologyVerts.size(); i++) {
            // Loose backface cull — allow perpendicular normals (center line points)
            if (i < m_retopologyNormals.size() && glm::length(m_retopologyNormals[i]) > 0.001f) {
                glm::vec3 toCamera = glm::normalize(sCamPos - m_retopologyVerts[i]);
                if (glm::dot(m_retopologyNormals[i], toCamera) < -0.3f) continue;
            }
            glm::vec4 clip = selVP * glm::vec4(m_retopologyVerts[i], 1.0f);
            if (clip.w <= 0.0f) continue;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            glm::vec2 sp((ndc.x + 1.0f) * 0.5f * sVpW, (1.0f - ndc.y) * 0.5f * sVpH);
            float d = glm::length(sp - glm::vec2(sMouse.x, sMouse.y));
            if (d < closestDist) { closestDist = d; closestIdx = static_cast<int>(i); }
        }

        if (closestIdx >= 0) {
            size_t idx = static_cast<size_t>(closestIdx);
            if (m_selectedScatterPoints.count(idx)) {
                m_selectedScatterPoints.erase(idx);  // Deselect
            } else if (m_selectedScatterPoints.size() < 4) {
                m_selectedScatterPoints.insert(idx);  // Select
            }
        }
    }

    // Scatter ring slide: Ctrl+G near a green dot → drag left/right to slide, up/down to raise/lower
    if (m_retopologyMode && !m_scatterRings.empty() && !m_ringDragging && !mouseOverImGui
        && Input::isKeyPressed(Input::KEY_G)
        && (Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL))) {
        Camera& rcam = m_ctx.getActiveCamera();
        glm::mat4 rcVP = rcam.getProjectionMatrix(
            static_cast<float>(m_ctx.window.getWidth()) / static_cast<float>(m_ctx.window.getHeight()))
            * rcam.getViewMatrix();
        float rVpW = static_cast<float>(m_ctx.window.getWidth());
        float rVpH = static_cast<float>(m_ctx.window.getHeight());
        ImVec2 rMouse = ImGui::GetMousePos();

        float closestDist = 30.0f;
        int closestRing = -1;
        for (size_t ri = 0; ri < m_scatterRings.size(); ri++) {
            if (m_scatterRings[ri].count < 1) continue;
            glm::vec3 pos = m_retopologyVerts[m_scatterRings[ri].startIdx];
            glm::vec4 clip = rcVP * glm::vec4(pos, 1.0f);
            if (clip.w <= 0.0f) continue;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            glm::vec2 sp((ndc.x + 1.0f) * 0.5f * rVpW, (1.0f - ndc.y) * 0.5f * rVpH);
            float d = glm::length(sp - glm::vec2(rMouse.x, rMouse.y));
            if (d < closestDist) { closestDist = d; closestRing = static_cast<int>(ri); }
        }
        if (closestRing >= 0) {
            m_ringDragging = true;
            m_selectedRing = closestRing;
            m_ringDragStartX = rMouse.x;
            m_ringDragStartY = rMouse.y;
            m_ringDragStartOffset = m_scatterRings[closestRing].slideOffset;
            m_ringDragStartSliceY = m_scatterRings[closestRing].sliceY;
            m_scatterEdges.clear();
            m_retopologyQuads.clear();
            std::cout << "[Scatter] Grabbed ring " << closestRing
                      << " — drag L/R to slide, U/D to raise/lower, LMB confirm, ESC cancel" << std::endl;
        }
    }

    if (m_ringDragging && m_selectedRing >= 0 && m_selectedRing < static_cast<int>(m_scatterRings.size())) {
        ImVec2 rMouse = ImGui::GetMousePos();
        float dx = rMouse.x - m_ringDragStartX;
        float dy = rMouse.y - m_ringDragStartY;

        auto& ring = m_scatterRings[m_selectedRing];

        // Left/right: slide along contour
        float slideSensitivity = ring.totalLen / static_cast<float>(m_ctx.window.getWidth());
        ring.slideOffset = m_ringDragStartOffset + dx * slideSensitivity;
        resampleRing(static_cast<size_t>(m_selectedRing));

        // Up/down: raise/lower ring (re-slice at new Y)
        // Mouse Y is inverted (up = negative), scale by model height / screen height
        float heightRange = 2.0f;  // approximate model height in units
        float heightSensitivity = heightRange / static_cast<float>(m_ctx.window.getHeight());
        float newY = m_ringDragStartSliceY - dy * heightSensitivity;
        if (std::abs(newY - ring.sliceY) > 0.001f) {
            resliceRing(static_cast<size_t>(m_selectedRing), newY);
        }

        if (Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
            std::cout << "[Scatter] Ring " << m_selectedRing << " adjusted (slide=" << ring.slideOffset
                      << " Y=" << ring.sliceY << ")" << std::endl;
            m_ringDragging = false;
        }

        if (Input::isKeyPressed(Input::KEY_ESCAPE)) {
            ring.slideOffset = m_ringDragStartOffset;
            resliceRing(static_cast<size_t>(m_selectedRing), m_ringDragStartSliceY);
            std::cout << "[Scatter] Ring adjustment cancelled" << std::endl;
            m_ringDragging = false;
        }
    }

    // Retopology: G key grab — press G near a vertex to grab it, move mouse to slide on surface,
    // LMB to confirm, ESC to cancel
    // Works with both quad vertices and scatter points
    if (m_retopologyMode && m_retopologyLiveObj && !m_ringDragging
        && (!m_retopologyQuads.empty() || !m_retopologyVerts.empty())) {
        // If already dragging and G pressed, cancel the current drag
        if (m_retopologyDragging && Input::isKeyPressed(Input::KEY_G)
            && !Input::isKeyDown(Input::KEY_LEFT_CONTROL) && !Input::isKeyDown(Input::KEY_RIGHT_CONTROL)) {
            // Restore original position
            if (m_retopologyDragQuadIdx >= 0) {
                glm::vec3 currentPos = m_retopologyQuads[m_retopologyDragQuadIdx].verts[m_retopologyDragQuadVert];
                const float mt = 0.001f;
                for (auto& quad : m_retopologyQuads) {
                    for (int vi = 0; vi < 4; ++vi) {
                        if (glm::length(quad.verts[vi] - currentPos) < mt)
                            quad.verts[vi] = m_retopologyDragOrigPos;
                    }
                }
            } else if (m_retopologyDragQuadVert >= 0 && m_retopologyDragQuadVert < static_cast<int>(m_retopologyVerts.size())) {
                m_retopologyVerts[m_retopologyDragQuadVert] = m_retopologyDragOrigPos;
            }
            m_retopologyDragging = false;
            m_retopologyDragQuadIdx = -1;
            m_retopologyDragQuadVert = -1;
            std::cout << "[Retopo] Grab cancelled (G pressed while dragging)" << std::endl;
        }

        if (!m_retopologyDragging && Input::isKeyPressed(Input::KEY_G)
            && !Input::isKeyDown(Input::KEY_LEFT_CONTROL) && !Input::isKeyDown(Input::KEY_RIGHT_CONTROL)) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            Camera& grabCam = m_ctx.getActiveCamera();
            glm::mat4 grabVP = grabCam.getProjectionMatrix(
                static_cast<float>(m_ctx.window.getWidth()) / static_cast<float>(m_ctx.window.getHeight()))
                * grabCam.getViewMatrix();
            float gVpW = static_cast<float>(m_ctx.window.getWidth());
            float gVpH = static_cast<float>(m_ctx.window.getHeight());
            ImVec2 gMousePos = ImGui::GetMousePos();

            auto grabWorldToScreen = [&](const glm::vec3& pos) -> glm::vec2 {
                glm::vec4 clip = grabVP * glm::vec4(pos, 1.0f);
                if (clip.w <= 0.0f) return glm::vec2(-10000.0f);
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                return glm::vec2((ndc.x + 1.0f) * 0.5f * gVpW, (1.0f - ndc.y) * 0.5f * gVpH);
            };

            float closestPixelDist = std::numeric_limits<float>::max();
            int foundQuadIdx = -1;
            int foundQuadVert = -1;
            int foundScatterIdx = -1;  // Index into m_retopologyVerts
            const float grabPixelThreshold = 20.0f;

            // Search quad vertices
            for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
                glm::vec3 e1 = m_retopologyQuads[qi].verts[1] - m_retopologyQuads[qi].verts[0];
                glm::vec3 e2 = m_retopologyQuads[qi].verts[3] - m_retopologyQuads[qi].verts[0];
                glm::vec3 fNorm = glm::cross(e1, e2);
                glm::vec3 qCenter = (m_retopologyQuads[qi].verts[0] + m_retopologyQuads[qi].verts[1] +
                                      m_retopologyQuads[qi].verts[2] + m_retopologyQuads[qi].verts[3]) * 0.25f;
                if (glm::dot(fNorm, rayOrigin - qCenter) <= 0.0f) continue;

                for (int vi = 0; vi < 4; ++vi) {
                    glm::vec2 screenPos = grabWorldToScreen(m_retopologyQuads[qi].verts[vi]);
                    float pixelDist = glm::length(screenPos - glm::vec2(gMousePos.x, gMousePos.y));
                    if (pixelDist < grabPixelThreshold && pixelDist < closestPixelDist) {
                        closestPixelDist = pixelDist;
                        foundQuadIdx = static_cast<int>(qi);
                        foundQuadVert = vi;
                        foundScatterIdx = -1;
                    }
                }
            }

            // Search scatter vertices (backface culling disabled for debugging)
            for (size_t i = 0; i < m_retopologyVerts.size(); i++) {
                glm::vec2 screenPos = grabWorldToScreen(m_retopologyVerts[i]);
                float pixelDist = glm::length(screenPos - glm::vec2(gMousePos.x, gMousePos.y));
                if (pixelDist < grabPixelThreshold && pixelDist < closestPixelDist) {
                    closestPixelDist = pixelDist;
                    foundScatterIdx = static_cast<int>(i);
                    foundQuadIdx = -1;
                    foundQuadVert = -1;
                }
            }

            if (foundQuadIdx >= 0) {
                m_retopologyDragging = true;
                m_retopologyDragQuadIdx = foundQuadIdx;
                m_retopologyDragQuadVert = foundQuadVert;
                m_retopologyDragOrigPos = m_retopologyQuads[foundQuadIdx].verts[foundQuadVert];
                std::cout << "[Retopo] Grabbed quad vertex — move mouse, LMB to confirm, ESC to cancel" << std::endl;
            } else if (foundScatterIdx >= 0) {
                m_retopologyDragging = true;
                m_retopologyDragQuadIdx = -1;
                m_retopologyDragQuadVert = foundScatterIdx;
                m_retopologyDragOrigPos = m_retopologyVerts[foundScatterIdx];

                // Find mirror point at grab start using stored pairs
                m_dragMirrorIdx = -1;
                if (m_scatterSymmetryX) {
                    auto pairIt = m_mirrorPairs.find(static_cast<size_t>(foundScatterIdx));
                    if (pairIt != m_mirrorPairs.end()) {
                        m_dragMirrorIdx = static_cast<int>(pairIt->second);
                    }
                }
                std::cout << "[Retopo] Grabbed scatter point " << foundScatterIdx
                          << (m_dragMirrorIdx >= 0 ? " (mirror: " + std::to_string(m_dragMirrorIdx) + ")" : "")
                          << " — move mouse, LMB to confirm, ESC to cancel" << std::endl;
            }
        }

        // While grabbing, slide vertex on live surface each frame
        if (m_retopologyDragging) {
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            auto hit = m_retopologyLiveObj->raycast(rayOrigin, rayDir);
            if (hit.hit) {
                if (m_retopologyDragQuadIdx >= 0) {
                    // Dragging a quad vertex
                    glm::vec3 oldPos = m_retopologyQuads[m_retopologyDragQuadIdx].verts[m_retopologyDragQuadVert];
                    const float mergeThreshold = 0.001f;
                    for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
                        for (int vi = 0; vi < 4; ++vi) {
                            if (glm::length(m_retopologyQuads[qi].verts[vi] - oldPos) < mergeThreshold) {
                                m_retopologyQuads[qi].verts[vi] = hit.position;
                            }
                        }
                    }
                } else if (m_retopologyDragQuadVert >= 0 && m_retopologyDragQuadVert < static_cast<int>(m_retopologyVerts.size())) {
                    // Dragging a scatter point
                    int si = m_retopologyDragQuadVert;
                    glm::vec3 oldPos = m_retopologyVerts[si];
                    m_retopologyVerts[si] = hit.position;
                    m_retopologyNormals[si] = hit.normal;

                    // Symmetry X: move the mirror point (found at grab start)
                    if (m_scatterSymmetryX && m_dragMirrorIdx >= 0
                        && m_dragMirrorIdx < static_cast<int>(m_retopologyVerts.size())) {
                        glm::vec3 mirrorTarget(-hit.position.x, hit.position.y, hit.position.z);
                        glm::vec3 mirrorNormal = m_retopologyNormals[m_dragMirrorIdx];
                        if (glm::length(mirrorNormal) < 0.001f)
                            mirrorNormal = glm::vec3(-hit.normal.x, hit.normal.y, hit.normal.z);

                        glm::vec3 mirrorRayOrigin = mirrorTarget + mirrorNormal * 0.05f;
                        auto mirrorHit = m_retopologyLiveObj->raycast(mirrorRayOrigin, -mirrorNormal);

                        glm::vec3 oldMirrorPos = m_retopologyVerts[m_dragMirrorIdx];
                        glm::vec3 newMirrorPos = (mirrorHit.hit && glm::length(mirrorHit.position - mirrorTarget) < 0.15f)
                            ? mirrorHit.position : mirrorTarget;
                        glm::vec3 newMirrorNrm = (mirrorHit.hit && glm::length(mirrorHit.position - mirrorTarget) < 0.15f)
                            ? mirrorHit.normal : mirrorNormal;

                        m_retopologyVerts[m_dragMirrorIdx] = newMirrorPos;
                        m_retopologyNormals[m_dragMirrorIdx] = newMirrorNrm;

                        const float mt = 0.001f;
                        for (auto& quad : m_retopologyQuads) {
                            for (int vi = 0; vi < 4; ++vi) {
                                if (glm::length(quad.verts[vi] - oldMirrorPos) < mt)
                                    quad.verts[vi] = newMirrorPos;
                            }
                        }
                    }

                    // Update quads referencing the dragged point
                    const float mergeThreshold = 0.001f;
                    for (auto& quad : m_retopologyQuads) {
                        for (int vi = 0; vi < 4; ++vi) {
                            if (glm::length(quad.verts[vi] - oldPos) < mergeThreshold) {
                                quad.verts[vi] = hit.position;
                            }
                        }
                    }
                }
            }

            // LMB confirms the grab
            if (Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
                std::cout << "[Retopo] Vertex placement confirmed" << std::endl;
                m_retopologyDragging = false;
                m_retopologyDragQuadIdx = -1;
                m_retopologyDragQuadVert = -1;
            }

            // ESC or RMB cancels — restore original position
            if (Input::isKeyPressed(Input::KEY_ESCAPE) || Input::isMouseButtonPressed(Input::MOUSE_RIGHT)) {
                if (m_retopologyDragQuadIdx >= 0) {
                    glm::vec3 currentPos = m_retopologyQuads[m_retopologyDragQuadIdx].verts[m_retopologyDragQuadVert];
                    const float mergeThreshold = 0.001f;
                    for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
                        for (int vi = 0; vi < 4; ++vi) {
                            if (glm::length(m_retopologyQuads[qi].verts[vi] - currentPos) < mergeThreshold) {
                                m_retopologyQuads[qi].verts[vi] = m_retopologyDragOrigPos;
                            }
                        }
                    }
                } else if (m_retopologyDragQuadVert >= 0 && m_retopologyDragQuadVert < static_cast<int>(m_retopologyVerts.size())) {
                    m_retopologyVerts[m_retopologyDragQuadVert] = m_retopologyDragOrigPos;
                }
                std::cout << "[Retopo] Grab cancelled" << std::endl;
                m_retopologyDragging = false;
                m_retopologyDragQuadIdx = -1;
                m_retopologyDragQuadVert = -1;
            }
        }
    }

    // Retopology: Ctrl+Z undo — remove last placed vertex, or last quad if no verts pending
    if (m_retopologyMode && !m_retopologyDragging &&
        Input::isKeyPressed(Input::KEY_Z) &&
        (Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL))) {
        if (!m_retopologyVerts.empty()) {
            m_retopologyVerts.pop_back();
            m_retopologyNormals.pop_back();
            m_retopologyVertMeshIdx.pop_back();
            std::cout << "[Retopo] Undo: removed last vertex, " << m_retopologyVerts.size() << " remaining" << std::endl;
        } else if (!m_retopologyQuads.empty()) {
            m_retopologyQuads.pop_back();
            std::cout << "[Retopo] Undo: removed last quad, " << m_retopologyQuads.size() << " remaining" << std::endl;
        }
    }

    // Retopology: Del key — delete the front-facing quad nearest to cursor (screen-space)
    if (m_retopologyMode && !m_retopologyDragging && !m_retopologyQuads.empty() &&
        Input::isKeyPressed(Input::KEY_DELETE)) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        Camera& delCam = m_ctx.getActiveCamera();
        glm::mat4 delVP = delCam.getProjectionMatrix(
            static_cast<float>(m_ctx.window.getWidth()) / static_cast<float>(m_ctx.window.getHeight()))
            * delCam.getViewMatrix();
        float dVpW = static_cast<float>(m_ctx.window.getWidth());
        float dVpH = static_cast<float>(m_ctx.window.getHeight());
        ImVec2 dMousePos = ImGui::GetMousePos();

        auto delWorldToScreen = [&](const glm::vec3& pos) -> glm::vec2 {
            glm::vec4 clip = delVP * glm::vec4(pos, 1.0f);
            if (clip.w <= 0.0f) return glm::vec2(-10000.0f);
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return glm::vec2((ndc.x + 1.0f) * 0.5f * dVpW, (1.0f - ndc.y) * 0.5f * dVpH);
        };

        float closestPixelDist = std::numeric_limits<float>::max();
        int deleteQuadIdx = -1;
        const float delPixelThreshold = 20.0f;

        for (size_t qi = 0; qi < m_retopologyQuads.size(); ++qi) {
            // Backface cull
            glm::vec3 e1 = m_retopologyQuads[qi].verts[1] - m_retopologyQuads[qi].verts[0];
            glm::vec3 e2 = m_retopologyQuads[qi].verts[3] - m_retopologyQuads[qi].verts[0];
            glm::vec3 fNorm = glm::cross(e1, e2);
            glm::vec3 qCenter = (m_retopologyQuads[qi].verts[0] + m_retopologyQuads[qi].verts[1] +
                                  m_retopologyQuads[qi].verts[2] + m_retopologyQuads[qi].verts[3]) * 0.25f;
            if (glm::dot(fNorm, rayOrigin - qCenter) <= 0.0f) continue;

            for (int vi = 0; vi < 4; ++vi) {
                glm::vec2 screenPos = delWorldToScreen(m_retopologyQuads[qi].verts[vi]);
                float pixelDist = glm::length(screenPos - glm::vec2(dMousePos.x, dMousePos.y));
                if (pixelDist < delPixelThreshold && pixelDist < closestPixelDist) {
                    closestPixelDist = pixelDist;
                    deleteQuadIdx = static_cast<int>(qi);
                }
            }
        }

        if (deleteQuadIdx >= 0) {
            m_retopologyQuads.erase(m_retopologyQuads.begin() + deleteQuadIdx);
            std::cout << "[Retopo] Deleted quad, " << m_retopologyQuads.size() << " remaining" << std::endl;
        }
    }

    // ESC cancels retopology mode (only when not grabbing a vertex)
    if (m_retopologyMode && !m_retopologyDragging && Input::isKeyPressed(Input::KEY_ESCAPE)) {
        cancelRetopologyMode();
    }

    // Slice mode input
    if (m_sliceMode) {
        if (Input::isKeyPressed(Input::KEY_ESCAPE)) {
            cancelSliceMode();
            return;
        }
        if (Input::isKeyPressed(Input::KEY_ENTER)) {
            performSlice();
            return;
        }
    }

    // Path tube input processing
    processPathTubeInput(mouseOverImGui);

    // Snap mode face selection
    if (m_snapMode && !mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        // Raycast against all objects to find clicked face
        SceneObject* hitObject = nullptr;
        int hitFace = -1;
        float closestDist = std::numeric_limits<float>::max();

        for (auto& objPtr : m_ctx.sceneObjects) {
            SceneObject* obj = objPtr.get();
            if (!obj || !obj->hasEditableMeshData()) continue;

            glm::mat4 modelMatrix = obj->getTransform().getMatrix();
            glm::mat4 invModel = glm::inverse(modelMatrix);
            glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

            // Raycast against faces using half-edge data
            const auto& heVerts = obj->getHEVertices();
            const auto& heEdges = obj->getHEHalfEdges();
            const auto& heFaces = obj->getHEFaces();

            for (size_t faceIdx = 0; faceIdx < heFaces.size(); ++faceIdx) {
                // Collect face vertices
                std::vector<uint32_t> faceVertIndices;
                uint32_t startHE = heFaces[faceIdx].halfEdgeIndex;
                uint32_t currHE = startHE;
                do {
                    faceVertIndices.push_back(heEdges[currHE].vertexIndex);
                    currHE = heEdges[currHE].nextIndex;
                } while (currHE != startHE && faceVertIndices.size() < 10);

                if (faceVertIndices.size() < 3) continue;

                // Triangulate and test each triangle (fan triangulation)
                for (size_t i = 1; i + 1 < faceVertIndices.size(); ++i) {
                    glm::vec3 v0 = heVerts[faceVertIndices[0]].position;
                    glm::vec3 v1 = heVerts[faceVertIndices[i]].position;
                    glm::vec3 v2 = heVerts[faceVertIndices[i + 1]].position;

                    // Moller-Trumbore intersection
                    glm::vec3 edge1 = v1 - v0;
                    glm::vec3 edge2 = v2 - v0;
                    glm::vec3 h = glm::cross(localRayDir, edge2);
                    float a = glm::dot(edge1, h);

                    if (std::abs(a) < 0.0001f) continue;

                    float f = 1.0f / a;
                    glm::vec3 s = localRayOrigin - v0;
                    float u = f * glm::dot(s, h);

                    if (u < 0.0f || u > 1.0f) continue;

                    glm::vec3 q = glm::cross(s, edge1);
                    float v = f * glm::dot(localRayDir, q);

                    if (v < 0.0f || u + v > 1.0f) continue;

                    float t = f * glm::dot(edge2, q);
                    if (t > 0.0001f && t < closestDist) {
                        closestDist = t;
                        hitObject = obj;
                        hitFace = static_cast<int>(faceIdx);
                    }
                }
            }
        }

        if (hitObject && hitFace >= 0) {
            if (m_snapSourceFace == -1) {
                // First face selection - store as source
                m_snapSourceObject = hitObject;
                m_snapSourceFace = hitFace;
                m_snapSourceCenter = getFaceCenter(hitObject, hitFace);
                m_snapSourceNormal = getFaceNormal(hitObject, hitFace);
                std::cout << "[Snap] Selected source face " << hitFace << " on " << hitObject->getName() << std::endl;
            } else {
                // Second face selection - execute snap based on mode
                if (hitObject == m_snapSourceObject) {
                    std::cout << "[Snap] Cannot snap to same object" << std::endl;
                } else {
                    if (m_snapMergeMode) {
                        // Snap, weld vertices, and merge
                        snapAndMergeObjects(m_snapSourceObject, m_snapSourceFace, hitObject, hitFace);
                    } else {
                        // Just snap (keep separate)
                        glm::vec3 snapPoint = getFaceCenter(hitObject, hitFace);
                        snapObjectToFace(m_snapSourceObject, m_snapSourceFace, hitObject, hitFace);

                        // Select the snapped object and set up rotation gizmo at snap point
                        m_ctx.selectedObject = m_snapSourceObject;
                        m_ctx.objectMode = true;
                        m_ctx.gizmoMode = GizmoMode::Rotate;
                        m_useCustomGizmoPivot = true;
                        m_customGizmoPivot = snapPoint;
                        buildEditableMeshFromObject();
                    }
                    cancelSnapMode();
                }
            }
        }
        return;  // Don't process other input when in snap mode
    }

    // Object mode viewport selection
    // Works when: Q mode (no gizmo), OR when gizmo active but clicking off the gizmo (to quickly switch objects)
    bool canSelectInViewport = m_ctx.objectMode && !mouseOverImGui && !m_ctx.gizmoDragging &&
                               !m_retopologyMode &&
                               Input::isMouseButtonPressed(Input::MOUSE_LEFT) &&
                               (m_ctx.gizmoMode == GizmoMode::None || m_ctx.gizmoHoveredAxis == GizmoAxis::None);

    if (canSelectInViewport) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        // Raycast against all scene objects (SceneObject::raycast expects world space)
        SceneObject* hitObject = nullptr;
        float closestDist = FLT_MAX;

        for (auto& obj : m_ctx.sceneObjects) {
            if (!obj->isVisible() || !obj->hasMeshData()) continue;

            auto hit = obj->raycast(rayOrigin, rayDir);
            if (hit.hit && hit.distance < closestDist) {
                closestDist = hit.distance;
                hitObject = obj.get();
            }
        }

        bool shiftHeld = Input::isKeyDown(Input::KEY_LEFT_SHIFT) || Input::isKeyDown(Input::KEY_RIGHT_SHIFT);
        bool ctrlHeld = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);

        if (hitObject) {
            // Only change selection if clicking a DIFFERENT object, or using modifier keys
            bool isDifferentObject = (hitObject != m_ctx.selectedObject);

            if (ctrlHeld) {
                // Ctrl+Click: Toggle selection
                if (m_ctx.selectedObjects.count(hitObject) > 0) {
                    m_ctx.selectedObjects.erase(hitObject);
                } else {
                    m_ctx.selectedObjects.insert(hitObject);
                }
                // Update primary selection
                m_ctx.selectedObject = hitObject;
                buildEditableMeshFromObject();
            } else if (shiftHeld) {
                // Shift+Click: Add to selection
                m_ctx.selectedObjects.insert(hitObject);
                m_ctx.selectedObject = hitObject;
                buildEditableMeshFromObject();
            } else if (isDifferentObject) {
                // Normal click on different object: Select it (gizmo stays active)
                m_ctx.selectedObjects.clear();
                m_ctx.selectedObjects.insert(hitObject);
                m_ctx.selectedObject = hitObject;
                buildEditableMeshFromObject();
            }
            // If clicking same object without modifiers and gizmo is active, do nothing (let gizmo handle it)
        } else if (m_ctx.gizmoMode == GizmoMode::None) {
            // Only clear selection when in Q mode (no gizmo) and clicking on empty space
            // When gizmo is active, clicking empty space does nothing (preserves selection)
            m_ctx.selectedObjects.clear();
            m_ctx.selectedObject = nullptr;
            m_ctx.editableMesh.clear();
            m_ctx.meshDirty = false;
        }
    }

    // Mouse selection (skip if gizmo is active or in object mode)
    if (!mouseOverImGui && !gizmoActive && !m_ctx.objectMode && m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
        glm::mat4 invModel = glm::inverse(modelMatrix);
        glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
        glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

        float threshold = (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) ?
                          m_ctx.vertexDisplaySize * 2.0f : 0.05f;

        auto hit = m_ctx.editableMesh.raycast(localRayOrigin, localRayDir, m_ctx.modelingSelectionMode, threshold, m_ctx.hiddenFaces);

        // For edge and vertex modes, override with screen-space picking for accuracy
        if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Edge ||
            m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
            float fullW = static_cast<float>(m_ctx.window.getWidth());
            float fullH = static_cast<float>(m_ctx.window.getHeight());
            float vpX = 0.0f, vpW = fullW, vpH = fullH;
            bool useRight = false;
            if (m_ctx.splitView) {
                vpW = fullW / 2.0f;
                glm::vec2 mp = Input::getMousePosition();
                if (mp.x >= fullW / 2.0f) { vpX = fullW / 2.0f; useRight = true; }
            }
            Camera& cam = (m_ctx.splitView && useRight) ? m_ctx.camera2 : m_ctx.camera;
            glm::mat4 vMat = cam.getViewMatrix();
            glm::mat4 pMat = cam.getProjectionMatrix(vpW / vpH);
            glm::mat4 mvpMat = pMat * vMat * modelMatrix;
            glm::vec3 camPos = cam.getPosition();
            glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
            glm::vec2 mousePos = Input::getMousePosition();

            auto localToScreen = [&](const glm::vec3& localPos) -> glm::vec3 {
                glm::vec4 clip = mvpMat * glm::vec4(localPos, 1.0f);
                if (clip.w <= 0.0f) return glm::vec3(-10000, -10000, -1.0f);
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                return glm::vec3(vpX + (ndc.x * 0.5f + 0.5f) * vpW,
                                 (1.0f - (ndc.y * 0.5f + 0.5f)) * vpH,
                                 ndc.z);
            };

            // Backface test helper
            auto isFaceFront = [&](uint32_t fIdx) -> bool {
                if (fIdx == UINT32_MAX) return true; // boundary = visible
                if (m_ctx.hiddenFaces.count(fIdx)) return false;
                glm::vec3 fc = m_ctx.editableMesh.getFaceCenter(fIdx);
                glm::vec3 fn = m_ctx.editableMesh.getFaceNormal(fIdx);
                glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(fc, 1.0f));
                glm::vec3 wn = glm::normalize(normalMat * fn);
                return glm::dot(wn, glm::normalize(camPos - wc)) > 0.0f;
            };

            if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Edge) {
                // Point-to-line-segment distance in 2D
                auto pointToSegDist = [](glm::vec2 p, glm::vec2 a, glm::vec2 b) -> float {
                    glm::vec2 ab = b - a;
                    float len2 = glm::dot(ab, ab);
                    if (len2 < 0.0001f) return glm::length(p - a);
                    float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
                    glm::vec2 proj = a + ab * t;
                    return glm::length(p - proj);
                };

                float bestDist = 15.0f; // pixel threshold
                int bestHE = -1;
                std::set<uint64_t> processedEdges;

                for (uint32_t i = 0; i < m_ctx.editableMesh.getHalfEdgeCount(); ++i) {
                    auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(i);
                    if (v0 == UINT32_MAX || v1 == UINT32_MAX) continue;
                    uint32_t minV = std::min(v0, v1), maxV = std::max(v0, v1);
                    uint64_t key = (static_cast<uint64_t>(minV) << 32) | maxV;
                    if (processedEdges.count(key)) continue;
                    processedEdges.insert(key);

                    // Backface cull: skip edges where both adjacent faces are back-facing
                    uint32_t faceA = m_ctx.editableMesh.getHalfEdge(i).faceIndex;
                    uint32_t twin = m_ctx.editableMesh.getHalfEdge(i).twinIndex;
                    uint32_t faceB = (twin != UINT32_MAX) ? m_ctx.editableMesh.getHalfEdge(twin).faceIndex : UINT32_MAX;
                    if (!isFaceFront(faceA) && !isFaceFront(faceB)) continue;

                    glm::vec3 s0 = localToScreen(m_ctx.editableMesh.getVertex(v0).position);
                    glm::vec3 s1 = localToScreen(m_ctx.editableMesh.getVertex(v1).position);
                    if (s0.x < -5000 || s1.x < -5000) continue;

                    float d = pointToSegDist(mousePos, glm::vec2(s0), glm::vec2(s1));
                    if (d < bestDist) {
                        bestDist = d;
                        bestHE = static_cast<int>(i);
                    }
                }

                hit.hit = (bestHE >= 0);
                hit.edgeIndex = (bestHE >= 0) ? static_cast<uint32_t>(bestHE) : UINT32_MAX;
            } else {
                // Vertex mode: screen-space point distance with backface culling
                float bestDist = 15.0f; // pixel threshold
                int bestVert = -1;

                // Build set of vertices on front-facing faces
                std::set<uint32_t> frontVertices;
                for (uint32_t fIdx = 0; fIdx < m_ctx.editableMesh.getFaceCount(); ++fIdx) {
                    if (!isFaceFront(fIdx)) continue;
                    auto verts = m_ctx.editableMesh.getFaceVertices(fIdx);
                    for (uint32_t vi : verts) frontVertices.insert(vi);
                }

                for (uint32_t vi : frontVertices) {
                    glm::vec3 sp = localToScreen(m_ctx.editableMesh.getVertex(vi).position);
                    if (sp.x < -5000) continue;
                    float d = glm::length(mousePos - glm::vec2(sp));
                    if (d < bestDist) {
                        bestDist = d;
                        bestVert = static_cast<int>(vi);
                    }
                }

                hit.hit = (bestVert >= 0);
                hit.vertexIndex = (bestVert >= 0) ? static_cast<uint32_t>(bestVert) : UINT32_MAX;
            }
        }

        // Disable hover highlighting when in paint mode
        if (m_ctx.isPainting) {
            m_ctx.hoveredVertex = -1;
            m_ctx.hoveredEdge = -1;
            m_ctx.hoveredFace = -1;
        } else {
            m_ctx.hoveredVertex = (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex && hit.hit) ?
                                  static_cast<int>(hit.vertexIndex) : -1;
            m_ctx.hoveredEdge = (m_ctx.modelingSelectionMode == ModelingSelectionMode::Edge && hit.hit) ?
                                static_cast<int>(hit.edgeIndex) : -1;
            m_ctx.hoveredFace = (m_ctx.modelingSelectionMode == ModelingSelectionMode::Face && hit.hit) ?
                                static_cast<int>(hit.faceIndex) : -1;
        }

        // Skip selection when in paint mode (painting uses LMB, Ctrl+Click sets clone source)
        bool shiftHeld = Input::isKeyDown(Input::KEY_LEFT_SHIFT) || Input::isKeyDown(Input::KEY_RIGHT_SHIFT);
        bool ctrlHeld = Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL);

        // Normal selection mode - click for point select, drag for rectangle select
        if (m_ctx.selectionTool == SelectionTool::Normal && !m_ctx.isPainting && !m_patchBlanketMode) {
            if (Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
                // Start tracking for potential rectangle selection
                m_ctx.isRectSelecting = true;
                m_ctx.rectSelectStart = Input::getMousePosition();
                m_ctx.rectSelectEnd = m_ctx.rectSelectStart;
            }
            if (m_ctx.isRectSelecting && Input::isMouseButtonDown(Input::MOUSE_LEFT)) {
                // Update rectangle end point while dragging
                m_ctx.rectSelectEnd = Input::getMousePosition();
            }
            if (m_ctx.isRectSelecting && !Input::isMouseButtonDown(Input::MOUSE_LEFT)) {
                // Mouse released - decide between point select and rect select
                m_ctx.isRectSelecting = false;

                // Calculate drag distance
                float dragDist = glm::length(m_ctx.rectSelectEnd - m_ctx.rectSelectStart);
                const float dragThreshold = 5.0f;  // Pixels - below this is a click

                if (dragDist >= dragThreshold) {
                    // Rectangle selection
                    if (!shiftHeld) {
                        m_ctx.editableMesh.clearSelection();
                    }

                    // Get viewport info for projection (handle split view)
                    float fullWidth = static_cast<float>(m_ctx.window.getWidth());
                    float fullHeight = static_cast<float>(m_ctx.window.getHeight());

                    float vpX = 0.0f;
                    float vpWidth = fullWidth;
                    float vpHeight = fullHeight;

                    bool useRightViewport = false;
                    if (m_ctx.splitView) {
                        vpWidth = fullWidth / 2.0f;
                        if (m_ctx.rectSelectStart.x >= fullWidth / 2.0f) {
                            vpX = fullWidth / 2.0f;
                            useRightViewport = true;
                        }
                    }

                    Camera& cam = (m_ctx.splitView && useRightViewport) ? m_ctx.camera2 : m_ctx.camera;
                    glm::mat4 view = cam.getViewMatrix();
                    glm::mat4 proj = cam.getProjectionMatrix(vpWidth / vpHeight);
                    glm::mat4 mvp = proj * view * m_ctx.selectedObject->getTransform().getMatrix();

                    float minX = std::min(m_ctx.rectSelectStart.x, m_ctx.rectSelectEnd.x);
                    float maxX = std::max(m_ctx.rectSelectStart.x, m_ctx.rectSelectEnd.x);
                    float minY = std::min(m_ctx.rectSelectStart.y, m_ctx.rectSelectEnd.y);
                    float maxY = std::max(m_ctx.rectSelectStart.y, m_ctx.rectSelectEnd.y);

                    if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
                        for (size_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                            glm::vec3 pos = m_ctx.editableMesh.getVertex(i).position;
                            glm::vec4 clip = mvp * glm::vec4(pos, 1.0f);
                            if (clip.w > 0.0f) {
                                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                float screenX = vpX + (ndc.x * 0.5f + 0.5f) * vpWidth;
                                float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * vpHeight;
                                if (screenX >= minX && screenX <= maxX && screenY >= minY && screenY <= maxY) {
                                    m_ctx.editableMesh.selectVertex(i, true);
                                }
                            }
                        }
                    } else if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Face) {
                        for (size_t i = 0; i < m_ctx.editableMesh.getFaceCount(); ++i) {
                            glm::vec3 center = m_ctx.editableMesh.getFaceCenter(i);
                            glm::vec4 clip = mvp * glm::vec4(center, 1.0f);
                            if (clip.w > 0.0f) {
                                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                float screenX = vpX + (ndc.x * 0.5f + 0.5f) * vpWidth;
                                float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * vpHeight;
                                if (screenX >= minX && screenX <= maxX && screenY >= minY && screenY <= maxY) {
                                    m_ctx.editableMesh.selectFace(i, true);
                                }
                            }
                        }
                    } else if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Edge) {
                        for (size_t i = 0; i < m_ctx.editableMesh.getHalfEdgeCount(); ++i) {
                            auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(i);
                            glm::vec3 midpoint = (m_ctx.editableMesh.getVertex(v0).position +
                                                  m_ctx.editableMesh.getVertex(v1).position) * 0.5f;
                            glm::vec4 clip = mvp * glm::vec4(midpoint, 1.0f);
                            if (clip.w > 0.0f) {
                                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                                float screenX = vpX + (ndc.x * 0.5f + 0.5f) * vpWidth;
                                float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * vpHeight;
                                if (screenX >= minX && screenX <= maxX && screenY >= minY && screenY <= maxY) {
                                    m_ctx.editableMesh.selectEdge(i, true);
                                }
                            }
                        }
                    }
                } else {
                    // Point selection (click)
                    double currentTime = glfwGetTime();
                    bool isDoubleClick = (currentTime - m_ctx.lastClickTime) < 0.3;
                    m_ctx.lastClickTime = currentTime;

                    if (hit.hit) {
                        switch (m_ctx.modelingSelectionMode) {
                            case ModelingSelectionMode::Vertex:
                                if (ctrlHeld) {
                                    m_ctx.editableMesh.toggleVertexSelection(hit.vertexIndex);
                                } else {
                                    m_ctx.editableMesh.selectVertex(hit.vertexIndex, shiftHeld);
                                }
                                break;

                            case ModelingSelectionMode::Edge: {
                                bool altHeld = Input::isKeyDown(Input::KEY_LEFT_ALT) || Input::isKeyDown(Input::KEY_RIGHT_ALT);
                                if (altHeld) {
                                    // Walk faces in the direction established by the clicked edge
                                    m_ctx.editableMesh.clearSelection();
                                    std::set<uint32_t> facesToSelect;

                                    auto walkFaceLoop = [&](uint32_t startHe) {
                                        uint32_t currentHe = startHe;
                                        uint32_t iterations = 0;
                                        const uint32_t maxIter = 1000;

                                        while (iterations++ < maxIter) {
                                            const auto& he = m_ctx.editableMesh.getHalfEdge(currentHe);
                                            if (he.faceIndex == UINT32_MAX) break;
                                            if (facesToSelect.count(he.faceIndex) > 0) break;

                                            const auto& face = m_ctx.editableMesh.getFace(he.faceIndex);
                                            facesToSelect.insert(he.faceIndex);

                                            if (face.vertexCount != 4) break;

                                            uint32_t next1 = he.nextIndex;
                                            uint32_t next2 = m_ctx.editableMesh.getHalfEdge(next1).nextIndex;
                                            uint32_t twinHe = m_ctx.editableMesh.getHalfEdge(next2).twinIndex;
                                            if (twinHe == UINT32_MAX) break;

                                            currentHe = twinHe;
                                        }
                                    };

                                    walkFaceLoop(hit.edgeIndex);
                                    uint32_t twinHe = m_ctx.editableMesh.getHalfEdge(hit.edgeIndex).twinIndex;
                                    if (twinHe != UINT32_MAX) {
                                        walkFaceLoop(twinHe);
                                    }

                                    for (uint32_t faceIdx : facesToSelect) {
                                        m_ctx.editableMesh.selectFace(faceIdx, true);
                                    }

                                    m_ctx.modelingSelectionMode = ModelingSelectionMode::Face;
                                } else if (isDoubleClick) {
                                    m_ctx.editableMesh.selectEdgeRing(hit.edgeIndex);
                                } else if (ctrlHeld) {
                                    m_ctx.editableMesh.toggleEdgeSelection(hit.edgeIndex);
                                } else {
                                    m_ctx.editableMesh.selectEdge(hit.edgeIndex, shiftHeld);
                                }
                                break;
                            }

                            case ModelingSelectionMode::Face: {
                                bool altHeld = Input::isKeyDown(Input::KEY_LEFT_ALT) || Input::isKeyDown(Input::KEY_RIGHT_ALT);
                                if (altHeld) {
                                    MeshRayHit edgeHit = m_ctx.editableMesh.raycastEdge(rayOrigin, rayDir, 0.1f);
                                    if (edgeHit.hit) {
                                        if (!shiftHeld) {
                                            m_ctx.editableMesh.clearSelection();
                                        }

                                        std::set<uint32_t> facesToSelect;

                                        auto walkFaceLoop = [&](uint32_t startHe) {
                                            uint32_t currentHe = startHe;
                                            uint32_t iterations = 0;
                                            const uint32_t maxIter = 1000;

                                            while (iterations++ < maxIter) {
                                                const auto& he = m_ctx.editableMesh.getHalfEdge(currentHe);
                                                if (he.faceIndex == UINT32_MAX) break;
                                                if (facesToSelect.count(he.faceIndex) > 0) break;
                                                // Skip hidden faces in loop
                                                if (m_ctx.hiddenFaces.find(he.faceIndex) != m_ctx.hiddenFaces.end()) break;

                                                const auto& face = m_ctx.editableMesh.getFace(he.faceIndex);
                                                facesToSelect.insert(he.faceIndex);

                                                if (face.vertexCount != 4) break;

                                                uint32_t next1 = he.nextIndex;
                                                uint32_t next2 = m_ctx.editableMesh.getHalfEdge(next1).nextIndex;
                                                uint32_t twinHe = m_ctx.editableMesh.getHalfEdge(next2).twinIndex;
                                                if (twinHe == UINT32_MAX) break;

                                                currentHe = twinHe;
                                            }
                                        };

                                        walkFaceLoop(edgeHit.edgeIndex);
                                        uint32_t twinHe = m_ctx.editableMesh.getHalfEdge(edgeHit.edgeIndex).twinIndex;
                                        if (twinHe != UINT32_MAX) {
                                            walkFaceLoop(twinHe);
                                        }

                                        for (uint32_t faceIdx : facesToSelect) {
                                            m_ctx.editableMesh.selectFace(faceIdx, true);
                                        }
                                    }
                                } else if (ctrlHeld) {
                                    m_ctx.editableMesh.toggleFaceSelection(hit.faceIndex);
                                } else {
                                    m_ctx.editableMesh.selectFace(hit.faceIndex, shiftHeld);
                                }
                                break;
                            }
                        }
                    } else {
                        // Click missed geometry - clear selection
                        if (m_ctx.gizmoMode == GizmoMode::None) {
                            m_ctx.editableMesh.clearSelection();
                        }
                    }
                }
            }
        }

        // Paint select handling - continuous selection while dragging
        // Always accumulates; click empty space to clear, or Ctrl+click to deselect
        if (m_ctx.selectionTool == SelectionTool::Paint && !m_ctx.isPainting) {
            if (Input::isMouseButtonPressed(Input::MOUSE_LEFT) && !hit.hit && !shiftHeld) {
                m_ctx.editableMesh.clearSelection();
            }
            if (Input::isMouseButtonDown(Input::MOUSE_LEFT)) {
                if (hit.hit) {
                    switch (m_ctx.modelingSelectionMode) {
                        case ModelingSelectionMode::Vertex:
                            m_ctx.editableMesh.selectVertex(hit.vertexIndex, true);
                            break;
                        case ModelingSelectionMode::Edge:
                            m_ctx.editableMesh.selectEdge(hit.edgeIndex, true);
                            break;
                        case ModelingSelectionMode::Face:
                            m_ctx.editableMesh.selectFace(hit.faceIndex, true);
                            break;
                    }
                }
            }
        }
    }

    // RMB always starts tumble (useful in paint mode or when model fills screen)
    if (!mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_RIGHT)) {
        startCameraTumble();
    }
}

void ModelingMode::startCameraTumble() {
    Camera& cam = m_ctx.getActiveCamera();
    m_ctx.isTumbling = true;

    // Calculate current orbit angles from camera position relative to target
    glm::vec3 offset = cam.getPosition() - m_ctx.orbitTarget;
    float dist = glm::length(offset);
    if (dist > 0.001f) {
        offset = glm::normalize(offset);
        m_ctx.orbitPitch = glm::degrees(asin(glm::clamp(offset.y, -1.0f, 1.0f)));
        m_ctx.orbitYaw = glm::degrees(atan2(offset.z, offset.x));
    }
}

void ModelingMode::renderModelingOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->isVisible()) return;

    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();

    // Render selected faces (only in face mode to avoid showing stale selections)
    if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Face) {
        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
        if (!selectedFaces.empty()) {
            std::vector<uint32_t> triangleIndices;
            for (uint32_t faceIdx : selectedFaces) {
                auto origTris = m_ctx.faceToTriangles.find(faceIdx);
                if (origTris != m_ctx.faceToTriangles.end()) {
                    for (uint32_t triIdx : origTris->second) {
                        triangleIndices.push_back(triIdx);
                    }
                }
            }
            if (!triangleIndices.empty()) {
                m_ctx.modelRenderer.renderSelection(cmd, viewProj, m_ctx.selectedObject->getBufferHandle(),
                                                     modelMatrix, triangleIndices, m_ctx.modelingSelectionColor);
            }
        }
    }

    // Render hovered face
    if (m_ctx.hoveredFace >= 0 && m_ctx.modelingSelectionMode == ModelingSelectionMode::Face) {
        std::vector<uint32_t> triangleIndices;
        auto origTris = m_ctx.faceToTriangles.find(static_cast<uint32_t>(m_ctx.hoveredFace));
        if (origTris != m_ctx.faceToTriangles.end()) {
            for (uint32_t triIdx : origTris->second) {
                triangleIndices.push_back(triIdx);
            }
        }
        if (!triangleIndices.empty()) {
            m_ctx.modelRenderer.renderSelection(cmd, viewProj, m_ctx.selectedObject->getBufferHandle(),
                                                 modelMatrix, triangleIndices, m_ctx.modelingHoverColor);
        }
    }
}

void ModelingMode::renderWireframeOverlay3D(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->isVisible()) return;

    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();

    // Check if cache needs invalidation
    auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
    auto selectedVertSet = m_ctx.editableMesh.getSelectedVertices();
    if (modelMatrix != m_cachedModelMatrix ||
        selectedEdges.size() != m_cachedSelectedEdgeCount ||
        selectedVertSet.size() != m_cachedSelectedVertCount ||
        m_ctx.hoveredVertex != m_cachedHoveredVertex ||
        m_ctx.modelingSelectionMode != m_cachedSelectionMode) {
        m_wireframeDirty = true;
    }

    if (m_wireframeDirty) {
        m_wireframeDirty = false;
        m_cachedModelMatrix = modelMatrix;
        m_cachedSelectedEdgeCount = selectedEdges.size();
        m_cachedSelectedVertCount = selectedVertSet.size();
        m_cachedHoveredVertex = m_ctx.hoveredVertex;
        m_cachedSelectionMode = m_ctx.modelingSelectionMode;

        // --- Rebuild wireframe edge cache ---
        auto posKey = [](const glm::vec3& p) -> uint64_t {
            int32_t x = static_cast<int32_t>(p.x * 10000.0f);
            int32_t y = static_cast<int32_t>(p.y * 10000.0f);
            int32_t z = static_cast<int32_t>(p.z * 10000.0f);
            return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
                   (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
                   static_cast<uint64_t>(z & 0xFFFFF);
        };
        auto edgePosKey = [&](const glm::vec3& p0, const glm::vec3& p1) -> std::pair<uint64_t, uint64_t> {
            uint64_t k0 = posKey(p0);
            uint64_t k1 = posKey(p1);
            return k0 < k1 ? std::make_pair(k0, k1) : std::make_pair(k1, k0);
        };

        // Use unordered_set for O(1) edge dedup instead of std::set O(log n)
        struct PairHash {
            size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
                return std::hash<uint64_t>()(p.first) ^ (std::hash<uint64_t>()(p.second) << 32);
            }
        };
        std::unordered_set<std::pair<uint64_t, uint64_t>, PairHash> drawnEdges;

        m_cachedWireLines.clear();
        m_cachedSelectedLines.clear();

        // Get selected edges (using position-based keys)
        std::unordered_set<std::pair<uint64_t, uint64_t>, PairHash> selectedEdgeKeys;
        for (uint32_t he : selectedEdges) {
            auto [vi0, vi1] = m_ctx.editableMesh.getEdgeVertices(he);
            const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
            const auto& v1 = m_ctx.editableMesh.getVertex(vi1);
            selectedEdgeKeys.insert(edgePosKey(v0.position, v1.position));
        }

        uint32_t vertexCount = m_ctx.editableMesh.getVertexCount();
        uint32_t faceCount = m_ctx.editableMesh.getFaceCount();

        // Reserve approximate space (3 edges per face, 2 vertices per edge)
        drawnEdges.reserve(faceCount * 3);
        m_cachedWireLines.reserve(faceCount * 6);

        for (uint32_t faceIdx = 0; faceIdx < faceCount; ++faceIdx) {
            auto verts = m_ctx.editableMesh.getFaceVertices(faceIdx);
            if (verts.size() < 3) continue;

            for (size_t i = 0; i < verts.size(); ++i) {
                uint32_t vi0 = verts[i];
                uint32_t vi1 = verts[(i + 1) % verts.size()];
                if (vi0 >= vertexCount || vi1 >= vertexCount) continue;

                const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
                const auto& v1 = m_ctx.editableMesh.getVertex(vi1);

                auto edgeKey = edgePosKey(v0.position, v1.position);
                if (!drawnEdges.insert(edgeKey).second) continue;

                glm::vec3 worldV0 = glm::vec3(modelMatrix * glm::vec4(v0.position, 1.0f));
                glm::vec3 worldV1 = glm::vec3(modelMatrix * glm::vec4(v1.position, 1.0f));

                if (selectedEdgeKeys.count(edgeKey) > 0) {
                    m_cachedSelectedLines.push_back(worldV0);
                    m_cachedSelectedLines.push_back(worldV1);
                } else {
                    m_cachedWireLines.push_back(worldV0);
                    m_cachedWireLines.push_back(worldV1);
                }
            }
        }

        // --- Rebuild vertex cache ---
        m_cachedNormalVerts.clear();
        m_cachedSelectedVerts.clear();
        m_cachedHoveredVerts.clear();

        if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
            std::set<uint32_t> selectedSet(selectedVertSet.begin(), selectedVertSet.end());
            std::unordered_set<uint64_t> addedPositions;
            addedPositions.reserve(vertexCount);

            for (uint32_t vi = 0; vi < vertexCount; ++vi) {
                const auto& v = m_ctx.editableMesh.getVertex(vi);
                uint64_t key = posKey(v.position);
                if (!addedPositions.insert(key).second) continue;

                glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));

                if (static_cast<int>(vi) == m_ctx.hoveredVertex) {
                    m_cachedHoveredVerts.push_back(worldPos);
                } else if (selectedSet.count(vi) > 0) {
                    m_cachedSelectedVerts.push_back(worldPos);
                } else {
                    m_cachedNormalVerts.push_back(worldPos);
                }
            }
        }
    }

    // Render cached data
    if (!m_cachedWireLines.empty()) {
        m_ctx.modelRenderer.renderLines(cmd, viewProj, m_cachedWireLines, glm::vec3(0.0f, 0.0f, 0.0f));
    }
    if (!m_cachedSelectedLines.empty()) {
        m_ctx.modelRenderer.renderLines(cmd, viewProj, m_cachedSelectedLines, glm::vec3(0.2f, 0.4f, 1.0f));
    }

    if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
        if (!m_cachedNormalVerts.empty()) {
            m_ctx.modelRenderer.renderPoints(cmd, viewProj, m_cachedNormalVerts, glm::vec3(0.0f, 0.8f, 1.0f), 8.0f);
        }
        if (!m_cachedSelectedVerts.empty()) {
            m_ctx.modelRenderer.renderPoints(cmd, viewProj, m_cachedSelectedVerts, glm::vec3(1.0f, 0.6f, 0.0f), 10.0f);
        }
        if (!m_cachedHoveredVerts.empty()) {
            m_ctx.modelRenderer.renderPoints(cmd, viewProj, m_cachedHoveredVerts, glm::vec3(1.0f, 1.0f, 0.0f), 12.0f);
        }
    }

    // =========================================================================
    // UV Selection Highlighting in 3D View
    // =========================================================================
    // Show what's selected in the UV editor on the actual 3D model

    // Magenta color for UV selection
    glm::vec3 uvHighlightColor(1.0f, 0.0f, 1.0f);

    // Highlight UV-selected faces (render their edges)
    if (!m_ctx.uvSelectedFaces.empty()) {
        std::vector<glm::vec3> uvFaceLines;
        for (uint32_t faceIdx : m_ctx.uvSelectedFaces) {
            if (faceIdx >= m_ctx.editableMesh.getFaceCount()) continue;
            auto verts = m_ctx.editableMesh.getFaceVertices(faceIdx);
            for (size_t i = 0; i < verts.size(); ++i) {
                uint32_t vi0 = verts[i];
                uint32_t vi1 = verts[(i + 1) % verts.size()];
                if (vi0 >= m_ctx.editableMesh.getVertexCount() || vi1 >= m_ctx.editableMesh.getVertexCount()) continue;
                const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
                const auto& v1 = m_ctx.editableMesh.getVertex(vi1);
                glm::vec3 worldV0 = glm::vec3(modelMatrix * glm::vec4(v0.position, 1.0f));
                glm::vec3 worldV1 = glm::vec3(modelMatrix * glm::vec4(v1.position, 1.0f));
                uvFaceLines.push_back(worldV0);
                uvFaceLines.push_back(worldV1);
            }
        }
        if (!uvFaceLines.empty()) {
            m_ctx.modelRenderer.renderLines(cmd, viewProj, uvFaceLines, uvHighlightColor);
        }
    }

    // Highlight UV-selected vertices
    if (!m_ctx.uvSelectedVertices.empty()) {
        std::vector<glm::vec3> uvVertPoints;
        for (uint32_t vi : m_ctx.uvSelectedVertices) {
            if (vi >= m_ctx.editableMesh.getVertexCount()) continue;
            const auto& v = m_ctx.editableMesh.getVertex(vi);
            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));
            uvVertPoints.push_back(worldPos);
        }
        if (!uvVertPoints.empty()) {
            m_ctx.modelRenderer.renderPoints(cmd, viewProj, uvVertPoints, uvHighlightColor, 12.0f);
        }
    }

    // Highlight UV-selected edge
    if (m_ctx.uvSelectedEdge.first != UINT32_MAX) {
        uint32_t faceIdx = m_ctx.uvSelectedEdge.first;
        uint32_t edgeIdx = m_ctx.uvSelectedEdge.second;
        if (faceIdx < m_ctx.editableMesh.getFaceCount()) {
            auto verts = m_ctx.editableMesh.getFaceVertices(faceIdx);
            if (edgeIdx < verts.size()) {
                uint32_t vi0 = verts[edgeIdx];
                uint32_t vi1 = verts[(edgeIdx + 1) % verts.size()];
                if (vi0 < m_ctx.editableMesh.getVertexCount() && vi1 < m_ctx.editableMesh.getVertexCount()) {
                    const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
                    const auto& v1 = m_ctx.editableMesh.getVertex(vi1);
                    glm::vec3 worldV0 = glm::vec3(modelMatrix * glm::vec4(v0.position, 1.0f));
                    glm::vec3 worldV1 = glm::vec3(modelMatrix * glm::vec4(v1.position, 1.0f));
                    std::vector<glm::vec3> uvEdgeLine = {worldV0, worldV1};
                    m_ctx.modelRenderer.renderLines(cmd, viewProj, uvEdgeLine, uvHighlightColor);
                }
            }
        }
    }
}

void ModelingMode::renderGrid3D(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    std::vector<glm::vec3> gridLines;
    const float gridSize = 10.0f;
    const int gridLines_count = 21;
    const float spacing = gridSize * 2.0f / (gridLines_count - 1);

    for (int i = 0; i < gridLines_count; ++i) {
        float z = -gridSize + i * spacing;
        gridLines.push_back(glm::vec3(-gridSize, 0, z));
        gridLines.push_back(glm::vec3(gridSize, 0, z));
    }

    for (int i = 0; i < gridLines_count; ++i) {
        float x = -gridSize + i * spacing;
        gridLines.push_back(glm::vec3(x, 0, -gridSize));
        gridLines.push_back(glm::vec3(x, 0, gridSize));
    }

    m_ctx.modelRenderer.renderLines(cmd, viewProj, gridLines, glm::vec3(0.3f, 0.3f, 0.35f));

    // Axis lines
    std::vector<glm::vec3> axisLines;
    axisLines.push_back(glm::vec3(-gridSize, 0, 0));
    axisLines.push_back(glm::vec3(gridSize, 0, 0));
    m_ctx.modelRenderer.renderLines(cmd, viewProj, axisLines, glm::vec3(0.8f, 0.3f, 0.3f));

    axisLines.clear();
    axisLines.push_back(glm::vec3(0, 0, -gridSize));
    axisLines.push_back(glm::vec3(0, 0, gridSize));
    m_ctx.modelRenderer.renderLines(cmd, viewProj, axisLines, glm::vec3(0.3f, 0.3f, 0.8f));

    axisLines.clear();
    axisLines.push_back(glm::vec3(0, 0, 0));
    axisLines.push_back(glm::vec3(0, 0.5f, 0));
    m_ctx.modelRenderer.renderLines(cmd, viewProj, axisLines, glm::vec3(0.3f, 0.8f, 0.3f));
}

void ModelingMode::buildEditableMeshFromObject() {
    std::cout << "buildEditableMeshFromObject called" << std::endl;
    g_wireframeDebugPrinted = false;  // Reset debug flag so we print again
    invalidateWireframeCache();
    if (!m_ctx.selectedObject || !m_ctx.selectedObject->hasMeshData()) return;

    // Check if we have stored EditableMesh data (preserves quad topology)
    if (m_ctx.selectedObject->hasEditableMeshData()) {
        std::cout << "  -> Path A: restoring from stored HE data" << std::endl;
        const auto& storedVerts = m_ctx.selectedObject->getHEVertices();
        const auto& storedHE = m_ctx.selectedObject->getHEHalfEdges();
        const auto& storedFaces = m_ctx.selectedObject->getHEFaces();

        // Debug: print UV range from stored HE data
        glm::vec2 uvMin(FLT_MAX), uvMax(-FLT_MAX);
        for (const auto& v : storedVerts) {
            uvMin = glm::min(uvMin, v.uv);
            uvMax = glm::max(uvMax, v.uv);
        }
        std::cout << "  -> Stored HE UV range: [" << uvMin.x << "," << uvMin.y << "] - [" << uvMax.x << "," << uvMax.y << "]" << std::endl;

        // Convert from SceneObject storage format to EditableMesh format
        std::vector<HEVertex> heVerts;
        heVerts.reserve(storedVerts.size());
        for (const auto& v : storedVerts) {
            HEVertex hv;
            hv.position = v.position;
            hv.normal = v.normal;
            hv.uv = v.uv;
            hv.color = v.color;
            hv.halfEdgeIndex = v.halfEdgeIndex;
            hv.selected = v.selected;
            hv.boneIndices = v.boneIndices;
            hv.boneWeights = v.boneWeights;
            heVerts.push_back(hv);
        }

        std::vector<HalfEdge> heHalfEdges;
        heHalfEdges.reserve(storedHE.size());
        for (const auto& he : storedHE) {
            heHalfEdges.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<HEFace> heFaces;
        heFaces.reserve(storedFaces.size());
        for (const auto& f : storedFaces) {
            heFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        m_ctx.editableMesh.setFromData(heVerts, heHalfEdges, heFaces);

        // Restore control points from SceneObject
        m_ctx.editableMesh.clearControlPoints();
        for (const auto& cp : m_ctx.selectedObject->getControlPoints()) {
            m_ctx.editableMesh.addControlPoint(cp.vertexIndex, cp.name);
        }

        // Restore ports from SceneObject
        m_ctx.editableMesh.clearPorts();
        for (const auto& p : m_ctx.selectedObject->getPorts()) {
            m_ctx.editableMesh.addPort({p.name, p.position, p.forward, p.up});
        }

        // Restore skeleton from SceneObject if present
        if (m_ctx.selectedObject->hasEditorSkeleton()) {
            m_ctx.editableMesh.setSkeleton(*m_ctx.selectedObject->getEditorSkeleton());
        }

        // Build faceToTriangles mapping - only for visible faces
        m_ctx.faceToTriangles.clear();
        uint32_t triIndex = 0;
        for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
            // Skip hidden faces - they're not in the rendered mesh
            if (m_ctx.hiddenFaces.find(faceIdx) != m_ctx.hiddenFaces.end()) {
                continue;
            }
            uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
            uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
            for (uint32_t i = 0; i < triCount; ++i) {
                m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
            }
        }

        m_ctx.meshDirty = false;
        return;
    }

    // Fall back to building from triangles and attempting to merge back to quads
    std::cout << "  -> Path B: rebuilding from triangles (no stored HE data)" << std::endl;
    const auto& vertices = m_ctx.selectedObject->getVertices();
    const auto& indices = m_ctx.selectedObject->getIndices();

    m_ctx.editableMesh.buildFromTriangles(vertices, indices);
    m_ctx.editableMesh.mergeTrianglesToQuads();  // Uses 0.85 threshold for faceted geometry

    // IMPORTANT: must rebuild the render mesh after mergeTrianglesToQuads().
    // mergeTrianglesToQuads reorders the HE face array (merged quads first, remaining
    // tris after), so the GPU render buffer's triangle order no longer matches.
    // Without this, faceToTriangles maps HE faces to wrong render triangles, causing
    // face selection to highlight random faces on the model.
    // NOTE: this only manifests on models where mergeTrianglesToQuads actually merges
    // pairs (smooth surfaces). Faceted models where no pairs pass the 0.85 normal
    // threshold return early from merge with face order unchanged, so they worked fine.
    updateMeshFromEditable();
}

void ModelingMode::duplicateSelectedObject() {
    if (!m_ctx.selectedObject) return;

    // Ensure mesh is synced before duplicating
    if (m_ctx.meshDirty) {
        updateMeshFromEditable();
    }

    auto& srcObj = m_ctx.selectedObject;
    auto newObj = std::make_unique<SceneObject>(srcObj->getName() + "_copy");

    // Copy mesh data with optional random color
    auto srcVerts = srcObj->getVertices();  // Copy so we can modify colors
    const auto& srcIndices = srcObj->getIndices();

    if (!srcVerts.empty() && !srcIndices.empty()) {
        // Apply random color if enabled
        if (m_ctx.randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            glm::vec4 randomColor(dist(m_ctx.rng), dist(m_ctx.rng), dist(m_ctx.rng), 1.0f);
            for (auto& v : srcVerts) {
                v.color = randomColor;
            }
        }

        // Create GPU model with texture if present
        uint32_t handle;
        if (srcObj->hasTextureData()) {
            const auto& texData = srcObj->getTextureData();
            int texW = srcObj->getTextureWidth();
            int texH = srcObj->getTextureHeight();
            handle = m_ctx.modelRenderer.createModel(srcVerts, srcIndices, texData.data(), texW, texH);
            newObj->setTextureData(texData, texW, texH);
        } else {
            handle = m_ctx.modelRenderer.createModel(srcVerts, srcIndices, nullptr, 0, 0);
        }
        newObj->setBufferHandle(handle);
        newObj->setIndexCount(static_cast<uint32_t>(srcIndices.size()));
        newObj->setVertexCount(static_cast<uint32_t>(srcVerts.size()));
        newObj->setMeshData(srcVerts, srcIndices);

        // Copy EditableMesh half-edge data (preserves quad topology)
        if (srcObj->hasEditableMeshData()) {
            // Also update HE vertex colors if random color applied
            if (m_ctx.randomMeshColors) {
                auto heVerts = srcObj->getHEVertices();
                std::uniform_real_distribution<float> dist(0.0f, 1.0f);
                glm::vec4 randomColor(dist(m_ctx.rng), dist(m_ctx.rng), dist(m_ctx.rng), 1.0f);
                for (auto& v : heVerts) {
                    v.color = randomColor;
                }
                newObj->setEditableMeshData(heVerts, srcObj->getHEHalfEdges(), srcObj->getHEFaces());
            } else {
                newObj->setEditableMeshData(
                    srcObj->getHEVertices(),
                    srcObj->getHEHalfEdges(),
                    srcObj->getHEFaces()
                );
            }
        }

        // Copy transform - same position as original
        auto& srcTransform = srcObj->getTransform();
        auto& transform = newObj->getTransform();
        transform.setScale(srcTransform.getScale());
        transform.setRotation(srcTransform.getRotation());
        transform.setPosition(srcTransform.getPosition());
    }

    // Add to scene and select the duplicate with move gizmo
    m_ctx.sceneObjects.push_back(std::move(newObj));
    m_ctx.selectedObject = m_ctx.sceneObjects.back().get();
    m_ctx.objectMode = true;
    m_ctx.gizmoMode = GizmoMode::Move;
    m_ctx.meshDirty = false;

    std::cout << "Duplicated object: " << m_ctx.selectedObject->getName() << std::endl;
}

void ModelingMode::connectByControlPoints() {
    if (m_ctx.selectedObjects.size() != 2) return;

    // Sync current edits
    if (m_ctx.meshDirty && m_ctx.selectedObject) {
        updateMeshFromEditable();
    }

    // obj1 = stationary (first selected), obj2 = moves to obj1 (last selected / active)
    // m_ctx.selectedObject is the last-clicked (active) object, so it becomes obj2.
    SceneObject* obj2 = m_ctx.selectedObject;
    SceneObject* obj1 = nullptr;
    for (auto* obj : m_ctx.selectedObjects) {
        if (obj != obj2) { obj1 = obj; break; }
    }
    if (!obj1 || !obj2) return;

    // ── Save backup for undo ──
    m_connectCPsBackup = ConnectCPsBackup{};
    auto saveObjBackup = [](SceneObject* obj) -> ConnectCPsBackup::ObjectBackup {
        ConnectCPsBackup::ObjectBackup b;
        b.name = obj->getName();
        b.transform = obj->getTransform();
        b.eulerRotation = obj->getEulerRotation();
        b.heVerts = obj->getHEVertices();
        b.heHalfEdges = obj->getHEHalfEdges();
        b.heFaces = obj->getHEFaces();
        b.controlPoints = obj->getControlPoints();
        b.meshVerts = obj->getVertices();
        b.meshIndices = obj->getIndices();
        if (obj->hasTextureData()) {
            b.textureData = obj->getTextureData();
            b.texWidth = obj->getTextureWidth();
            b.texHeight = obj->getTextureHeight();
        }
        return b;
    };
    m_connectCPsBackup.originals.push_back(saveObjBackup(obj1));
    m_connectCPsBackup.originals.push_back(saveObjBackup(obj2));

    if (!obj1->hasEditableMeshData() || !obj2->hasEditableMeshData()) {
        std::cout << "[Connect CPs] Both objects must have editable mesh data" << std::endl;
        return;
    }
    if (!obj1->hasControlPoints() || !obj2->hasControlPoints()) {
        std::cout << "[Connect CPs] Both objects must have control points" << std::endl;
        return;
    }

    std::cout << "[Connect CPs] Obj1='" << obj1->getName() << "' (" << obj1->getHEVertices().size()
              << " verts, " << obj1->getHEFaces().size() << " faces, " << obj1->getControlPoints().size() << " CPs)"
              << std::endl;
    std::cout << "[Connect CPs] Obj2='" << obj2->getName() << "' (" << obj2->getHEVertices().size()
              << " verts, " << obj2->getHEFaces().size() << " faces, " << obj2->getControlPoints().size() << " CPs)"
              << std::endl;

    // ── 1. Find matching CPs ──
    auto& cps1 = obj1->getControlPoints();
    auto& cps2 = obj2->getControlPoints();
    struct CPMatch { std::string name; uint32_t vi1; uint32_t vi2; };
    std::vector<CPMatch> matches;
    for (auto& cp1 : cps1)
        for (auto& cp2 : cps2)
            if (cp1.name == cp2.name)
                matches.push_back({cp1.name, cp1.vertexIndex, cp2.vertexIndex});

    if (matches.size() < 2) {
        std::cout << "[Connect CPs] Need 2+ matching CP names, found " << matches.size() << std::endl;
        return;
    }
    for (auto& m : matches)
        std::cout << "[Connect CPs]   Match: '" << m.name << "' vi1=" << m.vi1 << " vi2=" << m.vi2 << std::endl;

    // ── 2. Load HE data into EditableMesh for each object (preserves original twin info) ──
    auto loadMesh = [](SceneObject* obj, eden::EditableMesh& mesh) {
        auto& sv = obj->getHEVertices();
        auto& sh = obj->getHEHalfEdges();
        auto& sf = obj->getHEFaces();
        std::vector<eden::HEVertex> verts(sv.size());
        std::vector<eden::HalfEdge> hes(sh.size());
        std::vector<eden::HEFace> faces(sf.size());
        for (size_t i = 0; i < sv.size(); i++) {
            verts[i] = {sv[i].position, sv[i].normal, sv[i].uv, sv[i].color,
                        sv[i].halfEdgeIndex, sv[i].selected, sv[i].boneIndices, sv[i].boneWeights};
        }
        for (size_t i = 0; i < sh.size(); i++) {
            hes[i] = {sh[i].vertexIndex, sh[i].faceIndex, sh[i].nextIndex, sh[i].prevIndex, sh[i].twinIndex};
        }
        for (size_t i = 0; i < sf.size(); i++) {
            faces[i] = {sf[i].halfEdgeIndex, sf[i].vertexCount, sf[i].selected};
        }
        mesh.setFromData(verts, hes, faces);
        mesh.clearControlPoints();
        for (auto& cp : obj->getControlPoints())
            mesh.addControlPoint(cp.vertexIndex, cp.name);
    };

    eden::EditableMesh mesh1, mesh2;
    loadMesh(obj1, mesh1);
    loadMesh(obj2, mesh2);

    // ── 3. Build boundary adjacency maps ──
    // boundaryNext[fromVert] = toVert  (for half-edges with no twin = boundary)
    auto buildBoundaryMap = [](const eden::EditableMesh& mesh) {
        std::unordered_map<uint32_t, uint32_t> boundaryNext;
        for (uint32_t i = 0; i < mesh.getHalfEdgeCount(); i++) {
            auto& he = mesh.getHalfEdge(i);
            if (he.twinIndex == UINT32_MAX) {
                // he goes from prev->vertexIndex TO he.vertexIndex
                uint32_t from = mesh.getHalfEdge(he.prevIndex).vertexIndex;
                uint32_t to = he.vertexIndex;
                boundaryNext[from] = to;
            }
        }
        return boundaryNext;
    };

    auto bndNext1 = buildBoundaryMap(mesh1);
    auto bndNext2 = buildBoundaryMap(mesh2);

    std::cout << "[Connect CPs] Boundary edges: obj1=" << bndNext1.size() << " obj2=" << bndNext2.size() << std::endl;

    if (bndNext1.empty() || bndNext2.empty()) {
        std::cout << "[Connect CPs] Objects must have boundary edges (open meshes)" << std::endl;
        return;
    }

    // Check that our first CP is actually on a boundary
    if (bndNext1.find(matches[0].vi1) == bndNext1.end()) {
        std::cout << "[Connect CPs] CP '" << matches[0].name << "' vertex " << matches[0].vi1
                  << " is NOT on a boundary of obj1!" << std::endl;
        return;
    }
    if (bndNext2.find(matches[0].vi2) == bndNext2.end()) {
        std::cout << "[Connect CPs] CP '" << matches[0].name << "' vertex " << matches[0].vi2
                  << " is NOT on a boundary of obj2!" << std::endl;
        return;
    }

    // ── 4. Walk boundary ring starting from first matched CP ──
    auto walkRing = [](const std::unordered_map<uint32_t, uint32_t>& bndNext, uint32_t startVert) {
        std::vector<uint32_t> ring;
        uint32_t cur = startVert;
        do {
            ring.push_back(cur);
            auto it = bndNext.find(cur);
            if (it == bndNext.end()) break;
            cur = it->second;
        } while (cur != startVert && ring.size() < 10000);
        return ring;
    };

    auto ring1 = walkRing(bndNext1, matches[0].vi1);
    auto ring2 = walkRing(bndNext2, matches[0].vi2);

    if (ring1.empty() || ring2.empty()) {
        std::cout << "[Connect CPs] Failed to walk boundary rings" << std::endl;
        return;
    }

    std::cout << "[Connect CPs] Ring1: " << ring1.size() << " verts, Ring2: " << ring2.size() << " verts" << std::endl;

    // Debug: print first few ring vertex positions
    glm::mat4 mat1 = obj1->getTransform().getMatrix();
    glm::mat4 mat2 = obj2->getTransform().getMatrix();
    for (size_t i = 0; i < std::min(ring1.size(), (size_t)5); i++) {
        auto p = glm::vec3(mat1 * glm::vec4(mesh1.getVertex(ring1[i]).position, 1.0f));
        std::cout << "[Connect CPs]   ring1[" << i << "] v" << ring1[i]
                  << " pos=(" << p.x << ", " << p.y << ", " << p.z << ")" << std::endl;
    }
    for (size_t i = 0; i < std::min(ring2.size(), (size_t)5); i++) {
        auto p = glm::vec3(mat2 * glm::vec4(mesh2.getVertex(ring2[i]).position, 1.0f));
        std::cout << "[Connect CPs]   ring2[" << i << "] v" << ring2[i]
                  << " pos=(" << p.x << ", " << p.y << ", " << p.z << ")" << std::endl;
    }

    if (ring1.size() != ring2.size()) {
        std::cout << "[Connect CPs] Boundary rings have different vertex counts ("
                  << ring1.size() << " vs " << ring2.size() << ") — cannot connect" << std::endl;
        return;
    }

    // ── 5. Compute alignment rotation from ring frames (uses CPs, always correct) ──

    // Get world positions for both rings
    std::vector<glm::vec3> ring1World(ring1.size()), ring2World(ring2.size());
    for (size_t i = 0; i < ring1.size(); i++)
        ring1World[i] = glm::vec3(mat1 * glm::vec4(mesh1.getVertex(ring1[i]).position, 1.0f));
    for (size_t i = 0; i < ring2.size(); i++)
        ring2World[i] = glm::vec3(mat2 * glm::vec4(mesh2.getVertex(ring2[i]).position, 1.0f));

    // Centroids
    glm::vec3 centroid1(0), centroid2(0);
    for (size_t i = 0; i < ring1.size(); i++) centroid1 += ring1World[i];
    for (size_t i = 0; i < ring2.size(); i++) centroid2 += ring2World[i];
    centroid1 /= (float)ring1.size();
    centroid2 /= (float)ring2.size();

    // Compute ring normals from cross products of consecutive edges
    auto ringNormal = [](const std::vector<glm::vec3>& worldPositions, const glm::vec3& center) {
        glm::vec3 n(0);
        for (size_t i = 0; i < worldPositions.size(); i++) {
            glm::vec3 a = worldPositions[i] - center;
            glm::vec3 b = worldPositions[(i + 1) % worldPositions.size()] - center;
            n += glm::cross(a, b);
        }
        float len = glm::length(n);
        return len > 0.0001f ? n / len : glm::vec3(0, 1, 0);
    };

    glm::vec3 normal1 = ringNormal(ring1World, centroid1);
    glm::vec3 normal2 = ringNormal(ring2World, centroid2);

    // Build orthonormal frames using CP tangent vectors (CP0 → CP_halfway)
    // These are always correct regardless of winding.
    glm::vec3 tangent1 = glm::normalize(ring1World[ring1.size() / 2] - ring1World[0]);
    glm::vec3 tangent2 = glm::normalize(ring2World[ring2.size() / 2] - ring2World[0]);

    // Orthogonalize tangents against their normals (Gram-Schmidt)
    tangent1 = glm::normalize(tangent1 - glm::dot(tangent1, normal1) * normal1);
    tangent2 = glm::normalize(tangent2 - glm::dot(tangent2, normal2) * normal2);

    glm::vec3 bitangent1 = glm::cross(normal1, tangent1);
    glm::vec3 bitangent2 = glm::cross(normal2, tangent2);

    // Source frame: ring2's actual orientation
    glm::mat3 srcFrame(tangent2, normal2, bitangent2);
    // Target frame: ring2's normal should face opposite to ring1's normal (they face each other)
    glm::mat3 dstFrame(tangent1, -normal1, -bitangent1);
    glm::mat3 alignRotation = dstFrame * glm::transpose(srcFrame);

    // Full alignment transform: rotate obj2 around its ring centroid, then translate to centroid1
    glm::mat4 alignTransform = glm::translate(glm::mat4(1.0f), centroid1)
                              * glm::mat4(alignRotation)
                              * glm::translate(glm::mat4(1.0f), -centroid2);

    // Apply alignment to ring2 world positions
    std::vector<glm::vec3> ring2Aligned(ring2.size());
    for (size_t i = 0; i < ring2.size(); i++)
        ring2Aligned[i] = glm::vec3(alignTransform * glm::vec4(ring2World[i], 1.0f));

    std::cout << "[Connect CPs] Ring centroid1=(" << centroid1.x << ", " << centroid1.y << ", " << centroid1.z << ")" << std::endl;
    std::cout << "[Connect CPs] Ring centroid2=(" << centroid2.x << ", " << centroid2.y << ", " << centroid2.z << ")" << std::endl;

    // ── 6. Verify winding using ALIGNED positions ──
    // Compare ring1[1] against ring2Aligned[1] (forward) vs ring2Aligned[N-1] (reversed).
    {
        float distFwd = glm::length(ring1World[1] - ring2Aligned[1]);
        float distRev = glm::length(ring1World[1] - ring2Aligned[ring2.size() - 1]);

        std::cout << "[Connect CPs] Winding test (post-align): distFwd=" << distFwd << " distRev=" << distRev << std::endl;

        if (distRev < distFwd) {
            std::reverse(ring2.begin() + 1, ring2.end());
            // Also reverse ring2Aligned to match
            std::reverse(ring2Aligned.begin() + 1, ring2Aligned.end());
            std::cout << "[Connect CPs] Reversed ring2 winding (rev is closer)" << std::endl;
        } else {
            std::cout << "[Connect CPs] Winding OK (forward)" << std::endl;
        }
    }

    // Compute merged positions: average of obj1 ring pos and aligned obj2 ring pos
    std::vector<glm::vec3> mergedWorldPositions(ring1.size());
    for (size_t i = 0; i < ring1.size(); i++) {
        mergedWorldPositions[i] = (ring1World[i] + ring2Aligned[i]) * 0.5f;
    }

    // ── 7. Identify cap faces (faces with ALL vertices on the boundary ring) ──
    auto findCapFaces = [](const eden::EditableMesh& mesh, const std::vector<uint32_t>& ring) {
        std::set<uint32_t> ringSet(ring.begin(), ring.end());
        std::vector<uint32_t> capFaces;
        for (uint32_t fi = 0; fi < mesh.getFaceCount(); fi++) {
            auto fverts = mesh.getFaceVertices(fi);
            bool allOnRing = true;
            for (uint32_t v : fverts) {
                if (ringSet.find(v) == ringSet.end()) {
                    allOnRing = false;
                    break;
                }
            }
            if (allOnRing) capFaces.push_back(fi);
        }
        return capFaces;
    };

    auto capFaces1 = findCapFaces(mesh1, ring1);
    auto capFaces2 = findCapFaces(mesh2, ring2);
    std::cout << "[Connect CPs] Cap faces: obj1=" << capFaces1.size() << " obj2=" << capFaces2.size() << std::endl;

    // ── 8. Build combined mesh WITHOUT using deleteFaces ──
    // (deleteFaces rebuilds topology and renumbers everything, invalidating ring indices)
    // Instead, we skip cap faces during combination and let removeOrphanedVertices clean up.

    glm::mat4 invMat1 = glm::inverse(mat1);
    // Obj2 transform: apply obj2's model matrix, then alignment (rotation + translation), then into obj1's local space
    glm::mat4 obj2ToObj1Local = invMat1 * alignTransform * mat2;
    glm::mat3 normalMat2 = glm::transpose(glm::inverse(glm::mat3(obj2ToObj1Local)));

    // Work directly from the stored HE data (not from EditableMesh which may have been modified)
    auto& storedVerts1 = obj1->getHEVertices();
    auto& storedHEs1 = obj1->getHEHalfEdges();
    auto& storedFaces1 = obj1->getHEFaces();

    auto& storedVerts2 = obj2->getHEVertices();
    auto& storedHEs2 = obj2->getHEHalfEdges();
    auto& storedFaces2 = obj2->getHEFaces();

    // Build cap face skip sets
    std::set<uint32_t> skipFaces1(capFaces1.begin(), capFaces1.end());
    std::set<uint32_t> skipFaces2(capFaces2.begin(), capFaces2.end());

    // Build face remap (old face index → new face index, UINT32_MAX if skipped)
    std::vector<uint32_t> faceRemap1(storedFaces1.size(), UINT32_MAX);
    std::vector<uint32_t> faceRemap2(storedFaces2.size(), UINT32_MAX);

    // Collect kept faces from obj1
    struct FaceData {
        std::vector<uint32_t> vertIndices;  // vertex indices for this face
    };
    std::vector<FaceData> keptFaces;

    // For obj1: walk each non-cap face and collect vertex indices
    for (uint32_t fi = 0; fi < storedFaces1.size(); fi++) {
        if (skipFaces1.count(fi)) continue;
        auto& f = storedFaces1[fi];
        FaceData fd;
        uint32_t heIdx = f.halfEdgeIndex;
        for (uint32_t k = 0; k < f.vertexCount; k++) {
            auto& he = storedHEs1[heIdx];
            fd.vertIndices.push_back(he.vertexIndex);
            heIdx = he.nextIndex;
        }
        faceRemap1[fi] = (uint32_t)keptFaces.size();
        keptFaces.push_back(fd);
    }
    uint32_t faceCountFromObj1 = (uint32_t)keptFaces.size();

    // Build vertex remap for obj2:
    // ring2 vertices → corresponding ring1 vertex index
    // other vertices → offset past obj1's vertex count
    std::unordered_map<uint32_t, uint32_t> ring2ToRing1;
    for (size_t i = 0; i < ring1.size(); i++) {
        ring2ToRing1[ring2[i]] = ring1[i];
    }

    uint32_t vertOffset = (uint32_t)storedVerts1.size();
    std::vector<uint32_t> vert2Remap(storedVerts2.size());
    for (uint32_t i = 0; i < storedVerts2.size(); i++) {
        auto it = ring2ToRing1.find(i);
        if (it != ring2ToRing1.end()) {
            vert2Remap[i] = it->second;  // Maps to ring1 vertex
        } else {
            vert2Remap[i] = vertOffset + i;  // Append after obj1 verts
        }
    }

    // For obj2: walk each non-cap face and collect remapped vertex indices
    for (uint32_t fi = 0; fi < storedFaces2.size(); fi++) {
        if (skipFaces2.count(fi)) continue;
        auto& f = storedFaces2[fi];
        FaceData fd;
        uint32_t heIdx = f.halfEdgeIndex;
        for (uint32_t k = 0; k < f.vertexCount; k++) {
            auto& he = storedHEs2[heIdx];
            fd.vertIndices.push_back(vert2Remap[he.vertexIndex]);
            heIdx = he.nextIndex;
        }
        faceRemap2[fi] = (uint32_t)keptFaces.size();
        keptFaces.push_back(fd);
    }

    // Build combined vertex array
    uint32_t totalVerts = vertOffset + (uint32_t)storedVerts2.size();
    std::vector<eden::HEVertex> combinedVerts(totalVerts);

    // Copy obj1 verts (in obj1 local space — unchanged)
    for (uint32_t i = 0; i < storedVerts1.size(); i++) {
        auto& sv = storedVerts1[i];
        combinedVerts[i] = {sv.position, sv.normal, sv.uv, sv.color,
                            UINT32_MAX, false, sv.boneIndices, sv.boneWeights};
    }

    // Set merged positions on ring1 vertices
    for (size_t i = 0; i < ring1.size(); i++) {
        glm::vec3 localPos = glm::vec3(invMat1 * glm::vec4(mergedWorldPositions[i], 1.0f));
        combinedVerts[ring1[i]].position = localPos;
    }

    // Copy obj2 verts (transformed into obj1 local space)
    for (uint32_t i = 0; i < storedVerts2.size(); i++) {
        if (ring2ToRing1.count(i)) continue;  // Ring verts already handled via ring1
        uint32_t newIdx = vert2Remap[i];
        auto& sv = storedVerts2[i];
        glm::vec3 pos = glm::vec3(obj2ToObj1Local * glm::vec4(sv.position, 1.0f));
        glm::vec3 nrm = glm::normalize(normalMat2 * sv.normal);
        combinedVerts[newIdx] = {pos, nrm, sv.uv, sv.color,
                                  UINT32_MAX, false, sv.boneIndices, sv.boneWeights};
    }

    // ── 9. Build new half-edge structure from face vertex lists ──
    // This is cleaner than trying to remap old half-edges with offset tricks.
    std::vector<eden::HalfEdge> combinedHEs;
    std::vector<eden::HEFace> combinedFaces;

    for (uint32_t fi = 0; fi < keptFaces.size(); fi++) {
        auto& fd = keptFaces[fi];
        uint32_t n = (uint32_t)fd.vertIndices.size();
        uint32_t heStart = (uint32_t)combinedHEs.size();

        eden::HEFace face;
        face.halfEdgeIndex = heStart;
        face.vertexCount = n;
        face.selected = false;

        for (uint32_t k = 0; k < n; k++) {
            eden::HalfEdge he;
            he.vertexIndex = fd.vertIndices[(k + 1) % n];  // Points TO next vertex
            he.faceIndex = fi;
            he.nextIndex = heStart + (k + 1) % n;
            he.prevIndex = heStart + (k + n - 1) % n;
            he.twinIndex = UINT32_MAX;  // Will be linked later
            combinedHEs.push_back(he);

            // Update vertex → half-edge reference
            uint32_t vi = fd.vertIndices[k];
            if (vi < combinedVerts.size() && combinedVerts[vi].halfEdgeIndex == UINT32_MAX) {
                combinedVerts[vi].halfEdgeIndex = heStart + k;
            }
        }

        combinedFaces.push_back(face);
    }

    std::cout << "[Connect CPs] Combined: " << combinedVerts.size() << " verts, "
              << combinedFaces.size() << " faces, " << combinedHEs.size() << " half-edges" << std::endl;

    // ── 10. Create EditableMesh, link twins, clean up ──
    eden::EditableMesh combinedMesh;
    combinedMesh.setFromData(combinedVerts, combinedHEs, combinedFaces);
    combinedMesh.linkTwinsByPosition();
    combinedMesh.removeOrphanedVertices();
    combinedMesh.recalculateNormals();

    // ── 11. Triangulate and create new SceneObject ──
    std::vector<ModelVertex> outVerts;
    std::vector<uint32_t> outIndices;
    combinedMesh.triangulate(outVerts, outIndices);

    if (outVerts.empty()) {
        std::cout << "[Connect CPs] Combined mesh has no vertices — aborting" << std::endl;
        return;
    }

    std::string newName = obj1->getName() + "+" + obj2->getName();
    auto combinedObj = std::make_unique<SceneObject>(newName);
    combinedObj->getTransform() = obj1->getTransform();
    combinedObj->setEulerRotation(obj1->getEulerRotation());

    uint32_t bufHandle = m_ctx.modelRenderer.createModel(outVerts, outIndices, nullptr, 0, 0);
    combinedObj->setBufferHandle(bufHandle);
    combinedObj->setIndexCount((uint32_t)outIndices.size());
    combinedObj->setVertexCount((uint32_t)outVerts.size());
    combinedObj->setMeshData(outVerts, outIndices);

    // Store combined HE data
    {
        auto& cv = combinedMesh.getVerticesData();
        auto& ch = combinedMesh.getHalfEdges();
        auto& cf = combinedMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> sv(cv.size());
        std::vector<SceneObject::StoredHalfEdge> sh(ch.size());
        std::vector<SceneObject::StoredHEFace> sf(cf.size());
        for (size_t i = 0; i < cv.size(); i++) {
            sv[i] = {cv[i].position, cv[i].normal, cv[i].uv, cv[i].color,
                      cv[i].halfEdgeIndex, cv[i].selected, cv[i].boneIndices, cv[i].boneWeights};
        }
        for (size_t i = 0; i < ch.size(); i++) {
            sh[i] = {ch[i].vertexIndex, ch[i].faceIndex, ch[i].nextIndex, ch[i].prevIndex, ch[i].twinIndex};
        }
        for (size_t i = 0; i < cf.size(); i++) {
            sf[i] = {cf[i].halfEdgeIndex, cf[i].vertexCount, cf[i].selected};
        }
        combinedObj->setEditableMeshData(sv, sh, sf);
    }

    // Compute bounds
    AABB bounds;
    bounds.min = glm::vec3(FLT_MAX);
    bounds.max = glm::vec3(-FLT_MAX);
    for (auto& v : outVerts) {
        bounds.min = glm::min(bounds.min, v.position);
        bounds.max = glm::max(bounds.max, v.position);
    }
    combinedObj->setLocalBounds(bounds);

    // Queue originals for deletion
    for (SceneObject* obj : m_ctx.selectedObjects) {
        m_ctx.pendingDeletions.push_back(obj);
    }

    // Clear selection and mesh state
    m_ctx.selectedObject = nullptr;
    m_ctx.selectedObjects.clear();
    m_ctx.editableMesh.clear();
    m_ctx.meshDirty = false;

    m_ctx.sceneObjects.push_back(std::move(combinedObj));

    // Mark backup as valid for undo
    m_connectCPsBackup.combinedName = newName;
    m_connectCPsBackup.valid = true;

    std::cout << "[Connect CPs] Created '" << newName << "' with "
              << outVerts.size() << " vertices, " << outIndices.size() / 3 << " triangles"
              << " (Ctrl+Z to undo)" << std::endl;
}

void ModelingMode::undoConnectCPs() {
    if (!m_connectCPsBackup.valid) return;

    // Remove the combined object
    for (auto it = m_ctx.sceneObjects.begin(); it != m_ctx.sceneObjects.end(); ++it) {
        if ((*it)->getName() == m_connectCPsBackup.combinedName) {
            if ((*it)->getBufferHandle() != UINT32_MAX) {
                m_ctx.modelRenderer.destroyModel((*it)->getBufferHandle());
            }
            m_ctx.sceneObjects.erase(it);
            break;
        }
    }

    // Restore original objects
    for (auto& b : m_connectCPsBackup.originals) {
        auto obj = std::make_unique<SceneObject>(b.name);
        obj->getTransform() = b.transform;
        obj->setEulerRotation(b.eulerRotation);
        obj->setEditableMeshData(b.heVerts, b.heHalfEdges, b.heFaces);
        obj->setControlPoints(b.controlPoints);

        const unsigned char* texPtr = b.textureData.empty() ? nullptr : b.textureData.data();
        int texW = b.texWidth, texH = b.texHeight;
        uint32_t handle = m_ctx.modelRenderer.createModel(b.meshVerts, b.meshIndices, texPtr, texW, texH);
        obj->setBufferHandle(handle);
        obj->setIndexCount((uint32_t)b.meshIndices.size());
        obj->setVertexCount((uint32_t)b.meshVerts.size());
        obj->setMeshData(b.meshVerts, b.meshIndices);
        if (!b.textureData.empty()) {
            obj->setTextureData(b.textureData, texW, texH);
        }

        // Compute bounds
        AABB bounds;
        bounds.min = glm::vec3(FLT_MAX);
        bounds.max = glm::vec3(-FLT_MAX);
        for (auto& v : b.meshVerts) {
            bounds.min = glm::min(bounds.min, v.position);
            bounds.max = glm::max(bounds.max, v.position);
        }
        obj->setLocalBounds(bounds);

        m_ctx.sceneObjects.push_back(std::move(obj));
    }

    // Clear selection
    m_ctx.selectedObject = nullptr;
    m_ctx.selectedObjects.clear();
    m_ctx.editableMesh.clear();
    m_ctx.meshDirty = false;

    m_connectCPsBackup.valid = false;
    std::cout << "[Connect CPs] Undo — restored original objects" << std::endl;
}

void ModelingMode::updateMeshFromEditable() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;

    bool hasSkeleton = m_ctx.editableMesh.hasSkeleton();

    // For non-skinned rendering, we still need ModelVertex triangulation for raycasting/mesh data
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    m_ctx.editableMesh.triangulate(vertices, indices, m_ctx.hiddenFaces);

    // Destroy old handles
    uint32_t oldHandle = m_ctx.selectedObject->getBufferHandle();
    if (oldHandle != UINT32_MAX && oldHandle != 0) {
        m_ctx.modelRenderer.destroyModel(oldHandle);
    }
    // Also destroy old skinned handle if present
    if (m_ctx.selectedObject->hasEditorSkeleton()) {
        uint32_t oldSkinnedHandle = m_ctx.selectedObject->getSkinnedModelHandle();
        if (oldSkinnedHandle != UINT32_MAX) {
            m_ctx.skinnedModelRenderer.destroyModel(oldSkinnedHandle);
            m_ctx.selectedObject->setSkinnedModelHandle(UINT32_MAX);
        }
    }

    // Handle case where all faces are hidden (empty mesh)
    if (indices.empty()) {
        m_ctx.selectedObject->setBufferHandle(0);
        m_ctx.selectedObject->setIndexCount(0);
        m_ctx.selectedObject->setVertexCount(0);
        m_ctx.selectedObject->setMeshData({}, {});
        m_ctx.selectedObject->clearEditorSkeleton();
        m_ctx.meshDirty = false;
        return;
    }

    // Create GPU mesh
    uint32_t newHandle;
    if (m_ctx.selectedObject->hasTextureData()) {
        const auto& texData = m_ctx.selectedObject->getTextureData();
        int texW = m_ctx.selectedObject->getTextureWidth();
        int texH = m_ctx.selectedObject->getTextureHeight();
        newHandle = m_ctx.modelRenderer.createModel(vertices, indices, texData.data(), texW, texH);
    } else {
        newHandle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
    }
    m_ctx.selectedObject->setBufferHandle(newHandle);
    m_ctx.selectedObject->setIndexCount(static_cast<uint32_t>(indices.size()));
    m_ctx.selectedObject->setVertexCount(static_cast<uint32_t>(vertices.size()));
    m_ctx.selectedObject->setMeshData(vertices, indices);

    // If skeleton exists, also create skinned model and store skeleton on SceneObject
    if (hasSkeleton) {
        auto skelCopy = std::make_unique<Skeleton>(m_ctx.editableMesh.getSkeleton());
        m_ctx.selectedObject->setEditorSkeleton(std::move(skelCopy));
    } else {
        m_ctx.selectedObject->clearEditorSkeleton();
    }

    // Also save EditableMesh half-edge data (preserves quad topology for duplicate)
    const auto& heVerts = m_ctx.editableMesh.getVerticesData();
    const auto& heHalfEdges = m_ctx.editableMesh.getHalfEdges();
    const auto& heFaces = m_ctx.editableMesh.getFacesData();

    std::vector<SceneObject::StoredHEVertex> storedVerts;
    storedVerts.reserve(heVerts.size());
    for (const auto& v : heVerts) {
        storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected,
                               v.boneIndices, v.boneWeights});
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

    m_ctx.selectedObject->setEditableMeshData(storedVerts, storedHE, storedFaces);

    // Save control points to SceneObject
    {
        std::vector<SceneObject::StoredControlPoint> storedCPs;
        for (const auto& cp : m_ctx.editableMesh.getControlPoints()) {
            storedCPs.push_back({cp.vertexIndex, cp.name});
        }
        m_ctx.selectedObject->setControlPoints(storedCPs);
    }

    // Build faceToTriangles mapping - only for visible faces (matches triangulated mesh)
    m_ctx.faceToTriangles.clear();
    uint32_t triIndex = 0;
    for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
        // Skip hidden faces - they're not in the rendered mesh
        if (m_ctx.hiddenFaces.find(faceIdx) != m_ctx.hiddenFaces.end()) {
            continue;
        }
        uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
        uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
        for (uint32_t i = 0; i < triCount; ++i) {
            m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
        }
    }

    m_ctx.meshDirty = false;
}

void ModelingMode::bakeTextureToVertexColors() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->hasTextureData()) return;

    const auto& texData = m_ctx.selectedObject->getTextureData();
    int texW = m_ctx.selectedObject->getTextureWidth();
    int texH = m_ctx.selectedObject->getTextureHeight();
    if (texW <= 0 || texH <= 0 || texData.empty()) return;

    m_ctx.editableMesh.saveState();

    uint32_t vertCount = m_ctx.editableMesh.getVertexCount();
    int sampled = 0;

    for (uint32_t i = 0; i < vertCount; ++i) {
        auto& vert = m_ctx.editableMesh.getVertex(i);
        glm::vec2 uv = vert.uv;

        // Wrap UVs to [0,1] range
        uv.x = uv.x - std::floor(uv.x);
        uv.y = uv.y - std::floor(uv.y);

        // Bilinear sample the texture
        float fx = uv.x * (texW - 1);
        float fy = uv.y * (texH - 1);
        int x0 = std::max(0, std::min(static_cast<int>(fx), texW - 1));
        int y0 = std::max(0, std::min(static_cast<int>(fy), texH - 1));
        int x1 = std::min(x0 + 1, texW - 1);
        int y1 = std::min(y0 + 1, texH - 1);
        float sx = fx - x0;
        float sy = fy - y0;

        auto sample = [&](int x, int y) -> glm::vec4 {
            size_t idx = (static_cast<size_t>(y) * texW + x) * 4;
            if (idx + 3 >= texData.size()) return glm::vec4(1.0f);
            return glm::vec4(
                texData[idx] / 255.0f,
                texData[idx + 1] / 255.0f,
                texData[idx + 2] / 255.0f,
                texData[idx + 3] / 255.0f
            );
        };

        glm::vec4 c00 = sample(x0, y0);
        glm::vec4 c10 = sample(x1, y0);
        glm::vec4 c01 = sample(x0, y1);
        glm::vec4 c11 = sample(x1, y1);

        glm::vec4 color = glm::mix(glm::mix(c00, c10, sx), glm::mix(c01, c11, sx), sy);
        color.a = 1.0f;
        vert.color = color;
        sampled++;
    }

    // Defer texture removal to next frame (safe point for GPU sync)
    m_ctx.pendingTextureDelete = true;

    m_ctx.meshDirty = true;
    std::cout << "[ModelEditor] Baked texture to vertex colors (" << sampled
              << " vertices, " << texW << "x" << texH << " texture)" << std::endl;
}

void ModelingMode::storeUVsForReprojection() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->hasTextureData()) return;

    uint32_t vertCount = m_ctx.editableMesh.getVertexCount();
    m_storedOldUVs.resize(vertCount);
    for (uint32_t i = 0; i < vertCount; ++i) {
        m_storedOldUVs[i].uv = m_ctx.editableMesh.getVertex(i).uv;
    }

    const auto& texData = m_ctx.selectedObject->getTextureData();
    m_storedOldTexture = texData;
    m_storedOldTexW = m_ctx.selectedObject->getTextureWidth();
    m_storedOldTexH = m_ctx.selectedObject->getTextureHeight();
    m_hasStoredUVs = true;

    std::cout << "[ModelEditor] Stored " << vertCount << " vertex UVs and "
              << m_storedOldTexW << "x" << m_storedOldTexH << " texture for re-projection" << std::endl;
}

void ModelingMode::reprojectTexture(int outputSize) {
    if (!m_hasStoredUVs || m_storedOldTexture.empty()) return;
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;

    int outW = outputSize > 0 ? outputSize : m_storedOldTexW;
    int outH = outputSize > 0 ? outputSize : m_storedOldTexH;
    int srcW = m_storedOldTexW;
    int srcH = m_storedOldTexH;

    // Create output texture (RGBA, initialized to black transparent)
    // Initialize with black opaque (alpha=255) so paint shows in UV editor
    size_t outTexBytes = static_cast<size_t>(outW) * outH * 4;
    std::vector<unsigned char> outTex(outTexBytes, 0);
    for (size_t i = 3; i < outTexBytes; i += 4) {
        outTex[i] = 255;
    }

    uint32_t faceCount = m_ctx.editableMesh.getFaceCount();
    uint32_t vertCount = m_ctx.editableMesh.getVertexCount();

    // Verify stored UV count matches
    if (m_storedOldUVs.size() != vertCount) {
        std::cerr << "[ModelEditor] Re-project: vertex count changed ("
                  << m_storedOldUVs.size() << " stored vs " << vertCount << " current). Aborting." << std::endl;
        return;
    }

    // Helper: sample old texture with bilinear filtering
    auto sampleOld = [&](float u, float v) -> glm::vec4 {
        u = u - std::floor(u);
        v = v - std::floor(v);
        float fx = u * (srcW - 1);
        float fy = v * (srcH - 1);
        int x0 = std::max(0, std::min(static_cast<int>(fx), srcW - 1));
        int y0 = std::max(0, std::min(static_cast<int>(fy), srcH - 1));
        int x1 = std::min(x0 + 1, srcW - 1);
        int y1 = std::min(y0 + 1, srcH - 1);
        float sx = fx - x0;
        float sy = fy - y0;

        auto fetch = [&](int x, int y) -> glm::vec4 {
            size_t idx = (static_cast<size_t>(y) * srcW + x) * 4;
            if (idx + 3 >= m_storedOldTexture.size()) return glm::vec4(0.0f);
            return glm::vec4(
                m_storedOldTexture[idx] / 255.0f,
                m_storedOldTexture[idx + 1] / 255.0f,
                m_storedOldTexture[idx + 2] / 255.0f,
                m_storedOldTexture[idx + 3] / 255.0f
            );
        };

        glm::vec4 c00 = fetch(x0, y0);
        glm::vec4 c10 = fetch(x1, y0);
        glm::vec4 c01 = fetch(x0, y1);
        glm::vec4 c11 = fetch(x1, y1);
        return glm::mix(glm::mix(c00, c10, sx), glm::mix(c01, c11, sx), sy);
    };

    int pixelsFilled = 0;

    // Software rasterize each face: for each face, get its new UVs (current) and old UVs (stored)
    // Triangulate quads into triangles for rasterization
    for (uint32_t fi = 0; fi < faceCount; ++fi) {
        auto faceVerts = m_ctx.editableMesh.getFaceVertices(fi);
        if (faceVerts.size() < 3) continue;

        // Triangulate (fan from first vertex)
        for (size_t t = 1; t + 1 < faceVerts.size(); ++t) {
            uint32_t vi0 = faceVerts[0];
            uint32_t vi1 = faceVerts[t];
            uint32_t vi2 = faceVerts[t + 1];

            // New UVs (current mesh state) — where to write in output texture
            glm::vec2 newUV0 = m_ctx.editableMesh.getVertex(vi0).uv;
            glm::vec2 newUV1 = m_ctx.editableMesh.getVertex(vi1).uv;
            glm::vec2 newUV2 = m_ctx.editableMesh.getVertex(vi2).uv;

            // Old UVs (stored) — where to read from old texture
            glm::vec2 oldUV0 = m_storedOldUVs[vi0].uv;
            glm::vec2 oldUV1 = m_storedOldUVs[vi1].uv;
            glm::vec2 oldUV2 = m_storedOldUVs[vi2].uv;

            // Pixel bounding box of the triangle in output texture
            glm::vec2 uvMin = glm::min(glm::min(newUV0, newUV1), newUV2);
            glm::vec2 uvMax = glm::max(glm::max(newUV0, newUV1), newUV2);

            int minPX = std::max(0, static_cast<int>(std::floor(uvMin.x * outW)) - 1);
            int maxPX = std::min(outW - 1, static_cast<int>(std::ceil(uvMax.x * outW)) + 1);
            int minPY = std::max(0, static_cast<int>(std::floor(uvMin.y * outH)) - 1);
            int maxPY = std::min(outH - 1, static_cast<int>(std::ceil(uvMax.y * outH)) + 1);

            // Rasterize: for each pixel, check if inside triangle, compute barycentrics
            for (int py = minPY; py <= maxPY; ++py) {
                for (int px = minPX; px <= maxPX; ++px) {
                    glm::vec2 p((px + 0.5f) / outW, (py + 0.5f) / outH);

                    // Barycentric coordinates
                    glm::vec2 v0 = newUV2 - newUV0;
                    glm::vec2 v1 = newUV1 - newUV0;
                    glm::vec2 v2 = p - newUV0;

                    float dot00 = glm::dot(v0, v0);
                    float dot01 = glm::dot(v0, v1);
                    float dot02 = glm::dot(v0, v2);
                    float dot11 = glm::dot(v1, v1);
                    float dot12 = glm::dot(v1, v2);

                    float denom = dot00 * dot11 - dot01 * dot01;
                    if (std::abs(denom) < 1e-10f) continue;

                    float invDenom = 1.0f / denom;
                    float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
                    float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

                    // Check if inside triangle (with small margin for edge coverage)
                    if (u < -0.001f || v < -0.001f || u + v > 1.001f) continue;

                    // Interpolate old UVs using barycentrics
                    float w0 = 1.0f - u - v;
                    glm::vec2 oldUV = w0 * oldUV0 + v * oldUV1 + u * oldUV2;

                    // Sample old texture
                    glm::vec4 color = sampleOld(oldUV.x, oldUV.y);

                    // Write to output texture
                    size_t idx = (static_cast<size_t>(py) * outW + px) * 4;
                    outTex[idx]     = static_cast<unsigned char>(std::min(255.0f, color.r * 255.0f));
                    outTex[idx + 1] = static_cast<unsigned char>(std::min(255.0f, color.g * 255.0f));
                    outTex[idx + 2] = static_cast<unsigned char>(std::min(255.0f, color.b * 255.0f));
                    outTex[idx + 3] = 255;
                    pixelsFilled++;
                }
            }
        }
    }

    // Apply new texture to the scene object
    m_ctx.selectedObject->setTextureData(outTex, outW, outH);
    m_ctx.selectedObject->markTextureModified();
    m_ctx.meshDirty = true;

    std::cout << "[ModelEditor] Re-projected texture: " << outW << "x" << outH
              << ", " << pixelsFilled << " pixels filled from " << faceCount << " faces" << std::endl;
}

void ModelingMode::packAndReprojectUVs() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->hasTextureData()) return;

    const auto& srcTex = m_ctx.selectedObject->getTextureData();
    int srcW = m_ctx.selectedObject->getTextureWidth();
    int srcH = m_ctx.selectedObject->getTextureHeight();
    if (srcW <= 0 || srcH <= 0 || srcTex.empty()) return;

    // Store old UVs in case user wants manual re-project later
    storeUVsForReprojection();

    int outSize = m_reprojectTexSize;

    // Step 1: Create new texture, place downscaled original in chosen corner
    // Initialize with black opaque (alpha=255) so paint shows in UV editor
    size_t texBytes = static_cast<size_t>(outSize) * outSize * 4;
    std::vector<unsigned char> outTex(texBytes, 0);
    for (size_t i = 3; i < texBytes; i += 4) {
        outTex[i] = 255;  // Set alpha channel to opaque
    }

    // Corner pixel offsets
    int cornerPX = 0, cornerPY = 0;
    int regionSize = static_cast<int>(outSize * m_packScale);
    switch (m_packCorner) {
        case 0: cornerPX = 0; cornerPY = 0; break;                                // bottom-left
        case 1: cornerPX = outSize - regionSize; cornerPY = 0; break;              // bottom-right
        case 2: cornerPX = 0; cornerPY = outSize - regionSize; break;              // top-left
        case 3: cornerPX = outSize - regionSize; cornerPY = outSize - regionSize; break; // top-right
    }

    // Bilinear downscale original texture into the corner region
    for (int dy = 0; dy < regionSize; ++dy) {
        for (int dx = 0; dx < regionSize; ++dx) {
            // Map destination pixel to source texture coordinate
            float srcX = (static_cast<float>(dx) + 0.5f) / regionSize * srcW;
            float srcY = (static_cast<float>(dy) + 0.5f) / regionSize * srcH;

            // Bilinear sample
            int x0 = std::max(0, std::min(static_cast<int>(srcX), srcW - 1));
            int y0 = std::max(0, std::min(static_cast<int>(srcY), srcH - 1));
            int x1 = std::min(x0 + 1, srcW - 1);
            int y1 = std::min(y0 + 1, srcH - 1);
            float fx = srcX - x0;
            float fy = srcY - y0;

            auto sample = [&](int x, int y) -> glm::vec4 {
                size_t idx = (static_cast<size_t>(y) * srcW + x) * 4;
                if (idx + 3 >= srcTex.size()) return glm::vec4(0.0f);
                return glm::vec4(srcTex[idx], srcTex[idx+1], srcTex[idx+2], srcTex[idx+3]);
            };

            glm::vec4 c00 = sample(x0, y0);
            glm::vec4 c10 = sample(x1, y0);
            glm::vec4 c01 = sample(x0, y1);
            glm::vec4 c11 = sample(x1, y1);
            glm::vec4 c = glm::mix(glm::mix(c00, c10, fx), glm::mix(c01, c11, fx), fy);

            size_t outIdx = (static_cast<size_t>(cornerPY + dy) * outSize + (cornerPX + dx)) * 4;
            outTex[outIdx]     = static_cast<unsigned char>(std::min(255.0f, c.r));
            outTex[outIdx + 1] = static_cast<unsigned char>(std::min(255.0f, c.g));
            outTex[outIdx + 2] = static_cast<unsigned char>(std::min(255.0f, c.b));
            outTex[outIdx + 3] = static_cast<unsigned char>(std::min(255.0f, c.a));
        }
    }

    // Step 2: Scale and offset all UVs to point at the corner region
    uint32_t vertCount = m_ctx.editableMesh.getVertexCount();

    // Find actual UV bounds
    glm::vec2 uvMin(FLT_MAX), uvMax(-FLT_MAX);
    for (uint32_t i = 0; i < vertCount; ++i) {
        glm::vec2 uv = m_ctx.editableMesh.getVertex(i).uv;
        uv.x = uv.x - std::floor(uv.x);
        uv.y = uv.y - std::floor(uv.y);
        uvMin = glm::min(uvMin, uv);
        uvMax = glm::max(uvMax, uv);
    }

    glm::vec2 uvRange = uvMax - uvMin;
    if (uvRange.x < 0.001f || uvRange.y < 0.001f) {
        std::cerr << "[ModelEditor] UV range too small to pack" << std::endl;
        return;
    }

    // Corner UV offset (matches pixel placement)
    glm::vec2 cornerUV(static_cast<float>(cornerPX) / outSize,
                       static_cast<float>(cornerPY) / outSize);
    float uvScale = static_cast<float>(regionSize) / outSize;

    m_ctx.editableMesh.saveState();
    for (uint32_t i = 0; i < vertCount; ++i) {
        auto& vert = m_ctx.editableMesh.getVertex(i);
        glm::vec2 uv = vert.uv;
        uv.x = uv.x - std::floor(uv.x);
        uv.y = uv.y - std::floor(uv.y);

        // Normalize to [0,1] within actual UV bounds, then scale to corner
        uv = (uv - uvMin) / uvRange;
        uv = uv * uvScale + cornerUV;

        vert.uv = uv;
    }

    // Step 3: Apply new texture
    m_ctx.selectedObject->setTextureData(outTex, outSize, outSize);
    m_ctx.selectedObject->markTextureModified();
    m_ctx.meshDirty = true;

    std::cout << "[ModelEditor] Pack complete: texture " << srcW << "x" << srcH
              << " → " << regionSize << "x" << regionSize << " region in "
              << outSize << "x" << outSize << " texture (corner " << m_packCorner << ")" << std::endl;
}

void ModelingMode::saveEditableMeshAsGLB() {
    if (!m_ctx.selectedObject || !m_ctx.selectedObject->hasMeshData()) {
        std::cerr << "No mesh to save" << std::endl;
        return;
    }

    // Use the stored triangulated data from SceneObject directly
    // This is the same data used for GPU rendering, so it's known to be correct
    std::vector<ModelVertex> vertices = m_ctx.selectedObject->getVertices();
    std::vector<uint32_t> indices = m_ctx.selectedObject->getIndices();

    // Apply object transform to bake in scale/rotation/position
    if (m_ctx.selectedObject) {
        glm::mat4 transform = m_ctx.selectedObject->getTransform().getMatrix();
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

        for (auto& v : vertices) {
            // Transform position
            glm::vec4 worldPos = transform * glm::vec4(v.position, 1.0f);
            v.position = glm::vec3(worldPos);

            // Transform normal
            v.normal = glm::normalize(normalMatrix * v.normal);
        }
    }

    std::string defaultName = "model.glb";
    if (m_ctx.selectedObject) {
        defaultName = m_ctx.selectedObject->getName() + "_edited.glb";
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Model", "glb"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        if (filepath.size() < 4 || filepath.substr(filepath.size() - 4) != ".glb") {
            filepath += ".glb";
        }

        std::string meshName = std::filesystem::path(filepath).stem().string();

        bool saved = false;
        // Get texture data if available
        const unsigned char* texData = nullptr;
        int texW = 0, texH = 0;
        if (m_ctx.selectedObject->hasTextureData()) {
            texData = m_ctx.selectedObject->getTextureData().data();
            texW = m_ctx.selectedObject->getTextureWidth();
            texH = m_ctx.selectedObject->getTextureHeight();
        }

        // Save with half-edge data if available (preserves quad topology on reload)
        if (m_ctx.selectedObject->hasEditableMeshData()) {
            StoredHEData heData;
            heData.vertices = m_ctx.selectedObject->getHEVertices();
            heData.halfEdges = m_ctx.selectedObject->getHEHalfEdges();
            heData.faces = m_ctx.selectedObject->getHEFaces();
            saved = GLBLoader::saveWithHalfEdgeData(filepath, vertices, indices, heData, texData, texW, texH, meshName);
        } else if (texData) {
            saved = GLBLoader::save(filepath, vertices, indices, texData, texW, texH, meshName);
        } else {
            saved = GLBLoader::save(filepath, vertices, indices, meshName);
        }

        if (saved) {
            std::cout << "Saved mesh to: " << filepath << std::endl;
        } else {
            std::cerr << "Failed to save mesh to: " << filepath << std::endl;
        }
    }
}

void ModelingMode::saveEditableMeshAsOBJ() {
    if (!m_ctx.editableMesh.isValid()) {
        std::cerr << "No mesh to save" << std::endl;
        return;
    }

    std::string defaultName = "model.obj";
    if (m_ctx.selectedObject) {
        defaultName = m_ctx.selectedObject->getName() + ".obj";
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"OBJ Model", "obj"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        if (filepath.size() < 4 || filepath.substr(filepath.size() - 4) != ".obj") {
            filepath += ".obj";
        }

        if (m_ctx.editableMesh.saveOBJ(filepath)) {
            std::cout << "Saved OBJ to: " << filepath << std::endl;
        } else {
            std::cerr << "Failed to save OBJ: " << filepath << std::endl;
        }
    }
}

void ModelingMode::loadOBJFile() {
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"OBJ Model", "obj"}};
    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        if (m_ctx.editableMesh.loadOBJ(filepath)) {
            // Create a new SceneObject from the loaded mesh
            std::string meshName = std::filesystem::path(filepath).stem().string();
            auto obj = std::make_unique<SceneObject>(meshName);

            // Triangulate for GPU rendering
            std::vector<ModelVertex> vertices;
            std::vector<uint32_t> indices;
            m_ctx.editableMesh.triangulate(vertices, indices);

            // Apply default mesh color
            for (auto& v : vertices) {
                v.color = m_ctx.defaultMeshColor;
            }

            uint32_t handle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
            obj->setBufferHandle(handle);
            obj->setIndexCount(static_cast<uint32_t>(indices.size()));
            obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
            obj->setMeshData(vertices, indices);

            // Store half-edge data for proper quad preservation
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

            obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

            m_ctx.selectedObject = obj.get();
            m_ctx.sceneObjects.push_back(std::move(obj));

            // Update faceToTriangles mapping
            m_ctx.faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }

            // Store path for quick save (F5)
            m_ctx.currentFilePath = filepath;
            m_ctx.currentFileFormat = 1;  // OBJ

            std::cout << "Loaded OBJ: " << filepath << std::endl;
        } else {
            std::cerr << "Failed to load OBJ: " << filepath << std::endl;
        }
    }
}

void ModelingMode::saveEditableMeshAsLime() {
    if (!m_ctx.editableMesh.isValid()) {
        std::cerr << "No mesh to save" << std::endl;
        return;
    }

    std::string defaultName = "model.lime";
    if (m_ctx.selectedObject) {
        defaultName = m_ctx.selectedObject->getName() + ".lime";
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"LIME Model", "lime"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        if (filepath.size() < 5 || filepath.substr(filepath.size() - 5) != ".lime") {
            filepath += ".lime";
        }

        // Get texture data if available
        const unsigned char* texData = nullptr;
        int texWidth = 0, texHeight = 0;
        if (m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
            texData = m_ctx.selectedObject->getTextureData().data();
            texWidth = m_ctx.selectedObject->getTextureWidth();
            texHeight = m_ctx.selectedObject->getTextureHeight();
        }

        // Get transform from selected object
        glm::vec3 position(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);
        if (m_ctx.selectedObject) {
            position = m_ctx.selectedObject->getTransform().getPosition();
            rotation = m_ctx.selectedObject->getTransform().getRotation();
            scale = m_ctx.selectedObject->getTransform().getScale();
        }

        if (m_ctx.editableMesh.saveLime(filepath, texData, texWidth, texHeight, position, rotation, scale)) {
            std::cout << "Saved LIME to: " << filepath << std::endl;
        } else {
            std::cerr << "Failed to save LIME: " << filepath << std::endl;
        }
    }
}

void ModelingMode::exportTextureAsPNG() {
    if (!m_ctx.selectedObject || !m_ctx.selectedObject->hasTextureData()) {
        std::cerr << "No texture data to export" << std::endl;
        return;
    }

    std::string defaultName = "texture.png";
    if (m_ctx.selectedObject) {
        defaultName = m_ctx.selectedObject->getName() + "_texture.png";
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"PNG Image", "png"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        if (filepath.size() < 4 || filepath.substr(filepath.size() - 4) != ".png") {
            filepath += ".png";
        }

        const auto& texData = m_ctx.selectedObject->getTextureData();
        int w = m_ctx.selectedObject->getTextureWidth();
        int h = m_ctx.selectedObject->getTextureHeight();

        if (stbi_write_png(filepath.c_str(), w, h, 4, texData.data(), w * 4)) {
            std::cout << "Exported texture to: " << filepath << " (" << w << "x" << h << ")" << std::endl;
        } else {
            std::cerr << "Failed to export texture: " << filepath << std::endl;
        }
    }
}

void ModelingMode::loadLimeFile() {
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"LIME Model", "lime"}};
    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);

    if (result == NFD_OKAY) {
        std::string filepath = outPath;
        NFD_FreePath(outPath);

        std::vector<unsigned char> textureData;
        int texWidth = 0, texHeight = 0;
        glm::vec3 position(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if (m_ctx.editableMesh.loadLime(filepath, textureData, texWidth, texHeight, position, rotation, scale)) {
            // Reset port selection (CPs/ports are cleared by loadLime)
            m_selectedPortIndex = -1;

            // Create a new SceneObject from the loaded mesh
            std::string meshName = std::filesystem::path(filepath).stem().string();
            auto obj = std::make_unique<SceneObject>(meshName);

            // Triangulate for GPU rendering
            std::vector<ModelVertex> vertices;
            std::vector<uint32_t> indices;
            m_ctx.editableMesh.triangulate(vertices, indices);

            // Create GPU model with texture if available
            uint32_t handle;
            if (!textureData.empty() && texWidth > 0 && texHeight > 0) {
                handle = m_ctx.modelRenderer.createModel(vertices, indices, textureData.data(), texWidth, texHeight);
                obj->setTextureData(textureData, texWidth, texHeight);
            } else {
                handle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
            }

            obj->setBufferHandle(handle);
            obj->setIndexCount(static_cast<uint32_t>(indices.size()));
            obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
            obj->setMeshData(vertices, indices);

            // Store half-edge data for proper quad preservation
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

            obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

            // Sync control points from EditableMesh to SceneObject
            {
                std::vector<SceneObject::StoredControlPoint> storedCPs;
                for (const auto& cp : m_ctx.editableMesh.getControlPoints()) {
                    storedCPs.push_back({cp.vertexIndex, cp.name});
                }
                if (!storedCPs.empty()) obj->setControlPoints(storedCPs);
            }
            // Sync ports from EditableMesh to SceneObject
            {
                std::vector<SceneObject::StoredPort> storedPorts;
                for (const auto& p : m_ctx.editableMesh.getPorts()) {
                    storedPorts.push_back({p.name, p.position, p.forward, p.up});
                }
                if (!storedPorts.empty()) obj->setPorts(storedPorts);
            }

            // Apply saved transform
            obj->getTransform().setPosition(position);
            obj->getTransform().setRotation(rotation);
            obj->getTransform().setScale(scale);

            m_ctx.selectedObject = obj.get();
            m_ctx.sceneObjects.push_back(std::move(obj));

            // Update faceToTriangles mapping
            m_ctx.faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }

            // Store path for quick save (F5)
            m_ctx.currentFilePath = filepath;
            m_ctx.currentFileFormat = 2;  // LIME

            // Notify main editor of loaded metadata (for widget properties UI)
            if (m_ctx.onMetadataLoaded) {
                m_ctx.onMetadataLoaded(m_ctx.editableMesh.getMetadata());
            }

            std::cout << "Loaded LIME: " << filepath;
            if (texWidth > 0 && texHeight > 0) {
                std::cout << " (with " << texWidth << "x" << texHeight << " texture)";
            }
            std::cout << std::endl;
        } else {
            std::cerr << "Failed to load LIME: " << filepath << std::endl;
        }
    }
}

void ModelingMode::quickSave() {
    if (m_ctx.currentFilePath.empty() || m_ctx.currentFileFormat == 0) {
        std::cout << "No file loaded - use Save As instead" << std::endl;
        return;
    }

    if (!m_ctx.editableMesh.isValid() && !m_ctx.selectedObject) {
        std::cout << "No mesh to save" << std::endl;
        return;
    }

    bool success = false;
    std::string filepath = m_ctx.currentFilePath;

    switch (m_ctx.currentFileFormat) {
        case 1: {  // OBJ
            success = m_ctx.editableMesh.saveOBJ(filepath);
            break;
        }
        case 2: {  // LIME
            const unsigned char* texData = nullptr;
            int texWidth = 0, texHeight = 0;
            if (m_ctx.selectedObject && m_ctx.selectedObject->hasTextureData()) {
                texData = m_ctx.selectedObject->getTextureData().data();
                texWidth = m_ctx.selectedObject->getTextureWidth();
                texHeight = m_ctx.selectedObject->getTextureHeight();
            }

            glm::vec3 position(0.0f);
            glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 scale(1.0f);
            if (m_ctx.selectedObject) {
                position = m_ctx.selectedObject->getTransform().getPosition();
                rotation = m_ctx.selectedObject->getTransform().getRotation();
                scale = m_ctx.selectedObject->getTransform().getScale();
            }

            success = m_ctx.editableMesh.saveLime(filepath, texData, texWidth, texHeight, position, rotation, scale);
            break;
        }
        case 3: {  // GLB
            if (!m_ctx.selectedObject || !m_ctx.selectedObject->hasMeshData()) {
                std::cout << "No mesh data to save" << std::endl;
                return;
            }

            std::vector<ModelVertex> vertices = m_ctx.selectedObject->getVertices();
            std::vector<uint32_t> indices = m_ctx.selectedObject->getIndices();

            // Apply object transform to bake in scale/rotation/position
            glm::mat4 transform = m_ctx.selectedObject->getTransform().getMatrix();
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

            for (auto& v : vertices) {
                glm::vec4 worldPos = transform * glm::vec4(v.position, 1.0f);
                v.position = glm::vec3(worldPos);
                v.normal = glm::normalize(normalMatrix * v.normal);
            }

            std::string meshName = std::filesystem::path(filepath).stem().string();

            const unsigned char* texData = nullptr;
            int texW = 0, texH = 0;
            if (m_ctx.selectedObject->hasTextureData()) {
                texData = m_ctx.selectedObject->getTextureData().data();
                texW = m_ctx.selectedObject->getTextureWidth();
                texH = m_ctx.selectedObject->getTextureHeight();
            }

            if (m_ctx.selectedObject->hasEditableMeshData()) {
                StoredHEData heData;
                heData.vertices = m_ctx.selectedObject->getHEVertices();
                heData.halfEdges = m_ctx.selectedObject->getHEHalfEdges();
                heData.faces = m_ctx.selectedObject->getHEFaces();
                success = GLBLoader::saveWithHalfEdgeData(filepath, vertices, indices, heData, texData, texW, texH, meshName);
            } else if (texData) {
                success = GLBLoader::save(filepath, vertices, indices, texData, texW, texH, meshName);
            } else {
                success = GLBLoader::save(filepath, vertices, indices, meshName);
            }
            break;
        }
        case 4: {  // LIMES (scene)
            // Scene save doesn't use the single-file quicksave path — call saveLimeScene flow directly
            // But for quickSave we write to the known path without dialog
            if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
                updateMeshFromEditable();
            }

            std::ofstream sceneFile(filepath);
            if (!sceneFile.is_open()) break;

            sceneFile << "# LIME Scene Format v1.0\n";
            sceneFile << "# Multi-object scene file\n";
            sceneFile << "scene_objects: " << m_ctx.sceneObjects.size() << "\n\n";

            for (size_t oi = 0; oi < m_ctx.sceneObjects.size(); ++oi) {
                auto& sobj = m_ctx.sceneObjects[oi];
                sceneFile << "OBJECT_BEGIN \"" << sobj->getName() << "\"\n";

                glm::vec3 p = sobj->getTransform().getPosition();
                glm::quat r = sobj->getTransform().getRotation();
                glm::vec3 s = sobj->getTransform().getScale();
                sceneFile << "transform_pos: " << p.x << " " << p.y << " " << p.z << "\n";
                sceneFile << "transform_rot: " << r.w << " " << r.x << " " << r.y << " " << r.z << "\n";
                sceneFile << "transform_scale: " << s.x << " " << s.y << " " << s.z << "\n";
                if (!sobj->isVisible()) sceneFile << "visible: 0\n";

                if (sobj->hasTextureData()) {
                    const auto& td = sobj->getTextureData();
                    int tw = sobj->getTextureWidth(), th = sobj->getTextureHeight();
                    std::string enc = limes_base64_encode(td.data(), static_cast<size_t>(tw) * th * 4);
                    sceneFile << "tex_size: " << tw << " " << th << "\n";
                    sceneFile << "tex_data: " << enc << "\n";
                }

                if (sobj->hasEditableMeshData()) {
                    const auto& hv = sobj->getHEVertices();
                    const auto& hh = sobj->getHEHalfEdges();
                    const auto& hf = sobj->getHEFaces();

                    sceneFile << "# VERTICES: " << hv.size() << "\n";
                    for (size_t i = 0; i < hv.size(); ++i) {
                        const auto& v = hv[i];
                        sceneFile << "v " << i << ": "
                             << v.position.x << " " << v.position.y << " " << v.position.z << " | "
                             << v.normal.x << " " << v.normal.y << " " << v.normal.z << " | "
                             << v.uv.x << " " << v.uv.y << " | "
                             << v.color.r << " " << v.color.g << " " << v.color.b << " " << v.color.a << " | "
                             << v.halfEdgeIndex << " " << (v.selected ? 1 : 0);
                        if (v.boneIndices != glm::ivec4(0) || v.boneWeights != glm::vec4(0.0f)) {
                            sceneFile << " | " << v.boneIndices.x << " " << v.boneIndices.y << " " << v.boneIndices.z << " " << v.boneIndices.w
                                 << " | " << v.boneWeights.x << " " << v.boneWeights.y << " " << v.boneWeights.z << " " << v.boneWeights.w;
                        }
                        sceneFile << "\n";
                    }

                    sceneFile << "# FACES: " << hf.size() << "\n";
                    for (size_t i = 0; i < hf.size(); ++i) {
                        const auto& f = hf[i];
                        sceneFile << "f " << i << ": " << f.halfEdgeIndex << " " << f.vertexCount << " "
                             << (f.selected ? 1 : 0) << " |";
                        uint32_t he = f.halfEdgeIndex;
                        for (uint32_t vi = 0; vi < f.vertexCount; ++vi) {
                            if (he < hh.size()) {
                                sceneFile << " " << hh[he].vertexIndex;
                                he = hh[he].nextIndex;
                            }
                        }
                        sceneFile << "\n";
                    }

                    sceneFile << "# HALF_EDGES: " << hh.size() << "\n";
                    for (size_t i = 0; i < hh.size(); ++i) {
                        const auto& he = hh[i];
                        sceneFile << "he " << i << ": "
                             << he.vertexIndex << " " << he.faceIndex << " "
                             << he.nextIndex << " " << he.prevIndex << " " << he.twinIndex << "\n";
                    }
                }

                if (sobj->hasControlPoints()) {
                    const auto& cps = sobj->getControlPoints();
                    sceneFile << "# CONTROL_POINTS: " << cps.size() << "\n";
                    for (size_t i = 0; i < cps.size(); ++i) {
                        sceneFile << "cp " << i << ": " << cps[i].vertexIndex << " \"" << cps[i].name << "\"\n";
                    }
                }

                sceneFile << "OBJECT_END\n\n";
            }

            sceneFile.close();
            success = true;
            break;
        }
    }

    if (success) {
        m_saveNotificationTimer = 1.0f;  // Show notification for 1 second
        std::cout << "Saved: " << filepath << std::endl;
    } else {
        std::cerr << "Failed to save: " << filepath << std::endl;
    }
}

void ModelingMode::saveLimeScene() {
    if (m_ctx.sceneObjects.empty()) {
        std::cout << "No objects in scene to save" << std::endl;
        return;
    }

    // Sync current editable mesh to SceneObject before saving
    if (m_ctx.selectedObject && m_ctx.editableMesh.isValid()) {
        updateMeshFromEditable();
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"LIME Scene", "limes"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, "scene.limes");

    if (result != NFD_OKAY) return;

    std::string filepath = outPath;
    NFD_FreePath(outPath);

    if (filepath.size() < 6 || filepath.substr(filepath.size() - 6) != ".limes") {
        filepath += ".limes";
    }

    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for writing" << std::endl;
        return;
    }

    file << "# LIME Scene Format v1.0\n";
    file << "# Multi-object scene file\n";
    file << "scene_objects: " << m_ctx.sceneObjects.size() << "\n\n";

    for (size_t objIdx = 0; objIdx < m_ctx.sceneObjects.size(); ++objIdx) {
        auto& obj = m_ctx.sceneObjects[objIdx];

        file << "OBJECT_BEGIN \"" << obj->getName() << "\"\n";

        // Transform
        glm::vec3 pos = obj->getTransform().getPosition();
        glm::quat rot = obj->getTransform().getRotation();
        glm::vec3 scl = obj->getTransform().getScale();
        file << "transform_pos: " << pos.x << " " << pos.y << " " << pos.z << "\n";
        file << "transform_rot: " << rot.w << " " << rot.x << " " << rot.y << " " << rot.z << "\n";
        file << "transform_scale: " << scl.x << " " << scl.y << " " << scl.z << "\n";

        // Visibility
        if (!obj->isVisible()) {
            file << "visible: 0\n";
        }

        // Texture
        if (obj->hasTextureData()) {
            const auto& texData = obj->getTextureData();
            int texW = obj->getTextureWidth();
            int texH = obj->getTextureHeight();
            size_t texSize = static_cast<size_t>(texW) * texH * 4;
            std::string encoded = limes_base64_encode(texData.data(), texSize);
            file << "tex_size: " << texW << " " << texH << "\n";
            file << "tex_data: " << encoded << "\n";
        }

        // Half-edge mesh data from SceneObject's stored data
        if (obj->hasEditableMeshData()) {
            const auto& heVerts = obj->getHEVertices();
            const auto& heHalfEdges = obj->getHEHalfEdges();
            const auto& heFaces = obj->getHEFaces();

            // Vertices
            file << "# VERTICES: " << heVerts.size() << "\n";
            for (size_t i = 0; i < heVerts.size(); ++i) {
                const auto& v = heVerts[i];
                file << "v " << i << ": "
                     << v.position.x << " " << v.position.y << " " << v.position.z << " | "
                     << v.normal.x << " " << v.normal.y << " " << v.normal.z << " | "
                     << v.uv.x << " " << v.uv.y << " | "
                     << v.color.r << " " << v.color.g << " " << v.color.b << " " << v.color.a << " | "
                     << v.halfEdgeIndex << " " << (v.selected ? 1 : 0);
                // Bone data if non-zero
                if (v.boneIndices != glm::ivec4(0) || v.boneWeights != glm::vec4(0.0f)) {
                    file << " | " << v.boneIndices.x << " " << v.boneIndices.y << " " << v.boneIndices.z << " " << v.boneIndices.w
                         << " | " << v.boneWeights.x << " " << v.boneWeights.y << " " << v.boneWeights.z << " " << v.boneWeights.w;
                }
                file << "\n";
            }

            // Faces — need to reconstruct vertex indices from half-edge traversal
            file << "# FACES: " << heFaces.size() << "\n";
            for (size_t i = 0; i < heFaces.size(); ++i) {
                const auto& f = heFaces[i];
                file << "f " << i << ": " << f.halfEdgeIndex << " " << f.vertexCount << " "
                     << (f.selected ? 1 : 0) << " |";
                // Walk the half-edge loop to get vertex indices
                uint32_t he = f.halfEdgeIndex;
                for (uint32_t vi = 0; vi < f.vertexCount; ++vi) {
                    if (he < heHalfEdges.size()) {
                        file << " " << heHalfEdges[he].vertexIndex;
                        he = heHalfEdges[he].nextIndex;
                    }
                }
                file << "\n";
            }

            // Half-edges
            file << "# HALF_EDGES: " << heHalfEdges.size() << "\n";
            for (size_t i = 0; i < heHalfEdges.size(); ++i) {
                const auto& he = heHalfEdges[i];
                file << "he " << i << ": "
                     << he.vertexIndex << " "
                     << he.faceIndex << " "
                     << he.nextIndex << " "
                     << he.prevIndex << " "
                     << he.twinIndex << "\n";
            }
        }

        // Control points
        if (obj->hasControlPoints()) {
            const auto& cps = obj->getControlPoints();
            file << "# CONTROL_POINTS: " << cps.size() << "\n";
            for (size_t i = 0; i < cps.size(); ++i) {
                file << "cp " << i << ": " << cps[i].vertexIndex << " \"" << cps[i].name << "\"\n";
            }
        }

        file << "OBJECT_END\n\n";
    }

    file.close();

    m_ctx.currentFilePath = filepath;
    m_ctx.currentFileFormat = 4;  // LIMES
    m_saveNotificationTimer = 1.0f;
    std::cout << "Saved scene (" << m_ctx.sceneObjects.size() << " objects) to: " << filepath << std::endl;
}

void ModelingMode::loadLimeScene() {
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"LIME Scene", "limes"}};
    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);

    if (result != NFD_OKAY) return;

    std::string filepath = outPath;
    NFD_FreePath(outPath);

    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filepath << " for reading" << std::endl;
        return;
    }

    // Clear existing scene
    for (auto& obj : m_ctx.sceneObjects) {
        uint32_t handle = obj->getBufferHandle();
        if (handle != UINT32_MAX && handle != 0) {
            m_ctx.modelRenderer.destroyModel(handle);
        }
    }
    m_ctx.sceneObjects.clear();
    m_ctx.selectedObject = nullptr;
    m_ctx.selectedObjects.clear();
    m_ctx.editableMesh.clear();
    m_ctx.meshDirty = false;

    // Parse the scene file
    std::string line;
    int objectCount = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "scene_objects:") {
            iss >> objectCount;
        }
        else if (type == "OBJECT_BEGIN") {
            // Parse object name from "OBJECT_BEGIN "name""
            std::string objName;
            size_t q1 = line.find('"');
            size_t q2 = line.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                objName = line.substr(q1 + 1, q2 - q1 - 1);
            } else {
                objName = "Object_" + std::to_string(m_ctx.sceneObjects.size());
            }

            // Parse this object's data until OBJECT_END
            glm::vec3 pos(0.0f);
            glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 scl(1.0f);
            bool visible = true;
            std::vector<unsigned char> textureData;
            int texW = 0, texH = 0;

            std::vector<SceneObject::StoredHEVertex> heVerts;
            std::vector<SceneObject::StoredHalfEdge> heHalfEdges;
            std::vector<SceneObject::StoredHEFace> heFaces;
            std::vector<SceneObject::StoredControlPoint> controlPoints;

            while (std::getline(file, line)) {
                if (line == "OBJECT_END") break;
                if (line.empty() || line[0] == '#') continue;

                std::istringstream objIss(line);
                std::string objType;
                objIss >> objType;

                if (objType == "transform_pos:") {
                    objIss >> pos.x >> pos.y >> pos.z;
                }
                else if (objType == "transform_rot:") {
                    objIss >> rot.w >> rot.x >> rot.y >> rot.z;
                }
                else if (objType == "transform_scale:") {
                    objIss >> scl.x >> scl.y >> scl.z;
                }
                else if (objType == "visible:") {
                    int v; objIss >> v; visible = (v != 0);
                }
                else if (objType == "tex_size:") {
                    objIss >> texW >> texH;
                }
                else if (objType == "tex_data:") {
                    std::string encoded;
                    objIss >> encoded;
                    textureData = limes_base64_decode(encoded);
                }
                else if (objType == "v") {
                    uint32_t idx;
                    char colon, pipe1, pipe2, pipe3, pipe4;
                    SceneObject::StoredHEVertex v;
                    int selected;

                    objIss >> idx >> colon
                           >> v.position.x >> v.position.y >> v.position.z >> pipe1
                           >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                           >> v.uv.x >> v.uv.y >> pipe3;

                    float r, g, b, a;
                    if (objIss >> r >> g >> b >> a >> pipe4) {
                        v.color = glm::vec4(r, g, b, a);
                        objIss >> v.halfEdgeIndex >> selected;
                        // Try bone data
                        char pipe5;
                        int bi0, bi1, bi2, bi3;
                        if (objIss >> pipe5 >> bi0 >> bi1 >> bi2 >> bi3) {
                            v.boneIndices = glm::ivec4(bi0, bi1, bi2, bi3);
                            char pipe6;
                            float bw0, bw1, bw2, bw3;
                            if (objIss >> pipe6 >> bw0 >> bw1 >> bw2 >> bw3) {
                                v.boneWeights = glm::vec4(bw0, bw1, bw2, bw3);
                            }
                        }
                    } else {
                        v.color = glm::vec4(1.0f);
                        std::istringstream iss2(line);
                        std::string dummy;
                        iss2 >> dummy >> idx >> colon
                             >> v.position.x >> v.position.y >> v.position.z >> pipe1
                             >> v.normal.x >> v.normal.y >> v.normal.z >> pipe2
                             >> v.uv.x >> v.uv.y >> pipe3
                             >> v.halfEdgeIndex >> selected;
                    }
                    v.selected = (selected != 0);

                    if (idx >= heVerts.size()) heVerts.resize(idx + 1);
                    heVerts[idx] = v;
                }
                else if (objType == "f") {
                    uint32_t idx, heIdx, vertCount;
                    int selected;
                    char colon, pipe;
                    objIss >> idx >> colon >> heIdx >> vertCount >> selected >> pipe;

                    SceneObject::StoredHEFace f;
                    f.halfEdgeIndex = heIdx;
                    f.vertexCount = vertCount;
                    f.selected = (selected != 0);

                    if (idx >= heFaces.size()) heFaces.resize(idx + 1);
                    heFaces[idx] = f;
                }
                else if (objType == "he") {
                    uint32_t idx;
                    char colon;
                    SceneObject::StoredHalfEdge he;
                    objIss >> idx >> colon
                           >> he.vertexIndex >> he.faceIndex >> he.nextIndex >> he.prevIndex >> he.twinIndex;

                    if (idx >= heHalfEdges.size()) heHalfEdges.resize(idx + 1);
                    heHalfEdges[idx] = he;
                }
                else if (objType == "cp") {
                    uint32_t idx;
                    char colon;
                    uint32_t vertIdx;
                    objIss >> idx >> colon >> vertIdx;
                    std::string cpName = "CP" + std::to_string(idx);
                    std::string rest;
                    std::getline(objIss, rest);
                    size_t cq1 = rest.find('"');
                    size_t cq2 = rest.rfind('"');
                    if (cq1 != std::string::npos && cq2 != std::string::npos && cq2 > cq1) {
                        cpName = rest.substr(cq1 + 1, cq2 - cq1 - 1);
                    }
                    controlPoints.push_back({vertIdx, cpName});
                }
            }

            // Reconstruct the SceneObject
            if (heVerts.empty()) continue;  // Skip empty objects

            auto newObj = std::make_unique<SceneObject>(objName);

            // Store HE data on SceneObject
            newObj->setEditableMeshData(heVerts, heHalfEdges, heFaces);

            // Store control points
            if (!controlPoints.empty()) {
                newObj->setControlPoints(controlPoints);
            }

            // Triangulate using EditableMesh for GPU mesh creation
            eden::EditableMesh tempMesh;
            // Convert stored data to EditableMesh types
            std::vector<eden::HEVertex> emVerts;
            emVerts.reserve(heVerts.size());
            for (const auto& sv : heVerts) {
                eden::HEVertex ev;
                ev.position = sv.position;
                ev.normal = sv.normal;
                ev.uv = sv.uv;
                ev.color = sv.color;
                ev.halfEdgeIndex = sv.halfEdgeIndex;
                ev.selected = sv.selected;
                ev.boneIndices = sv.boneIndices;
                ev.boneWeights = sv.boneWeights;
                emVerts.push_back(ev);
            }
            std::vector<eden::HalfEdge> emHE;
            emHE.reserve(heHalfEdges.size());
            for (const auto& she : heHalfEdges) {
                emHE.push_back({she.vertexIndex, she.faceIndex, she.nextIndex, she.prevIndex, she.twinIndex});
            }
            std::vector<eden::HEFace> emFaces;
            emFaces.reserve(heFaces.size());
            for (const auto& sf : heFaces) {
                emFaces.push_back({sf.halfEdgeIndex, sf.vertexCount, sf.selected});
            }
            tempMesh.setFromData(emVerts, emHE, emFaces);

            std::vector<ModelVertex> vertices;
            std::vector<uint32_t> indices;
            tempMesh.triangulate(vertices, indices);

            // Create GPU model
            uint32_t handle;
            if (!textureData.empty() && texW > 0 && texH > 0) {
                handle = m_ctx.modelRenderer.createModel(vertices, indices, textureData.data(), texW, texH);
                newObj->setTextureData(textureData, texW, texH);
            } else {
                handle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
            }

            newObj->setBufferHandle(handle);
            newObj->setIndexCount(static_cast<uint32_t>(indices.size()));
            newObj->setVertexCount(static_cast<uint32_t>(vertices.size()));
            newObj->setMeshData(vertices, indices);

            // Apply transform
            newObj->getTransform().setPosition(pos);
            newObj->getTransform().setRotation(rot);
            newObj->getTransform().setScale(scl);
            newObj->setVisible(visible);

            m_ctx.sceneObjects.push_back(std::move(newObj));
        }
    }

    file.close();

    // Select first object
    if (!m_ctx.sceneObjects.empty()) {
        m_ctx.selectedObject = m_ctx.sceneObjects[0].get();
        buildEditableMeshFromObject();
    }

    m_ctx.currentFilePath = filepath;
    m_ctx.currentFileFormat = 4;  // LIMES
    invalidateWireframeCache();

    std::cout << "Loaded scene from " << filepath << ": " << m_ctx.sceneObjects.size() << " objects" << std::endl;
}

void ModelingMode::drawQuadWireframeOverlay(Camera& camera, float vpX, float vpY, float vpW, float vpH) {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->isVisible()) return;

    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
    glm::mat4 view = camera.getViewMatrix();
    float aspectRatio = vpW / vpH;
    glm::mat4 proj = camera.getProjectionMatrix(aspectRatio);
    glm::mat4 vp = proj * view;

    glm::vec3 cameraPos = camera.getPosition();

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    auto worldToScreen = [&](const glm::vec3& localPos) -> ImVec2 {
        glm::vec4 worldPos = modelMatrix * glm::vec4(localPos, 1.0f);
        glm::vec4 clip = vp * worldPos;
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    auto isFaceFrontFacing = [&](uint32_t faceIdx) -> bool {
        glm::vec3 localCenter = m_ctx.editableMesh.getFaceCenter(faceIdx);
        glm::vec3 localNormal = m_ctx.editableMesh.getFaceNormal(faceIdx);
        glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(localCenter, 1.0f));
        glm::vec3 worldNormal = glm::normalize(normalMatrix * localNormal);
        glm::vec3 viewDir = glm::normalize(cameraPos - worldCenter);
        return glm::dot(worldNormal, viewDir) > 0.0f;
    };

    ImU32 wireColor = IM_COL32(
        static_cast<int>(m_ctx.wireframeColor.r * 255),
        static_cast<int>(m_ctx.wireframeColor.g * 255),
        static_cast<int>(m_ctx.wireframeColor.b * 255),
        static_cast<int>(m_ctx.wireframeColor.a * 255));
    ImU32 selectedEdgeColor = IM_COL32(50, 100, 255, 255);
    float lineThickness = 2.0f;
    float selectedLineThickness = 4.0f;

    std::set<uint64_t> selectedEdgeKeys;
    auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
    for (uint32_t he : selectedEdges) {
        auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(he);
        uint64_t key = (static_cast<uint64_t>(std::min(v0, v1)) << 32) | static_cast<uint64_t>(std::max(v0, v1));
        selectedEdgeKeys.insert(key);
    }

    auto isEdgeSelected = [&](uint32_t vi0, uint32_t vi1) -> bool {
        uint64_t key = (static_cast<uint64_t>(std::min(vi0, vi1)) << 32) | static_cast<uint64_t>(std::max(vi0, vi1));
        return selectedEdgeKeys.count(key) > 0;
    };

    // Check if selected object has x-ray mode
    bool xrayMode = m_ctx.selectedObject && m_ctx.selectedObject->isXRay();

    if (xrayMode) {
        for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
            auto verts = m_ctx.editableMesh.getFaceVertices(faceIdx);
            if (verts.empty()) continue;

            for (size_t i = 0; i < verts.size(); ++i) {
                uint32_t vi0 = verts[i];
                uint32_t vi1 = verts[(i + 1) % verts.size()];
                const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
                const auto& v1 = m_ctx.editableMesh.getVertex(vi1);

                ImVec2 screenV0 = worldToScreen(v0.position);
                ImVec2 screenV1 = worldToScreen(v1.position);

                if (screenV0.x > -500 && screenV1.x > -500) {
                    bool selected = isEdgeSelected(vi0, vi1);
                    drawList->AddLine(screenV0, screenV1, selected ? selectedEdgeColor : wireColor,
                                     selected ? selectedLineThickness : lineThickness);
                }
            }
        }
    } else {
        std::set<uint64_t> drawnEdges;

        for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
            if (!isFaceFrontFacing(faceIdx)) continue;

            auto verts = m_ctx.editableMesh.getFaceVertices(faceIdx);
            if (verts.empty()) continue;

            for (size_t i = 0; i < verts.size(); ++i) {
                uint32_t vi0 = verts[i];
                uint32_t vi1 = verts[(i + 1) % verts.size()];

                uint64_t edgeKey = (static_cast<uint64_t>(std::min(vi0, vi1)) << 32) |
                                   static_cast<uint64_t>(std::max(vi0, vi1));

                if (drawnEdges.count(edgeKey) > 0) continue;
                drawnEdges.insert(edgeKey);

                const auto& v0 = m_ctx.editableMesh.getVertex(vi0);
                const auto& v1 = m_ctx.editableMesh.getVertex(vi1);

                ImVec2 screenV0 = worldToScreen(v0.position);
                ImVec2 screenV1 = worldToScreen(v1.position);

                if (screenV0.x > -500 && screenV1.x > -500) {
                    bool selected = isEdgeSelected(vi0, vi1);
                    drawList->AddLine(screenV0, screenV1, selected ? selectedEdgeColor : wireColor,
                                     selected ? selectedLineThickness : lineThickness);
                }
            }
        }
    }

    // Draw vertices in vertex mode
    if (m_ctx.modelingSelectionMode == ModelingSelectionMode::Vertex) {
        ImU32 vertexColor = IM_COL32(0, 200, 255, 255);
        ImU32 selectedVertexColor = IM_COL32(255, 150, 0, 255);
        ImU32 hoveredVertexColor = IM_COL32(255, 255, 0, 255);
        float vertexRadius = m_ctx.vertexDisplaySize * 100.0f;

        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
        std::set<uint32_t> selectedSet(selectedVerts.begin(), selectedVerts.end());

        for (uint32_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
            const auto& v = m_ctx.editableMesh.getVertex(vi);

            auto faces = m_ctx.editableMesh.getVertexFaces(vi);
            bool visible = false;
            for (uint32_t faceIdx : faces) {
                if (isFaceFrontFacing(faceIdx)) {
                    visible = true;
                    break;
                }
            }

            if (!visible && !xrayMode) continue;

            ImVec2 screenPos = worldToScreen(v.position);
            if (screenPos.x < -500) continue;

            ImU32 color = vertexColor;
            float radius = vertexRadius;

            if (selectedSet.count(vi) > 0) {
                color = selectedVertexColor;
                radius = vertexRadius * 1.3f;
            }
            if (static_cast<int>(vi) == m_ctx.hoveredVertex) {
                color = hoveredVertexColor;
                radius = vertexRadius * 1.5f;
            }

            drawList->AddCircleFilled(screenPos, radius, color);
            drawList->AddCircle(screenPos, radius, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        }
    }

    // Draw control points as magenta diamonds with names (visible in all selection modes)
    {
        ImU32 cpColor = IM_COL32(255, 0, 255, 255);
        ImU32 cpOutline = IM_COL32(0, 0, 0, 220);
        ImU32 cpTextColor = IM_COL32(255, 200, 255, 255);
        ImU32 cpTextBg = IM_COL32(0, 0, 0, 180);
        float cpSize = m_ctx.vertexDisplaySize * 150.0f;

        for (const auto& cp : m_ctx.editableMesh.getControlPoints()) {
            if (cp.vertexIndex >= m_ctx.editableMesh.getVertexCount()) continue;
            const auto& v = m_ctx.editableMesh.getVertex(cp.vertexIndex);
            ImVec2 sp = worldToScreen(v.position);
            if (sp.x < -500) continue;

            // Diamond shape
            ImVec2 top(sp.x, sp.y - cpSize);
            ImVec2 right(sp.x + cpSize, sp.y);
            ImVec2 bottom(sp.x, sp.y + cpSize);
            ImVec2 left(sp.x - cpSize, sp.y);
            drawList->AddQuadFilled(top, right, bottom, left, cpColor);
            drawList->AddQuad(top, right, bottom, left, cpOutline, 2.0f);

            // Draw name label
            if (!cp.name.empty()) {
                ImVec2 textSize = ImGui::CalcTextSize(cp.name.c_str());
                ImVec2 textPos(sp.x - textSize.x * 0.5f, sp.y - cpSize - textSize.y - 2);
                drawList->AddRectFilled(
                    ImVec2(textPos.x - 2, textPos.y - 1),
                    ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1),
                    cpTextBg, 2.0f);
                drawList->AddText(textPos, cpTextColor, cp.name.c_str());
            }
        }
    }

    drawList->PopClipRect();
}

void ModelingMode::drawFaceNormalsOverlay(Camera& camera, float vpX, float vpY, float vpW, float vpH) {
    if (!m_ctx.showFaceNormals || !m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;
    if (!m_ctx.selectedObject->isVisible()) return;

    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
    glm::mat4 view = camera.getViewMatrix();
    float aspectRatio = vpW / vpH;
    glm::mat4 proj = camera.getProjectionMatrix(aspectRatio);
    glm::mat4 vp = proj * view;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    auto worldToScreen = [&](const glm::vec3& localPos) -> ImVec2 {
        glm::vec4 worldPos = modelMatrix * glm::vec4(localPos, 1.0f);
        glm::vec4 clip = vp * worldPos;
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    ImU32 normalColor = IM_COL32(0, 255, 128, 255);

    for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
        glm::vec3 center = m_ctx.editableMesh.getFaceCenter(faceIdx);
        glm::vec3 normal = m_ctx.editableMesh.getFaceNormal(faceIdx);
        glm::vec3 endPoint = center + normal * m_ctx.normalDisplayLength;

        ImVec2 screenStart = worldToScreen(center);
        ImVec2 screenEnd = worldToScreen(endPoint);

        if (screenStart.x > -500 && screenEnd.x > -500) {
            drawList->AddLine(screenStart, screenEnd, normalColor, 2.0f);
            drawList->AddCircleFilled(screenEnd, 3.0f, normalColor);
        }
    }

    drawList->PopClipRect();
}

void ModelingMode::drawReferenceImages(Camera& camera, float vpX, float vpY, float vpW, float vpH) {
    // Only draw reference images in ortho views
    if (camera.getProjectionMode() != ProjectionMode::Orthographic) return;

    ViewPreset preset = camera.getViewPreset();
    if (preset == ViewPreset::Custom) return;

    int viewIndex = static_cast<int>(preset) - 1;  // ViewPreset enum offset
    if (viewIndex < 0 || viewIndex >= 6) return;

    auto& ref = m_ctx.referenceImages[viewIndex];
    if (!ref.visible || !ref.loaded || !ref.descriptorSet) return;

    // Calculate view-projection for screen space conversion
    float orthoSize = camera.getOrthoSize();
    float aspect = vpW / vpH;
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = glm::ortho(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, -1000.0f, 1000.0f);
    glm::mat4 viewProj = proj * view;

    // Get the plane axes and depth offset based on view preset
    // Place image at the far edge of the grid (behind the model)
    glm::vec3 right, up, depthOffset;
    float gridEdge = 10.0f;  // Match grid size

    switch (preset) {
        case ViewPreset::Top:
            right = glm::vec3(1, 0, 0);
            up = glm::vec3(0, 0, -1);
            depthOffset = glm::vec3(0, -0.1f, 0);  // Just below ground plane
            break;
        case ViewPreset::Bottom:
            right = glm::vec3(1, 0, 0);
            up = glm::vec3(0, 0, 1);
            depthOffset = glm::vec3(0, 0.1f, 0);  // Just above ground plane
            break;
        case ViewPreset::Front:
            right = glm::vec3(1, 0, 0);
            up = glm::vec3(0, 1, 0);
            depthOffset = glm::vec3(0, 0, -gridEdge);  // At back of grid
            break;
        case ViewPreset::Back:
            right = glm::vec3(-1, 0, 0);
            up = glm::vec3(0, 1, 0);
            depthOffset = glm::vec3(0, 0, gridEdge);  // At front of grid
            break;
        case ViewPreset::Right:
            right = glm::vec3(0, 0, -1);
            up = glm::vec3(0, 1, 0);
            depthOffset = glm::vec3(-gridEdge, 0, 0);  // At left edge
            break;
        case ViewPreset::Left:
            right = glm::vec3(0, 0, 1);
            up = glm::vec3(0, 1, 0);
            depthOffset = glm::vec3(gridEdge, 0, 0);  // At right edge
            break;
        default:
            return;
    }

    // Calculate world corners of the reference image
    glm::vec3 center = depthOffset + right * ref.offset.x + up * ref.offset.y;
    glm::vec3 halfSize = right * (ref.size.x * 0.5f) + up * (ref.size.y * 0.5f);

    glm::vec3 corners[4] = {
        center - right * (ref.size.x * 0.5f) - up * (ref.size.y * 0.5f),
        center + right * (ref.size.x * 0.5f) - up * (ref.size.y * 0.5f),
        center + right * (ref.size.x * 0.5f) + up * (ref.size.y * 0.5f),
        center - right * (ref.size.x * 0.5f) + up * (ref.size.y * 0.5f)
    };

    // Project to screen space
    ImVec2 screenCorners[4];
    for (int i = 0; i < 4; i++) {
        glm::vec4 clip = viewProj * glm::vec4(corners[i], 1.0f);
        glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
        screenCorners[i].x = vpX + (ndc.x * 0.5f + 0.5f) * vpW;
        screenCorners[i].y = vpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * vpH;
    }

    // Draw the image using ImGui, clipped to viewport bounds
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);
    ImU32 tintColor = IM_COL32(255, 255, 255, static_cast<int>(ref.opacity * 255));

    drawList->AddImageQuad(
        (ImTextureID)ref.descriptorSet,
        screenCorners[0], screenCorners[1], screenCorners[2], screenCorners[3],
        ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), ImVec2(0, 0),
        tintColor
    );
    drawList->PopClipRect();
}

// UV helper implementations

void ModelingMode::loadReferenceImage(int viewIndex) {
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filterItem[1] = {{"Images", "png,jpg,jpeg,bmp,tga"}};

    nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, nullptr);

    if (result == NFD_OKAY && outPath) {
        if (m_ctx.loadReferenceImageCallback) {
            m_ctx.loadReferenceImageCallback(viewIndex, std::string(outPath));
        }
        NFD_FreePath(outPath);
    }
}

// ============================================================================
// Gizmo Implementation
// ============================================================================


// ============================================================================
// Edge Path Extrusion - Create box tube along selected edges
// ============================================================================

