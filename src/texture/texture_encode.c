#include "texture_convert_internal.h"
#include "common/fs_utils.h"
#include "common/byteio.h"

const char* texExtension(CinnamonTextureFormat format) {
    if (format == CINNAMON_TEXFMT_CI4 || format == CINNAMON_TEXFMT_CI8 ||
        format == CINNAMON_TEXFMT_CMPR || format == CINNAMON_TEXFMT_RGB5A3)
        return "ctx";
    return "ctex";
}

const char* texFormatName(CinnamonTextureFormat format) {
    switch (format) {
    case CINNAMON_TEXFMT_ETC1A4:
        return "etc1a4";
    case CINNAMON_TEXFMT_L4:
        return "l4";
    case CINNAMON_TEXFMT_LA4:
        return "la4";
    case CINNAMON_TEXFMT_RGB565:
        return "rgb565";
    case CINNAMON_TEXFMT_RGBA4444:
        return "rgba4444";
    case CINNAMON_TEXFMT_CI4:
        return "ci4";
    case CINNAMON_TEXFMT_CI8:
        return "ci8";
    case CINNAMON_TEXFMT_CMPR:
        return "cmpr";
    case CINNAMON_TEXFMT_RGB5A3:
        return "rgb5a3";
    case CINNAMON_TEXFMT_ETC1:
        return "etc1";
    default:
        return "unknown";
    }
}

bool texParseFormat(const char* label, CinnamonTextureFormat* outFormat) {
    if (label == NULL || outFormat == NULL) return false;
    if (strcmp(label, "rgb565") == 0) {
        *outFormat = CINNAMON_TEXFMT_RGB565;
        return true;
    }
    if (strcmp(label, "rgba4444") == 0 || strcmp(label, "rgba444") == 0 ||
        strcmp(label, "rgba4") == 0) {
        *outFormat = CINNAMON_TEXFMT_RGBA4444;
        return true;
    }
    if (strcmp(label, "etc1a4") == 0) {
        *outFormat = CINNAMON_TEXFMT_ETC1A4;
        return true;
    }
    if (strcmp(label, "etc1") == 0) {
        *outFormat = CINNAMON_TEXFMT_ETC1;
        return true;
    }
    if (strcmp(label, "l4") == 0) {
        *outFormat = CINNAMON_TEXFMT_L4;
        return true;
    }
    if (strcmp(label, "la4") == 0) {
        *outFormat = CINNAMON_TEXFMT_LA4;
        return true;
    }
    if (strcmp(label, "ci4") == 0 || strcmp(label, "gx_ci4") == 0) {
        *outFormat = CINNAMON_TEXFMT_CI4;
        return true;
    }
    if (strcmp(label, "ci8") == 0 || strcmp(label, "gx_ci8") == 0) {
        *outFormat = CINNAMON_TEXFMT_CI8;
        return true;
    }
    if (strcmp(label, "cmpr") == 0 || strcmp(label, "gx_cmpr") == 0) {
        *outFormat = CINNAMON_TEXFMT_CMPR;
        return true;
    }
    if (strcmp(label, "rgb5a3") == 0 || strcmp(label, "gx_rgb5a3") == 0) {
        *outFormat = CINNAMON_TEXFMT_RGB5A3;
        return true;
    }
    return false;
}

static uint32_t nextPow2(uint32_t value) {
    uint32_t result = 8u;
    while (result < value && result < 1024u) result <<= 1u;
    return result;
}

