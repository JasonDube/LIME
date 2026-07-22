#include "ModelingMode.hpp"
#include "EditableMesh.hpp"
#include "Editor/GLBLoader.hpp"
#include <imgui.h>
#include <nfd.h>
#include <iostream>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace eden;

// Local copy of the NFD default-dir helper from ModelingMode.cpp; trivially
// small and avoids exposing the file-static there.
static const char* riggingNfdDefaultDir(const std::string& projectPath) {
    return projectPath.empty() ? nullptr : projectPath.c_str();
}

void ModelingMode::setBindPose() {
    if (!m_ctx.selectedObject || !m_ctx.editableMesh.isValid()) return;

    m_bindPoseOwner = m_ctx.selectedObject;
    const auto& verts = m_ctx.editableMesh.getVerticesData();
    m_bindPoseVerts.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) m_bindPoseVerts[i] = verts[i].position;
    m_bindPoseBonePositions = m_bonePositions;
    m_bindPoseBoneRotations.assign(m_bonePositions.size(), glm::quat(1, 0, 0, 0));
    m_boneWorldRotations    .assign(m_bonePositions.size(), glm::quat(1, 0, 0, 0));

    // Recompute IBMs from the bind-pose bone world positions, walking the
    // hierarchy so child bones correctly capture their parent's rest world
    // transform. (Translation-only rig: localTransform = translate(world_i - world_parent).)
    auto& skel = m_ctx.editableMesh.getSkeleton();
    std::vector<glm::mat4> worldXf(skel.bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < skel.bones.size() && i < m_bindPoseBonePositions.size(); ++i) {
        int p = skel.bones[i].parentIndex;
        glm::vec3 parentPos(0.0f);
        if (p >= 0 && p < static_cast<int>(i) && p < static_cast<int>(m_bindPoseBonePositions.size())) {
            parentPos = m_bindPoseBonePositions[p];
        }
        glm::vec3 localPos = m_bindPoseBonePositions[i] - parentPos;
        skel.bones[i].localTransform = glm::translate(glm::mat4(1.0f), localPos);
        worldXf[i] = glm::translate(glm::mat4(1.0f), m_bindPoseBonePositions[i]);
        skel.bones[i].inverseBindMatrix = glm::inverse(worldXf[i]);
    }

    m_hasBindPose = true;
    std::cout << "[Rigging] Bind pose set: " << m_bindPoseVerts.size() << " verts, "
              << m_bindPoseBonePositions.size() << " bones" << std::endl;
}

void ModelingMode::clearBindPose() {
    m_hasBindPose = false;
    m_bindPoseOwner = nullptr;
    m_bindPoseVerts.clear();
    m_bindPoseBonePositions.clear();
}

// Full flush: clear the selected object's animation + current pose and snap the
// bones AND the mesh back to the bind pose. Fixes "deleted the keys but the pose
// is still stuck" — the pose lives in m_bonePositions/rotations and the deformed
// editable-mesh verts, independently of the keyframes.
void ModelingMode::resetToBindPose() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) { std::cout << "[Reset] No object selected\n"; return; }

    // 1) Drop the animation track + stop/rewind the timeline.
    m_objectAnims.erase(obj);
    m_timelinePlaying = false;
    m_timelineCurrentTime = 0.0f;
    m_timelineLastAppliedTime = -1.0f;
    m_ikDragLeg = -1;

    // 2) Snap bones back to the bind pose (positions to bind, rotations to identity).
    if (m_hasBindPose && m_bindPoseOwner == obj &&
        m_bindPoseBonePositions.size() == m_bonePositions.size()) {
        m_bonePositions = m_bindPoseBonePositions;
        m_boneWorldRotations.assign(m_bonePositions.size(), glm::quat(1, 0, 0, 0));

        // Keep the skeleton's localTransforms in sync so the overlay is at bind too.
        auto& skel = m_ctx.editableMesh.getSkeleton();
        for (size_t b = 0; b < skel.bones.size() && b < m_bonePositions.size(); ++b) {
            int p = skel.bones[b].parentIndex;
            glm::vec3 pp = (p >= 0 && p < static_cast<int>(m_bonePositions.size())) ? m_bonePositions[p] : glm::vec3(0.0f);
            skel.bones[b].localTransform = glm::translate(glm::mat4(1.0f), m_bonePositions[b] - pp);
        }
        // 3) Re-skin the mesh back to the rest verts.
        reskinFromBoneDeltas();
    }

    // 4) Re-plant IK goals/poles at the bind foot positions so IK holds at bind.
    for (auto& leg : m_ikLegs) {
        if (leg.foot >= 0 && leg.foot < static_cast<int>(m_bonePositions.size()))
            leg.goal = m_bonePositions[leg.foot];
        leg.hingeValid = false;  // re-capture the hinge next time it's needed
    }

    std::cout << "[Reset] Cleared animation + pose; snapped to bind pose." << std::endl;
}

