#include "ModelingMode.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace eden;

// Force the current window to fit above the timeline strip — both shrinks
// the height and lifts the position if it has been saved out of bounds.
// Must be called from inside Begin().
// Call BEFORE ImGui::Begin(name, ...). When a layout reset is pending, this
// pushes SetNextWindowPos/Size with ImGuiCond_Always for known panels so
// they snap into a docked slot regardless of imgui.ini state. imgui docs
// explicitly say to prefer SetNextWindow* over the post-Begin SetWindow*
// variants, which "may incur tearing and side-effects" (= the blinking).
void ModelingMode::clampWindowAboveTimeline(const char* name) {
    if (!m_layoutResetPending) return;

    const float screenW = ImGui::GetIO().DisplaySize.x;
    const float screenH = ImGui::GetIO().DisplaySize.y;
    const float topY = 24.0f;  // below main menu bar
    const float maxBottom = screenH - kTimelineHeight - 4.0f;
    const float availH = std::max(120.0f, maxBottom - topY);

    if (std::strcmp(name, "Scene") == 0) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, topY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(250.0f, availH * 0.45f), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    } else if (std::strcmp(name, "Tools") == 0) {
        float toolsTop = topY + availH * 0.45f + 4.0f;
        ImGui::SetNextWindowPos(ImVec2(0.0f, toolsTop), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(250.0f, maxBottom - toolsTop), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    } else if (std::strcmp(name, "Camera") == 0) {
        ImGui::SetNextWindowPos(ImVec2(screenW - 250.0f, topY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(250.0f, std::min(availH, 400.0f)), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    } else if (std::strstr(name, "AI Generate") != nullptr) {
        ImGui::SetNextWindowPos(ImVec2((screenW - 520.0f) * 0.5f, topY + 20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(520.0f, std::min(availH - 40.0f, 620.0f)), ImGuiCond_Always);
    }
}

void ModelingMode::setKeyOnSelected() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;

    auto& track = m_objectAnims[obj];
    const auto& tf = obj->getTransform();
    const float t = m_timelineCurrentTime;

    // For rigged objects with a bind pose, snapshot only the bone positions.
    // Verts are recomputed on playback via skinning from the bind pose, so
    // per-key memory is O(bones) instead of O(verts).
    bool snapshotRig = m_hasBindPose && m_bindPoseOwner == obj &&
                       obj == m_ctx.selectedObject && m_ctx.editableMesh.isValid();
    std::vector<glm::vec3> bonesSnap;
    if (snapshotRig) {
        bonesSnap = m_bonePositions;
    }

    // Find the slot for this time. If a key already exists within a tiny
    // epsilon, replace it; otherwise insert sorted.
    auto it = std::lower_bound(track.times.begin(), track.times.end(), t);
    size_t idx = static_cast<size_t>(it - track.times.begin());

    bool replace = (idx < track.times.size() && std::abs(track.times[idx] - t) < 1e-4f);
    if (replace) {
        track.positions[idx] = tf.getPosition();
        track.rotations[idx] = tf.getRotation();
        track.scales[idx]    = tf.getScale();
        if (snapshotRig) {
            if (track.bonePositionsPerKey.size() < track.times.size())
                track.bonePositionsPerKey.resize(track.times.size());
            track.bonePositionsPerKey[idx] = std::move(bonesSnap);
        }
    } else {
        track.times.insert(track.times.begin() + idx, t);
        track.positions.insert(track.positions.begin() + idx, tf.getPosition());
        track.rotations.insert(track.rotations.begin() + idx, tf.getRotation());
        track.scales.insert(track.scales.begin() + idx, tf.getScale());
        if (snapshotRig) {
            if (track.bonePositionsPerKey.size() < track.times.size() - 1)
                track.bonePositionsPerKey.resize(track.times.size() - 1);
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx, std::move(bonesSnap));
        } else if (!track.bonePositionsPerKey.empty()) {
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx, std::vector<glm::vec3>{});
        }
    }
}

void ModelingMode::deleteKeyOnSelectedNearTime() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto found = m_objectAnims.find(obj);
    if (found == m_objectAnims.end()) return;
    auto& track = found->second;
    if (track.times.empty()) return;

    // Find the closest key within a small threshold.
    const float t = m_timelineCurrentTime;
    size_t best = 0;
    float bestDist = std::abs(track.times[0] - t);
    for (size_t i = 1; i < track.times.size(); ++i) {
        float d = std::abs(track.times[i] - t);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (bestDist > 0.05f) return;  // No key close enough — no-op

    track.times.erase(track.times.begin() + best);
    track.positions.erase(track.positions.begin() + best);
    track.rotations.erase(track.rotations.begin() + best);
    track.scales.erase(track.scales.begin() + best);
    if (best < track.bonePositionsPerKey.size())
        track.bonePositionsPerKey.erase(track.bonePositionsPerKey.begin() + best);
    if (track.times.empty()) m_objectAnims.erase(found);
}

