// ModelingMode_Primitives.cpp - Mesh creation/extrusion functions for ModelingMode
// Split from ModelingMode.cpp for better organization

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <iostream>
#include <map>
#include <set>
#include <random>

using namespace eden;

// Vertex classification for pipe network (local to this file)
enum class PipeVertexType {
    Endpoint,    // 1 edge (path terminus)
    Passthrough, // 2 edges, angle < 45° (smooth continuation)
    Corner,      // 2 edges, angle >= 45° (needs junction block)
    Junction     // 3+ edges (T-junction, cross, etc.)
};

std::vector<uint32_t> ModelingMode::orderSelectedEdgesIntoPath() {
    // Get selected edges as half-edge indices
    auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
    if (selectedEdges.empty()) return {};

    std::cout << "[EdgePath] Ordering " << selectedEdges.size() << " selected edges into path" << std::endl;

    // Build adjacency: for each vertex, which edges touch it
    std::map<uint32_t, std::vector<uint32_t>> vertexToEdges;
    std::set<uint32_t> edgeSet(selectedEdges.begin(), selectedEdges.end());

    for (uint32_t heIdx : selectedEdges) {
        auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(heIdx);
        vertexToEdges[v0].push_back(heIdx);
        vertexToEdges[v1].push_back(heIdx);
    }

    // Find endpoints (vertices with only 1 edge) - these are path start/end
    std::vector<uint32_t> endpoints;
    for (auto& [vertIdx, edges] : vertexToEdges) {
        if (edges.size() == 1) {
            endpoints.push_back(vertIdx);
        }
    }

    if (endpoints.size() != 2) {
        std::cout << "[EdgePath] Warning: Expected 2 endpoints, found " << endpoints.size()
                  << ". Path may be a loop or disconnected." << std::endl;
        if (endpoints.empty() && !vertexToEdges.empty()) {
            // It's a closed loop - pick any vertex as start
            endpoints.push_back(vertexToEdges.begin()->first);
        }
    }

    if (endpoints.empty()) return {};

    // Walk from first endpoint, collecting edges in order
    std::vector<uint32_t> orderedEdges;
    std::set<uint32_t> visitedEdges;
    uint32_t currentVertex = endpoints[0];

    while (orderedEdges.size() < selectedEdges.size()) {
        auto& edges = vertexToEdges[currentVertex];
        uint32_t nextEdge = UINT32_MAX;

        for (uint32_t heIdx : edges) {
            if (visitedEdges.count(heIdx) == 0 && edgeSet.count(heIdx) > 0) {
                nextEdge = heIdx;
                break;
            }
        }

        if (nextEdge == UINT32_MAX) break;  // No more edges to follow

        orderedEdges.push_back(nextEdge);
        visitedEdges.insert(nextEdge);

        // Move to the other vertex of this edge
        auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(nextEdge);
        currentVertex = (currentVertex == v0) ? v1 : v0;
    }

    std::cout << "[EdgePath] Ordered path has " << orderedEdges.size() << " edges" << std::endl;
    return orderedEdges;
}

