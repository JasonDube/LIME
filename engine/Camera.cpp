#include <eden/Camera.hpp>
#include <algorithm>
#include <cmath>

namespace eden {

Camera::Camera(const glm::vec3& position)
    : m_position(position)
{
    updateVectors();
}

void Camera::processKeyboard(float deltaTime, bool forward, bool backward, bool left, bool right, bool up, bool down) {
    float velocity = m_speed * deltaTime;

    if (forward)  m_position += m_front * velocity;
    if (backward) m_position -= m_front * velocity;
    if (left)     m_position -= m_right * velocity;
    if (right)    m_position += m_right * velocity;
    if (up)       m_position += m_worldUp * velocity;
    if (down)     m_position -= m_worldUp * velocity;
}

// Helper to get maximum terrain height in a radius around a point
static float getMaxHeightInRadius(const HeightQueryFunc& heightQuery, float x, float z, float radius) {
    float maxHeight = heightQuery(x, z);

    // Sample 8 points around the perimeter
    const int numSamples = 8;
    for (int i = 0; i < numSamples; i++) {
        float angle = (float)i / numSamples * 6.28318f; // 2*PI
        float sampleX = x + std::cos(angle) * radius;
        float sampleZ = z + std::sin(angle) * radius;
        float h = heightQuery(sampleX, sampleZ);
        if (h > maxHeight) maxHeight = h;
    }

    return maxHeight;
}

void Camera::resolveAABBCollision(const glm::vec3& oldPos, glm::vec3& newPos) {
    if (m_collisionBoxes.empty()) return;

    for (const auto& box : m_collisionBoxes) {
        // Expand box by collision radius on X/Z
        float bMinX = box.min.x - m_collisionRadius;
        float bMaxX = box.max.x + m_collisionRadius;
        float bMinZ = box.min.z - m_collisionRadius;
        float bMaxZ = box.max.z + m_collisionRadius;
        // Y uses feet/head directly against box faces
        float bMinY = box.min.y;
        float bMaxY = box.max.y;

        float feetY = newPos.y - m_eyeHeight;
        float headY = newPos.y + 0.2f;

        // Check if new position overlaps this box
        if (newPos.x < bMinX || newPos.x > bMaxX) continue;
        if (newPos.z < bMinZ || newPos.z > bMaxZ) continue;
        if (feetY > bMaxY || headY < bMinY) continue;

        // Overlapping — use old position to determine which axes to push back on.
        float oldFeetY = oldPos.y - m_eyeHeight;
        float oldHeadY = oldPos.y + 0.2f;

        bool wasOutX = (oldPos.x < bMinX || oldPos.x > bMaxX);
        bool wasOutZ = (oldPos.z < bMinZ || oldPos.z > bMaxZ);
        bool wasOutY = (oldFeetY > bMaxY || oldHeadY < bMinY);

        // Count how many axes were outside
        int outsideCount = (wasOutX ? 1 : 0) + (wasOutZ ? 1 : 0) + (wasOutY ? 1 : 0);

        if (outsideCount >= 2) {
            // Came from outside on multiple axes — pick the one with largest movement
            float dx = wasOutX ? std::abs(newPos.x - oldPos.x) : 0.0f;
            float dz = wasOutZ ? std::abs(newPos.z - oldPos.z) : 0.0f;
            float dy = wasOutY ? std::abs(newPos.y - oldPos.y) : 0.0f;

            if (dx >= dz && dx >= dy) {
                wasOutZ = false; wasOutY = false;
            } else if (dz >= dx && dz >= dy) {
                wasOutX = false; wasOutY = false;
            } else {
                wasOutX = false; wasOutZ = false;
            }
        }

        if (wasOutX) {
            if (oldPos.x < bMinX) newPos.x = bMinX;
            else                  newPos.x = bMaxX;
        } else if (wasOutZ) {
            if (oldPos.z < bMinZ) newPos.z = bMinZ;
            else                  newPos.z = bMaxZ;
        } else if (wasOutY) {
            // Vertical collision — push feet above or head below
            if (oldFeetY > bMaxY)  newPos.y = bMaxY + m_eyeHeight; // land on top
            else                   newPos.y = bMinY - 0.2f;        // push head below ceiling
        } else {
            // Already inside — push out on nearest axis
            float penNX = newPos.x - bMinX;
            float penPX = bMaxX - newPos.x;
            float penNZ = newPos.z - bMinZ;
            float penPZ = bMaxZ - newPos.z;
            float penTop = bMaxY - feetY;       // push feet up out of top
            float penBot = headY - bMinY;       // push head down out of bottom

            float minPen = penNX;
            int axis = 0;
            if (penPX  < minPen) { minPen = penPX;  axis = 1; }
            if (penNZ  < minPen) { minPen = penNZ;  axis = 2; }
            if (penPZ  < minPen) { minPen = penPZ;  axis = 3; }
            if (penTop < minPen) { minPen = penTop; axis = 4; }
            if (penBot < minPen) { minPen = penBot; axis = 5; }

            switch (axis) {
                case 0: newPos.x = bMinX; break;
                case 1: newPos.x = bMaxX; break;
                case 2: newPos.z = bMinZ; break;
                case 3: newPos.z = bMaxZ; break;
                case 4: newPos.y = bMaxY + m_eyeHeight; break;
                case 5: newPos.y = bMinY - 0.2f; break;
            }
        }
    }
}

void Camera::updateMovement(float deltaTime, bool forward, bool backward, bool left, bool right,
                            bool jump, bool descend, const HeightQueryFunc& heightQuery) {
    m_currentTime += deltaTime;
    float velocity = m_speed * deltaTime;
    glm::vec3 oldPosition = m_position; // saved for AABB collision

    // Get ground height at player center (allows descending into valleys)
    float groundHeight = heightQuery(m_position.x, m_position.z);

    // Calculate horizontal movement direction (ignore pitch for WASD)
    glm::vec3 frontHorizontal = glm::normalize(glm::vec3(m_front.x, 0.0f, m_front.z));
    glm::vec3 rightHorizontal = m_right;

    if (m_movementMode == MovementMode::Fly) {
        // Fly mode: original behavior with full 3D movement
        if (forward)  m_position += m_front * velocity;
        if (backward) m_position -= m_front * velocity;
        if (left)     m_position -= m_right * velocity;
        if (right)    m_position += m_right * velocity;
        if (jump)     m_position += m_worldUp * velocity;
        if (descend)  m_position -= m_worldUp * velocity;

        // AABB wall collision in fly mode (always active — noclip only skips terrain)
        resolveAABBCollision(oldPosition, m_position);

        // Skip terrain collision in noclip mode (editor mode)
        if (!m_noClip) {
            // Check terrain collision in fly mode using collision radius
            float newGroundHeight = getMaxHeightInRadius(heightQuery, m_position.x, m_position.z, m_collisionRadius);
            float minFlyHeight = newGroundHeight + 0.5f;
            if (m_position.y < minFlyHeight) {
                m_position.y = minFlyHeight;
            }

            // Keep smoothed ground height updated for transition to walk mode
            m_smoothedGroundHeight = newGroundHeight;

            // Check if we've descended to ground level - auto-switch to walk mode
            float feetHeight = m_position.y - m_eyeHeight;
            if (feetHeight <= newGroundHeight + 0.1f) {
                m_movementMode = MovementMode::Walk;
                m_position.y = newGroundHeight + m_eyeHeight;
                m_smoothedGroundHeight = newGroundHeight;
                m_verticalVelocity = 0.0f;
                m_onGround = true;
            }
        }
    } else {
        // Walk mode: horizontal movement with slope checking and sliding

        // Calculate slope gradient at current position (for sliding)
        const float sampleDist = 0.5f;
        float hPosX = heightQuery(m_position.x + sampleDist, m_position.z);
        float hNegX = heightQuery(m_position.x - sampleDist, m_position.z);
        float hPosZ = heightQuery(m_position.x, m_position.z + sampleDist);
        float hNegZ = heightQuery(m_position.x, m_position.z - sampleDist);

        // Gradient points in direction of steepest ascent
        glm::vec2 gradient((hPosX - hNegX) / (2.0f * sampleDist),
                           (hPosZ - hNegZ) / (2.0f * sampleDist));
        float gradientMag = glm::length(gradient);
        float currentSlopeAngle = glm::degrees(std::atan(gradientMag));

        // Player input movement
        glm::vec3 moveDir(0.0f);
        if (forward)  moveDir += frontHorizontal;
        if (backward) moveDir -= frontHorizontal;
        if (left)     moveDir -= rightHorizontal;
        if (right)    moveDir += rightHorizontal;

        if (glm::length(moveDir) > 0.001f) {
            moveDir = glm::normalize(moveDir);
            glm::vec3 intendedPos = m_position + moveDir * velocity;

            // Check slope at intended position
            float targetGroundHeight = heightQuery(intendedPos.x, intendedPos.z);
            float horizontalDist = velocity;
            float heightDiff = targetGroundHeight - groundHeight;

            // Calculate slope angle (only check uphill movement)
            if (heightDiff > 0.0f) {
                float slopeAngle = glm::degrees(std::atan2(heightDiff, horizontalDist));

                if (slopeAngle < m_maxSlopeAngle) {
                    // Slope is walkable - allow movement
                    m_position.x = intendedPos.x;
                    m_position.z = intendedPos.z;
                } else if (gradientMag > 0.001f) {
                    // Wall slide: project movement along the wall face
                    glm::vec2 wallNormal = glm::normalize(gradient);  // Points uphill
                    glm::vec2 move2D(moveDir.x, moveDir.z);
                    // Remove the component going into the wall
                    float dot = glm::dot(move2D, wallNormal);
                    if (dot > 0.0f) {
                        move2D -= dot * wallNormal;
                    }
                    if (glm::length(move2D) > 0.001f) {
                        glm::vec3 slidePos = m_position + glm::vec3(move2D.x, 0, move2D.y) * velocity;
                        float slideHeight = heightQuery(slidePos.x, slidePos.z);
                        float slideDiff = slideHeight - groundHeight;
                        // Only allow the slide if the resulting position isn't also too steep
                        if (slideDiff <= 0.0f || glm::degrees(std::atan2(slideDiff, velocity)) < m_maxSlopeAngle) {
                            m_position.x = slidePos.x;
                            m_position.z = slidePos.z;
                        }
                    }
                }
            } else {
                // Downhill or flat - always allow
                m_position.x = intendedPos.x;
                m_position.z = intendedPos.z;
            }
        }

        // AABB wall collision in walk mode
        resolveAABBCollision(oldPosition, m_position);

        // Get actual ground height at player center
        float actualGroundHeight = heightQuery(m_position.x, m_position.z);
        float feetHeight = m_position.y - m_eyeHeight;

        // Are we on or near the ground?
        bool nearGround = feetHeight <= actualGroundHeight + 0.5f;

        if (nearGround && m_verticalVelocity <= 0.0f) {
            // On ground - no gravity, just smoothly follow terrain
            m_verticalVelocity = 0.0f;
            m_onGround = true;

            // Target height is actual ground + eye height
            float targetY = actualGroundHeight + m_eyeHeight;

            // Smoothly move toward target - NEVER snap
            float diff = targetY - m_position.y;
            float smoothSpeed = 4.0f; // Lower = smoother
            m_position.y += diff * smoothSpeed * deltaTime;

        } else {
            // In air - apply gravity
            m_onGround = false;
            m_verticalVelocity -= m_gravity * deltaTime;
            m_position.y += m_verticalVelocity * deltaTime;

            // Landing check
            if (m_position.y - m_eyeHeight <= actualGroundHeight) {
                m_position.y = actualGroundHeight + m_eyeHeight;
                m_verticalVelocity = 0.0f;
                m_onGround = true;
            }
        }
    }

    // Final safety: camera must stay above terrain at center point (skip in noclip mode)
    if (!m_noClip) {
        float finalGroundHeight = heightQuery(m_position.x, m_position.z);
        float absoluteMin = finalGroundHeight + 0.3f;
        if (m_position.y < absoluteMin) {
            m_position.y = absoluteMin;
        }
    }
}

void Camera::onSpacePressed(float groundHeight) {
    float timeSinceLastSpace = m_currentTime - m_lastSpaceTime;

    if (m_movementMode == MovementMode::Walk) {
        // Check for double-tap to enter fly mode
        if (timeSinceLastSpace < m_doubleTapWindow && m_lastSpaceTime >= 0.0f) {
            // Double-tap detected - enter fly mode
            m_movementMode = MovementMode::Fly;
            m_verticalVelocity = 0.0f;
        } else if (m_onGround) {
            // Single tap while on ground - jump
            m_verticalVelocity = m_jumpVelocity;
            m_onGround = false;
        }
    } else {
        // In fly mode, space just moves up (handled in updateMovement)
        // But double-tap while near ground could also toggle back to walk
        float feetHeight = m_position.y - m_eyeHeight;
        if (feetHeight <= groundHeight + 2.0f && timeSinceLastSpace < m_doubleTapWindow && m_lastSpaceTime >= 0.0f) {
            m_movementMode = MovementMode::Walk;
            m_position.y = groundHeight + m_eyeHeight;
            m_verticalVelocity = 0.0f;
            m_onGround = true;
        }
    }

    m_lastSpaceTime = m_currentTime;
}

void Camera::processMouse(float xOffset, float yOffset) {
    xOffset *= m_mouseSensitivity;
    yOffset *= m_mouseSensitivity;

    m_yaw += xOffset;
    m_pitch += yOffset;

    // Constrain pitch to prevent flipping
    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);

