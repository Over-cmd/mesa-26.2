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

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util/cnd_monotonic.h"
#include "util/timespec.h"
#include "util/u_thread.h"
#include "vk_format.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#define D3D12_IGNORE_SDK_LAYERS
#include <dxgi1_4.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <dcomp.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#endif

struct wsi_win32;

struct wsi_win32 {
   struct wsi_interface                     base;
   struct wsi_device *wsi;
   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
   struct {
      IDXGIFactory4 *factory;
      IDCompositionDevice *dcomp;
   } dxgi;
};

enum wsi_win32_image_state {
   WSI_IMAGE_IDLE,
   WSI_IMAGE_DRAWING,
   WSI_IMAGE_QUEUED,
};

struct wsi_win32_image {
   struct wsi_image base;
   enum wsi_win32_image_state state;
   struct wsi_win32_swapchain *chain;
   struct {
      ID3D12Resource *swapchain_res;
   } dxgi;
   struct {
      HDC dc;
      HBITMAP bmp;
      int bmp_row_pitch;
      void *ppvBits;
   } sw;
};

struct wsi_win32_surface {
   VkIcdSurfaceWin32 base;
   IDCompositionTarget *target;
   IDCompositionVisual *visual;
   struct wsi_win32_swapchain *current_swapchain;
};

struct wsi_win32_swapchain {
   struct wsi_swapchain         base;
   IDXGISwapChain3            *dxgi;
   struct wsi_win32           *wsi;
   wsi_win32_surface          *surface;
   mtx_t                      acquire_mutex;
   struct u_cnd_monotonic     acquire_cond;
   uint64_t                     flip_sequence;
   VkResult                     status;
   VkExtent2D                 extent;
   HWND wnd;
   HDC chain_dc;
   struct wsi_win32_image     images[0];
};

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;
   return (wsi_device->queue_supports_blit & BITFIELD64_BIT(queueFamilyIndex)) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateWin32SurfaceKHR(VkInstance _instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR);

   surface = (wsi_win32_surface *)vk_zalloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.base.platform = VK_ICD_WSI_PLATFORM_WIN32;
   surface->base.hinstance = pCreateInfo->hinstance;
   surface->base.hwnd = pCreateInfo->hwnd;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base.base);
   return VK_SUCCESS;
}

void
wsi_win32_surface_destroy(VkIcdSurfaceBase *icd_surface, VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   if (surface->visual)
      surface->visual->Release();
   if (surface->target)
      surface->target->Release();
   vk_free2(&instance->alloc, pAllocator, icd_surface);
}