static bool writeTextureParts(
    const char* path, const uint8_t* dataA, size_t sizeA, const uint8_t* dataB, size_t sizeB) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = (sizeA == 0u || fwrite(dataA, 1, sizeA, file) == sizeA) &&
        (sizeB == 0u || fwrite(dataB, 1, sizeB, file) == sizeB);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static CinnamonNativeTextureFormat getNativeFormat(CinnamonTextureFormat format) {
    switch (format) {
    case CINNAMON_TEXFMT_RGB565:
        return CINNAMON_NATIVE_TEXFMT_RGB565;
    case CINNAMON_TEXFMT_RGBA4444:
        return CINNAMON_NATIVE_TEXFMT_RGBA4444;
    case CINNAMON_TEXFMT_L4:
        return CINNAMON_NATIVE_TEXFMT_L4;
    case CINNAMON_TEXFMT_LA4:
        return CINNAMON_NATIVE_TEXFMT_LA4;
    case CINNAMON_TEXFMT_CI4:
        return CINNAMON_NATIVE_TEXFMT_CI4;
    case CINNAMON_TEXFMT_CI8:
        return CINNAMON_NATIVE_TEXFMT_CI8;
    case CINNAMON_TEXFMT_CMPR:
        return CINNAMON_NATIVE_TEXFMT_CMPR;
    case CINNAMON_TEXFMT_RGB5A3:
        return CINNAMON_NATIVE_TEXFMT_RGB5A3;
    case CINNAMON_TEXFMT_ETC1A4:
        return CINNAMON_NATIVE_TEXFMT_ETC1A4;
    case CINNAMON_TEXFMT_ETC1:
        return CINNAMON_NATIVE_TEXFMT_ETC1;
    default:
        return CINNAMON_NATIVE_TEXFMT_RGBA4444;
    }
}

static bool uses3dsLayout(CinnamonNativeTextureFormat format) {
    switch (format) {
    case CINNAMON_NATIVE_TEXFMT_RGB565:
    case CINNAMON_NATIVE_TEXFMT_RGBA4444:
    case CINNAMON_NATIVE_TEXFMT_L4:
    case CINNAMON_NATIVE_TEXFMT_LA4:
    case CINNAMON_NATIVE_TEXFMT_ETC1:
    case CINNAMON_NATIVE_TEXFMT_ETC1A4:
        return true;
    default:
        return false;
    }
}

static uint64_t nativeImageBytes(
    uint32_t width, uint32_t height, CinnamonNativeTextureFormat format) {
    uint32_t paddedWidth = (width + 7u) & ~7u;
    uint32_t paddedHeight = (height + 7u) & ~7u;
    if (uses3dsLayout(format)) {
        paddedWidth = nextPow2(width);
        paddedHeight = nextPow2(height);
    }
    uint64_t paddedPixels = (uint64_t) paddedWidth * (uint64_t) paddedHeight;
    switch (format) {
    case CINNAMON_NATIVE_TEXFMT_L4:
        return (paddedPixels + 1u) / 2u;
    case CINNAMON_NATIVE_TEXFMT_LA4:
        return paddedPixels;
    case CINNAMON_NATIVE_TEXFMT_CI4:
        return WII_CI4_TLUT_BYTES + paddedPixels / 2u;
    case CINNAMON_NATIVE_TEXFMT_CI8:
        return WII_CI8_TLUT_BYTES + (uint64_t) paddedWidth * (uint64_t) ((height + 3u) & ~3u);
    case CINNAMON_NATIVE_TEXFMT_CMPR:
        return (uint64_t) ((width + 7u) / 8u) * (uint64_t) ((height + 7u) / 8u) * 32u;
    case CINNAMON_NATIVE_TEXFMT_RGB5A3:
        return (uint64_t) ((width + 3u) & ~3u) * (uint64_t) ((height + 3u) & ~3u) * 2u;
    case CINNAMON_NATIVE_TEXFMT_ETC1:
    case CINNAMON_NATIVE_TEXFMT_ETC1A4: {
        uint32_t texWidth = nextPow2(width);
        uint32_t texHeight = nextPow2(height);
        size_t tileBytes = format == CINNAMON_NATIVE_TEXFMT_ETC1 ? 32u : 64u;
        return (uint64_t) ((texWidth + 7u) / 8u) * (uint64_t) ((texHeight + 7u) / 8u) * tileBytes;
    }
    default:
        return paddedPixels * 2u;
    }
}

typedef struct {
    TextureRgbaRect source;
    uint32_t tilesPerRow;
    CinnamonNativeTextureFormat format;
    uint8_t* out;
} NativeTextureEncodeContext;

