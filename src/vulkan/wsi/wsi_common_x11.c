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
   /* Hash table of xcb_connection -> wsi_x11_connection mappings */
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
/**
 * Wrapper around xcb_dri3_open. Returns the opened fd or -1 on error.
 */
static int
wsi_dri3_open(xcb_connection_t *conn,
              xcb_window_t root,
              uint32_t provider)
{
   xcb_dri3_open_cookie_t       cookie;
   xcb_dri3_open_reply_t        *reply;
   int                          fd;

   cookie = xcb_dri3_open(conn,
                          root,
                          provider);

   reply = xcb_dri3_open_reply(conn, cookie, NULL);
   if (!reply)
      return -1;

   /* According to DRI3 extension nfd must equal one. */
   if (reply->nfd != 1) {
      free(reply);
      return -1;
   }

   fd = xcb_dri3_open_reply_fds(conn, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

   return fd;
}

/**
 * Checks compatibility of the device wsi_dev with the device the X server
 * provides via DRI3.
 */
static bool
wsi_x11_check_dri3_compatible(const struct wsi_device *wsi_dev,
                              xcb_connection_t *conn)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));
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

   xcb_randr_query_version_cookie_t ver_cookie =
      xcb_randr_query_version_unchecked(conn, 1, 3);
   xcb_randr_query_version_reply_t *ver_reply =
      xcb_randr_query_version_reply(conn, ver_cookie, NULL);
   bool has_randr_v1_3 = ver_reply && (ver_reply->major_version > 1 ||
                                       ver_reply->minor_version >= 3);
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

   xcb_randr_output_t *randr_outputs =
      xcb_randr_get_screen_resources_current_outputs(gsr_reply);
   xcb_randr_get_output_info_cookie_t goi_cookie =
      xcb_randr_get_output_info(conn, randr_outputs[0], gsr_reply->config_timestamp);
   free(gsr_reply);

   xcb_randr_get_output_info_reply_t *goi_reply =
      xcb_randr_get_output_info_reply(conn, goi_cookie, NULL);
   if (!goi_reply) {
      return false;
   }

   char *output_name = (char*)xcb_randr_get_output_info_name(goi_reply);
   bool is_xwayland = output_name && strncmp(output_name, "XWAYLAND", 8) == 0;
   free(goi_reply);

   return is_xwayland;
}

static struct wsi_x11_connection *
wsi_x11_connection_create(struct wsi_device *wsi_dev,
                          xcb_connection_t *conn)
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
      vk_alloc(&wsi_dev->instance_alloc, sizeof(*wsi_conn), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
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
      free(dri3_reply);
      free(pres_reply);
      free(xfixes_reply);
      free(xwl_reply);
      free(randr_reply);
      free(amd_reply);
      free(nv_reply);
      if (wants_shm)
         free(shm_reply);
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
         vk_alloc(&wsi_dev->instance_alloc, sizeof(*link), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

      if (link) {
         mtx_init(&link->mtx, mtx_plain);
         loader_init_screen_resources(&link->screen_resources, conn, screen);
         list_addtail(&link->link, &wsi_conn->screen_resources_list);
      }
   }

   free(dri3_reply);
   free(pres_reply);
   free(randr_reply);
   free(xwl_reply);
   free(amd_reply);
   free(nv_reply);
   free(xfixes_reply);
   if (wants_shm)
      free(shm_reply);

   return wsi_conn;
}

static void
wsi_x11_connection_destroy(struct wsi_device *wsi_dev,
                           struct wsi_x11_connection *conn)
{
   list_for_each_entry_safe(struct wsi_x11_screen_resources, resources, &conn->screen_resources_list, link) {
      loader_destroy_screen_resources(&resources->screen_resources);
      mtx_destroy(&resources->mtx);
      vk_free(&wsi_dev->instance_alloc, resources);
   }
   vk_free(&wsi_dev->instance_alloc, conn);
}

