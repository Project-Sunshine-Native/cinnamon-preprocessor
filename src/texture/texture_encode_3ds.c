#include "texture_convert_internal.h"
#include "common/byteio.h"

typedef struct {
    uint8_t r4;
    uint8_t g4;
    uint8_t b4;
    uint8_t table;
    uint32_t error;
} NativeTextureETC1A4Subblock;

typedef struct {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t blockX;
    uint32_t blockY;
    bool flip;
    uint32_t index;
} EtcSubblockInput;

static const int modifierTables[8][4] = {
    {2, 8, -2, -8},
    {5, 17, -5, -17},
    {9, 29, -9, -29},
    {13, 42, -13, -42},
    {18, 60, -18, -60},
    {24, 80, -24, -80},
    {33, 106, -33, -106},
    {47, 183, -47, -183},
};

uint32_t texMorton8(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) |
        ((y & 4u) << 3);
}

uint16_t texPackRGB565(const uint8_t* px) {
    uint16_t r = (uint16_t) (px[0] >> 3);
    uint16_t g = (uint16_t) (px[1] >> 2);
    uint16_t b = (uint16_t) (px[2] >> 3);
    return (uint16_t) ((r << 11) | (g << 5) | b);
}

uint16_t texPackRGBA4444(const uint8_t* px) {
    uint16_t r = (uint16_t) (px[0] >> 4);
    uint16_t g = (uint16_t) (px[1] >> 4);
    uint16_t b = (uint16_t) (px[2] >> 4);
    uint16_t a = (uint16_t) (px[3] >> 4);
    return (uint16_t) ((r << 12) | (g << 8) | (b << 4) | a);
}

uint8_t texLuma4(const uint8_t* px) {
    uint32_t luma = 77u * px[0] + 150u * px[1] + 29u * px[2];
    uint32_t value = (luma + 2048u) >> 12;
    return (uint8_t) (value > 15u ? 15u : value);
}

static uint8_t clampU8(int value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t) value;
}

static uint8_t expand4(uint8_t value) {
    value &= 0x0Fu;
    return (uint8_t) ((value << 4u) | value);
}

static uint32_t etc1a4PixelPos(uint32_t x, uint32_t y) {
    return x * 4u + y;
}

static uint32_t etc1a4ColorError(const uint8_t* px, int baseR, int baseG, int baseB, int modifier) {
    int r = (int) clampU8(baseR + modifier);
    int g = (int) clampU8(baseG + modifier);
    int b = (int) clampU8(baseB + modifier);
    int dr = (int) px[0] - r;
    int dg = (int) px[1] - g;
    int db = (int) px[2] - b;
    return (uint32_t) (dr * dr + dg * dg + db * db);
}

static bool etc1a4PixelInSubblock(uint32_t x, uint32_t y, bool flip, uint32_t subblock) {
    return flip ? ((y >= 2u) == (subblock != 0u)) : ((x >= 2u) == (subblock != 0u));
}

static uint8_t clampNativeNibble(int value) {
    if (value < 0) return 0u;
    if (value > 15) return 15u;
    return (uint8_t) value;
}

static uint32_t evaluateEtc1a4Subblock(
    const EtcSubblockInput* input, const NativeTextureETC1A4Subblock* candidate) {
    uint8_t baseR = expand4(candidate->r4);
    uint8_t baseG = expand4(candidate->g4);
    uint8_t baseB = expand4(candidate->b4);
    uint32_t totalError = 0u;
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            if (!etc1a4PixelInSubblock(x, y, input->flip, input->index)) continue;
            uint32_t pxX = input->blockX + x;
            uint32_t pxY = input->blockY + y;
            if (pxX >= input->width || pxY >= input->height) continue;
            const uint8_t* px = input->rgba + ((size_t) pxY * input->width + pxX) * 4u;
            if (px[3] == 0u) continue;
            uint32_t bestPixelError = UINT32_MAX;
            for (uint32_t selector = 0u; selector < 4u; ++selector) {
                uint32_t error = etc1a4ColorError(
                    px, baseR, baseG, baseB, modifierTables[candidate->table][selector]);
                if (error < bestPixelError) bestPixelError = error;
            }
            totalError += bestPixelError;
        }
    }
    return totalError;
}

static NativeTextureETC1A4Subblock chooseEtc1a4Subblock(const EtcSubblockInput* input) {
    uint32_t sumR = 0u;
    uint32_t sumG = 0u;
    uint32_t sumB = 0u;
    uint32_t visibleCount = 0u;
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            if (!etc1a4PixelInSubblock(x, y, input->flip, input->index)) continue;
            uint32_t pxX = input->blockX + x;
            uint32_t pxY = input->blockY + y;
            if (pxX >= input->width || pxY >= input->height) continue;
            const uint8_t* px = input->rgba + ((size_t) pxY * input->width + pxX) * 4u;
            if (px[3] == 0u) continue;
            sumR += px[0];
            sumG += px[1];
            sumB += px[2];
            visibleCount++;
        }
    }

    NativeTextureETC1A4Subblock best = {0};
    best.error = 0u;
    if (visibleCount == 0u) return best;

    uint8_t avgR4 = (uint8_t) ((sumR * 15u + visibleCount * 127u) / (visibleCount * 255u));
    uint8_t avgG4 = (uint8_t) ((sumG * 15u + visibleCount * 127u) / (visibleCount * 255u));
    uint8_t avgB4 = (uint8_t) ((sumB * 15u + visibleCount * 127u) / (visibleCount * 255u));
    best.error = UINT32_MAX;

    for (int dr = -1; dr <= 1; ++dr) {
        for (int dg = -1; dg <= 1; ++dg) {
            for (int db = -1; db <= 1; ++db) {
                uint8_t r4 = clampNativeNibble((int) avgR4 + dr);
                uint8_t g4 = clampNativeNibble((int) avgG4 + dg);
                uint8_t b4 = clampNativeNibble((int) avgB4 + db);
                for (uint8_t table = 0u; table < 8u; ++table) {
                    NativeTextureETC1A4Subblock candidate = {
                        .r4 = r4,
                        .g4 = g4,
                        .b4 = b4,
                        .table = table,
                    };
                    uint32_t error = evaluateEtc1a4Subblock(input, &candidate);
                    if (error < best.error) {
                        best.r4 = r4;
                        best.g4 = g4;
                        best.b4 = b4;
                        best.table = table;
                        best.error = error;
                    }
                }
            }
        }
    }
    return best;
}

