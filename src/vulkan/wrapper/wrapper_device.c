#include <sys/stat.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_bcdec.h"
#include "wrapper_bcn_spv.h"
#include "spirv_patcher.hpp"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_queue.h"
#include "vk_util.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "vk_enum_to_str.h"

const struct vk_device_extension_table wrapper_device_extensions =
{
   .KHR_swapchain = true,
   .EXT_swapchain_maintenance1 = true,
   .KHR_swapchain_mutable_format = true,
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .EXT_display_control = true,
#endif
   .KHR_present_id = true,
   .KHR_present_wait = true,
   .KHR_incremental_present = true,
};

const struct vk_device_extension_table wrapper_filter_extensions =
{
   .EXT_hdr_metadata = true,
   .GOOGLE_display_timing = true,
   .KHR_shared_presentable_image = true,
   .EXT_image_compression_control_swapchain = true,
};

inline struct wrapper_buffer *
get_wrapper_buffer_from_handle_locked(struct wrapper_device *device, VkBuffer buffer) {
   return _mesa_hash_table_u64_search(device->buffer_table, (uint64_t) buffer);
}

struct wrapper_buffer *
get_wrapper_buffer_from_handle(struct wrapper_device *device, VkBuffer buffer) {
   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_buffer *wb = get_wrapper_buffer_from_handle_locked(device, buffer);
   simple_mtx_unlock(&device->resource_mutex);

   return wb;
}

inline struct wrapper_image *
get_wrapper_image_from_handle_locked(struct wrapper_device *device, VkImage image) {
   return _mesa_hash_table_u64_search(device->image_table, (uint64_t) image);
}

struct wrapper_image *
get_wrapper_image_from_handle(struct wrapper_device *device, VkImage image) {   
   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_image *wi = get_wrapper_image_from_handle_locked(device, image);
   simple_mtx_unlock(&device->resource_mutex);
   
   return wi;
}

static struct wrapper_fence *
get_wrapper_fence_from_handle(struct wrapper_device *device, VkFence fence) {
   struct wrapper_fence *wf = NULL;

   simple_mtx_lock(&device->resource_mutex);
   wf = _mesa_hash_table_u64_search(device->fence_table, (uint64_t) fence);
   simple_mtx_unlock(&device->resource_mutex);

   return wf;
}