void ModelingMode::extrudeBoxAlongSelectedEdges(float boxSize, float taper, bool autoUV) {
    // Order selected edges into a path
    std::vector<uint32_t> pathEdges = orderSelectedEdgesIntoPath();
    if (pathEdges.size() < 1) {
        std::cout << "[EdgePath] Need at least 1 edge selected" << std::endl;
        return;
    }

    std::cout << "[EdgePath] Creating box tube along " << pathEdges.size() << " edges, box size: " << boxSize << ", taper: " << taper << ", autoUV: " << autoUV << std::endl;

    // Collect vertices along the path in order
    std::vector<glm::vec3> pathVertices;

    // Add first vertex
    auto [firstV0, firstV1] = m_ctx.editableMesh.getEdgeVertices(pathEdges[0]);
    glm::vec3 firstPos = m_ctx.editableMesh.getVertex(firstV0).position;

    // Determine correct starting vertex by checking connectivity
    if (pathEdges.size() > 1) {
        auto [secondV0, secondV1] = m_ctx.editableMesh.getEdgeVertices(pathEdges[1]);
        // First edge's v1 should connect to second edge
        if (firstV1 == secondV0 || firstV1 == secondV1) {
            pathVertices.push_back(m_ctx.editableMesh.getVertex(firstV0).position);
        } else {
            pathVertices.push_back(m_ctx.editableMesh.getVertex(firstV1).position);
        }
    } else {
        pathVertices.push_back(firstPos);
    }

    // Add remaining vertices
    uint32_t prevVertex = UINT32_MAX;
    for (size_t i = 0; i < pathEdges.size(); ++i) {
        auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(pathEdges[i]);
        glm::vec3 pos0 = m_ctx.editableMesh.getVertex(v0).position;
        glm::vec3 pos1 = m_ctx.editableMesh.getVertex(v1).position;

        // Add the vertex we haven't added yet
        float dist0 = glm::length(pos0 - pathVertices.back());
        float dist1 = glm::length(pos1 - pathVertices.back());

        if (dist0 < 0.0001f) {
            pathVertices.push_back(pos1);
        } else if (dist1 < 0.0001f) {
            pathVertices.push_back(pos0);
        } else {
            // First edge - add both vertices
            if (pathVertices.size() == 1) {
                pathVertices.push_back(pos1);
            }
        }
    }

    std::cout << "[EdgePath] Path has " << pathVertices.size() << " vertices" << std::endl;

    if (pathVertices.size() < 2) {
        std::cout << "[EdgePath] Need at least 2 vertices in path" << std::endl;
        return;
    }

    // Create the box tube mesh
    // For each vertex on the path, create a square cross-section (4 vertices)
    // Connect adjacent cross-sections with quads

    std::vector<ModelVertex> tubeVerts;
    std::vector<uint32_t> tubeIndices;

    float baseHalfSize = boxSize * 0.5f;

    // Get mesh color (random if enabled)
    glm::vec4 meshColor = m_ctx.defaultMeshColor;
    if (m_ctx.randomMeshColors) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        meshColor = glm::vec4(dist(rng), dist(rng), dist(rng), 1.0f);
    }

    // Pre-compute tangents for all vertices
    std::vector<glm::vec3> tangents(pathVertices.size());
    for (size_t i = 0; i < pathVertices.size(); ++i) {
        if (i == 0) {
            tangents[i] = glm::normalize(pathVertices[1] - pathVertices[0]);
        } else if (i == pathVertices.size() - 1) {
            tangents[i] = glm::normalize(pathVertices[i] - pathVertices[i - 1]);
        } else {
            glm::vec3 incoming = glm::normalize(pathVertices[i] - pathVertices[i - 1]);
            glm::vec3 outgoing = glm::normalize(pathVertices[i + 1] - pathVertices[i]);
            tangents[i] = glm::normalize(incoming + outgoing);
        }
    }

    // Compute frames using Double Reflection Method (rotation minimizing frames)
    // This avoids the twist accumulation problem of naive parallel transport
    std::vector<glm::vec3> rights(pathVertices.size());
    std::vector<glm::vec3> ups(pathVertices.size());

    // Initial frame
    {
        glm::vec3 tangent = tangents[0];
        glm::vec3 up = glm::vec3(0, 1, 0);
        if (std::abs(glm::dot(tangent, up)) > 0.9f) {
            up = glm::vec3(1, 0, 0);
        }
        rights[0] = glm::normalize(glm::cross(tangent, up));
        ups[0] = glm::normalize(glm::cross(rights[0], tangent));
    }

    // Propagate using Double Reflection Method
    for (size_t i = 1; i < pathVertices.size(); ++i) {
        glm::vec3 t0 = tangents[i - 1];
        glm::vec3 t1 = tangents[i];

        // Reflection 1: reflect previous frame across plane perpendicular to (t0 + t1)/2
        glm::vec3 v1 = pathVertices[i] - pathVertices[i - 1];
        float c1 = glm::dot(v1, v1);
        if (c1 < 0.0001f) {
            // Degenerate case: same position
            rights[i] = rights[i - 1];
            ups[i] = ups[i - 1];
            continue;
        }

        glm::vec3 rL = rights[i - 1] - (2.0f / c1) * glm::dot(v1, rights[i - 1]) * v1;
        glm::vec3 tL = t0 - (2.0f / c1) * glm::dot(v1, t0) * v1;

        // Reflection 2: reflect across plane perpendicular to (tL + t1)/2
        glm::vec3 v2 = t1 - tL;
        float c2 = glm::dot(v2, v2);
        if (c2 < 0.0001f) {
            // Tangents are the same after first reflection
            rights[i] = glm::normalize(rL);
        } else {
            rights[i] = glm::normalize(rL - (2.0f / c2) * glm::dot(v2, rL) * v2);
        }
        ups[i] = glm::normalize(glm::cross(rights[i], t1));
    }

    // For each path vertex, create cross-section
    for (size_t i = 0; i < pathVertices.size(); ++i) {
        glm::vec3 pos = pathVertices[i];
        glm::vec3 right = rights[i];
        glm::vec3 localUp = ups[i];

        // Calculate tapered size at this position along the path
        // t goes from 0 (start) to 1 (end), size interpolates from 1.0 to taper
        float t = (pathVertices.size() > 1) ? static_cast<float>(i) / (pathVertices.size() - 1) : 0.0f;
        float sizeMultiplier = glm::mix(1.0f, taper, t);
        float halfSize = baseHalfSize * sizeMultiplier;

        // Create 4 vertices for square cross-section
        // Order: bottom-left, bottom-right, top-right, top-left (CCW when looking along tangent)
        glm::vec3 corners[4] = {
            pos - right * halfSize - localUp * halfSize,  // 0: bottom-left
            pos + right * halfSize - localUp * halfSize,  // 1: bottom-right
            pos + right * halfSize + localUp * halfSize,  // 2: top-right
            pos - right * halfSize + localUp * halfSize   // 3: top-left
        };

        // V coordinate = position along path (0 to 1)
        float vCoord = (pathVertices.size() > 1) ? static_cast<float>(i) / (pathVertices.size() - 1) : 0.0f;

        // Create 5 vertices per cross-section (4 corners + 1 duplicate for UV seam)
        // U coordinates: 0.0, 0.25, 0.5, 0.75, 1.0 (corner 0 duplicated at U=1 for seam)
        for (int c = 0; c < 5; ++c) {
            ModelVertex v;
            v.position = corners[c % 4];  // Corner 4 is same position as corner 0
            v.normal = glm::vec3(0, 1, 0);  // Will recalculate later
            if (autoUV) {
                v.texCoord = glm::vec2(c / 4.0f, vCoord);  // U around tube, V along path
            } else {
                v.texCoord = glm::vec2(0.0f, 0.0f);  // Default UVs
            }
            v.color = meshColor;
            tubeVerts.push_back(v);
        }
    }

    // Create quad faces connecting adjacent cross-sections
    // Now using 5 vertices per cross-section (with seam duplicate)
    for (size_t i = 0; i < pathVertices.size() - 1; ++i) {
        uint32_t base0 = static_cast<uint32_t>(i * 5);
        uint32_t base1 = static_cast<uint32_t>((i + 1) * 5);

        // 4 quads connecting the two cross-sections (as triangles)
        // Use vertices 0-1, 1-2, 2-3, 3-4 for proper UV continuity
        for (int side = 0; side < 4; ++side) {
            uint32_t v0 = base0 + side;
            uint32_t v1 = base1 + side;
            uint32_t v2 = base1 + side + 1;
            uint32_t v3 = base0 + side + 1;

            // Two triangles per quad
            tubeIndices.push_back(v0);
            tubeIndices.push_back(v1);
            tubeIndices.push_back(v2);

            tubeIndices.push_back(v0);
            tubeIndices.push_back(v2);
            tubeIndices.push_back(v3);
        }
    }

    // Create separate vertices for caps with planar UVs
    // Caps are placed next to the tube in UV space (offset to the right)
    // Start cap at U: 1.05-1.55, V: 0.0-0.5
    // End cap at U: 1.05-1.55, V: 0.5-1.0 (stacked below start cap)

    // Cap UVs: corners map to 0.5x0.5 squares, offset to be visible
    float capScale = 0.5f;
    float capOffsetU = 1.05f;  // Place caps to the right of main UV area

    // Start cap UVs
    glm::vec2 startCapUVs[4] = {
        glm::vec2(capOffsetU, 0.5f),                           // Corner 0 (bottom-left)
        glm::vec2(capOffsetU + capScale, 0.5f),                // Corner 1 (bottom-right)
        glm::vec2(capOffsetU + capScale, 0.5f + capScale),     // Corner 2 (top-right)
        glm::vec2(capOffsetU, 0.5f + capScale)                 // Corner 3 (top-left)
    };

    // End cap UVs (same position - they share texture space)
    glm::vec2 endCapUVs[4] = {
        glm::vec2(capOffsetU, 0.0f),                           // Corner 0 (bottom-left)
        glm::vec2(capOffsetU + capScale, 0.0f),                // Corner 1 (bottom-right)
        glm::vec2(capOffsetU + capScale, capScale),            // Corner 2 (top-right)
        glm::vec2(capOffsetU, capScale)                        // Corner 3 (top-left)
    };

    // Start cap vertices (copy positions from first cross-section, add cap UVs)
    uint32_t startCapBase = static_cast<uint32_t>(tubeVerts.size());
    for (int c = 0; c < 4; ++c) {
        ModelVertex v;
        v.position = tubeVerts[c].position;  // Same position as tube vertex
        v.normal = glm::vec3(0, 1, 0);  // Will recalculate
        v.texCoord = autoUV ? startCapUVs[c] : glm::vec2(0.0f);
        v.color = meshColor;
        tubeVerts.push_back(v);
    }

    // End cap vertices (copy positions from last cross-section, add cap UVs)
    uint32_t endCapBase = static_cast<uint32_t>(tubeVerts.size());
    uint32_t lastTubeBase = static_cast<uint32_t>((pathVertices.size() - 1) * 5);
    for (int c = 0; c < 4; ++c) {
        ModelVertex v;
        v.position = tubeVerts[lastTubeBase + c].position;  // Same position as tube vertex
        v.normal = glm::vec3(0, 1, 0);  // Will recalculate
        v.texCoord = autoUV ? endCapUVs[c] : glm::vec2(0.0f);
        v.color = meshColor;
        tubeVerts.push_back(v);
    }

    // Cap the start - normal points backward (opposite to path direction)
    // Winding: 0→1→2, 0→2→3 (CCW when viewed from outside/behind)
    tubeIndices.push_back(startCapBase + 0);
    tubeIndices.push_back(startCapBase + 1);
    tubeIndices.push_back(startCapBase + 2);
    tubeIndices.push_back(startCapBase + 0);
    tubeIndices.push_back(startCapBase + 2);
    tubeIndices.push_back(startCapBase + 3);

    // Cap the end - normal points forward (along path direction)
    // Winding: 0→3→2, 0→2→1 (CCW when viewed from outside/front)
    tubeIndices.push_back(endCapBase + 0);
    tubeIndices.push_back(endCapBase + 3);
    tubeIndices.push_back(endCapBase + 2);
    tubeIndices.push_back(endCapBase + 0);
    tubeIndices.push_back(endCapBase + 2);
    tubeIndices.push_back(endCapBase + 1);

    // Recalculate normals
    // Reset all normals to zero
    for (auto& v : tubeVerts) {
        v.normal = glm::vec3(0);
    }

    // Accumulate face normals
    for (size_t i = 0; i < tubeIndices.size(); i += 3) {
        uint32_t i0 = tubeIndices[i];
        uint32_t i1 = tubeIndices[i + 1];
        uint32_t i2 = tubeIndices[i + 2];

        glm::vec3 p0 = tubeVerts[i0].position;
        glm::vec3 p1 = tubeVerts[i1].position;
        glm::vec3 p2 = tubeVerts[i2].position;

        glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);

        tubeVerts[i0].normal += faceNormal;
        tubeVerts[i1].normal += faceNormal;
        tubeVerts[i2].normal += faceNormal;
    }

    // Normalize all normals
    for (auto& v : tubeVerts) {
        if (glm::length(v.normal) > 0.0001f) {
            v.normal = glm::normalize(v.normal);
        }
    }

    std::cout << "[EdgePath] Created tube with " << tubeVerts.size() << " vertices, "
              << tubeIndices.size() / 3 << " triangles" << std::endl;

    // Build EditableMesh with proper quad topology for wireframe/editing
    EditableMesh tubeMesh;
    tubeMesh.clear();

    // Add vertices (4 per cross-section from the 5-vert pattern, plus cap vertices)
    std::vector<uint32_t> crossSectionStarts;

    // Tube body vertices: pathVertices.size() cross-sections, 5 verts each
    size_t tubeBodyVerts = pathVertices.size() * 5;
    for (size_t v = 0; v < tubeBodyVerts; v += 5) {
        crossSectionStarts.push_back(static_cast<uint32_t>(tubeMesh.getVertexCount()));
        for (int c = 0; c < 4; ++c) {
            HEVertex hv;
            hv.position = tubeVerts[v + c].position;
            hv.normal = tubeVerts[v + c].normal;
            hv.uv = tubeVerts[v + c].texCoord;
            hv.color = tubeVerts[v + c].color;
            hv.halfEdgeIndex = UINT32_MAX;
            hv.selected = false;
            tubeMesh.addVertex(hv);
        }
    }

    // Add cap vertices (startCapBase and endCapBase, 4 verts each)
    uint32_t startCapMeshBase = static_cast<uint32_t>(tubeMesh.getVertexCount());
    for (int c = 0; c < 4; ++c) {
        HEVertex hv;
        hv.position = tubeVerts[startCapBase + c].position;
        hv.normal = tubeVerts[startCapBase + c].normal;
        hv.uv = tubeVerts[startCapBase + c].texCoord;
        hv.color = tubeVerts[startCapBase + c].color;
        hv.halfEdgeIndex = UINT32_MAX;
        hv.selected = false;
        tubeMesh.addVertex(hv);
    }

    uint32_t endCapMeshBase = static_cast<uint32_t>(tubeMesh.getVertexCount());
    for (int c = 0; c < 4; ++c) {
        HEVertex hv;
        hv.position = tubeVerts[endCapBase + c].position;
        hv.normal = tubeVerts[endCapBase + c].normal;
        hv.uv = tubeVerts[endCapBase + c].texCoord;
        hv.color = tubeVerts[endCapBase + c].color;
        hv.halfEdgeIndex = UINT32_MAX;
        hv.selected = false;
        tubeMesh.addVertex(hv);
    }

    // Add quad faces for tube body (not including caps which are triangulated)
    // Tube body indices come before cap indices
    size_t tubeBodyTriangles = (pathVertices.size() - 1) * 4 * 2;  // 4 quads per segment, 2 tris each
    for (size_t i = 0; i < tubeBodyTriangles * 3; i += 6) {
        uint32_t triV0 = tubeIndices[i];
        uint32_t triV1 = tubeIndices[i + 1];
        uint32_t triV2 = tubeIndices[i + 2];
        uint32_t triV3 = tubeIndices[i + 5];

        uint32_t section0 = triV0 / 5;
        uint32_t corner0 = triV0 % 5 % 4;
        uint32_t section1 = triV1 / 5;
        uint32_t corner1 = triV1 % 5 % 4;
        uint32_t section2 = triV2 / 5;
        uint32_t corner2 = triV2 % 5 % 4;
        uint32_t section3 = triV3 / 5;
        uint32_t corner3 = triV3 % 5 % 4;

        if (section0 < crossSectionStarts.size() && section1 < crossSectionStarts.size() &&
            section2 < crossSectionStarts.size() && section3 < crossSectionStarts.size()) {
            std::vector<uint32_t> quadVerts = {
                crossSectionStarts[section0] + corner0,
                crossSectionStarts[section1] + corner1,
                crossSectionStarts[section2] + corner2,
                crossSectionStarts[section3] + corner3
            };
            tubeMesh.addQuadFace(quadVerts);
        }
    }

    // Add cap faces as quads
    tubeMesh.addQuadFace({startCapMeshBase + 0, startCapMeshBase + 1, startCapMeshBase + 2, startCapMeshBase + 3});
    tubeMesh.addQuadFace({endCapMeshBase + 0, endCapMeshBase + 3, endCapMeshBase + 2, endCapMeshBase + 1});

    std::cout << "[EdgePath] EditableMesh: " << tubeMesh.getVertexCount() << " vertices, "
              << tubeMesh.getFaceCount() << " faces" << std::endl;

    // Create a new scene object with this mesh
    auto newObject = std::make_unique<SceneObject>("EdgeTube");

    // Apply the source object's transform to position the tube correctly
    if (m_ctx.selectedObject) {
        newObject->getTransform() = m_ctx.selectedObject->getTransform();
    }

    // Upload to GPU
    uint32_t handle = m_ctx.modelRenderer.createModel(tubeVerts, tubeIndices, nullptr, 0, 0);
    if (handle != UINT32_MAX) {
        newObject->setBufferHandle(handle);
        newObject->setIndexCount(static_cast<uint32_t>(tubeIndices.size()));
        newObject->setVertexCount(static_cast<uint32_t>(tubeVerts.size()));
        newObject->setMeshData(tubeVerts, tubeIndices);

        // Store half-edge data to preserve quad topology
        const auto& meshVerts = tubeMesh.getVerticesData();
        const auto& meshHE = tubeMesh.getHalfEdges();
        const auto& meshFaces = tubeMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(meshVerts.size());
        for (const auto& v : meshVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }

        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(meshHE.size());
        for (const auto& he : meshHE) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(meshFaces.size());
        for (const auto& f : meshFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        newObject->setEditableMeshData(storedVerts, storedHE, storedFaces);

        m_ctx.sceneObjects.push_back(std::move(newObject));
        std::cout << "[EdgePath] Added new object 'EdgeTube' to scene" << std::endl;
    } else {
        std::cout << "[EdgePath] Failed to upload tube mesh to GPU" << std::endl;
    }
}

