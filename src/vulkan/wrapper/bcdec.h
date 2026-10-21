/* bcdec.h - v0.98 (Android Bionic / Mesa 26 Adaptation for Bannerlator / Winlator)
   Decompress blocks of BC compressed images for X11/Gallium emulation.
   License: MIT / Unlicense (See end of file)
*/

#ifndef BCDEC_HEADER_INCLUDED
#define BCDEC_HEADER_INCLUDED

#include <stdint.8>
#include <stddef.h>

/* Integración específica para Android Bionic y Mesa */
#if defined(__ANDROID__) || defined(DETECT_OS_ANDROID)
#  include <sys/types.h>
#endif

#define BCDEC_VERSION_MAJOR 0
#define BCDEC_VERSION_MINOR 98

/* Control de visibilidad de símbolos para Bionic NDK y compilación en Mesa */
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

#define BCDEC_BC1_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC1_BLOCK_SIZE)
#define BCDEC_BC2_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC2_BLOCK_SIZE)
#define BCDEC_BC3_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC3_BLOCK_SIZE)
#define BCDEC_BC4_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC4_BLOCK_SIZE)
#define BCDEC_BC5_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC5_BLOCK_SIZE)
#define BCDEC_BC6H_COMPRESSED_SIZE(w, h)    ((((w)>>2)*((h)>>2))*BCDEC_BC6H_BLOCK_SIZE)
#define BCDEC_BC7_COMPRESSED_SIZE(w, h)     ((((w)>>2)*((h)>>2))*BCDEC_BC7_BLOCK_SIZE)

BCDECDEF void bcdec_bc1(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc2(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc3(const void* compressedBlock, void* decompressedBlock, int destinationPitch);

#ifndef BCDEC_BC4BC5_PRECISE
BCDECDEF void bcdec_bc4(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
BCDECDEF void bcdec_bc5(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
#else
BCDECDEF void bcdec_bc4(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc5(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc4_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc5_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
#endif

BCDECDEF void bcdec_bc6h_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc6h_half(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned);
BCDECDEF void bcdec_bc7(const void* compressedBlock, void* decompressedBlock, int destinationPitch);

#endif /* BCDEC_HEADER_INCLUDED */

#ifdef BCDEC_IMPLEMENTATION

static void bcdec__color_block(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int onlyOpaqueMode) {
    uint16_t c0, c1;
    uint32_t refColors[4]; /* 0xAABBGGRR */
    uint8_t* dstColors;
    uint32_t colorIndices;
    int i, j, idx;
    uint32_t r0, g0, b0, r1, g1, b1, r, g, b;

    c0 = ((const uint16_t*)compressedBlock)[0];
    c1 = ((const uint16_t*)compressedBlock)[1];

    /* Unpack 565 ref colors */
    r0 = (c0 >> 11) & 0x1F;
    g0 = (c0 >> 5)  & 0x3F;
    b0 =  c0        & 0x1F;

    r1 = (c1 >> 11) & 0x1F;
    g1 = (c1 >> 5)  & 0x3F;
    b1 =  c1        & 0x1F;

    /* Expand 565 ref colors to 888 */
    r = (r0 * 527 + 23) >> 6;
    g = (g0 * 259 + 33) >> 6;
    b = (b0 * 527 + 23) >> 6;
    refColors[0] = 0xFF000000 | (b << 16) | (g << 8) | r;

    r = (r1 * 527 + 23) >> 6;
    g = (g1 * 259 + 33) >> 6;
    b = (b1 * 527 + 23) >> 6;
    refColors[1] = 0xFF000000 | (b << 16) | (g << 8) | r;

    if (c0 > c1 || onlyOpaqueMode) {
        r = ((2 * r0 + r1) *  351 +   61) >>  7;
        g = ((2 * g0 + g1) * 2763 + 1039) >> 11;
        b = ((2 * b0 + b1) *  351 +   61) >>  7;
        refColors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;

        r = ((r0 + r1 * 2) *  351 +   61) >>  7;
        g = ((g0 + g1 * 2) * 2763 + 1039) >> 11;
        b = ((b0 + b1 * 2) *  351 +   61) >>  7;
        refColors[3] = 0xFF000000 | (b << 16) | (g << 8) | r;
    } else {
        r = ((r0 + r1) * 1053 +  125) >>  8;
        g = ((g0 + g1) * 4145 + 1019) >> 11;
        b = ((b0 + b1) * 1053 +  125) >>  8;
        refColors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;
        refColors[3] = 0x00000000;
    }

    colorIndices = ((const uint32_t*)compressedBlock)[1];

    dstColors = (uint8_t*)decompressedBlock;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            idx = colorIndices & 0x03;
            ((uint32_t*)dstColors)[j] = refColors[idx];
            colorIndices >>= 2;
        }
        dstColors += destinationPitch;
    }
}

static void bcdec__sharp_alpha_block(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    const uint16_t* alpha = (const uint16_t*)compressedBlock;
    uint8_t* decompressed = (uint8_t*)decompressedBlock;
    int i, j;

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            decompressed[j * 4] = ((alpha[i] >> (4 * j)) & 0x0F) * 17;
        }
        decompressed += destinationPitch;
    }
}

static void bcdec__smooth_alpha_block(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int pixelSize) {
    uint8_t* decompressed;
    uint8_t alpha[8];
    int i, j;
    uint64_t block, indices;

    block = *(const uint64_t*)compressedBlock;
    decompressed = (uint8_t*)decompressedBlock;

    alpha[0] = block & 0xFF;
    alpha[1] = (block >> 8) & 0xFF;

    if (alpha[0] > alpha[1]) {
        alpha[2] = (6 * alpha[0] +     alpha[1]) / 7;
        alpha[3] = (5 * alpha[0] + 2 * alpha[1]) / 7;
        alpha[4] = (4 * alpha[0] + 3 * alpha[1]) / 7;
        alpha[5] = (3 * alpha[0] + 4 * alpha[1]) / 7;
        alpha[6] = (2 * alpha[0] + 5 * alpha[1]) / 7;
        alpha[7] = (    alpha[0] + 6 * alpha[1]) / 7;
    } else {
        alpha[2] = (4 * alpha[0] +     alpha[1]) / 5;
        alpha[3] = (3 * alpha[0] + 2 * alpha[1]) / 5;
        alpha[4] = (2 * alpha[0] + 3 * alpha[1]) / 5;
        alpha[5] = (    alpha[0] + 4 * alpha[1]) / 5;
        alpha[6] = 0x00;
        alpha[7] = 0xFF;
    }

    indices = block >> 16;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            decompressed[j * pixelSize] = alpha[indices & 0x07];
            indices >>= 3;
        }
        decompressed += destinationPitch;
    }
}

