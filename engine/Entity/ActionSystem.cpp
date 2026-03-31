#include <eden/ActionSystem.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace eden {

ActionSystem::ActionSystem() {}
ActionSystem::~ActionSystem() {}

// --- Entity Management ---

Entity* ActionSystem::createEntity(const std::string& name) {
    auto entity = std::make_unique<Entity>(m_nextEntityId++, name);
    Entity* ptr = entity.get();
    m_entities.push_back(std::move(entity));
    return ptr;
}

Entity* ActionSystem::spawnFromTemplate(const std::string& templateName, const glm::vec3& position) {
    auto it = m_templates.find(templateName);
    if (it == m_templates.end()) {
        return nullptr;
    }

    const EntityTemplate& tmpl = it->second;
    Entity* entity = createEntity(tmpl.name);

    // Copy default transform values from template
    entity->getTransform().setPosition(tmpl.defaultTransform.getPosition());
    entity->getTransform().setScale(tmpl.defaultTransform.getScale());
    // Override position with spawn position
    entity->getTransform().setPosition(position);
    entity->setFlags(tmpl.defaultFlags);

    for (const auto& behavior : tmpl.behaviors) {
        entity->addBehavior(behavior);
    }

    for (const auto& [key, value] : tmpl.defaultProperties) {
        entity->setProperty(key, value);
    }

    for (const auto& tag : tmpl.tags) {
        entity->addTag(tag);
    }

    // Trigger ON_START behaviors
    triggerBehavior(entity, TriggerType::ON_GAMESTART);

    return entity;
}

Entity* ActionSystem::getEntity(uint32_t id) {
    for (auto& entity : m_entities) {
        if (entity->getId() == id) {
            return entity.get();
        }
    }
    return nullptr;
}

Entity* ActionSystem::getEntityByName(const std::string& name) {
    for (auto& entity : m_entities) {
        if (entity->getName() == name) {
            return entity.get();
        }
    }
    return nullptr;
}

void ActionSystem::destroyEntity(uint32_t id) {
    if (Entity* e = getEntity(id)) {
        e->markForDestruction();
    }
}

void ActionSystem::destroyEntity(Entity* entity) {
    if (entity) {
        entity->markForDestruction();
    }
}

// --- Templates ---

void ActionSystem::registerTemplate(const EntityTemplate& tmpl) {
    m_templates[tmpl.name] = tmpl;
}

const EntityTemplate* ActionSystem::getTemplate(const std::string& name) const {
    auto it = m_templates.find(name);
    return (it != m_templates.end()) ? &it->second : nullptr;
}

// --- Signals ---

void ActionSystem::sendSignal(const Signal& signal) {
    m_signalQueue.push(signal);
}

void ActionSystem::sendSignalTo(const std::string& signalName, Entity* target, float value) {
    if (!target) return;
    Signal sig;
    sig.name = signalName;
    sig.targetId = target->getId();
    sig.value = value;
    m_signalQueue.push(sig);
}

void ActionSystem::sendSignalToName(const std::string& signalName, const std::string& targetName, float value) {
    Signal sig;
    sig.name = signalName;
    sig.targetName = targetName;
    sig.value = value;
    m_signalQueue.push(sig);
}

void ActionSystem::broadcastSignal(const std::string& signalName, const glm::vec3& position, float radius, float value) {
    // Find all entities within radius and send signal
    for (auto& entity : m_entities) {
        float dist = glm::length(entity->getTransform().getPosition() - position);
        if (dist <= radius) {
            Signal sig;
            sig.name = signalName;
            sig.targetId = entity->getId();
            sig.position = position;
            sig.value = value;
            m_signalQueue.push(sig);
        }
    }
}

// --- Update ---

void ActionSystem::update(float deltaTime, const glm::vec3& playerPosition) {
    m_lastPlayerPosition = playerPosition;

    // Process pending signals first
    processSignals();

    // Update all entities
    for (auto& entity : m_entities) {
        if (!entity->hasFlag(EntityFlags::ACTIVE)) continue;
        if (entity->isPendingDestruction()) continue;

        // Check proximity triggers
        float distToPlayer = glm::length(entity->getTransform().getPosition() - playerPosition);
        for (size_t i = 0; i < entity->getBehaviors().size(); ++i) {
            const Behavior& behavior = entity->getBehaviors()[i];
            BehaviorPlayer& player = entity->getBehaviorPlayers()[i];

            if (!behavior.enabled) continue;

            // Auto-trigger proximity behaviors
            if (behavior.trigger == TriggerType::ON_PROXIMITY && !player.isPlaying && !player.finished) {
                if (distToPlayer <= behavior.triggerRadius) {
                    player.start(&behavior);
                }
            }

            // Update playing behaviors
            if (player.isPlaying) {
                // Get current action
                const Action& action = behavior.actions[player.currentActionIndex];

                // Execute the action
                executeAction(*entity, action, player);

                // Tick the player
                player.tick(deltaTime, *entity, *this);
            }
        }
    }

    // Cleanup destroyed entities
    cleanupDestroyedEntities();
}

