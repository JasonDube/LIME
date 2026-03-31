#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// Forward declaration
class EditorContext;

/**
 * @brief Interface for editor modes (Modeling, UV Editor, Animation)
 *
 * Each mode manages its own UI, input handling, and rendering overlays.
 * The main editor delegates mode-specific behavior to the active mode.
 */
class IEditorMode {
public:
    virtual ~IEditorMode() = default;

    /**
     * @brief Called when this mode becomes active
     */
    virtual void onActivate() = 0;

    /**
     * @brief Called when this mode is deactivated
     */
    virtual void onDeactivate() = 0;

    /**
     * @brief Process input for this mode
     * @param deltaTime Time since last frame
     */
    virtual void processInput(float deltaTime) = 0;

    /**
     * @brief Update mode state
     * @param deltaTime Time since last frame
     */
    virtual void update(float deltaTime) = 0;

    /**
     * @brief Render ImGui UI for this mode
     */
    virtual void renderUI() = 0;

    /**
     * @brief Render 3D scene overlays using Vulkan
     * @param cmd Vulkan command buffer
     * @param viewProj View-projection matrix
     */
    virtual void renderSceneOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) = 0;

    /**
     * @brief Draw 2D overlays using ImGui draw lists
     * @param vpX Viewport X offset
     * @param vpY Viewport Y offset
     * @param vpW Viewport width
     * @param vpH Viewport height
     */
    virtual void drawOverlays(float vpX, float vpY, float vpW, float vpH) = 0;

    /**
     * @brief Get the name of this mode
     * @return Mode name for display
     */
    virtual const char* getName() const = 0;

    /**
     * @brief Check if this mode wants the grid to be shown
     * @return true if grid should be rendered
     */
    virtual bool wantsGrid() const { return false; }

    /**
     * @brief Check if this mode uses split view
     * @return true if split view should be available
     */
    virtual bool supportsSplitView() const { return false; }

protected:
    explicit IEditorMode(EditorContext& ctx) : m_ctx(ctx) {}
    EditorContext& m_ctx;
};
