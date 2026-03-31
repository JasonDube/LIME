#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace eden {

class VulkanContext;

class Buffer {
public:
    Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    VkBuffer getHandle() const { return m_buffer; }
    VkDeviceSize getSize() const { return m_size; }

    void* map();
    void unmap();
    void upload(const void* data, VkDeviceSize size);

    static void copy(VulkanContext& context, Buffer& src, Buffer& dst, VkDeviceSize size);

    // VRAM usage tracking (across all Vulkan allocations)
    static std::atomic<int64_t> s_vramUsedBytes;
    static std::unordered_map<VkDeviceMemory, int64_t> s_vramAllocSizes;
    static std::mutex s_vramMutex;

    static void trackVramAlloc(int64_t bytes) { s_vramUsedBytes += bytes; }
    static void trackVramFree(int64_t bytes)  { s_vramUsedBytes -= bytes; }
    static int64_t getVramUsedBytes() { return s_vramUsedBytes.load(); }

    // Track by handle — records size on alloc, looks it up on free
    static void trackVramAllocHandle(VkDeviceMemory mem, int64_t bytes);
    static void trackVramFreeHandle(VkDeviceMemory mem);

private:
    VulkanContext& m_context;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_size;
    void* m_mapped = nullptr;

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

// Manages multiple mesh buffers
class BufferManager {
public:
    BufferManager(VulkanContext& context);
    ~BufferManager();

    struct MeshBuffers {
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };

    uint32_t createMeshBuffers(const void* vertices, uint32_t vertexCount, size_t vertexSize,
                               const uint32_t* indices = nullptr, uint32_t indexCount = 0);

    MeshBuffers* getMeshBuffers(uint32_t handle);
    void destroyMeshBuffers(uint32_t handle);

private:
    VulkanContext& m_context;
    std::vector<std::unique_ptr<MeshBuffers>> m_meshBuffers;
    std::vector<uint32_t> m_freeHandles;
};

} // namespace eden