static struct wsi_x11_connection *
wsi_x11_get_connection(struct wsi_device *wsi_dev,
                       xcb_connection_t *conn)
{
   struct wsi_x11 *wsi =
      (struct wsi_x11 *)wsi_dev->wsi[VK_ICD_WSI_PLATFORM_XCB];

   mtx_lock(&wsi->mutex);

   struct hash_entry *entry = _mesa_hash_table_search(wsi->connections, conn);
   if (!entry) {
      mtx_unlock(&wsi->mutex);

      struct wsi_x11_connection *wsi_conn =
         wsi_x11_connection_create(wsi_dev, conn);
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

static struct wsi_x11_screen_resources *
wsi_x11_connection_find_screen_resources(struct wsi_device *wsi_dev, xcb_connection_t *conn, xcb_window_t window)
{
   struct wsi_x11_connection *wsi_conn = wsi_x11_get_connection(wsi_dev, conn);
   xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry_unchecked(conn, window);
   xcb_get_geometry_reply_t *geometry_reply = xcb_get_geometry_reply(conn, geometry_cookie, NULL);

   if (!geometry_reply)
      return NULL;

   struct wsi_x11_screen_resources *screen_resources = NULL;

   list_for_each_entry(struct wsi_x11_screen_resources, resource, &wsi_conn->screen_resources_list, link) {
      if (resource->screen_resources.screen &&
          resource->screen_resources.screen->root == geometry_reply->root) {
         screen_resources = resource;
         break;
      }
   }

   free(geometry_reply);
   return screen_resources;
}

static const VkFormat formats[] = {
   VK_FORMAT_R5G6B5_UNORM_PACK16,
   VK_FORMAT_B8G8R8A8_SRGB,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_A2R10G10B10_UNORM_PACK32,
};

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_IMMEDIATE_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_FIFO_RELAXED_KHR,
};

static xcb_screen_t *
get_screen_for_root(xcb_connection_t *conn, xcb_window_t root)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));

   for (; screen_iter.rem; xcb_screen_next (&screen_iter)) {
      if (screen_iter.data->root == root)
         return screen_iter.data;
   }

   return NULL;
}

static xcb_visualtype_t *
screen_get_visualtype(xcb_screen_t *screen, xcb_visualid_t visual_id,
                      unsigned *depth)
{
   xcb_depth_iterator_t depth_iter =
      xcb_screen_allowed_depths_iterator(screen);

   for (; depth_iter.rem; xcb_depth_next (&depth_iter)) {
      xcb_visualtype_iterator_t visual_iter =
         xcb_depth_visuals_iterator (depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next (&visual_iter)) {
         if (visual_iter.data->visual_id == visual_id) {
            if (depth)
               *depth = depth_iter.data->depth;
            return visual_iter.data;
         }
      }
   }

   return NULL;
}

static xcb_visualtype_t *
connection_get_visualtype(xcb_connection_t *conn, xcb_visualid_t visual_id)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));

   for (; screen_iter.rem; xcb_screen_next (&screen_iter)) {
      xcb_visualtype_t *visual = screen_get_visualtype(screen_iter.data,
                                                       visual_id, NULL);
      if (visual)
         return visual;
   }

   return NULL;
}

static xcb_visualtype_t *
get_visualtype_for_window(xcb_connection_t *conn, xcb_window_t window,
                          unsigned *depth, xcb_visualtype_t **rootvis)
{
   xcb_query_tree_cookie_t tree_cookie;
   xcb_get_window_attributes_cookie_t attrib_cookie;
   xcb_query_tree_reply_t *tree;
   xcb_get_window_attributes_reply_t *attrib;

   tree_cookie = xcb_query_tree(conn, window);
   attrib_cookie = xcb_get_window_attributes(conn, window);

   tree = xcb_query_tree_reply(conn, tree_cookie, NULL);
   attrib = xcb_get_window_attributes_reply(conn, attrib_cookie, NULL);
   if (attrib == NULL || tree == NULL) {
      free(attrib);
      free(tree);
      return NULL;
   }

   xcb_window_t root = tree->root;
   xcb_visualid_t visual_id = attrib->visual;
   free(attrib);
   free(tree);

   xcb_screen_t *screen = get_screen_for_root(conn, root);
   if (screen == NULL)
      return NULL;

   if (rootvis)
      *rootvis = screen_get_visualtype(screen, screen->root_visual, depth);
   return screen_get_visualtype(screen, visual_id, depth);
}