void ModelingMode::reskinFromBoneDeltas() {
    if (!m_hasBindPose || m_bindPoseOwner != m_ctx.selectedObject) return;
    if (!m_ctx.editableMesh.isValid()) return;

    const uint32_t vertCount = m_ctx.editableMesh.getVertexCount();
    if (m_bindPoseVerts.size() != vertCount) return;
    if (m_bindPoseBonePositions.size() != m_bonePositions.size()) return;

    const bool haveRots = (m_boneWorldRotations.size() == m_bonePositions.size());

    // Per-bone skin transform (R, T) in world space:
    //   M_skin_b = translate(anim_pos) * mat(anim_rot) * translate(-bind_pos)
    //            = [ anim_rot | anim_pos - anim_rot * bind_pos ]
    // We need this for both LBS and DQS, so precompute once per bone.
    std::vector<glm::quat> R_bone(m_bonePositions.size(), glm::quat(1, 0, 0, 0));
    std::vector<glm::vec3> T_bone(m_bonePositions.size(), glm::vec3(0.0f));
    for (size_t b = 0; b < m_bonePositions.size(); ++b) {
        glm::quat r = haveRots ? m_boneWorldRotations[b] : glm::quat(1, 0, 0, 0);
        R_bone[b] = r;
        T_bone[b] = m_bonePositions[b] - r * m_bindPoseBonePositions[b];
    }

    if (m_useDQS) {
        // Dual-quaternion skinning. Each (R, T) becomes a unit dual quat
        //   real = R, dual = 0.5 * (T as pure quat) * R
        // We blend dual quats by weight (with antipodal sign-fix), normalize
        // by the real-part magnitude, then transform the rest vertex by the
        // resulting (R', T') reconstructed from the blended DQ.
        std::vector<glm::quat> realQ(m_bonePositions.size());
        std::vector<glm::quat> dualQ(m_bonePositions.size());
        for (size_t b = 0; b < m_bonePositions.size(); ++b) {
            realQ[b] = R_bone[b];
            glm::quat tq(0.0f, T_bone[b].x, T_bone[b].y, T_bone[b].z);
            dualQ[b] = 0.5f * (tq * R_bone[b]);
        }

        for (uint32_t vi = 0; vi < vertCount; ++vi) {
            auto& v = m_ctx.editableMesh.getVertex(vi);
            const glm::vec3 vRest = m_bindPoseVerts[vi];

            glm::quat blendedReal(0, 0, 0, 0);
            glm::quat blendedDual(0, 0, 0, 0);
            glm::quat pivotReal(1, 0, 0, 0);
            bool havePivot = false;
            float wSum = 0.0f;

            for (int j = 0; j < 4; ++j) {
                if (v.boneWeights[j] <= 0.0f) continue;
                int b = v.boneIndices[j];
                if (b < 0 || b >= static_cast<int>(m_bonePositions.size())) continue;

                glm::quat r = realQ[b];
                glm::quat d = dualQ[b];
                if (!havePivot) { pivotReal = r; havePivot = true; }
                // Antipodal fix — quats q and -q represent the same rotation
                // but blend differently. Force consistent hemisphere.
                if (glm::dot(pivotReal, r) < 0.0f) { r = -r; d = -d; }

                float w = v.boneWeights[j];
                blendedReal += w * r;
                blendedDual += w * d;
                wSum += w;
            }
            if (wSum <= 0.0f) { v.position = vRest; continue; }

            // Normalize by real magnitude (DQS spec)
            float n = glm::length(blendedReal);
            if (n < 1e-8f) { v.position = vRest; continue; }
            blendedReal /= n;
            blendedDual /= n;

            // Reconstruct (R, T):
            //   T_blend = 2 * (real.w * dual.vec - dual.w * real.vec
            //                  + cross(real.vec, dual.vec))
            const glm::vec3 rv(blendedReal.x, blendedReal.y, blendedReal.z);
            const glm::vec3 dv(blendedDual.x, blendedDual.y, blendedDual.z);
            glm::vec3 t = 2.0f * (blendedReal.w * dv - blendedDual.w * rv + glm::cross(rv, dv));
            v.position = blendedReal * vRest + t;
        }
    } else {
        // Linear blend skinning:
        //   v' = sum_i w_i * (R_i * v_rest + T_i)
        // Identical math to DQS for a single influence; differs only when
        // multiple bones blend (LBS lerps positions, DQS lerps rotations).
        for (uint32_t vi = 0; vi < vertCount; ++vi) {
            auto& v = m_ctx.editableMesh.getVertex(vi);
            glm::vec3 sum(0.0f);
            float wSum = 0.0f;
            const glm::vec3 vRest = m_bindPoseVerts[vi];
            for (int j = 0; j < 4; ++j) {
                if (v.boneWeights[j] <= 0.0f) continue;
                int b = v.boneIndices[j];
                if (b < 0 || b >= static_cast<int>(m_bonePositions.size())) continue;
                glm::vec3 contribution = R_bone[b] * vRest + T_bone[b];
                sum += v.boneWeights[j] * contribution;
                wSum += v.boneWeights[j];
            }
            v.position = (wSum > 0.0f) ? sum * (1.0f / wSum) : vRest;
        }
    }

    // Push to GPU via the in-place fast path so we don't rebuild buffers.
    uint32_t handle = m_bindPoseOwner->getBufferHandle();
    if (handle != UINT32_MAX && handle != 0) {
        std::vector<ModelVertex> verts;
        std::vector<uint32_t> idx;
        m_ctx.editableMesh.triangulate(verts, idx, m_ctx.hiddenFaces);
        applyHeatMapToVerts(verts);
        m_ctx.modelRenderer.updateModelBuffer(handle, verts);
        m_bindPoseOwner->setMeshData(verts, idx);
    }
}

