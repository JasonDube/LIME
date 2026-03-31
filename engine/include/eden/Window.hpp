#pragma once

#include <string>
#include <functional>

// Forward declare GLFW types to avoid exposing them in public header
struct GLFWwindow;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
typedef struct VkInstance_T* VkInstance;

namespace eden {

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void close();
    void pollEvents();

    GLFWwindow* getHandle() const { return m_window; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    bool wasResized() const { return m_framebufferResized; }
    void resetResizedFlag() { m_framebufferResized = false; }

    VkSurfaceKHR createSurface(VkInstance instance);

    using ResizeCallback = std::function<void(int, int)>;
    void setResizeCallback(ResizeCallback callback) { m_resizeCallback = callback; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    int m_width;
    int m_height;
    bool m_framebufferResized = false;
    ResizeCallback m_resizeCallback;
};

} // namespace eden
