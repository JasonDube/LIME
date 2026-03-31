#include <eden/Action.hpp>
#include <eden/Entity.hpp>
#include <eden/ActionSystem.hpp>

namespace eden {

// --- Action Helper Constructors ---

Action Action::Rotate(glm::vec3 delta, float duration, Easing ease) {
    Action a;
    a.type = ActionType::ROTATE;
    a.vec3Param = delta;
    a.duration = duration;
    a.easing = ease;
    return a;
}

Action Action::RotateTo(glm::vec3 target, float duration, Easing ease) {
    Action a;
    a.type = ActionType::ROTATE_TO;
    a.vec3Param = target;
    a.duration = duration;
    a.easing = ease;
    return a;
}

Action Action::Move(glm::vec3 delta, float duration, Easing ease) {
    Action a;
    a.type = ActionType::MOVE;
    a.vec3Param = delta;
    a.duration = duration;
    a.easing = ease;
    return a;
}

Action Action::MoveTo(glm::vec3 target, float duration, Easing ease) {
    Action a;
    a.type = ActionType::MOVE_TO;
    a.vec3Param = target;
    a.duration = duration;
    a.easing = ease;
    return a;
}

Action Action::Wait(float duration) {
    Action a;
    a.type = ActionType::WAIT;
    a.duration = duration;
    return a;
}

Action Action::SendSignal(const std::string& signalName, const std::string& targetEntity) {
    Action a;
    a.type = ActionType::SEND_SIGNAL;
    a.stringParam = signalName + (targetEntity.empty() ? "" : ":" + targetEntity);
    a.duration = 0.0f;  // Instant
    return a;
}

Action Action::SpawnEntity(const std::string& templateName, glm::vec3 offset) {
    Action a;
    a.type = ActionType::SPAWN_ENTITY;
    a.stringParam = templateName;
    a.vec3Param = offset;
    a.duration = 0.0f;
    return a;
}

Action Action::DestroySelf() {
    Action a;
    a.type = ActionType::DESTROY_SELF;
    a.duration = 0.0f;
    return a;
}

Action Action::SetVisible(bool visible) {
    Action a;
    a.type = ActionType::SET_VISIBLE;
    a.boolParam = visible;
    a.duration = 0.0f;
    return a;
}

Action Action::FollowPath(const std::string& pathName) {
    Action a;
    a.type = ActionType::FOLLOW_PATH;
    a.stringParam = pathName;
    a.duration = 0.0f;  // Duration is determined by path traversal
    return a;
}

// --- BehaviorPlayer ---

void BehaviorPlayer::start(const Behavior* b) {
    behavior = b;
    currentActionIndex = 0;
    actionTimer = 0.0f;
    isPlaying = true;
    finished = false;
}

void BehaviorPlayer::stop() {
    isPlaying = false;
    finished = true;
}

bool BehaviorPlayer::tick(float dt, Entity& entity, ActionSystem& system) {
    if (!isPlaying || !behavior || behavior->actions.empty()) {
        return false;
    }

    actionTimer += dt;

    const Action& currentAction = behavior->actions[currentActionIndex];

    // Check if current action is complete
    bool actionComplete = false;
    if (currentAction.duration <= 0.0f) {
        // Instant action
        actionComplete = true;
    } else if (actionTimer >= currentAction.duration) {
        // Timed action complete
        actionComplete = true;
        actionTimer = currentAction.duration;  // Clamp for final lerp
    }

    // If action complete, move to next
    if (actionComplete) {
        currentActionIndex++;
        actionTimer = 0.0f;

        if (currentActionIndex >= behavior->actions.size()) {
            // Behavior complete
            if (behavior->loop) {
                currentActionIndex = 0;
            } else {
                isPlaying = false;
                finished = true;
                return false;
            }
        }
    }

    return true;  // Still playing
}

} // namespace eden
