#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace eden {

class Transform {
public:
    Transform() = default;

    void setPosition(const glm::vec3& pos) { m_position = pos; m_dirty = true; }
    void setPosition(float x, float y, float z = 0.0f) { setPosition({x, y, z}); }

    void setRotation(const glm::vec3& eulerDegrees) {
        m_rotation = glm::quat(glm::radians(eulerDegrees));
        m_dirty = true;
    }
    void setRotation(float degrees, const glm::vec3& axis = {0, 0, 1}) {
        m_rotation = glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
        m_dirty = true;
    }
    void setRotation(const glm::quat& quat) {
        m_rotation = quat;
        m_dirty = true;
    }
    void setRotationIdentity() {
        m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // w, x, y, z - identity
        m_dirty = true;
    }

    void setScale(const glm::vec3& scale) { m_scale = scale; m_dirty = true; }
    void setScale(float uniform) { setScale({uniform, uniform, uniform}); }

    void translate(const glm::vec3& delta) { m_position += delta; m_dirty = true; }
    void translate(float x, float y, float z = 0.0f) { translate({x, y, z}); }

    void rotate(float degrees, const glm::vec3& axis = {0, 0, 1}) {
        m_rotation = glm::angleAxis(glm::radians(degrees), glm::normalize(axis)) * m_rotation;
        m_dirty = true;
    }

    void scale(const glm::vec3& factor) { m_scale *= factor; m_dirty = true; }
    void scale(float uniform) { scale({uniform, uniform, uniform}); }

    const glm::vec3& getPosition() const { return m_position; }
    const glm::quat& getRotation() const { return m_rotation; }
    const glm::vec3& getScale() const { return m_scale; }

    const glm::mat4& getMatrix() const {
        if (m_dirty) {
            m_matrix = glm::translate(glm::mat4(1.0f), m_position)
                     * glm::mat4_cast(m_rotation)
                     * glm::scale(glm::mat4(1.0f), m_scale);
            m_dirty = false;
        }
        return m_matrix;
    }

private:
    glm::vec3 m_position{0.0f};
    glm::quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_scale{1.0f};
    mutable glm::mat4 m_matrix{1.0f};  // mutable for lazy evaluation in const getMatrix()
    mutable bool m_dirty = true;
};

} // namespace eden
