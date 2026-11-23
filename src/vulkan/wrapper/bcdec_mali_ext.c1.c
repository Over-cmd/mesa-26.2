#ifndef BCDEC_MALI_EXT_NO_WRAPPERS
#include "wrapper_bcdec.h"
#endif
#ifndef BCDEC_MAIN_HEADER
#define BCDEC_MAIN_HEADER "bcdec.h"
#endif
#ifndef BCDEC_HEADER_INCLUDED
#include BCDEC_MAIN_HEADER
#endif
#include "bcdec_mali_ext.h"
#ifndef BCDEC_MALI_EXT_NO_WRAPPERS
#include "wrapper_astc.h" // Conexión con nuestro codificador optimizado (astc_encode_block)
#include "wrapper_log.h"  // Para logs y depuración (si imprimes mensajes)
#else
/* Modo pruebas (sin wrappers): el llamador aporta astc_encode_block */
void astc_encode_block(const uint8_t* texels, int width_height, int format_type, uint8_t* out);
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>       // Esencial para uint32_t / uint64_t
#include <stdbool.h>      // Esencial para bool
#include <stddef.h>       // Esencial para size_t
#if defined(__unix__) || defined(__ANDROID__) || defined(__APPLE__)
#  include <unistd.h>
#endif

/*
 * Adaptación para Vulkan 1.0 y 1.1 - Mali G52
 * Compatible con especificaciones de bajo nivel sin características de Vulkan 1.2+ (como descriptor indexing avanzado o buffer device address nativo V1.2).
 */

static int bcdec__ext_astc_dim(bcdec_astc_block_dim_t d) {
    return (d == BCDEC_ASTC_BLOCK_4X4) ? 4 : (d == BCDEC_ASTC_BLOCK_6X6) ? 6 : 8;
}

size_t bcdec_mali_astc_output_size(int width, int height, bcdec_astc_block_dim_t blockDim, int writeAstcFileHeader) {
    if (width <= 0 || height <= 0) return 0;
    int dim = bcdec__ext_astc_dim(blockDim);
    size_t bx = (size_t)((width + dim - 1) / dim), by = (size_t)((height + dim - 1) / dim);
    return (writeAstcFileHeader ? 16u : 0u) + bx * by * 16u;
}

/* Decodifica un bloque BC a 16 texels RGBA8 (fila-mayor). BC4 -> (R,R,R,255), BC5 -> (R,G,0,255). */
static void bcdec__ext_decode_rgba(bcdec_format_t f, const uint8_t* blk, uint8_t* rgba) {
    if (f == BCDEC_FORMAT_BC4) {
        uint8_t r[16];
        bcdec_decompress_block(f, blk, r, 4);
        for (int i = 0; i < 16; ++i) { rgba[i*4] = r[i]; rgba[i*4+1] = r[i]; rgba[i*4+2] = r[i]; rgba[i*4+3] = 255; }   /* como wrapper_bcdec.c */
    } else if (f == BCDEC_FORMAT_BC5) {
        uint8_t rg[32];
        bcdec_decompress_block(f, blk, rg, 8);
        for (int i = 0; i < 16; ++i) { rgba[i*4] = rg[i*2]; rgba[i*4+1] = rg[i*2+1]; rgba[i*4+2] = 0; rgba[i*4+3] = 255; }
    } else {
        bcdec_decompress_block(f, blk, rgba, 16);     /* BC1, BC2, BC3, BC7 */
    }
}

bool bcdec_mali_transcode_bc_to_astc(
    const void* restrict bcData,
    void* restrict astcData,
    int width,
    int height,
    bcdec_astc_block_dim_t blockDim)
{
    /* La versión original nunca decodificaba el BC (enviaba memoria sin inicializar al codificador ASTC) y no sabía
       el formato de origen. Se desactiva en vez de producir basura: use bcdec_mali_transcode_bc_to_astc_ex(). */
    (void)bcData; (void)astcData; (void)width; (void)height; (void)blockDim;
    return false;
}

