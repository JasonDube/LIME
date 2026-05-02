#pragma once

#include <eden/Transform.hpp>
#include <eden/Action.hpp>
#include <eden/Animation.hpp>
#include <glm/glm.hpp>
#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>

namespace eden {

struct ModelVertex;  // Forward declaration

// Bullet Physics collision types
enum class BulletCollisionType {
    NONE = 0,
    BOX,           // Simple oriented box (rotates with object)
    CONVEX_HULL,   // Convex hull computed from vertices (tighter fit)
    MESH           // Triangle mesh (most accurate, most expensive)
};

// Get string name for bullet collision type
inline const char* getBulletCollisionTypeName(BulletCollisionType type) {
    switch (type) {
        case BulletCollisionType::NONE:        return "None";
        case BulletCollisionType::BOX:         return "Box";
        case BulletCollisionType::CONVEX_HULL: return "Convex Hull";
        case BulletCollisionType::MESH:        return "Mesh";
        default: return "Unknown";
    }
}

// Type of being for dialogue/interaction purposes
enum class BeingType {
    STATIC = 0,    // Non-interactive object (default)
    HUMAN,         // Human character
    CLONE,         // Cloned human
    ROBOT,         // Mechanical robot
    ANDROID,       // Human-like robot
    CYBORG,        // Part human, part machine
    ALIEN,         // Extraterrestrial being
    EVE,           // Eve companion android
    AI_ARCHITECT,  // AI world architect (Xenk)
    ALGOBOT,       // Algorithmic bot — executes Grove scripts, no chat
    EDEN_COMPANION // EDEN companion — tabula rasa AI partner (Liora etc.)
};

// Get string name for being type
inline const char* getBeingTypeName(BeingType type) {
    switch (type) {
        case BeingType::STATIC:       return "Static";
        case BeingType::HUMAN:        return "Human";
        case BeingType::CLONE:        return "Clone";
        case BeingType::ROBOT:        return "Robot";
        case BeingType::ANDROID:      return "Android";
        case BeingType::CYBORG:       return "Cyborg";
        case BeingType::ALIEN:        return "Alien";
        case BeingType::EVE:          return "Eve";
        case BeingType::AI_ARCHITECT: return "AI Architect";
        case BeingType::ALGOBOT:      return "AlgoBot";
        case BeingType::EDEN_COMPANION: return "EDEN Companion";
        default: return "Unknown";
    }
}

// Check if being type is sentient (can be talked to)
inline bool isSentient(BeingType type) {
    return type != BeingType::STATIC;
}

// Primitive types for programmatically created objects
enum class PrimitiveType {
    None = 0,       // GLB model (uses modelPath)
    Cube = 1,
    Cylinder = 2,
    SpawnMarker = 3,
    Door = 4        // Level transition trigger zone
};

// Axis-aligned bounding box for picking
struct AABB {
    glm::vec3 min{0};
    glm::vec3 max{0};

    glm::vec3 getCenter() const { return (min + max) * 0.5f; }
    glm::vec3 getSize() const { return max - min; }

    // Ray-AABB intersection test, returns distance or -1 if no hit
    float intersect(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const;
};

class SceneObject {
public:
    SceneObject() = default;
    SceneObject(const std::string& name);

    // Identity
    void setName(const std::string& name) { m_name = name; }
    const std::string& getName() const { return m_name; }

    // Description (visible to AI perception)
    void setDescription(const std::string& desc) { m_description = desc; }
    const std::string& getDescription() const { return m_description; }

    // Building type (from building catalog, e.g. "farm", "house"). Empty = not a building.
    void setBuildingType(const std::string& type) { m_buildingType = type; }
    const std::string& getBuildingType() const { return m_buildingType; }

    // Source file path (for save/load)
    void setModelPath(const std::string& path) { m_modelPath = path; }
    const std::string& getModelPath() const { return m_modelPath; }

    // Primitive type (for programmatically created objects)
    void setPrimitiveType(PrimitiveType type) { m_primitiveType = type; }
    PrimitiveType getPrimitiveType() const { return m_primitiveType; }
    bool isPrimitive() const { return m_primitiveType != PrimitiveType::None; }

    void setPrimitiveSize(float size) { m_primitiveSize = size; }
    float getPrimitiveSize() const { return m_primitiveSize; }

    void setPrimitiveRadius(float radius) { m_primitiveRadius = radius; }
    float getPrimitiveRadius() const { return m_primitiveRadius; }

    void setPrimitiveHeight(float height) { m_primitiveHeight = height; }
    float getPrimitiveHeight() const { return m_primitiveHeight; }

    void setPrimitiveSegments(int segments) { m_primitiveSegments = segments; }
    int getPrimitiveSegments() const { return m_primitiveSegments; }

    void setPrimitiveColor(const glm::vec4& color) { m_primitiveColor = color; }
    const glm::vec4& getPrimitiveColor() const { return m_primitiveColor; }

