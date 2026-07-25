#include "ModelingMode.hpp"
#include "EditableMesh.hpp"
#include "Editor/SkinnedGLBLoader.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_map>

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

    // For rigged objects with a bind pose, snapshot bone positions AND
    // world-space rotations. Verts are recomputed on playback via skinning
    // from the bind pose, so per-key memory is O(bones) instead of O(verts).
    bool snapshotRig = m_hasBindPose && m_bindPoseOwner == obj &&
                       obj == m_ctx.selectedObject && m_ctx.editableMesh.isValid();
    std::vector<glm::vec3> bonesSnap;
    std::vector<glm::quat> rotsSnap;
    if (snapshotRig) {
        bonesSnap = m_bonePositions;
        rotsSnap  = m_boneWorldRotations;
        // Pad with identities if rotation tracking wasn't initialized
        // (e.g. bind pose was set before this feature shipped).
        if (rotsSnap.size() != bonesSnap.size()) {
            rotsSnap.assign(bonesSnap.size(), glm::quat(1, 0, 0, 0));
        }
    }

    // Find the slot for this time. If a key already exists within a tiny
    // epsilon, replace it; otherwise insert sorted.
    auto it = std::lower_bound(track.times.begin(), track.times.end(), t);
    size_t idx = static_cast<size_t>(it - track.times.begin());

    auto resizeAligned = [&](size_t target) {
        if (track.bonePositionsPerKey.size() < target) track.bonePositionsPerKey.resize(target);
        if (track.boneRotationsPerKey.size() < target) track.boneRotationsPerKey.resize(target);
    };

    bool replace = (idx < track.times.size() && std::abs(track.times[idx] - t) < 1e-4f);
    if (replace) {
        track.positions[idx] = tf.getPosition();
        track.rotations[idx] = tf.getRotation();
        track.scales[idx]    = tf.getScale();
        if (snapshotRig) {
            resizeAligned(track.times.size());
            track.bonePositionsPerKey[idx] = std::move(bonesSnap);
            track.boneRotationsPerKey[idx] = std::move(rotsSnap);
        }
    } else {
        track.times.insert(track.times.begin() + idx, t);
        track.positions.insert(track.positions.begin() + idx, tf.getPosition());
        track.rotations.insert(track.rotations.begin() + idx, tf.getRotation());
        track.scales.insert(track.scales.begin() + idx, tf.getScale());
        if (snapshotRig) {
            resizeAligned(track.times.size() - 1);
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx, std::move(bonesSnap));
            track.boneRotationsPerKey.insert(track.boneRotationsPerKey.begin() + idx, std::move(rotsSnap));
        } else if (!track.bonePositionsPerKey.empty() || !track.boneRotationsPerKey.empty()) {
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx, std::vector<glm::vec3>{});
            track.boneRotationsPerKey.insert(track.boneRotationsPerKey.begin() + idx, std::vector<glm::quat>{});
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
    if (best < track.boneRotationsPerKey.size())
        track.boneRotationsPerKey.erase(track.boneRotationsPerKey.begin() + best);
    if (track.times.empty()) m_objectAnims.erase(found);
}

// Import a pose2anim *.limeanim.json (per-frame bone HEAD positions) onto the
// selected rigged object. Rung 1 = positions only: boneRotationsPerKey is left
// empty so playback takes the identity-rotation path and re-skins from bind pose.
// Bones are matched to the current rig BY NAME (case-insensitive); unmatched rig
// bones hold their current position so nothing collapses to the origin.
// Precondition for *seeing* it play: Set Bind Pose on this object first.
void ModelingMode::importBoneAnimationJSON(const std::string& path) {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) { std::cout << "[ImportAnim] No object selected\n"; return; }
    if (!m_ctx.editableMesh.isValid() || !m_ctx.editableMesh.hasSkeleton()) {
        std::cout << "[ImportAnim] Selected object has no skeleton — Load/Add a skeleton first\n";
        return;
    }

    std::ifstream file(path);
    if (!file.is_open()) { std::cout << "[ImportAnim] Could not open " << path << "\n"; return; }
    nlohmann::json j;
    try { file >> j; }
    catch (const std::exception& e) { std::cout << "[ImportAnim] JSON parse error: " << e.what() << "\n"; return; }

    if (!j.contains("frames") || !j.contains("skeleton_bones")) {
        std::cout << "[ImportAnim] Not a LIMEANIM file (missing 'frames'/'skeleton_bones')\n";
        return;
    }

    std::vector<std::string> animNames = j["skeleton_bones"].get<std::vector<std::string>>();
    const Skeleton& skel = m_ctx.editableMesh.getSkeleton();
    const size_t rigCount = skel.bones.size();

    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c)); return s; };

    // rig bone index -> column in the anim file (by name)
    std::vector<int> rigToCol(rigCount, -1);
    int matched = 0;
    for (size_t b = 0; b < rigCount; ++b) {
        std::string rn = lower(skel.bones[b].name);
        for (size_t c = 0; c < animNames.size(); ++c) {
            if (lower(animNames[c]) == rn) { rigToCol[b] = static_cast<int>(c); ++matched; break; }
        }
    }
    if (matched == 0) {
        std::cout << "[ImportAnim] No bone names matched between the anim file and the current rig\n";
        return;
    }

    // Fallback positions for unmatched bones = current editor bone positions.
    std::vector<glm::vec3> restPos = m_bonePositions;
    if (restPos.size() != rigCount) restPos.assign(rigCount, glm::vec3(0.0f));

    ObjectAnimTrack track;
    const glm::vec3 tp = obj->getTransform().getPosition();
    const glm::quat tr = obj->getTransform().getRotation();
    const glm::vec3 ts = obj->getTransform().getScale();

    // Rung 2: LIMEANIM v2 files also carry per-bone WORLD rotation deltas
    // (quat wxyz) — the twist/roll that positions can't encode. When present we
    // fill boneRotationsPerKey so playback slerps them; unmatched rig bones get
    // identity.
    bool hasRot = !j["frames"].empty() && j["frames"][0].contains("bone_rotations");
    int matchedRot = 0;

    for (const auto& jf : j["frames"]) {
        float t = jf.value("time", 0.0f);
        std::vector<glm::vec3> bones(rigCount, glm::vec3(0.0f));
        const auto& bp = jf["bone_positions"];
        for (size_t b = 0; b < rigCount; ++b) {
            int c = rigToCol[b];
            if (c >= 0 && c < static_cast<int>(bp.size()) && bp[c].size() >= 3) {
                bones[b] = glm::vec3(bp[c][0].get<float>(), bp[c][1].get<float>(), bp[c][2].get<float>());
            } else {
                bones[b] = restPos[b];
            }
        }
        track.times.push_back(t);
        track.positions.push_back(tp);
        track.rotations.push_back(tr);
        track.scales.push_back(ts);
        track.bonePositionsPerKey.push_back(std::move(bones));

        if (hasRot && jf.contains("bone_rotations")) {
            const auto& br = jf["bone_rotations"];
            std::vector<glm::quat> rots(rigCount, glm::quat(1, 0, 0, 0));
            for (size_t b = 0; b < rigCount; ++b) {
                int c = rigToCol[b];
                if (c >= 0 && c < static_cast<int>(br.size()) && br[c].size() >= 4) {
                    // stored wxyz -> glm::quat(w, x, y, z)
                    rots[b] = glm::normalize(glm::quat(br[c][0].get<float>(), br[c][1].get<float>(),
                                                       br[c][2].get<float>(), br[c][3].get<float>()));
                    ++matchedRot;
                }
            }
            track.boneRotationsPerKey.push_back(std::move(rots));
        } else {
            track.boneRotationsPerKey.emplace_back();  // positions-only (Rung 1)
        }
    }
    if (hasRot) std::cout << "[ImportAnim] rotations: " << (matchedRot / std::max<size_t>(1, track.times.size()))
                          << " bones/frame\n";

    if (track.times.empty()) { std::cout << "[ImportAnim] File had no frames\n"; return; }

    m_objectAnims[obj] = std::move(track);
    m_timelineDuration = std::max(m_timelineDuration, m_objectAnims[obj].times.back());
    m_timelineCurrentTime = 0.0f;
    m_timelineLastAppliedTime = -1.0f;  // force re-apply on next tick
    m_stancePristine.erase(obj); m_stanceWidth = 0.0f;  // fresh anim -> stance slider resets
    m_feetPristine.erase(obj); m_plantFeet = 0.0f;       // and the plant-feet slider
    m_shoulderPristine.erase(obj); m_relaxShoulders = 0.0f; m_rollPristine.erase(obj); m_footRoll = 0.0f;

    // Body-part -> rig-bone map from the retargeter, so correction sliders find
    // the right bones on any rig (esp. anonymous UniRig bone_N names).
    m_correctionBones.erase(obj);
    if (j.contains("correction_bones")) {
        for (auto& [role, bname] : j["correction_bones"].items()) {
            std::string want = lower(bname.get<std::string>());
            for (size_t b = 0; b < rigCount; ++b)
                if (lower(skel.bones[b].name) == want) { m_correctionBones[obj][role] = static_cast<int>(b); break; }
        }
        std::cout << "[ImportAnim] correction_bones: " << m_correctionBones[obj].size() << " roles mapped\n";
    }

    std::cout << "[ImportAnim] " << m_objectAnims[obj].times.size() << " keyframes, "
              << matched << "/" << rigCount << " bones matched, duration "
              << m_objectAnims[obj].times.back() << "s, from " << path << "\n";
    if (!(m_hasBindPose && m_bindPoseOwner == obj))
        std::cout << "[ImportAnim] NOTE: Set Bind Pose on this object to see the animation play/deform.\n";
}