void ModelingMode::jumpToPrevKey() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto it = m_objectAnims.find(obj);
    if (it == m_objectAnims.end() || it->second.times.empty()) return;
    const auto& times = it->second.times;
    // Strictly less than current time so we don't sit on the same key.
    float best = -1.0f;
    for (float t : times) {
        if (t < m_timelineCurrentTime - 1e-4f && t > best) best = t;
    }
    if (best >= 0.0f) {
        m_timelineCurrentTime = best;
        m_timelinePlaying = false;
    } else {
        // Already at/before first key — wrap to last key.
        m_timelineCurrentTime = times.back();
        m_timelinePlaying = false;
    }
}

void ModelingMode::jumpToNextKey() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto it = m_objectAnims.find(obj);
    if (it == m_objectAnims.end() || it->second.times.empty()) return;
    const auto& times = it->second.times;
    float best = -1.0f;
    for (float t : times) {
        if (t > m_timelineCurrentTime + 1e-4f && (best < 0.0f || t < best)) best = t;
    }
    if (best >= 0.0f) {
        m_timelineCurrentTime = best;
        m_timelinePlaying = false;
    } else {
        // Past the last key — wrap to first.
        m_timelineCurrentTime = times.front();
        m_timelinePlaying = false;
    }
}

void ModelingMode::copyKeyAtCurrentTime() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto it = m_objectAnims.find(obj);
    if (it == m_objectAnims.end() || it->second.times.empty()) return;
    const auto& tr = it->second;

    // Find the key closest to the playhead within 50 ms.
    size_t best = 0;
    float bestDist = std::abs(tr.times[0] - m_timelineCurrentTime);
    for (size_t i = 1; i < tr.times.size(); ++i) {
        float d = std::abs(tr.times[i] - m_timelineCurrentTime);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (bestDist > 0.05f) return;

    m_keyClipboard.valid = true;
    m_keyClipboard.position = tr.positions[best];
    m_keyClipboard.rotation = tr.rotations[best];
    m_keyClipboard.scale    = tr.scales[best];
    if (best < tr.bonePositionsPerKey.size()) {
        m_keyClipboard.bonePositions = tr.bonePositionsPerKey[best];
    } else {
        m_keyClipboard.bonePositions.clear();
    }
}

void ModelingMode::pasteKeyAtCurrentTime() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj || !m_keyClipboard.valid) return;

    auto& track = m_objectAnims[obj];
    const float t = m_timelineCurrentTime;
    auto lb = std::lower_bound(track.times.begin(), track.times.end(), t);
    size_t idx = static_cast<size_t>(lb - track.times.begin());
    bool replace = (idx < track.times.size() && std::abs(track.times[idx] - t) < 1e-4f);

    bool hasRig = !m_keyClipboard.bonePositions.empty();
    if (replace) {
        track.positions[idx] = m_keyClipboard.position;
        track.rotations[idx] = m_keyClipboard.rotation;
        track.scales[idx]    = m_keyClipboard.scale;
        if (hasRig) {
            if (track.bonePositionsPerKey.size() < track.times.size())
                track.bonePositionsPerKey.resize(track.times.size());
            track.bonePositionsPerKey[idx] = m_keyClipboard.bonePositions;
        }
    } else {
        track.times.insert(track.times.begin() + idx, t);
        track.positions.insert(track.positions.begin() + idx, m_keyClipboard.position);
        track.rotations.insert(track.rotations.begin() + idx, m_keyClipboard.rotation);
        track.scales.insert(track.scales.begin() + idx, m_keyClipboard.scale);
        if (hasRig) {
            if (track.bonePositionsPerKey.size() < track.times.size() - 1)
                track.bonePositionsPerKey.resize(track.times.size() - 1);
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx,
                                             m_keyClipboard.bonePositions);
        } else if (!track.bonePositionsPerKey.empty()) {
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx,
                                             std::vector<glm::vec3>{});
        }
    }
    // Force the playback path to apply the pasted state on the next tick.
    m_timelineLastAppliedTime = -1.0f;
}

