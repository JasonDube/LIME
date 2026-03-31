#include "SceneObject.hpp"
#include "Renderer/ModelRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace eden {

// Static signal callback
SceneObject::SignalCallback SceneObject::s_signalCallback = nullptr;

float AABB::intersect(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const {
    // Slab method for ray-AABB intersection
    float tmin = -INFINITY;
    float tmax = INFINITY;

    for (int i = 0; i < 3; i++) {
        if (std::abs(rayDir[i]) < 1e-8f) {
            // Ray is parallel to slab
            if (rayOrigin[i] < min[i] || rayOrigin[i] > max[i]) {
                return -1.0f;
            }
        } else {
            float invD = 1.0f / rayDir[i];
            float t0 = (min[i] - rayOrigin[i]) * invD;
            float t1 = (max[i] - rayOrigin[i]) * invD;

            if (invD < 0.0f) {
                std::swap(t0, t1);
            }

            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);

            if (tmax < tmin) {
                return -1.0f;
            }
        }
    }

    // Return the entry point (or exit if we're inside)
    return tmin >= 0 ? tmin : tmax;
}

SceneObject::SceneObject(const std::string& name)
    : m_name(name)
{
}

AABB SceneObject::getWorldBounds() {
    // Transform the 8 corners of the local AABB and compute new AABB
    glm::mat4 matrix = m_transform.getMatrix();

    glm::vec3 corners[8] = {
        {m_localBounds.min.x, m_localBounds.min.y, m_localBounds.min.z},
        {m_localBounds.max.x, m_localBounds.min.y, m_localBounds.min.z},
        {m_localBounds.min.x, m_localBounds.max.y, m_localBounds.min.z},
        {m_localBounds.max.x, m_localBounds.max.y, m_localBounds.min.z},
        {m_localBounds.min.x, m_localBounds.min.y, m_localBounds.max.z},
        {m_localBounds.max.x, m_localBounds.min.y, m_localBounds.max.z},
        {m_localBounds.min.x, m_localBounds.max.y, m_localBounds.max.z},
        {m_localBounds.max.x, m_localBounds.max.y, m_localBounds.max.z},
    };

    AABB worldBounds;
    worldBounds.min = glm::vec3(INFINITY);
    worldBounds.max = glm::vec3(-INFINITY);

    for (const auto& corner : corners) {
        glm::vec4 transformed = matrix * glm::vec4(corner, 1.0f);
        glm::vec3 point = glm::vec3(transformed);

        worldBounds.min = glm::min(worldBounds.min, point);
        worldBounds.max = glm::max(worldBounds.max, point);
    }

    return worldBounds;
}

void SceneObject::addBehavior(const Behavior& behavior) {
    // Pre-reserve to reduce reallocation risk (pointers/refs into this vector
    // may be held by Grove host functions during script evaluation)
    if (m_behaviors.size() == m_behaviors.capacity()) {
        m_behaviors.reserve(m_behaviors.capacity() + 8);
    }
    m_behaviors.push_back(behavior);
}

void SceneObject::triggerBehavior(TriggerType type) {
    for (size_t i = 0; i < m_behaviors.size(); i++) {
        if (m_behaviors[i].trigger == type && m_behaviors[i].enabled) {
            // Check if this behavior is already playing - don't retrigger
            bool alreadyPlaying = false;
            for (const auto& p : m_behaviorPlayers) {
                if (p.behavior == &m_behaviors[i] && p.isPlaying) {
                    alreadyPlaying = true;
                    break;
                }
            }
            if (alreadyPlaying) continue;

            // Create a player for this behavior
            BehaviorPlayer player;
            player.start(&m_behaviors[i]);

            // Store initial values - startValue for position, endValue for rotation
            player.startValue = m_transform.getPosition();
            // Get current rotation as euler angles for rotation tracking
            player.endValue = glm::degrees(glm::eulerAngles(m_transform.getRotation()));

            m_behaviorPlayers.push_back(player);
        }
    }
}

void SceneObject::triggerBehaviorBySignal(const std::string& signalName) {
    for (size_t i = 0; i < m_behaviors.size(); i++) {
        if (m_behaviors[i].trigger == TriggerType::ON_SIGNAL &&
            m_behaviors[i].triggerParam == signalName &&
            m_behaviors[i].enabled) {

            // Don't retrigger if already active
            if (m_activeBehaviorIndex == static_cast<int>(i)) {
                continue;
            }

            // Use the active behavior system (same as ON_GAMESTART)
            m_activeBehaviorIndex = static_cast<int>(i);
            m_activeActionIndex = 0;

            // Clear any path state
            m_pathWaypoints.clear();
            m_pathComplete = false;

            break;  // Only trigger one behavior per signal
        }
    }
}

void SceneObject::updateBehaviors(float deltaTime) {
    for (auto it = m_behaviorPlayers.begin(); it != m_behaviorPlayers.end(); ) {
        BehaviorPlayer& player = *it;

        if (!player.isPlaying || !player.behavior) {
            it = m_behaviorPlayers.erase(it);
            continue;
        }

        const Behavior* behavior = player.behavior;
        if (player.currentActionIndex >= behavior->actions.size()) {
            it = m_behaviorPlayers.erase(it);
            continue;
        }

        const Action& action = behavior->actions[player.currentActionIndex];

        player.actionTimer += deltaTime;

        // Calculate progress (0 to 1)
        float t = (action.duration > 0.0f) ?
            std::min(player.actionTimer / action.duration, 1.0f) : 1.0f;

        // Apply easing
        switch (action.easing) {
            case Action::Easing::EASE_IN:
                t = t * t;
                break;
            case Action::Easing::EASE_OUT:
                t = 1.0f - (1.0f - t) * (1.0f - t);
                break;
            case Action::Easing::EASE_IN_OUT:
                t = t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
                break;
            default:
                break;
        }

        // Execute action based on type
        // startValue = position tracking, endValue = rotation tracking
        switch (action.type) {
            case ActionType::ROTATE: {
                // Rotate by delta over duration (endValue tracks rotation)
                glm::vec3 currentRot = player.endValue + action.vec3Param * t;
                m_transform.setRotation(currentRot);
                break;
            }
            case ActionType::MOVE: {
                // Move by delta over duration (startValue tracks position)
                glm::vec3 currentPos = player.startValue + action.vec3Param * t;
                m_transform.setPosition(currentPos);
                break;
            }
            case ActionType::WAIT:
                // Just wait, do nothing
                break;
            case ActionType::SET_VISIBLE:
                m_visible = action.boolParam;
                break;
            case ActionType::SEND_SIGNAL:
                // Only send on first frame of action (timer was 0 before deltaTime was added)
                if (player.actionTimer <= deltaTime + 0.001f && s_signalCallback) {
                    std::cout << "SEND_SIGNAL executing: " << action.stringParam << std::endl;
                    // Parse "signalName" or "signalName:targetObject"
                    size_t colonPos = action.stringParam.find(':');
                    if (colonPos != std::string::npos) {
                        std::string signalName = action.stringParam.substr(0, colonPos);
                        std::string targetName = action.stringParam.substr(colonPos + 1);
                        s_signalCallback(signalName, targetName, this);
                    } else {
                        // Broadcast (empty target)
                        s_signalCallback(action.stringParam, "", this);
                    }
                }
                break;
            default:
                break;
        }

        // Check if action is complete
        if (player.actionTimer >= action.duration) {
            // Update tracking values for next action
            if (action.type == ActionType::MOVE) {
                player.startValue = m_transform.getPosition();
            } else if (action.type == ActionType::ROTATE) {
                // Add the completed rotation delta to our tracked rotation
                player.endValue = player.endValue + action.vec3Param;
            }

            player.currentActionIndex++;
            player.actionTimer = 0.0f;

            if (player.currentActionIndex >= behavior->actions.size()) {
                if (behavior->loop) {
                    player.currentActionIndex = 0;
                    // Reset tracking for loop
                    player.startValue = m_transform.getPosition();
                    player.endValue = glm::degrees(glm::eulerAngles(m_transform.getRotation()));
                } else {
                    player.isPlaying = false;
                    player.finished = true;
                }
            }
        }

        ++it;
    }
}

bool SceneObject::isPlayingBehavior() const {
    for (const auto& player : m_behaviorPlayers) {
        if (player.isPlaying) return true;
    }
    return false;
}

void SceneObject::setMeshData(const std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices) {
    m_vertices = vertices;
    m_indices = indices;
}

void SceneObject::setTextureData(const std::vector<unsigned char>& data, int width, int height) {
    m_textureData = data;
    m_textureWidth = width;
    m_textureHeight = height;
    m_textureModified = false;
}

// Expression texture system
void SceneObject::addExpression(const std::string& name, const std::vector<unsigned char>& pixels, int w, int h) {
    m_expressions.push_back({name, pixels, w, h});
}

static const std::string s_emptyExpressionName;

const std::string& SceneObject::getExpressionName(int index) const {
    if (index < 0 || index >= static_cast<int>(m_expressions.size())) return s_emptyExpressionName;
    return m_expressions[index].name;
}

bool SceneObject::setExpression(int index) {
    if (index == m_currentExpression) return false;
    if (index < -1 || index >= static_cast<int>(m_expressions.size())) return false;

    if (index == -1) {
        // Revert to base texture — caller would need to reload base, but we don't store it separately.
        // For now, -1 just clears the expression tracking (base texture should already be in m_textureData).
        m_currentExpression = -1;
        return false;
    }

    const auto& expr = m_expressions[index];
    setTextureData(expr.pixels, expr.width, expr.height);
    m_currentExpression = index;
    return true;
}

