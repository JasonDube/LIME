#include "ModelingMode.hpp"
#include "EditableMesh.hpp"
#include "Editor/GLBLoader.hpp"
#include <imgui.h>
#include <nfd.h>
#include <iostream>

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

void ModelingMode::reskinFromBoneDeltas() {
    if (!m_hasBindPose || m_bindPoseOwner != m_ctx.selectedObject) return;
    if (!m_ctx.editableMesh.isValid()) return;

    const uint32_t vertCount = m_ctx.editableMesh.getVertexCount();
    if (m_bindPoseVerts.size() != vertCount) return;
    if (m_bindPoseBonePositions.size() != m_bonePositions.size()) return;

    // Translation-only skinning: deformed = rest + sum(weight_i * (bone_i - bind_bone_i))
    // Matches LIME's existing translate-by-weighted-delta deform model and
    // is what GPU skinning will compute when bones have only translation.
    for (uint32_t vi = 0; vi < vertCount; ++vi) {
        auto& v = m_ctx.editableMesh.getVertex(vi);
        glm::vec3 delta(0.0f);
        for (int j = 0; j < 4; ++j) {
            if (v.boneWeights[j] <= 0.0f) continue;
            int b = v.boneIndices[j];
            if (b < 0 || b >= static_cast<int>(m_bonePositions.size())) continue;
            delta += v.boneWeights[j] * (m_bonePositions[b] - m_bindPoseBonePositions[b]);
        }
        v.position = m_bindPoseVerts[vi] + delta;
    }

    // Push to GPU via the in-place fast path so we don't rebuild buffers.
    uint32_t handle = m_bindPoseOwner->getBufferHandle();
    if (handle != UINT32_MAX && handle != 0) {
        std::vector<ModelVertex> verts;
        std::vector<uint32_t> idx;
        m_ctx.editableMesh.triangulate(verts, idx, m_ctx.hiddenFaces);
        m_ctx.modelRenderer.updateModelBuffer(handle, verts);
        m_bindPoseOwner->setMeshData(verts, idx);
    }
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
