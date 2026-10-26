#ifndef WRAPPER_BCDEC_H
#define WRAPPER_BCDEC_H

#pragma once

#include "wrapper_private.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) && __GNUC__ >= 4
#define WRAPPER_BCDEC_EXPORT __attribute__((visibility("default")))
#else
#define WRAPPER_BCDEC_EXPORT
#endif

WRAPPER_BCDEC_EXPORT VkFormat
get_format_for_bcn(VkFormat bcn_format);

WRAPPER_BCDEC_EXPORT VkFormat
get_decode_format_for_bcn(VkFormat bcn_format);

WRAPPER_BCDEC_EXPORT int
is_astc_4x4(VkFormat format);

WRAPPER_BCDEC_EXPORT int
is_astc_6x6(VkFormat format);

WRAPPER_BCDEC_EXPORT int
is_astc_8x8(VkFormat format);

WRAPPER_BCDEC_EXPORT int
is_astc(VkFormat format);

WRAPPER_BCDEC_EXPORT int
is_astc_hdr_4x4(VkFormat format);

WRAPPER_BCDEC_EXPORT const char *
bcn_policy_desc(void);

WRAPPER_BCDEC_EXPORT int
bcn_policy_bc6h_hdr(void);

WRAPPER_BCDEC_EXPORT void
bcn_set_astc_hdr(int on);

WRAPPER_BCDEC_EXPORT int
bcn_scan_shader(const uint32_t *code, size_t size);

WRAPPER_BCDEC_EXPORT size_t
bcn_upload_size(VkFormat bcn_format, int w, int h);

WRAPPER_BCDEC_EXPORT int
get_texel_size_for_format(VkFormat format);

WRAPPER_BCDEC_EXPORT int
is_emulated_bcn(struct wrapper_physical_device *pdev, VkFormat format);

WRAPPER_BCDEC_EXPORT int
bcn_max_dim(void);

WRAPPER_BCDEC_EXPORT uint32_t
bcn_cap_mip_drop(const VkImageCreateInfo *ci);

WRAPPER_BCDEC_EXPORT bool
bcn_cap_range(uint32_t mip_drop, VkImageSubresourceRange *range);

WRAPPER_BCDEC_EXPORT uint32_t
bcn_cap_copy_regions(uint32_t mip_drop, const VkBufferImageCopy *regions,
                     uint32_t count, VkBufferImageCopy *kept);

WRAPPER_BCDEC_EXPORT int
bcn_cache_enabled(void);

WRAPPER_BCDEC_EXPORT int
bcn_upload_enabled(void);

WRAPPER_BCDEC_EXPORT void
bcn_cache_note_source(void *srcBuffer, int w, int h, int src_w,
                      VkFormat format, int offset);

WRAPPER_BCDEC_EXPORT void *
bcn_cache_gpu_lookup(void *srcBuffer, int w, int h, int src_w, VkFormat format,
                     int offset, size_t *size);

WRAPPER_BCDEC_EXPORT void
decompress_bcn_format(void *srcBuffer,
                      void *dstBuffer,
                      int w,
                      int h,
                      int src_w,
                      VkFormat format,
                      int offset);

#ifdef __cplusplus
}
#endif

#endif /* WRAPPER_BCDEC_H */
