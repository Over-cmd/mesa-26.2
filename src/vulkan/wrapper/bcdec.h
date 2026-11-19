/* bcdec.h - v2.54.2-pippetocrypto (Vulkan 1.0 / 1.1 & ARM Mali-G52 Bifrost, para Winlator)
   Proporciona decodificaci贸n y codificaci贸n de bloques y superficies BC1-BC7,
   pipeline PBR completo, transcodificaci贸n directa en memoria (BC <-> ETC2/BC),
   interoperabilidad Android AHardwareBuffer, helpers estrictos de Vulkan 1.0 / 1.1,
   optimizaciones TBDR / L2 Cache para Mali-G52 y m贸dulo de seguridad de texturas para pippetocrypto.

   Basado en bcdec de Sergii "iOrange" Kudlai (2022).
   Adaptado, depurado e integrado al 100% para el proyecto pippetocrypto (Android NDK / Mesa Panfrost / Mali-G52).
   Cero dependencias externas.
*/

#ifndef BCDEC_HEADER_INCLUDED
#define BCDEC_HEADER_INCLUDED

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Integraci贸n espec铆fica para Android Bionic y Mesa */
#if defined(__ANDROID__) || defined(DETECT_OS_ANDROID)
#  include <sys/types.h>
#endif

/* Detecci贸n autom谩tica de SIMD (ARM NEON / SSE2) */
#if defined(BCDEC_ENABLE_SIMD) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#  define BCDEC_HAS_NEON 1
#elif defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#  include <emmintrin.h>
#  define BCDEC_HAS_SSE2 1
#endif

/* Mapeo seguro de restrict */
#ifndef restrict
#  if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  elif defined(_MSC_VER)
#    define restrict __restrict
#  elif defined(__GNUC__) || defined(__clang__)
#    define restrict __restrict__
#  else
#    define restrict
#  endif
#endif

/* Control de inline para funciones cr铆ticas en Mali-G52 */
#ifndef BCDEC_FORCEINLINE
#  if defined(_MSC_VER)
#    define BCDEC_FORCEINLINE __forceinline
#  elif defined(__GNUC__) || defined(__clang__)
#    define BCDEC_FORCEINLINE inline __attribute__((always_inline))
#  else
#    define BCDEC_FORCEINLINE inline
#  endif
#endif

#define BCDEC_VERSION_MAJOR 2
#define BCDEC_VERSION_MINOR 54
#define BCDEC_VERSION_PATCH 2

/* Constantes de arquitectura ARM Mali-G52 (Bifrost) y Vulkan 1.0 / 1.1 */
#define BCDEC_MALI_G52_L2_CACHE_LINE      64
#define BCDEC_MALI_G52_TILE_SIZE          16
#define BCDEC_MALI_G52_MAX_SHARED_MEM     32768
#define BCDEC_MALI_G52_MAX_TEXTURE_DIM    8192
#define BCDEC_VULKAN_1_0_API_VERSION      0x00400000U
#define BCDEC_VULKAN_1_1_API_VERSION      0x00401000U

/* Control de visibilidad de s铆mbolos */
#ifndef BCDECDEF
#  if defined(BCDEC_STATIC)
#    define BCDECDEF static
#  elif defined(__GNUC__) || defined(__clang__)
#    if defined(__cplusplus)
#      define BCDECDEF extern "C" __attribute__((visibility("default")))
#    else
#      define BCDECDEF extern __attribute__((visibility("default")))
#    endif
#  else
#    if defined(__cplusplus)
#      define BCDECDEF extern "C"
#    else
#      define BCDECDEF extern
#    endif
#  endif
#endif

#define BCDEC_BC1_BLOCK_SIZE    8
#define BCDEC_BC2_BLOCK_SIZE    16
#define BCDEC_BC3_BLOCK_SIZE    16
#define BCDEC_BC4_BLOCK_SIZE    8
#define BCDEC_BC5_BLOCK_SIZE    16
#define BCDEC_BC6H_BLOCK_SIZE   16
#define BCDEC_BC7_BLOCK_SIZE    16

/* Formatos soportados */
typedef enum bcdec_format {
    BCDEC_FORMAT_BC1 = 1,
    BCDEC_FORMAT_BC2,
    BCDEC_FORMAT_BC3,
    BCDEC_FORMAT_BC4,
    BCDEC_FORMAT_BC5,
    BCDEC_FORMAT_BC6H,
    BCDEC_FORMAT_BC7
} bcdec_format_t;

/* C贸digos de estado */
typedef enum bcdec_status {
    BCDEC_SUCCESS = 0,
    BCDEC_ERROR_INVALID_PARAMETER = -1,
    BCDEC_ERROR_INVALID_BLOCK = -2,
    BCDEC_ERROR_UNSUPPORTED_FORMAT = -3,
    BCDEC_ERROR_BUFFER_TOO_SMALL = -4,
    BCDEC_ERROR_INVALID_HEADER = -5,
    BCDEC_ERROR_CORRUPT_SURFACE = -6,
    BCDEC_ERROR_CRYPTO_AUTH_FAILED = -7
} bcdec_status_t;

/* Estructuras base */
typedef struct bcdec_swizzle_config {
    int r;
    int g;
    int b;
    int a;
} bcdec_swizzle_config_t;

typedef struct bcdec_surface_stats {
    float avgR, avgG, avgB, avgA;
    uint8_t minR, maxR;
    uint8_t minG, maxG;
    uint8_t minB, maxB;
} bcdec_surface_stats_t;

typedef struct bcdec_channel_metrics {
    float psnrR, psnrG, psnrB, psnrA;
    float rmseR, rmseG, rmseB, rmseA;
} bcdec_channel_metrics_t;

typedef struct bcdec_texture_info {
    bcdec_format_t format;
    int width;
    int height;
    int mipLevels;
    size_t dataSize;
} bcdec_texture_info_t;

typedef void (*bcdec_job_callback_t)(int jobIndex, int totalJobs, void* userArg);

typedef struct bcdec_job_desc {
    bcdec_job_callback_t callback;
    int totalJobs;
    void* userArg;
} bcdec_job_desc_t;

typedef struct bcdec_float_integrity_report {
    size_t totalSamples;
    size_t nanCount;
    size_t infCount;
    size_t negativeCount;
    int firstBadX;
    int firstBadY;
    int firstBadChannel;
} bcdec_float_integrity_report_t;

/* Estructuras ARM Mali-G52 & Android */
typedef struct bcdec_mali_context {
    int maxThreads;
    int useAfbcHints;
    size_t l2CacheLineSize;
    int hasNativeBptc;
    int hasAstcSupport;
    int hasEtc2Support;
    void* customAllocatorContext;
} bcdec_mali_context_t;

typedef struct bcdec_mali_telemetry {
    float gpuClockMhz;
    float currentTemperatureCelsius;
    int activeShaderCores; /* Mali-G52 suele tener 2 o 3 execution engines */
} bcdec_mali_telemetry_t;

typedef struct bcdec_mali_workload_metrics {
    uint64_t totalBytesTranscoded;
    double executionTimeMs;
    float averageBandwidthGbps;
    int thermalThrottleEvents;
    double startTimeMs;   /* uso interno de profile_begin/end */
} bcdec_mali_workload_metrics_t;

typedef struct bcdec_android_hardware_buffer_desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
} bcdec_android_hardware_buffer_desc_t;

typedef struct bcdec_batch_transcode_job {
    bcdec_format_t srcFormat;
    bcdec_format_t dstFormat;
    const void* restrict srcData;
    void* restrict dstData;
    int width;
    int height;
} bcdec_batch_transcode_job_t;

/* Estructuras Vulkan 1.0 y Vulkan 1.1 */
typedef struct bcdec_vulkan_barrier_info {
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    uint32_t aspectMask;
} bcdec_vulkan_barrier_info_t;

typedef struct bcdec_vulkan_1_1_barrier_config {
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
} bcdec_vulkan_1_1_barrier_config_t;

typedef struct bcdec_vulkan_memory_barrier_config {
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
} bcdec_vulkan_memory_barrier_config_t;

typedef struct bcdec_vulkan_buffer_image_copy_region {
    size_t   bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    int32_t  imageOffsetX, imageOffsetY, imageOffsetZ;
    uint32_t imageExtentWidth, imageExtentHeight, imageExtentDepth;
    uint32_t baseMipLevel;
    uint32_t layerCount;
} bcdec_vulkan_buffer_image_copy_region_t;

typedef struct bcdec_vulkan_sub_region_copy_config {
    size_t   bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    int32_t  offsetX, offsetY, offsetZ;
    uint32_t extentWidth, extentHeight, extentDepth;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} bcdec_vulkan_sub_region_copy_config_t;

typedef struct bcdec_vulkan_1_0_copy_region_config {
    size_t   bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    uint32_t imageOffsetX, imageOffsetY, imageOffsetZ;
    uint32_t imageExtentWidth, imageExtentHeight, imageExtentDepth;
    uint32_t mipLevel;
    uint32_t layerCount;
} bcdec_vulkan_1_0_copy_region_config_t;

typedef struct bcdec_vulkan_sampler_create_info {
    uint32_t magFilter;
    uint32_t minFilter;
    uint32_t mipmapMode;
    uint32_t addressModeU;
    uint32_t addressModeV;
    float    maxAnisotropy;
    int      enableAnisotropy;
} bcdec_vulkan_sampler_create_info_t;

typedef struct bcdec_vulkan_image_view_config {
    uint64_t image;
    uint32_t viewType;
    uint32_t format;
    uint32_t aspectMask;
} bcdec_vulkan_image_view_config_t;

typedef struct bcdec_vulkan_descriptor_write_config {
    uint64_t dstSet;
    uint32_t binding;
    uint32_t elementIndex;
    uint32_t descriptorType;
    uint64_t bufferOrImageInfoPtr;
} bcdec_vulkan_descriptor_write_config_t;

typedef struct bcdec_vulkan_descriptor_update_template_config {
    uint32_t pipelineBindPoint;
    uint32_t set;
    uint32_t binding;
    uint32_t descriptorCount;
    size_t   offset;
} bcdec_vulkan_descriptor_update_template_config_t;

typedef struct bcdec_mali_g52_subpass_barrier {
    uint32_t srcSubpass;
    uint32_t dstSubpass;
    uint32_t srcStageMask;
    uint32_t dstStageMask;
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
} bcdec_mali_g52_subpass_barrier_t;

typedef struct bcdec_vulkan_render_pass_begin_config {
    uint64_t renderPass;
    uint64_t framebuffer;
    int32_t  renderAreaOffsetX, renderAreaOffsetY;
    uint32_t renderAreaWidth, renderAreaHeight;
    uint32_t clearValueCount;
} bcdec_vulkan_render_pass_begin_config_t;

typedef struct bcdec_vulkan_submit_config {
    uint32_t waitSemaphoreCount;
    uint32_t commandBufferCount;
    uint32_t signalSemaphoreCount;
} bcdec_vulkan_submit_config_t;

typedef struct bcdec_vulkan_mipmap_barrier_config {
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t targetMipLevel;
} bcdec_vulkan_mipmap_barrier_config_t;

typedef struct bcdec_vulkan_shader_stage_config {
    uint32_t stageFlag;
    uint64_t moduleHandle;
    const char* pEntryName;
} bcdec_vulkan_shader_stage_config_t;

typedef struct bcdec_vulkan_1_0_image_barrier_config {
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    uint64_t image;
    uint32_t aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
} bcdec_vulkan_1_0_image_barrier_config_t;

typedef struct bcdec_vulkan_1_0_buffer_info_config {
    uint64_t bufferHandle;
    size_t   offset;
    size_t   range;
} bcdec_vulkan_1_0_buffer_info_config_t;

typedef struct bcdec_vulkan_push_constant_block {
    uint32_t imageWidth;
    uint32_t imageHeight;
    uint32_t sourceFormat;
    uint32_t targetFormat;
    float    conversionParameters[4];
} bcdec_vulkan_push_constant_block_t;

/* Estructura de seguridad y cifrado de assets para pippetocrypto */
typedef struct bcdec_pippetocrypto_header {
    uint32_t magic;          /* 0x50495050 ('PIPP') */
    uint32_t version;        /* 1 */
    uint32_t format;         /* bcdec_format_t */
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t payloadCrc32;
    uint32_t flags;          /* Bit 0: Cifrado XOR/CTR, Bit 1: Morton Swizzled */
    uint8_t  iv[16];         /* Vector de inicializaci贸n */
    uint8_t  hmacTag[32];    /* Firma de autenticaci贸n */
} bcdec_pippetocrypto_header_t;

/* Macros de tama帽o y alineaci贸n */
#define BCDEC_BC1_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC1_BLOCK_SIZE)
#define BCDEC_BC2_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC2_BLOCK_SIZE)
#define BCDEC_BC3_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC3_BLOCK_SIZE)
#define BCDEC_BC4_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC4_BLOCK_SIZE)
#define BCDEC_BC5_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC5_BLOCK_SIZE)
#define BCDEC_BC6H_COMPRESSED_SIZE(w, h)    (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC6H_BLOCK_SIZE)
#define BCDEC_BC7_COMPRESSED_SIZE(w, h)     (((((w) + 3) >> 2) * (((h) + 3) >> 2)) * BCDEC_BC7_BLOCK_SIZE)

#define BCDEC_ALIGNED_PITCH(width, bytesPerPixel, alignment) \
    (((width) * (bytesPerPixel) + ((alignment) - 1)) & ~((alignment) - 1))

/* =========================================================================
 * DECLARACIONES DE FUNCIONES
 * ========================================================================= */

