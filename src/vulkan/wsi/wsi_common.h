/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef WSI_COMMON_H
#define WSI_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#include "util/log.h"
#include "vk_alloc.h"
#include "vk_dispatch_table.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* Compatibilidad para tipos modernos de Vulkan en entornos Android */
#ifndef VK_VERSION_1_3
#ifndef VK_IMAGE_USAGE_FLAGS_2_KHR_DEFINED
#define VK_IMAGE_USAGE_FLAGS_2_KHR_DEFINED
typedef VkFlags VkImageUsageFlags2KHR;
typedef struct VkImageUsageFlags2CreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkImageUsageFlags2KHR usage;
} VkImageUsageFlags2CreateInfoKHR;
#ifndef VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR
#define VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR ((VkStructureType) 1000499000)
#endif
#endif
#endif

#include "vk_internal_exts.h"
#include "vk_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WSI_ENTRYPOINTS_H
extern const struct vk_instance_entrypoint_table wsi_instance_entrypoints;
extern const struct vk_physical_device_entrypoint_table wsi_physical_device_entrypoints;
extern const struct vk_device_entrypoint_table wsi_device_entrypoints;
#endif

#include <util/list.h>

struct wsi_interface;
struct vk_instance;
struct vk_queue;

struct driOptionCache;

#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_METAL + 1)

struct wsi_device {
   /* Allocator for the instance */
   VkAllocationCallbacks instance_alloc;

   VkPhysicalDevice pdevice;
   VkPhysicalDeviceMemoryProperties memory_props;
   uint32_t queue_family_count;
   uint64_t queue_supports_blit;
   uint64_t queue_supports_timestamps;
   float timestamp_period;
   uint32_t timestamp_bits;

   VkPhysicalDeviceDrmPropertiesEXT drm_info;
   VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus_info;

   VkExternalSemaphoreHandleTypeFlags semaphore_export_handle_types;
   VkExternalSemaphoreHandleTypeFlags timeline_semaphore_export_handle_types;

   bool has_import_memory_host;
   bool has_timeline_semaphore;
   bool has_host_query_reset;

   bool emulate_24as32;
   bool supports_scanout;
   bool supports_protected[VK_ICD_WSI_PLATFORM_MAX];

   bool supports_modifiers;
   uint32_t maxImageDimension2D;
   uint32_t optimalBufferCopyRowPitchAlignment;
   VkPresentModeKHR override_present_mode;
   bool force_bgra8_unorm_first;

   uint32_t cached_sync_file_import_export_result;
   bool enable_adaptive_sync;
   struct list_head hotplug_fences;
   bool force_headless_swapchain;
   bool force_swapchain_to_currentExtent;
   bool disable_unordered_submits;

   struct {
      uint32_t override_minImageCount;
      bool strict_imageCount;
      bool ensure_minImageCount;
      bool extra_xwayland_image;
      bool ignore_suboptimal;
   } x11;

   struct {
      void *(*get_d3d12_command_queue)(VkDevice device);
      bool (*requires_blits)(VkDevice device);
      VkResult (*create_image_memory)(VkDevice device, void *resource,
                                      const VkAllocationCallbacks *alloc,
                                      VkDeviceMemory *out);
   } win32;

   struct {
      VkResult (*bind_drawable_to_vkimage)(VkImage image, void *drawable);
      void (*encode_drawable_present)(VkCommandBuffer cmd, void *drawable);
   } metal;

   bool sw;
   bool wants_linear;
   bool has_present_wait;

   struct {
      bool disable_timestamps;
   } wayland;

   void (*set_memory_ownership)(VkDevice device,
                                VkDeviceMemory memory,
                                VkBool32 ownership);