void ModelingMode::exportMultiClipGLB() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) { std::cout << "[Export] No object selected\n"; return; }
    if (!m_hasBindPose || m_bindPoseOwner != obj) { std::cout << "[Export] Set Bind Pose first\n"; return; }
    if (!m_ctx.editableMesh.isValid()) return;
    auto libIt = m_namedClips.find(obj);
    if (libIt == m_namedClips.end() || libIt->second.empty()) { std::cout << "[Export] No saved clips\n"; return; }

    // Bind-pose mesh (same as exportSkinnedAnimatedGLB).
    std::vector<ModelVertex> verts; std::vector<uint32_t> indices;
    m_ctx.editableMesh.triangulate(verts, indices, m_ctx.hiddenFaces);
    if (verts.size() != m_bindPoseVerts.size()) { std::cout << "[Export] Vertex count changed since Set Bind Pose; re-bind\n"; return; }
    for (size_t i = 0; i < verts.size(); ++i) verts[i].position = m_bindPoseVerts[i];

    const auto& heVerts = m_ctx.editableMesh.getVerticesData();
    std::vector<glm::ivec4> boneIdx(heVerts.size());
    std::vector<glm::vec4>  boneWts(heVerts.size());
    for (size_t i = 0; i < heVerts.size(); ++i) { boneIdx[i] = heVerts[i].boneIndices; boneWts[i] = heVerts[i].boneWeights; }

    // Gather every saved clip (only its rigged keys) into the multi-clip
    // format — head positions AND world rotation deltas (without rotations,
    // GPU skinning translates verts but never rotates them around the bone,
    // shearing any limb posed away from bind — e.g. arms out of an A-pose).
    std::vector<GLBLoader::SkinnedAnimClip> clips;
    for (const auto& [name, tr] : libIt->second) {
        GLBLoader::SkinnedAnimClip clip; clip.name = name;
        for (size_t k = 0; k < tr.times.size(); ++k) {
            if (k < tr.bonePositionsPerKey.size() && !tr.bonePositionsPerKey[k].empty()) {
                clip.times.push_back(tr.times[k]);
                clip.boneWorldPosPerKey.push_back(tr.bonePositionsPerKey[k]);
                clip.boneWorldRotPerKey.push_back(
                    k < tr.boneRotationsPerKey.size()
                        ? tr.boneRotationsPerKey[k]
                        : std::vector<glm::quat>(tr.bonePositionsPerKey[k].size(),
                                                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
            }
        }
        if (!clip.times.empty()) clips.push_back(std::move(clip));
    }
    if (clips.empty()) { std::cout << "[Export] Saved clips have no rigged keys\n"; return; }

    const unsigned char* texData = nullptr; int texW = 0, texH = 0;
    if (obj->hasTextureData()) { texData = obj->getTextureData().data(); texW = obj->getTextureWidth(); texH = obj->getTextureHeight(); }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Skinned Model", "glb"}};
    std::string defaultName = obj->getName() + "_clips.glb";
    if (NFD_SaveDialog(&outPath, filters, 1, riggingNfdDefaultDir(m_ctx.projectPath), defaultName.c_str()) != NFD_OKAY) return;
    std::string filepath = outPath; NFD_FreePath(outPath);
    if (filepath.size() < 4 || filepath.substr(filepath.size() - 4) != ".glb") filepath += ".glb";

    bool ok = GLBLoader::saveSkinnedAnimatedMulti(
        filepath, verts, indices, boneIdx, boneWts,
        m_ctx.editableMesh.getSkeleton(), m_bindPoseBonePositions,
        clips, texData, texW, texH, obj->getName());
    std::cout << (ok ? "[Export] OK (" : "[Export] FAILED (") << clips.size() << " clips): " << filepath << std::endl;
}

void ModelingMode::exportSkinnedAnimatedGLB() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) {
        std::cout << "[Export] No object selected" << std::endl;
        return;
    }
    if (!m_hasBindPose || m_bindPoseOwner != obj) {
        std::cout << "[Export] Set Bind Pose first (rigging panel)" << std::endl;
        return;
    }
    if (!m_ctx.editableMesh.isValid()) return;

    // Build bind-pose mesh (positions = m_bindPoseVerts, other attrs from current
    // editable mesh — bind pose was the snapshot of those at "Set Bind Pose" time
    // and the topology hasn't changed since, so cached normals/uvs/colors apply).
    std::vector<ModelVertex> verts;
    std::vector<uint32_t> indices;
    m_ctx.editableMesh.triangulate(verts, indices, m_ctx.hiddenFaces);
    if (verts.size() != m_bindPoseVerts.size()) {
        std::cout << "[Export] Vertex count changed since Set Bind Pose; re-bind first" << std::endl;
        return;
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i].position = m_bindPoseVerts[i];
    }

    // Per-vertex bone indices/weights from the editable mesh.
    const auto& heVerts = m_ctx.editableMesh.getVerticesData();
    std::vector<glm::ivec4> boneIdx(heVerts.size());
    std::vector<glm::vec4>  boneWts(heVerts.size());
    for (size_t i = 0; i < heVerts.size(); ++i) {
        boneIdx[i] = heVerts[i].boneIndices;
        boneWts[i] = heVerts[i].boneWeights;
    }

    // Animation: gather track for this object, if any.
    std::vector<float> animTimes;
    std::vector<std::vector<glm::vec3>> animBonePerKey;
    auto it = m_objectAnims.find(obj);
    if (it != m_objectAnims.end()) {
        const auto& tr = it->second;
        for (size_t k = 0; k < tr.times.size(); ++k) {
            // Only include keys that have a bone snapshot (rigged keys).
            if (k < tr.bonePositionsPerKey.size() && !tr.bonePositionsPerKey[k].empty()) {
                animTimes.push_back(tr.times[k]);
                animBonePerKey.push_back(tr.bonePositionsPerKey[k]);
            }
        }
    }

    // Texture (optional).
    const unsigned char* texData = nullptr;
    int texW = 0, texH = 0;
    if (obj->hasTextureData()) {
        texData = obj->getTextureData().data();
        texW = obj->getTextureWidth();
        texH = obj->getTextureHeight();
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"GLB Skinned Model", "glb"}};
    std::string defaultName = obj->getName() + "_skinned.glb";
    if (NFD_SaveDialog(&outPath, filters, 1, riggingNfdDefaultDir(m_ctx.projectPath), defaultName.c_str()) != NFD_OKAY) {
        return;
    }
    std::string filepath = outPath;
    NFD_FreePath(outPath);
    if (filepath.size() < 4 || filepath.substr(filepath.size() - 4) != ".glb") filepath += ".glb";

    bool ok = GLBLoader::saveSkinnedAnimated(
        filepath, verts, indices, boneIdx, boneWts,
        m_ctx.editableMesh.getSkeleton(),
        m_bindPoseBonePositions,
        animTimes, animBonePerKey,
        texData, texW, texH,
        obj->getName(), "Take 001");
    std::cout << (ok ? "[Export] OK: " : "[Export] FAILED: ") << filepath << std::endl;
}

