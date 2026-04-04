#pragma once

#include <glm/glm.hpp>
#include <string>

struct GLFWwindow;

namespace eden {

class Input {
public:
    static void init(GLFWwindow* window);
    static void update();

    static bool isKeyDown(int key);
    static bool isKeyPressed(int key);  // Just pressed this frame

    static bool isMouseButtonDown(int button);
    static bool isMouseButtonPressed(int button);  // Just pressed this frame

    static glm::vec2 getMousePosition();
    static glm::vec2 getMouseDelta();
    static float getScrollDelta();  // Scroll wheel delta this frame

    static void setMouseCaptured(bool captured);
    static bool isMouseCaptured();

    // Text input (for Minecraft-style chat)
    static const std::string& getTypedChars();  // Characters typed this frame
    static void clearTypedChars();

    // Mouse button codes
    static constexpr int MOUSE_LEFT = 0;
    static constexpr int MOUSE_RIGHT = 1;
    static constexpr int MOUSE_MIDDLE = 2;

    // Common key codes (matches GLFW)
    static constexpr int KEY_W = 87;
    static constexpr int KEY_A = 65;
    static constexpr int KEY_B = 66;
    static constexpr int KEY_C = 67;
    static constexpr int KEY_S = 83;
    static constexpr int KEY_D = 68;
    static constexpr int KEY_E = 69;
    static constexpr int KEY_F = 70;
    static constexpr int KEY_G = 71;
    static constexpr int KEY_I = 73;
    static constexpr int KEY_J = 74;
    static constexpr int KEY_R = 82;
    static constexpr int KEY_N = 78;
    static constexpr int KEY_Y = 89;
    static constexpr int KEY_O = 79;
    static constexpr int KEY_P = 80;
    static constexpr int KEY_Q = 81;
    static constexpr int KEY_K = 75;
    static constexpr int KEY_L = 76;
    static constexpr int KEY_M = 77;
    static constexpr int KEY_V = 86;
    static constexpr int KEY_X = 88;
    static constexpr int KEY_U = 85;
    static constexpr int KEY_T = 84;
    static constexpr int KEY_Z = 90;
    static constexpr int KEY_SPACE = 32;

    // Arrow keys
    static constexpr int KEY_RIGHT = 262;
    static constexpr int KEY_LEFT = 263;
    static constexpr int KEY_DOWN = 264;
    static constexpr int KEY_UP = 265;
    static constexpr int KEY_LEFT_CONTROL = 341;
    static constexpr int KEY_RIGHT_CONTROL = 345;
    static constexpr int KEY_LEFT_SHIFT = 340;
    static constexpr int KEY_RIGHT_SHIFT = 344;
    static constexpr int KEY_LEFT_ALT = 342;
    static constexpr int KEY_RIGHT_ALT = 346;
    static constexpr int KEY_ESCAPE = 256;
    static constexpr int KEY_ENTER = 257;
    static constexpr int KEY_TAB = 258;
    static constexpr int KEY_BACKSPACE = 259;
    static constexpr int KEY_DELETE = 261;
    static constexpr int KEY_HOME = 268;
    static constexpr int KEY_F1 = 290;
    static constexpr int KEY_F2 = 291;
    static constexpr int KEY_F3 = 292;
    static constexpr int KEY_F5 = 294;

    // Number keys (for weapon slots, toolbar, etc.)
    static constexpr int KEY_0 = 48;
    static constexpr int KEY_1 = 49;
    static constexpr int KEY_2 = 50;
    static constexpr int KEY_3 = 51;
    static constexpr int KEY_4 = 52;
    static constexpr int KEY_5 = 53;
    static constexpr int KEY_6 = 54;
    static constexpr int KEY_7 = 55;
    static constexpr int KEY_8 = 56;
    static constexpr int KEY_9 = 57;

    // Punctuation keys
    static constexpr int KEY_PERIOD = 46;
    static constexpr int KEY_COMMA = 44;
    static constexpr int KEY_MINUS = 45;
    static constexpr int KEY_EQUAL = 61;  // Plus with shift
    static constexpr int KEY_SLASH = 47;  // Forward slash /

private:
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void charCallback(GLFWwindow* window, unsigned int codepoint);

    static GLFWwindow* s_window;
    static bool s_keys[512];
    static bool s_keysPressed[512];
    static bool s_mouseButtons[8];
    static bool s_mouseButtonsPressed[8];
    static glm::vec2 s_mousePos;
    static glm::vec2 s_lastMousePos;
    static glm::vec2 s_mouseDelta;
    static float s_scrollDelta;
    static bool s_mouseCaptured;
    static bool s_firstMouse;
    static std::string s_typedChars;
};

} // namespace eden