bool bcdec_mali_transcode_bc_to_astc_ex(
    bcdec_format_t srcFormat,
    const void* restrict bcData,
    void* restrict astcData,
    size_t astcCapacity,
    int width,
    int height,
    bcdec_astc_block_dim_t blockDim,
    int writeAstcFileHeader,
    size_t* restrict outBytes)
{
    if (!bcData || !astcData || width <= 0 || height <= 0) return false;
    if (srcFormat != BCDEC_FORMAT_BC1 && srcFormat != BCDEC_FORMAT_BC2 && srcFormat != BCDEC_FORMAT_BC3 &&
        srcFormat != BCDEC_FORMAT_BC4 && srcFormat != BCDEC_FORMAT_BC5 && srcFormat != BCDEC_FORMAT_BC7) return false;

    const int dim = bcdec__ext_astc_dim(blockDim);
    const int blocksX = (width + dim - 1) / dim;
    const int blocksY = (height + dim - 1) / dim;
    const size_t need = bcdec_mali_astc_output_size(width, height, blockDim, writeAstcFileHeader);
    if (astcCapacity < need) return false;

    const uint8_t* src = (const uint8_t*)bcData;
    const size_t srcBlockSize = bcdec_format_block_size(srcFormat);
    const int bcBlocksX = (width + 3) >> 2;
    uint8_t* astcBytes = (uint8_t*)astcData;
    uint8_t* payloadPtr = astcBytes;

    if (writeAstcFileHeader) {
        /* Cabecera estándar de contenedor .astc (16 bytes). Las dimensiones son en TEXELES, no en bloques. */
        astcBytes[0] = 0x13; astcBytes[1] = 0xAB; astcBytes[2] = 0xA1; astcBytes[3] = 0x5C;
        astcBytes[4] = (uint8_t)dim; astcBytes[5] = (uint8_t)dim; astcBytes[6] = 1;
        astcBytes[7]  = (uint8_t)(width & 0xFF);  astcBytes[8]  = (uint8_t)((width >> 8) & 0xFF);  astcBytes[9]  = (uint8_t)((width >> 16) & 0xFF);
        astcBytes[10] = (uint8_t)(height & 0xFF); astcBytes[11] = (uint8_t)((height >> 8) & 0xFF); astcBytes[12] = (uint8_t)((height >> 16) & 0xFF);
        astcBytes[13] = 1; astcBytes[14] = 0; astcBytes[15] = 0;
        payloadPtr += 16;
    }

    /* Búfer del bloque ASTC actual: dim*dim texels RGBA8 (máx. 8x8 = 256 bytes), fila-mayor con stride = dim */
    uint8_t pixels[8 * 8 * 4];

    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            const int x0 = bx * dim, y0 = by * dim;
            /* Rango de bloques BC 4x4 cubiertos por este bloque ASTC (con el borde replicado) */
            const int lastX = (x0 + dim - 1 < width)  ? x0 + dim - 1 : width - 1;
            const int lastY = (y0 + dim - 1 < height) ? y0 + dim - 1 : height - 1;
            const int tx0 = x0 >> 2, tx1 = lastX >> 2, ty0 = y0 >> 2, ty1 = lastY >> 2;
            uint8_t decoded[3][3][64];
            int have[3][3] = {{0}};
            (void)tx1; (void)ty1;

            for (int j = 0; j < dim; ++j) {
                int sy = y0 + j; if (sy > height - 1) sy = height - 1;
                for (int i = 0; i < dim; ++i) {
                    int sx = x0 + i; if (sx > width - 1) sx = width - 1;
                    const int cx = (sx >> 2) - tx0, cy = (sy >> 2) - ty0;
                    if (!have[cy][cx]) {
                        const uint8_t* blk = src + ((size_t)(sy >> 2) * (size_t)bcBlocksX + (size_t)(sx >> 2)) * srcBlockSize;
                        bcdec__ext_decode_rgba(srcFormat, blk, decoded[cy][cx]);
                        have[cy][cx] = 1;
                    }
                    memcpy(pixels + ((size_t)j * (size_t)dim + (size_t)i) * 4, decoded[cy][cx] + (((sy & 3) * 4) + (sx & 3)) * 4, 4);
                }
            }
            /* astc_encode_block(texels, dim, format_type, out): format_type 0 = RGB, 1 = RGBA, 2 = RG (BC5).
               Para formatos con alfa se usa RGBA solo si el bloque tiene algún alfa < 255; el RG no existe en 6x6. */
            int formatType = 0;
            if (srcFormat == BCDEC_FORMAT_BC5) {
                formatType = (dim == 6) ? 0 : 2;
            } else if (srcFormat != BCDEC_FORMAT_BC4) {
                for (int p = 0; p < dim * dim; ++p) if (pixels[p * 4 + 3] != 255) { formatType = 1; break; }
            }
            astc_encode_block((const uint8_t*)pixels, dim, formatType, payloadPtr);
            payloadPtr += 16; // Cada bloque ASTC codificado ocupa exactamente 16 bytes (128 bits)
        }
    }
    if (outBytes) *outBytes = need;
    return true;
}