void ModelingMode::applyHeatMapToVerts(std::vector<ModelVertex>& verts) {
    if (!m_showWeightHeatMap || m_selectedBone < 0) return;
    if (!m_ctx.editableMesh.isValid()) return;

    const int bone = m_selectedBone;
    const uint32_t vc = m_ctx.editableMesh.getVertexCount();
    if (verts.size() != vc) return;

    for (uint32_t vi = 0; vi < vc; ++vi) {
        const auto& heV = m_ctx.editableMesh.getVertex(vi);
        float w = 0.0f;
        for (int j = 0; j < 4; ++j) {
            if (heV.boneIndices[j] == bone) {
                w = heV.boneWeights[j];
                break;
            }
        }

        // Blue → green → red heat ramp.
        glm::vec3 col;
        if (w < 0.5f) {
            col = glm::mix(glm::vec3(0.05f, 0.05f, 0.7f),
                           glm::vec3(0.0f,  0.7f,  0.0f), w * 2.0f);
        } else {
            col = glm::mix(glm::vec3(0.0f,  0.7f,  0.0f),
                           glm::vec3(1.0f,  0.0f,  0.0f), (w - 0.5f) * 2.0f);
        }
        verts[vi].color = glm::vec4(col, 1.0f);
    }
}

void ModelingMode::pushMeshWithHeatMap() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj || !m_ctx.editableMesh.isValid()) return;
    uint32_t handle = obj->getBufferHandle();
    if (handle == UINT32_MAX || handle == 0) return;

    std::vector<ModelVertex> verts;
    std::vector<uint32_t> idx;
    m_ctx.editableMesh.triangulate(verts, idx, m_ctx.hiddenFaces);
    applyHeatMapToVerts(verts);
    m_ctx.modelRenderer.updateModelBuffer(handle, verts);
    obj->setMeshData(verts, idx);
}

void ModelingMode::cancelRiggingMode() {
    m_riggingMode = false;
    m_selectedBone = -1;
    m_placingBone = false;
    m_showSkeleton = false;
    // Restore the view state captured when rigging was entered.
    // Without this the user stays in component+vertex+wireframe, which keeps
    // the per-frame hover picking running on dense meshes.
    m_ctx.objectMode = m_preRiggingObjectMode;
    m_ctx.modelingSelectionMode = m_preRiggingSelectionMode;
    m_ctx.showModelingWireframe = m_preRiggingShowWireframe;
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

            // Draw bone name. "Show Bone Names" is an explicit user toggle, so
            // honor it for every bone — the old numBones<=20 declutter cap hid
            // names on typical humanoid rigs (24 bones).
            if (m_showBoneNames) {
                drawList->AddText(ImVec2(headScreen.x + radius + 3, headScreen.y - 7),
                                  isSelected ? IM_COL32(255, 255, 100, 255) : IM_COL32(200, 200, 200, 180),
                                  skel.bones[i].name.c_str());
            }
        }
    }

    drawList->PopClipRect();
}