    // Door properties (for level transitions)
    bool isDoor() const { return m_primitiveType == PrimitiveType::Door; }
    void setDoorId(const std::string& id) { m_doorId = id; }
    const std::string& getDoorId() const { return m_doorId; }
    void setTargetLevel(const std::string& path) { m_targetLevel = path; }
    const std::string& getTargetLevel() const { return m_targetLevel; }
    void setTargetDoorId(const std::string& id) { m_targetDoorId = id; }
    const std::string& getTargetDoorId() const { return m_targetDoorId; }

    // Transform
    Transform& getTransform() { return m_transform; }
    const Transform& getTransform() const { return m_transform; }

    // Euler rotation (stored separately to avoid gimbal lock in UI)
    void setEulerRotation(const glm::vec3& euler) {
        m_eulerRotation = euler;
        m_transform.setRotation(euler);  // Also update quaternion
    }
    const glm::vec3& getEulerRotation() const { return m_eulerRotation; }

    // Reset transform to defaults
    void resetTransform() {
        m_transform.setPosition(glm::vec3(0));
        m_transform.setScale(glm::vec3(1));
        m_eulerRotation = glm::vec3(0);
        m_transform.setRotation(m_eulerRotation);
    }

    // Rendering data
    void setBufferHandle(uint32_t handle) { m_bufferHandle = handle; }
    uint32_t getBufferHandle() const { return m_bufferHandle; }

    void setIndexCount(uint32_t count) { m_indexCount = count; }
    uint32_t getIndexCount() const { return m_indexCount; }

    void setVertexCount(uint32_t count) { m_vertexCount = count; }
    uint32_t getVertexCount() const { return m_vertexCount; }

    // Bounding box (local space)
    void setLocalBounds(const AABB& bounds) { m_localBounds = bounds; }
    const AABB& getLocalBounds() const { return m_localBounds; }

    // Get world-space bounding box (transformed)
    AABB getWorldBounds();

    // Selection state
    void setSelected(bool selected) { m_selected = selected; }
    bool isSelected() const { return m_selected; }

    // Visibility
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const { return m_visible; }

    // X-Ray mode (render both sides, see through backfaces)
    void setXRay(bool xray) { m_xray = xray; }
    bool isXRay() const { return m_xray; }

    // Flat shading: fragment shader derives the face normal at draw time
    // from screen-space derivatives, so verts don't need to be split for
    // a faceted look.
    void setFlatShading(bool flat) { m_flatShading = flat; }
    bool isFlatShaded() const { return m_flatShading; }

    // Collision (simple built-in)
    void setAABBCollision(bool collision) { m_aabbCollision = collision; }
    bool hasAABBCollision() const { return m_aabbCollision; }
    void setPolygonCollision(bool collision) { m_polygonCollision = collision; }
    bool hasPolygonCollision() const { return m_polygonCollision; }
    bool hasCollision() const { return m_aabbCollision || m_polygonCollision || m_bulletCollisionType != BulletCollisionType::NONE; }

    // Bullet Physics collision
    void setBulletCollisionType(BulletCollisionType type) { m_bulletCollisionType = type; }
    BulletCollisionType getBulletCollisionType() const { return m_bulletCollisionType; }
    bool hasBulletCollision() const { return m_bulletCollisionType != BulletCollisionType::NONE; }

    // Kinematic platform (lift) support - moves through Jolt physics in play mode
    void setKinematicPlatform(bool isKinematic) { m_isKinematicPlatform = isKinematic; }
    bool isKinematicPlatform() const { return m_isKinematicPlatform; }

    // Jolt body ID (runtime only, for kinematic platforms in play mode)
    void setJoltBodyId(uint32_t id) { m_joltBodyId = id; }
    uint32_t getJoltBodyId() const { return m_joltBodyId; }
    bool hasJoltBody() const { return m_joltBodyId != UINT32_MAX; }
    void clearJoltBody() { m_joltBodyId = UINT32_MAX; }

    // Physics offset (local center offset for physics body alignment)
    void setPhysicsOffset(const glm::vec3& offset) { m_physicsOffset = offset; }
    glm::vec3 getPhysicsOffset() const { return m_physicsOffset; }

    // Frozen transform - stores rotation/scale that was baked into vertices
    // Used to re-apply freeze on level load
    void setFrozenTransform(const glm::vec3& rotation, const glm::vec3& scale) {
        m_frozenRotation = rotation;
        m_frozenScale = scale;
        m_hasFrozenTransform = true;
    }
    bool hasFrozenTransform() const { return m_hasFrozenTransform; }
    glm::vec3 getFrozenRotation() const { return m_frozenRotation; }
    glm::vec3 getFrozenScale() const { return m_frozenScale; }
    void clearFrozenTransform() { m_hasFrozenTransform = false; }