   bool (*can_present_on_device)(VkPhysicalDevice pdevice, int fd);
   struct vk_queue *(*get_blit_queue)(VkDevice device);

#define WSI_CB(cb) PFN_vk##cb cb
   WSI_CB(AllocateMemory);
   WSI_CB(AllocateCommandBuffers);
   WSI_CB(BindBufferMemory);
   WSI_CB(BindImageMemory);
   WSI_CB(BeginCommandBuffer);
   WSI_CB(CmdPipelineBarrier);
   WSI_CB(CmdCopyImage);
   WSI_CB(CmdCopyImageToBuffer);
   WSI_CB(CmdResetQueryPool);
   WSI_CB(ResetQueryPoolEXT);
   WSI_CB(CmdWriteTimestamp);
   WSI_CB(CreateBuffer);
   WSI_CB(CreateCommandPool);
   WSI_CB(CreateFence);
   WSI_CB(CreateImage);
   WSI_CB(CreateQueryPool);
   WSI_CB(CreateSemaphore);
   WSI_CB(DestroyBuffer);
   WSI_CB(DestroyCommandPool);
   WSI_CB(DestroyFence);
   WSI_CB(DestroyImage);
   WSI_CB(DestroyQueryPool);
   WSI_CB(DestroySemaphore);
   WSI_CB(EndCommandBuffer);
   WSI_CB(FreeMemory);
   WSI_CB(FreeCommandBuffers);
   WSI_CB(GetBufferMemoryRequirements);
   WSI_CB(GetCalibratedTimestampsKHR);
   WSI_CB(GetFenceStatus);
   WSI_CB(GetImageDrmFormatModifierPropertiesEXT);
   WSI_CB(GetImageMemoryRequirements);
   WSI_CB(GetImageSubresourceLayout);
   WSI_CB(GetMemoryFdKHR);
   WSI_CB(GetPhysicalDeviceCalibrateableTimeDomainsKHR);
   WSI_CB(GetPhysicalDeviceProperties);
   WSI_CB(GetPhysicalDeviceFormatProperties);
   WSI_CB(GetPhysicalDeviceFormatProperties2);
   WSI_CB(GetPhysicalDeviceImageFormatProperties2);
   WSI_CB(GetPhysicalDeviceQueueFamilyProperties);
   WSI_CB(GetQueryPoolResults);
   WSI_CB(GetSemaphoreFdKHR);
   WSI_CB(ResetFences);
   WSI_CB(QueueSubmit2);
   WSI_CB(SetDebugUtilsObjectNameEXT);
   WSI_CB(WaitForFences);
   WSI_CB(MapMemory);
   WSI_CB(UnmapMemory);
   WSI_CB(WaitSemaphores);
#undef WSI_CB

    struct wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

typedef PFN_vkVoidFunction (VKAPI_PTR *WSI_FN_GetPhysicalDeviceProcAddr)(VkPhysicalDevice physicalDevice, const char* pName);

struct wsi_device_options {
   bool sw_device;
   bool extra_xwayland_image;
   bool emulate_24as32;
};

VkResult
wsi_device_init(struct wsi_device *wsi,
                VkPhysicalDevice pdevice,
                WSI_FN_GetPhysicalDeviceProcAddr proc_addr,
                const VkAllocationCallbacks *alloc,
                int display_fd,
                const struct driOptionCache *dri_options,
                const struct wsi_device_options *device_options);

void
wsi_device_finish(struct wsi_device *wsi,
                  const VkAllocationCallbacks *alloc);

void
wsi_device_setup_syncobj_fd(struct wsi_device *wsi_device,
                            int fd);

#define ICD_DEFINE_NONDISP_HANDLE_CASTS(__VkIcdType, __VkType)             \
                                                                           \
   static inline __VkIcdType *                                             \
   __VkIcdType ## _from_handle(__VkType _handle)                           \
   {                                                                       \
      return (__VkIcdType *)(uintptr_t) _handle;                           \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __VkIcdType ## _to_handle(__VkIcdType *_obj)                            \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define ICD_FROM_HANDLE(__VkIcdType, __name, __handle) \
   __VkIcdType *__name = __VkIcdType ## _from_handle(__handle)

ICD_DEFINE_NONDISP_HANDLE_CASTS(VkIcdSurfaceBase, VkSurfaceKHR)

VkDeviceMemory
wsi_common_get_memory(VkSwapchainKHR _swapchain, uint32_t index);

VkResult
wsi_common_acquire_next_image2(const struct wsi_device *wsi,
                               VkDevice device,
                               const VkAcquireNextImageInfoKHR *pAcquireInfo,
                               uint32_t *pImageIndex);

VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         struct vk_queue *queue,
                         const VkPresentInfoKHR *pPresentInfo);

static inline bool
wsi_common_is_swapchain_image(const VkImageCreateInfo *pCreateInfo)
{
#ifdef VK_USE_PLATFORM_ANDROID_KHR
   return false;
# else
   const VkImageSwapchainCreateInfoKHR *swapchain_info =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_SWAPCHAIN_CREATE_INFO_KHR);
   return swapchain_info && swapchain_info->swapchain != VK_NULL_HANDLE;
#endif
}

VkResult
wsi_common_create_swapchain_image(const struct wsi_device *wsi,
                                  const VkImageCreateInfo *pCreateInfo,
                                  VkImage *pImage);

VkImageUsageFlags
wsi_caps_get_image_usage(void);

bool
wsi_instance_supports_google_display_timing(const struct vk_instance *instance,
                                            const struct driOptionCache *dri_options);

bool
wsi_device_supports_explicit_sync(struct wsi_device *device);

static inline VkImageUsageFlags2KHR
vk_swapchain_usage_flags(const VkSwapchainCreateInfoKHR *info)
{
   const VkImageUsageFlags2CreateInfoKHR *usage2 =
      vk_find_struct_const(info->pNext,
                           IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2)
      return usage2->usage;
   else
      return info->imageUsage;
}

#define wsi_common_vk_warn_once(warning) \
   do { \
      static int warned = false; \
      if (!warned) { \
         mesa_loge(warning); \
         warned = true; \
      } \
   } while (0)

#ifdef __cplusplus
}
#endif

#endif