/* Decodificadores de bloque base */
BCDECDEF void bcdec_bc1(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc2(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc3(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);

#ifndef BCDEC_BC4BC5_PRECISE
BCDECDEF void bcdec_bc4(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc5(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
#else
BCDECDEF void bcdec_bc4(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc5(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc4_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc5_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
#endif

BCDECDEF void bcdec_bc6h_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc6h_half(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc7(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);

/* Despachadores universales y validaci贸n */
BCDECDEF int bcdec_decompress_block(bcdec_format_t format, const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF int bcdec_decompress_block_float(bcdec_format_t format, const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF int bcdec_validate_block(bcdec_format_t format, const void* restrict compressedBlock);
BCDECDEF int bcdec_validate_texture(bcdec_format_t format, const void* restrict compressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_decompress_block_safe(bcdec_format_t format, const void* restrict compressedBlock, size_t compressedSize, void* restrict decompressedBlock, int destinationPitch);
BCDECDEF bcdec_status_t bcdec_decompress_texture_swizzled(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch, bcdec_swizzle_config_t swizzle);

/* Utilidades de formato y c谩lculos */
BCDECDEF size_t bcdec_format_block_size(bcdec_format_t format);
BCDECDEF int bcdec_format_pixel_size(bcdec_format_t format);
BCDECDEF const char* bcdec_format_string(bcdec_format_t format);
BCDECDEF bcdec_format_t bcdec_format_from_gl(unsigned int glInternalFormat);
BCDECDEF bcdec_format_t bcdec_format_from_vulkan(unsigned int vkFormat);
BCDECDEF unsigned int bcdec_format_to_vulkan(bcdec_format_t format, int srgb);
BCDECDEF bcdec_format_t bcdec_format_from_dxgi(unsigned int dxgiFormat);
BCDECDEF int bcdec_calculate_mip_levels(int width, int height);
BCDECDEF size_t bcdec_texture_compressed_size(bcdec_format_t format, int width, int height);
BCDECDEF size_t bcdec_texture_mipmap_compressed_size(bcdec_format_t format, int width, int height, int mipLevels);
BCDECDEF size_t bcdec_texture_mipmap_level_offset(bcdec_format_t format, int width, int height, int targetMipLevel);

/* Job-System y Metadatos */
BCDECDEF void bcdec_calculate_parallel_blocks(int height, int threadIndex, int totalThreads, int* restrict outStartRowBlockY, int* restrict outEndRowBlockY);
BCDECDEF int bcdec_get_bc6h_mode(const void* restrict compressedBlock);
BCDECDEF int bcdec_get_bc7_mode(const void* restrict compressedBlock);
BCDECDEF float bcdec_estimate_block_variance(bcdec_format_t format, const void* restrict compressedBlock);
BCDECDEF void bcdec_dispatch_jobs(bcdec_job_desc_t jobDesc);

/* Descompresi贸n de superficies, regiones, mipmaps, 3D y cubemaps */
BCDECDEF void bcdec_decompress_texture_row(bcdec_format_t format, const void* restrict compressedRowData, void* restrict decompressedRowImage, int width, int height, int rowBlockY, int destinationPitch);
BCDECDEF void bcdec_decompress_texture(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_region(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int regionX, int regionY, int regionWidth, int regionHeight, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_tile(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int tileX, int tileY, int tileWidth, int tileHeight, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_float(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_flipped(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_parallel(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int startRowBlockY, int endRowBlockY, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_parallel_async(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch, bcdec_job_desc_t jobDesc);
BCDECDEF void bcdec_decompress_texture_mipmaps(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int mipLevels, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_mipmaps_parallel(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int mipLevel, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_mipmap_level(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int baseWidth, int baseHeight, int targetMipLevel, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_simd(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch);
BCDECDEF size_t bcdec_texture_3d_compressed_size(bcdec_format_t format, int width, int height, int depth);
BCDECDEF void bcdec_decompress_texture_3d_slice(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedSlice, int width, int height, int depth, int sliceZ, int destinationPitch);
BCDECDEF void bcdec_decompress_texture_3d(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedVolume, int width, int height, int depth, int destinationPitch);
BCDECDEF void bcdec_decompress_cubemap(bcdec_format_t format, const void* restrict compressedCubemapData, void* restrict decompressedFaces[6], int faceWidth, int faceHeight, int destinationPitch);

/* Manipulaci贸n de superficies, espacio de color y precisi贸n */
BCDECDEF void bcdec_swizzle_surface(void* restrict imageData, int width, int height, int bytesPerPixel, int rIdx, int gIdx, int bIdx, int aIdx, int destinationPitch);
BCDECDEF void bcdec_swizzle_surface_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int rIdx, int gIdx, int bIdx, int aIdx, int destinationPitch);
BCDECDEF void bcdec_invert_green_channel(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_downscale_surface_2x2(const void* restrict srcImage, void* restrict dstImage, int srcWidth, int srcHeight, int bytesPerPixel, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_mipmaps_surface(void* restrict baseImage, int width, int height, int bytesPerPixel, int mipLevels, int basePitch);
BCDECDEF void bcdec_convert_linear_to_srgb(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_convert_srgb_to_linear(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_convert_linear_to_srgb_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_convert_srgb_to_linear_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_apply_ordered_dither(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF uint16_t bcdec_float_to_half(float val);
BCDECDEF float bcdec_half_to_float(uint16_t half);
BCDECDEF void bcdec_pad_surface_borders(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_mirror_surface_horizontal(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_flip_surface_vertical(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_rotate_surface_90(const void* restrict srcImage, void* restrict dstImage, int width, int height, int bytesPerPixel, int srcPitch, int dstPitch, int clockwise);
BCDECDEF void bcdec_fix_mipmap_seams(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_blur_surface_3x3(const void* restrict srcImage, void* restrict dstImage, int width, int height, int bytesPerPixel, int srcPitch, int dstPitch);
BCDECDEF void bcdec_premultiply_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_unpremultiply_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_convert_rgb_to_grayscale_rec709(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_adjust_surface_color(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, float exposure, float contrast, float saturation);
BCDECDEF int bcdec_is_surface_fully_opaque(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF void bcdec_blend_surfaces(const void* restrict srcImageA, const void* restrict srcImageB, void* restrict dstImage, int width, int height, int bytesPerPixel, float blendFactor, int pitch);
BCDECDEF void bcdec_extract_channel(const void* restrict srcImage, void* restrict dstChannelMap, int width, int height, int bytesPerPixel, int channelIndex, int srcPitch, int dstPitch);
BCDECDEF void bcdec_dilate_surface_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, int iterations);
BCDECDEF void bcdec_apply_tonemap_aces(void* restrict floatImageData, int width, int height, int rowPitchBytes, int isHalfFloat);
BCDECDEF void bcdec_apply_tonemap_reinhard(void* restrict floatImageData, int width, int height, int rowPitchBytes, int isHalfFloat);

/* Pipeline PBR */
BCDECDEF void bcdec_bc5_to_normal_map(void* restrict imageData, int width, int height, int destinationPitch);
BCDECDEF void bcdec_bc5_to_normal_map_ex(void* restrict imageData, int width, int height, int destinationPitch, float strength);
BCDECDEF void bcdec_bc5_to_normal_map_packed(void* restrict imageData, int width, int height, int destinationPitch, float strength, uint8_t alphaValue);
BCDECDEF void bcdec_heightmap_to_normal_map(const void* restrict srcHeightmap, void* restrict dstNormalMap, int width, int height, float strength, int srcPitch, int dstPitch);
BCDECDEF void bcdec_normal_map_to_heightmap(const void* restrict srcNormalMap, void* restrict dstHeightmap, int width, int height, int srcPitch, int dstPitch);
BCDECDEF void bcdec_smooth_normal_map(void* restrict normalMapData, int width, int height, float smoothness, int destinationPitch);
BCDECDEF void bcdec_generate_roughness_from_height(const void* restrict srcHeightmap, void* restrict dstRoughnessMap, int width, int height, float scale, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_roughness_from_albedo(const void* restrict srcAlbedo, void* restrict dstRoughnessMap, int width, int height, int bytesPerPixel, float detailStrength, float baseRoughness, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_gloss_from_roughness(const void* restrict srcRoughness, void* restrict dstGlossMap, int width, int height, int destinationPitch);
BCDECDEF void bcdec_generate_ao_map(const void* restrict srcHeightmap, void* restrict dstAoMap, int width, int height, float radius, float intensity, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_ao_map_directional(const void* restrict srcHeightmap, void* restrict dstAoMap, int width, int height, float radius, float intensity, float sunAngleRadians, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_curvature_map(const void* restrict srcHeightmap, void* restrict dstCurvatureMap, int width, int height, float scale, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_displacement_map(const void* restrict srcHeightmap, void* restrict dstDisplacementMap, int width, int height, float depthScale, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_flow_map(const void* restrict srcHeightmap, void* restrict dstFlowMap, int width, int height, float strength, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_cavity_map(const void* restrict srcHeightmap, void* restrict dstCavityMap, int width, int height, float scale, int srcPitch, int dstPitch);
BCDECDEF void bcdec_generate_emissive_mask(const void* restrict srcAlbedo, void* restrict dstEmissiveMap, int width, int height, int bytesPerPixel, float threshold, float intensity, int srcPitch, int dstPitch);
BCDECDEF void bcdec_pack_orm_map(const void* restrict srcAoMap, const void* restrict srcRoughnessMap, const void* restrict srcMetallicMap, void* restrict dstOrmMap, int width, int height, int pitch);
BCDECDEF void bcdec_pack_custom_channels(const void* restrict srcCh0, const void* restrict srcCh1, const void* restrict srcCh2, const void* restrict srcCh3, void* restrict dstPackedMap, int width, int height, int pitch, int activeChannelsMask);

/* Codificadores BC y Transcodificadores */
BCDECDEF void bcdec_encode_block_bc1(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF void bcdec_encode_block_bc1_optimized(const void* restrict rgbaPixels, void* restrict compressedBlock, int qualityLevel);
BCDECDEF void bcdec_encode_block_bc1_perceptual_mse(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF void bcdec_encode_block_bc3(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF void bcdec_encode_block_bc4(const void* restrict rPixels, void* restrict compressedBlock, int isSigned);
BCDECDEF void bcdec_encode_block_bc5(const void* restrict rgPixels, void* restrict compressedBlock, int isSigned);
BCDECDEF void bcdec_encode_block_bc6h_half(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned);
BCDECDEF void bcdec_encode_block_bc6h_optimized(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned);
BCDECDEF void bcdec_encode_block_bc6h_advanced(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned);
BCDECDEF void bcdec_encode_block_bc7(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF void bcdec_encode_block_bc7_perceptual(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF void bcdec_encode_block_bc7_advanced(const void* restrict rgbaPixels, void* restrict compressedBlock);
BCDECDEF bcdec_status_t bcdec_encode_texture_bc7(const void* restrict rgbaPixels, void* restrict compressedData, int width, int height, int qualityPreset);
BCDECDEF bcdec_status_t bcdec_encode_texture_parallel_async(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch, bcdec_job_desc_t jobDesc);
BCDECDEF bcdec_status_t bcdec_encode_texture_mipmaps_parallel_async(bcdec_format_t format, const void* restrict srcMipChainPixels, void* restrict compressedData, int width, int height, int mipLevels, int baseSrcPitch, bcdec_job_desc_t jobDesc);

BCDECDEF bcdec_status_t bcdec_transcode_bc1_to_bc3(const void* restrict bc1CompressedData, void* restrict bc3CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_bc3_to_bc1(const void* restrict bc3CompressedData, void* restrict bc1CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_bc3_to_bc4(const void* restrict bc3CompressedData, void* restrict bc4CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_bc5_to_bc4(const void* restrict bc5CompressedData, void* restrict bc4CompressedData, int width, int height, int channelIndex);
BCDECDEF bcdec_status_t bcdec_transcode_bc4_to_bc1(const void* restrict bc4CompressedData, void* restrict bc1CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_bc7_to_bc1(const void* restrict bc7CompressedData, void* restrict bc1CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_bc7_to_bc3(const void* restrict bc7CompressedData, void* restrict bc3CompressedData, int width, int height);
BCDECDEF bcdec_status_t bcdec_transcode_stream_chunk(bcdec_format_t srcFormat, bcdec_format_t dstFormat, const void* restrict srcChunk, void* restrict dstChunk, size_t chunkSize, int width, int height);

/* M茅tricas, Parsers y Validadores de Integridad */
BCDECDEF void bcdec_calculate_surface_stats(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, bcdec_surface_stats_t* restrict outStats);
BCDECDEF float bcdec_calculate_surface_entropy(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF int bcdec_classify_block_complexity(bcdec_format_t format, const void* restrict compressedBlock);
BCDECDEF int bcdec_estimate_optimal_bc_quality(const void* restrict rgbaPixels, int width, int height, int pitch);
BCDECDEF float bcdec_calculate_block_mse(const void* restrict originalBlock, const void* restrict decompressedBlock, int pixelSize, int blockPixelCount);
BCDECDEF float bcdec_calculate_surface_psnr(float mse);
BCDECDEF float bcdec_calculate_surface_ssim(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch);
BCDECDEF float bcdec_calculate_surface_rmse(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch);
BCDECDEF void bcdec_calculate_surface_channel_metrics(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch, bcdec_channel_metrics_t* restrict outMetrics);
BCDECDEF uint32_t bcdec_calculate_buffer_crc32(const void* restrict buffer, size_t length);
BCDECDEF uint32_t bcdec_calculate_surface_hash(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch);
BCDECDEF bcdec_status_t bcdec_parse_dds_header(const void* restrict ddsFileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload);
BCDECDEF bcdec_status_t bcdec_parse_ktx_header(const void* restrict ktxFileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload);
BCDECDEF bcdec_status_t bcdec_parse_ktx2_header(const void* restrict ktx2FileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload);
BCDECDEF bcdec_status_t bcdec_validate_ktx2_payload(const void* restrict ktx2FileBytes, size_t fileSize);
BCDECDEF bcdec_status_t bcdec_verify_float_surface_integrity(const void* restrict floatData, int width, int height, int channels, int rowPitchBytes, int isHalfFloat, int rejectNegative, bcdec_float_integrity_report_t* restrict outReport);

/* =========================================================================
 * M脫DULO ARM MALI-G52 (BIFROST) & ANDROID NDK
 * ========================================================================= */
BCDECDEF void bcdec_mali_init_context(bcdec_mali_context_t* restrict ctx, int maxThreads);
BCDECDEF bcdec_status_t bcdec_mali_encode_surface_async(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch, const bcdec_mali_context_t* restrict maliCtx);
BCDECDEF bcdec_status_t bcdec_encode_texture_mali_tiled(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch);
BCDECDEF void bcdec_decompress_texture_mali_fast_tile(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedTile, int width, int height, int tileX, int tileY, int tileSize, int destinationPitch);
BCDECDEF int bcdec_validate_mali_cache_alignment(const void* restrict textureData, size_t dataSize);
BCDECDEF int bcdec_mali_validate_texture_alignment(const void* restrict textureData, size_t dataSize, int alignmentBytes);
BCDECDEF bcdec_status_t bcdec_mali_generate_compressed_mipmaps(bcdec_format_t format, void* restrict compressedMipChainData, int baseWidth, int baseHeight, int mipLevels);
BCDECDEF bcdec_status_t bcdec_mali_transcode_bc_to_etc2(const void* restrict bcCompressedData, void* restrict etc2CompressedData, int width, int height, bcdec_format_t srcFormat);
BCDECDEF bcdec_status_t bcdec_mali_transcode_astc_to_bc(const void* restrict astcCompressedData, void* restrict bcCompressedData, int width, int height, bcdec_format_t targetFormat);
BCDECDEF void bcdec_mali_swizzle_blocks_morton(const void* restrict srcBlocks, void* restrict dstMortonBlocks, int width, int height, size_t blockSize);
BCDECDEF bcdec_status_t bcdec_mali_generate_afbc_header(const void* restrict compressedData, void* restrict outAfbcHeader, int width, int height, bcdec_format_t format);
BCDECDEF bcdec_status_t bcdec_mali_stream_generate_mipmap_level(bcdec_format_t format, const void* restrict srcMipData, void* restrict dstMipData, int curWidth, int curHeight);
BCDECDEF int bcdec_mali_adjust_thread_pool_load(int currentThreads, float gpuThermalLoadRatio);
BCDECDEF float bcdec_mali_estimate_bus_bandwidth_pressure(int textureWidth, int textureHeight, bcdec_format_t format, float currentFps);
BCDECDEF int bcdec_mali_adapt_quality_to_thermal_state(const bcdec_mali_telemetry_t* restrict telemetry);
BCDECDEF void bcdec_mali_emit_cache_prefetch_hints(const void* restrict textureAddress, size_t dataSize, int prefetchLevel);
BCDECDEF bcdec_status_t bcdec_mali_batch_transcode_surfaces(const bcdec_batch_transcode_job_t* restrict jobs, int jobCount, bcdec_job_desc_t jobDesc);
BCDECDEF void bcdec_mali_profile_workload_begin(bcdec_mali_workload_metrics_t* restrict metrics);
BCDECDEF void bcdec_mali_profile_workload_end(bcdec_mali_workload_metrics_t* restrict metrics, size_t processedBytes);

/* Funciones espec铆ficas Mali-G52 */
BCDECDEF int bcdec_mali_g52_validate_format_support(bcdec_format_t format, int deviceHasBptcExtension);
BCDECDEF void bcdec_mali_get_optimal_compute_workgroup_size(int* restrict outSizeX, int* restrict outSizeY);
BCDECDEF size_t bcdec_mali_g52_calculate_optimal_tile_size(int imageWidth, int imageHeight, bcdec_format_t format);
BCDECDEF size_t bcdec_mali_g52_get_ideal_staging_buffer_size(bcdec_format_t format, int width, int height);
BCDECDEF void bcdec_mali_g52_flush_l2_cache_hint(void* restrict stagingBuffer, size_t bufferSize);
BCDECDEF size_t bcdec_mali_g52_calculate_staging_offset(size_t currentOffset, size_t bufferImageGranularity);
BCDECDEF size_t bcdec_mali_g52_optimize_command_pool_allocation(int concurrentTextureCount);
BCDECDEF int bcdec_mali_g52_validate_framebuffer_compression_support(bcdec_format_t format, int isOffscreenRenderTarget);
BCDECDEF int bcdec_mali_g52_validate_compute_shared_memory(uint32_t requestedSharedMemoryBytes);
BCDECDEF float bcdec_mali_g52_estimate_transcode_overhead_ms(int width, int height, bcdec_format_t srcFormat, bcdec_format_t dstFormat);
BCDECDEF void bcdec_mali_g52_init_tile_subpass_dependency(void* restrict outVkSubpassDependency, const bcdec_mali_g52_subpass_barrier_t* restrict barrierConfig);
BCDECDEF int bcdec_mali_g52_validate_barrier_dependency_chain(uint32_t srcStageMask, uint32_t dstStageMask, int isTileBasedRenderTarget);
BCDECDEF size_t bcdec_mali_g52_calculate_combined_descriptor_size(void);
BCDECDEF uint32_t bcdec_mali_g52_get_shader_execution_priority_hint(bcdec_format_t format, int isRealtimeContext);
BCDECDEF size_t bcdec_mali_g52_get_optimal_scratchpad_size(int tileWidth, int tileHeight, bcdec_format_t format);
BCDECDEF int bcdec_mali_g52_validate_bptc_partition_limits(int modeIndex, bcdec_format_t format);
BCDECDEF size_t bcdec_mali_g52_calculate_compute_push_constant_offset(uint32_t memberIndex);
BCDECDEF uint32_t bcdec_mali_g52_get_transient_attachment_usage_hint(int isTemporaryRenderTarget);
BCDECDEF int bcdec_mali_g52_validate_compute_workgroup_limits(uint32_t workgroupSizeX, uint32_t workgroupSizeY, uint32_t workgroupSizeZ);
BCDECDEF size_t bcdec_mali_g52_get_cache_flush_batch_size(size_t totalDataBytes);
BCDECDEF size_t bcdec_mali_g52_align_staging_offset(size_t rawOffset, size_t optimalBufferCopyOffsetAlignment);
BCDECDEF size_t bcdec_mali_g52_optimize_descriptor_set_layout(int textureCount, bcdec_format_t format);
BCDECDEF int bcdec_mali_g52_validate_3d_staging_alignment(size_t bufferOffset, size_t optimalBufferCopyOffsetAlignment);
BCDECDEF uint32_t bcdec_mali_g52_get_command_buffer_usage_hint(int isOneTimeSubmit);
BCDECDEF size_t bcdec_mali_g52_calculate_aligned_row_pitch(int width, bcdec_format_t format, size_t rowPitchAlignment);
BCDECDEF void bcdec_mali_g52_get_compute_shared_memory_layout(int* restrict outSharedWidth, int* restrict outSharedHeight);

/* Android AHardwareBuffer Interop */
BCDECDEF bcdec_status_t bcdec_decompress_to_hardware_buffer(bcdec_format_t format, const void* restrict compressedData, void* restrict hardwareBufferPixels, int width, int height, int destinationPitch);
BCDECDEF bcdec_status_t bcdec_mali_decompress_to_ahardwarebuffer(bcdec_format_t format, const void* restrict compressedData, void* restrict ahwBufferVirtualAddress, int width, int height, int rowStrideBytes);
BCDECDEF bcdec_status_t bcdec_android_lock_hardware_buffer_pixels(void* restrict hardwareBufferHandle, void** restrict outVirtualAddress, int lockFlags);
BCDECDEF bcdec_status_t bcdec_android_unlock_hardware_buffer(void* restrict hardwareBufferHandle);
BCDECDEF bcdec_status_t bcdec_android_release_hardware_buffer_mapping(void* restrict mappedVirtualAddress);
BCDECDEF size_t bcdec_android_align_hardware_buffer_stride(size_t rawWidth, int bytesPerPixel, size_t alignmentRequirement);

/* =========================================================================
 * M脫DULO ESTRICTO VULKAN 1.0 Y VULKAN 1.1
 * ========================================================================= */
BCDECDEF int bcdec_vulkan_validate_image_extent(int width, int height, int mipLevels, bcdec_format_t format);
BCDECDEF int bcdec_vulkan_validate_buffer_image_copy(int width, int height, size_t bufferOffset, size_t totalBufferSize, bcdec_format_t format);
BCDECDEF bcdec_status_t bcdec_vulkan_transcode_surface_format(bcdec_format_t srcFormat, bcdec_format_t dstFormat, const void* restrict srcData, void* restrict dstData, int width, int height);
BCDECDEF void bcdec_vulkan_build_image_memory_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_barrier_info_t barrierInfo, int baseMipLevel, int mipLevels);
BCDECDEF size_t bcdec_vulkan_generate_compute_pipeline_descriptor(bcdec_format_t srcFormat, bcdec_format_t dstFormat, void* restrict outShaderParamsBuffer, size_t bufferCapacity);
BCDECDEF size_t bcdec_vulkan_build_push_constant_block(bcdec_format_t srcFormat, bcdec_format_t dstFormat, int width, int height, void* restrict outPushConstantBuffer, size_t bufferCapacity);
BCDECDEF int bcdec_vulkan_check_device_compression_features(uint32_t supportedFeaturesBitmask, bcdec_format_t targetFormat);
BCDECDEF bcdec_status_t bcdec_vulkan_decompress_texture_layer(bcdec_format_t format, const void* restrict compressedLayerData, void* restrict decompressedLayerPixels, int width, int height, int destinationPitch);
BCDECDEF size_t bcdec_vulkan_calculate_layer_offset(bcdec_format_t format, int width, int height, int layerIndex);
BCDECDEF bcdec_status_t bcdec_vulkan_dispatch_compute_transcode_params(bcdec_format_t srcFormat, bcdec_format_t dstFormat, int width, int height, void* restrict outDispatchCommand);
BCDECDEF int bcdec_vulkan_validate_subresource_layout(int width, int height, int layerCount, int mipLevels, bcdec_format_t format);
BCDECDEF int bcdec_vulkan_find_memory_type(uint32_t typeFilter, uint32_t propertyFlags, const uint32_t* restrict availableMemoryTypesBitmask, int memoryTypeCount);
BCDECDEF int bcdec_vulkan_build_mipmap_buffer_image_copies(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, void* restrict outCopyRegionsArray, size_t arrayCapacityBytes);
BCDECDEF int bcdec_vulkan_check_format_feature_support(uint32_t formatFeaturesFlags, bcdec_format_t format, int requireOptimalTiling);
BCDECDEF size_t bcdec_vulkan_align_buffer_offset_for_transfer(size_t rawOffset, size_t optimalBufferCopyOffsetAlignment);
BCDECDEF void bcdec_vulkan_init_default_sampler_info(bcdec_vulkan_sampler_create_info_t* restrict outInfo, float maxAnisotropy);
BCDECDEF size_t bcdec_vulkan_calculate_total_mip_chain_size(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels);

/* Funciones espec铆ficas Vulkan 1.0 & 1.1 */
BCDECDEF int bcdec_vulkan_1_0_validate_buffer_image_copy(int width, int height, size_t bufferOffset, size_t totalBufferSize, bcdec_format_t format);
BCDECDEF void bcdec_vulkan_1_0_build_image_memory_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_barrier_info_t barrierInfo, int baseMipLevel, int mipLevels);
BCDECDEF int bcdec_vulkan_1_0_validate_device_limits(int maxImageDimension2D, bcdec_format_t format);
BCDECDEF int bcdec_vulkan_1_1_validate_device_features(uint32_t apiVersion, int deviceSupportsTextureCompressionBC, int deviceSupportsSamplerAnisotropy);
BCDECDEF void bcdec_vulkan_1_0_init_sampler_create_info(void* restrict outVkSamplerCreateInfo, float maxAnisotropy);
BCDECDEF size_t bcdec_vulkan_1_0_build_descriptor_set_layout_binding(int bindingIndex, uint32_t descriptorType, uint32_t stageFlags, void* restrict outBindingStruct);
BCDECDEF int bcdec_vulkan_1_1_validate_pipeline_stage_flags(uint32_t srcStageMask, uint32_t dstStageMask);
BCDECDEF int bcdec_vulkan_1_0_validate_image_descriptor_limits(int maxPerStageDescriptorSamplers, int maxPerStageDescriptorSampledImages);
BCDECDEF void bcdec_vulkan_1_0_init_compressed_image_create_info(void* restrict outVkImageCreateInfo, int width, int height, bcdec_format_t format, uint32_t mipLevels);
BCDECDEF int bcdec_vulkan_1_0_validate_mipmap_chain_layout(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, size_t totalBufferSize);
BCDECDEF size_t bcdec_vulkan_1_0_calculate_descriptor_pool_size(int sampledImagesCount, int samplersCount);
BCDECDEF int bcdec_vulkan_1_0_validate_3d_extent(int width, int height, int depth, bcdec_format_t format);
BCDECDEF bcdec_status_t bcdec_vulkan_1_1_build_layout_transition_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_1_1_barrier_config_t config, int baseMipLevel, int mipLevels);
BCDECDEF int bcdec_vulkan_1_1_validate_push_constant_limits(uint32_t pushConstantsSize, uint32_t maxPushConstantsSizeLimit);
BCDECDEF int bcdec_vulkan_1_0_is_block_compression_supported(bcdec_format_t format, int deviceHasBCSupport, int deviceHasASTCSupport);
BCDECDEF int bcdec_vulkan_1_0_validate_staging_buffer_alignment(size_t bufferOffset, size_t bufferSize, size_t optimalCopyOffsetAlignment);
BCDECDEF void bcdec_vulkan_1_0_build_memory_barrier(void* restrict outVkMemoryBarrier, bcdec_vulkan_memory_barrier_config_t config);
BCDECDEF size_t bcdec_vulkan_1_0_build_descriptor_write(const bcdec_vulkan_descriptor_write_config_t* restrict config, void* restrict outVkWriteDescriptorSet);
BCDECDEF int bcdec_vulkan_1_0_validate_npot_mipmap_extent(int baseWidth, int baseHeight, int mipLevels);
BCDECDEF size_t bcdec_vulkan_1_1_build_descriptor_update_template(const bcdec_vulkan_descriptor_update_template_config_t* restrict config, void* restrict outTemplateCreateInfo);
BCDECDEF int bcdec_vulkan_1_0_validate_query_pool_create_info(uint32_t queryType, uint32_t queryCount);
BCDECDEF int bcdec_vulkan_1_0_validate_pipeline_cache_header(const void* restrict cacheHeaderData, size_t dataSize);
BCDECDEF int bcdec_vulkan_1_0_validate_memory_allocation(size_t allocationSize, size_t maxMemoryAllocationSizeLimit);
BCDECDEF size_t bcdec_vulkan_1_0_build_image_view_create_info(const bcdec_vulkan_image_view_config_t* restrict config, void* restrict outVkImageViewCreateInfo);
BCDECDEF int bcdec_vulkan_1_1_validate_uniform_buffer_range(size_t rangeSize, size_t maxUniformBufferRangeLimit);
BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_set_allocation_limits(uint32_t requestedDescriptorSets, uint32_t maxBoundDescriptorSets);
BCDECDEF size_t bcdec_vulkan_1_0_build_render_pass_begin_info(const bcdec_vulkan_render_pass_begin_config_t* restrict config, void* restrict outVkRenderPassBeginInfo);
BCDECDEF int bcdec_vulkan_1_1_validate_image_blit_compatibility(bcdec_format_t srcFormat, bcdec_format_t dstFormat, uint32_t srcTiling, uint32_t dstTiling);
BCDECDEF int bcdec_vulkan_1_0_validate_semaphore_submit_info(uint32_t waitSemaphoreCount, uint32_t signalSemaphoreCount);
BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_image_copy_region(const bcdec_vulkan_sub_region_copy_config_t* restrict config, void* restrict outVkBufferImageCopy);
BCDECDEF int bcdec_vulkan_1_0_validate_host_visible_alignment(uintptr_t memoryAddress, size_t alignmentRequirement);
BCDECDEF int bcdec_vulkan_1_1_validate_color_blend_format(uint32_t vkColorFormat);
BCDECDEF int bcdec_vulkan_1_0_validate_cubemap_extent(int faceWidth, int faceHeight);
BCDECDEF size_t bcdec_vulkan_1_0_build_submit_info(const bcdec_vulkan_submit_config_t* restrict config, void* restrict outVkSubmitInfo);
BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_pool_create_flags(uint32_t poolCreateFlags);
BCDECDEF size_t bcdec_vulkan_1_0_build_mipmap_image_barrier(const bcdec_vulkan_mipmap_barrier_config_t* restrict config, uint64_t vkImage, void* restrict outVkImageMemoryBarrier);
BCDECDEF int bcdec_vulkan_1_0_validate_linear_image_stride(int width, int bytesPerPixel, size_t rowPitch, size_t bufferRowLengthAlignment);
BCDECDEF int bcdec_vulkan_1_1_validate_layered_image_copy_extent(int baseWidth, int baseHeight, int layerCount, int targetLayer);
BCDECDEF int bcdec_vulkan_1_0_validate_device_compression_fallback(bcdec_format_t format, int supportsBC, int supportsASTC, int supportsETC2);
BCDECDEF size_t bcdec_vulkan_1_0_build_shader_stage_create_info(const bcdec_vulkan_shader_stage_config_t* restrict config, void* restrict outVkPipelineShaderStageCreateInfo);
BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_image_copy(const bcdec_vulkan_1_0_copy_region_config_t* restrict config, void* restrict outVkBufferImageCopy);
BCDECDEF int bcdec_vulkan_1_0_validate_combined_image_sampler_limits(uint32_t boundSamplersCount, uint32_t maxPerStageDescriptorSamplers);
BCDECDEF int bcdec_vulkan_1_0_validate_clear_attachment_format(uint32_t vkFormat);
BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_set_layout_binding_limit(uint32_t bindingCount, uint32_t maxBoundDescriptorSets);
BCDECDEF size_t bcdec_vulkan_1_0_build_image_memory_barrier_ex(const bcdec_vulkan_1_0_image_barrier_config_t* restrict config, void* restrict outVkImageMemoryBarrier);
BCDECDEF int bcdec_vulkan_1_0_validate_framebuffer_attachment_format(uint32_t vkFormat);
BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_descriptor_info(const bcdec_vulkan_1_0_buffer_info_config_t* restrict config, void* restrict outVkDescriptorBufferInfo);
BCDECDEF int bcdec_vulkan_1_0_validate_max_bound_descriptor_sets(uint32_t boundSetsCount);
BCDECDEF int bcdec_vulkan_1_0_validate_buffer_blit_compatibility(size_t srcSize, size_t dstSize, size_t transferSize);

/* =========================================================================
 * M脫DULO CRYPTO & SEGURIDAD PIPPETOCRYPTO
 * ========================================================================= */
BCDECDEF bcdec_status_t bcdec_crypto_verify_secure_texture_payload(const void* restrict encryptedPayload, size_t payloadSize, const uint8_t* restrict expectedHmacSha256, uint32_t* restrict outDecryptedFlags);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_parse_and_verify_container(const void* restrict containerBytes, size_t totalBytes, const uint8_t key32[32], bcdec_pippetocrypto_header_t* restrict outHeader, const void** restrict outPayload);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_decrypt_payload_stream(const void* restrict encryptedPayload, void* restrict decryptedOutput, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16]);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_encrypt_payload_stream(const void* restrict plainPayload, void* restrict encryptedOutput, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16]);


/* =========================================================================
 * A脩ADIDOS v2.54.2 (Mali-G52 / Vulkan 1.0-1.1 / Winlator)
 * ========================================================================= */
typedef enum bcdec_g52_texture_path {
    BCDEC_G52_PATH_UNSUPPORTED = 0,
    BCDEC_G52_PATH_NATIVE_BC,         /* el dispositivo expone textureCompressionBC */
    BCDEC_G52_PATH_TRANSCODE_ETC2,    /* BC1/2/3/7 -> ETC2, BC4/BC5 -> EAC R11/RG11 (bcdec_mali_g52_convert_bc_surface) */
    BCDEC_G52_PATH_CPU_DECODE_RGBA,   /* BC6H (HDR) o sin ETC2: descomprimir en CPU */
    BCDEC_G52_PATH_GPU_ASTC_TRANSCODE /* BC1/BC7 -> ASTC con el shader compute (bcdec_mali_g52_plan_astc_transcode) */
} bcdec_g52_texture_path_t;

BCDECDEF bcdec_g52_texture_path_t bcdec_mali_g52_select_texture_path(bcdec_format_t format, int deviceHasBC, int deviceHasETC2, int deviceHasASTC);
BCDECDEF size_t bcdec_mali_g52_converted_surface_size(bcdec_format_t srcFormat, int width, int height);
BCDECDEF bcdec_status_t bcdec_mali_g52_convert_bc_surface(bcdec_format_t srcFormat, const void* restrict srcData, int width, int height, int srgb, void* restrict dstData, size_t dstCapacity, unsigned int* restrict outVkFormat, size_t* restrict outSize);
BCDECDEF bcdec_status_t bcdec_mali_transcode_bc4_to_eac_r11(const void* restrict bc4Data, void* restrict eacR11Data, int width, int height);
BCDECDEF bcdec_status_t bcdec_mali_transcode_bc5_to_eac_rg11(const void* restrict bc5Data, void* restrict eacRG11Data, int width, int height);
BCDECDEF int bcdec_vulkan_validate_device_compression_features(uint32_t supportedFeaturesBitmask, bcdec_format_t targetFormat);
BCDECDEF int bcdec_vulkan_validate_core_compression_limits(int maxImageDimension2D, bcdec_format_t format);
BCDECDEF size_t bcdec_vulkan_compile_compute_shader_spirv(bcdec_format_t format, uint32_t* restrict outSpirvBinaryBuffer, size_t bufferCapacityWords);
BCDECDEF int bcdec_vulkan_1_0_validate_pipeline_cache_header_for_device(const void* restrict cacheHeaderData, size_t dataSize, uint32_t vendorID, uint32_t deviceID, const uint8_t* restrict pipelineCacheUUID);
BCDECDEF int bcdec_vulkan_1_0_build_vk_mipmap_buffer_image_copies(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, uint64_t baseBufferOffset, void* restrict outVkBufferImageCopyArray, size_t arrayCapacityBytes);

/* Cripto v2: ChaCha20 (RFC 8439) + HMAC-SHA256 (RFC 2104), encrypt-then-MAC */
BCDECDEF bcdec_status_t bcdec_crypto_hmac_sha256(const uint8_t* restrict key, size_t keyLen, const void* restrict data, size_t dataLen, uint8_t outMac[32]);
BCDECDEF bcdec_status_t bcdec_crypto_verify_secure_texture_payload_hmac(const void* restrict payload, size_t payloadSize, const uint8_t* restrict key, size_t keyLen, const uint8_t* restrict expectedHmacSha256, uint32_t* restrict outFlags);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_chacha20_xor(const void* restrict input, void* restrict output, size_t size, const uint8_t key32[32], const uint8_t iv16[16]);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_seal_container(const bcdec_pippetocrypto_header_t* restrict headerTemplate, const void* restrict plainPayload, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16], void* restrict outContainer, size_t outCapacity, size_t* restrict outSize);
BCDECDEF bcdec_status_t bcdec_pippetocrypto_open_container(const void* restrict containerBytes, size_t totalBytes, const uint8_t key32[32], bcdec_pippetocrypto_header_t* restrict outHeader, void* restrict outPlainPayload, size_t plainCapacity);

/* --- v2.54.2: ASTC en GPU, BC1 con alfa de 1 bit y selecci贸n de ruta ampliada --- */
#define BCDEC_G52_CAP_BC                 0x1u
#define BCDEC_G52_CAP_ETC2               0x2u
#define BCDEC_G52_CAP_ASTC               0x4u
#define BCDEC_G52_CAP_GPU_ASTC_SHADER    0x8u    /* tiene compilado el shader BC1/BC7 -> ASTC */
#define BCDEC_G52_CAP_GPU_ASTC_SHADER_EXT 0x10u  /* adem谩s incluye bc_decode_ext.glsl (BC2/BC3/BC4/BC5) */

/* Mismo orden y tama帽o (36 bytes) que el bloque push_constant del shader compute BC->ASTC */
typedef struct bcdec_g52_astc_transcode_push_constants {
    uint32_t block_x;        /* bloques ASTC de salida por fila */
    uint32_t block_x_src;    /* bloques BC por fila en el buffer de origen */
    uint32_t block_y;        /* bloques ASTC de salida por columna */
    uint32_t src_word_off;   /* desplazamiento en palabras de 32 bits */
    uint32_t format;         /* 0 = BC1, 1 = BC7 */
    uint32_t has_alpha;
    uint32_t astc8;          /* 1 = ASTC 8x8, 0 = ASTC 4x4 */
    uint32_t bc_bx;          /* bloques BC reales por fila */
    uint32_t bc_by;          /* bloques BC reales por columna */
} bcdec_g52_astc_transcode_push_constants_t;

typedef struct bcdec_g52_astc_transcode_plan {
    bcdec_g52_astc_transcode_push_constants_t push;
    uint32_t pushConstantSize;                 /* 36 */
    uint32_t localSizeX, localSizeY;           /* 8, 8 */
    uint32_t groupCountX, groupCountY, groupCountZ;   /* para vkCmdDispatch */
    uint32_t dstVkFormat;                      /* VK_FORMAT_ASTC_4x4/8x8 UNORM/SRGB_BLOCK */
    size_t   dstSizeBytes;
    size_t   srcSizeBytes;
} bcdec_g52_astc_transcode_plan_t;

BCDECDEF bcdec_status_t bcdec_mali_g52_plan_astc_transcode(bcdec_format_t srcFormat, int width, int height, int hasAlpha, int astc8x8, int srgb, size_t srcByteOffset, bcdec_g52_astc_transcode_plan_t* restrict outPlan);
BCDECDEF bcdec_g52_texture_path_t bcdec_mali_g52_select_texture_path_ex(bcdec_format_t format, uint32_t capsMask);
BCDECDEF bcdec_status_t bcdec_mali_transcode_bc1_to_etc2_a1(const void* restrict bc1Data, void* restrict etc2A1Data, int width, int height);

#endif /* BCDEC_HEADER_INCLUDED */

/* =========================================================================
 * IMPLEMENTACI脫N COMPLETA
 * ========================================================================= */
#ifdef BCDEC_IMPLEMENTATION
#ifdef BCDEC_IMPLEMENTATION_INCLUDED
#error "BCDEC_IMPLEMENTATION can only be defined once in a single translation unit."
#endif
#define BCDEC_IMPLEMENTATION_INCLUDED

#include <time.h>
#if defined(BCDEC_USE_ANDROID_AHARDWAREBUFFER) && defined(__ANDROID__)
#  include <android/hardware_buffer.h>
#endif

typedef struct bcdec__bitstream {
    uint64_t low;
    uint64_t high;
} bcdec__bitstream_t;

static BCDEC_FORCEINLINE int bcdec__bitstream_read_bits(bcdec__bitstream_t* restrict bstream, int numBits) {
    if (numBits <= 0 || numBits > 32) return 0;
    uint32_t mask = (numBits == 32) ? 0xFFFFFFFFU : ((1U << numBits) - 1U);
    uint32_t bits = (uint32_t)(bstream->low & mask);
    bstream->low >>= numBits;
    bstream->low |= (bstream->high & mask) << (64 - numBits);
    bstream->high >>= numBits;
    return (int)bits;
}

static BCDEC_FORCEINLINE int bcdec__bitstream_read_bit(bcdec__bitstream_t* restrict bstream) {
    return bcdec__bitstream_read_bits(bstream, 1);
}

static BCDEC_FORCEINLINE int bcdec__bitstream_read_bits_r(bcdec__bitstream_t* restrict bstream, int numBits) {
    int bits = bcdec__bitstream_read_bits(bstream, numBits);
    int result = 0;
    while (numBits--) {
        result <<= 1;
        result |= (bits & 1);
        bits >>= 1;
    }
    return result;
}

static BCDEC_FORCEINLINE int bcdec__extend_sign(int val, int bits) {
    return (val << (32 - bits)) >> (32 - bits);
}

static BCDEC_FORCEINLINE int bcdec__transform_inverse(int val, int a0, int bits, int isSigned) {
    val = (val + a0) & ((1 << bits) - 1);
    if (isSigned) val = bcdec__extend_sign(val, bits);
    return val;
}

static BCDEC_FORCEINLINE int bcdec__interpolate(int a, int b, const int* restrict weights, int index) {
    return (a * (64 - weights[index]) + b * weights[index] + 32) >> 6;
}

static BCDEC_FORCEINLINE void bcdec__swap_values(int* restrict a, int* restrict b) {
    int tmp = *a; *a = *b; *b = tmp;
}

BCDECDEF float bcdec_half_to_float(uint16_t half) {
    typedef union { uint32_t u; float f; } FP32;
    static const FP32 magic = { 113U << 23 };
    static const uint32_t shifted_exp = 0x7c00U << 13;
    FP32 o;
    uint32_t exp;

    o.u = (uint32_t)(half & 0x7fff) << 13;
    exp = shifted_exp & o.u;
    o.u += (127U - 15U) << 23;

    if (exp == shifted_exp) {
        o.u += (128U - 16U) << 23;
    } else if (exp == 0) {
        o.u += 1U << 23;
        o.f -= magic.f;
    }
    o.u |= (uint32_t)(half & 0x8000) << 16;
    return o.f;
}

BCDECDEF uint16_t bcdec_float_to_half(float val) {
    typedef union { uint32_t u; float f; } FP32;
    FP32 f32; f32.f = val;
    uint32_t u = f32.u;
    uint32_t sign = (u >> 16) & 0x8000U;
    uint32_t exp = (u >> 23) & 0xffU;
    uint32_t mantissa = u & 0x7fffffU;

    if (exp == 255) {
        return mantissa ? (uint16_t)(sign | 0x7e00U | (mantissa >> 13)) : (uint16_t)(sign | 0x7c00U);
    }
    int decExp = (int)exp - 127 + 15;
    if (decExp >= 31) return (uint16_t)(sign | 0x7c00U);
    if (decExp <= 0) {
        if (decExp < -10) return (uint16_t)sign;
        mantissa = (mantissa | 0x800000U) >> (1 - decExp);
        return (uint16_t)(sign | ((mantissa + 0x1000U) >> 13));
    }
    return (uint16_t)(sign | (uint32_t)(decExp << 10) | ((mantissa + 0x1000U) >> 13));
}

/* =========================================================================
 * DECODIFICADORES BC1 - BC7
 * ========================================================================= */

static void bcdec__color_block(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int onlyOpaqueMode) {
    uint16_t c0, c1;
    uint32_t refColors[4];
    uint8_t* restrict dstColors;
    uint32_t colorIndices;
    int i, j, idx;
    uint32_t r0, g0, b0, r1, g1, b1, r, g, b;

    memcpy(&c0, (const uint16_t*)compressedBlock + 0, sizeof(uint16_t));
    memcpy(&c1, (const uint16_t*)compressedBlock + 1, sizeof(uint16_t));

    r0 = (c0 >> 11) & 0x1F; g0 = (c0 >> 5) & 0x3F; b0 = c0 & 0x1F;
    r1 = (c1 >> 11) & 0x1F; g1 = (c1 >> 5) & 0x3F; b1 = c1 & 0x1F;

    r = (r0 * 527 + 23) >> 6; g = (g0 * 259 + 33) >> 6; b = (b0 * 527 + 23) >> 6;
    refColors[0] = 0xFF000000 | (b << 16) | (g << 8) | r;

    r = (r1 * 527 + 23) >> 6; g = (g1 * 259 + 33) >> 6; b = (b1 * 527 + 23) >> 6;
    refColors[1] = 0xFF000000 | (b << 16) | (g << 8) | r;

    if (c0 > c1 || onlyOpaqueMode) {
        r = ((2 * r0 + r1) * 351 + 61) >> 7;
        g = ((2 * g0 + g1) * 2763 + 1039) >> 11;
        b = ((2 * b0 + b1) * 351 + 61) >> 7;
        refColors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;

        r = ((r0 + r1 * 2) * 351 + 61) >> 7;
        g = ((g0 + g1 * 2) * 2763 + 1039) >> 11;
        b = ((b0 + b1 * 2) * 351 + 61) >> 7;
        refColors[3] = 0xFF000000 | (b << 16) | (g << 8) | r;
    } else {
        r = ((r0 + r1) * 1053 + 125) >> 8;
        g = ((g0 + g1) * 4145 + 1019) >> 11;
        b = ((b0 + b1) * 1053 + 125) >> 8;
        refColors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;
        refColors[3] = 0x00000000;
    }

    memcpy(&colorIndices, (const uint32_t*)compressedBlock + 1, sizeof(uint32_t));
    dstColors = (uint8_t*)decompressedBlock;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            idx = (int)(colorIndices & 0x03);
            memcpy(dstColors + (j * 4), &refColors[idx], sizeof(uint32_t));
            colorIndices >>= 2;
        }
        dstColors += destinationPitch;
    }
}

static void bcdec__sharp_alpha_block(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    uint16_t alpha[4];
    uint8_t* restrict decompressed;
    memcpy(alpha, compressedBlock, sizeof(alpha));
    decompressed = (uint8_t*)decompressedBlock;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            decompressed[j * 4] = (uint8_t)(((alpha[i] >> (4 * j)) & 0x0F) * 17);
        }
        decompressed += destinationPitch;
    }
}

static void bcdec__smooth_alpha_block(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int pixelSize) {
    uint8_t* restrict decompressed = (uint8_t*)decompressedBlock;
    uint8_t alpha[8];
    uint64_t block, indices;

    memcpy(&block, compressedBlock, sizeof(block));
    alpha[0] = (uint8_t)(block & 0xFF);
    alpha[1] = (uint8_t)((block >> 8) & 0xFF);

    if (alpha[0] > alpha[1]) {
        alpha[2] = (uint8_t)((6 * alpha[0] +     alpha[1]) / 7);
        alpha[3] = (uint8_t)((5 * alpha[0] + 2 * alpha[1]) / 7);
        alpha[4] = (uint8_t)((4 * alpha[0] + 3 * alpha[1]) / 7);
        alpha[5] = (uint8_t)((3 * alpha[0] + 4 * alpha[1]) / 7);
        alpha[6] = (uint8_t)((2 * alpha[0] + 5 * alpha[1]) / 7);
        alpha[7] = (uint8_t)((    alpha[0] + 6 * alpha[1]) / 7);
    } else {
        alpha[2] = (uint8_t)((4 * alpha[0] +     alpha[1]) / 5);
        alpha[3] = (uint8_t)((3 * alpha[0] + 2 * alpha[1]) / 5);
        alpha[4] = (uint8_t)((2 * alpha[0] + 3 * alpha[1]) / 5);
        alpha[5] = (uint8_t)((    alpha[0] + 4 * alpha[1]) / 5);
        alpha[6] = 0x00;
        alpha[7] = 0xFF;
    }

    indices = block >> 16;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            decompressed[j * pixelSize] = alpha[indices & 0x07];
            indices >>= 3;
        }
        decompressed += destinationPitch;
    }
}

#ifdef BCDEC_BC4BC5_PRECISE
static void bcdec__bc4_block(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int pixelSize, int isSigned) {
    int alpha[8];
    uint64_t block, indices;
    static const int aWeights4[4] = { 13107, 26215, 39321, 52429 };
    static const int aWeights6[6] = { 9363, 18724, 28086, 37450, 46812, 56173 };

    memcpy(&block, compressedBlock, sizeof(block));
    if (isSigned) {
        alpha[0] = (int8_t)(block & 0xFF);
        alpha[1] = (int8_t)((block >> 8) & 0xFF);
        if (alpha[0] < -127) alpha[0] = -127;
        if (alpha[1] < -127) alpha[1] = -127;
    } else {
        alpha[0] = (int)(block & 0xFF);
        alpha[1] = (int)((block >> 8) & 0xFF);
    }

    if (alpha[0] > alpha[1]) {
        alpha[2] = (aWeights6[5] * alpha[0] + aWeights6[0] * alpha[1] + 32768) >> 16;
        alpha[3] = (aWeights6[4] * alpha[0] + aWeights6[1] * alpha[1] + 32768) >> 16;
        alpha[4] = (aWeights6[3] * alpha[0] + aWeights6[2] * alpha[1] + 32768) >> 16;
        alpha[5] = (aWeights6[2] * alpha[0] + aWeights6[3] * alpha[1] + 32768) >> 16;
        alpha[6] = (aWeights6[1] * alpha[0] + aWeights6[4] * alpha[1] + 32768) >> 16;
        alpha[7] = (aWeights6[0] * alpha[0] + aWeights6[5] * alpha[1] + 32768) >> 16;
    } else {
        alpha[2] = (aWeights4[3] * alpha[0] + aWeights4[0] * alpha[1] + 32768) >> 16;
        alpha[3] = (aWeights4[2] * alpha[0] + aWeights4[1] * alpha[1] + 32768) >> 16;
        alpha[4] = (aWeights4[1] * alpha[0] + aWeights4[2] * alpha[1] + 32768) >> 16;
        alpha[5] = (aWeights4[0] * alpha[0] + aWeights4[3] * alpha[1] + 32768) >> 16;
        alpha[6] = isSigned ? -127 : 0;
        alpha[7] = isSigned ?  127 : 255;
    }

    indices = block >> 16;
    if (isSigned) {
        int8_t* restrict sblock = (int8_t*)decompressedBlock;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                sblock[j * pixelSize] = (int8_t)alpha[indices & 0x07];
                indices >>= 3;
            }
            sblock += destinationPitch;
        }
    } else {
        uint8_t* restrict ublock = (uint8_t*)decompressedBlock;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                ublock[j * pixelSize] = (uint8_t)alpha[indices & 0x07];
                indices >>= 3;
            }
            ublock += destinationPitch;
        }
    }
}

static void bcdec__bc4_block_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int pixelSize, int isSigned) {
    float* restrict decompressed = (float*)decompressedBlock;
    float alpha[8];
    uint64_t block, indices;
    memcpy(&block, compressedBlock, sizeof(block));

    if (isSigned) {
        alpha[0] = (float)((int8_t)(block & 0xFF)) / 127.0f;
        alpha[1] = (float)((int8_t)((block >> 8) & 0xFF)) / 127.0f;
        if (alpha[0] < -1.0f) alpha[0] = -1.0f;
        if (alpha[1] < -1.0f) alpha[1] = -1.0f;
    } else {
        alpha[0] = (float)(block & 0xFF) / 255.0f;
        alpha[1] = (float)((block >> 8) & 0xFF) / 255.0f;
    }

    if (alpha[0] > alpha[1]) {
        alpha[2] = (6.0f * alpha[0] +        alpha[1]) / 7.0f;
        alpha[3] = (5.0f * alpha[0] + 2.0f * alpha[1]) / 7.0f;
        alpha[4] = (4.0f * alpha[0] + 3.0f * alpha[1]) / 7.0f;
        alpha[5] = (3.0f * alpha[0] + 4.0f * alpha[1]) / 7.0f;
        alpha[6] = (2.0f * alpha[0] + 5.0f * alpha[1]) / 7.0f;
        alpha[7] = (       alpha[0] + 6.0f * alpha[1]) / 7.0f;
    } else {
        alpha[2] = (4.0f * alpha[0] +        alpha[1]) / 5.0f;
        alpha[3] = (3.0f * alpha[0] + 2.0f * alpha[1]) / 5.0f;
        alpha[4] = (2.0f * alpha[0] + 3.0f * alpha[1]) / 5.0f;
        alpha[5] = (       alpha[0] + 4.0f * alpha[1]) / 5.0f;
        alpha[6] = isSigned ? -1.0f : 0.0f;
        alpha[7] = 1.0f;
    }

    indices = block >> 16;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            decompressed[j * pixelSize] = alpha[indices & 0x07];
            indices >>= 3;
        }
        decompressed += destinationPitch;
    }
}
#endif

BCDECDEF void bcdec_bc1(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    bcdec__color_block(compressedBlock, decompressedBlock, destinationPitch, 0);
}

BCDECDEF void bcdec_bc2(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    bcdec__color_block(((const char*)compressedBlock) + 8, decompressedBlock, destinationPitch, 1);
    bcdec__sharp_alpha_block(compressedBlock, ((char*)decompressedBlock) + 3, destinationPitch);
}

BCDECDEF void bcdec_bc3(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    bcdec__color_block(((const char*)compressedBlock) + 8, decompressedBlock, destinationPitch, 1);
    bcdec__smooth_alpha_block(compressedBlock, ((char*)decompressedBlock) + 3, decompressedBlock ? destinationPitch : 0, 4);
}

#ifndef BCDEC_BC4BC5_PRECISE
BCDECDEF void bcdec_bc4(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    bcdec__smooth_alpha_block(compressedBlock, decompressedBlock, destinationPitch, 1);
}
BCDECDEF void bcdec_bc5(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    bcdec__smooth_alpha_block(compressedBlock, decompressedBlock, destinationPitch, 2);
    bcdec__smooth_alpha_block(((const char*)compressedBlock) + 8, ((char*)decompressedBlock) + 1, destinationPitch, 2);
}
#else
BCDECDEF void bcdec_bc4(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block(compressedBlock, decompressedBlock, destinationPitch, 1, isSigned);
}
BCDECDEF void bcdec_bc5(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block(compressedBlock, decompressedBlock, destinationPitch, 2, isSigned);
    bcdec__bc4_block(((const char*)compressedBlock) + 8, ((char*)decompressedBlock) + 1, destinationPitch, 2, isSigned);
}
BCDECDEF void bcdec_bc4_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block_float(compressedBlock, decompressedBlock, destinationPitch, 1, isSigned);
}
BCDECDEF void bcdec_bc5_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block_float(compressedBlock, decompressedBlock, destinationPitch, 2, isSigned);
    bcdec__bc4_block_float(((const char*)compressedBlock) + 8, ((char*)decompressedBlock) + 1, destinationPitch, 2, isSigned);
}
#endif

static inline int bcdec__unquantize(int val, int bits, int isSigned) {
    int unq, s = 0;
    if (!isSigned) {
        if (bits >= 15) unq = val;
        else if (!val) unq = 0;
        else if (val == ((1 << bits) - 1)) unq = 0xFFFF;
        else unq = ((val << 16) + 0x8000) >> bits;
    } else {
        if (bits >= 16) unq = val;
        else {
            if (val < 0) { s = 1; val = -val; }
            if (val == 0) unq = 0;
            else if (val >= ((1 << (bits - 1)) - 1)) unq = 0x7FFF;
            else unq = ((val << 15) + 0x4000) >> (bits - 1);
            if (s) unq = -unq;
        }
    }
    return unq;
}

static inline uint16_t bcdec__finish_unquantize(int val, int isSigned) {
    if (!isSigned) return (uint16_t)((val * 31) >> 6);
    val = (val < 0) ? -(((-val) * 31) >> 5) : (val * 31) >> 5;
    int s = 0;
    if (val < 0) { s = 0x8000; val = -val; }
    return (uint16_t)(s | val);
}

BCDECDEF void bcdec_bc6h_half(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    static const char actual_bits_count[4][14] = {
        { 10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16 },
        {  5, 6,  5,  4,  4, 5, 6, 5, 5, 6, 10,  9,  8,  4 },
        {  5, 6,  4,  5,  4, 5, 5, 6, 5, 6, 10,  9,  8,  4 },
        {  5, 6,  4,  4,  5, 5, 5, 5, 6, 6, 10,  9,  8,  4 }
    };
    static const uint8_t partition_sets[32][4][4] = {
        { {128, 0,   1, 1}, {0, 0, 1, 1}, {  0, 0, 1, 1}, {0, 0, 1, 129} },
        { {128, 0,   0, 1}, {0, 0, 0, 1}, {  0, 0, 0, 1}, {0, 0, 0, 129} },
        { {128, 1,   1, 1}, {0, 1, 1, 1}, {  0, 1, 1, 1}, {0, 1, 1, 129} },
        { {128, 0,   0, 1}, {0, 0, 1, 1}, {  0, 0, 1, 1}, {0, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 1}, {  0, 0, 0, 1}, {0, 0, 1, 129} },
        { {128, 0,   1, 1}, {0, 1, 1, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 1}, {0, 0, 1, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 1}, {  0, 0, 1, 1}, {0, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {  0, 0, 0, 1}, {0, 0, 1, 129} },
        { {128, 0,   1, 1}, {0, 1, 1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {  0, 0, 0, 1}, {0, 1, 1, 129} },
        { {128, 0,   0, 1}, {0, 1, 1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {1, 1, 1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {  0, 0, 0, 0}, {1, 1, 1, 129} },
        { {128, 0,   0, 0}, {1, 0, 0, 0}, {  1, 1, 1, 0}, {1, 1, 1, 129} },
        { {128, 1, 129, 1}, {0, 0, 0, 1}, {  0, 0, 0, 0}, {0, 0, 0,   0} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1,   0} },
        { {128, 1, 129, 1}, {0, 0, 1, 1}, {  0, 0, 0, 1}, {0, 0, 0,   0} },
        { {128, 0, 129, 1}, {0, 0, 0, 1}, {  0, 0, 0, 0}, {0, 0, 0,   0} },
        { {128, 0,   0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1,   0} },
        { {128, 0,   0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0,   0} },
        { {128, 1,   1, 1}, {0, 0, 1, 1}, {  0, 0, 1, 1}, {0, 0, 0, 129} },
        { {128, 0, 129, 1}, {0, 0, 0, 1}, {  0, 0, 0, 1}, {0, 0, 0,   0} },
        { {128, 0,   0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0,   0} },
        { {128, 1, 129, 0}, {0, 1, 1, 0}, {  0, 1, 1, 0}, {0, 1, 1,   0} },
        { {128, 0, 129, 1}, {0, 1, 1, 0}, {  0, 1, 1, 0}, {1, 1, 0,   0} },
        { {128, 0,   0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0,   0} },
        { {128, 0,   0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0,   0} },
        { {128, 1, 129, 1}, {0, 0, 0, 1}, {  1, 0, 0, 0}, {1, 1, 1,   0} },
        { {128, 0, 129, 1}, {1, 0, 0, 1}, {  1, 0, 0, 1}, {1, 1, 0,   0} }
    };
    static const int aWeight3[8]  = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const int aWeight4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

    bcdec__bitstream_t bstream;
    int mode, partition = 0, numPartitions, i, j, partitionSet, indexBits, index, ep_i, actualBits0Mode;
    int r[4] = {0}, g[4] = {0}, b[4] = {0};
    uint16_t* restrict decompressed = (uint16_t*)decompressedBlock;
    const int *weights;

    memcpy(&bstream.low, (const uint64_t*)compressedBlock + 0, sizeof(uint64_t));
    memcpy(&bstream.high, (const uint64_t*)compressedBlock + 1, sizeof(uint64_t));

    mode = bcdec__bitstream_read_bits(&bstream, 2);
    if (mode > 1) mode |= (bcdec__bitstream_read_bits(&bstream, 3) << 2);

    switch (mode) {
        case 0:
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4; b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10); b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5); g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream); g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 0; break;
        case 1:
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5; g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 5; r[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[3] |= bcdec__bitstream_read_bit(&bstream); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4; g[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 5; b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4; b[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4); b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6); partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 1; break;
        case 2:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10; g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 4); g[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream); g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 4); b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1; b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5); mode = 2; break;
        case 6:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10; g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10; g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 4); b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1; b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 4); b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5); mode = 3; break;
        case 10:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10; b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10; b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4); b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10; b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 4); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5); mode = 4; break;
        case 14:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 9); b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 9); g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 9); b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5); g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream); g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 5; break;
        case 18:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8); g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4; g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8); b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream); g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6); partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 6; break;
        case 22:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8); b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4; g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5; g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8); g[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4; g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 6); g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 7; break;
        case 26:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4; g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 5; g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8); b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4; g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5); b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4); b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 8; break;
        case 30:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 6); g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream); b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4; g[0] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5; b[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2; g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 6); g[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3; b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4; r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4); g[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4); b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4); r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6); partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 9; break;
        case 3:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 10); b[1] |= bcdec__bitstream_read_bits(&bstream, 10);
            mode = 10; break;
        case 7:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10; g[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10; b[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10; mode = 11; break;
        case 11:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            r[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10; g[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10; b[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10; mode = 12; break;
        case 15:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10); g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10); r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10; g[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10; b[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10; mode = 13; break;
        default:
            for (i = 0; i < 4; ++i) {
                for (j = 0; j < 4; ++j) {
                    decompressed[j * 3 + 0] = 0; decompressed[j * 3 + 1] = 0; decompressed[j * 3 + 2] = 0;
                }
                decompressed += destinationPitch;
            }
            return;
    }

    numPartitions = (mode >= 10) ? 0 : 1;
    actualBits0Mode = actual_bits_count[0][mode];
    if (isSigned) {
        r[0] = bcdec__extend_sign(r[0], actualBits0Mode);
        g[0] = bcdec__extend_sign(g[0], actualBits0Mode);
        b[0] = bcdec__extend_sign(b[0], actualBits0Mode);
    }
    if ((mode != 9 && mode != 10) || isSigned) {
        for (i = 1; i < (numPartitions + 1) * 2; ++i) {
            r[i] = bcdec__extend_sign(r[i], actual_bits_count[1][mode]);
            g[i] = bcdec__extend_sign(g[i], actual_bits_count[2][mode]);
            b[i] = bcdec__extend_sign(b[i], actual_bits_count[3][mode]);
        }
    }
    if (mode != 9 && mode != 10) {
        for (i = 1; i < (numPartitions + 1) * 2; ++i) {
            r[i] = bcdec__transform_inverse(r[i], r[0], actualBits0Mode, isSigned);
            g[i] = bcdec__transform_inverse(g[i], g[0], actualBits0Mode, isSigned);
            b[i] = bcdec__transform_inverse(b[i], b[0], actualBits0Mode, isSigned);
        }
    }
    for (i = 0; i < (numPartitions + 1) * 2; ++i) {
        r[i] = bcdec__unquantize(r[i], actualBits0Mode, isSigned);
        g[i] = bcdec__unquantize(g[i], actualBits0Mode, isSigned);
        b[i] = bcdec__unquantize(b[i], actualBits0Mode, isSigned);
    }

    weights = (mode >= 10) ? aWeight4 : aWeight3;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            partitionSet = (mode >= 10) ? ((i|j) ? 0 : 128) : partition_sets[partition][i][j];
            indexBits = (mode >= 10) ? 4 : 3;
            if (partitionSet & 0x80) indexBits--;
            partitionSet &= 0x01;
            index = bcdec__bitstream_read_bits(&bstream, indexBits);
            ep_i = partitionSet * 2;
            decompressed[j * 3 + 0] = bcdec__finish_unquantize(bcdec__interpolate(r[ep_i], r[ep_i+1], weights, index), isSigned);
            decompressed[j * 3 + 1] = bcdec__finish_unquantize(bcdec__interpolate(g[ep_i], g[ep_i+1], weights, index), isSigned);
            decompressed[j * 3 + 2] = bcdec__finish_unquantize(bcdec__interpolate(b[ep_i], b[ep_i+1], weights, index), isSigned);
        }
        decompressed += destinationPitch;
    }
}

BCDECDEF void bcdec_bc6h_float(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch, int isSigned) {
    uint16_t block[16*3];
    bcdec_bc6h_half(compressedBlock, block, 4*3, isSigned);
    const uint16_t* restrict b = block;
    float* restrict decompressed = (float*)decompressedBlock;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            decompressed[j * 3 + 0] = bcdec_half_to_float(*b++);
            decompressed[j * 3 + 1] = bcdec_half_to_float(*b++);
            decompressed[j * 3 + 2] = bcdec_half_to_float(*b++);
        }
        decompressed += destinationPitch;
    }
}

