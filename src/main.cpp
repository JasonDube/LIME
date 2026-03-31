/**
 * @file main.cpp
 * @brief LIME Editor - Model Editor Application
 *
 * Refactored architecture with two editor modes:
 * - ModelingMode: Mesh editing with vertex/edge/face selection, UV editing
 * - AnimationMode: Skeletal animation combining
 */

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include "Renderer/VulkanApplicationBase.hpp"
#include "Renderer/VulkanContext.hpp"
#include "Renderer/Swapchain.hpp"
#include "Renderer/ImGuiManager.hpp"
#include "Renderer/ModelRenderer.hpp"
#include "Renderer/SkinnedModelRenderer.hpp"
#include "Editor/SceneObject.hpp"
#include "Editor/GLBLoader.hpp"

#include "IEditorMode.hpp"
#include "EditorContext.hpp"
#include "AnimationMode.hpp"
#include "ModelingMode.hpp"
#include "EditableMesh.hpp"
#include "MCPServer.hpp"
#include "Hunyuan3DClient.hpp"

#include <eden/Camera.hpp>
#include <eden/Input.hpp>
#include <eden/Window.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <nfd.h>
#include <GLFW/glfw3.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <iostream>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

using namespace eden;
namespace fs = std::filesystem;

// Editor mode types
enum class EditorModeType {
    ModelingEditor,
    AnimationCombiner
};

class ModelEditor : public VulkanApplicationBase {
public:
    ModelEditor() : VulkanApplicationBase(1600, 900, "LIME Editor") {}

protected:
    void onInit() override {
        // Initialize NFD
        NFD_Init();

        // Initialize renderers
        m_modelRenderer = std::make_unique<ModelRenderer>(getContext(), getSwapchain().getRenderPass(), getSwapchain().getExtent());
        m_skinnedModelRenderer = std::make_unique<SkinnedModelRenderer>(getContext(), getSwapchain().getRenderPass(), getSwapchain().getExtent());

        // Initialize ImGui
        m_imguiManager.init(getContext(), getSwapchain(), getWindow().getHandle());

        // Setup camera
        m_camera.setPosition(glm::vec3(3, 2, 5));
        m_camera.setYaw(-120.0f);
        m_camera.setPitch(-15.0f);
        m_camera.setNoClip(true);

        m_camera2.setPosition(glm::vec3(0, 10, 0));
        m_camera2.setNoClip(true);

        // Create editor context
        createEditorContext();

        // Create mode instances
        m_modelingMode = std::make_unique<ModelingMode>(*m_editorContext);
        m_animationMode = std::make_unique<AnimationMode>(*m_editorContext);

        // Start in modeling mode
        switchMode(EditorModeType::ModelingEditor);

        // Initialize library path from project source directory
        m_libraryPath = std::string(CMAKE_SOURCE_DIR) + "/library";

        // Initialize MCP server
        initMCPServer();
    }

    void initMCPServer() {
        m_mcpServer = std::make_unique<MCPServer>(9999);

        // Register tools for scene manipulation
        m_mcpServer->registerTool("ping", "Test if server is responsive", [](const MCPParams&) {
            return MCPResult{{"message", MCPValue("pong")}};
        });

        m_mcpServer->registerTool("list_objects", "List all objects in the scene", [this](const MCPParams&) {
            std::string names;
            for (size_t i = 0; i < m_sceneObjects.size(); ++i) {
                if (i > 0) names += ", ";
                names += m_sceneObjects[i]->getName();
            }
            return MCPResult{
                {"count", MCPValue(static_cast<int>(m_sceneObjects.size()))},
                {"names", MCPValue(names)}
            };
        });

        m_mcpServer->registerTool("create_cube", "Create a cube primitive", [this](const MCPParams& params) {
            createTestCube();
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube created")}};
        });

        m_mcpServer->registerTool("create_cube_ring", "Create a ring of cubes", [this](const MCPParams& params) {
            auto segIt = params.find("segments");
            auto innerIt = params.find("inner_radius");
            auto outerIt = params.find("outer_radius");
            auto heightIt = params.find("height");

            if (segIt != params.end()) m_cubeRingSegments = segIt->second.getInt();
            if (innerIt != params.end()) m_cubeRingInnerRadius = innerIt->second.getFloat();
            if (outerIt != params.end()) m_cubeRingOuterRadius = outerIt->second.getFloat();
            if (heightIt != params.end()) m_cubeRingHeight = heightIt->second.getFloat();

            createCubeRing();
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube ring created")}};
        });

        m_mcpServer->registerTool("create_cylinder", "Create a cylinder primitive", [this](const MCPParams& params) {
            auto segIt = params.find("segments");
            auto radiusIt = params.find("radius");
            auto heightIt = params.find("height");

            if (segIt != params.end()) m_cylinderSegments = segIt->second.getInt();
            if (radiusIt != params.end()) m_cylinderRadius = radiusIt->second.getFloat();
            if (heightIt != params.end()) m_cylinderHeight = heightIt->second.getFloat();

            createCylinder();
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cylinder created")}};
        });

        m_mcpServer->registerTool("create_cube_arch", "Create an arch made of cubes", [this](const MCPParams& params) {
            int segments = 8;
            float innerRadius = 0.4f;
            float outerRadius = 0.6f;
            float depth = 0.3f;
            float arcDegrees = 180.0f;

            auto it = params.find("segments");
            if (it != params.end()) segments = it->second.getInt();
            it = params.find("inner_radius");
            if (it != params.end()) innerRadius = it->second.getFloat();
            it = params.find("outer_radius");
            if (it != params.end()) outerRadius = it->second.getFloat();
            it = params.find("depth");
            if (it != params.end()) depth = it->second.getFloat();
            it = params.find("arc_degrees");
            if (it != params.end()) arcDegrees = it->second.getFloat();

            createCubeArch(segments, innerRadius, outerRadius, depth, arcDegrees);
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube arch created")}};
        });

        m_mcpServer->registerTool("create_cube_column", "Create a solid column made of cubes", [this](const MCPParams& params) {
            int segments = 8;
            float radius = 0.5f;
            float height = 1.0f;

            auto it = params.find("segments");
            if (it != params.end()) segments = it->second.getInt();
            it = params.find("radius");
            if (it != params.end()) radius = it->second.getFloat();
            it = params.find("height");
            if (it != params.end()) height = it->second.getFloat();

            createCubeColumn(segments, radius, height);
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube column created")}};
        });

        m_mcpServer->registerTool("create_cube_stairs", "Create stairs made of cubes", [this](const MCPParams& params) {
            int steps = 5;
            float width = 1.0f;
            float stepHeight = 0.2f;
            float stepDepth = 0.3f;

            auto it = params.find("steps");
            if (it != params.end()) steps = it->second.getInt();
            it = params.find("width");
            if (it != params.end()) width = it->second.getFloat();
            it = params.find("step_height");
            if (it != params.end()) stepHeight = it->second.getFloat();
            it = params.find("step_depth");
            if (it != params.end()) stepDepth = it->second.getFloat();

            createCubeStairs(steps, width, stepHeight, stepDepth);
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube stairs created")}};
        });

        m_mcpServer->registerTool("create_cube_room", "Create a hollow room made of cubes with window openings", [this](const MCPParams& params) {
            int width = 10;
            int height = 4;
            int depth = 12;
            float cubeSize = 0.5f;
            int windowFront = 3;

            auto it = params.find("width");
            if (it != params.end()) width = it->second.getInt();
            it = params.find("height");
            if (it != params.end()) height = it->second.getInt();
            it = params.find("depth");
            if (it != params.end()) depth = it->second.getInt();
            it = params.find("cube_size");
            if (it != params.end()) cubeSize = it->second.getFloat();
            it = params.find("window_front");
            if (it != params.end()) windowFront = it->second.getInt();

            createCubeRoom(width, height, depth, cubeSize, windowFront);
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Cube room created")}};
        });

        m_mcpServer->registerTool("load_lime", "Load a .lime file", [this](const MCPParams& params) {
            auto pathIt = params.find("path");
            if (pathIt == params.end()) {
                return MCPResult{{"error", MCPValue("Missing 'path' parameter")}};
            }
            std::string path = pathIt->second.getString();
            // TODO: Implement lime loading via MCP
            return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Loading: " + path)}};
        });

        m_mcpServer->registerTool("select_object", "Select an object by name", [this](const MCPParams& params) {
            auto nameIt = params.find("name");
            if (nameIt == params.end()) {
                return MCPResult{{"error", MCPValue("Missing 'name' parameter")}};
            }
            std::string name = nameIt->second.getString();
            for (auto& obj : m_sceneObjects) {
                if (obj->getName() == name) {
                    m_selectedObject = obj.get();
                    return MCPResult{{"success", MCPValue(true)}, {"message", MCPValue("Selected: " + name)}};
                }
            }
            return MCPResult{{"error", MCPValue("Object not found: " + name)}};
        });

        m_mcpServer->registerTool("set_position", "Set position of selected object", [this](const MCPParams& params) {
            if (!m_selectedObject) {
                return MCPResult{{"error", MCPValue("No object selected")}};
            }
            auto xIt = params.find("x");
            auto yIt = params.find("y");
            auto zIt = params.find("z");

            glm::vec3 pos = m_selectedObject->getTransform().getPosition();
            if (xIt != params.end()) pos.x = xIt->second.getFloat();
            if (yIt != params.end()) pos.y = yIt->second.getFloat();
            if (zIt != params.end()) pos.z = zIt->second.getFloat();

            m_selectedObject->getTransform().setPosition(pos);
            return MCPResult{{"success", MCPValue(true)}};
        });

        // TODO: Add duplicate_selected tool when duplication is implemented

        m_mcpServer->registerTool("get_bounds", "Get bounds of selected object", [this](const MCPParams&) {
            if (!m_selectedObject) {
                return MCPResult{{"error", MCPValue("No object selected")}};
            }
            auto bounds = m_selectedObject->getWorldBounds();
            return MCPResult{
                {"min_x", MCPValue(bounds.min.x)},
                {"min_y", MCPValue(bounds.min.y)},
                {"min_z", MCPValue(bounds.min.z)},
                {"max_x", MCPValue(bounds.max.x)},
                {"max_y", MCPValue(bounds.max.y)},
                {"max_z", MCPValue(bounds.max.z)}
            };
        });

        m_mcpServer->start();
        std::cout << "[MCP] LIME Editor MCP server ready at http://localhost:9999" << std::endl;
        std::cout << "[MCP] Test with: curl http://localhost:9999/tools" << std::endl;
    }

    void onCleanup() override {
        // Stop AI generation thread (always try to join if joinable)
        m_aiGenerateCancelled = true;
        if (m_aiGenerateThread.joinable()) {
            m_aiGenerateThread.join();
        }

        // Stop Hunyuan server if we started it
        stopHunyuanServer();

        // Always join startup thread to prevent std::terminate on destroy
        if (m_aiServerStartupThread.joinable()) {
            m_aiServerRunning = false;  // Signal thread to exit
            m_aiServerStartupThread.join();
        }

        // Stop MCP server
        if (m_mcpServer) {
            m_mcpServer->stop();
            m_mcpServer.reset();
        }
        vkDeviceWaitIdle(getContext().getDevice());

        // Cleanup reference images
        for (auto& ref : m_referenceImages) {
            cleanupReferenceImage(ref);
        }

        // Cleanup stamp preview
        cleanupStampPreview();

        // Cleanup clone source images
        for (auto& img : m_cloneSourceImages) {
            cleanupCloneSourceImage(img);
        }
        m_cloneSourceImages.clear();

        m_modelingMode.reset();
        m_animationMode.reset();
        m_editorContext.reset();

        for (auto& obj : m_sceneObjects) {
            if (obj->getBufferHandle() != UINT32_MAX) {
                m_modelRenderer->destroyModel(obj->getBufferHandle());
            }
        }
        m_sceneObjects.clear();

        m_imguiManager.cleanup();
        m_skinnedModelRenderer.reset();
        m_modelRenderer.reset();

        NFD_Quit();
    }

    void cleanupReferenceImage(ReferenceImage& ref) {
        if (!ref.loaded) return;

        VkDevice device = getContext().getDevice();

        if (ref.descriptorSet) {
            ImGui_ImplVulkan_RemoveTexture(ref.descriptorSet);
            ref.descriptorSet = VK_NULL_HANDLE;
        }
        if (ref.sampler) {
            vkDestroySampler(device, ref.sampler, nullptr);
            ref.sampler = VK_NULL_HANDLE;
        }
        if (ref.view) {
            vkDestroyImageView(device, ref.view, nullptr);
            ref.view = VK_NULL_HANDLE;
        }
        if (ref.image) {
            vkDestroyImage(device, ref.image, nullptr);
            ref.image = VK_NULL_HANDLE;
        }
        if (ref.memory) {
            vkFreeMemory(device, ref.memory, nullptr);
            ref.memory = VK_NULL_HANDLE;
        }

        ref.loaded = false;
        ref.filepath.clear();
        ref.imageWidth = 0;
        ref.imageHeight = 0;
        ref.pixelData.clear();
    }

    void cleanupStampPreview() {
        VkDevice device = getContext().getDevice();

        // Wait for GPU to finish using the resources
        vkDeviceWaitIdle(device);

        if (m_stampPreviewDescriptor) {
            ImGui_ImplVulkan_RemoveTexture(m_stampPreviewDescriptor);
            m_stampPreviewDescriptor = VK_NULL_HANDLE;
        }
        if (m_stampPreviewSampler) {
            vkDestroySampler(device, m_stampPreviewSampler, nullptr);
            m_stampPreviewSampler = VK_NULL_HANDLE;
        }
        if (m_stampPreviewView) {
            vkDestroyImageView(device, m_stampPreviewView, nullptr);
            m_stampPreviewView = VK_NULL_HANDLE;
        }
        if (m_stampPreviewImage) {
            vkDestroyImage(device, m_stampPreviewImage, nullptr);
            m_stampPreviewImage = VK_NULL_HANDLE;
        }
        if (m_stampPreviewMemory) {
            vkFreeMemory(device, m_stampPreviewMemory, nullptr);
            m_stampPreviewMemory = VK_NULL_HANDLE;
        }
    }

    void cleanupCloneSourceImage(CloneSourceImage& img) {
        VkDevice device = getContext().getDevice();

        // Wait for GPU to finish using the resources
        vkDeviceWaitIdle(device);

        if (img.descriptorSet) {
            ImGui_ImplVulkan_RemoveTexture(img.descriptorSet);
            img.descriptorSet = VK_NULL_HANDLE;
        }
        if (img.sampler) {
            vkDestroySampler(device, img.sampler, nullptr);
            img.sampler = VK_NULL_HANDLE;
        }
        if (img.view) {
            vkDestroyImageView(device, img.view, nullptr);
            img.view = VK_NULL_HANDLE;
        }
        if (img.image) {
            vkDestroyImage(device, img.image, nullptr);
            img.image = VK_NULL_HANDLE;
        }
        if (img.memory) {
            vkFreeMemory(device, img.memory, nullptr);
            img.memory = VK_NULL_HANDLE;
        }
    }

    // Forward declaration of LibraryItem struct for thumbnail functions
    struct LibraryItem {
        std::string filepath;
        std::string thumbnailPath;
        std::string name;
        std::string category;
        // Vulkan resources for thumbnail
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        bool thumbnailLoaded = false;
    };

    void cleanupLibraryThumbnail(LibraryItem& item) {
        VkDevice device = getContext().getDevice();

        if (item.descriptor) {
            ImGui_ImplVulkan_RemoveTexture(item.descriptor);
            item.descriptor = VK_NULL_HANDLE;
        }
        if (item.sampler) {
            vkDestroySampler(device, item.sampler, nullptr);
            item.sampler = VK_NULL_HANDLE;
        }
        if (item.view) {
            vkDestroyImageView(device, item.view, nullptr);
            item.view = VK_NULL_HANDLE;
        }
        if (item.image) {
            vkDestroyImage(device, item.image, nullptr);
            item.image = VK_NULL_HANDLE;
        }
        if (item.memory) {
            vkFreeMemory(device, item.memory, nullptr);
            item.memory = VK_NULL_HANDLE;
        }
        item.thumbnailLoaded = false;
    }

    void loadLibraryThumbnail(LibraryItem& item) {
        if (item.thumbnailLoaded || item.thumbnailPath.empty()) return;
        if (!fs::exists(item.thumbnailPath)) return;

        // Load image with stb_image
        int width, height, channels;
        unsigned char* pixels = stbi_load(item.thumbnailPath.c_str(), &width, &height, &channels, 4);
        if (!pixels) return;

        VkDevice device = getContext().getDevice();
        VkDeviceSize imageSize = width * height * 4;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mappedData;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mappedData);
        memcpy(mappedData, pixels, imageSize);
        vkUnmapMemory(device, stagingMemory);

        stbi_image_free(pixels);

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        vkCreateImage(device, &imageInfo, nullptr, &item.image);

