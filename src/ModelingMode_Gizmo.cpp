// ModelingMode_Gizmo.cpp - Gizmo functions for ModelingMode
// Split from ModelingMode.cpp for better organization

#include "ModelingMode.hpp"
#include "Renderer/Swapchain.hpp"

#include <imgui.h>

#include <iostream>
#include <limits>
#include <set>

using namespace eden;

// Check if we should use the ortho sphere gizmo: ortho view + edge selection + move mode
bool ModelingMode::isOrthoEdgeMoveMode() {
    if (m_ctx.gizmoMode != GizmoMode::Move) return false;
    if (m_ctx.objectMode) return false;
    Camera& cam = m_ctx.getActiveCamera();
    if (cam.getViewPreset() == ViewPreset::Custom) return false;
    if (m_ctx.editableMesh.getSelectedEdges().empty()) return false;
    return true;
}

// Get the two world axes visible in the current ortho view
void ModelingMode::getOrthoAxes(glm::vec3& axis1, glm::vec3& axis2) {
    Camera& cam = m_ctx.getActiveCamera();
    switch (cam.getViewPreset()) {
        case ViewPreset::Top:    // looking down -Y
        case ViewPreset::Bottom: // looking up +Y
            axis1 = glm::vec3(1, 0, 0); axis2 = glm::vec3(0, 0, 1); break;
        case ViewPreset::Front:  // looking down -Z
        case ViewPreset::Back:   // looking down +Z
            axis1 = glm::vec3(1, 0, 0); axis2 = glm::vec3(0, 1, 0); break;
        case ViewPreset::Right:  // looking down -X
        case ViewPreset::Left:   // looking down +X
            axis1 = glm::vec3(0, 0, 1); axis2 = glm::vec3(0, 1, 0); break;
        default:
            axis1 = glm::vec3(1, 0, 0); axis2 = glm::vec3(0, 1, 0); break;
    }
}

glm::vec3 ModelingMode::getGizmoPosition() {
    if (!m_ctx.selectedObject) return glm::vec3(0.0f);

    // Rigging mode: gizmo at selected bone position
    if (m_riggingMode && m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bonePositions.size())) {
        glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
        return glm::vec3(modelMatrix * glm::vec4(m_bonePositions[m_selectedBone], 1.0f)) + m_ctx.gizmoOffset;
    }

    // Use custom pivot if set (e.g., after face snap)
    if (m_useCustomGizmoPivot) {
        return m_customGizmoPivot;
    }

    glm::vec3 basePos;

    // In object mode, gizmo is on top of the model
    if (m_ctx.objectMode) {
        // Calculate bounding box of mesh and place gizmo on top
        if (m_ctx.editableMesh.getVertexCount() > 0) {
            glm::vec3 center(0.0f);
            float maxY = -std::numeric_limits<float>::max();

            for (size_t i = 0; i < m_ctx.editableMesh.getVertexCount(); ++i) {
                const glm::vec3& pos = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(i)).position;
                center += pos;
                if (pos.y > maxY) {
                    maxY = pos.y;
                }
            }
            center /= static_cast<float>(m_ctx.editableMesh.getVertexCount());

            // Use center X/Z but top Y
            glm::vec3 gizmoLocalPos(center.x, maxY, center.z);
            glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();
            basePos = glm::vec3(modelMatrix * glm::vec4(gizmoLocalPos, 1.0f));
        } else {
            basePos = m_ctx.selectedObject->getTransform().getPosition();
        }
    } else {
        // Count selected components
        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();

        int numSelectedComponents = 0;
        if (!selectedVerts.empty()) numSelectedComponents += selectedVerts.size();
        else if (!selectedEdges.empty()) numSelectedComponents += selectedEdges.size();
        else if (!selectedFaces.empty()) numSelectedComponents += selectedFaces.size();

        // Collect all vertices from any selection type
        std::set<uint32_t> allVerts;

        // Add directly selected vertices
        for (uint32_t vi : selectedVerts) {
            allVerts.insert(vi);
        }

        // Add vertices from selected edges
        for (uint32_t ei : selectedEdges) {
            auto edgeVerts = m_ctx.editableMesh.getEdgeVertices(ei);
            allVerts.insert(edgeVerts.first);
            allVerts.insert(edgeVerts.second);
        }

        // Add vertices from selected faces
        for (uint32_t fi : selectedFaces) {
            auto verts = m_ctx.editableMesh.getFaceVertices(fi);
            for (uint32_t vi : verts) {
                allVerts.insert(vi);
            }
        }

        if (!allVerts.empty()) {
            glm::mat4 modelMatrix = m_ctx.selectedObject->getTransform().getMatrix();

            if (numSelectedComponents == 1) {
                // Single component selected: place gizmo at its center
                glm::vec3 center(0.0f);
                for (uint32_t vi : allVerts) {
                    center += m_ctx.editableMesh.getVertex(vi).position;
                }
                center /= static_cast<float>(allVerts.size());
                basePos = glm::vec3(modelMatrix * glm::vec4(center, 1.0f));
            } else {
                // Multiple components selected: place gizmo on top (highest Y)
                // Calculate center X/Z but use max Y
                glm::vec3 center(0.0f);
                float maxY = -std::numeric_limits<float>::max();

                for (uint32_t vi : allVerts) {
                    glm::vec3 pos = m_ctx.editableMesh.getVertex(vi).position;
                    center += pos;
                    if (pos.y > maxY) {
                        maxY = pos.y;
                    }
                }
                center /= static_cast<float>(allVerts.size());

                // Use center X/Z but top Y
                glm::vec3 gizmoLocalPos(center.x, maxY, center.z);
                basePos = glm::vec3(modelMatrix * glm::vec4(gizmoLocalPos, 1.0f));
            }
        } else {
            // Default to object origin
            basePos = m_ctx.selectedObject->getTransform().getPosition();
        }
    }

    // Apply user offset
    return basePos + m_ctx.gizmoOffset;
}