// Probe the chosen video for total frame count + fps (via ffprobe) so the
// clip-range UI can work in FRAMES and validate the range up front.
void ModelingMode::probeVideoInfo() {
    m_vid2animTotalFrames = 0;
    m_vid2animFps = 0.0;
    if (m_vid2animVideoPath.empty()) return;
    std::string cmd =
        "ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate "
        "-of csv=p=0 '" + m_vid2animVideoPath + "' 2>/dev/null; "
        "ffprobe -v error -show_entries format=duration -of csv=p=0 '"
        + m_vid2animVideoPath + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return;
    char buf[128];
    double fps = 0.0, dur = 0.0;
    if (fgets(buf, sizeof(buf), pipe)) {   // r_frame_rate, e.g. "24/1"
        int a = 0, b = 1;
        if (std::sscanf(buf, "%d/%d", &a, &b) >= 1 && b != 0) fps = static_cast<double>(a) / b;
    }
    if (fgets(buf, sizeof(buf), pipe)) dur = std::atof(buf);  // duration seconds
    pclose(pipe);
    m_vid2animFps = fps > 0 ? fps : 24.0;
    m_vid2animTotalFrames = static_cast<int>(dur * m_vid2animFps + 0.5);
    if (m_vid2animEndFrame <= 0 && m_vid2animTotalFrames > 0)
        m_vid2animEndFrame = m_vid2animTotalFrames - 1;
}

// Spawn the pose2anim clip2anim pipeline (GVHMR local 3D mocap + rotation
// retarget) on the chosen video, in a detached worker so the UI stays live.
// The rig GLB is taken from the selected object (or the currently open file);
// completion is polled in update(), which auto-imports the resulting JSON.
void ModelingMode::launchVideoToAnim() {
    if (m_vid2animRunning) return;

    std::string glb;
    if (m_ctx.selectedObject && !m_ctx.selectedObject->getModelPath().empty())
        glb = m_ctx.selectedObject->getModelPath();
    else
        glb = m_ctx.currentFilePath;
    if (glb.empty() || m_vid2animVideoPath.empty()) {
        std::cout << "[Vid2Anim] Need an imported rig GLB and a video first\n";
        return;
    }

    const char* homeEnv = std::getenv("HOME");
    std::string script = std::string(homeEnv ? homeEnv : "") + "/Desktop/pose2anim/clip2anim.sh";
    if (!std::filesystem::exists(script)) {
        std::cout << "[Vid2Anim] Pipeline script not found: " << script << "\n";
        return;
    }

    std::filesystem::path vp(m_vid2animVideoPath);
    std::string outName = vp.stem().string();
    std::string rangeArgs;
    if (m_vid2animUseRange) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %d %d", m_vid2animStartFrame, m_vid2animEndFrame);
        rangeArgs = buf;
        char tag[48];
        std::snprintf(tag, sizeof(tag), "_f%d-%d", m_vid2animStartFrame, m_vid2animEndFrame);
        outName += tag;
    }
    std::string out = (vp.parent_path() / (outName + ".limeanim.json")).string();

    std::string cmd = "'" + script + "' '" + m_vid2animVideoPath + "' '" + glb + "' '" + out + "'"
                    + rangeArgs + " > /tmp/clip2anim_lime.log 2>&1";

    m_vid2animOutJson = out;
    m_vid2animRunning = true;
    m_vid2animDone = false;
    m_vid2animOk = false;
    m_vid2animStatus = "Generating from " + vp.filename().string()
                     + (m_vid2animUseRange ? " (frames " + std::to_string(m_vid2animStartFrame)
                                             + ".." + std::to_string(m_vid2animEndFrame) + ")"
                                           : " (whole clip)") + "...";
    std::cout << "[Vid2Anim] Running: " << cmd << "\n";

    std::thread([this, cmd, out]() {
        int rc = std::system(cmd.c_str());
        m_vid2animOk = (rc == 0) && std::filesystem::exists(out);
        m_vid2animDone = true;
    }).detach();
}

// Auto-Rig: run the mesh2rig pipeline (UniRig skeleton + skinning, local GPU)
// on the selected object's source mesh in a detached worker. Completion is
// polled in update(), which reloads the rigged GLB via m_ctx.requestLoadModel.
void ModelingMode::launchAutoRig() {
    if (m_autoRigRunning || m_vid2animRunning) return;

    std::string mesh;
    if (m_ctx.selectedObject && !m_ctx.selectedObject->getModelPath().empty())
        mesh = m_ctx.selectedObject->getModelPath();
    else
        mesh = m_ctx.currentFilePath;
    if (mesh.empty()) {
        m_autoRigStatus = "FAILED: no source mesh file known (open the model first)";
        return;
    }

    const char* homeEnv = std::getenv("HOME");
    std::string script = std::string(homeEnv ? homeEnv : "") + "/Desktop/pose2anim/mesh2rig.sh";
    if (!std::filesystem::exists(script)) {
        m_autoRigStatus = "FAILED: pipeline script not found: " + script;
        return;
    }

    std::filesystem::path mp(mesh);
    std::string out = (mp.parent_path() / (mp.stem().string() + "_rigged.glb")).string();
    std::string cmd = "'" + script + "' '" + mesh + "' '" + out + "' > /tmp/mesh2rig_lime.log 2>&1";

    m_autoRigOut = out;
    m_autoRigSourceName = m_ctx.selectedObject ? m_ctx.selectedObject->getName() : "";
    m_autoRigRunning = true;
    m_autoRigDone = false;
    m_autoRigOk = false;
    m_autoRigStatus = "Auto-rigging " + mp.filename().string() + "...";
    std::cout << "[AutoRig] Running: " << cmd << "\n";

    std::thread([this, cmd, out]() {
        int rc = std::system(cmd.c_str());
        m_autoRigOk = (rc == 0) && std::filesystem::exists(out);
        m_autoRigDone = true;
    }).detach();
}

// Normalized world rotation of a matrix's upper 3x3 (removes any scale, e.g.
// the 0.01 armature scale) so quat_cast returns a pure rotation.
static glm::quat rotationOf(const glm::mat4& m) {
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    float l0 = glm::length(c0), l1 = glm::length(c1), l2 = glm::length(c2);
    glm::mat3 basis(c0 / (l0 > 1e-8f ? l0 : 1.0f),
                    c1 / (l1 > 1e-8f ? l1 : 1.0f),
                    c2 / (l2 > 1e-8f ? l2 : 1.0f));
    return glm::normalize(glm::quat_cast(basis));
}