// --- Triggers ---

void ActionSystem::triggerBehavior(Entity* entity, const std::string& behaviorName) {
    if (!entity) return;

    auto& behaviors = entity->getBehaviors();
    auto& players = entity->getBehaviorPlayers();

    for (size_t i = 0; i < behaviors.size(); ++i) {
        if (behaviors[i].name == behaviorName && behaviors[i].enabled) {
            players[i].start(&behaviors[i]);
            return;
        }
    }
}

void ActionSystem::triggerBehavior(Entity* entity, TriggerType trigger, const std::string& param) {
    if (!entity) return;

    auto& behaviors = entity->getBehaviors();
    auto& players = entity->getBehaviorPlayers();

    for (size_t i = 0; i < behaviors.size(); ++i) {
        if (behaviors[i].trigger == trigger && behaviors[i].enabled) {
            // For ON_SIGNAL, check param matches
            if (trigger == TriggerType::ON_SIGNAL && behaviors[i].triggerParam != param) {
                continue;
            }
            players[i].start(&behaviors[i]);
        }
    }
}

void ActionSystem::playerInteract(const glm::vec3& playerPosition, float interactRadius) {
    for (auto& entity : m_entities) {
        if (!entity->hasFlag(EntityFlags::INTERACTABLE)) continue;
        if (entity->isPendingDestruction()) continue;

        float dist = glm::length(entity->getTransform().getPosition() - playerPosition);
        if (dist <= interactRadius) {
            triggerBehavior(entity.get(), TriggerType::ON_INTERACT);
        }
    }
}

// --- Custom Actions ---

void ActionSystem::registerCustomAction(const std::string& name, CustomActionCallback callback) {
    m_customActions[name] = callback;
}

// --- Serialization ---

ActionSystem::SaveData ActionSystem::getSaveData() const {
    SaveData data;
    for (const auto& entity : m_entities) {
        SaveData::EntitySave es;
        es.name = entity->getName();
        es.transform = entity->getTransform();
        es.flags = entity->getFlags();
        es.behaviors = entity->getBehaviors();
        // Properties would need iteration - simplified here
        es.tags = entity->getTags();
        data.entities.push_back(es);
    }
    return data;
}

void ActionSystem::loadSaveData(const SaveData& data) {
    clear();
    for (const auto& es : data.entities) {
        Entity* entity = createEntity(es.name);
        // Copy transform using setters
        entity->getTransform().setPosition(es.transform.getPosition());
        entity->getTransform().setScale(es.transform.getScale());
        // Note: rotation would need quaternion handling for proper copy
        entity->setFlags(es.flags);
        for (const auto& behavior : es.behaviors) {
            entity->addBehavior(behavior);
        }
        for (const auto& tag : es.tags) {
            entity->addTag(tag);
        }
        // Trigger ON_START behaviors
        triggerBehavior(entity, TriggerType::ON_GAMESTART);
    }
}

void ActionSystem::clear() {
    m_entities.clear();
    while (!m_signalQueue.empty()) m_signalQueue.pop();
}

// --- Private Methods ---

void ActionSystem::processSignals() {
    while (!m_signalQueue.empty()) {
        Signal sig = m_signalQueue.front();
        m_signalQueue.pop();

        // Find target entity/entities
        if (sig.targetId != 0) {
            if (Entity* target = getEntity(sig.targetId)) {
                triggerBehavior(target, TriggerType::ON_SIGNAL, sig.name);
            }
        } else if (!sig.targetName.empty()) {
            if (Entity* target = getEntityByName(sig.targetName)) {
                triggerBehavior(target, TriggerType::ON_SIGNAL, sig.name);
            }
        }
    }
}

void ActionSystem::cleanupDestroyedEntities() {
    m_entities.erase(
        std::remove_if(m_entities.begin(), m_entities.end(),
            [](const std::unique_ptr<Entity>& e) { return e->isPendingDestruction(); }),
        m_entities.end()
    );
}

