#include <eden/Input.hpp>
#include <GLFW/glfw3.h>
#include <cstring>

namespace eden {

GLFWwindow* Input::s_window = nullptr;
bool Input::s_keys[512] = {};
bool Input::s_keysPressed[512] = {};
bool Input::s_mouseButtons[8] = {};
bool Input::s_mouseButtonsPressed[8] = {};
glm::vec2 Input::s_mousePos = {0, 0};
glm::vec2 Input::s_lastMousePos = {0, 0};
glm::vec2 Input::s_mouseDelta = {0, 0};
float Input::s_scrollDelta = 0.0f;
bool Input::s_mouseCaptured = false;
bool Input::s_firstMouse = true;
std::string Input::s_typedChars;

void Input::init(GLFWwindow* window) {
    s_window = window;
    std::memset(s_keys, 0, sizeof(s_keys));
    std::memset(s_keysPressed, 0, sizeof(s_keysPressed));
    std::memset(s_mouseButtons, 0, sizeof(s_mouseButtons));
    std::memset(s_mouseButtonsPressed, 0, sizeof(s_mouseButtonsPressed));

    glfwSetKeyCallback(window, keyCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetCharCallback(window, charCallback);

    // Get initial mouse position
    double x, y;
    glfwGetCursorPos(window, &x, &y);
    s_mousePos = {static_cast<float>(x), static_cast<float>(y)};
    s_lastMousePos = s_mousePos;
}

void Input::update() {
    // Clear per-frame state
    std::memset(s_keysPressed, 0, sizeof(s_keysPressed));
    std::memset(s_mouseButtonsPressed, 0, sizeof(s_mouseButtonsPressed));
    s_mouseDelta = {0, 0};
    s_scrollDelta = 0.0f;

    // Calculate mouse delta
    if (s_mouseCaptured && !s_firstMouse) {
        s_mouseDelta = s_mousePos - s_lastMousePos;
    }
    s_lastMousePos = s_mousePos;
    s_firstMouse = false;
}

bool Input::isKeyDown(int key) {
    if (key >= 0 && key < 512) {
        return s_keys[key];
    }
    return false;
}

bool Input::isKeyPressed(int key) {
    if (key >= 0 && key < 512) {
        return s_keysPressed[key];
    }
    return false;
}

bool Input::isMouseButtonDown(int button) {
    if (button >= 0 && button < 8) {
        return s_mouseButtons[button];
    }
    return false;
}

bool Input::isMouseButtonPressed(int button) {
    if (button >= 0 && button < 8) {
        return s_mouseButtonsPressed[button];
    }
    return false;
}

glm::vec2 Input::getMousePosition() {
    return s_mousePos;
}

glm::vec2 Input::getMouseDelta() {
    return s_mouseDelta;
}

float Input::getScrollDelta() {
    return s_scrollDelta;
}

void Input::setMouseCaptured(bool captured) {
    s_mouseCaptured = captured;
    if (s_window) {
        glfwSetInputMode(s_window, GLFW_CURSOR,
                         captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
    s_firstMouse = true;
}

bool Input::isMouseCaptured() {
    return s_mouseCaptured;
}

void Input::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key >= 0 && key < 512) {
        if (action == GLFW_PRESS) {
            s_keys[key] = true;
            s_keysPressed[key] = true;
        } else if (action == GLFW_RELEASE) {
            s_keys[key] = false;
        }
    }
}

void Input::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    s_mousePos = {static_cast<float>(xpos), static_cast<float>(ypos)};
}

void Input::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if (button >= 0 && button < 8) {
        if (action == GLFW_PRESS) {
            s_mouseButtons[button] = true;
            s_mouseButtonsPressed[button] = true;
        } else if (action == GLFW_RELEASE) {
            s_mouseButtons[button] = false;
        }
    }
}

void Input::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    s_scrollDelta = static_cast<float>(yoffset);
}

void Input::charCallback(GLFWwindow* window, unsigned int codepoint) {
    // Only handle ASCII printable characters for simplicity
    if (codepoint >= 32 && codepoint < 127) {
        s_typedChars += static_cast<char>(codepoint);
    }
}

const std::string& Input::getTypedChars() {
    return s_typedChars;
}

void Input::clearTypedChars() {
    s_typedChars.clear();
}

} // namespace eden