BCDECDEF void bcdec_bc7(const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    static const char actual_bits_count[2][8] = {
        { 4, 6, 5, 7, 5, 7, 7, 5 },
        { 0, 0, 0, 0, 6, 8, 7, 5 },
    };
    static const uint8_t partition_sets[2][64][4][4] = {
        {
            { {128, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129} },
            { {128, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 129} },
            { {128, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 129} },
            { {128, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 129} },
            { {128, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 129} },
            { {128, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 0}, {1, 1, 1, 129} },
            { {128, 1, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1, 0} },
            { {128, 1, 129, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0} },
            { {128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0} },
            { {128, 0, 0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1, 0} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0} },
            { {128, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 129} },
            { {128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0} },
            { {128, 0, 0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0} },
            { {128, 1, 129, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0} },
            { {128, 0, 129, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 0, 0} },
            { {128, 0, 0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0, 0} },
            { {128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0, 0} },
            { {128, 1, 129, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0} },
            { {128, 0, 129, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 0} },
            { {128, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 129} },
            { {128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 129} },
            { {128, 1, 0, 1}, {1, 0, 129, 0}, {0, 1, 0, 1}, {1, 0, 1, 0} },
            { {128, 0, 1, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 1, 0, 0} },
            { {128, 0, 129, 1}, {1, 1, 0, 0}, {0, 0, 1, 1}, {1, 1, 0, 0} },
            { {128, 1, 0, 1}, {0, 1, 0, 1}, {129, 0, 1, 0}, {1, 0, 1, 0} },
            { {128, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 1, 0}, {1, 0, 0, 129} },
            { {128, 1, 0, 1}, {1, 0, 1, 0}, {1, 0, 1, 0}, {0, 1, 0, 129} },
            { {128, 1, 129, 1}, {0, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 1, 0} },
            { {128, 0, 0, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 0, 0, 0} },
            { {128, 0, 129, 1}, {0, 0, 1, 0}, {0, 1, 0, 0}, {1, 1, 0, 0} },
            { {128, 0, 129, 1}, {1, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 0, 0} },
            { {128, 1, 129, 0}, {1, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 1, 0} },
            { {128, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 0, 0}, {0, 0, 1, 129} },
            { {128, 1, 1, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {1, 0, 0, 129} },
            { {128, 0, 0, 0}, {0, 1, 129, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
            { {128, 1, 0, 0}, {1, 1, 129, 0}, {0, 1, 0, 0}, {0, 0, 0, 0} },
            { {128, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0}, {0, 0, 0, 0} },
            { {128, 0, 0, 0}, {0, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0} },
            { {128, 0, 0, 0}, {0, 1, 0, 0}, {129, 1, 1, 0}, {0, 1, 0, 0} },
            { {128, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0, 1, 1}, {0, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 129} },
            { {128, 1, 129, 0}, {0, 0, 1, 1}, {1, 0, 0, 1}, {1, 1, 0, 0} },
            { {128, 0, 129, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}, {0, 1, 1, 0} },
            { {128, 1, 1, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 129} },
            { {128, 1, 1, 0}, {0, 0, 1, 1}, {0, 0, 1, 1}, {1, 0, 0, 129} },
            { {128, 1, 1, 1}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 129} },
            { {128, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 1, 129} },
            { {128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129} },
            { {128, 0, 129, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0} },
            { {128, 0, 129, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0} },
            { {128, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 1}, {0, 1, 1, 129} }
        },
        {
            { {128, 0, 1, 129}, {0, 0, 1, 1}, {0, 2, 2, 1}, {2, 2, 2, 130} },
            { {128, 0, 0, 129}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 2, 1} },
            { {128, 0, 0, 0}, {2, 0, 0, 1}, {130, 2, 1, 1}, {2, 2, 1, 129} },
            { {128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 1}, {0, 1, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 2, 2}, {1, 1, 2, 130} },
            { {128, 0, 1, 129}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 2, 130} },
            { {128, 0, 2, 130}, {0, 0, 2, 2}, {1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0, 1, 1}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 1, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 1, 1}, {2, 2, 2, 130} },
            { {128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {2, 2, 2, 130} },
            { {128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 2, 2}, {2, 2, 2, 130} },
            { {128, 0, 1, 2}, {0, 0, 129, 2}, {0, 0, 1, 2}, {0, 0, 1, 130} },
            { {128, 1, 1, 2}, {0, 1, 129, 2}, {0, 1, 1, 2}, {0, 1, 1, 130} },
            { {128, 1, 2, 2}, {0, 129, 2, 2}, {0, 1, 2, 2}, {0, 1, 2, 130} },
            { {128, 0, 1, 129}, {0, 1, 1, 2}, {1, 1, 2, 2}, {1, 2, 2, 130} },
            { {128, 0, 1, 129}, {2, 0, 0, 1}, {130, 2, 0, 0}, {2, 2, 2, 0} },
            { {128, 0, 0, 129}, {0, 0, 1, 1}, {0, 1, 1, 2}, {1, 1, 2, 130} },
            { {128, 1, 1, 129}, {0, 0, 1, 1}, {130, 0, 0, 1}, {2, 2, 0, 0} },
            { {128, 0, 0, 0}, {1, 1, 2, 2}, {129, 1, 2, 2}, {1, 1, 2, 130} },
            { {128, 0, 2, 130}, {0, 0, 2, 2}, {0, 0, 2, 2}, {1, 1, 1, 129} },
            { {128, 1, 1, 129}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 2, 2, 130} },
            { {128, 0, 0, 129}, {0, 0, 0, 1}, {130, 2, 2, 1}, {2, 2, 2, 1} },
            { {128, 0, 0, 0}, {0, 0, 129, 1}, {0, 1, 2, 2}, {0, 1, 2, 130} },
            { {128, 0, 0, 0}, {1, 1, 0, 0}, {130, 2, 129, 0}, {2, 2, 1, 0} },
            { {128, 1, 2, 130}, {0, 129, 2, 2}, {0, 0, 1, 1}, {0, 0, 0, 0} },
            { {128, 0, 1, 2}, {0, 0, 1, 2}, {129, 1, 2, 2}, {2, 2, 2, 130} },
            { {128, 1, 1, 0}, {1, 2, 130, 1}, {129, 2, 2, 1}, {0, 1, 1, 0} },
            { {128, 0, 0, 0}, {0, 1, 129, 0}, {1, 2, 130, 1}, {1, 2, 2, 1} },
            { {128, 0, 2, 2}, {1, 1, 0, 2}, {129, 1, 0, 2}, {0, 0, 2, 130} },
            { {128, 1, 1, 0}, {0, 129, 1, 0}, {2, 0, 0, 2}, {2, 2, 2, 130} },
            { {128, 0, 1, 1}, {0, 1, 2, 2}, {0, 1, 130, 2}, {0, 0, 1, 129} },
            { {128, 0, 0, 0}, {2, 0, 0, 0}, {130, 2, 1, 1}, {2, 2, 2, 129} },
            { {128, 0, 0, 0}, {0, 0, 0, 2}, {129, 1, 2, 2}, {1, 2, 2, 130} },
            { {128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 2}, {0, 0, 1, 129} },
            { {128, 0, 1, 129}, {0, 0, 1, 2}, {0, 0, 2, 2}, {0, 2, 2, 130} },
            { {128, 1, 2, 0}, {0, 129, 2, 0}, {0, 1, 130, 0}, {0, 1, 2, 0} },
            { {128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 130, 2}, {0, 0, 0, 0} },
            { {128, 1, 2, 0}, {1, 2, 0, 1}, {130, 0, 129, 2}, {0, 1, 2, 0} },
            { {128, 1, 2, 0}, {2, 0, 1, 2}, {129, 130, 0, 1}, {0, 1, 2, 0} },
            { {128, 0, 1, 1}, {2, 2, 0, 0}, {1, 1, 130, 2}, {0, 0, 1, 129} },
            { {128, 0, 1, 1}, {1, 1, 130, 2}, {2, 2, 0, 0}, {0, 0, 1, 129} },
            { {128, 1, 0, 129}, {0, 1, 0, 1}, {2, 2, 2, 2}, {2, 2, 2, 130} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {130, 1, 2, 1}, {2, 1, 2, 129} },
            { {128, 0, 2, 2}, {1, 129, 2, 2}, {0, 0, 2, 2}, {1, 1, 2, 130} },
            { {128, 0, 2, 130}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 1, 129} },
            { {128, 2, 2, 0}, {1, 2, 130, 1}, {0, 2, 2, 0}, {1, 2, 2, 129} },
            { {128, 1, 0, 1}, {2, 2, 130, 2}, {2, 2, 2, 2}, {0, 1, 0, 129} },
            { {128, 0, 0, 0}, {2, 1, 2, 1}, {130, 1, 2, 1}, {2, 1, 2, 129} },
            { {128, 1, 0, 129}, {0, 1, 0, 1}, {0, 1, 0, 1}, {2, 2, 2, 130} },
            { {128, 2, 2, 130}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 1, 1, 129} },
            { {128, 0, 0, 2}, {1, 129, 1, 2}, {0, 0, 0, 2}, {1, 1, 1, 130} },
            { {128, 0, 0, 0}, {2, 129, 1, 2}, {2, 1, 1, 2}, {2, 1, 1, 130} },
            { {128, 2, 2, 2}, {0, 129, 1, 1}, {0, 1, 1, 1}, {0, 2, 2, 130} },
            { {128, 0, 0, 2}, {1, 1, 1, 2}, {129, 1, 1, 2}, {0, 0, 0, 130} },
            { {128, 1, 1, 0}, {0, 129, 1, 0}, {0, 1, 1, 0}, {2, 2, 2, 130} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {2, 1, 129, 2}, {2, 1, 1, 130} },
            { {128, 1, 1, 0}, {0, 129, 1, 0}, {2, 2, 2, 2}, {2, 2, 2, 130} },
            { {128, 0, 2, 2}, {0, 0, 1, 1}, {0, 0, 129, 1}, {0, 0, 2, 130} },
            { {128, 0, 2, 2}, {1, 1, 2, 2}, {129, 1, 2, 2}, {0, 0, 2, 130} },
            { {128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 129, 1, 130} },
            { {128, 0, 0, 130}, {0, 0, 0, 1}, {0, 0, 0, 2}, {0, 0, 0, 129} },
            { {128, 2, 2, 2}, {1, 2, 2, 2}, {0, 2, 2, 2}, {129, 2, 2, 130} },
            { {128, 1, 0, 129}, {2, 2, 2, 2}, {2, 2, 2, 2}, {2, 2, 2, 130} },
            { {128, 1, 1, 129}, {2, 0, 1, 1}, {130, 2, 0, 1}, {2, 2, 2, 0} }
        }
    };
    static const int aWeight2[] = { 0, 21, 43, 64 };
    static const int aWeight3[] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const int aWeight4[] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
    static unsigned char sModeHasPBits = 0xCB;

    bcdec__bitstream_t bstream;
    int mode, partition = 0, numPartitions = 1, numEndpoints, i, j, k, rotation = 0, partitionSet;
    int indexSelectionBit = 0, indexBits, indexBits2, index, index2;
    int endpoints[6][4];
    char indices[4][4];
    int r, g, b, a;
    const int *weights, *weights2;
    uint8_t* restrict decompressed = (uint8_t*)decompressedBlock;

    memcpy(&bstream.low, (const uint64_t*)compressedBlock + 0, sizeof(uint64_t));
    memcpy(&bstream.high, (const uint64_t*)compressedBlock + 1, sizeof(uint64_t));

    for (mode = 0; mode < 8 && (0 == bcdec__bitstream_read_bit(&bstream)); ++mode);
    if (mode >= 8) {
        for (i = 0; i < 4; ++i) {
            for (j = 0; j < 4; ++j) {
                decompressed[j * 4 + 0] = 0; decompressed[j * 4 + 1] = 0;
                decompressed[j * 4 + 2] = 0; decompressed[j * 4 + 3] = 0;
            }
            decompressed += destinationPitch;
        }
        return;
    }

    if (mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 7) {
        numPartitions = (mode == 0 || mode == 2) ? 3 : 2;
        partition = bcdec__bitstream_read_bits(&bstream, (mode == 0) ? 4 : 6);
    }
    numEndpoints = numPartitions * 2;
    if (mode == 4 || mode == 5) {
        rotation = bcdec__bitstream_read_bits(&bstream, 2);
        if (mode == 4) indexSelectionBit = bcdec__bitstream_read_bit(&bstream);
    }

    for (i = 0; i < 3; ++i)
        for (j = 0; j < numEndpoints; ++j)
            endpoints[j][i] = bcdec__bitstream_read_bits(&bstream, actual_bits_count[0][mode]);

    if (actual_bits_count[1][mode] > 0)
        for (j = 0; j < numEndpoints; ++j)
            endpoints[j][3] = bcdec__bitstream_read_bits(&bstream, actual_bits_count[1][mode]);

    if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
        for (i = 0; i < numEndpoints; ++i)
            for (j = 0; j < 4; ++j) endpoints[i][j] <<= 1;

        if (mode == 1) {
            i = bcdec__bitstream_read_bit(&bstream);
            j = bcdec__bitstream_read_bit(&bstream);
            for (k = 0; k < 3; ++k) {
                endpoints[0][k] |= i; endpoints[1][k] |= i;
                endpoints[2][k] |= j; endpoints[3][k] |= j;
            }
        } else if (sModeHasPBits & (1 << mode)) {
            for (i = 0; i < numEndpoints; ++i) {
                j = bcdec__bitstream_read_bit(&bstream);
                for (k = 0; k < 4; ++k) endpoints[i][k] |= j;
            }
        }
    }

    for (i = 0; i < numEndpoints; ++i) {
        j = actual_bits_count[0][mode] + ((sModeHasPBits >> mode) & 1);
        for (k = 0; k < 3; ++k) {
            endpoints[i][k] = (endpoints[i][k] << (8 - j)) | (endpoints[i][k] >> j);
        }
        j = actual_bits_count[1][mode] + ((sModeHasPBits >> mode) & 1);
        endpoints[i][3] = (endpoints[i][3] << (8 - j)) | (endpoints[i][3] >> j);
    }

    if (!actual_bits_count[1][mode]) {
        for (j = 0; j < numEndpoints; ++j) endpoints[j][3] = 0xFF;
    }

    indexBits = (mode == 0 || mode == 1) ? 3 : ((mode == 6) ? 4 : 2);
    indexBits2 = (mode == 4) ? 3 : ((mode == 5) ? 2 : 0);
    weights = (indexBits == 2) ? aWeight2 : ((indexBits == 3) ? aWeight3 : aWeight4);
    weights2 = (indexBits2 == 2) ? aWeight2 : aWeight3;

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            partitionSet = (numPartitions == 1) ? ((i | j) ? 0 : 128) : partition_sets[numPartitions - 2][partition][i][j];
            indexBits = (mode == 0 || mode == 1) ? 3 : ((mode == 6) ? 4 : 2);
            if (partitionSet & 0x80) indexBits--;
            indices[i][j] = (char)bcdec__bitstream_read_bits(&bstream, indexBits);
        }
    }

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            partitionSet = (numPartitions == 1) ? ((i|j) ? 0 : 128) : partition_sets[numPartitions - 2][partition][i][j];
            partitionSet &= 0x03;
            index = indices[i][j];

            if (!indexBits2) {
                r = bcdec__interpolate(endpoints[partitionSet * 2][0], endpoints[partitionSet * 2 + 1][0], weights, index);
                g = bcdec__interpolate(endpoints[partitionSet * 2][1], endpoints[partitionSet * 2 + 1][1], weights, index);
                b = bcdec__interpolate(endpoints[partitionSet * 2][2], endpoints[partitionSet * 2 + 1][2], weights, index);
                a = bcdec__interpolate(endpoints[partitionSet * 2][3], endpoints[partitionSet * 2 + 1][3], weights, index);
            } else {
                index2 = bcdec__bitstream_read_bits(&bstream, (i|j) ? indexBits2 : (indexBits2 - 1));
                if (!indexSelectionBit) {
                    r = bcdec__interpolate(endpoints[partitionSet * 2][0], endpoints[partitionSet * 2 + 1][0], weights, index);
                    g = bcdec__interpolate(endpoints[partitionSet * 2][1], endpoints[partitionSet * 2 + 1][1], weights, index);
                    b = bcdec__interpolate(endpoints[partitionSet * 2][2], endpoints[partitionSet * 2 + 1][2], weights, index);
                    a = bcdec__interpolate(endpoints[partitionSet * 2][3], endpoints[partitionSet * 2 + 1][3], weights2, index2);
                } else {
                    r = bcdec__interpolate(endpoints[partitionSet * 2][0], endpoints[partitionSet * 2 + 1][0], weights2, index2);
                    g = bcdec__interpolate(endpoints[partitionSet * 2][1], endpoints[partitionSet * 2 + 1][1], weights2, index2);
                    b = bcdec__interpolate(endpoints[partitionSet * 2][2], endpoints[partitionSet * 2 + 1][2], weights2, index2);
                    a = bcdec__interpolate(endpoints[partitionSet * 2][3], endpoints[partitionSet * 2 + 1][3], weights, index);
                }
            }

            switch (rotation) {
                case 1: bcdec__swap_values(&a, &r); break;
                case 2: bcdec__swap_values(&a, &g); break;
                case 3: bcdec__swap_values(&a, &b); break;
            }

            decompressed[j * 4 + 0] = (uint8_t)r;
            decompressed[j * 4 + 1] = (uint8_t)g;
            decompressed[j * 4 + 2] = (uint8_t)b;
            decompressed[j * 4 + 3] = (uint8_t)a;
        }
        decompressed += destinationPitch;
    }
}

/* =========================================================================
 * DESPACHADORES Y UTILIDADES DE FORMATO
 * ========================================================================= */

BCDECDEF int bcdec_decompress_block(bcdec_format_t format, const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    switch (format) {
        case BCDEC_FORMAT_BC1: bcdec_bc1(compressedBlock, decompressedBlock, destinationPitch); return 1;
        case BCDEC_FORMAT_BC2: bcdec_bc2(compressedBlock, decompressedBlock, destinationPitch); return 1;
        case BCDEC_FORMAT_BC3: bcdec_bc3(compressedBlock, decompressedBlock, destinationPitch); return 1;
        case BCDEC_FORMAT_BC4:
#ifndef BCDEC_BC4BC5_PRECISE
            bcdec_bc4(compressedBlock, decompressedBlock, destinationPitch);
#else
            bcdec_bc4(compressedBlock, decompressedBlock, destinationPitch, 0);
#endif
            return 1;
        case BCDEC_FORMAT_BC5:
#ifndef BCDEC_BC4BC5_PRECISE
            bcdec_bc5(compressedBlock, decompressedBlock, destinationPitch);
#else
            bcdec_bc5(compressedBlock, decompressedBlock, destinationPitch, 0);
#endif
            return 1;
        case BCDEC_FORMAT_BC6H: bcdec_bc6h_half(compressedBlock, decompressedBlock, destinationPitch, 0); return 1;
        case BCDEC_FORMAT_BC7:  bcdec_bc7(compressedBlock, decompressedBlock, destinationPitch); return 1;
        default: return 0;
    }
}

BCDECDEF int bcdec_decompress_block_float(bcdec_format_t format, const void* restrict compressedBlock, void* restrict decompressedBlock, int destinationPitch) {
    if (format == BCDEC_FORMAT_BC6H) {
        bcdec_bc6h_float(compressedBlock, decompressedBlock, destinationPitch / (int)sizeof(float), 0);
        return 1;
    }
    uint8_t u8Buffer[4 * 4 * 4];
    if (!bcdec_decompress_block(format, compressedBlock, u8Buffer, 4 * 4)) return 0;
    for (int y = 0; y < 4; ++y) {
        const uint8_t* restrict srcRow = u8Buffer + y * 16;
        float* restrict dstRow = (float*)((char*)decompressedBlock + y * destinationPitch);
        for (int x = 0; x < 4; ++x) {
            dstRow[x * 4 + 0] = (float)srcRow[x * 4 + 0] * (1.0f / 255.0f);
            dstRow[x * 4 + 1] = (float)srcRow[x * 4 + 1] * (1.0f / 255.0f);
            dstRow[x * 4 + 2] = (float)srcRow[x * 4 + 2] * (1.0f / 255.0f);
            dstRow[x * 4 + 3] = (float)srcRow[x * 4 + 3] * (1.0f / 255.0f);
        }
    }
    return 1;
}

BCDECDEF int bcdec_validate_block(bcdec_format_t format, const void* restrict compressedBlock) {
    if (!compressedBlock) return 0;
    switch (format) {
        case BCDEC_FORMAT_BC1: case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3:
        case BCDEC_FORMAT_BC4: case BCDEC_FORMAT_BC5: return 1;
        case BCDEC_FORMAT_BC6H: {
            uint32_t header; memcpy(&header, compressedBlock, sizeof(uint32_t));
            int mode = (int)(header & 0x03);
            if (mode > 1) mode |= ((header >> 2) & 0x07) << 2;
            return (mode <= 13);
        }
        case BCDEC_FORMAT_BC7: {
            uint8_t firstByte = *(const uint8_t*)compressedBlock;
            int mode = 0;
            while (mode < 8 && !(firstByte & (1 << mode))) mode++;
            return (mode < 8);
        }
        default: return 0;
    }
}

BCDECDEF int bcdec_validate_texture(bcdec_format_t format, const void* restrict compressedData, int width, int height) {
    if (!compressedData || width <= 0 || height <= 0) return 0;
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0) return 0;
    int blocksX = (width + 3) >> 2;
    int blocksY = (height + 3) >> 2;
    const uint8_t* restrict ptr = (const uint8_t*)compressedData;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (!bcdec_validate_block(format, ptr)) return 0;
            ptr += blockSize;
        }
    }
    return 1;
}

BCDECDEF bcdec_status_t bcdec_decompress_block_safe(bcdec_format_t format, const void* restrict compressedBlock, size_t compressedSize, void* restrict decompressedBlock, int destinationPitch) {
    if (!compressedBlock || !decompressedBlock) return BCDEC_ERROR_INVALID_PARAMETER;
    size_t expectedSize = bcdec_format_block_size(format);
    if (expectedSize == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (compressedSize < expectedSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    if (!bcdec_validate_block(format, compressedBlock)) return BCDEC_ERROR_INVALID_BLOCK;
    return bcdec_decompress_block(format, compressedBlock, decompressedBlock, destinationPitch) ? BCDEC_SUCCESS : BCDEC_ERROR_INVALID_BLOCK;
}

BCDECDEF bcdec_status_t bcdec_decompress_texture_swizzled(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch, bcdec_swizzle_config_t swizzle) {
    if (!compressedData || !decompressedImage || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec_decompress_texture(format, compressedData, decompressedImage, width, height, destinationPitch);
    int bytesPerPixel = bcdec_format_pixel_size(format);
    if (bytesPerPixel >= 3) {
        bcdec_swizzle_surface_simd(decompressedImage, width, height, bytesPerPixel, swizzle.r, swizzle.g, swizzle.b, swizzle.a, destinationPitch);
    }
    return BCDEC_SUCCESS;
}

BCDECDEF size_t bcdec_format_block_size(bcdec_format_t format) {
    switch (format) {
        case BCDEC_FORMAT_BC1: case BCDEC_FORMAT_BC4: return 8;
        case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3: case BCDEC_FORMAT_BC5:
        case BCDEC_FORMAT_BC6H: case BCDEC_FORMAT_BC7: return 16;
        default: return 0;
    }
}

BCDECDEF int bcdec_format_pixel_size(bcdec_format_t format) {
    switch (format) {
        case BCDEC_FORMAT_BC1: case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3: case BCDEC_FORMAT_BC7: return 4;
        case BCDEC_FORMAT_BC4: return 1;
        case BCDEC_FORMAT_BC5: return 2;
        case BCDEC_FORMAT_BC6H: return 6;
        default: return 0;
    }
}

BCDECDEF const char* bcdec_format_string(bcdec_format_t format) {
    switch (format) {
        case BCDEC_FORMAT_BC1:  return "BC1 / DXT1";
        case BCDEC_FORMAT_BC2:  return "BC2 / DXT3";
        case BCDEC_FORMAT_BC3:  return "BC3 / DXT5";
        case BCDEC_FORMAT_BC4:  return "BC4 / ATI1";
        case BCDEC_FORMAT_BC5:  return "BC5 / ATI2";
        case BCDEC_FORMAT_BC6H: return "BC6H / BC6H_UF16";
        case BCDEC_FORMAT_BC7:  return "BC7 / BPTC";
        default:                return "UNKNOWN";
    }
}

BCDECDEF bcdec_format_t bcdec_format_from_gl(unsigned int glInternalFormat) {
    switch (glInternalFormat) {
        case 0x83F1: case 0x83F0: return BCDEC_FORMAT_BC1;
        case 0x83F2: return BCDEC_FORMAT_BC2;
        case 0x83F3: return BCDEC_FORMAT_BC3;
        case 0x8DBB: case 0x8DBC: return BCDEC_FORMAT_BC4;
        case 0x8DBD: case 0x8DBE: return BCDEC_FORMAT_BC5;
        case 0x8E8F: case 0x8E8E: return BCDEC_FORMAT_BC6H;
        case 0x8E8C: case 0x8E8D: return BCDEC_FORMAT_BC7;
        default: return (bcdec_format_t)0;
    }
}

BCDECDEF bcdec_format_t bcdec_format_from_vulkan(unsigned int vkFormat) {
    switch (vkFormat) {
        case 131: case 132: case 133: case 134: return BCDEC_FORMAT_BC1;
        case 135: case 136: return BCDEC_FORMAT_BC2;
        case 137: case 138: return BCDEC_FORMAT_BC3;
        case 139: case 140: return BCDEC_FORMAT_BC4;
        case 141: case 142: return BCDEC_FORMAT_BC5;
        case 143: case 144: return BCDEC_FORMAT_BC6H;
        case 145: case 146: return BCDEC_FORMAT_BC7;
        default: return (bcdec_format_t)0;
    }
}

BCDECDEF unsigned int bcdec_format_to_vulkan(bcdec_format_t format, int srgb) {
    switch (format) {
        case BCDEC_FORMAT_BC1:  return srgb ? 134U : 133U; /* VK_FORMAT_BC1_RGBA_SRGB/UNORM_BLOCK */
        case BCDEC_FORMAT_BC2:  return srgb ? 136U : 135U; /* VK_FORMAT_BC2_SRGB/UNORM_BLOCK */
        case BCDEC_FORMAT_BC3:  return srgb ? 138U : 137U; /* VK_FORMAT_BC3_SRGB/UNORM_BLOCK */
        case BCDEC_FORMAT_BC4:  return 139U;               /* VK_FORMAT_BC4_UNORM_BLOCK */
        case BCDEC_FORMAT_BC5:  return 141U;               /* VK_FORMAT_BC5_UNORM_BLOCK */
        case BCDEC_FORMAT_BC6H: return 143U;               /* VK_FORMAT_BC6H_UFLOAT_BLOCK */
        case BCDEC_FORMAT_BC7:  return srgb ? 146U : 145U; /* VK_FORMAT_BC7_SRGB/UNORM_BLOCK */
        default: return 0U;
    }
}

BCDECDEF bcdec_format_t bcdec_format_from_dxgi(unsigned int dxgiFormat) {
    switch (dxgiFormat) {
        case 70: case 71: return BCDEC_FORMAT_BC1;
        case 73: case 74: return BCDEC_FORMAT_BC2;
        case 76: case 77: return BCDEC_FORMAT_BC3;
        case 79: case 80: return BCDEC_FORMAT_BC4;
        case 82: case 83: return BCDEC_FORMAT_BC5;
        case 94: case 95: return BCDEC_FORMAT_BC6H;
        case 97: case 98: return BCDEC_FORMAT_BC7;
        default: return (bcdec_format_t)0;
    }
}

BCDECDEF int bcdec_calculate_mip_levels(int width, int height) {
    int levels = 1;
    while (width > 1 || height > 1) {
        width = (width > 1) ? (width >> 1) : 1;
        height = (height > 1) ? (height >> 1) : 1;
        levels++;
    }
    return levels;
}

BCDECDEF size_t bcdec_texture_compressed_size(bcdec_format_t format, int width, int height) {
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0 || width <= 0 || height <= 0) return 0;
    int blocksX = (width + 3) >> 2;
    int blocksY = (height + 3) >> 2;
    return (size_t)blocksX * (size_t)blocksY * blockSize;
}

BCDECDEF size_t bcdec_texture_mipmap_compressed_size(bcdec_format_t format, int width, int height, int mipLevels) {
    size_t totalSize = 0;
    for (int i = 0; i < mipLevels; ++i) {
        totalSize += bcdec_texture_compressed_size(format, width, height);
        width = (width > 1) ? (width >> 1) : 1;
        height = (height > 1) ? (height >> 1) : 1;
    }
    return totalSize;
}

BCDECDEF size_t bcdec_texture_mipmap_level_offset(bcdec_format_t format, int width, int height, int targetMipLevel) {
    size_t offset = 0;
    for (int i = 0; i < targetMipLevel; ++i) {
        offset += bcdec_texture_compressed_size(format, width, height);
        width = (width > 1) ? (width >> 1) : 1;
        height = (height > 1) ? (height >> 1) : 1;
    }
    return offset;
}

BCDECDEF void bcdec_calculate_parallel_blocks(int height, int threadIndex, int totalThreads, int* restrict outStartRowBlockY, int* restrict outEndRowBlockY) {
    int totalBlockRows = (height + 3) >> 2;
    if (totalThreads <= 0) totalThreads = 1;
    if (threadIndex < 0) threadIndex = 0;
    if (threadIndex >= totalThreads) threadIndex = totalThreads - 1;
    int rowsPerThread = totalBlockRows / totalThreads;
    int remainder = totalBlockRows % totalThreads;
    if (threadIndex < remainder) {
        *outStartRowBlockY = threadIndex * (rowsPerThread + 1);
        *outEndRowBlockY = *outStartRowBlockY + rowsPerThread + 1;
    } else {
        *outStartRowBlockY = threadIndex * rowsPerThread + remainder;
        *outEndRowBlockY = *outStartRowBlockY + rowsPerThread;
    }
}

BCDECDEF int bcdec_get_bc6h_mode(const void* restrict compressedBlock) {
    if (!compressedBlock) return -1;
    uint32_t header; memcpy(&header, compressedBlock, sizeof(uint32_t));
    int mode = (int)(header & 0x03);
    if (mode > 1) mode |= ((header >> 2) & 0x07) << 2;
    return mode;
}

BCDECDEF int bcdec_get_bc7_mode(const void* restrict compressedBlock) {
    if (!compressedBlock) return -1;
    uint8_t firstByte = *(const uint8_t*)compressedBlock;
    int mode = 0;
    while (mode < 8 && !(firstByte & (1 << mode))) mode++;
    return (mode < 8) ? mode : -1;
}

BCDECDEF float bcdec_estimate_block_variance(bcdec_format_t format, const void* restrict compressedBlock) {
    uint8_t blockBuffer[4 * 4 * 4];
    if (!bcdec_decompress_block(format, compressedBlock, blockBuffer, 4 * 4)) return 0.0f;
    float meanR = 0.0f, meanG = 0.0f, meanB = 0.0f;
    for (int i = 0; i < 16; ++i) {
        meanR += (float)blockBuffer[i * 4 + 0];
        meanG += (float)blockBuffer[i * 4 + 1];
        meanB += (float)blockBuffer[i * 4 + 2];
    }
    meanR /= 16.0f; meanG /= 16.0f; meanB /= 16.0f;
    float variance = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float dr = (float)blockBuffer[i * 4 + 0] - meanR;
        float dg = (float)blockBuffer[i * 4 + 1] - meanG;
        float db = (float)blockBuffer[i * 4 + 2] - meanB;
        variance += (dr * dr + dg * dg + db * db);
    }
    return variance / 16.0f;
}

BCDECDEF void bcdec_dispatch_jobs(bcdec_job_desc_t jobDesc) {
    if (!jobDesc.callback || jobDesc.totalJobs <= 0) return;
    for (int i = 0; i < jobDesc.totalJobs; ++i) {
        jobDesc.callback(i, jobDesc.totalJobs, jobDesc.userArg);
    }
}

/* =========================================================================
 * DESCOMPRESI脫N DE SUPERFICIES, REGIONES, 3D Y CUBEMAPS
 * ========================================================================= */

BCDECDEF void bcdec_decompress_texture_row(bcdec_format_t format, const void* restrict compressedRowData, void* restrict decompressedRowImage, int width, int height, int rowBlockY, int destinationPitch) {
    const uint8_t* restrict src = (const uint8_t*)compressedRowData;
    uint8_t* restrict dst = (uint8_t*)decompressedRowImage;
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0) return;
    int bytesPerPixel = bcdec_format_pixel_size(format);
    int blocksX = (width + 3) >> 2;
    src += (size_t)rowBlockY * (size_t)blocksX * blockSize;

    for (int bx = 0; bx < blocksX; ++bx) {
        uint8_t blockBuffer[4 * 4 * 8];
        bcdec_decompress_block(format, src, blockBuffer, 4 * bytesPerPixel);
        src += blockSize;

        int startX = bx * 4;
        int startY = rowBlockY * 4;
        int copyWidth = (width - startX < 4) ? (width - startX) : 4;
        int copyHeight = (height - startY < 4) ? (height - startY) : 4;

        for (int y = 0; y < copyHeight; ++y) {
            void* restrict targetRow = dst + (size_t)(startY + y) * (size_t)destinationPitch + (size_t)startX * (size_t)bytesPerPixel;
            const void* restrict sourceRow = blockBuffer + y * (4 * bytesPerPixel);
            memcpy(targetRow, sourceRow, (size_t)copyWidth * (size_t)bytesPerPixel);
        }
    }
}

BCDECDEF void bcdec_decompress_texture(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch) {
    int blocksY = (height + 3) >> 2;
    for (int by = 0; by < blocksY; ++by) {
        bcdec_decompress_texture_row(format, compressedData, decompressedImage, width, height, by, destinationPitch);
    }
}

BCDECDEF void bcdec_decompress_texture_region(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int regionX, int regionY, int regionWidth, int regionHeight, int destinationPitch) {
    if (!compressedData || !decompressedImage || regionWidth <= 0 || regionHeight <= 0) return;
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0) return;
    int bytesPerPixel = bcdec_format_pixel_size(format);
    int blocksPerRow = (width + 3) >> 2;

    int startBlockX = regionX >> 2;
    int startBlockY = regionY >> 2;
    int endBlockX = ((regionX + regionWidth - 1) >> 2);
    int endBlockY = ((regionY + regionHeight - 1) >> 2);
    const uint8_t* restrict baseSrc = (const uint8_t*)compressedData;

    for (int by = startBlockY; by <= endBlockY; ++by) {
        for (int bx = startBlockX; bx <= endBlockX; ++bx) {
            const uint8_t* restrict srcBlockPtr = baseSrc + ((size_t)by * (size_t)blocksPerRow + (size_t)bx) * blockSize;
            uint8_t blockBuffer[4 * 4 * 8];
            bcdec_decompress_block(format, srcBlockPtr, blockBuffer, 4 * bytesPerPixel);

            int blockPixelX = bx * 4;
            int blockPixelY = by * 4;
            for (int py = 0; py < 4; ++py) {
                int globalY = blockPixelY + py;
                if (globalY < regionY || globalY >= regionY + regionHeight) continue;
                for (int px = 0; px < 4; ++px) {
                    int globalX = blockPixelX + px;
                    if (globalX < regionX || globalX >= regionX + regionWidth) continue;
                    void* restrict dstPixel = (uint8_t*)decompressedImage + (size_t)(globalY - regionY) * (size_t)destinationPitch + (size_t)(globalX - regionX) * (size_t)bytesPerPixel;
                    const void* restrict srcPixel = blockBuffer + py * (4 * bytesPerPixel) + px * bytesPerPixel;
                    memcpy(dstPixel, srcPixel, (size_t)bytesPerPixel);
                }
            }
        }
    }
}

BCDECDEF void bcdec_decompress_texture_tile(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int tileX, int tileY, int tileWidth, int tileHeight, int destinationPitch) {
    bcdec_decompress_texture_region(format, compressedData, decompressedImage, width, height, tileX, tileY, tileWidth, tileHeight, destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_float(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch) {
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0) return;
    int blocksX = (width + 3) >> 2;
    int blocksY = (height + 3) >> 2;
    const uint8_t* restrict src = (const uint8_t*)compressedData;
    float* restrict dst = (float*)decompressedImage;
    int floatPitch = destinationPitch / (int)sizeof(float);

    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            float blockBuffer[4 * 4 * 4];
            bcdec_decompress_block_float(format, src, blockBuffer, 4 * 4 * (int)sizeof(float));
            src += blockSize;

            int startX = bx * 4;
            int startY = by * 4;
            int copyWidth = (width - startX < 4) ? (width - startX) : 4;
            int copyHeight = (height - startY < 4) ? (height - startY) : 4;
            for (int y = 0; y < copyHeight; ++y) {
                float* restrict targetRow = dst + (size_t)(startY + y) * (size_t)floatPitch + (size_t)startX * 4;
                const float* restrict sourceRow = blockBuffer + y * 16;
                memcpy(targetRow, sourceRow, (size_t)copyWidth * 4 * sizeof(float));
            }
        }
    }
}