    // Being type (for dialogue/interaction)
    void setBeingType(BeingType type) { m_beingType = type; }
    BeingType getBeingType() const { return m_beingType; }
    bool isSentient() const { return eden::isSentient(m_beingType); }

    // Schedule system - sequential actions (patrol paths, waits, etc.)
    void setSchedule(const std::vector<Action>& schedule) { m_schedule = schedule; m_currentScheduleIndex = 0; }
    const std::vector<Action>& getSchedule() const { return m_schedule; }
    std::vector<Action>& getSchedule() { return m_schedule; }
    void clearSchedule() { m_schedule.clear(); m_currentScheduleIndex = 0; }
    bool hasSchedule() const { return !m_schedule.empty(); }
    size_t getCurrentScheduleIndex() const { return m_currentScheduleIndex; }
    void setCurrentScheduleIndex(size_t idx) { m_currentScheduleIndex = idx; }
    void advanceSchedule() {
        if (m_schedule.empty()) return;
        m_currentScheduleIndex++;
        if (m_currentScheduleIndex >= m_schedule.size()) {
            m_currentScheduleIndex = m_scheduleLoop ? 0 : m_schedule.size() - 1;
        }
    }
    const Action* getCurrentScheduleAction() const {
        if (m_schedule.empty()) return nullptr;
        return &m_schedule[m_currentScheduleIndex];
    }
    void setScheduleLoop(bool loop) { m_scheduleLoop = loop; }
    bool isScheduleLooping() const { return m_scheduleLoop; }

    // Current path being followed (by name)
    void setCurrentPathName(const std::string& name) { m_currentPathName = name; }
    const std::string& getCurrentPathName() const { return m_currentPathName; }

    // Current path waypoints (positions, not node IDs)
    void setCurrentPathWaypoints(const std::vector<glm::vec3>& waypoints) {
        m_pathWaypoints = waypoints;
        m_currentWaypointIndex = 0;
    }
    const std::vector<glm::vec3>& getCurrentPathWaypoints() const { return m_pathWaypoints; }
    bool hasPathWaypoints() const { return !m_pathWaypoints.empty(); }
    void clearPathWaypoints() { m_pathWaypoints.clear(); m_currentWaypointIndex = 0; m_currentPathName.clear(); }

    // Legacy patrol path support (for backwards compatibility)
    void setPatrolPath(const std::vector<uint32_t>& path) { m_patrolPath = path; }
    const std::vector<uint32_t>& getPatrolPath() const { return m_patrolPath; }
    void clearPatrolPath() { m_patrolPath.clear(); m_currentWaypointIndex = 0; }
    bool hasPatrolPath() const { return !m_patrolPath.empty() || !m_pathWaypoints.empty(); }

    void setPatrolSpeed(float speed) { m_patrolSpeed = speed; }
    float getPatrolSpeed() const { return m_patrolSpeed; }

    void setPatrolLoop(bool loop) { m_patrolLoop = loop; }
    bool isPatrolLooping() const { return m_patrolLoop; }

    void setPatrolPaused(bool paused) { m_patrolPaused = paused; }
    bool isPatrolPaused() const { return m_patrolPaused; }

    // Current waypoint tracking
    size_t getCurrentWaypointIndex() const { return m_currentWaypointIndex; }
    void setCurrentWaypointIndex(size_t index) { m_currentWaypointIndex = index; }
    void advanceWaypoint() {
        size_t pathSize = m_pathWaypoints.empty() ? m_patrolPath.size() : m_pathWaypoints.size();
        if (pathSize == 0) return;
        m_currentWaypointIndex++;
        if (m_currentWaypointIndex >= pathSize) {
            if (m_patrolLoop) {
                m_currentWaypointIndex = 0;
            } else {
                m_currentWaypointIndex = pathSize - 1;
                m_pathComplete = true;  // Signal path is done
            }
        }
    }
    glm::vec3 getCurrentWaypointPosition() const {
        if (!m_pathWaypoints.empty() && m_currentWaypointIndex < m_pathWaypoints.size()) {
            return m_pathWaypoints[m_currentWaypointIndex];
        }
        return glm::vec3(0);  // Legacy path uses node lookup externally
    }
    uint32_t getCurrentWaypointId() const {
        if (m_patrolPath.empty()) return 0;
        return m_patrolPath[m_currentWaypointIndex];
    }
    bool isPathComplete() const { return m_pathComplete; }
    void resetPathComplete() { m_pathComplete = false; }

    // Wait timer for schedule WAIT actions
    void setWaitTimer(float time) { m_waitTimer = time; }
    float getWaitTimer() const { return m_waitTimer; }
    void decrementWaitTimer(float dt) { m_waitTimer -= dt; }

    // Color adjustments (HSB)
    void setHueShift(float hue) { m_hueShift = hue; }
    float getHueShift() const { return m_hueShift; }

    void setSaturation(float sat) { m_saturation = sat; }
    float getSaturation() const { return m_saturation; }

