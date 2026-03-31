#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "Action.hpp"
#include "Transform.hpp"

namespace eden {

// Entity flags
enum class EntityFlags : uint32_t {
    NONE        = 0,
    VISIBLE     = 1 << 0,
    ACTIVE      = 1 << 1,       // Processes behaviors
    STATIC      = 1 << 2,       // Won't move (optimization hint)
    INTERACTABLE = 1 << 3,      // Can be interacted with by player
    COLLIDABLE  = 1 << 4,       // Participates in collision
};

inline EntityFlags operator|(EntityFlags a, EntityFlags b) {
    return static_cast<EntityFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline EntityFlags operator&(EntityFlags a, EntityFlags b) {
    return static_cast<EntityFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool hasFlag(EntityFlags flags, EntityFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

// The Entity class
class Entity {
public:
    Entity(uint32_t id, const std::string& name = "");

    // Identity
    uint32_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    // Transform
    Transform& getTransform() { return m_transform; }
    const Transform& getTransform() const { return m_transform; }

    // Flags
    EntityFlags getFlags() const { return m_flags; }
    void setFlags(EntityFlags flags) { m_flags = flags; }
    void addFlag(EntityFlags flag) { m_flags = m_flags | flag; }
    void removeFlag(EntityFlags flag) {
        m_flags = static_cast<EntityFlags>(static_cast<uint32_t>(m_flags) & ~static_cast<uint32_t>(flag));
    }
    bool hasFlag(EntityFlags flag) const { return eden::hasFlag(m_flags, flag); }

    // Visual representation (handle to ModelRenderer model, or -1 for none)
    void setModelHandle(uint32_t handle) { m_modelHandle = handle; }
    uint32_t getModelHandle() const { return m_modelHandle; }

    // Behaviors
    void addBehavior(const Behavior& behavior);
    void removeBehavior(const std::string& name);
    std::vector<Behavior>& getBehaviors() { return m_behaviors; }
    const std::vector<Behavior>& getBehaviors() const { return m_behaviors; }

    // Behavior players (runtime state)
    std::vector<BehaviorPlayer>& getBehaviorPlayers() { return m_behaviorPlayers; }

    // Properties (generic key-value storage for game logic)
    void setProperty(const std::string& key, float value) { m_properties[key] = value; }
    float getProperty(const std::string& key, float defaultVal = 0.0f) const;
    bool hasProperty(const std::string& key) const { return m_properties.count(key) > 0; }

    // Tags for grouping/filtering
    void addTag(const std::string& tag) { m_tags.push_back(tag); }
    bool hasTag(const std::string& tag) const;
    const std::vector<std::string>& getTags() const { return m_tags; }

    // Pending destruction (will be cleaned up by EntityManager)
    void markForDestruction() { m_pendingDestroy = true; }
    bool isPendingDestruction() const { return m_pendingDestroy; }

private:
    uint32_t m_id;
    std::string m_name;
    Transform m_transform;
    EntityFlags m_flags = EntityFlags::VISIBLE | EntityFlags::ACTIVE;

    uint32_t m_modelHandle = UINT32_MAX;  // No model by default

    std::vector<Behavior> m_behaviors;
    std::vector<BehaviorPlayer> m_behaviorPlayers;

    std::unordered_map<std::string, float> m_properties;
    std::vector<std::string> m_tags;

    bool m_pendingDestroy = false;
};

} // namespace eden