// Draw the imported GLB skinned model's skeleton, posed at the current animation
// frame. Unlike drawSkeletonOverlay (which draws the LIME editor skeleton from
// m_bonePositions), this reads the live pose from the skinned model's animation
// player, so the bones move with playback / scrubbing.
void ModelingMode::drawSkinnedSkeletonOverlay(float vpX, float vpY, float vpW, float vpH) {
    if (!m_showSkinnedSkeleton || !m_ctx.selectedObject) return;
    if (!m_ctx.selectedObject->isSkinned()) return;
    uint32_t handle = m_ctx.selectedObject->getSkinnedModelHandle();
    if (handle == UINT32_MAX) return;

    auto* data = m_ctx.skinnedModelRenderer.getModelData(handle);
    if (!data || !data->skeleton) return;
    const Skeleton& skel = *data->skeleton;
    if (skel.bones.empty()) return;

    // Skinning matrices (globalBoneTransform * inverseBindMatrix), model space.
    const std::vector<glm::mat4>& skinMats = data->animPlayer.getBoneMatrices();

    // Recover each joint's world position in mesh/model space:
    //   global = skin * inverse(inverseBind);  jointPos = global[3]
    // If no animation pose has been computed yet, fall back to the bind pose.
    std::vector<glm::vec3> jointPos(skel.bones.size());
    for (size_t i = 0; i < skel.bones.size(); ++i) {
        glm::mat4 global;
        if (i < skinMats.size()) {
            global = skinMats[i] * glm::inverse(skel.bones[i].inverseBindMatrix);
        } else {
            global = glm::inverse(skel.bones[i].inverseBindMatrix);
        }
        jointPos[i] = glm::vec3(global[3]);
    }

    Camera& activeCamera = (m_ctx.splitView && vpX > 0) ? m_ctx.camera2 : m_ctx.camera;
    glm::mat4 view = activeCamera.getViewMatrix();
    glm::mat4 proj = activeCamera.getProjectionMatrix(vpW / vpH);
    glm::mat4 vp = proj * view;
    glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();

    auto worldToScreen = [&](const glm::vec3& p) -> ImVec2 {
        glm::vec4 clip = vp * modelMatrix * glm::vec4(p, 1.0f);
        if (clip.w <= 0.0f) return ImVec2(-1000, -1000);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
    };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    int numBones = static_cast<int>(skel.bones.size());
    for (int i = 0; i < numBones; ++i) {
        ImVec2 headScreen = worldToScreen(jointPos[i]);

        int parentIdx = skel.bones[i].parentIndex;
        if (parentIdx >= 0 && parentIdx < numBones) {
            ImVec2 parentScreen = worldToScreen(jointPos[parentIdx]);
            if (headScreen.x > -500 && parentScreen.x > -500) {
                dl->AddLine(headScreen, parentScreen, IM_COL32(120, 230, 160, 220), 2.0f);
            }
        }

        if (headScreen.x > -500) {
            dl->AddCircleFilled(headScreen, 4.0f, IM_COL32(0, 0, 0, 200));
            dl->AddCircleFilled(headScreen, 3.0f, IM_COL32(80, 255, 140, 235));
            if (m_showBoneNames && numBones <= 40) {
                dl->AddText(ImVec2(headScreen.x + 5, headScreen.y - 7),
                            IM_COL32(150, 255, 190, 200), skel.bones[i].name.c_str());
            }
        }
    }

    dl->PopClipRect();
}

// ============================ Two-bone leg IK ============================

// Rotation that swings unit vector a onto unit vector b (shortest arc, no twist).
static glm::quat swingRotation(glm::vec3 a, glm::vec3 b) {
    float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) return glm::quat(1, 0, 0, 0);
    a /= la; b /= lb;
    float d = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f) {  // opposite — pick any perpendicular axis
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), a);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0, 1, 0), a);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}

// Analytic two-bone IK. root=hip, goal=foot target, L1=thigh len, L2=shin len,
// poleHint = a point on the knee-bend side. Out: knee + ankle positions. If the
// goal is out of reach the leg straightens toward it (ankle clamped to reach).
static void solveTwoBone(const glm::vec3& root, const glm::vec3& goal,
                         float L1, float L2, const glm::vec3& poleHint,
                         glm::vec3& outKnee, glm::vec3& outAnkle) {
    glm::vec3 toGoal = goal - root;
    float dist = glm::length(toGoal);
    glm::vec3 dir = (dist > 1e-6f) ? (toGoal / dist) : glm::vec3(0, -1, 0);
    float dmin = std::fabs(L1 - L2) + 1e-4f;
    float dmax = L1 + L2 - 1e-4f;
    float d = glm::clamp(dist, dmin, dmax);
    outAnkle = root + dir * d;  // ankle clamped to the reachable point along dir
    float a = (d * d + L1 * L1 - L2 * L2) / (2.0f * d);
    float h = std::sqrt(std::max(0.0f, L1 * L1 - a * a));
    glm::vec3 bend = (poleHint - root);
    bend = bend - dir * glm::dot(bend, dir);  // perpendicular component
    if (glm::length(bend) < 1e-5f) {
        glm::vec3 up(0, 1, 0);
        bend = up - dir * glm::dot(up, dir);
        if (glm::length(bend) < 1e-5f) bend = glm::vec3(1, 0, 0);
    }
    bend = glm::normalize(bend);
    outKnee = root + dir * a + bend * h;
}