    void setBrightness(float bright) { m_brightness = bright; }
    float getBrightness() const { return m_brightness; }

    // Hit flash effect (for damage feedback)
    void triggerHitFlash(float duration = 0.15f) { m_hitFlashTimer = duration; }
    void updateHitFlash(float deltaTime) { if (m_hitFlashTimer > 0) m_hitFlashTimer -= deltaTime; }
    bool isHitFlashing() const { return m_hitFlashTimer > 0; }
    float getHitFlashTimer() const { return m_hitFlashTimer; }

    // Combat/alert state (for NPCs reacting to attacks)
    void setUnderAttack(bool attacked, const glm::vec3& attackerPos = glm::vec3(0)) {
        m_isUnderAttack = attacked;
        if (attacked) m_attackerPosition = attackerPos;
    }
    bool isUnderAttack() const { return m_isUnderAttack; }
    const glm::vec3& getAttackerPosition() const { return m_attackerPosition; }
    void clearAttackState() { m_isUnderAttack = false; }

    // Health system
    void setHealth(float health) { m_health = std::clamp(health, 0.0f, m_maxHealth); }
    float getHealth() const { return m_health; }
    void setMaxHealth(float max) { m_maxHealth = max; m_health = std::min(m_health, max); }
    float getMaxHealth() const { return m_maxHealth; }
    float getHealthPercent() const { return m_maxHealth > 0 ? m_health / m_maxHealth : 0.0f; }
    bool isAlive() const { return m_health > 0.0f; }
    bool isDead() const { return m_health <= 0.0f; }
    void takeDamage(float damage) {
        m_health = std::max(0.0f, m_health - damage);
        triggerHitFlash(0.4f);
    }
    void heal(float amount) { m_health = std::min(m_maxHealth, m_health + amount); }
    void resetHealth() { m_health = m_maxHealth; }

    // Cargo jettison tracking (for traders under attack)
    bool hasJettisonedCargo() const { return m_hasJettisonedCargo; }
    void setJettisonedCargo(bool jettisoned) { m_hasJettisonedCargo = jettisoned; }

    // Ejection state (pilot ejected from destroyed vehicle)
    bool hasEjected() const { return m_hasEjected; }
    void setEjected(bool ejected) { m_hasEjected = ejected; }

    // Carry state (NPC carrying a world object)
    bool isCarrying() const { return m_carriedItemObject != nullptr; }
    void setCarriedItem(const std::string& itemName, SceneObject* itemObj) {
        m_carriedItemName = itemName;
        m_carriedItemObject = itemObj;
    }
    void clearCarriedItem() {
        m_carriedItemName.clear();
        m_carriedItemObject = nullptr;
    }
    const std::string& getCarriedItemName() const { return m_carriedItemName; }
    SceneObject* getCarriedItemObject() const { return m_carriedItemObject; }

    // Scripts (simple script names like "trader", "patrol", etc.)
    void addScript(const std::string& script) {
        if (std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
            m_scripts.push_back(script);
        }
    }
    void removeScript(const std::string& script) {
        m_scripts.erase(std::remove(m_scripts.begin(), m_scripts.end(), script), m_scripts.end());
    }
    bool hasScript(const std::string& script) const {
        return std::find(m_scripts.begin(), m_scripts.end(), script) != m_scripts.end();
    }
    const std::vector<std::string>& getScripts() const { return m_scripts; }
    std::vector<std::string>& getScripts() { return m_scripts; }

    // Grove script for AlgoBot execution
    void setGroveScriptPath(const std::string& path) { m_groveScriptPath = path; }
    const std::string& getGroveScriptPath() const { return m_groveScriptPath; }

    // Trader script link (when "trader" script is active)
    void setTraderId(uint32_t id) { m_traderId = id; }
    uint32_t getTraderId() const { return m_traderId; }
    bool isTrader() const { return m_traderId != 0 || hasScript("trader"); }

    // Behaviors
    void addBehavior(const Behavior& behavior);
    std::vector<Behavior>& getBehaviors() { return m_behaviors; }
    const std::vector<Behavior>& getBehaviors() const { return m_behaviors; }
    bool hasBehaviors() const { return !m_behaviors.empty(); }

    // Trigger a behavior by type (e.g., ON_INTERACT)
    void triggerBehavior(TriggerType type);

    // Trigger a behavior by signal name (for ON_SIGNAL triggers)
    void triggerBehaviorBySignal(const std::string& signalName);

    // Update active behavior players
    void updateBehaviors(float deltaTime);

    // Check if any behavior is currently playing
    bool isPlayingBehavior() const;

    // Signal callback for SEND_SIGNAL actions
    // Callback signature: void(signalName, targetName, senderObject)
    using SignalCallback = std::function<void(const std::string&, const std::string&, SceneObject*)>;
    static void setSignalCallback(SignalCallback callback) { s_signalCallback = callback; }
    static SignalCallback s_signalCallback;