void ModelingMode::extrudePipeNetwork(float boxSize, float blockSizeMultiplier, bool autoUV) {
    // Get selected edges
    auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
    if (selectedEdges.empty()) {
        std::cout << "[PipeNetwork] No edges selected" << std::endl;
        return;
    }

    std::cout << "[PipeNetwork] Processing " << selectedEdges.size() << " edges" << std::endl;

    // Step 1: Build vertex→edges adjacency map
    std::map<uint32_t, std::vector<uint32_t>> vertexToEdges;
    std::set<uint32_t> edgeSet(selectedEdges.begin(), selectedEdges.end());

    for (uint32_t heIdx : selectedEdges) {
        auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(heIdx);
        vertexToEdges[v0].push_back(heIdx);
        vertexToEdges[v1].push_back(heIdx);
    }

    // Step 2: Classify each vertex
    std::map<uint32_t, PipeVertexType> vertexTypes;
    const float cornerAngleThreshold = glm::radians(45.0f);

    for (auto& [vertIdx, edges] : vertexToEdges) {
        if (edges.size() == 1) {
            vertexTypes[vertIdx] = PipeVertexType::Endpoint;
        } else if (edges.size() == 2) {
            auto [e0v0, e0v1] = m_ctx.editableMesh.getEdgeVertices(edges[0]);
            auto [e1v0, e1v1] = m_ctx.editableMesh.getEdgeVertices(edges[1]);
            uint32_t other0 = (e0v0 == vertIdx) ? e0v1 : e0v0;
            uint32_t other1 = (e1v0 == vertIdx) ? e1v1 : e1v0;

            glm::vec3 centerPos = m_ctx.editableMesh.getVertex(vertIdx).position;
            glm::vec3 pos0 = m_ctx.editableMesh.getVertex(other0).position;
            glm::vec3 pos1 = m_ctx.editableMesh.getVertex(other1).position;

            glm::vec3 dir0 = glm::normalize(pos0 - centerPos);
            glm::vec3 dir1 = glm::normalize(pos1 - centerPos);

            float dotProduct = glm::dot(dir0, dir1);
            float angle = std::acos(glm::clamp(dotProduct, -1.0f, 1.0f));
            float deviationFromStraight = std::abs(glm::pi<float>() - angle);

            if (deviationFromStraight >= cornerAngleThreshold) {
                vertexTypes[vertIdx] = PipeVertexType::Corner;
            } else {
                vertexTypes[vertIdx] = PipeVertexType::Passthrough;
            }
        } else {
            vertexTypes[vertIdx] = PipeVertexType::Junction;
        }
    }

    // Count vertex types for debugging
    int endpoints = 0, passthroughs = 0, corners = 0, junctions = 0;
    for (auto& [vertIdx, type] : vertexTypes) {
        switch (type) {
            case PipeVertexType::Endpoint: endpoints++; break;
            case PipeVertexType::Passthrough: passthroughs++; break;
            case PipeVertexType::Corner: corners++; break;
            case PipeVertexType::Junction: junctions++; break;
        }
    }
    std::cout << "[PipeNetwork] Vertex types - Endpoints: " << endpoints
              << ", Passthroughs: " << passthroughs
              << ", Corners: " << corners
              << ", Junctions: " << junctions << std::endl;

    // Step 3: Find segment break points (only endpoints and junctions, NOT corners)
    // Corners are just bends in the continuous tube, not segment breaks
    std::set<uint32_t> blockVertices;
    for (auto& [vertIdx, type] : vertexTypes) {
        if (type == PipeVertexType::Endpoint || type == PipeVertexType::Junction) {
            blockVertices.insert(vertIdx);
        }
    }

    // Step 4: Segment the network into paths between block vertices
    struct PathSegment {
        std::vector<glm::vec3> vertices;
        uint32_t startBlockVertex;
        uint32_t endBlockVertex;
        bool isClosedLoop = false;  // True if this is a closed loop with no junctions
    };
    std::vector<PathSegment> segments;
    std::set<uint32_t> visitedEdges;

    // Special case: closed loop with no block vertices (all passthrough)
    if (blockVertices.empty() && !vertexToEdges.empty()) {
        std::cout << "[PipeNetwork] Detected closed loop with no junctions" << std::endl;

        // Start from any vertex and walk around the loop
        uint32_t startVertex = vertexToEdges.begin()->first;
        PathSegment segment;
        segment.startBlockVertex = startVertex;
        segment.endBlockVertex = startVertex;
        segment.isClosedLoop = true;
        segment.vertices.push_back(m_ctx.editableMesh.getVertex(startVertex).position);

        uint32_t currentVertex = startVertex;
        uint32_t currentEdge = vertexToEdges[startVertex][0];

        while (true) {
            visitedEdges.insert(currentEdge);

            auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(currentEdge);
            uint32_t nextVertex = (currentVertex == v0) ? v1 : v0;

            // If we're back at the start, close the loop
            if (nextVertex == startVertex) {
                break;
            }

            segment.vertices.push_back(m_ctx.editableMesh.getVertex(nextVertex).position);

            // Find next unvisited edge
            auto& nextEdges = vertexToEdges[nextVertex];
            uint32_t nextEdge = UINT32_MAX;
            for (uint32_t e : nextEdges) {
                if (visitedEdges.count(e) == 0) {
                    nextEdge = e;
                    break;
                }
            }

            if (nextEdge == UINT32_MAX) break;

            currentVertex = nextVertex;
            currentEdge = nextEdge;
        }

        if (segment.vertices.size() >= 2) {
            segments.push_back(segment);
        }
    }

    // Normal case: segment between block vertices
    for (uint32_t startVertex : blockVertices) {
        for (uint32_t startEdge : vertexToEdges[startVertex]) {
            if (visitedEdges.count(startEdge) > 0) continue;

            PathSegment segment;
            segment.startBlockVertex = startVertex;
            segment.vertices.push_back(m_ctx.editableMesh.getVertex(startVertex).position);

            uint32_t currentVertex = startVertex;
            uint32_t currentEdge = startEdge;

            while (true) {
                visitedEdges.insert(currentEdge);

                auto [v0, v1] = m_ctx.editableMesh.getEdgeVertices(currentEdge);
                uint32_t nextVertex = (currentVertex == v0) ? v1 : v0;

                segment.vertices.push_back(m_ctx.editableMesh.getVertex(nextVertex).position);

                if (blockVertices.count(nextVertex) > 0) {
                    segment.endBlockVertex = nextVertex;
                    break;
                }

                auto& nextEdges = vertexToEdges[nextVertex];
                uint32_t nextEdge = UINT32_MAX;
                for (uint32_t e : nextEdges) {
                    if (e != currentEdge && visitedEdges.count(e) == 0) {
                        nextEdge = e;
                        break;
                    }
                }

                if (nextEdge == UINT32_MAX) {
                    segment.endBlockVertex = nextVertex;
                    break;
                }

                currentVertex = nextVertex;
                currentEdge = nextEdge;
            }

            if (segment.vertices.size() >= 2) {
                segments.push_back(segment);
            }
        }
    }

    std::cout << "[PipeNetwork] Created " << segments.size() << " path segments" << std::endl;

    // Collect all geometry
    std::vector<ModelVertex> allVerts;
    std::vector<uint32_t> allIndices;

    float baseHalfSize = boxSize * 0.5f;

    // Get mesh color (random if enabled)
    glm::vec4 meshColor = m_ctx.defaultMeshColor;
    if (m_ctx.randomMeshColors) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        meshColor = glm::vec4(dist(rng), dist(rng), dist(rng), 1.0f);
    }

    // No junction blocks - tubes pass through each other at junctions
    std::cout << "[PipeNetwork] Creating tubes without junction blocks" << std::endl;

    // Step 6: Create tube segments (no junction blocks, tubes pass through each other)
    for (const auto& segment : segments) {
        if (segment.vertices.size() < 2) continue;

        std::vector<glm::vec3> pathVertices = segment.vertices;

        // Pre-compute tangents
        std::vector<glm::vec3> tangents(pathVertices.size());
        for (size_t i = 0; i < pathVertices.size(); ++i) {
            if (segment.isClosedLoop) {
                // For closed loops, wrap around
                size_t prevIdx = (i == 0) ? pathVertices.size() - 1 : i - 1;
                size_t nextIdx = (i == pathVertices.size() - 1) ? 0 : i + 1;
                glm::vec3 incoming = glm::normalize(pathVertices[i] - pathVertices[prevIdx]);
                glm::vec3 outgoing = glm::normalize(pathVertices[nextIdx] - pathVertices[i]);
                tangents[i] = glm::normalize(incoming + outgoing);
            } else if (i == 0) {
                tangents[i] = glm::normalize(pathVertices[1] - pathVertices[0]);
            } else if (i == pathVertices.size() - 1) {
                tangents[i] = glm::normalize(pathVertices[i] - pathVertices[i - 1]);
            } else {
                glm::vec3 incoming = glm::normalize(pathVertices[i] - pathVertices[i - 1]);
                glm::vec3 outgoing = glm::normalize(pathVertices[i + 1] - pathVertices[i]);
                tangents[i] = glm::normalize(incoming + outgoing);
            }
        }

        // Compute frames using consistent world-up reference
        std::vector<glm::vec3> rights(pathVertices.size());
        std::vector<glm::vec3> ups(pathVertices.size());

        // Use world Y as reference, fall back to X if tangent is vertical
        glm::vec3 worldUp = glm::vec3(0, 1, 0);

        // Check if most tangents are vertical - if so, use X as reference
        float avgVertical = 0.0f;
        for (const auto& t : tangents) {
            avgVertical += std::abs(glm::dot(t, worldUp));
        }
        avgVertical /= tangents.size();
        if (avgVertical > 0.7f) {
            worldUp = glm::vec3(1, 0, 0);
        }

        for (size_t i = 0; i < pathVertices.size(); ++i) {
            glm::vec3 tangent = tangents[i];
            // Project worldUp onto plane perpendicular to tangent
            glm::vec3 projected = worldUp - tangent * glm::dot(worldUp, tangent);
            float projLen = glm::length(projected);
            if (projLen > 0.0001f) {
                ups[i] = glm::normalize(projected);
            } else {
                // Tangent is parallel to worldUp, use fallback
                glm::vec3 fallback = (worldUp.y != 0.0f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                ups[i] = glm::normalize(fallback - tangent * glm::dot(fallback, tangent));
            }
            rights[i] = glm::normalize(glm::cross(tangent, ups[i]));
        }

        if (!segment.isClosedLoop) {
            // For open paths: use Double Reflection Method
            // Initial frame
            {
                glm::vec3 tangent = tangents[0];
                glm::vec3 up = glm::vec3(0, 1, 0);
                if (std::abs(glm::dot(tangent, up)) > 0.9f) {
                    up = glm::vec3(1, 0, 0);
                }
                rights[0] = glm::normalize(glm::cross(tangent, up));
                ups[0] = glm::normalize(glm::cross(rights[0], tangent));
            }

            // Propagate using Double Reflection Method
            for (size_t i = 1; i < pathVertices.size(); ++i) {
                glm::vec3 t0 = tangents[i - 1];
                glm::vec3 t1 = tangents[i];

                glm::vec3 v1 = pathVertices[i] - pathVertices[i - 1];
                float c1 = glm::dot(v1, v1);
                if (c1 < 0.0001f) {
                    rights[i] = rights[i - 1];
                    ups[i] = ups[i - 1];
                    continue;
                }

                glm::vec3 rL = rights[i - 1] - (2.0f / c1) * glm::dot(v1, rights[i - 1]) * v1;
                glm::vec3 tL = t0 - (2.0f / c1) * glm::dot(v1, t0) * v1;

                glm::vec3 v2 = t1 - tL;
                float c2 = glm::dot(v2, v2);
                if (c2 < 0.0001f) {
                    rights[i] = glm::normalize(rL);
                } else {
                    rights[i] = glm::normalize(rL - (2.0f / c2) * glm::dot(v2, rL) * v2);
                }
                ups[i] = glm::normalize(glm::cross(rights[i], t1));
            }
        }

        uint32_t tubeBaseIdx = static_cast<uint32_t>(allVerts.size());

        for (size_t i = 0; i < pathVertices.size(); ++i) {
            glm::vec3 pos = pathVertices[i];
            glm::vec3 right = rights[i];
            glm::vec3 localUp = ups[i];

            glm::vec3 corners[4] = {
                pos - right * baseHalfSize - localUp * baseHalfSize,
                pos + right * baseHalfSize - localUp * baseHalfSize,
                pos + right * baseHalfSize + localUp * baseHalfSize,
                pos - right * baseHalfSize + localUp * baseHalfSize
            };

            float vCoord = (pathVertices.size() > 1) ? static_cast<float>(i) / (pathVertices.size() - 1) : 0.0f;

            for (int c = 0; c < 5; ++c) {
                ModelVertex v;
                v.position = corners[c % 4];
                v.normal = glm::vec3(0, 1, 0);
                v.texCoord = autoUV ? glm::vec2(c / 4.0f, vCoord) : glm::vec2(0.0f);
                v.color = meshColor;
                allVerts.push_back(v);
            }
        }

        // Create quads connecting adjacent cross-sections
        for (size_t i = 0; i < pathVertices.size() - 1; ++i) {
            uint32_t base0 = tubeBaseIdx + static_cast<uint32_t>(i * 5);
            uint32_t base1 = tubeBaseIdx + static_cast<uint32_t>((i + 1) * 5);

            for (int side = 0; side < 4; ++side) {
                uint32_t v0 = base0 + side;
                uint32_t v1 = base1 + side;
                uint32_t v2 = base1 + side + 1;
                uint32_t v3 = base0 + side + 1;

                allIndices.push_back(v0);
                allIndices.push_back(v1);
                allIndices.push_back(v2);
                allIndices.push_back(v0);
                allIndices.push_back(v2);
                allIndices.push_back(v3);
            }
        }

        // For closed loops, connect last cross-section back to first
        // Need to find corner correspondence since frames may have rotated
        if (segment.isClosedLoop && pathVertices.size() >= 2) {
            uint32_t baseLast = tubeBaseIdx + static_cast<uint32_t>((pathVertices.size() - 1) * 5);
            uint32_t baseFirst = tubeBaseIdx;

            // Find which corner of the last cross-section best matches corner 0 of the first
            // We compare directions from center, not absolute positions
            glm::vec3 firstCenter = pathVertices[0];
            glm::vec3 lastCenter = pathVertices[pathVertices.size() - 1];

            glm::vec3 firstCorner0Dir = glm::normalize(allVerts[baseFirst].position - firstCenter);

            int bestOffset = 0;
            float bestDot = -2.0f;
            for (int c = 0; c < 4; ++c) {
                glm::vec3 lastCornerDir = glm::normalize(allVerts[baseLast + c].position - lastCenter);
                float d = glm::dot(firstCorner0Dir, lastCornerDir);
                if (d > bestDot) {
                    bestDot = d;
                    bestOffset = c;
                }
            }

            // Connect with the correct corner correspondence
            for (int side = 0; side < 4; ++side) {
                // Map last cross-section corners with offset
                int lastSide0 = (side + bestOffset) % 4;
                int lastSide1 = (side + 1 + bestOffset) % 4;

                uint32_t v0 = baseLast + lastSide0;
                uint32_t v1 = baseFirst + side;
                uint32_t v2 = baseFirst + side + 1;
                uint32_t v3 = baseLast + lastSide1;

                allIndices.push_back(v0);
                allIndices.push_back(v1);
                allIndices.push_back(v2);
                allIndices.push_back(v0);
                allIndices.push_back(v2);
                allIndices.push_back(v3);
            }
        }
    }

    std::cout << "[PipeNetwork] Created " << segments.size() << " tube segments" << std::endl;

    // Recalculate normals
    for (auto& v : allVerts) {
        v.normal = glm::vec3(0);
    }

    for (size_t i = 0; i < allIndices.size(); i += 3) {
        uint32_t i0 = allIndices[i];
        uint32_t i1 = allIndices[i + 1];
        uint32_t i2 = allIndices[i + 2];

        glm::vec3 p0 = allVerts[i0].position;
        glm::vec3 p1 = allVerts[i1].position;
        glm::vec3 p2 = allVerts[i2].position;

        glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);

        allVerts[i0].normal += faceNormal;
        allVerts[i1].normal += faceNormal;
        allVerts[i2].normal += faceNormal;
    }

    for (auto& v : allVerts) {
        if (glm::length(v.normal) > 0.0001f) {
            v.normal = glm::normalize(v.normal);
        }
    }

    std::cout << "[PipeNetwork] Total: " << allVerts.size() << " vertices, "
              << allIndices.size() / 3 << " triangles" << std::endl;

    // Build EditableMesh with proper quad topology for wireframe/editing
    // This preserves the quad structure so wireframe doesn't show triangle diagonals
    EditableMesh pipeMesh;
    pipeMesh.clear();

    // Rebuild with quads - use 4 verts per cross-section (not 5)
    // We need to re-add vertices and build quad faces
    uint32_t quadVertBase = 0;
    std::vector<uint32_t> crossSectionStarts;  // Track where each cross-section's verts start

    // Add vertices (4 per cross-section, from the 5-vert pattern)
    for (size_t v = 0; v < allVerts.size(); v += 5) {
        crossSectionStarts.push_back(static_cast<uint32_t>(pipeMesh.getVertexCount()));
        for (int c = 0; c < 4; ++c) {
            HEVertex hv;
            hv.position = allVerts[v + c].position;
            hv.normal = allVerts[v + c].normal;
            hv.uv = allVerts[v + c].texCoord;
            hv.color = allVerts[v + c].color;
            hv.halfEdgeIndex = UINT32_MAX;
            hv.selected = false;
            pipeMesh.addVertex(hv);
        }
    }

    // Add quad faces for each tube segment
    // Walk through the original index buffer in groups of 6 (2 triangles per quad)
    for (size_t i = 0; i < allIndices.size(); i += 6) {
        // The triangles are (v0,v1,v2) and (v0,v2,v3)
        // So the quad is v0,v1,v2,v3
        uint32_t triV0 = allIndices[i];
        uint32_t triV1 = allIndices[i + 1];
        uint32_t triV2 = allIndices[i + 2];
        uint32_t triV3 = allIndices[i + 5];  // From second triangle

        // Convert from 5-vert indexing to 4-vert indexing
        uint32_t section0 = triV0 / 5;
        uint32_t corner0 = triV0 % 5;
        uint32_t section1 = triV1 / 5;
        uint32_t corner1 = triV1 % 5;
        uint32_t section2 = triV2 / 5;
        uint32_t corner2 = triV2 % 5;
        uint32_t section3 = triV3 / 5;
        uint32_t corner3 = triV3 % 5;

        // Map corner indices (corner 4 wraps to corner 0)
        corner0 = corner0 % 4;
        corner1 = corner1 % 4;
        corner2 = corner2 % 4;
        corner3 = corner3 % 4;

        // Build quad vertex indices
        if (section0 < crossSectionStarts.size() && section1 < crossSectionStarts.size() &&
            section2 < crossSectionStarts.size() && section3 < crossSectionStarts.size()) {
            std::vector<uint32_t> quadVerts = {
                crossSectionStarts[section0] + corner0,
                crossSectionStarts[section1] + corner1,
                crossSectionStarts[section2] + corner2,
                crossSectionStarts[section3] + corner3
            };
            pipeMesh.addQuadFace(quadVerts);
        }
    }

    std::cout << "[PipeNetwork] EditableMesh: " << pipeMesh.getVertexCount() << " vertices, "
              << pipeMesh.getFaceCount() << " quad faces" << std::endl;

    // Create scene object
    auto newObject = std::make_unique<SceneObject>("PipeNetwork");
    if (m_ctx.selectedObject) {
        newObject->getTransform() = m_ctx.selectedObject->getTransform();
    }

    uint32_t handle = m_ctx.modelRenderer.createModel(allVerts, allIndices, nullptr, 0, 0);
    if (handle != UINT32_MAX) {
        newObject->setBufferHandle(handle);
        newObject->setIndexCount(static_cast<uint32_t>(allIndices.size()));
        newObject->setVertexCount(static_cast<uint32_t>(allVerts.size()));
        newObject->setMeshData(allVerts, allIndices);

        // Store half-edge data to preserve quad topology
        // Convert from EditableMesh types to SceneObject storage types
        const auto& meshVerts = pipeMesh.getVerticesData();
        const auto& meshHE = pipeMesh.getHalfEdges();
        const auto& meshFaces = pipeMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(meshVerts.size());
        for (const auto& v : meshVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }

        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(meshHE.size());
        for (const auto& he : meshHE) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(meshFaces.size());
        for (const auto& f : meshFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        newObject->setEditableMeshData(storedVerts, storedHE, storedFaces);

        m_ctx.sceneObjects.push_back(std::move(newObject));
        std::cout << "[PipeNetwork] Added new object 'PipeNetwork' to scene" << std::endl;
    } else {
        std::cout << "[PipeNetwork] Failed to upload mesh to GPU" << std::endl;
    }
}

// ============================================================================
// Face Snap Methods
// ============================================================================