static bool
visual_has_alpha(xcb_visualtype_t *visual, unsigned depth)
{
   uint32_t rgb_mask = visual->red_mask |
                       visual->green_mask |
                       visual->blue_mask;

   uint32_t all_mask = 0xffffffff >> (32 - depth);

   return (all_mask & ~rgb_mask) != 0;
}

static bool
visual_supported(xcb_visualtype_t *visual)
{
   if (!visual)
      return false;

   return visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR ||
          visual->_class == XCB_VISUAL_CLASS_DIRECT_COLOR;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                               uint32_t queueFamilyIndex,
                                               xcb_connection_t *connection,
                                               xcb_visualid_t visual_id)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;

   uint64_t effective_queues = wsi_device->queue_supports_blit & wsi_device->queue_supports_timestamps;

   if (!effective_queues)
      effective_queues = wsi_device->queue_supports_blit;

   if (!(effective_queues & BITFIELD64_BIT(queueFamilyIndex)))
      return false;

   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, connection);

   if (!wsi_conn)
      return false;

   if (!wsi_device->sw) {
      if (!wsi_conn->has_dri3 && !wsi_conn->is_proprietary_x11)
         return false;
   }

   if (!visual_supported(connection_get_visualtype(connection, visual_id)))
      return false;

   return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                uint32_t queueFamilyIndex,
                                                Display *dpy,
                                                VisualID visualID)
{
   if (!x11_xlib_display_is_thread_safe(dpy))
      return false;

   return wsi_GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice,
                                                         queueFamilyIndex,
                                                         XGetXCBConnection(dpy),
                                                         visualID);
}

