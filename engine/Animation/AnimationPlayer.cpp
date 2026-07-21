#include "eden/Animation.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace eden {

void AnimationPlayer::play(const AnimationClip* clip, bool loop) {
    m_currentClip = clip;
    m_currentTime = 0.0f;
    m_playing = true;
    m_paused = false;
    m_loop = loop;

    if (m_skeleton) {
        m_boneMatrices.resize(m_skeleton->bones.size(), glm::mat4(1.0f));
        m_localTransforms.resize(m_skeleton->bones.size(), glm::mat4(1.0f));
    }
}

void AnimationPlayer::stop() {
    m_playing = false;
    m_paused = false;
    m_currentTime = 0.0f;

    // Reset to bind pose
    if (m_skeleton) {
        for (size_t i = 0; i < m_boneMatrices.size(); i++) {
            m_boneMatrices[i] = glm::mat4(1.0f);
        }
    }
}

void AnimationPlayer::pause() {
    m_paused = true;
}

void AnimationPlayer::resume() {
    m_paused = false;
}

void AnimationPlayer::update(float deltaTime) {
    if (!m_playing || m_paused || !m_currentClip || !m_skeleton) {
        return;
    }

    m_currentTime += deltaTime * m_playbackSpeed;

    if (m_currentTime >= m_currentClip->duration) {
        if (m_loop) {
            // Wrap around
            while (m_currentTime >= m_currentClip->duration) {
                m_currentTime -= m_currentClip->duration;
            }
        } else {
            m_currentTime = m_currentClip->duration;
            m_playing = false;
        }
    }

    computeBoneMatrices();
}

glm::mat4 AnimationPlayer::computeBoneTransform(int boneIndex, float time) {
    // Find the animation channel for this bone
    const AnimationChannel* channel = nullptr;
    for (const auto& ch : m_currentClip->channels) {
        if (ch.boneIndex == boneIndex) {
            channel = &ch;
            break;
        }
    }

    // Default each component to the bone's REST (bind) local TRS. A glTF channel
    // often animates only rotation; translation/scale must then stay at the rest
    // pose, NOT collapse to 0/1, or the skeleton distorts.
    const glm::mat4& rest = m_skeleton->bones[boneIndex].localTransform;
    glm::vec3 translation = glm::vec3(rest[3]);
    glm::vec3 restScale(glm::length(glm::vec3(rest[0])),
                        glm::length(glm::vec3(rest[1])),
                        glm::length(glm::vec3(rest[2])));
    glm::mat3 restBasis(
        glm::vec3(rest[0]) / (restScale.x > 1e-8f ? restScale.x : 1.0f),
        glm::vec3(rest[1]) / (restScale.y > 1e-8f ? restScale.y : 1.0f),
        glm::vec3(rest[2]) / (restScale.z > 1e-8f ? restScale.z : 1.0f));
    glm::quat rotation = glm::quat_cast(restBasis);
    glm::vec3 scale = restScale;

    if (channel) {
        // Interpolate position
        if (!channel->positions.empty()) {
            translation = lerpVec3(channel->positionTimes, channel->positions, time);
        }

        // Interpolate rotation
        if (!channel->rotations.empty()) {
            rotation = lerpQuat(channel->rotationTimes, channel->rotations, time);
        }

        // Interpolate scale
        if (!channel->scales.empty()) {
            scale = lerpVec3(channel->scaleTimes, channel->scales, time);
        }
    }

    // Compose TRS matrix
    glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
    glm::mat4 R = glm::mat4_cast(rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

    return T * R * S;
}

void AnimationPlayer::computeBoneMatrices() {
    if (!m_skeleton || !m_currentClip) return;

    const size_t boneCount = m_skeleton->bones.size();

    // Compute local transforms for each bone
    for (size_t i = 0; i < boneCount; i++) {
        m_localTransforms[i] = computeBoneTransform(static_cast<int>(i), m_currentTime);
    }

    // Compute world transforms by traversing hierarchy
    std::vector<glm::mat4> worldTransforms(boneCount, glm::mat4(1.0f));

    for (size_t i = 0; i < boneCount; i++) {
        const Bone& bone = m_skeleton->bones[i];
        if (bone.parentIndex < 0) {
            // Root bone: prepend the armature/ancestor transform so the animated
            // hierarchy lives in the same space as the inverse-bind matrices.
            worldTransforms[i] = m_skeleton->rootTransform * m_localTransforms[i];
        } else {
            // Child bone - multiply by parent's world transform
            worldTransforms[i] = worldTransforms[bone.parentIndex] * m_localTransforms[i];
        }
    }

    // Compute final bone matrices (world * inverse bind)
    for (size_t i = 0; i < boneCount; i++) {
        m_boneMatrices[i] = worldTransforms[i] * m_skeleton->bones[i].inverseBindMatrix;
    }
}

} // namespace eden