    // Active behavior tracking (for FOLLOW_PATH, etc.)
    void setActiveBehaviorIndex(int index) { m_activeBehaviorIndex = index; }
    int getActiveBehaviorIndex() const { return m_activeBehaviorIndex; }
    void setActiveActionIndex(int index) { m_activeActionIndex = index; }
    int getActiveActionIndex() const { return m_activeActionIndex; }
    bool hasActiveBehavior() const { return m_activeBehaviorIndex >= 0; }
    void clearActiveBehavior() { m_activeBehaviorIndex = -1; m_activeActionIndex = 0; }

    // Daily schedule mode - behaviors reset at midnight to repeat each day
    void setDailySchedule(bool daily) { m_dailySchedule = daily; }
    bool hasDailySchedule() const { return m_dailySchedule; }

    // MOVE_TO action support (smooth movement to target position)
    void startMoveTo(const glm::vec3& from, const glm::vec3& to, float duration, bool linear = false) {
        m_moveStartPos = from;
        m_moveEndPos = to;
        m_moveDuration = duration;
        m_moveTimer = 0.0f;
        m_moveLinear = linear;
        m_isMovingTo = true;
    }
    void updateMoveTo(float deltaTime) {
        if (!m_isMovingTo) return;
        if (m_moveUpdatedThisFrame) return;  // Already updated this frame
        m_moveUpdatedThisFrame = true;
        m_moveTimer += deltaTime;
        float t = std::min(m_moveTimer / m_moveDuration, 1.0f);
        // Apply easing unless linear mode
        if (!m_moveLinear) {
            t = t * t * (3.0f - 2.0f * t);  // Smooth ease in-out
        }
        glm::vec3 pos = glm::mix(m_moveStartPos, m_moveEndPos, t);
        m_transform.setPosition(pos);
        if (m_moveTimer >= m_moveDuration) {
            m_isMovingTo = false;
        }
    }
    bool isMovingTo() const { return m_isMovingTo; }
    void stopMoveTo() { m_isMovingTo = false; }
    void resetMoveUpdateFlag() { m_moveUpdatedThisFrame = false; }

    // Get the instantaneous velocity of the MOVE_TO animation
    // This is calculated analytically from the easing function, not from position deltas
    glm::vec3 getMoveVelocity() const {
        if (!m_isMovingTo || m_moveDuration <= 0.0f) return glm::vec3(0.0f);

        float t = std::min(m_moveTimer / m_moveDuration, 1.0f);
        // Derivative of position with respect to time
        // Position p(t) = mix(start, end, f(t)) where f is the easing function
        // Velocity = (end - start) * f'(t) / duration
        float dt_df;  // Derivative of easing function
        if (m_moveLinear) {
            dt_df = 1.0f;  // Linear: f(t) = t, f'(t) = 1
        } else {
            // Smoothstep: f(t) = t * t * (3 - 2t) = 3t² - 2t³
            // f'(t) = 6t - 6t² = 6t(1-t)
            dt_df = 6.0f * t * (1.0f - t);
        }
        glm::vec3 displacement = m_moveEndPos - m_moveStartPos;
        return displacement * (dt_df / m_moveDuration);
    }

    // Get the movement's target position and progress
    glm::vec3 getMoveTargetPosition() const { return m_moveEndPos; }
    glm::vec3 getMoveStartPosition() const { return m_moveStartPos; }
    float getMoveProgress() const {
        if (m_moveDuration <= 0.0f) return 1.0f;
        return std::min(m_moveTimer / m_moveDuration, 1.0f);
    }

    // TURN_TO action support (smooth yaw rotation)
    void startTurnTo(float fromYaw, float toYaw, float duration) {
        m_turnStartYaw = fromYaw;
        m_turnEndYaw = toYaw;
        m_turnDuration = duration;
        m_turnTimer = 0.0f;
        m_isTurning = true;
    }
    void updateTurnTo(float deltaTime) {
        if (!m_isTurning) return;
        m_turnTimer += deltaTime;
        float t = std::min(m_turnTimer / m_turnDuration, 1.0f);
        // Smooth ease in-out
        t = t * t * (3.0f - 2.0f * t);
        float yaw = glm::mix(m_turnStartYaw, m_turnEndYaw, t);
        glm::vec3 rot = m_eulerRotation;
        rot.y = yaw;
        setEulerRotation(rot);
        if (m_turnTimer >= m_turnDuration) {
            m_isTurning = false;
        }
    }
    bool isTurning() const { return m_isTurning; }
    void stopTurning() { m_isTurning = false; }

