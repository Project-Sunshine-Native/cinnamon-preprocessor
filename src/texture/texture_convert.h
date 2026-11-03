#ifndef CINNAMON_PREPROCESSOR_TEXTURE_CONVERT_H
#define CINNAMON_PREPROCESSOR_TEXTURE_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include "data_win.h"
#include "native_texture.h"

typedef struct PreprocessRunConfig PreprocessRunConfig;

typedef enum {
    CINNAMON_TEXFMT_ETC1A4 = 1,
    CINNAMON_TEXFMT_L4 = 3,
    CINNAMON_TEXFMT_LA4 = 4,
    CINNAMON_TEXFMT_RGB565 = 5,
    CINNAMON_TEXFMT_RGBA4444 = 6,
    CINNAMON_TEXFMT_CI8 = 7,
    CINNAMON_TEXFMT_CMPR = 8,
    CINNAMON_TEXFMT_CI4 = 9,
    CINNAMON_TEXFMT_RGB5A3 = 10,
    CINNAMON_TEXFMT_ETC1 = 11,
} CinnamonTextureFormat;

typedef struct {
    const uint8_t* rgba;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t sourceX;
    uint32_t sourceY;
    uint32_t width;
    uint32_t height;
} TextureRgbaRect;

const char* texExtension(CinnamonTextureFormat format);
const char* texFormatName(CinnamonTextureFormat format);
bool texParseFormat(const char* label, CinnamonTextureFormat* outFormat);
void texBlit(uint8_t* dst, uint32_t dstStride, uint32_t dstX, uint32_t dstY, const uint8_t* src,
    uint32_t srcStride, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height);
bool texBlitClipped(uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight, int32_t dstX, int32_t dstY,
    const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, int32_t srcX, int32_t srcY,
    int32_t width, int32_t height);
bool texRectIsMono(const TextureRgbaRect* source);
bool texWriteRgba(const uint8_t* rgba, uint32_t width, uint32_t height, const char* outputPath,
    CinnamonTextureFormat format);
bool texWriteRegions(const uint8_t* rgba, uint32_t width, uint32_t height, const char* outputPath,
    CinnamonTextureFormat format, const CinnamonNativeTextureRegion* regions, uint32_t regionCount);
bool texWriteRect(const TextureRgbaRect* source, const char* outputPath,
    CinnamonTextureFormat format, bool compress);
bool texConvert(const PreprocessRunConfig* options, DataWin* dataWin);

#endif