#ifdef BCDEC_BC4BC5_PRECISE
static void bcdec__bc4_block(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int pixelSize, int isSigned) {
    int8_t* sblock;
    uint8_t* ublock;
    int alpha[8];
    int i, j;
    uint64_t block, indices;

    static const int aWeights4[4] = { 13107, 26215, 39321, 52429 };
    static const int aWeights6[6] = { 9363, 18724, 28086, 37450, 46812, 56173 };

    block = *(const uint64_t*)compressedBlock;

    if (isSigned) {
        alpha[0] = (int8_t)(block & 0xFF);
        alpha[1] = (int8_t)((block >> 8) & 0xFF);
        if (alpha[0] < -127) alpha[0] = -127;
        if (alpha[1] < -127) alpha[1] = -127;
    } else {
        alpha[0] = block & 0xFF;
        alpha[1] = (block >> 8) & 0xFF;
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
        alpha[6] = isSigned ? -127 :   0;
        alpha[7] = isSigned ?  127 : 255;
    }

    indices = block >> 16;
    if (isSigned) {
        sblock = (int8_t*)decompressedBlock;
        for (i = 0; i < 4; ++i) {
            for (j = 0; j < 4; ++j) {
                sblock[j * pixelSize] = (int8_t)alpha[indices & 0x07];
                indices >>= 3;
            }
            sblock += destinationPitch;
        }
    } else {
        ublock = (uint8_t*)decompressedBlock;
        for (i = 0; i < 4; ++i) {
            for (j = 0; j < 4; ++j) {
                ublock[j * pixelSize] = (uint8_t)alpha[indices & 0x07];
                indices >>= 3;
            }
            ublock += destinationPitch;
        }
    }
}

static void bcdec__bc4_block_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int pixelSize, int isSigned) {
    float* decompressed;
    float alpha[8];
    int i, j;
    uint64_t block, indices;

    block = *(const uint64_t*)compressedBlock;
    decompressed = (float*)decompressedBlock;

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
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            decompressed[j * pixelSize] = alpha[indices & 0x07];
            indices >>= 3;
        }
        decompressed += destinationPitch;
    }
}
#endif /* BCDEC_BC4BC5_PRECISE */

typedef struct bcdec__bitstream {
    uint64_t low;
    uint64_t high;
} bcdec__bitstream_t;

static int bcdec__bitstream_read_bits(bcdec__bitstream_t* bstream, int numBits) {
    uint32_t mask = (1U << numBits) - 1U;
    uint32_t bits = (uint32_t)(bstream->low & mask);

    bstream->low >>= numBits;
    bstream->low |= (bstream->high & mask) << (sizeof(bstream->high) * 8 - numBits);
    bstream->high >>= numBits;

    return (int)bits;
}

static int bcdec__bitstream_read_bit(bcdec__bitstream_t* bstream) {
    return bcdec__bitstream_read_bits(bstream, 1);
}

static int bcdec__bitstream_read_bits_r(bcdec__bitstream_t* bstream, int numBits) {
    int bits = bcdec__bitstream_read_bits(bstream, numBits);
    int result = 0;
    while (numBits--) {
        result <<= 1;
        result |= (bits & 1);
        bits >>= 1;
    }
    return result;
}

BCDECDEF void bcdec_bc1(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__color_block(compressedBlock, decompressedBlock, destinationPitch, 0);
}

BCDECDEF void bcdec_bc2(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__color_block(((const char*)compressedBlock) + 8, decompressedBlock, destinationPitch, 1);
    bcdec__sharp_alpha_block(compressedBlock, ((char*)decompressedBlock) + 3, destinationPitch);
}

BCDECDEF void bcdec_bc3(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__color_block(((const char*)compressedBlock) + 8, decompressedBlock, destinationPitch, 1);
    bcdec__smooth_alpha_block(compressedBlock, ((char*)decompressedBlock) + 3, destinationPitch, 4);
}