    // Skinned model support (for animated models)
    void setSkinnedModelHandle(uint32_t handle) { m_skinnedModelHandle = handle; m_isSkinned = true; }
    uint32_t getSkinnedModelHandle() const { return m_skinnedModelHandle; }
    bool isSkinned() const { return m_isSkinned; }
    void setCurrentAnimation(const std::string& name) { m_currentAnimation = name; }
    const std::string& getCurrentAnimation() const { return m_currentAnimation; }
    void setAnimationNames(const std::vector<std::string>& names) { m_animationNames = names; }
    const std::vector<std::string>& getAnimationNames() const { return m_animationNames; }

    // Editor skeleton (for LIME rigging - separate from GLB skinned model)
    void setEditorSkeleton(std::unique_ptr<Skeleton> skel) { m_editorSkeleton = std::move(skel); m_isEditorRigged = true; }
    const Skeleton* getEditorSkeleton() const { return m_editorSkeleton.get(); }
    Skeleton* getEditorSkeleton() { return m_editorSkeleton.get(); }
    bool hasEditorSkeleton() const { return m_isEditorRigged && m_editorSkeleton != nullptr; }
    void clearEditorSkeleton() { m_editorSkeleton.reset(); m_isEditorRigged = false; }

    // Mesh data for raycasting (CPU copy)
    void setMeshData(const std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices);
    const std::vector<ModelVertex>& getVertices() const { return m_vertices; }
    const std::vector<uint32_t>& getIndices() const { return m_indices; }
    bool hasMeshData() const { return !m_vertices.empty(); }

    // EditableMesh data storage (preserves quad topology)
    // These structs mirror the half-edge data structure for serialization
    struct StoredHEVertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
        glm::vec4 color;
        uint32_t halfEdgeIndex;
        bool selected;
        glm::ivec4 boneIndices = glm::ivec4(0);
        glm::vec4 boneWeights = glm::vec4(0.0f);
    };
    struct StoredHalfEdge {
        uint32_t vertexIndex;
        uint32_t faceIndex;
        uint32_t nextIndex;
        uint32_t prevIndex;
        uint32_t twinIndex;
    };
    struct StoredHEFace {
        uint32_t halfEdgeIndex;
        uint32_t vertexCount;
        bool selected;
    };

    void setEditableMeshData(const std::vector<StoredHEVertex>& vertices,
                             const std::vector<StoredHalfEdge>& halfEdges,
                             const std::vector<StoredHEFace>& faces) {
        m_heVertices = vertices;
        m_heHalfEdges = halfEdges;
        m_heFaces = faces;
    }
    const std::vector<StoredHEVertex>& getHEVertices() const { return m_heVertices; }
    const std::vector<StoredHalfEdge>& getHEHalfEdges() const { return m_heHalfEdges; }
    const std::vector<StoredHEFace>& getHEFaces() const { return m_heFaces; }
    bool hasEditableMeshData() const { return !m_heVertices.empty(); }
    void clearEditableMeshData() {
        m_heVertices.clear();
        m_heHalfEdges.clear();
        m_heFaces.clear();
    }

    // Named control points for modular part connections
    struct StoredControlPoint {
        uint32_t vertexIndex;
        std::string name;
    };
    void setControlPoints(const std::vector<StoredControlPoint>& cps) { m_controlPoints = cps; }
    const std::vector<StoredControlPoint>& getControlPoints() const { return m_controlPoints; }
    bool hasControlPoints() const { return !m_controlPoints.empty(); }

    // Connection ports for pipe/assembly snap system (vertex-independent)
    struct StoredPort {
        std::string name;
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, 1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
    };
    void setPorts(const std::vector<StoredPort>& ports) { m_ports = ports; }
    const std::vector<StoredPort>& getPorts() const { return m_ports; }
    bool hasPorts() const { return !m_ports.empty(); }

    // Texture data for painting (CPU copy)
    void setTextureData(const std::vector<unsigned char>& data, int width, int height);
    std::vector<unsigned char>& getTextureData() { return m_textureData; }
    const std::vector<unsigned char>& getTextureData() const { return m_textureData; }
    int getTextureWidth() const { return m_textureWidth; }
    int getTextureHeight() const { return m_textureHeight; }
    bool hasTextureData() const { return !m_textureData.empty(); }
    void clearTextureData() { m_textureData.clear(); m_textureWidth = 0; m_textureHeight = 0; m_textureModified = false; }
    void markTextureModified() { m_textureModified = true; }
    bool isTextureModified() const { return m_textureModified; }
    void clearTextureModified() { m_textureModified = false; }

    // Expression texture system (for NPC facial expression swapping)
    struct ExpressionTexture {
        std::string name;           // "happy", "sad", "angry", etc.
        std::vector<unsigned char> pixels;
        int width, height;
    };

    void addExpression(const std::string& name, const std::vector<unsigned char>& pixels, int w, int h);
    int getExpressionCount() const { return static_cast<int>(m_expressions.size()); }
    int getCurrentExpression() const { return m_currentExpression; }
    const std::string& getExpressionName(int index) const;
    bool setExpression(int index);  // returns true if changed (caller must call updateTexture)
    bool setExpressionByName(const std::string& name);  // convenience lookup by name
    const ExpressionTexture* getExpression(int index) const;

