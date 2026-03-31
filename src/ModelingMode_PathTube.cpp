// ModelingMode_PathTube.cpp - Path tube mesh generation tool
// Click to place nodes along a spline, then generate a quad tube mesh

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>

#include <iostream>
#include <limits>
#include <cmath>
#include <algorithm>
#include <array>
#include <glm/gtc/constants.hpp>

using namespace eden;

// --- Spline math (adapted from PathTool.cpp) ---

glm::vec3 ModelingMode::pathCatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                        const glm::vec3& p2, const glm::vec3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;

    glm::vec3 result = 0.5f * (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
    );

    return result;
}

glm::vec3 ModelingMode::pathEvaluateSpline(const std::vector<glm::vec3>& points, float t) {
    if (points.size() < 2) {
        if (points.size() == 1) return points[0];
        return glm::vec3(0);
    }

    size_t numSegments = points.size() - 1;
    float scaledT = t * numSegments;
    size_t segmentIndex = static_cast<size_t>(std::floor(scaledT));
    segmentIndex = std::min(segmentIndex, numSegments - 1);

    float localT = scaledT - segmentIndex;

    size_t i0 = (segmentIndex == 0) ? 0 : segmentIndex - 1;
    size_t i1 = segmentIndex;
    size_t i2 = segmentIndex + 1;
    size_t i3 = std::min(segmentIndex + 2, points.size() - 1);

    return pathCatmullRom(points[i0], points[i1], points[i2], points[i3], localT);
}

std::vector<glm::vec3> ModelingMode::pathSampleSpline(const std::vector<glm::vec3>& points,
                                                       int samplesPerSegment) {
    std::vector<glm::vec3> samples;

    if (points.size() < 2) {
        for (const auto& pt : points) {
            samples.push_back(pt);
        }
        return samples;
    }

    size_t numSegments = points.size() - 1;
    int totalSamples = static_cast<int>(numSegments) * samplesPerSegment + 1;

    samples.reserve(totalSamples);

    for (int i = 0; i < totalSamples; i++) {
        float t = static_cast<float>(i) / (totalSamples - 1);
        glm::vec3 point = pathEvaluateSpline(points, t);
        // No terrain height snapping — free 3D placement
        samples.push_back(point);
    }

    return samples;
}

// --- Profile editor ---

void ModelingMode::resetPathTubeProfile() {
    int segments = m_pathTubeSegments;
    m_pathTubeProfile.resize(segments);
    const float twoPi = glm::two_pi<float>();
    for (int i = 0; i < segments; ++i) {
        float angle = (static_cast<float>(i) / segments) * twoPi;
        m_pathTubeProfile[i] = glm::vec2(std::cos(angle), std::sin(angle));
    }
    m_profileDragIdx = -1;
}