// Intrinsic X-Y-Z (roll/pitch/yaw) decomposition, matching the analysis that
// measured the A-pose spread as a local-Z (yaw) offset. Radians.
static glm::vec3 quatToEulerXYZ(const glm::quat& q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float roll  = std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y));
    float sp    = glm::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
    float pitch = std::asin(sp);
    float yaw   = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
    return glm::vec3(roll, pitch, yaw);
}
static glm::quat eulerXYZToQuat(const glm::vec3& e) {
    glm::quat qx = glm::angleAxis(e.x, glm::vec3(1, 0, 0));
    glm::quat qy = glm::angleAxis(e.y, glm::vec3(0, 1, 0));
    glm::quat qz = glm::angleAxis(e.z, glm::vec3(0, 0, 1));
    return glm::normalize(qz * qy * qx);  // inverse of the extraction above
}

// Forward-kinematics one frame → world transform per bone. `localCorrDeg`
// (optional, per-bone) subtracts a constant LOCAL euler rotation before the
// compose (used for the legs, whose abduction is constant in the hip-local frame).
static void fkFrame(const Skeleton& skel, const AnimationClip& clip, float t,
                    const std::vector<glm::vec3>* localCorrDeg,
                    std::vector<glm::mat4>& world) {
    const size_t n = skel.bones.size();
    world.assign(n, glm::mat4(1.0f));
    auto findCh = [&](int bi) -> const AnimationChannel* {
        for (const auto& ch : clip.channels) if (ch.boneIndex == bi) return &ch;
        return nullptr;
    };
    for (size_t i = 0; i < n; ++i) {
        const glm::mat4& rest = skel.bones[i].localTransform;
        glm::vec3 T(rest[3]);
        glm::vec3 S(glm::length(glm::vec3(rest[0])),
                    glm::length(glm::vec3(rest[1])),
                    glm::length(glm::vec3(rest[2])));
        glm::mat3 rb(glm::vec3(rest[0]) / (S.x > 1e-8f ? S.x : 1.0f),
                     glm::vec3(rest[1]) / (S.y > 1e-8f ? S.y : 1.0f),
                     glm::vec3(rest[2]) / (S.z > 1e-8f ? S.z : 1.0f));
        glm::quat R = glm::quat_cast(rb);
        if (const AnimationChannel* ch = findCh(static_cast<int>(i))) {
            if (!ch->positions.empty()) T = lerpVec3(ch->positionTimes, ch->positions, t);
            if (!ch->rotations.empty()) R = lerpQuat(ch->rotationTimes, ch->rotations, t);
            if (!ch->scales.empty())    S = lerpVec3(ch->scaleTimes, ch->scales, t);
        }
        if (localCorrDeg && i < localCorrDeg->size()) {
            const glm::vec3& c = (*localCorrDeg)[i];
            if (glm::dot(c, c) > 1e-8f) {
                glm::vec3 e = quatToEulerXYZ(R);
                e -= glm::radians(c);
                R = eulerXYZToQuat(e);
            }
        }
        glm::mat4 local = glm::translate(glm::mat4(1.0f), T)
                        * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
        int p = skel.bones[i].parentIndex;
        world[i] = (p >= 0 && p < static_cast<int>(i)) ? world[p] * local
                                                       : skel.rootTransform * local;
    }
}

// True if bone k is b or a descendant of b (walks parent chain).
static bool isInSubtree(const Skeleton& skel, int k, int b) {
    while (k >= 0) { if (k == b) return true; k = skel.bones[k].parentIndex; }
    return false;
}

void ModelingMode::buildNativeTrackFromClip(SceneObject* obj, const Skeleton& skel,
                                            const AnimationClip& clip,
                                            const std::vector<glm::vec3>* legLocalCorrDeg,
                                            const std::vector<float>* armWorldZDeg) {
    const size_t boneCount = skel.bones.size();

    // Shared timeline = the union of every channel's sample times.
    std::set<float> timeSet;
    for (const auto& ch : clip.channels) {
        for (float t : ch.positionTimes) timeSet.insert(t);
        for (float t : ch.rotationTimes) timeSet.insert(t);
        for (float t : ch.scaleTimes)    timeSet.insert(t);
    }
    if (timeSet.empty()) return;
    std::vector<float> times(timeSet.begin(), timeSet.end());

    // Rest world rotation per bone (from inverse bind), so the stored rotation
    // is a DELTA from rest — what reskinFromBoneDeltas expects.
    std::vector<glm::quat> restRot(boneCount);
    for (size_t i = 0; i < boneCount; ++i)
        restRot[i] = rotationOf(glm::inverse(skel.bones[i].inverseBindMatrix));

    ObjectAnimTrack track;
    const glm::vec3 tp = obj->getTransform().getPosition();
    const glm::quat tr = obj->getTransform().getRotation();
    const glm::vec3 ts = obj->getTransform().getScale();

    std::vector<glm::mat4> world;
    for (float t : times) {
        fkFrame(skel, clip, t, legLocalCorrDeg, world);  // legs corrected in-FK

        // Arms: rotate each arm subtree about the shoulder in the world coronal
        // plane (about world +Z) to cancel the constant lateral spread. Local
        // euler can't isolate it (the arm bone's local frame is rotated ~90°),
        // but a rigid rotation of the whole arm about the shoulder does exactly
        // what we want and preserves the fore/aft swing.
        if (armWorldZDeg) {
            for (size_t b = 0; b < boneCount && b < armWorldZDeg->size(); ++b) {
                float deg = (*armWorldZDeg)[b];
                if (std::abs(deg) < 1e-4f) continue;
                glm::vec3 head = glm::vec3(world[b][3]);
                glm::mat4 P = glm::translate(glm::mat4(1.0f), head)
                            * glm::rotate(glm::mat4(1.0f), glm::radians(-deg), glm::vec3(0, 0, 1))
                            * glm::translate(glm::mat4(1.0f), -head);
                for (size_t k = 0; k < boneCount; ++k)
                    if (isInSubtree(skel, static_cast<int>(k), static_cast<int>(b)))
                        world[k] = P * world[k];
            }
        }

        std::vector<glm::vec3> heads(boneCount);
        std::vector<glm::quat> rots(boneCount);
        for (size_t i = 0; i < boneCount; ++i) {
            heads[i] = glm::vec3(world[i][3]);
            rots[i]  = glm::normalize(rotationOf(world[i]) * glm::inverse(restRot[i]));
        }
        track.times.push_back(t);
        track.positions.push_back(tp);
        track.rotations.push_back(tr);
        track.scales.push_back(ts);
        track.bonePositionsPerKey.push_back(std::move(heads));
        track.boneRotationsPerKey.push_back(std::move(rots));
    }

    m_objectAnims[obj] = std::move(track);
    m_timelineDuration = std::max(m_timelineDuration, m_objectAnims[obj].times.back());
}

std::vector<glm::vec3> ModelingMode::detectAPoseSpread(const Skeleton& skel,
                                                       const AnimationClip& clip) {
    const size_t n = skel.bones.size();
    std::vector<glm::vec3> floors(n, glm::vec3(0.0f));

    std::set<float> ts;
    for (const auto& ch : clip.channels) for (float t : ch.rotationTimes) ts.insert(t);
    if (ts.empty()) return floors;
    std::vector<float> times(ts.begin(), ts.end());

    // LEGS ONLY. The hip's abduction is constant in the UpLeg-local frame (the
    // hip sways it through world-neutral each stride), so a local Y+Z floor
    // captures it. Arms are handled separately in world space.
    auto isUpLeg = [](const std::string& name) {
        std::string s; s.reserve(name.size());
        for (char c : name) s += static_cast<char>(std::tolower((unsigned char)c));
        return s.find("upleg") != std::string::npos;
    };
    auto findCh = [&](int bi) -> const AnimationChannel* {
        for (const auto& ch : clip.channels) if (ch.boneIndex == bi) return &ch;
        return nullptr;
    };

    for (size_t i = 0; i < n; ++i) {
        if (!isUpLeg(skel.bones[i].name)) continue;
        const AnimationChannel* ch = findCh(static_cast<int>(i));
        glm::quat restR = rotationOf(skel.bones[i].localTransform);
        glm::vec2 bestYZ(0.0f); bool set = false;
        for (float t : times) {
            glm::quat R = restR;
            if (ch && !ch->rotations.empty()) R = lerpQuat(ch->rotationTimes, ch->rotations, t);
            glm::vec3 e = glm::degrees(quatToEulerXYZ(R));
            if (!set) { bestYZ = glm::vec2(e.y, e.z); set = true; }
            else {
                if (std::abs(e.y) < std::abs(bestYZ.x)) bestYZ.x = e.y;
                if (std::abs(e.z) < std::abs(bestYZ.y)) bestYZ.y = e.z;
            }
        }
        floors[i] = glm::vec3(0.0f, bestYZ.x, bestYZ.y);  // X (swing) kept at 0
    }
    return floors;
}

