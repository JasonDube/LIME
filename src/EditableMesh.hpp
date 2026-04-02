#pragma once

#include "Renderer/ModelRenderer.hpp"
#include "Renderer/SkinnedModelRenderer.hpp"
#include "eden/Animation.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <set>
#include <array>
#include <cstdint>

namespace eden {

// Half-edge data structure for efficient topology queries
struct HalfEdge {
    uint32_t vertexIndex;      // Vertex this half-edge points TO
    uint32_t faceIndex;        // Face this half-edge belongs to (UINT32_MAX if boundary)
    uint32_t nextIndex;        // Next half-edge in face loop (CCW)
    uint32_t prevIndex;        // Previous half-edge in face loop
    uint32_t twinIndex;        // Opposite half-edge (UINT32_MAX if boundary)
};

// Half-edge vertex
struct HEVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 color;
    uint32_t halfEdgeIndex;    // One outgoing half-edge
    bool selected = false;
    glm::ivec4 boneIndices = glm::ivec4(0);   // Up to 4 bone indices
    glm::vec4 boneWeights = glm::vec4(0.0f);  // Corresponding weights (sum to 1.0)
};

// Half-edge face (supports quads and n-gons)
struct HEFace {
    uint32_t halfEdgeIndex;    // One half-edge on this face
    uint32_t vertexCount;      // 3 for tri, 4 for quad, n for n-gon
    bool selected = false;
};

// Edge selection info (for highlighting)
struct EdgeSelection {
    uint32_t halfEdgeIndex;
    bool selected = false;
};

// Selection mode for modeling
enum class ModelingSelectionMode {
    Vertex,
    Edge,
    Face
};

// Raycasting result
struct MeshRayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 position;
    glm::vec3 normal;
    uint32_t faceIndex = UINT32_MAX;
    uint32_t vertexIndex = UINT32_MAX;
    uint32_t edgeIndex = UINT32_MAX;
};

// Snapshot of mesh state for undo/redo
struct MeshState {
    std::vector<HEVertex> vertices;
    std::vector<HalfEdge> halfEdges;
    std::vector<HEFace> faces;
    std::unordered_map<uint64_t, uint32_t> edgeMap;
    std::set<uint32_t> selectedEdges;
};

class EditableMesh {
public:
    EditableMesh() = default;
    ~EditableMesh() = default;

    // Construction from triangle mesh
    void buildFromTriangles(const std::vector<ModelVertex>& vertices,
                           const std::vector<uint32_t>& indices);

    // Construction from quad mesh (4 indices per face)
    void buildFromQuads(const std::vector<ModelVertex>& vertices,
                       const std::vector<uint32_t>& indices);

    // Create a unit cube with proper quad faces
    void buildCube(float size = 1.0f);

    // Create a rectangular box with independent dimensions
    void buildBox(float width, float height, float depth);

    // Create a cylinder with proper quad faces
    // capRings: number of concentric rings in each cap (2+ creates quad grid caps for edge loops)
    void buildCylinder(float radius = 0.5f, float height = 2.0f, int segments = 16, int divisions = 1, bool caps = true, int capRings = 2);

    // Create a UV sphere with quad/triangle faces
    void buildSphere(float radius = 0.5f, int rings = 8, int segments = 16);

    // Create a ring of cube segments (cylinder made of 6-sided boxes)
    // Each segment is a deformed cube forming a wedge shape
    void buildCubeRing(int segments = 8, float innerRadius = 0.3f, float outerRadius = 0.5f, float height = 1.0f);

    // Create an arch of cube segments (half-ring or partial ring)
    // arcDegrees: how much of a circle (180 = half circle/arch, 90 = quarter, etc.)
    void buildCubeArch(int segments = 8, float innerRadius = 0.3f, float outerRadius = 0.5f, float depth = 0.5f, float arcDegrees = 180.0f);

    // Create a solid column (like cube ring but no hole in middle)
    void buildCubeColumn(int segments = 8, float radius = 0.5f, float height = 1.0f);

    // Create stairs from cubes
    void buildCubeStairs(int steps = 5, float width = 1.0f, float stepHeight = 0.2f, float stepDepth = 0.3f);

    // Create a blocky human head from cubes
    void buildCubeHead(float scale = 1.0f);

    // Create a hollow room made of cube walls with optional window openings
    // windowFront: number of cubes to leave out of front wall center for viewing
    void buildCubeRoom(int width = 10, int height = 4, int depth = 12, float cubeSize = 0.5f, int windowFront = 3);

