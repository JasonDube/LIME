#include "ImGuiManager.hpp"
#include "VulkanContext.hpp"
#include "Swapchain.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace eden {

ImGuiManager::~ImGuiManager() {
    cleanup();
}

ImGuiManager::ImGuiManager(ImGuiManager&& other) noexcept
    : m_device(other.m_device)
    , m_descriptorPool(other.m_descriptorPool)
    , m_initialized(other.m_initialized)
    , m_iniFilename(std::move(other.m_iniFilename))
{
    other.m_device = VK_NULL_HANDLE;
    other.m_descriptorPool = VK_NULL_HANDLE;
    other.m_initialized = false;
}

ImGuiManager& ImGuiManager::operator=(ImGuiManager&& other) noexcept {
    if (this != &other) {
        cleanup();
        m_device = other.m_device;
        m_descriptorPool = other.m_descriptorPool;
        m_initialized = other.m_initialized;
        m_iniFilename = std::move(other.m_iniFilename);

        other.m_device = VK_NULL_HANDLE;
        other.m_descriptorPool = VK_NULL_HANDLE;
        other.m_initialized = false;
    }
    return *this;
}

void ImGuiManager::init(VulkanContext& context, Swapchain& swapchain, GLFWwindow* window,
                        const std::string& iniFilename) {
    if (m_initialized) {
        return;
    }

    m_device = context.getDevice();
    m_iniFilename = iniFilename;

    // Create descriptor pool for ImGui
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImGui descriptor pool");
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // NOTE: NavEnableKeyboard is intentionally NOT enabled
    // Tab key is reserved for editor mode switching, not ImGui navigation
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // Enable docking

    // Set INI filename (static storage to ensure pointer remains valid)
    static std::string s_iniFilename;
    s_iniFilename = m_iniFilename;
    io.IniFilename = s_iniFilename.c_str();

    ImGui::StyleColorsDark();

    // Initialize GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // Initialize Vulkan backend
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = context.getInstance();
    initInfo.PhysicalDevice = context.getPhysicalDevice();
    initInfo.Device = context.getDevice();
    initInfo.QueueFamily = context.getGraphicsQueueFamily();
    initInfo.Queue = context.getGraphicsQueue();
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = static_cast<uint32_t>(swapchain.getImageCount());
    initInfo.PipelineInfoMain.RenderPass = swapchain.getRenderPass();

    ImGui_ImplVulkan_Init(&initInfo);

    m_initialized = true;
}

void ImGuiManager::cleanup() {
    if (!m_initialized) {
        return;
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptorPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    m_initialized = false;
}

} // namespace eden