std::vector<float> ModelingMode::detectArmSpreadWorldZ(const Skeleton& skel,
                                                       const AnimationClip& clip) {
    const size_t n = skel.bones.size();
    std::vector<float> floors(n, 0.0f);

    std::set<float> ts;
    for (const auto& ch : clip.channels) for (float t : ch.rotationTimes) ts.insert(t);
    if (ts.empty()) return floors;
    std::vector<float> times(ts.begin(), ts.end());

    auto isArm = [](const std::string& name) {
        std::string s; s.reserve(name.size());
        for (char c : name) s += static_cast<char>(std::tolower((unsigned char)c));
        return s.find("arm") != std::string::npos && s.find("forearm") == std::string::npos;
    };
    // First child of each bone → gives the limb direction (Arm→ForeArm).
    std::vector<int> childOf(n, -1);
    for (size_t k = 0; k < n; ++k) {
        int p = skel.bones[k].parentIndex;
        if (p >= 0 && p < static_cast<int>(n) && childOf[p] < 0) childOf[p] = static_cast<int>(k);
    }

    std::vector<glm::mat4> world;
    std::vector<bool> set(n, false);
    for (float t : times) {
        fkFrame(skel, clip, t, nullptr, world);
        for (size_t b = 0; b < n; ++b) {
            if (!isArm(skel.bones[b].name)) continue;
            int c = childOf[b];
            if (c < 0) continue;
            glm::vec3 dir = glm::vec3(world[c][3]) - glm::vec3(world[b][3]);
            float L = glm::length(dir);
            if (L < 1e-6f) continue;
            dir /= L;
            // Lateral (coronal) angle from straight-down; the floor (min-abs
            // across frames) = the always-present sideways spread.
            float lat = glm::degrees(std::atan2(dir.x, -dir.y));
            if (!set[b] || std::abs(lat) < std::abs(floors[b])) { floors[b] = lat; set[b] = true; }
        }
    }
    return floors;
}

void ModelingMode::applyAPoseNeutralize(SceneObject* obj, float strength) {
    auto it = m_importedClipSource.find(obj);
    if (it == m_importedClipSource.end()) {
        std::cout << "[APose] No stored source clip for this object\n";
        return;
    }
    const Skeleton& skel = it->second.first;
    const AnimationClip& clip = it->second.second;

    std::vector<glm::vec3> legFloors = detectAPoseSpread(skel, clip);      // legs, local
    std::vector<float>     armFloors = detectArmSpreadWorldZ(skel, clip);  // arms, world Z
    float s = glm::clamp(strength, 0.0f, 1.0f);
    for (glm::vec3& f : legFloors) f *= s;
    for (float& f : armFloors)     f *= s;

    buildNativeTrackFromClip(obj, skel, clip, &legFloors, &armFloors);
    m_timelineLastAppliedTime = -1.0f;  // force re-apply on next tick

    std::cout << "[APose] Neutralize " << static_cast<int>(strength * 100) << "% applied (legs+arms)\n";

    // A-Pose rebuilt the base track — refresh the stance pristine and re-layer
    // any stance-width so the two corrections compose.
    m_stancePristine.erase(obj);
    if (std::abs(m_stanceWidth) > 1e-4f) applyStanceWidth(obj, m_stanceWidth);
}

void ModelingMode::applyStanceWidth(SceneObject* obj, float degrees) {
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) return;
    if (!m_ctx.editableMesh.hasSkeleton()) return;
    const Skeleton& skel = m_ctx.editableMesh.getSkeleton();
    const size_t n = skel.bones.size();

    // Capture the pre-stance track the first time (slider is at 0 then, so the
    // current track IS pristine). Cleared on anim load / A-Pose change.
    if (!m_stancePristine.count(obj)) m_stancePristine[obj] = ai->second;
    ObjectAnimTrack out = m_stancePristine[obj];   // copy, then correct legs
    m_stanceWidth = degrees;

    auto lc = [](const std::string& nm){ std::string s; for(char c:nm) s+=static_cast<char>(std::tolower((unsigned char)c)); return s; };

    // Thigh bones: PREFER the retargeter's role map (works for anonymous rigs);
    // fall back to name matching for imported (named) anims.
    std::vector<int> thighs;
    auto ci = m_correctionBones.find(obj);
    if (ci != m_correctionBones.end()) {
        for (const char* role : {"thigh_l", "thigh_r"}) {
            auto r = ci->second.find(role);
            if (r != ci->second.end() && r->second >= 0 && r->second < static_cast<int>(n))
                thighs.push_back(r->second);
        }
    }
    if (thighs.empty()) {
        for (size_t b = 0; b < n; ++b) {
            std::string s = lc(skel.bones[b].name);
            if (s.find("upleg")!=std::string::npos || s.find("thigh")!=std::string::npos ||
                s.find("upperleg")!=std::string::npos || s.find("upper_leg")!=std::string::npos)
                thighs.push_back(static_cast<int>(b));
        }
    }

    // Diagnostic (prints on every slider move): what matched? If nothing, list
    // the leg-ish bone names so we can teach the matcher this rig's naming.
    std::cout << "[Stance] " << degrees << " deg, matched " << thighs.size() << " thigh(s):";
    for (int t : thighs) std::cout << " '" << skel.bones[t].name << "'";
    if (thighs.empty()) {
        std::cout << " -- NONE. Leg/hip bones in this rig:";
        for (size_t b = 0; b < n; ++b) {
            std::string s = lc(skel.bones[b].name);
            if (s.find("leg")!=std::string::npos || s.find("thigh")!=std::string::npos || s.find("hip")!=std::string::npos)
                std::cout << " '" << skel.bones[b].name << "'";
        }
    }
    std::cout << std::endl;

    if (std::abs(degrees) > 1e-4f && !thighs.empty()) {
        const size_t F = out.times.size();
        for (size_t f = 0; f < F; ++f) {
            if (f >= out.bonePositionsPerKey.size() || f >= out.boneRotationsPerKey.size()) continue;
            auto& heads = out.bonePositionsPerKey[f];   // world head positions
            auto& rots  = out.boneRotationsPerKey[f];   // world rotation deltas
            if (heads.size() < n || rots.size() < n) continue;
            for (int th : thighs) {
                int hip = skel.bones[th].parentIndex;
                glm::vec3 pivot = (hip >= 0 && hip < static_cast<int>(heads.size())) ? heads[hip] : heads[th];
                // Inward = toward the body centerline. Sign from which side the
                // thigh sits relative to the hip (X), so it's correct for L and R.
                float side = (heads[th].x >= pivot.x) ? 1.0f : -1.0f;
                glm::quat Rz = glm::angleAxis(glm::radians(degrees) * (-side), glm::vec3(0, 0, 1));
                glm::mat3 R = glm::mat3_cast(Rz);
                for (size_t k = 0; k < n; ++k) {
                    if (!isInSubtree(skel, static_cast<int>(k), th)) continue;
                    heads[k] = pivot + R * (heads[k] - pivot);   // swing the whole leg about the hip
                    rots[k]  = glm::normalize(Rz * rots[k]);      // and rotate its world orientation
                }
            }
        }
    }

    m_objectAnims[obj] = std::move(out);
    m_timelineLastAppliedTime = -1.0f;   // force re-pose next tick
    reskinFromBoneDeltas();
}