    // Build a hollow sphere made of cubes
    // radius: sphere radius
    // cubeSize: size of each cube
    // rings: number of latitude rings
    // segments: number of longitude segments per ring
    // interior: if true, faces point inward (for inside viewing)
    // Create a hollow sphere made of cubes (spaced or solid shell)
    // solidShell: if true, cubes are sized to touch each other forming a continuous shell
    // interior: if true, normals point inward for viewing from inside
    void buildCubeSphere(float radius = 2.0f, float cubeSize = 0.3f, int rings = 8, int segments = 16, bool interior = false, bool solidShell = false);

    // Create a hollow sphere by extruding each face of a UV sphere outward
    // Creates a blocky/faceted shell with proper topology
    void buildExtrudedSphere(float radius = 1.0f, float thickness = 0.2f, int rings = 8, int segments = 16, bool interior = false);

    // Create a solid rectangular block made of cubes
    void buildCubeBlock(int width = 3, int height = 2, int depth = 3, float cubeSize = 0.5f);

    // Create a flat wall/plate of cubes (single layer, width x height)
    void buildBlockPlate(int width = 5, int height = 3, float cubeSize = 0.5f, bool beveled = false, float bevelAmount = 0.1f);

    // Auto-UV for cube-based meshes - each cube becomes an island packed into UV space
    void autoUVCubes();

    // Try to merge coplanar triangle pairs into quads
    void mergeTrianglesToQuads(float normalThreshold = 0.85f);  // 0.85 allows ~32° difference for faceted geometry

    // Export for GPU rendering (triangulates quads/n-gons)
    void triangulate(std::vector<ModelVertex>& outVerts,
                    std::vector<uint32_t>& outIndices) const;
    void triangulate(std::vector<ModelVertex>& outVerts,
                    std::vector<uint32_t>& outIndices,
                    const std::set<uint32_t>& hiddenFaces) const;

    // Export skinned mesh for GPU rendering (includes bone indices/weights)
    void triangulateSkinned(std::vector<SkinnedVertex>& outVerts,
                           std::vector<uint32_t>& outIndices) const;
    void triangulateSkinned(std::vector<SkinnedVertex>& outVerts,
                           std::vector<uint32_t>& outIndices,
                           const std::set<uint32_t>& hiddenFaces) const;

    // Save/Load .lime format (with optional embedded texture and transform)
    bool saveLime(const std::string& filepath) const;
    bool saveLime(const std::string& filepath, const unsigned char* textureData, int texWidth, int texHeight) const;
    bool saveLime(const std::string& filepath, const unsigned char* textureData, int texWidth, int texHeight,
                  const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale) const;
    bool loadLime(const std::string& filepath);
    bool loadLime(const std::string& filepath, std::vector<unsigned char>& outTextureData, int& outTexWidth, int& outTexHeight);
    bool loadLime(const std::string& filepath, std::vector<unsigned char>& outTextureData, int& outTexWidth, int& outTexHeight,
                  glm::vec3& outPosition, glm::quat& outRotation, glm::vec3& outScale);

    // Save/Load .obj format (preserves quads natively)
    bool saveOBJ(const std::string& filepath) const;
    bool loadOBJ(const std::string& filepath);

    // Check if mesh is valid
    bool isValid() const { return !m_vertices.empty() && !m_faces.empty(); }

    // Clear all mesh data
    void clear();

    // Set mesh data directly (for restoring from stored data)
    void setMeshData(const std::vector<HEVertex>& verts,
                     const std::vector<HalfEdge>& halfEdges,
                     const std::vector<HEFace>& faces);

    // Add a vertex and return its index
    uint32_t addVertex(const HEVertex& vertex);

    // Set position of an existing vertex
    void setVertexPosition(uint32_t idx, const glm::vec3& pos) {
        if (idx < m_vertices.size()) m_vertices[idx].position = pos;
    }

    // Add a quad face from 4 vertex indices (CCW winding)
    uint32_t addQuadFace(const std::vector<uint32_t>& vertIndices);

    // Batch-add quad faces (skips per-face rebuildEdgeMap, call rebuildEdgeMap after)
    void addQuadFacesBatch(const std::vector<std::array<uint32_t, 4>>& faces);

    // Basic queries
    size_t getVertexCount() const { return m_vertices.size(); }
    size_t getFaceCount() const { return m_faces.size(); }
    size_t getHalfEdgeCount() const { return m_halfEdges.size(); }

