#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <functional>
#include <vector>

namespace eden {

enum class MovementMode {
    Fly,    // Free flight (original behavior)
    Walk    // Ground-based with gravity and jumping
};

enum class ProjectionMode {
    Perspective,
    Orthographic
};

enum class ViewPreset {
    Custom,     // Free camera
    Top,        // Looking down -Y
    Bottom,     // Looking up +Y
    Front,      // Looking down -Z
    Back,       // Looking down +Z
    Right,      // Looking down -X
    Left        // Looking down +X
};

// Simple AABB for wall collision (avoids depending on SceneObject.hpp)
struct CollisionBox {
    glm::vec3 min{0};
    glm::vec3 max{0};
};

// Function type for querying terrain height at a world position
using HeightQueryFunc = std::function<float(float x, float z)>;

class Camera {
public:
    Camera(const glm::vec3& position = {0, 50, 0});

    // Original fly-mode keyboard handler (kept for compatibility)
    void processKeyboard(float deltaTime, bool forward, bool backward, bool left, bool right, bool up, bool down);
    void processMouse(float xOffset, float yOffset);

    // New movement system with walk/fly modes
    // heightQuery: function to get terrain height at any (x,z) position
    void updateMovement(float deltaTime, bool forward, bool backward, bool left, bool right,
                        bool jump, bool descend, const HeightQueryFunc& heightQuery);

    // Handle space key press for jump/fly toggle (call when space is pressed)
    void onSpacePressed(float groundHeight);

    void setPosition(const glm::vec3& pos) { m_position = pos; }
    void setSpeed(float speed) { m_speed = speed; }
    void setMouseSensitivity(float sens) { m_mouseSensitivity = sens; }
    void setFov(float fov) { m_fov = fov; }

    const glm::vec3& getPosition() const { return m_position; }
    const glm::vec3& getFront() const { return m_front; }
    float getYaw() const { return m_yaw; }
    float getPitch() const { return m_pitch; }
    void setYaw(float yaw) { m_yaw = yaw; updateVectors(); }
    void setPitch(float pitch) { m_pitch = pitch; updateVectors(); }
    float getFov() const { return m_fov; }
    MovementMode getMovementMode() const { return m_movementMode; }
    void setMovementMode(MovementMode mode) { m_movementMode = mode; }
    bool isOnGround() const { return m_onGround; }

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspectRatio, float nearPlane = 0.1f, float farPlane = 1000.0f) const;

    // Get camera orientation vectors
    glm::vec3 getRight() const { return m_right; }
    glm::vec3 getUp() const { return m_up; }

    // Orthographic projection support
    void setProjectionMode(ProjectionMode mode) { m_projectionMode = mode; }
    ProjectionMode getProjectionMode() const { return m_projectionMode; }
    void setOrthoSize(float size) { m_orthoSize = size; }  // Half-height of ortho view
    float getOrthoSize() const { return m_orthoSize; }
    void toggleProjectionMode() { m_projectionMode = (m_projectionMode == ProjectionMode::Perspective) ? ProjectionMode::Orthographic : ProjectionMode::Perspective; }

    // View presets for standard orthographic views
    void setViewPreset(ViewPreset preset, const glm::vec3& targetCenter = glm::vec3(0));
    ViewPreset getViewPreset() const { return m_viewPreset; }

    // Get ray direction from camera through normalized screen point (0-1)
    glm::vec3 screenToWorldRay(float normalizedX, float normalizedY, float aspectRatio) const;

    // Movement mode configuration
    void setGravity(float gravity) { m_gravity = gravity; }
    void setJumpVelocity(float velocity) { m_jumpVelocity = velocity; }
    void setEyeHeight(float height) { m_eyeHeight = height; }
    float getEyeHeight() const { return m_eyeHeight; }
    void setDoubleTapWindow(float seconds) { m_doubleTapWindow = seconds; }
    void setMaxSlopeAngle(float degrees) { m_maxSlopeAngle = degrees; }
    void setCollisionRadius(float radius) { m_collisionRadius = radius; }

    // Noclip mode - disables all terrain collision (for editor mode)
    void setNoClip(bool noClip) { m_noClip = noClip; }
    bool isNoClip() const { return m_noClip; }

    // AABB wall collision boxes (e.g. basement walls)
    void addCollisionBox(const glm::vec3& bmin, const glm::vec3& bmax) {
        m_collisionBoxes.push_back({bmin, bmax});
    }
    void clearCollisionBoxes() { m_collisionBoxes.clear(); }

private:
    void resolveAABBCollision(const glm::vec3& oldPos, glm::vec3& newPos);
    void updateVectors();

    glm::vec3 m_position;
    glm::vec3 m_front{0, 0, -1};
    glm::vec3 m_up{0, 1, 0};
    glm::vec3 m_right{1, 0, 0};
    glm::vec3 m_worldUp{0, 1, 0};

    float m_yaw = -90.0f;   // Looking along -Z
    float m_pitch = 0.0f;
    float m_speed = 50.0f;
    float m_mouseSensitivity = 0.1f;
    float m_fov = 60.0f;

    // Orthographic projection
    ProjectionMode m_projectionMode = ProjectionMode::Perspective;
    float m_orthoSize = 5.0f;  // Half-height of orthographic view
    ViewPreset m_viewPreset = ViewPreset::Custom;

    // Movement mode state
    MovementMode m_movementMode = MovementMode::Fly;
    float m_verticalVelocity = 0.0f;
    bool m_onGround = false;
    float m_smoothedGroundHeight = 0.0f; // Smoothly tracked ground level

    // Physics constants
    float m_gravity = 30.0f;        // Units per second squared
    float m_jumpVelocity = 12.0f;   // Initial upward velocity when jumping
    float m_eyeHeight = 1.7f;       // Height of camera above feet (player ~6'1")
    float m_maxSlopeAngle = 60.0f;  // Maximum walkable slope in degrees
    float m_collisionRadius = 0.5f; // Radius around player to check for terrain collision

    // Double-tap detection for fly mode toggle
    float m_lastSpaceTime = -1.0f;
    float m_doubleTapWindow = 0.3f; // Seconds to detect double-tap
    float m_currentTime = 0.0f;     // Accumulated time for double-tap detection

    // Noclip mode - camera ignores terrain collision (for editor mode)
    bool m_noClip = false;

    // AABB wall collision boxes
    std::vector<CollisionBox> m_collisionBoxes;
};

} // namespace eden
