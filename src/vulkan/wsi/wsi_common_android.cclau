#include "wsi_common.h"
#include "wsi_common_private.h"
#include "vk_log.h"
#include "../wrapper/wrapper_log.h"
#include "../wrapper/wrapper_private.h"

enum wsi_swapchain_blit_type
wsi_get_android_blit_type(const struct wsi_device *wsi,
                            const struct wsi_base_image_params *params,
                            VkDevice device)
{
   int wrapper_blit = getenv("WRAPPER_BLIT") && atoi(getenv("WRAPPER_BLIT"));
   if (wsi->needs_blit || wrapper_blit || true) {
      WRAPPER_LOG(info, "Forzando WSI_SWAPCHAIN_IMAGE_BLIT para compatibilidad con Mali");
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   }

   return WSI_SWAPCHAIN_NO_BLIT;
}

static VkResult
wsi_create_android_image_mem(const struct wsi_swapchain *chain,
                             const struct wsi_image_info *info,
                             struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   result = wsi->CreateImage(chain->device, &info->create,
                             &chain->alloc, &image->image);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to create Android image, res %d", result);
      return result;
   }

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = wsi_select_device_memory_type(wsi, reqs.memoryTypeBits),
   };

   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to allocate Android image memory, res %d", result);
      wsi->DestroyImage(chain->device, image->image, &chain->alloc);
      return result;
   }

   result = wsi->BindImageMemory(chain->device, image->image, image->memory, 0);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to bind Android image memory, res %d", result);
      wsi->FreeMemory(chain->device, image->memory, &chain->alloc);
      wsi->DestroyImage(chain->device, image->image, &chain->alloc);
      return result;
   }

   image->num_planes = 1;
   image->drm_modifier = 0;

   return VK_SUCCESS;
}

VkResult
wsi_configure_android_image(const struct wsi_swapchain *chain,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const struct wsi_base_image_params *params,
                            struct wsi_image_info *info)
{
   VkResult result;

   result = wsi_configure_image(chain, pCreateInfo, 0, info);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to configure Android image, res %d", result);
      return result;
   }

   if (chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT) {
      wsi_configure_image_blit_image(chain, info);
   }

   info->create_mem = wsi_create_android_image_mem;

   return VK_SUCCESS;
}
