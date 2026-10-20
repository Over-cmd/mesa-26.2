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

#include <X11/Xlib-xcb.h>
#include <X11/xshmfence.h>
#define XK_MISCELLANY
#define XK_LATIN1
#include <X11/keysymdef.h>
#include <xcb/xcb.h>
#ifdef XCB_KEYSYMS_AVAILABLE
#include <xcb/xcb_keysyms.h>
#endif
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>

#include "util/macros.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "drm-uapi/drm_fourcc.h"
#include "util/libdrm.h"
#include "util/cnd_monotonic.h"
#include "util/hash_table.h"
#include "util/mesa-blake3.h"
#include "util/os_file.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_thread.h"
#include "util/xmlconfig.h"
#include "util/timespec.h"
#include "x11/x11_display.h"

#include "vk_format.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_device.h"
#include "vk_util.h"
#include "vk_enum_to_str.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "wsi_common_queue.h"
#include "loader/loader_dri_helper_screen.h"

#ifdef HAVE_SYS_SHM_H
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#ifndef XCB_PRESENT_OPTION_ASYNC_MAY_TEAR
#define XCB_PRESENT_OPTION_ASYNC_MAY_TEAR 16
#endif
#ifndef XCB_PRESENT_CAPABILITY_ASYNC_MAY_TEAR
#define XCB_PRESENT_CAPABILITY_ASYNC_MAY_TEAR 8
#endif

#define MAX_DAMAGE_RECTS 64

struct wsi_x11_screen_resources {
   struct list_head link;
   struct loader_screen_resources screen_resources;
   mtx_t mtx;
};

struct wsi_x11_connection {
   bool has_dri3;
   bool has_dri3_modifiers;
   bool has_dri3_explicit_sync;
   bool has_present;
   bool is_proprietary_x11;
   bool is_xwayland;
   bool has_mit_shm;
   bool has_xfixes;

   struct list_head screen_resources_list;
};

struct wsi_x11 {
   struct wsi_interface base;
   mtx_t mutex;
   struct hash_table *connections;
};

struct wsi_x11_vk_surface {
   union {
      VkIcdSurfaceXlib xlib;
      VkIcdSurfaceXcb xcb;
   };
   bool has_alpha;
};

#ifdef HAVE_X11_DRM
static int
wsi_dri3_open(xcb_connection_t *conn, xcb_window_t root, uint32_t provider)
{
   xcb_dri3_open_cookie_t cookie = xcb_dri3_open(conn, root, provider);
   xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(conn, cookie, NULL);
   if (!reply)
      return -1;

   if (reply->nfd != 1) {
      free(reply);
      return -1;
   }

   int fd = xcb_dri3_open_reply_fds(conn, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
   return fd;
}

static bool
wsi_x11_check_dri3_compatible(const struct wsi_device *wsi_dev, xcb_connection_t *conn)
{
   xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
   xcb_screen_t *screen = screen_iter.data;

   int dri3_fd = wsi_dri3_open(conn, screen->root, None);
   if (dri3_fd == -1)
      return true;

   bool match = wsi_dev->can_present_on_device(wsi_dev->pdevice, dri3_fd);
   close(dri3_fd);
   return match;
}
#endif

static bool
wsi_x11_detect_xwayland(xcb_connection_t *conn,
                        xcb_query_extension_reply_t *randr_reply,
                        xcb_query_extension_reply_t *xwl_reply)
{
   if (xwl_reply && xwl_reply->present)
      return true;
   if (!randr_reply || !randr_reply->present)
      return false;

   xcb_randr_query_version_cookie_t ver_cookie = xcb_randr_query_version_unchecked(conn, 1, 3);
   xcb_randr_query_version_reply_t *ver_reply = xcb_randr_query_version_reply(conn, ver_cookie, NULL);
   bool has_randr_v1_3 = ver_reply && (ver_reply->major_version > 1 || ver_reply->minor_version >= 3);
   free(ver_reply);
   if (!has_randr_v1_3)
      return false;

   const xcb_setup_t *setup = xcb_get_setup(conn);
   xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);

   xcb_randr_get_screen_resources_current_cookie_t gsr_cookie =
      xcb_randr_get_screen_resources_current_unchecked(conn, iter.data->root);
   xcb_randr_get_screen_resources_current_reply_t *gsr_reply =
      xcb_randr_get_screen_resources_current_reply(conn, gsr_cookie, NULL);

   if (!gsr_reply || gsr_reply->num_outputs == 0) {
      free(gsr_reply);
      return false;
   }

   xcb_randr_output_t *randr_outputs = xcb_randr_get_screen_resources_current_outputs(gsr_reply);
   xcb_randr_get_output_info_cookie_t goi_cookie = xcb_randr_get_output_info(conn, randr_outputs[0], gsr_reply->config_timestamp);
   free(gsr_reply);

   xcb_randr_get_output_info_reply_t *goi_reply = xcb_randr_get_output_info_reply(conn, goi_cookie, NULL);
   if (!goi_reply)
      return false;

   char *output_name = (char*)xcb_randr_get_output_info_name(goi_reply);
   bool is_xwayland = output_name && strncmp(output_name, "XWAYLAND", 8) == 0;
   free(goi_reply);

   return is_xwayland;
}