#ifndef BCDEC_BC4BC5_PRECISE
BCDECDEF void bcdec_bc4(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__smooth_alpha_block(compressedBlock, decompressedBlock, destinationPitch, 1);
}

BCDECDEF void bcdec_bc5(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__smooth_alpha_block(compressedBlock, decompressedBlock, destinationPitch, 2);
    bcdec__smooth_alpha_block(((const char*)compressedBlock) + 8, ((char*)decompressedBlock) + 1, destinationPitch, 2);
}
#else
BCDECDEF void bcdec_bc4(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block(compressedBlock, decompressedBlock, destinationPitch, 1, isSigned);
}

BCDECDEF void bcdec_bc5(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block(compressedBlock, decompressedBlock, destinationPitch, 2, isSigned);
    bcdec__bc4_block(((const char*)compressedBlock) + 8, ((char*)decompressedBlock) + 1, destinationPitch, 2, isSigned);
}

BCDECDEF void bcdec_bc4_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block_float(compressedBlock, decompressedBlock, destinationPitch, 1, isSigned);
}

BCDECDEF void bcdec_bc5_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
    bcdec__bc4_block_float(compressedBlock, decompressedBlock, destinationPitch, 2, isSigned);
    bcdec__bc4_block_float(((const char*)compressedBlock) + 8, ((float*)decompressedBlock) + 1, destinationPitch, 2, isSigned);
}
#endif

static int bcdec__extend_sign(int val, int bits) {
    return (val << (32 - bits)) >> (32 - bits);
}

static int bcdec__transform_inverse(int val, int a0, int bits, int isSigned) {
    val = (val + a0) & ((1 << bits) - 1);
    if (isSigned) {
        val = bcdec__extend_sign(val, bits);
    }
    return val;
}

static int bcdec__unquantize(int val, int bits, int isSigned) {
    int unq, s = 0;

    if (!isSigned) {
        if (bits >= 15) {
            unq = val;
        } else if (!val) {
            unq = 0;
        } else if (val == ((1 << bits) - 1)) {
            unq = 0xFFFF;
        } else {
            unq = ((val << 16) + 0x8000) >> bits;
        }
    } else {
        if (bits >= 16) {
            unq = val;
        } else {
            if (val < 0) {
                s = 1;
                val = -val;
            }

            if (val == 0) {
                unq = 0;
            } else if (val >= ((1 << (bits - 1)) - 1)) {
                unq = 0x7FFF;
            } else {
                unq = ((val << 15) + 0x4000) >> (bits - 1);
            }

            if (s) {
                unq = -unq;
            }
        }
    }
    return unq;
}

static int bcdec__interpolate(int a, int b, const int* weights, int index) {
    return (a * (64 - weights[index]) + b * weights[index] + 32) >> 6;
}

static uint16_t bcdec__finish_unquantize(int val, int isSigned) {
    int s;

    if (!isSigned) {
        return (uint16_t)((val * 31) >> 6);
    } else {
        val = (val < 0) ? -(((-val) * 31) >> 5) : (val * 31) >> 5;
        s = 0;
        if (val < 0) {
            s = 0x8000;
            val = -val;
        }
        return (uint16_t)(s | val);
    }
}

