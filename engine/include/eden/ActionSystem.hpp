#pragma once

#include "Entity.hpp"
#include <functional>
#include <queue>
#include <memory>

namespace eden {

// Signal sent between entities
struct Signal {
    std::string name;               // Signal identifier (e.g., "DAMAGE", "ACTIVATE")
    uint32_t senderId = 0;          // Entity that sent it
    uint32_t targetId = 0;          // Specific target (0 = broadcast based on name)
    std::string targetName;         // Target by name (if targetId is 0)
    glm::vec3 position{0.0f};       // Where signal originated
    float value = 0.0f;             // Optional value (damage amount, etc.)
};

// Entity template for spawning
struct EntityTemplate {
    std::string name;
    std::string modelPath;          // Path to GLB model (or empty for no model)
    Transform defaultTransform;
    EntityFlags defaultFlags = EntityFlags::VISIBLE | EntityFlags::ACTIVE;
    std::vector<Behavior> behaviors;
    std::unordered_map<std::string, float> defaultProperties;
    std::vector<std::string> tags;
};

// Callback for custom actions
using CustomActionCallback = std::function<void(Entity&, const Action&, ActionSystem&)>;

// The main system that manages entities and processes their behaviors
class ActionSystem {
public:
    ActionSystem();
    ~ActionSystem();

    // --- Entity Management ---

    // Create a new entity
    Entity* createEntity(const std::string& name = "");

    // Create entity from template
    Entity* spawnFromTemplate(const std::string& templateName, const glm::vec3& position);

    // Get entity by ID or name
    Entity* getEntity(uint32_t id);
    Entity* getEntityByName(const std::string& name);

    // Get all entities
    std::vector<std::unique_ptr<Entity>>& getEntities() { return m_entities; }
    const std::vector<std::unique_ptr<Entity>>& getEntities() const { return m_entities; }

    // Remove entity (deferred until end of frame)
    void destroyEntity(uint32_t id);
    void destroyEntity(Entity* entity);

    // --- Templates ---

    void registerTemplate(const EntityTemplate& tmpl);
    const EntityTemplate* getTemplate(const std::string& name) const;

    // --- Signals ---

    // Send a signal (processed at end of frame)
    void sendSignal(const Signal& signal);

    // Send signal helpers
    void sendSignalTo(const std::string& signalName, Entity* target, float value = 0.0f);
    void sendSignalToName(const std::string& signalName, const std::string& targetName, float value = 0.0f);
    void broadcastSignal(const std::string& signalName, const glm::vec3& position, float radius, float value = 0.0f);

    // --- Update ---

    // Call every frame
    void update(float deltaTime, const glm::vec3& playerPosition);

    // --- Triggers ---

    // Manually trigger a behavior on an entity
    void triggerBehavior(Entity* entity, const std::string& behaviorName);
    void triggerBehavior(Entity* entity, TriggerType trigger, const std::string& param = "");

    // Player interaction (call when player presses interact key near entities)
    void playerInteract(const glm::vec3& playerPosition, float interactRadius = 3.0f);

    // --- Custom Actions ---

    void registerCustomAction(const std::string& name, CustomActionCallback callback);

    // --- Serialization (basic) ---

    // Get data for saving
    struct SaveData {
        struct EntitySave {
            std::string name;
            std::string templateName;  // If spawned from template
            Transform transform;
            EntityFlags flags;
            std::vector<Behavior> behaviors;
            std::unordered_map<std::string, float> properties;
            std::vector<std::string> tags;
        };
        std::vector<EntitySave> entities;
    };

    SaveData getSaveData() const;
    void loadSaveData(const SaveData& data);
    void clear();  // Remove all entities

private:
    void processSignals();
    void cleanupDestroyedEntities();
    void executeAction(Entity& entity, const Action& action, BehaviorPlayer& player);
    float applyEasing(float t, Action::Easing easing);

    std::vector<std::unique_ptr<Entity>> m_entities;
    uint32_t m_nextEntityId = 1;

    std::unordered_map<std::string, EntityTemplate> m_templates;
    std::queue<Signal> m_signalQueue;
    std::unordered_map<std::string, CustomActionCallback> m_customActions;

    glm::vec3 m_lastPlayerPosition{0.0f};
};

} // namespace eden