void ModelingMode::applyAnimatedTransforms() {
    const float t = m_timelineCurrentTime;
    for (auto& [obj, track] : m_objectAnims) {
        if (!obj || track.times.empty()) continue;

        // Find the bracketing keyframes.
        size_t i0 = 0, i1 = 0;
        float u = 0.0f;
        if (t <= track.times.front()) {
            i0 = i1 = 0;
        } else if (t >= track.times.back()) {
            i0 = i1 = track.times.size() - 1;
        } else {
            while (i0 + 1 < track.times.size() && track.times[i0 + 1] < t) ++i0;
            i1 = i0 + 1;
            float t0 = track.times[i0];
            float t1 = track.times[i1];
            u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
        }

        glm::vec3 pos = glm::mix(track.positions[i0], track.positions[i1], u);
        glm::quat rot = glm::slerp(track.rotations[i0], track.rotations[i1], u);
        glm::vec3 scl = glm::mix(track.scales[i0], track.scales[i1], u);
        auto& tf = obj->getTransform();
        tf.setPosition(pos);
        tf.setRotation(rot);
        tf.setScale(scl);

        // Skeleton playback: lerp bone positions and re-skin from the bind
        // pose. Only "shows" while the bound object is the active editable
        // mesh (we don't want to touch another object's editableMesh data).
        if (obj != m_ctx.selectedObject) continue;
        if (!m_hasBindPose || m_bindPoseOwner != obj) continue;
        if (i0 >= track.bonePositionsPerKey.size() || i1 >= track.bonePositionsPerKey.size()) continue;
        const auto& bonesA = track.bonePositionsPerKey[i0];
        const auto& bonesB = track.bonePositionsPerKey[i1];
        if (bonesA.empty() || bonesB.empty() || bonesA.size() != bonesB.size()) continue;
        if (m_bonePositions.size() != bonesA.size()) continue;

        // Lerp bone world positions, sync skeleton localTransforms (relative
        // to parent — matches the bind-pose hierarchy we computed earlier).
        auto& skel = m_ctx.editableMesh.getSkeleton();
        for (size_t b = 0; b < bonesA.size(); ++b) {
            m_bonePositions[b] = glm::mix(bonesA[b], bonesB[b], u);
            if (b < skel.bones.size()) {
                int p = skel.bones[b].parentIndex;
                glm::vec3 parentPos(0.0f);
                if (p >= 0 && p < static_cast<int>(b) && p < static_cast<int>(m_bonePositions.size())) {
                    parentPos = m_bonePositions[p];
                }
                skel.bones[b].localTransform =
                    glm::translate(glm::mat4(1.0f), m_bonePositions[b] - parentPos);
            }
        }

        // Reskin from the bind pose; pushes new verts via updateModelBuffer.
        // No vertex snapshots; per-key memory is O(bones).
        reskinFromBoneDeltas();
    }
}