void ModelingMode::drawProfileEditor() {
    const float widgetSize = 200.0f;
    const float halfSize = widgetSize * 0.5f;
    const float profileRange = 1.2f;  // profile coords at widget edge

    ImGui::Text("Cross-Section Profile");

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##profile_canvas", ImVec2(widgetSize, widgetSize));
    bool canvasHovered = ImGui::IsItemHovered();
    bool canvasActive = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + widgetSize, canvasPos.y + widgetSize),
                      IM_COL32(40, 40, 40, 255));

    // Coordinate mapping helpers
    auto profileToPixel = [&](const glm::vec2& p) -> ImVec2 {
        float px = canvasPos.x + halfSize + (p.x / profileRange) * halfSize;
        float py = canvasPos.y + halfSize - (p.y / profileRange) * halfSize;  // Y flipped
        return ImVec2(px, py);
    };
    auto pixelToProfile = [&](const ImVec2& px) -> glm::vec2 {
        float x = (px.x - canvasPos.x - halfSize) / halfSize * profileRange;
        float y = -(px.y - canvasPos.y - halfSize) / halfSize * profileRange;
        return glm::vec2(x, y);
    };

    ImVec2 center(canvasPos.x + halfSize, canvasPos.y + halfSize);

    // Grid crosshairs
    dl->AddLine(ImVec2(canvasPos.x, center.y), ImVec2(canvasPos.x + widgetSize, center.y),
                IM_COL32(80, 80, 80, 255));
    dl->AddLine(ImVec2(center.x, canvasPos.y), ImVec2(center.x, canvasPos.y + widgetSize),
                IM_COL32(80, 80, 80, 255));

    // Unit circle reference (faint)
    float unitRadius = halfSize / profileRange;  // pixels for radius=1.0
    dl->AddCircle(center, unitRadius, IM_COL32(100, 100, 100, 80), 64);

    // Ensure profile is initialized
    if (m_pathTubeProfile.empty() || static_cast<int>(m_pathTubeProfile.size()) != m_pathTubeSegments) {
        resetPathTubeProfile();
    }

    int segments = static_cast<int>(m_pathTubeProfile.size());

    // Draw profile line loop
    for (int i = 0; i < segments; ++i) {
        ImVec2 a = profileToPixel(m_pathTubeProfile[i]);
        ImVec2 b = profileToPixel(m_pathTubeProfile[(i + 1) % segments]);
        dl->AddLine(a, b, IM_COL32(0, 220, 255, 255), 2.0f);
    }

    // Interaction: pick / drag vertices
    ImVec2 mousePos = ImGui::GetMousePos();

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Find nearest vertex within 10px
        float bestDist = 10.0f;
        int bestIdx = -1;
        for (int i = 0; i < segments; ++i) {
            ImVec2 vp = profileToPixel(m_pathTubeProfile[i]);
            float dist = std::sqrt((mousePos.x - vp.x) * (mousePos.x - vp.x) +
                                   (mousePos.y - vp.y) * (mousePos.y - vp.y));
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }
        m_profileDragIdx = bestIdx;
    }

    if (m_profileDragIdx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        glm::vec2 newPos = pixelToProfile(mousePos);
        newPos = glm::clamp(newPos, glm::vec2(-1.5f), glm::vec2(1.5f));
        m_pathTubeProfile[m_profileDragIdx] = newPos;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_profileDragIdx = -1;
    }

    // Draw vertex handles
    for (int i = 0; i < segments; ++i) {
        ImVec2 vp = profileToPixel(m_pathTubeProfile[i]);
        bool isActive = (i == m_profileDragIdx);
        // Check hover
        bool isHovered = false;
        if (canvasHovered) {
            float dist = std::sqrt((mousePos.x - vp.x) * (mousePos.x - vp.x) +
                                   (mousePos.y - vp.y) * (mousePos.y - vp.y));
            isHovered = (dist < 10.0f);
        }
        ImU32 color = (isActive || isHovered) ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 220, 255, 255);
        dl->AddCircleFilled(vp, 4.0f, color);
        dl->AddCircle(vp, 4.0f, IM_COL32(0, 0, 0, 255));
    }

    // Reset button
    if (ImGui::Button("Reset Circle")) {
        resetPathTubeProfile();
    }
}

// --- Input handling ---