static void
wrapper_filter_enabled_extensions(const struct wrapper_device *device,
                                  uint32_t *enable_extension_count,
                                  const char **enable_extensions)
{
   for (int idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
      if (!device->vk.enabled_extensions.extensions[idx])
         continue;

      const char *ext_name = vk_device_extensions[idx].extensionName;
      bool is_pipeline_lib = (strcmp(ext_name, "VK_KHR_pipeline_library") == 0);

      if (!device->physical->base_supported_extensions.extensions[idx] && !is_pipeline_lib)
         continue;

      if (strcmp(ext_name, "VK_EXT_vertex_attribute_divisor") == 0 ||
          strcmp(ext_name, "VK_KHR_vertex_attribute_divisor") == 0 ||
          strcmp(ext_name, "VK_EXT_texture_compression_astc_hdr") == 0 ||
          strcmp(ext_name, "VK_EXT_extended_dynamic_state") == 0 ||
          strcmp(ext_name, "VK_EXT_extended_dynamic_state2") == 0 ||
          strcmp(ext_name, "VK_EXT_vertex_input_dynamic_state") == 0 ||
          strcmp(ext_name, "VK_KHR_sampler_ycbcr_conversion") == 0 ||
          strcmp(ext_name, "VK_KHR_shader_draw_parameters") == 0 ||
          strcmp(ext_name, "VK_EXT_filter_cubic") == 0 ||
          strcmp(ext_name, "VK_EXT_image_robustness") == 0 ||
          strcmp(ext_name, "VK_EXT_calibrated_timestamps") == 0 ||
          strcmp(ext_name, "VK_EXT_blend_operation_advanced") == 0 ||
          strcmp(ext_name, "VK_EXT_discard_rectangles") == 0 ||
          strcmp(ext_name, "VK_EXT_sample_locations") == 0 ||
          strcmp(ext_name, "VK_KHR_multiview") == 0 ||
          strcmp(ext_name, "VK_EXT_color_write_enable") == 0 ||
          strcmp(ext_name, "VK_KHR_shader_float_controls") == 0 ||
          strcmp(ext_name, "VK_EXT_shader_subgroup_ballot") == 0 ||
          strcmp(ext_name, "VK_KHR_push_descriptor") == 0 ||
          strcmp(ext_name, "VK_EXT_custom_border_color") == 0 ||
          strcmp(ext_name, "VK_EXT_private_data") == 0 ||
          strcmp(ext_name, "VK_EXT_memory_budget") == 0 ||
          strcmp(ext_name, "VK_KHR_map_memory2") == 0 ||
          strcmp(ext_name, "VK_KHR_separate_depth_stencil_layouts") == 0 ||
          strcmp(ext_name, "VK_KHR_create_renderpass2") == 0 ||
          strcmp(ext_name, "VK_KHR_depth_stencil_resolve") == 0 ||
          strcmp(ext_name, "VK_KHR_dynamic_rendering") == 0 ||
          strcmp(ext_name, "VK_KHR_image_format_list") == 0 ||
          strcmp(ext_name, "VK_KHR_maintenance5") == 0 ||
          is_pipeline_lib) {

         if (strcmp(ext_name, "VK_EXT_robustness2") == 0) {
            continue;
         }

         enable_extensions[(*enable_extension_count)++] = ext_name;
         continue;
      }

      enable_extensions[(*enable_extension_count)++] = ext_name;
   }

   if (device->vk.enabled_extensions.EXT_vertex_attribute_divisor &&
       !device->vk.enabled_extensions.KHR_vertex_attribute_divisor &&
       device->physical->base_supported_extensions.KHR_vertex_attribute_divisor) {
      enable_extensions[(*enable_extension_count)++] =
         "VK_KHR_vertex_attribute_divisor";
   }

   if (device->vk.enabled_extensions.KHR_vertex_attribute_divisor &&
       !device->vk.enabled_extensions.EXT_vertex_attribute_divisor &&
       device->physical->base_supported_extensions.EXT_vertex_attribute_divisor) {
      enable_extensions[(*enable_extension_count)++] =
         "VK_EXT_vertex_attribute_divisor";
   }
}

static inline void
wrapper_append_required_extensions(const struct vk_device *device,
                                  uint32_t *count,
                                  const char **exts) {
#define REQUIRED_EXTENSION(name) \
   if (device->physical->supported_extensions.name) { \
      exts[(*count)++] = "VK_" #name; \
   }
   
   REQUIRED_EXTENSION(KHR_external_fence);
   REQUIRED_EXTENSION(KHR_external_semaphore);
   REQUIRED_EXTENSION(KHR_external_memory);
   REQUIRED_EXTENSION(KHR_external_fence_fd);
   REQUIRED_EXTENSION(KHR_external_semaphore_fd);
   REQUIRED_EXTENSION(KHR_external_memory_fd);
   REQUIRED_EXTENSION(KHR_dedicated_allocation);
   REQUIRED_EXTENSION(EXT_queue_family_foreign);
   REQUIRED_EXTENSION(KHR_maintenance1)
   REQUIRED_EXTENSION(KHR_maintenance2)
   REQUIRED_EXTENSION(KHR_image_format_list)
   REQUIRED_EXTENSION(KHR_swapchain);
   REQUIRED_EXTENSION(KHR_timeline_semaphore);
   REQUIRED_EXTENSION(EXT_external_memory_host);
   REQUIRED_EXTENSION(EXT_external_memory_dma_buf);
   REQUIRED_EXTENSION(EXT_image_drm_format_modifier);
   REQUIRED_EXTENSION(ANDROID_external_memory_android_hardware_buffer);
#undef REQUIRED_EXTENSION
}

static void unlink_vk_struct(VkBaseInStructure *create_info, const VkBaseInStructure **current, VkBaseInStructure **prev) {
   if (!*prev) 
      create_info->pNext = (*current)->pNext;
   else
      (*prev)->pNext = (*current)->pNext;                                                

   *current = (*current)->pNext;
}