void texEncodeEtc1a4Block(const uint8_t* rgba8888, uint32_t width, uint32_t height, uint32_t blockX,
    uint32_t blockY, uint8_t* out) {
    uint32_t alphaBytes[8] = {0};

    for (uint32_t y = 0; y < 4u; ++y) {
        for (uint32_t x = 0; x < 4u; ++x) {
            uint32_t pxX = blockX + x;
            uint32_t pxY = blockY + y;
            uint32_t pos = etc1a4PixelPos(x, y);
            uint8_t alpha = 0u;
            if (pxX < width && pxY < height) {
                const uint8_t* px = rgba8888 + ((size_t) pxY * width + pxX) * 4u;
                alpha = px[3];
            }
            uint8_t alpha4 = (uint8_t) (((uint32_t) alpha * 15u + 127u) / 255u);
            uint32_t byteIndex = pos >> 1u;
            if ((pos & 1u) == 0u)
                alphaBytes[byteIndex] |= alpha4;
            else
                alphaBytes[byteIndex] |= (uint32_t) alpha4 << 4u;
        }
    }

    NativeTextureETC1A4Subblock bestSubblocks[2] = {0};
    bool bestFlip = false;
    uint32_t bestError = UINT32_MAX;
    for (uint32_t flipValue = 0u; flipValue < 2u; ++flipValue) {
        bool flip = flipValue != 0u;
        EtcSubblockInput input = {
            .rgba = rgba8888,
            .width = width,
            .height = height,
            .blockX = blockX,
            .blockY = blockY,
            .flip = flip,
        };
        NativeTextureETC1A4Subblock sub0 = chooseEtc1a4Subblock(&input);
        input.index = 1u;
        NativeTextureETC1A4Subblock sub1 = chooseEtc1a4Subblock(&input);
        uint32_t error = sub0.error + sub1.error;
        if (error < bestError) {
            bestError = error;
            bestFlip = flip;
            bestSubblocks[0] = sub0;
            bestSubblocks[1] = sub1;
        }
    }

    uint32_t selectorLo = 0u;
    uint32_t selectorHi = 0u;
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            uint32_t pxX = blockX + x;
            uint32_t pxY = blockY + y;
            uint32_t subblock = etc1a4PixelInSubblock(x, y, bestFlip, 0u) ? 0u : 1u;
            NativeTextureETC1A4Subblock* sub = &bestSubblocks[subblock];
            uint8_t baseR = expand4(sub->r4);
            uint8_t baseG = expand4(sub->g4);
            uint8_t baseB = expand4(sub->b4);
            uint32_t selector = 3u;
            if (pxX < width && pxY < height) {
                const uint8_t* px = rgba8888 + ((size_t) pxY * width + pxX) * 4u;
                if (px[3] != 0u) {
                    uint32_t bestPixelError = UINT32_MAX;
                    for (uint32_t candidate = 0u; candidate < 4u; ++candidate) {
                        uint32_t error = etc1a4ColorError(
                            px, baseR, baseG, baseB, modifierTables[sub->table][candidate]);
                        if (error < bestPixelError) {
                            bestPixelError = error;
                            selector = candidate;
                        }
                    }
                }
            }
            uint32_t pos = etc1a4PixelPos(x, y);
            if ((selector & 1u) != 0u) selectorLo |= 1u << pos;
            if ((selector & 2u) != 0u) selectorHi |= 1u << pos;
        }
    }

    for (uint32_t i = 0u; i < 8u; ++i) out[i] = (uint8_t) alphaBytes[i];
    uint64_t colorBlock = ((uint64_t) ((bestSubblocks[0].r4 << 4u) | bestSubblocks[1].r4) << 56u) |
        ((uint64_t) ((bestSubblocks[0].g4 << 4u) | bestSubblocks[1].g4) << 48u) |
        ((uint64_t) ((bestSubblocks[0].b4 << 4u) | bestSubblocks[1].b4) << 40u) |
        ((uint64_t) bestSubblocks[0].table << 37u) | ((uint64_t) bestSubblocks[1].table << 34u) |
        (bestFlip ? (1ull << 32u) : 0ull) | ((uint64_t) selectorHi << 16u) | (uint64_t) selectorLo;
    writeLe64(out + 8u, colorBlock);
}