void bcdec_mali_apply_foveated_compression_mask(
    void* restrict compressedData,
    int width,
    int height,
    int bytesPerBlock,
    float foveationIntensity)
{
    if (!compressedData || foveationIntensity <= 0.0f || width <= 0 || height <= 0) return;
    if (bytesPerBlock != 8) return;   /* con 16 bytes por bloque no se puede saber si es BC7/BC6H (bits de modo en el byte 0) */
    if (foveationIntensity > 1.0f) foveationIntensity = 1.0f;

    uint8_t* data = (uint8_t*)compressedData;
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    const float centerX = (float)blocksX * 0.5f;
    const float centerY = (float)blocksY * 0.5f;

    for (int y = 0; y < blocksY; y++) {
        for (int x = 0; x < blocksX; x++) {
            float normX = (centerX > 0.0f) ? ((float)x - centerX) / centerX : 0.0f;
            float normY = (centerY > 0.0f) ? ((float)y - centerY) / centerY : 0.0f;
            float distance = sqrtf(normX * normX + normY * normY);
            if (distance > (1.0f - foveationIntensity)) {
                size_t blockIdx = ((size_t)y * (size_t)blocksX + (size_t)x) * (size_t)bytesPerBlock;
                data[blockIdx] &= 0xFC;     /* BC1: bits bajos del extremo c0 / BC4: del extremo alfa0 (sin tocar modos) */
            }
        }
    }
}

void bcdec_mali_apply_foveated_compression_mask_ex(
    bcdec_format_t format,
    void* restrict compressedData,
    int width,
    int height,
    float foveationIntensity)
{
    if (!compressedData || foveationIntensity <= 0.0f || width <= 0 || height <= 0) return;
    if (format < BCDEC_FORMAT_BC1 || format > BCDEC_FORMAT_BC5) return;   /* BC6H / BC7: no se tocan */
    if (foveationIntensity > 1.0f) foveationIntensity = 1.0f;

    const size_t bytesPerBlock = bcdec_format_block_size(format);
    uint8_t* data = (uint8_t*)compressedData;
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    const float centerX = (float)blocksX * 0.5f;
    const float centerY = (float)blocksY * 0.5f;

    for (int y = 0; y < blocksY; y++) {
        for (int x = 0; x < blocksX; x++) {
            float normX = (centerX > 0.0f) ? ((float)x - centerX) / centerX : 0.0f;
            float normY = (centerY > 0.0f) ? ((float)y - centerY) / centerY : 0.0f;
            float distance = sqrtf(normX * normX + normY * normY);
            if (distance > (1.0f - foveationIntensity)) {
                size_t blockIdx = ((size_t)y * (size_t)blocksX + (size_t)x) * bytesPerBlock;
                data[blockIdx] &= 0xFC;     /* byte 0 de BC1-BC5 = bits de un extremo / valores de alfa, nunca bits de modo */
            }
        }
    }
}

bool bcdec_vulkan_create_bindless_table(
    const void* device,
    const void* const* imageViews,
    uint32_t imageViewCount,
    bcdec_vulkan_bindless_descriptor_t* outDescriptors)
{
    if (!device || !imageViews || !outDescriptors || imageViewCount == 0) return false;

    // Adaptado para Vulkan 1.0 / 1.1 (emulación mediante índices y una tabla host-side; no es bindless real)
    for (uint32_t i = 0; i < imageViewCount; i++) {
        outDescriptors[i].descriptorIndex = i;
        outDescriptors[i].bufferDeviceAddress = (uint64_t)(uintptr_t)imageViews[i];
        outDescriptors[i].range = sizeof(bcdec_vulkan_bindless_descriptor_t);
    }
    return true;
}

bool bcdec_vulkan_create_handle_table(
    uint32_t imageViewCount,
    const uint64_t* imageViewHandles,
    bcdec_vulkan_bindless_descriptor_t* outDescriptors)
{
    if (!imageViewHandles || !outDescriptors || imageViewCount == 0) return false;
    for (uint32_t i = 0; i < imageViewCount; i++) {
        outDescriptors[i].descriptorIndex = i;
        outDescriptors[i].bufferDeviceAddress = imageViewHandles[i];
        outDescriptors[i].range = sizeof(bcdec_vulkan_bindless_descriptor_t);
    }
    return true;
}

int bcdec_mali_get_optimal_worker_threads(bcdec_power_mode_t powerMode) {
    int want;
    switch (powerMode) {
        case BCDEC_POWER_MODE_PERFORMANCE: want = 4; break;
        case BCDEC_POWER_MODE_BALANCED: want = 2; break;
        case BCDEC_POWER_MODE_BATTERY_SAVER: want = 1; break;
        default: want = 2; break;
    }
#if defined(_SC_NPROCESSORS_ONLN)
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores >= 1 && want > (int)cores) want = (int)cores;     /* no más hilos que núcleos activos */
#endif
    return want;
}