// Blend each foot toward its rest orientation on ALL axes. Monocular mocap can't
// see foot roll, so it injects a big CONSTANT twist that wrings the foot around the
// ankle; a yaw-only fix is blind to it (roll doesn't change where the toe points).
// We compute each foot bone's MEAN world-rotation over the clip (= the constant
// bias) and remove `amount` of it, so at 1.0 the foot sits flat/planted with only
// the real per-frame motion left. Non-destructive (re-layers from a pristine copy).
void ModelingMode::applyPlantFeet(SceneObject* obj, float amount) {
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) return;
    if (!m_ctx.editableMesh.hasSkeleton()) return;
    const Skeleton& skel = m_ctx.editableMesh.getSkeleton();
    const size_t n = skel.bones.size();

    if (!m_feetPristine.count(obj)) m_feetPristine[obj] = ai->second;
    ObjectAnimTrack out = m_feetPristine[obj];   // copy, then correct the feet
    m_plantFeet = amount;

    // Foot bones: prefer the retargeter's role map (anonymous rigs), else match names.
    std::vector<int> feet;
    auto ci = m_correctionBones.find(obj);
    if (ci != m_correctionBones.end()) {
        for (const char* role : {"ankle_l", "ankle_r", "foot_l", "foot_r"}) {
            auto r = ci->second.find(role);
            if (r != ci->second.end() && r->second >= 0 && r->second < static_cast<int>(n))
                feet.push_back(r->second);
        }
    }
    if (feet.empty()) {
        auto lc = [](const std::string& nm){ std::string s; for(char c:nm) s+=static_cast<char>(std::tolower((unsigned char)c)); return s; };
        for (size_t b = 0; b < n; ++b) {
            std::string s = lc(skel.bones[b].name);
            if (s.find("ankle")!=std::string::npos || s.find("foot")!=std::string::npos)
                feet.push_back(static_cast<int>(b));
        }
    }
    std::cout << "[PlantFeet] " << amount << ", matched " << feet.size() << " foot bone(s):";
    for (int f : feet) std::cout << " '" << skel.bones[f].name << "'";
    std::cout << std::endl;

    if (amount > 1e-4f && !feet.empty()) {
        const size_t F = out.times.size();
        for (int fb : feet) {
            // Mean world-rotation delta over the clip = the constant bias to remove.
            glm::quat ref(1, 0, 0, 0); bool haveRef = false;
            glm::quat acc(0, 0, 0, 0);
            for (size_t f = 0; f < F; ++f) {
                if (f >= out.boneRotationsPerKey.size()) continue;
                auto& rots = out.boneRotationsPerKey[f];
                if (static_cast<int>(rots.size()) <= fb) continue;
                glm::quat q = glm::normalize(rots[fb]);
                if (!haveRef) { ref = q; haveRef = true; }
                if (glm::dot(q, ref) < 0.0f) q = -q;         // align hemisphere (double cover)
                acc.w += q.w; acc.x += q.x; acc.y += q.y; acc.z += q.z;
            }
            if (!haveRef) continue;
            glm::quat mean = glm::normalize(acc);
            // Partial de-bias: at amount=1 remove the whole mean → foot at rest.
            glm::quat deBias = glm::slerp(glm::quat(1, 0, 0, 0), glm::inverse(mean), amount);
            glm::mat3 C = glm::mat3_cast(deBias);
            for (size_t f = 0; f < F; ++f) {
                if (f >= out.bonePositionsPerKey.size() || f >= out.boneRotationsPerKey.size()) continue;
                auto& heads = out.bonePositionsPerKey[f];
                auto& rots  = out.boneRotationsPerKey[f];
                if (static_cast<int>(heads.size()) <= fb || static_cast<int>(rots.size()) <= fb) continue;
                glm::vec3 pivot = heads[fb];
                for (size_t k = 0; k < n; ++k) {
                    if (!isInSubtree(skel, static_cast<int>(k), fb)) continue;   // foot + toe
                    heads[k] = pivot + C * (heads[k] - pivot);
                    rots[k]  = glm::normalize(deBias * rots[k]);
                }
            }
        }
    }

    m_objectAnims[obj] = std::move(out);
    m_timelineLastAppliedTime = -1.0f;
    reskinFromBoneDeltas();
}

// Keep every keepEvery-th keyframe (plus the final one so the clip keeps its full
// length) and delete the rest — mocap comes in far denser than a game clip needs.
// Destructive to the loaded track; re-import to get the full density back.
void ModelingMode::thinKeyframes(SceneObject* obj, int keepEvery) {
    if (keepEvery < 2) return;
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) return;
    ObjectAnimTrack& t = ai->second;
    const size_t F = t.times.size();
    if (F == 0) return;

    std::vector<size_t> keep;
    for (size_t i = 0; i < F; i += static_cast<size_t>(keepEvery)) keep.push_back(i);
    if (keep.empty() || keep.back() != F - 1) keep.push_back(F - 1);   // always keep the end

    ObjectAnimTrack out;
    auto pick = [&](auto& dst, const auto& src) {
        if (src.size() == F) for (size_t k : keep) dst.push_back(src[k]);
    };
    for (size_t k : keep) out.times.push_back(t.times[k]);
    pick(out.positions, t.positions);
    pick(out.rotations, t.rotations);
    pick(out.scales,    t.scales);
    pick(out.bonePositionsPerKey, t.bonePositionsPerKey);
    pick(out.boneRotationsPerKey, t.boneRotationsPerKey);

    std::cout << "[ThinKeys] kept " << out.times.size() << " of " << F
              << " keys (every " << keepEvery << ")\n";

    m_objectAnims[obj] = std::move(out);
    // Base track changed → drop corrective pristines + reset sliders so they
    // recapture from the thinned track.
    m_stancePristine.erase(obj); m_feetPristine.erase(obj);
    m_shoulderPristine.erase(obj); m_rollPristine.erase(obj);
    m_stanceWidth = 0.0f; m_plantFeet = 0.0f; m_relaxShoulders = 0.0f; m_footRoll = 0.0f;
    m_timelineLastAppliedTime = -1.0f;
    reskinFromBoneDeltas();
}

// Drop the shoulder girdle. SMPL's rest has raised/shrugged collars vs most rigs,
// so the retarget bakes in a permanent shrug; this swings each collar+arm subtree
// DOWN in the frontal plane about the collar root, by `degrees`. Same shape as
// applyStanceWidth (legs), different bones + a known L/R side from the role name.
void ModelingMode::applyRelaxShoulders(SceneObject* obj, float degrees) {
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) return;
    if (!m_ctx.editableMesh.hasSkeleton()) return;
    const Skeleton& skel = m_ctx.editableMesh.getSkeleton();
    const size_t n = skel.bones.size();

    if (!m_shoulderPristine.count(obj)) m_shoulderPristine[obj] = ai->second;
    ObjectAnimTrack out = m_shoulderPristine[obj];
    m_relaxShoulders = degrees;

    std::vector<int> shs;
    auto ci = m_correctionBones.find(obj);
    if (ci != m_correctionBones.end()) {
        for (const char* role : {"shoulder_l", "shoulder_r"}) {
            auto r = ci->second.find(role);
            if (r != ci->second.end() && r->second >= 0 && r->second < static_cast<int>(n))
                shs.push_back(r->second);
        }
    }
    std::cout << "[Shoulders] drop " << degrees << ", matched " << shs.size() << " collar(s):";
    for (int s : shs) std::cout << " '" << skel.bones[s].name << "'";
    if (shs.empty()) std::cout << " -- NONE (regenerate the anim so it exports shoulder roles)";
    std::cout << std::endl;

    // Straight DOWN: translate the whole collar+arm subtree in −Y. A rotation about
    // the collar root would arc the arm toward the centerline (the "brings arms in"
    // the user saw); a pure translation drops the shoulders vertically and leaves
    // the arms hanging where they are. `degrees` is reused as a downward distance.
    if (degrees > 1e-4f && !shs.empty()) {
        const glm::vec3 downV(0.0f, -degrees, 0.0f);
        const size_t F = out.times.size();
        for (size_t f = 0; f < F; ++f) {
            if (f >= out.bonePositionsPerKey.size()) continue;
            auto& heads = out.bonePositionsPerKey[f];
            if (heads.size() < n) continue;
            for (int sh : shs)
                for (size_t k = 0; k < n; ++k)
                    if (isInSubtree(skel, static_cast<int>(k), sh)) heads[k] += downV;
        }
    }

    m_objectAnims[obj] = std::move(out);
    m_timelineLastAppliedTime = -1.0f;
    reskinFromBoneDeltas();
}

