#include <eden/Entity.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace eden {

Entity::Entity(uint32_t id, const std::string& name)
    : m_id(id), m_name(name)
{
}

void Entity::addBehavior(const Behavior& behavior) {
    m_behaviors.push_back(behavior);
    m_behaviorPlayers.emplace_back();  // Add corresponding player
}

void Entity::removeBehavior(const std::string& name) {
    for (size_t i = 0; i < m_behaviors.size(); ++i) {
        if (m_behaviors[i].name == name) {
            m_behaviors.erase(m_behaviors.begin() + i);
            m_behaviorPlayers.erase(m_behaviorPlayers.begin() + i);
            return;
        }
    }
}

float Entity::getProperty(const std::string& key, float defaultVal) const {
    auto it = m_properties.find(key);
    return (it != m_properties.end()) ? it->second : defaultVal;
}

bool Entity::hasTag(const std::string& tag) const {
    return std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end();
}

} // namespace eden