static void process_pnext_chain(VkBaseInStructure *create_info, struct wrapper_physical_device *pdevice) {
   const uint32_t api_version = pdevice->properties2.properties.apiVersion;
   const VkBaseInStructure *current = (VkBaseInStructure *)create_info->pNext;
   VkBaseInStructure *prev = NULL;

   while (current != NULL) {
      switch(current->sType) {
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
             VkPhysicalDeviceTransformFeedbackFeaturesEXT *transform_features =
                (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)current;
             transform_features->geometryStreams &= pdevice->base_supported_features.geometryStreams;
             break;
          }
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
             if (pdevice->base_supported_extensions.EXT_robustness2)
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceRobustness2FeaturesEXT from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
             if (pdevice->base_supported_extensions.EXT_dynamic_rendering_unused_attachments)
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
             if (pdevice->base_supported_extensions.KHR_maintenance5)
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceMaintenance5Features from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
             if (pdevice->base_supported_extensions.EXT_vertex_attribute_divisor)
                break;
             if (pdevice->base_supported_extensions.KHR_vertex_attribute_divisor) {
                ((VkBaseOutStructure *)current)->sType =
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR;
                break;
             }
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
             if (api_version >= VK_MAKE_VERSION(1, 1, 0))
                break;
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
             if (api_version >= VK_MAKE_VERSION(1, 2, 0))
                break;
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
             if (api_version >= VK_MAKE_VERSION(1, 3, 0))
                break;
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          default:
             break;
      }
      prev = (VkBaseInStructure *)current;
      current = current->pNext;
   }
}

static VkResult
wrapper_create_device_queue(struct wrapper_device *device,
                            const VkDeviceCreateInfo* pCreateInfo)
{
   const VkDeviceQueueCreateInfo *create_info;
   struct wrapper_queue *queue;
   VkResult result;

   for (int i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      create_info = &pCreateInfo->pQueueCreateInfos[i];
      for (int j = 0; j < create_info->queueCount; j++) {
         queue = vk_zalloc(&device->vk.alloc, sizeof(*queue), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!queue)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

         if (create_info->flags) {
            device->dispatch_table.GetDeviceQueue2(
               device->dispatch_handle,
               &(VkDeviceQueueInfo2) {
                  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                  .flags = create_info->flags,
                  .queueFamilyIndex = create_info->queueFamilyIndex,
                  .queueIndex = j,
               },
               &queue->dispatch_handle);
         } else {
            device->dispatch_table.GetDeviceQueue(
               device->dispatch_handle, create_info->queueFamilyIndex,
               j, &queue->dispatch_handle);
         }
         queue->device = device;

         result = vk_queue_init(&queue->vk, &device->vk, create_info, j);
         if (result != VK_SUCCESS) {
            vk_free(&device->vk.alloc, queue);
            return result;
         }
      }
   }

   return VK_SUCCESS;
}

