#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace eden {

class VulkanContext;

class Pipeline {
public:
    Pipeline(VulkanContext& context, VkRenderPass renderPass, VkExtent2D extent);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    VkPipeline getHandle() const { return m_pipeline; }
    VkPipelineLayout getLayout() const { return m_pipelineLayout; }

private:
    void createPipelineLayout();
    void createPipeline(VkRenderPass renderPass, VkExtent2D extent);

    VulkanContext& m_context;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace eden