        vkGetImageMemoryRequirements(device, item.image, &memReq);
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &item.memory);
        vkBindImageMemory(device, item.image, item.memory, 0);

        // Transition and copy
        VkCommandBuffer cmd = getContext().beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = item.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

        vkCmdCopyBufferToImage(cmd, stagingBuffer, item.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        getContext().endSingleTimeCommands(cmd);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = item.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &item.view);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        vkCreateSampler(device, &samplerInfo, nullptr, &item.sampler);

        // Create ImGui descriptor
        item.descriptor = ImGui_ImplVulkan_AddTexture(item.sampler, item.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        item.thumbnailLoaded = true;
    }

    void createCloneSourceImageTexture(CloneSourceImage& img) {
        if (img.pixelData.empty() || img.width <= 0 || img.height <= 0) {
            return;
        }

        // Cleanup existing
        cleanupCloneSourceImage(img);

        VkDevice device = getContext().getDevice();
        VkDeviceSize imageSize = img.width * img.height * 4;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mappedData;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mappedData);
        memcpy(mappedData, img.pixelData.data(), imageSize);
        vkUnmapMemory(device, stagingMemory);

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = img.width;
        imageInfo.extent.height = img.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        vkCreateImage(device, &imageInfo, nullptr, &img.image);

        vkGetImageMemoryRequirements(device, img.image, &memReq);
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &img.memory);
        vkBindImageMemory(device, img.image, img.memory, 0);

        // Transition and copy
        VkCommandBuffer cmd = getContext().beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1};

        vkCmdCopyBufferToImage(cmd, stagingBuffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        getContext().endSingleTimeCommands(cmd);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = img.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &img.view);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        vkCreateSampler(device, &samplerInfo, nullptr, &img.sampler);

        // Create ImGui descriptor
        img.descriptorSet = ImGui_ImplVulkan_AddTexture(img.sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        std::cout << "Created clone source image texture: " << img.width << "x" << img.height
                  << " descriptor=" << img.descriptorSet
                  << " image=" << img.image << std::endl;
    }

    void updateStampPreview(const unsigned char* data, int width, int height) {
        if (!data || width <= 0 || height <= 0) {
            cleanupStampPreview();
            return;
        }

        // Cleanup existing
        cleanupStampPreview();

        VkDevice device = getContext().getDevice();
        VkDeviceSize imageSize = width * height * 4;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mappedData;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mappedData);
        memcpy(mappedData, data, imageSize);
        vkUnmapMemory(device, stagingMemory);

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        vkCreateImage(device, &imageInfo, nullptr, &m_stampPreviewImage);

        vkGetImageMemoryRequirements(device, m_stampPreviewImage, &memReq);
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &m_stampPreviewMemory);
        vkBindImageMemory(device, m_stampPreviewImage, m_stampPreviewMemory, 0);

        // Transition and copy
        VkCommandBuffer cmd = getContext().beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_stampPreviewImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

        vkCmdCopyBufferToImage(cmd, stagingBuffer, m_stampPreviewImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        getContext().endSingleTimeCommands(cmd);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_stampPreviewImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &m_stampPreviewView);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        vkCreateSampler(device, &samplerInfo, nullptr, &m_stampPreviewSampler);

        // Create ImGui descriptor
        m_stampPreviewDescriptor = ImGui_ImplVulkan_AddTexture(m_stampPreviewSampler, m_stampPreviewView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        std::cout << "Created stamp preview texture: " << width << "x" << height << std::endl;
    }

    bool loadReferenceImageFile(int viewIndex, const std::string& filepath) {
        if (viewIndex < 0 || viewIndex >= 6) return false;

        auto& ref = m_referenceImages[viewIndex];

        // Cleanup existing
        cleanupReferenceImage(ref);

        // Load image
        int channels;
        unsigned char* pixels = stbi_load(filepath.c_str(), &ref.imageWidth, &ref.imageHeight, &channels, STBI_rgb_alpha);
        if (!pixels) {
            std::cerr << "Failed to load reference image: " << filepath << std::endl;
            return false;
        }

        VkDevice device = getContext().getDevice();
        VkDeviceSize imageSize = ref.imageWidth * ref.imageHeight * 4;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* data;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
        memcpy(data, pixels, imageSize);
        vkUnmapMemory(device, stagingMemory);

        // Store pixel data for eyedropper sampling
        ref.pixelData.assign(pixels, pixels + imageSize);

        stbi_image_free(pixels);

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = ref.imageWidth;
        imageInfo.extent.height = ref.imageHeight;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        vkCreateImage(device, &imageInfo, nullptr, &ref.image);

        vkGetImageMemoryRequirements(device, ref.image, &memReq);
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &ref.memory);
        vkBindImageMemory(device, ref.image, ref.memory, 0);

        // Transition and copy
        VkCommandBuffer cmd = getContext().beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = ref.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<uint32_t>(ref.imageWidth), static_cast<uint32_t>(ref.imageHeight), 1};

        vkCmdCopyBufferToImage(cmd, stagingBuffer, ref.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        getContext().endSingleTimeCommands(cmd);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ref.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(device, &viewInfo, nullptr, &ref.view);

        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        vkCreateSampler(device, &samplerInfo, nullptr, &ref.sampler);

        // Create ImGui descriptor
        ref.descriptorSet = ImGui_ImplVulkan_AddTexture(ref.sampler, ref.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        ref.loaded = true;
        ref.filepath = filepath;
        ref.name = filepath.substr(filepath.find_last_of("/\\") + 1);

        // Set initial size based on aspect ratio
        float aspect = static_cast<float>(ref.imageWidth) / ref.imageHeight;
        ref.size = glm::vec2(5.0f * aspect, 5.0f);

        std::cout << "Loaded reference image: " << ref.name << " (" << ref.imageWidth << "x" << ref.imageHeight << ")" << std::endl;
        return true;
    }

    void update(float deltaTime) override {
        // Process MCP commands from AI
        if (m_mcpServer) {
            m_mcpServer->processCommands();
        }

        // Check for completed AI generation
        if (m_aiGenerateComplete) {
            m_aiGenerateComplete = false;
            if (m_aiGenerateThread.joinable()) {
                m_aiGenerateThread.join();
            }
            m_aiGenerating = false;
            if (!m_aiGeneratedGLBPath.empty()) {
                std::cout << "[Hunyuan3D] Loading generated model: " << m_aiGeneratedGLBPath << std::endl;
                loadModel(m_aiGeneratedGLBPath);
                std::cout << "[Hunyuan3D] Model loaded, framing..." << std::endl;
                // Auto-frame the generated model so the camera shows it properly
                if (m_selectedObject) {
                    Camera& cam = (m_splitView && !m_activeViewportLeft) ? m_camera2 : m_camera;
                    frameSelected(cam);
                }
                m_aiGenerateStatus = "Model loaded!";
                std::cout << "[Hunyuan3D] Auto-loaded generated model" << std::endl;
            }
        }

        // Initialize ImGui frame BEFORE input processing so IsWindowHovered() uses current state
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();

        // CRITICAL: Clear Tab key from ImGui IMMEDIATELY after GLFW backend reads input
        // This prevents Tab from being used for ImGui widget navigation
        // Tab is reserved exclusively for toggling object/component mode
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);

        // Process deferred deletions at start of frame (safe point for GPU sync)
        if (!m_pendingDeletions.empty()) {
            vkDeviceWaitIdle(getContext().getDevice());
            for (SceneObject* obj : m_pendingDeletions) {
                if (!obj) continue;

                // Remove from multi-selection set first
                m_selectedObjects.erase(obj);

                // Clear primary selection if it's being deleted
                if (m_selectedObject == obj) {
                    m_selectedObject = nullptr;
                }

                // Clear editable mesh if it was built from this object
                if (m_modelingMode && m_editorContext) {
                    m_editorContext->editableMesh.clear();
                    m_editorContext->meshDirty = false;
                }

                // Find and remove from scene
                for (auto it = m_sceneObjects.begin(); it != m_sceneObjects.end(); ++it) {
                    if (it->get() == obj) {
                        uint32_t handle = (*it)->getBufferHandle();
                        if (handle != UINT32_MAX && handle != 0) {
                            m_modelRenderer->destroyModel(handle);
                        }
                        m_sceneObjects.erase(it);
                        break;
                    }
                }
            }
            m_pendingDeletions.clear();
        }

        // Process deferred texture deletion (safe point for GPU sync)
        if (m_pendingTextureDelete) {
            m_pendingTextureDelete = false;
            if (m_selectedObject) {
                vkDeviceWaitIdle(getContext().getDevice());
                uint32_t handle = m_selectedObject->getBufferHandle();
                m_modelRenderer->destroyTexture(handle);
                m_selectedObject->clearTextureData();
                std::cout << "Deleted texture (deferred)" << std::endl;
            }
        }

        // Process input
        processInput(deltaTime);

        // Update active mode
        if (m_activeMode) {
            m_activeMode->update(deltaTime);
        }
    }

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) override {
        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Note: ImGui_ImplVulkan_NewFrame and ImGui_ImplGlfw_NewFrame are called
        // in update() before input processing, so IsWindowHovered() uses current state

        // Begin render pass
        auto& swapchain = getSwapchain();
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = swapchain.getRenderPass();
        renderPassInfo.framebuffer = swapchain.getFramebuffers()[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapchain.getExtent();

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{m_backgroundColor.r, m_backgroundColor.g, m_backgroundColor.b, m_backgroundColor.a}};
        clearValues[1].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        float screenWidth = static_cast<float>(swapchain.getExtent().width);
        float screenHeight = static_cast<float>(swapchain.getExtent().height);

        // Render scene (potentially split view)
        if (m_splitView && m_activeMode && m_activeMode->supportsSplitView()) {
            // Left viewport (perspective)
            renderSceneToViewport(cmd, m_camera, 0, 0, screenWidth / 2.0f, screenHeight);

            // Right viewport (ortho)
            renderSceneToViewport(cmd, m_camera2, screenWidth / 2.0f, 0, screenWidth / 2.0f, screenHeight);
        } else {
            // Full screen
            renderSceneToViewport(cmd, m_camera, 0, 0, screenWidth, screenHeight);
        }

        // Render UI (ImGui NewFrame was called at start of recordCommandBuffer)
        renderUI();

        // Render ImGui draw data
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkCmdEndRenderPass(cmd);

        // End command buffer
        vkEndCommandBuffer(cmd);

#ifdef TRACY_ENABLE
        FrameMark;
#endif
    }