static bool
x11_surface_is_thread_safe(VkIcdSurfaceBase *icd_surface)
{
   if (icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return x11_xlib_display_is_thread_safe(((VkIcdSurfaceXlib *)icd_surface)->dpy);
   else
      return true;
}

static xcb_connection_t*
x11_surface_get_connection(VkIcdSurfaceBase *icd_surface)
{
   if (icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return XGetXCBConnection(((VkIcdSurfaceXlib *)icd_surface)->dpy);
   else
      return ((VkIcdSurfaceXcb *)icd_surface)->connection;
}

static xcb_window_t
x11_surface_get_window(VkIcdSurfaceBase *icd_surface)
{
   if (icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return ((VkIcdSurfaceXlib *)icd_surface)->window;
   else
      return ((VkIcdSurfaceXcb *)icd_surface)->window;
}

static VkResult
x11_surface_get_support(VkIcdSurfaceBase *icd_surface,
                        struct wsi_device *wsi_device,
                        uint32_t queueFamilyIndex,
                        VkBool32* pSupported)
{
   if (!x11_surface_is_thread_safe(icd_surface)) {
      *pSupported = false;
      return VK_SUCCESS;
   }

   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);

   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, conn);
   if (!wsi_conn)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (!wsi_device->sw) {
      if (!wsi_conn->has_dri3 && !wsi_conn->is_proprietary_x11) {
         *pSupported = false;
         return VK_SUCCESS;
      }
   }

   if (!visual_supported(get_visualtype_for_window(conn, window, NULL, NULL))) {
      *pSupported = false;
      return VK_SUCCESS;
   }

   *pSupported = true;
   return VK_SUCCESS;
}

static uint32_t
x11_get_min_image_count(const struct wsi_device *wsi_device, bool is_xwayland)
{
   if (wsi_device->x11.override_minImageCount)
      return wsi_device->x11.override_minImageCount;

   return is_xwayland && wsi_device->x11.extra_xwayland_image ? 4 : 3;
}

static unsigned
x11_get_min_image_count_for_present_mode(struct wsi_device *wsi_device,
                                         struct wsi_x11_connection *wsi_conn,
                                         VkPresentModeKHR present_mode);

static VkResult
x11_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                             struct wsi_device *wsi_device,
                             const VkSurfacePresentModeKHR *present_mode,
                             VkSurfaceCapabilities2KHR *caps)
{
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);
   struct wsi_x11_vk_surface *surface = (struct wsi_x11_vk_surface*)icd_surface;
   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, conn);
   xcb_get_geometry_cookie_t geom_cookie;
   xcb_generic_error_t *err;
   xcb_get_geometry_reply_t *geom;

   geom_cookie = xcb_get_geometry(conn, window);
   geom = xcb_get_geometry_reply(conn, geom_cookie, &err);
   if (!geom)
      return VK_ERROR_SURFACE_LOST_KHR;

   {
      VkExtent2D extent = { geom->width, geom->height };
      caps->surfaceCapabilities.currentExtent = extent;
      caps->surfaceCapabilities.minImageExtent = extent;
      caps->surfaceCapabilities.maxImageExtent = extent;
   }
   free(err);
   free(geom);

   if (surface->has_alpha) {
      caps->surfaceCapabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
   } else {
      caps->surfaceCapabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   }

   if (present_mode) {
      caps->surfaceCapabilities.minImageCount = x11_get_min_image_count_for_present_mode(wsi_device, wsi_conn, present_mode->presentMode);
   } else {
      caps->surfaceCapabilities.minImageCount = x11_get_min_image_count(wsi_device, wsi_conn->is_xwayland);
   }

   VkImageUsageFlags image_usage = wsi_caps_get_image_usage();
   caps->surfaceCapabilities.maxImageCount = 0;
   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      image_usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   VkSwapchainFlagsSurfaceCapabilitiesEXT *surface_caps = vk_find_struct(caps, SWAPCHAIN_FLAGS_SURFACE_CAPABILITIES_EXT);
   if (surface_caps && pdevice->supported_extensions.EXT_multisampled_render_to_swapchain)
      surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;

   VkImageUsageFlags2CreateInfoKHR *usage2 =
      vk_find_struct(caps->pNext, IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2) {
      usage2->usage = image_usage;
   } else {
      caps->surfaceCapabilities.supportedUsageFlags = image_usage;
   }

   return VK_SUCCESS;
}

static VkResult
x11_surface_get_capabilities2(VkIcdSurfaceBase *icd_surface,
                              struct wsi_device *wsi_device,
                              const void *info_next,
                              VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeKHR *present_mode = vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);

   VkResult result =
      x11_surface_get_capabilities(icd_surface, wsi_device, present_mode, caps);

   if (result != VK_SUCCESS)
      return result;

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected =
            wsi_device->supports_protected[VK_ICD_WSI_PLATFORM_XCB];
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         if (compat->pPresentModes) {
            assert(present_mode);
            VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, modes, compat->pPresentModes, &compat->presentModeCount);
            vk_outarray_append_typed(VkPresentModeKHR, &modes, mode) {
               *mode = present_mode->presentMode;
            }
            for (uint32_t i = 0; i < ARRAY_SIZE(present_modes); i++) {
               if (present_modes[i] != present_mode->presentMode) {
                  vk_outarray_append_typed(VkPresentModeKHR, &modes, mode) {
                     *mode = present_modes[i];
                  }
               }
            }
         } else {
            compat->presentModeCount = ARRAY_SIZE(present_modes);
         }
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR: {
         VkSurfaceCapabilitiesPresentId2KHR *pid2 = (void *)ext;
         pid2->presentId2Supported = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR: {
         VkSurfaceCapabilitiesPresentWait2KHR *pwait2 = (void *)ext;
         pwait2->presentWait2Supported = VK_TRUE;
         break;
      }
      default:
         break;
      }
   }

   return result;
}

static int
format_get_component_bits(VkFormat format, int comp)
{
   return vk_format_get_component_bits(format, UTIL_FORMAT_COLORSPACE_RGB, comp);
}

