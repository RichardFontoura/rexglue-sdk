#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace rex::ui::vulkan {

class VulkanDevice;
class VulkanSubmissionTracker;

class VulkanDlssUpscaler {
 public:
  static constexpr uint32_t kMaxScale = 3;

  static bool Requested();
  static std::vector<std::string> InstanceExtensions();
  static std::vector<std::string> DeviceExtensions(VkInstance instance,
                                                   VkPhysicalDevice physical_device);
  static std::unique_ptr<VulkanDlssUpscaler> Create(const VulkanDevice* vulkan_device);

  VulkanDlssUpscaler(const VulkanDlssUpscaler&) = delete;
  VulkanDlssUpscaler& operator=(const VulkanDlssUpscaler&) = delete;
  ~VulkanDlssUpscaler();

  bool Dispatch(VkCommandBuffer command_buffer, VulkanSubmissionTracker& tracker,
                uint64_t submission, VkImage input_image, VkImageView input_view,
                uint32_t input_width, uint32_t input_height, VkImage output_image,
                VkImageView output_view, uint32_t output_width, uint32_t output_height);

 private:
  struct AuxImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  explicit VulkanDlssUpscaler(const VulkanDevice* vulkan_device) : vulkan_device_(vulkan_device) {}

  bool Initialize();
  bool EnsureFeature(VkCommandBuffer command_buffer, VulkanSubmissionTracker& tracker,
                     uint32_t input_width, uint32_t input_height, uint32_t output_width,
                     uint32_t output_height);
  void ReleaseFeature();
  bool CreateAuxImage(AuxImage& aux, VkFormat format, uint32_t width, uint32_t height);
  void DestroyAuxImage(AuxImage& aux);
  void ClearAuxImages(VkCommandBuffer command_buffer);

  const VulkanDevice* vulkan_device_;
  bool ngx_initialized_ = false;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t feature_input_width_ = 0;
  uint32_t feature_input_height_ = 0;
  uint32_t feature_output_width_ = 0;
  uint32_t feature_output_height_ = 0;
  bool feature_failed_ = false;
  uint64_t last_submission_ = 0;
  AuxImage depth_;
  AuxImage motion_vectors_;
};

}  // namespace rex::ui::vulkan
