#include "texture_convert_internal.h"

typedef struct {
    uint16_t color;
    uint32_t count;
} NativeTexturePaletteCandidate;

static void writeBe16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t) (value >> 8u);
    dst[1] = (uint8_t) (value & 0xFFu);
}

static uint16_t packRgb5a3(const uint8_t* px) {
    if (px == NULL || px[3] < 128u) return 0u;
    if (px[3] < 224u) {
        uint16_t a = (uint16_t) ((px[3] * 7u + 127u) / 255u);
        uint16_t r = (uint16_t) ((px[0] * 15u + 127u) / 255u);
        uint16_t g = (uint16_t) ((px[1] * 15u + 127u) / 255u);
        uint16_t b = (uint16_t) ((px[2] * 15u + 127u) / 255u);
        return (uint16_t) ((a << 12u) | (r << 8u) | (g << 4u) | b);
    }

    uint16_t r = (uint16_t) ((px[0] * 31u + 127u) / 255u);
    uint16_t g = (uint16_t) ((px[1] * 31u + 127u) / 255u);
    uint16_t b = (uint16_t) ((px[2] * 31u + 127u) / 255u);
    return (uint16_t) (0x8000u | (r << 10u) | (g << 5u) | b);
}

static void decodeRgb5a3(uint16_t color, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
    if ((color & 0x8000u) != 0u) {
        uint8_t r5 = (uint8_t) ((color >> 10u) & 31u);
        uint8_t g5 = (uint8_t) ((color >> 5u) & 31u);
        uint8_t b5 = (uint8_t) (color & 31u);
        *r = (uint8_t) ((r5 << 3u) | (r5 >> 2u));
        *g = (uint8_t) ((g5 << 3u) | (g5 >> 2u));
        *b = (uint8_t) ((b5 << 3u) | (b5 >> 2u));
        *a = 255u;
    } else {
        uint8_t a3 = (uint8_t) ((color >> 12u) & 7u);
        uint8_t r4 = (uint8_t) ((color >> 8u) & 15u);
        uint8_t g4 = (uint8_t) ((color >> 4u) & 15u);
        uint8_t b4 = (uint8_t) (color & 15u);
        *r = (uint8_t) ((r4 << 4u) | r4);
        *g = (uint8_t) ((g4 << 4u) | g4);
        *b = (uint8_t) ((b4 << 4u) | b4);
        *a = (uint8_t) ((a3 * 255u + 3u) / 7u);
    }
}

static int comparePaletteCandidates(const void* a, const void* b) {
    const NativeTexturePaletteCandidate* ca = (const NativeTexturePaletteCandidate*) a;
    const NativeTexturePaletteCandidate* cb = (const NativeTexturePaletteCandidate*) b;
    if (ca->count != cb->count) return ca->count < cb->count ? 1 : -1;
    if (ca->color != cb->color) return ca->color > cb->color ? 1 : -1;
    return 0;
}

static uint32_t rgb5a3Distance(uint16_t left, uint16_t right) {
    uint8_t lr, lg, lb, la;
    uint8_t rr, rg, rb, ra;
    decodeRgb5a3(left, &lr, &lg, &lb, &la);
    decodeRgb5a3(right, &rr, &rg, &rb, &ra);
    int dr = (int) lr - (int) rr;
    int dg = (int) lg - (int) rg;
    int db = (int) lb - (int) rb;
    int da = (int) la - (int) ra;
    return (uint32_t) (dr * dr * 3 + dg * dg * 4 + db * db * 2 + da * da);
}