static void encodeEtcNativeTexture(const NativeTextureEncodeContext* ctx) {
    uint32_t texWidth = nextPow2(ctx->source.width);
    uint32_t texHeight = nextPow2(ctx->source.height);
    uint32_t tilesPerRow = (texWidth + 7u) / 8u;
    uint32_t tileRows = (texHeight + 7u) / 8u;
    size_t tileBytes = ctx->format == CINNAMON_NATIVE_TEXFMT_ETC1 ? 32u : 64u;
    for (uint32_t tileY = 0u; tileY < tileRows; ++tileY) {
        for (uint32_t tileX = 0u; tileX < tilesPerRow; ++tileX) {
            uint8_t* tileOut = ctx->out + ((size_t) tileY * tilesPerRow + tileX) * tileBytes;
            for (uint32_t block = 0u; block < 4u; ++block) {
                uint32_t blockX = tileX * 8u + (block & 1u) * 4u;
                uint32_t blockY = tileY * 8u + (block >> 1u) * 4u;
                uint8_t encoded[16];
                texEncodeEtc1a4Block(ctx->source.rgba, ctx->source.width, ctx->source.height,
                    blockX, blockY, encoded);
                if (ctx->format == CINNAMON_NATIVE_TEXFMT_ETC1)
                    memcpy(tileOut + block * 8u, encoded + 8u, 8u);
                else
                    memcpy(tileOut + block * 16u, encoded, 16u);
            }
        }
    }
}

static void encodeCi4NativeTexture(const NativeTextureEncodeContext* ctx) {
    uint16_t palette[WII_CI4_PALETTE_COLORS];
    uint16_t* paletteLookup = safeMalloc(WII_RGB5A3_COLOR_COUNT * sizeof(uint16_t));
    uint32_t paletteCount =
        texBuildPalette(&ctx->source, palette, paletteLookup, WII_CI4_PALETTE_COLORS);
    texWriteCi4Palette(ctx->out, palette);
    uint8_t* indices = ctx->out + WII_CI4_TLUT_BYTES;
    for (uint32_t y = 0; y < ctx->source.height; ++y) {
        for (uint32_t x = 0; x < ctx->source.width; ++x) {
            uint32_t tileIndex = (y / 8u) * ctx->tilesPerRow + (x / 8u);
            uint32_t texelIndex = tileIndex * 32u + (y & 7u) * 4u + ((x & 7u) >> 1u);
            const uint8_t* px = texSampleRect(&ctx->source, x, y);
            uint8_t index =
                (uint8_t) (texPaletteIndex(px, palette, paletteCount, paletteLookup) & 0x0Fu);
            if ((x & 1u) == 0u)
                indices[texelIndex] = (uint8_t) ((indices[texelIndex] & 0x0Fu) | (index << 4u));
            else
                indices[texelIndex] = (uint8_t) ((indices[texelIndex] & 0xF0u) | index);
        }
    }
    free(paletteLookup);
}

static void encodeCi8NativeTexture(const NativeTextureEncodeContext* ctx) {
    uint16_t palette[WII_CI8_PALETTE_COLORS];
    uint16_t* paletteLookup = safeMalloc(WII_RGB5A3_COLOR_COUNT * sizeof(uint16_t));
    uint32_t paletteCount =
        texBuildPalette(&ctx->source, palette, paletteLookup, WII_CI8_PALETTE_COLORS);
    texWriteCi8Palette(ctx->out, palette);
    uint8_t* indices = ctx->out + WII_CI8_TLUT_BYTES;
    for (uint32_t y = 0; y < ctx->source.height; ++y) {
        for (uint32_t x = 0; x < ctx->source.width; ++x) {
            uint32_t tileIndex = (y / 4u) * ctx->tilesPerRow + (x / 8u);
            uint32_t texelIndex = tileIndex * 32u + (y & 3u) * 8u + (x & 7u);
            const uint8_t* px = texSampleRect(&ctx->source, x, y);
            indices[texelIndex] = texPaletteIndex(px, palette, paletteCount, paletteLookup);
        }
    }
    free(paletteLookup);
}

static void encode3dsPixels(const NativeTextureEncodeContext* ctx) {
    for (uint32_t y = 0; y < ctx->source.height; ++y) {
        for (uint32_t x = 0; x < ctx->source.width; ++x) {
            uint32_t tileIndex = (y / 8u) * ctx->tilesPerRow + (x / 8u);
            uint32_t texelIndex = tileIndex * 64u + texMorton8(x & 7u, y & 7u);
            const uint8_t* px = texSampleRect(&ctx->source, x, y);
            if (ctx->format == CINNAMON_NATIVE_TEXFMT_RGB565) {
                writeLe16(ctx->out + (size_t) texelIndex * 2u, texPackRGB565(px));
            } else if (ctx->format == CINNAMON_NATIVE_TEXFMT_RGBA4444) {
                writeLe16(ctx->out + (size_t) texelIndex * 2u, texPackRGBA4444(px));
            } else if (ctx->format == CINNAMON_NATIVE_TEXFMT_LA4) {
                ctx->out[texelIndex] = (uint8_t) ((texLuma4(px) << 4u) | (px[3] >> 4u));
            } else {
                uint32_t byteIndex = texelIndex / 2u;
                uint8_t luma = texLuma4(px);
                if ((texelIndex & 1u) == 0)
                    ctx->out[byteIndex] = (uint8_t) ((ctx->out[byteIndex] & 0xF0u) | luma);
                else
                    ctx->out[byteIndex] = (uint8_t) ((ctx->out[byteIndex] & 0x0Fu) | (luma << 4u));
            }
        }
    }
}