    // Texture undo for painting
    void saveTextureState();    // Call before starting a paint stroke
    bool undoTexture();         // Returns true if undo was performed
    bool canUndoTexture() const { return !m_textureUndoStack.empty(); }
    void clearTextureUndoStack() { m_textureUndoStack.clear(); }

    // Raycast against mesh triangles, returns hit info
    struct RayHit {
        bool hit = false;
        float distance = 0.0f;
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
        uint32_t triangleIndex = 0;
    };
    RayHit raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const;

    // Paint on texture at UV coordinates
    // squareBrush: true = square brush with no falloff (pixel art style), false = circular with soft falloff
    void paintAt(const glm::vec2& uv, const glm::vec3& color, float radius, float strength, bool squareBrush = false);

    // Smear brush - picks up and drags color
    // Returns the color sampled at the UV position (to be carried to next stroke point)
    glm::vec3 smearAt(const glm::vec2& uv, const glm::vec3& carriedColor, float radius, float strength, float pickup = 0.3f);

    // Stamp an image onto texture at UV coordinates
    void stampAt(const glm::vec2& uv, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);
    // Stamp with UV density correction (uses triangle to calculate proper scale)
    void stampAt(const glm::vec2& uv, uint32_t triangleIndex, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);

    // Stamp preview (temporary, can be reverted)
    void stampPreviewAt(const glm::vec2& uv, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);
    // Stamp preview with UV density correction
    void stampPreviewAt(const glm::vec2& uv, uint32_t triangleIndex, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);
    void clearStampPreview();

    // TRUE project-from-view stamping: projects stamp from camera onto 3D surface
    // Each stamp pixel is raycast individually to find its UV on the mesh
    void stampProjectedFromView(const glm::vec3& hitPoint, const glm::vec3& camPos, const glm::vec3& camRight, const glm::vec3& camUp,
                                 const unsigned char* stampData, int stampWidth, int stampHeight,
                                 float worldSizeH, float worldSizeV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);
    void stampProjectedFromViewPreview(const glm::vec3& hitPoint, const glm::vec3& camPos, const glm::vec3& camRight, const glm::vec3& camUp,
                                        const unsigned char* stampData, int stampWidth, int stampHeight,
                                        float worldSizeH, float worldSizeV, float rotation = 0.0f, float opacity = 1.0f, bool flipH = false, bool flipV = false);

    // Calculate UV density scale at a triangle (returns scale factors to correct for UV stretching)
    glm::vec2 getUVDensityScale(uint32_t triangleIndex) const;

    // Calculate full UV correction (scale + rotation) for world-space aligned stamps
    // Returns: x = scaleU correction, y = scaleV correction, z = rotation correction in degrees
    glm::vec3 getUVCorrection(uint32_t triangleIndex) const;
    bool hasStampPreview() const { return m_hasStampPreview; }

    // Stamp to quad: map stamp corners directly to quad's 4 UV corners
    // uvs should be in order: bottom-left, bottom-right, top-right, top-left (CCW from bottom-left)
    void stampToQuad(const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2, const glm::vec2& uv3,
                     const unsigned char* stampData, int stampWidth, int stampHeight, float opacity = 1.0f);

    // Generate new UV coordinates using box projection (non-overlapping)
    void generateBoxUVs();

    // Generate uniform square UVs - every quad gets same size square, packed in grid
    // This makes stamps appear identical on every face regardless of 3D size
    void generateUniformSquareUVs();

    // Seam Buster: extend edge pixels outward to prevent texture seams
    // Samples colors from UV island edges and dilates them outward by specified pixels
    void applySeamBuster(int pixels = 2);

private:
    std::string m_name = "Object";
    std::string m_description;  // Description visible to AI perception
    std::string m_buildingType; // Building catalog type (e.g. "farm"). Empty = not a building.
    std::string m_modelPath;  // Source file path for save/load

    // Primitive object support
    PrimitiveType m_primitiveType = PrimitiveType::None;
    float m_primitiveSize = 1.0f;       // Size for cube
    float m_primitiveRadius = 0.5f;     // Radius for cylinder
    float m_primitiveHeight = 1.0f;     // Height for cylinder
    int m_primitiveSegments = 16;       // Segments for cylinder
    glm::vec4 m_primitiveColor = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);

    // Door properties (for level transitions)
    std::string m_doorId;
    std::string m_targetLevel;
    std::string m_targetDoorId;