void ModelingMode::addIKLegFromSelected() {
    if (m_selectedBone < 0) { std::cout << "[IK] Select the FOOT/ankle bone first\n"; return; }
    const auto& skel = m_ctx.editableMesh.getSkeleton();
    int foot = m_selectedBone;
    if (foot >= static_cast<int>(skel.bones.size())) return;
    int shin = skel.bones[foot].parentIndex;
    if (shin < 0) { std::cout << "[IK] Foot bone has no parent (need shin)\n"; return; }
    int thigh = skel.bones[shin].parentIndex;
    if (thigh < 0) { std::cout << "[IK] Shin bone has no parent (need thigh)\n"; return; }

    IKLeg leg;
    leg.thigh = thigh; leg.shin = shin; leg.foot = foot;
    if (foot < static_cast<int>(m_bonePositions.size())) leg.goal = m_bonePositions[foot];  // plant at current ankle
    // Place the pole out in front of the knee, along the current bend direction, so
    // it steers the knee where it already points (and is easy to grab).
    if (thigh < static_cast<int>(m_bonePositions.size()) && foot < static_cast<int>(m_bonePositions.size())) {
        glm::vec3 hipP = m_bonePositions[thigh], kneeP = m_bonePositions[shin], ankP = m_bonePositions[foot];
        float legLen = glm::length(kneeP - hipP) + glm::length(ankP - kneeP);
        glm::vec3 bend = kneeP - 0.5f * (hipP + ankP);
        if (glm::length(bend) < 1e-4f) bend = glm::vec3(0, 0, 1);  // straight leg fallback: forward
        leg.pole = kneeP + glm::normalize(bend) * std::max(0.3f, legLen * 0.6f);
    }
    leg.active = true;
    m_ikLegs.push_back(leg);
    m_ikEnabled = true;
    std::cout << "[IK] Added leg: " << skel.bones[thigh].name << " -> "
              << skel.bones[shin].name << " -> " << skel.bones[foot].name << "\n";
}

void ModelingMode::solveIKLegs() {
    if (!m_ikEnabled || m_ikLegs.empty()) return;
    if (m_bindPoseBonePositions.size() != m_bonePositions.size()) return;  // need a bind pose
    if (m_boneWorldRotations.size() != m_bonePositions.size())
        m_boneWorldRotations.assign(m_bonePositions.size(), glm::quat(1, 0, 0, 0));

    bool any = false;
    for (IKLeg& leg : m_ikLegs) {
        if (!leg.active) continue;
        if (leg.thigh < 0 || leg.shin < 0 || leg.foot < 0) continue;
        if (leg.foot >= static_cast<int>(m_bonePositions.size())) continue;

        const glm::vec3 hip = m_bonePositions[leg.thigh];   // driven by the pelvis (FK)
        const glm::vec3 hipR  = m_bindPoseBonePositions[leg.thigh];
        const glm::vec3 kneeR = m_bindPoseBonePositions[leg.shin];
        const glm::vec3 ankR  = m_bindPoseBonePositions[leg.foot];
        const float L1 = glm::length(kneeR - hipR);
        const float L2 = glm::length(ankR - kneeR);
        if (L1 < 1e-5f || L2 < 1e-5f) continue;

        // Bend direction source. Locked: derive from a fixed hinge axis so the
        // knee folds the same anatomical way as the leg swings (kicks) and can
        // never invert. Unlocked: the draggable pole point steers it.
        glm::vec3 poleHint = leg.pole;
        if (leg.lockKnee) {
            if (!leg.hingeValid) {
                // Capture the current bend plane as the hinge axis.
                glm::vec3 la0 = glm::normalize(m_bonePositions[leg.foot] - hip);
                glm::vec3 kc = m_bonePositions[leg.shin];
                glm::vec3 bend0 = kc - (hip + la0 * glm::dot(kc - hip, la0));
                if (glm::length(bend0) > 1e-4f) {
                    leg.hingeAxis = glm::normalize(glm::cross(la0, glm::normalize(bend0)));
                    leg.hingeValid = true;
                }
            }
            if (leg.hingeValid) {
                glm::vec3 la = leg.goal - hip;
                if (glm::length(la) > 1e-5f) {
                    la = glm::normalize(la);
                    glm::vec3 bendDir = glm::cross(leg.hingeAxis, la);  // consistent side of the leg
                    if (glm::length(bendDir) > 1e-5f)
                        poleHint = hip + glm::normalize(bendDir) * (L1 + L2);
                }
            }
        }

        glm::vec3 knee, ankle;
        solveTwoBone(hip, leg.goal, L1, L2, poleHint, knee, ankle);

        m_bonePositions[leg.shin] = knee;
        m_bonePositions[leg.foot] = ankle;
        m_boneWorldRotations[leg.thigh] = swingRotation(glm::normalize(kneeR - hipR), glm::normalize(knee - hip));
        m_boneWorldRotations[leg.shin]  = swingRotation(glm::normalize(ankR - kneeR), glm::normalize(ankle - knee));

        // Foot follows the shin, with Foot Pitch applied in the leg's OWN frame
        // (right-multiplied). Right-multiplication commutes with slerp, so when
        // the keyed foot rotation is interpolated it provably stays glued to the
        // shin's path — no flailing mid-blend. Pitch axis = the hinge axis (the
        // leg's left/right) when captured, else model X.
        glm::quat shinRot = m_boneWorldRotations[leg.shin];
        glm::vec3 localPitchAxis = leg.hingeValid ? leg.hingeAxis : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::quat footRot = shinRot * glm::angleAxis(glm::radians(leg.footPitch), localPitchAxis);
        m_boneWorldRotations[leg.foot] = footRot;

        // Carry the foot's DOWNSTREAM joints (heel, toe, ...) with the ankle,
        // rotating them around it by footRot so the whole foot pitches together.
        for (int d : getDescendantBones(leg.foot)) {
            if (d >= 0 && d < static_cast<int>(m_bonePositions.size()) &&
                d < static_cast<int>(m_bindPoseBonePositions.size())) {
                m_bonePositions[d] = ankle + footRot * (m_bindPoseBonePositions[d] - ankR);
                if (d < static_cast<int>(m_boneWorldRotations.size()))
                    m_boneWorldRotations[d] = footRot;
            }
        }
        any = true;
    }
    if (!any) return;

    // Keep skeleton localTransforms in sync so the overlay bone lines follow.
    auto& skel = m_ctx.editableMesh.getSkeleton();
    for (size_t b = 0; b < skel.bones.size() && b < m_bonePositions.size(); ++b) {
        int p = skel.bones[b].parentIndex;
        glm::vec3 pp = (p >= 0 && p < static_cast<int>(m_bonePositions.size())) ? m_bonePositions[p] : glm::vec3(0.0f);
        skel.bones[b].localTransform = glm::translate(glm::mat4(1.0f), m_bonePositions[b] - pp);
    }
    reskinFromBoneDeltas();
}