void ModelingMode::processPathTubeInput(bool mouseOverImGui) {
    if (!m_pathTubeMode) return;

    Camera& activeCamera = m_ctx.getActiveCamera();

    // ESC: cancel mode
    if (Input::isKeyPressed(Input::KEY_ESCAPE)) {
        cancelPathTubeMode();
        return;
    }

    // Enter: generate mesh
    if (Input::isKeyPressed(Input::KEY_ENTER)) {
        if (m_pathNodes.size() >= 2) {
            generatePathTubeMesh();
        }
        return;
    }

    // Ctrl+Z: remove last node
    if (Input::isKeyPressed(Input::KEY_Z) &&
        (Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL))) {
        if (!m_pathNodes.empty()) {
            m_pathNodes.pop_back();
            if (m_pathSelectedNode >= static_cast<int>(m_pathNodes.size())) {
                m_pathSelectedNode = m_pathNodes.empty() ? -1 : static_cast<int>(m_pathNodes.size()) - 1;
            }
            // Reset surface attachment if first node was removed
            if (m_pathNodes.empty()) {
                m_pathTubeAttached = false;
            }
            std::cout << "[PathTube] Undo — " << m_pathNodes.size() << " nodes" << std::endl;
        }
        return;
    }

    // Del: delete selected node
    if (Input::isKeyPressed(Input::KEY_DELETE) && m_pathSelectedNode >= 0 &&
        m_pathSelectedNode < static_cast<int>(m_pathNodes.size())) {
        m_pathNodes.erase(m_pathNodes.begin() + m_pathSelectedNode);
        m_pathSelectedNode = -1;
        std::cout << "[PathTube] Deleted node — " << m_pathNodes.size() << " nodes" << std::endl;
        return;
    }

    if (mouseOverImGui) return;

    // Helper: intersect ray with a plane to get placement position
    auto intersectPlane = [](const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                             const glm::vec3& planeNormal, float planeD,
                             glm::vec3& hitPos) -> bool {
        float denom = glm::dot(planeNormal, rayDir);
        if (std::abs(denom) < 1e-6f) return false;
        float t = -(glm::dot(planeNormal, rayOrigin) + planeD) / denom;
        if (t < 0.0f) return false;
        hitPos = rayOrigin + rayDir * t;
        return true;
    };

    // Helper: get placement plane based on camera view
    auto getPlacementPlane = [&](glm::vec3& planeNormal, float& planeD) {
        ViewPreset preset = activeCamera.getViewPreset();
        switch (preset) {
            case ViewPreset::Top:
            case ViewPreset::Bottom:
                planeNormal = glm::vec3(0, 1, 0);  // XZ plane
                planeD = 0.0f;
                break;
            case ViewPreset::Front:
            case ViewPreset::Back:
                planeNormal = glm::vec3(0, 0, 1);  // XY plane
                planeD = 0.0f;
                break;
            case ViewPreset::Right:
            case ViewPreset::Left:
                planeNormal = glm::vec3(1, 0, 0);  // YZ plane
                planeD = 0.0f;
                break;
            default:
                // Perspective: use Y=0 ground plane
                planeNormal = glm::vec3(0, 1, 0);
                planeD = 0.0f;
                break;
        }
    };

    // Viewport-aware worldToScreen (matches drawPathTubeOverlay exactly)
    auto ext = m_ctx.swapchain.getExtent();
    float screenW = static_cast<float>(ext.width);
    float screenH = static_cast<float>(ext.height);
    float vpX = 0, vpY = 0, vpW = screenW, vpH = screenH;
    if (m_ctx.splitView) {
        if (m_ctx.activeViewportLeft) {
            vpW = screenW / 2.0f;
        } else {
            vpX = screenW / 2.0f;
            vpW = screenW / 2.0f;
        }
    }
    float vpAspect = vpW / vpH;
    glm::mat4 pickVP = activeCamera.getProjectionMatrix(vpAspect) * activeCamera.getViewMatrix();

    auto worldToScreen = [&](const glm::vec3& worldPos) -> glm::vec2 {
        glm::vec4 clip = pickVP * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return glm::vec2(-10000.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW,
                         vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    ImVec2 mPos = ImGui::GetMousePos();
    glm::vec2 mouseScreen(mPos.x, mPos.y);

    // G key: grab nearest node — press G, node follows mouse, LMB confirms, ESC cancels
    if (!m_pathDragging && Input::isKeyPressed(Input::KEY_G) && !m_pathNodes.empty()) {
        float bestDist = 30.0f;
        int bestIdx = -1;

        for (int i = 0; i < static_cast<int>(m_pathNodes.size()); ++i) {
            glm::vec2 sp = worldToScreen(m_pathNodes[i]);
            float dist = glm::length(sp - mouseScreen);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }

        if (bestIdx >= 0) {
            m_pathDragging = true;
            m_pathDragNodeIdx = bestIdx;
            m_pathDragOrigPos = m_pathNodes[bestIdx];
            m_pathSelectedNode = bestIdx;
            std::cout << "[PathTube] Grab node " << bestIdx << " — move mouse, LMB to confirm, ESC to cancel" << std::endl;
        }
    }

    // During drag: node follows mouse on placement plane
    if (m_pathDragging) {
        glm::vec3 rayOrigin, rayDir;
        m_ctx.getMouseRay(rayOrigin, rayDir);

        glm::vec3 planeNormal;
        float planeD;
        getPlacementPlane(planeNormal, planeD);

        // Use the plane through the original drag position
        float d = -glm::dot(planeNormal, m_pathDragOrigPos);
        glm::vec3 hitPos;
        if (intersectPlane(rayOrigin, rayDir, planeNormal, d, hitPos)) {
            m_pathNodes[m_pathDragNodeIdx] = hitPos;
        }

        // LMB confirms grab
        if (Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
            m_pathDragging = false;
            std::cout << "[PathTube] Confirmed grab" << std::endl;
        }

        // ESC cancels grab
        if (Input::isKeyPressed(Input::KEY_ESCAPE)) {
            m_pathNodes[m_pathDragNodeIdx] = m_pathDragOrigPos;
            m_pathDragging = false;
            std::cout << "[PathTube] Cancelled grab" << std::endl;
        }
        return;
    }

    // LMB click: select existing node (20px threshold) or place new node
    if (Input::isMouseButtonPressed(Input::MOUSE_LEFT)) {
        // Check if clicking near an existing node
        float bestDist = 20.0f;
        int bestIdx = -1;
        for (int i = 0; i < static_cast<int>(m_pathNodes.size()); ++i) {
            glm::vec2 sp = worldToScreen(m_pathNodes[i]);
            float dist = glm::length(sp - mouseScreen);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }

        if (bestIdx >= 0) {
            // Select existing node
            m_pathSelectedNode = bestIdx;
            std::cout << "[PathTube] Selected node " << bestIdx << std::endl;
        } else {
            // Place new node
            glm::vec3 rayOrigin, rayDir;
            m_ctx.getMouseRay(rayOrigin, rayDir);

            // First node: try to snap to live mesh surface
            if (m_pathNodes.empty() && m_retopologyLiveObj) {
                auto hit = m_retopologyLiveObj->raycast(rayOrigin, rayDir);
                if (hit.hit) {
                    m_pathNodes.push_back(hit.position);
                    m_pathTubeAttachNormal = hit.normal;
                    m_pathTubeAttached = true;
                    m_pathSelectedNode = 0;
                    std::cout << "[PathTube] Attached node 1 to surface '"
                              << m_retopologyLiveObj->getName()
                              << "' at (" << hit.position.x << ", " << hit.position.y << ", " << hit.position.z
                              << ") normal (" << hit.normal.x << ", " << hit.normal.y << ", " << hit.normal.z << ")" << std::endl;
                    // Skip plane intersection — attached to surface
                } else {
                    goto plane_fallback;
                }
            } else {
                plane_fallback:
                // Standard plane intersection for all other nodes (or if raycast missed)
                glm::vec3 planeNormal;
                float planeD;
                getPlacementPlane(planeNormal, planeD);

                glm::vec3 hitPos;
                if (intersectPlane(rayOrigin, rayDir, planeNormal, planeD, hitPos)) {
                    m_pathNodes.push_back(hitPos);
                    m_pathSelectedNode = static_cast<int>(m_pathNodes.size()) - 1;
                    std::cout << "[PathTube] Placed node " << m_pathNodes.size()
                              << " at (" << hitPos.x << ", " << hitPos.y << ", " << hitPos.z << ")" << std::endl;
                }
            }
        }
    }
}

// --- 2D overlay ---

void ModelingMode::drawPathTubeOverlay(float vpX, float vpY, float vpW, float vpH) {
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

    // Draw connecting lines between nodes
    if (m_pathNodes.size() >= 2) {
        ImU32 lineColor = IM_COL32(100, 200, 255, 180);
        for (size_t i = 0; i < m_pathNodes.size() - 1; ++i) {
            ImVec2 a = worldToScreen(m_pathNodes[i]);
            ImVec2 b = worldToScreen(m_pathNodes[i + 1]);
            if (a.x > -500 && b.x > -500) {
                drawList->AddLine(a, b, lineColor, 2.0f);
            }
        }
    }

    // Draw surface normal indicator for attached first node
    if (m_pathTubeAttached && !m_pathNodes.empty()) {
        ImVec2 basePos = worldToScreen(m_pathNodes[0]);
        glm::vec3 normalTip = m_pathNodes[0] + m_pathTubeAttachNormal * 0.15f;
        ImVec2 tipPos = worldToScreen(normalTip);
        if (basePos.x > -500 && tipPos.x > -500) {
            drawList->AddLine(basePos, tipPos, IM_COL32(0, 255, 100, 255), 2.5f);
            drawList->AddCircleFilled(tipPos, 4.0f, IM_COL32(0, 255, 100, 255));
        }
    }

    // Draw node circles
    for (size_t i = 0; i < m_pathNodes.size(); ++i) {
        ImVec2 screenPos = worldToScreen(m_pathNodes[i]);
        if (screenPos.x <= -500) continue;

        bool isSelected = (static_cast<int>(i) == m_pathSelectedNode);
        ImU32 fillColor = isSelected ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 200, 255, 255);
        float radius = isSelected ? 10.0f : 7.0f;

        drawList->AddCircleFilled(screenPos, radius, fillColor);
        drawList->AddCircle(screenPos, radius, IM_COL32(0, 0, 0, 255), 0, 1.5f);

        // Draw number
        char numStr[8];
        snprintf(numStr, sizeof(numStr), "%zu", i + 1);
        ImVec2 textSize = ImGui::CalcTextSize(numStr);
        drawList->AddText(ImVec2(screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f),
                         IM_COL32(0, 0, 0, 255), numStr);
    }

    drawList->PopClipRect();
}

