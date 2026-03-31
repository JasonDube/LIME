#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <variant>
#include <functional>

namespace eden {

// Action types - extend as needed
enum class ActionType {
    // Transform actions
    ROTATE,             // Rotate by delta over duration
    ROTATE_TO,          // Rotate to absolute rotation
    TURN_TO,            // Turn to face a position (yaw only)
    MOVE,               // Move by delta over duration
    MOVE_TO,            // Move to absolute position
    SCALE,              // Scale by factor over duration

    // Timing
    WAIT,               // Wait for duration

    // Signals
    SEND_SIGNAL,        // Send signal to another entity or broadcast

    // Spawning
    SPAWN_ENTITY,       // Spawn another entity at position
    DESTROY_SELF,       // Remove this entity

    // State
    SET_VISIBLE,        // Show/hide entity
    SET_PROPERTY,       // Set a named property value

    // Audio (placeholder for future)
    PLAY_SOUND,

    // AI/Movement
    FOLLOW_PATH,        // Follow a named path (stringParam = path name)

    // Grove construction commands (parsed from stringParam at execution time)
    GROVE_COMMAND,      // Execute a construction command (spawn, rotate, scale, delete)

    // Object interaction (queued in behaviors)
    PICKUP,             // Walk to named object (stringParam), pick it up (hide + carry)
    PLACE_VERTICAL,     // Walk to named target (stringParam), place carried item vertically into it
    PLACE_AT,           // Walk to vec3Param position, place carried item on terrain there
    PLACE_HORIZONTAL,   // Walk to midpoint of two named targets (stringParam = "nameA|nameB"), place carried item as horizontal beam
    PLACE_ROOF,         // Walk to center of 4 named corners (stringParam = "c1|c2|c3|c4"), place carried item as roof on top
    PLACE_WALL,         // Walk to midpoint of two named posts (stringParam = "postA|postB"), place carried item as wall panel

    // Custom
    CUSTOM              // For game-specific actions via callback
};

// When does a behavior start?
enum class TriggerType {
    ON_GAMESTART,       // When game/play mode starts
    ON_GAME_TIME,       // When game time reaches specified time (e.g., "0600" = 6:00 AM)
    ON_INTERACT,        // When player interacts (e.g., press E)
    ON_PROXIMITY,       // When player enters radius
    ON_SIGNAL,          // When receiving a named signal
    ON_COLLISION,       // When colliding with something
    ON_COMMAND,         // Only when explicitly commanded (script, LLM, or programmatic call)
    MANUAL              // Only triggered via code
};

// When does a behavior/action end and move to next?
enum class ExitCondition {
    NONE,               // Run until actions complete naturally
    ON_PATH_COMPLETE,   // When FOLLOW_PATH finishes
    ON_GAME_TIME,       // When game time reaches specified time (e.g., "0100" = 1:00 AM)
    ON_DURATION,        // After specified duration in seconds
    ON_SIGNAL,          // When receiving a named signal
    ON_PROXIMITY_EXIT   // When player leaves radius
};

// A single action with parameters
struct Action {
    ActionType type = ActionType::WAIT;

    // Parameters (interpretation depends on type)
    glm::vec3 vec3Param{0.0f};      // Position, rotation, scale delta/target
    float floatParam = 0.0f;        // Duration, radius, etc.
    std::string stringParam;         // Entity name, signal name, sound file
    std::string animationParam;      // Animation to play during action (for skinned models)
    bool boolParam = false;          // Visibility, etc.

    // Easing (for transform actions)
    enum class Easing { LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT } easing = Easing::LINEAR;

    // For action chaining - run next action only after this completes
    float duration = 0.0f;           // 0 = instant

    // Helper constructors
    static Action Rotate(glm::vec3 delta, float duration, Easing ease = Easing::LINEAR);
    static Action RotateTo(glm::vec3 target, float duration, Easing ease = Easing::LINEAR);
    static Action Move(glm::vec3 delta, float duration, Easing ease = Easing::LINEAR);
    static Action MoveTo(glm::vec3 target, float duration, Easing ease = Easing::LINEAR);
    static Action Wait(float duration);
    static Action SendSignal(const std::string& signalName, const std::string& targetEntity = "");
    static Action SpawnEntity(const std::string& templateName, glm::vec3 offset = glm::vec3(0));
    static Action DestroySelf();
    static Action SetVisible(bool visible);
    static Action FollowPath(const std::string& pathName);
};

// A behavior is a list of actions with a trigger
struct Behavior {
    std::string name;                // Optional name for debugging
    TriggerType trigger = TriggerType::MANUAL;
    std::string triggerParam;        // Signal name for ON_SIGNAL, etc.
    float triggerRadius = 5.0f;      // For ON_PROXIMITY

    std::vector<Action> actions;     // Actions to execute in sequence

    // Exit conditions - when to stop this behavior and move to next
    ExitCondition exitCondition = ExitCondition::NONE;
    std::string exitParam;           // Time string for ON_GAME_TIME, signal name, etc.
    float exitDuration = 0.0f;       // For ON_DURATION

    bool loop = false;               // Repeat when finished?
    bool enabled = true;             // Can be disabled
};

// Runtime state for playing through a behavior
struct BehaviorPlayer {
    const Behavior* behavior = nullptr;
    size_t currentActionIndex = 0;
    float actionTimer = 0.0f;
    bool isPlaying = false;
    bool finished = false;

    // Lerp state for transform actions
    glm::vec3 startValue{0.0f};
    glm::vec3 endValue{0.0f};

    void start(const Behavior* b);
    void stop();
    bool tick(float dt, class Entity& entity, class ActionSystem& system);
};

} // namespace eden