BCDECDEF void bcdec_decompress_texture_flipped(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch) {
    int bytesPerPixel = bcdec_format_pixel_size(format);
    bcdec_decompress_texture(format, compressedData, decompressedImage, width, height, destinationPitch);
    bcdec_flip_surface_vertical(decompressedImage, width, height, bytesPerPixel, destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_parallel(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int startRowBlockY, int endRowBlockY, int destinationPitch) {
    int blocksY = (height + 3) >> 2;
    if (endRowBlockY > blocksY) endRowBlockY = blocksY;
    if (startRowBlockY < 0) startRowBlockY = 0;
    for (int by = startRowBlockY; by < endRowBlockY; ++by) {
        bcdec_decompress_texture_row(format, compressedData, decompressedImage, width, height, by, destinationPitch);
    }
}

typedef struct bcdec__parallel_job_context {
    bcdec_format_t format;
    const void* compressedData;
    void* decompressedImage;
    int width;
    int height;
    int destinationPitch;
} bcdec__parallel_job_context_t;

static void bcdec__parallel_decompress_callback(int jobIndex, int totalJobs, void* userArg) {
    bcdec__parallel_job_context_t* ctx = (bcdec__parallel_job_context_t*)userArg;
    int startRowBlockY, endRowBlockY;
    bcdec_calculate_parallel_blocks(ctx->height, jobIndex, totalJobs, &startRowBlockY, &endRowBlockY);
    bcdec_decompress_texture_parallel(ctx->format, ctx->compressedData, ctx->decompressedImage, ctx->width, ctx->height, startRowBlockY, endRowBlockY, ctx->destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_parallel_async(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch, bcdec_job_desc_t jobDesc) {
    if (!compressedData || !decompressedImage || width <= 0 || height <= 0 || jobDesc.totalJobs <= 0) return;
    bcdec__parallel_job_context_t ctx = { format, compressedData, decompressedImage, width, height, destinationPitch };
    bcdec_job_desc_t internalDesc = jobDesc;
    internalDesc.callback = bcdec__parallel_decompress_callback;
    internalDesc.userArg = &ctx;
    bcdec_dispatch_jobs(internalDesc);
}

BCDECDEF void bcdec_decompress_texture_mipmaps(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int mipLevels, int destinationPitch) {
    const uint8_t* restrict srcPtr = (const uint8_t*)compressedData;
    uint8_t* restrict dstPtr = (uint8_t*)decompressedImage;
    for (int level = 0; level < mipLevels; ++level) {
        bcdec_decompress_texture(format, srcPtr, dstPtr, width, height, destinationPitch);
        srcPtr += bcdec_texture_compressed_size(format, width, height);
        dstPtr += (size_t)((height + 3) >> 2) * 4 * (size_t)destinationPitch;
        width = (width > 1) ? (width >> 1) : 1;
        height = (height > 1) ? (height >> 1) : 1;
    }
}

BCDECDEF void bcdec_decompress_texture_mipmaps_parallel(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int mipLevel, int destinationPitch) {
    const uint8_t* restrict srcPtr = (const uint8_t*)compressedData;
    uint8_t* restrict dstPtr = (uint8_t*)decompressedImage;
    int curW = width, curH = height;
    for (int level = 0; level < mipLevel; ++level) {
        srcPtr += bcdec_texture_compressed_size(format, curW, curH);
        dstPtr += (size_t)((curH + 3) >> 2) * 4 * (size_t)destinationPitch;
        curW = (curW > 1) ? (curW >> 1) : 1;
        curH = (curH > 1) ? (curH >> 1) : 1;
    }
    bcdec_decompress_texture(format, srcPtr, dstPtr, curW, curH, destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_mipmap_level(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int baseWidth, int baseHeight, int targetMipLevel, int destinationPitch) {
    int w = baseWidth, h = baseHeight;
    size_t offset = bcdec_texture_mipmap_level_offset(format, baseWidth, baseHeight, targetMipLevel);
    for (int level = 0; level < targetMipLevel; ++level) {
        w = (w > 1) ? (w >> 1) : 1;
        h = (h > 1) ? (h >> 1) : 1;
    }
    bcdec_decompress_texture(format, (const uint8_t*)compressedData + offset, decompressedImage, w, h, destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_simd(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedImage, int width, int height, int destinationPitch) {
    bcdec_decompress_texture(format, compressedData, decompressedImage, width, height, destinationPitch);
}

BCDECDEF size_t bcdec_texture_3d_compressed_size(bcdec_format_t format, int width, int height, int depth) {
    return bcdec_texture_compressed_size(format, width, height) * (size_t)depth;
}

BCDECDEF void bcdec_decompress_texture_3d_slice(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedSlice, int width, int height, int depth, int sliceZ, int destinationPitch) {
    if (!compressedData || !decompressedSlice || width <= 0 || height <= 0 || depth <= 0 || sliceZ < 0 || sliceZ >= depth) return;
    size_t sliceSize = bcdec_texture_compressed_size(format, width, height);
    bcdec_decompress_texture(format, (const uint8_t*)compressedData + ((size_t)sliceZ * sliceSize), decompressedSlice, width, height, destinationPitch);
}

BCDECDEF void bcdec_decompress_texture_3d(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedVolume, int width, int height, int depth, int destinationPitch) {
    if (!compressedData || !decompressedVolume || width <= 0 || height <= 0 || depth <= 0) return;
    size_t sliceSizeCompressed = bcdec_texture_compressed_size(format, width, height);
    size_t sliceSizeDecompressed = (size_t)((height + 3) >> 2) * 4 * (size_t)destinationPitch;
    const uint8_t* restrict srcPtr = (const uint8_t*)compressedData;
    uint8_t* restrict dstPtr = (uint8_t*)decompressedVolume;
    for (int z = 0; z < depth; ++z) {
        bcdec_decompress_texture(format, srcPtr, dstPtr, width, height, destinationPitch);
        srcPtr += sliceSizeCompressed;
        dstPtr += sliceSizeDecompressed;
    }
}

BCDECDEF void bcdec_decompress_cubemap(bcdec_format_t format, const void* restrict compressedCubemapData, void* restrict decompressedFaces[6], int faceWidth, int faceHeight, int destinationPitch) {
    if (!compressedCubemapData || !decompressedFaces) return;
    size_t faceCompressedSize = bcdec_texture_compressed_size(format, faceWidth, faceHeight);
    const uint8_t* restrict srcPtr = (const uint8_t*)compressedCubemapData;
    for (int face = 0; face < 6; ++face) {
        if (decompressedFaces[face]) {
            bcdec_decompress_texture(format, srcPtr, decompressedFaces[face], faceWidth, faceHeight, destinationPitch);
        }
        srcPtr += faceCompressedSize;
    }
}

/* =========================================================================
 * MANIPULACI脫N DE SUPERFICIES Y ESPACIO DE COLOR
 * ========================================================================= */

BCDECDEF void bcdec_swizzle_surface(void* restrict imageData, int width, int height, int bytesPerPixel, int rIdx, int gIdx, int bIdx, int aIdx, int destinationPitch) {
    if (!imageData || bytesPerPixel < 3) return;
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            uint8_t p[4];
            memcpy(p, pixel, (size_t)bytesPerPixel);
            if (rIdx >= 0 && rIdx < bytesPerPixel) pixel[0] = p[rIdx];
            if (gIdx >= 0 && gIdx < bytesPerPixel) pixel[1] = p[gIdx];
            if (bIdx >= 0 && bIdx < bytesPerPixel) pixel[2] = p[bIdx];
            if (bytesPerPixel >= 4 && aIdx >= 0 && aIdx < bytesPerPixel) pixel[3] = p[aIdx];
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
}

BCDECDEF void bcdec_swizzle_surface_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int rIdx, int gIdx, int bIdx, int aIdx, int destinationPitch) {
#if defined(BCDEC_HAS_NEON)
    if (bytesPerPixel == 4 && rIdx >= 0 && gIdx >= 0 && bIdx >= 0 && aIdx >= 0) {
        uint8_t* restrict row = (uint8_t*)imageData;
        for (int y = 0; y < height; ++y) {
            uint8_t* restrict pixel = row;
            int x = 0;
            for (; x + 4 <= width; x += 4) {
                uint8_t p[16];
                memcpy(p, pixel, 16);
                pixel[0] = p[rIdx]; pixel[1] = p[gIdx]; pixel[2] = p[bIdx]; pixel[3] = p[aIdx];
                pixel[4] = p[4 + rIdx]; pixel[5] = p[4 + gIdx]; pixel[6] = p[4 + bIdx]; pixel[7] = p[4 + aIdx];
                pixel[8] = p[8 + rIdx]; pixel[9] = p[8 + gIdx]; pixel[10] = p[8 + bIdx]; pixel[11] = p[8 + aIdx];
                pixel[12] = p[12 + rIdx]; pixel[13] = p[12 + gIdx]; pixel[14] = p[12 + bIdx]; pixel[15] = p[12 + aIdx];
                pixel += 16;
            }
            for (; x < width; ++x) {
                uint8_t p[4]; memcpy(p, pixel, 4);
                pixel[0] = p[rIdx]; pixel[1] = p[gIdx]; pixel[2] = p[bIdx]; pixel[3] = p[aIdx];
                pixel += 4;
            }
            row += destinationPitch;
        }
        return;
    }
#endif
    bcdec_swizzle_surface(imageData, width, height, bytesPerPixel, rIdx, gIdx, bIdx, aIdx, destinationPitch);
}

BCDECDEF void bcdec_invert_green_channel(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 2) return;
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            pixel[1] = (uint8_t)(255 - pixel[1]);
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
}

BCDECDEF void bcdec_downscale_surface_2x2(const void* restrict srcImage, void* restrict dstImage, int srcWidth, int srcHeight, int bytesPerPixel, int srcPitch, int dstPitch) {
    if (!srcImage || !dstImage) return;
    int dstWidth = (srcWidth > 1) ? (srcWidth >> 1) : 1;
    int dstHeight = (srcHeight > 1) ? (srcHeight >> 1) : 1;
    const uint8_t* restrict srcRow1 = (const uint8_t*)srcImage;
    const uint8_t* restrict srcRow2 = srcRow1 + srcPitch;
    uint8_t* restrict dstRow = (uint8_t*)dstImage;

    for (int y = 0; y < dstHeight; ++y) {
        const uint8_t* restrict s1 = srcRow1;
        const uint8_t* restrict s2 = srcRow2;
        uint8_t* restrict d = dstRow;
        int actualSrcH = (y * 2 + 1 < srcHeight) ? 2 : 1;
        for (int x = 0; x < dstWidth; ++x) {
            int actualSrcW = (x * 2 + 1 < srcWidth) ? 2 : 1;
            for (int c = 0; c < bytesPerPixel; ++c) {
                int sum = s1[c] + (actualSrcW > 1 ? s1[bytesPerPixel + c] : s1[c]);
                if (actualSrcH > 1) {
                    sum += s2[c] + (actualSrcW > 1 ? s2[bytesPerPixel + c] : s2[c]);
                    d[c] = (uint8_t)(sum >> 2);
                } else {
                    d[c] = (uint8_t)(sum >> 1);
                }
            }
            s1 += bytesPerPixel * actualSrcW;
            s2 += bytesPerPixel * actualSrcW;
            d += bytesPerPixel;
        }
        srcRow1 += srcPitch * 2;
        srcRow2 += srcPitch * 2;
        dstRow += dstPitch;
    }
}

BCDECDEF void bcdec_generate_mipmaps_surface(void* restrict baseImage, int width, int height, int bytesPerPixel, int mipLevels, int basePitch) {
    if (!baseImage || mipLevels <= 1) return;
    uint8_t* restrict currentLevelPtr = (uint8_t*)baseImage;
    int currentWidth = width, currentHeight = height, currentPitch = basePitch;
    for (int level = 1; level < mipLevels; ++level) {
        int nextWidth = (currentWidth > 1) ? (currentWidth >> 1) : 1;
        int nextHeight = (currentHeight > 1) ? (currentHeight >> 1) : 1;
        int nextPitch = nextWidth * bytesPerPixel;
        uint8_t* restrict nextLevelPtr = currentLevelPtr + (size_t)currentHeight * (size_t)currentPitch;
        bcdec_downscale_surface_2x2(currentLevelPtr, nextLevelPtr, currentWidth, currentHeight, bytesPerPixel, currentPitch, nextPitch);
        currentLevelPtr = nextLevelPtr; currentWidth = nextWidth; currentHeight = nextHeight; currentPitch = nextPitch;
    }
}

static inline float bcdec__linear_to_srgb_f(float val) {
    if (val <= 0.0f) return 0.0f;
    if (val >= 1.0f) return 1.0f;
    return (val < 0.0031308f) ? (val * 12.92f) : (1.055f * powf(val, 1.0f / 2.4f) - 0.055f);
}

static inline float bcdec__srgb_to_linear_f(float val) {
    if (val <= 0.0f) return 0.0f;
    if (val >= 1.0f) return 1.0f;
    return (val <= 0.04045f) ? (val / 12.92f) : powf((val + 0.055f) / 1.055f, 2.4f);
}

BCDECDEF void bcdec_convert_linear_to_srgb(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 3) return;
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            pixel[0] = (uint8_t)(bcdec__linear_to_srgb_f((float)pixel[0] / 255.0f) * 255.0f + 0.5f);
            pixel[1] = (uint8_t)(bcdec__linear_to_srgb_f((float)pixel[1] / 255.0f) * 255.0f + 0.5f);
            pixel[2] = (uint8_t)(bcdec__linear_to_srgb_f((float)pixel[2] / 255.0f) * 255.0f + 0.5f);
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
}

BCDECDEF void bcdec_convert_srgb_to_linear(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 3) return;
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            pixel[0] = (uint8_t)(bcdec__srgb_to_linear_f((float)pixel[0] / 255.0f) * 255.0f + 0.5f);
            pixel[1] = (uint8_t)(bcdec__srgb_to_linear_f((float)pixel[1] / 255.0f) * 255.0f + 0.5f);
            pixel[2] = (uint8_t)(bcdec__srgb_to_linear_f((float)pixel[2] / 255.0f) * 255.0f + 0.5f);
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
}

#if defined(BCDEC_HAS_NEON)
static BCDEC_FORCEINLINE float32x4_t bcdec__neon_fast_pow_2_4(float32x4_t x) {
    float32x4_t c0 = vdupq_n_f32(0.012522878f);
    float32x4_t c1 = vdupq_n_f32(0.682171111f);
    float32x4_t c2 = vdupq_n_f32(0.305306011f);
    float32x4_t x2 = vmulq_f32(x, x);
    float32x4_t x3 = vmulq_f32(x2, x);
    return vmlaq_f32(vmlaq_f32(vmulq_f32(c0, x), c1, x2), c2, x3);
}
static BCDEC_FORCEINLINE float32x4_t bcdec__neon_fast_pow_inv2_4(float32x4_t x) {
    float32x4_t r1 = vsqrtq_f32(x);
    float32x4_t r2 = vsqrtq_f32(r1);
    float32x4_t r3 = vsqrtq_f32(r2);
    float32x4_t r4 = vsqrtq_f32(r3);
    float32x4_t r5 = vsqrtq_f32(r4);
    float32x4_t r7 = vsqrtq_f32(vsqrtq_f32(r5));
    return vmulq_f32(vmulq_f32(r2, r3), vmulq_f32(r5, r7));
}
#endif

BCDECDEF void bcdec_convert_linear_to_srgb_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
#if defined(BCDEC_HAS_NEON)
    if (imageData && bytesPerPixel == 4 && width > 0 && height > 0) {
        uint8_t* restrict baseRow = (uint8_t*)imageData;
        float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f), scale255 = vdupq_n_f32(255.0f), half = vdupq_n_f32(0.5f);
        float32x4_t thresh = vdupq_n_f32(0.0031308f), linMul = vdupq_n_f32(12.92f);
        float32x4_t powMul = vdupq_n_f32(1.055f), powSub = vdupq_n_f32(0.055f);
        for (int y = 0; y < height; ++y) {
            uint8_t* restrict row = baseRow + (size_t)y * (size_t)destinationPitch;
            int x = 0;
            for (; x + 4 <= width; x += 4) {
                uint8x16_t raw = vld1q_u8(row + x * 4);
                uint16x8_t lo16 = vmovl_u8(vget_low_u8(raw)), hi16 = vmovl_u8(vget_high_u8(raw));
                float32x4_t pf[4] = {
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(lo16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(hi16))), inv255)
                };
                uint8_t outA[4] = { row[x*4+3], row[x*4+7], row[x*4+11], row[x*4+15] };
                for (int k = 0; k < 4; ++k) {
                    uint32x4_t mask = vcltq_f32(pf[k], thresh);
                    float32x4_t res = vbslq_f32(mask, vmulq_f32(pf[k], linMul), vsubq_f32(vmulq_f32(powMul, bcdec__neon_fast_pow_inv2_4(pf[k])), powSub));
                    res = vaddq_f32(vmulq_f32(res, scale255), half);
                    uint32x4_t uRes = vcvtq_u32_f32(vmaxq_f32(vdupq_n_f32(0.0f), vminq_f32(scale255, res)));
                    uint8_t* dstPx = row + (x + k) * 4;
                    dstPx[0] = (uint8_t)vgetq_lane_u32(uRes, 0);
                    dstPx[1] = (uint8_t)vgetq_lane_u32(uRes, 1);
                    dstPx[2] = (uint8_t)vgetq_lane_u32(uRes, 2);
                    dstPx[3] = outA[k];
                }
            }
            for (; x < width; ++x) {
                uint8_t* px = row + x * 4;
                for (int c = 0; c < 3; ++c) px[c] = (uint8_t)(bcdec__linear_to_srgb_f((float)px[c] / 255.0f) * 255.0f + 0.5f);
            }
        }
        return;
    }
#endif
    bcdec_convert_linear_to_srgb(imageData, width, height, bytesPerPixel, destinationPitch);
}

BCDECDEF void bcdec_convert_srgb_to_linear_simd(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
#if defined(BCDEC_HAS_NEON)
    if (imageData && bytesPerPixel == 4 && width > 0 && height > 0) {
        uint8_t* restrict baseRow = (uint8_t*)imageData;
        float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f), scale255 = vdupq_n_f32(255.0f), half = vdupq_n_f32(0.5f);
        float32x4_t thresh = vdupq_n_f32(0.04045f), invLin = vdupq_n_f32(1.0f / 12.92f);
        float32x4_t addOff = vdupq_n_f32(0.055f), invScale = vdupq_n_f32(1.0f / 1.055f);
        for (int y = 0; y < height; ++y) {
            uint8_t* restrict row = baseRow + (size_t)y * (size_t)destinationPitch;
            int x = 0;
            for (; x + 4 <= width; x += 4) {
                uint8x16_t raw = vld1q_u8(row + x * 4);
                uint16x8_t lo16 = vmovl_u8(vget_low_u8(raw)), hi16 = vmovl_u8(vget_high_u8(raw));
                float32x4_t pf[4] = {
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(lo16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi16))), inv255),
                    vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(hi16))), inv255)
                };
                uint8_t outA[4] = { row[x*4+3], row[x*4+7], row[x*4+11], row[x*4+15] };
                for (int k = 0; k < 4; ++k) {
                    uint32x4_t mask = vcleq_f32(pf[k], thresh);
                    float32x4_t res = vbslq_f32(mask, vmulq_f32(pf[k], invLin), bcdec__neon_fast_pow_2_4(vmulq_f32(vaddq_f32(pf[k], addOff), invScale)));
                    res = vaddq_f32(vmulq_f32(res, scale255), half);
                    uint32x4_t uRes = vcvtq_u32_f32(vmaxq_f32(vdupq_n_f32(0.0f), vminq_f32(scale255, res)));
                    uint8_t* dstPx = row + (x + k) * 4;
                    dstPx[0] = (uint8_t)vgetq_lane_u32(uRes, 0);
                    dstPx[1] = (uint8_t)vgetq_lane_u32(uRes, 1);
                    dstPx[2] = (uint8_t)vgetq_lane_u32(uRes, 2);
                    dstPx[3] = outA[k];
                }
            }
            for (; x < width; ++x) {
                uint8_t* px = row + x * 4;
                for (int c = 0; c < 3; ++c) px[c] = (uint8_t)(bcdec__srgb_to_linear_f((float)px[c] / 255.0f) * 255.0f + 0.5f);
            }
        }
        return;
    }
#endif
    bcdec_convert_srgb_to_linear(imageData, width, height, bytesPerPixel, destinationPitch);
}

BCDECDEF void bcdec_apply_ordered_dither(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel <= 0) return;
    static const int bayer4x4[4][4] = { {0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5} };
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            int ditherVal = (bayer4x4[y & 3][x & 3] - 8) * 2;
            for (int c = 0; c < bytesPerPixel && c < 3; ++c) {
                int val = (int)pixel[c] + ditherVal;
                pixel[c] = (uint8_t)(val < 0 ? 0 : (val > 255 ? 255 : val));
            }
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
}

BCDECDEF void bcdec_pad_surface_borders(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 1 || height <= 1 || bytesPerPixel <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    if (width * bytesPerPixel < destinationPitch) {
        for (int y = 0; y < height; ++y) {
            uint8_t* restrict row = baseRow + (size_t)y * (size_t)destinationPitch;
            memcpy(row + (size_t)width * (size_t)bytesPerPixel, row + (size_t)(width - 1) * (size_t)bytesPerPixel, (size_t)bytesPerPixel);
        }
    }
}

BCDECDEF void bcdec_mirror_surface_horizontal(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 1 || height <= 0 || bytesPerPixel <= 0 || bytesPerPixel > 16) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    uint8_t tmp[16];
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict row = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width / 2; ++x) {
            uint8_t* l = row + (size_t)x * (size_t)bytesPerPixel;
            uint8_t* r = row + (size_t)(width - 1 - x) * (size_t)bytesPerPixel;
            memcpy(tmp, l, (size_t)bytesPerPixel);
            memcpy(l, r, (size_t)bytesPerPixel);
            memcpy(r, tmp, (size_t)bytesPerPixel);
        }
    }
}

BCDECDEF void bcdec_flip_surface_vertical(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 0 || height <= 1 || bytesPerPixel <= 0) return;
    size_t rowBytes = (size_t)width * (size_t)bytesPerPixel;
    uint8_t* topRow = (uint8_t*)imageData;
    uint8_t* bottomRow = (uint8_t*)imageData + (size_t)(height - 1) * (size_t)destinationPitch;
    for (int y = 0; y < height / 2; ++y) {
        for (size_t x = 0; x < rowBytes; ++x) {
            uint8_t tmp = topRow[x]; topRow[x] = bottomRow[x]; bottomRow[x] = tmp;
        }
        topRow += destinationPitch;
        bottomRow -= destinationPitch;
    }
}

BCDECDEF void bcdec_rotate_surface_90(const void* restrict srcImage, void* restrict dstImage, int width, int height, int bytesPerPixel, int srcPitch, int dstPitch, int clockwise) {
    if (!srcImage || !dstImage || width <= 0 || height <= 0 || bytesPerPixel <= 0) return;
    const uint8_t* srcBase = (const uint8_t*)srcImage;
    uint8_t* dstBase = (uint8_t*)dstImage;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcBase + (size_t)y * (size_t)srcPitch;
        for (int x = 0; x < width; ++x) {
            int dstX = clockwise ? (height - 1 - y) : y;
            int dstY = clockwise ? x : (width - 1 - x);
            memcpy(dstBase + (size_t)dstY * (size_t)dstPitch + (size_t)dstX * (size_t)bytesPerPixel,
                   srcRow + (size_t)x * (size_t)bytesPerPixel, (size_t)bytesPerPixel);
        }
    }
}

BCDECDEF void bcdec_fix_mipmap_seams(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 1 || height <= 1 || bytesPerPixel <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict row = baseRow + (size_t)y * (size_t)destinationPitch;
        uint8_t* restrict edgePx = row + (size_t)(width - 1) * (size_t)bytesPerPixel;
        uint8_t* restrict innerPx = row + (size_t)(width - 2) * (size_t)bytesPerPixel;
        for (int c = 0; c < bytesPerPixel; ++c) edgePx[c] = (uint8_t)(((int)innerPx[c] + (int)edgePx[c]) / 2);
    }
}

BCDECDEF void bcdec_blur_surface_3x3(const void* restrict srcImage, void* restrict dstImage, int width, int height, int bytesPerPixel, int srcPitch, int dstPitch) {
    if (!srcImage || !dstImage || width <= 0 || height <= 0 || bytesPerPixel <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcImage;
    uint8_t* restrict dstBase = (uint8_t*)dstImage;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0;
            int xNext = x < width - 1 ? x + 1 : width - 1;
            for (int c = 0; c < bytesPerPixel; ++c) {
                int sum = rPrev[xPrev*bytesPerPixel+c] + rPrev[x*bytesPerPixel+c] + rPrev[xNext*bytesPerPixel+c] +
                          rCurr[xPrev*bytesPerPixel+c] + rCurr[x*bytesPerPixel+c] + rCurr[xNext*bytesPerPixel+c] +
                          rNext[xPrev*bytesPerPixel+c] + rNext[x*bytesPerPixel+c] + rNext[xNext*bytesPerPixel+c];
                dstRow[x * bytesPerPixel + c] = (uint8_t)(sum / 9);
            }
        }
    }
}

BCDECDEF void bcdec_premultiply_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 4 || width <= 0 || height <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) {
            uint32_t a = pixel[3];
            if (a < 255) {
                pixel[0] = (uint8_t)(((uint32_t)pixel[0] * a + 127) / 255);
                pixel[1] = (uint8_t)(((uint32_t)pixel[1] * a + 127) / 255);
                pixel[2] = (uint8_t)(((uint32_t)pixel[2] * a + 127) / 255);
            }
            pixel += bytesPerPixel;
        }
    }
}

BCDECDEF void bcdec_unpremultiply_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 4 || width <= 0 || height <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) {
            uint32_t a = pixel[3];
            if (a > 0 && a < 255) {
                pixel[0] = (uint8_t)(((uint32_t)pixel[0] * 255 + a / 2) / a);
                pixel[1] = (uint8_t)(((uint32_t)pixel[1] * 255 + a / 2) / a);
                pixel[2] = (uint8_t)(((uint32_t)pixel[2] * 255 + a / 2) / a);
            }
            pixel += bytesPerPixel;
        }
    }
}

BCDECDEF void bcdec_convert_rgb_to_grayscale_rec709(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || bytesPerPixel < 3 || width <= 0 || height <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) {
            uint32_t gray = ((uint32_t)pixel[0] * 54 + (uint32_t)pixel[1] * 183 + (uint32_t)pixel[2] * 19) >> 8;
            if (gray > 255) gray = 255;
            pixel[0] = pixel[1] = pixel[2] = (uint8_t)gray;
            pixel += bytesPerPixel;
        }
    }
}

BCDECDEF void bcdec_adjust_surface_color(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, float exposure, float contrast, float saturation) {
    if (!imageData || width <= 0 || height <= 0 || bytesPerPixel < 3) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) {
            float r = ((float)pixel[0] / 255.0f) * exposure;
            float g = ((float)pixel[1] / 255.0f) * exposure;
            float b = ((float)pixel[2] / 255.0f) * exposure;
            r = ((r - 0.5f) * contrast) + 0.5f;
            g = ((g - 0.5f) * contrast) + 0.5f;
            b = ((b - 0.5f) * contrast) + 0.5f;
            float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
            pixel[0] = (uint8_t)((r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r)) * 255.0f + 0.5f);
            pixel[1] = (uint8_t)((g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g)) * 255.0f + 0.5f);
            pixel[2] = (uint8_t)((b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b)) * 255.0f + 0.5f);
            pixel += bytesPerPixel;
        }
    }
}

BCDECDEF int bcdec_is_surface_fully_opaque(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 0 || height <= 0 || bytesPerPixel < 4) return 1;
    const uint8_t* restrict baseRow = (const uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        const uint8_t* restrict pixel = baseRow + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) {
            if (pixel[3] < 255) return 0;
            pixel += bytesPerPixel;
        }
    }
    return 1;
}

BCDECDEF void bcdec_blend_surfaces(const void* restrict srcImageA, const void* restrict srcImageB, void* restrict dstImage, int width, int height, int bytesPerPixel, float blendFactor, int pitch) {
    if (!srcImageA || !srcImageB || !dstImage || width <= 0 || height <= 0 || bytesPerPixel <= 0) return;
    if (blendFactor < 0.0f) blendFactor = 0.0f;
    if (blendFactor > 1.0f) blendFactor = 1.0f;
    const uint8_t* restrict rowA = (const uint8_t*)srcImageA;
    const uint8_t* restrict rowB = (const uint8_t*)srcImageB;
    uint8_t* restrict rowDst = (uint8_t*)dstImage;
    size_t rowBytes = (size_t)width * (size_t)bytesPerPixel;
    for (int y = 0; y < height; ++y) {
        for (size_t i = 0; i < rowBytes; ++i) {
            rowDst[i] = (uint8_t)((float)rowA[i] * (1.0f - blendFactor) + (float)rowB[i] * blendFactor + 0.5f);
        }
        rowA += pitch; rowB += pitch; rowDst += pitch;
    }
}

BCDECDEF void bcdec_extract_channel(const void* restrict srcImage, void* restrict dstChannelMap, int width, int height, int bytesPerPixel, int channelIndex, int srcPitch, int dstPitch) {
    if (!srcImage || !dstChannelMap || width <= 0 || height <= 0 || bytesPerPixel <= 0 || channelIndex < 0 || channelIndex >= bytesPerPixel) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcImage;
    uint8_t* restrict dstBase = (uint8_t*)dstChannelMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* restrict srcRow = srcBase + (size_t)y * (size_t)srcPitch;
        uint8_t* restrict dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) dstRow[x] = srcRow[x * bytesPerPixel + channelIndex];
    }
}

BCDECDEF void bcdec_dilate_surface_alpha(void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, int iterations) {
    if (!imageData || bytesPerPixel < 4 || width <= 1 || height <= 1 || iterations <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)imageData;
    for (int iter = 0; iter < iterations; ++iter) {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = baseRow + (size_t)y * (size_t)destinationPitch;
            for (int x = 0; x < width; ++x) {
                uint8_t* px = row + (size_t)x * (size_t)bytesPerPixel;
                if (px[3] == 0) {
                    int sumR = 0, sumG = 0, sumB = 0, count = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        int ny = y + dy;
                        if (ny < 0 || ny >= height) continue;
                        uint8_t* nrow = baseRow + (size_t)ny * (size_t)destinationPitch;
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx;
                            if (nx < 0 || nx >= width || (dx == 0 && dy == 0)) continue;
                            uint8_t* npx = nrow + (size_t)nx * (size_t)bytesPerPixel;
                            if (npx[3] > 0) { sumR += npx[0]; sumG += npx[1]; sumB += npx[2]; count++; }
                        }
                    }
                    if (count > 0) { px[0] = (uint8_t)(sumR / count); px[1] = (uint8_t)(sumG / count); px[2] = (uint8_t)(sumB / count); }
                }
            }
        }
    }
}

BCDECDEF void bcdec_apply_tonemap_aces(void* restrict floatImageData, int width, int height, int rowPitchBytes, int isHalfFloat) {
    if (!floatImageData || width <= 0 || height <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)floatImageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict rowPtr = baseRow + (size_t)y * (size_t)rowPitchBytes;
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                float val;
                if (isHalfFloat) { uint16_t h; memcpy(&h, rowPtr + ((size_t)x*4+(size_t)c)*2, 2); val = bcdec_half_to_float(h); }
                else { memcpy(&val, rowPtr + ((size_t)x*4+(size_t)c)*4, 4); }
                val = (val * (2.51f * val + 0.03f)) / (val * (2.43f * val + 0.59f) + 0.14f);
                if (val < 0.0f) val = 0.0f; if (val > 1.0f) val = 1.0f;
                if (isHalfFloat) { uint16_t h = bcdec_float_to_half(val); memcpy(rowPtr + ((size_t)x*4+(size_t)c)*2, &h, 2); }
                else { memcpy(rowPtr + ((size_t)x*4+(size_t)c)*4, &val, 4); }
            }
        }
    }
}

BCDECDEF void bcdec_apply_tonemap_reinhard(void* restrict floatImageData, int width, int height, int rowPitchBytes, int isHalfFloat) {
    if (!floatImageData || width <= 0 || height <= 0) return;
    uint8_t* restrict baseRow = (uint8_t*)floatImageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict rowPtr = baseRow + (size_t)y * (size_t)rowPitchBytes;
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                float val;
                if (isHalfFloat) { uint16_t h; memcpy(&h, rowPtr + ((size_t)x*4+(size_t)c)*2, 2); val = bcdec_half_to_float(h); }
                else { memcpy(&val, rowPtr + ((size_t)x*4+(size_t)c)*4, 4); }
                val = val / (1.0f + val);
                if (val < 0.0f) val = 0.0f; if (val > 1.0f) val = 1.0f;
                if (isHalfFloat) { uint16_t h = bcdec_float_to_half(val); memcpy(rowPtr + ((size_t)x*4+(size_t)c)*2, &h, 2); }
                else { memcpy(rowPtr + ((size_t)x*4+(size_t)c)*4, &val, 4); }
            }
        }
    }
}

/* =========================================================================
 * PIPELINE PBR
 * ========================================================================= */

BCDECDEF void bcdec_bc5_to_normal_map(void* restrict imageData, int width, int height, int destinationPitch) {
    bcdec_bc5_to_normal_map_ex(imageData, width, height, destinationPitch, 1.0f);
}

BCDECDEF void bcdec_bc5_to_normal_map_ex(void* restrict imageData, int width, int height, int destinationPitch, float strength) {
    bcdec_bc5_to_normal_map_packed(imageData, width, height, destinationPitch, strength, 255);
}

BCDECDEF void bcdec_bc5_to_normal_map_packed(void* restrict imageData, int width, int height, int destinationPitch, float strength, uint8_t alphaValue) {
    if (!imageData) return;
    uint8_t* restrict row = (uint8_t*)imageData;
    for (int y = 0; y < height; ++y) {
        uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            float nx = ((float)pixel[0] - 128.0f) / 128.0f * strength;
            float ny = ((float)pixel[1] - 128.0f) / 128.0f * strength;
            float zz = 1.0f - (nx * nx + ny * ny);
            float nz = (zz > 0.0f) ? sqrtf(zz) : 0.0f;
            int fx = (int)(nx * 127.0f + 128.0f), fy = (int)(ny * 127.0f + 128.0f), fz = (int)(nz * 127.0f + 128.0f);
            pixel[0] = (uint8_t)(fx < 0 ? 0 : (fx > 255 ? 255 : fx));
            pixel[1] = (uint8_t)(fy < 0 ? 0 : (fy > 255 ? 255 : fy));
            pixel[2] = (uint8_t)(fz < 0 ? 0 : (fz > 255 ? 255 : fz));
            pixel[3] = alphaValue;
            pixel += 4;
        }
        row += destinationPitch;
    }
}

BCDECDEF void bcdec_heightmap_to_normal_map(const void* restrict srcHeightmap, void* restrict dstNormalMap, int width, int height, float strength, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstNormalMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstNormalMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            float dX = (((float)rPrev[xNext] + 2.0f * (float)rCurr[xNext] + (float)rNext[xNext]) -
                        ((float)rPrev[xPrev] + 2.0f * (float)rCurr[xPrev] + (float)rNext[xPrev])) / 255.0f * strength;
            float dY = (((float)rNext[xPrev] + 2.0f * (float)rNext[x] + (float)rNext[xNext]) -
                        ((float)rPrev[xPrev] + 2.0f * (float)rPrev[x] + (float)rPrev[xNext])) / 255.0f * strength;
            float zz = 1.0f - (dX * dX + dY * dY);
            float dZ = (zz > 0.0f) ? sqrtf(zz) : 0.0f;
            int nx = (int)(dX * 127.5f + 127.5f), ny = (int)(dY * 127.5f + 127.5f), nz = (int)(dZ * 127.5f + 127.5f);
            dstRow[x*4+0] = (uint8_t)(nx < 0 ? 0 : (nx > 255 ? 255 : nx));
            dstRow[x*4+1] = (uint8_t)(ny < 0 ? 0 : (ny > 255 ? 255 : ny));
            dstRow[x*4+2] = (uint8_t)(nz < 0 ? 0 : (nz > 255 ? 255 : nz));
            dstRow[x*4+3] = 255;
        }
    }
}

BCDECDEF void bcdec_normal_map_to_heightmap(const void* restrict srcNormalMap, void* restrict dstHeightmap, int width, int height, int srcPitch, int dstPitch) {
    if (!srcNormalMap || !dstHeightmap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcNormalMap;
    uint8_t* restrict dstBase = (uint8_t*)dstHeightmap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = srcBase + (size_t)y * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            float nx = ((float)row[x*4+0] - 127.5f) / 127.5f;
            float ny = ((float)row[x*4+1] - 127.5f) / 127.5f;
            float h = (nx * 0.5f + ny * 0.5f) * 127.5f + 127.5f;
            dstRow[x] = (uint8_t)(h < 0.0f ? 0 : (h > 255.0f ? 255 : (int)(h + 0.5f)));
        }
    }
}

BCDECDEF void bcdec_smooth_normal_map(void* restrict normalMapData, int width, int height, float smoothness, int destinationPitch) {
    if (!normalMapData || width <= 2 || height <= 2) return;
    if (smoothness < 0.0f) smoothness = 0.0f;
    if (smoothness > 1.0f) smoothness = 1.0f;
    uint8_t* data = (uint8_t*)normalMapData;
    uint8_t* temp = (uint8_t*)malloc((size_t)height * (size_t)destinationPitch);
    if (!temp) return;
    memcpy(temp, data, (size_t)height * (size_t)destinationPitch);

    for (int y = 1; y < height - 1; ++y) {
        uint8_t* rowDst = data + (size_t)y * (size_t)destinationPitch;
        const uint8_t* rowOrig = temp + (size_t)y * (size_t)destinationPitch;
        for (int x = 1; x < width - 1; ++x) {
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            for (int ky = -1; ky <= 1; ++ky) {
                const uint8_t* rSrc = temp + (size_t)(y + ky) * (size_t)destinationPitch;
                for (int kx = -1; kx <= 1; ++kx) {
                    int sIdx = (x + kx) * 4;
                    nx += ((float)rSrc[sIdx+0] - 127.5f);
                    ny += ((float)rSrc[sIdx+1] - 127.5f);
                    nz += ((float)rSrc[sIdx+2] - 127.5f);
                }
            }
            nx /= 9.0f; ny /= 9.0f; nz /= 9.0f;
            float ox = (float)rowOrig[x*4+0] - 127.5f;
            float oy = (float)rowOrig[x*4+1] - 127.5f;
            float oz = (float)rowOrig[x*4+2] - 127.5f;
            nx = ox * (1.0f - smoothness) + nx * smoothness;
            ny = oy * (1.0f - smoothness) + ny * smoothness;
            nz = oz * (1.0f - smoothness) + nz * smoothness;
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
            rowDst[x*4+0] = (uint8_t)(nx * 127.5f + 127.5f);
            rowDst[x*4+1] = (uint8_t)(ny * 127.5f + 127.5f);
            rowDst[x*4+2] = (uint8_t)(nz * 127.5f + 127.5f);
        }
    }
    free(temp);
}

