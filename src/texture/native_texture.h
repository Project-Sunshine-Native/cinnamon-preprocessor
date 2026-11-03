#ifndef CINNAMON_PREPROCESSOR_NATIVE_TEXTURE_H
#define CINNAMON_PREPROCESSOR_NATIVE_TEXTURE_H

#include <stdint.h>

#define CINNAMON_NATIVE_TEXTURE_MAGIC 0x31545843u /* CTX1 */
#define CINNAMON_NATIVE_TEXTURE_VERSION 1u
#define CINNAMON_NATIVE_TEXTURE_HEADER_SIZE 32u
#define CINNAMON_NATIVE_TEXTURE_COMPRESSION_NONE 0u
#define CINNAMON_NATIVE_TEXTURE_COMPRESSION_LZ11 1u
#define CINNAMON_NATIVE_TEXTURE_REGION_MAGIC 0x31525443u /* CTR1 */
#define CINNAMON_NATIVE_TEXTURE_REGION_VERSION 2u

typedef enum {
    CINNAMON_NATIVE_TEXFMT_RGB565 = 1,
    CINNAMON_NATIVE_TEXFMT_RGBA4444 = 2,
    CINNAMON_NATIVE_TEXFMT_RGB5A3 = 3,
    CINNAMON_NATIVE_TEXFMT_L4 = 4,
    CINNAMON_NATIVE_TEXFMT_LA4 = 5,
    CINNAMON_NATIVE_TEXFMT_CI4 = 7,
    CINNAMON_NATIVE_TEXFMT_CI8 = 8,
    CINNAMON_NATIVE_TEXFMT_ETC1 = 12,
    CINNAMON_NATIVE_TEXFMT_ETC1A4 = 13,
    CINNAMON_NATIVE_TEXFMT_CMPR = 14,
} CinnamonNativeTextureFormat;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t headerSize;
    uint16_t width;
    uint16_t height;
    uint32_t textureFormat;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t reserved0;
    uint32_t reserved1;
} CinnamonNativeTextureHeader;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} CinnamonNativeTextureRegion;

#endif