// Playback fix: after keyframe interpolation, rebuild each IK leg so its bone
// ROTATIONS are derived from its interpolated POSITIONS (not separately slerped).
// When position and rotation interpolate independently they disagree mid-blend,
// so the shin's verts get rotated to a spot that doesn't line up with the ankle
// — the mesh stretches to fill the gap and the joint SWELLS. Deriving rotations
// from the (length-corrected) positions keeps them consistent = no swell, no
// flail. Foot follows the shin + Foot Pitch; heel/toe ride rigidly.
void ModelingMode::rederiveIKFeet() {
    if (m_ikLegs.empty()) return;
    if (m_boneWorldRotations.size() != m_bonePositions.size()) return;
    if (m_bindPoseBonePositions.size() != m_bonePositions.size()) return;
    const size_t n = m_bonePositions.size();
    for (size_t li = 0; li < m_ikLegs.size(); ++li) {
        const IKLeg& leg = m_ikLegs[li];
        if (leg.thigh < 0 || leg.shin < 0 || leg.foot < 0) continue;
        if (leg.thigh >= static_cast<int>(n) || leg.shin >= static_cast<int>(n) ||
            leg.foot >= static_cast<int>(n)) continue;

        const glm::vec3 hip = m_bonePositions[leg.thigh];
        const glm::vec3 knee = m_bonePositions[leg.shin];
        const glm::vec3 ankle = m_bonePositions[leg.foot];
        const glm::vec3 hipR  = m_bindPoseBonePositions[leg.thigh];
        const glm::vec3 kneeR = m_bindPoseBonePositions[leg.shin];
        const glm::vec3 ankR  = m_bindPoseBonePositions[leg.foot];

        // Thigh + shin rotations straight from the interpolated joint directions.
        m_boneWorldRotations[leg.thigh] = swingRotation(glm::normalize(kneeR - hipR), glm::normalize(knee - hip));
        glm::quat shinRot = swingRotation(glm::normalize(ankR - kneeR), glm::normalize(ankle - knee));
        m_boneWorldRotations[leg.shin] = shinRot;

        // Foot follows the shin. Its local rotation is the PER-KEY foot pose
        // interpolated from the keyframes (m_ikLocalFootRot); if unavailable,
        // fall back to the single Foot Pitch slider value.
        glm::quat footLocal;
        if (m_ikHaveLocalFoot && li < m_ikLocalFootRot.size()) {
            footLocal = m_ikLocalFootRot[li];
        } else {
            glm::vec3 localAxis = leg.hingeValid ? leg.hingeAxis : glm::vec3(1.0f, 0.0f, 0.0f);
            footLocal = glm::angleAxis(glm::radians(leg.footPitch), localAxis);
        }
        glm::quat footRot = shinRot * footLocal;
        m_boneWorldRotations[leg.foot] = footRot;

        // Heel/toe ride rigidly on the ankle.
        for (int d : getDescendantBones(leg.foot)) {
            if (d >= 0 && d < static_cast<int>(n) && d < static_cast<int>(m_bindPoseBonePositions.size())) {
                m_bonePositions[d] = ankle + footRot * (m_bindPoseBonePositions[d] - ankR);
                if (d < static_cast<int>(m_boneWorldRotations.size()))
                    m_boneWorldRotations[d] = footRot;
            }
        }
    }
}