    Transform m_transform;
    glm::vec3 m_eulerRotation{0.0f};  // Stored Euler angles to avoid gimbal lock
    uint32_t m_bufferHandle = UINT32_MAX;
    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;
    AABB m_localBounds;
    bool m_selected = false;
    bool m_visible = true;
    bool m_xray = false;             // X-Ray mode (render both sides)
    bool m_flatShading = false;      // Flat (faceted) shading via fragment derivatives
    bool m_aabbCollision = false;    // AABB collision (default off)
    bool m_polygonCollision = false; // Polygon collision (default off)
    BulletCollisionType m_bulletCollisionType = BulletCollisionType::NONE;  // Bullet physics collision
    bool m_isKinematicPlatform = false;  // Is this a lift/moving platform?
    uint32_t m_joltBodyId = UINT32_MAX;  // Runtime Jolt body ID (play mode only)
    glm::vec3 m_physicsOffset{0.0f};     // Local center offset for physics body alignment

    // Frozen transform - rotation/scale baked into vertices (for serialization)
    bool m_hasFrozenTransform = false;
    glm::vec3 m_frozenRotation{0.0f};
    glm::vec3 m_frozenScale{1.0f};

    BeingType m_beingType = BeingType::STATIC;

    // Schedule system
    std::vector<Action> m_schedule;
    size_t m_currentScheduleIndex = 0;
    bool m_scheduleLoop = true;
    float m_waitTimer = 0.0f;

    // Path-based patrol (new system)
    std::string m_currentPathName;
    std::vector<glm::vec3> m_pathWaypoints;
    bool m_pathComplete = false;

    // Legacy patrol path (node IDs)
    std::vector<uint32_t> m_patrolPath;
    size_t m_currentWaypointIndex = 0;
    float m_patrolSpeed = 5.0f;
    bool m_patrolLoop = true;
    bool m_patrolPaused = false;

    // Color adjustments
    float m_hueShift = 0.0f;      // -180 to +180 degrees
    float m_saturation = 1.0f;    // 0 to 2 (1 = original)
    float m_brightness = 1.0f;    // 0 to 2 (1 = original)
    float m_hitFlashTimer = 0.0f; // Timer for hit flash effect

    // Carry state
    std::string m_carriedItemName;
    SceneObject* m_carriedItemObject = nullptr;

    // Combat state
    bool m_isUnderAttack = false;
    glm::vec3 m_attackerPosition{0.0f};

    // Health system
    float m_health = 100.0f;
    float m_maxHealth = 100.0f;
    bool m_hasJettisonedCargo = false;
    bool m_hasEjected = false;

    // Scripts
    std::vector<std::string> m_scripts;
    uint32_t m_traderId = 0;  // Link to TraderAI when "trader" script is active
    std::string m_groveScriptPath;  // .grove file for AlgoBot execution

    // Behaviors
    std::vector<Behavior> m_behaviors;
    std::vector<BehaviorPlayer> m_behaviorPlayers;
    int m_activeBehaviorIndex = -1;  // Currently executing behavior
    int m_activeActionIndex = 0;     // Current action within behavior
    bool m_dailySchedule = false;    // Reset behaviors at midnight for daily cycle

    // MOVE_TO action state
    glm::vec3 m_moveStartPos{0.0f};
    glm::vec3 m_moveEndPos{0.0f};
    float m_moveDuration = 1.0f;
    float m_moveTimer = 0.0f;
    bool m_isMovingTo = false;
    bool m_moveLinear = false;
    bool m_moveUpdatedThisFrame = false;  // Prevent double-updates

    // TURN_TO action state
    float m_turnStartYaw = 0.0f;
    float m_turnEndYaw = 0.0f;
    float m_turnDuration = 0.5f;
    float m_turnTimer = 0.0f;
    bool m_isTurning = false;

    // Mesh data for raycasting
    std::vector<ModelVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    // EditableMesh half-edge data (preserves quad topology)
    std::vector<StoredHEVertex> m_heVertices;
    std::vector<StoredHalfEdge> m_heHalfEdges;
    std::vector<StoredHEFace> m_heFaces;

    // Named control points
    std::vector<StoredControlPoint> m_controlPoints;

    // Connection ports
    std::vector<StoredPort> m_ports;

    // Texture data for painting
    std::vector<unsigned char> m_textureData;
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    bool m_textureModified = false;

    // Expression textures (for NPC face swapping)
    std::vector<ExpressionTexture> m_expressions;
    int m_currentExpression = -1;  // -1 = base texture, 0+ = expression index

    // Texture undo stack for paint operations
    static constexpr size_t MAX_TEXTURE_UNDO_LEVELS = 20;
    std::vector<std::vector<unsigned char>> m_textureUndoStack;

    // Stamp preview (temporary texture backup)
    std::vector<unsigned char> m_previewTextureBackup;
    bool m_hasStampPreview = false;

    // Skinned model data
    uint32_t m_skinnedModelHandle = UINT32_MAX;
    bool m_isSkinned = false;
    std::string m_currentAnimation;
    std::vector<std::string> m_animationNames;

    // Editor skeleton (for LIME-rigged meshes)
    std::unique_ptr<Skeleton> m_editorSkeleton;
    bool m_isEditorRigged = false;
};

} // namespace eden
