#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {

class VulkanDevice;
class VulkanSubmissionTracker;

class VulkanNisUpscaler {
 public:
  static constexpr uint32_t kMaxScale = 2;

  static std::unique_ptr<VulkanNisUpscaler> Create(const VulkanDevice* vulkan_device,
                                                   VkSampler linear_clamp_sampler);

  VulkanNisUpscaler(const VulkanNisUpscaler&) = delete;
  VulkanNisUpscaler& operator=(const VulkanNisUpscaler&) = delete;
  ~VulkanNisUpscaler();

  bool Dispatch(VkCommandBuffer command_buffer, VulkanSubmissionTracker& tracker,
                uint64_t submission, VkImageView input_view, uint32_t input_width,
                uint32_t input_height, VkImageView output_view, uint32_t output_width,
                uint32_t output_height, float sharpness);

 private:
  static constexpr uint32_t kDescriptorSetCount = 4;
  static constexpr uint32_t kCoefficientWidth = 2;
  static constexpr uint32_t kCoefficientHeight = 64;

  struct Coefficients {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  explicit VulkanNisUpscaler(const VulkanDevice* vulkan_device) : vulkan_device_(vulkan_device) {}

  bool Initialize(VkSampler linear_clamp_sampler);
  bool CreateCoefficients(Coefficients& coefficients);
  void UploadCoefficients(VkCommandBuffer command_buffer);

  const VulkanDevice* vulkan_device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kDescriptorSetCount> descriptor_sets_ = {};
  std::array<uint64_t, kDescriptorSetCount> descriptor_set_submissions_ = {};
  Coefficients scaler_;
  Coefficients usm_;
  VkBuffer staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  bool coefficients_uploaded_ = false;
  uint32_t logged_input_width_ = 0;
  uint32_t logged_input_height_ = 0;
  uint32_t logged_output_width_ = 0;
  uint32_t logged_output_height_ = 0;
};

}  // namespace rex::ui::vulkan
