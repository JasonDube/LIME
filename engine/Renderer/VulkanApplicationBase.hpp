#pragma once

#include <eden/Window.hpp>
#include <eden/Input.hpp>
#include "VulkanContext.hpp"
#include "Swapchain.hpp"
#include "Buffer.hpp"

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <chrono>
#include <functional>

namespace eden {

/**
 * Base class for Vulkan applications.
 * Handles common boilerplate: window creation, Vulkan context, swapchain,
 * command buffers, sync objects, and the main render loop.
 *
 * Derived classes implement:
 *   - onInit(): Application-specific initialization (renderers, UI, etc.)
 *   - onCleanup(): Application-specific cleanup
 *   - update(deltaTime): Per-frame logic
 *   - recordCommandBuffer(): Record rendering commands
 *   - onSwapchainRecreated(): Handle swapchain resize
 */
class VulkanApplicationBase {
public:
    VulkanApplicationBase(int width, int height, const char* title);
    virtual ~VulkanApplicationBase();

    // Non-copyable
    VulkanApplicationBase(const VulkanApplicationBase&) = delete;
    VulkanApplicationBase& operator=(const VulkanApplicationBase&) = delete;

    // Main entry point
    void run();

protected:
    // Override these in derived classes
    virtual void onInit() = 0;
    virtual void onCleanup() = 0;
    virtual void onBeforeMainLoop() {}  // Called before main loop starts (for preloading)
    virtual void update(float deltaTime) = 0;
    virtual void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) = 0;
    virtual void onSwapchainRecreated() {}

    // Frame rendering helpers
    bool beginFrame(uint32_t& imageIndex);
    void endFrame(uint32_t imageIndex);

    // Swapchain recreation
    void recreateSwapchain();

    // Accessors for derived classes
    Window& getWindow() { return *m_window; }
    const Window& getWindow() const { return *m_window; }
    VulkanContext& getContext() { return *m_context; }
    const VulkanContext& getContext() const { return *m_context; }
    Swapchain& getSwapchain() { return *m_swapchain; }
    const Swapchain& getSwapchain() const { return *m_swapchain; }
    BufferManager& getBufferManager() { return *m_bufferManager; }
    const BufferManager& getBufferManager() const { return *m_bufferManager; }

    VkCommandBuffer getCurrentCommandBuffer() const { return m_commandBuffers[m_currentFrame]; }
    uint32_t getCurrentFrame() const { return m_currentFrame; }

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

private:
    void init();
    void mainLoop();
    void cleanup();

    void createCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();

protected:
    // Core Vulkan resources (accessible to derived classes)
    std::unique_ptr<Window> m_window;
    std::unique_ptr<VulkanContext> m_context;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<Swapchain> m_swapchain;
    std::unique_ptr<BufferManager> m_bufferManager;

    // Command buffers and sync objects
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_currentFrame = 0;

    // State
    bool m_framebufferResized = false;
    bool m_cleanedUp = false;

private:
    int m_initialWidth;
    int m_initialHeight;
    const char* m_title;
};

} // namespace eden
