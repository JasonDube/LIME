#include "Buffer.hpp"
#include "VulkanContext.hpp"
#include <stdexcept>
#include <cstring>

namespace eden {

std::atomic<int64_t> Buffer::s_vramUsedBytes{0};
std::unordered_map<VkDeviceMemory, int64_t> Buffer::s_vramAllocSizes;
std::mutex Buffer::s_vramMutex;

void Buffer::trackVramAllocHandle(VkDeviceMemory mem, int64_t bytes) {
    s_vramUsedBytes += bytes;
    std::lock_guard<std::mutex> lock(s_vramMutex);
    s_vramAllocSizes[mem] = bytes;
}

void Buffer::trackVramFreeHandle(VkDeviceMemory mem) {
    std::lock_guard<std::mutex> lock(s_vramMutex);
    auto it = s_vramAllocSizes.find(mem);
    if (it != s_vramAllocSizes.end()) {
        s_vramUsedBytes -= it->second;
        s_vramAllocSizes.erase(it);
    }
}

Buffer::Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags properties)
    : m_context(context), m_size(size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(context.getDevice(), &bufferInfo, nullptr, &m_buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context.getDevice(), m_buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(context.getDevice(), &allocInfo, nullptr, &m_memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }
    trackVramAllocHandle(m_memory, static_cast<int64_t>(memRequirements.size));

    vkBindBufferMemory(context.getDevice(), m_buffer, m_memory, 0);
}

Buffer::~Buffer() {
    if (m_mapped) {
        unmap();
    }
    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context.getDevice(), m_buffer, nullptr);
    }
    if (m_memory != VK_NULL_HANDLE) {
        trackVramFreeHandle(m_memory);
        vkFreeMemory(m_context.getDevice(), m_memory, nullptr);
    }
}

void* Buffer::map() {
    if (!m_mapped) {
        vkMapMemory(m_context.getDevice(), m_memory, 0, m_size, 0, &m_mapped);
    }
    return m_mapped;
}

void Buffer::unmap() {
    if (m_mapped) {
        vkUnmapMemory(m_context.getDevice(), m_memory);
        m_mapped = nullptr;
    }
}

void Buffer::upload(const void* data, VkDeviceSize size) {
    void* mapped = map();
    memcpy(mapped, data, static_cast<size_t>(size));
    unmap();
}

void Buffer::copy(VulkanContext& context, Buffer& src, Buffer& dst, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = context.getCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(context.getDevice(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, src.getHandle(), dst.getHandle(), 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(context.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(context.getGraphicsQueue());

    vkFreeCommandBuffers(context.getDevice(), context.getCommandPool(), 1, &commandBuffer);
}

uint32_t Buffer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context.getPhysicalDevice(), &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

// BufferManager implementation

BufferManager::BufferManager(VulkanContext& context) : m_context(context) {}

BufferManager::~BufferManager() {
    m_meshBuffers.clear();
}

uint32_t BufferManager::createMeshBuffers(const void* vertices, uint32_t vertexCount, size_t vertexSize,
                                          const uint32_t* indices, uint32_t indexCount) {
    auto meshBuffers = std::make_unique<MeshBuffers>();
    VkDeviceSize vertexBufferSize = vertexCount * vertexSize;

    // Create staging buffer
    Buffer stagingBuffer(m_context, vertexBufferSize,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    stagingBuffer.upload(vertices, vertexBufferSize);

    // Create vertex buffer
    meshBuffers->vertexBuffer = std::make_unique<Buffer>(
        m_context, vertexBufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    Buffer::copy(m_context, stagingBuffer, *meshBuffers->vertexBuffer, vertexBufferSize);
    meshBuffers->vertexCount = vertexCount;

    // Create index buffer if needed
    if (indices && indexCount > 0) {
        VkDeviceSize indexBufferSize = indexCount * sizeof(uint32_t);

        Buffer indexStagingBuffer(m_context, indexBufferSize,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        indexStagingBuffer.upload(indices, indexBufferSize);

        meshBuffers->indexBuffer = std::make_unique<Buffer>(
            m_context, indexBufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        Buffer::copy(m_context, indexStagingBuffer, *meshBuffers->indexBuffer, indexBufferSize);
        meshBuffers->indexCount = indexCount;
    }

    uint32_t handle;
    if (!m_freeHandles.empty()) {
        handle = m_freeHandles.back();
        m_freeHandles.pop_back();
        m_meshBuffers[handle] = std::move(meshBuffers);
    } else {
        handle = static_cast<uint32_t>(m_meshBuffers.size());
        m_meshBuffers.push_back(std::move(meshBuffers));
    }

    return handle;
}

BufferManager::MeshBuffers* BufferManager::getMeshBuffers(uint32_t handle) {
    if (handle < m_meshBuffers.size() && m_meshBuffers[handle]) {
        return m_meshBuffers[handle].get();
    }
    return nullptr;
}

void BufferManager::destroyMeshBuffers(uint32_t handle) {
    if (handle < m_meshBuffers.size() && m_meshBuffers[handle]) {
        m_meshBuffers[handle].reset();
        m_freeHandles.push_back(handle);
    }
}

} // namespace eden
