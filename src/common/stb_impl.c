#include "preprocess_common.h"
#include "stb_allocator.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void* imageAlloc(size_t size) {
    if (size == 0u) size = 1u;
    return malloc(size);
}

void imageFree(void* ptr) {
    free(ptr);
}

static void* stbiReallocSized(void* ptr, size_t oldSize, size_t newSize) {
    (void) oldSize;
    if (ptr == NULL) return imageAlloc(newSize);
    if (newSize == 0u) {
        imageFree(ptr);
        return NULL;
    }
    return realloc(ptr, newSize);
}

static CINNAMON_PREPROCESSOR_MAYBE_UNUSED void* stbiRealloc(void* ptr, size_t newSize) {
    return stbiReallocSized(ptr, 0u, newSize);
}

#define STBI_MALLOC(size) imageAlloc(size)
#define STBI_FREE(ptr) imageFree(ptr)
#define STBI_REALLOC(ptr, size) stbiRealloc(ptr, size)
#define STBI_REALLOC_SIZED(ptr, oldSize, newSize) stbiReallocSized(ptr, oldSize, newSize)

#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include <stb/image/stb_image.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t idatSize;
} PngRgbaMetadata;

static uint32_t pngReadBe32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) |
        (uint32_t) p[3];
}

static int pngPaeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static bool readPngRgbaMetadata(const uint8_t* blob, size_t blobSize, PngRgbaMetadata* metadata) {
    static const uint8_t pngSignature[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
    if (blob == NULL || metadata == NULL || blobSize < 33u ||
        memcmp(blob, pngSignature, sizeof(pngSignature)) != 0) {
        return false;
    }

    memset(metadata, 0, sizeof(*metadata));
    bool sawHeader = false;
    bool sawImageData = false;
    bool sawEnd = false;
    size_t pos = 8u;
    while (pos <= blobSize && blobSize - pos >= 12u) {
        uint32_t length = pngReadBe32(blob + pos);
        const uint8_t* type = blob + pos + 4u;
        if ((size_t) length > blobSize - pos - 12u) return false;
        const uint8_t* data = blob + pos + 8u;

        if (memcmp(type, "IHDR", 4u) == 0) {
            if (sawHeader || pos != 8u || length != 13u) return false;
            metadata->width = pngReadBe32(data);
            metadata->height = pngReadBe32(data + 4u);
            if (data[8] != 8u || data[9] != 6u || data[10] != 0u || data[11] != 0u ||
                data[12] != 0u) {
                return false;
            }
            sawHeader = true;
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            if (!sawHeader || metadata->idatSize > SIZE_MAX - length) return false;
            metadata->idatSize += length;
            sawImageData = true;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            if (length != 0u) return false;
            sawEnd = true;
            break;
        }

        pos += 12u + length;
    }

    return sawHeader && sawImageData && sawEnd && metadata->width > 0u && metadata->height > 0u &&
        metadata->width <= 65535u && metadata->height <= 65535u && metadata->idatSize > 0u &&
        metadata->idatSize <= (size_t) INT_MAX;
}

static bool copyPngImageData(
    const uint8_t* blob, size_t blobSize, uint8_t* compressed, size_t compressedSize) {
    size_t outputPos = 0u;
    size_t pos = 8u;
    while (pos <= blobSize && blobSize - pos >= 12u) {
        uint32_t length = pngReadBe32(blob + pos);
        const uint8_t* type = blob + pos + 4u;
        if (memcmp(type, "IDAT", 4u) == 0) {
            if (outputPos > compressedSize || (size_t) length > compressedSize - outputPos)
                return false;
            memcpy(compressed + outputPos, blob + pos + 8u, length);
            outputPos += length;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            break;
        }
        pos += 12u + length;
    }
    return outputPos == compressedSize;
}

uint8_t* imageLoadPngRgba(const uint8_t* blob, size_t blobSize, int* outWidth, int* outHeight) {
    if (outWidth != NULL) *outWidth = 0;
    if (outHeight != NULL) *outHeight = 0;

    PngRgbaMetadata metadata;
    if (!readPngRgbaMetadata(blob, blobSize, &metadata)) return NULL;

    size_t rowBytes = (size_t) metadata.width * 4u;
    if (rowBytes == SIZE_MAX || metadata.height > SIZE_MAX / (rowBytes + 1u)) return NULL;
    size_t rawStride = rowBytes + 1u;
    size_t rawSize = rawStride * metadata.height;
    if (rawSize > (size_t) INT_MAX) return NULL;

    uint8_t* compressed = malloc(metadata.idatSize);
    if (compressed == NULL) return NULL;
    if (!copyPngImageData(blob, blobSize, compressed, metadata.idatSize)) {
        free(compressed);
        return NULL;
    }

    uint8_t* pixels = imageAlloc(rawSize);
    if (pixels == NULL) {
        free(compressed);
        return NULL;
    }

    int inflated = stbi_zlib_decode_buffer(
        (char*) pixels, (int) rawSize, (const char*) compressed, (int) metadata.idatSize);
    free(compressed);
    if (inflated != (int) rawSize) {
        imageFree(pixels);
        return NULL;
    }

    for (uint32_t y = 0u; y < metadata.height; ++y) {
        uint8_t* src = pixels + y * rawStride;
        uint8_t* dst = pixels + y * rowBytes;
        const uint8_t* prior = y > 0u ? dst - rowBytes : NULL;
        uint8_t filter = src[0];
        src++;
        if (filter > 4u) {
            imageFree(pixels);
            return NULL;
        }

        for (size_t x = 0u; x < rowBytes; ++x) {
            uint8_t raw = src[x];
            uint8_t left = x >= 4u ? dst[x - 4u] : 0u;
            uint8_t up = prior != NULL ? prior[x] : 0u;
            uint8_t upLeft = (prior != NULL && x >= 4u) ? prior[x - 4u] : 0u;
            switch (filter) {
            case 0u:
                dst[x] = raw;
                break;
            case 1u:
                dst[x] = (uint8_t) (raw + left);
                break;
            case 2u:
                dst[x] = (uint8_t) (raw + up);
                break;
            case 3u:
                dst[x] = (uint8_t) (raw + ((uint32_t) left + (uint32_t) up) / 2u);
                break;
            case 4u:
                dst[x] = (uint8_t) (raw + pngPaeth(left, up, upLeft));
                break;
            default:
                break;
            }
        }
    }

    if (outWidth != NULL) *outWidth = (int) metadata.width;
    if (outHeight != NULL) *outHeight = (int) metadata.height;
    return pixels;
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/image/stb_image_write.h>

#define STB_DS_IMPLEMENTATION
#include <stb/ds/stb_ds.h>

#include "stb/vorbis/stb_vorbis.c"