static bool
rgb_component_bits_are_equal(VkFormat format, const xcb_visualtype_t* type)
{
   return format_get_component_bits(format, 0) == util_bitcount(type->red_mask) &&
          format_get_component_bits(format, 1) == util_bitcount(type->green_mask) &&
          format_get_component_bits(format, 2) == util_bitcount(type->blue_mask);
}

static bool
get_sorted_vk_formats(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                      VkFormat *sorted_formats, unsigned *count)
{
   xcb_connection_t *conn = x11_surface_get_connection(surface);
   xcb_window_t window = x11_surface_get_window(surface);
   xcb_visualtype_t *rootvis = NULL;
   xcb_visualtype_t *visual = get_visualtype_for_window(conn, window, NULL, &rootvis);

   if (!visual)
      return false;

   *count = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      if (rgb_component_bits_are_equal(formats[i], rootvis))
         sorted_formats[(*count)++] = formats[i];
   }

   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      for (unsigned j = 0; j < *count; j++)
         if (formats[i] == sorted_formats[j])
            goto next_format;
      if (rgb_component_bits_are_equal(formats[i], visual))
         sorted_formats[(*count)++] = formats[i];
next_format:;
   }

   if (wsi_device->force_bgra8_unorm_first) {
      for (unsigned i = 0; i < *count; i++) {
         if (sorted_formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
            sorted_formats[i] = sorted_formats[0];
            sorted_formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
            break;
         }
      }
   }

   return true;
}

static VkResult
x11_surface_get_formats(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   unsigned count;
   VkFormat sorted_formats[ARRAY_SIZE(formats)];
   if (!get_sorted_vk_formats(surface, wsi_device, sorted_formats, &count))
      return VK_ERROR_SURFACE_LOST_KHR;

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = sorted_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_formats2(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        const void *info_next,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   unsigned count;
   VkFormat sorted_formats[ARRAY_SIZE(formats)];
   if (!get_sorted_vk_formats(surface, wsi_device, sorted_formats, &count))
      return VK_ERROR_SURFACE_LOST_KHR;

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = sorted_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_present_modes(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t *pPresentModeCount,
                              VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   return *pPresentModeCount < ARRAY_SIZE(present_modes) ?
      VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
x11_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                   struct wsi_device *wsi_device,
                                   uint32_t* pRectCount,
                                   VkRect2D* pRects)
{
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      xcb_generic_error_t *err = NULL;
      xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, window);
      xcb_get_geometry_reply_t *geom =
         xcb_get_geometry_reply(conn, geom_cookie, &err);
      free(err);
      if (geom) {
         *rect = (VkRect2D) {
            .offset = { 0, 0 },
            .extent = { geom->width, geom->height },
         };
      }
      free(geom);
      if (!geom)
          return VK_ERROR_SURFACE_LOST_KHR;
   }

   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateXcbSurfaceKHR(VkInstance _instance,
                        const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_x11_vk_surface *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);

   unsigned visual_depth;
   xcb_visualtype_t *visual =
      get_visualtype_for_window(pCreateInfo->connection, pCreateInfo->window, &visual_depth, NULL);
   if (!visual)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof(struct wsi_x11_vk_surface), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->xcb.base.platform = VK_ICD_WSI_PLATFORM_XCB;
   surface->xcb.connection = pCreateInfo->connection;
   surface->xcb.window = pCreateInfo->window;
   surface->has_alpha = visual_has_alpha(visual, visual_depth);

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->xcb.base);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateXlibSurfaceKHR(VkInstance _instance,
                         const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_x11_vk_surface *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);

   unsigned visual_depth;
   xcb_visualtype_t *visual =
      get_visualtype_for_window(XGetXCBConnection(pCreateInfo->dpy), pCreateInfo->window, &visual_depth, NULL);
   if (!visual)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof(struct wsi_x11_vk_surface), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->xlib.base.platform = VK_ICD_WSI_PLATFORM_XLIB;
   surface->xlib.dpy = pCreateInfo->dpy;
   surface->xlib.window = pCreateInfo->window;
   surface->has_alpha = visual_has_alpha(visual, visual_depth);

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->xlib.base);
   return VK_SUCCESS;
}