// Add a clean, procedural HEEL-TO-TOE roll, phased to ground contact. Monocular
// mocap barely reads foot articulation on a fast run, so the ankle roll it does
// capture is noisy and out of phase → looks flat/heel-planted. We detect each
// foot's contact from its toe height, then during STANCE roll it heel→toe (a bit
// dorsiflexed at strike → plantarflexed at push-off) and during SWING lift the
// toe for clearance, about the foot's own lateral axis. Blended in by `amount`.
// v1 — the angle constants below are eyeball starting points.
void ModelingMode::applyFootRoll(SceneObject* obj, float amount) {
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) return;
    if (!m_ctx.editableMesh.hasSkeleton()) return;
    const Skeleton& skel = m_ctx.editableMesh.getSkeleton();
    const size_t n = skel.bones.size();

    if (!m_rollPristine.count(obj)) m_rollPristine[obj] = ai->second;
    ObjectAnimTrack out = m_rollPristine[obj];
    m_footRoll = amount;

    // Ankle + its toe (toe from the role map if present, else the ankle's child).
    struct Foot { int ankle; int toe; };
    std::vector<Foot> feet;
    auto ci = m_correctionBones.find(obj);
    if (ci != m_correctionBones.end()) {
        auto grab = [&](const char* r){ auto it=ci->second.find(r); return (it!=ci->second.end())?it->second:-1; };
        for (auto pr : {std::make_pair("ankle_l","toe_l"), std::make_pair("ankle_r","toe_r")}) {
            int a = grab(pr.first); if (a < 0 || a >= (int)n) continue;
            int t = grab(pr.second);
            if (t < 0)   // no toe role — take the ankle's first child
                for (size_t b = 0; b < n; ++b) if (skel.bones[b].parentIndex == a) { t = (int)b; break; }
            feet.push_back({a, t});
        }
    }
    std::cout << "[FootRoll] " << amount << ", feet: " << feet.size() << std::endl;

    const size_t F = out.times.size();
    if (amount > 1e-4f && !feet.empty() && F > 2) {
        // Roll shape (degrees): + = toe UP (dorsiflex), - = toe DOWN (plantarflex).
        // A run pushes off the BALL of the foot, so push-off needs strong toe-down
        // (heel-to-flat isn't enough); strike is light since a run is mid/forefoot.
        const float STRIKE = +5.0f, PUSHOFF = -60.0f, SWING = +15.0f;
        // Bias the roll toward push-off across stance so it keeps rolling onto the
        // ball rather than settling at flat mid-stride.
        auto rollAt = [&](float p) {
            float t = p * p;                       // ease-IN: reach push-off sooner + harder
            return STRIKE * (1.0f - t) + PUSHOFF * t;
        };
        for (auto& ft : feet) {
            // Contact from toe height: planted when low.
            std::vector<float> toeY(F);
            for (size_t f = 0; f < F; ++f)
                toeY[f] = (ft.toe >= 0 && ft.toe < (int)out.bonePositionsPerKey[f].size())
                          ? out.bonePositionsPerKey[f][ft.toe].y : 0.0f;
            float lo = *std::min_element(toeY.begin(), toeY.end());
            float hi = *std::max_element(toeY.begin(), toeY.end());
            float thresh = lo + 0.30f * (hi - lo);
            std::vector<char> planted(F);
            for (size_t f = 0; f < F; ++f) planted[f] = toeY[f] < thresh;

            for (size_t f = 0; f < F; ++f) {
                if (f >= out.bonePositionsPerKey.size() || f >= out.boneRotationsPerKey.size()) continue;
                auto& heads = out.bonePositionsPerKey[f];
                auto& rots  = out.boneRotationsPerKey[f];
                if ((int)heads.size() <= ft.ankle || (int)rots.size() <= ft.ankle) continue;

                float pitch;   // degrees this frame
                if (planted[f]) {
                    // phase within this contiguous stance segment
                    size_t s = f; while (s > 0 && planted[s - 1]) --s;
                    size_t e = f; while (e + 1 < F && planted[e + 1]) ++e;
                    float p = (e > s) ? float(f - s) / float(e - s) : 0.0f;
                    pitch = rollAt(p);
                } else {
                    pitch = SWING;
                }
                pitch *= amount;

                // Rotate the foot (ankle + toe) about its own lateral axis.
                glm::vec3 aH = heads[ft.ankle];
                glm::vec3 tH = (ft.toe >= 0 && ft.toe < (int)heads.size()) ? heads[ft.toe] : aH + glm::vec3(0,0,0.1f);
                glm::vec3 fwd = tH - aH; fwd.y = 0.0f;
                if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, 0, 1);
                fwd = glm::normalize(fwd);
                glm::vec3 lateral = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd)); // med-lateral
                glm::quat R = glm::angleAxis(glm::radians(pitch), lateral);
                glm::mat3 Rm = glm::mat3_cast(R);
                for (size_t k = 0; k < n; ++k) {
                    if (!isInSubtree(skel, static_cast<int>(k), ft.ankle)) continue;
                    heads[k] = aH + Rm * (heads[k] - aH);
                    rots[k]  = glm::normalize(R * rots[k]);
                }
            }
        }
    }

    m_objectAnims[obj] = std::move(out);
    m_timelineLastAppliedTime = -1.0f;
    reskinFromBoneDeltas();
}

// Pair each bone with its left/right partner by matching rest heads across X=0.
// Centre bones (spine/head, x≈0) pair with themselves. Used by Mirror Pose.
void ModelingMode::buildBoneMirrorPairs() {
    const auto& rest = m_bindPoseBonePositions;
    const size_t n = rest.size();
    m_boneMirror.assign(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (std::abs(rest[i].x) < 0.02f) { m_boneMirror[i] = (int)i; continue; }  // centre → self
        glm::vec3 want(-rest[i].x, rest[i].y, rest[i].z);
        int best = -1; float bd = 1e9f;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            float d = glm::length(rest[j] - want);
            if (d < bd) { bd = d; best = (int)j; }
        }
        m_boneMirror[i] = (bd < 0.05f) ? best : (int)i;   // no partner found → treat as centre
    }
}

// Re-plant every IK leg's goal at its current ankle position — locks the feet
// where they are now so posing the body won't drag them.
void ModelingMode::plantFeetAtCurrent() {
    for (IKLeg& leg : m_ikLegs) {
        if (leg.foot >= 0 && leg.foot < (int)m_bonePositions.size())
            leg.goal = m_bonePositions[leg.foot];
    }
    std::cout << "[IK] planted " << m_ikLegs.size() << " foot goal(s) at current position\n";
}

// Mirror the keyframe at the playhead left<->right: pose one stride, then mirror it
// to get the opposite stride. Reflects across the sagittal (X=0) plane — swaps L/R
// bones and flips each transform.
void ModelingMode::mirrorPoseAtCurrentTime() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end()) { std::cout << "[Mirror] no animation\n"; return; }
    ObjectAnimTrack& t = ai->second;
    if (m_boneMirror.size() != m_bindPoseBonePositions.size()) buildBoneMirrorPairs();
    const size_t n = m_bindPoseBonePositions.size();

    // Find the key at the playhead.
    int idx = -1;
    for (size_t k = 0; k < t.times.size(); ++k)
        if (std::abs(t.times[k] - m_timelineCurrentTime) < 1e-3f) { idx = (int)k; break; }
    if (idx < 0) { std::cout << "[Mirror] no key at the playhead — Set Key first\n"; return; }
    if (idx >= (int)t.bonePositionsPerKey.size() || idx >= (int)t.boneRotationsPerKey.size()) return;

    auto& heads = t.bonePositionsPerKey[idx];
    auto& rots  = t.boneRotationsPerKey[idx];
    if (heads.size() < n || rots.size() < n) { std::cout << "[Mirror] key has no rig pose\n"; return; }

    std::vector<glm::vec3> nh(heads);
    std::vector<glm::quat> nr(rots);
    for (size_t i = 0; i < n; ++i) {
        int p = (i < m_boneMirror.size() && m_boneMirror[i] >= 0) ? m_boneMirror[i] : (int)i;
        glm::vec3 h = heads[p]; glm::quat q = rots[p];
        nh[i] = glm::vec3(-h.x, h.y, h.z);              // reflect position across X=0
        nr[i] = glm::quat(q.w, q.x, -q.y, -q.z);        // reflect rotation across X=0
    }
    heads = std::move(nh); rots = std::move(nr);
    m_timelineLastAppliedTime = -1.0f;
    reskinFromBoneDeltas();
    std::cout << "[Mirror] mirrored the key at t=" << m_timelineCurrentTime << "\n";
}