    updateVectors();
}

void Camera::updateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));

    m_front = glm::normalize(front);
    m_right = glm::normalize(glm::cross(m_front, m_worldUp));
    m_up = glm::normalize(glm::cross(m_right, m_front));
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::getProjectionMatrix(float aspectRatio, float nearPlane, float farPlane) const {
    if (m_projectionMode == ProjectionMode::Orthographic) {
        float halfHeight = m_orthoSize;
        float halfWidth = halfHeight * aspectRatio;
        // Use Vulkan-compatible orthographic projection (right-handed, depth [0,1])
        return glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, nearPlane, farPlane);
    }
    // Use Vulkan-compatible perspective projection (right-handed, depth [0,1])
    return glm::perspectiveRH_ZO(glm::radians(m_fov), aspectRatio, nearPlane, farPlane);
}

void Camera::setViewPreset(ViewPreset preset, const glm::vec3& targetCenter) {
    m_viewPreset = preset;

    // Distance from target for orthographic views
    float viewDistance = m_orthoSize * 2.0f + 10.0f;

    switch (preset) {
        case ViewPreset::Top:
            m_position = targetCenter + glm::vec3(0, viewDistance, 0);
            m_yaw = -90.0f;
            m_pitch = -89.9f;  // Looking straight down
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Bottom:
            m_position = targetCenter + glm::vec3(0, -viewDistance, 0);
            m_yaw = -90.0f;
            m_pitch = 89.9f;   // Looking straight up
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Front:
            m_position = targetCenter + glm::vec3(0, 0, viewDistance);
            m_yaw = -90.0f;
            m_pitch = 0.0f;
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Back:
            m_position = targetCenter + glm::vec3(0, 0, -viewDistance);
            m_yaw = 90.0f;
            m_pitch = 0.0f;
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Right:
            m_position = targetCenter + glm::vec3(viewDistance, 0, 0);
            m_yaw = -180.0f;
            m_pitch = 0.0f;
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Left:
            m_position = targetCenter + glm::vec3(-viewDistance, 0, 0);
            m_yaw = 0.0f;
            m_pitch = 0.0f;
            m_projectionMode = ProjectionMode::Orthographic;
            break;

        case ViewPreset::Custom:
        default:
            // Don't change anything for custom view
            break;
    }

    updateVectors();
}

glm::vec3 Camera::screenToWorldRay(float normalizedX, float normalizedY, float aspectRatio) const {
    // Convert from [0,1] to [-1,1] NDC
    float ndcX = normalizedX * 2.0f - 1.0f;
    float ndcY = 1.0f - normalizedY * 2.0f;  // Flip Y for screen coordinates

    // Calculate ray in view space
    float tanHalfFov = tan(glm::radians(m_fov) * 0.5f);
    glm::vec3 rayView;
    rayView.x = ndcX * aspectRatio * tanHalfFov;
    rayView.y = ndcY * tanHalfFov;
    rayView.z = -1.0f;  // Looking down -Z in view space

    // Transform to world space using camera orientation
    glm::vec3 rayWorld = rayView.x * m_right + rayView.y * m_up + rayView.z * (-m_front);

    return glm::normalize(rayWorld);
}

} // namespace eden