bool SceneObject::setExpressionByName(const std::string& name) {
    for (int i = 0; i < static_cast<int>(m_expressions.size()); i++) {
        if (m_expressions[i].name == name) {
            return setExpression(i);
        }
    }
    return false;
}

const SceneObject::ExpressionTexture* SceneObject::getExpression(int index) const {
    if (index < 0 || index >= static_cast<int>(m_expressions.size())) return nullptr;
    return &m_expressions[index];
}

SceneObject::RayHit SceneObject::raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const {
    RayHit result;
    if (m_vertices.empty() || m_indices.empty()) {
        return result;
    }

    // Transform ray to local space
    glm::mat4 modelMatrix = m_transform.getMatrix();
    glm::mat4 invModel = glm::inverse(modelMatrix);
    glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
    glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

    float closestT = std::numeric_limits<float>::max();

    // Test all triangles
    for (size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        // Use indices directly from the stored mesh data
        uint32_t idx0 = m_indices[i];
        uint32_t idx1 = m_indices[i + 1];
        uint32_t idx2 = m_indices[i + 2];

        const glm::vec3& v0 = m_vertices[idx0].position;
        const glm::vec3& v1 = m_vertices[idx1].position;
        const glm::vec3& v2 = m_vertices[idx2].position;

        // Moller-Trumbore intersection
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 h = glm::cross(localDir, edge2);
        float a = glm::dot(edge1, h);

        if (std::abs(a) < 1e-8f) continue;  // Parallel

        float f = 1.0f / a;
        glm::vec3 s = localOrigin - v0;
        float u = f * glm::dot(s, h);

        if (u < 0.0f || u > 1.0f) continue;

        glm::vec3 q = glm::cross(s, edge1);
        float v = f * glm::dot(localDir, q);

        if (v < 0.0f || u + v > 1.0f) continue;

        float t = f * glm::dot(edge2, q);

        if (t > 0.001f && t < closestT) {
            closestT = t;
            result.hit = true;
            result.distance = t;
            result.triangleIndex = static_cast<uint32_t>(i / 3);

            // Barycentric coordinates
            float w = 1.0f - u - v;

            // Interpolate UV
            const glm::vec2& uv0 = m_vertices[m_indices[i]].texCoord;
            const glm::vec2& uv1 = m_vertices[m_indices[i + 1]].texCoord;
            const glm::vec2& uv2 = m_vertices[m_indices[i + 2]].texCoord;
            result.uv = w * uv0 + u * uv1 + v * uv2;

            // Hit position in local space, convert to world
            glm::vec3 localHit = localOrigin + localDir * t;
            result.position = glm::vec3(modelMatrix * glm::vec4(localHit, 1.0f));

            // Interpolate normal
            const glm::vec3& n0 = m_vertices[m_indices[i]].normal;
            const glm::vec3& n1 = m_vertices[m_indices[i + 1]].normal;
            const glm::vec3& n2 = m_vertices[m_indices[i + 2]].normal;
            glm::vec3 localNormal = glm::normalize(w * n0 + u * n1 + v * n2);
            result.normal = glm::normalize(glm::vec3(glm::transpose(invModel) * glm::vec4(localNormal, 0.0f)));
        }
    }

    return result;
}

void SceneObject::generateBoxUVs() {
    if (m_vertices.empty() || m_indices.empty()) {
        return;
    }

    // Calculate bounding box
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (const auto& v : m_vertices) {
        minBounds = glm::min(minBounds, v.position);
        maxBounds = glm::max(maxBounds, v.position);
    }

    glm::vec3 size = maxBounds - minBounds;
    glm::vec3 center = (minBounds + maxBounds) * 0.5f;

    // Avoid division by zero
    if (size.x < 0.001f) size.x = 1.0f;
    if (size.y < 0.001f) size.y = 1.0f;
    if (size.z < 0.001f) size.z = 1.0f;

    // Group triangles by dominant normal direction
    // Layout: top row [+X, -X, +Y], bottom row [-Y, +Z, -Z]
    // Each cell is 1/3 width, 1/2 height

    for (size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        uint32_t i0 = m_indices[i];
        uint32_t i1 = m_indices[i + 1];
        uint32_t i2 = m_indices[i + 2];

        // Calculate face normal
        glm::vec3 v0 = m_vertices[i0].position;
        glm::vec3 v1 = m_vertices[i1].position;
        glm::vec3 v2 = m_vertices[i2].position;

        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));

        // Find dominant axis
        float absX = std::abs(normal.x);
        float absY = std::abs(normal.y);
        float absZ = std::abs(normal.z);

        int cellX, cellY;  // Which cell in the 3x2 grid
        glm::vec2 uv0, uv1, uv2;

        if (absX >= absY && absX >= absZ) {
            // Project onto YZ plane
            if (normal.x > 0) { cellX = 0; cellY = 0; }  // +X
            else { cellX = 1; cellY = 0; }               // -X

            uv0 = glm::vec2((v0.z - minBounds.z) / size.z, (v0.y - minBounds.y) / size.y);
            uv1 = glm::vec2((v1.z - minBounds.z) / size.z, (v1.y - minBounds.y) / size.y);
            uv2 = glm::vec2((v2.z - minBounds.z) / size.z, (v2.y - minBounds.y) / size.y);
        } else if (absY >= absX && absY >= absZ) {
            // Project onto XZ plane
            if (normal.y > 0) { cellX = 2; cellY = 0; }  // +Y
            else { cellX = 0; cellY = 1; }               // -Y

            uv0 = glm::vec2((v0.x - minBounds.x) / size.x, (v0.z - minBounds.z) / size.z);
            uv1 = glm::vec2((v1.x - minBounds.x) / size.x, (v1.z - minBounds.z) / size.z);
            uv2 = glm::vec2((v2.x - minBounds.x) / size.x, (v2.z - minBounds.z) / size.z);
        } else {
            // Project onto XY plane
            if (normal.z > 0) { cellX = 1; cellY = 1; }  // +Z
            else { cellX = 2; cellY = 1; }               // -Z

            uv0 = glm::vec2((v0.x - minBounds.x) / size.x, (v0.y - minBounds.y) / size.y);
            uv1 = glm::vec2((v1.x - minBounds.x) / size.x, (v1.y - minBounds.y) / size.y);
            uv2 = glm::vec2((v2.x - minBounds.x) / size.x, (v2.y - minBounds.y) / size.y);
        }

        // Scale and offset to fit in cell (with small margin)
        // Place UVs to the RIGHT of 0-1 space (starting at U=1.1) as placeholder/reference UVs
        float uvSpaceOffset = 1.1f;  // Offset to place outside 0-1 range
        float margin = 0.02f;
        float cellW = (1.0f - margin * 4) / 3.0f;
        float cellH = (1.0f - margin * 3) / 2.0f;

        float offsetX = uvSpaceOffset + margin + cellX * (cellW + margin);
        float offsetY = margin + cellY * (cellH + margin);

        uv0 = glm::vec2(offsetX + uv0.x * cellW, offsetY + uv0.y * cellH);
        uv1 = glm::vec2(offsetX + uv1.x * cellW, offsetY + uv1.y * cellH);
        uv2 = glm::vec2(offsetX + uv2.x * cellW, offsetY + uv2.y * cellH);

        // Update vertex UVs
        m_vertices[i0].texCoord = uv0;
        m_vertices[i1].texCoord = uv1;
        m_vertices[i2].texCoord = uv2;
    }

    // Create a fresh white texture for painting
    int texSize = 1024;
    m_textureData.assign(texSize * texSize * 4, 255);
    m_textureWidth = texSize;
    m_textureHeight = texSize;
    m_textureModified = true;
}