static void encodeNativePixels(const NativeTextureEncodeContext* ctx) {
    switch (ctx->format) {
    case CINNAMON_NATIVE_TEXFMT_ETC1:
    case CINNAMON_NATIVE_TEXFMT_ETC1A4:
        encodeEtcNativeTexture(ctx);
        break;
    case CINNAMON_NATIVE_TEXFMT_CMPR:
        texEncodeCmpr(&ctx->source, ctx->out);
        break;
    case CINNAMON_NATIVE_TEXFMT_RGB5A3:
        texEncodeRgb5a3(&ctx->source, ctx->out);
        break;
    case CINNAMON_NATIVE_TEXFMT_CI4:
        encodeCi4NativeTexture(ctx);
        break;
    case CINNAMON_NATIVE_TEXFMT_CI8:
        encodeCi8NativeTexture(ctx);
        break;
    default:
        encode3dsPixels(ctx);
        break;
    }
}

static bool writeNativeTextureBlob(const char* outputPath, uint8_t* blob, size_t dataOffset,
    size_t totalSize, size_t rawDataSize, bool compress) {
    uint8_t* compressed = NULL;
    size_t compressedSize = 0u;
    if (compress) {
        compressed =
            lz11CompressBounded(blob + dataOffset, rawDataSize, rawDataSize, &compressedSize);
    }
    if (compressed == NULL) {
        writeLe32(blob + 24, CINNAMON_NATIVE_TEXTURE_COMPRESSION_NONE);
        writeLe32(blob + 28, (uint32_t) rawDataSize);
        bool ok = fsWriteFile(outputPath, blob, (uint32_t) totalSize);
        if (!ok) fprintf(stderr, "native texture writer failed to write %s\n", outputPath);
        return ok;
    }
    if (compressedSize > UINT32_MAX) {
        fprintf(stderr, "native texture writer failed to lz11-compress %s\n", outputPath);
        free(compressed);
        return false;
    }

    writeLe32(blob + 20, (uint32_t) compressedSize);
    writeLe32(blob + 24, CINNAMON_NATIVE_TEXTURE_COMPRESSION_LZ11);
    writeLe32(blob + 28, (uint32_t) rawDataSize);
    bool ok = writeTextureParts(outputPath, blob, dataOffset, compressed, compressedSize);
    free(compressed);
    if (!ok) fprintf(stderr, "native texture writer failed to write %s\n", outputPath);
    return ok;
}