// Copy the FIRST key's pose to a new key one frame past the last, so the clip ends
// on its start pose and loops seamlessly.
void ModelingMode::makeLoopClosed() {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) return;
    auto ai = m_objectAnims.find(obj);
    if (ai == m_objectAnims.end() || ai->second.times.size() < 2) { std::cout << "[Loop] need >=2 keys\n"; return; }
    ObjectAnimTrack& t = ai->second;
    float dt = t.times[1] - t.times[0];
    float endT = t.times.back() + (dt > 1e-4f ? dt : 1.0f / 24.0f);
    t.times.push_back(endT);
    t.positions.push_back(t.positions.front());
    t.rotations.push_back(t.rotations.front());
    t.scales.push_back(t.scales.front());
    if (!t.bonePositionsPerKey.empty()) t.bonePositionsPerKey.push_back(t.bonePositionsPerKey.front());
    if (!t.boneRotationsPerKey.empty()) t.boneRotationsPerKey.push_back(t.boneRotationsPerKey.front());
    std::cout << "[Loop] closed: copied start pose to t=" << endT << " (" << t.times.size() << " keys)\n";
    reskinFromBoneDeltas();
}

void ModelingMode::importSkinnedGLBNative(const SkinnedLoadResult& skinned) {
    SceneObject* obj = m_ctx.selectedObject;
    if (!obj) { std::cout << "[SkinnedImport] No object selected\n"; return; }
    if (!skinned.skeleton || skinned.skeleton->bones.empty()) {
        std::cout << "[SkinnedImport] No skeleton in file\n"; return;
    }
    if (skinned.meshes.empty()) { std::cout << "[SkinnedImport] No skinned mesh\n"; return; }
    if (!m_ctx.editableMesh.isValid()) {
        std::cout << "[SkinnedImport] Editable mesh not built — cannot rig\n"; return;
    }

    const Skeleton& glbSkel = *skinned.skeleton;
    const size_t boneCount = glbSkel.bones.size();
    const auto& smesh = skinned.meshes[0];

    // 1) Skeleton onto the editable mesh (names / parents / IBMs / armature scale).
    m_ctx.editableMesh.setSkeleton(glbSkel);

    // 2) Transfer Meshy's own per-vertex weights by POSITION. buildEditableMesh
    //    welds/merges verts so index order is lost, but positions are preserved.
    //    This reuses the ORIGINAL (good) weights — no guessing.
    const float cell = 1e-3f;
    auto hashKey = [&](long x, long y, long z) -> long {
        return (x * 73856093L) ^ (y * 19349663L) ^ (z * 83492791L);
    };
    std::unordered_map<long, std::vector<uint32_t>> grid;
    grid.reserve(smesh.vertices.size() * 2);
    for (uint32_t i = 0; i < smesh.vertices.size(); ++i) {
        const glm::vec3& p = smesh.vertices[i].position;
        grid[hashKey(std::lround(p.x / cell), std::lround(p.y / cell), std::lround(p.z / cell))].push_back(i);
    }

    const uint32_t vcount = m_ctx.editableMesh.getVertexCount();
    uint32_t exact = 0;
    for (uint32_t vi = 0; vi < vcount; ++vi) {
        auto& hv = m_ctx.editableMesh.getVertex(vi);
        const glm::vec3 p = hv.position;
        long bx = std::lround(p.x / cell), by = std::lround(p.y / cell), bz = std::lround(p.z / cell);
        float bestD = FLT_MAX; int best = -1;
        for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
            auto it = grid.find(hashKey(bx + dx, by + dy, bz + dz));
            if (it == grid.end()) continue;
            for (uint32_t si : it->second) {
                float d = glm::distance2(smesh.vertices[si].position, p);
                if (d < bestD) { bestD = d; best = static_cast<int>(si); }
            }
        }
        if (best >= 0) {
            hv.boneIndices = smesh.vertices[best].joints;
            hv.boneWeights = smesh.vertices[best].weights;
            if (bestD < (cell * 4.0f) * (cell * 4.0f)) ++exact;
            // Renormalize defensively.
            float s = hv.boneWeights.x + hv.boneWeights.y + hv.boneWeights.z + hv.boneWeights.w;
            if (s > 1e-6f) hv.boneWeights /= s;
            else { hv.boneIndices = glm::ivec4(0); hv.boneWeights = glm::vec4(1, 0, 0, 0); }
        }
    }
    float matchRate = vcount ? static_cast<float>(exact) / vcount : 0.0f;
    std::cout << "[SkinnedImport] weight transfer: " << exact << "/" << vcount
              << " exact (" << static_cast<int>(matchRate * 100) << "%)\n";

    // 3) Enter rigging mode + derive rest bone head positions from the IBMs
    //    (mirrors the "Enter Rigging Mode" button).
    m_riggingMode = true;
    m_showSkeleton = true;
    m_selectedBone = -1;
    m_bonePositions.assign(boneCount, glm::vec3(0.0f));
    for (size_t i = 0; i < boneCount; ++i) {
        const glm::mat4& ibm = glbSkel.bones[i].inverseBindMatrix;
        if (ibm != glm::mat4(1.0f)) {
            m_bonePositions[i] = glm::vec3(glm::inverse(ibm)[3]);
        } else {
            int p = glbSkel.bones[i].parentIndex;
            glm::vec3 pp = (p >= 0 && p < static_cast<int>(i))
                ? m_bonePositions[p] : glm::vec3(glbSkel.rootTransform[3]);
            m_bonePositions[i] = pp + glm::vec3(glbSkel.bones[i].localTransform[3]);
        }
    }

    // If weight transfer largely failed (mismatched spaces), fall back to auto
    // weights so the mesh still deforms.
    if (matchRate < 0.25f) {
        std::cout << "[SkinnedImport] low match rate — falling back to auto weights\n";
        m_ctx.editableMesh.generateAutoWeights(m_bonePositions, FLT_MAX);
    }

    // 4) Bind pose (mandatory for native skinning to deform).
    setBindPose();

    // 5) Convert the animation clip into native editable keyframes. Keep a
    //    pristine copy of the source so the A-pose neutralizer can re-derive the
    //    track at any strength later without re-importing.
    m_aposeNeutralize = 0.0f;
    m_stancePristine.erase(obj); m_stanceWidth = 0.0f;  // fresh anim -> stance slider resets
    m_feetPristine.erase(obj); m_plantFeet = 0.0f;       // and the plant-feet slider
    m_shoulderPristine.erase(obj); m_relaxShoulders = 0.0f; m_rollPristine.erase(obj); m_footRoll = 0.0f;
    if (!skinned.animations.empty()) {
        m_importedClipSource[obj] = { glbSkel, skinned.animations[0] };
        buildNativeTrackFromClip(obj, glbSkel, skinned.animations[0]);
    }

    // 6) Show frame 0 and auto-play so the walk is visible immediately.
    m_timelineCurrentTime = 0.0f;
    m_timelineLastAppliedTime = -1.0f;
    m_timelinePlaying = !m_objectAnims[obj].times.empty();
    reskinFromBoneDeltas();

    std::cout << "[SkinnedImport] Native rig ready: " << boneCount << " bones, "
              << (skinned.animations.empty() ? 0 : static_cast<int>(m_objectAnims[obj].times.size()))
              << " keyframes. Bones are in the list; keys are on the timeline.\n";
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
    if (best < tr.boneRotationsPerKey.size()) {
        m_keyClipboard.boneRotations = tr.boneRotationsPerKey[best];
    } else {
        m_keyClipboard.boneRotations.clear();
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
    auto resizeAligned = [&](size_t target) {
        if (track.bonePositionsPerKey.size() < target) track.bonePositionsPerKey.resize(target);
        if (track.boneRotationsPerKey.size() < target) track.boneRotationsPerKey.resize(target);
    };
    if (replace) {
        track.positions[idx] = m_keyClipboard.position;
        track.rotations[idx] = m_keyClipboard.rotation;
        track.scales[idx]    = m_keyClipboard.scale;
        if (hasRig) {
            resizeAligned(track.times.size());
            track.bonePositionsPerKey[idx] = m_keyClipboard.bonePositions;
            track.boneRotationsPerKey[idx] = m_keyClipboard.boneRotations;
        }
    } else {
        track.times.insert(track.times.begin() + idx, t);
        track.positions.insert(track.positions.begin() + idx, m_keyClipboard.position);
        track.rotations.insert(track.rotations.begin() + idx, m_keyClipboard.rotation);
        track.scales.insert(track.scales.begin() + idx, m_keyClipboard.scale);
        if (hasRig) {
            resizeAligned(track.times.size() - 1);
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx,
                                             m_keyClipboard.bonePositions);
            track.boneRotationsPerKey.insert(track.boneRotationsPerKey.begin() + idx,
                                             m_keyClipboard.boneRotations);
        } else if (!track.bonePositionsPerKey.empty()) {
            track.bonePositionsPerKey.insert(track.bonePositionsPerKey.begin() + idx,
                                             std::vector<glm::vec3>{});
            track.boneRotationsPerKey.insert(track.boneRotationsPerKey.begin() + idx,
                                             std::vector<glm::quat>{});
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

        // Lerp bone world positions and slerp world rotations (when the
        // track has rotations recorded). Sync skeleton localTransforms
        // (relative to parent) so the skeleton overlay draws correctly.
        auto& skel = m_ctx.editableMesh.getSkeleton();
        const bool haveRotsA = (i0 < track.boneRotationsPerKey.size() &&
                                track.boneRotationsPerKey[i0].size() == bonesA.size());
        const bool haveRotsB = (i1 < track.boneRotationsPerKey.size() &&
                                track.boneRotationsPerKey[i1].size() == bonesB.size());
        const bool slerpRots = haveRotsA && haveRotsB;
        if (m_boneWorldRotations.size() != bonesA.size())
            m_boneWorldRotations.assign(bonesA.size(), glm::quat(1, 0, 0, 0));
        const bool haveBind = (m_bindPoseBonePositions.size() == m_bonePositions.size());
        for (size_t b = 0; b < bonesA.size(); ++b) {
            m_bonePositions[b] = glm::mix(bonesA[b], bonesB[b], u);

            // Preserve bone length: lerping two joint positions between keys
            // shortens the segment (the "smush"). Snap this bone's head back to
            // its REST distance from its parent, keeping the interpolated
            // direction. Parents have lower indices, so they're already fixed.
            if (haveBind && b < skel.bones.size()) {
                int lp = skel.bones[b].parentIndex;
                if (lp >= 0 && lp < static_cast<int>(b)) {
                    float restLen = glm::length(m_bindPoseBonePositions[b] - m_bindPoseBonePositions[lp]);
                    glm::vec3 dir = m_bonePositions[b] - m_bonePositions[lp];
                    float d = glm::length(dir);
                    if (d > 1e-6f && restLen > 1e-6f)
                        m_bonePositions[b] = m_bonePositions[lp] + dir * (restLen / d);
                }
            }

            if (slerpRots) {
                glm::quat qa = track.boneRotationsPerKey[i0][b];
                glm::quat qb = track.boneRotationsPerKey[i1][b];
                if (glm::dot(qa, qb) < 0.0f) qb = -qb;  // antipodal
                m_boneWorldRotations[b] = glm::normalize(glm::slerp(qa, qb, u));
            } else {
                m_boneWorldRotations[b] = glm::quat(1, 0, 0, 0);
            }
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

        // Per-key foot pitch: interpolate each leg's LOCAL foot rotation (foot
        // relative to shin) from the keyframes so the ankle angle can animate.
        m_ikHaveLocalFoot = false;
        if (slerpRots && !m_ikLegs.empty() &&
            i0 < track.boneRotationsPerKey.size() && i1 < track.boneRotationsPerKey.size()) {
            const auto& rotsA = track.boneRotationsPerKey[i0];
            const auto& rotsB = track.boneRotationsPerKey[i1];
            m_ikLocalFootRot.assign(m_ikLegs.size(), glm::quat(1, 0, 0, 0));
            for (size_t li = 0; li < m_ikLegs.size(); ++li) {
                const IKLeg& leg = m_ikLegs[li];
                if (leg.shin < 0 || leg.foot < 0) continue;
                if (leg.foot < static_cast<int>(rotsA.size()) && leg.shin < static_cast<int>(rotsA.size()) &&
                    leg.foot < static_cast<int>(rotsB.size()) && leg.shin < static_cast<int>(rotsB.size())) {
                    glm::quat pA = glm::normalize(glm::inverse(rotsA[leg.shin]) * rotsA[leg.foot]);
                    glm::quat pB = glm::normalize(glm::inverse(rotsB[leg.shin]) * rotsB[leg.foot]);
                    if (glm::dot(pA, pB) < 0.0f) pB = -pB;
                    m_ikLocalFootRot[li] = glm::normalize(glm::slerp(pA, pB, u));
                    m_ikHaveLocalFoot = true;
                }
            }
        }

        // IK feet: rebuild rigidly from the interpolated shin so they follow the
        // leg instead of flailing (foot/heel/toe keyframes interpolate separately
        // and disagree). Must run after the bone interp, before the reskin.
        rederiveIKFeet();

        // Keep IK goals synced to the interpolated foot positions. Otherwise, when
        // you author at an in-between frame and move one control, the OTHER legs
        // snap to their stale (last-authored) goals — the "other leg flies off"
        // bug. Syncing here pins each leg to where the animation currently shows it.
        for (auto& leg : m_ikLegs) {
            if (leg.foot >= 0 && leg.foot < static_cast<int>(m_bonePositions.size()))
                leg.goal = m_bonePositions[leg.foot];
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

    // Advance time when playing (speed-scaled; negative speed plays in reverse).
    if (m_timelinePlaying) {
        m_timelineCurrentTime += ImGui::GetIO().DeltaTime * m_timelineSpeed;
        if (m_timelineDuration > 0.0f) {
            m_timelineCurrentTime = std::fmod(m_timelineCurrentTime, m_timelineDuration);
            if (m_timelineCurrentTime < 0.0f) m_timelineCurrentTime += m_timelineDuration;  // wrap reverse
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
    // Playback speed: type/drag a multiplier, or use presets (negative = reverse).
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::DragFloat("Speed", &m_timelineSpeed, 0.05f, -5.0f, 5.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button("1x", ImVec2(28, 0))) m_timelineSpeed = 1.0f;
    ImGui::SameLine();
    if (ImGui::Button("2x", ImVec2(28, 0))) m_timelineSpeed = 2.0f;
    ImGui::SameLine();
    if (ImGui::Button("5x", ImVec2(28, 0))) m_timelineSpeed = 5.0f;
    ImGui::SameLine();
    if (ImGui::Button("Rev", ImVec2(34, 0))) m_timelineSpeed = -m_timelineSpeed;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reverse: flip the current speed's sign");
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

        // ── Cycle tools: hand-key locomotion without fighting mocap ──
        if (!haveObj) ImGui::BeginDisabled();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Cycle Tools");
        // Foot-plant: keep planted feet from sliding while you pose the body.
        ImGui::Checkbox("Lock Feet (IK)", &m_ikLockFeet);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Freeze the IK foot goals so moving the pelvis/body can't\n"
                              "drag the planted feet. Turn off to step a foot forward.");
        ImGui::SameLine();
        if (ImGui::Button("Plant Feet Here")) plantFeetAtCurrent();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Re-lock each IK foot goal at its CURRENT position.");
        if (ImGui::Button("Mirror Pose")) mirrorPoseAtCurrentTime();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mirror the key at the playhead left<->right. Pose one stride,\n"
                              "copy it to the opposite phase, then Mirror Pose there.");
        ImGui::SameLine();
        if (ImGui::Button("Close Loop")) makeLoopClosed();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Append the start pose one frame past the end so the clip\n"
                              "loops seamlessly.");
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

            // Warn when a rigged model is being keyed without a bind pose —
            // Set Key in that case only stores the object transform, so the
            // skeleton won't animate on playback.
            bool isRigged = obj->hasEditorSkeleton();
            bool bindReady = m_hasBindPose && m_bindPoseOwner == obj;
            if (isRigged && !bindReady) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                   "(no bind pose - bones won't animate)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click 'Set Bind Pose' in the rigging panel\n"
                                      "before keying so bone poses get recorded.");
                }
            }
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