    // Get vertex/face data
    const HEVertex& getVertex(uint32_t idx) const { return m_vertices[idx]; }
    HEVertex& getVertex(uint32_t idx) { return m_vertices[idx]; }
    const HEFace& getFace(uint32_t idx) const { return m_faces[idx]; }
    HEFace& getFace(uint32_t idx) { return m_faces[idx]; }
    const HalfEdge& getHalfEdge(uint32_t idx) const { return m_halfEdges[idx]; }

    // Topology queries
    std::vector<uint32_t> getVertexFaces(uint32_t vertIdx) const;
    std::vector<uint32_t> getVertexEdges(uint32_t vertIdx) const;
    std::vector<uint32_t> getVertexNeighbors(uint32_t vertIdx) const;
    std::vector<uint32_t> getFaceVertices(uint32_t faceIdx) const;
    std::vector<uint32_t> getFaceEdges(uint32_t faceIdx) const;
    std::vector<uint32_t> getFaceNeighbors(uint32_t faceIdx) const;

    // Get the two vertices of an edge (from half-edge index)
    std::pair<uint32_t, uint32_t> getEdgeVertices(uint32_t heIdx) const;

    // Edge loop detection - walks perpendicular to edge through quads
    std::vector<uint32_t> getEdgeLoop(uint32_t heIdx) const;

    // Edge ring detection - walks along edge through quads
    std::vector<uint32_t> getEdgeRing(uint32_t heIdx) const;

    // Selection operations
    void selectVertex(uint32_t idx, bool additive = false);
    void selectEdge(uint32_t heIdx, bool additive = false);
    void selectFace(uint32_t idx, bool additive = false);
    void selectEdgeLoop(uint32_t heIdx);
    void selectEdgeRing(uint32_t heIdx);
    void selectFacesByNormal(const glm::vec3& viewDir, float maxAngleDeg, const std::set<uint32_t>& skipFaces = {});
    void clearSelection();
    void invertSelection(ModelingSelectionMode mode);

    // Toggle selection (for Ctrl+click)
    void toggleVertexSelection(uint32_t idx);
    void toggleEdgeSelection(uint32_t heIdx);
    void toggleFaceSelection(uint32_t idx);

    // Get selected elements
    std::vector<uint32_t> getSelectedVertices() const;
    std::vector<uint32_t> getSelectedEdges() const;
    std::vector<uint32_t> getSelectedFaces() const;
    bool hasSelection() const;

    // Mesh operations
    void extrudeFaces(const std::vector<uint32_t>& faceIndices, float distance);
    void extrudeSelectedFaces(float distance);
    void extrudeEdges(const std::vector<uint32_t>& halfEdgeIndices, float distance);
    void extrudeSelectedEdges(float distance);
    void insetSelectedFaces(float amount);  // Create inner quad with border quads (amount 0-1)
    void deleteFaces(const std::vector<uint32_t>& faceIndices);
    void deleteSelectedFaces();
    void removeOrphanedVertices();  // Compact vertex array, removing vertices not used by any face
    void mergeVertices(const std::vector<uint32_t>& vertIndices);
    void mergeSelectedVertices();
    void mirrorMergeX(float weldThreshold = 0.001f);  // Mirror mesh across X=0, merge seam vertices

    // Named control points for modular part connections
    struct ControlPoint {
        uint32_t vertexIndex;
        std::string name;
    };
    void addControlPoint(uint32_t vertexIndex, const std::string& name);
    void removeControlPoint(uint32_t vertexIndex);
    void clearControlPoints();
    const std::vector<ControlPoint>& getControlPoints() const { return m_controlPoints; }
    bool isControlPoint(uint32_t vertexIndex) const;
    std::string getControlPointName(uint32_t vertexIndex) const;

    // Connection ports for pipe/assembly snap system (vertex-independent)
    struct Port {
        std::string name;       // e.g., "pipe_end_1"
        glm::vec3 position;     // Local-space position
        glm::vec3 forward;      // Outward direction (pipe opening faces this way)
        glm::vec3 up;           // Up direction (locks roll orientation)
    };
    void addPort(const Port& port);
    void removePort(size_t index);
    void clearPorts();
    const std::vector<Port>& getPorts() const { return m_ports; }
    size_t getPortCount() const { return m_ports.size(); }