static bool writeNativeRegions(const TextureRgbaRect* source, const char* outputPath,
    CinnamonTextureFormat format, const CinnamonNativeTextureRegion* regions, uint32_t regionCount,
    bool compress) {
    if (source->rgba == NULL || outputPath == NULL || outputPath[0] == '\0' ||
        source->sourceWidth == 0 || source->sourceHeight == 0 || source->width == 0 ||
        source->height == 0)
        return false;
    if (source->width > UINT16_MAX || source->height > UINT16_MAX || regionCount > UINT16_MAX ||
        (regionCount > 0u && regions == NULL))
        return false;

    CinnamonNativeTextureFormat nativeFormat = getNativeFormat(format);
    bool uses3DSLayout = uses3dsLayout(nativeFormat);
    if (uses3DSLayout &&
        (source->width > CINNAMON_LARGE_TEXTURE_SIZE ||
            source->height > CINNAMON_LARGE_TEXTURE_SIZE))
        return false;
    if ((nativeFormat == CINNAMON_NATIVE_TEXFMT_ETC1 ||
            nativeFormat == CINNAMON_NATIVE_TEXFMT_ETC1A4) &&
        (source->sourceX != 0u || source->sourceY != 0u || source->sourceWidth != source->width ||
            source->sourceHeight != source->height))
        return false;

    uint64_t rawDataBytes = nativeImageBytes(source->width, source->height, nativeFormat);
    if (rawDataBytes > UINT32_MAX || rawDataBytes > SIZE_MAX) return false;
    size_t rawDataSize = (size_t) rawDataBytes;
    size_t regionTableSize = regionCount > 0u ? 8u + (size_t) regionCount * 8u : 0u;
    size_t dataOffset = CINNAMON_NATIVE_TEXTURE_HEADER_SIZE + regionTableSize;
    if (dataOffset > UINT32_MAX - rawDataSize || dataOffset > SIZE_MAX - rawDataSize) return false;
    size_t totalSize = dataOffset + rawDataSize;
    uint8_t* blob = safeCalloc(1, totalSize);
    writeLe32(blob + 0, CINNAMON_NATIVE_TEXTURE_MAGIC);
    writeLe16(blob + 4, CINNAMON_NATIVE_TEXTURE_VERSION);
    writeLe16(blob + 6, CINNAMON_NATIVE_TEXTURE_HEADER_SIZE);
    writeLe16(blob + 8, (uint16_t) source->width);
    writeLe16(blob + 10, (uint16_t) source->height);
    writeLe32(blob + 12, (uint32_t) nativeFormat);
    writeLe32(blob + 16, (uint32_t) dataOffset);
    writeLe32(blob + 20, (uint32_t) rawDataSize);

    if (regionCount > 0u) {
        uint8_t* table = blob + CINNAMON_NATIVE_TEXTURE_HEADER_SIZE;
        writeLe32(table + 0u, CINNAMON_NATIVE_TEXTURE_REGION_MAGIC);
        writeLe16(table + 4u, CINNAMON_NATIVE_TEXTURE_REGION_VERSION);
        writeLe16(table + 6u, (uint16_t) regionCount);
        preprocessRepeat(regionCount, i) {
            uint8_t* row = table + 8u + i * 8u;
            writeLe16(row + 0u, regions[i].x);
            writeLe16(row + 2u, regions[i].y);
            writeLe16(row + 4u, regions[i].width);
            writeLe16(row + 6u, regions[i].height);
        }
    }

    uint32_t storageWidth = uses3DSLayout ? nextPow2(source->width) : source->width;
    if (nativeFormat == CINNAMON_NATIVE_TEXFMT_ETC1 ||
        nativeFormat == CINNAMON_NATIVE_TEXFMT_ETC1A4) {
        fprintf(stderr, "native texture writer: rgba8888 -> %s %lux%lu -> %s\n",
            nativeFormat == CINNAMON_NATIVE_TEXFMT_ETC1 ? "etc1" : "etc1a4",
            (unsigned long) source->width, (unsigned long) source->height, outputPath);
    }
    NativeTextureEncodeContext encodeCtx = {
        .source = *source,
        .tilesPerRow = (storageWidth + 7u) / 8u,
        .format = nativeFormat,
        .out = blob + dataOffset,
    };
    encodeNativePixels(&encodeCtx);
    bool ok =
        writeNativeTextureBlob(outputPath, blob, dataOffset, totalSize, rawDataSize, compress);
    free(blob);
    return ok;
}

bool texWriteRegions(const uint8_t* rgba, uint32_t width, uint32_t height, const char* outputPath,
    CinnamonTextureFormat format, const CinnamonNativeTextureRegion* regions,
    uint32_t regionCount) {
    return writeNativeRegions(&(TextureRgbaRect){.rgba = rgba,
                                  .sourceWidth = width,
                                  .sourceHeight = height,
                                  .sourceX = 0u,
                                  .sourceY = 0u,
                                  .width = width,
                                  .height = height},
        outputPath, format, regions, regionCount, true);
}

bool texWriteRect(const TextureRgbaRect* source, const char* outputPath,
    CinnamonTextureFormat format, bool compress) {
    return writeNativeRegions(source, outputPath, format, NULL, 0u, compress);
}

bool texWriteRgba(const uint8_t* rgba, uint32_t width, uint32_t height, const char* outputPath,
    CinnamonTextureFormat format) {
    return texWriteRegions(rgba, width, height, outputPath, format, NULL, 0u);
}