static uint8_t nearestCi8Color(uint16_t color, const uint16_t* palette, uint32_t paletteCount) {
    if (color == 0u || palette == NULL || paletteCount <= 1u) return 0u;
    uint8_t bestIndex = 1u;
    uint32_t bestDistance = UINT32_MAX;
    for (uint32_t i = 1u; i < paletteCount; ++i) {
        if (palette[i] == color) return (uint8_t) i;
        uint32_t distance = rgb5a3Distance(color, palette[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = (uint8_t) i;
            if (distance == 0u) break;
        }
    }
    return bestIndex;
}

uint32_t texBuildPalette(const TextureRgbaRect* source, uint16_t* palette, uint16_t* paletteLookup,
    uint32_t paletteCapacity) {
    if (palette == NULL || paletteLookup == NULL || paletteCapacity == 0u) return 0u;
    memset(palette, 0, (size_t) paletteCapacity * sizeof(uint16_t));
    for (uint32_t i = 0u; i < WII_RGB5A3_COLOR_COUNT; ++i) paletteLookup[i] = UINT16_MAX;

    uint32_t* counts = safeCalloc(WII_RGB5A3_COLOR_COUNT, sizeof(uint32_t));
    NativeTexturePaletteCandidate* candidates =
        safeMalloc((WII_RGB5A3_COLOR_COUNT - 1u) * sizeof(*candidates));
    uint32_t candidateCount = 0u;
    for (uint32_t y = 0u; y < source->height; ++y) {
        for (uint32_t x = 0u; x < source->width; ++x) {
            uint16_t color = packRgb5a3(texSampleRect(source, x, y));
            if (color != 0u) counts[color]++;
        }
    }

    for (uint32_t color = 1u; color < WII_RGB5A3_COLOR_COUNT; ++color) {
        if (counts[color] == 0u) continue;
        NativeTexturePaletteCandidate candidate = {
            .color = (uint16_t) color,
            .count = counts[color],
        };
        candidates[candidateCount++] = candidate;
    }
    free(counts);

    qsort(candidates, candidateCount, sizeof(candidates[0]), comparePaletteCandidates);
    palette[0] = 0u;
    paletteLookup[0] = 0u;
    uint32_t paletteCount = 1u;
    preprocessRepeat(candidateCount, i) {
        if (paletteCount >= paletteCapacity) break;
        uint16_t color = candidates[i].color;
        palette[paletteCount] = color;
        paletteLookup[color] = (uint16_t) paletteCount;
        paletteCount++;
    }
    free(candidates);
    return paletteCount;
}

uint8_t texPaletteIndex(
    const uint8_t* px, const uint16_t* palette, uint32_t paletteCount, uint16_t* paletteLookup) {
    uint16_t color = packRgb5a3(px);
    if (color == 0u) return 0u;
    if (paletteLookup[color] != UINT16_MAX) return (uint8_t) paletteLookup[color];
    uint8_t index = nearestCi8Color(color, palette, paletteCount);
    paletteLookup[color] = index;
    return index;
}

void texWriteCi4Palette(uint8_t* out, const uint16_t* palette) {
    if (out == NULL || palette == NULL) return;
    for (uint32_t i = 0u; i < WII_CI4_PALETTE_COLORS; ++i) {
        writeBe16(out + i * 2u, palette[i]);
    }
}

void texWriteCi8Palette(uint8_t* out, const uint16_t* palette) {
    if (out == NULL || palette == NULL) return;
    for (uint32_t i = 0u; i < WII_CI8_PALETTE_COLORS; ++i) {
        writeBe16(out + i * 2u, palette[i]);
    }
}

bool texCi4PaletteFits(const TextureRgbaRect* source) {
    if (source->rgba == NULL || source->sourceWidth == 0u || source->sourceHeight == 0u ||
        source->width == 0u || source->height == 0u)
        return true;
    bool* seen = safeCalloc(WII_RGB5A3_COLOR_COUNT, sizeof(bool));
    uint32_t nonTransparentColors = 0u;

    for (uint32_t y = 0u; y < source->height; ++y) {
        for (uint32_t x = 0u; x < source->width; ++x) {
            uint16_t color = packRgb5a3(texSampleRect(source, x, y));
            if (color == 0u || seen[color]) continue;
            seen[color] = true;
            nonTransparentColors++;
            if (nonTransparentColors >= WII_CI4_PALETTE_COLORS) {
                free(seen);
                return false;
            }
        }
    }

    free(seen);
    return true;
}

bool texCi8PaletteFits(const TextureRgbaRect* source) {
    if (source->rgba == NULL || source->sourceWidth == 0u || source->sourceHeight == 0u ||
        source->width == 0u || source->height == 0u)
        return true;
    bool* seen = safeCalloc(WII_RGB5A3_COLOR_COUNT, sizeof(bool));
    uint32_t nonTransparentColors = 0u;

    for (uint32_t y = 0u; y < source->height; ++y) {
        for (uint32_t x = 0u; x < source->width; ++x) {
            uint16_t color = packRgb5a3(texSampleRect(source, x, y));
            if (color == 0u || seen[color]) continue;
            seen[color] = true;
            nonTransparentColors++;
            if (nonTransparentColors >= WII_CI8_PALETTE_COLORS) {
                free(seen);
                return false;
            }
        }
    }

    free(seen);
    return true;
}

static void decodeRgb565(uint16_t color, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t r5 = (uint8_t) ((color >> 11u) & 31u);
    uint8_t g6 = (uint8_t) ((color >> 5u) & 63u);
    uint8_t b5 = (uint8_t) (color & 31u);
    *r = (uint8_t) ((r5 << 3u) | (r5 >> 2u));
    *g = (uint8_t) ((g6 << 2u) | (g6 >> 4u));
    *b = (uint8_t) ((b5 << 3u) | (b5 >> 2u));
}

static uint32_t rgbDistance(
    uint8_t ar, uint8_t ag, uint8_t ab, uint8_t br, uint8_t bg, uint8_t bb) {
    int dr = (int) ar - (int) br;
    int dg = (int) ag - (int) bg;
    int db = (int) ab - (int) bb;
    return (uint32_t) (dr * dr * 3 + dg * dg * 4 + db * db * 2);
}

static void buildCmprPalette(uint16_t color0, uint16_t color1, uint8_t palette[4][4]) {
    decodeRgb565(color0, &palette[0][0], &palette[0][1], &palette[0][2]);
    decodeRgb565(color1, &palette[1][0], &palette[1][1], &palette[1][2]);
    palette[0][3] = 255u;
    palette[1][3] = 255u;
    if (color0 > color1) {
        palette[2][0] = (uint8_t) ((2u * palette[0][0] + palette[1][0] + 1u) / 3u);
        palette[2][1] = (uint8_t) ((2u * palette[0][1] + palette[1][1] + 1u) / 3u);
        palette[2][2] = (uint8_t) ((2u * palette[0][2] + palette[1][2] + 1u) / 3u);
        palette[2][3] = 255u;
        palette[3][0] = (uint8_t) ((palette[0][0] + 2u * palette[1][0] + 1u) / 3u);
        palette[3][1] = (uint8_t) ((palette[0][1] + 2u * palette[1][1] + 1u) / 3u);
        palette[3][2] = (uint8_t) ((palette[0][2] + 2u * palette[1][2] + 1u) / 3u);
        palette[3][3] = 255u;
    } else {
        palette[2][0] = (uint8_t) (((uint32_t) palette[0][0] + palette[1][0] + 1u) / 2u);
        palette[2][1] = (uint8_t) (((uint32_t) palette[0][1] + palette[1][1] + 1u) / 2u);
        palette[2][2] = (uint8_t) (((uint32_t) palette[0][2] + palette[1][2] + 1u) / 2u);
        palette[2][3] = 255u;
        palette[3][0] = 0u;
        palette[3][1] = 0u;
        palette[3][2] = 0u;
        palette[3][3] = 0u;
    }
}

static void encodeCmprBlock(
    const TextureRgbaRect* source, uint32_t blockX, uint32_t blockY, uint8_t* out) {
    uint8_t transparent[4] = {0u, 0u, 0u, 0u};
    const uint8_t* pixels[WII_CMPR_BLOCK_DIM * WII_CMPR_BLOCK_DIM];
    uint32_t opaqueIndices[WII_CMPR_BLOCK_DIM * WII_CMPR_BLOCK_DIM];
    uint32_t opaqueCount = 0u;
    bool hasTransparent = false;

    for (uint32_t y = 0u; y < WII_CMPR_BLOCK_DIM; ++y) {
        for (uint32_t x = 0u; x < WII_CMPR_BLOCK_DIM; ++x) {
            uint32_t index = y * WII_CMPR_BLOCK_DIM + x;
            const uint8_t* px = blockX + x < source->width && blockY + y < source->height
                ? texSampleRect(source, blockX + x, blockY + y)
                : transparent;
            pixels[index] = px;
            if (px[3] < 128u) {
                hasTransparent = true;
            } else {
                opaqueIndices[opaqueCount++] = index;
            }
        }
    }

    if (opaqueCount == 0u) {
        writeBe16(out + 0u, 0u);
        writeBe16(out + 2u, 0u);
        out[4] = 0xFFu;
        out[5] = 0xFFu;
        out[6] = 0xFFu;
        out[7] = 0xFFu;
        return;
    }

    uint32_t endpointA = opaqueIndices[0];
    uint32_t endpointB = opaqueIndices[0];
    uint32_t bestDistance = 0u;
    for (uint32_t i = 0u; i < opaqueCount; ++i) {
        const uint8_t* a = pixels[opaqueIndices[i]];
        for (uint32_t j = i + 1u; j < opaqueCount; ++j) {
            const uint8_t* b = pixels[opaqueIndices[j]];
            uint32_t distance = rgbDistance(a[0], a[1], a[2], b[0], b[1], b[2]);
            if (distance > bestDistance) {
                bestDistance = distance;
                endpointA = opaqueIndices[i];
                endpointB = opaqueIndices[j];
            }
        }
    }

    uint16_t color0 = texPackRGB565(pixels[endpointA]);
    uint16_t color1 = texPackRGB565(pixels[endpointB]);
    if ((!hasTransparent && color0 < color1) || (hasTransparent && color0 > color1)) {
        uint16_t tmp = color0;
        color0 = color1;
        color1 = tmp;
    }
    writeBe16(out + 0u, color0);
    writeBe16(out + 2u, color1);

    uint8_t palette[4][4];
    buildCmprPalette(color0, color1, palette);
    uint32_t candidateCount = color0 > color1 ? 4u : 3u;
    for (uint32_t y = 0u; y < WII_CMPR_BLOCK_DIM; ++y) {
        uint8_t row = 0u;
        for (uint32_t x = 0u; x < WII_CMPR_BLOCK_DIM; ++x) {
            uint32_t index = y * WII_CMPR_BLOCK_DIM + x;
            uint8_t selector = 0u;
            const uint8_t* px = pixels[index];
            if (px[3] < 128u && color0 <= color1) {
                selector = 3u;
            } else {
                uint32_t best = UINT32_MAX;
                for (uint32_t i = 0u; i < candidateCount; ++i) {
                    uint32_t distance = rgbDistance(
                        px[0], px[1], px[2], palette[i][0], palette[i][1], palette[i][2]);
                    if (distance < best) {
                        best = distance;
                        selector = (uint8_t) i;
                    }
                }
            }
            row |= (uint8_t) (selector << (6u - x * 2u));
        }
        out[WII_CMPR_BLOCK_DIM + y] = row;
    }
}

void texEncodeCmpr(const TextureRgbaRect* source, uint8_t* out) {
    uint32_t tilesPerRow = (source->width + WII_CMPR_TILE_DIM - 1u) / WII_CMPR_TILE_DIM;
    uint32_t tileRows = (source->height + WII_CMPR_TILE_DIM - 1u) / WII_CMPR_TILE_DIM;
    for (uint32_t tileY = 0u; tileY < tileRows; ++tileY) {
        for (uint32_t tileX = 0u; tileX < tilesPerRow; ++tileX) {
            uint8_t* tileOut = out + ((size_t) tileY * tilesPerRow + tileX) * WII_CMPR_TILE_BYTES;
            for (uint32_t block = 0u; block < 4u; ++block) {
                uint32_t blockX = tileX * WII_CMPR_TILE_DIM + (block & 1u) * WII_CMPR_BLOCK_DIM;
                uint32_t blockY = tileY * WII_CMPR_TILE_DIM + (block >> 1u) * WII_CMPR_BLOCK_DIM;
                encodeCmprBlock(source, blockX, blockY, tileOut + block * WII_CMPR_BLOCK_BYTES);
            }
        }
    }
}

void texEncodeRgb5a3(const TextureRgbaRect* source, uint8_t* out) {
    uint32_t paddedWidth = (source->width + WII_RGB5A3_TILE_DIM - 1u) & ~(WII_RGB5A3_TILE_DIM - 1u);
    uint32_t paddedHeight =
        (source->height + WII_RGB5A3_TILE_DIM - 1u) & ~(WII_RGB5A3_TILE_DIM - 1u);
    uint32_t tilesPerRow = paddedWidth / WII_RGB5A3_TILE_DIM;
    uint8_t transparent[4] = {0u, 0u, 0u, 0u};

    for (uint32_t y = 0u; y < paddedHeight; ++y) {
        for (uint32_t x = 0u; x < paddedWidth; ++x) {
            uint32_t tileIndex =
                (y / WII_RGB5A3_TILE_DIM) * tilesPerRow + (x / WII_RGB5A3_TILE_DIM);
            uint32_t texelIndex = tileIndex * WII_RGB5A3_TILE_DIM * WII_RGB5A3_TILE_DIM +
                (y & (WII_RGB5A3_TILE_DIM - 1u)) * WII_RGB5A3_TILE_DIM +
                (x & (WII_RGB5A3_TILE_DIM - 1u));
            const uint8_t* px =
                x < source->width && y < source->height ? texSampleRect(source, x, y) : transparent;
            writeBe16(out + (size_t) texelIndex * 2u, packRgb5a3(px));
        }
    }
}