static float bcdec__half_to_float_quick(uint16_t half) {
    typedef union {
        uint32_t u;
        float f;
    } FP32;

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

BCDECDEF void bcdec_bc6h_half(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
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
    int mode, partition, numPartitions, i, j, partitionSet, indexBits, index, ep_i, actualBits0Mode;
    int r[4], g[4], b[4];
    uint16_t* decompressed = (uint16_t*)decompressedBlock;
    const int* weights;

    bstream.low  = ((const uint64_t*)compressedBlock)[0];
    bstream.high = ((const uint64_t*)compressedBlock)[1];

    r[0] = r[1] = r[2] = r[3] = 0;
    g[0] = g[1] = g[2] = g[3] = 0;
    b[0] = b[1] = b[2] = b[3] = 0;

    mode = bcdec__bitstream_read_bits(&bstream, 2);
    if (mode > 1) {
        mode |= (bcdec__bitstream_read_bits(&bstream, 3) << 2);
    }

    partition = 0;

    switch (mode) {
        case 0b00:
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 0;
            break;

        case 0b01:
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            r[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 7);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6);
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 1;
            break;

        case 0b00010:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 2;
            break;

        case 0b00110:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 3;
            break;

        case 0b01010:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 4;
            break;

        case 0b01110:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 9);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 9);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 9);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 5;
            break;

        case 0b10010:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6);
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 6;
            break;

        case 0b10110:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 7;
            break;

        case 0b11010:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            r[3] |= bcdec__bitstream_read_bits(&bstream, 5);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 8;
            break;

        case 0b11110:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[3] |= bcdec__bitstream_read_bit(&bstream);
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 1;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            g[0] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[2] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 2;
            g[2] |= bcdec__bitstream_read_bit(&bstream) << 4;
            b[0] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 3;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 5;
            b[3] |= bcdec__bitstream_read_bit(&bstream) << 4;
            r[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            g[3] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 6);
            b[2] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[2] |= bcdec__bitstream_read_bits(&bstream, 6);
            r[3] |= bcdec__bitstream_read_bits(&bstream, 6);
            partition = bcdec__bitstream_read_bits(&bstream, 5);
            mode = 9;
            break;

        case 0b00011:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[1] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[1] |= bcdec__bitstream_read_bits(&bstream, 10);
            mode = 10;
            break;

        case 0b00111:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            r[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            g[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            g[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            b[1] |= bcdec__bitstream_read_bits(&bstream, 9);
            b[0] |= bcdec__bitstream_read_bit(&bstream) << 10;
            mode = 11;
            break;

        case 0b01011:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            r[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10;
            g[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            g[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10;
            b[1] |= bcdec__bitstream_read_bits(&bstream, 8);
            b[0] |= bcdec__bitstream_read_bits_r(&bstream, 2) << 10;
            mode = 12;
            break;

        case 0b01111:
            r[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            g[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            b[0] |= bcdec__bitstream_read_bits(&bstream, 10);
            r[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            r[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10;
            g[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            g[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10;
            b[1] |= bcdec__bitstream_read_bits(&bstream, 4);
            b[0] |= bcdec__bitstream_read_bits_r(&bstream, 6) << 10;
            mode = 13;
            break;

        default:
            for (i = 0; i < 4; ++i) {
                for (j = 0; j < 4; ++j) {
                    decompressed[j * 3 + 0] = 0;
                    decompressed[j * 3 + 1] = 0;
                    decompressed[j * 3 + 2] = 0;
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

            if (partitionSet & 0x80) {
                indexBits--;
            }
            partitionSet &= 0x01;

            index = bcdec__bitstream_read_bits(&bstream, indexBits);
            ep_i = partitionSet * 2;

            decompressed[j * 3 + 0] = bcdec__finish_unquantize(
                                            bcdec__interpolate(r[ep_i], r[ep_i+1], weights, index), isSigned);
            decompressed[j * 3 + 1] = bcdec__finish_unquantize(
                                            bcdec__interpolate(g[ep_i], g[ep_i+1], weights, index), isSigned);
            decompressed[j * 3 + 2] = bcdec__finish_unquantize(
                                            bcdec__interpolate(b[ep_i], b[ep_i+1], weights, index), isSigned);
        }
        decompressed += destinationPitch;
    }
}

BCDECDEF void bcdec_bc6h_float(const void* compressedBlock, void* decompressedBlock, int destinationPitch, int isSigned) {
    uint16_t block[16*3];
    float* decompressed;
    const uint16_t* b;
    int i, j;

    bcdec_bc6h_half(compressedBlock, block, 4*3, isSigned);
    b = block;
    decompressed = (float*)decompressedBlock;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            decompressed[j * 3 + 0] = bcdec__half_to_float_quick(*b++);
            decompressed[j * 3 + 1] = bcdec__half_to_float_quick(*b++);
            decompressed[j * 3 + 2] = bcdec__half_to_float_quick(*b++);
        }
        decompressed += destinationPitch;
    }
}

static void bcdec__swap_values(int* a, int* b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

BCDECDEF void bcdec_bc7(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    static const char actual_bits_count[2][8] = {
        { 4, 6, 5, 7, 5, 7, 7, 5 },
        { 0, 0, 0, 0, 6, 8, 7, 5 },
    };

    static const uint8_t partition_sets[2][64][4][4] = {
        {
            { {128, 0,   1, 1}, {0, 0,   1, 1}, {  0, 0, 1, 1}, {0, 0, 1, 129} },
            { {128, 0,   0, 1}, {0, 0,   0, 1}, {  0, 0, 0, 1}, {0, 0, 0, 129} },
            { {128, 1,   1, 1}, {0, 1,   1, 1}, {  0, 1, 1, 1}, {0, 1, 1, 129} },
            { {128, 0,   0, 1}, {0, 0,   1, 1}, {  0, 0, 1, 1}, {0, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 1}, {  0, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0,   1, 1}, {0, 1,   1, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 1}, {0, 0,   1, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 1}, {  0, 0, 1, 1}, {0, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {  0, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0,   1, 1}, {0, 1,   1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 1}, {  0, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {  0, 0, 0, 1}, {0, 1, 1, 129} },
            { {128, 0,   0, 1}, {0, 1,   1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {1, 1,   1, 1}, {  1, 1, 1, 1}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {  0, 0, 0, 0}, {1, 1, 1, 129} },
            { {128, 0,   0, 0}, {1, 0,   0, 0}, {  1, 1, 1, 0}, {1, 1, 1, 129} },
            { {128, 1, 129, 1}, {0, 0,   0, 1}, {  0, 0, 0, 0}, {0, 0, 0,   0} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {129, 0, 0, 0}, {1, 1, 1,   0} },
            { {128, 1, 129, 1}, {0, 0,   1, 1}, {  0, 0, 0, 1}, {0, 0, 0,   0} },
            { {128, 0, 129, 1}, {0, 0,   0, 1}, {  0, 0, 0, 0}, {0, 0, 0,   0} },
            { {128, 0,   0, 0}, {1, 0,   0, 0}, {129, 1, 0, 0}, {1, 1, 1,   0} },
            { {128, 0,   0, 0}, {0, 0,   0, 0}, {129, 0, 0, 0}, {1, 1, 0,   0} },
            { {128, 1,   1, 1}, {0, 0,   1, 1}, {  0, 0, 1, 1}, {0, 0, 0, 129} },
            { {128, 0, 129, 1}, {0, 0,   0, 1}, {  0, 0, 0, 1}, {0, 0, 0,   0} },
            { {128, 0,   0, 0}, {1, 0,   0, 0}, {129, 0, 0, 0}, {1, 1, 0,   0} },
            { {128, 1, 129, 0}, {0, 1,   1, 0}, {  0, 1, 1, 0}, {0, 1, 1,   0} },
            { {128, 0, 129, 1}, {0, 1,   1, 0}, {  0, 1, 1, 0}, {1, 1, 0,   0} },
            { {128, 0,   0, 1}, {0, 1,   1, 1}, {129, 1, 1, 0}, {1, 0, 0,   0} },
            { {128, 0,   0, 0}, {1, 1,   1, 1}, {129, 1, 1, 1}, {0, 0, 0,   0} },
            { {128, 1, 129, 1}, {0, 0,   0, 1}, {  1, 0, 0, 0}, {1, 1, 1,   0} },
            { {128, 0, 129, 1}, {1, 0,   0, 1}, {  1, 0, 0, 1}, {1, 1, 0,   0} },
            { {128, 1,   0, 1}, {0, 1,   0, 1}, {  0, 1, 0, 1}, {0, 1, 0, 129} },
            { {128, 0,   0, 0}, {1, 1,   1, 1}, {  0, 0, 0, 0}, {1, 1, 1, 129} },
            { {128, 1,   0, 1}, {1, 0, 129, 0}, {  0, 1, 0, 1}, {1, 0, 1,   0} },
            { {128, 0,   1, 1}, {0, 0,   1, 1}, {129, 1, 0, 0}, {1, 1, 0,   0} },
            { {128, 0, 129, 1}, {1, 1,   0, 0}, {  0, 0, 1, 1}, {1, 1, 0,   0} },
            { {128, 1,   0, 1}, {0, 1,   0, 1}, {129, 0, 1, 0}, {1, 0, 1,   0} },
            { {128, 1,   1, 0}, {1, 0,   0, 1}, {  0, 1, 1, 0}, {1, 0, 0, 129} },
            { {128, 1,   0, 1}, {1, 0,   1, 0}, {  1, 0, 1, 0}, {0, 1, 0, 129} },
            { {128, 1, 129, 1}, {0, 0,   1, 1}, {  1, 1, 0, 0}, {1, 1, 1,   0} },
            { {128, 0,   0, 1}, {0, 0,   1, 1}, {129, 1, 0, 0}, {1, 0, 0,   0} },
            { {128, 0, 129, 1}, {0, 0,   1, 0}, {  0, 1, 0, 0}, {1, 1, 0,   0} },
            { {128, 0, 129, 1}, {1, 0,   1, 1}, {  1, 1, 0, 1}, {1, 1, 0,   0} },
            { {128, 1, 129, 0}, {1, 0,   0, 1}, {  1, 0, 0, 1}, {0, 1, 1,   0} },
            { {128, 0,   1, 1}, {1, 1,   0, 0}, {  1, 1, 0, 0}, {0, 0, 1, 129} },
            { {128, 1,   1, 0}, {0, 1,   1, 0}, {  1, 0, 0, 1}, {1, 0, 0, 129} },
            { {128, 0,   0, 0}, {0, 1, 129, 0}, {  0, 1, 1, 0}, {0, 0, 0,   0} },
            { {128, 1,   0, 0}, {1, 1, 129, 0}, {  0, 1, 0, 0}, {0, 0, 0,   0} },
            { {128, 0, 129, 0}, {0, 1,   1, 1}, {  0, 0, 1, 0}, {0, 0, 0,   0} },
            { {128, 0,   0, 0}, {0, 0, 129, 0}, {  0, 1, 1, 1}, {0, 0, 1,   0} },
            { {128, 0,   0, 0}, {0, 1,   0, 0}, {129, 1, 1, 0}, {0, 1, 0,   0} },
            { {128, 1,   1, 0}, {1, 1,   0, 0}, {  1, 0, 0, 1}, {0, 0, 1, 129} },
            { {128, 0,   1, 1}, {0, 1,   1, 0}, {  1, 1, 0, 0}, {1, 0, 0, 129} },
            { {128, 1, 129, 0}, {0, 0,   1, 1}, {  1, 0, 0, 1}, {1, 1, 0,   0} },
            { {128, 0, 129, 1}, {1, 0,   0, 1}, {  1, 1, 0, 0}, {0, 1, 1,   0} },
            { {128, 1,   1, 0}, {1, 1,   0, 0}, {  1, 1, 0, 0}, {1, 0, 0, 129} },
            { {128, 1,   1, 0}, {0, 0,   1, 1}, {  0, 0, 1, 1}, {1, 0, 0, 129} },
            { {128, 1,   1, 1}, {1, 1,   1, 0}, {  1, 0, 0, 0}, {0, 0, 0, 129} },
            { {128, 0,   0, 1}, {1, 0,   0, 0}, {  1, 1, 1, 0}, {0, 1, 1, 129} },
            { {128, 0,   0, 0}, {1, 1,   1, 1}, {  0, 0, 1, 1}, {0, 0, 1, 129} },
            { {128, 0, 129, 1}, {0, 0,   1, 1}, {  1, 1, 1, 1}, {0, 0, 0,   0} },
            { {128, 0, 129, 0}, {0, 0,   1, 0}, {  1, 1, 1, 0}, {1, 1, 1,   0} },
            { {128, 1,   0, 0}, {0, 1,   0, 0}, {  0, 1, 1, 1}, {0, 1, 1, 129} }
        },
        {
            { {128, 0, 1, 129}, {0,   0,   1, 1}, {  0,   2,   2, 1}, {  2,   2, 2, 130} },
            { {128, 0, 0, 129}, {0,   0,   1, 1}, {130,   2,   1, 1}, {  2,   2, 2,   1} },
            { {128, 0, 0,   0}, {2,   0,   0, 1}, {130,   2,   1, 1}, {  2,   2, 1, 129} },
            { {128, 2, 2, 130}, {0,   0,   2, 2}, {  0,   0,   1, 1}, {  0,   1, 1, 129} },
            { {128, 0, 0,   0}, {0,   0,   0, 0}, {129,   1,   2, 2}, {  1,   1, 2, 130} },
            { {128, 0, 1, 129}, {0,   0,   1, 1}, {  0,   0,   2, 2}, {  0,   0, 2, 130} },
            { {128, 0, 2, 130}, {0,   0,   2, 2}, {  1,   1,   1, 1}, {  1,   1, 1, 129} },
            { {128, 0, 1,   1}, {0,   0,   1, 1}, {130,   2,   1, 1}, {  2,   2, 1, 129} },
            { {128, 0, 0,   0}, {0,   0,   0, 0}, {129,   1,   1, 1}, {  2,   2, 2, 130} },
            { {128, 0, 0,   0}, {1,   1,   1, 1}, {129,   1,   1, 1}, {  2,   2, 2, 130} },
            { {128, 0, 0,   0}, {1,   1, 129, 1}, {  2,   2,   2, 2}, {  2,   2, 2, 130} },
            { {128, 0, 1,   2}, {0,   0, 129, 2}, {  0,   0,   1, 2}, {  0,   0, 1, 130} },
            { {128, 1, 1,   2}, {0,   1, 129, 2}, {  0,   1,   1, 2}, {  0,   1, 1, 130} },
            { {128, 1, 2,   2}, {0, 129,   2, 2}, {  0,   1,   2, 2}, {  0,   1, 2, 130} },
            { {128, 0, 1, 129}, {0,   1,   1, 2}, {  1,   1,   2, 2}, {  1,   2, 2, 130} },
            { {128, 0, 1, 129}, {2,   0,   0, 1}, {130,   2,   0, 0}, {  2,   2, 2,   0} },
            { {128, 0, 0, 129}, {0,   0,   1, 1}, {  0,   1,   1, 2}, {  1,   1, 2, 130} },
            { {128, 1, 1, 129}, {0,   0,   1, 1}, {130,   0,   0, 1}, {  2,   2, 0,   0} },
            { {128, 0, 0,   0}, {1,   1,   2, 2}, {129,   1,   2, 2}, {  1,   1, 2, 130} },
            { {128, 0, 2, 130}, {0,   0,   2, 2}, {  0,   0,   2, 2}, {  1,   1, 1, 129} },
            { {128, 1, 1, 129}, {0,   1,   1, 1}, {  0,   2,   2, 2}, {  0,   2, 2, 130} },
            { {128, 0, 0, 129}, {0,   0,   0, 1}, {130,   2,   2, 1}, {  2,   2, 2,   1} },
            { {128, 0, 0,   0}, {0,   0, 129, 1}, {  0,   1,   2, 2}, {  0,   1, 2, 130} },
            { {128, 0, 0,   0}, {1,   1,   0, 0}, {130,   2, 129, 0}, {  2,   2, 1,   0} },
            { {128, 1, 2, 130}, {0, 129,   2, 2}, {  0,   0,   1, 1}, {  0,   0, 0,   0} },
            { {128, 0, 1,   2}, {0,   0,   1, 2}, {129,   1,   2, 2}, {  2,   2, 2, 130} },
            { {128, 1, 1,   0}, {1,   2, 130, 1}, {129,   2,   2, 1}, {  0,   1, 1,   0} },
            { {128, 0, 0,   0}, {0,   1, 129, 0}, {  1,   2, 130, 1}, {  1,   2, 2,   1} },
            { {128, 0, 2,   2}, {1,   1,   0, 2}, {129,   1,   0, 2}, {  0,   0, 2, 130} },
            { {128, 1, 1,   0}, {0, 129,   1, 0}, {  2,   0,   0, 2}, {  2,   2, 2, 130} },
            { {128, 0, 1,   1}, {0,   1,   2, 2}, {  0,   1, 130, 2}, {  0,   0, 1, 129} },
            { {128, 0, 0,   0}, {2,   0,   0, 0}, {130,   2,   1, 1}, {  2,   2, 2, 129} },
            { {128, 0, 0,   0}, {0,   0,   0, 2}, {129,   1,   2, 2}, {  1,   2, 2, 130} },
            { {128, 2, 2, 130}, {0,   0,   2, 2}, {  0,   0,   1, 2}, {  0,   0, 1, 129} },
            { {128, 0, 1, 129}, {0,   0,   1, 2}, {  0,   0,   2, 2}, {  0,   2, 2, 130} },
            { {128, 1, 2,   0}, {0, 129,   2, 0}, {  0,   1, 130, 0}, {  0,   1, 2,   0} },
            { {128, 0, 0,   0}, {1,   1, 129, 1}, {  2,   2, 130, 2}, {  0,   0, 0,   0} },
            { {128, 1, 2,   0}, {1,   2,   0, 1}, {130,   0, 129, 2}, {  0,   1, 2,   0} },
            { {128, 1, 2,   0}, {2,   0,   1, 2}, {129, 130,   0, 1}, {  0,   1, 2,   0} },
            { {128, 0, 1,   1}, {2,   2,   0, 0}, {  1,   1, 130, 2}, {  0,   0, 1, 129} },
            { {128, 0, 1,   1}, {1,   1, 130, 2}, {  2,   2,   0, 0}, {  0,   0, 1, 129} },
            { {128, 1, 0, 129}, {0,   1,   0, 1}, {  2,   2,   2, 2}, {  2,   2, 2, 130} },
            { {128, 0, 0,   0}, {0,   0,   0, 0}, {130,   1,   2, 1}, {  2,   1, 2, 129} },
            { {128, 0, 2,   2}, {1, 129,   2, 2}, {  0,   0,   2, 2}, {  1,   1, 2, 130} },
            { {128, 0, 2, 130}, {0,   0,   1, 1}, {  0,   0,   2, 2}, {  0,   0, 1, 129} },
            { {128, 2, 2,   0}, {1,   2, 130, 1}, {  0,   2,   2, 0}, {  1,   2, 2, 129} },
            { {128, 1, 0,   1}, {2,   2, 130, 2}, {  2,   2,   2, 2}, {  0,   1, 0, 129} },
            { {128, 0, 0,   0}, {2,   1,   2, 1}, {130,   1,   2, 1}, {  2,   1, 2, 129} },
            { {128, 1, 0, 129}, {0,   1,   0, 1}, {  0,   1,   0, 1}, {  2,   2, 2, 130} },
            { {128, 2, 2, 130}, {0,   1,   1, 1}, {  0,   2,   2, 2}, {  0,   1, 1, 129} },
            { {128, 0, 0,   2}, {1, 129,   1, 2}, {  0,   0,   0, 2}, {  1,   1, 1, 130} },
            { {128, 0, 0,   0}, {2, 129,   1, 2}, {  2,   1,   1, 2}, {  2,   1, 1, 130} },
            { {128, 2, 2,   2}, {0, 129,   1, 1}, {  0,   1,   1, 1}, {  0,   2, 2, 130} },
            { {128, 0, 0,   2}, {1,   1,   1, 2}, {129,   1,   1, 2}, {  0,   0, 0, 130} },
            { {128, 1, 1,   0}, {0, 129,   1, 0}, {  0,   1,   1, 0}, {  2,   2, 2, 130} },
            { {128, 0, 0,   0}, {0,   0,   0, 0}, {  2,   1, 129, 2}, {  2,   1, 1, 130} },
            { {128, 1, 1,   0}, {0, 129,   1, 0}, {  2,   2,   2, 2}, {  2,   2, 2, 130} },
            { {128, 0, 2,   2}, {0,   0,   1, 1}, {  0,   0, 129, 1}, {  0,   0, 2, 130} },
            { {128, 0, 2,   2}, {1,   1,   2, 2}, {129,   1,   2, 2}, {  0,   0, 2, 130} },
            { {128, 0, 0,   0}, {0,   0,   0, 0}, {  0,   0,   0, 0}, {  2, 129, 1, 130} },
            { {128, 0, 0, 130}, {0,   0,   0, 1}, {  0,   0,   0, 2}, {  0,   0, 0, 129} },
            { {128, 2, 2,   2}, {1,   2,   2, 2}, {  0,   2,   2, 2}, {129,   2, 2, 130} },
            { {128, 1, 0, 129}, {2,   2,   2, 2}, {  2,   2,   2, 2}, {  2,   2, 2, 130} },
            { {128, 1, 1, 129}, {2,   0,   1, 1}, {130,   2,   0, 1}, {  2,   2, 2,   0} }
        }
    };

    static const int aWeight2[] = { 0, 21, 43, 64 };
    static const int aWeight3[] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const int aWeight4[] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

    static const uint8_t sModeHasPBits = 0b11001011;

    bcdec__bitstream_t bstream;
    int mode, partition, numPartitions, numEndpoints, i, j, k, rotation, partitionSet;
    int indexSelectionBit, indexBits, indexBits2, index, index2;
    int endpoints[6][4];
    char indices[4][4];
    int r, g, b, a;
    const int *weights, *weights2;
    uint8_t* decompressed = (uint8_t*)decompressedBlock;

    bstream.low  = ((const uint64_t*)compressedBlock)[0];
    bstream.high = ((const uint64_t*)compressedBlock)[1];

    for (mode = 0; mode < 8 && (0 == bcdec__bitstream_read_bit(&bstream)); ++mode);

    if (mode >= 8) {
        for (i = 0; i < 4; ++i) {
            for (j = 0; j < 4; ++j) {
                decompressed[j * 4 + 0] = 0;
                decompressed[j * 4 + 1] = 0;
                decompressed[j * 4 + 2] = 0;
                decompressed[j * 4 + 3] = 0;
            }
            decompressed += destinationPitch;
        }
        return;
    }

    partition = 0;
    numPartitions = 1;
    rotation = 0;
    indexSelectionBit = 0;

    if (mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 7) {
        numPartitions = (mode == 0 || mode == 2) ? 3 : 2;
        partition = bcdec__bitstream_read_bits(&bstream, (mode == 0) ? 4 : 6);
    }

    numEndpoints = numPartitions * 2;

    if (mode == 4 || mode == 5) {
        rotation = bcdec__bitstream_read_bits(&bstream, 2);
        if (mode == 4) {
            indexSelectionBit = bcdec__bitstream_read_bit(&bstream);
        }
    }

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < numEndpoints; ++j) {
            endpoints[j][i] = bcdec__bitstream_read_bits(&bstream, actual_bits_count[0][mode]);
        }
    }
    if (actual_bits_count[1][mode] > 0) {
        for (j = 0; j < numEndpoints; ++j) {
            endpoints[j][3] = bcdec__bitstream_read_bits(&bstream, actual_bits_count[1][mode]);
        }
    }

    if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
        for (i = 0; i < numEndpoints; ++i) {
            for (j = 0; j < 4; ++j) {
                endpoints[i][j] <<= 1;
            }
        }

        if (mode == 1) {
            i = bcdec__bitstream_read_bit(&bstream);
            j = bcdec__bitstream_read_bit(&bstream);
            for (k = 0; k < 3; ++k) {
                endpoints[0][k] |= i;
                endpoints[1][k] |= i;
                endpoints[2][k] |= j;
                endpoints[3][k] |= j;
            }
        } else if (sModeHasPBits & (1 << mode)) {
            for (i = 0; i < numEndpoints; ++i) {
                j = bcdec__bitstream_read_bit(&bstream);
                for (k = 0; k < 4; ++k) {
                    endpoints[i][k] |= j;
                }
            }
        }
    }

    for (i = 0; i < numEndpoints; ++i) {
        j = actual_bits_count[0][mode] + ((sModeHasPBits >> mode) & 1);
        for (k = 0; k < 3; ++k) {
            endpoints[i][k] = endpoints[i][k] << (8 - j);
            endpoints[i][k] = endpoints[i][k] | (endpoints[i][k] >> j);
        }

        j = actual_bits_count[1][mode] + ((sModeHasPBits >> mode) & 1);
        endpoints[i][3] = endpoints[i][3] << (8 - j);
        endpoints[i][3] = endpoints[i][3] | (endpoints[i][3] >> j);
    }

    if (!actual_bits_count[1][mode]) {
        for (j = 0; j < numEndpoints; ++j) {
            endpoints[j][3] = 0xFF;
        }
    }

    indexBits = (mode == 0 || mode == 1) ? 3 : ((mode == 6) ? 4 : 2);
    indexBits2 = (mode == 4) ? 3 : ((mode == 5) ? 2 : 0);
    weights = (indexBits == 2) ? aWeight2 : ((indexBits == 3) ? aWeight3 : aWeight4);
    weights2 = (indexBits2 == 2) ? aWeight2 : aWeight3;

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            partitionSet = (numPartitions == 1) ? ((i | j) ? 0 : 128) : partition_sets[numPartitions - 2][partition][i][j];
            indexBits = (mode == 0 || mode == 1) ? 3 : ((mode == 6) ? 4 : 2);

            if (partitionSet & 0x80) {
                indexBits--;
            }
            indices[i][j] = bcdec__bitstream_read_bits(&bstream, indexBits);
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
                    r = bcdec__interpolate(endpoints[partitionSet * 2][0], endpoints[partitionSet * 2 + 1][0],  weights,  index);
                    g = bcdec__interpolate(endpoints[partitionSet * 2][1], endpoints[partitionSet * 2 + 1][1],  weights,  index);
                    b = bcdec__interpolate(endpoints[partitionSet * 2][2], endpoints[partitionSet * 2 + 1][2],  weights,  index);
                    a = bcdec__interpolate(endpoints[partitionSet * 2][3], endpoints[partitionSet * 2 + 1][3], weights2, index2);
                } else {
                    r = bcdec__interpolate(endpoints[partitionSet * 2][0], endpoints[partitionSet * 2 + 1][0], weights2, index2);
                    g = bcdec__interpolate(endpoints[partitionSet * 2][1], endpoints[partitionSet * 2 + 1][1], weights2, index2);
                    b = bcdec__interpolate(endpoints[partitionSet * 2][2], endpoints[partitionSet * 2 + 1][2], weights2, index2);
                    a = bcdec__interpolate(endpoints[partitionSet * 2][3], endpoints[partitionSet * 2 + 1][3],  weights,  index);
                }
            }

            switch (rotation) {
                case 1: bcdec__swap_values(&a, &r); break;
                case 2: bcdec__swap_values(&a, &g); break;
                case 3: bcdec__swap_values(&a, &b); break;
            }

            decompressed[j * 4 + 0] = r;
            decompressed[j * 4 + 1] = g;
            decompressed[j * 4 + 2] = b;
            decompressed[j * 4 + 3] = a;
        }
        decompressed += destinationPitch;
    }
}

#endif /* BCDEC_IMPLEMENTATION */
