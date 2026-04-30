#include "AnimationMode.hpp"
#include "Editor/SkinnedGLBLoader.hpp"

#include <nfd.h>
#include <imgui.h>
#include <tiny_gltf.h>

#include <iostream>
#include <algorithm>

using namespace eden;

AnimationMode::AnimationMode(EditorContext& ctx)
    : IEditorMode(ctx)
{
}

void AnimationMode::onActivate() {
    // Nothing special to do on activation
}

void AnimationMode::onDeactivate() {
    // Nothing special to do on deactivation
}

void AnimationMode::processInput(float deltaTime) {
    bool mouseOverImGui = ImGui::GetIO().WantCaptureMouse;

    // Q/W/E gizmo mode switching
    if (!ImGui::GetIO().WantTextInput) {
        if (Input::isKeyPressed(Input::KEY_Q)) m_gizmoMode = GizmoMode::None;
        if (Input::isKeyPressed(Input::KEY_W)) m_gizmoMode = GizmoMode::Move;
        if (Input::isKeyPressed(Input::KEY_E)) m_gizmoMode = GizmoMode::Rotate;
    }

    // RMB tumble (orbit) — same as modeling mode
    if (!mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_RIGHT)) {
        Camera& cam = m_ctx.getActiveCamera();
        m_ctx.isTumbling = true;
        glm::vec3 offset = cam.getPosition() - m_ctx.orbitTarget;
        float dist = glm::length(offset);
        if (dist > 0.001f) {
            offset = glm::normalize(offset);
            m_ctx.orbitPitch = glm::degrees(asin(glm::clamp(offset.y, -1.0f, 1.0f)));
            m_ctx.orbitYaw = glm::degrees(atan2(offset.z, offset.x));
        }
    }

    // LMB click to select bone in viewport
    if (!mouseOverImGui && Input::isMouseButtonPressed(Input::MOUSE_LEFT) && m_skinnedModelHandle != UINT32_MAX) {
        auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
        if (data && data->skeleton && !data->animPlayer.getBoneMatrices().empty()) {
            const auto& skeleton = *data->skeleton;
            const auto& boneMatrices = data->animPlayer.getBoneMatrices();
            glm::mat4 modelMatrix = getModelMatrix();

            Camera& cam = m_ctx.camera;
            float vpW = static_cast<float>(m_ctx.window.getWidth());
            float vpH = static_cast<float>(m_ctx.window.getHeight());
            glm::mat4 view = cam.getViewMatrix();
            glm::mat4 proj = cam.getProjectionMatrix(vpW / vpH);
            glm::mat4 vp = proj * view;
            glm::vec2 mousePos = Input::getMousePosition();

            float closestDist = 15.0f;
            int closestBone = -1;
            for (int i = 0; i < static_cast<int>(skeleton.bones.size()) && i < static_cast<int>(boneMatrices.size()); i++) {
                glm::mat4 wti = boneMatrices[i] * glm::inverse(skeleton.bones[i].inverseBindMatrix);
                glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(glm::vec3(wti[3]), 1.0f));
                glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
                if (clip.w <= 0.0f) continue;
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                glm::vec2 screen((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);
                float dist = glm::length(screen - mousePos);
                if (dist < closestDist) {
                    closestDist = dist;
                    closestBone = i;
                }
            }
            if (closestBone >= 0) m_selectedBone = closestBone;
        }
    }
}

void AnimationMode::update(float deltaTime) {
    if (m_skinnedModelHandle == UINT32_MAX) return;

    auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
    if (!data) return;

    if (m_scrubbing) {
        // Scrubbing: we set the time, player follows
        data->animPlayer.setCurrentTime(m_animationTime);
        const auto& boneMatrices = data->animPlayer.getBoneMatrices();
        if (!boneMatrices.empty() && data->boneMappedMemory) {
            size_t copySize = std::min(boneMatrices.size(), eden::MAX_BONES) * sizeof(glm::mat4);
            memcpy(data->boneMappedMemory, boneMatrices.data(), copySize);
        }
        m_scrubbing = false;
    } else {
        // Player is the single clock — we just read from it
        data->animPlayer.setPlaybackSpeed(m_animationPlaying ? m_animationSpeed : 0.0f);
        m_ctx.skinnedModelRenderer.updateAnimation(m_skinnedModelHandle, deltaTime);
    }

    // Always sync our timeline display from the player's actual time
    m_animationTime = data->animPlayer.getCurrentTime();
}

void AnimationMode::renderUI() {
    renderAnimationCombinerUI();
    renderTimelineWindow();
}

glm::mat4 AnimationMode::getModelMatrix() const {
    glm::mat4 modelMatrix = glm::mat4(1.0f);
    if (m_animationSource == AnimationSource::Mixamo) {
        modelMatrix = glm::rotate(modelMatrix, glm::radians(90.0f), glm::vec3(1, 0, 0));
    }
    modelMatrix = glm::scale(modelMatrix, glm::vec3(0.012f));
    return modelMatrix;
}

void AnimationMode::renderSceneOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (m_skinnedModelHandle != UINT32_MAX) {
        glm::mat4 modelMatrix = getModelMatrix();
        m_ctx.skinnedModelRenderer.render(cmd, viewProj, m_skinnedModelHandle, modelMatrix);

        // Gizmo is drawn in 2D overlay (drawOverlays) so it renders on top of the mesh
    }
}