void SceneObject::generateUniformSquareUVs() {
    if (m_vertices.empty() || m_indices.empty()) {
        return;
    }

    // Count faces (assuming triangles, pair them into quads where possible)
    size_t numTriangles = m_indices.size() / 3;

    // For simplicity, treat each pair of triangles as a quad
    // If odd number, last triangle gets its own square
    size_t numFaces = (numTriangles + 1) / 2;  // Round up

    // Calculate grid size to fit all faces
    int gridSize = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(numFaces))));
    if (gridSize < 1) gridSize = 1;

    // Each face gets a square cell in the UV grid
    float cellSize = 1.0f / gridSize;
    float margin = cellSize * 0.02f;  // Small margin between cells
    float innerSize = cellSize - margin * 2;

    // Process triangles in pairs (quads) or singles
    size_t faceIndex = 0;
    for (size_t i = 0; i + 2 < m_indices.size(); i += 6) {  // Step by 6 = 2 triangles
        // Calculate cell position in grid
        int cellX = faceIndex % gridSize;
        int cellY = faceIndex / gridSize;

        float baseU = cellX * cellSize + margin;
        float baseV = cellY * cellSize + margin;

        // First triangle of the quad
        uint32_t i0 = m_indices[i];
        uint32_t i1 = m_indices[i + 1];
        uint32_t i2 = m_indices[i + 2];

        // Calculate face normal for consistent orientation
        glm::vec3 v0 = m_vertices[i0].position;
        glm::vec3 v1 = m_vertices[i1].position;
        glm::vec3 v2 = m_vertices[i2].position;
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));

        // Determine projection axes based on face normal
        glm::vec3 uAxis, vAxis;
        float absX = std::abs(normal.x);
        float absY = std::abs(normal.y);
        float absZ = std::abs(normal.z);

        if (absY >= absX && absY >= absZ) {
            // Face is mostly horizontal (floor/ceiling) - project onto XZ
            uAxis = glm::vec3(1, 0, 0);
            vAxis = glm::vec3(0, 0, 1);
        } else if (absX >= absZ) {
            // Face is mostly facing X - project onto YZ
            uAxis = glm::vec3(0, 0, 1);
            vAxis = glm::vec3(0, 1, 0);
        } else {
            // Face is mostly facing Z - project onto XY
            uAxis = glm::vec3(1, 0, 0);
            vAxis = glm::vec3(0, 1, 0);
        }

        // Find bounds of this quad in projection space
        glm::vec3 positions[6];
        positions[0] = v0;
        positions[1] = v1;
        positions[2] = v2;

        int vertCount = 3;
        if (i + 5 < m_indices.size()) {
            // Second triangle exists
            positions[3] = m_vertices[m_indices[i + 3]].position;
            positions[4] = m_vertices[m_indices[i + 4]].position;
            positions[5] = m_vertices[m_indices[i + 5]].position;
            vertCount = 6;
        }

        float minU = std::numeric_limits<float>::max();
        float maxU = std::numeric_limits<float>::lowest();
        float minV = std::numeric_limits<float>::max();
        float maxV = std::numeric_limits<float>::lowest();

        for (int vi = 0; vi < vertCount; vi++) {
            float u = glm::dot(positions[vi], uAxis);
            float v = glm::dot(positions[vi], vAxis);
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }

        float rangeU = maxU - minU;
        float rangeV = maxV - minV;
        if (rangeU < 0.0001f) rangeU = 1.0f;
        if (rangeV < 0.0001f) rangeV = 1.0f;

        // Map first triangle vertices to uniform square
        for (int ti = 0; ti < 3; ti++) {
            glm::vec3 pos = m_vertices[m_indices[i + ti]].position;
            float u = (glm::dot(pos, uAxis) - minU) / rangeU;
            float v = (glm::dot(pos, vAxis) - minV) / rangeV;
            m_vertices[m_indices[i + ti]].texCoord = glm::vec2(baseU + u * innerSize, baseV + v * innerSize);
        }

        // Map second triangle if it exists
        if (i + 5 < m_indices.size()) {
            for (int ti = 3; ti < 6; ti++) {
                glm::vec3 pos = m_vertices[m_indices[i + ti]].position;
                float u = (glm::dot(pos, uAxis) - minU) / rangeU;
                float v = (glm::dot(pos, vAxis) - minV) / rangeV;
                m_vertices[m_indices[i + ti]].texCoord = glm::vec2(baseU + u * innerSize, baseV + v * innerSize);
            }
        }

        faceIndex++;
    }

    // Handle any remaining single triangle (odd count)
    size_t processed = (numFaces - (numTriangles % 2 == 1 ? 1 : 0)) * 6;
    if (processed < m_indices.size() && numTriangles % 2 == 1) {
        size_t i = m_indices.size() - 3;
        int cellX = faceIndex % gridSize;
        int cellY = faceIndex / gridSize;
        float baseU = cellX * cellSize + margin;
        float baseV = cellY * cellSize + margin;

        // Simple mapping for lone triangle
        m_vertices[m_indices[i]].texCoord = glm::vec2(baseU, baseV);
        m_vertices[m_indices[i + 1]].texCoord = glm::vec2(baseU + innerSize, baseV);
        m_vertices[m_indices[i + 2]].texCoord = glm::vec2(baseU + innerSize * 0.5f, baseV + innerSize);
    }

    // Create a fresh white texture for painting
    int texSize = 1024;
    m_textureData.assign(texSize * texSize * 4, 255);
    m_textureWidth = texSize;
    m_textureHeight = texSize;
    m_textureModified = true;
}

