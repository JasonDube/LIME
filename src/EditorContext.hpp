#pragma once

#include "Renderer/VulkanApplicationBase.hpp"
#include "Renderer/ImGuiManager.hpp"
#include "Renderer/ModelRenderer.hpp"
#include "Renderer/SkinnedModelRenderer.hpp"
#include "Editor/SceneObject.hpp"
#include "EditableMesh.hpp"

#include <eden/Camera.hpp>
#include <eden/Input.hpp>
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <random>
#include <string>
#include <functional>

namespace eden {

// Forward declarations
class VulkanContext;
class Swapchain;
class Window;

} // namespace eden

using namespace eden;

// UV Island - a group of faces projected together with a unique color
struct UVIsland {
    uint32_t id;
    std::set<uint32_t> faceIndices;  // Triangle indices belonging to this island
    glm::vec3 color;                  // Random color for visualization
};

// Reference image for orthographic views
struct ReferenceImage {
    std::string name = "Reference";
    std::string filepath;
    bool loaded = false;
    bool visible = true;
    glm::vec2 offset{0, 0};         // Offset in view plane
    glm::vec2 size{5, 5};           // World units
    int imageWidth = 0;
    int imageHeight = 0;
    float opacity = 0.5f;

    // Pixel data for sampling (RGBA)
    std::vector<unsigned char> pixelData;

    // Vulkan resources
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// Clone source image (for Image Reference window)
struct CloneSourceImage {
    std::string name = "Image";
    std::string filepath;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixelData;  // RGBA

    // Vulkan resources for ImGui display
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// UV Editor sub-modes
enum class EditMode {
    Paint,
    FaceSelect
};

// Selection tool types
enum class SelectionTool {
    Normal,     // Click to select single element, drag for rectangle select
    Paint       // Paint/brush to select elements under cursor
};

// Gizmo types
enum class GizmoMode {
    None,
    Move,
    Rotate,
    Scale
};

// Gizmo axis
enum class GizmoAxis {
    None,
    X,
    Y,
    Z,
    Uniform  // Center cube for uniform scaling
};

/**
 * @brief Shared context for all editor modes
 *
 * Contains references to renderers, cameras, scene objects, and shared state
 * that all modes need access to.
 */
struct EditorContext {
    // Core systems (non-owning references)
    VulkanContext& vulkanContext;
    Swapchain& swapchain;
    Window& window;

    // Renderers (owned by main editor, references here)
    ModelRenderer& modelRenderer;
    SkinnedModelRenderer& skinnedModelRenderer;
    ImGuiManager& imguiManager;

    // Cameras
    Camera& camera;
    Camera& camera2;
    float& cameraSpeed;

    // Split view state
    bool& splitView;
    bool& activeViewportLeft;
    ViewPreset& splitOrthoPreset;

    // Scene objects
    std::vector<std::unique_ptr<SceneObject>>& sceneObjects;
    SceneObject*& selectedObject;              // Primary selection (for editable mesh)
    std::set<SceneObject*>& selectedObjects;   // Multi-selection set (for object mode operations)

    // UV Editor state
    EditMode& editMode;
    glm::vec3& paintColor;
    float& paintRadius;
    float& paintStrength;
    bool& isPainting;
    bool& squareBrush;  // Square brush with no falloff (pixel art style)
    bool& fillPaintToFace;  // Click a face to fill it with current color