void ModelingMode::getGizmoAxes(glm::vec3& xAxis, glm::vec3& yAxis, glm::vec3& zAxis) {
    // Default world space axes
    xAxis = glm::vec3(1, 0, 0);
    yAxis = glm::vec3(0, 1, 0);
    zAxis = glm::vec3(0, 0, 1);

    // Only apply local space for Move mode with face selection
    if (!m_ctx.gizmoLocalSpace || m_ctx.gizmoMode != GizmoMode::Move || m_ctx.objectMode) {
        return;
    }

    // Calculate average normal from selected faces
    auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
    if (selectedFaces.empty()) return;

    glm::vec3 avgNormal(0.0f);
    for (uint32_t fi : selectedFaces) {
        glm::vec3 faceNormal = m_ctx.editableMesh.getFaceNormal(fi);
        avgNormal += faceNormal;
    }
    avgNormal = glm::normalize(avgNormal);

    // Transform normal to world space if we have an object
    if (m_ctx.selectedObject) {
        glm::mat4 modelMat = m_ctx.selectedObject->getTransform().getMatrix();
        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(modelMat)));
        avgNormal = glm::normalize(normalMat * avgNormal);
    }

    // Z axis is the face normal
    zAxis = avgNormal;

    // Build orthonormal basis - X and Y perpendicular to Z
    glm::vec3 up = (std::abs(avgNormal.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    xAxis = glm::normalize(glm::cross(up, zAxis));
    yAxis = glm::normalize(glm::cross(zAxis, xAxis));
}

float ModelingMode::rayAxisDistance(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                     const glm::vec3& axisOrigin, const glm::vec3& axisDir) {
    // Find closest point between ray and axis line
    glm::vec3 w0 = rayOrigin - axisOrigin;
    float a = glm::dot(rayDir, rayDir);
    float b = glm::dot(rayDir, axisDir);
    float c = glm::dot(axisDir, axisDir);
    float d = glm::dot(rayDir, w0);
    float e = glm::dot(axisDir, w0);

    float denom = a * c - b * b;
    if (std::abs(denom) < 0.0001f) return FLT_MAX;

    float t = (b * e - c * d) / denom;
    float s = (a * e - b * d) / denom;

    // Clamp s to axis length (0 to gizmoSize)
    s = std::clamp(s, 0.0f, m_ctx.gizmoSize);

    glm::vec3 pointOnRay = rayOrigin + rayDir * t;
    glm::vec3 pointOnAxis = axisOrigin + axisDir * s;

    return glm::length(pointOnRay - pointOnAxis);
}

glm::vec3 ModelingMode::projectPointOntoAxis(const glm::vec3& point, const glm::vec3& axisOrigin, const glm::vec3& axisDir) {
    glm::vec3 v = point - axisOrigin;
    float t = glm::dot(v, axisDir);
    return axisOrigin + axisDir * t;
}

GizmoAxis ModelingMode::pickGizmoAxis(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& gizmoPos) {
    float threshold = 0.15f * m_ctx.gizmoSize;  // Pick tolerance

    // Ortho edge move: check if near the sphere
    if (isOrthoEdgeMoveMode()) {
        float orthoSize = m_ctx.getActiveCamera().getOrthoSize();
        float sphereRadius = orthoSize * 0.02f;
        glm::vec3 toGizmo = gizmoPos - rayOrigin;
        float t = glm::dot(toGizmo, rayDir);
        if (t > 0) {
            glm::vec3 closest = rayOrigin + rayDir * t;
            float dist = glm::length(closest - gizmoPos);
            if (dist < sphereRadius * 2.0f) {
                return GizmoAxis::OrthoFree;
            }
        }
        return GizmoAxis::None;
    }

    // In scale mode, check center cube first for uniform scaling
    if (m_ctx.gizmoMode == GizmoMode::Scale) {
        float centerCubeSize = m_ctx.gizmoSize * 0.12f * 1.2f;  // Match render size
        // Simple ray-box intersection for center cube
        glm::vec3 toGizmo = gizmoPos - rayOrigin;
        float t = glm::dot(toGizmo, rayDir);
        if (t > 0) {
            glm::vec3 closestPoint = rayOrigin + rayDir * t;
            float dist = glm::length(closestPoint - gizmoPos);
            if (dist < centerCubeSize * 1.5f) {
                return GizmoAxis::Uniform;
            }
        }
    }

    // In rotate mode, pick circles instead of axis lines
    if (m_ctx.gizmoMode == GizmoMode::Rotate) {
        float circleRadius = m_ctx.gizmoSize * 0.9f;
        float ringThreshold = threshold * 1.5f;

        // Helper lambda to check ray-circle intersection
        auto checkCircle = [&](const glm::vec3& normal) -> float {
            // Intersect ray with plane
            float denom = glm::dot(rayDir, normal);
            if (std::abs(denom) < 0.0001f) return 999.0f;
            float t = glm::dot(gizmoPos - rayOrigin, normal) / denom;
            if (t < 0) return 999.0f;
            glm::vec3 hitPoint = rayOrigin + rayDir * t;
            float distFromCenter = glm::length(hitPoint - gizmoPos);
            // Check if hit is on the ring (near the circle radius)
            return std::abs(distFromCenter - circleRadius);
        };

        float distX = checkCircle(glm::vec3(1, 0, 0));
        float distY = checkCircle(glm::vec3(0, 1, 0));
        float distZ = checkCircle(glm::vec3(0, 0, 1));

        float minDist = std::min({distX, distY, distZ});
        if (minDist > ringThreshold) return GizmoAxis::None;

        if (minDist == distX) return GizmoAxis::X;
        if (minDist == distY) return GizmoAxis::Y;
        return GizmoAxis::Z;
    }

    // Move/Scale mode: pick axis lines using local or world space axes
    glm::vec3 gizmoXAxis, gizmoYAxis, gizmoZAxis;
    getGizmoAxes(gizmoXAxis, gizmoYAxis, gizmoZAxis);

    float distX = rayAxisDistance(rayOrigin, rayDir, gizmoPos, gizmoXAxis);
    float distY = rayAxisDistance(rayOrigin, rayDir, gizmoPos, gizmoYAxis);
    float distZ = rayAxisDistance(rayOrigin, rayDir, gizmoPos, gizmoZAxis);

    float minDist = std::min({distX, distY, distZ});

    if (minDist > threshold) return GizmoAxis::None;

    if (minDist == distX) return GizmoAxis::X;
    if (minDist == distY) return GizmoAxis::Y;
    return GizmoAxis::Z;
}

bool ModelingMode::processGizmoInput() {
    if (m_ctx.gizmoMode == GizmoMode::None) return false;
    // Use IsWindowHovered for mouse-over detection
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) return false;
    if (!m_ctx.selectedObject) return false;

    // In rigging mode, gizmo shows for selected bone
    bool riggingBoneSelected = m_riggingMode && m_selectedBone >= 0 &&
                               m_selectedBone < static_cast<int>(m_bonePositions.size());

    // In object mode, gizmo always shows for selected object
    // In component mode, need actual component selection
    if (!m_ctx.objectMode && !riggingBoneSelected) {
        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        bool hasSelection = !selectedVerts.empty() || !selectedFaces.empty() || !selectedEdges.empty();
        if (!hasSelection) return false;
    }

    glm::vec3 gizmoPos = getGizmoPosition();

    // Get mouse ray
    glm::vec3 rayOrigin, rayDir;
    m_ctx.getMouseRay(rayOrigin, rayDir);

    // Handle dragging
    if (m_ctx.gizmoDragging) {
        if (!Input::isMouseButtonDown(Input::MOUSE_LEFT)) {
            // End drag
            m_ctx.gizmoDragging = false;
            m_ctx.gizmoActiveAxis = GizmoAxis::None;
        } else {
            // Continue drag
            // Get local or world space axes
            glm::vec3 gizmoXAxis, gizmoYAxis, gizmoZAxis;
            getGizmoAxes(gizmoXAxis, gizmoYAxis, gizmoZAxis);

            // Handle OrthoFree drag: project mouse delta onto 2 visible axes
            if (m_ctx.gizmoActiveAxis == GizmoAxis::OrthoFree) {
                glm::vec3 orthoAxis1, orthoAxis2;
                getOrthoAxes(orthoAxis1, orthoAxis2);

                // Intersect ray with the plane defined by the two ortho axes through gizmo start pos
                Camera& cam = m_ctx.getActiveCamera();
                glm::vec3 planeNormal = glm::normalize(glm::cross(orthoAxis1, orthoAxis2));
                float denom = glm::dot(rayDir, planeNormal);
                if (std::abs(denom) > 0.0001f) {
                    float t = glm::dot(m_ctx.gizmoDragStartPos - rayOrigin, planeNormal) / denom;
                    glm::vec3 currentPoint = rayOrigin + rayDir * t;
                    glm::vec3 delta = currentPoint - m_ctx.gizmoDragStart;

                    if (glm::length(delta) > 0.0001f) {
                        // Apply snap if enabled
                        if (m_ctx.snapEnabled && m_ctx.moveSnapIncrement > 0.0f) {
                            float d1 = glm::dot(delta, orthoAxis1);
                            float d2 = glm::dot(delta, orthoAxis2);
                            float snap = m_ctx.moveSnapIncrement;
                            d1 = std::round(d1 / snap) * snap;
                            d2 = std::round(d2 / snap) * snap;
                            delta = orthoAxis1 * d1 + orthoAxis2 * d2;
                        }

                        if (m_ctx.objectMode && m_ctx.selectedObject) {
                            m_ctx.selectedObject->getTransform().translate(delta);
                        } else if (!m_ctx.objectMode && m_ctx.selectedObject) {
                            glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                            glm::vec3 localDelta = glm::vec3(invModel * glm::vec4(delta, 0.0f));
                            m_ctx.editableMesh.translateSelectedVertices(localDelta);
                            m_ctx.meshDirty = true;
                        }
                        m_ctx.gizmoDragStart = currentPoint;
                    }
                }
                return true;
            }

            glm::vec3 axisDir(0.0f);
            switch (m_ctx.gizmoActiveAxis) {
                case GizmoAxis::X: axisDir = gizmoXAxis; break;
                case GizmoAxis::Y: axisDir = gizmoYAxis; break;
                case GizmoAxis::Z: axisDir = gizmoZAxis; break;
                case GizmoAxis::Uniform: axisDir = glm::normalize(glm::vec3(1, 1, 1)); break;
                default: return true;  // Still dragging, consume input
            }

            // Handle ROTATE mode separately using mouse position delta
            if (m_ctx.gizmoMode == GizmoMode::Rotate) {
                // For rotation, gizmoDragStart.x/y stores the last mouse position
                glm::vec2 currentMouse = Input::getMousePosition();
                glm::vec2 lastMouse(m_ctx.gizmoDragStart.x, m_ctx.gizmoDragStart.y);
                glm::vec2 mouseDelta = currentMouse - lastMouse;

                float sensitivity = 0.5f;  // degrees per pixel
                float angle = -(mouseDelta.x) * sensitivity;

                // Apply rotation snap if enabled
                if (m_ctx.snapEnabled && m_ctx.rotateSnapIncrement > 0.0f) {
                    // Track accumulated rotation and only apply when crossing snap threshold
                    static float accumulatedAngle = 0.0f;
                    accumulatedAngle += angle;
                    float snappedAngle = std::floor(accumulatedAngle / m_ctx.rotateSnapIncrement) * m_ctx.rotateSnapIncrement;
                    angle = snappedAngle;
                    accumulatedAngle -= snappedAngle;
                }

                if (std::abs(angle) > 0.001f) {
                    if (riggingBoneSelected && m_ctx.selectedObject) {
                        // Rigging mode: rotate selected bone's children around bone pivot
                        glm::mat4 modelMat = m_ctx.selectedObject->getTransform().getMatrix();
                        glm::mat4 invModel = glm::inverse(modelMat);
                        float angleRad = glm::radians(angle);
                        glm::vec3 localAxis = glm::normalize(glm::vec3(invModel * glm::vec4(axisDir, 0.0f)));
                        glm::quat rotation = glm::angleAxis(angleRad, localAxis);
                        glm::mat4 rotMat = glm::mat4_cast(rotation);

                        glm::vec3 pivot = m_bonePositions[m_selectedBone];

                        // Rotate all descendant bone positions
                        auto descendants = getDescendantBones(m_selectedBone);
                        for (int di : descendants) {
                            if (di < static_cast<int>(m_bonePositions.size())) {
                                glm::vec3 rel = m_bonePositions[di] - pivot;
                                m_bonePositions[di] = pivot + glm::vec3(rotMat * glm::vec4(rel, 1.0f));
                            }
                        }

                        // Deform weighted vertices
                        std::set<int> affectedSet(descendants.begin(), descendants.end());
                        affectedSet.insert(m_selectedBone);
                        for (size_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
                            auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                            float totalAffectedWeight = 0.0f;
                            for (int j = 0; j < 4; ++j) {
                                if (v.boneWeights[j] > 0.0f && affectedSet.count(v.boneIndices[j])) {
                                    totalAffectedWeight += v.boneWeights[j];
                                }
                            }
                            if (totalAffectedWeight > 0.001f) {
                                glm::vec3 rel = v.position - pivot;
                                glm::vec3 rotatedPos = pivot + glm::vec3(rotMat * glm::vec4(rel, 1.0f));
                                v.position = glm::mix(v.position, rotatedPos, totalAffectedWeight);
                            }
                        }
                        m_ctx.meshDirty = true;
                        invalidateWireframeCache();
                    } else if (m_ctx.objectMode && m_ctx.selectedObject) {
                        // Object mode: rotate the object's transform
                        m_ctx.selectedObject->getTransform().rotate(angle, axisDir);
                    } else if (!m_ctx.objectMode && m_ctx.selectedObject) {
                        // Component mode: rotate selected vertices around selection center
                        std::set<uint32_t> vertsToTransform;
                        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
                        for (uint32_t vi : selectedVerts) vertsToTransform.insert(vi);
                        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
                        for (uint32_t ei : selectedEdges) {
                            auto edgeVerts = m_ctx.editableMesh.getEdgeVertices(ei);
                            vertsToTransform.insert(edgeVerts.first);
                            vertsToTransform.insert(edgeVerts.second);
                        }
                        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
                        for (uint32_t fi : selectedFaces) {
                            auto faceVerts = m_ctx.editableMesh.getFaceVertices(fi);
                            for (uint32_t vi : faceVerts) vertsToTransform.insert(vi);
                        }

                        if (!vertsToTransform.empty()) {
                            auto posKey = [](const glm::vec3& p) -> uint64_t {
                                int32_t x = static_cast<int32_t>(p.x * 10000.0f);
                                int32_t y = static_cast<int32_t>(p.y * 10000.0f);
                                int32_t z = static_cast<int32_t>(p.z * 10000.0f);
                                return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
                                       (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
                                       static_cast<uint64_t>(z & 0xFFFFF);
                            };

                            std::set<uint64_t> positionsToTransform;
                            for (uint32_t vi : vertsToTransform) {
                                positionsToTransform.insert(posKey(m_ctx.editableMesh.getVertex(vi).position));
                            }

                            glm::vec3 pivot = m_ctx.editableMesh.getSelectionCenter();
                            glm::mat4 modelMat = m_ctx.selectedObject->getTransform().getMatrix();
                            glm::mat4 invModel = glm::inverse(modelMat);

                            // Transform axis to local space for rotation (radians for component mode)
                            float angleRad = glm::radians(angle);
                            glm::vec3 localAxis = glm::normalize(glm::vec3(invModel * glm::vec4(axisDir, 0.0f)));
                            glm::quat rotation = glm::angleAxis(angleRad, localAxis);
                            glm::mat4 rotMat = glm::mat4_cast(rotation);

                            for (size_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
                                auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                                if (positionsToTransform.count(posKey(v.position))) {
                                    glm::vec3 relPos = v.position - pivot;
                                    v.position = pivot + glm::vec3(rotMat * glm::vec4(relPos, 1.0f));
                                }
                            }
                            m_ctx.meshDirty = true;
                        }
                    }
                }
                // Update stored mouse position for next frame
                m_ctx.gizmoDragStart = glm::vec3(currentMouse.x, currentMouse.y, 0.0f);
                return true;  // Rotation handled, don't continue to move/scale logic
            }

            // For Move/Scale: project mouse onto axis
            glm::vec3 w0 = rayOrigin - m_ctx.gizmoDragStartPos;
            float a = glm::dot(rayDir, rayDir);
            float b = glm::dot(rayDir, axisDir);
            float c = glm::dot(axisDir, axisDir);
            float d = glm::dot(rayDir, w0);
            float e = glm::dot(axisDir, w0);

            float denom = a * c - b * b;
            if (std::abs(denom) > 0.0001f) {
                float s = (a * e - b * d) / denom;
                glm::vec3 currentPoint = m_ctx.gizmoDragStartPos + axisDir * s;
                glm::vec3 startPoint = projectPointOntoAxis(m_ctx.gizmoDragStart, m_ctx.gizmoDragStartPos, axisDir);

                glm::vec3 delta = currentPoint - startPoint;

                // Apply move snap if enabled (for Move mode only)
                bool useSnap = m_ctx.snapEnabled && m_ctx.moveSnapIncrement > 0.0f && m_ctx.gizmoMode == GizmoMode::Move;
                if (useSnap) {
                    // Snap the delta length to the nearest increment
                    float deltaLen = glm::dot(delta, axisDir);  // Signed length along axis
                    float snappedLen = std::round(deltaLen / m_ctx.moveSnapIncrement) * m_ctx.moveSnapIncrement;
                    delta = axisDir * snappedLen;
                }

                if (riggingBoneSelected && m_ctx.selectedObject && m_ctx.gizmoMode == GizmoMode::Move) {
                    // Rigging mode: move selected bone + all children + deform weighted vertices
                    glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                    glm::vec3 localDelta = glm::vec3(invModel * glm::vec4(delta, 0.0f));

                    // Move selected bone
                    m_bonePositions[m_selectedBone] += localDelta;

                    // Move all descendant bones by same delta
                    auto descendants = getDescendantBones(m_selectedBone);
                    for (int di : descendants) {
                        if (di < static_cast<int>(m_bonePositions.size())) {
                            m_bonePositions[di] += localDelta;
                        }
                    }

                    // Deform weighted vertices proportional to bone weights
                    std::set<int> affectedSet(descendants.begin(), descendants.end());
                    affectedSet.insert(m_selectedBone);
                    for (size_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
                        auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                        float totalAffectedWeight = 0.0f;
                        for (int j = 0; j < 4; ++j) {
                            if (v.boneWeights[j] > 0.0f && affectedSet.count(v.boneIndices[j])) {
                                totalAffectedWeight += v.boneWeights[j];
                            }
                        }
                        if (totalAffectedWeight > 0.001f) {
                            v.position += localDelta * totalAffectedWeight;
                        }
                    }
                    m_ctx.meshDirty = true;
                    invalidateWireframeCache();

                    // Update drag start for incremental movement
                    m_ctx.gizmoDragStart = currentPoint;
                    m_ctx.gizmoDragStartPos = getGizmoPosition();
                } else if (m_ctx.objectMode && m_ctx.selectedObject) {
                    auto& transform = m_ctx.selectedObject->getTransform();

                    if (m_ctx.gizmoMode == GizmoMode::Scale) {
                        // Object mode scale: adjust transform scale
                        float deltaLen = glm::dot(delta, axisDir);
                        float scaleFactor = 1.0f + deltaLen * 0.5f;
                        scaleFactor = std::max(0.1f, std::min(scaleFactor, 3.0f));  // Clamp factor to safe range

                        glm::vec3 currentScale = transform.getScale();
                        if (m_ctx.gizmoActiveAxis == GizmoAxis::X) {
                            currentScale.x *= scaleFactor;
                        } else if (m_ctx.gizmoActiveAxis == GizmoAxis::Y) {
                            currentScale.y *= scaleFactor;
                        } else if (m_ctx.gizmoActiveAxis == GizmoAxis::Z) {
                            currentScale.z *= scaleFactor;
                        } else if (m_ctx.gizmoActiveAxis == GizmoAxis::Uniform) {
                            currentScale *= scaleFactor;
                        }
                        // Prevent scale from going negative or too small
                        currentScale = glm::max(currentScale, glm::vec3(0.01f));
                        transform.setScale(currentScale);
                        // Update drag start for incremental scale
                        m_ctx.gizmoDragStart = currentPoint;
                        m_ctx.gizmoDragStartPos = getGizmoPosition();
                    } else {
                        // Object mode move
                        if (useSnap) {
                            // For snap: set position relative to original object position
                            // Use stored original position to avoid initial jump
                            transform.setPosition(m_ctx.gizmoOriginalObjPos + delta);
                            // DON'T update gizmoDragStart - keep original for total delta calculation
                        } else {
                            // Non-snap: incremental movement
                            transform.setPosition(transform.getPosition() + delta);
                            // Update drag start for incremental movement
                            m_ctx.gizmoDragStart = currentPoint;
                            m_ctx.gizmoDragStartPos = getGizmoPosition();
                        }
                    }
                } else {
                    // Component mode: move or scale selected vertices/edges/faces
                    std::set<uint32_t> vertsToTransform;

                    // Add directly selected vertices
                    auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
                    for (uint32_t vi : selectedVerts) {
                        vertsToTransform.insert(vi);
                    }

                    // Add vertices from selected edges
                    auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
                    for (uint32_t ei : selectedEdges) {
                        auto edgeVerts = m_ctx.editableMesh.getEdgeVertices(ei);
                        vertsToTransform.insert(edgeVerts.first);
                        vertsToTransform.insert(edgeVerts.second);
                    }

                    // Add vertices from selected faces
                    auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
                    for (uint32_t fi : selectedFaces) {
                        auto faceVerts = m_ctx.editableMesh.getFaceVertices(fi);
                        for (uint32_t vi : faceVerts) {
                            vertsToTransform.insert(vi);
                        }
                    }

                    // Apply transform to all collected vertices AND any duplicates at same positions
                    if (!vertsToTransform.empty() && m_ctx.selectedObject) {
                        // Collect positions of vertices to transform (handles duplicate verts for hard edges)
                        auto posKey = [](const glm::vec3& p) -> uint64_t {
                            int32_t x = static_cast<int32_t>(p.x * 10000.0f);
                            int32_t y = static_cast<int32_t>(p.y * 10000.0f);
                            int32_t z = static_cast<int32_t>(p.z * 10000.0f);
                            return (static_cast<uint64_t>(x & 0xFFFFF) << 40) |
                                   (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
                                   static_cast<uint64_t>(z & 0xFFFFF);
                        };

                        std::set<uint64_t> positionsToTransform;
                        for (uint32_t vi : vertsToTransform) {
                            positionsToTransform.insert(posKey(m_ctx.editableMesh.getVertex(vi).position));
                        }

                        if (m_ctx.gizmoMode == GizmoMode::Scale) {
                            // SCALE MODE: scale along axis around selection center
                            // Calculate scale factor based on movement along axis
                            float deltaLen = glm::dot(delta, axisDir);
                            float scaleFactor = 1.0f + deltaLen * 0.5f;  // Adjust sensitivity
                            scaleFactor = std::max(0.01f, scaleFactor);  // Prevent negative/zero scale

                            // Get selection center in local space as pivot
                            glm::vec3 pivot = m_ctx.editableMesh.getSelectionCenter();

                            // Build scale vector (only scale along active axis, or all for uniform)
                            glm::vec3 scaleVec(1.0f);
                            switch (m_ctx.gizmoActiveAxis) {
                                case GizmoAxis::X: scaleVec.x = scaleFactor; break;
                                case GizmoAxis::Y: scaleVec.y = scaleFactor; break;
                                case GizmoAxis::Z: scaleVec.z = scaleFactor; break;
                                case GizmoAxis::Uniform: scaleVec = glm::vec3(scaleFactor); break;
                                default: break;
                            }

                            // Scale ALL vertices at those positions around pivot
                            for (size_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
                                auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                                if (positionsToTransform.count(posKey(v.position))) {
                                    v.position = pivot + (v.position - pivot) * scaleVec;
                                }
                            }
                        } else if (m_ctx.gizmoMode == GizmoMode::Move) {
                            // MOVE MODE: translate vertices
                            glm::mat4 invModel = glm::inverse(m_ctx.selectedObject->getTransform().getMatrix());
                            glm::vec3 localDelta = glm::vec3(invModel * glm::vec4(delta, 0.0f));

                            // Move ALL vertices at those positions
                            for (size_t vi = 0; vi < m_ctx.editableMesh.getVertexCount(); ++vi) {
                                auto& v = m_ctx.editableMesh.getVertex(static_cast<uint32_t>(vi));
                                if (positionsToTransform.count(posKey(v.position))) {
                                    v.position = v.position + localDelta;
                                }
                            }
                        }
                        m_ctx.meshDirty = true;

                        // Update drag start for incremental movement
                        m_ctx.gizmoDragStart = currentPoint;
                    }
                }
            }
        }
    } else {
        // Not dragging - check for hover and start drag
        m_ctx.gizmoHoveredAxis = pickGizmoAxis(rayOrigin, rayDir, gizmoPos);

        if (Input::isMouseButtonPressed(Input::MOUSE_LEFT) && m_ctx.gizmoHoveredAxis != GizmoAxis::None) {
            // Start drag
            m_ctx.gizmoDragging = true;
            m_ctx.gizmoActiveAxis = m_ctx.gizmoHoveredAxis;
            m_ctx.gizmoDragStartPos = gizmoPos;

            // Find initial point on axis - use local or world space axes
            glm::vec3 gizmoXAxis, gizmoYAxis, gizmoZAxis;
            getGizmoAxes(gizmoXAxis, gizmoYAxis, gizmoZAxis);

            glm::vec3 axisDir(0.0f);
            switch (m_ctx.gizmoActiveAxis) {
                case GizmoAxis::X: axisDir = gizmoXAxis; break;
                case GizmoAxis::Y: axisDir = gizmoYAxis; break;
                case GizmoAxis::Z: axisDir = gizmoZAxis; break;
                case GizmoAxis::Uniform: axisDir = glm::normalize(glm::vec3(1, 1, 1)); break;
                default: break;
            }

            if (m_ctx.gizmoActiveAxis == GizmoAxis::OrthoFree) {
                // For ortho free drag, intersect ray with the 2-axis plane
                glm::vec3 orthoAxis1, orthoAxis2;
                getOrthoAxes(orthoAxis1, orthoAxis2);
                glm::vec3 planeNormal = glm::normalize(glm::cross(orthoAxis1, orthoAxis2));
                float denom = glm::dot(rayDir, planeNormal);
                if (std::abs(denom) > 0.0001f) {
                    float t = glm::dot(gizmoPos - rayOrigin, planeNormal) / denom;
                    m_ctx.gizmoDragStart = rayOrigin + rayDir * t;
                } else {
                    m_ctx.gizmoDragStart = gizmoPos;
                }
            } else if (m_ctx.gizmoMode == GizmoMode::Rotate) {
                // For rotation, store mouse screen position (in x,y of gizmoDragStart)
                glm::vec2 mousePos = Input::getMousePosition();
                m_ctx.gizmoDragStart = glm::vec3(mousePos.x, mousePos.y, 0.0f);
            } else {
                // For move/scale, find point on axis closest to ray
                glm::vec3 w0 = rayOrigin - gizmoPos;
                float a = glm::dot(rayDir, rayDir);
                float b = glm::dot(rayDir, axisDir);
                float c = glm::dot(axisDir, axisDir);
                float d = glm::dot(rayDir, w0);
                float e = glm::dot(axisDir, w0);

                float denom = a * c - b * b;
                if (std::abs(denom) > 0.0001f) {
                    float s = (a * e - b * d) / denom;
                    m_ctx.gizmoDragStart = gizmoPos + axisDir * s;
                } else {
                    m_ctx.gizmoDragStart = gizmoPos;
                }
            }

            // Store original object position for snap mode
            if (m_ctx.objectMode && m_ctx.selectedObject) {
                m_ctx.gizmoOriginalObjPos = m_ctx.selectedObject->getTransform().getPosition();
            }

            // Save state for undo
            m_ctx.editableMesh.saveState();

            return true;  // Gizmo consumed this click
        }
    }

    // Return true only if actively dragging (to block other input)
    // Hovering over gizmo highlights the axis but doesn't block point selection
    return m_ctx.gizmoDragging;
}

void ModelingMode::renderGizmo(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (m_ctx.gizmoMode == GizmoMode::None) return;
    if (!m_ctx.selectedObject) return;

    // In rigging mode, show gizmo for selected bone
    bool riggingGizmo = m_riggingMode && m_selectedBone >= 0 &&
                        m_selectedBone < static_cast<int>(m_bonePositions.size());

    // In object mode, always show gizmo for selected object
    // In component mode, need actual component selection
    if (!m_ctx.objectMode && !riggingGizmo) {
        auto selectedVerts = m_ctx.editableMesh.getSelectedVertices();
        auto selectedFaces = m_ctx.editableMesh.getSelectedFaces();
        auto selectedEdges = m_ctx.editableMesh.getSelectedEdges();
        bool hasSelection = !selectedVerts.empty() || !selectedFaces.empty() || !selectedEdges.empty();
        if (!hasSelection) return;
    }

    glm::vec3 gizmoPos = getGizmoPosition();
    float size = m_ctx.gizmoSize;

    // Define axis colors
    glm::vec3 xColor = (m_ctx.gizmoHoveredAxis == GizmoAxis::X || m_ctx.gizmoActiveAxis == GizmoAxis::X)
                       ? glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.2f, 0.2f);  // Red / Yellow
    glm::vec3 yColor = (m_ctx.gizmoHoveredAxis == GizmoAxis::Y || m_ctx.gizmoActiveAxis == GizmoAxis::Y)
                       ? glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(0.2f, 1.0f, 0.2f);  // Green / Yellow
    glm::vec3 zColor = (m_ctx.gizmoHoveredAxis == GizmoAxis::Z || m_ctx.gizmoActiveAxis == GizmoAxis::Z)
                       ? glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(0.2f, 0.2f, 1.0f);  // Blue / Yellow

    bool isScaleMode = (m_ctx.gizmoMode == GizmoMode::Scale);
    bool isRotateMode = (m_ctx.gizmoMode == GizmoMode::Rotate);
    float cubeSize = size * 0.12f;  // Size of cube handles for scale gizmo

    // Helper lambda to create cube wireframe lines
    auto makeCubeLines = [](glm::vec3 center, float halfSize) -> std::vector<glm::vec3> {
        float s = halfSize;
        std::vector<glm::vec3> lines;
        // Bottom face
        lines.push_back(center + glm::vec3(-s, -s, -s)); lines.push_back(center + glm::vec3(s, -s, -s));
        lines.push_back(center + glm::vec3(s, -s, -s)); lines.push_back(center + glm::vec3(s, -s, s));
        lines.push_back(center + glm::vec3(s, -s, s)); lines.push_back(center + glm::vec3(-s, -s, s));
        lines.push_back(center + glm::vec3(-s, -s, s)); lines.push_back(center + glm::vec3(-s, -s, -s));
        // Top face
        lines.push_back(center + glm::vec3(-s, s, -s)); lines.push_back(center + glm::vec3(s, s, -s));
        lines.push_back(center + glm::vec3(s, s, -s)); lines.push_back(center + glm::vec3(s, s, s));
        lines.push_back(center + glm::vec3(s, s, s)); lines.push_back(center + glm::vec3(-s, s, s));
        lines.push_back(center + glm::vec3(-s, s, s)); lines.push_back(center + glm::vec3(-s, s, -s));
        // Vertical edges
        lines.push_back(center + glm::vec3(-s, -s, -s)); lines.push_back(center + glm::vec3(-s, s, -s));
        lines.push_back(center + glm::vec3(s, -s, -s)); lines.push_back(center + glm::vec3(s, s, -s));
        lines.push_back(center + glm::vec3(s, -s, s)); lines.push_back(center + glm::vec3(s, s, s));
        lines.push_back(center + glm::vec3(-s, -s, s)); lines.push_back(center + glm::vec3(-s, s, s));
        return lines;
    };

    // Helper lambda to create circle lines around an axis
    auto makeCircleLines = [](glm::vec3 center, float radius, glm::vec3 axis, int segments = 32) -> std::vector<glm::vec3> {
        std::vector<glm::vec3> lines;
        // Find perpendicular axes
        glm::vec3 perp1, perp2;
        if (std::abs(axis.x) < 0.9f) {
            perp1 = glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)));
        } else {
            perp1 = glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
        }
        perp2 = glm::normalize(glm::cross(axis, perp1));

        for (int i = 0; i < segments; ++i) {
            float angle1 = (float)i / segments * 2.0f * 3.14159265f;
            float angle2 = (float)(i + 1) / segments * 2.0f * 3.14159265f;
            glm::vec3 p1 = center + (perp1 * std::cos(angle1) + perp2 * std::sin(angle1)) * radius;
            glm::vec3 p2 = center + (perp1 * std::cos(angle2) + perp2 * std::sin(angle2)) * radius;
            lines.push_back(p1);
            lines.push_back(p2);
        }
        return lines;
    };

    // Get the gizmo axes (local or world space depending on settings)
    glm::vec3 gizmoXAxis, gizmoYAxis, gizmoZAxis;
    getGizmoAxes(gizmoXAxis, gizmoYAxis, gizmoZAxis);

    // Ortho edge move: draw a small sphere instead of arrows
    if (isOrthoEdgeMoveMode()) {
        float orthoSize = m_ctx.getActiveCamera().getOrthoSize();
        float sphereRadius = orthoSize * 0.02f;
        bool hovered = (m_ctx.gizmoHoveredAxis == GizmoAxis::OrthoFree || m_ctx.gizmoActiveAxis == GizmoAxis::OrthoFree);
        glm::vec3 sphereColor = hovered ? glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 1.0f, 1.0f);

        // Draw 3 circles to form a wireframe sphere
        auto makeCircle = [](glm::vec3 center, float radius, glm::vec3 axis, int segments = 16) -> std::vector<glm::vec3> {
            std::vector<glm::vec3> lines;
            glm::vec3 perp1, perp2;
            if (std::abs(axis.x) < 0.9f)
                perp1 = glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)));
            else
                perp1 = glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
            perp2 = glm::normalize(glm::cross(axis, perp1));
            for (int i = 0; i < segments; ++i) {
                float a1 = (float)i / segments * 6.28318f;
                float a2 = (float)(i + 1) / segments * 6.28318f;
                lines.push_back(center + (perp1 * std::cos(a1) + perp2 * std::sin(a1)) * radius);
                lines.push_back(center + (perp1 * std::cos(a2) + perp2 * std::sin(a2)) * radius);
            }
            return lines;
        };

        auto c1 = makeCircle(gizmoPos, sphereRadius, glm::vec3(1, 0, 0));
        auto c2 = makeCircle(gizmoPos, sphereRadius, glm::vec3(0, 1, 0));
        auto c3 = makeCircle(gizmoPos, sphereRadius, glm::vec3(0, 0, 1));
        m_ctx.modelRenderer.renderLines(cmd, viewProj, c1, sphereColor);
        m_ctx.modelRenderer.renderLines(cmd, viewProj, c2, sphereColor);
        m_ctx.modelRenderer.renderLines(cmd, viewProj, c3, sphereColor);
        return;
    }

    if (isRotateMode) {
        // ROTATE MODE: Draw circles around each axis
        auto xCircle = makeCircleLines(gizmoPos, size * 0.9f, glm::vec3(1, 0, 0));
        m_ctx.modelRenderer.renderLines(cmd, viewProj, xCircle, xColor);

        auto yCircle = makeCircleLines(gizmoPos, size * 0.9f, glm::vec3(0, 1, 0));
        m_ctx.modelRenderer.renderLines(cmd, viewProj, yCircle, yColor);

        auto zCircle = makeCircleLines(gizmoPos, size * 0.9f, glm::vec3(0, 0, 1));
        m_ctx.modelRenderer.renderLines(cmd, viewProj, zCircle, zColor);
    } else {
        // MOVE or SCALE MODE: Draw axis lines with arrows or cubes

        // Helper to get perpendicular vectors for arrow heads
        auto getArrowPerps = [](const glm::vec3& axis) -> std::pair<glm::vec3, glm::vec3> {
            glm::vec3 up = (std::abs(axis.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            glm::vec3 perp1 = glm::normalize(glm::cross(axis, up));
            glm::vec3 perp2 = glm::normalize(glm::cross(axis, perp1));
            return {perp1, perp2};
        };

        // Draw X axis (red)
        glm::vec3 xEnd = gizmoPos + gizmoXAxis * size;
        std::vector<glm::vec3> xLines = { gizmoPos, xEnd };
        if (isScaleMode) {
            auto cubeLines = makeCubeLines(xEnd, cubeSize);
            xLines.insert(xLines.end(), cubeLines.begin(), cubeLines.end());
        } else {
            // Arrow head for move mode
            auto [xPerp1, xPerp2] = getArrowPerps(gizmoXAxis);
            glm::vec3 arrowBase = gizmoPos + gizmoXAxis * (size * 0.85f);
            xLines.push_back(xEnd); xLines.push_back(arrowBase + xPerp1 * (size * 0.1f));
            xLines.push_back(xEnd); xLines.push_back(arrowBase - xPerp1 * (size * 0.1f));
            xLines.push_back(xEnd); xLines.push_back(arrowBase + xPerp2 * (size * 0.1f));
            xLines.push_back(xEnd); xLines.push_back(arrowBase - xPerp2 * (size * 0.1f));
        }
        m_ctx.modelRenderer.renderLines(cmd, viewProj, xLines, xColor);

        // Draw Y axis (green)
        glm::vec3 yEnd = gizmoPos + gizmoYAxis * size;
        std::vector<glm::vec3> yLines = { gizmoPos, yEnd };
        if (isScaleMode) {
            auto cubeLines = makeCubeLines(yEnd, cubeSize);
            yLines.insert(yLines.end(), cubeLines.begin(), cubeLines.end());
        } else {
            auto [yPerp1, yPerp2] = getArrowPerps(gizmoYAxis);
            glm::vec3 arrowBase = gizmoPos + gizmoYAxis * (size * 0.85f);
            yLines.push_back(yEnd); yLines.push_back(arrowBase + yPerp1 * (size * 0.1f));
            yLines.push_back(yEnd); yLines.push_back(arrowBase - yPerp1 * (size * 0.1f));
            yLines.push_back(yEnd); yLines.push_back(arrowBase + yPerp2 * (size * 0.1f));
            yLines.push_back(yEnd); yLines.push_back(arrowBase - yPerp2 * (size * 0.1f));
        }
        m_ctx.modelRenderer.renderLines(cmd, viewProj, yLines, yColor);

        // Draw Z axis (blue)
        glm::vec3 zEnd = gizmoPos + gizmoZAxis * size;
        std::vector<glm::vec3> zLines = { gizmoPos, zEnd };
        if (isScaleMode) {
            auto cubeLines = makeCubeLines(zEnd, cubeSize);
            zLines.insert(zLines.end(), cubeLines.begin(), cubeLines.end());
        } else {
            auto [zPerp1, zPerp2] = getArrowPerps(gizmoZAxis);
            glm::vec3 arrowBase = gizmoPos + gizmoZAxis * (size * 0.85f);
            zLines.push_back(zEnd); zLines.push_back(arrowBase + zPerp1 * (size * 0.1f));
            zLines.push_back(zEnd); zLines.push_back(arrowBase - zPerp1 * (size * 0.1f));
            zLines.push_back(zEnd); zLines.push_back(arrowBase + zPerp2 * (size * 0.1f));
            zLines.push_back(zEnd); zLines.push_back(arrowBase - zPerp2 * (size * 0.1f));
        }
        m_ctx.modelRenderer.renderLines(cmd, viewProj, zLines, zColor);

        // Draw center cube for uniform scale (only in scale mode)
        if (isScaleMode) {
            glm::vec3 uniformColor = (m_ctx.gizmoHoveredAxis == GizmoAxis::Uniform || m_ctx.gizmoActiveAxis == GizmoAxis::Uniform)
                                     ? glm::vec3(1.0f, 1.0f, 0.0f) : glm::vec3(0.9f, 0.9f, 0.9f);  // White / Yellow
            auto centerCubeLines = makeCubeLines(gizmoPos, cubeSize * 1.2f);
            m_ctx.modelRenderer.renderLines(cmd, viewProj, centerCubeLines, uniformColor);
        }
    }
}