void SceneObject::paintAt(const glm::vec2& uv, const glm::vec3& color, float radius, float strength, bool squareBrush) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return;
    }

    // Properly wrap UV to 0-1 range using fract (handles negative and >1 values)
    float uvX = uv.x - std::floor(uv.x);
    float uvY = uv.y - std::floor(uv.y);

    // Convert UV to pixel coordinates
    int centerX = static_cast<int>(uvX * m_textureWidth);
    int centerY = static_cast<int>(uvY * m_textureHeight);

    // Clamp to valid range
    centerX = std::clamp(centerX, 0, m_textureWidth - 1);
    centerY = std::clamp(centerY, 0, m_textureHeight - 1);

    // Radius in pixels (allow 0 for single-pixel painting)
    int pixelRadius = static_cast<int>(radius * std::max(m_textureWidth, m_textureHeight));

    // Paint in square or circular area
    for (int dy = -pixelRadius; dy <= pixelRadius; dy++) {
        for (int dx = -pixelRadius; dx <= pixelRadius; dx++) {
            float alpha;

            if (squareBrush) {
                // Square brush: no distance check, uniform strength (pixel art style)
                alpha = strength;
            } else {
                // Circular brush: distance check with soft falloff
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (dist > pixelRadius) continue;

                // Soft falloff (avoid divide by zero for single pixel)
                float falloff = (pixelRadius > 0) ? (1.0f - (dist / pixelRadius)) : 1.0f;
                alpha = strength * falloff;
            }

            int px = centerX + dx;
            int py = centerY + dy;

            // Skip pixels outside texture bounds (no wrapping for paint strokes)
            if (px < 0 || px >= m_textureWidth || py < 0 || py >= m_textureHeight) continue;

            size_t idx = (py * m_textureWidth + px) * 4;  // RGBA
            if (idx + 3 >= m_textureData.size()) continue;

            // Blend with existing color
            float r = m_textureData[idx] / 255.0f;
            float g = m_textureData[idx + 1] / 255.0f;
            float b = m_textureData[idx + 2] / 255.0f;

            r = r * (1.0f - alpha) + color.r * alpha;
            g = g * (1.0f - alpha) + color.g * alpha;
            b = b * (1.0f - alpha) + color.b * alpha;

            m_textureData[idx] = static_cast<unsigned char>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            m_textureData[idx + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            m_textureData[idx + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            // Blend alpha too — ensures painted pixels become visible in UV editor
            float a = m_textureData[idx + 3] / 255.0f;
            a = a * (1.0f - alpha) + alpha;
            m_textureData[idx + 3] = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        }
    }

    m_textureModified = true;
}

glm::vec3 SceneObject::smearAt(const glm::vec2& uv, const glm::vec3& carriedColor, float radius, float strength, float pickup) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return carriedColor;
    }

    // Wrap UV to 0-1 range
    float uvX = uv.x - std::floor(uv.x);
    float uvY = uv.y - std::floor(uv.y);

    // Convert UV to pixel coordinates
    int centerX = static_cast<int>(uvX * m_textureWidth);
    int centerY = static_cast<int>(uvY * m_textureHeight);

    // Radius in pixels
    int pixelRadius = static_cast<int>(radius * m_textureWidth);
    if (pixelRadius < 1) pixelRadius = 1;

    // Sample the average color at the brush center for carrying forward
    glm::vec3 sampledColor(0.0f);
    int sampleCount = 0;

    // First pass: sample center color
    for (int dy = -pixelRadius/2; dy <= pixelRadius/2; dy++) {
        for (int dx = -pixelRadius/2; dx <= pixelRadius/2; dx++) {
            int px = centerX + dx;
            int py = centerY + dy;

            if (px < 0 || px >= m_textureWidth || py < 0 || py >= m_textureHeight) continue;

            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (dist > pixelRadius/2) continue;

            size_t idx = (py * m_textureWidth + px) * 4;
            if (idx + 3 >= m_textureData.size()) continue;

            sampledColor.r += m_textureData[idx] / 255.0f;
            sampledColor.g += m_textureData[idx + 1] / 255.0f;
            sampledColor.b += m_textureData[idx + 2] / 255.0f;
            sampleCount++;
        }
    }

    if (sampleCount > 0) {
        sampledColor /= static_cast<float>(sampleCount);
    } else {
        sampledColor = carriedColor;
    }

    // Second pass: apply smear (blend carried color with existing)
    for (int dy = -pixelRadius; dy <= pixelRadius; dy++) {
        for (int dx = -pixelRadius; dx <= pixelRadius; dx++) {
            int px = centerX + dx;
            int py = centerY + dy;

            if (px < 0 || px >= m_textureWidth || py < 0 || py >= m_textureHeight) continue;

            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (dist > pixelRadius) continue;

            // Falloff based on distance from center
            float falloff = 1.0f - (dist / pixelRadius);
            float alpha = strength * falloff;

            size_t idx = (py * m_textureWidth + px) * 4;
            if (idx + 3 >= m_textureData.size()) continue;

            // Get existing color
            float r = m_textureData[idx] / 255.0f;
            float g = m_textureData[idx + 1] / 255.0f;
            float b = m_textureData[idx + 2] / 255.0f;

            // Blend carried color into existing
            r = r * (1.0f - alpha) + carriedColor.r * alpha;
            g = g * (1.0f - alpha) + carriedColor.g * alpha;
            b = b * (1.0f - alpha) + carriedColor.b * alpha;

            m_textureData[idx] = static_cast<unsigned char>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            m_textureData[idx + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            m_textureData[idx + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
        }
    }

    m_textureModified = true;

    // Return blend of sampled and carried color for next stroke point
    return glm::mix(carriedColor, sampledColor, pickup);  // Pick up some new color
}

void SceneObject::saveTextureState() {
    if (m_textureData.empty()) return;

    // Save a copy of the current texture state
    m_textureUndoStack.push_back(m_textureData);

    // Limit stack size
    if (m_textureUndoStack.size() > MAX_TEXTURE_UNDO_LEVELS) {
        m_textureUndoStack.erase(m_textureUndoStack.begin());
    }
}

bool SceneObject::undoTexture() {
    if (m_textureUndoStack.empty()) return false;

    // Restore the previous texture state
    m_textureData = m_textureUndoStack.back();
    m_textureUndoStack.pop_back();
    m_textureModified = true;  // Mark for GPU upload

    return true;
}

void SceneObject::stampAt(const glm::vec2& uv, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation, float opacity, bool flipH, bool flipV) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return;
    }
    if (!stampData || stampWidth <= 0 || stampHeight <= 0) {
        return;
    }

    // Wrap UV to 0-1 range
    float uvX = uv.x - std::floor(uv.x);
    float uvY = uv.y - std::floor(uv.y);

    // Convert UV to pixel coordinates (center of stamp)
    int centerX = static_cast<int>(uvX * m_textureWidth);
    int centerY = static_cast<int>(uvY * m_textureHeight);

    // Calculate scaled stamp size in texture pixels (separate H and V scales)
    // Use the smaller texture dimension to maintain aspect ratio
    float texScale = static_cast<float>(std::min(m_textureWidth, m_textureHeight));
    float scaledStampW = stampWidth * scaleH * texScale / 256.0f;
    float scaledStampH = stampHeight * scaleV * texScale / 256.0f;
    if (scaledStampW < 1) scaledStampW = 1;
    if (scaledStampH < 1) scaledStampH = 1;

    // Rotation in radians
    float radians = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    // Calculate rotated bounding box size
    float halfW = scaledStampW * 0.5f;
    float halfH = scaledStampH * 0.5f;
    float rotatedHalfW = std::abs(halfW * cosR) + std::abs(halfH * sinR);
    float rotatedHalfH = std::abs(halfW * sinR) + std::abs(halfH * cosR);
    int boundW = static_cast<int>(std::ceil(rotatedHalfW * 2));
    int boundH = static_cast<int>(std::ceil(rotatedHalfH * 2));

    // Stamp bounds in texture space (expanded for rotation)
    int startX = centerX - boundW / 2;
    int startY = centerY - boundH / 2;

    // Copy stamp pixels with alpha blending
    for (int dy = 0; dy < boundH; dy++) {
        for (int dx = 0; dx < boundW; dx++) {
            int texX = startX + dx;
            int texY = startY + dy;

            // Skip pixels outside texture bounds
            if (texX < 0 || texX >= m_textureWidth || texY < 0 || texY >= m_textureHeight) continue;

            // Position relative to stamp center
            float relX = dx - boundW * 0.5f;
            float relY = dy - boundH * 0.5f;

            // Apply inverse rotation to find source position in unrotated stamp
            float srcRelX = relX * cosR + relY * sinR;
            float srcRelY = -relX * sinR + relY * cosR;

            // Convert to stamp coordinates (0 to scaledStamp size)
            float stampCoordX = srcRelX + scaledStampW * 0.5f;
            float stampCoordY = srcRelY + scaledStampH * 0.5f;

            // Skip if outside stamp bounds
            if (stampCoordX < 0 || stampCoordX >= scaledStampW || stampCoordY < 0 || stampCoordY >= scaledStampH) continue;

            // Apply flip transformations
            if (flipH) stampCoordX = scaledStampW - 1 - stampCoordX;
            if (flipV) stampCoordY = scaledStampH - 1 - stampCoordY;

            // Map to original stamp pixel coordinates (floating point for bilinear)
            float srcX = stampCoordX * stampWidth / scaledStampW;
            float srcY = stampCoordY * stampHeight / scaledStampH;

            // Bilinear interpolation
            int x0 = static_cast<int>(std::floor(srcX));
            int y0 = static_cast<int>(std::floor(srcY));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            // Clamp to valid range
            x0 = std::clamp(x0, 0, stampWidth - 1);
            y0 = std::clamp(y0, 0, stampHeight - 1);
            x1 = std::clamp(x1, 0, stampWidth - 1);
            y1 = std::clamp(y1, 0, stampHeight - 1);

            // Fractional parts for interpolation
            float fx = srcX - std::floor(srcX);
            float fy = srcY - std::floor(srcY);

            // Sample 4 corners
            size_t idx00 = (y0 * stampWidth + x0) * 4;
            size_t idx10 = (y0 * stampWidth + x1) * 4;
            size_t idx01 = (y1 * stampWidth + x0) * 4;
            size_t idx11 = (y1 * stampWidth + x1) * 4;

            // Bilinear interpolation for each channel
            float r00 = stampData[idx00] / 255.0f, r10 = stampData[idx10] / 255.0f;
            float r01 = stampData[idx01] / 255.0f, r11 = stampData[idx11] / 255.0f;
            float g00 = stampData[idx00 + 1] / 255.0f, g10 = stampData[idx10 + 1] / 255.0f;
            float g01 = stampData[idx01 + 1] / 255.0f, g11 = stampData[idx11 + 1] / 255.0f;
            float b00 = stampData[idx00 + 2] / 255.0f, b10 = stampData[idx10 + 2] / 255.0f;
            float b01 = stampData[idx01 + 2] / 255.0f, b11 = stampData[idx11 + 2] / 255.0f;
            float a00 = stampData[idx00 + 3] / 255.0f, a10 = stampData[idx10 + 3] / 255.0f;
            float a01 = stampData[idx01 + 3] / 255.0f, a11 = stampData[idx11 + 3] / 255.0f;

            // Interpolate
            float stampR = (r00 * (1 - fx) + r10 * fx) * (1 - fy) + (r01 * (1 - fx) + r11 * fx) * fy;
            float stampG = (g00 * (1 - fx) + g10 * fx) * (1 - fy) + (g01 * (1 - fx) + g11 * fx) * fy;
            float stampB = (b00 * (1 - fx) + b10 * fx) * (1 - fy) + (b01 * (1 - fx) + b11 * fx) * fy;
            float stampA = ((a00 * (1 - fx) + a10 * fx) * (1 - fy) + (a01 * (1 - fx) + a11 * fx) * fy) * opacity;

            // Skip fully transparent pixels
            if (stampA < 0.01f) continue;

            // Texture pixel index
            size_t texIdx = (texY * m_textureWidth + texX) * 4;
            if (texIdx + 3 >= m_textureData.size()) continue;

            // Alpha blend
            float texR = m_textureData[texIdx] / 255.0f;
            float texG = m_textureData[texIdx + 1] / 255.0f;
            float texB = m_textureData[texIdx + 2] / 255.0f;

            float outR = texR * (1.0f - stampA) + stampR * stampA;
            float outG = texG * (1.0f - stampA) + stampG * stampA;
            float outB = texB * (1.0f - stampA) + stampB * stampA;

            m_textureData[texIdx] = static_cast<unsigned char>(std::clamp(outR, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 1] = static_cast<unsigned char>(std::clamp(outG, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 2] = static_cast<unsigned char>(std::clamp(outB, 0.0f, 1.0f) * 255.0f);
        }
    }

    m_textureModified = true;
}

glm::vec2 SceneObject::getUVDensityScale(uint32_t triangleIndex) const {
    // Returns scale factors to correct for UV stretching along U and V axes
    // Computes the Jacobian of the UV-to-3D mapping to find how much 3D distance
    // corresponds to a unit movement in UV-U vs UV-V directions

    if (m_vertices.empty() || m_indices.empty()) {
        return glm::vec2(1.0f);
    }

    uint32_t baseIdx = triangleIndex * 3;
    if (baseIdx + 2 >= m_indices.size()) {
        return glm::vec2(1.0f);
    }

    uint32_t i0 = m_indices[baseIdx];
    uint32_t i1 = m_indices[baseIdx + 1];
    uint32_t i2 = m_indices[baseIdx + 2];

    if (i0 >= m_vertices.size() || i1 >= m_vertices.size() || i2 >= m_vertices.size()) {
        return glm::vec2(1.0f);
    }

    const auto& v0 = m_vertices[i0];
    const auto& v1 = m_vertices[i1];
    const auto& v2 = m_vertices[i2];

    // Calculate 3D edge vectors from v0
    glm::vec3 dP1 = v1.position - v0.position;
    glm::vec3 dP2 = v2.position - v0.position;

    // Calculate UV edge vectors from v0
    glm::vec2 dUV1 = v1.texCoord - v0.texCoord;
    glm::vec2 dUV2 = v2.texCoord - v0.texCoord;

    // Compute determinant of the UV matrix (for inverting)
    // | du1  du2 |
    // | dv1  dv2 |
    float det = dUV1.x * dUV2.y - dUV2.x * dUV1.y;

    if (std::abs(det) < 0.0000001f) {
        return glm::vec2(1.0f);  // Degenerate UV mapping
    }

    // Compute dP/dU and dP/dV using the inverse of the UV matrix
    // dP/dU = (dP1 * dv2 - dP2 * dv1) / det
    // dP/dV = (-dP1 * du2 + dP2 * du1) / det
    glm::vec3 dPdU = (dP1 * dUV2.y - dP2 * dUV1.y) / det;
    glm::vec3 dPdV = (-dP1 * dUV2.x + dP2 * dUV1.x) / det;

    // The lengths tell us: 3D units per UV unit in each direction
    float lenU = glm::length(dPdU);  // 3D distance per unit U
    float lenV = glm::length(dPdV);  // 3D distance per unit V

    if (lenU < 0.0001f || lenV < 0.0001f) {
        return glm::vec2(1.0f);
    }

    // Normalize so average is 1.0
    // If lenU > lenV, the surface is stretched in U direction (needs smaller stamp in U)
    float avgLen = (lenU + lenV) * 0.5f;

    // Return the ratio - higher value means more 3D distance per UV, so stamp appears smaller
    // We want to scale the stamp inversely to compensate
    return glm::vec2(lenU / avgLen, lenV / avgLen);
}

glm::vec3 SceneObject::getUVCorrection(uint32_t triangleIndex) const {
    // Returns full correction: x = scaleU, y = scaleV, z = rotation in degrees
    // This version ignores UV layout and computes correction purely from 3D geometry
    // to make stamps appear uniform regardless of UV mapping

    if (m_vertices.empty() || m_indices.empty()) {
        return glm::vec3(1.0f, 1.0f, 0.0f);
    }

    uint32_t baseIdx = triangleIndex * 3;
    if (baseIdx + 2 >= m_indices.size()) {
        return glm::vec3(1.0f, 1.0f, 0.0f);
    }

    uint32_t i0 = m_indices[baseIdx];
    uint32_t i1 = m_indices[baseIdx + 1];
    uint32_t i2 = m_indices[baseIdx + 2];

    if (i0 >= m_vertices.size() || i1 >= m_vertices.size() || i2 >= m_vertices.size()) {
        return glm::vec3(1.0f, 1.0f, 0.0f);
    }

    const auto& v0 = m_vertices[i0];
    const auto& v1 = m_vertices[i1];
    const auto& v2 = m_vertices[i2];

    // Calculate 3D edge vectors from v0
    glm::vec3 dP1 = v1.position - v0.position;
    glm::vec3 dP2 = v2.position - v0.position;

    // Calculate UV edge vectors from v0
    glm::vec2 dUV1 = v1.texCoord - v0.texCoord;
    glm::vec2 dUV2 = v2.texCoord - v0.texCoord;

    // Compute determinant of the UV matrix (for inverting)
    float det = dUV1.x * dUV2.y - dUV2.x * dUV1.y;

    if (std::abs(det) < 0.0000001f) {
        return glm::vec3(1.0f, 1.0f, 0.0f);  // Degenerate UV mapping
    }

    // Compute dP/dU and dP/dV (tangent and bitangent in 3D)
    glm::vec3 dPdU = (dP1 * dUV2.y - dP2 * dUV1.y) / det;
    glm::vec3 dPdV = (-dP1 * dUV2.x + dP2 * dUV1.x) / det;

    float lenU = glm::length(dPdU);
    float lenV = glm::length(dPdV);

    if (lenU < 0.0001f || lenV < 0.0001f) {
        return glm::vec3(1.0f, 1.0f, 0.0f);
    }

    // Face normal
    glm::vec3 normal = glm::normalize(glm::cross(dP1, dP2));

    // === SCALE CORRECTION ===
    // Compute 3D area vs UV area to get overall texel density
    float area3D = glm::length(glm::cross(dP1, dP2)) * 0.5f;
    float areaUV = std::abs(det) * 0.5f;

    if (area3D < 0.0001f || areaUV < 0.0001f) {
        return glm::vec3(1.0f, 1.0f, 0.0f);
    }

    // For uniform stamps, we want equal world-space size regardless of UV density
    // Compute the aspect ratio of the UV mapping
    float aspectRatio = lenU / lenV;  // How much U is stretched relative to V

    // Scale corrections to make stamp square in world space
    // If aspectRatio > 1, U direction covers more world space per UV, so shrink stamp in U
    float scaleU = std::sqrt(aspectRatio);
    float scaleV = 1.0f / std::sqrt(aspectRatio);

    // === ROTATION CORRECTION ===
    // We want stamp "up" to align with world Y projected onto the surface
    glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    float dotNY = glm::dot(normal, worldUp);

    float rotationDeg = 0.0f;

    // Only rotate if surface isn't horizontal
    if (std::abs(dotNY) < 0.99f) {
        // Project world Y onto tangent plane to get "surface up"
        glm::vec3 surfaceUp = worldUp - dotNY * normal;
        surfaceUp = glm::normalize(surfaceUp);

        // Get normalized UV directions
        glm::vec3 uvU = glm::normalize(dPdU);
        glm::vec3 uvV = glm::normalize(dPdV);

        // Find how "surface up" is expressed in UV coordinates
        // surfaceUp = a * uvU + b * uvV (approximately, on the tangent plane)
        // We want the angle in UV space that corresponds to "up"
        float dotU = glm::dot(surfaceUp, uvU);
        float dotV = glm::dot(surfaceUp, uvV);

        // The angle in UV space (atan2 gives angle from +U axis toward +V axis)
        // We want angle from +V axis (which is "up" in texture), so adjust
        float uvAngle = std::atan2(dotU, dotV);  // Angle from V toward U

        // Convert to degrees - this is how much to rotate stamp so its "up" aligns with surface up
        rotationDeg = -uvAngle * 180.0f / 3.14159265f;
    }

    return glm::vec3(scaleU, scaleV, rotationDeg);
}

void SceneObject::stampAt(const glm::vec2& uv, uint32_t triangleIndex, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation, float opacity, bool flipH, bool flipV) {
    // Scale-only correction (no rotation) - use manual rotation control
    glm::vec2 uvDensity = getUVDensityScale(triangleIndex);

    float correctedScaleH = scaleH / uvDensity.x;
    float correctedScaleV = scaleV / uvDensity.y;

    stampAt(uv, stampData, stampWidth, stampHeight, correctedScaleH, correctedScaleV, rotation, opacity, flipH, flipV);
}

void SceneObject::stampPreviewAt(const glm::vec2& uv, uint32_t triangleIndex, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation, float opacity, bool flipH, bool flipV) {
    // Scale-only correction (no rotation) - use manual rotation control
    glm::vec2 uvDensity = getUVDensityScale(triangleIndex);

    float correctedScaleH = scaleH / uvDensity.x;
    float correctedScaleV = scaleV / uvDensity.y;

    stampPreviewAt(uv, stampData, stampWidth, stampHeight, correctedScaleH, correctedScaleV, rotation, opacity, flipH, flipV);
}

void SceneObject::stampPreviewAt(const glm::vec2& uv, const unsigned char* stampData, int stampWidth, int stampHeight, float scaleH, float scaleV, float rotation, float opacity, bool flipH, bool flipV) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return;
    }
    if (!stampData || stampWidth <= 0 || stampHeight <= 0) {
        return;
    }

    // If we already have a preview, restore the original first
    if (m_hasStampPreview && !m_previewTextureBackup.empty()) {
        m_textureData = m_previewTextureBackup;
    } else {
        // Save backup of current texture for preview
        m_previewTextureBackup = m_textureData;
    }

    // Apply the stamp (same logic as stampAt)
    float uvX = uv.x - std::floor(uv.x);
    float uvY = uv.y - std::floor(uv.y);

    int centerX = static_cast<int>(uvX * m_textureWidth);
    int centerY = static_cast<int>(uvY * m_textureHeight);

    // Calculate scaled stamp size in texture pixels (separate H and V scales)
    // Use the smaller texture dimension to maintain aspect ratio
    float texScale = static_cast<float>(std::min(m_textureWidth, m_textureHeight));
    float scaledStampW = stampWidth * scaleH * texScale / 256.0f;
    float scaledStampH = stampHeight * scaleV * texScale / 256.0f;
    if (scaledStampW < 1) scaledStampW = 1;
    if (scaledStampH < 1) scaledStampH = 1;

    // Rotation in radians
    float radians = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    // Calculate rotated bounding box size
    float halfW = scaledStampW * 0.5f;
    float halfH = scaledStampH * 0.5f;
    float rotatedHalfW = std::abs(halfW * cosR) + std::abs(halfH * sinR);
    float rotatedHalfH = std::abs(halfW * sinR) + std::abs(halfH * cosR);
    int boundW = static_cast<int>(std::ceil(rotatedHalfW * 2));
    int boundH = static_cast<int>(std::ceil(rotatedHalfH * 2));

    int startX = centerX - boundW / 2;
    int startY = centerY - boundH / 2;

    for (int dy = 0; dy < boundH; dy++) {
        for (int dx = 0; dx < boundW; dx++) {
            int texX = startX + dx;
            int texY = startY + dy;

            if (texX < 0 || texX >= m_textureWidth || texY < 0 || texY >= m_textureHeight) continue;

            // Position relative to stamp center
            float relX = dx - boundW * 0.5f;
            float relY = dy - boundH * 0.5f;

            // Apply inverse rotation to find source position in unrotated stamp
            float srcRelX = relX * cosR + relY * sinR;
            float srcRelY = -relX * sinR + relY * cosR;

            // Convert to stamp coordinates (0 to scaledStamp size)
            float stampCoordX = srcRelX + scaledStampW * 0.5f;
            float stampCoordY = srcRelY + scaledStampH * 0.5f;

            // Skip if outside stamp bounds
            if (stampCoordX < 0 || stampCoordX >= scaledStampW || stampCoordY < 0 || stampCoordY >= scaledStampH) continue;

            // Apply flip transformations
            if (flipH) stampCoordX = scaledStampW - 1 - stampCoordX;
            if (flipV) stampCoordY = scaledStampH - 1 - stampCoordY;

            // Map to original stamp pixel coordinates (floating point for bilinear)
            float srcX = stampCoordX * stampWidth / scaledStampW;
            float srcY = stampCoordY * stampHeight / scaledStampH;

            // Bilinear interpolation
            int x0 = static_cast<int>(std::floor(srcX));
            int y0 = static_cast<int>(std::floor(srcY));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            // Clamp to valid range
            x0 = std::clamp(x0, 0, stampWidth - 1);
            y0 = std::clamp(y0, 0, stampHeight - 1);
            x1 = std::clamp(x1, 0, stampWidth - 1);
            y1 = std::clamp(y1, 0, stampHeight - 1);

            // Fractional parts for interpolation
            float fx = srcX - std::floor(srcX);
            float fy = srcY - std::floor(srcY);

            // Sample 4 corners
            size_t idx00 = (y0 * stampWidth + x0) * 4;
            size_t idx10 = (y0 * stampWidth + x1) * 4;
            size_t idx01 = (y1 * stampWidth + x0) * 4;
            size_t idx11 = (y1 * stampWidth + x1) * 4;

            // Bilinear interpolation for each channel
            float r00 = stampData[idx00] / 255.0f, r10 = stampData[idx10] / 255.0f;
            float r01 = stampData[idx01] / 255.0f, r11 = stampData[idx11] / 255.0f;
            float g00 = stampData[idx00 + 1] / 255.0f, g10 = stampData[idx10 + 1] / 255.0f;
            float g01 = stampData[idx01 + 1] / 255.0f, g11 = stampData[idx11 + 1] / 255.0f;
            float b00 = stampData[idx00 + 2] / 255.0f, b10 = stampData[idx10 + 2] / 255.0f;
            float b01 = stampData[idx01 + 2] / 255.0f, b11 = stampData[idx11 + 2] / 255.0f;
            float a00 = stampData[idx00 + 3] / 255.0f, a10 = stampData[idx10 + 3] / 255.0f;
            float a01 = stampData[idx01 + 3] / 255.0f, a11 = stampData[idx11 + 3] / 255.0f;

            // Interpolate
            float stampR = (r00 * (1 - fx) + r10 * fx) * (1 - fy) + (r01 * (1 - fx) + r11 * fx) * fy;
            float stampG = (g00 * (1 - fx) + g10 * fx) * (1 - fy) + (g01 * (1 - fx) + g11 * fx) * fy;
            float stampB = (b00 * (1 - fx) + b10 * fx) * (1 - fy) + (b01 * (1 - fx) + b11 * fx) * fy;
            float stampA = ((a00 * (1 - fx) + a10 * fx) * (1 - fy) + (a01 * (1 - fx) + a11 * fx) * fy) * opacity;

            if (stampA < 0.01f) continue;

            size_t texIdx = (texY * m_textureWidth + texX) * 4;
            if (texIdx + 3 >= m_textureData.size()) continue;

            float texR = m_textureData[texIdx] / 255.0f;
            float texG = m_textureData[texIdx + 1] / 255.0f;
            float texB = m_textureData[texIdx + 2] / 255.0f;

            float outR = texR * (1.0f - stampA) + stampR * stampA;
            float outG = texG * (1.0f - stampA) + stampG * stampA;
            float outB = texB * (1.0f - stampA) + stampB * stampA;

            m_textureData[texIdx] = static_cast<unsigned char>(std::clamp(outR, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 1] = static_cast<unsigned char>(std::clamp(outG, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 2] = static_cast<unsigned char>(std::clamp(outB, 0.0f, 1.0f) * 255.0f);
        }
    }

    m_hasStampPreview = true;
    m_textureModified = true;
}

void SceneObject::clearStampPreview() {
    if (m_hasStampPreview && !m_previewTextureBackup.empty()) {
        m_textureData = m_previewTextureBackup;
        m_previewTextureBackup.clear();
        m_textureModified = true;
    }
    m_hasStampPreview = false;
}

void SceneObject::stampProjectedFromView(const glm::vec3& hitPoint, const glm::vec3& camPos, const glm::vec3& camRight, const glm::vec3& camUp,
                                          const unsigned char* stampData, int stampWidth, int stampHeight,
                                          float worldSizeH, float worldSizeV, float rotation, float opacity, bool flipH, bool flipV) {
    if (m_textureData.empty() || !stampData || stampWidth <= 0 || stampHeight <= 0) return;
    if (m_vertices.empty() || m_indices.empty()) return;

    // Rotation
    float radians = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    // Rotated camera axes define the stamp plane
    glm::vec3 stampRight = camRight * cosR + camUp * sinR;
    glm::vec3 stampUp = -camRight * sinR + camUp * cosR;
    glm::vec3 stampNormal = glm::normalize(glm::cross(stampRight, stampUp));

    float halfSizeH = worldSizeH * 0.5f;
    float halfSizeV = worldSizeV * 0.5f;
    float maxHalfSize = std::max(halfSizeH, halfSizeV);

    // OPTIMIZED: Instead of raycasting, iterate over texture pixels and back-project
    // For each triangle, check if it might be under the stamp, then process its pixels

    glm::mat4 modelMatrix = m_transform.getMatrix();

    for (size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        uint32_t i0 = m_indices[i];
        uint32_t i1 = m_indices[i + 1];
        uint32_t i2 = m_indices[i + 2];

        // Get world positions
        glm::vec3 p0 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i0].position, 1.0f));
        glm::vec3 p1 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i1].position, 1.0f));
        glm::vec3 p2 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i2].position, 1.0f));

        // Quick rejection: check if triangle is roughly in stamp area
        glm::vec3 triCenter = (p0 + p1 + p2) / 3.0f;
        float distToHit = glm::length(triCenter - hitPoint);
        if (distToHit > maxHalfSize * 2.0f) continue;  // Too far from stamp center

        // Skip back-facing triangles (not facing the camera)
        glm::vec3 triNormal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        glm::vec3 viewDir = glm::normalize(camPos - triCenter);
        if (glm::dot(triNormal, viewDir) < 0.0f) continue;  // Back-facing, skip

        // Get UVs
        glm::vec2 uv0 = m_vertices[i0].texCoord;
        glm::vec2 uv1 = m_vertices[i1].texCoord;
        glm::vec2 uv2 = m_vertices[i2].texCoord;

        // Find UV bounding box for this triangle
        glm::vec2 uvMin = glm::min(glm::min(uv0, uv1), uv2);
        glm::vec2 uvMax = glm::max(glm::max(uv0, uv1), uv2);

        // Convert to texture pixel range
        int minTX = static_cast<int>(std::floor(uvMin.x * m_textureWidth));
        int maxTX = static_cast<int>(std::ceil(uvMax.x * m_textureWidth));
        int minTY = static_cast<int>(std::floor(uvMin.y * m_textureHeight));
        int maxTY = static_cast<int>(std::ceil(uvMax.y * m_textureHeight));

        // Clamp to texture bounds
        minTX = std::max(0, minTX);
        maxTX = std::min(m_textureWidth - 1, maxTX);
        minTY = std::max(0, minTY);
        maxTY = std::min(m_textureHeight - 1, maxTY);

        // Process each texture pixel in this triangle's UV range
        for (int ty = minTY; ty <= maxTY; ty++) {
            for (int tx = minTX; tx <= maxTX; tx++) {
                // UV of this pixel center
                float u = (tx + 0.5f) / m_textureWidth;
                float v = (ty + 0.5f) / m_textureHeight;

                // Check if UV is inside this triangle using barycentric coords
                glm::vec2 uv(u, v);
                glm::vec2 v0v = uv - uv0;
                glm::vec2 v01 = uv1 - uv0;
                glm::vec2 v02 = uv2 - uv0;

                float dot00 = glm::dot(v01, v01);
                float dot01 = glm::dot(v01, v02);
                float dot02 = glm::dot(v01, v0v);
                float dot11 = glm::dot(v02, v02);
                float dot12 = glm::dot(v02, v0v);

                float denom = dot00 * dot11 - dot01 * dot01;
                if (std::abs(denom) < 0.000001f) continue;

                float invDenom = 1.0f / denom;
                float bu = (dot11 * dot02 - dot01 * dot12) * invDenom;
                float bv = (dot00 * dot12 - dot01 * dot02) * invDenom;

                // Check if inside triangle
                if (bu < 0 || bv < 0 || bu + bv > 1) continue;

                // Interpolate 3D position
                float bw = 1.0f - bu - bv;
                glm::vec3 worldPos = bw * p0 + bu * p1 + bv * p2;

                // Project onto stamp plane
                glm::vec3 toPoint = worldPos - hitPoint;
                float projRight = glm::dot(toPoint, stampRight);
                float projUp = glm::dot(toPoint, stampUp);

                // Normalize to stamp coordinates (-1 to 1) using separate H/V scaling
                float nx = projRight / halfSizeH;
                float ny = projUp / halfSizeV;

                // Check if within stamp bounds
                if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) continue;

                // Apply flip
                if (flipH) nx = -nx;
                if (flipV) ny = -ny;

                // Sample stamp with bilinear filtering
                float stampU = (nx * 0.5f + 0.5f) * (stampWidth - 1);
                float stampV = (ny * 0.5f + 0.5f) * (stampHeight - 1);

                int sx0 = std::clamp(static_cast<int>(stampU), 0, stampWidth - 1);
                int sy0 = std::clamp(static_cast<int>(stampV), 0, stampHeight - 1);
                int sx1 = std::clamp(sx0 + 1, 0, stampWidth - 1);
                int sy1 = std::clamp(sy0 + 1, 0, stampHeight - 1);
                float fx = stampU - sx0;
                float fy = stampV - sy0;

                auto sampleStamp = [&](int x, int y) -> glm::vec4 {
                    size_t idx = (y * stampWidth + x) * 4;
                    return glm::vec4(stampData[idx]/255.0f, stampData[idx+1]/255.0f,
                                     stampData[idx+2]/255.0f, stampData[idx+3]/255.0f);
                };

                glm::vec4 s00 = sampleStamp(sx0, sy0);
                glm::vec4 s10 = sampleStamp(sx1, sy0);
                glm::vec4 s01 = sampleStamp(sx0, sy1);
                glm::vec4 s11 = sampleStamp(sx1, sy1);

                glm::vec4 stampColor = glm::mix(glm::mix(s00, s10, fx), glm::mix(s01, s11, fx), fy);
                stampColor.a *= opacity;

                if (stampColor.a < 0.01f) continue;

                // Paint
                size_t texIdx = (ty * m_textureWidth + tx) * 4;
                if (texIdx + 3 >= m_textureData.size()) continue;

                float texR = m_textureData[texIdx] / 255.0f;
                float texG = m_textureData[texIdx + 1] / 255.0f;
                float texB = m_textureData[texIdx + 2] / 255.0f;

                m_textureData[texIdx] = static_cast<unsigned char>(std::clamp(texR * (1 - stampColor.a) + stampColor.r * stampColor.a, 0.0f, 1.0f) * 255.0f);
                m_textureData[texIdx + 1] = static_cast<unsigned char>(std::clamp(texG * (1 - stampColor.a) + stampColor.g * stampColor.a, 0.0f, 1.0f) * 255.0f);
                m_textureData[texIdx + 2] = static_cast<unsigned char>(std::clamp(texB * (1 - stampColor.a) + stampColor.b * stampColor.a, 0.0f, 1.0f) * 255.0f);
            }
        }
    }

    m_textureModified = true;
}

