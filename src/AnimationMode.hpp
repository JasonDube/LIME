#pragma once

#include "IEditorMode.hpp"
#include "EditorContext.hpp"

#include <eden/Animation.hpp>

#include <string>
#include <vector>

// Forward declaration for tinygltf
namespace tinygltf {
    class Model;
}

// Animation source determines axis correction
enum class AnimationSource {
    Mixamo = 0,
    Meshy = 1
};

// Stored animation data for combining
struct StoredAnimation {
    std::string name;
    std::string sourceFile;
    eden::AnimationClip clip;
    bool selected = false;
};

/**
 * @brief Animation Combiner mode for loading and combining skeletal animations
 *
 * Features:
 * - Load base skinned model
 * - Add animations from other GLB files
 * - Remap bone names between different Mixamo exports
 * - Preview animations with playback controls
 * - Export combined GLB with all animations
 */
class AnimationMode : public IEditorMode {
public:
    explicit AnimationMode(EditorContext& ctx);
    ~AnimationMode() override = default;

    void onActivate() override;
    void onDeactivate() override;
    void processInput(float deltaTime) override;
    void update(float deltaTime) override;
    void renderUI() override;
    void renderSceneOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) override;
    void drawOverlays(float vpX, float vpY, float vpW, float vpH) override;
    const char* getName() const override { return "Animation"; }

    // Animation mode specific methods
    void openSkinnedModelDialog();
    void loadSkinnedModel(const std::string& path);
    void addAnimationDialog();
    void addAnimationFromFile(const std::string& path);
    void exportCombinedGLB();

    // State accessors for main editor
    uint32_t getSkinnedModelHandle() const { return m_skinnedModelHandle; }
    const std::vector<StoredAnimation>& getAnimations() const { return m_animations; }
    int getCurrentAnimationIndex() const { return m_currentAnimationIndex; }
    float getAnimationTime() const { return m_animationTime; }
    bool isAnimationPlaying() const { return m_animationPlaying; }
    float getAnimationSpeed() const { return m_animationSpeed; }

private:
    void renderAnimationCombinerUI();
    void renderTimelineWindow();
    void exportToGLB(const std::string& path);
    glm::mat4 getModelMatrix() const;
    int createFloatAccessor(tinygltf::Model& model, const std::vector<float>& data, int type);
    std::string detectBonePrefix(const std::string& boneName);
    std::string remapBoneName(const std::string& srcName, const std::string& srcPrefix, const std::string& dstPrefix);
    bool animationNameExists(const std::string& name);

    // Animation state
    uint32_t m_skinnedModelHandle = UINT32_MAX;
    std::vector<std::string> m_boneNames;
    std::vector<StoredAnimation> m_animations;
    std::string m_baseModelPath;
    int m_currentAnimationIndex = -1;
    float m_animationTime = 0.0f;
    bool m_animationPlaying = false;
    bool m_scrubbing = false;
    float m_animationSpeed = 1.0f;
    char m_newAnimationName[64] = "";
    AnimationSource m_animationSource = AnimationSource::Mixamo;
    bool m_showJoints = true;
    bool m_showBoneNames = true;
    bool m_showBoneAxes = false;
    bool m_showWeightHeatMap = false;
    int m_selectedBone = -1;
    bool m_weightHeatMapActive = false; // tracks if colors are currently overridden
    int m_weightHeatMapBone = -1;       // which bone the heat map was last computed for

    // Timeline zoom/pan
    float m_timelineZoom = 1.0f;   // 1.0 = fit to window, >1 = zoomed in
    float m_timelinePanX = 0.0f;   // horizontal scroll offset in pixels

    // Gizmo
    GizmoMode m_gizmoMode = GizmoMode::None;
    GizmoAxis m_gizmoActiveAxis = GizmoAxis::None;
    bool m_gizmoDragging = false;
    glm::vec2 m_gizmoDragStartMouse{0};
    glm::vec3 m_gizmoDragStartBonePos{0};
    bool m_hasUnsavedPoseEdit = false;
};
