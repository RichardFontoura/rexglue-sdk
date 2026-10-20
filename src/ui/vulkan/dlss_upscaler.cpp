#include "dlss_upscaler.h"

#include <algorithm>
#include <cctype>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, present_effect);

namespace rex::ui::vulkan {

bool VulkanDlssUpscaler::Requested() {
#if defined(REX_HAS_DLSS) && REX_HAS_DLSS
  std::string effect = REXCVAR_GET(present_effect);
  std::transform(effect.begin(), effect.end(), effect.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return effect == "dlss";
#else
  return false;
#endif
}

}  // namespace rex::ui::vulkan

#if defined(REX_HAS_DLSS) && REX_HAS_DLSS

#include <array>
#include <filesystem>
#include <system_error>

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/submission_tracker.h>
#include <rex/ui/vulkan/util.h>

namespace rex::ui::vulkan {

namespace {

constexpr char kProjectId[] = "5f3f1c3e-8f0e-4b8e-9c56-2b6f1a7d9e41";
constexpr char kEngineVersion[] = "0.10";
constexpr VkFormat kColorFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotionVectorFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr NVSDK_NGX_PerfQuality_Value kQualityModes[] = {
    NVSDK_NGX_PerfQuality_Value_DLAA,
    NVSDK_NGX_PerfQuality_Value_MaxQuality,
    NVSDK_NGX_PerfQuality_Value_Balanced,
    NVSDK_NGX_PerfQuality_Value_MaxPerf,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance,
};

struct NgxPaths {
  std::wstring data;
  std::array<std::wstring, 2> search;
  std::array<const wchar_t*, 2> search_pointers;
  NVSDK_NGX_FeatureCommonInfo common_info = {};
};

void NVSDK_CONV LogNgxMessage(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string_view line(message);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  REXLOG_DEBUG("NGX: {}", line);
}

NgxPaths& Paths() {
  static NgxPaths paths = [] {
    NgxPaths result;
    std::error_code ec;
    const std::filesystem::path data = std::filesystem::temp_directory_path(ec) / "rexglue-ngx";
    std::filesystem::create_directories(data, ec);
    result.data = data.wstring();
    const std::filesystem::path exe_folder = rex::filesystem::GetExecutableFolder();
    result.search = {exe_folder.wstring(), (exe_folder.parent_path() / "lib").wstring()};
    for (size_t i = 0; i < result.search.size(); ++i) {
      result.search_pointers[i] = result.search[i].c_str();
    }
    result.common_info.PathListInfo.Path = result.search_pointers.data();
    result.common_info.PathListInfo.Length = uint32_t(result.search_pointers.size());
    result.common_info.LoggingInfo.LoggingCallback = LogNgxMessage;
    result.common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    result.common_info.LoggingInfo.DisableOtherLoggingSinks = true;
    return result;
  }();
  return paths;
}

NVSDK_NGX_FeatureDiscoveryInfo DiscoveryInfo() {
  NVSDK_NGX_FeatureDiscoveryInfo info = {};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
  info.ApplicationDataPath = Paths().data.c_str();
  info.FeatureInfo = &Paths().common_info;
  return info;
}

std::vector<std::string> ExtensionNames(uint32_t count, const VkExtensionProperties* properties) {
  std::vector<std::string> names;
  for (uint32_t i = 0; properties && i < count; ++i) {
    names.emplace_back(properties[i].extensionName);
  }
  return names;
}

NVSDK_NGX_Resource_VK ImageResource(VkImage image, VkImageView view, VkFormat format,
                                    uint32_t width, uint32_t height, bool read_write) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, util::InitializeSubresourceRange(),
                                                format, width, height, read_write);
}

}  // namespace

std::vector<std::string> VulkanDlssUpscaler::InstanceExtensions() {
  const NVSDK_NGX_FeatureDiscoveryInfo info = DiscoveryInfo();
  uint32_t count = 0;
  VkExtensionProperties* properties = nullptr;
  if (NVSDK_NGX_FAILED(
          NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties))) {
    return {};
  }
  return ExtensionNames(count, properties);
}

std::vector<std::string> VulkanDlssUpscaler::DeviceExtensions(VkInstance instance,
                                                              VkPhysicalDevice physical_device) {
  const NVSDK_NGX_FeatureDiscoveryInfo info = DiscoveryInfo();
  uint32_t count = 0;
  VkExtensionProperties* properties = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
          instance, physical_device, &info, &count, &properties))) {
    return {};
  }
  return ExtensionNames(count, properties);
}

std::unique_ptr<VulkanDlssUpscaler> VulkanDlssUpscaler::Create(const VulkanDevice* vulkan_device) {
  auto upscaler = std::unique_ptr<VulkanDlssUpscaler>(new VulkanDlssUpscaler(vulkan_device));
  if (!upscaler->Initialize()) {
    return nullptr;
  }
  return upscaler;
}