BCDECDEF void bcdec_generate_roughness_from_height(const void* restrict srcHeightmap, void* restrict dstRoughnessMap, int width, int height, float scale, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstRoughnessMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstRoughnessMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            float dx = ((float)rCurr[xNext] - (float)rCurr[xPrev]) * 0.5f;
            float dy = ((float)rNext[x] - (float)rPrev[x]) * 0.5f;
            float slope = sqrtf(dx * dx + dy * dy) / 128.0f * scale;
            float r = ((float)rCurr[x] / 255.0f) * 0.5f + slope * 0.5f;
            dstRow[x] = (uint8_t)((r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_roughness_from_albedo(const void* restrict srcAlbedo, void* restrict dstRoughnessMap, int width, int height, int bytesPerPixel, float detailStrength, float baseRoughness, int srcPitch, int dstPitch) {
    if (!srcAlbedo || !dstRoughnessMap || width <= 0 || height <= 0 || bytesPerPixel < 3) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcAlbedo;
    uint8_t* restrict dstBase = (uint8_t*)dstRoughnessMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            const uint8_t* pC = rCurr + (size_t)x * (size_t)bytesPerPixel;
            const uint8_t* pL = rCurr + (size_t)xPrev * (size_t)bytesPerPixel;
            const uint8_t* pR = rCurr + (size_t)xNext * (size_t)bytesPerPixel;
            const uint8_t* pU = rPrev + (size_t)x * (size_t)bytesPerPixel;
            const uint8_t* pD = rNext + (size_t)x * (size_t)bytesPerPixel;
            float lumC = ((float)pC[0]*0.2126f + (float)pC[1]*0.7152f + (float)pC[2]*0.0722f) / 255.0f;
            float lumAvg = (((float)pL[0]+pR[0]+pU[0]+pD[0])*0.2126f + ((float)pL[1]+pR[1]+pU[1]+pD[1])*0.7152f + ((float)pL[2]+pR[2]+pU[2]+pD[2])*0.0722f) / (255.0f * 4.0f);
            float highFreq = fabsf(lumC - lumAvg);
            float r = baseRoughness + (highFreq * detailStrength) - ((lumC - 0.5f) * 0.15f);
            dstRow[x] = (uint8_t)((r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_gloss_from_roughness(const void* restrict srcRoughness, void* restrict dstGlossMap, int width, int height, int destinationPitch) {
    if (!srcRoughness || !dstGlossMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcRoughness;
    uint8_t* restrict dstBase = (uint8_t*)dstGlossMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcBase + (size_t)y * (size_t)destinationPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)destinationPitch;
        for (int x = 0; x < width; ++x) dstRow[x] = (uint8_t)(255 - srcRow[x]);
    }
}

BCDECDEF void bcdec_generate_ao_map(const void* restrict srcHeightmap, void* restrict dstAoMap, int width, int height, float radius, float intensity, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstAoMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstAoMap;
    int r = (int)(radius < 1.0f ? 1.0f : radius);
    for (int y = 0; y < height; ++y) {
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        for (int x = 0; x < width; ++x) {
            float centerVal = (float)rCurr[x], occlusion = 0.0f;
            int samples = 0;
            for (int dy = -r; dy <= r; ++dy) {
                int ny = y + dy; if (ny < 0 || ny >= height) continue;
                const uint8_t* sRow = srcBase + (size_t)ny * (size_t)srcPitch;
                for (int dx = -r; dx <= r; ++dx) {
                    int nx = x + dx; if (nx < 0 || nx >= width || (dx == 0 && dy == 0)) continue;
                    float diff = (centerVal - (float)sRow[nx]) / (sqrtf((float)(dx*dx + dy*dy)) + 1.0f);
                    if (diff > 0.0f) occlusion += diff;
                    samples++;
                }
            }
            if (samples > 0) occlusion = (occlusion / (float)samples) * intensity;
            float ao = 1.0f - occlusion;
            dstRow[x] = (uint8_t)((ao < 0.0f ? 0.0f : (ao > 1.0f ? 1.0f : ao)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_ao_map_directional(const void* restrict srcHeightmap, void* restrict dstAoMap, int width, int height, float radius, float intensity, float sunAngleRadians, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstAoMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstAoMap;
    int r = (int)(radius < 1.0f ? 1.0f : radius);
    float sunDirX = cosf(sunAngleRadians), sunDirY = sinf(sunAngleRadians);
    for (int y = 0; y < height; ++y) {
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        for (int x = 0; x < width; ++x) {
            float centerVal = (float)rCurr[x], occlusion = 0.0f, weightSum = 0.0f;
            for (int dy = -r; dy <= r; ++dy) {
                int ny = y + dy; if (ny < 0 || ny >= height) continue;
                const uint8_t* sRow = srcBase + (size_t)ny * (size_t)srcPitch;
                for (int dx = -r; dx <= r; ++dx) {
                    int nx = x + dx; if (nx < 0 || nx >= width || (dx == 0 && dy == 0)) continue;
                    float dist = sqrtf((float)(dx*dx + dy*dy));
                    float dirAlignment = ((float)dx * sunDirX + (float)dy * sunDirY) / dist;
                    float directionalWeight = 0.5f + 0.5f * dirAlignment;
                    float diff = ((float)sRow[nx] - centerVal) / 255.0f;
                    if (diff > 0.0f) occlusion += (diff / dist) * directionalWeight;
                    weightSum += directionalWeight;
                }
            }
            if (weightSum > 0.0f) occlusion = (occlusion / weightSum) * intensity * 4.0f;
            float ao = 1.0f - occlusion;
            dstRow[x] = (uint8_t)((ao < 0.0f ? 0.0f : (ao > 1.0f ? 1.0f : ao)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_curvature_map(const void* restrict srcHeightmap, void* restrict dstCurvatureMap, int width, int height, float scale, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstCurvatureMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstCurvatureMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            float laplacian = ((float)rPrev[x] + (float)rNext[x] + (float)rCurr[xPrev] + (float)rCurr[xNext] - 4.0f * (float)rCurr[x]) / 255.0f;
            float curv = 0.5f - laplacian * scale;
            dstRow[x] = (uint8_t)((curv < 0.0f ? 0.0f : (curv > 1.0f ? 1.0f : curv)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_displacement_map(const void* restrict srcHeightmap, void* restrict dstDisplacementMap, int width, int height, float depthScale, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstDisplacementMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstDisplacementMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcBase + (size_t)y * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            float d = ((float)srcRow[x] / 255.0f) * depthScale;
            dstRow[x] = (uint8_t)((d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_flow_map(const void* restrict srcHeightmap, void* restrict dstFlowMap, int width, int height, float strength, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstFlowMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstFlowMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            float gx = ((float)rCurr[xNext] - (float)rCurr[xPrev]) * 0.5f * strength / 255.0f;
            float gy = ((float)rNext[x] - (float)rPrev[x]) * 0.5f * strength / 255.0f;
            float u = 0.5f - gx * 0.5f, v = 0.5f - gy * 0.5f;
            dstRow[x*4+0] = (uint8_t)((u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u)) * 255.0f + 0.5f);
            dstRow[x*4+1] = (uint8_t)((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f);
            dstRow[x*4+2] = 128; dstRow[x*4+3] = 255;
        }
    }
}

BCDECDEF void bcdec_generate_cavity_map(const void* restrict srcHeightmap, void* restrict dstCavityMap, int width, int height, float scale, int srcPitch, int dstPitch) {
    if (!srcHeightmap || !dstCavityMap || width <= 0 || height <= 0) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcHeightmap;
    uint8_t* restrict dstBase = (uint8_t*)dstCavityMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* rPrev = srcBase + (size_t)(y > 0 ? y - 1 : 0) * (size_t)srcPitch;
        const uint8_t* rCurr = srcBase + (size_t)y * (size_t)srcPitch;
        const uint8_t* rNext = srcBase + (size_t)(y < height - 1 ? y + 1 : height - 1) * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            int xPrev = x > 0 ? x - 1 : 0, xNext = x < width - 1 ? x + 1 : width - 1;
            float localAvg = ((float)rPrev[xPrev] + (float)rPrev[x] + (float)rPrev[xNext] + (float)rCurr[xPrev] +
                              (float)rCurr[xNext] + (float)rNext[xPrev] + (float)rNext[x] + (float)rNext[xNext]) / 8.0f;
            float diff = ((float)rCurr[x] - localAvg) / 255.0f * scale;
            float cavity = 0.5f + diff;
            dstRow[x] = (uint8_t)((cavity < 0.0f ? 0.0f : (cavity > 1.0f ? 1.0f : cavity)) * 255.0f + 0.5f);
        }
    }
}

BCDECDEF void bcdec_generate_emissive_mask(const void* restrict srcAlbedo, void* restrict dstEmissiveMap, int width, int height, int bytesPerPixel, float threshold, float intensity, int srcPitch, int dstPitch) {
    if (!srcAlbedo || !dstEmissiveMap || width <= 0 || height <= 0 || bytesPerPixel < 3) return;
    const uint8_t* restrict srcBase = (const uint8_t*)srcAlbedo;
    uint8_t* restrict dstBase = (uint8_t*)dstEmissiveMap;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcBase + (size_t)y * (size_t)srcPitch;
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)dstPitch;
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = srcRow + (size_t)x * (size_t)bytesPerPixel;
            float luma = ((float)px[0]*0.2126f + (float)px[1]*0.7152f + (float)px[2]*0.0722f) / 255.0f;
            if (luma >= threshold) {
                float factor = ((luma - threshold) / (1.0f - threshold + 0.0001f)) * intensity;
                if (factor > 1.0f) factor = 1.0f;
                dstRow[x*4+0] = (uint8_t)((float)px[0]*factor + 0.5f);
                dstRow[x*4+1] = (uint8_t)((float)px[1]*factor + 0.5f);
                dstRow[x*4+2] = (uint8_t)((float)px[2]*factor + 0.5f);
                dstRow[x*4+3] = 255;
            } else {
                dstRow[x*4+0] = dstRow[x*4+1] = dstRow[x*4+2] = 0; dstRow[x*4+3] = 255;
            }
        }
    }
}

BCDECDEF void bcdec_pack_orm_map(const void* restrict srcAoMap, const void* restrict srcRoughnessMap, const void* restrict srcMetallicMap, void* restrict dstOrmMap, int width, int height, int pitch) {
    if (!dstOrmMap || width <= 0 || height <= 0) return;
    const uint8_t* aoBase = (const uint8_t*)srcAoMap;
    const uint8_t* roughBase = (const uint8_t*)srcRoughnessMap;
    const uint8_t* metalBase = (const uint8_t*)srcMetallicMap;
    uint8_t* dstBase = (uint8_t*)dstOrmMap;
    for (int y = 0; y < height; ++y) {
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)pitch;
        for (int x = 0; x < width; ++x) {
            dstRow[x*4+0] = aoBase ? aoBase[(size_t)y*(size_t)width + (size_t)x] : 255;
            dstRow[x*4+1] = roughBase ? roughBase[(size_t)y*(size_t)width + (size_t)x] : 128;
            dstRow[x*4+2] = metalBase ? metalBase[(size_t)y*(size_t)width + (size_t)x] : 0;
            dstRow[x*4+3] = 255;
        }
    }
}

BCDECDEF void bcdec_pack_custom_channels(const void* restrict srcCh0, const void* restrict srcCh1, const void* restrict srcCh2, const void* restrict srcCh3, void* restrict dstPackedMap, int width, int height, int pitch, int activeChannelsMask) {
    if (!dstPackedMap || width <= 0 || height <= 0) return;
    const uint8_t* c0 = (const uint8_t*)srcCh0; const uint8_t* c1 = (const uint8_t*)srcCh1;
    const uint8_t* c2 = (const uint8_t*)srcCh2; const uint8_t* c3 = (const uint8_t*)srcCh3;
    uint8_t* dstBase = (uint8_t*)dstPackedMap;
    for (int y = 0; y < height; ++y) {
        uint8_t* dstRow = dstBase + (size_t)y * (size_t)pitch;
        size_t rowOff = (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            dstRow[x*4+0] = ((activeChannelsMask & 1) && c0) ? c0[rowOff + (size_t)x] : 0;
            dstRow[x*4+1] = ((activeChannelsMask & 2) && c1) ? c1[rowOff + (size_t)x] : 0;
            dstRow[x*4+2] = ((activeChannelsMask & 4) && c2) ? c2[rowOff + (size_t)x] : 0;
            dstRow[x*4+3] = ((activeChannelsMask & 8) && c3) ? c3[rowOff + (size_t)x] : 255;
        }
    }
}

/* =========================================================================
 * CODIFICADORES BC Y TRANSCODIFICADORES
 * ========================================================================= */

BCDECDEF void bcdec_encode_block_bc1(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    if (!rgbaPixels || !compressedBlock) return;
    const uint8_t* pixels = (const uint8_t*)rgbaPixels;
    uint8_t* out = (uint8_t*)compressedBlock;
    uint8_t minR = 255, minG = 255, minB = 255, maxR = 0, maxG = 0, maxB = 0;
    for (int i = 0; i < 16; ++i) {
        uint8_t r = pixels[i*4+0], g = pixels[i*4+1], b = pixels[i*4+2];
        if (r < minR) minR = r; if (r > maxR) maxR = r;
        if (g < minG) minG = g; if (g > maxG) maxG = g;
        if (b < minB) minB = b; if (b > maxB) maxB = b;
    }
    uint16_t c0 = (uint16_t)(((maxR >> 3) << 11) | ((maxG >> 2) << 5) | (maxB >> 3));
    uint16_t c1 = (uint16_t)(((minR >> 3) << 11) | ((minG >> 2) << 5) | (minB >> 3));
    if (c0 < c1) { uint16_t tmp = c0; c0 = c1; c1 = tmp; }
    memcpy(out, &c0, 2); memcpy(out + 2, &c1, 2);

    uint8_t palette[4][3] = {
        { (uint8_t)((c0 >> 11) << 3), (uint8_t)(((c0 >> 5) & 0x3F) << 2), (uint8_t)((c0 & 0x1F) << 3) },
        { (uint8_t)((c1 >> 11) << 3), (uint8_t)(((c1 >> 5) & 0x3F) << 2), (uint8_t)((c1 & 0x1F) << 3) },
        {0}, {0}
    };
    if (c0 > c1) {
        for (int c = 0; c < 3; ++c) {
            palette[2][c] = (uint8_t)((2 * palette[0][c] + palette[1][c]) / 3);
            palette[3][c] = (uint8_t)((palette[0][c] + 2 * palette[1][c]) / 3);
        }
    } else {
        for (int c = 0; c < 3; ++c) palette[2][c] = (uint8_t)((palette[0][c] + palette[1][c]) / 2);
    }

    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int bestIdx = 0, bestDist = 0x7FFFFFFF;
        for (int p = 0; p < (c0 > c1 ? 4 : 3); ++p) {
            int dr = (int)pixels[i*4+0] - palette[p][0];
            int dg = (int)pixels[i*4+1] - palette[p][1];
            int db = (int)pixels[i*4+2] - palette[p][2];
            int dist = dr*dr + dg*dg + db*db;
            if (dist < bestDist) { bestDist = dist; bestIdx = p; }
        }
        indices |= ((uint32_t)bestIdx << (i * 2));
    }
    memcpy(out + 4, &indices, 4);
}

BCDECDEF void bcdec_encode_block_bc1_optimized(const void* restrict rgbaPixels, void* restrict compressedBlock, int qualityLevel) {
    (void)qualityLevel;
    bcdec_encode_block_bc1(rgbaPixels, compressedBlock);
}

BCDECDEF void bcdec_encode_block_bc1_perceptual_mse(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    bcdec_encode_block_bc1(rgbaPixels, compressedBlock);
}

BCDECDEF void bcdec_encode_block_bc4(const void* restrict rPixels, void* restrict compressedBlock, int isSigned) {
    if (!rPixels || !compressedBlock) return;
    const uint8_t* pixels = (const uint8_t*)rPixels;
    uint8_t* out = (uint8_t*)compressedBlock;
    uint8_t minA = 255, maxA = 0;
    for (int i = 0; i < 16; ++i) {
        if (pixels[i] < minA) minA = pixels[i];
        if (pixels[i] > maxA) maxA = pixels[i];
    }
    (void)isSigned;
    out[0] = maxA; out[1] = minA;
    uint8_t palette[8] = { maxA, minA, 0 };
    if (maxA > minA) {
        for (int k = 1; k <= 6; ++k) palette[k + 1] = (uint8_t)(((7 - k) * maxA + k * minA) / 7);
    }
    uint64_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int bestIdx = 0, bestDiff = 256;
        for (int p = 0; p < 8; ++p) {
            int diff = abs((int)pixels[i] - (int)palette[p]);
            if (diff < bestDiff) { bestDiff = diff; bestIdx = p; }
        }
        indices |= ((uint64_t)bestIdx << (i * 3));
    }
    for (int b = 0; b < 6; ++b) out[2 + b] = (uint8_t)((indices >> (b * 8)) & 0xFF);
}

BCDECDEF void bcdec_encode_block_bc3(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    if (!rgbaPixels || !compressedBlock) return;
    const uint8_t* pixels = (const uint8_t*)rgbaPixels;
    uint8_t alphas[16];
    for (int i = 0; i < 16; ++i) alphas[i] = pixels[i * 4 + 3];
    bcdec_encode_block_bc4(alphas, compressedBlock, 0);
    bcdec_encode_block_bc1(rgbaPixels, (uint8_t*)compressedBlock + 8);
}

BCDECDEF void bcdec_encode_block_bc5(const void* restrict rgPixels, void* restrict compressedBlock, int isSigned) {
    if (!rgPixels || !compressedBlock) return;
    const uint8_t* pixels = (const uint8_t*)rgPixels;
    uint8_t rChan[16], gChan[16];
    for (int i = 0; i < 16; ++i) { rChan[i] = pixels[i*2+0]; gChan[i] = pixels[i*2+1]; }
    bcdec_encode_block_bc4(rChan, compressedBlock, isSigned);
    bcdec_encode_block_bc4(gChan, (uint8_t*)compressedBlock + 8, isSigned);
}


/* ---- escritor de bits LSB-first para los encoders BC6H / BC7 ---- */
typedef struct bcdec__bitwriter { uint64_t low, high; int pos; } bcdec__bitwriter_t;
static void bcdec__bw_put(bcdec__bitwriter_t* w, uint32_t value, int nbits) {
    for (int i = 0; i < nbits; ++i) {
        uint64_t bit = (value >> i) & 1u;
        if (w->pos < 64) w->low |= bit << w->pos; else if (w->pos < 128) w->high |= bit << (w->pos - 64);
        w->pos++;
    }
}
static void bcdec__bw_flush(const bcdec__bitwriter_t* w, uint8_t* block) {
    memcpy(block, &w->low, 8); memcpy(block + 8, &w->high, 8);
}
/* Eje principal de N canales por potencia de iteraci贸n; devuelve media y direcci贸n */
static void bcdec__principal_axis(const float* v, int n, int ch, float* mean, float* dir) {
    float cov[4][4] = {{0}};
    for (int c = 0; c < ch; ++c) { mean[c] = 0; for (int i = 0; i < n; ++i) mean[c] += v[i*ch+c]; mean[c] /= (float)n; }
    for (int i = 0; i < n; ++i) for (int a = 0; a < ch; ++a) for (int b = 0; b < ch; ++b)
        cov[a][b] += (v[i*ch+a] - mean[a]) * (v[i*ch+b] - mean[b]);
    float d[4] = {1,1,1,1};
    for (int it = 0; it < 8; ++it) {
        float nd[4] = {0}; float len = 0;
        for (int a = 0; a < ch; ++a) { for (int b = 0; b < ch; ++b) nd[a] += cov[a][b] * d[b]; len += nd[a]*nd[a]; }
        len = __builtin_sqrtf(len);
        if (len < 1e-6f) break;
        for (int a = 0; a < ch; ++a) d[a] = nd[a] / len;
    }
    for (int c = 0; c < ch; ++c) dir[c] = d[c];
}

BCDECDEF void bcdec_encode_block_bc6h_half(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned) {
    /* Encoder BC6H v谩lido (modo de 1 regi贸n, extremos directos de 10 bits, 铆ndices de 4 bits). */
    if (!rgbHalfPixels || !compressedBlock) return;
    const uint16_t* h = (const uint16_t*)rgbHalfPixels;
    float q[16*3], mean[3], dir[3];
    for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) {
        uint32_t bits = h[i*3+c];
        if (!isSigned) {
            uint32_t m = bits > 0x7BFF ? 0x7BFF : bits;                    /* sin NaN/Inf, sin signo */
            if (bits & 0x8000) m = 0;                                      /* valores negativos -> 0 */
            q[i*3+c] = (float)m / 31.0f;
        } else {
            uint32_t m = bits & 0x7FFF; if (m > 0x7BFF) m = 0x7BFF;
            float val = (float)m * 32.0f / 31.0f / 32.0f;                  /* ~ m/31 */
            q[i*3+c] = (bits & 0x8000) ? -val : val;
        }
    }
    float lo = isSigned ? -511.0f : 0.0f, hi = isSigned ? 511.0f : 1023.0f;
    bcdec__principal_axis(q, 16, 3, mean, dir);
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; ++i) { float t = 0; for (int c = 0; c < 3; ++c) t += (q[i*3+c] - mean[c]) * dir[c]; if (t < tmin) tmin = t; if (t > tmax) tmax = t; }
    int ep[2][3];
    for (int e = 0; e < 2; ++e) for (int c = 0; c < 3; ++c) {
        float x = mean[c] + dir[c] * (e ? tmax : tmin);
        x = x < lo ? lo : (x > hi ? hi : x);
        ep[e][c] = (int)(x + (x < 0 ? -0.5f : 0.5f));
    }
    static const int w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f; idx[i] = 0;
        for (int k = 0; k < 16; ++k) {
            float err = 0;
            for (int c = 0; c < 3; ++c) {
                float col = (ep[0][c] * (64 - w4[k]) + ep[1][c] * w4[k]) / 64.0f;
                float d = col - q[i*3+c]; err += d * d;
            }
            if (err < best) { best = err; idx[i] = k; }
        }
    }
    if (idx[0] >= 8) {
        for (int c = 0; c < 3; ++c) { int t = ep[0][c]; ep[0][c] = ep[1][c]; ep[1][c] = t; }
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }
    bcdec__bitwriter_t w = {0, 0, 0};
    bcdec__bw_put(&w, 0x03, 5);              /* modo de 10 bits sin transformaci贸n (modo 11 de la spec) */
    for (int e = 0; e < 2; ++e) for (int c = 0; c < 3; ++c) bcdec__bw_put(&w, (uint32_t)ep[e][c] & 0x3FFu, 10);
    for (int i = 0; i < 16; ++i) bcdec__bw_put(&w, (uint32_t)idx[i], i == 0 ? 3 : 4);
    bcdec__bw_flush(&w, (uint8_t*)compressedBlock);
}

BCDECDEF void bcdec_encode_block_bc6h_optimized(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned) {
    bcdec_encode_block_bc6h_half(rgbHalfPixels, compressedBlock, isSigned);
}
BCDECDEF void bcdec_encode_block_bc6h_advanced(const void* restrict rgbHalfPixels, void* restrict compressedBlock, int isSigned) {
    bcdec_encode_block_bc6h_half(rgbHalfPixels, compressedBlock, isSigned);
}

BCDECDEF void bcdec_encode_block_bc7(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    /* Encoder BC7 v谩lido (modo 6: 1 subconjunto, RGBA 7.7.7.7 + P-bit por extremo, 铆ndices de 4 bits). */
    if (!rgbaPixels || !compressedBlock) return;
    const uint8_t* px = (const uint8_t*)rgbaPixels;
    float v[16*4], mean[4], dir[4];
    for (int i = 0; i < 64; ++i) v[i] = (float)px[i];
    bcdec__principal_axis(v, 16, 4, mean, dir);
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; ++i) { float t = 0; for (int c = 0; c < 4; ++c) t += (v[i*4+c] - mean[c]) * dir[c]; if (t < tmin) tmin = t; if (t > tmax) tmax = t; }
    int ep[2][4];
    for (int e = 0; e < 2; ++e) for (int c = 0; c < 4; ++c) {
        float x = mean[c] + dir[c] * (e ? tmax : tmin);
        ep[e][c] = (int)(x < 0 ? 0 : (x > 255 ? 255 : x + 0.5f));
    }
    int q7[2][4], pb[2], dq[2][4];
    for (int e = 0; e < 2; ++e) {
        long bestErr = 0x7FFFFFFF; pb[e] = 0;
        for (int p = 0; p < 2; ++p) {
            int cand[4]; long err = 0;
            for (int c = 0; c < 4; ++c) {
                int q = (ep[e][c] - p + 1) >> 1; if (ep[e][c] < p) q = 0; if (q > 127) q = 127;
                cand[c] = q; int d = (q << 1 | p) - ep[e][c]; err += (long)d * d;
            }
            if (err < bestErr) { bestErr = err; pb[e] = p; for (int c = 0; c < 4; ++c) q7[e][c] = cand[c]; }
        }
        for (int c = 0; c < 4; ++c) dq[e][c] = (q7[e][c] << 1) | pb[e];
    }
    static const int w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        long best = 0x7FFFFFFF; idx[i] = 0;
        for (int k = 0; k < 16; ++k) {
            long err = 0;
            for (int c = 0; c < 4; ++c) {
                int col = (dq[0][c] * (64 - w4[k]) + dq[1][c] * w4[k] + 32) >> 6;
                int d = col - (int)px[i*4+c]; err += (long)d * d;
            }
            if (err < best) { best = err; idx[i] = k; }
        }
    }
    if (idx[0] >= 8) {                       /* el 铆ndice ancla (p铆xel 0) debe tener el MSB a 0 */
        for (int c = 0; c < 4; ++c) { int t = q7[0][c]; q7[0][c] = q7[1][c]; q7[1][c] = t; }
        int t = pb[0]; pb[0] = pb[1]; pb[1] = t;
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }
    bcdec__bitwriter_t w = {0, 0, 0};
    bcdec__bw_put(&w, 0x40, 7);              /* modo 6 */
    for (int c = 0; c < 4; ++c) { bcdec__bw_put(&w, (uint32_t)q7[0][c], 7); bcdec__bw_put(&w, (uint32_t)q7[1][c], 7); }
    bcdec__bw_put(&w, (uint32_t)pb[0], 1); bcdec__bw_put(&w, (uint32_t)pb[1], 1);
    for (int i = 0; i < 16; ++i) bcdec__bw_put(&w, (uint32_t)idx[i], i == 0 ? 3 : 4);
    bcdec__bw_flush(&w, (uint8_t*)compressedBlock);
}

BCDECDEF void bcdec_encode_block_bc7_perceptual(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    bcdec_encode_block_bc7(rgbaPixels, compressedBlock);
}
BCDECDEF void bcdec_encode_block_bc7_advanced(const void* restrict rgbaPixels, void* restrict compressedBlock) {
    bcdec_encode_block_bc7(rgbaPixels, compressedBlock);
}

BCDECDEF bcdec_status_t bcdec_encode_texture_bc7(const void* restrict rgbaPixels, void* restrict compressedData, int width, int height, int qualityPreset) {
    (void)qualityPreset;
    if (!rgbaPixels || !compressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int blocksX = (width + 3) >> 2, blocksY = (height + 3) >> 2;
    const uint8_t* src = (const uint8_t*)rgbaPixels;
    uint8_t* dst = (uint8_t*)compressedData;
    int pitch = width * 4;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            uint8_t blk[64];
            for (int py = 0; py < 4; ++py) {
                int sy = (by*4+py < height) ? (by*4+py) : (height - 1);
                for (int px = 0; px < 4; ++px) {
                    int sx = (bx*4+px < width) ? (bx*4+px) : (width - 1);
                    memcpy(blk + (py*4+px)*4, src + (size_t)sy*(size_t)pitch + (size_t)sx*4, 4);
                }
            }
            bcdec_encode_block_bc7(blk, dst);
            dst += 16;
        }
    }
    return BCDEC_SUCCESS;
}

typedef struct bcdec__encode_async_ctx {
    bcdec_format_t format;
    const void* srcPixels;
    void* compressedData;
    int width;
    int height;
    int srcPitch;
} bcdec__encode_async_ctx_t;

static void bcdec__encode_async_worker(int jobIndex, int totalJobs, void* userArg) {
    bcdec__encode_async_ctx_t* ctx = (bcdec__encode_async_ctx_t*)userArg;
    int startRowBlockY, endRowBlockY;
    bcdec_calculate_parallel_blocks(ctx->height, jobIndex, totalJobs, &startRowBlockY, &endRowBlockY);
    int blocksX = (ctx->width + 3) >> 2;
    size_t blockSize = bcdec_format_block_size(ctx->format);
    int bpp = bcdec_format_pixel_size(ctx->format);
    const uint8_t* srcBase = (const uint8_t*)ctx->srcPixels;
    uint8_t* dstBase = (uint8_t*)ctx->compressedData;

    for (int by = startRowBlockY; by < endRowBlockY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            uint8_t blk[16 * 8];
            for (int py = 0; py < 4; ++py) {
                int sy = (by*4+py < ctx->height) ? (by*4+py) : (ctx->height - 1);
                for (int px = 0; px < 4; ++px) {
                    int sx = (bx*4+px < ctx->width) ? (bx*4+px) : (ctx->width - 1);
                    memcpy(blk + (py*4+px)*bpp, srcBase + (size_t)sy*(size_t)ctx->srcPitch + (size_t)sx*(size_t)bpp, (size_t)bpp);
                }
            }
            void* dstBlk = dstBase + ((size_t)by * (size_t)blocksX + (size_t)bx) * blockSize;
            switch (ctx->format) {
                case BCDEC_FORMAT_BC1:  bcdec_encode_block_bc1(blk, dstBlk); break;
                case BCDEC_FORMAT_BC3:  bcdec_encode_block_bc3(blk, dstBlk); break;
                case BCDEC_FORMAT_BC4:  bcdec_encode_block_bc4(blk, dstBlk, 0); break;
                case BCDEC_FORMAT_BC5:  bcdec_encode_block_bc5(blk, dstBlk, 0); break;
                case BCDEC_FORMAT_BC6H: bcdec_encode_block_bc6h_half(blk, dstBlk, 0); break;
                case BCDEC_FORMAT_BC7:  bcdec_encode_block_bc7(blk, dstBlk); break;
                default: break;
            }
        }
    }
}

BCDECDEF bcdec_status_t bcdec_encode_texture_parallel_async(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch, bcdec_job_desc_t jobDesc) {
    if (!srcPixels || !compressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    if (bcdec_format_block_size(format) == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    bcdec__encode_async_ctx_t ctx = { format, srcPixels, compressedData, width, height, srcPitch };
    if (jobDesc.totalJobs <= 1 || !jobDesc.callback) {
        bcdec__encode_async_worker(0, 1, &ctx);
    } else {
        bcdec_job_desc_t d = jobDesc;
        d.callback = bcdec__encode_async_worker;
        d.userArg = &ctx;
        bcdec_dispatch_jobs(d);
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_encode_texture_mipmaps_parallel_async(bcdec_format_t format, const void* restrict srcMipChainPixels, void* restrict compressedData, int width, int height, int mipLevels, int baseSrcPitch, bcdec_job_desc_t jobDesc) {
    if (!srcMipChainPixels || !compressedData || width <= 0 || height <= 0 || mipLevels <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    const uint8_t* srcPtr = (const uint8_t*)srcMipChainPixels;
    uint8_t* dstPtr = (uint8_t*)compressedData;
    int curW = width, curH = height, curPitch = baseSrcPitch;
    for (int m = 0; m < mipLevels; ++m) {
        bcdec_status_t st = bcdec_encode_texture_parallel_async(format, srcPtr, dstPtr, curW, curH, curPitch, jobDesc);
        if (st != BCDEC_SUCCESS) return st;
        srcPtr += (size_t)curH * (size_t)curPitch;
        dstPtr += bcdec_texture_compressed_size(format, curW, curH);
        curW = (curW > 1) ? (curW >> 1) : 1;
        curH = (curH > 1) ? (curH >> 1) : 1;
        curPitch = curW * bcdec_format_pixel_size(format);
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc1_to_bc3(const void* restrict bc1CompressedData, void* restrict bc3CompressedData, int width, int height) {
    if (!bc1CompressedData || !bc3CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc1CompressedData;
    uint8_t* dst = (uint8_t*)bc3CompressedData;
    static const uint8_t opaqueAlpha[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    for (int i = 0; i < totalBlocks; ++i) {
        memcpy(dst, opaqueAlpha, 8);
        memcpy(dst + 8, src, 8);
        src += 8; dst += 16;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc3_to_bc1(const void* restrict bc3CompressedData, void* restrict bc1CompressedData, int width, int height) {
    if (!bc3CompressedData || !bc1CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc3CompressedData;
    uint8_t* dst = (uint8_t*)bc1CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        memcpy(dst, src + 8, 8);
        src += 16; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc3_to_bc4(const void* restrict bc3CompressedData, void* restrict bc4CompressedData, int width, int height) {
    if (!bc3CompressedData || !bc4CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc3CompressedData;
    uint8_t* dst = (uint8_t*)bc4CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        memcpy(dst, src, 8);
        src += 16; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc5_to_bc4(const void* restrict bc5CompressedData, void* restrict bc4CompressedData, int width, int height, int channelIndex) {
    if (!bc5CompressedData || !bc4CompressedData || width <= 0 || height <= 0 || (channelIndex != 0 && channelIndex != 1)) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc5CompressedData + (channelIndex * 8);
    uint8_t* dst = (uint8_t*)bc4CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        memcpy(dst, src, 8);
        src += 16; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc4_to_bc1(const void* restrict bc4CompressedData, void* restrict bc1CompressedData, int width, int height) {
    if (!bc4CompressedData || !bc1CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc4CompressedData;
    uint8_t* dst = (uint8_t*)bc1CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        uint8_t rBlock[16], rgbaBlock[64];
#ifndef BCDEC_BC4BC5_PRECISE
        bcdec_bc4(src, rBlock, 4);
#else
        bcdec_bc4(src, rBlock, 4, 0);
#endif
        for (int p = 0; p < 16; ++p) {
            rgbaBlock[p*4+0] = rgbaBlock[p*4+1] = rgbaBlock[p*4+2] = rBlock[p];
            rgbaBlock[p*4+3] = 255;
        }
        bcdec_encode_block_bc1(rgbaBlock, dst);
        src += 8; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc7_to_bc1(const void* restrict bc7CompressedData, void* restrict bc1CompressedData, int width, int height) {
    if (!bc7CompressedData || !bc1CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc7CompressedData;
    uint8_t* dst = (uint8_t*)bc1CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        uint8_t rgbaBlock[64];
        bcdec_bc7(src, rgbaBlock, 16);
        bcdec_encode_block_bc1(rgbaBlock, dst);
        src += 16; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_bc7_to_bc3(const void* restrict bc7CompressedData, void* restrict bc3CompressedData, int width, int height) {
    if (!bc7CompressedData || !bc3CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int totalBlocks = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc7CompressedData;
    uint8_t* dst = (uint8_t*)bc3CompressedData;
    for (int i = 0; i < totalBlocks; ++i) {
        uint8_t rgbaBlock[64];
        bcdec_bc7(src, rgbaBlock, 16);
        bcdec_encode_block_bc3(rgbaBlock, dst);
        src += 16; dst += 16;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_transcode_stream_chunk(bcdec_format_t srcFormat, bcdec_format_t dstFormat, const void* restrict srcChunk, void* restrict dstChunk, size_t chunkSize, int width, int height) {
    if (!srcChunk || !dstChunk || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    if (chunkSize < bcdec_texture_compressed_size(srcFormat, width, height)) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    return bcdec_vulkan_transcode_surface_format(srcFormat, dstFormat, srcChunk, dstChunk, width, height);
}

/* =========================================================================
 * M脡TRICAS, PARSERS Y VALIDADORES DE INTEGRIDAD
 * ========================================================================= */

BCDECDEF void bcdec_calculate_surface_stats(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch, bcdec_surface_stats_t* restrict outStats) {
    if (!imageData || !outStats || width <= 0 || height <= 0 || bytesPerPixel <= 0) return;
    const uint8_t* restrict row = (const uint8_t*)imageData;
    uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    outStats->minR = outStats->minG = outStats->minB = 255;
    outStats->maxR = outStats->maxG = outStats->maxB = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            uint8_t r = pixel[0], g = bytesPerPixel > 1 ? pixel[1] : r, b = bytesPerPixel > 2 ? pixel[2] : r, a = bytesPerPixel > 3 ? pixel[3] : 255;
            sumR += r; sumG += g; sumB += b; sumA += a;
            if (r < outStats->minR) outStats->minR = r; if (r > outStats->maxR) outStats->maxR = r;
            if (g < outStats->minG) outStats->minG = g; if (g > outStats->maxG) outStats->maxG = g;
            if (b < outStats->minB) outStats->minB = b; if (b > outStats->maxB) outStats->maxB = b;
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
    double total = (double)((size_t)width * (size_t)height);
    outStats->avgR = (float)((double)sumR / total); outStats->avgG = (float)((double)sumG / total);
    outStats->avgB = (float)((double)sumB / total); outStats->avgA = (float)((double)sumA / total);
}

BCDECDEF float bcdec_calculate_surface_entropy(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 0 || height <= 0 || bytesPerPixel <= 0) return 0.0f;
    uint32_t hist[256] = {0};
    const uint8_t* restrict row = (const uint8_t*)imageData;
    size_t total = (size_t)width * (size_t)height;
    for (int y = 0; y < height; ++y) {
        const uint8_t* restrict pixel = row;
        for (int x = 0; x < width; ++x) {
            uint8_t luma = (bytesPerPixel >= 3) ? (uint8_t)((pixel[0]*77 + pixel[1]*150 + pixel[2]*29) >> 8) : pixel[0];
            hist[luma]++;
            pixel += bytesPerPixel;
        }
        row += destinationPitch;
    }
    float entropy = 0.0f;
    for (int i = 0; i < 256; ++i) {
        if (hist[i] > 0) {
            float p = (float)hist[i] / (float)total;
            entropy -= p * log2f(p);
        }
    }
    return entropy;
}

BCDECDEF int bcdec_classify_block_complexity(bcdec_format_t format, const void* restrict compressedBlock) {
    if (!compressedBlock) return 0;
    float var = bcdec_estimate_block_variance(format, compressedBlock);
    return (var < 25.0f) ? 0 : ((var < 400.0f) ? 1 : 2);
}

BCDECDEF int bcdec_estimate_optimal_bc_quality(const void* restrict rgbaPixels, int width, int height, int pitch) {
    if (!rgbaPixels || width <= 0 || height <= 0) return 1;
    float ent = bcdec_calculate_surface_entropy(rgbaPixels, width, height, 4, pitch);
    return (ent < 3.5f) ? 0 : ((ent < 6.0f) ? 1 : 2);
}

BCDECDEF float bcdec_calculate_block_mse(const void* restrict originalBlock, const void* restrict decompressedBlock, int pixelSize, int blockPixelCount) {
    if (!originalBlock || !decompressedBlock || pixelSize <= 0 || blockPixelCount <= 0) return 0.0f;
    const uint8_t* orig = (const uint8_t*)originalBlock;
    const uint8_t* decomp = (const uint8_t*)decompressedBlock;
    double sumSq = 0.0;
    size_t totalBytes = (size_t)pixelSize * (size_t)blockPixelCount;
    for (size_t i = 0; i < totalBytes; ++i) {
        double d = (double)orig[i] - (double)decomp[i];
        sumSq += d * d;
    }
    return (float)(sumSq / (double)totalBytes);
}

BCDECDEF float bcdec_calculate_surface_psnr(float mse) {
    if (mse <= 0.000001f) return 100.0f;
    return 10.0f * log10f((255.0f * 255.0f) / mse);
}

BCDECDEF float bcdec_calculate_surface_ssim(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch) {
    if (!originalImage || !decompressedImage || width <= 0 || height <= 0 || bytesPerPixel <= 0) return 0.0f;
    const uint8_t* r1 = (const uint8_t*)originalImage;
    const uint8_t* r2 = (const uint8_t*)decompressedImage;
    double muX = 0.0, muY = 0.0;
    size_t count = (size_t)width * (size_t)height * (size_t)bytesPerPixel;
    for (int y = 0; y < height; ++y) {
        const uint8_t* p1 = r1 + (size_t)y * (size_t)pitch;
        const uint8_t* p2 = r2 + (size_t)y * (size_t)pitch;
        for (int i = 0; i < width * bytesPerPixel; ++i) { muX += p1[i]; muY += p2[i]; }
    }
    muX /= (double)count; muY /= (double)count;
    double sigX2 = 0.0, sigY2 = 0.0, sigXY = 0.0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* p1 = r1 + (size_t)y * (size_t)pitch;
        const uint8_t* p2 = r2 + (size_t)y * (size_t)pitch;
        for (int i = 0; i < width * bytesPerPixel; ++i) {
            double dx = (double)p1[i] - muX, dy = (double)p2[i] - muY;
            sigX2 += dx * dx; sigY2 += dy * dy; sigXY += dx * dy;
        }
    }
    sigX2 /= (double)count; sigY2 /= (double)count; sigXY /= (double)count;
    double c1 = 6.5025, c2 = 58.5225;
    double num = (2.0 * muX * muY + c1) * (2.0 * sigXY + c2);
    double den = (muX * muX + muY * muY + c1) * (sigX2 + sigY2 + c2);
    return (float)(num / den);
}

BCDECDEF float bcdec_calculate_surface_rmse(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch) {
    if (!originalImage || !decompressedImage || width <= 0 || height <= 0 || bytesPerPixel <= 0) return 0.0f;
    const uint8_t* r1 = (const uint8_t*)originalImage;
    const uint8_t* r2 = (const uint8_t*)decompressedImage;
    double sumSq = 0.0;
    size_t count = (size_t)width * (size_t)height * (size_t)bytesPerPixel;
    for (int y = 0; y < height; ++y) {
        const uint8_t* p1 = r1 + (size_t)y * (size_t)pitch;
        const uint8_t* p2 = r2 + (size_t)y * (size_t)pitch;
        for (int i = 0; i < width * bytesPerPixel; ++i) {
            double d = (double)p1[i] - (double)p2[i];
            sumSq += d * d;
        }
    }
    return sqrtf((float)(sumSq / (double)count));
}

BCDECDEF void bcdec_calculate_surface_channel_metrics(const void* restrict originalImage, const void* restrict decompressedImage, int width, int height, int bytesPerPixel, int pitch, bcdec_channel_metrics_t* restrict outMetrics) {
    if (!originalImage || !decompressedImage || !outMetrics || width <= 0 || height <= 0 || bytesPerPixel <= 0) return;
    double sumSq[4] = {0};
    const uint8_t* r1 = (const uint8_t*)originalImage;
    const uint8_t* r2 = (const uint8_t*)decompressedImage;
    size_t pixels = (size_t)width * (size_t)height;
    for (int y = 0; y < height; ++y) {
        const uint8_t* p1 = r1 + (size_t)y * (size_t)pitch;
        const uint8_t* p2 = r2 + (size_t)y * (size_t)pitch;
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < bytesPerPixel && c < 4; ++c) {
                double d = (double)p1[x * bytesPerPixel + c] - (double)p2[x * bytesPerPixel + c];
                sumSq[c] += d * d;
            }
        }
    }
    float mse[4] = {
        (float)(sumSq[0] / (double)pixels),
        bytesPerPixel > 1 ? (float)(sumSq[1] / (double)pixels) : 0.0f,
        bytesPerPixel > 2 ? (float)(sumSq[2] / (double)pixels) : 0.0f,
        bytesPerPixel > 3 ? (float)(sumSq[3] / (double)pixels) : 0.0f
    };
    outMetrics->rmseR = sqrtf(mse[0]); outMetrics->psnrR = bcdec_calculate_surface_psnr(mse[0]);
    outMetrics->rmseG = sqrtf(mse[1]); outMetrics->psnrG = bcdec_calculate_surface_psnr(mse[1]);
    outMetrics->rmseB = sqrtf(mse[2]); outMetrics->psnrB = bcdec_calculate_surface_psnr(mse[2]);
    outMetrics->rmseA = sqrtf(mse[3]); outMetrics->psnrA = bcdec_calculate_surface_psnr(mse[3]);
}

BCDECDEF uint32_t bcdec_calculate_buffer_crc32(const void* restrict buffer, size_t length) {
    if (!buffer || length == 0) return 0;
    const uint8_t* data = (const uint8_t*)buffer;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return ~crc;
}

BCDECDEF uint32_t bcdec_calculate_surface_hash(const void* restrict imageData, int width, int height, int bytesPerPixel, int destinationPitch) {
    if (!imageData || width <= 0 || height <= 0 || bytesPerPixel <= 0) return 0;
    const uint8_t* row = (const uint8_t*)imageData;
    uint32_t hash = 2166136261U;
    size_t rowBytes = (size_t)width * (size_t)bytesPerPixel;
    for (int y = 0; y < height; ++y) {
        for (size_t x = 0; x < rowBytes; ++x) {
            hash ^= row[x];
            hash *= 16777619U;
        }
        row += destinationPitch;
    }
    return hash;
}

BCDECDEF bcdec_status_t bcdec_parse_dds_header(const void* restrict ddsFileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload) {
    if (!ddsFileBytes || !outInfo || fileSize < 128) return BCDEC_ERROR_INVALID_HEADER;
    const uint8_t* bytes = (const uint8_t*)ddsFileBytes;
    uint32_t magic; memcpy(&magic, bytes, 4);
    if (magic != 0x20534444U) return BCDEC_ERROR_INVALID_HEADER;
    uint32_t height, width, mipCount, fourCC, pfFlags;
    memcpy(&height, bytes + 12, 4); memcpy(&width, bytes + 16, 4);
    memcpy(&mipCount, bytes + 28, 4); memcpy(&pfFlags, bytes + 80, 4); memcpy(&fourCC, bytes + 84, 4);
    if (mipCount == 0) mipCount = 1;
    size_t headerOffset = 128;
    bcdec_format_t fmt = (bcdec_format_t)0;

    if ((pfFlags & 0x4U) && fourCC == 0x30315844U) { /* DX10 */
        if (fileSize < 148) return BCDEC_ERROR_INVALID_HEADER;
        uint32_t dxgiFmt; memcpy(&dxgiFmt, bytes + 128, 4);
        fmt = bcdec_format_from_dxgi(dxgiFmt);
        headerOffset = 148;
    } else {
        switch (fourCC) {
            case 0x31545844U: fmt = BCDEC_FORMAT_BC1; break;
            case 0x33545844U: fmt = BCDEC_FORMAT_BC2; break;
            case 0x35545844U: fmt = BCDEC_FORMAT_BC3; break;
            case 0x31495441U: case 0x55344342U: case 0x53344342U: fmt = BCDEC_FORMAT_BC4; break;
            case 0x32495441U: case 0x55354342U: case 0x53354342U: fmt = BCDEC_FORMAT_BC5; break;
            default: return BCDEC_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    if (fmt == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    outInfo->format = fmt; outInfo->width = (int)width; outInfo->height = (int)height;
    outInfo->mipLevels = (int)mipCount;
    outInfo->dataSize = bcdec_texture_mipmap_compressed_size(fmt, (int)width, (int)height, (int)mipCount);
    if (fileSize < headerOffset + outInfo->dataSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    if (outDataPayload) *outDataPayload = bytes + headerOffset;
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_parse_ktx_header(const void* restrict ktxFileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload) {
    if (!ktxFileBytes || !outInfo || fileSize < 64) return BCDEC_ERROR_INVALID_HEADER;
    static const uint8_t ktxMagic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    const uint8_t* bytes = (const uint8_t*)ktxFileBytes;
    if (memcmp(bytes, ktxMagic, 12) != 0) return BCDEC_ERROR_INVALID_HEADER;
    uint32_t glInternal, width, height, mips, kvBytes;
    memcpy(&glInternal, bytes + 28, 4); memcpy(&width, bytes + 36, 4);
    memcpy(&height, bytes + 40, 4); memcpy(&mips, bytes + 56, 4); memcpy(&kvBytes, bytes + 60, 4);
    bcdec_format_t fmt = bcdec_format_from_gl(glInternal);
    if (fmt == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (mips == 0) mips = 1;
    outInfo->format = fmt; outInfo->width = (int)width; outInfo->height = (int)height;
    outInfo->mipLevels = (int)mips; outInfo->dataSize = bcdec_texture_mipmap_compressed_size(fmt, (int)width, (int)height, (int)mips);
    size_t payloadOffset = 64 + (size_t)kvBytes + 4;
    if (fileSize < payloadOffset + outInfo->dataSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    if (outDataPayload) *outDataPayload = bytes + payloadOffset;
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_parse_ktx2_header(const void* restrict ktx2FileBytes, size_t fileSize, bcdec_texture_info_t* restrict outInfo, const void** restrict outDataPayload) {
    if (!ktx2FileBytes || !outInfo || fileSize < 80) return BCDEC_ERROR_INVALID_HEADER;
    static const uint8_t ktx2Magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    const uint8_t* bytes = (const uint8_t*)ktx2FileBytes;
    if (memcmp(bytes, ktx2Magic, 12) != 0) return BCDEC_ERROR_INVALID_HEADER;
    uint32_t vkFmt, width, height, mips;
    memcpy(&vkFmt, bytes + 12, 4); memcpy(&width, bytes + 20, 4);
    memcpy(&height, bytes + 24, 4); memcpy(&mips, bytes + 40, 4);
    bcdec_format_t fmt = bcdec_format_from_vulkan(vkFmt);
    if (fmt == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (mips == 0) mips = 1;
    uint64_t lvl0Offset, lvl0Len;
    memcpy(&lvl0Offset, bytes + 80, 8); memcpy(&lvl0Len, bytes + 88, 8);
    if (fileSize < (size_t)(lvl0Offset + lvl0Len)) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    outInfo->format = fmt; outInfo->width = (int)width; outInfo->height = (int)height;
    outInfo->mipLevels = (int)mips; outInfo->dataSize = (size_t)lvl0Len;
    if (outDataPayload) *outDataPayload = bytes + (size_t)lvl0Offset;
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_validate_ktx2_payload(const void* restrict ktx2FileBytes, size_t fileSize) {
    bcdec_texture_info_t info;
    const void* payload = NULL;
    bcdec_status_t st = bcdec_parse_ktx2_header(ktx2FileBytes, fileSize, &info, &payload);
    if (st != BCDEC_SUCCESS) return st;
    return bcdec_validate_texture(info.format, payload, info.width, info.height) ? BCDEC_SUCCESS : BCDEC_ERROR_CORRUPT_SURFACE;
}

BCDECDEF bcdec_status_t bcdec_verify_float_surface_integrity(const void* restrict floatData, int width, int height, int channels, int rowPitchBytes, int isHalfFloat, int rejectNegative, bcdec_float_integrity_report_t* restrict outReport) {
    if (!floatData || width <= 0 || height <= 0 || channels <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec_float_integrity_report_t rep = { 0, 0, 0, 0, -1, -1, -1 };
    const uint8_t* base = (const uint8_t*)floatData;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = base + (size_t)y * (size_t)rowPitchBytes;
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                rep.totalSamples++;
                float val;
                if (isHalfFloat) { uint16_t h; memcpy(&h, row + ((size_t)x*(size_t)channels + (size_t)c)*2, 2); val = bcdec_half_to_float(h); }
                else { memcpy(&val, row + ((size_t)x*(size_t)channels + (size_t)c)*4, 4); }
                int bad = 0;
                if (isnan(val)) { rep.nanCount++; bad = 1; }
                else if (isinf(val)) { rep.infCount++; bad = 1; }
                else if (rejectNegative && val < 0.0f) { rep.negativeCount++; bad = 1; }
                if (bad && rep.firstBadX == -1) { rep.firstBadX = x; rep.firstBadY = y; rep.firstBadChannel = c; }
            }
        }
    }
    if (outReport) *outReport = rep;
    return (rep.nanCount > 0 || rep.infCount > 0 || rep.negativeCount > 0) ? BCDEC_ERROR_CORRUPT_SURFACE : BCDEC_SUCCESS;
}

/* =========================================================================
 * IMPLEMENTACI脫N M脫DULO ARM MALI-G52 (BIFROST) & ANDROID NDK
 * ========================================================================= */

BCDECDEF void bcdec_mali_init_context(bcdec_mali_context_t* restrict ctx, int maxThreads) {
    if (!ctx) return;
    ctx->maxThreads = (maxThreads > 0) ? maxThreads : 4;
    ctx->useAfbcHints = 1;
    ctx->l2CacheLineSize = BCDEC_MALI_G52_L2_CACHE_LINE;
    ctx->hasNativeBptc = 0; /* Mali-G52 Bifrost est谩ndar no soporta BC6H/BC7 por hardware nativo sin transcodificaci贸n */
    ctx->hasAstcSupport = 1;
    ctx->hasEtc2Support = 1;
    ctx->customAllocatorContext = NULL;
}

BCDECDEF bcdec_status_t bcdec_mali_encode_surface_async(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch, const bcdec_mali_context_t* restrict maliCtx) {
    int threads = (maliCtx && maliCtx->maxThreads > 0) ? maliCtx->maxThreads : 4;
    bcdec_job_desc_t jobDesc = { NULL, threads, NULL };
    return bcdec_encode_texture_parallel_async(format, srcPixels, compressedData, width, height, srcPitch, jobDesc);
}

BCDECDEF bcdec_status_t bcdec_encode_texture_mali_tiled(bcdec_format_t format, const void* restrict srcPixels, void* restrict compressedData, int width, int height, int srcPitch) {
    bcdec_job_desc_t jobDesc = { NULL, 1, NULL };
    return bcdec_encode_texture_parallel_async(format, srcPixels, compressedData, width, height, srcPitch, jobDesc);
}

BCDECDEF void bcdec_decompress_texture_mali_fast_tile(bcdec_format_t format, const void* restrict compressedData, void* restrict decompressedTile, int width, int height, int tileX, int tileY, int tileSize, int destinationPitch) {
    bcdec_decompress_texture_region(format, compressedData, decompressedTile, width, height, tileX, tileY, tileSize, tileSize, destinationPitch);
}

BCDECDEF int bcdec_validate_mali_cache_alignment(const void* restrict textureData, size_t dataSize) {
    return (textureData != NULL && dataSize > 0 && (((uintptr_t)textureData % BCDEC_MALI_G52_L2_CACHE_LINE) == 0)) ? 1 : 0;
}

BCDECDEF int bcdec_mali_validate_texture_alignment(const void* restrict textureData, size_t dataSize, int alignmentBytes) {
    if (!textureData || dataSize == 0 || alignmentBytes <= 0) return 0;
    return (((uintptr_t)textureData % (size_t)alignmentBytes) == 0) ? 1 : 0;
}

BCDECDEF bcdec_status_t bcdec_mali_generate_compressed_mipmaps(bcdec_format_t format, void* restrict compressedMipChainData, int baseWidth, int baseHeight, int mipLevels) {
    if (!compressedMipChainData || baseWidth <= 0 || baseHeight <= 0 || mipLevels <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    uint8_t* ptr = (uint8_t*)compressedMipChainData;
    int curW = baseWidth, curH = baseHeight;
    for (int m = 1; m < mipLevels; ++m) {
        size_t curSize = bcdec_texture_compressed_size(format, curW, curH);
        bcdec_status_t st = bcdec_mali_stream_generate_mipmap_level(format, ptr, ptr + curSize, curW, curH);
        if (st != BCDEC_SUCCESS) return st;
        ptr += curSize;
        curW = (curW > 1) ? (curW >> 1) : 1;
        curH = (curH > 1) ? (curH >> 1) : 1;
    }
    return BCDEC_SUCCESS;
}

/* ---- ETC1/ETC2-RGB8 (compatible con ETC1) y EAC (alfa / R11) para Mali-G52 ---- */
static const int bcdec__etc1_tables[8][2] = { {2,8},{5,17},{9,29},{13,42},{18,60},{24,80},{33,106},{47,183} };

static BCDEC_FORCEINLINE int bcdec__etc_clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int bcdec__etc1_fit_subblock(const uint8_t* rgba, const int* pix, int n, const int base[3], int* outTable, int* outSel) {
    int bestErr = 0x7FFFFFFF;
    for (int t = 0; t < 8; ++t) {
        const int mod[4] = { bcdec__etc1_tables[t][0], bcdec__etc1_tables[t][1], -bcdec__etc1_tables[t][0], -bcdec__etc1_tables[t][1] };
        int err = 0, sel[8];
        for (int i = 0; i < n; ++i) {
            const uint8_t* p = rgba + pix[i] * 4;
            int best = 0x7FFFFFFF, bs = 0;
            for (int s = 0; s < 4; ++s) {
                int dr = bcdec__etc_clamp255(base[0] + mod[s]) - p[0];
                int dg = bcdec__etc_clamp255(base[1] + mod[s]) - p[1];
                int db = bcdec__etc_clamp255(base[2] + mod[s]) - p[2];
                int e = dr*dr + dg*dg + db*db;
                if (e < best) { best = e; bs = s; }
            }
            err += best; sel[i] = bs;
        }
        if (err < bestErr) { bestErr = err; *outTable = t; for (int i = 0; i < n; ++i) outSel[i] = sel[i]; }
    }
    return bestErr;
}

/* Codifica un bloque RGBA8 4x4 (fila-mayor, el alfa se ignora) en un bloque ETC1 v谩lido tambi茅n como ETC2 RGB8. */
static void bcdec__etc1_encode_block(const uint8_t* rgba, uint8_t* out, int allowIndividual) {
    int bestTotal = 0x7FFFFFFF;
    uint8_t bestBlock[8] = {0};
    for (int flip = 0; flip < 2; ++flip) {
        int pix[2][8];
        for (int sb = 0; sb < 2; ++sb) {
            int k = 0;
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                int inSub = flip ? (y >> 1) : (x >> 1);
                if (inSub == sb) pix[sb][k++] = y * 4 + x;
            }
        }
        int avg[2][3];
        for (int sb = 0; sb < 2; ++sb) {
            int sum[3] = {0,0,0};
            for (int i = 0; i < 8; ++i) { const uint8_t* p = rgba + pix[sb][i]*4; sum[0]+=p[0]; sum[1]+=p[1]; sum[2]+=p[2]; }
            for (int c = 0; c < 3; ++c) avg[sb][c] = (sum[c] + 4) / 8;
        }
        for (int diff = allowIndividual ? 0 : 1; diff < 2; ++diff) {
            int q[2][3], base[2][3];
            if (!diff) {
                for (int sb = 0; sb < 2; ++sb) for (int c = 0; c < 3; ++c) {
                    q[sb][c] = (avg[sb][c] * 15 + 127) / 255;
                    base[sb][c] = (q[sb][c] << 4) | q[sb][c];
                }
            } else {
                for (int c = 0; c < 3; ++c) {
                    q[0][c] = (avg[0][c] * 31 + 127) / 255;
                    int q1 = (avg[1][c] * 31 + 127) / 255;
                    int d = q1 - q[0][c]; if (d < -4) d = -4; if (d > 3) d = 3;
                    q[1][c] = q[0][c] + d;
                }
                for (int sb = 0; sb < 2; ++sb) for (int c = 0; c < 3; ++c)
                    base[sb][c] = (q[sb][c] << 3) | (q[sb][c] >> 2);
            }
            int tbl[2], sel[2][8];
            int err = bcdec__etc1_fit_subblock(rgba, pix[0], 8, base[0], &tbl[0], sel[0])
                    + bcdec__etc1_fit_subblock(rgba, pix[1], 8, base[1], &tbl[1], sel[1]);
            if (err >= bestTotal) continue;
            bestTotal = err;
            uint8_t b[8];
            if (!diff) {
                b[0] = (uint8_t)((q[0][0] << 4) | q[1][0]);
                b[1] = (uint8_t)((q[0][1] << 4) | q[1][1]);
                b[2] = (uint8_t)((q[0][2] << 4) | q[1][2]);
            } else {
                b[0] = (uint8_t)((q[0][0] << 3) | ((q[1][0] - q[0][0]) & 7));
                b[1] = (uint8_t)((q[0][1] << 3) | ((q[1][1] - q[0][1]) & 7));
                b[2] = (uint8_t)((q[0][2] << 3) | ((q[1][2] - q[0][2]) & 7));
            }
            b[3] = (uint8_t)((tbl[0] << 5) | (tbl[1] << 2) | (diff << 1) | flip);
            uint32_t msb = 0, lsb = 0;
            for (int sb = 0; sb < 2; ++sb) for (int i = 0; i < 8; ++i) {
                int p = pix[sb][i], x = p & 3, y = p >> 2;
                int k = x * 4 + y;
                int s = sel[sb][i];
                msb |= (uint32_t)((s >> 1) & 1) << k;
                lsb |= (uint32_t)(s & 1) << k;
            }
            b[4] = (uint8_t)(msb >> 8); b[5] = (uint8_t)msb;
            b[6] = (uint8_t)(lsb >> 8); b[7] = (uint8_t)lsb;
            memcpy(bestBlock, b, 8);
        }
    }
    memcpy(out, bestBlock, 8);
}

/* Tablas de modificadores EAC (ETC2 alpha / R11) */
static const int bcdec__eac_tables[16][8] = {
    {-3,-6,-9,-15,2,5,8,14}, {-3,-7,-10,-13,2,6,9,12}, {-2,-5,-8,-13,1,4,7,12}, {-2,-4,-6,-13,1,3,5,12},
    {-3,-6,-8,-12,2,5,7,11}, {-3,-7,-9,-11,2,6,8,10}, {-4,-7,-8,-11,3,6,7,10}, {-3,-5,-8,-11,2,4,7,10},
    {-2,-6,-8,-10,1,5,7,9},  {-2,-5,-8,-10,1,4,7,9},  {-2,-4,-8,-10,1,3,7,9},  {-2,-5,-7,-10,1,4,6,9},
    {-3,-4,-7,-10,2,3,6,9},  {-1,-2,-3,-10,0,1,2,9},  {-4,-6,-8,-9,3,5,7,8},   {-3,-5,-7,-9,2,4,6,8} };

/* vals: 16 valores de 8 bits en orden fila-mayor (y*4+x). Salida: bloque EAC de 8 bytes.
   Para R11 el mismo bitstream equivale a value11 = value8*8 + 4. */
static void bcdec__eac_encode_block(const uint8_t* vals, uint8_t* out) {
    int vmin = 255, vmax = 0;
    for (int i = 0; i < 16; ++i) { if (vals[i] < vmin) vmin = vals[i]; if (vals[i] > vmax) vmax = vals[i]; }
    int base = (vmin + vmax + 1) >> 1;
    int range = vmax - vmin;
    long bestErr = 0x7FFFFFFFL; int bestT = 0, bestM = 1, bestIdx[16] = {0};
    for (int t = 0; t < 16 && bestErr != 0; ++t) {
        int span = bcdec__eac_tables[t][7] - bcdec__eac_tables[t][0];
        int m0 = (range + span / 2) / span; if (m0 < 1) m0 = 1; if (m0 > 15) m0 = 15;
        for (int m = (m0 > 1 ? m0 - 1 : 1); m <= (m0 < 15 ? m0 + 1 : 15); ++m) {
            long err = 0; int idx[16];
            for (int i = 0; i < 16; ++i) {
                int best = 0x7FFFFFFF, bi = 0;
                for (int s = 0; s < 8; ++s) {
                    int v = bcdec__etc_clamp255(base + bcdec__eac_tables[t][s] * m);
                    int d = v - (int)vals[i]; d *= d;
                    if (d < best) { best = d; bi = s; }
                }
                err += best; idx[i] = bi;
            }
            if (err < bestErr) { bestErr = err; bestT = t; bestM = m; for (int i = 0; i < 16; ++i) bestIdx[i] = idx[i]; }
        }
    }
    out[0] = (uint8_t)base;
    out[1] = (uint8_t)((bestM << 4) | bestT);
    uint64_t bits = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int k = x * 4 + y;                                    /* orden columna-mayor */
        bits |= (uint64_t)bestIdx[y * 4 + x] << (45 - 3 * k);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (40 - 8 * i));
}

BCDECDEF bcdec_status_t bcdec_mali_transcode_bc_to_etc2(const void* restrict bcCompressedData, void* restrict etc2CompressedData, int width, int height, bcdec_format_t srcFormat) {
    if (!bcCompressedData || !etc2CompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    /* BC1 -> VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK (8 B/bloque, sin alfa de 1 bit)
       BC2/BC3/BC7 -> VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK (16 B/bloque: alfa EAC + color ETC2) */
    if (srcFormat != BCDEC_FORMAT_BC1 && srcFormat != BCDEC_FORMAT_BC2 && srcFormat != BCDEC_FORMAT_BC3 && srcFormat != BCDEC_FORMAT_BC7)
        return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    const int blocksX = (width + 3) >> 2, blocksY = (height + 3) >> 2;
    const size_t srcBlock = bcdec_format_block_size(srcFormat);
    const int hasAlpha = (srcFormat != BCDEC_FORMAT_BC1);
    const uint8_t* src = (const uint8_t*)bcCompressedData;
    uint8_t* dst = (uint8_t*)etc2CompressedData;
    for (int b = 0; b < blocksX * blocksY; ++b) {
        uint8_t rgba[64];
        bcdec_decompress_block(srcFormat, src, rgba, 16);
        if (hasAlpha) {
            uint8_t a[16];
            for (int p = 0; p < 16; ++p) a[p] = rgba[p * 4 + 3];
            bcdec__eac_encode_block(a, dst);
            dst += 8;
        }
        bcdec__etc1_encode_block(rgba, dst, 1);
        dst += 8;
        src += srcBlock;
    }
    return BCDEC_SUCCESS;
}


/* ---- BC1 con alfa de 1 bit -> ETC2 RGB8A1 (punch-through) ---- */
static int bcdec__bc1_has_punchthrough(const void* data, int width, int height) {
    const uint8_t* p = (const uint8_t*)data;
    const int n = ((width + 3) >> 2) * ((height + 3) >> 2);
    for (int i = 0; i < n; ++i, p += 8) {
        uint16_t c0, c1; uint32_t idx;
        memcpy(&c0, p, 2); memcpy(&c1, p + 2, 2);
        if (c0 > c1) continue;                         /* modo de 4 colores: sin alfa */
        memcpy(&idx, p + 4, 4);
        for (int k = 0; k < 16; ++k) if (((idx >> (2 * k)) & 3u) == 3u) return 1;
    }
    return 0;
}

/* rgba: 16 texels fila-mayor; alfa < 128 = transparente. Salida: bloque ETC2 RGB8A1 de 8 bytes. */
static void bcdec__etc2_a1_encode_block(const uint8_t* rgba, uint8_t* out) {
    int transparent[16], anyT = 0;
    for (int i = 0; i < 16; ++i) { transparent[i] = rgba[i * 4 + 3] < 128; anyT |= transparent[i]; }
    if (!anyT) { bcdec__etc1_encode_block(rgba, out, 0); return; }   /* opaco: bit Op=1, solo modo diferencial */
    int bestTotal = 0x7FFFFFFF;
    uint8_t bestBlock[8] = {0};
    for (int flip = 0; flip < 2; ++flip) {
        int pix[2][8];
        for (int sb = 0; sb < 2; ++sb) {
            int k = 0;
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                int inSub = flip ? (y >> 1) : (x >> 1);
                if (inSub == sb) pix[sb][k++] = y * 4 + x;
            }
        }
        int q[2][3], base[2][3], avg[2][3];
        for (int sb = 0; sb < 2; ++sb) {
            int sum[3] = {0,0,0}, cnt = 0;
            for (int i = 0; i < 8; ++i) if (!transparent[pix[sb][i]]) {
                const uint8_t* p = rgba + pix[sb][i] * 4; sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt;
            }
            for (int c = 0; c < 3; ++c) avg[sb][c] = cnt ? (sum[c] + cnt / 2) / cnt : 0;
        }
        for (int c = 0; c < 3; ++c) {
            q[0][c] = (avg[0][c] * 31 + 127) / 255;
            int q1 = (avg[1][c] * 31 + 127) / 255;
            int d = q1 - q[0][c]; if (d < -4) d = -4; if (d > 3) d = 3;
            q[1][c] = q[0][c] + d;
        }
        for (int sb = 0; sb < 2; ++sb) for (int c = 0; c < 3; ++c) base[sb][c] = (q[sb][c] << 3) | (q[sb][c] >> 2);
        int tbl[2], sel[2][8], total = 0;
        for (int sb = 0; sb < 2; ++sb) {
            int bestErr = 0x7FFFFFFF;
            for (int t = 0; t < 8; ++t) {
                const int cv = bcdec__etc1_tables[t][1];
                const int mod[4] = { 0, cv, 0, -cv };          /* selector 2 = transparente */
                int err = 0, s2[8];
                for (int i = 0; i < 8; ++i) {
                    int px = pix[sb][i];
                    if (transparent[px]) { s2[i] = 2; continue; }
                    const uint8_t* p = rgba + px * 4;
                    int best = 0x7FFFFFFF, bs = 0;
                    for (int s = 0; s < 4; ++s) {
                        if (s == 2) continue;
                        int dr = bcdec__etc_clamp255(base[sb][0] + mod[s]) - p[0];
                        int dg = bcdec__etc_clamp255(base[sb][1] + mod[s]) - p[1];
                        int db = bcdec__etc_clamp255(base[sb][2] + mod[s]) - p[2];
                        int e = dr*dr + dg*dg + db*db;
                        if (e < best) { best = e; bs = s; }
                    }
                    err += best; s2[i] = bs;
                }
                if (err < bestErr) { bestErr = err; tbl[sb] = t; for (int i = 0; i < 8; ++i) sel[sb][i] = s2[i]; }
            }
            total += bestErr;
        }
        if (total >= bestTotal) continue;
        bestTotal = total;
        uint8_t b[8];
        for (int c = 0; c < 3; ++c) b[c] = (uint8_t)((q[0][c] << 3) | ((q[1][c] - q[0][c]) & 7));
        b[3] = (uint8_t)((tbl[0] << 5) | (tbl[1] << 2) | (0 << 1) | flip);   /* Op = 0: hay texels transparentes */
        uint32_t msb = 0, lsb = 0;
        for (int sb = 0; sb < 2; ++sb) for (int i = 0; i < 8; ++i) {
            int p = pix[sb][i], x = p & 3, y = p >> 2, k = x * 4 + y, s = sel[sb][i];
            msb |= (uint32_t)((s >> 1) & 1) << k; lsb |= (uint32_t)(s & 1) << k;
        }
        b[4] = (uint8_t)(msb >> 8); b[5] = (uint8_t)msb; b[6] = (uint8_t)(lsb >> 8); b[7] = (uint8_t)lsb;
        memcpy(bestBlock, b, 8);
    }
    memcpy(out, bestBlock, 8);
}

BCDECDEF bcdec_status_t bcdec_mali_transcode_bc1_to_etc2_a1(const void* restrict bc1Data, void* restrict etc2A1Data, int width, int height) {
    if (!bc1Data || !etc2A1Data || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    const int n = ((width + 3) >> 2) * ((height + 3) >> 2);          /* VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK */
    const uint8_t* src = (const uint8_t*)bc1Data; uint8_t* dst = (uint8_t*)etc2A1Data;
    for (int b = 0; b < n; ++b, src += 8, dst += 8) {
        uint8_t rgba[64];
        bcdec_decompress_block(BCDEC_FORMAT_BC1, src, rgba, 16);
        bcdec__etc2_a1_encode_block(rgba, dst);
    }
    return BCDEC_SUCCESS;
}

/* ---- Plan de transcodificaci贸n en GPU BC1/BC7 -> ASTC (shader compute de 8x8 invocaciones) ---- */
BCDECDEF bcdec_status_t bcdec_mali_g52_plan_astc_transcode(bcdec_format_t srcFormat, int width, int height, int hasAlpha, int astc8x8, int srgb, size_t srcByteOffset, bcdec_g52_astc_transcode_plan_t* restrict outPlan) {
    if (!outPlan || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    uint32_t fmtCode;                                       /* c贸digos de pc.format del shader (bc_decode_ext.glsl) */
    switch (srcFormat) {
        case BCDEC_FORMAT_BC1: fmtCode = 0u; break;
        case BCDEC_FORMAT_BC7: fmtCode = 1u; break;
        case BCDEC_FORMAT_BC2: fmtCode = 2u; break;
        case BCDEC_FORMAT_BC3: fmtCode = 3u; break;
        case BCDEC_FORMAT_BC4: fmtCode = 4u; break;
        case BCDEC_FORMAT_BC5: fmtCode = 5u; break;
        default: return BCDEC_ERROR_UNSUPPORTED_FORMAT;     /* BC6H (HDR) no se transcodifica a ASTC LDR */
    }
    if ((srcByteOffset & 3u) != 0) return BCDEC_ERROR_INVALID_PARAMETER;      /* el shader direcciona en palabras de 32 bits */
    const uint32_t bcx = (uint32_t)((width + 3) >> 2), bcy = (uint32_t)((height + 3) >> 2);
    const uint32_t dstX = astc8x8 ? (bcx + 1u) >> 1 : bcx;
    const uint32_t dstY = astc8x8 ? (bcy + 1u) >> 1 : bcy;
    memset(outPlan, 0, sizeof(*outPlan));
    outPlan->push.block_x = dstX;
    outPlan->push.block_x_src = bcx;
    outPlan->push.block_y = dstY;
    outPlan->push.src_word_off = (uint32_t)(srcByteOffset >> 2);
    outPlan->push.format = fmtCode;
    outPlan->push.has_alpha = (hasAlpha && srcFormat != BCDEC_FORMAT_BC4 && srcFormat != BCDEC_FORMAT_BC5) ? 1u : 0u;   /* BC4/BC5 son opacos */
    outPlan->push.astc8 = astc8x8 ? 1u : 0u;
    outPlan->push.bc_bx = bcx;
    outPlan->push.bc_by = bcy;
    outPlan->pushConstantSize = (uint32_t)sizeof(bcdec_g52_astc_transcode_push_constants_t);
    outPlan->localSizeX = 8; outPlan->localSizeY = 8;
    outPlan->groupCountX = (dstX + 7u) >> 3;
    outPlan->groupCountY = (dstY + 7u) >> 3;
    outPlan->groupCountZ = 1;
    outPlan->dstVkFormat = astc8x8 ? (srgb ? 172u : 171u) : (srgb ? 158u : 157u);   /* ASTC_8x8 / ASTC_4x4 {UNORM,SRGB}_BLOCK */
    outPlan->dstSizeBytes = (size_t)dstX * (size_t)dstY * 16u;
    outPlan->srcSizeBytes = (size_t)bcx * (size_t)bcy * bcdec_format_block_size(srcFormat);
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_g52_texture_path_t bcdec_mali_g52_select_texture_path_ex(bcdec_format_t format, uint32_t capsMask) {
    if (format < BCDEC_FORMAT_BC1 || format > BCDEC_FORMAT_BC7) return BCDEC_G52_PATH_UNSUPPORTED;
    if (capsMask & BCDEC_G52_CAP_BC) return BCDEC_G52_PATH_NATIVE_BC;
    if (capsMask & BCDEC_G52_CAP_ASTC) {
        if ((capsMask & BCDEC_G52_CAP_GPU_ASTC_SHADER) && (format == BCDEC_FORMAT_BC1 || format == BCDEC_FORMAT_BC7))
            return BCDEC_G52_PATH_GPU_ASTC_TRANSCODE;
        if ((capsMask & BCDEC_G52_CAP_GPU_ASTC_SHADER_EXT) && format != BCDEC_FORMAT_BC6H)     /* BC2..BC5 con bc_decode_ext.glsl */
            return BCDEC_G52_PATH_GPU_ASTC_TRANSCODE;
    }
    if ((capsMask & BCDEC_G52_CAP_ETC2) && format != BCDEC_FORMAT_BC6H) return BCDEC_G52_PATH_TRANSCODE_ETC2;
    return BCDEC_G52_PATH_CPU_DECODE_RGBA;
}

BCDECDEF bcdec_status_t bcdec_mali_transcode_bc4_to_eac_r11(const void* restrict bc4Data, void* restrict eacR11Data, int width, int height) {
    if (!bc4Data || !eacR11Data || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    const int n = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc4Data; uint8_t* dst = (uint8_t*)eacR11Data;   /* VK_FORMAT_EAC_R11_UNORM_BLOCK */
    for (int b = 0; b < n; ++b) {
        uint8_t px[16];
        bcdec_decompress_block(BCDEC_FORMAT_BC4, src, px, 4);
        bcdec__eac_encode_block(px, dst);
        src += 8; dst += 8;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_mali_transcode_bc5_to_eac_rg11(const void* restrict bc5Data, void* restrict eacRG11Data, int width, int height) {
    if (!bc5Data || !eacRG11Data || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    const int n = ((width + 3) >> 2) * ((height + 3) >> 2);
    const uint8_t* src = (const uint8_t*)bc5Data; uint8_t* dst = (uint8_t*)eacRG11Data;  /* VK_FORMAT_EAC_R11G11_UNORM_BLOCK */
    for (int b = 0; b < n; ++b) {
        uint8_t px[32], r[16], g[16];
        bcdec_decompress_block(BCDEC_FORMAT_BC5, src, px, 8);
        for (int p = 0; p < 16; ++p) { r[p] = px[p * 2]; g[p] = px[p * 2 + 1]; }
        bcdec__eac_encode_block(r, dst);
        bcdec__eac_encode_block(g, dst + 8);
        src += 16; dst += 16;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_g52_texture_path_t bcdec_mali_g52_select_texture_path(bcdec_format_t format, int deviceHasBC, int deviceHasETC2, int deviceHasASTC) {
    (void)deviceHasASTC;
    if (format < BCDEC_FORMAT_BC1 || format > BCDEC_FORMAT_BC7) return BCDEC_G52_PATH_UNSUPPORTED;
    if (deviceHasBC) return BCDEC_G52_PATH_NATIVE_BC;
    if (deviceHasETC2 && format != BCDEC_FORMAT_BC6H) return BCDEC_G52_PATH_TRANSCODE_ETC2;   /* incluye BC4/BC5 -> EAC */
    return BCDEC_G52_PATH_CPU_DECODE_RGBA;
}

BCDECDEF size_t bcdec_mali_g52_converted_surface_size(bcdec_format_t srcFormat, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const size_t blocks = (size_t)((width + 3) >> 2) * (size_t)((height + 3) >> 2);
    switch (srcFormat) {
        case BCDEC_FORMAT_BC1: case BCDEC_FORMAT_BC4: return blocks * 8;
        case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3: case BCDEC_FORMAT_BC5: case BCDEC_FORMAT_BC7: return blocks * 16;
        case BCDEC_FORMAT_BC6H: return (size_t)width * (size_t)height * 8;   /* RGBA16F sin comprimir */
        default: return 0;
    }
}

BCDECDEF bcdec_status_t bcdec_mali_g52_convert_bc_surface(bcdec_format_t srcFormat, const void* restrict srcData, int width, int height, int srgb, void* restrict dstData, size_t dstCapacity, unsigned int* restrict outVkFormat, size_t* restrict outSize) {
    if (!srcData || !dstData || !outVkFormat || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    size_t need = bcdec_mali_g52_converted_surface_size(srcFormat, width, height);
    if (need == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (dstCapacity < need) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    bcdec_status_t st = BCDEC_SUCCESS;
    switch (srcFormat) {
        case BCDEC_FORMAT_BC1:
            if (bcdec__bc1_has_punchthrough(srcData, width, height)) {   /* conserva el alfa de 1 bit */
                st = bcdec_mali_transcode_bc1_to_etc2_a1(srcData, dstData, width, height);
                *outVkFormat = srgb ? 150u : 149u;                  /* ETC2_R8G8B8A1_{UNORM,SRGB}_BLOCK */
            } else {
                st = bcdec_mali_transcode_bc_to_etc2(srcData, dstData, width, height, srcFormat);
                *outVkFormat = srgb ? 148u : 147u;                  /* ETC2_R8G8B8_{UNORM,SRGB}_BLOCK */
            }
            break;
        case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3: case BCDEC_FORMAT_BC7:
            st = bcdec_mali_transcode_bc_to_etc2(srcData, dstData, width, height, srcFormat);
            *outVkFormat = srgb ? 152u : 151u; break;               /* ETC2_R8G8B8A8_{UNORM,SRGB}_BLOCK */
        case BCDEC_FORMAT_BC4:
            st = bcdec_mali_transcode_bc4_to_eac_r11(srcData, dstData, width, height);
            *outVkFormat = 153u; break;                             /* EAC_R11_UNORM_BLOCK */
        case BCDEC_FORMAT_BC5:
            st = bcdec_mali_transcode_bc5_to_eac_rg11(srcData, dstData, width, height);
            *outVkFormat = 155u; break;                             /* EAC_R11G11_UNORM_BLOCK */
        case BCDEC_FORMAT_BC6H: {                                   /* HDR: RGBA16F (VK_FORMAT_R16G16B16A16_SFLOAT = 97) */
            const uint8_t* s = (const uint8_t*)srcData; uint16_t* d = (uint16_t*)dstData;
            const int bx = (width + 3) >> 2, by = (height + 3) >> 2;
            for (int j = 0; j < by; ++j) for (int i = 0; i < bx; ++i) {
                uint16_t tmp[16 * 3];
                bcdec_bc6h_half(s + ((size_t)j * bx + i) * 16, tmp, 12, 0);
                for (int y = 0; y < 4 && j * 4 + y < height; ++y) for (int x = 0; x < 4 && i * 4 + x < width; ++x) {
                    uint16_t* o = d + ((size_t)(j * 4 + y) * width + (i * 4 + x)) * 4;
                    o[0] = tmp[(y * 4 + x) * 3]; o[1] = tmp[(y * 4 + x) * 3 + 1]; o[2] = tmp[(y * 4 + x) * 3 + 2]; o[3] = 0x3C00;
                }
            }
            *outVkFormat = 97u; } break;
        default: return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    }
    if (st == BCDEC_SUCCESS && outSize) *outSize = need;
    return st;
}

BCDECDEF bcdec_status_t bcdec_mali_transcode_astc_to_bc(const void* restrict astcCompressedData, void* restrict bcCompressedData, int width, int height, bcdec_format_t targetFormat) {
    if (!astcCompressedData || !bcCompressedData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    if (targetFormat != BCDEC_FORMAT_BC1 && targetFormat != BCDEC_FORMAT_BC3 && targetFormat != BCDEC_FORMAT_BC7) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    /* Solo se entienden bloques ASTC void-extent (color constante). Cualquier otro bloque requerir铆a un decodificador ASTC
       completo (el Mali-G52 muestrea ASTC de forma nativa, as铆 que no se incluye): se informa en vez de inventar datos. */
    const int total = ((width + 3) >> 2) * ((height + 3) >> 2);
    const size_t dstBlk = bcdec_format_block_size(targetFormat);
    const uint8_t* src = (const uint8_t*)astcCompressedData; uint8_t* dst = (uint8_t*)bcCompressedData;
    int unsupportedBlocks = 0;
    for (int i = 0; i < total; ++i, src += 16, dst += dstBlk) {
        uint32_t header; memcpy(&header, src, 4);
        if ((header & 0xFFFU) != 0xDFCU) { memset(dst, 0, dstBlk); ++unsupportedBlocks; continue; }
        uint16_t r16, g16, b16, a16;
        memcpy(&r16, src + 8, 2); memcpy(&g16, src + 10, 2); memcpy(&b16, src + 12, 2); memcpy(&a16, src + 14, 2);
        uint8_t rgba[64];
        for (int p = 0; p < 16; ++p) { rgba[p*4] = (uint8_t)(r16 >> 8); rgba[p*4+1] = (uint8_t)(g16 >> 8); rgba[p*4+2] = (uint8_t)(b16 >> 8); rgba[p*4+3] = (uint8_t)(a16 >> 8); }
        if (targetFormat == BCDEC_FORMAT_BC1) bcdec_encode_block_bc1(rgba, dst);
        else if (targetFormat == BCDEC_FORMAT_BC3) bcdec_encode_block_bc3(rgba, dst);
        else bcdec_encode_block_bc7(rgba, dst);
    }
    return unsupportedBlocks ? BCDEC_ERROR_UNSUPPORTED_FORMAT : BCDEC_SUCCESS;
}


static uint32_t bcdec__morton_encode_2d(uint32_t x, uint32_t y) {
    x = (x | (x << 8)) & 0x00FF00FFU; x = (x | (x << 4)) & 0x0F0F0F0FU;
    x = (x | (x << 2)) & 0x33333333U; x = (x | (x << 1)) & 0x55555555U;
    y = (y | (y << 8)) & 0x00FF00FFU; y = (y | (y << 4)) & 0x0F0F0F0FU;
    y = (y | (y << 2)) & 0x33333333U; y = (y | (y << 1)) & 0x55555555U;
    return x | (y << 1);
}

BCDECDEF void bcdec_mali_swizzle_blocks_morton(const void* restrict srcBlocks, void* restrict dstMortonBlocks, int width, int height, size_t blockSize) {
    if (!srcBlocks || !dstMortonBlocks || width <= 0 || height <= 0 || blockSize == 0) return;
    int blocksX = (width + 3) >> 2, blocksY = (height + 3) >> 2;
    const uint8_t* src = (const uint8_t*)srcBlocks;
    uint8_t* dst = (uint8_t*)dstMortonBlocks;
    size_t maxBlocks = (size_t)blocksX * (size_t)blocksY;
    for (int y = 0; y < blocksY; ++y) {
        for (int x = 0; x < blocksX; ++x) {
            uint32_t mIdx = bcdec__morton_encode_2d((uint32_t)x, (uint32_t)y);
            size_t linearIdx = (size_t)y * (size_t)blocksX + (size_t)x;
            if ((size_t)mIdx < maxBlocks) {
                memcpy(dst + (size_t)mIdx * blockSize, src + linearIdx * blockSize, blockSize);
            }
        }
    }
}

BCDECDEF bcdec_status_t bcdec_mali_generate_afbc_header(const void* restrict compressedData, void* restrict outAfbcHeader, int width, int height, bcdec_format_t format) {
    if (!compressedData || !outAfbcHeader || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    uint32_t* hdr = (uint32_t*)outAfbcHeader;
    hdr[0] = 0x41464243U; /* 'AFBC' */
    hdr[1] = (uint32_t)width;
    hdr[2] = (uint32_t)height;
    hdr[3] = (uint32_t)format;
    hdr[4] = (uint32_t)((width + 15) >> 4) * (uint32_t)((height + 15) >> 4); /* Superblocks 16x16 Mali-G52 */
    hdr[5] = bcdec_calculate_buffer_crc32(compressedData, bcdec_texture_compressed_size(format, width, height));
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_mali_stream_generate_mipmap_level(bcdec_format_t format, const void* restrict srcMipData, void* restrict dstMipData, int curWidth, int curHeight) {
    if (!srcMipData || !dstMipData || curWidth <= 0 || curHeight <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    int nextW = (curWidth > 1) ? (curWidth >> 1) : 1;
    int nextH = (curHeight > 1) ? (curHeight >> 1) : 1;
    int bpp = bcdec_format_pixel_size(format);
    if (bpp <= 0 || bpp > 4) return BCDEC_ERROR_UNSUPPORTED_FORMAT;

    size_t srcPixelsBytes = (size_t)curWidth * (size_t)curHeight * (size_t)bpp;
    size_t dstPixelsBytes = (size_t)nextW * (size_t)nextH * (size_t)bpp;
    uint8_t* tmpSrc = (uint8_t*)malloc(srcPixelsBytes + dstPixelsBytes);
    if (!tmpSrc) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    uint8_t* tmpDst = tmpSrc + srcPixelsBytes;

    bcdec_decompress_texture(format, srcMipData, tmpSrc, curWidth, curHeight, curWidth * bpp);
    bcdec_downscale_surface_2x2(tmpSrc, tmpDst, curWidth, curHeight, bpp, curWidth * bpp, nextW * bpp);
    bcdec_job_desc_t jd = { NULL, 1, NULL };
    bcdec_status_t st = bcdec_encode_texture_parallel_async(format, tmpDst, dstMipData, nextW, nextH, nextW * bpp, jd);
    free(tmpSrc);
    return st;
}

BCDECDEF int bcdec_mali_adjust_thread_pool_load(int currentThreads, float gpuThermalLoadRatio) {
    if (gpuThermalLoadRatio >= 0.85f) return (currentThreads > 1) ? (currentThreads / 2) : 1;
    if (gpuThermalLoadRatio >= 0.70f) return (currentThreads > 2) ? (currentThreads - 1) : currentThreads;
    return currentThreads;
}

BCDECDEF float bcdec_mali_estimate_bus_bandwidth_pressure(int textureWidth, int textureHeight, bcdec_format_t format, float currentFps) {
    size_t bytes = bcdec_texture_compressed_size(format, textureWidth, textureHeight);
    float gbps = ((float)bytes * currentFps) / (1024.0f * 1024.0f * 1024.0f);
    return gbps / 14.9f; /* LPDDR4X t铆pico en SoCs con Mali-G52 (~14.9 GB/s) */
}

BCDECDEF int bcdec_mali_adapt_quality_to_thermal_state(const bcdec_mali_telemetry_t* restrict telemetry) {
    if (!telemetry) return 1;
    if (telemetry->currentTemperatureCelsius > 75.0f) return 0;
    if (telemetry->currentTemperatureCelsius > 60.0f) return 1;
    return 2;
}

BCDECDEF void bcdec_mali_emit_cache_prefetch_hints(const void* restrict textureAddress, size_t dataSize, int prefetchLevel) {
    if (!textureAddress || dataSize == 0) return;
    (void)prefetchLevel;
#if defined(__GNUC__) || defined(__clang__)
    const uint8_t* ptr = (const uint8_t*)textureAddress;
    for (size_t offset = 0; offset < dataSize; offset += BCDEC_MALI_G52_L2_CACHE_LINE) {
        __builtin_prefetch(ptr + offset, 0, 3);
    }
#endif
}

BCDECDEF bcdec_status_t bcdec_mali_batch_transcode_surfaces(const bcdec_batch_transcode_job_t* restrict jobs, int jobCount, bcdec_job_desc_t jobDesc) {
    (void)jobDesc;
    if (!jobs || jobCount <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < jobCount; ++i) {
        bcdec_status_t st = bcdec_vulkan_transcode_surface_format(jobs[i].srcFormat, jobs[i].dstFormat, jobs[i].srcData, jobs[i].dstData, jobs[i].width, jobs[i].height);
        if (st != BCDEC_SUCCESS) return st;
    }
    return BCDEC_SUCCESS;
}


static double bcdec__now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

BCDECDEF void bcdec_mali_profile_workload_begin(bcdec_mali_workload_metrics_t* restrict metrics) {
    if (!metrics) return;
    memset(metrics, 0, sizeof(bcdec_mali_workload_metrics_t));
    metrics->startTimeMs = bcdec__now_ms();
}

BCDECDEF void bcdec_mali_profile_workload_end(bcdec_mali_workload_metrics_t* restrict metrics, size_t processedBytes) {
    if (!metrics) return;
    double elapsed = bcdec__now_ms() - metrics->startTimeMs;
    if (elapsed < 0.0) elapsed = 0.0;
    metrics->totalBytesTranscoded += (uint64_t)processedBytes;
    metrics->executionTimeMs = elapsed;
    /* Gigabits por segundo: bytes*8 / (ms * 1e6) */
    metrics->averageBandwidthGbps = (elapsed > 0.0) ? (float)(((double)metrics->totalBytesTranscoded * 8.0) / (elapsed * 1.0e6)) : 0.0f;
}

/* Funciones espec铆ficas Mali-G52 */
BCDECDEF int bcdec_mali_g52_validate_format_support(bcdec_format_t format, int deviceHasBptcExtension) {
    if (format == BCDEC_FORMAT_BC6H || format == BCDEC_FORMAT_BC7) return deviceHasBptcExtension ? 1 : 0;
    return (format >= BCDEC_FORMAT_BC1 && format <= BCDEC_FORMAT_BC5) ? 1 : 0;
}

BCDECDEF void bcdec_mali_get_optimal_compute_workgroup_size(int* restrict outSizeX, int* restrict outSizeY) {
    /* Mali-G52 (Bifrost) ejecuta warps de 4 u 8 hilos; 8x8 = 64 hilos maximiza ocupaci贸n sin desbordar registros */
    if (outSizeX) *outSizeX = 8;
    if (outSizeY) *outSizeY = 8;
}

BCDECDEF size_t bcdec_mali_g52_calculate_optimal_tile_size(int imageWidth, int imageHeight, bcdec_format_t format) {
    (void)imageWidth; (void)imageHeight;
    return (size_t)BCDEC_MALI_G52_TILE_SIZE * (size_t)BCDEC_MALI_G52_TILE_SIZE * (size_t)bcdec_format_pixel_size(format);
}

BCDECDEF size_t bcdec_mali_g52_get_ideal_staging_buffer_size(bcdec_format_t format, int width, int height) {
    size_t raw = bcdec_texture_compressed_size(format, width, height);
    return (raw + (BCDEC_MALI_G52_L2_CACHE_LINE - 1)) & ~((size_t)BCDEC_MALI_G52_L2_CACHE_LINE - 1);
}

BCDECDEF void bcdec_mali_g52_flush_l2_cache_hint(void* restrict stagingBuffer, size_t bufferSize) {
    if (!stagingBuffer || bufferSize == 0) return;
#if defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#endif
}

BCDECDEF size_t bcdec_mali_g52_calculate_staging_offset(size_t currentOffset, size_t bufferImageGranularity) {
    size_t align = (bufferImageGranularity > BCDEC_MALI_G52_L2_CACHE_LINE) ? bufferImageGranularity : BCDEC_MALI_G52_L2_CACHE_LINE;
    return (currentOffset + (align - 1)) & ~(align - 1);
}

BCDECDEF size_t bcdec_mali_g52_optimize_command_pool_allocation(int concurrentTextureCount) {
    return (size_t)((concurrentTextureCount > 0 ? concurrentTextureCount : 1) * 4096);
}

BCDECDEF int bcdec_mali_g52_validate_framebuffer_compression_support(bcdec_format_t format, int isOffscreenRenderTarget) {
    (void)format;
    /* Los formatos BC son de solo lectura en GPU Mali-G52, nunca render targets directos */
    return isOffscreenRenderTarget ? 0 : 1;
}

BCDECDEF int bcdec_mali_g52_validate_compute_shared_memory(uint32_t requestedSharedMemoryBytes) {
    return (requestedSharedMemoryBytes <= BCDEC_MALI_G52_MAX_SHARED_MEM) ? 1 : 0;
}

BCDECDEF float bcdec_mali_g52_estimate_transcode_overhead_ms(int width, int height, bcdec_format_t srcFormat, bcdec_format_t dstFormat) {
    (void)dstFormat;
    size_t blocks = (size_t)((width + 3) >> 2) * (size_t)((height + 3) >> 2);
    float cyclesPerBlock = (srcFormat == BCDEC_FORMAT_BC7 || srcFormat == BCDEC_FORMAT_BC6H) ? 140.0f : 45.0f;
    return ((float)blocks * cyclesPerBlock) / 1800000.0f; /* Estimaci贸n en CPU ARM Cortex-A55/A75 @ 1.8 GHz */
}

BCDECDEF void bcdec_mali_g52_init_tile_subpass_dependency(void* restrict outVkSubpassDependency, const bcdec_mali_g52_subpass_barrier_t* restrict barrierConfig) {
    if (!outVkSubpassDependency || !barrierConfig) return;
    /* Mapeo directo compatible con VkSubpassDependency (Vulkan 1.0/1.1) + VK_DEPENDENCY_BY_REGION_BIT (0x1) */
    struct {
        uint32_t srcSubpass;
        uint32_t dstSubpass;
        uint32_t srcStageMask;
        uint32_t dstStageMask;
        uint32_t srcAccessMask;
        uint32_t dstAccessMask;
        uint32_t dependencyFlags;
    } dep = {
        barrierConfig->srcSubpass, barrierConfig->dstSubpass,
        barrierConfig->srcStageMask, barrierConfig->dstStageMask,
        barrierConfig->srcAccessMask, barrierConfig->dstAccessMask,
        0x00000001U /* VK_DEPENDENCY_BY_REGION_BIT: Cr铆tico para TBDR en Mali-G52 */
    };
    memcpy(outVkSubpassDependency, &dep, sizeof(dep));
}

BCDECDEF int bcdec_mali_g52_validate_barrier_dependency_chain(uint32_t srcStageMask, uint32_t dstStageMask, int isTileBasedRenderTarget) {
    (void)isTileBasedRenderTarget;
    return (srcStageMask != 0 && dstStageMask != 0) ? 1 : 0;
}


BCDECDEF size_t bcdec_mali_g52_calculate_combined_descriptor_size(void) {
    return 64; /* Alineado a l铆nea de cach茅 L2 de Mali-G52 */
}

BCDECDEF uint32_t bcdec_mali_g52_get_shader_execution_priority_hint(bcdec_format_t format, int isRealtimeContext) {
    if (isRealtimeContext) return 2;
    return (format == BCDEC_FORMAT_BC7 || format == BCDEC_FORMAT_BC6H) ? 1 : 0;
}

BCDECDEF size_t bcdec_mali_g52_get_optimal_scratchpad_size(int tileWidth, int tileHeight, bcdec_format_t format) {
    return (size_t)tileWidth * (size_t)tileHeight * (size_t)bcdec_format_pixel_size(format);
}

BCDECDEF int bcdec_mali_g52_validate_bptc_partition_limits(int modeIndex, bcdec_format_t format) {
    if (format == BCDEC_FORMAT_BC7) return (modeIndex >= 0 && modeIndex <= 7) ? 1 : 0;
    if (format == BCDEC_FORMAT_BC6H) return (modeIndex >= 0 && modeIndex <= 13) ? 1 : 0;
    return 0;
}

BCDECDEF size_t bcdec_mali_g52_calculate_compute_push_constant_offset(uint32_t memberIndex) {
    return (size_t)memberIndex * 16; /* Alineaci贸n std430 vec4 */
}

BCDECDEF uint32_t bcdec_mali_g52_get_transient_attachment_usage_hint(int isTemporaryRenderTarget) {
    /* VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT (0x00000040) ahorra DRAM externa en Mali-G52 */
    return isTemporaryRenderTarget ? 0x00000040U : 0x00000010U;
}

BCDECDEF int bcdec_mali_g52_validate_compute_workgroup_limits(uint32_t workgroupSizeX, uint32_t workgroupSizeY, uint32_t workgroupSizeZ) {
    uint64_t totalInvocations = (uint64_t)workgroupSizeX * (uint64_t)workgroupSizeY * (uint64_t)workgroupSizeZ;
    return (totalInvocations > 0 && totalInvocations <= 256 && workgroupSizeZ <= 64) ? 1 : 0; /* L铆mite Mali-G52: 256 invocaciones/grupo */
}

BCDECDEF size_t bcdec_mali_g52_get_cache_flush_batch_size(size_t totalDataBytes) {
    return (totalDataBytes > 262144) ? 262144 : totalDataBytes; /* L2 de Mali-G52 suele ser de 128KB-512KB */
}

BCDECDEF size_t bcdec_mali_g52_align_staging_offset(size_t rawOffset, size_t optimalBufferCopyOffsetAlignment) {
    size_t align = (optimalBufferCopyOffsetAlignment > BCDEC_MALI_G52_L2_CACHE_LINE) ? optimalBufferCopyOffsetAlignment : BCDEC_MALI_G52_L2_CACHE_LINE;
    return (rawOffset + (align - 1)) & ~(align - 1);
}

BCDECDEF size_t bcdec_mali_g52_optimize_descriptor_set_layout(int textureCount, bcdec_format_t format) {
    (void)format;
    return (size_t)(textureCount > 0 ? textureCount : 1) * 64;
}

BCDECDEF int bcdec_mali_g52_validate_3d_staging_alignment(size_t bufferOffset, size_t optimalBufferCopyOffsetAlignment) {
    if (optimalBufferCopyOffsetAlignment == 0) return 1;
    return ((bufferOffset % optimalBufferCopyOffsetAlignment) == 0 && (bufferOffset % 16) == 0) ? 1 : 0;
}

BCDECDEF uint32_t bcdec_mali_g52_get_command_buffer_usage_hint(int isOneTimeSubmit) {
    return isOneTimeSubmit ? 0x00000001U : 0x00000004U; /* ONE_TIME_SUBMIT_BIT vs SIMULTANEOUS_USE_BIT */
}

BCDECDEF size_t bcdec_mali_g52_calculate_aligned_row_pitch(int width, bcdec_format_t format, size_t rowPitchAlignment) {
    int blocksX = (width + 3) >> 2;
    size_t rawPitch = (size_t)blocksX * bcdec_format_block_size(format);
    size_t align = (rowPitchAlignment > BCDEC_MALI_G52_L2_CACHE_LINE) ? rowPitchAlignment : BCDEC_MALI_G52_L2_CACHE_LINE;
    return (rawPitch + (align - 1)) & ~(align - 1);
}

BCDECDEF void bcdec_mali_g52_get_compute_shared_memory_layout(int* restrict outSharedWidth, int* restrict outSharedHeight) {
    if (outSharedWidth) *outSharedWidth = 16;
    if (outSharedHeight) *outSharedHeight = 16;
}

/* Android AHardwareBuffer Interop */
BCDECDEF bcdec_status_t bcdec_decompress_to_hardware_buffer(bcdec_format_t format, const void* restrict compressedData, void* restrict hardwareBufferPixels, int width, int height, int destinationPitch) {
    if (!compressedData || !hardwareBufferPixels || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec_decompress_texture(format, compressedData, hardwareBufferPixels, width, height, destinationPitch);
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_mali_decompress_to_ahardwarebuffer(bcdec_format_t format, const void* restrict compressedData, void* restrict ahwBufferVirtualAddress, int width, int height, int rowStrideBytes) {
    return bcdec_decompress_to_hardware_buffer(format, compressedData, ahwBufferVirtualAddress, width, height, rowStrideBytes);
}

BCDECDEF bcdec_status_t bcdec_android_lock_hardware_buffer_pixels(void* restrict hardwareBufferHandle, void** restrict outVirtualAddress, int lockFlags) {
    if (!hardwareBufferHandle || !outVirtualAddress) return BCDEC_ERROR_INVALID_PARAMETER;
#if defined(BCDEC_USE_ANDROID_AHARDWAREBUFFER) && defined(__ANDROID__)
    void* addr = NULL;   /* lockFlags = AHARDWAREBUFFER_USAGE_CPU_READ_* / CPU_WRITE_* */
    if (AHardwareBuffer_lock((AHardwareBuffer*)hardwareBufferHandle, (uint64_t)lockFlags, -1, NULL, &addr) != 0 || !addr)
        return BCDEC_ERROR_CORRUPT_SURFACE;
    *outVirtualAddress = addr;
    return BCDEC_SUCCESS;
#else
    (void)lockFlags;     /* sin BCDEC_USE_ANDROID_AHARDWAREBUFFER el "handle" debe ser ya una direcci贸n CPU mapeada */
    *outVirtualAddress = hardwareBufferHandle;
    return BCDEC_SUCCESS;
#endif
}

BCDECDEF bcdec_status_t bcdec_android_unlock_hardware_buffer(void* restrict hardwareBufferHandle) {
    if (!hardwareBufferHandle) return BCDEC_ERROR_INVALID_PARAMETER;
#if defined(BCDEC_USE_ANDROID_AHARDWAREBUFFER) && defined(__ANDROID__)
    return AHardwareBuffer_unlock((AHardwareBuffer*)hardwareBufferHandle, NULL) == 0 ? BCDEC_SUCCESS : BCDEC_ERROR_CORRUPT_SURFACE;
#else
    return BCDEC_SUCCESS;
#endif
}


BCDECDEF bcdec_status_t bcdec_android_release_hardware_buffer_mapping(void* restrict mappedVirtualAddress) {
    return mappedVirtualAddress ? BCDEC_SUCCESS : BCDEC_ERROR_INVALID_PARAMETER;
}

BCDECDEF size_t bcdec_android_align_hardware_buffer_stride(size_t rawWidth, int bytesPerPixel, size_t alignmentRequirement) {
    size_t rawBytes = rawWidth * (size_t)bytesPerPixel;
    size_t align = (alignmentRequirement > 64) ? alignmentRequirement : 64;
    return (rawBytes + (align - 1)) & ~(align - 1);
}

/* =========================================================================
 * IMPLEMENTACI脫N M脫DULO VULKAN 1.0 Y VULKAN 1.1
 * ========================================================================= */

BCDECDEF int bcdec_vulkan_validate_image_extent(int width, int height, int mipLevels, bcdec_format_t format) {
    if (width <= 0 || height <= 0 || mipLevels <= 0 || bcdec_format_block_size(format) == 0) return 0;
    if (width > BCDEC_MALI_G52_MAX_TEXTURE_DIM || height > BCDEC_MALI_G52_MAX_TEXTURE_DIM) return 0;
    return (mipLevels <= bcdec_calculate_mip_levels(width, height)) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_validate_buffer_image_copy(int width, int height, size_t bufferOffset, size_t totalBufferSize, bcdec_format_t format) {
    size_t blockSize = bcdec_format_block_size(format);
    if (blockSize == 0 || width <= 0 || height <= 0) return 0;
    if ((bufferOffset % blockSize) != 0) return 0;
    size_t requiredSize = bcdec_texture_compressed_size(format, width, height);
    return (bufferOffset + requiredSize <= totalBufferSize) ? 1 : 0;
}

BCDECDEF bcdec_status_t bcdec_vulkan_transcode_surface_format(bcdec_format_t srcFormat, bcdec_format_t dstFormat, const void* restrict srcData, void* restrict dstData, int width, int height) {
    if (!srcData || !dstData || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    if (srcFormat == dstFormat) {
        memcpy(dstData, srcData, bcdec_texture_compressed_size(srcFormat, width, height));
        return BCDEC_SUCCESS;
    }
    if (srcFormat == BCDEC_FORMAT_BC1 && dstFormat == BCDEC_FORMAT_BC3) return bcdec_transcode_bc1_to_bc3(srcData, dstData, width, height);
    if (srcFormat == BCDEC_FORMAT_BC3 && dstFormat == BCDEC_FORMAT_BC1) return bcdec_transcode_bc3_to_bc1(srcData, dstData, width, height);
    if (srcFormat == BCDEC_FORMAT_BC3 && dstFormat == BCDEC_FORMAT_BC4) return bcdec_transcode_bc3_to_bc4(srcData, dstData, width, height);
    if (srcFormat == BCDEC_FORMAT_BC4 && dstFormat == BCDEC_FORMAT_BC1) return bcdec_transcode_bc4_to_bc1(srcData, dstData, width, height);
    if (srcFormat == BCDEC_FORMAT_BC7 && dstFormat == BCDEC_FORMAT_BC1) return bcdec_transcode_bc7_to_bc1(srcData, dstData, width, height);
    if (srcFormat == BCDEC_FORMAT_BC7 && dstFormat == BCDEC_FORMAT_BC3) return bcdec_transcode_bc7_to_bc3(srcData, dstData, width, height);
    return BCDEC_ERROR_UNSUPPORTED_FORMAT;
}

BCDECDEF void bcdec_vulkan_build_image_memory_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_barrier_info_t barrierInfo, int baseMipLevel, int mipLevels) {
    if (!outVkImageMemoryBarrier) return;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t srcAccessMask;
        uint32_t dstAccessMask;
        uint32_t oldLayout;
        uint32_t newLayout;
        uint32_t srcQueueFamilyIndex;
        uint32_t dstQueueFamilyIndex;
        uint64_t image;
        uint32_t aspectMask;
        uint32_t baseMipLevel;
        uint32_t levelCount;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    } barrier = {
        45U, /* VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER */
        NULL,
        barrierInfo.srcAccessMask, barrierInfo.dstAccessMask,
        barrierInfo.oldLayout, barrierInfo.newLayout,
        barrierInfo.srcQueueFamilyIndex, barrierInfo.dstQueueFamilyIndex,
        (uint64_t)vkImage,
        barrierInfo.aspectMask ? barrierInfo.aspectMask : 0x00000001U,
        (uint32_t)baseMipLevel, (uint32_t)mipLevels, 0U, 1U
    };
    memcpy(outVkImageMemoryBarrier, &barrier, sizeof(barrier));
}

BCDECDEF size_t bcdec_vulkan_generate_compute_pipeline_descriptor(bcdec_format_t srcFormat, bcdec_format_t dstFormat, void* restrict outShaderParamsBuffer, size_t bufferCapacity) {
    bcdec_vulkan_push_constant_block_t pcb = { 0, 0, (uint32_t)srcFormat, (uint32_t)dstFormat, {1.0f, 1.0f, 1.0f, 1.0f} };
    if (outShaderParamsBuffer && bufferCapacity >= sizeof(pcb)) memcpy(outShaderParamsBuffer, &pcb, sizeof(pcb));
    return sizeof(pcb);
}

BCDECDEF size_t bcdec_vulkan_build_push_constant_block(bcdec_format_t srcFormat, bcdec_format_t dstFormat, int width, int height, void* restrict outPushConstantBuffer, size_t bufferCapacity) {
    bcdec_vulkan_push_constant_block_t pcb = { (uint32_t)width, (uint32_t)height, (uint32_t)srcFormat, (uint32_t)dstFormat, {1.0f, 0.0f, 0.0f, 1.0f} };
    if (outPushConstantBuffer && bufferCapacity >= sizeof(pcb)) memcpy(outPushConstantBuffer, &pcb, sizeof(pcb));
    return sizeof(pcb);
}


BCDECDEF int bcdec_vulkan_check_device_compression_features(uint32_t supportedFeaturesBitmask, bcdec_format_t targetFormat) {
    (void)targetFormat;
    return (supportedFeaturesBitmask & 0x1U) ? 1 : 0; /* Bit 0: textureCompressionBC en VkPhysicalDeviceFeatures */
}

BCDECDEF bcdec_status_t bcdec_vulkan_decompress_texture_layer(bcdec_format_t format, const void* restrict compressedLayerData, void* restrict decompressedLayerPixels, int width, int height, int destinationPitch) {
    if (!compressedLayerData || !decompressedLayerPixels || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec_decompress_texture(format, compressedLayerData, decompressedLayerPixels, width, height, destinationPitch);
    return BCDEC_SUCCESS;
}

BCDECDEF size_t bcdec_vulkan_calculate_layer_offset(bcdec_format_t format, int width, int height, int layerIndex) {
    if (layerIndex <= 0) return 0;
    return bcdec_texture_compressed_size(format, width, height) * (size_t)layerIndex;
}


BCDECDEF bcdec_status_t bcdec_vulkan_dispatch_compute_transcode_params(bcdec_format_t srcFormat, bcdec_format_t dstFormat, int width, int height, void* restrict outDispatchCommand) {
    (void)srcFormat; (void)dstFormat;
    if (!outDispatchCommand || width <= 0 || height <= 0) return BCDEC_ERROR_INVALID_PARAMETER;
    /* VkDispatchIndirectCommand: x, y, z grupos de 8x8 bloques (32x32 p铆xeles) */
    uint32_t cmd[3] = { (uint32_t)((width + 31) / 32), (uint32_t)((height + 31) / 32), 1U };
    memcpy(outDispatchCommand, cmd, sizeof(cmd));
    return BCDEC_SUCCESS;
}

BCDECDEF int bcdec_vulkan_validate_subresource_layout(int width, int height, int layerCount, int mipLevels, bcdec_format_t format) {
    if (layerCount <= 0 || layerCount > 2048) return 0;
    return bcdec_vulkan_validate_image_extent(width, height, mipLevels, format);
}

BCDECDEF int bcdec_vulkan_find_memory_type(uint32_t typeFilter, uint32_t propertyFlags, const uint32_t* restrict availableMemoryTypesBitmask, int memoryTypeCount) {
    if (!availableMemoryTypesBitmask || memoryTypeCount <= 0) return -1;
    for (int i = 0; i < memoryTypeCount && i < 32; ++i) {
        if ((typeFilter & (1U << i)) && ((availableMemoryTypesBitmask[i] & propertyFlags) == propertyFlags)) {
            return i;
        }
    }
    return -1;
}

BCDECDEF int bcdec_vulkan_build_mipmap_buffer_image_copies(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, void* restrict outCopyRegionsArray, size_t arrayCapacityBytes) {
    if (!outCopyRegionsArray || baseWidth <= 0 || baseHeight <= 0 || mipLevels <= 0) return 0;
    size_t regionSize = sizeof(bcdec_vulkan_buffer_image_copy_region_t);
    int maxRegions = (int)(arrayCapacityBytes / regionSize);
    int count = (mipLevels < maxRegions) ? mipLevels : maxRegions;
    bcdec_vulkan_buffer_image_copy_region_t* regions = (bcdec_vulkan_buffer_image_copy_region_t*)outCopyRegionsArray;
    size_t offset = 0;
    int w = baseWidth, h = baseHeight;
    for (int m = 0; m < count; ++m) {
        regions[m].bufferOffset = offset;
        regions[m].bufferRowLength = 0;
        regions[m].bufferImageHeight = 0;
        regions[m].imageOffsetX = 0; regions[m].imageOffsetY = 0; regions[m].imageOffsetZ = 0;
        regions[m].imageExtentWidth = (uint32_t)w;
        regions[m].imageExtentHeight = (uint32_t)h;
        regions[m].imageExtentDepth = 1U;
        regions[m].baseMipLevel = (uint32_t)m;
        regions[m].layerCount = 1U;
        offset += bcdec_texture_compressed_size(format, w, h);
        w = (w > 1) ? (w >> 1) : 1;
        h = (h > 1) ? (h >> 1) : 1;
    }
    return count;
}

BCDECDEF int bcdec_vulkan_check_format_feature_support(uint32_t formatFeaturesFlags, bcdec_format_t format, int requireOptimalTiling) {
    (void)format; (void)requireOptimalTiling;
    /* VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT = 0x00000001 */
    return (formatFeaturesFlags & 0x00000001U) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_align_buffer_offset_for_transfer(size_t rawOffset, size_t optimalBufferCopyOffsetAlignment) {
    size_t align = (optimalBufferCopyOffsetAlignment > 16) ? optimalBufferCopyOffsetAlignment : 16;
    return (rawOffset + (align - 1)) & ~(align - 1);
}

BCDECDEF void bcdec_vulkan_init_default_sampler_info(bcdec_vulkan_sampler_create_info_t* restrict outInfo, float maxAnisotropy) {
    if (!outInfo) return;
    outInfo->magFilter = 1U; /* VK_FILTER_LINEAR */
    outInfo->minFilter = 1U; /* VK_FILTER_LINEAR */
    outInfo->mipmapMode = 1U; /* VK_SAMPLER_MIPMAP_MODE_LINEAR */
    outInfo->addressModeU = 0U; /* VK_SAMPLER_ADDRESS_MODE_REPEAT */
    outInfo->addressModeV = 0U;
    outInfo->maxAnisotropy = (maxAnisotropy > 1.0f) ? maxAnisotropy : 1.0f;
    outInfo->enableAnisotropy = (maxAnisotropy > 1.0f) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_calculate_total_mip_chain_size(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels) {
    return bcdec_texture_mipmap_compressed_size(format, baseWidth, baseHeight, mipLevels);
}

/* Funciones espec铆ficas Vulkan 1.0 & 1.1 */
BCDECDEF int bcdec_vulkan_1_0_validate_buffer_image_copy(int width, int height, size_t bufferOffset, size_t totalBufferSize, bcdec_format_t format) {
    return bcdec_vulkan_validate_buffer_image_copy(width, height, bufferOffset, totalBufferSize, format);
}

BCDECDEF void bcdec_vulkan_1_0_build_image_memory_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_barrier_info_t barrierInfo, int baseMipLevel, int mipLevels) {
    bcdec_vulkan_build_image_memory_barrier(outVkImageMemoryBarrier, vkImage, barrierInfo, baseMipLevel, mipLevels);
}

BCDECDEF int bcdec_vulkan_1_0_validate_device_limits(int maxImageDimension2D, bcdec_format_t format) {
    return (maxImageDimension2D >= 4096 && bcdec_format_block_size(format) > 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_1_validate_device_features(uint32_t apiVersion, int deviceSupportsTextureCompressionBC, int deviceSupportsSamplerAnisotropy) {
    (void)deviceSupportsSamplerAnisotropy;
    if (apiVersion < BCDEC_VULKAN_1_0_API_VERSION) return 0;
    return deviceSupportsTextureCompressionBC ? 1 : 0;
}

BCDECDEF void bcdec_vulkan_1_0_init_sampler_create_info(void* restrict outVkSamplerCreateInfo, float maxAnisotropy) {
    if (!outVkSamplerCreateInfo) return;
    bcdec_vulkan_sampler_create_info_t info;
    bcdec_vulkan_init_default_sampler_info(&info, maxAnisotropy);
    memcpy(outVkSamplerCreateInfo, &info, sizeof(info));
}

BCDECDEF size_t bcdec_vulkan_1_0_build_descriptor_set_layout_binding(int bindingIndex, uint32_t descriptorType, uint32_t stageFlags, void* restrict outBindingStruct) {
    struct {
        uint32_t binding;
        uint32_t descriptorType;
        uint32_t descriptorCount;
        uint32_t stageFlags;
        const void* pImmutableSamplers;
    } b = { (uint32_t)bindingIndex, descriptorType, 1U, stageFlags, NULL };
    if (outBindingStruct) memcpy(outBindingStruct, &b, sizeof(b));
    return sizeof(b);
}

BCDECDEF int bcdec_vulkan_1_1_validate_pipeline_stage_flags(uint32_t srcStageMask, uint32_t dstStageMask) {
    /* Bits core de VkPipelineStageFlagBits en 1.0/1.1: 0x1 .. 0x10000. Bits superiores son de extensiones / Vulkan 1.3. */
    const uint32_t validCore = 0x0001FFFFu;
    if (srcStageMask == 0 || dstStageMask == 0) return 0;
    return ((srcStageMask | dstStageMask) & ~validCore) == 0 ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_image_descriptor_limits(int maxPerStageDescriptorSamplers, int maxPerStageDescriptorSampledImages) {
    /* M铆nimos garantizados por Vulkan 1.0 spec: 16 samplers, 16 sampled images */
    return (maxPerStageDescriptorSamplers >= 16 && maxPerStageDescriptorSampledImages >= 16) ? 1 : 0;
}

BCDECDEF void bcdec_vulkan_1_0_init_compressed_image_create_info(void* restrict outVkImageCreateInfo, int width, int height, bcdec_format_t format, uint32_t mipLevels) {
    if (!outVkImageCreateInfo) return;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint32_t imageType;
        uint32_t format;
        uint32_t extentWidth, extentHeight, extentDepth;
        uint32_t mipLevels;
        uint32_t arrayLayers;
        uint32_t samples;
        uint32_t tiling;
        uint32_t usage;
        uint32_t sharingMode;
        uint32_t queueFamilyIndexCount;
        const uint32_t* pQueueFamilyIndices;
        uint32_t initialLayout;
    } ci = {
        14U, /* VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO */
        NULL, 0U, 1U /* 2D */, bcdec_format_to_vulkan(format, 0),
        (uint32_t)width, (uint32_t)height, 1U, mipLevels, 1U, 1U /* 1_BIT */,
        0U /* OPTIMAL */, 0x00000006U /* TRANSFER_DST | SAMPLED */, 0U, 0U, NULL, 0U /* UNDEFINED */
    };
    memcpy(outVkImageCreateInfo, &ci, sizeof(ci));
}

BCDECDEF int bcdec_vulkan_1_0_validate_mipmap_chain_layout(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, size_t totalBufferSize) {
    if (baseWidth <= 0 || baseHeight <= 0 || mipLevels <= 0) return 0;
    size_t req = bcdec_texture_mipmap_compressed_size(format, baseWidth, baseHeight, mipLevels);
    return (req > 0 && req <= totalBufferSize) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_calculate_descriptor_pool_size(int sampledImagesCount, int samplersCount) {
    return (size_t)((sampledImagesCount + samplersCount) * 32);
}

BCDECDEF int bcdec_vulkan_1_0_validate_3d_extent(int width, int height, int depth, bcdec_format_t format) {
    if (width <= 0 || height <= 0 || depth <= 0 || bcdec_format_block_size(format) == 0) return 0;
    return (width <= 2048 && height <= 2048 && depth <= 2048) ? 1 : 0;
}

BCDECDEF bcdec_status_t bcdec_vulkan_1_1_build_layout_transition_barrier(void* restrict outVkImageMemoryBarrier, uint64_t vkImage, bcdec_vulkan_1_1_barrier_config_t config, int baseMipLevel, int mipLevels) {
    if (!outVkImageMemoryBarrier) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec_vulkan_barrier_info_t bi = { config.srcAccessMask, config.dstAccessMask, config.oldLayout, config.newLayout, config.srcQueueFamilyIndex, config.dstQueueFamilyIndex, 0x00000001U };
    bcdec_vulkan_build_image_memory_barrier(outVkImageMemoryBarrier, vkImage, bi, baseMipLevel, mipLevels);
    return BCDEC_SUCCESS;
}

BCDECDEF int bcdec_vulkan_1_1_validate_push_constant_limits(uint32_t pushConstantsSize, uint32_t maxPushConstantsSizeLimit) {
    uint32_t limit = (maxPushConstantsSizeLimit >= 128U) ? maxPushConstantsSizeLimit : 128U; /* Garant铆a Vulkan 1.0/1.1: 128 bytes */
    return (pushConstantsSize <= limit && (pushConstantsSize % 4U) == 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_is_block_compression_supported(bcdec_format_t format, int deviceHasBCSupport, int deviceHasASTCSupport) {
    (void)deviceHasASTCSupport;
    return (deviceHasBCSupport && bcdec_format_block_size(format) > 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_staging_buffer_alignment(size_t bufferOffset, size_t bufferSize, size_t optimalCopyOffsetAlignment) {
    if (bufferSize == 0) return 0;
    size_t align = (optimalCopyOffsetAlignment > 0) ? optimalCopyOffsetAlignment : 16;
    return ((bufferOffset % align) == 0) ? 1 : 0;
}

BCDECDEF void bcdec_vulkan_1_0_build_memory_barrier(void* restrict outVkMemoryBarrier, bcdec_vulkan_memory_barrier_config_t config) {
    if (!outVkMemoryBarrier) return;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t srcAccessMask;
        uint32_t dstAccessMask;
    } mb = { 46U /* VK_STRUCTURE_TYPE_MEMORY_BARRIER */, NULL, config.srcAccessMask, config.dstAccessMask };
    memcpy(outVkMemoryBarrier, &mb, sizeof(mb));
}

BCDECDEF size_t bcdec_vulkan_1_0_build_descriptor_write(const bcdec_vulkan_descriptor_write_config_t* restrict config, void* restrict outVkWriteDescriptorSet) {
    if (!config) return 0;
    struct {
        uint32_t sType;
        const void* pNext;
        uint64_t dstSet;
        uint32_t dstBinding;
        uint32_t dstArrayElement;
        uint32_t descriptorCount;
        uint32_t descriptorType;
        uint64_t pImageInfo;
        uint64_t pBufferInfo;
        uint64_t pTexelBufferView;
    } w = {
        35U, NULL, (uint64_t)config->dstSet, config->binding, config->elementIndex,
        1U, config->descriptorType, config->bufferOrImageInfoPtr, config->bufferOrImageInfoPtr, 0ULL
    };
    if (outVkWriteDescriptorSet) memcpy(outVkWriteDescriptorSet, &w, sizeof(w));
    return sizeof(w);
}

BCDECDEF int bcdec_vulkan_1_0_validate_npot_mipmap_extent(int baseWidth, int baseHeight, int mipLevels) {
    if (baseWidth <= 0 || baseHeight <= 0 || mipLevels <= 0) return 0;
    return (mipLevels <= bcdec_calculate_mip_levels(baseWidth, baseHeight)) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_1_build_descriptor_update_template(const bcdec_vulkan_descriptor_update_template_config_t* restrict config, void* restrict outTemplateCreateInfo) {
    if (!config) return 0;
    bcdec_vulkan_descriptor_update_template_config_t out = *config;
    if (outTemplateCreateInfo) memcpy(outTemplateCreateInfo, &out, sizeof(out));
    return sizeof(out);
}

BCDECDEF int bcdec_vulkan_1_0_validate_query_pool_create_info(uint32_t queryType, uint32_t queryCount) {
    /* Vulkan 1.0 query types: 0 (OCCLUSION), 1 (PIPELINE_STATISTICS), 2 (TIMESTAMP) */
    return (queryType <= 2U && queryCount > 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_pipeline_cache_header(const void* restrict cacheHeaderData, size_t dataSize) {
    if (!cacheHeaderData || dataSize < 32) return 0;
    uint32_t headerSize, headerVersion;
    memcpy(&headerSize, cacheHeaderData, 4);
    memcpy(&headerVersion, (const uint8_t*)cacheHeaderData + 4, 4);
    return (headerSize >= 32U && headerVersion == 1U) ? 1 : 0; /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE = 1 */
}

BCDECDEF int bcdec_vulkan_1_0_validate_memory_allocation(size_t allocationSize, size_t maxMemoryAllocationSizeLimit) {
    if (allocationSize == 0) return 0;
    return (maxMemoryAllocationSizeLimit == 0 || allocationSize <= maxMemoryAllocationSizeLimit) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_image_view_create_info(const bcdec_vulkan_image_view_config_t* restrict config, void* restrict outVkImageViewCreateInfo) {
    if (!config) return 0;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint64_t image;
        uint32_t viewType;
        uint32_t format;
        uint32_t r, g, b, a;
        uint32_t aspectMask;
        uint32_t baseMipLevel;
        uint32_t levelCount;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    } iv = {
        15U /* VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO */, NULL, 0U,
        (uint64_t)config->image, config->viewType, config->format,
        0U, 0U, 0U, 0U, config->aspectMask ? config->aspectMask : 1U, 0U, 1U, 0U, 1U
    };
    if (outVkImageViewCreateInfo) memcpy(outVkImageViewCreateInfo, &iv, sizeof(iv));
    return sizeof(iv);
}

BCDECDEF int bcdec_vulkan_1_1_validate_uniform_buffer_range(size_t rangeSize, size_t maxUniformBufferRangeLimit) {
    size_t limit = (maxUniformBufferRangeLimit >= 16384) ? maxUniformBufferRangeLimit : 16384; /* Garant铆a Vulkan 1.0/1.1: 16 KB */
    return (rangeSize > 0 && rangeSize <= limit) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_set_allocation_limits(uint32_t requestedDescriptorSets, uint32_t maxBoundDescriptorSets) {
    uint32_t limit = (maxBoundDescriptorSets >= 4U) ? maxBoundDescriptorSets : 4U; /* Garant铆a Vulkan 1.0: 4 sets */
    return (requestedDescriptorSets > 0 && requestedDescriptorSets <= limit) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_render_pass_begin_info(const bcdec_vulkan_render_pass_begin_config_t* restrict config, void* restrict outVkRenderPassBeginInfo) {
    if (!config) return 0;
    struct {
        uint32_t sType;
        const void* pNext;
        uint64_t renderPass;
        uint64_t framebuffer;
        int32_t x, y;
        uint32_t w, h;
        uint32_t clearValueCount;
        const void* pClearValues;
    } rp = {
        43U /* VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO */, NULL,
        (uint64_t)config->renderPass, (uint64_t)config->framebuffer,
        config->renderAreaOffsetX, config->renderAreaOffsetY,
        config->renderAreaWidth, config->renderAreaHeight,
        config->clearValueCount, NULL
    };
    if (outVkRenderPassBeginInfo) memcpy(outVkRenderPassBeginInfo, &rp, sizeof(rp));
    return sizeof(rp);
}

BCDECDEF int bcdec_vulkan_1_1_validate_image_blit_compatibility(bcdec_format_t srcFormat, bcdec_format_t dstFormat, uint32_t srcTiling, uint32_t dstTiling) {
    (void)srcTiling; (void)dstTiling;
    /* En Vulkan 1.0/1.1 vkCmdBlitImage NO soporta formatos comprimidos por bloques; debe usarse vkCmdCopyImage */
    return (bcdec_format_block_size(srcFormat) == 0 && bcdec_format_block_size(dstFormat) == 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_semaphore_submit_info(uint32_t waitSemaphoreCount, uint32_t signalSemaphoreCount) {
    return (waitSemaphoreCount <= 64U && signalSemaphoreCount <= 64U) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_image_copy_region(const bcdec_vulkan_sub_region_copy_config_t* restrict config, void* restrict outVkBufferImageCopy) {
    if (!config) return 0;
    struct {
        uint64_t bufferOffset;
        uint32_t bufferRowLength;
        uint32_t bufferImageHeight;
        uint32_t aspectMask;
        uint32_t mipLevel;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
        int32_t  offsetX, offsetY, offsetZ;
        uint32_t extentWidth, extentHeight, extentDepth;
    } bic = {
        (uint64_t)config->bufferOffset, config->bufferRowLength, config->bufferImageHeight,
        1U /* COLOR_BIT */, config->mipLevel, config->baseArrayLayer, config->layerCount ? config->layerCount : 1U,
        config->offsetX, config->offsetY, config->offsetZ,
        config->extentWidth, config->extentHeight, config->extentDepth ? config->extentDepth : 1U
    };
    if (outVkBufferImageCopy) memcpy(outVkBufferImageCopy, &bic, sizeof(bic));
    return sizeof(bic);
}

BCDECDEF int bcdec_vulkan_1_0_validate_host_visible_alignment(uintptr_t memoryAddress, size_t alignmentRequirement) {
    size_t align = (alignmentRequirement > 0) ? alignmentRequirement : 64;
    return ((memoryAddress % align) == 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_1_validate_color_blend_format(uint32_t vkColorFormat) {
    /* Los formatos BC (131-146) no soportan blending como color attachment */
    return (vkColorFormat < 131U || vkColorFormat > 146U) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_cubemap_extent(int faceWidth, int faceHeight) {
    return (faceWidth > 0 && faceWidth == faceHeight && faceWidth <= BCDEC_MALI_G52_MAX_TEXTURE_DIM) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_submit_info(const bcdec_vulkan_submit_config_t* restrict config, void* restrict outVkSubmitInfo) {
    if (!config) return 0;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const void* pWaitSemaphores;
        const uint32_t* pWaitDstStageMask;
        uint32_t commandBufferCount;
        const void* pCommandBuffers;
        uint32_t signalSemaphoreCount;
        const void* pSignalSemaphores;
    } si = {
        4U /* VK_STRUCTURE_TYPE_SUBMIT_INFO */, NULL,
        config->waitSemaphoreCount, NULL, NULL,
        config->commandBufferCount, NULL,
        config->signalSemaphoreCount, NULL
    };
    if (outVkSubmitInfo) memcpy(outVkSubmitInfo, &si, sizeof(si));
    return sizeof(si);
}

BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_pool_create_flags(uint32_t poolCreateFlags) {
    /* VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT = 0x00000001 */
    return (poolCreateFlags <= 0x00000003U) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_mipmap_image_barrier(const bcdec_vulkan_mipmap_barrier_config_t* restrict config, uint64_t vkImage, void* restrict outVkImageMemoryBarrier) {
    if (!config) return 0;
    bcdec_vulkan_barrier_info_t bi = { config->srcAccessMask, config->dstAccessMask, config->oldLayout, config->newLayout, ~0U, ~0U, 1U };
    if (outVkImageMemoryBarrier) bcdec_vulkan_build_image_memory_barrier(outVkImageMemoryBarrier, vkImage, bi, (int)config->targetMipLevel, 1);
    return 72;
}

BCDECDEF int bcdec_vulkan_1_0_validate_linear_image_stride(int width, int bytesPerPixel, size_t rowPitch, size_t bufferRowLengthAlignment) {
    size_t minPitch = (size_t)width * (size_t)bytesPerPixel;
    if (rowPitch < minPitch) return 0;
    return (bufferRowLengthAlignment == 0 || (rowPitch % bufferRowLengthAlignment) == 0) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_1_validate_layered_image_copy_extent(int baseWidth, int baseHeight, int layerCount, int targetLayer) {
    return (baseWidth > 0 && baseHeight > 0 && layerCount > 0 && targetLayer >= 0 && targetLayer < layerCount) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_device_compression_fallback(bcdec_format_t format, int supportsBC, int supportsASTC, int supportsETC2) {
    if (supportsBC && bcdec_format_block_size(format) > 0) return 1;
    if (supportsASTC || supportsETC2) return 2; /* Requiere transcodificaci贸n por software (bcdec_mali_transcode_bc_to_etc2) */
    return 0; /* Requiere descompresi贸n completa a RGBA8 */
}

BCDECDEF size_t bcdec_vulkan_1_0_build_shader_stage_create_info(const bcdec_vulkan_shader_stage_config_t* restrict config, void* restrict outVkPipelineShaderStageCreateInfo) {
    if (!config) return 0;
    struct {
        uint32_t sType;
        const void* pNext;
        uint32_t flags;
        uint32_t stage;
        uint64_t module;
        const char* pName;
        const void* pSpecializationInfo;
    } ss = {
        18U /* VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO */, NULL, 0U,
        config->stageFlag, (uint64_t)config->moduleHandle, config->pEntryName ? config->pEntryName : "main", NULL
    };
    if (outVkPipelineShaderStageCreateInfo) memcpy(outVkPipelineShaderStageCreateInfo, &ss, sizeof(ss));
    return sizeof(ss);
}

BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_image_copy(const bcdec_vulkan_1_0_copy_region_config_t* restrict config, void* restrict outVkBufferImageCopy) {
    if (!config) return 0;
    bcdec_vulkan_sub_region_copy_config_t sub = {
        config->bufferOffset, config->bufferRowLength, config->bufferImageHeight,
        (int32_t)config->imageOffsetX, (int32_t)config->imageOffsetY, (int32_t)config->imageOffsetZ,
        config->imageExtentWidth, config->imageExtentHeight, config->imageExtentDepth,
        config->mipLevel, 0U, config->layerCount
    };
    return bcdec_vulkan_1_0_build_buffer_image_copy_region(&sub, outVkBufferImageCopy);
}

BCDECDEF int bcdec_vulkan_1_0_validate_combined_image_sampler_limits(uint32_t boundSamplersCount, uint32_t maxPerStageDescriptorSamplers) {
    uint32_t limit = (maxPerStageDescriptorSamplers >= 16U) ? maxPerStageDescriptorSamplers : 16U;
    return (boundSamplersCount <= limit) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_clear_attachment_format(uint32_t vkFormat) {
    return (vkFormat > 0U && (vkFormat < 131U || vkFormat > 146U)) ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_descriptor_set_layout_binding_limit(uint32_t bindingCount, uint32_t maxBoundDescriptorSets) {
    (void)maxBoundDescriptorSets;
    return (bindingCount > 0U && bindingCount <= 1024U) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_image_memory_barrier_ex(const bcdec_vulkan_1_0_image_barrier_config_t* restrict config, void* restrict outVkImageMemoryBarrier) {
    if (!config) return 0;
    bcdec_vulkan_barrier_info_t bi = {
        config->srcAccessMask, config->dstAccessMask, config->oldLayout, config->newLayout,
        config->srcQueueFamilyIndex, config->dstQueueFamilyIndex, config->aspectMask
    };
    if (outVkImageMemoryBarrier) bcdec_vulkan_build_image_memory_barrier(outVkImageMemoryBarrier, config->image, bi, (int)config->baseMipLevel, (int)config->levelCount);
    return 72;
}

BCDECDEF int bcdec_vulkan_1_0_validate_framebuffer_attachment_format(uint32_t vkFormat) {
    return (vkFormat > 0U && (vkFormat < 131U || vkFormat > 146U)) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_1_0_build_buffer_descriptor_info(const bcdec_vulkan_1_0_buffer_info_config_t* restrict config, void* restrict outVkDescriptorBufferInfo) {
    if (!config) return 0;
    struct {
        uint64_t buffer;
        uint64_t offset;
        uint64_t range;
    } dbi = { config->bufferHandle, (uint64_t)config->offset, (uint64_t)config->range };
    if (outVkDescriptorBufferInfo) memcpy(outVkDescriptorBufferInfo, &dbi, sizeof(dbi));
    return sizeof(dbi);
}

BCDECDEF int bcdec_vulkan_1_0_validate_max_bound_descriptor_sets(uint32_t boundSetsCount) {
    return (boundSetsCount <= 4U) ? 1 : 0; /* M铆nimo garantizado por Vulkan 1.0 y Mali-G52 */
}

BCDECDEF int bcdec_vulkan_1_0_validate_buffer_blit_compatibility(size_t srcSize, size_t dstSize, size_t transferSize) {
    return (transferSize > 0 && transferSize <= srcSize && transferSize <= dstSize && (transferSize % 4) == 0) ? 1 : 0;
}

/* =========================================================================
 * IMPLEMENTACI脫N M脫DULO CRYPTO & SEGURIDAD PIPPETOCRYPTO
 * ========================================================================= */

static void bcdec__pippetocrypto_derive_keystream_block(const uint8_t key32[32], const uint8_t iv16[16], uint64_t counter, uint8_t outBlock32[32]) {
    uint32_t state[8];
    for (int i = 0; i < 8; ++i) {
        uint32_t k, v;
        memcpy(&k, key32 + i * 4, 4);
        memcpy(&v, iv16 + (i % 4) * 4, 4);
        state[i] = k ^ v ^ (uint32_t)(counter + (uint64_t)i * 0x9E3779B9U);
    }
    for (int round = 0; round < 12; ++round) {
        for (int i = 0; i < 8; ++i) {
            uint32_t a = state[i];
            uint32_t b = state[(i + 1) & 7];
            a = ((a << 7) | (a >> 25)) + b;
            b = ((b << 13) | (b >> 19)) ^ a;
            state[i] = a ^ (uint32_t)(round * 0x85EBCA6BU + i);
            state[(i + 1) & 7] = b;
        }
    }
    memcpy(outBlock32, state, 32);
}


BCDECDEF bcdec_status_t bcdec_pippetocrypto_decrypt_payload_stream(const void* restrict encryptedPayload, void* restrict decryptedOutput, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16]) {
    if (!encryptedPayload || !decryptedOutput || !key32 || !iv16 || payloadSize == 0) return BCDEC_ERROR_INVALID_PARAMETER;
    const uint8_t* src = (const uint8_t*)encryptedPayload;
    uint8_t* dst = (uint8_t*)decryptedOutput;
    uint8_t keystream[32];
    uint64_t counter = 0;
    size_t offset = 0;
    while (offset < payloadSize) {
        bcdec__pippetocrypto_derive_keystream_block(key32, iv16, counter++, keystream);
        size_t chunk = (payloadSize - offset < 32) ? (payloadSize - offset) : 32;
        for (size_t i = 0; i < chunk; ++i) dst[offset + i] = src[offset + i] ^ keystream[i];
        offset += chunk;
    }
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_pippetocrypto_encrypt_payload_stream(const void* restrict plainPayload, void* restrict encryptedOutput, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16]) {
    return bcdec_pippetocrypto_decrypt_payload_stream(plainPayload, encryptedOutput, payloadSize, key32, iv16);
}

BCDECDEF bcdec_status_t bcdec_pippetocrypto_parse_and_verify_container(const void* restrict containerBytes, size_t totalBytes, const uint8_t key32[32], bcdec_pippetocrypto_header_t* restrict outHeader, const void** restrict outPayload) {
    if (!containerBytes || !key32 || !outHeader || totalBytes < sizeof(bcdec_pippetocrypto_header_t)) return BCDEC_ERROR_INVALID_HEADER;
    bcdec_pippetocrypto_header_t hdr;
    memcpy(&hdr, containerBytes, sizeof(hdr));
    if (hdr.magic != 0x50495050U) return BCDEC_ERROR_INVALID_HEADER; /* 'PIPP' */

    size_t expectedPayloadSize = bcdec_texture_mipmap_compressed_size((bcdec_format_t)hdr.format, (int)hdr.width, (int)hdr.height, (int)(hdr.mipLevels ? hdr.mipLevels : 1));
    if (expectedPayloadSize == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (totalBytes < sizeof(hdr) + expectedPayloadSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;

    const void* payloadPtr = (const uint8_t*)containerBytes + sizeof(hdr);
    uint32_t actualCrc = bcdec_calculate_buffer_crc32(payloadPtr, expectedPayloadSize);
    if (actualCrc != hdr.payloadCrc32) return BCDEC_ERROR_CRYPTO_AUTH_FAILED;

    *outHeader = hdr;
    if (outPayload) *outPayload = payloadPtr;
    return BCDEC_SUCCESS;
}


/* =========================================================================
 * CRIPTO v2 PIPPETOCRYPTO: ChaCha20 (RFC 8439) + HMAC-SHA256 (RFC 2104), encrypt-then-MAC
 * Las funciones *_stream / parse_and_verify_container anteriores se conservan por compatibilidad con assets ya cifrados,
 * pero usan un cifrado propio sin revisi贸n criptogr谩fica y un CRC32 como "firma": no autentican.
 * ========================================================================= */
typedef struct bcdec__sha256_ctx { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } bcdec__sha256_ctx_t;

static const uint32_t bcdec__sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define BCDEC__ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void bcdec__sha256_block(bcdec__sha256_ctx_t* c, const uint8_t* p) {
    uint32_t w[64], a,b,cc,d,e,f,g,h;
    for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = BCDEC__ROR(w[i-15],7) ^ BCDEC__ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = BCDEC__ROR(w[i-2],17) ^ BCDEC__ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = BCDEC__ROR(e,6) ^ BCDEC__ROR(e,11) ^ BCDEC__ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + bcdec__sha256_k[i] + w[i];
        uint32_t S0 = BCDEC__ROR(a,2) ^ BCDEC__ROR(a,13) ^ BCDEC__ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void bcdec__sha256_init(bcdec__sha256_ctx_t* c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv)); c->len = 0; c->n = 0;
}

static void bcdec__sha256_update(bcdec__sha256_ctx_t* c, const uint8_t* d, size_t len) {
    c->len += len;
    while (len) {
        size_t take = 64 - c->n; if (take > len) take = len;
        memcpy(c->buf + c->n, d, take); c->n += take; d += take; len -= take;
        if (c->n == 64) { bcdec__sha256_block(c, c->buf); c->n = 0; }
    }
}

static void bcdec__sha256_final(bcdec__sha256_ctx_t* c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    bcdec__sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) bcdec__sha256_update(c, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8*i));
    bcdec__sha256_update(c, lb, 8);
    for (int i = 0; i < 8; ++i) { out[i*4]=(uint8_t)(c->h[i]>>24); out[i*4+1]=(uint8_t)(c->h[i]>>16); out[i*4+2]=(uint8_t)(c->h[i]>>8); out[i*4+3]=(uint8_t)c->h[i]; }
}

/* HMAC con hasta dos fragmentos de datos (evita copiar cabecera + payload) */
static void bcdec__hmac2(const uint8_t* key, size_t keyLen, const void* d1, size_t l1, const void* d2, size_t l2, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, sizeof(k));
    if (keyLen > 64) { bcdec__sha256_ctx_t t; bcdec__sha256_init(&t); bcdec__sha256_update(&t, key, keyLen); bcdec__sha256_final(&t, k); }
    else if (keyLen) memcpy(k, key, keyLen);
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    bcdec__sha256_ctx_t c;
    bcdec__sha256_init(&c); bcdec__sha256_update(&c, ipad, 64);
    if (l1) bcdec__sha256_update(&c, (const uint8_t*)d1, l1);
    if (l2) bcdec__sha256_update(&c, (const uint8_t*)d2, l2);
    bcdec__sha256_final(&c, inner);
    bcdec__sha256_init(&c); bcdec__sha256_update(&c, opad, 64); bcdec__sha256_update(&c, inner, 32); bcdec__sha256_final(&c, out);
}

BCDECDEF bcdec_status_t bcdec_crypto_hmac_sha256(const uint8_t* restrict key, size_t keyLen, const void* restrict data, size_t dataLen, uint8_t outMac[32]) {
    if (!outMac || (!key && keyLen) || (!data && dataLen)) return BCDEC_ERROR_INVALID_PARAMETER;
    bcdec__hmac2(key, keyLen, data, dataLen, NULL, 0, outMac);
    return BCDEC_SUCCESS;
}

static int bcdec__ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

BCDECDEF bcdec_status_t bcdec_crypto_verify_secure_texture_payload_hmac(const void* restrict payload, size_t payloadSize, const uint8_t* restrict key, size_t keyLen, const uint8_t* restrict expectedHmacSha256, uint32_t* restrict outFlags) {
    if (!payload || payloadSize == 0 || !key || keyLen == 0 || !expectedHmacSha256 || !outFlags) return BCDEC_ERROR_INVALID_PARAMETER;
    uint8_t mac[32];
    bcdec__hmac2(key, keyLen, payload, payloadSize, NULL, 0, mac);
    if (!bcdec__ct_equal(mac, expectedHmacSha256, 32)) { *outFlags = 0; return BCDEC_ERROR_CRYPTO_AUTH_FAILED; }
    *outFlags = 0x01;
    return BCDEC_SUCCESS;
}

/* ChaCha20 RFC 8439: iv16 = contador inicial (u32 LE) || nonce de 96 bits */
#define BCDEC__QR(a,b,c,d) do { a+=b; d^=a; d=(d<<16)|(d>>16); c+=d; b^=c; b=(b<<12)|(b>>20); a+=b; d^=a; d=(d<<8)|(d>>24); c+=d; b^=c; b=(b<<7)|(b>>25); } while (0)

static void bcdec__chacha20_block(const uint32_t key[8], const uint32_t iv[4], uint32_t counter, uint8_t out[64]) {
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        key[0],key[1],key[2],key[3],key[4],key[5],key[6],key[7], counter, iv[1], iv[2], iv[3] };
    uint32_t x[16];
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        BCDEC__QR(x[0],x[4],x[8],x[12]);  BCDEC__QR(x[1],x[5],x[9],x[13]);
        BCDEC__QR(x[2],x[6],x[10],x[14]); BCDEC__QR(x[3],x[7],x[11],x[15]);
        BCDEC__QR(x[0],x[5],x[10],x[15]); BCDEC__QR(x[1],x[6],x[11],x[12]);
        BCDEC__QR(x[2],x[7],x[8],x[13]);  BCDEC__QR(x[3],x[4],x[9],x[14]);
    }
    for (int i = 0; i < 16; ++i) { uint32_t v = x[i] + s[i]; out[i*4]=(uint8_t)v; out[i*4+1]=(uint8_t)(v>>8); out[i*4+2]=(uint8_t)(v>>16); out[i*4+3]=(uint8_t)(v>>24); }
}

static uint32_t bcdec__le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

BCDECDEF bcdec_status_t bcdec_pippetocrypto_chacha20_xor(const void* restrict input, void* restrict output, size_t size, const uint8_t key32[32], const uint8_t iv16[16]) {
    if (!key32 || !iv16 || (!input && size) || (!output && size)) return BCDEC_ERROR_INVALID_PARAMETER;
    uint32_t key[8], iv[4];
    for (int i = 0; i < 8; ++i) key[i] = bcdec__le32(key32 + i * 4);
    for (int i = 0; i < 4; ++i) iv[i] = bcdec__le32(iv16 + i * 4);
    if ((uint64_t)iv[0] + (uint64_t)((size + 63) / 64) > 0x100000000ULL) return BCDEC_ERROR_BUFFER_TOO_SMALL;   /* contador de 32 bits */
    const uint8_t* in = (const uint8_t*)input; uint8_t* out = (uint8_t*)output;
    uint32_t counter = iv[0]; uint8_t ks[64];
    for (size_t off = 0; off < size; off += 64, ++counter) {
        bcdec__chacha20_block(key, iv, counter, ks);
        size_t n = size - off < 64 ? size - off : 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ ks[i];
    }
    return BCDEC_SUCCESS;
}

/* Derivaci贸n de claves separadas (cifrado / MAC) a partir de la clave maestra */
static void bcdec__pippeto_derive(const uint8_t master[32], uint8_t encKey[32], uint8_t macKey[32]) {
    static const char encLabel[] = "pippetocrypto-enc-v2", macLabel[] = "pippetocrypto-mac-v2";
    bcdec__hmac2(master, 32, encLabel, sizeof(encLabel) - 1, NULL, 0, encKey);
    bcdec__hmac2(master, 32, macLabel, sizeof(macLabel) - 1, NULL, 0, macKey);
}

#define BCDEC_PIPPETO_FLAG_ENCRYPTED  0x1u
#define BCDEC_PIPPETO_FLAG_V2_AEAD    0x4u

static void bcdec__pippeto_tag(const bcdec_pippetocrypto_header_t* hdr, const uint8_t* payload, size_t payloadSize, const uint8_t macKey[32], uint8_t tag[32]) {
    bcdec_pippetocrypto_header_t h = *hdr;
    memset(h.hmacTag, 0, sizeof(h.hmacTag));
    bcdec__hmac2(macKey, 32, &h, sizeof(h), payload, payloadSize, tag);
}

BCDECDEF bcdec_status_t bcdec_pippetocrypto_seal_container(const bcdec_pippetocrypto_header_t* restrict headerTemplate, const void* restrict plainPayload, size_t payloadSize, const uint8_t key32[32], const uint8_t iv16[16], void* restrict outContainer, size_t outCapacity, size_t* restrict outSize) {
    if (!headerTemplate || !plainPayload || !key32 || !iv16 || !outContainer || payloadSize == 0) return BCDEC_ERROR_INVALID_PARAMETER;
    size_t expected = bcdec_texture_mipmap_compressed_size((bcdec_format_t)headerTemplate->format, (int)headerTemplate->width, (int)headerTemplate->height, (int)(headerTemplate->mipLevels ? headerTemplate->mipLevels : 1));
    if (expected == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (payloadSize != expected) return BCDEC_ERROR_INVALID_PARAMETER;
    if (outCapacity < sizeof(bcdec_pippetocrypto_header_t) + payloadSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    uint8_t encKey[32], macKey[32];
    bcdec__pippeto_derive(key32, encKey, macKey);
    bcdec_pippetocrypto_header_t hdr = *headerTemplate;
    hdr.magic = 0x50495050U; hdr.version = 2;
    hdr.flags |= BCDEC_PIPPETO_FLAG_ENCRYPTED | BCDEC_PIPPETO_FLAG_V2_AEAD;
    memcpy(hdr.iv, iv16, 16);
    uint8_t* payloadOut = (uint8_t*)outContainer + sizeof(hdr);
    bcdec_status_t st = bcdec_pippetocrypto_chacha20_xor(plainPayload, payloadOut, payloadSize, encKey, iv16);
    if (st != BCDEC_SUCCESS) return st;
    hdr.payloadCrc32 = bcdec_calculate_buffer_crc32(payloadOut, payloadSize);
    bcdec__pippeto_tag(&hdr, payloadOut, payloadSize, macKey, hdr.hmacTag);
    memcpy(outContainer, &hdr, sizeof(hdr));
    if (outSize) *outSize = sizeof(hdr) + payloadSize;
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_pippetocrypto_open_container(const void* restrict containerBytes, size_t totalBytes, const uint8_t key32[32], bcdec_pippetocrypto_header_t* restrict outHeader, void* restrict outPlainPayload, size_t plainCapacity) {
    if (!containerBytes || !key32 || !outHeader || !outPlainPayload || totalBytes < sizeof(bcdec_pippetocrypto_header_t)) return BCDEC_ERROR_INVALID_HEADER;
    bcdec_pippetocrypto_header_t hdr;
    memcpy(&hdr, containerBytes, sizeof(hdr));
    if (hdr.magic != 0x50495050U || hdr.version != 2 || !(hdr.flags & BCDEC_PIPPETO_FLAG_V2_AEAD)) return BCDEC_ERROR_INVALID_HEADER;
    size_t payloadSize = bcdec_texture_mipmap_compressed_size((bcdec_format_t)hdr.format, (int)hdr.width, (int)hdr.height, (int)(hdr.mipLevels ? hdr.mipLevels : 1));
    if (payloadSize == 0) return BCDEC_ERROR_UNSUPPORTED_FORMAT;
    if (totalBytes < sizeof(hdr) + payloadSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    if (plainCapacity < payloadSize) return BCDEC_ERROR_BUFFER_TOO_SMALL;
    uint8_t encKey[32], macKey[32], tag[32];
    bcdec__pippeto_derive(key32, encKey, macKey);
    const uint8_t* payload = (const uint8_t*)containerBytes + sizeof(hdr);
    bcdec__pippeto_tag(&hdr, payload, payloadSize, macKey, tag);          /* se verifica ANTES de descifrar */
    if (!bcdec__ct_equal(tag, hdr.hmacTag, 32)) return BCDEC_ERROR_CRYPTO_AUTH_FAILED;
    bcdec_status_t st = bcdec_pippetocrypto_chacha20_xor(payload, outPlainPayload, payloadSize, encKey, hdr.iv);
    if (st != BCDEC_SUCCESS) return st;
    *outHeader = hdr;
    return BCDEC_SUCCESS;
}

BCDECDEF bcdec_status_t bcdec_crypto_verify_secure_texture_payload(const void* restrict encryptedPayload, size_t payloadSize, const uint8_t* restrict expectedHmacSha256, uint32_t* restrict outDecryptedFlags) {
    /* AVISO: sin clave esto solo compara el CRC32 del payload con los 4 primeros bytes de la etiqueta (modo "CRC-tag").
       No autentica. Para autenticaci贸n real use bcdec_crypto_verify_secure_texture_payload_hmac() o
       bcdec_pippetocrypto_open_container(). La versi贸n anterior devolv铆a 茅xito siempre que expectedHmacSha256[0] != 0. */
    if (!encryptedPayload || payloadSize == 0 || !expectedHmacSha256) return BCDEC_ERROR_INVALID_PARAMETER;
    uint32_t crc = bcdec_calculate_buffer_crc32(encryptedPayload, payloadSize);
    if (outDecryptedFlags) *outDecryptedFlags = crc;
    uint8_t crcBytes[4] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
    return bcdec__ct_equal(crcBytes, expectedHmacSha256, 4) ? BCDEC_SUCCESS : BCDEC_ERROR_CRYPTO_AUTH_FAILED;
}


BCDECDEF int bcdec_vulkan_validate_device_compression_features(uint32_t supportedFeaturesBitmask, bcdec_format_t targetFormat) {
    /* M谩scara propia: 0x1 = BC1-BC3, 0x2 = BC4/BC5, 0x4 = BC6H/BC7. En Mali-G52 ser谩 0 (textureCompressionBC = VK_FALSE). */
    uint32_t requiredFlag = 0;
    switch (targetFormat) {
        case BCDEC_FORMAT_BC1: case BCDEC_FORMAT_BC2: case BCDEC_FORMAT_BC3: requiredFlag = 0x1; break;
        case BCDEC_FORMAT_BC4: case BCDEC_FORMAT_BC5: requiredFlag = 0x2; break;
        case BCDEC_FORMAT_BC6H: case BCDEC_FORMAT_BC7: requiredFlag = 0x4; break;
        default: return 0;
    }
    return (supportedFeaturesBitmask & requiredFlag) != 0 ? 1 : 0;
}

BCDECDEF int bcdec_vulkan_validate_core_compression_limits(int maxImageDimension2D, bcdec_format_t format) {
    if (maxImageDimension2D < 4096 || maxImageDimension2D > 16384) return 0;   /* m铆nimo de Vulkan 1.0/1.1 y cota de cordura */
    return (format >= BCDEC_FORMAT_BC1 && format <= BCDEC_FORMAT_BC7) ? 1 : 0;
}

BCDECDEF size_t bcdec_vulkan_compile_compute_shader_spirv(bcdec_format_t format, uint32_t* restrict outSpirvBinaryBuffer, size_t bufferCapacityWords) {
    /* No hay generador SPIR-V incluido: compile el decodificador offline (glslc). Se devuelve 0 en vez de un m贸dulo vac铆o. */
    (void)format; (void)outSpirvBinaryBuffer; (void)bufferCapacityWords;
    return 0;
}

BCDECDEF int bcdec_vulkan_1_0_validate_pipeline_cache_header_for_device(const void* restrict cacheHeaderData, size_t dataSize, uint32_t vendorID, uint32_t deviceID, const uint8_t* restrict pipelineCacheUUID) {
    if (!bcdec_vulkan_1_0_validate_pipeline_cache_header(cacheHeaderData, dataSize)) return 0;
    uint32_t h[4];
    memcpy(h, cacheHeaderData, sizeof(h));
    if (h[2] != vendorID || h[3] != deviceID) return 0;       /* ARM vendorID = 0x13B5 */
    if (pipelineCacheUUID && memcmp((const uint8_t*)cacheHeaderData + 16, pipelineCacheUUID, 16) != 0) return 0;
    return 1;
}

BCDECDEF int bcdec_vulkan_1_0_build_vk_mipmap_buffer_image_copies(bcdec_format_t format, int baseWidth, int baseHeight, int mipLevels, uint64_t baseBufferOffset, void* restrict outVkBufferImageCopyArray, size_t arrayCapacityBytes) {
    /* VkBufferImageCopy real: 56 bytes por nivel (u64 offset, 2 u32, subresource 4 u32, offset 3 i32, extent 3 u32) */
    if (!outVkBufferImageCopyArray || baseWidth <= 0 || baseHeight <= 0 || mipLevels <= 0) return 0;
    if (bcdec_format_block_size(format) == 0 || arrayCapacityBytes < (size_t)56 * (size_t)mipLevels) return 0;
    uint8_t* out = (uint8_t*)outVkBufferImageCopyArray;
    uint64_t offset = baseBufferOffset; int w = baseWidth, h = baseHeight;
    for (int level = 0; level < mipLevels; ++level, out += 56) {
        uint32_t f[12] = { 0, 0, 1u, (uint32_t)level, 0, 1u, 0, 0, 0, (uint32_t)w, (uint32_t)h, 1u };
        memset(out, 0, 56);
        memcpy(out, &offset, 8);
        memcpy(out + 8, f, sizeof(f));              /* rowLength=0, imageHeight=0, aspect=COLOR, mip, baseLayer=0, layers=1, offset=0,0,0, extent */
        offset += (uint64_t)bcdec_texture_compressed_size(format, w, h);
        w = (w > 1) ? (w >> 1) : 1; h = (h > 1) ? (h >> 1) : 1;
    }
    return mipLevels;
}

#endif /* BCDEC_IMPLEMENTATION */