    // Brush modes
    bool& useStamp;
    bool& useSmear;
    bool& useEyedropper;
    bool& useClone;
    int& cloneSourceViewIndex;      // Which reference image is the clone source
    glm::vec2& cloneSourcePixel;    // Pixel position in reference image (origin)
    glm::vec2& cloneCurrentSample;  // Current sampling position (tracks during painting)
    glm::vec2& cloneLastPaintUV;    // Last paint position on model (for offset calculation)
    bool& cloneSourceSet;           // Whether a clone source has been set
    bool& clonePaintingActive;      // Currently in a clone paint stroke
    glm::vec2& lastPaintUV;         // Last paint position for line tool (Shift+Click)
    bool& hasLastPaintPosition;     // Whether we have a valid last paint position
    float& smearStrength;
    float& smearPickup;  // How much new color to pick up (0-1)
    glm::vec3& smearCarriedColor;  // Color being smeared
    bool& isSmearing;  // Currently in a smear stroke
    std::vector<unsigned char>& stampData;
    int& stampWidth;
    int& stampHeight;
    float& stampScale;
    float& stampScaleH;
    float& stampScaleV;
    float& stampRotation;
    float& stampOpacity;  // Transparency (0-1)
    bool& stampFlipH;  // Flip horizontally
    bool& stampFlipV;  // Flip vertically
    bool& stampProjectFromView;  // Project stamp from camera view (ignores UV distortion)
    bool& stampFitToFace;  // Fit stamp to clicked face
    int& stampFitRotation;  // 0-3: rotate corners by 90° increments
    int& seamBusterPixels;  // Number of pixels to extend beyond UV edges (1-5)
    VkDescriptorSet& stampPreviewDescriptor;  // For ImGui display
    glm::vec3& uvWireframeColor;
    float& uvZoom;
    glm::vec2& uvPan;
    bool& uvPanning;
    glm::vec2& uvPanStart;
    bool& showWireframe;
    std::set<uint32_t>& selectedFaces;
    std::set<uint32_t>& hiddenFaces;
    glm::vec4& selectionColor;
    std::vector<UVIsland>& uvIslands;
    std::set<uint32_t>& selectedIslands;
    uint32_t& nextIslandId;
    std::mt19937& rng;

    // UV manipulation
    glm::vec2& uvIslandOffset;
    glm::vec2& uvIslandScale;
    bool& uvDragging;
    bool& uvResizing;
    int& uvResizeCorner;
    glm::vec2& uvDragStart;
    glm::vec2& uvIslandOriginalMin;
    glm::vec2& uvIslandOriginalMax;
    bool& uvHandleHovered;

    // Modeling Editor state
    EditableMesh& editableMesh;
    std::map<uint32_t, std::vector<uint32_t>>& faceToTriangles;
    ModelingSelectionMode& modelingSelectionMode;
    float& extrudeDistance;
    int& extrudeCount;
    float& insetAmount;
    float& hollowThickness;
    float& vertexDisplaySize;
    float& edgeDisplayWidth;
    glm::vec4& modelingSelectionColor;
    glm::vec4& modelingHoverColor;
    glm::vec4& modelingVertexColor;
    glm::vec4& modelingEdgeColor;
    bool& showModelingWireframe;
    bool& showFaceNormals;
    float& normalDisplayLength;
    float& uvProjectionScale;
    float& uvAngleThreshold;
    float& uvIslandMargin;
    int& cylinderAxisIndex;
    glm::vec3& cylinderAxisHint;
    bool& cylinderUsePCA;
    int& hoveredVertex;
    int& hoveredEdge;
    int& hoveredFace;
    double& lastClickTime;
    bool& meshDirty;

    // Selection tool state
    SelectionTool& selectionTool;
    bool& isRectSelecting;
    glm::vec2& rectSelectStart;
    glm::vec2& rectSelectEnd;
    float& paintSelectRadius;

    // Grid settings
    bool& showGrid;
    float& gridSize;
    float& gridSpacing;
    glm::vec4& gridColor;
    glm::vec4& gridAxisColor;

    // Custom colors
    glm::vec4& backgroundColor;      // Viewport background color
    glm::vec4& defaultMeshColor;     // Default color for new primitives
    glm::vec4& wireframeColor;       // Color for quad borders/wireframe
    bool& randomMeshColors;          // Randomize color for each new primitive

    // Reference images (one per ortho view)
    std::array<ReferenceImage, 6>& referenceImages;

    // Window visibility
    bool& showSceneWindow;
    bool& showToolsWindow;
    bool& showUVWindow;
    bool& showCameraWindow;
    bool& showImageRefWindow;  // Clone source images window

    // Image Reference Window state (for clone brush)
    float& imageRefZoom;
    glm::vec2& imageRefPan;
    bool& imageRefPanning;
    int& imageRefSelectedIndex;  // Currently selected image for cloning
    std::vector<CloneSourceImage>& cloneSourceImages;  // Loaded images for cloning

    // Object mode
    bool& objectMode;
    int& renamingObjectIndex;
    char* renameBuffer;
    size_t renameBufferSize;

    // Transform sliders
    glm::vec3& transformMove;
    glm::vec3& transformScale;
    glm::vec3& transformRotate;
    glm::vec3& lastScale;
    glm::vec3& lastRotate;
    bool& transformActive;