VulkanDlssUpscaler::~VulkanDlssUpscaler() {
  ReleaseFeature();
  if (parameters_) {
    NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
  }
  if (ngx_initialized_) {
    NVSDK_NGX_VULKAN_Shutdown1(vulkan_device_->device());
  }
}

bool VulkanDlssUpscaler::Initialize() {
  const VulkanInstance* vulkan_instance = vulkan_device_->vulkan_instance();
  const PFN_vkGetInstanceProcAddr get_instance_proc_addr =
      vulkan_instance->functions().vkGetInstanceProcAddr;
  const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      get_instance_proc_addr(vulkan_instance->instance(), "vkGetDeviceProcAddr"));
  if (!get_device_proc_addr) {
    REXLOG_WARN("VulkanDlssUpscaler: vkGetDeviceProcAddr is unavailable");
    return false;
  }

  NgxPaths& paths = Paths();
  const NVSDK_NGX_Result init_result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, paths.data.c_str(),
      vulkan_instance->instance(), vulkan_device_->physical_device(), vulkan_device_->device(),
      get_instance_proc_addr, get_device_proc_addr, &paths.common_info);
  if (NVSDK_NGX_FAILED(init_result)) {
    REXLOG_WARN("VulkanDlssUpscaler: NGX initialization failed (0x{:08X})", uint32_t(init_result));
    return false;
  }
  ngx_initialized_ = true;

  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters_))) {
    REXLOG_WARN("VulkanDlssUpscaler: NGX capability parameters are unavailable");
    return false;
  }
  int needs_driver = 0;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
          parameters_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver)) &&
      needs_driver) {
    REXLOG_WARN("VulkanDlssUpscaler: DLSS needs a newer NVIDIA driver");
    return false;
  }
  int available = 0;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(
          parameters_, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
      !available) {
    REXLOG_WARN("VulkanDlssUpscaler: DLSS is not available on this GPU");
    return false;
  }
  REXLOG_INFO("VulkanDlssUpscaler: DLSS available");
  return true;
}

bool VulkanDlssUpscaler::CreateAuxImage(AuxImage& aux, VkFormat format, uint32_t width,
                                        uint32_t height) {
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(
          vulkan_device_, image_info, util::MemoryPurpose::kDeviceLocal, aux.image, aux.memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = aux.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = util::InitializeSubresourceRange();
  return vulkan_device_->functions().vkCreateImageView(vulkan_device_->device(), &view_info,
                                                       nullptr, &aux.view) == VK_SUCCESS;
}

void VulkanDlssUpscaler::DestroyAuxImage(AuxImage& aux) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, aux.view);
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, aux.image);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, aux.memory);
}

void VulkanDlssUpscaler::ClearAuxImages(VkCommandBuffer command_buffer) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkImage images[] = {depth_.image, motion_vectors_.image};

  std::array<VkImageMemoryBarrier, 2> barriers = {};
  for (size_t i = 0; i < barriers.size(); ++i) {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = util::InitializeSubresourceRange();
  }
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           uint32_t(barriers.size()), barriers.data());

  const VkClearColorValue zero = {};
  const VkImageSubresourceRange range = util::InitializeSubresourceRange();
  for (VkImage image : images) {
    dfn.vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
  }

  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                           uint32_t(barriers.size()), barriers.data());
}

void VulkanDlssUpscaler::ReleaseFeature() {
  if (feature_) {
    NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
    feature_ = nullptr;
  }
  DestroyAuxImage(depth_);
  DestroyAuxImage(motion_vectors_);
}

