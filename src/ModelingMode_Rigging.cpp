#include "ModelingMode.hpp"
#include "EditableMesh.hpp"
#include <imgui.h>
#include <iostream>

using namespace eden;

void ModelingMode::cancelRiggingMode() {
    m_riggingMode = false;
    m_selectedBone = -1;
    m_placingBone = false;
    m_showSkeleton = false;
}

int ModelingMode::pickBoneAtScreenPos(const glm::vec2& screenPos, float threshold) {
    if (m_bonePositions.empty() || !m_ctx.editableMesh.isValid()) return -1;

    Camera& activeCamera = m_ctx.getActiveCamera();
    float vpW = static_cast<float>(m_ctx.window.getWidth());
    float vpH = static_cast<float>(m_ctx.window.getHeight());
    glm::mat4 view = activeCamera.getViewMatrix();
    glm::mat4 proj = activeCamera.getProjectionMatrix(vpW / vpH);
    glm::mat4 vp = proj * view;

    glm::mat4 modelMatrix(1.0f);
    if (m_ctx.selectedObject) {
        modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
    }

    float closestDist = threshold;
    int closestBone = -1;

    for (int i = 0; i < static_cast<int>(m_bonePositions.size()); ++i) {
        glm::vec4 clip = vp * modelMatrix * glm::vec4(m_bonePositions[i], 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 screen((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);

        float dist = glm::length(screen - screenPos);
        if (dist < closestDist) {
            closestDist = dist;
            closestBone = i;
        }
    }

    return closestBone;
}

std::vector<int> ModelingMode::getDescendantBones(int boneIdx) {
    std::vector<int> result;
    const auto& skel = m_ctx.editableMesh.getSkeleton();
    // BFS to find all descendants
    std::vector<int> queue = {boneIdx};
    while (!queue.empty()) {
        int current = queue.back();
        queue.pop_back();
        for (int i = 0; i < static_cast<int>(skel.bones.size()); ++i) {
            if (skel.bones[i].parentIndex == current) {
                result.push_back(i);
                queue.push_back(i);
            }
        }
    }
    return result;
}

void ModelingMode::drawSkeletonOverlay(float vpX, float vpY, float vpW, float vpH) {
    if (!m_showSkeleton || !m_ctx.editableMesh.isValid()) return;

    const auto& skel = m_ctx.editableMesh.getSkeleton();
    if (skel.bones.empty() || m_bonePositions.empty()) return;

    Camera& activeCamera = (m_ctx.splitView && vpX > 0) ? m_ctx.camera2 : m_ctx.camera;
    glm::mat4 view = activeCamera.getViewMatrix();
    float aspectRatio = vpW / vpH;
    glm::mat4 proj = activeCamera.getProjectionMatrix(aspectRatio);
    glm::mat4 vp = proj * view;

    // Get model matrix for the selected object
    glm::mat4 modelMatrix(1.0f);
    if (m_ctx.selectedObject) {
        modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
    }

    auto worldToScreen = [&](const glm::vec3& worldPos) -> ImVec2 {
        glm::vec4 clip = vp * modelMatrix * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    int numBones = static_cast<int>(skel.bones.size());

    for (int i = 0; i < numBones && i < static_cast<int>(m_bonePositions.size()); ++i) {
        glm::vec3 headPos = m_bonePositions[i];
        ImVec2 headScreen = worldToScreen(headPos);

        bool isSelected = (m_selectedBone == i);

        // Draw bone line from head to parent head
        int parentIdx = skel.bones[i].parentIndex;
        if (parentIdx >= 0 && parentIdx < static_cast<int>(m_bonePositions.size())) {
            glm::vec3 parentPos = m_bonePositions[parentIdx];
            ImVec2 parentScreen = worldToScreen(parentPos);

            ImU32 lineColor = isSelected ? IM_COL32(255, 200, 50, 255) : IM_COL32(200, 200, 200, 200);
            float lineThick = isSelected ? 3.0f : 2.0f;

            if (headScreen.x > -500 && parentScreen.x > -500) {
                drawList->AddLine(headScreen, parentScreen, lineColor, lineThick);
            }
        }

        // Draw joint marker (circle at bone head)
        if (headScreen.x > -500) {
            float radius = isSelected ? 6.0f : 4.0f;
            ImU32 markerColor = isSelected ? IM_COL32(255, 255, 50, 255) : IM_COL32(100, 200, 255, 230);
            ImU32 outlineColor = IM_COL32(0, 0, 0, 200);
            drawList->AddCircleFilled(headScreen, radius + 1.0f, outlineColor);
            drawList->AddCircleFilled(headScreen, radius, markerColor);

            // Draw bone name
            if (m_showBoneNames && (isSelected || numBones <= 20)) {
                drawList->AddText(ImVec2(headScreen.x + radius + 3, headScreen.y - 7),
                                  isSelected ? IM_COL32(255, 255, 100, 255) : IM_COL32(200, 200, 200, 180),
                                  skel.bones[i].name.c_str());
            }
        }
    }

    drawList->PopClipRect();
}