private:
    void createEditorContext() {
        m_editorContext = std::make_unique<EditorContext>(EditorContext{
            .vulkanContext = getContext(),
            .swapchain = getSwapchain(),
            .window = getWindow(),
            .modelRenderer = *m_modelRenderer,
            .skinnedModelRenderer = *m_skinnedModelRenderer,
            .imguiManager = m_imguiManager,
            .camera = m_camera,
            .camera2 = m_camera2,
            .cameraSpeed = m_cameraSpeed,
            .splitView = m_splitView,
            .activeViewportLeft = m_activeViewportLeft,
            .splitOrthoPreset = m_splitOrthoPreset,
            .sceneObjects = m_sceneObjects,
            .selectedObject = m_selectedObject,
            .selectedObjects = m_selectedObjects,
            .editMode = m_editMode,
            .paintColor = m_paintColor,
            .paintRadius = m_paintRadius,
            .paintStrength = m_paintStrength,
            .isPainting = m_isPainting,
            .squareBrush = m_squareBrush,
            .fillPaintToFace = m_fillPaintToFace,
            .useStamp = m_useStamp,
            .useSmear = m_useSmear,
            .useEyedropper = m_useEyedropper,
            .useClone = m_useClone,
            .cloneSourceViewIndex = m_cloneSourceViewIndex,
            .cloneSourcePixel = m_cloneSourcePixel,
            .cloneCurrentSample = m_cloneCurrentSample,
            .cloneLastPaintUV = m_cloneLastPaintUV,
            .cloneSourceSet = m_cloneSourceSet,
            .clonePaintingActive = m_clonePaintingActive,
            .lastPaintUV = m_lastPaintUV,
            .hasLastPaintPosition = m_hasLastPaintPosition,
            .smearStrength = m_smearStrength,
            .smearPickup = m_smearPickup,
            .smearCarriedColor = m_smearCarriedColor,
            .isSmearing = m_isSmearing,
            .stampData = m_stampData,
            .stampWidth = m_stampWidth,
            .stampHeight = m_stampHeight,
            .stampScale = m_stampScale,
            .stampScaleH = m_stampScaleH,
            .stampScaleV = m_stampScaleV,
            .stampRotation = m_stampRotation,
            .stampOpacity = m_stampOpacity,
            .stampFlipH = m_stampFlipH,
            .stampFlipV = m_stampFlipV,
            .stampProjectFromView = m_stampProjectFromView,
            .stampFitToFace = m_stampFitToFace,
            .stampFitRotation = m_stampFitRotation,
            .seamBusterPixels = m_seamBusterPixels,
            .stampPreviewDescriptor = m_stampPreviewDescriptor,
            .uvWireframeColor = m_uvWireframeColor,
            .uvZoom = m_uvZoom,
            .uvPan = m_uvPan,
            .uvPanning = m_uvPanning,
            .uvPanStart = m_uvPanStart,
            .showWireframe = m_showWireframe,
            .selectedFaces = m_selectedFaces,
            .hiddenFaces = m_hiddenFaces,
            .selectionColor = m_selectionColor,
            .uvIslands = m_uvIslands,
            .selectedIslands = m_selectedIslands,
            .nextIslandId = m_nextIslandId,
            .rng = m_rng,
            .uvIslandOffset = m_uvIslandOffset,
            .uvIslandScale = m_uvIslandScale,
            .uvDragging = m_uvDragging,
            .uvResizing = m_uvResizing,
            .uvResizeCorner = m_uvResizeCorner,
            .uvDragStart = m_uvDragStart,
            .uvIslandOriginalMin = m_uvIslandOriginalMin,
            .uvIslandOriginalMax = m_uvIslandOriginalMax,
            .uvHandleHovered = m_uvHandleHovered,
            .editableMesh = m_editableMesh,
            .faceToTriangles = m_faceToTriangles,
            .modelingSelectionMode = m_modelingSelectionMode,
            .extrudeDistance = m_extrudeDistance,
            .extrudeCount = m_extrudeCount,
            .insetAmount = m_insetAmount,
            .hollowThickness = m_hollowThickness,
            .vertexDisplaySize = m_vertexDisplaySize,
            .edgeDisplayWidth = m_edgeDisplayWidth,
            .modelingSelectionColor = m_modelingSelectionColor,
            .modelingHoverColor = m_modelingHoverColor,
            .modelingVertexColor = m_modelingVertexColor,
            .modelingEdgeColor = m_modelingEdgeColor,
            .showModelingWireframe = m_showModelingWireframe,
            .showFaceNormals = m_showFaceNormals,
            .normalDisplayLength = m_normalDisplayLength,
            .uvProjectionScale = m_uvProjectionScale,
            .uvAngleThreshold = m_uvAngleThreshold,
            .uvIslandMargin = m_uvIslandMargin,
            .cylinderAxisIndex = m_cylinderAxisIndex,
            .cylinderAxisHint = m_cylinderAxisHint,
            .cylinderUsePCA = m_cylinderUsePCA,
            .hoveredVertex = m_hoveredVertex,
            .hoveredEdge = m_hoveredEdge,
            .hoveredFace = m_hoveredFace,
            .lastClickTime = m_lastClickTime,
            .meshDirty = m_meshDirty,
            .selectionTool = m_selectionTool,
            .isRectSelecting = m_isRectSelecting,
            .rectSelectStart = m_rectSelectStart,
            .rectSelectEnd = m_rectSelectEnd,
            .paintSelectRadius = m_paintSelectRadius,
            .showGrid = m_showGrid,
            .gridSize = m_gridSize,
            .gridSpacing = m_gridSpacing,
            .gridColor = m_gridColor,
            .gridAxisColor = m_gridAxisColor,
            .backgroundColor = m_backgroundColor,
            .defaultMeshColor = m_defaultMeshColor,
            .wireframeColor = m_wireframeColor,
            .randomMeshColors = m_randomMeshColors,
            .referenceImages = m_referenceImages,
            .showSceneWindow = m_showSceneWindow,
            .showToolsWindow = m_showToolsWindow,
            .showUVWindow = m_showUVWindow,
            .showCameraWindow = m_showCameraWindow,
            .showImageRefWindow = m_showImageRefWindow,
            .imageRefZoom = m_imageRefZoom,
            .imageRefPan = m_imageRefPan,
            .imageRefPanning = m_imageRefPanning,
            .imageRefSelectedIndex = m_imageRefSelectedIndex,
            .cloneSourceImages = m_cloneSourceImages,
            .objectMode = m_objectMode,
            .renamingObjectIndex = m_renamingObjectIndex,
            .renameBuffer = m_renameBuffer,
            .renameBufferSize = sizeof(m_renameBuffer),
            .transformMove = m_transformMove,
            .transformScale = m_transformScale,
            .transformRotate = m_transformRotate,
            .lastScale = m_lastScale,
            .lastRotate = m_lastRotate,
            .transformActive = m_transformActive,
            .uvDraggingSelection = m_uvDraggingSelection,
            .uvScaling = m_uvScaling,
            .uvRotating = m_uvRotating,
            .uvChildHovered = m_uvChildHovered,
            .uvScaleCenter = m_uvScaleCenter,
            .uvScaleStart = m_uvScaleStart,
            .uvRotateStartAngle = m_uvRotateStartAngle,
            .uvSelectedFaces = m_uvSelectedFaces,
            .uvOriginalCoords = m_uvOriginalCoords,
            .uvScaleHandle = m_uvScaleHandle,
            .uvScaleAnchor = m_uvScaleAnchor,
            .uvScaleOriginalMin = m_uvScaleOriginalMin,
            .uvScaleOriginalMax = m_uvScaleOriginalMax,
            .uvEdgeSelectionMode = m_uvEdgeSelectionMode,
            .uvSelectedEdge = m_uvSelectedEdge,
            .uvTwinEdges = m_uvTwinEdges,
            .uvSelectionMode = m_uvSelectionMode,
            .uvSelectedVertices = m_uvSelectedVertices,
            .uvDraggingVertex = m_uvDraggingVertex,
            .isLooking = m_isLooking,
            .isTumbling = m_isTumbling,
            .isPanning = m_isPanning,
            .orbitTarget = m_orbitTarget,
            .orbitYaw = m_orbitYaw,
            .orbitPitch = m_orbitPitch,
            .mouseLookMode = m_mouseLookMode,
            .gizmoMode = m_gizmoMode,
            .gizmoHoveredAxis = m_gizmoHoveredAxis,
            .gizmoActiveAxis = m_gizmoActiveAxis,
            .gizmoDragging = m_gizmoDragging,
            .gizmoDragStart = m_gizmoDragStart,
            .gizmoDragStartPos = m_gizmoDragStartPos,
            .gizmoOriginalObjPos = m_gizmoOriginalObjPos,
            .gizmoSize = m_gizmoSize,
            .gizmoOffset = m_gizmoOffset,
            .gizmoLocalSpace = m_gizmoLocalSpace,
            .snapEnabled = m_snapEnabled,
            .moveSnapIncrement = m_moveSnapIncrement,
            .rotateSnapIncrement = m_rotateSnapIncrement,
            .pendingDeletions = m_pendingDeletions,
            .pendingTextureDelete = m_pendingTextureDelete,
            .currentFilePath = m_currentFilePath,
            .currentFileFormat = m_currentFileFormat,
            .loadReferenceImageCallback = [this](int viewIndex, const std::string& path) {
                return loadReferenceImageFile(viewIndex, path);
            },
            .clearReferenceImageCallback = [this](int viewIndex) {
                if (viewIndex >= 0 && viewIndex < 6) {
                    cleanupReferenceImage(m_referenceImages[viewIndex]);
                }
            },
            .updateStampPreviewCallback = [this](const unsigned char* data, int w, int h) {
                updateStampPreview(data, w, h);
            },
            .createCloneImageTextureCallback = [this](CloneSourceImage& img) {
                createCloneSourceImageTexture(img);
            },
            .destroyCloneImageTextureCallback = [this](CloneSourceImage& img) {
                cleanupCloneSourceImage(img);
            },
            .generateModelCallback = [this](const std::string& prompt, const std::string& imagePath) {
                startAIGeneration(prompt, imagePath);
            },
            .cancelGenerationCallback = [this]() {
                cancelAIGeneration();
            },
            .toggleServerCallback = [this](bool lowVRAM, bool enableTex) {
                toggleHunyuanServer(lowVRAM, enableTex);
            },
            .aiGenerating = m_aiGenerating,
            .aiGenerateStatus = m_aiGenerateStatus,
            .aiServerRunning = m_aiServerRunning,
            .aiServerReady = m_aiServerReady,
            .aiLogLines = m_aiLogLines,
            .onMetadataLoaded = [this](const std::unordered_map<std::string, std::string>& meta) {
                m_widgetTypeIndex = 0;
                memset(m_widgetParamName, 0, sizeof(m_widgetParamName));
                memset(m_widgetMachineName, 0, sizeof(m_widgetMachineName));
                auto wtIt = meta.find("widget_type");
                if (wtIt != meta.end()) {
                    const char* typeKeys[] = {"machine", "input", "output", "button", "checkbox", "slider", "log"};
                    for (int i = 0; i < 7; ++i) {
                        if (wtIt->second == typeKeys[i]) { m_widgetTypeIndex = i + 1; break; }
                    }
                    auto mnIt = meta.find("machine_name");
                    if (mnIt != meta.end()) strncpy(m_widgetMachineName, mnIt->second.c_str(), sizeof(m_widgetMachineName) - 1);
                    auto pnIt = meta.find("param_name");
                    if (pnIt != meta.end()) strncpy(m_widgetParamName, pnIt->second.c_str(), sizeof(m_widgetParamName) - 1);
                }
            }
        });
    }

    void switchMode(EditorModeType type) {
        if (m_activeMode) {
            m_activeMode->onDeactivate();
        }

        m_currentModeType = type;

        switch (type) {
            case EditorModeType::ModelingEditor:
                m_activeMode = m_modelingMode.get();
                break;
            case EditorModeType::AnimationCombiner:
                m_activeMode = m_animationMode.get();
                break;
        }

        if (m_activeMode) {
            m_activeMode->onActivate();
        }
    }

    void processInput(float deltaTime) {
        // Handle camera control (shared across modes)
        processCameraInput(deltaTime);

        // Delegate mode-specific input
        if (m_activeMode) {
            m_activeMode->processInput(deltaTime);
        }

        // Mode switching with number keys
        if (Input::isKeyPressed(Input::KEY_1) &&
            (Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL))) {
            switchMode(EditorModeType::ModelingEditor);
        }
        if (Input::isKeyPressed(Input::KEY_2) &&
            (Input::isKeyDown(Input::KEY_LEFT_CONTROL) || Input::isKeyDown(Input::KEY_RIGHT_CONTROL))) {
            switchMode(EditorModeType::AnimationCombiner);
        }
    }

    void processCameraInput(float deltaTime) {
        // Handle split view viewport switching
        if (m_splitView) {
            double mouseX, mouseY;
            glfwGetCursorPos(getWindow().getHandle(), &mouseX, &mouseY);
            float centerX = getWindow().getWidth() / 2.0f;

            if (Input::isMouseButtonPressed(Input::MOUSE_LEFT) ||
                Input::isMouseButtonPressed(Input::MOUSE_MIDDLE) ||
                Input::isMouseButtonPressed(Input::MOUSE_RIGHT)) {
                m_activeViewportLeft = (mouseX < centerX);
            }
        }

        // Get active camera
        Camera& activeCamera = (m_splitView && !m_activeViewportLeft) ? m_camera2 : m_camera;
        bool isPerspective = activeCamera.getProjectionMode() == ProjectionMode::Perspective;

        // View preset shortcuts (F1-F3 for front/right/top, Ctrl variants for back/left/bottom)
        glm::vec3 viewCenter = m_selectedObject ? m_selectedObject->getTransform().getPosition() : glm::vec3(0);

        if (Input::isKeyPressed(Input::KEY_F1)) {
            activeCamera.setViewPreset(Input::isKeyDown(Input::KEY_LEFT_CONTROL) ? ViewPreset::Back : ViewPreset::Front, viewCenter);
        }
        if (Input::isKeyPressed(Input::KEY_F2)) {
            activeCamera.setViewPreset(Input::isKeyDown(Input::KEY_LEFT_CONTROL) ? ViewPreset::Left : ViewPreset::Right, viewCenter);
        }
        if (Input::isKeyPressed(Input::KEY_F3)) {
            activeCamera.setViewPreset(Input::isKeyDown(Input::KEY_LEFT_CONTROL) ? ViewPreset::Bottom : ViewPreset::Top, viewCenter);
        }
        // F5 is now used for quick save (handled in ModelingMode)

        // Frame selected (F or . key) - Maya style
        // F or . to frame selected
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            if (Input::isKeyPressed(Input::KEY_F) || Input::isKeyPressed(Input::KEY_PERIOD)) {
                frameSelected(activeCamera);
            }

            // Explode cube group into individual cubes (minus key)
            if (Input::isKeyPressed(Input::KEY_MINUS)) {
                explodeCubeObject();
            }

            // Group all objects into one (plus/equal key)
            if (Input::isKeyPressed(Input::KEY_EQUAL)) {
                groupSelectedObjects();
            }

            // Auto-UV for cube objects (U key)
            if (Input::isKeyPressed(Input::KEY_U)) {
                autoUVSelectedObject();
            }
        }

        // Get current mouse position
        double mouseX, mouseY;
        glfwGetCursorPos(getWindow().getHandle(), &mouseX, &mouseY);
        glm::vec2 currentMousePos(static_cast<float>(mouseX), static_cast<float>(mouseY));

        // Scroll wheel zoom
        // Don't process if ImGui wants the mouse (hovering over windows, scrolling lists, etc.)
        float scroll = Input::getScrollDelta();
        if (scroll != 0 && !ImGui::GetIO().WantCaptureMouse) {
            float orbitDistance = glm::length(activeCamera.getPosition() - m_orbitTarget);
            if (orbitDistance < 0.01f) orbitDistance = 5.0f;

            if (isPerspective) {
                // Speed scales with distance and camera speed setting
                float dollySpeed = std::max(orbitDistance * 0.15f, 0.05f) * (m_cameraSpeed * 10.0f);
                glm::vec3 forward = glm::normalize(m_orbitTarget - activeCamera.getPosition());
                glm::vec3 newPos = activeCamera.getPosition() + forward * scroll * dollySpeed;
                float newDistance = glm::length(newPos - m_orbitTarget);
                // Allow getting much closer (0.01 instead of 0.1)
                if (newDistance > 0.01f) {
                    activeCamera.setPosition(newPos);
                }
            } else {
                float currentSize = activeCamera.getOrthoSize();
                float zoomFactor = 1.0f - scroll * 0.1f;
                activeCamera.setOrthoSize(std::clamp(currentSize * zoomFactor, 0.5f, 100.0f));
            }
        }

        // Check if mouse is over ImGui for other controls
        bool mouseOverImGui = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);

        // Clear ImGui focus when mouse is not over any ImGui window
        // But don't clear if a popup/menu is open (WantCaptureMouse catches this)
        if (!mouseOverImGui && !ImGui::GetIO().WantCaptureMouse) {
            ImGui::SetWindowFocus(nullptr);
        }

        // Skip other camera controls if mouse is over an ImGui window or menu is open
        if (mouseOverImGui || ImGui::GetIO().WantCaptureMouse) {
            m_lastMousePos = currentMousePos;
            m_isTumbling = false;
            m_isPanning = false;
            return;
        }

        // Camera controls:
        // - LMB: Tumble (handled in ModelingMode when click misses geometry)
        // - MMB: Pan

        // MMB pan (no modifier needed)
        if (Input::isMouseButtonPressed(Input::MOUSE_MIDDLE)) {
            m_isPanning = true;
            m_lastMousePos = currentMousePos;
        }

        // Stop operations when buttons released
        // Tumble can be started with LMB (on empty space) or RMB (anywhere)
        if (!Input::isMouseButtonDown(Input::MOUSE_LEFT) && !Input::isMouseButtonDown(Input::MOUSE_RIGHT)) {
            m_isTumbling = false;
        }
        if (!Input::isMouseButtonDown(Input::MOUSE_MIDDLE)) m_isPanning = false;

        // Calculate mouse delta
        glm::vec2 mouseDelta = currentMousePos - m_lastMousePos;

        // Note: Orbit target is set when framing (F key) or during pan
        // We don't auto-update it to follow the object as that causes camera snapping

        // Get orbit distance (distance from camera to target)
        float orbitDistance = glm::length(activeCamera.getPosition() - m_orbitTarget);
        if (orbitDistance < 0.01f) orbitDistance = 5.0f;  // Minimum distance

        if (isPerspective) {
            // LMB: Tumble (orbit around target) - started from ModelingMode when click misses geometry
            if (m_isTumbling) {
                // On first frame of tumbling, store the orbit target and distance
                if (!m_wasTumbling) {
                    m_lastMousePos = currentMousePos;
                    mouseDelta = glm::vec2(0.0f);
                    m_tumbleOrbitTarget = m_orbitTarget;
                    m_tumbleOrbitDistance = glm::length(activeCamera.getPosition() - m_orbitTarget);
                    if (m_tumbleOrbitDistance < 0.01f) m_tumbleOrbitDistance = 5.0f;
                }

                float sensitivity = 0.25f;

                if (m_mouseLookMode) {
                    // Mouse-look: rotate camera in place
                    float currentYaw = activeCamera.getYaw();
                    float currentPitch = activeCamera.getPitch();

                    currentYaw += mouseDelta.x * sensitivity;
                    currentPitch -= mouseDelta.y * sensitivity;  // Inverted for natural feel

                    // Clamp pitch to avoid gimbal lock
                    currentPitch = std::clamp(currentPitch, -89.0f, 89.0f);

                    activeCamera.setYaw(currentYaw);
                    activeCamera.setPitch(currentPitch);

                    // Update orbit target to be in front of camera (for F key framing later)
                    glm::vec3 forward = activeCamera.getFront();
                    m_orbitTarget = activeCamera.getPosition() + forward * 5.0f;
                } else {
                    // Orbit: rotate around target point
                    // Update orbit angles based on mouse movement
                    m_orbitYaw += mouseDelta.x * sensitivity;
                    m_orbitPitch += mouseDelta.y * sensitivity;

                    // Clamp pitch to avoid gimbal lock
                    m_orbitPitch = std::clamp(m_orbitPitch, -89.0f, 89.0f);

                    // Calculate new camera position orbiting around stored target
                    float yawRad = glm::radians(m_orbitYaw);
                    float pitchRad = glm::radians(m_orbitPitch);

                    glm::vec3 offset;
                    offset.x = m_tumbleOrbitDistance * cos(pitchRad) * cos(yawRad);
                    offset.y = m_tumbleOrbitDistance * sin(pitchRad);
                    offset.z = m_tumbleOrbitDistance * cos(pitchRad) * sin(yawRad);

                    activeCamera.setPosition(m_tumbleOrbitTarget + offset);

                    // Make camera look at target
                    glm::vec3 lookDir = glm::normalize(m_tumbleOrbitTarget - activeCamera.getPosition());
                    float camYaw = glm::degrees(atan2(lookDir.z, lookDir.x));
                    float camPitch = glm::degrees(asin(glm::clamp(lookDir.y, -1.0f, 1.0f)));
                    activeCamera.setYaw(camYaw);
                    activeCamera.setPitch(camPitch);
                }
            }

            // MMB: Pan (track camera and target together)
            if (m_isPanning) {
                float panSpeed = orbitDistance * 0.002f * (m_cameraSpeed * 10.0f);

                glm::vec3 right = activeCamera.getRight();
                glm::vec3 up = activeCamera.getUp();

                glm::vec3 panOffset = -right * mouseDelta.x * panSpeed + up * mouseDelta.y * panSpeed;

                activeCamera.setPosition(activeCamera.getPosition() + panOffset);
                m_orbitTarget += panOffset;
            }
        } else {
            // Ortho camera: Pan with MMB
            if (m_isPanning) {
                float panSpeed = activeCamera.getOrthoSize() * 0.003f * (m_cameraSpeed * 10.0f);
                glm::vec3 right = activeCamera.getRight();
                glm::vec3 up = activeCamera.getUp();

                glm::vec3 panOffset = -right * mouseDelta.x * panSpeed + up * mouseDelta.y * panSpeed;
                activeCamera.setPosition(activeCamera.getPosition() + panOffset);
                m_orbitTarget += panOffset;
            }
        }

        m_lastMousePos = currentMousePos;
        m_isLooking = m_isTumbling || m_isPanning;
        m_wasTumbling = m_isTumbling;
    }

    void frameSelected(Camera& camera) {
        // Frame the selected object, or origin if nothing selected
        glm::vec3 targetPos = glm::vec3(0.0f);
        float frameDistance = 5.0f;

        if (m_selectedObject) {
            targetPos = m_selectedObject->getTransform().getPosition();

            // Try to calculate bounding sphere for better framing
            const auto& vertices = m_selectedObject->getVertices();
            if (!vertices.empty()) {
                // Find bounding box
                glm::vec3 minBounds(FLT_MAX), maxBounds(-FLT_MAX);
                for (const auto& v : vertices) {
                    minBounds = glm::min(minBounds, v.position);
                    maxBounds = glm::max(maxBounds, v.position);
                }
                targetPos = (minBounds + maxBounds) * 0.5f;
                float radius = glm::length(maxBounds - minBounds) * 0.5f;
                frameDistance = radius * 2.5f;  // Frame with some padding
                if (frameDistance < 1.0f) frameDistance = 1.0f;
            }
        }

        m_orbitTarget = targetPos;

        if (camera.getProjectionMode() == ProjectionMode::Perspective) {
            // Move camera to frame the object
            glm::vec3 forward = camera.getFront();
            camera.setPosition(targetPos - forward * frameDistance);
        } else {
            // For ortho, center on target and adjust size
            camera.setPosition(targetPos - camera.getFront() * 10.0f);
            camera.setOrthoSize(frameDistance);
        }
    }

    void renderSceneToViewport(VkCommandBuffer cmd, Camera& camera, float vpX, float vpY, float vpW, float vpH) {
        // Set viewport
        VkViewport viewport{};
        viewport.x = vpX;
        viewport.y = vpY;
        viewport.width = vpW;
        viewport.height = vpH;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {static_cast<int32_t>(vpX), static_cast<int32_t>(vpY)};
        scissor.extent = {static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Get view-projection matrix
        float aspectRatio = vpW / vpH;
        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 proj = camera.getProjectionMatrix(aspectRatio);
        proj[1][1] *= -1;  // Flip Y for Vulkan coordinate system
        glm::mat4 viewProj = proj * view;

        // Delegate to active mode for rendering
        if (m_activeMode) {
            m_activeMode->renderSceneOverlay(cmd, viewProj);
        }
    }

    void renderUI() {
        ImGui::NewFrame();

        // Create dockspace over the entire viewport (below menu bar)
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGuiWindowFlags dockspaceFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("DockSpaceWindow", nullptr, dockspaceFlags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();

        // Main menu bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Model...", "Ctrl+O")) {
                    openModelDialog();
                }
                if (ImGui::MenuItem("Load OBJ...")) {
                    if (m_modelingMode) {
                        m_modelingMode->loadOBJFile();
                    }
                }
                if (ImGui::MenuItem("Load LIME...")) {
                    if (m_modelingMode) {
                        m_modelingMode->loadLimeFile();
                    }
                }
                ImGui::Separator();
                bool hasScene = m_modelingMode && m_editorContext && !m_editorContext->sceneObjects.empty();
                if (ImGui::MenuItem("Save Scene (.limes)...", nullptr, false, hasScene)) {
                    if (m_modelingMode) {
                        m_modelingMode->saveLimeScene();
                    }
                }
                if (ImGui::MenuItem("Load Scene (.limes)...")) {
                    if (m_modelingMode) {
                        m_modelingMode->loadLimeScene();
                    }
                }
                ImGui::Separator();
                bool hasEditableMesh = m_modelingMode && m_editorContext && m_editorContext->editableMesh.isValid();
                if (ImGui::MenuItem("Save as LIME...", "Ctrl+S", false, hasEditableMesh)) {
                    if (m_modelingMode) {
                        m_modelingMode->saveEditableMeshAsLime();
                    }
                }
                if (ImGui::MenuItem("Save as OBJ...", nullptr, false, hasEditableMesh)) {
                    if (m_modelingMode) {
                        m_modelingMode->saveEditableMeshAsOBJ();
                    }
                }
                if (ImGui::MenuItem("Save as GLB...", nullptr, false, hasEditableMesh)) {
                    if (m_modelingMode) {
                        m_modelingMode->saveEditableMeshAsGLB();
                    }
                }
                bool hasTexture = m_selectedObject && m_selectedObject->hasTextureData();
                if (ImGui::MenuItem("Export Texture as PNG...", nullptr, false, hasTexture)) {
                    if (m_modelingMode) {
                        m_modelingMode->exportTextureAsPNG();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    glfwSetWindowShouldClose(getWindow().getHandle(), GLFW_TRUE);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Add")) {
                if (ImGui::MenuItem("Cube")) {
                    createTestCube();
                }
                if (ImGui::MenuItem("Quad")) {
                    createTestQuad();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Cylinder...")) {
                    m_showCylinderDialog = true;
                }
                if (ImGui::MenuItem("Sphere...")) {
                    m_showSphereDialog = true;
                }
                if (ImGui::MenuItem("Cube Ring...")) {
                    m_showCubeRingDialog = true;
                }
                if (ImGui::MenuItem("Cube Arch...")) {
                    m_showCubeArchDialog = true;
                }
                if (ImGui::MenuItem("Cube Column...")) {
                    m_showCubeColumnDialog = true;
                }
                if (ImGui::MenuItem("Cube Stairs...")) {
                    m_showCubeStairsDialog = true;
                }
                if (ImGui::MenuItem("Cube Sphere...")) {
                    m_showCubeSphereDialog = true;
                }
                if (ImGui::MenuItem("Extruded Sphere...")) {
                    m_showExtrudedSphereDialog = true;
                }
                if (ImGui::MenuItem("Cube Room...")) {
                    m_showCubeRoomDialog = true;
                }
                if (ImGui::MenuItem("Cube Block...")) {
                    m_showCubeBlockDialog = true;
                }
                if (ImGui::MenuItem("Block Plate...")) {
                    m_showBlockPlateDialog = true;
                }
                if (ImGui::MenuItem("Head")) {
                    createHead(1.0f);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Unit Beam")) {
                    createUnitBeam();
                }
                if (ImGui::MenuItem("4m Post")) {
                    create4mPost();
                }
                if (ImGui::BeginMenu("Panels")) {
                    if (ImGui::MenuItem("1m x 4m Panel")) {
                        createPanel(1.0f, 4.0f, 0.075f, "Panel_1x4");
                    }
                    if (ImGui::MenuItem("2m x 4m Panel")) {
                        createPanel(2.0f, 4.0f, 0.075f, "Panel_2x4");
                    }
                    if (ImGui::MenuItem("3m x 4m Panel")) {
                        createPanel(3.0f, 4.0f, 0.075f, "Panel_3x4");
                    }
                    if (ImGui::MenuItem("4m x 4m Panel")) {
                        createPanel(4.0f, 4.0f, 0.075f, "Panel_4x4");
                    }
                    if (ImGui::MenuItem("5m x 4m Panel")) {
                        createPanel(5.0f, 4.0f, 0.075f, "Panel_5x4");
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Mode")) {
                if (ImGui::MenuItem("Modeling Editor", "Ctrl+1", m_currentModeType == EditorModeType::ModelingEditor)) {
                    switchMode(EditorModeType::ModelingEditor);
                }
                if (ImGui::MenuItem("Animation", "Ctrl+2", m_currentModeType == EditorModeType::AnimationCombiner)) {
                    switchMode(EditorModeType::AnimationCombiner);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Scene", nullptr, &m_showSceneWindow);
                ImGui::MenuItem("Tools", nullptr, &m_showToolsWindow);
                ImGui::MenuItem("UV Editor", nullptr, &m_showUVWindow);
                ImGui::MenuItem("Camera", nullptr, &m_showCameraWindow);
                ImGui::MenuItem("Image References", nullptr, &m_showImageRefWindow);
                ImGui::MenuItem("Library", nullptr, &m_showLibraryWindow);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("UV")) {
                if (ImGui::MenuItem("Auto-UV Cubes", "U")) {
                    autoUVSelectedObject();
                }
                ImGui::EndMenu();
            }

            // Mode indicator
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 200);
            if (m_activeMode) {
                ImGui::TextDisabled("Mode: %s", m_activeMode->getName());
            }

            ImGui::EndMainMenuBar();
        }

        // Cylinder dialog
        if (m_showCylinderDialog) {
            ImGui::OpenPopup("Add Cylinder");
            m_showCylinderDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cylinder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cylinder Parameters");
            ImGui::Separator();

            ImGui::SliderFloat("Radius", &m_cylinderRadius, 0.1f, 5.0f, "%.2f");
            ImGui::SliderFloat("Height", &m_cylinderHeight, 0.1f, 10.0f, "%.2f");
            ImGui::SliderInt("Segments", &m_cylinderSegments, 3, 64);
            ImGui::SliderInt("Divisions", &m_cylinderDivisions, 1, 32);
            ImGui::Checkbox("Caps", &m_cylinderCaps);
            if (m_cylinderCaps) {
                ImGui::SliderInt("Cap Rings", &m_cylinderCapRings, 1, 8);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Number of concentric quad rings in caps.\n2+ allows edge loop insertion on caps.");
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCylinder();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Sphere dialog
        if (m_showSphereDialog) {
            ImGui::OpenPopup("Add Sphere");
            m_showSphereDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Sphere", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Sphere Parameters");
            ImGui::Separator();

            ImGui::SliderFloat("Radius", &m_sphereRadius, 0.1f, 5.0f, "%.2f");
            ImGui::SliderInt("Rings", &m_sphereRings, 3, 32);
            ImGui::SliderInt("Segments", &m_sphereSegments, 3, 64);

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createSphere();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Ring dialog
        if (m_showCubeRingDialog) {
            ImGui::OpenPopup("Add Cube Ring");
            m_showCubeRingDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Ring", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Ring Parameters");
            ImGui::Separator();

            ImGui::SliderInt("Segments", &m_cubeRingSegments, 3, 32);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of cube segments around the ring");
            }
            ImGui::SliderFloat("Inner Radius", &m_cubeRingInnerRadius, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Outer Radius", &m_cubeRingOuterRadius, 0.1f, 5.0f, "%.2f");
            // Ensure outer > inner
            if (m_cubeRingOuterRadius <= m_cubeRingInnerRadius) {
                m_cubeRingOuterRadius = m_cubeRingInnerRadius + 0.1f;
            }
            ImGui::SliderFloat("Height", &m_cubeRingHeight, 0.1f, 10.0f, "%.2f");

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeRing();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Arch dialog
        if (m_showCubeArchDialog) {
            ImGui::OpenPopup("Add Cube Arch");
            m_showCubeArchDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Arch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Arch Parameters");
            ImGui::Separator();

            ImGui::SliderInt("Segments", &m_cubeArchSegments, 3, 32);
            ImGui::SliderFloat("Inner Radius", &m_cubeArchInnerRadius, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Outer Radius", &m_cubeArchOuterRadius, 0.1f, 5.0f, "%.2f");
            if (m_cubeArchOuterRadius <= m_cubeArchInnerRadius) {
                m_cubeArchOuterRadius = m_cubeArchInnerRadius + 0.1f;
            }
            ImGui::SliderFloat("Depth", &m_cubeArchDepth, 0.1f, 5.0f, "%.2f");
            ImGui::SliderFloat("Arc Degrees", &m_cubeArchArcDegrees, 30.0f, 360.0f, "%.1f");

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeArch(m_cubeArchSegments, m_cubeArchInnerRadius, m_cubeArchOuterRadius, m_cubeArchDepth, m_cubeArchArcDegrees);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Column dialog
        if (m_showCubeColumnDialog) {
            ImGui::OpenPopup("Add Cube Column");
            m_showCubeColumnDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Column", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Column Parameters");
            ImGui::Separator();

            ImGui::SliderInt("Segments", &m_cubeColumnSegments, 3, 32);
            ImGui::SliderFloat("Radius", &m_cubeColumnRadius, 0.1f, 5.0f, "%.2f");
            ImGui::SliderFloat("Height", &m_cubeColumnHeight, 0.1f, 10.0f, "%.2f");

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeColumn(m_cubeColumnSegments, m_cubeColumnRadius, m_cubeColumnHeight);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Stairs dialog
        if (m_showCubeStairsDialog) {
            ImGui::OpenPopup("Add Cube Stairs");
            m_showCubeStairsDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Stairs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Stairs Parameters");
            ImGui::Separator();

            ImGui::SliderInt("Steps", &m_cubeStairsSteps, 1, 20);
            ImGui::SliderFloat("Width", &m_cubeStairsWidth, 0.1f, 5.0f, "%.2f");
            ImGui::SliderFloat("Step Height", &m_cubeStairsStepHeight, 0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Step Depth", &m_cubeStairsStepDepth, 0.1f, 2.0f, "%.2f");

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeStairs(m_cubeStairsSteps, m_cubeStairsWidth, m_cubeStairsStepHeight, m_cubeStairsStepDepth);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Sphere dialog
        if (m_showCubeSphereDialog) {
            ImGui::OpenPopup("Add Cube Sphere");
            m_showCubeSphereDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Sphere", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Sphere Parameters");
            ImGui::Separator();

            ImGui::SliderFloat("Radius", &m_cubeSphereRadius, 0.5f, 10.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Radius of the sphere");
            }

            ImGui::Checkbox("Solid Shell", &m_cubeSphereSolidShell);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cubes sized to touch each other, forming a continuous shell");
            }

            if (!m_cubeSphereSolidShell) {
                ImGui::SliderFloat("Cube Size", &m_cubeSphereCubeSize, 0.1f, 2.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Size of each cube (only used when Solid Shell is off)");
                }
            }

            ImGui::SliderInt("Rings", &m_cubeSphereRings, 2, 24);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of latitude rings (excluding poles)");
            }
            ImGui::SliderInt("Segments", &m_cubeSphereSegments, 4, 48);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of longitude segments");
            }
            ImGui::Checkbox("Interior (view from inside)", &m_cubeSphereInterior);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Flip normals for viewing from inside the sphere");
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeSphere();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Extruded Sphere dialog
        if (m_showExtrudedSphereDialog) {
            ImGui::OpenPopup("Add Extruded Sphere");
            m_showExtrudedSphereDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Extruded Sphere", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Extruded Sphere Parameters");
            ImGui::Text("(Each face of a UV sphere extruded outward)");
            ImGui::Separator();

            ImGui::SliderFloat("Radius", &m_extrudedSphereRadius, 0.5f, 10.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Inner radius of the sphere");
            }
            ImGui::SliderFloat("Thickness", &m_extrudedSphereThickness, 0.05f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far each face is extruded outward");
            }
            ImGui::SliderInt("Rings", &m_extrudedSphereRings, 2, 24);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of latitude divisions");
            }
            ImGui::SliderInt("Segments", &m_extrudedSphereSegments, 4, 48);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Number of longitude divisions");
            }
            ImGui::Checkbox("Interior (view from inside)", &m_extrudedSphereInterior);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Flip normals for viewing from inside the sphere");
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createExtrudedSphere();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Room dialog
        if (m_showCubeRoomDialog) {
            ImGui::OpenPopup("Add Cube Room");
            m_showCubeRoomDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Room", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Room Parameters");
            ImGui::Text("(Interior room made of cubes with window)");
            ImGui::Separator();

            ImGui::SliderInt("Width", &m_cubeRoomWidth, 3, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Room width in cubes");
            }
            ImGui::SliderInt("Height", &m_cubeRoomHeight, 2, 10);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Room height in cubes");
            }
            ImGui::SliderInt("Depth", &m_cubeRoomDepth, 3, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Room depth in cubes");
            }
            ImGui::SliderFloat("Cube Size", &m_cubeRoomCubeSize, 0.1f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Size of each cube");
            }
            ImGui::SliderInt("Window Width", &m_cubeRoomWindowFront, 0, m_cubeRoomWidth - 2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Width of window opening in front wall (0 = no window)");
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeRoom(m_cubeRoomWidth, m_cubeRoomHeight, m_cubeRoomDepth, m_cubeRoomCubeSize, m_cubeRoomWindowFront);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Cube Block dialog
        if (m_showCubeBlockDialog) {
            ImGui::OpenPopup("Add Cube Block");
            m_showCubeBlockDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Cube Block", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Cube Block Parameters");
            ImGui::Text("(Solid rectangular block made of cubes)");
            ImGui::Separator();

            ImGui::SliderInt("Width", &m_cubeBlockWidth, 1, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Block width in cubes (X axis)");
            }
            ImGui::SliderInt("Height", &m_cubeBlockHeight, 1, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Block height in cubes (Y axis)");
            }
            ImGui::SliderInt("Depth", &m_cubeBlockDepth, 1, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Block depth in cubes (Z axis)");
            }
            ImGui::SliderFloat("Cube Size", &m_cubeBlockCubeSize, 0.1f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Size of each cube");
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createCubeBlock();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Block Plate dialog
        if (m_showBlockPlateDialog) {
            ImGui::OpenPopup("Add Block Plate");
            m_showBlockPlateDialog = false;
        }
        if (ImGui::BeginPopupModal("Add Block Plate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Block Plate Parameters");
            ImGui::Text("(A flat wall of blocks)");
            ImGui::Separator();

            ImGui::SliderInt("Width", &m_blockPlateWidth, 1, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Plate width in blocks (X axis)");
            }
            ImGui::SliderInt("Height", &m_blockPlateHeight, 1, 20);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Plate height in blocks (Y axis)");
            }
            ImGui::SliderFloat("Block Size", &m_blockPlateCubeSize, 0.1f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Size of each block");
            }

            ImGui::Separator();
            ImGui::Checkbox("Beveled", &m_blockPlateBeveled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Add beveled/chamfered edges to each block");
            }

            if (m_blockPlateBeveled) {
                ImGui::SliderFloat("Bevel Amount", &m_blockPlateBevelAmount, 0.05f, 0.4f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Size of bevel as fraction of block half-size");
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                createBlockPlate();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Library panel
        if (m_showLibraryWindow) {
            ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Library", &m_showLibraryWindow)) {
                // Scan library folder if needed
                if (m_libraryNeedsRefresh) {
                    scanLibraryFolder();
                    m_libraryNeedsRefresh = false;
                }

                // Refresh button and stats
                if (ImGui::Button("Refresh")) {
                    m_libraryNeedsRefresh = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu items, %zu categories)", m_libraryItems.size(), m_libraryCategories.size());

                ImGui::Separator();

                // Save to Library section
                if (m_selectedObject) {
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Save to Library:");

                    // Name input
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputText("##libname", m_librarySaveNameBuffer, sizeof(m_librarySaveNameBuffer));
                    ImGui::SameLine();

                    // Category dropdown for saving
                    static int saveCategoryIndex = 0;
                    ImGui::SetNextItemWidth(100);
                    if (ImGui::BeginCombo("##savecat", saveCategoryIndex == 0 ? "(root)" :
                            m_libraryCategories[saveCategoryIndex - 1].c_str())) {
                        if (ImGui::Selectable("(root)", saveCategoryIndex == 0)) {
                            saveCategoryIndex = 0;
                        }
                        for (size_t i = 0; i < m_libraryCategories.size(); ++i) {
                            if (ImGui::Selectable(m_libraryCategories[i].c_str(), saveCategoryIndex == static_cast<int>(i + 1))) {
                                saveCategoryIndex = static_cast<int>(i + 1);
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // Widget Properties (metadata for wall machine system)
                    if (ImGui::TreeNode("Widget Properties")) {
                        static const char* widgetTypes[] = {
                            "None", "Machine", "Input", "Output", "Button", "Checkbox", "Slider", "Log"
                        };
                        ImGui::SetNextItemWidth(120);
                        ImGui::Combo("Widget Type", &m_widgetTypeIndex, widgetTypes, IM_ARRAYSIZE(widgetTypes));

                        if (m_widgetTypeIndex == 1) { // Machine
                            ImGui::SetNextItemWidth(160);
                            ImGui::InputTextWithHint("##machname", "e.g. hunyuan3d", m_widgetMachineName, sizeof(m_widgetMachineName));
                            ImGui::SameLine();
                            ImGui::TextDisabled("Machine Name");
                        } else if (m_widgetTypeIndex >= 2) { // Input, Output, Button, etc.
                            ImGui::SetNextItemWidth(160);
                            ImGui::InputTextWithHint("##paramname", "e.g. image, generate", m_widgetParamName, sizeof(m_widgetParamName));
                            ImGui::SameLine();
                            ImGui::TextDisabled("Param Name");
                        }

                        // CP naming guide
                        ImGui::TextDisabled("Control point naming:");
                        ImGui::BulletText("port:name  - wire connection point");
                        ImGui::BulletText("btn:name   - built-in button");
                        ImGui::BulletText("screen:name - built-in display");
                        ImGui::BulletText("slider:name - built-in slider");
                        ImGui::BulletText("checkbox:name - built-in toggle");

                        ImGui::TreePop();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Save")) {
                        // Write widget metadata into the mesh before saving
                        m_editableMesh.clearMetadata();
                        if (m_widgetTypeIndex > 0) {
                            static const char* typeKeys[] = {
                                "", "machine", "input", "output", "button", "checkbox", "slider", "log"
                            };
                            m_editableMesh.setMetadata("widget_type", typeKeys[m_widgetTypeIndex]);
                            if (m_widgetTypeIndex == 1 && m_widgetMachineName[0] != '\0') {
                                m_editableMesh.setMetadata("machine_name", m_widgetMachineName);
                            } else if (m_widgetTypeIndex >= 2 && m_widgetParamName[0] != '\0') {
                                m_editableMesh.setMetadata("param_name", m_widgetParamName);
                            }
                        }
                        std::string cat = (saveCategoryIndex == 0) ? "" : m_libraryCategories[saveCategoryIndex - 1];
                        saveToLibrary(cat);
                    }

                    // New category input
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputTextWithHint("##newcat", "New category", m_libraryNewCategoryBuffer, sizeof(m_libraryNewCategoryBuffer));
                    ImGui::SameLine();
                    if (ImGui::Button("Add##cat")) {
                        if (strlen(m_libraryNewCategoryBuffer) > 0) {
                            std::string newCat = m_libraryNewCategoryBuffer;
                            std::string catPath = m_libraryPath + "/" + newCat;
                            if (!fs::exists(catPath)) {
                                fs::create_directories(catPath);
                                m_libraryNeedsRefresh = true;
                            }
                            memset(m_libraryNewCategoryBuffer, 0, sizeof(m_libraryNewCategoryBuffer));
                        }
                    }

                    ImGui::Separator();
                }

                // Category filter tabs
                if (ImGui::BeginTabBar("CategoryTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
                    // "All" tab
                    if (ImGui::BeginTabItem("All")) {
                        m_selectedCategory = 0;
                        ImGui::EndTabItem();
                    }

                    // Category tabs
                    for (size_t i = 0; i < m_libraryCategories.size(); ++i) {
                        if (ImGui::BeginTabItem(m_libraryCategories[i].c_str())) {
                            m_selectedCategory = static_cast<int>(i + 1);
                            ImGui::EndTabItem();
                        }
                    }

                    // Uncategorized tab (if there are root items)
                    bool hasUncategorized = false;
                    for (const auto& item : m_libraryItems) {
                        if (item.category.empty()) { hasUncategorized = true; break; }
                    }
                    if (hasUncategorized) {
                        if (ImGui::BeginTabItem("(uncategorized)")) {
                            m_selectedCategory = -1;  // Special value for uncategorized
                            ImGui::EndTabItem();
                        }
                    }

                    ImGui::EndTabBar();
                }

                // Library items grid
                ImGui::BeginChild("LibraryItems", ImVec2(0, 0), true);

                float windowWidth = ImGui::GetContentRegionAvail().x;
                float itemSize = 70.0f;
                int columns = std::max(1, static_cast<int>(windowWidth / (itemSize + 8)));

                int col = 0;
                for (size_t i = 0; i < m_libraryItems.size(); ++i) {
                    LibraryItem& item = m_libraryItems[i];

                    // Filter by category
                    bool show = false;
                    if (m_selectedCategory == 0) {
                        show = true;  // Show all
                    } else if (m_selectedCategory == -1) {
                        show = item.category.empty();  // Uncategorized only
                    } else {
                        show = (item.category == m_libraryCategories[m_selectedCategory - 1]);
                    }

                    if (!show) continue;

                    ImGui::PushID(static_cast<int>(i));

                    // Lazy load thumbnail when item becomes visible
                    if (!item.thumbnailLoaded && !item.thumbnailPath.empty()) {
                        loadLibraryThumbnail(item);
                    }

                    // Button for the item (with thumbnail if available)
                    ImGui::BeginGroup();
                    bool clicked = false;
                    if (item.thumbnailLoaded && item.descriptor != VK_NULL_HANDLE) {
                        // Use ImageButton with the thumbnail texture
                        clicked = ImGui::ImageButton("##item", (ImTextureID)item.descriptor,
                            ImVec2(itemSize, itemSize));
                    } else {
                        // Fallback to regular button
                        clicked = ImGui::Button("##item", ImVec2(itemSize, itemSize));
                    }

                    if (clicked) {
                        loadFromLibrary(item.filepath);
                    }
                    if (ImGui::IsItemHovered()) {
                        std::string tooltip = item.name;
                        if (!item.category.empty()) {
                            tooltip += "\n[" + item.category + "]";
                        }
                        tooltip += "\nClick to insert";
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }

                    // Draw name below button (truncated)
                    std::string displayName = item.name;
                    if (displayName.length() > 9) {
                        displayName = displayName.substr(0, 7) + "..";
                    }
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemSize);
                    ImGui::TextWrapped("%s", displayName.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndGroup();

                    ImGui::PopID();

                    col++;
                    if (col < columns) {
                        ImGui::SameLine();
                    } else {
                        col = 0;
                    }
                }

                ImGui::EndChild();
            }
            ImGui::End();
        }

        // Delegate UI rendering to active mode
        if (m_activeMode) {
            m_activeMode->renderUI();
        }

        // Draw overlays
        if (m_activeMode) {
            float screenWidth = static_cast<float>(getSwapchain().getExtent().width);
            float screenHeight = static_cast<float>(getSwapchain().getExtent().height);

            if (m_splitView && m_activeMode->supportsSplitView()) {
                // Draw overlays for left viewport (perspective)
                m_activeMode->drawOverlays(0, 0, screenWidth / 2.0f, screenHeight);
                // Draw overlays for right viewport (ortho) - reference images go here
                m_activeMode->drawOverlays(screenWidth / 2.0f, 0, screenWidth / 2.0f, screenHeight);
            } else {
                m_activeMode->drawOverlays(0, 0, screenWidth, screenHeight);
            }
        }

        ImGui::Render();
    }

    void scanLibraryFolder() {
        // Cleanup existing thumbnail resources before clearing
        for (auto& item : m_libraryItems) {
            cleanupLibraryThumbnail(item);
        }
        m_libraryItems.clear();
        m_libraryCategories.clear();

        if (!fs::exists(m_libraryPath)) {
            std::cout << "Library folder does not exist: " << m_libraryPath << std::endl;
            // Create default categories
            fs::create_directories(m_libraryPath + "/walls");
            fs::create_directories(m_libraryPath + "/floors");
            fs::create_directories(m_libraryPath + "/props");
            fs::create_directories(m_libraryPath + "/structures");
            fs::create_directories(m_libraryPath + "/misc");
            std::cout << "Created default library categories" << std::endl;
        }

        // Scan root folder for uncategorized items
        for (const auto& entry : fs::directory_iterator(m_libraryPath)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".lime") {
                    LibraryItem item;
                    item.filepath = entry.path().string();
                    item.name = entry.path().stem().string();
                    item.category = "";  // Uncategorized
                    // Set thumbnail path (same name but .png extension)
                    item.thumbnailPath = entry.path().parent_path().string() + "/" + item.name + ".png";
                    m_libraryItems.push_back(item);
                }
            }
        }

        // Scan subfolders (categories)
        for (const auto& entry : fs::directory_iterator(m_libraryPath)) {
            if (entry.is_directory()) {
                std::string categoryName = entry.path().filename().string();
                m_libraryCategories.push_back(categoryName);

                // Scan .lime files in this category
                for (const auto& fileEntry : fs::directory_iterator(entry.path())) {
                    if (fileEntry.is_regular_file()) {
                        std::string ext = fileEntry.path().extension().string();
                        if (ext == ".lime") {
                            LibraryItem item;
                            item.filepath = fileEntry.path().string();
                            item.name = fileEntry.path().stem().string();
                            item.category = categoryName;
                            // Set thumbnail path (same name but .png extension)
                            item.thumbnailPath = fileEntry.path().parent_path().string() + "/" + item.name + ".png";
                            m_libraryItems.push_back(item);
                        }
                    }
                }
            }
        }

        // Sort categories alphabetically
        std::sort(m_libraryCategories.begin(), m_libraryCategories.end());

        // Sort items alphabetically within categories
        std::sort(m_libraryItems.begin(), m_libraryItems.end(), [](const LibraryItem& a, const LibraryItem& b) {
            if (a.category != b.category) return a.category < b.category;
            return a.name < b.name;
        });

        std::cout << "Library scan: found " << m_libraryItems.size() << " items in "
                  << m_libraryCategories.size() << " categories" << std::endl;
    }

    void generateThumbnail(const std::string& thumbnailPath) {
        // Capture the current screen as the thumbnail
        VkDevice device = getContext().getDevice();
        auto& swapchain = getSwapchain();
        VkExtent2D extent = swapchain.getExtent();

        // Wait for GPU to finish
        vkDeviceWaitIdle(device);

        // Get the current swapchain image (use image 0 since we just finished rendering)
        const auto& images = swapchain.getImages();
        if (images.empty()) {
            std::cout << "No swapchain images available for thumbnail" << std::endl;
            return;
        }
        VkImage srcImage = images[0];

        // Create staging buffer for readback
        VkDeviceSize bufferSize = extent.width * extent.height * 4;
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = getContext().findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        // Copy image to buffer
        VkCommandBuffer cmd = getContext().beginSingleTimeCommands();

        // Transition image to transfer source
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = srcImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy image to buffer
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {extent.width, extent.height, 1};

        vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

        // Transition back to present
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        getContext().endSingleTimeCommands(cmd);

        // Read pixels from staging buffer
        void* data;
        vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);

        // Swapchain format is typically BGRA, need to convert to RGBA
        std::vector<unsigned char> screenPixels(extent.width * extent.height * 4);
        unsigned char* srcPixels = static_cast<unsigned char*>(data);

        for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
            screenPixels[i * 4 + 0] = srcPixels[i * 4 + 2];  // R <- B
            screenPixels[i * 4 + 1] = srcPixels[i * 4 + 1];  // G <- G
            screenPixels[i * 4 + 2] = srcPixels[i * 4 + 0];  // B <- R
            screenPixels[i * 4 + 3] = 255;                    // A = opaque
        }

        vkUnmapMemory(device, stagingMemory);

        // Cleanup staging resources
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        // Downsample to thumbnail size
        std::vector<unsigned char> thumbnail(THUMBNAIL_SIZE * THUMBNAIL_SIZE * 4);
        int srcW = extent.width;
        int srcH = extent.height;

        for (int y = 0; y < THUMBNAIL_SIZE; ++y) {
            for (int x = 0; x < THUMBNAIL_SIZE; ++x) {
                int srcX = (x * srcW) / THUMBNAIL_SIZE;
                int srcY = (y * srcH) / THUMBNAIL_SIZE;
                srcX = std::min(srcX, srcW - 1);
                srcY = std::min(srcY, srcH - 1);

                int srcIdx = (srcY * srcW + srcX) * 4;
                int dstIdx = (y * THUMBNAIL_SIZE + x) * 4;

                thumbnail[dstIdx + 0] = screenPixels[srcIdx + 0];
                thumbnail[dstIdx + 1] = screenPixels[srcIdx + 1];
                thumbnail[dstIdx + 2] = screenPixels[srcIdx + 2];
                thumbnail[dstIdx + 3] = screenPixels[srcIdx + 3];
            }
        }

        // Save as PNG
        stbi_write_png(thumbnailPath.c_str(), THUMBNAIL_SIZE, THUMBNAIL_SIZE, 4, thumbnail.data(), THUMBNAIL_SIZE * 4);
        std::cout << "Generated thumbnail from screen capture: " << thumbnailPath << std::endl;
    }

    void saveToLibrary(const std::string& category = "") {
        if (!m_selectedObject) return;

        std::string name = m_librarySaveNameBuffer;
        if (name.empty()) {
            name = m_selectedObject->getName();
        }

        // Sanitize filename
        for (char& c : name) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }

        // Build path with category
        std::string filepath;
        std::string thumbnailPath;
        if (category.empty()) {
            filepath = m_libraryPath + "/" + name + ".lime";
            thumbnailPath = m_libraryPath + "/" + name + ".png";
        } else {
            // Ensure category folder exists
            std::string categoryPath = m_libraryPath + "/" + category;
            if (!fs::exists(categoryPath)) {
                fs::create_directories(categoryPath);
            }
            filepath = categoryPath + "/" + name + ".lime";
            thumbnailPath = categoryPath + "/" + name + ".png";
        }

        // Load mesh into editable mesh from selected object
        if (m_selectedObject->hasEditableMeshData()) {
            const auto& storedVerts = m_selectedObject->getHEVertices();
            const auto& storedHE = m_selectedObject->getHEHalfEdges();
            const auto& storedFaces = m_selectedObject->getHEFaces();

            std::vector<HEVertex> heVerts;
            for (const auto& v : storedVerts) {
                heVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
            }
            std::vector<HalfEdge> heHalfEdges;
            for (const auto& he : storedHE) {
                heHalfEdges.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
            }
            std::vector<HEFace> heFaces;
            for (const auto& f : storedFaces) {
                heFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
            }
            m_editableMesh.setMeshData(heVerts, heHalfEdges, heFaces);
        }

        // Get transform from selected object
        glm::vec3 position = m_selectedObject->getTransform().getPosition();
        glm::quat rotation = m_selectedObject->getTransform().getRotation();
        glm::vec3 scale = m_selectedObject->getTransform().getScale();

        // Save with texture and transform
        if (m_selectedObject->hasTextureData()) {
            const auto& texData = m_selectedObject->getTextureData();
            int w = m_selectedObject->getTextureWidth();
            int h = m_selectedObject->getTextureHeight();
            m_editableMesh.saveLime(filepath, texData.data(), w, h, position, rotation, scale);
        } else {
            m_editableMesh.saveLime(filepath, nullptr, 0, 0, position, rotation, scale);
        }

        // Generate thumbnail
        generateThumbnail(thumbnailPath);

        std::cout << "Saved to library: " << filepath << std::endl;
        m_libraryNeedsRefresh = true;
        memset(m_librarySaveNameBuffer, 0, sizeof(m_librarySaveNameBuffer));
    }

    void loadFromLibrary(const std::string& filepath) {
        // Load the .lime file (with transform)
        std::vector<unsigned char> textureData;
        int texWidth = 0, texHeight = 0;
        glm::vec3 position(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if (!m_editableMesh.loadLime(filepath, textureData, texWidth, texHeight, position, rotation, scale)) {
            std::cout << "Failed to load from library: " << filepath << std::endl;
            return;
        }

        // Extract name from filepath
        std::string name = fs::path(filepath).stem().string();

        // Create scene object
        auto obj = std::make_unique<SceneObject>(name);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        // Create GPU resources
        uint32_t handle;
        if (!textureData.empty() && texWidth > 0 && texHeight > 0) {
            handle = m_modelRenderer->createModel(vertices, indices, textureData.data(), texWidth, texHeight);
            obj->setTextureData(textureData, texWidth, texHeight);
        } else {
            handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        }

        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        // Store half-edge data
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Sync control points from EditableMesh to SceneObject
        {
            const auto& cps = m_editableMesh.getControlPoints();
            if (!cps.empty()) {
                std::vector<SceneObject::StoredControlPoint> storedCPs;
                for (const auto& cp : cps) storedCPs.push_back({cp.vertexIndex, cp.name});
                obj->setControlPoints(storedCPs);
            }
        }
        // Sync ports from EditableMesh to SceneObject
        {
            const auto& ports = m_editableMesh.getPorts();
            if (!ports.empty()) {
                std::vector<SceneObject::StoredPort> storedPorts;
                for (const auto& p : ports) storedPorts.push_back({p.name, p.position, p.forward, p.up});
                obj->setPorts(storedPorts);
            }
        }

        obj->getTransform().setPosition(position);
        obj->getTransform().setRotation(rotation);
        obj->getTransform().setScale(scale);
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;

        // Restore widget properties UI from metadata
        const auto& meta = m_editableMesh.getMetadata();
        m_widgetTypeIndex = 0;
        memset(m_widgetParamName, 0, sizeof(m_widgetParamName));
        memset(m_widgetMachineName, 0, sizeof(m_widgetMachineName));
        auto wtIt = meta.find("widget_type");
        if (wtIt != meta.end()) {
            const char* typeKeys[] = {"machine", "input", "output", "button", "checkbox", "slider", "log"};
            for (int i = 0; i < 7; ++i) {
                if (wtIt->second == typeKeys[i]) { m_widgetTypeIndex = i + 1; break; }
            }
            auto mnIt = meta.find("machine_name");
            if (mnIt != meta.end()) {
                strncpy(m_widgetMachineName, mnIt->second.c_str(), sizeof(m_widgetMachineName) - 1);
            }
            auto pnIt = meta.find("param_name");
            if (pnIt != meta.end()) {
                strncpy(m_widgetParamName, pnIt->second.c_str(), sizeof(m_widgetParamName) - 1);
            }
        }

        std::cout << "Loaded from library: " << name
                  << " (scale: " << scale.x << ", " << scale.y << ", " << scale.z << ")" << std::endl;
    }

    void openModelDialog() {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filters[2] = {
            {"GLB Models", "glb"},
            {"OBJ Models", "obj"}
        };

        nfdresult_t result = NFD_OpenDialog(&outPath, filters, 2, nullptr);

        if (result == NFD_OKAY) {
            loadModel(outPath);
            NFD_FreePath(outPath);
        }
    }

    void loadModel(const std::string& path) {
        LoadResult loadResult = GLBLoader::load(path);

        if (!loadResult.success) {
            std::cerr << "Failed to load model: " << loadResult.error << std::endl;
            return;
        }

        // Store path for quick save (F5)
        m_currentFilePath = path;
        m_currentFileFormat = 3;  // GLB

        for (auto& mesh : loadResult.meshes) {
            auto obj = std::make_unique<SceneObject>(mesh.name);

            uint32_t handle = m_modelRenderer->createModel(
                mesh.vertices, mesh.indices,
                mesh.hasTexture ? mesh.texture.data.data() : nullptr,
                mesh.texture.width, mesh.texture.height
            );

            // Apply default mesh color to vertices if no texture
            if (!mesh.hasTexture) {
                for (auto& v : mesh.vertices) {
                    v.color = m_defaultMeshColor;
                }
            }

            obj->setBufferHandle(handle);
            obj->setIndexCount(static_cast<uint32_t>(mesh.indices.size()));
            obj->setVertexCount(static_cast<uint32_t>(mesh.vertices.size()));
            obj->setMeshData(mesh.vertices, mesh.indices);

            // Update GPU with recolored vertices
            if (!mesh.hasTexture) {
                m_modelRenderer->updateModelBuffer(handle, mesh.vertices);
            }

            // Restore half-edge data if available (from EDEN-exported GLB files)
            if (mesh.halfEdgeData.has_value()) {
                const auto& heData = mesh.halfEdgeData.value();
                obj->setEditableMeshData(heData.vertices, heData.halfEdges, heData.faces);
                std::cout << "Restored half-edge data: " << heData.faces.size() << " faces" << std::endl;
            }

            // Store texture data for painting
            if (mesh.hasTexture) {
                obj->setTextureData(mesh.texture.data, mesh.texture.width, mesh.texture.height);
                std::cout << "Loaded texture for painting: " << mesh.texture.width << "x" << mesh.texture.height << std::endl;
            }

            m_selectedObject = obj.get();
            m_sceneObjects.push_back(std::move(obj));
        }

        // Rebuild editable mesh if in modeling mode
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_modelingMode->buildEditableMeshFromObject();
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void startAIGeneration(const std::string& prompt, const std::string& imagePath) {
        if (m_aiGenerating) return;

        // Check server
        if (!m_hunyuanClient.isServerRunning()) {
            m_aiGenerateStatus = "Server not running (localhost:8081)";
            std::cerr << "[Hunyuan3D] Server not reachable" << std::endl;
            return;
        }

        // Get generation params from the modeling mode UI state
        int steps = 5, octreeRes = 256, maxFaces = 10000, seed = 12345, texSize = 1024;
        float guidance = 5.0f;
        bool texture = true, remBG = true, multiView = false;
        std::string leftPath, rightPath, backPath;
        if (m_modelingMode) {
            auto* mode = m_modelingMode.get();
            steps = mode->m_generateSteps;
            octreeRes = mode->m_generateOctreeRes;
            guidance = mode->m_generateGuidance;
            maxFaces = mode->m_generateMaxFaces;
            texture = mode->m_generateTexture;
            texSize = mode->m_generateTexSize;
            remBG = mode->m_generateRemBG;
            seed = mode->m_generateSeed;
            multiView = mode->m_generateMultiView;
            leftPath = mode->m_generateLeftPath;
            rightPath = mode->m_generateRightPath;
            backPath = mode->m_generateBackPath;
        }

        // Base64 encode front/single image
        std::string imageBase64;
        if (!imagePath.empty()) {
            imageBase64 = Hunyuan3DClient::base64EncodeFile(imagePath);
            if (imageBase64.empty()) {
                m_aiGenerateStatus = "Failed to read image file";
                return;
            }
        }

        // Base64 encode multi-view images (if provided)
        std::string leftBase64, rightBase64, backBase64;
        if (multiView) {
            if (!leftPath.empty())  leftBase64  = Hunyuan3DClient::base64EncodeFile(leftPath);
            if (!rightPath.empty()) rightBase64 = Hunyuan3DClient::base64EncodeFile(rightPath);
            if (!backPath.empty())  backBase64  = Hunyuan3DClient::base64EncodeFile(backPath);
        }

        // Send generation request
        std::string uid = m_hunyuanClient.startGeneration(
            prompt, imageBase64, steps, octreeRes, guidance, maxFaces, texture, seed, texSize, remBG,
            multiView, leftBase64, rightBase64, backBase64);
        if (uid.empty()) {
            m_aiGenerateStatus = "Failed to start generation";
            return;
        }

        m_aiGenerateJobUID = uid;
        m_aiGenerating = true;
        m_aiGenerateComplete = false;
        m_aiGenerateCancelled = false;
        m_aiGenerateStatus = "Generating...";

        // Reset log index for new generation
        m_aiLogIndex = 0;
        m_aiLogLines.clear();

        // Launch background polling thread
        m_aiGenerateThread = std::thread([this, uid]() {
            Hunyuan3DClient client("localhost", 8081);
            while (!m_aiGenerateCancelled) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (m_aiGenerateCancelled) break;

                // Fetch new log lines from server
                std::vector<std::string> newLines;
                int newTotal = client.fetchLog(m_aiLogIndex, newLines);
                if (newTotal >= 0) {
                    for (auto& line : newLines) {
                        m_aiLogLines.push_back(std::move(line));
                    }
                    m_aiLogIndex = newTotal;
                }

                std::string base64GLB;
                std::string status = client.checkStatus(uid, base64GLB);

                if (status == "completed" && !base64GLB.empty()) {
                    // Save GLB to models directory
                    auto now = std::chrono::system_clock::now();
                    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                        now.time_since_epoch()).count();
                    std::string outputDir = "models";
                    std::filesystem::create_directories(outputDir);
                    std::string outputPath = outputDir + "/ai_generated_" + std::to_string(timestamp) + ".glb";

                    if (Hunyuan3DClient::base64DecodeToFile(base64GLB, outputPath)) {
                        m_aiGeneratedGLBPath = outputPath;
                        m_aiGenerateComplete = true;
                        std::cout << "[Hunyuan3D] Model saved to: " << outputPath << std::endl;
                    } else {
                        m_aiGenerateStatus = "Failed to decode model data";
                        m_aiGenerating = false;
                    }
                    return;
                } else if (status == "error") {
                    m_aiGenerateStatus = "Server error during generation";
                    m_aiGenerating = false;
                    return;
                }
                // Still processing, continue polling
            }
            // Cancelled
            m_aiGenerating = false;
            m_aiGenerateStatus = "Cancelled";
        });
    }

    void cancelAIGeneration() {
        if (!m_aiGenerating) return;
        m_aiGenerateCancelled = true;
        if (m_aiGenerateThread.joinable()) {
            m_aiGenerateThread.join();
        }
        m_aiGenerating = false;
        m_aiGenerateStatus = "Cancelled";
    }

    void toggleHunyuanServer(bool lowVRAM, bool enableTex) {
        if (m_aiServerRunning) {
            stopHunyuanServer();
        } else {
            startHunyuanServer(lowVRAM, enableTex);
        }
    }

    void startHunyuanServer(bool lowVRAM, bool enableTex = false) {
        if (m_aiServerRunning) return;

        // Kill any orphaned server still holding the port from a previous session
        {
            FILE* fp = popen("lsof -ti :8081 2>/dev/null", "r");
            if (fp) {
                char buf[64];
                while (fgets(buf, sizeof(buf), fp)) {
                    pid_t orphan = atoi(buf);
                    if (orphan > 0) {
                        kill(orphan, SIGTERM);
                        std::cout << "[Hunyuan3D] Killed orphaned process " << orphan << " on port 8081" << std::endl;
                    }
                }
                pclose(fp);
                usleep(500000); // 500ms for port to free up
            }
        }

        pid_t pid = fork();
        if (pid == 0) {
            // New process group so we can kill all children cleanly
            setpgid(0, 0);

            // Child process — launch the server via bash
            // Texture is loaded on-demand (freed between shape/tex steps)
            // so --enable_tex is safe on 12GB — it just enables the capability
            // --low_vram adds CPU offload for texture pipeline
            std::string modelPath = lowVRAM ? "tencent/Hunyuan3D-2mini" : "tencent/Hunyuan3D-2";
            std::string subfolder = lowVRAM ? "hunyuan3d-dit-v2-mini-turbo" : "hunyuan3d-dit-v2-0-turbo";
            std::string cmd =
                "cd ~/Desktop/hunyuan3d2/Hunyuan3D-2 && "
                "source .venv/bin/activate && "
                "python api_server.py"
                " --model_path " + modelPath +
                " --subfolder " + subfolder +
                " --port 8081"
                " --enable_tex" +
                std::string(lowVRAM ? " --low_vram" : "");

            execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
            _exit(1);  // exec failed
        } else if (pid > 0) {
            // Also set pgid from parent side (race condition guard)
            setpgid(pid, pid);
            m_aiServerPID = pid;
            m_aiServerRunning = true;
            m_aiServerReady = false;
            std::string modeDesc = std::string(lowVRAM ? "mini" : "full") + (enableTex ? " + texture" : "");
            m_aiGenerateStatus = "Starting server (" + modeDesc + ")...";
            std::cout << "[Hunyuan3D] Server process launched, PID=" << pid << " (" << modeDesc << ")" << std::endl;

            // Launch background thread to poll until server is actually responding
            if (m_aiServerStartupThread.joinable()) {
                m_aiServerStartupThread.join();
            }
            m_aiServerStartupThread = std::thread([this]() {
                Hunyuan3DClient probe("localhost", 8081);
                for (int attempt = 0; attempt < 120; ++attempt) {  // Up to ~4 minutes
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    if (!m_aiServerRunning) return;  // Server was stopped

                    // Check if child process is still alive using kill(pid, 0)
                    // Returns 0 if process exists, -1 with ESRCH if it doesn't
                    if (m_aiServerPID > 0 && kill(m_aiServerPID, 0) != 0) {
                        // Process is gone
                        m_aiServerRunning = false;
                        m_aiServerReady = false;
                        m_aiServerPID = -1;
                        m_aiGenerateStatus = "Server process exited unexpectedly";
                        std::cerr << "[Hunyuan3D] Server process no longer exists" << std::endl;
                        return;
                    }

                    if (probe.isServerRunning()) {
                        m_aiServerReady = true;
                        m_aiGenerateStatus = "Server ready";
                        std::cout << "[Hunyuan3D] Server is ready (took ~" << (attempt + 1) * 2 << "s)" << std::endl;
                        return;
                    }

                    // Update status with elapsed time
                    m_aiGenerateStatus = "Starting server... (" + std::to_string((attempt + 1) * 2) + "s)";
                }
                // Timed out
                m_aiGenerateStatus = "Server startup timed out";
                std::cerr << "[Hunyuan3D] Server did not respond after 4 minutes" << std::endl;
            });
        } else {
            m_aiGenerateStatus = "Failed to start server (fork error)";
            std::cerr << "[Hunyuan3D] fork() failed" << std::endl;
        }
    }

    void stopHunyuanServer() {
        if (!m_aiServerRunning || m_aiServerPID <= 0) return;

        // Cancel any in-progress generation first
        if (m_aiGenerating) {
            cancelAIGeneration();
        }

        // Signal stop so startup thread exits
        m_aiServerRunning = false;
        m_aiServerReady = false;

        // Join startup polling thread
        if (m_aiServerStartupThread.joinable()) {
            m_aiServerStartupThread.join();
        }

        // Kill the server process group (SIGTERM first, then SIGKILL if needed)
        pid_t pid = m_aiServerPID;
        m_aiServerPID = -1;

        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);

        // Wait up to 3 seconds for graceful shutdown
        int status;
        bool exited = false;
        for (int i = 0; i < 30; i++) {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid || ret == -1) { exited = true; break; }
            usleep(100000);  // 100ms
        }

        if (!exited) {
            // Force kill if SIGTERM didn't work
            std::cout << "[Hunyuan3D] Server didn't exit gracefully, sending SIGKILL" << std::endl;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);  // Blocking wait to reap
        }

        m_aiGenerateStatus = "Server stopped";
        std::cout << "[Hunyuan3D] Server stopped" << std::endl;
    }

    void createTestCube() {
        auto obj = std::make_unique<SceneObject>("Cube");

        // Use default mesh color, or random if enabled
        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        std::vector<ModelVertex> vertices = {
            // Front face
            {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}, meshColor},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f}, meshColor},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}, meshColor},
            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f}, meshColor},
            // Back face
            {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}, meshColor},
            {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f}, meshColor},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}, meshColor},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f}, meshColor},
            // Top face
            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}, meshColor},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f}, meshColor},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}, meshColor},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f}, meshColor},
            // Bottom face
            {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}, meshColor},
            {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f}, meshColor},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}, meshColor},
            {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f}, meshColor},
            // Right face
            {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}, meshColor},
            {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}, meshColor},
            {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}, meshColor},
            {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}, meshColor},
            // Left face
            {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}, meshColor},
            {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}, meshColor},
            {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}, meshColor},
            {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}, meshColor},
        };

        std::vector<uint32_t> indices = {
            0,  1,  2,  0,  2,  3,   // Front
            4,  5,  6,  4,  6,  7,   // Back
            8,  9,  10, 8,  10, 11,  // Top
            12, 13, 14, 12, 14, 15,  // Bottom
            16, 17, 18, 16, 18, 19,  // Right
            20, 21, 22, 20, 22, 23   // Left
        };

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setLocalBounds({{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}});

        // Position so it sits on top of the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, 0.5f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        // Build editable mesh directly as a cube with proper quad faces
        // This bypasses the triangle merging which can be unreliable
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_editableMesh.buildCube(1.0f);
            m_editableMesh.setAllVertexColors(meshColor);

            // Update faceToTriangles mapping (2 triangles per quad face)
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }

            // Save EditableMesh half-edge data (preserves quad topology for duplicate/save)
            const auto& heVerts = m_editableMesh.getVerticesData();
            const auto& heHalfEdges = m_editableMesh.getHalfEdges();
            const auto& heFaces = m_editableMesh.getFacesData();

            std::vector<SceneObject::StoredHEVertex> storedVerts;
            storedVerts.reserve(heVerts.size());
            for (const auto& v : heVerts) {
                storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
            }

            std::vector<SceneObject::StoredHalfEdge> storedHE;
            storedHE.reserve(heHalfEdges.size());
            for (const auto& he : heHalfEdges) {
                storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
            }

            std::vector<SceneObject::StoredHEFace> storedFaces;
            storedFaces.reserve(heFaces.size());
            for (const auto& f : heFaces) {
                storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
            }

            m_selectedObject->setEditableMeshData(storedVerts, storedHE, storedFaces);
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createUnitBeam() {
        auto obj = std::make_unique<SceneObject>("UnitBeam");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // 0.15m x 0.15m cross-section, 1m long along Z
        float w = 0.15f, h = 0.15f, d = 1.0f;
        m_editableMesh.buildBox(w, h, d);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setLocalBounds({{-w/2, -h/2, -d/2}, {w/2, h/2, d/2}});

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }
        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Position so it sits on the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, h / 2.0f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void create4mPost() {
        auto obj = std::make_unique<SceneObject>("4mPost");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // 0.15m x 0.15m cross-section, 4m tall along Y
        float w = 0.15f, h = 4.0f, d = 0.15f;
        m_editableMesh.buildBox(w, h, d);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setLocalBounds({{-w/2, -h/2, -d/2}, {w/2, h/2, d/2}});

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }
        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Position so it sits on the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, h / 2.0f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createPanel(float width, float panelHeight, float thickness, const std::string& name) {
        auto obj = std::make_unique<SceneObject>(name);

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        m_editableMesh.buildBox(width, panelHeight, thickness);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);
        obj->setLocalBounds({{-width/2, -panelHeight/2, -thickness/2}, {width/2, panelHeight/2, thickness/2}});

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }
        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, panelHeight / 2.0f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createTestQuad() {
        auto obj = std::make_unique<SceneObject>("Quad");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // Simple quad vertices - 4 corners
        std::vector<ModelVertex> vertices = {
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, meshColor},  // 0: bottom-left
            {{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, meshColor},  // 1: bottom-right
            {{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, meshColor},  // 2: top-right
            {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, meshColor},  // 3: top-left
        };

        // Triangle indices for GPU (need triangles for rendering)
        std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        // Build editable mesh directly as a single quad face
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_editableMesh.clear();

            // Add vertices to editable mesh
            for (const auto& v : vertices) {
                HEVertex hv;
                hv.position = v.position;
                hv.normal = v.normal;
                hv.uv = v.texCoord;
                hv.color = v.color;
                hv.halfEdgeIndex = UINT32_MAX;
                hv.selected = false;
                m_editableMesh.addVertex(hv);
            }

            // Add single quad face with vertices 0, 1, 2, 3
            std::vector<uint32_t> quadVerts = {0, 1, 2, 3};
            m_editableMesh.addQuadFace(quadVerts);

            std::cout << "Created quad: " << m_editableMesh.getFaceCount() << " faces, "
                      << m_editableMesh.getVertexCount() << " vertices" << std::endl;

            // Check if face is quad
            if (m_editableMesh.getFaceCount() > 0) {
                std::cout << "Face 0 vertex count: " << m_editableMesh.getFace(0).vertexCount << std::endl;
            }

            m_faceToTriangles.clear();
            m_faceToTriangles[0] = {0, 1};  // Quad maps to 2 triangles
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCylinder() {
        auto obj = std::make_unique<SceneObject>("Cylinder");

        // Determine mesh color before building (random if enabled)
        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // Build editable mesh with cylinder geometry
        m_editableMesh.buildCylinder(m_cylinderRadius, m_cylinderHeight, m_cylinderSegments, m_cylinderDivisions, m_cylinderCaps, m_cylinderCapRings);
        m_editableMesh.setAllVertexColors(meshColor);

        // Triangulate for GPU rendering
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        // Save EditableMesh half-edge data (preserves quad topology for duplicate/save)
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }

        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Position so it sits on top of the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, m_cylinderHeight / 2.0f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        // Update faceToTriangles mapping
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createSphere() {
        auto obj = std::make_unique<SceneObject>("Sphere");

        // Determine mesh color before building (random if enabled)
        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // Build editable mesh with sphere geometry
        m_editableMesh.buildSphere(m_sphereRadius, m_sphereRings, m_sphereSegments);
        m_editableMesh.setAllVertexColors(meshColor);

        // Triangulate for GPU rendering
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        // Save EditableMesh half-edge data (preserves quad topology for duplicate/save)
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }

        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Position so it sits on top of the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, m_sphereRadius, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        // Update faceToTriangles mapping
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeRing() {
        auto obj = std::make_unique<SceneObject>("CubeRing");

        // Determine mesh color before building (random if enabled)
        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        // Build editable mesh with cube ring geometry
        m_editableMesh.buildCubeRing(m_cubeRingSegments, m_cubeRingInnerRadius, m_cubeRingOuterRadius, m_cubeRingHeight);
        m_editableMesh.setAllVertexColors(meshColor);

        // Triangulate for GPU rendering
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        // Save EditableMesh half-edge data (preserves quad topology for duplicate/save)
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();

        std::vector<SceneObject::StoredHEVertex> storedVerts;
        storedVerts.reserve(heVerts.size());
        for (const auto& v : heVerts) {
            storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        }

        std::vector<SceneObject::StoredHalfEdge> storedHE;
        storedHE.reserve(heHalfEdges.size());
        for (const auto& he : heHalfEdges) {
            storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        }

        std::vector<SceneObject::StoredHEFace> storedFaces;
        storedFaces.reserve(heFaces.size());
        for (const auto& f : heFaces) {
            storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        }

        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Position so it sits on top of the grid
        obj->getTransform().setPosition(glm::vec3(0.0f, m_cubeRingHeight * 0.5f, 0.0f));

        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        // Update faceToTriangles mapping
        if (m_currentModeType == EditorModeType::ModelingEditor && m_modelingMode) {
            m_faceToTriangles.clear();
            uint32_t triIndex = 0;
            for (uint32_t faceIdx = 0; faceIdx < m_editableMesh.getFaceCount(); ++faceIdx) {
                uint32_t vertCount = m_editableMesh.getFace(faceIdx).vertexCount;
                uint32_t triCount = (vertCount >= 3) ? (vertCount - 2) : 0;
                for (uint32_t i = 0; i < triCount; ++i) {
                    m_faceToTriangles[faceIdx].push_back(triIndex++);
                }
            }
        }

        // Switch to object mode with move gizmo for immediate positioning
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeArch(int segments, float innerRadius, float outerRadius, float depth, float arcDegrees) {
        auto obj = std::make_unique<SceneObject>("CubeArch");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        m_editableMesh.buildCubeArch(segments, innerRadius, outerRadius, depth, arcDegrees);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeColumn(int segments, float radius, float height) {
        auto obj = std::make_unique<SceneObject>("CubeColumn");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        m_editableMesh.buildCubeColumn(segments, radius, height);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, height * 0.5f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeStairs(int steps, float width, float stepHeight, float stepDepth) {
        auto obj = std::make_unique<SceneObject>("CubeStairs");

        glm::vec4 meshColor = m_defaultMeshColor;
        if (m_randomMeshColors) {
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            meshColor = glm::vec4(dist(m_rng), dist(m_rng), dist(m_rng), 1.0f);
        }

        m_editableMesh.buildCubeStairs(steps, width, stepHeight, stepDepth);
        m_editableMesh.setAllVertexColors(meshColor);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeSphere() {
        auto obj = std::make_unique<SceneObject>("CubeSphere");

        // Build the cube sphere (colors are varied in the build function)
        m_editableMesh.buildCubeSphere(m_cubeSphereRadius, m_cubeSphereCubeSize,
                                        m_cubeSphereRings, m_cubeSphereSegments,
                                        m_cubeSphereInterior, m_cubeSphereSolidShell);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        // Center the sphere at origin
        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createExtrudedSphere() {
        auto obj = std::make_unique<SceneObject>("ExtrudedSphere");

        m_editableMesh.buildExtrudedSphere(m_extrudedSphereRadius, m_extrudedSphereThickness,
                                            m_extrudedSphereRings, m_extrudedSphereSegments,
                                            m_extrudedSphereInterior);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeBlock() {
        auto obj = std::make_unique<SceneObject>("CubeBlock");

        m_editableMesh.buildCubeBlock(m_cubeBlockWidth, m_cubeBlockHeight, m_cubeBlockDepth, m_cubeBlockCubeSize);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createBlockPlate() {
        std::string name = m_blockPlateBeveled ? "BeveledBlockPlate" : "BlockPlate";
        auto obj = std::make_unique<SceneObject>(name);

        m_editableMesh.buildBlockPlate(m_blockPlateWidth, m_blockPlateHeight, m_blockPlateCubeSize,
                                        m_blockPlateBeveled, m_blockPlateBevelAmount);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createCubeRoom(int width, int height, int depth, float cubeSize, int windowFront) {
        auto obj = std::make_unique<SceneObject>("CubeRoom");

        m_editableMesh.buildCubeRoom(width, height, depth, cubeSize, windowFront);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    void createHead(float scale) {
        auto obj = std::make_unique<SceneObject>("Head");

        m_editableMesh.buildCubeHead(scale);

        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(indices.size()));
        obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
        obj->setMeshData(vertices, indices);

        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));
        m_objectMode = true;
        m_gizmoMode = GizmoMode::Move;
    }

    // Explode a cube-based object into individual cube objects
    void explodeCubeObject() {
        if (!m_selectedObject) return;

        // Get the mesh data from the selected object
        const auto& meshVerts = m_selectedObject->getVertices();
        const auto& meshIndices = m_selectedObject->getIndices();

        if (meshVerts.empty() || meshIndices.empty()) {
            std::cout << "Cannot explode: no mesh data" << std::endl;
            return;
        }

        // Each cube has 24 vertices (4 per face * 6 faces) and 36 indices (6 per face * 6 faces)
        // But our quads use 4 indices each, so 6 faces = 24 indices per cube
        // Actually with triangulation: 6 faces * 2 triangles * 3 = 36 indices per cube

        // Simpler approach: group vertices by their bounding box center
        // Find all unique cube centers by grouping nearby vertices
        std::vector<glm::vec3> cubeCenters;
        std::vector<std::vector<uint32_t>> cubeVertexIndices;

        // Group vertices into cubes by finding clusters
        // Each cube's vertices should be within cubeSize of each other
        float tolerance = 0.01f;  // Small tolerance for floating point

        for (size_t i = 0; i < meshVerts.size(); i += 24) {
            if (i + 24 > meshVerts.size()) break;

            // Calculate center of this cube (average of 24 vertices)
            glm::vec3 center(0);
            for (size_t j = 0; j < 24; ++j) {
                center += meshVerts[i + j].position;
            }
            center /= 24.0f;

            cubeCenters.push_back(center);
        }

        if (cubeCenters.empty()) {
            std::cout << "No cubes found to explode" << std::endl;
            return;
        }

        // Get the parent object's transform
        glm::vec3 parentPos = m_selectedObject->getTransform().getPosition();
        glm::vec3 parentScale = m_selectedObject->getTransform().getScale();
        std::string baseName = m_selectedObject->getName();

        // Calculate cube size from first cube's vertices
        float cubeSize = 0.5f;
        if (meshVerts.size() >= 24) {
            glm::vec3 minV = meshVerts[0].position;
            glm::vec3 maxV = meshVerts[0].position;
            for (size_t j = 0; j < 24; ++j) {
                minV = glm::min(minV, meshVerts[j].position);
                maxV = glm::max(maxV, meshVerts[j].position);
            }
            cubeSize = glm::max(maxV.x - minV.x, glm::max(maxV.y - minV.y, maxV.z - minV.z));
        }

        // Mark current object for deletion
        m_pendingDeletions.push_back(m_selectedObject);
        m_selectedObject = nullptr;

        // Create individual cube objects
        int cubeIndex = 0;
        for (size_t i = 0; i < meshVerts.size(); i += 24) {
            if (i + 24 > meshVerts.size()) break;

            auto obj = std::make_unique<SceneObject>(baseName + "_" + std::to_string(cubeIndex));

            // Get the color from the first vertex of this cube
            glm::vec4 color = meshVerts[i].color;

            // Build a single cube
            m_editableMesh.buildCube(cubeSize);
            m_editableMesh.setAllVertexColors(color);

            std::vector<ModelVertex> vertices;
            std::vector<uint32_t> indices;
            m_editableMesh.triangulate(vertices, indices);

            uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
            obj->setBufferHandle(handle);
            obj->setIndexCount(static_cast<uint32_t>(indices.size()));
            obj->setVertexCount(static_cast<uint32_t>(vertices.size()));
            obj->setMeshData(vertices, indices);

            // Store half-edge data
            const auto& heVerts = m_editableMesh.getVerticesData();
            const auto& heHalfEdges = m_editableMesh.getHalfEdges();
            const auto& heFaces = m_editableMesh.getFacesData();
            std::vector<SceneObject::StoredHEVertex> storedVerts;
            for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
            std::vector<SceneObject::StoredHalfEdge> storedHE;
            for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
            std::vector<SceneObject::StoredHEFace> storedFaces;
            for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
            obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

            // Position at the cube's center (transformed by parent)
            glm::vec3 cubeCenter = cubeCenters[cubeIndex] * parentScale + parentPos;
            obj->getTransform().setPosition(cubeCenter);
            obj->getTransform().setScale(parentScale);

            m_sceneObjects.push_back(std::move(obj));
            cubeIndex++;
        }

        std::cout << "Exploded into " << cubeIndex << " individual cubes" << std::endl;
        m_objectMode = true;
    }

    // Auto-UV the selected cube-based object
    void autoUVSelectedObject() {
        if (!m_selectedObject) {
            std::cout << "No object selected for auto-UV" << std::endl;
            return;
        }

        // Load the mesh into EditableMesh
        const auto& storedVerts = m_selectedObject->getHEVertices();
        const auto& storedHE = m_selectedObject->getHEHalfEdges();
        const auto& storedFaces = m_selectedObject->getHEFaces();

        if (storedVerts.empty()) {
            std::cout << "No editable mesh data for auto-UV" << std::endl;
            return;
        }

        // Restore EditableMesh from stored data
        m_editableMesh.clear();
        std::vector<HEVertex> verts;
        for (const auto& sv : storedVerts) {
            verts.push_back({sv.position, sv.normal, sv.uv, sv.color, sv.halfEdgeIndex, sv.selected});
        }
        std::vector<HalfEdge> halfEdges;
        for (const auto& she : storedHE) {
            halfEdges.push_back({she.vertexIndex, she.faceIndex, she.nextIndex, she.prevIndex, she.twinIndex});
        }
        std::vector<HEFace> faces;
        for (const auto& sf : storedFaces) {
            faces.push_back({sf.halfEdgeIndex, sf.vertexCount, sf.selected});
        }
        m_editableMesh.setMeshData(verts, halfEdges, faces);

        // Apply auto-UV
        m_editableMesh.autoUVCubes();

        // Triangulate and update the GPU mesh
        std::vector<ModelVertex> vertices;
        std::vector<uint32_t> indices;
        m_editableMesh.triangulate(vertices, indices);

        // Update the model on GPU
        m_modelRenderer->destroyModel(m_selectedObject->getBufferHandle());
        uint32_t handle = m_modelRenderer->createModel(vertices, indices, nullptr, 0, 0);
        m_selectedObject->setBufferHandle(handle);
        m_selectedObject->setIndexCount(static_cast<uint32_t>(indices.size()));
        m_selectedObject->setVertexCount(static_cast<uint32_t>(vertices.size()));
        m_selectedObject->setMeshData(vertices, indices);

        // Save updated EditableMesh data back to object
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> newStoredVerts;
        for (const auto& v : heVerts) newStoredVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> newStoredHE;
        for (const auto& he : heHalfEdges) newStoredHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> newStoredFaces;
        for (const auto& f : heFaces) newStoredFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        m_selectedObject->setEditableMeshData(newStoredVerts, newStoredHE, newStoredFaces);

        std::cout << "Auto-UV applied to " << m_selectedObject->getName() << std::endl;
    }

    // Group selected objects into one combined mesh
    void groupSelectedObjects() {
        // Collect all selected objects (for now, just use shift-click selection or implement multi-select)
        // For simplicity, group ALL objects in the scene into one
        if (m_sceneObjects.size() < 2) {
            std::cout << "Need at least 2 objects to group" << std::endl;
            return;
        }

        // Build combined mesh using EditableMesh to preserve quad topology
        m_editableMesh.clear();

        // First, combine all objects' EditableMesh data
        std::vector<HEVertex> combinedHEVerts;
        std::vector<HalfEdge> combinedHE;
        std::vector<HEFace> combinedFaces;

        for (auto& sceneObj : m_sceneObjects) {
            glm::vec3 pos = sceneObj->getTransform().getPosition();
            glm::vec3 scale = sceneObj->getTransform().getScale();

            if (sceneObj->hasEditableMeshData()) {
                // Use stored half-edge data
                const auto& storedVerts = sceneObj->getHEVertices();
                const auto& storedHE = sceneObj->getHEHalfEdges();
                const auto& storedFaces = sceneObj->getHEFaces();

                uint32_t vertOffset = static_cast<uint32_t>(combinedHEVerts.size());
                uint32_t heOffset = static_cast<uint32_t>(combinedHE.size());
                uint32_t faceOffset = static_cast<uint32_t>(combinedFaces.size());

                // Add transformed vertices
                for (const auto& v : storedVerts) {
                    HEVertex newV;
                    newV.position = v.position * scale + pos;
                    newV.normal = v.normal;  // Normals stay same for uniform scale
                    newV.uv = v.uv;
                    newV.color = v.color;
                    newV.halfEdgeIndex = v.halfEdgeIndex + heOffset;
                    newV.selected = false;
                    combinedHEVerts.push_back(newV);
                }

                // Add half-edges with offset indices
                for (const auto& he : storedHE) {
                    HalfEdge newHE;
                    newHE.vertexIndex = he.vertexIndex + vertOffset;
                    newHE.faceIndex = he.faceIndex + faceOffset;
                    newHE.nextIndex = he.nextIndex + heOffset;
                    newHE.prevIndex = he.prevIndex + heOffset;
                    newHE.twinIndex = (he.twinIndex == UINT32_MAX) ? UINT32_MAX : (he.twinIndex + heOffset);
                    combinedHE.push_back(newHE);
                }

                // Add faces with offset indices
                for (const auto& f : storedFaces) {
                    HEFace newF;
                    newF.halfEdgeIndex = f.halfEdgeIndex + heOffset;
                    newF.vertexCount = f.vertexCount;
                    newF.selected = false;
                    combinedFaces.push_back(newF);
                }
            }

            // Mark for deletion
            m_pendingDeletions.push_back(sceneObj.get());
        }

        m_selectedObject = nullptr;

        // Set combined data into editable mesh
        m_editableMesh.setMeshData(combinedHEVerts, combinedHE, combinedFaces);

        // Triangulate for GPU and raycasting
        std::vector<ModelVertex> combinedVerts;
        std::vector<uint32_t> combinedIndices;
        m_editableMesh.triangulate(combinedVerts, combinedIndices);

        // Create the grouped object
        auto obj = std::make_unique<SceneObject>("CubeGroup");

        uint32_t handle = m_modelRenderer->createModel(combinedVerts, combinedIndices, nullptr, 0, 0);
        obj->setBufferHandle(handle);
        obj->setIndexCount(static_cast<uint32_t>(combinedIndices.size()));
        obj->setVertexCount(static_cast<uint32_t>(combinedVerts.size()));
        obj->setMeshData(combinedVerts, combinedIndices);

        // Store half-edge data for editing
        const auto& heVerts = m_editableMesh.getVerticesData();
        const auto& heHalfEdges = m_editableMesh.getHalfEdges();
        const auto& heFaces = m_editableMesh.getFacesData();
        std::vector<SceneObject::StoredHEVertex> storedVerts;
        for (const auto& v : heVerts) storedVerts.push_back({v.position, v.normal, v.uv, v.color, v.halfEdgeIndex, v.selected});
        std::vector<SceneObject::StoredHalfEdge> storedHE;
        for (const auto& he : heHalfEdges) storedHE.push_back({he.vertexIndex, he.faceIndex, he.nextIndex, he.prevIndex, he.twinIndex});
        std::vector<SceneObject::StoredHEFace> storedFaces;
        for (const auto& f : heFaces) storedFaces.push_back({f.halfEdgeIndex, f.vertexCount, f.selected});
        obj->setEditableMeshData(storedVerts, storedHE, storedFaces);

        obj->getTransform().setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        obj->getTransform().setScale(glm::vec3(1.0f, 1.0f, 1.0f));
        m_selectedObject = obj.get();
        m_sceneObjects.push_back(std::move(obj));

        std::cout << "Grouped " << m_pendingDeletions.size() << " objects into CubeGroup ("
                  << combinedVerts.size() << " verts, " << combinedIndices.size() << " indices)" << std::endl;
        m_objectMode = true;
    }

private:
    // MCP Server for AI integration
    std::unique_ptr<MCPServer> m_mcpServer;

    // Hunyuan3D generation state
    Hunyuan3DClient m_hunyuanClient{"localhost", 8081};
    bool m_aiGenerating = false;
    std::string m_aiGenerateStatus;
    std::string m_aiGenerateJobUID;
    std::thread m_aiGenerateThread;
    std::atomic<bool> m_aiGenerateComplete{false};
    std::atomic<bool> m_aiGenerateCancelled{false};
    std::string m_aiGeneratedGLBPath;

    // Hunyuan3D server process management
    bool m_aiServerRunning = false;       // Process launched (checkbox state)
    bool m_aiServerReady = false;         // Server actually responding to HTTP
    pid_t m_aiServerPID = -1;
    std::thread m_aiServerStartupThread;

    // Hunyuan3D server log
    std::vector<std::string> m_aiLogLines;
    int m_aiLogIndex = 0;  // Tracks /log?since= position

    // Renderers
    std::unique_ptr<ModelRenderer> m_modelRenderer;
    std::unique_ptr<SkinnedModelRenderer> m_skinnedModelRenderer;
    ImGuiManager m_imguiManager;

    // Mode management
    std::unique_ptr<EditorContext> m_editorContext;
    std::unique_ptr<ModelingMode> m_modelingMode;
    std::unique_ptr<AnimationMode> m_animationMode;
    IEditorMode* m_activeMode = nullptr;
    EditorModeType m_currentModeType = EditorModeType::ModelingEditor;

    // Cameras
    Camera m_camera{glm::vec3(0, 50, 0)};
    Camera m_camera2{glm::vec3(0, 10, 0)};
    float m_cameraSpeed = 0.1f;
    bool m_isLooking = false;

    // Camera controls
    glm::vec3 m_orbitTarget = glm::vec3(0.0f);  // Point camera orbits around
    glm::vec2 m_lastMousePos = glm::vec2(0.0f);
    bool m_isTumbling = false;   // Alt + LMB
    bool m_wasTumbling = false;  // Previous frame's tumbling state (for snap prevention)
    glm::vec3 m_tumbleOrbitTarget = glm::vec3(0.0f);  // Stored orbit target for current tumble
    float m_tumbleOrbitDistance = 5.0f;  // Stored orbit distance for current tumble
    bool m_isPanning = false;    // MMB
    bool m_mouseLookMode = false;  // Tumble style: false=orbit, true=mouse-look
    float m_orbitYaw = -90.0f;   // Orbit angles (calculated from camera position)
    float m_orbitPitch = 0.0f;

    // Split view
    bool m_splitView = false;
    bool m_activeViewportLeft = true;
    ViewPreset m_splitOrthoPreset = ViewPreset::Top;

    // Scene
    std::vector<std::unique_ptr<SceneObject>> m_sceneObjects;
    SceneObject* m_selectedObject = nullptr;
    std::set<SceneObject*> m_selectedObjects;  // Multi-selection for object mode

    // UV Editor state
    EditMode m_editMode = EditMode::Paint;
    glm::vec3 m_paintColor = glm::vec3(1.0f, 0.0f, 0.0f);
    float m_paintRadius = 0.02f;
    float m_paintStrength = 0.5f;
    bool m_isPainting = false;
    bool m_squareBrush = false;  // Square brush (pixel art style)
    bool m_fillPaintToFace = false;  // Click face to fill with color

    // Brush modes
    bool m_useStamp = false;
    bool m_useSmear = false;
    bool m_useEyedropper = false;
    bool m_useClone = false;
    int m_cloneSourceViewIndex = -1;
    glm::vec2 m_cloneSourcePixel = glm::vec2(0.0f);
    glm::vec2 m_cloneCurrentSample = glm::vec2(0.0f);
    glm::vec2 m_cloneLastPaintUV = glm::vec2(0.0f);
    bool m_cloneSourceSet = false;
    bool m_clonePaintingActive = false;
    glm::vec2 m_lastPaintUV = glm::vec2(0.0f);
    bool m_hasLastPaintPosition = false;
    float m_smearStrength = 0.5f;
    float m_smearPickup = 0.3f;
    glm::vec3 m_smearCarriedColor = glm::vec3(0.0f);
    bool m_isSmearing = false;
    std::vector<unsigned char> m_stampData;
    int m_stampWidth = 0;
    int m_stampHeight = 0;
    float m_stampScale = 1.0f;
    float m_stampScaleH = 1.0f;
    float m_stampScaleV = 1.0f;
    float m_stampRotation = 0.0f;
    float m_stampOpacity = 1.0f;
    bool m_stampFlipH = false;
    bool m_stampFlipV = false;
    bool m_stampProjectFromView = false;
    bool m_stampFitToFace = false;  // Fit stamp to clicked face
    int m_stampFitRotation = 0;    // 0-3: rotate corners by 90° increments
    int m_seamBusterPixels = 2;    // Number of pixels to extend beyond UV edges

    // Stamp preview resources
    VkImage m_stampPreviewImage = VK_NULL_HANDLE;
    VkDeviceMemory m_stampPreviewMemory = VK_NULL_HANDLE;
    VkImageView m_stampPreviewView = VK_NULL_HANDLE;
    VkSampler m_stampPreviewSampler = VK_NULL_HANDLE;
    VkDescriptorSet m_stampPreviewDescriptor = VK_NULL_HANDLE;
    glm::vec3 m_uvWireframeColor = glm::vec3(0.0f, 0.0f, 0.0f);
    float m_uvZoom = 1.0f;
    glm::vec2 m_uvPan = glm::vec2(0.0f);
    bool m_uvPanning = false;
    glm::vec2 m_uvPanStart = glm::vec2(0.0f);
    bool m_showWireframe = false;
    std::set<uint32_t> m_selectedFaces;
    std::set<uint32_t> m_hiddenFaces;
    glm::vec4 m_selectionColor = glm::vec4(0.2f, 0.4f, 1.0f, 0.5f);
    std::vector<UVIsland> m_uvIslands;
    std::set<uint32_t> m_selectedIslands;
    uint32_t m_nextIslandId = 0;
    std::mt19937 m_rng{std::random_device{}()};

    // UV manipulation
    glm::vec2 m_uvIslandOffset = glm::vec2(0.0f);
    glm::vec2 m_uvIslandScale = glm::vec2(1.0f);
    bool m_uvDragging = false;
    bool m_uvResizing = false;
    int m_uvResizeCorner = -1;
    glm::vec2 m_uvDragStart = glm::vec2(0.0f);
    glm::vec2 m_uvIslandOriginalMin = glm::vec2(0.0f);
    glm::vec2 m_uvIslandOriginalMax = glm::vec2(1.0f);
    bool m_uvHandleHovered = false;

    // Modeling Editor state
    EditableMesh m_editableMesh;
    std::map<uint32_t, std::vector<uint32_t>> m_faceToTriangles;
    ModelingSelectionMode m_modelingSelectionMode = ModelingSelectionMode::Face;
    float m_extrudeDistance = 0.5f;
    int m_extrudeCount = 1;
    float m_insetAmount = 0.3f;
    float m_hollowThickness = 0.1f;
    float m_vertexDisplaySize = 0.05f;
    float m_edgeDisplayWidth = 2.0f;
    glm::vec4 m_modelingSelectionColor = glm::vec4(1.0f, 0.5f, 0.0f, 0.8f);
    glm::vec4 m_modelingHoverColor = glm::vec4(1.0f, 1.0f, 0.0f, 0.6f);
    glm::vec4 m_modelingVertexColor = glm::vec4(0.0f, 0.8f, 1.0f, 1.0f);
    glm::vec4 m_modelingEdgeColor = glm::vec4(0.0f, 1.0f, 0.5f, 1.0f);
    bool m_showModelingWireframe = true;
    bool m_showFaceNormals = false;
    float m_normalDisplayLength = 0.2f;
    float m_uvProjectionScale = 1.0f;
    float m_uvAngleThreshold = 66.0f;
    float m_uvIslandMargin = 0.02f;
    int m_cylinderAxisIndex = 0;  // 0=Y, 1=X, 2=Z
    glm::vec3 m_cylinderAxisHint = glm::vec3(0, 1, 0);
    bool m_cylinderUsePCA = true;
    int m_hoveredVertex = -1;
    int m_hoveredEdge = -1;
    int m_hoveredFace = -1;
    double m_lastClickTime = 0.0;
    bool m_meshDirty = false;

    // Selection tool state
    SelectionTool m_selectionTool = SelectionTool::Normal;
    bool m_isRectSelecting = false;
    glm::vec2 m_rectSelectStart{0.0f};
    glm::vec2 m_rectSelectEnd{0.0f};
    float m_paintSelectRadius = 30.0f;

    // Grid settings
    bool m_showGrid = true;
    float m_gridSize = 10.0f;
    float m_gridSpacing = 1.0f;
    glm::vec4 m_gridColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
    glm::vec4 m_gridAxisColor = glm::vec4(0.3f, 0.3f, 0.3f, 0.8f);

    // Custom colors
    glm::vec4 m_backgroundColor = glm::vec4(0.1f, 0.1f, 0.12f, 1.0f);  // Viewport background
    glm::vec4 m_defaultMeshColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // Default mesh color (white)
    glm::vec4 m_wireframeColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);    // Wireframe/edge color (black)
    bool m_randomMeshColors = true;  // Randomize color for each new primitive

    // Reference images
    std::array<ReferenceImage, 6> m_referenceImages;

    // Window visibility
    bool m_showSceneWindow = true;
    bool m_showToolsWindow = true;
    bool m_showUVWindow = false;
    bool m_showCameraWindow = true;
    bool m_showImageRefWindow = false;
    bool m_showLibraryWindow = false;

    // Library state
    std::string m_libraryPath;
    std::vector<LibraryItem> m_libraryItems;
    std::vector<std::string> m_libraryCategories;  // List of category names
    int m_selectedCategory = 0;  // 0 = "All", 1+ = specific category
    bool m_libraryNeedsRefresh = true;
    char m_librarySaveNameBuffer[128] = "";
    char m_libraryNewCategoryBuffer[64] = "";

    // Widget properties for library save (metadata)
    int m_widgetTypeIndex = 0;
    char m_widgetParamName[64] = "";
    char m_widgetMachineName[64] = "";
    static const int THUMBNAIL_SIZE = 128;

    // Image Reference window state
    float m_imageRefZoom = 1.0f;
    glm::vec2 m_imageRefPan{0.0f, 0.0f};
    bool m_imageRefPanning = false;
    int m_imageRefSelectedIndex = -1;
    std::vector<CloneSourceImage> m_cloneSourceImages;

    // Object mode
    bool m_objectMode = false;
    int m_renamingObjectIndex = -1;
    char m_renameBuffer[128] = "";

    // Transform sliders
    glm::vec3 m_transformMove = glm::vec3(0);
    glm::vec3 m_transformScale = glm::vec3(1);
    glm::vec3 m_transformRotate = glm::vec3(0);
    glm::vec3 m_lastScale = glm::vec3(1);
    glm::vec3 m_lastRotate = glm::vec3(0);
    bool m_transformActive = false;

    // UV advanced state
    bool m_uvDraggingSelection = false;
    bool m_uvScaling = false;
    bool m_uvRotating = false;
    bool m_uvChildHovered = false;
    glm::vec2 m_uvScaleCenter = glm::vec2(0);
    glm::vec2 m_uvScaleStart = glm::vec2(0);
    float m_uvRotateStartAngle = 0.0f;
    std::set<uint32_t> m_uvSelectedFaces;
    std::map<uint32_t, glm::vec2> m_uvOriginalCoords;
    int m_uvScaleHandle = -1;  // -1=none, 0-3=corners, 4-7=sides
    glm::vec2 m_uvScaleAnchor = glm::vec2(0);
    glm::vec2 m_uvScaleOriginalMin = glm::vec2(0);
    glm::vec2 m_uvScaleOriginalMax = glm::vec2(0);
    bool m_uvEdgeSelectionMode = false;
    std::pair<uint32_t, uint32_t> m_uvSelectedEdge = {UINT32_MAX, UINT32_MAX};
    std::vector<std::pair<uint32_t, uint32_t>> m_uvTwinEdges;

    // UV vertex editing
    int m_uvSelectionMode = 0;  // 0=Face, 1=Edge, 2=Vertex
    std::set<uint32_t> m_uvSelectedVertices;
    bool m_uvDraggingVertex = false;

    // Gizmo state
    GizmoMode m_gizmoMode = GizmoMode::None;  // Off by default, W to enable
    GizmoAxis m_gizmoHoveredAxis = GizmoAxis::None;
    GizmoAxis m_gizmoActiveAxis = GizmoAxis::None;
    bool m_gizmoDragging = false;
    glm::vec3 m_gizmoDragStart = glm::vec3(0.0f);
    glm::vec3 m_gizmoDragStartPos = glm::vec3(0.0f);
    glm::vec3 m_gizmoOriginalObjPos = glm::vec3(0.0f);  // Original object position for snap
    float m_gizmoSize = 1.0f;
    glm::vec3 m_gizmoOffset = glm::vec3(0.0f);
    bool m_gizmoLocalSpace = false;

    // Snap/increment settings
    bool m_snapEnabled = false;
    float m_moveSnapIncrement = 0.25f;   // Move snap in world units
    float m_rotateSnapIncrement = 15.0f; // Rotation snap in degrees

    // Add primitive dialog state
    bool m_showCylinderDialog = false;
    bool m_showSphereDialog = false;
    float m_cylinderRadius = 0.5f;
    float m_cylinderHeight = 2.0f;
    int m_cylinderSegments = 16;
    int m_cylinderDivisions = 1;
    bool m_cylinderCaps = true;
    int m_cylinderCapRings = 2;
    float m_sphereRadius = 0.5f;
    int m_sphereRings = 8;
    int m_sphereSegments = 16;
    bool m_showCubeRingDialog = false;
    int m_cubeRingSegments = 8;
    float m_cubeRingInnerRadius = 0.3f;
    float m_cubeRingOuterRadius = 0.5f;
    float m_cubeRingHeight = 1.0f;

    bool m_showCubeArchDialog = false;
    int m_cubeArchSegments = 8;
    float m_cubeArchInnerRadius = 0.4f;
    float m_cubeArchOuterRadius = 0.6f;
    float m_cubeArchDepth = 0.3f;
    float m_cubeArchArcDegrees = 180.0f;

    bool m_showCubeColumnDialog = false;
    int m_cubeColumnSegments = 8;
    float m_cubeColumnRadius = 0.5f;
    float m_cubeColumnHeight = 2.0f;

    bool m_showCubeStairsDialog = false;
    int m_cubeStairsSteps = 5;
    float m_cubeStairsWidth = 1.0f;
    float m_cubeStairsStepHeight = 0.2f;
    float m_cubeStairsStepDepth = 0.3f;

    bool m_showCubeSphereDialog = false;
    float m_cubeSphereRadius = 2.0f;
    float m_cubeSphereCubeSize = 0.3f;
    int m_cubeSphereRings = 8;
    int m_cubeSphereSegments = 16;
    bool m_cubeSphereInterior = false;
    bool m_cubeSphereSolidShell = true;

    bool m_showExtrudedSphereDialog = false;
    float m_extrudedSphereRadius = 1.0f;
    float m_extrudedSphereThickness = 0.2f;
    int m_extrudedSphereRings = 8;
    int m_extrudedSphereSegments = 16;
    bool m_extrudedSphereInterior = false;

    bool m_showCubeRoomDialog = false;
    int m_cubeRoomWidth = 8;
    int m_cubeRoomHeight = 4;
    int m_cubeRoomDepth = 10;
    float m_cubeRoomCubeSize = 0.5f;
    int m_cubeRoomWindowFront = 3;

    bool m_showCubeBlockDialog = false;
    int m_cubeBlockWidth = 3;
    int m_cubeBlockHeight = 2;
    int m_cubeBlockDepth = 3;
    float m_cubeBlockCubeSize = 0.5f;

    bool m_showBlockPlateDialog = false;
    int m_blockPlateWidth = 5;
    int m_blockPlateHeight = 3;
    float m_blockPlateCubeSize = 0.5f;
    bool m_blockPlateBeveled = false;
    float m_blockPlateBevelAmount = 0.15f;

    // Deferred deletion queue (processed at start of frame to avoid GPU sync issues)
    std::vector<SceneObject*> m_pendingDeletions;

    // Deferred texture deletion flag
    bool m_pendingTextureDelete = false;

    // Quick save state (for F5)
    std::string m_currentFilePath;
    int m_currentFileFormat = 0;  // 0=none, 1=OBJ, 2=LIME, 3=GLB
};

// EditorContext helper method implementations
void EditorContext::getMouseRay(glm::vec3& rayOrigin, glm::vec3& rayDir) const {
    double mouseX, mouseY;
    glfwGetCursorPos(window.getHandle(), &mouseX, &mouseY);

    float screenWidth = static_cast<float>(window.getWidth());
    float screenHeight = static_cast<float>(window.getHeight());

    // Normalize to 0-1
    float normalizedX = static_cast<float>(mouseX) / screenWidth;
    float normalizedY = static_cast<float>(mouseY) / screenHeight;

    // Handle split view
    Camera* activeCamera = &camera;
    if (splitView && !activeViewportLeft) {
        activeCamera = &camera2;
        normalizedX = (normalizedX - 0.5f) * 2.0f;
    } else if (splitView) {
        normalizedX = normalizedX * 2.0f;
    }

    float aspectRatio = screenWidth / screenHeight;
    if (splitView) aspectRatio *= 0.5f;

    if (activeCamera->getProjectionMode() == ProjectionMode::Orthographic) {
        // Orthographic mode: parallel rays, origin varies on near plane
        // Convert from [0,1] to [-1,1] NDC
        float ndcX = normalizedX * 2.0f - 1.0f;
        float ndcY = 1.0f - normalizedY * 2.0f;  // Flip Y

        // Get ortho size and calculate world position on the near plane
        float orthoSize = activeCamera->getOrthoSize();
        float halfHeight = orthoSize;
        float halfWidth = orthoSize * aspectRatio;

        // Ray origin is on the near plane at the mouse position
        glm::vec3 camPos = activeCamera->getPosition();
        glm::vec3 camRight = activeCamera->getRight();
        glm::vec3 camUp = activeCamera->getUp();
        glm::vec3 camFront = activeCamera->getFront();

        rayOrigin = camPos + camRight * (ndcX * halfWidth) + camUp * (ndcY * halfHeight);
        rayDir = camFront;  // Parallel rays in ortho mode
    } else {
        // Perspective mode
        rayOrigin = activeCamera->getPosition();
        rayDir = activeCamera->screenToWorldRay(normalizedX, normalizedY, aspectRatio);
    }
}

Camera& EditorContext::getActiveCamera() {
    return (splitView && !activeViewportLeft) ? camera2 : camera;
}

bool EditorContext::isMouseInLeftViewport() const {
    double mouseX, mouseY;
    glfwGetCursorPos(window.getHandle(), &mouseX, &mouseY);
    return mouseX < window.getWidth() / 2.0f;
}

ReferenceImage* EditorContext::getReferenceForView(ViewPreset preset) {
    int index = static_cast<int>(preset) - 1;
    if (index >= 0 && index < 6) {
        return &referenceImages[index];
    }
    return nullptr;
}

const char* EditorContext::getViewPresetName(int index) {
    static const char* names[] = {"Top", "Bottom", "Front", "Back", "Right", "Left"};
    if (index >= 0 && index < 6) return names[index];
    return "Unknown";
}

int main() {
    try {
        ModelEditor editor;
        editor.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