bool VulkanDlssUpscaler::EnsureFeature(VkCommandBuffer command_buffer,
                                       VulkanSubmissionTracker& tracker, uint32_t input_width,
                                       uint32_t input_height, uint32_t output_width,
                                       uint32_t output_height) {
  if (feature_input_width_ == input_width && feature_input_height_ == input_height &&
      feature_output_width_ == output_width && feature_output_height_ == output_height) {
    return feature_ != nullptr && !feature_failed_;
  }
  if (feature_) {
    tracker.AwaitSubmissionCompletion(last_submission_);
  }
  ReleaseFeature();
  feature_input_width_ = input_width;
  feature_input_height_ = input_height;
  feature_output_width_ = output_width;
  feature_output_height_ = output_height;
  feature_failed_ = true;

  NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
  bool quality_found = false;
  for (const NVSDK_NGX_PerfQuality_Value mode : kQualityModes) {
    uint32_t optimal_width = 0, optimal_height = 0, max_width = 0, max_height = 0;
    uint32_t min_width = 0, min_height = 0;
    float sharpness = 0.0f;
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(
            parameters_, output_width, output_height, mode, &optimal_width, &optimal_height,
            &max_width, &max_height, &min_width, &min_height, &sharpness)) ||
        !optimal_width || !optimal_height) {
      continue;
    }
    if (input_width >= min_width && input_width <= max_width && input_height >= min_height &&
        input_height <= max_height) {
      quality = mode;
      quality_found = true;
      break;
    }
  }
  if (!quality_found) {
    REXLOG_WARN("VulkanDlssUpscaler: no DLSS mode accepts {}x{} to {}x{}", input_width,
                input_height, output_width, output_height);
    return false;
  }

  if (!CreateAuxImage(depth_, kDepthFormat, input_width, input_height) ||
      !CreateAuxImage(motion_vectors_, kMotionVectorFormat, input_width, input_height)) {
    REXLOG_ERROR("VulkanDlssUpscaler: Failed to create the depth and motion vector images");
    ReleaseFeature();
    return false;
  }
  ClearAuxImages(command_buffer);

  NVSDK_NGX_DLSS_Create_Params create_params = {};
  create_params.Feature.InWidth = input_width;
  create_params.Feature.InHeight = input_height;
  create_params.Feature.InTargetWidth = output_width;
  create_params.Feature.InTargetHeight = output_height;
  create_params.Feature.InPerfQualityValue = quality;
  create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
  const NVSDK_NGX_Result create_result = NGX_VULKAN_CREATE_DLSS_EXT1(
      vulkan_device_->device(), command_buffer, 1, 1, &feature_, parameters_, &create_params);
  if (NVSDK_NGX_FAILED(create_result)) {
    REXLOG_WARN("VulkanDlssUpscaler: DLSS feature creation failed (0x{:08X})",
                uint32_t(create_result));
    feature_ = nullptr;
    ReleaseFeature();
    return false;
  }
  REXLOG_INFO("VulkanDlssUpscaler: {}x{} to {}x{}, quality mode {}", input_width, input_height,
              output_width, output_height, int(quality));
  feature_failed_ = false;
  return true;
}

bool VulkanDlssUpscaler::Dispatch(VkCommandBuffer command_buffer, VulkanSubmissionTracker& tracker,
                                  uint64_t submission, VkImage input_image, VkImageView input_view,
                                  uint32_t input_width, uint32_t input_height, VkImage output_image,
                                  VkImageView output_view, uint32_t output_width,
                                  uint32_t output_height) {
  if (!EnsureFeature(command_buffer, tracker, input_width, input_height, output_width,
                     output_height)) {
    return false;
  }

  NVSDK_NGX_Resource_VK color =
      ImageResource(input_image, input_view, kColorFormat, input_width, input_height, false);
  NVSDK_NGX_Resource_VK output =
      ImageResource(output_image, output_view, kColorFormat, output_width, output_height, true);
  NVSDK_NGX_Resource_VK depth =
      ImageResource(depth_.image, depth_.view, kDepthFormat, input_width, input_height, false);
  NVSDK_NGX_Resource_VK motion_vectors =
      ImageResource(motion_vectors_.image, motion_vectors_.view, kMotionVectorFormat, input_width,
                    input_height, false);

  NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
  eval_params.Feature.pInColor = &color;
  eval_params.Feature.pInOutput = &output;
  eval_params.pInDepth = &depth;
  eval_params.pInMotionVectors = &motion_vectors;
  eval_params.InRenderSubrectDimensions.Width = input_width;
  eval_params.InRenderSubrectDimensions.Height = input_height;
  eval_params.InReset = 1;
  eval_params.InMVScaleX = 1.0f;
  eval_params.InMVScaleY = 1.0f;
  const NVSDK_NGX_Result eval_result =
      NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, feature_, parameters_, &eval_params);
  if (NVSDK_NGX_FAILED(eval_result)) {
    REXLOG_WARN("VulkanDlssUpscaler: DLSS evaluation failed (0x{:08X})", uint32_t(eval_result));
    return false;
  }
  last_submission_ = submission;
  return true;
}

}  // namespace rex::ui::vulkan

#else

namespace rex::ui::vulkan {

std::vector<std::string> VulkanDlssUpscaler::InstanceExtensions() {
  return {};
}

std::vector<std::string> VulkanDlssUpscaler::DeviceExtensions(VkInstance, VkPhysicalDevice) {
  return {};
}

std::unique_ptr<VulkanDlssUpscaler> VulkanDlssUpscaler::Create(const VulkanDevice*) {
  return nullptr;
}

VulkanDlssUpscaler::~VulkanDlssUpscaler() = default;

bool VulkanDlssUpscaler::Dispatch(VkCommandBuffer, VulkanSubmissionTracker&, uint64_t, VkImage,
                                  VkImageView, uint32_t, uint32_t, VkImage, VkImageView, uint32_t,
                                  uint32_t) {
  return false;
}

}  // namespace rex::ui::vulkan

#endif