static struct wsi_x11_connection *
wsi_x11_connection_create(struct wsi_device *wsi_dev, xcb_connection_t *conn)
{
   xcb_query_extension_cookie_t dri3_cookie, pres_cookie, randr_cookie,
                                amd_cookie, nv_cookie, shm_cookie, sync_cookie,
                                xfixes_cookie, xwl_cookie;
   xcb_query_extension_reply_t *dri3_reply, *pres_reply, *randr_reply,
                               *amd_reply, *nv_reply, *shm_reply = NULL,
                               *xfixes_reply, *xwl_reply;
   bool wants_shm = wsi_dev->sw && !(WSI_DEBUG & WSI_DEBUG_NOSHM) &&
                    wsi_dev->has_import_memory_host;
   bool has_dri3_v1_2 = false;
   bool has_present_v1_2 = false;
   bool has_dri3_v1_4 = false;
   bool has_present_v1_4 = false;

   struct wsi_x11_connection *wsi_conn =
      vk_alloc(&wsi_dev->instance_alloc, sizeof(*wsi_conn), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi_conn)
      return NULL;

   sync_cookie = xcb_query_extension(conn, 4, "SYNC");
   dri3_cookie = xcb_query_extension(conn, 4, "DRI3");
   pres_cookie = xcb_query_extension(conn, 7, "Present");
   randr_cookie = xcb_query_extension(conn, 5, "RANDR");
   xfixes_cookie = xcb_query_extension(conn, 6, "XFIXES");
   xwl_cookie = xcb_query_extension(conn, 8, "XWAYLAND");

   if (wants_shm)
      shm_cookie = xcb_query_extension(conn, 7, "MIT-SHM");

   amd_cookie = xcb_query_extension(conn, 11, "ATIFGLRXDRI");
   nv_cookie = xcb_query_extension(conn, 10, "NV-CONTROL");

   xcb_discard_reply(conn, sync_cookie.sequence);
   dri3_reply = xcb_query_extension_reply(conn, dri3_cookie, NULL);
   pres_reply = xcb_query_extension_reply(conn, pres_cookie, NULL);
   randr_reply = xcb_query_extension_reply(conn, randr_cookie, NULL);
   amd_reply = xcb_query_extension_reply(conn, amd_cookie, NULL);
   nv_reply = xcb_query_extension_reply(conn, nv_cookie, NULL);
   xfixes_reply = xcb_query_extension_reply(conn, xfixes_cookie, NULL);
   xwl_reply = xcb_query_extension_reply(conn, xwl_cookie, NULL);
   if (wants_shm)
      shm_reply = xcb_query_extension_reply(conn, shm_cookie, NULL);

   if (!dri3_reply || !pres_reply || !xfixes_reply) {
      free(dri3_reply); free(pres_reply); free(xfixes_reply);
      free(xwl_reply); free(randr_reply); free(amd_reply); free(nv_reply);
      if (wants_shm) free(shm_reply);
      vk_free(&wsi_dev->instance_alloc, wsi_conn);
      return NULL;
   }

   wsi_conn->has_dri3 = dri3_reply->present != 0;
#ifdef HAVE_X11_DRM
   if (wsi_conn->has_dri3) {
      xcb_dri3_query_version_cookie_t ver_cookie = xcb_dri3_query_version(conn, 1, 4);
      xcb_dri3_query_version_reply_t *ver_reply = xcb_dri3_query_version_reply(conn, ver_cookie, NULL);
      has_dri3_v1_2 = ver_reply != NULL && (ver_reply->major_version > 1 || ver_reply->minor_version >= 2);
      has_dri3_v1_4 = ver_reply != NULL && (ver_reply->major_version > 1 || ver_reply->minor_version >= 4);
      free(ver_reply);
   }
#endif

   wsi_conn->has_present = pres_reply->present != 0;
#ifdef HAVE_X11_DRM
   if (wsi_conn->has_present) {
      xcb_present_query_version_cookie_t ver_cookie = xcb_present_query_version(conn, 1, 4);
      xcb_present_query_version_reply_t *ver_reply = xcb_present_query_version_reply(conn, ver_cookie, NULL);
      has_present_v1_2 = ver_reply != NULL && (ver_reply->major_version > 1 || ver_reply->minor_version >= 2);
      has_present_v1_4 = ver_reply != NULL && (ver_reply->major_version > 1 || ver_reply->minor_version >= 4);
      free(ver_reply);
   }
#endif

   wsi_conn->has_xfixes = xfixes_reply->present != 0;
   if (wsi_conn->has_xfixes) {
      xcb_xfixes_query_version_cookie_t ver_cookie = xcb_xfixes_query_version(conn, 6, 0);
      xcb_xfixes_query_version_reply_t *ver_reply = xcb_xfixes_query_version_reply(conn, ver_cookie, NULL);
      if (ver_reply) {
         wsi_conn->has_xfixes = (ver_reply->major_version >= 2);
         free(ver_reply);
      }
   }

   wsi_conn->is_xwayland = wsi_x11_detect_xwayland(conn, randr_reply, xwl_reply);
   wsi_conn->has_dri3_modifiers = has_dri3_v1_2 && has_present_v1_2;
   wsi_conn->has_dri3_explicit_sync = has_dri3_v1_4 && has_present_v1_4;
   wsi_conn->is_proprietary_x11 = (amd_reply && amd_reply->present) || (nv_reply && nv_reply->present);
   
   wsi_conn->has_mit_shm = false;
#if defined(HAVE_X11_DRM) && defined(HAVE_SYS_SHM_H)
   if (wsi_conn->has_dri3 && wsi_conn->has_present && wants_shm) {
      wsi_conn->has_mit_shm = x11_xcb_display_supports_xshm(conn, NULL);
   }
#endif

   list_inithead(&wsi_conn->screen_resources_list);
   xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
   for (xcb_screen_t *screen = it.data; it.rem != 0; xcb_screen_next(&it), screen = it.data) {
      struct wsi_x11_screen_resources *link =
         vk_alloc(&wsi_dev->instance_alloc, sizeof(*link), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (link) {
         mtx_init(&link->mtx, mtx_plain);
         loader_init_screen_resources(&link->screen_resources, conn, screen);
         list_addtail(&link->link, &wsi_conn->screen_resources_list);
      }
   }

   free(dri3_reply); free(pres_reply); free(randr_reply); free(xwl_reply);
   free(amd_reply); free(nv_reply); free(xfixes_reply);
   if (wants_shm) free(shm_reply);

   return wsi_conn;
}

static void
wsi_x11_connection_destroy(struct wsi_device *wsi_dev, struct wsi_x11_connection *conn)
{
   list_for_each_entry_safe(struct wsi_x11_screen_resources, resources, &conn->screen_resources_list, link) {
      loader_destroy_screen_resources(&resources->screen_resources);
      mtx_destroy(&resources->mtx);
      vk_free(&wsi_dev->instance_alloc, resources);
   }
   vk_free(&wsi_dev->instance_alloc, conn);
}

static struct wsi_x11_connection *
wsi_x11_get_connection(struct wsi_device *wsi_dev, xcb_connection_t *conn)
{
   struct wsi_x11 *wsi = (struct wsi_x11 *)wsi_dev->wsi[VK_ICD_WSI_PLATFORM_XCB];
   mtx_lock(&wsi->mutex);

   struct hash_entry *entry = _mesa_hash_table_search(wsi->connections, conn);
   if (!entry) {
      mtx_unlock(&wsi->mutex);
      struct wsi_x11_connection *wsi_conn = wsi_x11_connection_create(wsi_dev, conn);
      if (!wsi_conn)
         return NULL;

      mtx_lock(&wsi->mutex);
      entry = _mesa_hash_table_search(wsi->connections, conn);
      if (entry) {
         wsi_x11_connection_destroy(wsi_dev, wsi_conn);
      } else {
         entry = _mesa_hash_table_insert(wsi->connections, conn, wsi_conn);
      }
   }
   mtx_unlock(&wsi->mutex);
   return entry->data;
}

static VkResult
x11_surface_get_support(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        uint32_t queueFamilyIndex,
                        VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static VkResult
x11_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              const void *pNext,
                              VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   VkSurfaceCapabilitiesKHR *caps = &pSurfaceCapabilities->surfaceCapabilities;
   
   caps->minImageCount = 2;
   caps->maxImageCount = 0;
   caps->currentExtent = (VkExtent2D){ UINT32_MAX, UINT32_MAX };
   caps->minImageExtent = (VkExtent2D){ 1, 1 };
   caps->maxImageExtent = (VkExtent2D){ wsi_device->maxImageDimension2D, wsi_device->maxImageDimension2D };
   caps->maxImageArrayLayers = 1;
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
   caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

   return VK_SUCCESS;
}

static VkResult
x11_surface_get_formats(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   vk_outarray_append_typed(VkSurfaceFormatKHR, &out, fmt) {
      fmt->format = VK_FORMAT_B8G8R8A8_UNORM;
      fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   vk_outarray_append_typed(VkSurfaceFormatKHR, &out, fmt) {
      fmt->format = VK_FORMAT_B8G8R8A8_SRGB;
      fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_formats2(VkIcdSurfaceBase *surface,
                         struct wsi_device *wsi_device,
                         const void *pNext,
                         uint32_t *pSurfaceFormatCount,
                         VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, fmt) {
      fmt->surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
      fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, fmt) {
      fmt->surfaceFormat.format = VK_FORMAT_B8G8R8A8_SRGB;
      fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_present_modes(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t *pPresentModeCount,
                              VkPresentModeKHR *pPresentModes)
{
   VkPresentModeKHR modes[] = {
      VK_PRESENT_MODE_FIFO_KHR,
      VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_IMMEDIATE_KHR,
   };

   uint32_t count = ARRAY_SIZE(modes);
   if (!pPresentModes) {
      *pPresentModeCount = count;
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, count);
   memcpy(pPresentModes, modes, *pPresentModeCount * sizeof(VkPresentModeKHR));

   return *pPresentModeCount < count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
x11_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                   struct wsi_device *wsi_device,
                                   uint32_t *pRectCount,
                                   VkRect2D *pRects)
{
   *pRectCount = 1;
   if (!pRects)
      return VK_SUCCESS;

   pRects[0] = (VkRect2D){
      .offset = { 0, 0 },
      .extent = { wsi_device->maxImageDimension2D, wsi_device->maxImageDimension2D },
   };

   return VK_SUCCESS;
}

static VkResult
x11_surface_create_swapchain(VkIcdSurfaceBase *surface,
                             VkDevice device,
                             struct wsi_device *wsi_device,
                             const VkSwapchainCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             struct wsi_swapchain **out_swapchain)
{
   struct wsi_swapchain *chain = vk_zalloc(pAllocator, sizeof(*chain), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chain)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = wsi_swapchain_init(wsi_device, chain, device, pCreateInfo, NULL, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   *out_swapchain = chain;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_x11_init_wsi(struct wsi_device *wsi_device,
                 const VkAllocationCallbacks *alloc,
                 const struct driOptionCache *dri_options)
{
   struct wsi_x11 *wsi;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (mtx_init(&wsi->mutex, mtx_plain) != thrd_success) {
      vk_free(alloc, wsi);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wsi->connections = _mesa_hash_table_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!wsi->connections) {
      mtx_destroy(&wsi->mutex);
      vk_free(alloc, wsi);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   if (dri_options) {
      if (driCheckOption(dri_options, "vk_x11_override_min_image_count", DRI_INT))
         wsi_device->x11.override_minImageCount = driQueryOptioni(dri_options, "vk_x11_override_min_image_count");
      if (driCheckOption(dri_options, "vk_x11_strict_image_count", DRI_BOOL))
         wsi_device->x11.strict_imageCount = driQueryOptionb(dri_options, "vk_x11_strict_image_count");
      if (driCheckOption(dri_options, "vk_x11_ensure_min_image_count", DRI_BOOL))
         wsi_device->x11.ensure_minImageCount = driQueryOptionb(dri_options, "vk_x11_ensure_min_image_count");
      if (driCheckOption(dri_options, "vk_x11_ignore_suboptimal", DRI_BOOL))
         wsi_device->x11.ignore_suboptimal = driQueryOptionb(dri_options, "vk_x11_ignore_suboptimal");
   }

   wsi->base.get_support = x11_surface_get_support;
   wsi->base.get_capabilities2 = x11_surface_get_capabilities2;
   wsi->base.get_formats = x11_surface_get_formats;
   wsi->base.get_formats2 = x11_surface_get_formats2;
   wsi->base.get_present_modes = x11_surface_get_present_modes;
   wsi->base.get_present_rectangles = x11_surface_get_present_rectangles;
   wsi->base.create_swapchain = x11_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XCB] = &wsi->base;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XLIB] = &wsi->base;

   return VK_SUCCESS;
}

void
wsi_x11_finish_wsi(struct wsi_device *wsi_device,
                   const VkAllocationCallbacks *alloc)
{
   struct wsi_x11 *wsi =
      (struct wsi_x11 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_XCB];

   if (wsi) {
      hash_table_foreach(wsi->connections, entry)
         wsi_x11_connection_destroy(wsi_device, entry->data);

      _mesa_hash_table_destroy(wsi->connections, NULL);
      mtx_destroy(&wsi->mutex);
      vk_free(alloc, wsi);
   }
}