// Always-visible timeline strip pinned to the bottom of the screen.
// First slice: visual scrubber + play/pause + time readout. No keyframes,
// no clip storage, no playback effects yet — those layers come next.
void ModelingMode::renderAnimationTimeline() {
    const float screenW = ImGui::GetIO().DisplaySize.x;
    const float screenH = ImGui::GetIO().DisplaySize.y;
    const float timelineH = kTimelineHeight;

    ImGui::SetNextWindowPos(ImVec2(0, screenH - timelineH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(screenW, timelineH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##LimeTimeline", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Advance time when playing.
    if (m_timelinePlaying) {
        m_timelineCurrentTime += ImGui::GetIO().DeltaTime;
        if (m_timelineCurrentTime >= m_timelineDuration) {
            m_timelineCurrentTime = std::fmod(m_timelineCurrentTime, m_timelineDuration);
        }
    }

    // Transport row.
    if (ImGui::Button(m_timelinePlaying ? "||" : ">", ImVec2(34, 0))) {
        m_timelinePlaying = !m_timelinePlaying;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<", ImVec2(34, 0))) {
        m_timelineCurrentTime = 0.0f;
    }
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2fs", m_timelineCurrentTime, m_timelineDuration);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("Length", &m_timelineDuration, 0.1f, 0.1f, 600.0f, "%.1fs");
    ImGui::SameLine();
    {
        SceneObject* obj = m_ctx.selectedObject;
        bool haveObj = (obj != nullptr);
        auto trackIt = haveObj ? m_objectAnims.find(obj) : m_objectAnims.end();
        bool haveKeys = (trackIt != m_objectAnims.end() && !trackIt->second.times.empty());

        if (!haveObj) ImGui::BeginDisabled();
        if (ImGui::Button("Set Key")) setKeyOnSelected();
        ImGui::SameLine();
        if (ImGui::Button("Delete Key")) deleteKeyOnSelectedNearTime();
        if (!haveObj) ImGui::EndDisabled();

        ImGui::SameLine();
        if (!haveKeys) ImGui::BeginDisabled();
        if (ImGui::Button("|<<")) jumpToPrevKey();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to previous keyframe");
        ImGui::SameLine();
        if (ImGui::Button(">>|")) jumpToNextKey();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to next keyframe");
        ImGui::SameLine();
        if (ImGui::Button("Copy")) copyKeyAtCurrentTime();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the key nearest the playhead (within 50 ms)");
        if (!haveKeys) ImGui::EndDisabled();

        ImGui::SameLine();
        if (!haveObj || !m_keyClipboard.valid) ImGui::BeginDisabled();
        if (ImGui::Button("Paste")) pasteKeyAtCurrentTime();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Insert (or overwrite) a key at the playhead from the clipboard");
        if (!haveObj || !m_keyClipboard.valid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (haveObj) {
            int n = haveKeys ? static_cast<int>(trackIt->second.times.size()) : 0;
            ImGui::TextDisabled("%s [%d keys]%s",
                                obj->getName().c_str(), n,
                                m_keyClipboard.valid ? " (clipboard)" : "");
        } else {
            ImGui::TextDisabled("(select an object to key)");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Layout")) {
        m_layoutResetPending = true;
    }

    // Scrub bar.
    ImVec2 barOrigin = ImGui::GetCursorScreenPos();
    const float barLeft = barOrigin.x + 10.0f;
    const float barTop = barOrigin.y + 6.0f;
    const float barW = screenW - 20.0f;
    const float barH = 22.0f;
    const float barRight = barLeft + barW;
    const float barBot = barTop + barH;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Track background.
    dl->AddRectFilled(ImVec2(barLeft, barTop), ImVec2(barRight, barBot),
                      IM_COL32(40, 40, 40, 255), 3.0f);
    dl->AddRect(ImVec2(barLeft, barTop), ImVec2(barRight, barBot),
                IM_COL32(80, 80, 80, 255), 3.0f);

    // Tick marks every second.
    const float secondsPerPx = m_timelineDuration / barW;
    if (secondsPerPx > 0.0f) {
        int firstTick = 0;
        int lastTick = static_cast<int>(std::ceil(m_timelineDuration));
        for (int s = firstTick; s <= lastTick; ++s) {
            float x = barLeft + (s / m_timelineDuration) * barW;
            bool major = (s % 5 == 0);
            float h = major ? 8.0f : 4.0f;
            dl->AddLine(ImVec2(x, barBot), ImVec2(x, barBot + h),
                        IM_COL32(140, 140, 140, 200), 1.0f);
            if (major) {
                char label[16];
                std::snprintf(label, sizeof(label), "%ds", s);
                dl->AddText(ImVec2(x + 2, barBot + 2),
                            IM_COL32(160, 160, 160, 220), label);
            }
        }
    }

    // Key markers for the selected object's track (yellow diamonds).
    if (m_ctx.selectedObject) {
        auto it = m_objectAnims.find(m_ctx.selectedObject);
        if (it != m_objectAnims.end()) {
            for (float kt : it->second.times) {
                if (kt < 0.0f || kt > m_timelineDuration) continue;
                float kx = barLeft + (kt / m_timelineDuration) * barW;
                float midY = (barTop + barBot) * 0.5f;
                ImVec2 a(kx, midY - 5);
                ImVec2 b(kx + 5, midY);
                ImVec2 c(kx, midY + 5);
                ImVec2 d(kx - 5, midY);
                dl->AddQuadFilled(a, b, c, d, IM_COL32(80, 200, 255, 255));
                dl->AddQuad(a, b, c, d, IM_COL32(20, 60, 90, 255), 1.0f);
            }
        }
    }

    // Playhead.
    float playheadX = barLeft + (m_timelineCurrentTime / m_timelineDuration) * barW;
    dl->AddLine(ImVec2(playheadX, barTop - 2), ImVec2(playheadX, barBot + 12),
                IM_COL32(255, 200, 60, 255), 2.0f);
    dl->AddTriangleFilled(ImVec2(playheadX - 5, barTop - 2),
                          ImVec2(playheadX + 5, barTop - 2),
                          ImVec2(playheadX, barTop + 4),
                          IM_COL32(255, 200, 60, 255));

    // Hit-testable invisible button over the bar for click + drag scrub.
    ImGui::SetCursorScreenPos(ImVec2(barLeft, barTop));
    ImGui::InvisibleButton("##scrub", ImVec2(barW, barH + 14));
    if (ImGui::IsItemActive()) {
        float mouseX = ImGui::GetIO().MousePos.x;
        float t = std::clamp((mouseX - barLeft) / barW, 0.0f, 1.0f);
        m_timelineCurrentTime = t * m_timelineDuration;
        m_timelinePlaying = false;
    }

    ImGui::End();
}
