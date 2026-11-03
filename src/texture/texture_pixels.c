#include "texture_convert_internal.h"

bool texRgbaBytes(uint32_t width, uint32_t height, size_t* outBytes) {
    size_t pixels = 0u;
    return width != 0u && height != 0u &&
        checkedMulSize((size_t) width, (size_t) height, &pixels) &&
        checkedMulSize(pixels, 4u, outBytes);
}

const uint8_t* texSampleRect(const TextureRgbaRect* source, uint32_t x, uint32_t y) {
    static const uint8_t transparent[4] = {0};
    uint64_t absoluteX = (uint64_t) source->sourceX + x;
    uint64_t absoluteY = (uint64_t) source->sourceY + y;
    if (source->rgba == NULL || absoluteX >= source->sourceWidth ||
        absoluteY >= source->sourceHeight)
        return transparent;
    return source->rgba + ((size_t) absoluteY * source->sourceWidth + (size_t) absoluteX) * 4u;
}

uint32_t texCountOpaque(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (rgba == NULL || width == 0u || height == 0u) return 0u;
    uint32_t count = 0u;
    preprocessRepeat((size_t) width * height, pxIndex) {
        if (rgba[pxIndex * 4u + 3u] != 0u) count++;
    }
    return count;
}

uint32_t texCountOpaqueRect(const TextureRgbaRect* source) {
    if (source->rgba == NULL || source->sourceWidth == 0u || source->sourceHeight == 0u ||
        source->width == 0u || source->height == 0u)
        return 0u;
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < source->height; ++y) {
        for (uint32_t x = 0u; x < source->width; ++x) {
            const uint8_t* px = texSampleRect(source, x, y);
            if (px[3] != 0u) count++;
        }
    }
    return count;
}

void texBlit(uint8_t* dst, uint32_t dstStride, uint32_t dstX, uint32_t dstY, const uint8_t* src,
    uint32_t srcStride, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height) {
    for (uint32_t row = 0u; row < height; ++row) {
        memcpy(dst + (((size_t) dstY + row) * dstStride + dstX) * 4u,
            src + (((size_t) srcY + row) * srcStride + srcX) * 4u, (size_t) width * 4u);
    }
}

bool texRectFits(uint32_t texWidth, uint32_t texHeight, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height) {
    return texWidth != 0u && texHeight != 0u && width != 0u && height != 0u && x < texWidth &&
        y < texHeight && width <= texWidth - x && height <= texHeight - y;
}

bool texBlitClipped(uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight, int32_t dstX, int32_t dstY,
    const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, int32_t srcX, int32_t srcY,
    int32_t width, int32_t height) {
    if (dst == NULL || src == NULL) return false;
    if (dstWidth == 0 || dstHeight == 0 || srcWidth == 0 || srcHeight == 0) return false;
    if (width <= 0 || height <= 0) return false;

    if (dstX < 0) {
        srcX -= dstX;
        width += dstX;
        dstX = 0;
    }
    if (dstY < 0) {
        srcY -= dstY;
        height += dstY;
        dstY = 0;
    }
    if (srcX < 0) {
        dstX -= srcX;
        width += srcX;
        srcX = 0;
    }
    if (srcY < 0) {
        dstY -= srcY;
        height += srcY;
        srcY = 0;
    }

    if (dstX >= (int32_t) dstWidth || dstY >= (int32_t) dstHeight) return false;
    if (srcX >= (int32_t) srcWidth || srcY >= (int32_t) srcHeight) return false;

    if (dstX + width > (int32_t) dstWidth) width = (int32_t) dstWidth - dstX;
    if (dstY + height > (int32_t) dstHeight) height = (int32_t) dstHeight - dstY;
    if (srcX + width > (int32_t) srcWidth) width = (int32_t) srcWidth - srcX;
    if (srcY + height > (int32_t) srcHeight) height = (int32_t) srcHeight - srcY;

    if (width <= 0 || height <= 0) return false;
    texBlit(dst, dstWidth, (uint32_t) dstX, (uint32_t) dstY, src, srcWidth, (uint32_t) srcX,
        (uint32_t) srcY, (uint32_t) width, (uint32_t) height);
    return true;
}

void texPadBorder(uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t padding) {
    if (dst == NULL || dstWidth == 0u || dstHeight == 0u || width == 0u || height == 0u ||
        padding == 0u)
        return;
    if (x >= dstWidth || y >= dstHeight || x + width > dstWidth || y + height > dstHeight) return;

    for (uint32_t row = 0u; row < height; ++row) {
        uint8_t* dstRow = dst + ((size_t) y + row) * dstWidth * 4u;
        for (uint32_t pad = 1u; pad <= padding && x >= pad; ++pad) {
            memcpy(dstRow + ((size_t) x - pad) * 4u, dstRow + (size_t) x * 4u, 4u);
        }
        uint32_t right = x + width - 1u;
        for (uint32_t pad = 1u; pad <= padding && right + pad < dstWidth; ++pad) {
            memcpy(dstRow + ((size_t) right + pad) * 4u, dstRow + (size_t) right * 4u, 4u);
        }
    }

    uint32_t copyX = x > padding ? x - padding : 0u;
    uint32_t copyEndX = x + width + padding;
    if (copyEndX > dstWidth) copyEndX = dstWidth;
    size_t copyBytes = (size_t) (copyEndX - copyX) * 4u;
    uint8_t* topRow = dst + ((size_t) y * dstWidth + copyX) * 4u;
    uint8_t* bottomRow = dst + (((size_t) y + height - 1u) * dstWidth + copyX) * 4u;
    for (uint32_t pad = 1u; pad <= padding && y >= pad; ++pad) {
        memcpy(dst + (((size_t) y - pad) * dstWidth + copyX) * 4u, topRow, copyBytes);
    }
    uint32_t bottom = y + height - 1u;
    for (uint32_t pad = 1u; pad <= padding && bottom + pad < dstHeight; ++pad) {
        memcpy(dst + (((size_t) bottom + pad) * dstWidth + copyX) * 4u, bottomRow, copyBytes);
    }
}

bool texRectIsMono(const TextureRgbaRect* source) {
    if (source->rgba == NULL || source->width == 0u || source->height == 0u) return false;

    uint32_t visiblePixels = 0;
    uint32_t tintedPixels = 0;
    for (uint32_t row = 0u; row < source->height; ++row) {
        for (uint32_t col = 0u; col < source->width; ++col) {
            const uint8_t* px = texSampleRect(source, col, row);
            if (px[3] == 0u) continue;

            visiblePixels++;
            uint8_t maxRgb = px[0];
            if (px[1] > maxRgb) maxRgb = px[1];
            if (px[2] > maxRgb) maxRgb = px[2];

            uint8_t minRgb = px[0];
            if (minRgb > px[1]) minRgb = px[1];
            if (minRgb > px[2]) minRgb = px[2];

            if ((uint8_t) (maxRgb - minRgb) > 18u) tintedPixels++;
        }
    }

    return visiblePixels > 0u && tintedPixels == 0u;
}
