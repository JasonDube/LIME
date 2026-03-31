#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace eden {

// Maximum bones per skeleton (matches shader UBO size)
constexpr size_t MAX_BONES = 128;

// A single bone in the skeleton hierarchy
struct Bone {
    std::string name;
    int parentIndex = -1;  // -1 = root bone
    glm::mat4 inverseBindMatrix = glm::mat4(1.0f);  // Transforms from mesh space to bone space
    glm::mat4 localTransform = glm::mat4(1.0f);     // Local transform relative to parent
};

// Complete skeleton structure
struct Skeleton {
    std::vector<Bone> bones;
    std::unordered_map<std::string, int> boneNameToIndex;

    int findBone(const std::string& name) const {
        auto it = boneNameToIndex.find(name);
        return it != boneNameToIndex.end() ? it->second : -1;
    }
};

// Keyframe for a single bone at a specific time
struct BoneKeyframe {
    float time = 0.0f;
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
};

// Animation channel - keyframes for one bone
struct AnimationChannel {
    int boneIndex = -1;
    std::vector<float> positionTimes;
    std::vector<glm::vec3> positions;
    std::vector<float> rotationTimes;
    std::vector<glm::quat> rotations;
    std::vector<float> scaleTimes;
    std::vector<glm::vec3> scales;
};

// Complete animation clip
struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

// Interpolation helpers
inline glm::vec3 lerpVec3(const std::vector<float>& times, const std::vector<glm::vec3>& values, float t) {
    if (values.empty()) return glm::vec3(0.0f);
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();

    // Find surrounding keyframes
    for (size_t i = 0; i < times.size() - 1; i++) {
        if (t >= times[i] && t < times[i + 1]) {
            float factor = (t - times[i]) / (times[i + 1] - times[i]);
            return glm::mix(values[i], values[i + 1], factor);
        }
    }
    return values.back();
}

inline glm::quat lerpQuat(const std::vector<float>& times, const std::vector<glm::quat>& values, float t) {
    if (values.empty()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();

    // Find surrounding keyframes
    for (size_t i = 0; i < times.size() - 1; i++) {
        if (t >= times[i] && t < times[i + 1]) {
            float factor = (t - times[i]) / (times[i + 1] - times[i]);
            return glm::slerp(values[i], values[i + 1], factor);
        }
    }
    return values.back();
}

// Animation playback state
class AnimationPlayer {
public:
    void setSkeleton(const Skeleton* skeleton) { m_skeleton = skeleton; }
    void play(const AnimationClip* clip, bool loop = true);
    void stop();
    void pause();
    void resume();

    void update(float deltaTime);

    // Get final bone matrices for rendering (model space)
    const std::vector<glm::mat4>& getBoneMatrices() const { return m_boneMatrices; }

    bool isPlaying() const { return m_playing; }
    float getCurrentTime() const { return m_currentTime; }
    const AnimationClip* getCurrentClip() const { return m_currentClip; }

    void setCurrentTime(float t) { m_currentTime = t; computeBoneMatrices(); }

    void setPlaybackSpeed(float speed) { m_playbackSpeed = speed; }
    float getPlaybackSpeed() const { return m_playbackSpeed; }

private:
    void computeBoneMatrices();
    glm::mat4 computeBoneTransform(int boneIndex, float time);

    const Skeleton* m_skeleton = nullptr;
    const AnimationClip* m_currentClip = nullptr;

    float m_currentTime = 0.0f;
    float m_playbackSpeed = 1.0f;
    bool m_playing = false;
    bool m_paused = false;
    bool m_loop = true;

    // Cached bone matrices (inverse bind * animated transform)
    std::vector<glm::mat4> m_boneMatrices;

    // Local transforms for each bone (before hierarchy multiplication)
    std::vector<glm::mat4> m_localTransforms;
};

} // namespace eden
