#pragma once

#include "IEditorMode.hpp"
#include "EditorContext.hpp"

/**
 * @brief Modeling Editor mode for mesh editing
 *
 * Features:
 * - Vertex/Edge/Face selection modes
 * - Extrude, delete, merge operations
 * - Edge loops and rings
 * - UV editor integration
 * - Reference images for ortho views
 * - Grid and transform tools
 */
class ModelingMode : public IEditorMode {
public:
    explicit ModelingMode(EditorContext& ctx);
    ~ModelingMode() override = default;

    void onActivate() override;
    void onDeactivate() override;
    void processInput(float deltaTime) override;
    void update(float deltaTime) override;
    void renderUI() override;
    void renderSceneOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj) override;
    void drawOverlays(float vpX, float vpY, float vpW, float vpH) override;
    const char* getName() const override { return "Modeling Editor"; }
    bool wantsGrid() const override { return m_ctx.showGrid; }
    bool supportsSplitView() const override { return true; }

    // Mesh operations
    void buildEditableMeshFromObject();
    void updateMeshFromEditable();
    void saveEditableMeshAsGLB();
    void saveEditableMeshAsOBJ();
    void saveEditableMeshAsLime();
    void exportTextureAsPNG();
    void loadOBJFile();
    void loadLimeFile();
    void quickSave();  // F5 - save to current file path/format

    // Scene save/load (.limes format - all objects in scene)
    void saveLimeScene();
    void loadLimeScene();

    // Reference image operations
    void loadReferenceImage(int viewIndex);

public:
    static constexpr float kTimelineHeight = 110.0f;  // Shared with main.cpp dockspace reservation