// --- 3D preview ---

void ModelingMode::renderPathTubePreview3D(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (m_pathNodes.size() < 2) return;

    auto samples = pathSampleSpline(m_pathNodes, m_pathTubeSamplesPerSpan);
    if (samples.size() < 2) return;

    // Draw spline as cyan line segments
    {
        std::vector<glm::vec3> splineLines;
        splineLines.reserve((samples.size() - 1) * 2);
        for (size_t i = 0; i < samples.size() - 1; ++i) {
            splineLines.push_back(samples[i]);
            splineLines.push_back(samples[i + 1]);
        }
        m_ctx.modelRenderer.renderLines(cmd, viewProj, splineLines, glm::vec3(0.0f, 0.8f, 1.0f));
    }

    // Draw tube wireframe preview using parallel transport frames
    const float twoPi = glm::two_pi<float>();
    int segments = m_pathTubeSegments;

    // Compute frames and ring positions
    glm::vec3 prevNormal(0, 1, 0);
    std::vector<std::vector<glm::vec3>> rings(samples.size());

    for (size_t i = 0; i < samples.size(); ++i) {
        glm::vec3 tangent;
        if (i == 0) {
            tangent = glm::normalize(samples[1] - samples[0]);
        } else if (i == samples.size() - 1) {
            tangent = glm::normalize(samples[i] - samples[i - 1]);
        } else {
            tangent = glm::normalize(samples[i + 1] - samples[i - 1]);
        }

        glm::vec3 normal;
        if (i == 0 && m_pathTubeAttached) {
            normal = m_pathTubeAttachNormal - glm::dot(m_pathTubeAttachNormal, tangent) * tangent;
            if (glm::length(normal) < 0.001f) {
                normal = glm::abs(tangent.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                normal = normal - glm::dot(normal, tangent) * tangent;
            }
        } else {
            normal = prevNormal - glm::dot(prevNormal, tangent) * tangent;
            if (glm::length(normal) < 0.001f) {
                normal = glm::abs(tangent.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                normal = normal - glm::dot(normal, tangent) * tangent;
            }
        }
        normal = glm::normalize(normal);
        prevNormal = normal;

        glm::vec3 binormal = glm::cross(tangent, normal);

        // Taper: interpolate radius along the path
        float pathT = static_cast<float>(i) / (samples.size() - 1);
        float taperMult = glm::mix(m_pathTubeRadiusStart, m_pathTubeRadiusEnd, pathT);
        float r = m_pathTubeRadius * taperMult;

        // Blend factor: custom profile → circle based on profileExtent
        float profileBlend = 1.0f;  // 1 = full custom profile, 0 = circle
        if (m_pathTubeProfileExtent < 1.0f) {
            if (pathT <= m_pathTubeProfileExtent) {
                profileBlend = 1.0f;
            } else if (m_pathTubeProfileExtent <= 0.0f) {
                profileBlend = 0.0f;
            } else {
                // Smooth transition over 10% of tube length after the extent point
                float fadeZone = 0.1f;
                float fadeT = (pathT - m_pathTubeProfileExtent) / fadeZone;
                profileBlend = 1.0f - glm::clamp(fadeT, 0.0f, 1.0f);
            }
        }

        rings[i].resize(segments);
        for (int j = 0; j < segments; ++j) {
            float angle = (static_cast<float>(j) / segments) * twoPi;
            glm::vec2 circle(std::cos(angle), std::sin(angle));
            glm::vec2 prof = (j < static_cast<int>(m_pathTubeProfile.size()))
                ? m_pathTubeProfile[j] : circle;
            glm::vec2 blended = glm::mix(circle, prof, profileBlend);
            glm::vec3 offset = (normal * blended.y + binormal * blended.x) * r;
            rings[i][j] = samples[i] + offset;
        }

        // Snap first ring vertices onto live mesh surface
        if (i == 0 && m_pathTubeAttached && m_retopologyLiveObj) {
            for (int j = 0; j < segments; ++j) {
                // Cast ray from above the surface (along normal) down toward it
                glm::vec3 rayOrigin = rings[0][j] + m_pathTubeAttachNormal * m_pathTubeRadius * 2.0f;
                glm::vec3 rayDir = -m_pathTubeAttachNormal;
                auto hit = m_retopologyLiveObj->raycast(rayOrigin, rayDir);
                if (hit.hit) {
                    rings[0][j] = hit.position;
                }
            }
        }
    }

    // Draw ring outlines every few samples + 4 longitudinal lines
    std::vector<glm::vec3> wireLines;

    // Ring outlines (every 4th sample, plus first and last)
    for (size_t i = 0; i < rings.size(); i += 4) {
        for (int j = 0; j < segments; ++j) {
            wireLines.push_back(rings[i][j]);
            wireLines.push_back(rings[i][(j + 1) % segments]);
        }
    }
    // Always draw last ring
    if ((rings.size() - 1) % 4 != 0) {
        size_t last = rings.size() - 1;
        for (int j = 0; j < segments; ++j) {
            wireLines.push_back(rings[last][j]);
            wireLines.push_back(rings[last][(j + 1) % segments]);
        }
    }

    // 4 longitudinal lines (at segment indices 0, seg/4, seg/2, 3*seg/4)
    for (int k = 0; k < 4; ++k) {
        int j = (k * segments) / 4;
        for (size_t i = 0; i < rings.size() - 1; ++i) {
            wireLines.push_back(rings[i][j]);
            wireLines.push_back(rings[i + 1][j]);
        }
    }

    if (!wireLines.empty()) {
        m_ctx.modelRenderer.renderLines(cmd, viewProj, wireLines, glm::vec3(0.5f, 0.5f, 0.5f));
    }
}

// --- Mesh generation ---

void ModelingMode::generatePathTubeMesh() {
    if (m_pathNodes.size() < 2) {
        std::cout << "[PathTube] Need at least 2 nodes" << std::endl;
        return;
    }

    auto samples = pathSampleSpline(m_pathNodes, m_pathTubeSamplesPerSpan);
    if (samples.size() < 2) {
        std::cout << "[PathTube] Spline sampling failed" << std::endl;
        return;
    }

    int segments = m_pathTubeSegments;
    const float twoPi = glm::two_pi<float>();

    // Build editable mesh
    EditableMesh tubeMesh;

    // Parallel transport frames + ring vertices
    glm::vec3 prevNormal(0, 1, 0);

    // Track ring start indices for quad connectivity
    std::vector<uint32_t> ringStartIndices;

    for (size_t i = 0; i < samples.size(); ++i) {
        glm::vec3 tangent;
        if (i == 0) {
            tangent = glm::normalize(samples[1] - samples[0]);
        } else if (i == samples.size() - 1) {
            tangent = glm::normalize(samples[i] - samples[i - 1]);
        } else {
            tangent = glm::normalize(samples[i + 1] - samples[i - 1]);
        }

        glm::vec3 normal;
        if (i == 0 && m_pathTubeAttached) {
            // First ring: orient perpendicular to surface normal for flush fit
            // Use the surface normal as the frame normal, then orthogonalize
            normal = m_pathTubeAttachNormal - glm::dot(m_pathTubeAttachNormal, tangent) * tangent;
            if (glm::length(normal) < 0.001f) {
                // Surface normal parallel to tangent — fall back to default
                normal = glm::abs(tangent.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                normal = normal - glm::dot(normal, tangent) * tangent;
            }
        } else {
            normal = prevNormal - glm::dot(prevNormal, tangent) * tangent;
            if (glm::length(normal) < 0.001f) {
                normal = glm::abs(tangent.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                normal = normal - glm::dot(normal, tangent) * tangent;
            }
        }
        normal = glm::normalize(normal);
        prevNormal = normal;

        glm::vec3 binormal = glm::cross(tangent, normal);

        // Taper
        float pathT = static_cast<float>(i) / (samples.size() - 1);
        float taperMult = glm::mix(m_pathTubeRadiusStart, m_pathTubeRadiusEnd, pathT);
        float r = m_pathTubeRadius * taperMult;

        // Blend factor: custom profile → circle based on profileExtent
        float profileBlend = 1.0f;
        if (m_pathTubeProfileExtent < 1.0f) {
            if (pathT <= m_pathTubeProfileExtent) {
                profileBlend = 1.0f;
            } else if (m_pathTubeProfileExtent <= 0.0f) {
                profileBlend = 0.0f;
            } else {
                float fadeZone = 0.1f;
                float fadeT = (pathT - m_pathTubeProfileExtent) / fadeZone;
                profileBlend = 1.0f - glm::clamp(fadeT, 0.0f, 1.0f);
            }
        }

        uint32_t ringStart = static_cast<uint32_t>(tubeMesh.getVertexCount());
        ringStartIndices.push_back(ringStart);

        for (int j = 0; j < segments; ++j) {
            float angle = (static_cast<float>(j) / segments) * twoPi;
            glm::vec2 circle(std::cos(angle), std::sin(angle));
            glm::vec2 prof = (j < static_cast<int>(m_pathTubeProfile.size()))
                ? m_pathTubeProfile[j] : circle;
            glm::vec2 blended = glm::mix(circle, prof, profileBlend);

            glm::vec3 offset = (normal * blended.y + binormal * blended.x) * r;
            glm::vec3 pos = samples[i] + offset;

            // Compute outward normal from blended profile (finite difference)
            glm::vec2 prevCircle(std::cos(((j + segments - 1) % segments) / (float)segments * twoPi),
                                 std::sin(((j + segments - 1) % segments) / (float)segments * twoPi));
            glm::vec2 nextCircle(std::cos(((j + 1) % segments) / (float)segments * twoPi),
                                 std::sin(((j + 1) % segments) / (float)segments * twoPi));
            glm::vec2 prevProf = m_pathTubeProfile.empty() ? prevCircle
                : glm::mix(prevCircle, m_pathTubeProfile[(j + segments - 1) % segments], profileBlend);
            glm::vec2 nextProf = m_pathTubeProfile.empty() ? nextCircle
                : glm::mix(nextCircle, m_pathTubeProfile[(j + 1) % segments], profileBlend);
            glm::vec2 tangent2D = nextProf - prevProf;
            float tangentLen = glm::length(tangent2D);
            glm::vec3 norm;
            if (tangentLen > 1e-6f) {
                tangent2D /= tangentLen;
                glm::vec2 outward2D(-tangent2D.y, tangent2D.x);
                norm = glm::normalize(normal * outward2D.y + binormal * outward2D.x);
            } else {
                norm = glm::normalize(offset);
            }

            float u = static_cast<float>(j) / segments;
            float v = static_cast<float>(i) / (samples.size() - 1);

            // Snap first ring vertices onto live mesh surface
            if (i == 0 && m_pathTubeAttached && m_retopologyLiveObj) {
                glm::vec3 rayOrigin = pos + m_pathTubeAttachNormal * m_pathTubeRadius * 2.0f;
                glm::vec3 rayDir = -m_pathTubeAttachNormal;
                auto hit = m_retopologyLiveObj->raycast(rayOrigin, rayDir);
                if (hit.hit) {
                    pos = hit.position;
                    norm = hit.normal;
                }
            }

            HEVertex vert;
            vert.position = pos;
            vert.normal = norm;
            vert.color = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
            vert.halfEdgeIndex = UINT32_MAX;
            vert.selected = false;
            tubeMesh.addVertex(vert);
        }
    }

    // Connect adjacent rings with quads
    std::vector<std::array<uint32_t, 4>> quadFaces;
    for (size_t i = 0; i < samples.size() - 1; ++i) {
        uint32_t ring = ringStartIndices[i];
        uint32_t nextRing = ringStartIndices[i + 1];

        for (int j = 0; j < segments; ++j) {
            uint32_t j0 = static_cast<uint32_t>(j);
            uint32_t j1 = static_cast<uint32_t>((j + 1) % segments);

            // Winding: reversed for correct outward-facing normals
            std::array<uint32_t, 4> quad = {
                nextRing + j0,
                nextRing + j1,
                ring + j1,
                ring + j0
            };
            quadFaces.push_back(quad);
        }
    }

    tubeMesh.addQuadFacesBatch(quadFaces);

    // --- Create scene object (pattern from finalizeRetopologyMesh) ---

    // Find or create the path tube scene object
    static int tubeCounter = 0;
    std::string tubeName = "path_tube_" + std::to_string(++tubeCounter);
    auto newObj = std::make_unique<SceneObject>(tubeName);
    newObj->setDescription("Path tube mesh");
    SceneObject* tubeObj = newObj.get();
    m_ctx.sceneObjects.push_back(std::move(newObj));

    // Triangulate for GPU
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::set<uint32_t> noHidden;
    tubeMesh.triangulate(vertices, indices, noHidden);

    if (indices.empty()) {
        std::cout << "[PathTube] Triangulation produced no geometry" << std::endl;
        return;
    }

    // Create GPU model
    uint32_t newHandle = m_ctx.modelRenderer.createModel(vertices, indices, nullptr, 0, 0);
    tubeObj->setBufferHandle(newHandle);
    tubeObj->setIndexCount(static_cast<uint32_t>(indices.size()));
    tubeObj->setVertexCount(static_cast<uint32_t>(vertices.size()));
    tubeObj->setMeshData(vertices, indices);
    tubeObj->setVisible(true);

    // Store half-edge data on the scene object
    const auto& heVerts = tubeMesh.getVerticesData();
    const auto& heHalfEdges = tubeMesh.getHalfEdges();
    const auto& heFaces = tubeMesh.getFacesData();

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

    tubeObj->setEditableMeshData(storedVerts, storedHE, storedFaces);

    // Compute local bounds
    AABB bounds;
    bounds.min = glm::vec3(INFINITY);
    bounds.max = glm::vec3(-INFINITY);
    for (const auto& v : vertices) {
        bounds.min = glm::min(bounds.min, v.position);
        bounds.max = glm::max(bounds.max, v.position);
    }
    tubeObj->setLocalBounds(bounds);

    // Select the tube object and load its mesh into the editor
    m_ctx.selectedObject = tubeObj;
    m_ctx.editableMesh = tubeMesh;
    m_ctx.meshDirty = false;

    // Build faceToTriangles mapping
    m_ctx.faceToTriangles.clear();
    uint32_t triIndex = 0;
    for (uint32_t faceIdx = 0; faceIdx < m_ctx.editableMesh.getFaceCount(); ++faceIdx) {
        uint32_t vertCount = m_ctx.editableMesh.getFace(faceIdx).vertexCount;
        uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
        for (uint32_t ti = 0; ti < triCount; ++ti) {
            m_ctx.faceToTriangles[faceIdx].push_back(triIndex++);
        }
    }

    // Clear stale selection state
    m_ctx.selectedFaces.clear();
    m_ctx.hiddenFaces.clear();
    invalidateWireframeCache();

    // Clear path state and exit mode
    m_pathNodes.clear();
    m_pathSelectedNode = -1;
    m_pathTubeMode = false;
    m_pathTubeAttached = false;

    std::cout << "[PathTube] Generated tube: " << tubeMesh.getFaceCount()
              << " faces, " << tubeMesh.getVertexCount() << " vertices, "
              << indices.size() / 3 << " triangles" << std::endl;
}

// --- Cancel ---

void ModelingMode::cancelPathTubeMode() {
    m_pathTubeMode = false;
    m_pathNodes.clear();
    m_pathSelectedNode = -1;
    m_pathDragging = false;
    m_pathDragNodeIdx = -1;
    m_pathTubeAttached = false;
    std::cout << "[PathTube] Mode cancelled" << std::endl;
}