    // Metadata (key-value pairs written to LIME files)
    void setMetadata(const std::string& key, const std::string& value) { m_metadata[key] = value; }
    void removeMetadata(const std::string& key) { m_metadata.erase(key); }
    void clearMetadata() { m_metadata.clear(); }
    const std::unordered_map<std::string, std::string>& getMetadata() const { return m_metadata; }
    std::string getMetadataValue(const std::string& key) const {
        auto it = m_metadata.find(key);
        return it != m_metadata.end() ? it->second : "";
    }

    // Insert edge loop - cuts perpendicular edges through quads
    // count: number of edge loops to insert (evenly distributed)
    void insertEdgeLoop(uint32_t heIdx, int count = 1);

    // Hollow - create inner shell offset inward by thickness
    void hollow(float thickness);

    // Boolean cut - cut a rectangular hole through the mesh using an axis-aligned box
    // Creates frame faces around the cut to show wall thickness
    // cutterMin/cutterMax define the axis-aligned bounding box of the cutter
    void booleanCut(const glm::vec3& cutterMin, const glm::vec3& cutterMax);

    // Bridge two edges - create faces connecting them
    // segments: number of subdivisions (1 = single quad, 2+ = multiple quads)
    bool bridgeEdges(uint32_t heIdx1, uint32_t heIdx2, int segments = 1);

    // Flip normals of selected faces (reverses winding order)
    // Flips all selected to match the minority normal direction
    void flipSelectedNormals();

    // Make all face normals consistent via flood-fill.
    // Starts from face 0, propagates winding through shared edges.
    // Press once = all consistent. Press again = all flipped.
    void makeNormalsConsistent();

    // Catmull-Clark subdivision - smooths mesh by splitting each face into sub-quads
    // levels: number of subdivision iterations (1-3 recommended)
    void catmullClarkSubdivide(int levels = 1);

    // Raycasting for selection
    MeshRayHit raycastVertex(const glm::vec3& origin, const glm::vec3& dir,
                             float threshold = 0.1f) const;
    MeshRayHit raycastEdge(const glm::vec3& origin, const glm::vec3& dir,
                           float threshold = 0.05f) const;
    MeshRayHit raycastFace(const glm::vec3& origin, const glm::vec3& dir) const;
    MeshRayHit raycastFace(const glm::vec3& origin, const glm::vec3& dir,
                          const std::set<uint32_t>& skipFaces) const;

    // General raycast based on current mode
    MeshRayHit raycast(const glm::vec3& origin, const glm::vec3& dir,
                       ModelingSelectionMode mode, float threshold = 0.1f) const;
    MeshRayHit raycast(const glm::vec3& origin, const glm::vec3& dir,
                       ModelingSelectionMode mode, float threshold,
                       const std::set<uint32_t>& skipFaces) const;

    // Skeleton / rigging support
    Skeleton& getSkeleton() { return m_skeleton; }
    const Skeleton& getSkeleton() const { return m_skeleton; }
    void setSkeleton(const Skeleton& skel) { m_skeleton = skel; }
    bool hasSkeleton() const { return !m_skeleton.bones.empty(); }
    void clearSkeleton() { m_skeleton.bones.clear(); m_skeleton.boneNameToIndex.clear(); }

    // Auto-weight generation: assigns 4 closest bones per vertex using inverse distance
    void generateAutoWeights(const std::vector<glm::vec3>& boneHeadPositions);

    // Clear all bone weights from vertices
    void clearBoneWeights();

    // Recalculate normals from geometry
    void recalculateNormals();

    // Set all vertex colors at once
    void setAllVertexColors(const glm::vec4& color);

    // UV unwrapping
    void boxProjectUVs(float scale = 1.0f);  // Project UVs based on face normals
    void smartProjectUVs(float angleThreshold = 66.0f, float islandMargin = 0.02f);  // Auto-cut and pack UVs
    void planarProjectByNormal(float normalTolerance = 0.001f, float islandMargin = 0.02f);  // Group by normal, planar project each
    void projectSelectedFacesFromView(const glm::vec3& viewDir, const glm::vec3& viewUp, float scale = 1.0f);  // Project selected faces from camera view
    void cylindricalProjectUVs(const glm::vec3& axisHint = glm::vec3(0, 1, 0), bool usePCA = true);  // Cylindrical UV projection with automatic seam placement
    void perFaceProjectUVs(float margin = 0.02f);  // Each face becomes its own UV island, packed in grid
    void uniformSquareUVs(float margin = 0.02f);  // Every face gets identical square UV - stamps look same everywhere