    // UV editor advanced state (Modeling mode UV window)
    bool& uvDraggingSelection;
    bool& uvScaling;
    bool& uvRotating;
    bool& uvChildHovered;
    glm::vec2& uvScaleCenter;
    glm::vec2& uvScaleStart;
    float& uvRotateStartAngle;
    std::set<uint32_t>& uvSelectedFaces;
    std::map<uint32_t, glm::vec2>& uvOriginalCoords;
    int& uvScaleHandle;  // -1=none, 0-3=corners (TL,TR,BR,BL), 4-7=sides (T,R,B,L)
    glm::vec2& uvScaleAnchor;  // Fixed point during handle scaling
    glm::vec2& uvScaleOriginalMin;  // Original bounds when scale started
    glm::vec2& uvScaleOriginalMax;
    bool& uvEdgeSelectionMode;
    std::pair<uint32_t, uint32_t>& uvSelectedEdge;
    std::vector<std::pair<uint32_t, uint32_t>>& uvTwinEdges;

    // UV vertex editing
    int& uvSelectionMode;  // 0=Face, 1=Edge, 2=Vertex
    std::set<uint32_t>& uvSelectedVertices;
    bool& uvDraggingVertex;

    // Camera state
    bool& isLooking;
    bool& isTumbling;
    bool& isPanning;
    glm::vec3& orbitTarget;
    float& orbitYaw;
    float& orbitPitch;
    bool& mouseLookMode;  // Tumble style: false=orbit, true=mouse-look

    // Gizmo state
    GizmoMode& gizmoMode;
    GizmoAxis& gizmoHoveredAxis;
    GizmoAxis& gizmoActiveAxis;
    bool& gizmoDragging;
    glm::vec3& gizmoDragStart;
    glm::vec3& gizmoDragStartPos;  // Original position when drag started
    glm::vec3& gizmoOriginalObjPos; // Original object position for snap mode
    float& gizmoSize;
    glm::vec3& gizmoOffset;  // Offset to move gizmo away from geometry
    bool& gizmoLocalSpace;   // Use local/face normal space instead of world space

    // Snap/increment settings
    bool& snapEnabled;
    float& moveSnapIncrement;   // Move snap in world units
    float& rotateSnapIncrement; // Rotation snap in degrees

    // Deferred deletion queue (processed at safe point in frame)
    std::vector<SceneObject*>& pendingDeletions;

    // Deferred texture deletion (processed at safe point — avoids vkDeviceWaitIdle during render pass)
    bool& pendingTextureDelete;

    // Mode can block camera pan (e.g. retopo Shift+MMB vertex drag)
    bool blockCameraPan = false;

    // Quick save state (for F5)
    std::string& currentFilePath;  // Full path to the currently loaded file
    int& currentFileFormat;        // 0=none, 1=OBJ, 2=LIME, 3=GLB

    // Reference image callbacks (set by main editor)
    std::function<bool(int, const std::string&)> loadReferenceImageCallback;
    std::function<void(int)> clearReferenceImageCallback;

    // Stamp preview callback (set by main editor)
    std::function<void(const unsigned char*, int, int)> updateStampPreviewCallback;

    // Clone source image callbacks (set by main editor)
    std::function<void(CloneSourceImage&)> createCloneImageTextureCallback;
    std::function<void(CloneSourceImage&)> destroyCloneImageTextureCallback;

    // AI model generation (Hunyuan3D)
    std::function<void(const std::string& prompt, const std::string& imagePath)> generateModelCallback;
    std::function<void()> cancelGenerationCallback;
    std::function<void(bool, bool)> toggleServerCallback;  // start/stop (lowVRAM, enableTex)
    bool& aiGenerating;
    std::string& aiGenerateStatus;
    bool& aiServerRunning;
    bool& aiServerReady;
    std::vector<std::string>& aiLogLines;  // Server log output
    bool showAIGenerateWindow = false;

    // Callback to update widget properties UI when a .lime is loaded
    std::function<void(const std::unordered_map<std::string, std::string>&)> onMetadataLoaded;

    // Helper methods

    /**
     * @brief Get ray from camera through mouse position
     */
    void getMouseRay(glm::vec3& rayOrigin, glm::vec3& rayDir) const;

    /**
     * @brief Get the active camera (handles split view)
     */
    Camera& getActiveCamera();

    /**
     * @brief Check if mouse is in left viewport (for split view)
     */
    bool isMouseInLeftViewport() const;

    /**
     * @brief Get reference image for a given view preset
     */
    ReferenceImage* getReferenceForView(ViewPreset preset);

    /**
     * @brief Get view preset name
     */
    static const char* getViewPresetName(int index);
};