void AnimationMode::drawOverlays(float vpX, float vpY, float vpW, float vpH) {
    if (m_skinnedModelHandle == UINT32_MAX || !m_showJoints) return;

    auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
    if (!data || !data->skeleton) return;

    const auto& skeleton = *data->skeleton;
    const auto& boneMatrices = data->animPlayer.getBoneMatrices();
    int numBones = static_cast<int>(skeleton.bones.size());
    if (numBones == 0 || boneMatrices.empty()) return;

    Camera& cam = m_ctx.camera;
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix(vpW / vpH);
    glm::mat4 vp = proj * view;
    glm::mat4 modelMatrix = getModelMatrix();

    auto worldToScreen = [&](const glm::vec3& worldPos) -> ImVec2 {
        glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    // Compute world-space bone positions
    std::vector<glm::vec3> bonePositions(numBones);
    for (int i = 0; i < numBones && i < static_cast<int>(boneMatrices.size()); i++) {
        glm::mat4 wt = boneMatrices[i] * glm::inverse(skeleton.bones[i].inverseBindMatrix);
        glm::vec4 wp = modelMatrix * glm::vec4(glm::vec3(wt[3]), 1.0f);
        bonePositions[i] = glm::vec3(wp);
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    for (int i = 0; i < numBones; i++) {
        ImVec2 sp = worldToScreen(bonePositions[i]);
        bool isSelected = (m_selectedBone == i);

        // Line to parent
        int parentIdx = skeleton.bones[i].parentIndex;
        if (parentIdx >= 0 && parentIdx < numBones) {
            ImVec2 pp = worldToScreen(bonePositions[parentIdx]);
            if (sp.x > -500 && pp.x > -500) {
                ImU32 lineColor = isSelected ? IM_COL32(255, 200, 50, 255) : IM_COL32(200, 200, 200, 150);
                drawList->AddLine(pp, sp, lineColor, isSelected ? 3.0f : 1.5f);
            }
        }

        if (sp.x <= -500) continue;

        // Joint circle
        float radius = isSelected ? 6.0f : 3.5f;
        ImU32 markerColor = isSelected ? IM_COL32(255, 255, 50, 255) : IM_COL32(100, 200, 255, 220);
        drawList->AddCircleFilled(sp, radius + 1.0f, IM_COL32(0, 0, 0, 180));
        drawList->AddCircleFilled(sp, radius, markerColor);

        // Bone name
        if (m_showBoneNames) {
            ImU32 textColor = isSelected ? IM_COL32(255, 255, 100, 255) : IM_COL32(200, 200, 200, 160);
            drawList->AddText(ImVec2(sp.x + radius + 3, sp.y - 7), textColor, skeleton.bones[i].name.c_str());
        }

        // Bone local axes (2D lines so they always render on top)
        if (m_showBoneAxes && isSelected) {
            glm::mat4 wt = boneMatrices[i] * glm::inverse(skeleton.bones[i].inverseBindMatrix);
            float axisWorld = 0.06f;
            glm::vec3 wp = bonePositions[i];
            glm::vec3 rx = glm::normalize(glm::vec3(modelMatrix * wt * glm::vec4(1, 0, 0, 0)));
            glm::vec3 ry = glm::normalize(glm::vec3(modelMatrix * wt * glm::vec4(0, 1, 0, 0)));
            glm::vec3 rz = glm::normalize(glm::vec3(modelMatrix * wt * glm::vec4(0, 0, 1, 0)));
            ImVec2 xEnd = worldToScreen(wp + rx * axisWorld);
            ImVec2 yEnd = worldToScreen(wp + ry * axisWorld);
            ImVec2 zEnd = worldToScreen(wp + rz * axisWorld);
            if (xEnd.x > -500) drawList->AddLine(sp, xEnd, IM_COL32(255, 50, 50, 255), 2.5f);
            if (yEnd.x > -500) drawList->AddLine(sp, yEnd, IM_COL32(50, 255, 50, 255), 2.5f);
            if (zEnd.x > -500) drawList->AddLine(sp, zEnd, IM_COL32(50, 50, 255, 255), 2.5f);
        }
    }

    // Gizmo at selected bone (2D so it renders on top)
    if (m_gizmoMode != GizmoMode::None && m_selectedBone >= 0 && m_selectedBone < numBones) {
        ImVec2 center = worldToScreen(bonePositions[m_selectedBone]);
        if (center.x > -500) {
            float gizmoLen = 60.0f; // pixels
            ImU32 xCol = (m_gizmoActiveAxis == GizmoAxis::X) ? IM_COL32(255, 255, 50, 255) : IM_COL32(255, 50, 50, 255);
            ImU32 yCol = (m_gizmoActiveAxis == GizmoAxis::Y) ? IM_COL32(255, 255, 50, 255) : IM_COL32(50, 255, 50, 255);
            ImU32 zCol = (m_gizmoActiveAxis == GizmoAxis::Z) ? IM_COL32(255, 255, 50, 255) : IM_COL32(80, 80, 255, 255);

            if (m_gizmoMode == GizmoMode::Move) {
                // Project world axes to screen for direction
                glm::vec3 wp = bonePositions[m_selectedBone];
                ImVec2 xEnd = worldToScreen(wp + glm::vec3(0.03f, 0, 0));
                ImVec2 yEnd = worldToScreen(wp + glm::vec3(0, 0.03f, 0));
                ImVec2 zEnd = worldToScreen(wp + glm::vec3(0, 0, 0.03f));

                // Normalize screen directions and scale to gizmoLen
                auto drawAxis = [&](ImVec2 end, ImU32 col) {
                    float dx = end.x - center.x, dy = end.y - center.y;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1.0f) return;
                    dx /= len; dy /= len;
                    ImVec2 tip(center.x + dx * gizmoLen, center.y + dy * gizmoLen);
                    drawList->AddLine(center, tip, col, 2.5f);
                    // Arrow head
                    float px = -dy, py = dx; // perpendicular
                    ImVec2 arrowBase(center.x + dx * gizmoLen * 0.8f, center.y + dy * gizmoLen * 0.8f);
                    drawList->AddTriangleFilled(
                        tip,
                        ImVec2(arrowBase.x + px * 5, arrowBase.y + py * 5),
                        ImVec2(arrowBase.x - px * 5, arrowBase.y - py * 5),
                        col);
                };
                drawAxis(xEnd, xCol);
                drawAxis(yEnd, yCol);
                drawAxis(zEnd, zCol);

            } else if (m_gizmoMode == GizmoMode::Rotate) {
                // Draw circles in screen space approximating each rotation axis
                float r = gizmoLen * 0.8f;
                auto drawCircle = [&](ImU32 col, float tiltX, float tiltY) {
                    int segments = 48;
                    for (int s = 0; s < segments; s++) {
                        float a1 = (float)s / segments * 6.2831853f;
                        float a2 = (float)(s + 1) / segments * 6.2831853f;
                        ImVec2 p1(center.x + std::cos(a1) * r * tiltX, center.y + std::sin(a1) * r * tiltY);
                        ImVec2 p2(center.x + std::cos(a2) * r * tiltX, center.y + std::sin(a2) * r * tiltY);
                        drawList->AddLine(p1, p2, col, 2.0f);
                    }
                };
                // X rotation = circle in YZ plane (squashed horizontally)
                drawCircle(xCol, 0.4f, 1.0f);
                // Y rotation = circle in XZ plane (squashed vertically)
                drawCircle(yCol, 1.0f, 0.4f);
                // Z rotation = circle in XY plane (full circle)
                drawCircle(zCol, 1.0f, 1.0f);
            }
        }
    }

    drawList->PopClipRect();
}

void AnimationMode::renderAnimationCombinerUI() {
    ImGui::SetNextWindowPos(ImVec2(0, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 500), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Animation")) {
        // Base model section
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Base Model");
        ImGui::Separator();

        if (m_skinnedModelHandle == UINT32_MAX) {
            ImGui::TextDisabled("No model loaded");
            if (ImGui::Button("Load Base Model...", ImVec2(-1, 0))) {
                openSkinnedModelDialog();
            }
        } else {
            size_t lastSlash = m_baseModelPath.find_last_of("/\\");
            std::string filename = (lastSlash != std::string::npos) ?
                m_baseModelPath.substr(lastSlash + 1) : m_baseModelPath;
            ImGui::Text("Model: %s", filename.c_str());
            ImGui::Text("Bones: %zu", m_boneNames.size());

            // Show bone prefix
            if (!m_boneNames.empty()) {
                std::string prefix = detectBonePrefix(m_boneNames[0]);
                ImGui::Text("Bone prefix: %s", prefix.c_str());
            }

            // Animation source selector
            ImGui::Spacing();
            ImGui::Text("Source:");
            ImGui::SameLine();
            bool isMixamo = (m_animationSource == AnimationSource::Mixamo);
            bool isMeshy = (m_animationSource == AnimationSource::Meshy);
            if (isMixamo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
            if (ImGui::Button("Mixamo")) m_animationSource = AnimationSource::Mixamo;
            if (isMixamo) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (isMeshy) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
            if (ImGui::Button("Meshy")) m_animationSource = AnimationSource::Meshy;
            if (isMeshy) ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Animations section
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Animations");
        ImGui::Separator();

        if (ImGui::Button("Add Animation...", ImVec2(-1, 0))) {
            addAnimationDialog();
        }

        ImGui::Spacing();

        if (m_animations.empty()) {
            ImGui::TextDisabled("No animations loaded");
        } else {
            for (size_t i = 0; i < m_animations.size(); i++) {
                auto& anim = m_animations[i];
                ImGui::PushID(static_cast<int>(i));

                bool isSelected = (m_currentAnimationIndex == static_cast<int>(i));
                if (ImGui::Selectable(anim.name.c_str(), isSelected)) {
                    m_currentAnimationIndex = static_cast<int>(i);
                    m_animationTime = 0.0f;
                    // Play this animation
                    if (m_skinnedModelHandle != UINT32_MAX) {
                        m_ctx.skinnedModelRenderer.playAnimation(m_skinnedModelHandle, anim.name, true);
                    }
                }

                // Context menu for rename/delete
                if (ImGui::BeginPopupContextItem()) {
                    ImGui::InputText("Name", m_newAnimationName, sizeof(m_newAnimationName));
                    if (ImGui::Button("Rename")) {
                        if (strlen(m_newAnimationName) > 0) {
                            anim.name = m_newAnimationName;
                            m_newAnimationName[0] = '\0';
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Delete")) {
                        m_animations.erase(m_animations.begin() + i);
                        if (m_currentAnimationIndex >= static_cast<int>(m_animations.size())) {
                            m_currentAnimationIndex = static_cast<int>(m_animations.size()) - 1;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                // Show duration
                ImGui::SameLine(200);
                ImGui::TextDisabled("%.1fs", anim.clip.duration);

                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Playback controls (minimal — timeline window has the full controls)
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Playback");
        ImGui::Separator();

        if (m_currentAnimationIndex >= 0 && m_currentAnimationIndex < static_cast<int>(m_animations.size())) {
            const auto& clip = m_animations[m_currentAnimationIndex].clip;
            ImGui::SliderFloat("Speed", &m_animationSpeed, 0.1f, 2.0f, "%.1fx");
        } else {
            ImGui::TextDisabled("Select an animation to preview");
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Keyframe tools
        if (m_currentAnimationIndex >= 0 && m_currentAnimationIndex < static_cast<int>(m_animations.size())) {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Keyframe Tools");
            ImGui::Separator();
            ImGui::SliderInt("Keep every N", &m_thinInterval, 2, 10);

            auto& clip = m_animations[m_currentAnimationIndex].clip;
            // Count total keys for display
            int totalKeys = 0;
            for (const auto& ch : clip.channels) {
                totalKeys += static_cast<int>(ch.positions.size() + ch.rotations.size() + ch.scales.size());
            }
            int afterKeys = 0;
            for (const auto& ch : clip.channels) {
                afterKeys += static_cast<int>((ch.positions.size() + m_thinInterval - 1) / m_thinInterval);
                afterKeys += static_cast<int>((ch.rotations.size() + m_thinInterval - 1) / m_thinInterval);
                afterKeys += static_cast<int>((ch.scales.size() + m_thinInterval - 1) / m_thinInterval);
            }
            ImGui::Text("%d keys -> %d keys", totalKeys, afterKeys);

            if (ImGui::Button("Thin Keyframes", ImVec2(-1, 0))) {
                // Thin every channel in the current animation
                for (auto& ch : clip.channels) {
                    auto thinChannel = [&](std::vector<float>& times, auto& values) {
                        if (times.size() <= 1) return;
                        std::vector<float> newTimes;
                        std::remove_reference_t<decltype(values)> newValues;
                        for (size_t i = 0; i < times.size(); i++) {
                            if (i % m_thinInterval == 0 || i == times.size() - 1) {
                                newTimes.push_back(times[i]);
                                newValues.push_back(values[i]);
                            }
                        }
                        times = newTimes;
                        values = newValues;
                    };
                    thinChannel(ch.positionTimes, ch.positions);
                    thinChannel(ch.rotationTimes, ch.rotations);
                    thinChannel(ch.scaleTimes, ch.scales);
                }

                // Update renderer's copy
                auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
                if (data) {
                    for (auto& rendererAnim : data->animations) {
                        if (rendererAnim.name == clip.name) {
                            rendererAnim = clip;
                            break;
                        }
                    }
                }
                std::cout << "[Animation] Thinned keyframes: kept every " << m_thinInterval << "th frame" << std::endl;
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Rigging section
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Rigging");
        ImGui::Separator();
        ImGui::Checkbox("Show Joints", &m_showJoints);
        ImGui::Checkbox("Show Bone Names", &m_showBoneNames);
        ImGui::Checkbox("Show Bone Axes", &m_showBoneAxes);
        ImGui::Checkbox("Weight Heat Map", &m_showWeightHeatMap);

        // Bone list
        if (m_skinnedModelHandle != UINT32_MAX) {
            auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
            if (data && data->skeleton) {
                const auto& skel = *data->skeleton;
                int numBones = static_cast<int>(skel.bones.size());
                ImGui::Text("Bones: %d", numBones);

                float listHeight = std::min(numBones * 22.0f + 4.0f, 200.0f);
                ImGui::BeginChild("##bonelist", ImVec2(0, listHeight), true);
                for (int i = 0; i < numBones; ++i) {
                    ImGui::PushID(i);
                    bool isSelected = (m_selectedBone == i);
                    std::string boneName = std::to_string(i) + ": " + skel.bones[i].name;
                    if (ImGui::Selectable(boneName.c_str(), isSelected, 0, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
                        m_selectedBone = i;
                    }
                    // Show parent inline
                    ImGui::SameLine();
                    int parentIdx = skel.bones[i].parentIndex;
                    if (parentIdx >= 0 && parentIdx < numBones) {
                        ImGui::TextDisabled("< %s", skel.bones[parentIdx].name.c_str());
                    } else {
                        ImGui::TextDisabled("< (root)");
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                // Selected bone info
                if (m_selectedBone >= 0 && m_selectedBone < numBones) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "%s", skel.bones[m_selectedBone].name.c_str());

                    // Read-only bone info from current animation frame
                    const auto& boneMatrices = data->animPlayer.getBoneMatrices();
                    if (m_selectedBone < static_cast<int>(boneMatrices.size())) {
                        glm::mat4 worldTransform = boneMatrices[m_selectedBone] * glm::inverse(skel.bones[m_selectedBone].inverseBindMatrix);
                        glm::vec3 pos = glm::vec3(worldTransform[3]);
                        ImGui::Text("Pos: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);

                        glm::mat3 rotMat = glm::mat3(worldTransform);
                        for (int c = 0; c < 3; c++) rotMat[c] = glm::normalize(rotMat[c]);
                        glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::quat_cast(rotMat)));
                        ImGui::Text("Rot: %.1f, %.1f, %.1f", euler.x, euler.y, euler.z);
                    }

                    // Parent
                    int parentIdx2 = skel.bones[m_selectedBone].parentIndex;
                    if (parentIdx2 >= 0 && parentIdx2 < numBones) {
                        ImGui::Text("Parent: %s", skel.bones[parentIdx2].name.c_str());
                    } else {
                        ImGui::Text("Parent: (root)");
                    }

                    // Children
                    std::string children;
                    for (int j = 0; j < numBones; j++) {
                        if (skel.bones[j].parentIndex == m_selectedBone) {
                            if (!children.empty()) children += ", ";
                            children += skel.bones[j].name;
                        }
                    }
                    if (!children.empty()) {
                        ImGui::TextWrapped("Children: %s", children.c_str());
                    }

                    // Keyframe info for current animation
                    if (m_currentAnimationIndex >= 0 && m_currentAnimationIndex < static_cast<int>(m_animations.size())) {
                        const auto& clip = m_animations[m_currentAnimationIndex].clip;
                        for (const auto& ch : clip.channels) {
                            if (ch.boneIndex == m_selectedBone) {
                                ImGui::Text("Keys: %zu pos, %zu rot, %zu scl",
                                    ch.positions.size(), ch.rotations.size(), ch.scales.size());
                                break;
                            }
                        }
                    }
                }

                // Weight heat map
                if (m_showWeightHeatMap && m_selectedBone >= 0 && !m_cpuVertices.empty()) {
                    if (!m_weightHeatMapActive || m_selectedBone != m_weightHeatMapBone) {
                        auto vertices = m_cpuVertices;
                        for (auto& v : vertices) {
                            float w = 0.0f;
                            for (int j = 0; j < 4; j++) {
                                if (v.joints[j] == m_selectedBone) w += v.weights[j];
                            }
                            float r = std::min(1.0f, w * 2.0f);
                            float g = w < 0.5f ? w * 2.0f : 2.0f * (1.0f - w);
                            float b = std::max(0.0f, 1.0f - w * 2.0f);
                            v.color = glm::vec4(r, g, b, 1.0f);
                        }
                        VkDeviceSize vertexSize = sizeof(eden::SkinnedVertex) * vertices.size();
                        void* mapped;
                        vkMapMemory(m_ctx.vulkanContext.getDevice(), data->vertexMemory, 0, vertexSize, 0, &mapped);
                        memcpy(mapped, vertices.data(), vertexSize);
                        vkUnmapMemory(m_ctx.vulkanContext.getDevice(), data->vertexMemory);
                        m_weightHeatMapActive = true;
                        m_weightHeatMapBone = m_selectedBone;
                    }
                } else if (m_weightHeatMapActive && !m_cpuVertices.empty()) {
                    VkDeviceSize vertexSize = sizeof(eden::SkinnedVertex) * m_cpuVertices.size();
                    void* mapped;
                    vkMapMemory(m_ctx.vulkanContext.getDevice(), data->vertexMemory, 0, vertexSize, 0, &mapped);
                    memcpy(mapped, m_cpuVertices.data(), vertexSize);
                    vkUnmapMemory(m_ctx.vulkanContext.getDevice(), data->vertexMemory);
                    m_weightHeatMapActive = false;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Export section
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Export");
        ImGui::Separator();

        bool canExport = m_skinnedModelHandle != UINT32_MAX && !m_animations.empty();
        if (!canExport) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Export Combined GLB...", ImVec2(-1, 30))) {
            exportCombinedGLB();
        }

        if (!canExport) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("Load a model and add animations first");
        }
    }
    ImGui::End();

    // Camera controls window
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(m_ctx.window.getWidth()) - 220.0f, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Camera")) {
        ImGui::SliderFloat("Speed", &m_ctx.cameraSpeed, 0.01f, 0.2f, "%.3f");
    }
    ImGui::End();
}

void AnimationMode::renderTimelineWindow() {
    if (m_currentAnimationIndex < 0 || m_currentAnimationIndex >= static_cast<int>(m_animations.size())) return;

    const auto& clip = m_animations[m_currentAnimationIndex].clip;
    float duration = clip.duration;
    if (duration <= 0.0f) return;

    float screenW = static_cast<float>(m_ctx.window.getWidth());
    float screenH = static_cast<float>(m_ctx.window.getHeight());
    float timelineH = 140.0f;

    ImGui::SetNextWindowPos(ImVec2(0, screenH - timelineH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(screenW, timelineH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##Timeline", nullptr, flags)) {
        // Top row: transport controls + time info
        if (ImGui::Button(m_animationPlaying ? "||" : ">", ImVec2(30, 0))) {
            m_animationPlaying = !m_animationPlaying;
        }
        ImGui::SameLine();
        if (ImGui::Button("<<", ImVec2(30, 0))) {
            m_animationTime = 0.0f;
        }
        ImGui::SameLine();
        ImGui::Text("%.2f / %.2fs", m_animationTime, duration);
        ImGui::SameLine();
        ImGui::Text("(%d fps)", static_cast<int>(std::round(1.0f / (duration / std::max(1.0f, static_cast<float>(clip.channels.empty() ? 1 : clip.channels[0].positionTimes.size()))))));
        ImGui::SameLine(screenW - 200);
        if (m_selectedBone >= 0) {
            auto* data = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
            if (data && data->skeleton && m_selectedBone < static_cast<int>(data->skeleton->bones.size())) {
                ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "%s", data->skeleton->bones[m_selectedBone].name.c_str());
            }
        } else {
            ImGui::TextDisabled("No bone selected");
        }

        // Timeline bar area with zoom/pan
        ImVec2 barStart = ImGui::GetCursorScreenPos();
        float viewW = screenW - 20.0f;  // visible area width
        float barH = 20.0f;
        float barX = barStart.x + 10.0f;
        float barY = barStart.y;
        float totalBarW = viewW * m_timelineZoom;  // total zoomed width

        // Helper: time to screen X (accounting for zoom and pan)
        auto timeToX = [&](float t) -> float {
            return barX + (t / duration) * totalBarW + m_timelinePanX;
        };

        // Handle zoom (scroll wheel) and pan (MMB drag) over the timeline area
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool mouseInTimeline = mousePos.x >= barX && mousePos.x <= barX + viewW &&
                               mousePos.y >= barY && mousePos.y <= barY + 80.0f;
        if (mouseInTimeline) {
            // Scroll wheel: zoom centered on mouse position
            float scroll = ImGui::GetIO().MouseWheel;
            if (std::abs(scroll) > 0.01f) {
                float mouseT = ((mousePos.x - barX - m_timelinePanX) / totalBarW) * duration;
                float oldZoom = m_timelineZoom;
                m_timelineZoom *= (1.0f + scroll * 0.15f);
                m_timelineZoom = std::clamp(m_timelineZoom, 0.1f, 20.0f);
                // Adjust pan to keep mouse position stable
                float newTotalW = viewW * m_timelineZoom;
                float mouseXInBar = (mouseT / duration) * newTotalW;
                m_timelinePanX = (mousePos.x - barX) - mouseXInBar;
            }

            // MMB drag: pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                m_timelinePanX += ImGui::GetIO().MouseDelta.x;
            }
        }

        // Clamp pan so we don't scroll past the ends
        float maxPan = 0.0f;
        float minPan = viewW - totalBarW;
        if (totalBarW <= viewW) {
            m_timelinePanX = 0.0f; // no pan needed when everything fits
        } else {
            m_timelinePanX = std::clamp(m_timelinePanX, minPan, maxPan);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Clip to visible area
        dl->PushClipRect(ImVec2(barX, barY), ImVec2(barX + viewW, barY + 80.0f), true);

        // Background bar
        float barLeft = timeToX(0.0f);
        float barRight = timeToX(duration);
        dl->AddRectFilled(ImVec2(barLeft, barY), ImVec2(barRight, barY + barH),
                          IM_COL32(40, 40, 40, 255));
        dl->AddRect(ImVec2(barLeft, barY), ImVec2(barRight, barY + barH),
                    IM_COL32(80, 80, 80, 255));

        // Time tick marks — adapt interval to zoom level
        float pixelsPerSec = totalBarW / duration;
        float tickInterval = 1.0f;
        if (pixelsPerSec > 500.0f) tickInterval = 0.05f;
        else if (pixelsPerSec > 200.0f) tickInterval = 0.1f;
        else if (pixelsPerSec > 50.0f) tickInterval = 0.5f;
        float majorMult = 5.0f;

        for (float t = 0.0f; t <= duration; t += tickInterval) {
            float x = timeToX(t);
            if (x < barX - 20 || x > barX + viewW + 20) continue;
            bool isMajor = std::fmod(t, tickInterval * majorMult) < tickInterval * 0.5f || t < 0.001f;
            float tickH = isMajor ? barH : barH * 0.4f;
            dl->AddLine(ImVec2(x, barY + barH - tickH), ImVec2(x, barY + barH),
                        IM_COL32(100, 100, 100, 200));
            if (isMajor) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.2f", t);
                dl->AddText(ImVec2(x + 2, barY), IM_COL32(150, 150, 150, 200), buf);
            }
        }

        // Playhead (red line)
        float playX = timeToX(m_animationTime);
        dl->AddLine(ImVec2(playX, barY), ImVec2(playX, barY + barH), IM_COL32(255, 50, 50, 255), 2.0f);
        dl->AddTriangleFilled(ImVec2(playX - 5, barY), ImVec2(playX + 5, barY),
                              ImVec2(playX, barY + 6), IM_COL32(255, 50, 50, 255));

        // Scrub: LMB click/drag on bar to set time
        ImGui::SetCursorScreenPos(ImVec2(barX, barY));
        ImGui::InvisibleButton("##timeline_scrub", ImVec2(viewW, barH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float mouseX = ImGui::GetIO().MousePos.x;
            float t = ((mouseX - barX - m_timelinePanX) / totalBarW) * duration;
            m_animationTime = std::clamp(t, 0.0f, duration);
            m_scrubbing = true;
        }

        // Keyframe tracks for selected bone
        float trackY = barY + barH + 4.0f;
        float trackH = 16.0f;

        const eden::AnimationChannel* selChannel = nullptr;
        if (m_selectedBone >= 0) {
            for (const auto& ch : clip.channels) {
                if (ch.boneIndex == m_selectedBone) {
                    selChannel = &ch;
                    break;
                }
            }
        }

        if (selChannel) {
            auto drawKeyTrack = [&](float y, const char* label, ImU32 labelCol, ImU32 bgCol,
                                    ImU32 keyCol, const std::vector<float>& times) {
                dl->AddText(ImVec2(barX - 8, y), labelCol, label);
                dl->AddRectFilled(ImVec2(barLeft, y), ImVec2(barRight, y + trackH), bgCol);
                for (float t : times) {
                    float kx = timeToX(t);
                    if (kx < barX - 10 || kx > barX + viewW + 10) continue;
                    float ky = y + trackH * 0.5f;
                    float ds = 4.0f;
                    dl->AddQuadFilled(ImVec2(kx, ky - ds), ImVec2(kx + ds, ky),
                                      ImVec2(kx, ky + ds), ImVec2(kx - ds, ky), keyCol);
                }
            };

            drawKeyTrack(trackY, "P", IM_COL32(255, 100, 100, 255), IM_COL32(30, 20, 20, 255),
                         IM_COL32(255, 80, 80, 255), selChannel->positionTimes);
            drawKeyTrack(trackY + trackH + 2, "R", IM_COL32(100, 255, 100, 255), IM_COL32(20, 30, 20, 255),
                         IM_COL32(80, 255, 80, 255), selChannel->rotationTimes);
            drawKeyTrack(trackY + (trackH + 2) * 2, "S", IM_COL32(100, 100, 255, 255), IM_COL32(20, 20, 30, 255),
                         IM_COL32(80, 80, 255, 255), selChannel->scaleTimes);
        } else {
            dl->AddText(ImVec2(barX, trackY + 4), IM_COL32(100, 100, 100, 200),
                        m_selectedBone >= 0 ? "No keyframes for this bone" : "Select a bone to see keyframes");
        }

        dl->PopClipRect();
    }
    ImGui::End();
}

void AnimationMode::openSkinnedModelDialog() {
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Models", "glb"}};

    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, m_ctx.projectPath.empty() ? nullptr : m_ctx.projectPath.c_str());

    if (result == NFD_OKAY) {
        loadSkinnedModel(outPath);
        NFD_FreePath(outPath);
    }
}

void AnimationMode::loadSkinnedModel(const std::string& path) {
    try {
        SkinnedLoadResult result = SkinnedGLBLoader::load(path);

        if (!result.success) {
            std::cerr << "Failed to load skinned model: " << result.error << std::endl;
            return;
        }

        if (result.meshes.empty()) {
            std::cerr << "No meshes found in model: " << path << std::endl;
            return;
        }

        // Clean up old model
        if (m_skinnedModelHandle != UINT32_MAX) {
            m_ctx.skinnedModelRenderer.destroyModel(m_skinnedModelHandle);
        }
        m_animations.clear();
        m_currentAnimationIndex = -1;
        m_boneNames.clear();

        // Cache bone names before moving skeleton
        if (result.skeleton) {
            for (const auto& bone : result.skeleton->bones) {
                m_boneNames.push_back(bone.name);
            }
        }

        // Get mesh data
        const auto& mesh = result.meshes[0];

        // Store CPU vertex copy for weight heat map (kept in AnimationMode, not engine)
        m_cpuVertices = mesh.vertices;

        // Create new model
        m_skinnedModelHandle = m_ctx.skinnedModelRenderer.createModel(
            mesh.vertices,
            mesh.indices,
            std::move(result.skeleton),
            std::move(result.animations),
            mesh.textureData.empty() ? nullptr : mesh.textureData.data(),
            mesh.textureWidth,
            mesh.textureHeight
        );

        m_baseModelPath = path;

        // Get animations back from the renderer to store locally
        auto* modelData = m_ctx.skinnedModelRenderer.getModelData(m_skinnedModelHandle);
        if (modelData) {
            for (const auto& clip : modelData->animations) {
                StoredAnimation anim;
                anim.name = clip.name;
                anim.sourceFile = path;
                anim.clip = clip;
                m_animations.push_back(anim);
            }
        }

        if (!m_animations.empty()) {
            m_currentAnimationIndex = 0;
            m_ctx.skinnedModelRenderer.playAnimation(m_skinnedModelHandle, m_animations[0].name, true);
        }

        std::cout << "Loaded skinned model: " << path << std::endl;
        std::cout << "  Bones: " << m_boneNames.size() << std::endl;
        std::cout << "  Animations: " << m_animations.size() << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Failed to load skinned model: " << e.what() << std::endl;
    }
}

void AnimationMode::addAnimationDialog() {
    if (m_skinnedModelHandle == UINT32_MAX) {
        std::cerr << "Load a base model first!" << std::endl;
        return;
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Animation", "glb"}};

    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, m_ctx.projectPath.empty() ? nullptr : m_ctx.projectPath.c_str());

    if (result == NFD_OKAY) {
        addAnimationFromFile(outPath);
        NFD_FreePath(outPath);
    }
}

std::string AnimationMode::detectBonePrefix(const std::string& boneName) {
    // Common Mixamo prefixes
    if (boneName.find("mixamorig:") == 0) return "mixamorig:";
    if (boneName.find("mixamorig1:") == 0) return "mixamorig1:";
    if (boneName.find("mixamorig2:") == 0) return "mixamorig2:";
    if (boneName.find("mixamorig3:") == 0) return "mixamorig3:";
    if (boneName.find("mixamorig4:") == 0) return "mixamorig4:";
    if (boneName.find("mixamorig5:") == 0) return "mixamorig5:";
    if (boneName.find("mixamorig6:") == 0) return "mixamorig6:";
    if (boneName.find("mixamorig7:") == 0) return "mixamorig7:";
    if (boneName.find("mixamorig8:") == 0) return "mixamorig8:";
    if (boneName.find("mixamorig9:") == 0) return "mixamorig9:";

    // No prefix or unknown
    return "";
}

std::string AnimationMode::remapBoneName(const std::string& srcName, const std::string& srcPrefix, const std::string& dstPrefix) {
    if (srcPrefix.empty() || srcName.find(srcPrefix) != 0) {
        return srcName; // No remapping needed
    }
    // Replace prefix
    return dstPrefix + srcName.substr(srcPrefix.length());
}

void AnimationMode::addAnimationFromFile(const std::string& path) {
    try {
        SkinnedLoadResult result = SkinnedGLBLoader::load(path);

        if (!result.success) {
            std::cerr << "Failed to load animation: " << result.error << std::endl;
            return;
        }

        if (result.animations.empty()) {
            std::cerr << "No animations found in file: " << path << std::endl;
            return;
        }

        // Detect bone prefixes
        std::string srcPrefix = "";
        std::string dstPrefix = "";

        // Get source bone names
        std::vector<std::string> srcBoneNames;
        if (result.skeleton) {
            for (const auto& bone : result.skeleton->bones) {
                srcBoneNames.push_back(bone.name);
            }
        }

        if (!srcBoneNames.empty()) {
            srcPrefix = detectBonePrefix(srcBoneNames[0]);
        }
        if (!m_boneNames.empty()) {
            dstPrefix = detectBonePrefix(m_boneNames[0]);
        }

        bool needsRemap = !srcPrefix.empty() && !dstPrefix.empty() && srcPrefix != dstPrefix;

        if (needsRemap) {
            std::cout << "Remapping bones: " << srcPrefix << " -> " << dstPrefix << std::endl;
        }

        // Build bone name map from source to destination indices
        std::map<int, int> boneIndexMap;
        for (size_t i = 0; i < srcBoneNames.size(); i++) {
            std::string srcBone = srcBoneNames[i];
            std::string dstBone = needsRemap ? remapBoneName(srcBone, srcPrefix, dstPrefix) : srcBone;

            // Find matching bone in our skeleton
            for (size_t j = 0; j < m_boneNames.size(); j++) {
                if (m_boneNames[j] == dstBone) {
                    boneIndexMap[static_cast<int>(i)] = static_cast<int>(j);
                    break;
                }
            }
        }

        std::cout << "Mapped " << boneIndexMap.size() << "/" << srcBoneNames.size() << " bones" << std::endl;

        // Extract filename for default animation name
        size_t lastSlash = path.find_last_of("/\\");
        size_t lastDot = path.find_last_of(".");
        std::string filename = path.substr(lastSlash + 1, lastDot - lastSlash - 1);

        // Add each animation from the file
        for (auto& clip : result.animations) {
            // Remap bone indices in the animation
            AnimationClip remappedClip;
            remappedClip.name = clip.name.empty() ? filename : clip.name;
            remappedClip.duration = clip.duration;

            for (const auto& srcChannel : clip.channels) {
                auto it = boneIndexMap.find(srcChannel.boneIndex);
                if (it != boneIndexMap.end()) {
                    AnimationChannel dstChannel = srcChannel;
                    dstChannel.boneIndex = it->second;
                    remappedClip.channels.push_back(dstChannel);
                }
            }

            // Make name unique
            std::string baseName = remappedClip.name;
            int counter = 1;
            while (animationNameExists(remappedClip.name)) {
                remappedClip.name = baseName + "_" + std::to_string(counter++);
            }

            StoredAnimation anim;
            anim.name = remappedClip.name;
            anim.sourceFile = path;
            anim.clip = remappedClip;
            m_animations.push_back(anim);

            // Register with renderer
            m_ctx.skinnedModelRenderer.addAnimation(m_skinnedModelHandle, remappedClip);

            std::cout << "Added animation: " << remappedClip.name
                      << " (" << remappedClip.channels.size() << " channels, "
                      << remappedClip.duration << "s)" << std::endl;
        }

        // Select first new animation
        if (m_currentAnimationIndex < 0) {
            m_currentAnimationIndex = 0;
            m_ctx.skinnedModelRenderer.playAnimation(m_skinnedModelHandle, m_animations[0].name, true);
        }

    } catch (const std::exception& e) {
        std::cerr << "Failed to add animation: " << e.what() << std::endl;
    }
}

bool AnimationMode::animationNameExists(const std::string& name) {
    for (const auto& anim : m_animations) {
        if (anim.name == name) return true;
    }
    return false;
}

void AnimationMode::exportCombinedGLB() {
    if (m_skinnedModelHandle == UINT32_MAX || m_animations.empty()) {
        std::cerr << "Nothing to export!" << std::endl;
        return;
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Model", "glb"}};

    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, m_ctx.projectPath.empty() ? nullptr : m_ctx.projectPath.c_str(), "combined.glb");

    if (result == NFD_OKAY) {
        exportToGLB(outPath);
        NFD_FreePath(outPath);
    }
}

void AnimationMode::exportToGLB(const std::string& path) {
    // Re-load the base model to get the original glTF data
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    if (!loader.LoadBinaryFromFile(&model, &err, &warn, m_baseModelPath)) {
        std::cerr << "Failed to reload base model for export: " << err << std::endl;
        return;
    }

    // Find root bone name for Meshy correction (needed when writing animations)
    std::string rootBoneName;
    if (m_animationSource == AnimationSource::Meshy && !model.skins.empty()) {
        auto& skin = model.skins[0];
        int rootNodeIdx = skin.skeleton >= 0 ? skin.skeleton : (skin.joints.empty() ? -1 : skin.joints[0]);
        if (rootNodeIdx >= 0) {
            rootBoneName = model.nodes[rootNodeIdx].name;

            // Bake -90° X into root bone's rest pose so it matches Mixamo convention
            auto& rootNode = model.nodes[rootNodeIdx];
            glm::quat correction = glm::quat(glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f));
            if (rootNode.rotation.size() == 4) {
                glm::quat existing(static_cast<float>(rootNode.rotation[3]),
                                   static_cast<float>(rootNode.rotation[0]),
                                   static_cast<float>(rootNode.rotation[1]),
                                   static_cast<float>(rootNode.rotation[2]));
                glm::quat combined = correction * existing;
                rootNode.rotation = {combined.x, combined.y, combined.z, combined.w};
            } else {
                rootNode.rotation = {correction.x, correction.y, correction.z, correction.w};
            }
            if (rootNode.translation.size() == 3) {
                glm::mat4 rot = glm::mat4_cast(correction);
                glm::vec3 t(rootNode.translation[0], rootNode.translation[1], rootNode.translation[2]);
                t = glm::vec3(rot * glm::vec4(t, 1.0f));
                rootNode.translation = {t.x, t.y, t.z};
            }
            std::cout << "Meshy correction: rotating root bone '" << rootBoneName << "'" << std::endl;
        }
    }

    // Clear existing animations
    model.animations.clear();

    // Add our combined animations
    for (const auto& storedAnim : m_animations) {
        tinygltf::Animation gltfAnim;
        gltfAnim.name = storedAnim.name;

        const auto& clip = storedAnim.clip;

        for (const auto& channel : clip.channels) {
            // Find the node index for this bone
            int nodeIndex = -1;
            if (channel.boneIndex >= 0 && channel.boneIndex < static_cast<int>(m_boneNames.size())) {
                const std::string& boneName = m_boneNames[channel.boneIndex];
                for (size_t i = 0; i < model.nodes.size(); i++) {
                    if (model.nodes[i].name == boneName) {
                        nodeIndex = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (nodeIndex < 0) continue;

            // Check if this is the root bone for Meshy correction
            bool isRootBone = (m_animationSource == AnimationSource::Meshy &&
                               !rootBoneName.empty() &&
                               channel.boneIndex >= 0 &&
                               channel.boneIndex < static_cast<int>(m_boneNames.size()) &&
                               m_boneNames[channel.boneIndex] == rootBoneName);

            glm::quat meshyCorrection = glm::quat(glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f));
            glm::mat4 meshyCorrectionMat = glm::mat4_cast(meshyCorrection);

            // Add translation channel if we have position data
            if (!channel.positions.empty() && !channel.positionTimes.empty()) {
                tinygltf::AnimationSampler sampler;
                sampler.interpolation = "LINEAR";

                int timeAccessorIdx = createFloatAccessor(model, channel.positionTimes, TINYGLTF_TYPE_SCALAR);
                sampler.input = timeAccessorIdx;

                std::vector<float> positions;
                for (const auto& pos : channel.positions) {
                    if (isRootBone) {
                        glm::vec3 rotated = glm::vec3(meshyCorrectionMat * glm::vec4(pos, 1.0f));
                        positions.push_back(rotated.x);
                        positions.push_back(rotated.y);
                        positions.push_back(rotated.z);
                    } else {
                        positions.push_back(pos.x);
                        positions.push_back(pos.y);
                        positions.push_back(pos.z);
                    }
                }
                int posAccessorIdx = createFloatAccessor(model, positions, TINYGLTF_TYPE_VEC3);
                sampler.output = posAccessorIdx;

                int sampIdx = static_cast<int>(gltfAnim.samplers.size());
                gltfAnim.samplers.push_back(sampler);

                tinygltf::AnimationChannel gltfChannel;
                gltfChannel.sampler = sampIdx;
                gltfChannel.target_node = nodeIndex;
                gltfChannel.target_path = "translation";
                gltfAnim.channels.push_back(gltfChannel);
            }

            // Add rotation channel if we have rotation data
            if (!channel.rotations.empty() && !channel.rotationTimes.empty()) {
                tinygltf::AnimationSampler sampler;
                sampler.interpolation = "LINEAR";

                int timeAccessorIdx = createFloatAccessor(model, channel.rotationTimes, TINYGLTF_TYPE_SCALAR);
                sampler.input = timeAccessorIdx;

                std::vector<float> rotations;
                for (const auto& rot : channel.rotations) {
                    if (isRootBone) {
                        glm::quat corrected = meshyCorrection * rot;
                        rotations.push_back(corrected.x);
                        rotations.push_back(corrected.y);
                        rotations.push_back(corrected.z);
                        rotations.push_back(corrected.w);
                    } else {
                        rotations.push_back(rot.x);
                        rotations.push_back(rot.y);
                        rotations.push_back(rot.z);
                        rotations.push_back(rot.w);
                    }
                }
                int rotAccessorIdx = createFloatAccessor(model, rotations, TINYGLTF_TYPE_VEC4);
                sampler.output = rotAccessorIdx;

                int sampIdx = static_cast<int>(gltfAnim.samplers.size());
                gltfAnim.samplers.push_back(sampler);

                tinygltf::AnimationChannel gltfChannel;
                gltfChannel.sampler = sampIdx;
                gltfChannel.target_node = nodeIndex;
                gltfChannel.target_path = "rotation";
                gltfAnim.channels.push_back(gltfChannel);
            }

            // Add scale channel if we have scale data
            if (!channel.scales.empty() && !channel.scaleTimes.empty()) {
                tinygltf::AnimationSampler sampler;
                sampler.interpolation = "LINEAR";

                int timeAccessorIdx = createFloatAccessor(model, channel.scaleTimes, TINYGLTF_TYPE_SCALAR);
                sampler.input = timeAccessorIdx;

                // Create scale accessor - flatten vec3 array
                std::vector<float> scales;
                for (const auto& scl : channel.scales) {
                    scales.push_back(scl.x);
                    scales.push_back(scl.y);
                    scales.push_back(scl.z);
                }
                int scaleAccessorIdx = createFloatAccessor(model, scales, TINYGLTF_TYPE_VEC3);
                sampler.output = scaleAccessorIdx;

                int sampIdx = static_cast<int>(gltfAnim.samplers.size());
                gltfAnim.samplers.push_back(sampler);

                tinygltf::AnimationChannel gltfChannel;
                gltfChannel.sampler = sampIdx;
                gltfChannel.target_node = nodeIndex;
                gltfChannel.target_path = "scale";
                gltfAnim.channels.push_back(gltfChannel);
            }
        }

        if (!gltfAnim.channels.empty()) {
            model.animations.push_back(gltfAnim);
        }
    }

    // Write the combined GLB
    tinygltf::TinyGLTF writer;
    if (!writer.WriteGltfSceneToFile(&model, path, true, true, true, true)) {
        std::cerr << "Failed to write GLB: " << path << std::endl;
        return;
    }

    std::cout << "Exported combined GLB: " << path << std::endl;
    std::cout << "  Animations: " << model.animations.size() << std::endl;
}

int AnimationMode::createFloatAccessor(tinygltf::Model& model, const std::vector<float>& data, int type) {
    // Create buffer view
    tinygltf::BufferView bufferView;
    bufferView.buffer = 0;
    bufferView.byteOffset = model.buffers[0].data.size();
    bufferView.byteLength = data.size() * sizeof(float);
    bufferView.target = 0; // Not a vertex/index buffer

    // Append data to buffer
    const unsigned char* dataPtr = reinterpret_cast<const unsigned char*>(data.data());
    model.buffers[0].data.insert(model.buffers[0].data.end(), dataPtr, dataPtr + bufferView.byteLength);

    int bufferViewIdx = static_cast<int>(model.bufferViews.size());
    model.bufferViews.push_back(bufferView);

    // Create accessor
    tinygltf::Accessor accessor;
    accessor.bufferView = bufferViewIdx;
    accessor.byteOffset = 0;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = type;

    if (type == TINYGLTF_TYPE_SCALAR) {
        accessor.count = data.size();
        // Set min/max for time values
        float minVal = *std::min_element(data.begin(), data.end());
        float maxVal = *std::max_element(data.begin(), data.end());
        accessor.minValues = {minVal};
        accessor.maxValues = {maxVal};
    } else if (type == TINYGLTF_TYPE_VEC3) {
        accessor.count = data.size() / 3;
    } else if (type == TINYGLTF_TYPE_VEC4) {
        accessor.count = data.size() / 4;
    }

    int accessorIdx = static_cast<int>(model.accessors.size());
    model.accessors.push_back(accessor);

    return accessorIdx;
}