private:
    void renderModelingEditorUI();
    void duplicateSelectedObject();  // Duplicate with random color and select
    void renderModelingUVWindow();
    void renderAnimationTimeline();              // Always-visible timeline strip pinned to bottom
    void clampWindowAboveTimeline(const char* name); // Call BEFORE ImGui::Begin(name, ...)
    void requestLayoutReset() { m_layoutResetPending = true; }
    bool m_layoutResetPending = false;  // Triggered manually by the timeline's "Reset Layout" button; otherwise ImGui restores from imgui.ini

    // Animation timeline state (persistent across modes; will hold per-skeleton clips later)
    float m_timelineDuration = 5.0f;
    float m_timelineCurrentTime = 0.0f;
    bool  m_timelinePlaying = false;
    float m_timelineZoom = 1.0f;
    float m_timelinePanX = 0.0f;
    float m_timelineLastAppliedTime = -1.0f;  // Last time we applied animated transforms

    // Per-object animation track. Keyframes share timestamps for pos/rot/scale
    // so "Set Key" records all three at once (simplest possible authoring).
    // For rigged models with a bind pose set, each keyframe also stores
    // bone editor-positions (world space, ~12 bytes × bone count). No vert
    // snapshots — playback re-skins from m_bindPoseVerts using bone deltas,
    // which keeps per-key memory tiny and matches GPU skinning math.
    struct ObjectAnimTrack {
        std::vector<float>     times;
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> rotations;
        std::vector<glm::vec3> scales;
        std::vector<std::vector<glm::vec3>> bonePositionsPerKey;  // empty for un-rigged objects
        std::vector<std::vector<glm::quat>> boneRotationsPerKey;  // matched 1:1 with bonePositionsPerKey
    };
    std::unordered_map<SceneObject*, ObjectAnimTrack> m_objectAnims;

    // Bind pose for the currently selected rigged object. Populated by the
    // "Set Bind Pose" button in the rigging panel. After this is set, bone
    // gizmo drags stop baking deformation into vertex positions — the mesh
    // is re-skinned from the bind pose every frame using current bone
    // deltas, which is the same math GPU skinning will use after export.
    bool m_hasBindPose = false;
    SceneObject* m_bindPoseOwner = nullptr;        // bind pose belongs to this object
    std::vector<glm::vec3> m_bindPoseVerts;        // rest-pose vertex positions
    std::vector<glm::vec3> m_bindPoseBonePositions; // rest-pose bone positions (world)
    std::vector<glm::quat> m_bindPoseBoneRotations; // rest-pose world rotations (typically identity)
    std::vector<glm::quat> m_boneWorldRotations;    // current world rotation per bone (updated by gizmo)

    // Bone-state undo stacks. Pushed in lock-step with EditableMesh's mesh
    // undo (via its setSaveStateHook hook) so Ctrl+Z restores both the
    // mesh AND the bone positions/rotations together. Without this, undo
    // would put verts back where they were but leave the bones moved —
    // the next reskin would un-do the undo.
    struct BoneSnapshot {
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> rotations;
    };
    std::vector<BoneSnapshot> m_boneUndoStack;
    std::vector<BoneSnapshot> m_boneRedoStack;
    void installBoneUndoHooks();

    void setBindPose();           // Snapshot current verts/bones as rest pose, recompute IBMs
    void clearBindPose();
    void reskinFromBoneDeltas();  // Recompute deformed verts from bind + current bone positions (LBS or DQS based on m_useDQS)
    void exportSkinnedAnimatedGLB(); // Save the rigged + animated selected object

    // Linear blend skinning (LBS) is the default; toggle to dual-quaternion
    // skinning (DQS) for volume-preserving bends — fixes "candy wrapper"
    // collapse at sharp joints. Faint bone-swelling at very extreme rotations
    // is the known DQS tradeoff.
    bool m_useDQS = false;

    // Weight heatmap: colors mesh verts by their weight on the selected bone
    // (blue=0 → green=0.5 → red=1). Pushes overridden vertex colors via the
    // in-place updateModelBuffer fast-path; toggling off restores the
    // editable mesh's stored colors.
    bool m_showWeightHeatMap = false;
    int  m_lastHeatMapBone = -1;
    bool m_heatMapDirty = false;  // forces a re-push next update tick
    void applyHeatMapToVerts(std::vector<ModelVertex>& verts);  // overrides .color in-place
    void pushMeshWithHeatMap();  // triangulate + (optional heatmap) + updateModelBuffer

    // Weight paint brush. Active when m_weightPaintMode is true and a bone
    // is selected. LMB adds, Ctrl+LMB subtracts. `[` / `]` resize radius.
    bool  m_weightPaintMode = false;
    float m_weightPaintRadius = 0.3f;     // model-space radius
    float m_weightPaintStrength = 0.5f;   // weight delta applied per stroke-tick
    bool  m_weightPaintingActive = false; // edge-detect to save undo state once per stroke
    float m_autoWeightMaxReach = 0.5f;    // mesh-units; bones further than this don't influence a vertex

    void setKeyOnSelected();             // Records selected object's current transform at m_timelineCurrentTime
    void deleteKeyOnSelectedNearTime();  // Removes the closest key within a small time threshold
    void applyAnimatedTransforms();      // Lerp/slerp all tracked objects to m_timelineCurrentTime
    void jumpToPrevKey();                // Move playhead to the previous key time on the selected object's track
    void jumpToNextKey();                // ...or the next one
    void copyKeyAtCurrentTime();         // Snapshot the closest key on the selected track into a clipboard
    void pasteKeyAtCurrentTime();        // Insert (or overwrite) a key at the playhead from the clipboard

    // Single-slot keyframe clipboard (one entry across all objects/tracks).
    struct KeyClipboard {
        bool        valid = false;
        glm::vec3   position{0.0f};
        glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3   scale{1.0f};
        std::vector<glm::vec3> bonePositions;  // empty for un-rigged objects
        std::vector<glm::quat> boneRotations;  // matched 1:1 with bonePositions
    };
    KeyClipboard m_keyClipboard;
    void renderImageRefWindow();  // Clone source images window
    void createPerspectiveCorrectedStamp(const CloneSourceImage& img);  // Perspective correction
    void processModelingInput(float deltaTime, bool gizmoActive = false);
    void renderModelingOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void renderGrid3D(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void renderWireframeOverlay3D(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void drawQuadWireframeOverlay(Camera& camera, float vpX, float vpY, float vpW, float vpH);
    void drawFaceNormalsOverlay(Camera& camera, float vpX, float vpY, float vpW, float vpH);
    void drawReferenceImages(Camera& camera, float vpX, float vpY, float vpW, float vpH);

    // Gizmo methods
    void renderGizmo(VkCommandBuffer cmd, const glm::mat4& viewProj);
    bool processGizmoInput();  // Returns true if gizmo consumed the mouse click
    glm::vec3 getGizmoPosition();  // Get position for gizmo (selection center or object origin)
    void getGizmoAxes(glm::vec3& xAxis, glm::vec3& yAxis, glm::vec3& zAxis);  // Get local/world axes
    GizmoAxis pickGizmoAxis(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& gizmoPos);
    bool isOrthoEdgeMoveMode();  // Ortho + edge selection + move mode → sphere gizmo
    void getOrthoAxes(glm::vec3& axis1, glm::vec3& axis2);  // Get 2 visible axes in ortho view
    float rayAxisDistance(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                          const glm::vec3& axisOrigin, const glm::vec3& axisDir);
    glm::vec3 projectPointOntoAxis(const glm::vec3& point, const glm::vec3& axisOrigin, const glm::vec3& axisDir);

    // Camera helpers
    void startCameraTumble();

    // UV helpers
    bool pointInUVTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c);
    int findUVFaceAtPoint(const glm::vec2& uvPoint);
    int findUVVertexAtPoint(const glm::vec2& uvPoint, float threshold);
    void selectUVIsland(uint32_t startFace);
    std::set<uint32_t> getUVSelectedVertices();
    void getUVSelectionBounds(glm::vec2& outMin, glm::vec2& outMax);
    void storeOriginalUVs();
    void storeOriginalUVsForVertices();
    void moveSelectedUVs(const glm::vec2& delta);
    void moveSelectedUVVertices(const glm::vec2& delta);
    void scaleSelectedUVs(const glm::vec2& center, float scale);
    void scaleSelectedUVsFromAnchor(const glm::vec2& anchor, float scaleX, float scaleY);
    void rotateSelectedUVs(const glm::vec2& center, float angleDegrees);

    // Edge path extrusion
    void extrudeBoxAlongSelectedEdges(float boxSize, float taper = 1.0f, bool autoUV = true);
    std::vector<uint32_t> orderSelectedEdgesIntoPath();

    // Pipe network extrusion (handles junctions and corners)
    void extrudePipeNetwork(float boxSize, float blockSizeMultiplier = 1.0f, bool autoUV = true);

    // UV sewing helpers
    float pointToLineSegmentDistUV(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b);
    std::pair<uint32_t, uint32_t> findUVEdgeAtPoint(const glm::vec2& uvPoint, float threshold = 0.02f);
    std::pair<glm::vec3, glm::vec3> getEdge3DPositions(uint32_t faceIdx, uint32_t localEdgeIdx);
    std::pair<glm::vec2, glm::vec2> getEdgeUVs(uint32_t faceIdx, uint32_t localEdgeIdx);
    bool positions3DEqual(const glm::vec3& a, const glm::vec3& b, float tol = 0.0001f);
    void findTwinUVEdges(uint32_t selectedFaceIdx, uint32_t selectedEdgeIdx);
    void clearUVEdgeSelection();
    std::set<uint32_t> getUVIslandFaces(uint32_t startFace);
    std::set<uint32_t> getIslandVertices(const std::set<uint32_t>& faces);
    void sewSelectedEdge();
    void moveAndSewSelectedEdge();
    void unsewSelectedEdge();

    // UV baking - draws UV edges onto texture
    void bakeUVEdgesToTexture(const glm::vec3& edgeColor, int lineThickness = 1);

    // Bake texture colors into vertex colors (then remove texture)
    void bakeTextureToVertexColors();

    // UV re-projection: store old UVs + texture, then re-project after UV edit
    void storeUVsForReprojection();
    void reprojectTexture(int outputSize = 0);  // 0 = use original texture size
    void packAndReprojectUVs();  // One-click: store, shrink, reproject

    // Stored UV/texture state for re-projection
    struct StoredVertexUV {
        glm::vec2 uv;
    };
    std::vector<StoredVertexUV> m_storedOldUVs;
    std::vector<unsigned char> m_storedOldTexture;
    int m_storedOldTexW = 0;
    int m_storedOldTexH = 0;
    bool m_hasStoredUVs = false;
    int m_reprojectTexSize = 1024;  // Output texture size for re-projection
    float m_packScale = 0.5f;       // How much to shrink UVs (0.25 = 25%, 0.5 = 50%)
    int m_packCorner = 0;           // 0=bottom-left, 1=bottom-right, 2=top-left, 3=top-right

    // Vertex paint state
    bool m_vertexPaintMode = false;
    glm::vec3 m_vertexPaintColor = glm::vec3(1.0f, 0.0f, 0.0f);
    float m_vertexPaintRadius = 0.2f;
    float m_vertexPaintStrength = 1.0f;
    bool m_vertexPaintingActive = false;  // Currently in a paint stroke
    bool m_vertexPaintSquare = true;      // Square brush (pixel art style) vs circular

    // Image reference window state
    int m_pendingCloneImageDelete = -1;  // Index to delete, -1 means none

    // Rectangle stamp selection state
    bool m_stampSelecting = false;       // Currently dragging a selection
    glm::vec2 m_stampSelectStart{0.0f};  // Start pixel in image
    glm::vec2 m_stampSelectEnd{0.0f};    // End pixel in image
    int m_stampSelectImageIdx = -1;      // Which image we're selecting from
    bool m_pendingStampPreviewUpdate = false;  // Deferred stamp preview texture update

    // Perspective correction state
    bool m_perspectiveMode = false;      // Placing corners for perspective correction
    glm::vec2 m_perspectiveCorners[4];   // 4 corner positions in image pixel coordinates
    int m_perspectiveCornerCount = 0;    // How many corners placed (0-4)
    int m_perspectiveImageIdx = -1;      // Which image we're placing corners on

    // Face snap state
    bool m_snapMode = false;             // Snap tool is active
    bool m_snapMergeMode = false;        // If true, merge objects after snap
    SceneObject* m_snapSourceObject = nullptr;  // First object selected
    int m_snapSourceFace = -1;           // Face index on source object
    glm::vec3 m_snapSourceCenter{0.0f};  // Center of source face (world space)
    glm::vec3 m_snapSourceNormal{0.0f};  // Normal of source face (world space)

    // Snap & Merge vertex selection mode (ordered vertex correspondence)
    bool m_snapVertexMode = false;       // Vertex selection mode active
    SceneObject* m_snapSrcObj = nullptr; // Source object for merge
    SceneObject* m_snapDstObj = nullptr; // Target object for merge
    std::vector<glm::vec3> m_snapSrcVerts;   // Source vertices (world positions, in order)
    std::vector<glm::vec3> m_snapDstVerts;   // Target vertices (world positions, in order)
    std::vector<uint32_t> m_snapSrcVertIndices;  // Source vertex indices (for rendering)
    std::vector<uint32_t> m_snapDstVertIndices;  // Target vertex indices (for rendering)

    // Custom gizmo pivot (for post-snap rotation)
    bool m_useCustomGizmoPivot = false;
    glm::vec3 m_customGizmoPivot{0.0f};

    // Control point visibility
    bool m_showControlPoints = true;

    // Origin visualization
    bool m_showOrigin = true;

    // Port authoring state
    bool m_showPorts = true;
    int m_selectedPortIndex = -1;        // Which port is selected (-1 = none)
    int m_draggingPortAxis = -1;         // -1=none, 0=forward, 1=up, 2=position
    glm::vec3 m_portDragStart{0.0f};
    char m_portNameBuf[64] = "pipe_end";

    // Port methods (implemented in ModelingMode_Ports.cpp)
    void renderPortUI();
    void renderPortOverlay(float vpX, float vpY, float vpW, float vpH);
    void renderPortGizmos3D(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void syncPortsToSceneObject();

    // Metadata editor (implemented in ModelingMode_Ports.cpp)
    void renderMetadataUI();
    char m_metaKeyBuf[64] = "";
    char m_metaValueBuf[128] = "";

    // Mode switch notification
    float m_modeNotificationTimer = 0.0f;
    float m_saveNotificationTimer = 0.0f;

    // UV rectangle selection
    bool m_uvRectSelecting = false;
    glm::vec2 m_uvRectStart{0.0f};
    glm::vec2 m_uvRectEnd{0.0f};

    // Wireframe/vertex overlay cache (avoid per-frame rebuild for high-poly meshes)
    bool m_wireframeDirty = true;
    glm::mat4 m_cachedModelMatrix{1.0f};
    std::vector<glm::vec3> m_cachedWireLines;
    std::vector<glm::vec3> m_cachedSelectedLines;
    std::vector<glm::vec3> m_cachedNormalVerts;
    std::vector<glm::vec3> m_cachedSelectedVerts;
    std::vector<glm::vec3> m_cachedHoveredVerts;
    int m_cachedHoveredVertex = -1;
    size_t m_cachedSelectedEdgeCount = 0;
    size_t m_cachedSelectedVertCount = 0;
    ModelingSelectionMode m_cachedSelectionMode = ModelingSelectionMode::Face;
    void invalidateWireframeCache() {
        m_wireframeDirty = true;
        m_cachedWireLines.clear();
        m_cachedSelectedLines.clear();
        m_cachedNormalVerts.clear();
        m_cachedSelectedVerts.clear();
        m_cachedHoveredVerts.clear();
    }

    // Snap helper methods
    void cancelSnapMode();
    void cancelSnapVertexMode();
    glm::vec3 getFaceCenter(SceneObject* obj, int faceIdx);
    glm::vec3 getFaceNormal(SceneObject* obj, int faceIdx);
    void snapObjectToFace(SceneObject* srcObj, int srcFace, SceneObject* dstObj, int dstFace);
    void snapAndMergeObjects(SceneObject* srcObj, int srcFace, SceneObject* dstObj, int dstFace);
    void snapAndMergeWithVertexCorrespondence();  // Uses m_snapSrcVerts/m_snapDstVerts
    void drawSnapVertexOverlay(float vpX, float vpY, float vpW, float vpH);

    // Repeat last operation (X key)
    enum class LastOp { None, ExtrudeEdge, ExtrudeFace };
    LastOp m_lastOp = LastOp::None;

    // Retopology state
    bool m_retopologyMode = false;           // Place Vertex tool active
    SceneObject* m_retopologyLiveObj = nullptr;  // "Live" reference surface
    std::vector<glm::vec3> m_retopologyVerts;    // Placed vertices (world positions)
    std::vector<glm::vec3> m_retopologyNormals;  // Surface normals at placed vertices
    std::vector<uint32_t> m_retopologyVertMeshIdx; // Editable mesh index (UINT32_MAX = new vert)
    bool m_retopologyObjCreated = false;     // Whether we've created the retopo scene object
    bool m_retopologyDragging = false;       // G-key grab mode active
    int m_retopologyDragQuadIdx = -1;        // Which quad overlay entry to update
    int m_retopologyDragQuadVert = -1;       // Which corner of that quad (0-3)
    glm::vec3 m_retopologyDragOrigPos{0.0f}; // Original position for cancel

    // Accumulated retopo quads (for overlay drawing before finalize)
    struct RetopologyQuad {
        glm::vec3 verts[4];  // World positions
    };
    std::vector<RetopologyQuad> m_retopologyQuads;

    // Retopology methods
    void drawRetopologyOverlay(float vpX, float vpY, float vpW, float vpH);
    void cancelRetopologyMode();
    void createRetopologyQuad();     // Creates quad from 4 placed vertices
    void finalizeRetopologyMesh();   // Build GPU mesh from accumulated quads

    // Auto-retopology (voxel remesh)
    void autoRetopology();           // Auto-retopo from live surface
    int m_autoRetopResolution = 32;
    int m_autoRetopSmoothIter = 5;

    // Scatter points on surface (evenly spaced retopo vertices)
    void scatterPointsOnSurface();
    void scatterSpherical();       // Project UV sphere onto surface from all directions
    void connectScatterRings();    // Horizontal: connect points within each ring
    void connectScatterVertical(); // Vertical: connect matching indices between adjacent rings + quads
    void alignScatterAnchors();    // Re-slide rings so anchors land on axis crossings
    float m_scatterSpacing = 5.0f;  // cm
    struct ScatterRing {
        size_t startIdx;  // index into m_retopologyVerts
        size_t count;     // number of points in this ring
        // Stored contour path for sliding
        std::vector<glm::vec3> contourPts;
        std::vector<glm::vec3> contourNrms;
        std::vector<float> cumDist;  // cumulative distance along contour
        float totalLen = 0.0f;
        float slideOffset = 0.0f;  // current slide offset in distance units
        float sliceY = 0.0f;       // original slice height for re-slicing
    };
    std::vector<ScatterRing> m_scatterRings;
    struct ScatterEdge { size_t a, b; };
    std::vector<ScatterEdge> m_scatterEdges;
    size_t m_bottomPoleIdx = SIZE_MAX;
    size_t m_topPoleIdx = SIZE_MAX;
    bool m_scatterSymmetryX = false;  // Mirror point moves across X=0
    int m_dragMirrorIdx = -1;         // Mirror point index found at grab start
    std::unordered_map<size_t, size_t> m_mirrorPairs;  // Point index → its mirror partner index
    std::set<size_t> m_selectedScatterPoints;  // Up to 4 selected for subdivide
    void subdivideSelectedQuad();              // Split 4 selected points into 4 sub-quads
    int m_selectedRing = -1;
    bool m_ringDragging = false;
    float m_ringDragStartX = 0.0f;
    float m_ringDragStartY = 0.0f;
    float m_ringDragStartOffset = 0.0f;  // slideOffset when drag started
    float m_ringDragStartSliceY = 0.0f;  // sliceY when drag started
    void resampleRing(size_t ringIdx);    // Re-place points along stored contour at current slideOffset
    void resliceRing(size_t ringIdx, float newY);  // Re-slice at new Y, rebuild contour, resample

    // Quad blanket retopology (view-based grid projection)
    void quadBlanketRetopology();
    int m_quadBlanketResX = 32;
    int m_quadBlanketResY = 32;
    int m_quadBlanketSmoothIter = 3;
    bool m_quadBlanketTrimPartial = true;
    float m_quadBlanketPadding = 0.05f;

    // Patch Blanket (targeted rectangle → retopo quads accumulator)
    bool m_patchBlanketMode = false;
    bool m_patchBlanketDragging = false;
    glm::vec2 m_patchBlanketStart{0.0f};
    glm::vec2 m_patchBlanketEnd{0.0f};
    void executePatchBlanket();

    // Path Tube state
    bool m_pathTubeMode = false;
    std::vector<glm::vec3> m_pathNodes;
    int m_pathSelectedNode = -1;
    bool m_pathDragging = false;
    int m_pathDragNodeIdx = -1;
    glm::vec3 m_pathDragOrigPos{0.0f};

    // Path tube surface attachment (first node snapped to live mesh)
    bool m_pathTubeAttached = false;          // First node is attached to live surface
    glm::vec3 m_pathTubeAttachNormal{0,1,0};  // Surface normal at attachment point

    // Path Tube parameters
    float m_pathTubeRadius = 0.05f;
    float m_pathTubeRadiusStart = 1.0f;  // Taper multiplier at start
    float m_pathTubeRadiusEnd = 1.0f;    // Taper multiplier at end
    int m_pathTubeSegments = 8;
    int m_pathTubeSamplesPerSpan = 8;

    // Profile editor state
    std::vector<glm::vec2> m_pathTubeProfile;  // Custom cross-section shape (unit-scale)
    int m_profileDragIdx = -1;                 // Currently dragged profile vertex (-1 = none)
    float m_pathTubeProfileExtent = 1.0f;      // How much of tube uses custom profile (0=none, 1=all)
    void resetPathTubeProfile();               // Reset to circle from m_pathTubeSegments
    void drawProfileEditor();                  // ImGui widget for profile editing

    // Path Tube methods
    void processPathTubeInput(bool mouseOverImGui);
    void drawPathTubeOverlay(float vpX, float vpY, float vpW, float vpH);
    void renderPathTubePreview3D(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void generatePathTubeMesh();
    void cancelPathTubeMode();

    // Slice tool state
    bool m_sliceMode = false;
    glm::vec3 m_slicePlaneCenter{0.0f};
    glm::vec3 m_slicePlaneNormal{0, 1, 0};
    float m_slicePlaneOffset = 0.0f;
    float m_slicePlaneRotationX = 0.0f;   // Pitch (degrees)
    float m_slicePlaneRotationY = 0.0f;   // Yaw (degrees)
    int m_slicePresetAxis = 1;            // 0=X, 1=Y, 2=Z
    bool m_sliceCapHoles = false;         // Fill holes after slicing

    bool m_showVertexNumbers = false;     // Display vertex indices in viewport
    bool m_showFaceNumbers = false;       // Display face indices in viewport
    bool m_showFacePreview = true;        // Show face extrude preview on mouse hover

    // G-grab free move in component mode
    bool m_componentGrabbing = false;
    glm::vec3 m_grabPlaneOrigin;
    glm::vec3 m_grabPlaneNormal;
    glm::vec3 m_grabStartHit;
    std::vector<std::pair<uint32_t, glm::vec3>> m_grabOrigPositions;

    // Lasso selection
    bool m_isLassoSelecting = false;
    bool m_selectionBackfaceCull = true;
    std::vector<glm::vec2> m_lassoPoints;

    // Edge node dragging (green arrow edge vertex manipulation)
    int m_edgeNodeDrag = -1;              // -1=none, 0=vertA, 1=vertB, 2=both (arrow)
    glm::vec3 m_edgeNodeDragStart;        // Mouse world pos at drag start

    // Quad direction edge for face extrude
    int m_faceDirectionEdge = 0;          // Which edge of the selected face is the extrude direction
    // Face extrude preview (mouse-guided)
    bool m_faceExtrudePreviewValid = false;
    glm::vec3 m_faceExtrudePreview[4];    // Preview quad corners (world space)
    uint32_t m_previewStitchA = UINT32_MAX;
    uint32_t m_previewStitchB = UINT32_MAX;
    int m_prevPreviewEdge = -1;           // Which edge was shown last frame
    int m_previewOptionIndex = 0;         // Which option to show for this edge
    struct PreviewOption { uint32_t stitchA, stitchB; glm::vec3 pC, pD; };
    std::vector<PreviewOption> m_previewOptions;
    void quadMidAirExtrude();             // Create new quad off the direction edge (retopo path)

    // Extract: pull faces under retopo grid off the original mesh
    enum class ExtractMode { IncludePartial = 0, ExcludePartial = 1 };
    ExtractMode m_extractMode = ExtractMode::IncludePartial;
    void performExtract();

    // Slice methods
    void cancelSliceMode();
    void performSlice();
    void drawSlicePlaneOverlay3D(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void updateSlicePlaneFromParams();
    static glm::vec3 pathCatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                     const glm::vec3& p2, const glm::vec3& p3, float t);
    static glm::vec3 pathEvaluateSpline(const std::vector<glm::vec3>& points, float t);
    static std::vector<glm::vec3> pathSampleSpline(const std::vector<glm::vec3>& points,
                                                    int samplesPerSegment);

    // Rigging state
    bool m_riggingMode = false;
    int m_selectedBone = -1;
    bool m_showSkeleton = true;
    bool m_showBoneNames = true;
    bool m_placingBone = false;      // Click mesh surface to place bone head
    float m_boneInsetDepth = 0.2f;   // How far to push bone inward along surface normal
    std::vector<glm::vec3> m_bonePositions;  // Editor-side head positions per bone
    char m_newBoneName[64] = "Bone";
    glm::vec3 m_skeletonOffset{0.0f};        // Cumulative offset applied via "Move Skeleton" — drag changes are applied as deltas to all bones

    // Pre-rigging state — restored by cancelRiggingMode so exiting rigging
    // doesn't leave the user stuck in component+vertex mode (which triggers
    // expensive per-frame hover picking on dense meshes)
    bool m_preRiggingObjectMode = true;
    bool m_preRiggingShowWireframe = false;
    ModelingSelectionMode m_preRiggingSelectionMode = ModelingSelectionMode::Vertex;

    // Direct bone-marker drag in the rigging skeleton overlay. Pressing on a
    // bone joint dot picks the bone AND starts a camera-plane drag — release
    // ends it. Gizmo arrow drags stay world-axis-locked, so picking the
    // interaction target chooses the constraint (no checkbox).
    int       m_riggingBoneDragIdx = -1;
    glm::vec3 m_riggingBoneDragPrevPoint{0.0f};
    glm::vec3 m_riggingBoneDragPlaneNormal{0.0f, 0.0f, 1.0f};

    // Vertices weighted to the bones affected by the current bone-gizmo drag.
    // Built once at drag-start so the per-frame deform loop doesn't iterate
    // every vertex on dense meshes (a freshly-added bone has zero weights, so
    // this list is empty and the deform/upload work is skipped entirely).
    // Each entry pairs the vertex index with its summed weight against the
    // affected bone set, so the inner-loop weight test is also avoided.
    std::vector<std::pair<uint32_t, float>> m_riggingDragWeightedVerts;

    void drawSkeletonOverlay(float vpX, float vpY, float vpW, float vpH);
    void cancelRiggingMode();
    int pickBoneAtScreenPos(const glm::vec2& screenPos, float threshold = 20.0f);
    std::vector<int> getDescendantBones(int boneIdx);  // Get all children recursively

    // Patch Move state (UV editor: move UV island + texture pixels together)
    bool m_patchMoveMode = false;
    bool m_patchSelected = false;
    bool m_patchDragging = false;
    bool m_patchScaling = false;
    int m_patchScaleHandle = -1;
    glm::vec2 m_patchRectMin{0.0f};
    glm::vec2 m_patchRectMax{1.0f};
    glm::vec2 m_patchOrigRectMin{0.0f};
    glm::vec2 m_patchOrigRectMax{1.0f};
    glm::vec2 m_patchDragStart{0.0f};
    std::vector<unsigned char> m_patchTextureBackup;
    std::vector<unsigned char> m_patchPixels;
    int m_patchPixelW = 0, m_patchPixelH = 0;
    std::set<uint32_t> m_patchVertices;
    std::map<uint32_t, glm::vec2> m_patchOrigUVs;

    // Patch Move methods
    void cancelPatchMoveMode();
    void confirmPatchMove();
    void extractPatchPixels();
    void applyPatchTransform();

    // Connect two objects by matching control points (merges boundary rings)
    void connectByControlPoints();
    void undoConnectCPs();  // Restore pre-connect state

    // Undo state for Connect CPs operation
    struct ConnectCPsBackup {
        struct ObjectBackup {
            std::string name;
            eden::Transform transform;
            glm::vec3 eulerRotation;
            std::vector<SceneObject::StoredHEVertex> heVerts;
            std::vector<SceneObject::StoredHalfEdge> heHalfEdges;
            std::vector<SceneObject::StoredHEFace> heFaces;
            std::vector<SceneObject::StoredControlPoint> controlPoints;
            std::vector<ModelVertex> meshVerts;
            std::vector<uint32_t> meshIndices;
            std::vector<unsigned char> textureData;
            int texWidth = 0, texHeight = 0;
        };
        std::vector<ObjectBackup> originals;
        std::string combinedName;  // Name of the created object (to find & remove it)
        bool valid = false;
    };
    ConnectCPsBackup m_connectCPsBackup;

    // Auto UV island packing
    void autoPackUVIslands(bool fitToUV = false);

public:
    // AI Generate (Hunyuan3D) UI state — public so main.cpp can read params
    char m_generatePrompt[512] = "";
    std::string m_generateImagePath;         // Single mode image
    bool m_generateMultiView = false;        // Multi-view mode
    std::string m_generateFrontPath;         // Multi-view: front (required)
    std::string m_generateLeftPath;          // Multi-view: left (optional, defaults to front)
    std::string m_generateRightPath;         // Multi-view: right (optional, defaults to front)
    std::string m_generateBackPath;          // Multi-view: back (optional, defaults to front)
    int m_generateSteps = 5;
    int m_generateOctreeRes = 256;
    float m_generateGuidance = 5.0f;
    int m_generateMaxFaces = 10000;
    bool m_generateTexture = true;
    int m_generateTexSize = 1024;        // Texture resolution (512, 1024, 2048)
    bool m_generateRemBG = true;         // Remove background from input image
    int m_generateSeed = 12345;
    bool m_generateSettingsOpen = false;
    bool m_generateLowVRAM = false;      // Mini model + CPU offload for texture
};