void SceneObject::stampProjectedFromViewPreview(const glm::vec3& hitPoint, const glm::vec3& camPos, const glm::vec3& camRight, const glm::vec3& camUp,
                                                 const unsigned char* stampData, int stampWidth, int stampHeight,
                                                 float worldSizeH, float worldSizeV, float rotation, float opacity, bool flipH, bool flipV) {
    if (m_textureData.empty() || !stampData || stampWidth <= 0 || stampHeight <= 0) return;
    if (m_vertices.empty() || m_indices.empty()) return;

    // Restore or save backup
    if (m_hasStampPreview && !m_previewTextureBackup.empty()) {
        m_textureData = m_previewTextureBackup;
    } else {
        m_previewTextureBackup = m_textureData;
    }

    // Same optimized algorithm as final stamp
    float radians = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    glm::vec3 stampRight = camRight * cosR + camUp * sinR;
    glm::vec3 stampUp = -camRight * sinR + camUp * cosR;

    float halfSizeH = worldSizeH * 0.5f;
    float halfSizeV = worldSizeV * 0.5f;
    float maxHalfSize = std::max(halfSizeH, halfSizeV);
    glm::mat4 modelMatrix = m_transform.getMatrix();

    for (size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        uint32_t i0 = m_indices[i];
        uint32_t i1 = m_indices[i + 1];
        uint32_t i2 = m_indices[i + 2];

        glm::vec3 p0 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i0].position, 1.0f));
        glm::vec3 p1 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i1].position, 1.0f));
        glm::vec3 p2 = glm::vec3(modelMatrix * glm::vec4(m_vertices[i2].position, 1.0f));

        glm::vec3 triCenter = (p0 + p1 + p2) / 3.0f;
        float distToHit = glm::length(triCenter - hitPoint);
        if (distToHit > maxHalfSize * 2.0f) continue;

        // Skip back-facing triangles (not facing the camera)
        glm::vec3 triNormal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        glm::vec3 viewDir = glm::normalize(camPos - triCenter);
        if (glm::dot(triNormal, viewDir) < 0.0f) continue;  // Back-facing, skip

        glm::vec2 uv0 = m_vertices[i0].texCoord;
        glm::vec2 uv1 = m_vertices[i1].texCoord;
        glm::vec2 uv2 = m_vertices[i2].texCoord;

        glm::vec2 uvMin = glm::min(glm::min(uv0, uv1), uv2);
        glm::vec2 uvMax = glm::max(glm::max(uv0, uv1), uv2);

        int minTX = std::max(0, static_cast<int>(std::floor(uvMin.x * m_textureWidth)));
        int maxTX = std::min(m_textureWidth - 1, static_cast<int>(std::ceil(uvMax.x * m_textureWidth)));
        int minTY = std::max(0, static_cast<int>(std::floor(uvMin.y * m_textureHeight)));
        int maxTY = std::min(m_textureHeight - 1, static_cast<int>(std::ceil(uvMax.y * m_textureHeight)));

        for (int ty = minTY; ty <= maxTY; ty++) {
            for (int tx = minTX; tx <= maxTX; tx++) {
                float u = (tx + 0.5f) / m_textureWidth;
                float v = (ty + 0.5f) / m_textureHeight;

                glm::vec2 uv(u, v);
                glm::vec2 v0v = uv - uv0;
                glm::vec2 v01 = uv1 - uv0;
                glm::vec2 v02 = uv2 - uv0;

                float dot00 = glm::dot(v01, v01);
                float dot01 = glm::dot(v01, v02);
                float dot02 = glm::dot(v01, v0v);
                float dot11 = glm::dot(v02, v02);
                float dot12 = glm::dot(v02, v0v);

                float denom = dot00 * dot11 - dot01 * dot01;
                if (std::abs(denom) < 0.000001f) continue;

                float invDenom = 1.0f / denom;
                float bu = (dot11 * dot02 - dot01 * dot12) * invDenom;
                float bv = (dot00 * dot12 - dot01 * dot02) * invDenom;

                if (bu < 0 || bv < 0 || bu + bv > 1) continue;

                float bw = 1.0f - bu - bv;
                glm::vec3 worldPos = bw * p0 + bu * p1 + bv * p2;

                glm::vec3 toPoint = worldPos - hitPoint;
                float projRight = glm::dot(toPoint, stampRight);
                float projUp = glm::dot(toPoint, stampUp);

                // Normalize using separate H/V scaling
                float nx = projRight / halfSizeH;
                float ny = projUp / halfSizeV;

                if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) continue;

                if (flipH) nx = -nx;
                if (flipV) ny = -ny;

                float stampU = (nx * 0.5f + 0.5f) * (stampWidth - 1);
                float stampV = (ny * 0.5f + 0.5f) * (stampHeight - 1);

                int sx0 = std::clamp(static_cast<int>(stampU), 0, stampWidth - 1);
                int sy0 = std::clamp(static_cast<int>(stampV), 0, stampHeight - 1);
                int sx1 = std::clamp(sx0 + 1, 0, stampWidth - 1);
                int sy1 = std::clamp(sy0 + 1, 0, stampHeight - 1);
                float fx = stampU - sx0;
                float fy = stampV - sy0;

                auto sampleStamp = [&](int x, int y) -> glm::vec4 {
                    size_t idx = (y * stampWidth + x) * 4;
                    return glm::vec4(stampData[idx]/255.0f, stampData[idx+1]/255.0f,
                                     stampData[idx+2]/255.0f, stampData[idx+3]/255.0f);
                };

                glm::vec4 s00 = sampleStamp(sx0, sy0);
                glm::vec4 s10 = sampleStamp(sx1, sy0);
                glm::vec4 s01 = sampleStamp(sx0, sy1);
                glm::vec4 s11 = sampleStamp(sx1, sy1);

                glm::vec4 stampColor = glm::mix(glm::mix(s00, s10, fx), glm::mix(s01, s11, fx), fy);
                stampColor.a *= opacity;

                if (stampColor.a < 0.01f) continue;

                size_t texIdx = (ty * m_textureWidth + tx) * 4;
                if (texIdx + 3 >= m_textureData.size()) continue;

                float texR = m_textureData[texIdx] / 255.0f;
                float texG = m_textureData[texIdx + 1] / 255.0f;
                float texB = m_textureData[texIdx + 2] / 255.0f;

                m_textureData[texIdx] = static_cast<unsigned char>(std::clamp(texR * (1 - stampColor.a) + stampColor.r * stampColor.a, 0.0f, 1.0f) * 255.0f);
                m_textureData[texIdx + 1] = static_cast<unsigned char>(std::clamp(texG * (1 - stampColor.a) + stampColor.g * stampColor.a, 0.0f, 1.0f) * 255.0f);
                m_textureData[texIdx + 2] = static_cast<unsigned char>(std::clamp(texB * (1 - stampColor.a) + stampColor.b * stampColor.a, 0.0f, 1.0f) * 255.0f);
            }
        }
    }

    m_hasStampPreview = true;
    m_textureModified = true;
}