void ActionSystem::executeAction(Entity& entity, const Action& action, BehaviorPlayer& player) {
    Transform& transform = entity.getTransform();

    // Calculate interpolation factor
    float t = (action.duration > 0.0f) ? (player.actionTimer / action.duration) : 1.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    t = applyEasing(t, action.easing);

    switch (action.type) {
        case ActionType::ROTATE: {
            // First frame: store start rotation (as euler angles for interpolation)
            if (player.actionTimer < 0.001f) {
                // Convert current quaternion to euler for start value
                player.startValue = glm::degrees(glm::eulerAngles(transform.getRotation()));
                player.endValue = player.startValue + action.vec3Param;
            }
            glm::vec3 currentEuler = glm::mix(player.startValue, player.endValue, t);
            transform.setRotation(currentEuler);
            break;
        }

        case ActionType::ROTATE_TO: {
            if (player.actionTimer < 0.001f) {
                player.startValue = glm::degrees(glm::eulerAngles(transform.getRotation()));
                player.endValue = action.vec3Param;
            }
            glm::vec3 currentEuler = glm::mix(player.startValue, player.endValue, t);
            transform.setRotation(currentEuler);
            break;
        }

        case ActionType::MOVE: {
            if (player.actionTimer < 0.001f) {
                player.startValue = transform.getPosition();
                player.endValue = transform.getPosition() + action.vec3Param;
            }
            transform.setPosition(glm::mix(player.startValue, player.endValue, t));
            break;
        }

        case ActionType::MOVE_TO: {
            if (player.actionTimer < 0.001f) {
                player.startValue = transform.getPosition();
                player.endValue = action.vec3Param;
            }
            transform.setPosition(glm::mix(player.startValue, player.endValue, t));
            break;
        }

        case ActionType::SCALE: {
            if (player.actionTimer < 0.001f) {
                player.startValue = transform.getScale();
                player.endValue = transform.getScale() * action.vec3Param;
            }
            transform.setScale(glm::mix(player.startValue, player.endValue, t));
            break;
        }

        case ActionType::WAIT:
            // Do nothing, just wait
            break;

        case ActionType::SEND_SIGNAL: {
            // Only execute once (on first frame)
            if (player.actionTimer < 0.001f) {
                // Parse "signalName" or "signalName:targetEntity"
                size_t colonPos = action.stringParam.find(':');
                if (colonPos != std::string::npos) {
                    std::string signalName = action.stringParam.substr(0, colonPos);
                    std::string targetName = action.stringParam.substr(colonPos + 1);
                    sendSignalToName(signalName, targetName, action.floatParam);
                } else {
                    // Broadcast or self-signal
                    Signal sig;
                    sig.name = action.stringParam;
                    sig.senderId = entity.getId();
                    sig.position = transform.getPosition();
                    sig.value = action.floatParam;
                    sendSignal(sig);
                }
            }
            break;
        }

        case ActionType::SPAWN_ENTITY: {
            if (player.actionTimer < 0.001f) {
                glm::vec3 spawnPos = transform.getPosition() + action.vec3Param;
                spawnFromTemplate(action.stringParam, spawnPos);
            }
            break;
        }

        case ActionType::DESTROY_SELF: {
            if (player.actionTimer < 0.001f) {
                entity.markForDestruction();
            }
            break;
        }

        case ActionType::SET_VISIBLE: {
            if (player.actionTimer < 0.001f) {
                if (action.boolParam) {
                    entity.addFlag(EntityFlags::VISIBLE);
                } else {
                    entity.removeFlag(EntityFlags::VISIBLE);
                }
            }
            break;
        }

        case ActionType::SET_PROPERTY: {
            if (player.actionTimer < 0.001f) {
                entity.setProperty(action.stringParam, action.floatParam);
            }
            break;
        }

        case ActionType::PLAY_SOUND:
            // Placeholder - integrate with audio system when available
            break;

        case ActionType::CUSTOM: {
            if (player.actionTimer < 0.001f) {
                auto it = m_customActions.find(action.stringParam);
                if (it != m_customActions.end()) {
                    it->second(entity, action, *this);
                }
            }
            break;
        }
    }
}

float ActionSystem::applyEasing(float t, Action::Easing easing) {
    switch (easing) {
        case Action::Easing::LINEAR:
            return t;
        case Action::Easing::EASE_IN:
            return t * t;
        case Action::Easing::EASE_OUT:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case Action::Easing::EASE_IN_OUT:
            return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
        default:
            return t;
    }
}

} // namespace eden