int ModelingMode::pickIKGoalAtScreenPos(const glm::vec2& screenPos, float threshold) {
    if (m_ikLegs.empty() || !m_ctx.selectedObject) return -1;
    Camera& cam = m_ctx.getActiveCamera();
    float vpW = static_cast<float>(m_ctx.window.getWidth());
    float vpH = static_cast<float>(m_ctx.window.getHeight());
    glm::mat4 vp = cam.getProjectionMatrix(vpW / vpH) * cam.getViewMatrix();
    glm::mat4 model = m_ctx.selectedObject->getTransform().getMatrix();

    float best = threshold; int bestLeg = -1;
    for (size_t i = 0; i < m_ikLegs.size(); ++i) {
        if (!m_ikLegs[i].active) continue;
        glm::vec4 clip = vp * model * glm::vec4(m_ikLegs[i].goal, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 sp((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);
        float dsq = glm::length(sp - screenPos);
        if (dsq < best) { best = dsq; bestLeg = static_cast<int>(i); }
    }
    return bestLeg;
}

int ModelingMode::pickIKPoleAtScreenPos(const glm::vec2& screenPos, float threshold) {
    if (m_ikLegs.empty() || !m_ctx.selectedObject) return -1;
    Camera& cam = m_ctx.getActiveCamera();
    float vpW = static_cast<float>(m_ctx.window.getWidth());
    float vpH = static_cast<float>(m_ctx.window.getHeight());
    glm::mat4 vp = cam.getProjectionMatrix(vpW / vpH) * cam.getViewMatrix();
    glm::mat4 model = m_ctx.selectedObject->getTransform().getMatrix();

    float best = threshold; int bestLeg = -1;
    for (size_t i = 0; i < m_ikLegs.size(); ++i) {
        if (!m_ikLegs[i].active) continue;
        glm::vec4 clip = vp * model * glm::vec4(m_ikLegs[i].pole, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 sp((ndc.x + 1.0f) * 0.5f * vpW, (1.0f - ndc.y) * 0.5f * vpH);
        float dsq = glm::length(sp - screenPos);
        if (dsq < best) { best = dsq; bestLeg = static_cast<int>(i); }
    }
    return bestLeg;
}

void ModelingMode::drawIKGoalsOverlay(float vpX, float vpY, float vpW, float vpH) {
    if (!m_ikEnabled || m_ikLegs.empty() || !m_ctx.selectedObject) return;
    Camera& cam = (m_ctx.splitView && vpX > 0) ? m_ctx.camera2 : m_ctx.camera;
    glm::mat4 vp = cam.getProjectionMatrix(vpW / vpH) * cam.getViewMatrix();
    glm::mat4 model = m_ctx.selectedObject->getTransform().getMatrix();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);
    auto project = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = vp * model * glm::vec4(p, 1.0f);
        if (clip.w <= 0.0f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2(vpX + (ndc.x + 1.0f) * 0.5f * vpW, vpY + (1.0f - ndc.y) * 0.5f * vpH);
        return true;
    };
    for (size_t i = 0; i < m_ikLegs.size(); ++i) {
        const IKLeg& leg = m_ikLegs[i];
        if (!leg.active) continue;
        bool draggingThis = (static_cast<int>(i) == m_ikDragLeg);

        // --- knee -> pole guide line + blue pole diamond (steers knee direction) ---
        ImVec2 kneeSp, poleSp;
        bool haveKnee = (leg.shin >= 0 && leg.shin < static_cast<int>(m_bonePositions.size()))
                        && project(m_bonePositions[leg.shin], kneeSp);
        if (project(leg.pole, poleSp)) {
            if (haveKnee) dl->AddLine(kneeSp, poleSp, IM_COL32(80, 140, 255, 160), 1.5f);
            bool dragPole = draggingThis && m_ikDragIsPole;
            ImU32 pf = dragPole ? IM_COL32(150, 190, 255, 255) : IM_COL32(70, 130, 240, 220);
            ImVec2 d[4] = { {poleSp.x, poleSp.y - 9}, {poleSp.x + 9, poleSp.y}, {poleSp.x, poleSp.y + 9}, {poleSp.x - 9, poleSp.y} };
            dl->AddConvexPolyFilled(d, 4, pf);
            dl->AddPolyline(d, 4, IM_COL32(0, 0, 0, 220), ImDrawFlags_Closed, 2.0f);
        }

        // --- green foot goal handle ---
        ImVec2 sp;
        if (!project(leg.goal, sp)) continue;
        bool dragGoal = draggingThis && !m_ikDragIsPole;
        ImU32 fill = dragGoal ? IM_COL32(120, 255, 150, 255) : IM_COL32(40, 220, 90, 220);
        dl->AddRectFilled(ImVec2(sp.x - 9, sp.y - 9), ImVec2(sp.x + 9, sp.y + 9), fill, 3.0f);
        dl->AddRect(ImVec2(sp.x - 10, sp.y - 10), ImVec2(sp.x + 10, sp.y + 10), IM_COL32(0, 0, 0, 220), 3.0f, 0, 2.0f);
        dl->AddLine(ImVec2(sp.x - 14, sp.y), ImVec2(sp.x + 14, sp.y), IM_COL32(20, 120, 50, 200), 1.5f);
        dl->AddLine(ImVec2(sp.x, sp.y - 14), ImVec2(sp.x, sp.y + 14), IM_COL32(20, 120, 50, 200), 1.5f);
    }
    dl->PopClipRect();
}
