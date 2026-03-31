#pragma once

#include <vulkan/vulkan.h>
#include <string>

struct GLFWwindow;

namespace eden {

class VulkanContext;
class Swapchain;

/**
 * Manages ImGui initialization and cleanup for Vulkan-based applications.
 * Handles descriptor pool creation, ImGui context setup, and resource cleanup.
 */
class ImGuiManager {
public:
    ImGuiManager() = default;
    ~ImGuiManager();

    // Non-copyable
    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    // Move semantics
    ImGuiManager(ImGuiManager&& other) noexcept;
    ImGuiManager& operator=(ImGuiManager&& other) noexcept;

    /**
     * Initialize ImGui for Vulkan rendering.
     * @param context The Vulkan context (device, queues, etc.)
     * @param swapchain The swapchain (render pass, image count)
     * @param window The GLFW window handle
     * @param iniFilename The INI file for saving ImGui layout (e.g., "imgui_editor.ini")
     */
    void init(VulkanContext& context, Swapchain& swapchain, GLFWwindow* window,
              const std::string& iniFilename = "imgui.ini");

    /**
     * Cleanup ImGui resources. Called automatically by destructor.
     */
    void cleanup();

    /**
     * Check if ImGui has been initialized.
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * Get the descriptor pool used by ImGui.
     */
    VkDescriptorPool getDescriptorPool() const { return m_descriptorPool; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_initialized = false;
    std::string m_iniFilename;
};

} // namespace eden