static void
wrapper_create_null_resources(struct wrapper_device *device)
{
   const struct vk_device_dispatch_table *dt = &device->dispatch_table;
   VkDevice dev = device->dispatch_handle;
   VkPhysicalDeviceMemoryProperties *mp = &device->physical->memory_properties;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 65536,
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (dt->CreateBuffer(dev, &bci, NULL, &device->null_buffer) != VK_SUCCESS)
      return;
   VkMemoryRequirements mr;
   dt->GetBufferMemoryRequirements(dev, device->null_buffer, &mr);
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt = i; break; }
   if (mt == UINT32_MAX) return;
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   if (dt->AllocateMemory(dev, &mai, NULL, &device->null_buffer_memory) != VK_SUCCESS)
      return;
   dt->BindBufferMemory(dev, device->null_buffer, device->null_buffer_memory, 0);
   void *ptr = NULL;
   if (dt->MapMemory(dev, device->null_buffer_memory, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS) {
      memset(ptr, 0, mr.size);
      dt->UnmapMemory(dev, device->null_buffer_memory);
   }

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (dt->CreateImage(dev, &ici, NULL, &device->null_image) == VK_SUCCESS) {
      VkMemoryRequirements imr;
      dt->GetImageMemoryRequirements(dev, device->null_image, &imr);
      uint32_t imt = UINT32_MAX;
      for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
         if (imr.memoryTypeBits & (1u << i)) { imt = i; break; }
      VkMemoryAllocateInfo imai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = imr.size, .memoryTypeIndex = imt,
      };
      if (imt != UINT32_MAX &&
          dt->AllocateMemory(dev, &imai, NULL, &device->null_image_memory) == VK_SUCCESS) {
         dt->BindImageMemory(dev, device->null_image, device->null_image_memory, 0);
         VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = device->null_image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
         };
         dt->CreateImageView(dev, &vci, NULL, &device->null_image_view);
      }
   }
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
   dt->CreateSampler(dev, &sci, NULL, &device->null_sampler);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                             const VkWriteDescriptorSet *pDescriptorWrites,
                             uint32_t descriptorCopyCount,
                             const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (!device->emulate_null_descriptor || descriptorWriteCount == 0) {
      device->dispatch_table.UpdateDescriptorSets(device->dispatch_handle,
         descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
      return;
   }

   VkWriteDescriptorSet *writes =
      malloc(sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
   memcpy(writes, pDescriptorWrites, sizeof(VkWriteDescriptorSet) * descriptorWriteCount);

   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
      uint32_t n = w->descriptorCount;
      switch (w->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
         VkDescriptorBufferInfo *bi = malloc(sizeof(*bi) * n);
         memcpy(bi, w->pBufferInfo, sizeof(*bi) * n);
         for (uint32_t j = 0; j < n; j++)
            if (bi[j].buffer == VK_NULL_HANDLE) {
               bi[j].buffer = device->null_buffer;
               bi[j].offset = 0; bi[j].range = VK_WHOLE_SIZE;
            }
         writes[i].pBufferInfo = bi;
         break;
      }
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      case VK_DESCRIPTOR_TYPE_SAMPLER: {
         VkDescriptorImageInfo *ii = malloc(sizeof(*ii) * n);
         memcpy(ii, w->pImageInfo, sizeof(*ii) * n);
         for (uint32_t j = 0; j < n; j++) {
            if ((w->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) &&
                ii[j].imageView == VK_NULL_HANDLE) {
               ii[j].imageView = device->null_image_view;
               ii[j].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            if ((w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                 w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
                ii[j].sampler == VK_NULL_HANDLE)
               ii[j].sampler = device->null_sampler;
         }
         writes[i].pImageInfo = ii;
         break;
      }
      default:
         break;
      }
   }

   device->dispatch_table.UpdateDescriptorSets(device->dispatch_handle,
      descriptorWriteCount, writes, descriptorCopyCount, pDescriptorCopies);

   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
      if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
         free((void*)writes[i].pBufferInfo);
      else if (w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
               w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
               w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
               w->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
               w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
         free((void*)writes[i].pImageInfo);
   }
   free(writes);
}

/* =========================================================================
   🚨 IMPLEMENTACIÓN DE LA SWAPCHAIN Y WSI (Para evitar ResetSwapChain crash)
   ========================================================================= */

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateSwapchainKHR(
    VkDevice                                    _device,
    const VkSwapchainCreateInfoKHR*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSwapchainKHR*                             pSwapchain)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   WRAPPER_LOG(info, "Creating Swapchain: %dx%d, format: %d", 
               pCreateInfo->imageExtent.width, 
               pCreateInfo->imageExtent.height, 
               pCreateInfo->imageFormat);

   return device->dispatch_table.CreateSwapchainKHR(
      device->dispatch_handle, pCreateInfo, pAllocator, pSwapchain);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroySwapchainKHR(
    VkDevice                                    _device,
    VkSwapchainKHR                              swapchain,
    const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (swapchain == VK_NULL_HANDLE)
      return;

   device->dispatch_table.DestroySwapchainKHR(
      device->dispatch_handle, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetSwapchainImagesKHR(
    VkDevice                                    _device,
    VkSwapchainKHR                              swapchain,
    uint32_t*                                   pSwapchainImageCount,
    VkImage*                                    pSwapchainImages)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   return device->dispatch_table.GetSwapchainImagesKHR(
      device->dispatch_handle, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AcquireNextImageKHR(
    VkDevice                                    _device,
    VkSwapchainKHR                              swapchain,
    uint64_t                                    timeout,
    VkSemaphore                                 semaphore,
    VkFence                                     fence,
    uint32_t*                                   pImageIndex)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   return device->dispatch_table.AcquireNextImageKHR(
      device->dispatch_handle, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueuePresentKHR(
    VkQueue                                     _queue,
    const VkPresentInfoKHR*                     pPresentInfo)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);

   return queue->device->dispatch_table.QueuePresentKHR(
      queue->dispatch_handle, pPresentInfo);
}