static VkResult
wsi_win32_surface_get_support(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device, uint32_t queueFamilyIndex, VkBool32* pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities(VkIcdSurfaceBase *surf, struct wsi_device *wsi_device, VkSurfaceCapabilities2KHR* caps)
{
   VkIcdSurfaceWin32 *surface = (VkIcdSurfaceWin32 *)surf;
   RECT win_rect;
   if (!GetClientRect(surface->hwnd, &win_rect))
      return VK_ERROR_SURFACE_LOST_KHR;

   caps->surfaceCapabilities.minImageCount = 1;
   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {
      caps->surfaceCapabilities.minImageCount = caps->surfaceCapabilities.maxImageCount = 2;
   } else {
      caps->surfaceCapabilities.minImageCount = 1;
      caps->surfaceCapabilities.maxImageCount = 0;
   }

   caps->surfaceCapabilities.currentExtent = {
      (uint32_t)win_rect.right - (uint32_t)win_rect.left,
      (uint32_t)win_rect.bottom - (uint32_t)win_rect.top
   };
   caps->surfaceCapabilities.minImageExtent = { 1u, 1u };
   caps->surfaceCapabilities.maxImageExtent = {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;
   caps->surfaceCapabilities.supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;

   caps->surfaceCapabilities.supportedUsageFlags = wsi_caps_get_image_usage();
   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities2(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device, const void *info_next, VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
   return wsi_win32_surface_get_capabilities(surface, wsi_device, caps);
}

static const struct {
   VkFormat     format;
} available_surface_formats[] = {
   { VK_FORMAT_B8G8R8A8_UNORM },
   { VK_FORMAT_R8G8B8A8_UNORM },
   { VK_FORMAT_B8G8R8A8_SRGB },
};

static void
get_sorted_vk_formats(struct wsi_device *wsi_device, VkFormat *sorted_formats)
{
   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++)
      sorted_formats[i] = available_surface_formats[i].format;
}

static VkResult
wsi_win32_surface_get_formats(VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   VkFormat sorted_formats[ARRAY_SIZE(available_surface_formats)];
   get_sorted_vk_formats(wsi_device, sorted_formats);

   for (unsigned i = 0; i < ARRAY_SIZE(sorted_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = sorted_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_win32_surface_get_formats2(VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device, const void *info_next, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   VkFormat sorted_formats[ARRAY_SIZE(available_surface_formats)];
   get_sorted_vk_formats(wsi_device, sorted_formats);

   for (unsigned i = 0; i < ARRAY_SIZE(sorted_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = sorted_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static const VkPresentModeKHR present_modes_gdi[] = { VK_PRESENT_MODE_FIFO_KHR };
static const VkPresentModeKHR present_modes_dxgi[] = {
   VK_PRESENT_MODE_IMMEDIATE_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_win32_surface_get_present_modes(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes)
{
   const VkPresentModeKHR *array = (wsi_device->sw || !wsi_device->win32.get_d3d12_command_queue) ? present_modes_gdi : present_modes_dxgi;
   size_t array_size = (wsi_device->sw || !wsi_device->win32.get_d3d12_command_queue) ? ARRAY_SIZE(present_modes_gdi) : ARRAY_SIZE(present_modes_dxgi);

   if (pPresentModes == NULL) {
      *pPresentModeCount = array_size;
      return VK_SUCCESS;
   }
   *pPresentModeCount = MIN2(*pPresentModeCount, array_size);
   typed_memcpy(pPresentModes, array, *pPresentModeCount);
   return *pPresentModeCount < array_size ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_present_rectangles(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device, uint32_t* pRectCount, VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = { { 0, 0 }, { UINT32_MAX, UINT32_MAX } };
   }
   return vk_outarray_status(&out);
}

static void
wsi_win32_set_image_idle(struct wsi_win32_swapchain *chain, struct wsi_win32_image *image)
{
   if (!chain->dxgi)
      mtx_lock(&chain->acquire_mutex);

   image->state = WSI_IMAGE_IDLE;

   if (!chain->dxgi) {
      u_cnd_monotonic_broadcast(&chain->acquire_cond);
      mtx_unlock(&chain->acquire_mutex);
   }
}

static VkResult
wsi_win32_release_images(struct wsi_swapchain *drv_chain, uint32_t count, const uint32_t *indices)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   if (chain->status == VK_ERROR_SURFACE_LOST_KHR)
      return chain->status;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);
      assert(chain->images[index].state == WSI_IMAGE_DRAWING);
      wsi_win32_set_image_idle(chain, &chain->images[index]);
   }
   return VK_SUCCESS;
}

static bool
wsi_win32_find_idle_image(struct wsi_win32_swapchain *chain, uint32_t *out_image_index)
{
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].state == WSI_IMAGE_IDLE) {
         *out_image_index = i;
         chain->images[i].state = WSI_IMAGE_DRAWING;
         return true;
      }
   }
   return false;
}

static VkResult
wsi_win32_acquire_next_image(struct wsi_swapchain *drv_chain, const VkAcquireNextImageInfoKHR *info, uint32_t *image_index)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   if (chain->status != VK_SUCCESS)
      return chain->status;

   if (!chain->dxgi) {
      mtx_lock(&chain->acquire_mutex);
      if (wsi_win32_find_idle_image(chain, image_index)) {
         mtx_unlock(&chain->acquire_mutex);
         return VK_SUCCESS;
      }
      if (info->timeout == 0) {
         mtx_unlock(&chain->acquire_mutex);
         return VK_NOT_READY;
      }
      const uint64_t abs_timeout = os_time_get_absolute_timeout(info->timeout);
      struct timespec abs_timespec;
      timespec_from_nsec(&abs_timespec, abs_timeout);
      int ret = 0;
      while (!wsi_win32_find_idle_image(chain, image_index)) {
         ret = u_cnd_monotonic_timedwait(&chain->acquire_cond, &chain->acquire_mutex, &abs_timespec);
         if (ret == thrd_timedout) {
            mtx_unlock(&chain->acquire_mutex);
            return VK_TIMEOUT;
         } else if (ret != thrd_success) {
            mtx_unlock(&chain->acquire_mutex);
            return VK_ERROR_OUT_OF_DATE_KHR;
         }
      }
      mtx_unlock(&chain->acquire_mutex);
      return VK_SUCCESS;
   }

   if (wsi_win32_find_idle_image(chain, image_index))
      return VK_SUCCESS;

   uint32_t index = chain->dxgi->GetCurrentBackBufferIndex();
   if (chain->images[index].state == WSI_IMAGE_DRAWING) {
      index = (index + 1) % chain->base.image_count;
   }
   *image_index = index;
   chain->images[index].state = WSI_IMAGE_DRAWING;
   return VK_SUCCESS;
}

static VkResult
wsi_win32_swapchain_destroy(struct wsi_swapchain *drv_chain, const VkAllocationCallbacks *allocator)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *) drv_chain;
   u_cnd_monotonic_destroy(&chain->acquire_cond);
   mtx_destroy(&chain->acquire_mutex);
   wsi_swapchain_finish(&chain->base);
   vk_free(allocator, chain);
   return VK_SUCCESS;
}