    // Experimental: Sew all shared UV edges, skipping those that would cause overlap
    // If targetFaces is empty, operates on all faces. Otherwise only on specified faces.
    // Returns number of edges successfully sewn
    int sewAllUVs(const std::vector<uint32_t>& targetFaces = {});

    // Transform selected vertices
    void translateSelectedVertices(const glm::vec3& delta);
    void scaleSelectedVertices(const glm::vec3& scale, const glm::vec3& pivot);
    void rotateSelectedVertices(const glm::vec3& eulerDegrees, const glm::vec3& pivot);
    glm::vec3 getSelectionCenter() const;

    // Coplanar operations (require 3+ vertices)
    void makeCoplanar();          // Best-fit plane using PCA
    void flattenX();              // Flatten to average X position
    void flattenY();              // Flatten to average Y position
    void flattenZ();              // Flatten to average Z position

    // Get face normal
    glm::vec3 getFaceNormal(uint32_t faceIdx) const;

    // Get face center
    glm::vec3 getFaceCenter(uint32_t faceIdx) const;

    // Debug: validate half-edge structure
    bool validateTopology() const;

    // Undo/Redo support
    void saveState();           // Call before any modifying operation
    bool undo();                // Returns true if undo was performed
    bool redo();                // Returns true if redo was performed
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }
    void clearUndoHistory();
    size_t getUndoStackSize() const { return m_undoStack.size(); }
    size_t getRedoStackSize() const { return m_redoStack.size(); }

    // Get internal data (for serialization/cloning)
    const std::vector<HEVertex>& getVerticesData() const { return m_vertices; }
    const std::vector<HalfEdge>& getHalfEdges() const { return m_halfEdges; }
    const std::vector<HEFace>& getFacesData() const { return m_faces; }

    // Set from internal data (restore from serialization/clone)
    void setFromData(const std::vector<HEVertex>& vertices,
                     const std::vector<HalfEdge>& halfEdges,
                     const std::vector<HEFace>& faces);

private:
    // Get all vertices affected by current selection (vertices, edges, or faces)
    std::set<uint32_t> getAffectedVertices() const;

    // Helper to find or create half-edge between two vertices
    uint32_t findHalfEdge(uint32_t fromVert, uint32_t toVert) const;

public:
    // Add a face to the mesh (handles half-edge creation)
    uint32_t addFace(const std::vector<uint32_t>& vertIndices);

private:

    // Remove a face (updates topology)
    void removeFace(uint32_t faceIdx);

public:
    // Rebuild edge map from half-edges
    void rebuildEdgeMap();

    // Link twin half-edges by vertex POSITION (not index)
    // This handles meshes with duplicate vertices at same position (e.g. for different normals)
    void linkTwinsByPosition();

private:

    // Rebuild mesh from faces, removing deleted faces (vertexCount == 0)
    void rebuildFromFaces();

    // Find the next edge in an edge loop through a quad
    uint32_t findNextLoopEdge(uint32_t heIdx) const;

    // Check if face is a quad
    bool isQuad(uint32_t faceIdx) const;

    // Hash function for edge key (pair of vertex indices)
    static uint64_t makeEdgeKey(uint32_t v0, uint32_t v1) {
        uint32_t minV = std::min(v0, v1);
        uint32_t maxV = std::max(v0, v1);
        return (static_cast<uint64_t>(minV) << 32) | maxV;
    }

    // Data
    std::vector<HEVertex> m_vertices;
    std::vector<HalfEdge> m_halfEdges;
    std::vector<HEFace> m_faces;

    // Skeleton data for rigging
    Skeleton m_skeleton;

    // Edge lookup: (minVert, maxVert) -> one of the half-edges for that edge
    std::unordered_map<uint64_t, uint32_t> m_edgeMap;

    // Track which edges are selected (by half-edge index)
    std::set<uint32_t> m_selectedEdges;

    // Named control points for modular part connections
    std::vector<ControlPoint> m_controlPoints;

    // Connection ports (vertex-independent snap points)
    std::vector<Port> m_ports;

    // Key-value metadata (widget_type, machine_name, etc.)
    std::unordered_map<std::string, std::string> m_metadata;

    // Undo/Redo stacks
    std::vector<MeshState> m_undoStack;
    std::vector<MeshState> m_redoStack;
    static constexpr size_t MAX_UNDO_LEVELS = 50;
};

} // namespace eden