void SceneObject::stampToQuad(const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2, const glm::vec2& uv3,
                              const unsigned char* stampData, int stampWidth, int stampHeight, float opacity) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return;
    }
    if (!stampData || stampWidth <= 0 || stampHeight <= 0) {
        return;
    }

    // uv0 = bottom-left (stamp 0,0), uv1 = bottom-right (stamp 1,0)
    // uv2 = top-right (stamp 1,1), uv3 = top-left (stamp 0,1)

    // Find bounding box in texture space
    float minU = std::min({uv0.x, uv1.x, uv2.x, uv3.x});
    float maxU = std::max({uv0.x, uv1.x, uv2.x, uv3.x});
    float minV = std::min({uv0.y, uv1.y, uv2.y, uv3.y});
    float maxV = std::max({uv0.y, uv1.y, uv2.y, uv3.y});

    // Convert to pixel coordinates
    int startX = static_cast<int>(std::floor(minU * m_textureWidth));
    int endX = static_cast<int>(std::ceil(maxU * m_textureWidth));
    int startY = static_cast<int>(std::floor(minV * m_textureHeight));
    int endY = static_cast<int>(std::ceil(maxV * m_textureHeight));

    // Clamp to texture bounds
    startX = std::max(0, startX);
    endX = std::min(m_textureWidth, endX);
    startY = std::max(0, startY);
    endY = std::min(m_textureHeight, endY);

    // For each pixel in the bounding box, use inverse bilinear interpolation
    // to find if it's inside the quad and where in the stamp it maps to
    for (int texY = startY; texY < endY; ++texY) {
        for (int texX = startX; texX < endX; ++texX) {
            // Convert pixel to UV
            float u = (texX + 0.5f) / m_textureWidth;
            float v = (texY + 0.5f) / m_textureHeight;
            glm::vec2 p(u, v);

            // Inverse bilinear interpolation to find (s, t) where:
            // p = (1-s)*(1-t)*uv0 + s*(1-t)*uv1 + s*t*uv2 + (1-s)*t*uv3
            // This is a quadrilateral mapping where s,t in [0,1] means inside the quad

            // Solve using iterative Newton's method or direct formula
            // Using the direct formula for bilinear inverse:
            glm::vec2 e = uv1 - uv0;  // bottom edge direction
            glm::vec2 f = uv3 - uv0;  // left edge direction
            glm::vec2 g = uv0 - uv1 + uv2 - uv3;  // non-parallelism term
            glm::vec2 h = p - uv0;

            float k2 = g.x * f.y - g.y * f.x;
            float k1 = e.x * f.y - e.y * f.x + h.x * g.y - h.y * g.x;
            float k0 = h.x * e.y - h.y * e.x;

            float s, t;

            // Solve quadratic for t: k2*t^2 + k1*t + k0 = 0
            if (std::abs(k2) < 1e-6f) {
                // Linear case (parallelogram)
                if (std::abs(k1) < 1e-6f) continue;  // Degenerate
                t = -k0 / k1;
            } else {
                float disc = k1 * k1 - 4.0f * k2 * k0;
                if (disc < 0) continue;  // No solution

                float sqrtDisc = std::sqrt(disc);
                float t1 = (-k1 + sqrtDisc) / (2.0f * k2);
                float t2 = (-k1 - sqrtDisc) / (2.0f * k2);

                // Pick the solution in [0,1] range
                if (t1 >= -0.001f && t1 <= 1.001f) t = t1;
                else if (t2 >= -0.001f && t2 <= 1.001f) t = t2;
                else continue;
            }

            // Now find s
            glm::vec2 denom = e + g * t;
            if (std::abs(denom.x) > std::abs(denom.y)) {
                if (std::abs(denom.x) < 1e-6f) continue;
                s = (h.x - f.x * t) / denom.x;
            } else {
                if (std::abs(denom.y) < 1e-6f) continue;
                s = (h.y - f.y * t) / denom.y;
            }

            // Check if inside quad (with small tolerance)
            if (s < -0.001f || s > 1.001f || t < -0.001f || t > 1.001f) continue;

            // Clamp to valid range
            s = std::clamp(s, 0.0f, 1.0f);
            t = std::clamp(t, 0.0f, 1.0f);

            // Map (s, t) to stamp coordinates
            float stampX = s * (stampWidth - 1);
            float stampY = t * (stampHeight - 1);

            // Bilinear sample from stamp
            int x0 = static_cast<int>(std::floor(stampX));
            int y0 = static_cast<int>(std::floor(stampY));
            int x1 = std::min(x0 + 1, stampWidth - 1);
            int y1 = std::min(y0 + 1, stampHeight - 1);

            float fx = stampX - x0;
            float fy = stampY - y0;

            size_t idx00 = (y0 * stampWidth + x0) * 4;
            size_t idx10 = (y0 * stampWidth + x1) * 4;
            size_t idx01 = (y1 * stampWidth + x0) * 4;
            size_t idx11 = (y1 * stampWidth + x1) * 4;

            // Bilinear interpolation
            float r = (1-fx)*(1-fy)*stampData[idx00] + fx*(1-fy)*stampData[idx10] +
                      (1-fx)*fy*stampData[idx01] + fx*fy*stampData[idx11];
            float g_val = (1-fx)*(1-fy)*stampData[idx00+1] + fx*(1-fy)*stampData[idx10+1] +
                      (1-fx)*fy*stampData[idx01+1] + fx*fy*stampData[idx11+1];
            float b = (1-fx)*(1-fy)*stampData[idx00+2] + fx*(1-fy)*stampData[idx10+2] +
                      (1-fx)*fy*stampData[idx01+2] + fx*fy*stampData[idx11+2];
            float a = (1-fx)*(1-fy)*stampData[idx00+3] + fx*(1-fy)*stampData[idx10+3] +
                      (1-fx)*fy*stampData[idx01+3] + fx*fy*stampData[idx11+3];

            // Apply opacity
            float stampAlpha = (a / 255.0f) * opacity;
            if (stampAlpha < 0.001f) continue;

            // Blend with texture
            size_t texIdx = (texY * m_textureWidth + texX) * 4;
            if (texIdx + 3 >= m_textureData.size()) continue;

            float texR = m_textureData[texIdx] / 255.0f;
            float texG = m_textureData[texIdx + 1] / 255.0f;
            float texB = m_textureData[texIdx + 2] / 255.0f;

            m_textureData[texIdx] = static_cast<unsigned char>(std::clamp(texR * (1 - stampAlpha) + (r/255.0f) * stampAlpha, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 1] = static_cast<unsigned char>(std::clamp(texG * (1 - stampAlpha) + (g_val/255.0f) * stampAlpha, 0.0f, 1.0f) * 255.0f);
            m_textureData[texIdx + 2] = static_cast<unsigned char>(std::clamp(texB * (1 - stampAlpha) + (b/255.0f) * stampAlpha, 0.0f, 1.0f) * 255.0f);
        }
    }

    m_textureModified = true;
}

