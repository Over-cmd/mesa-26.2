#ifndef BCDEC_MALI_EXT_H
#define BCDEC_MALI_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Cabecera principal de bcdec (para bcdec_format_t). Cámbiala con -DBCDEC_MAIN_HEADER='"otro.h"' */
#ifndef BCDEC_MAIN_HEADER
#define BCDEC_MAIN_HEADER "bcdec.h"
#endif
#ifndef BCDEC_HEADER_INCLUDED
#include BCDEC_MAIN_HEADER
#endif

/* ============================================================================
 * VERSIONADO Y METADATOS DEL MÓDULO BCDEC MALI EXT
 * Adaptado para Vulkan 1.0 y 1.1 - Mali G52 (Proyecto pippetocrypto)
 * ============================================================================ */
#define BCDEC_MALI_EXT_VERSION_MAJOR 1
#define BCDEC_MALI_EXT_VERSION_MINOR 1
#define BCDEC_MALI_EXT_VERSION_PATCH 0

#define BCDEC_MALI_EXT_CHECK_VERSION(major, minor, patch) \
    ((BCDEC_MALI_EXT_VERSION_MAJOR > (major)) || \
     (BCDEC_MALI_EXT_VERSION_MAJOR == (major) && BCDEC_MALI_EXT_VERSION_MINOR > (minor)) || \
     (BCDEC_MALI_EXT_VERSION_MAJOR == (major) && BCDEC_MALI_EXT_VERSION_MINOR == (minor) && \
      BCDEC_MALI_EXT_VERSION_PATCH >= (patch)))

/* Compatibilidad del calificador restrict para C++ / Compiladores mixtos */
#ifndef restrict
#ifdef __cplusplus
#define restrict __restrict__
#elif defined(_MSC_VER)
#define restrict __restrict
#elif !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#define restrict
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BCDEC_ASTC_BLOCK_4X4,
    BCDEC_ASTC_BLOCK_6X6,
    BCDEC_ASTC_BLOCK_8X8
} bcdec_astc_block_dim_t;

typedef enum {
    BCDEC_POWER_MODE_PERFORMANCE,
    BCDEC_POWER_MODE_BALANCED,
    BCDEC_POWER_MODE_BATTERY_SAVER
} bcdec_power_mode_t;

/* Tabla host de handles (NO es bindless real: en Vulkan 1.0/1.1 y Mali-G52 no existe descriptor indexing
   ni bufferDeviceAddress; esto solo numera los image views para indexarlos en arrays de descriptores de tamaño fijo). */
typedef struct {
    uint32_t descriptorIndex;
    uint64_t bufferDeviceAddress;   /* nombre heredado: aquí guarda el handle del image view, no una dirección */
    size_t range;
} bcdec_vulkan_bindless_descriptor_t;

/* ---- API original (se conservan las firmas) ---- */

/* DESACTIVADA: no recibe el formato BC de origen, así que no puede decodificar. Devuelve siempre false.
   Use bcdec_mali_transcode_bc_to_astc_ex(). */
bool bcdec_mali_transcode_bc_to_astc(
    const void* restrict bcData,
    void* restrict astcData,
    int width,
    int height,
    bcdec_astc_block_dim_t blockDim);

/* Solo actúa sobre bloques de 8 bytes (BC1/BC4). Para bloques de 16 bytes no hace nada porque sin conocer el
   formato podría destruir los bits de modo de BC6H/BC7. Use bcdec_mali_apply_foveated_compression_mask_ex(). */
void bcdec_mali_apply_foveated_compression_mask(
    void* restrict compressedData,
    int width,
    int height,
    int bytesPerBlock,
    float foveationIntensity);

bool bcdec_vulkan_create_bindless_table(
    const void* device,
    const void* const* imageViews,
    uint32_t imageViewCount,
    bcdec_vulkan_bindless_descriptor_t* outDescriptors);

int bcdec_mali_get_optimal_worker_threads(bcdec_power_mode_t powerMode);

/* ---- API nueva ---- */

/* Tamaño en bytes de la salida de bcdec_mali_transcode_bc_to_astc_ex() */
size_t bcdec_mali_astc_output_size(int width, int height, bcdec_astc_block_dim_t blockDim, int writeAstcFileHeader);

/* BC1/BC2/BC3/BC4/BC5/BC7 -> bloques ASTC (16 bytes cada uno) usando astc_encode_block().
   Los bordes se rellenan replicando el último texel. BC6H (HDR) no se admite.
   writeAstcFileHeader = 1 antepone la cabecera de 16 bytes del formato .astc; 0 = solo bloques crudos (para subir a un VkImage). */
bool bcdec_mali_transcode_bc_to_astc_ex(
    bcdec_format_t srcFormat,
    const void* restrict bcData,
    void* restrict astcData,
    size_t astcCapacity,
    int width,
    int height,
    bcdec_astc_block_dim_t blockDim,
    int writeAstcFileHeader,
    size_t* restrict outBytes);

/* Reduce ligeramente la precisión de los bloques de la periferia. No reduce el tamaño de los datos.
   BC6H y BC7 no se tocan (su primer byte contiene los bits de modo). */
void bcdec_mali_apply_foveated_compression_mask_ex(
    bcdec_format_t format,
    void* restrict compressedData,
    int width,
    int height,
    float foveationIntensity);

/* Igual que create_bindless_table pero con los handles como uint64_t, que es lo que son los VkImageView
   en Vulkan (en 32 bits NO son punteros). */
bool bcdec_vulkan_create_handle_table(
    uint32_t imageViewCount,
    const uint64_t* imageViewHandles,
    bcdec_vulkan_bindless_descriptor_t* outDescriptors);

#ifdef __cplusplus
}
#endif

#endif // BCDEC_MALI_EXT_H