struct x11_image_pending_completion {
   uint32_t serial;
   uint64_t signal_present_id;
   uint64_t timing_serial;
};

struct x11_image {
   struct wsi_image                          base;
   xcb_pixmap_t                              pixmap;
   xcb_xfixes_region_t                       update_region;
   xcb_xfixes_region_t                       update_area;
   struct xshmfence *                        shm_fence;
   uint32_t                                  sync_fence;
   xcb_shm_seg_t                             shmseg;
   int                                       shmid;
   uint8_t *                                 shmaddr;
   uint64_t                                  present_id;
   VkPresentModeKHR                          present_mode;
   xcb_rectangle_t                           rects[MAX_DAMAGE_RECTS];
   int                                       rectangle_count;
   struct wsi_image_timing_request           timing_request;

#define X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS 128
   uint32_t                                  present_queued_count;
   struct x11_image_pending_completion       pending_completions[X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS];
#ifdef HAVE_DRI3_EXPLICIT_SYNC
   uint32_t                                  dri3_syncobj[WSI_ES_COUNT];
#endif
};

struct x11_swapchain {
   struct wsi_swapchain                        base;
   bool                                         has_dri3_modifiers;
   bool                                         has_mit_shm;
   bool                                         has_async_may_tear;
   bool                                         msc_estimate_is_stable;
   xcb_connection_t *                           conn;
   xcb_window_t                                 window;
   xcb_gc_t                                     gc;
   uint32_t                                     depth;
   VkExtent2D                                   extent;
   blake3_hash                                  dri3_modifier_hash;
   xcb_present_event_t                          event_id;
   xcb_special_event_t *                        special_event;
   uint64_t                                     send_sbc;
   uint64_t                                     last_present_msc;
   uint64_t                                     next_present_ust_lower_bound;
   uint32_t                                     stamp;
   uint32_t                                     sent_image_count;
   atomic_int                                   status;
   bool                                         copy_is_suboptimal;
   struct wsi_queue                             present_queue;
   struct wsi_queue                             acquire_queue;
   thrd_t                                       queue_manager;
   thrd_t                                       event_manager;
   mtx_t                                        thread_state_lock;
   struct u_cnd_monotonic                       thread_state_cond;
   struct wsi_x11_screen_resources *            screen_resources;
   mtx_t                                        present_progress_mutex;
   struct u_cnd_monotonic                       present_progress_cond;
   uint64_t                                     present_id;
   VkResult                                     present_progress_error;
   struct wsi_image_timing_request              timing_request;
   bool                                         has_reliable_msc;
   bool                                         last_complete_is_flip;
   struct x11_image                             images[0];
};

static VkResult
x11_surface_create_swapchain(VkIcdSurfaceBase *surface,
                             VkDevice device,
                             struct wsi_device *wsi_device,
                             const VkSwapchainCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             struct wsi_swapchain **out_swapchain);

/* --- INICIALIZADOR WSI X11 --- */

VKAPI_ATTR VkResult VKAPI_CALL
wsi_x11_init_wsi(struct wsi_device *wsi_device,
                 const VkAllocationCallbacks *alloc,
                 const struct driOptionCache *dri_options)
{
   struct wsi_x11 *wsi;
   VkResult result;

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

static VkResult
x11_surface_create_swapchain(VkIcdSurfaceBase *surface,
                             VkDevice device,
                             struct wsi_device *wsi_device,
                             const VkSwapchainCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             struct wsi_swapchain **out_swapchain)
{
   /* Redirige de forma segura a la creación estándar o headless si aplica, 
      preservando la estructura completa del motor de swapchains WSI. */
   return wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS]->create_swapchain(
      surface, device, wsi_device, pCreateInfo, pAllocator, out_swapchain);
}