void SceneObject::applySeamBuster(int pixels) {
    if (m_textureData.empty() || m_textureWidth <= 0 || m_textureHeight <= 0) {
        return;
    }
    if (pixels < 1) pixels = 1;

    // Create coverage mask by rasterizing UV triangles
    std::vector<bool> baseMask(m_textureWidth * m_textureHeight, false);

    // Rasterize each triangle to mark base UV coverage
    size_t numTriangles = m_indices.size() / 3;
    for (size_t t = 0; t < numTriangles; ++t) {
        uint32_t i0 = m_indices[t * 3 + 0];
        uint32_t i1 = m_indices[t * 3 + 1];
        uint32_t i2 = m_indices[t * 3 + 2];

        if (i0 >= m_vertices.size() || i1 >= m_vertices.size() || i2 >= m_vertices.size()) continue;

        glm::vec2 uv0 = m_vertices[i0].texCoord;
        glm::vec2 uv1 = m_vertices[i1].texCoord;
        glm::vec2 uv2 = m_vertices[i2].texCoord;

        // Convert to pixel coordinates
        glm::vec2 p0(uv0.x * m_textureWidth, uv0.y * m_textureHeight);
        glm::vec2 p1(uv1.x * m_textureWidth, uv1.y * m_textureHeight);
        glm::vec2 p2(uv2.x * m_textureWidth, uv2.y * m_textureHeight);

        // Bounding box
        int minX = static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x})));
        int maxX = static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x})));
        int minY = static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y})));
        int maxY = static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y})));

        minX = std::max(0, minX);
        maxX = std::min(m_textureWidth - 1, maxX);
        minY = std::max(0, minY);
        maxY = std::min(m_textureHeight - 1, maxY);

        // Rasterize using barycentric coordinates
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                glm::vec2 p(x + 0.5f, y + 0.5f);

                glm::vec2 v0 = p2 - p0;
                glm::vec2 v1 = p1 - p0;
                glm::vec2 v2 = p - p0;

                float dot00 = glm::dot(v0, v0);
                float dot01 = glm::dot(v0, v1);
                float dot02 = glm::dot(v0, v2);
                float dot11 = glm::dot(v1, v1);
                float dot12 = glm::dot(v1, v2);

                float invDenom = dot00 * dot11 - dot01 * dot01;
                if (std::abs(invDenom) < 1e-10f) continue;
                invDenom = 1.0f / invDenom;

                float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
                float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

                if (u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f) {
                    baseMask[y * m_textureWidth + x] = true;
                }
            }
        }
    }

    // Working mask starts from base coverage
    std::vector<bool> insideMask = baseMask;

    // Dilate the mask and sample colors for each iteration
    for (int iter = 0; iter < pixels; ++iter) {
        std::vector<std::pair<int, glm::vec4>> pixelsToFill;

        // Find outside pixels adjacent to inside pixels
        for (int y = 0; y < m_textureHeight; ++y) {
            for (int x = 0; x < m_textureWidth; ++x) {
                int idx = y * m_textureWidth + x;
                if (insideMask[idx]) continue;  // Already inside

                // Check 8 neighbors for inside pixels
                glm::vec4 colorSum(0.0f);
                int neighborCount = 0;

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx < 0 || nx >= m_textureWidth || ny < 0 || ny >= m_textureHeight) continue;

                        int nidx = ny * m_textureWidth + nx;
                        if (insideMask[nidx]) {
                            // Sample this neighbor's color
                            size_t texIdx = nidx * 4;
                            if (texIdx + 3 < m_textureData.size()) {
                                colorSum.r += m_textureData[texIdx] / 255.0f;
                                colorSum.g += m_textureData[texIdx + 1] / 255.0f;
                                colorSum.b += m_textureData[texIdx + 2] / 255.0f;
                                colorSum.a += m_textureData[texIdx + 3] / 255.0f;
                                neighborCount++;
                            }
                        }
                    }
                }

                if (neighborCount > 0) {
                    // Average the neighbor colors
                    colorSum /= static_cast<float>(neighborCount);
                    pixelsToFill.push_back({idx, colorSum});
                }
            }
        }

        // Apply the fill
        for (const auto& [idx, color] : pixelsToFill) {
            size_t texIdx = idx * 4;
            if (texIdx + 3 < m_textureData.size()) {
                m_textureData[texIdx] = static_cast<unsigned char>(color.r * 255.0f);
                m_textureData[texIdx + 1] = static_cast<unsigned char>(color.g * 255.0f);
                m_textureData[texIdx + 2] = static_cast<unsigned char>(color.b * 255.0f);
                m_textureData[texIdx + 3] = static_cast<unsigned char>(color.a * 255.0f);
                insideMask[idx] = true;  // Mark as inside for next iteration
            }
        }
    }

    m_textureModified = true;
    std::cout << "Seam Buster applied: " << pixels << " pixel(s) of edge padding" << std::endl;
}

} // namespace eden
