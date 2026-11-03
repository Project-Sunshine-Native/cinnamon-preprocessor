#include "common/preprocess_common.h"
#include "texture_lz11.h"
#include "common/byteio.h"
#include "utils.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define CINNAMON_LZ11_MIN_MATCH 3u
#define CINNAMON_LZ11_WINDOW_SIZE 0x1000u
#define CINNAMON_LZ11_MAX_MATCH 0x10110u
#define CINNAMON_LZ11_HASH_SIZE 4096u
#define CINNAMON_LZ11_MAX_CHAIN 128u

static uint32_t lz11Hash3(const uint8_t* data) {
    return (((uint32_t) data[0] * 251u) ^ ((uint32_t) data[1] * 31u) ^ (uint32_t) data[2]) &
        (CINNAMON_LZ11_HASH_SIZE - 1u);
}

#if !defined(__EMSCRIPTEN__)

static void lz11Insert(
    const uint8_t* data, size_t size, size_t pos, int32_t* heads, int32_t* prev) {
    if (pos + 2u >= size) return;
    uint32_t hash = lz11Hash3(data + pos);
    prev[pos] = heads[hash];
    heads[hash] = (int32_t) pos;
}

static void lz11FindMatch(const uint8_t* data, size_t size, size_t pos, const int32_t* heads,
    const int32_t* prev, size_t* outLength, size_t* outDisp) {
    *outLength = 0u;
    *outDisp = 0u;
    if (pos + CINNAMON_LZ11_MIN_MATCH > size) return;

    size_t maxLength = size - pos;
    if (maxLength > CINNAMON_LZ11_MAX_MATCH) maxLength = CINNAMON_LZ11_MAX_MATCH;
    uint32_t hash = lz11Hash3(data + pos);
    int32_t candidate = heads[hash];
    uint32_t searched = 0u;
    while (candidate >= 0 && searched++ < CINNAMON_LZ11_MAX_CHAIN) {
        size_t candidatePos = (size_t) candidate;
        if (candidatePos >= pos) break;
        size_t distance = pos - candidatePos;
        if (distance > CINNAMON_LZ11_WINDOW_SIZE) break;
        size_t candidateMaxLength = maxLength;
        if (candidateMaxLength > size - candidatePos) candidateMaxLength = size - candidatePos;
        size_t length = 0u;
        while (length < candidateMaxLength && data[candidatePos + length] == data[pos + length])
            length++;
        if (length >= CINNAMON_LZ11_MIN_MATCH && length > *outLength) {
            *outLength = length;
            *outDisp = distance - 1u;
            if (length == candidateMaxLength) break;
        }
        candidate = prev[candidatePos];
    }
}
#endif

#if defined(__EMSCRIPTEN__)
static void findLatestHashMatch(const uint8_t* data, size_t size, size_t pos, const int32_t* heads,
    size_t* outLength, size_t* outDisp) {
    *outLength = 0u;
    *outDisp = 0u;
    if (pos + CINNAMON_LZ11_MIN_MATCH > size) return;

    uint32_t hash = lz11Hash3(data + pos);
    int32_t candidate = heads[hash];
    if (candidate < 0) return;
    size_t candidatePos = (size_t) candidate;
    if (candidatePos >= pos) return;
    size_t distance = pos - candidatePos;
    if (distance > CINNAMON_LZ11_WINDOW_SIZE) return;

    size_t maxLength = size - pos;
    if (maxLength > size - candidatePos) maxLength = size - candidatePos;
    if (maxLength > CINNAMON_LZ11_MAX_MATCH) maxLength = CINNAMON_LZ11_MAX_MATCH;
    size_t length = 0u;
    while (length < maxLength && data[candidatePos + length] == data[pos + length]) length++;
    if (length >= CINNAMON_LZ11_MIN_MATCH) {
        *outLength = length;
        *outDisp = distance - 1u;
    }
}
#endif

static bool lz11EmitMatch(
    uint8_t* out, size_t outCapacity, size_t* inOutPos, size_t length, size_t disp) {
    if (length < CINNAMON_LZ11_MIN_MATCH || disp >= CINNAMON_LZ11_WINDOW_SIZE) return false;
    size_t pos = *inOutPos;
    if (length <= 0x10u) {
        if (pos + 2u > outCapacity) return false;
        out[pos++] = (uint8_t) (((length - 1u) << 4u) | ((disp >> 8u) & 0x0Fu));
        out[pos++] = (uint8_t) disp;
    } else if (length <= 0x110u) {
        if (pos + 3u > outCapacity) return false;
        size_t value = length - 0x11u;
        out[pos++] = (uint8_t) ((value >> 4u) & 0x0Fu);
        out[pos++] = (uint8_t) (((value & 0x0Fu) << 4u) | ((disp >> 8u) & 0x0Fu));
        out[pos++] = (uint8_t) disp;
    } else {
        if (pos + 4u > outCapacity) return false;
        size_t value = length - 0x111u;
        out[pos++] = (uint8_t) (0x10u | ((value >> 12u) & 0x0Fu));
        out[pos++] = (uint8_t) (value >> 4u);
        out[pos++] = (uint8_t) (((value & 0x0Fu) << 4u) | ((disp >> 8u) & 0x0Fu));
        out[pos++] = (uint8_t) disp;
    }
    *inOutPos = pos;
    return true;
}

uint8_t* lz11CompressBounded(
    const uint8_t* data, size_t size, size_t maxOutputSize, size_t* outSize) {
    if (data == NULL || outSize == NULL || size == 0u || size > 0xFFFFFFFFu || maxOutputSize == 0u)
        return NULL;
    size_t headerSize = size < 0x1000000u ? 4u : 8u;
    if (maxOutputSize < headerSize) return NULL;

    size_t capacity = 0u;
    if (!checkedAddSize(headerSize, size, &capacity) ||
        !checkedAddSize(capacity, size / 8u, &capacity) ||
        !checkedAddSize(capacity, 32u, &capacity)) {
        return NULL;
    }
    if (capacity > maxOutputSize) capacity = maxOutputSize;
    uint8_t* out = safeMalloc(capacity);
    int32_t* heads = safeMalloc(sizeof(int32_t) * CINNAMON_LZ11_HASH_SIZE);
#if !defined(__EMSCRIPTEN__)
    size_t prevBytes = 0u;
    if (!checkedMulSize(sizeof(int32_t), size, &prevBytes)) {
        free(out);
        free(heads);
        return NULL;
    }
    int32_t* prev = safeMalloc(prevBytes);
#endif
    preprocessRepeat(CINNAMON_LZ11_HASH_SIZE, i) heads[i] = -1;
#if !defined(__EMSCRIPTEN__)
    preprocessRepeat(size, i) prev[i] = -1;
#endif

    size_t outPos = 0u;
    out[outPos++] = 0x11u;
    if (headerSize == 4u) {
        out[outPos++] = (uint8_t) size;
        out[outPos++] = (uint8_t) (size >> 8u);
        out[outPos++] = (uint8_t) (size >> 16u);
    } else {
        out[outPos++] = 0u;
        out[outPos++] = 0u;
        out[outPos++] = 0u;
        writeLe32(out + outPos, (uint32_t) size);
        outPos += 4u;
    }

    size_t pos = 0u;
    while (pos < size) {
        if (outPos >= capacity) {
            free(out);
            free(heads);
#if !defined(__EMSCRIPTEN__)
            free(prev);
#endif
            return NULL;
        }
        size_t flagPos = outPos++;
        out[flagPos] = 0u;
        for (uint32_t bit = 0u; bit < 8u && pos < size; ++bit) {
            size_t matchLength = 0u;
            size_t matchDisp = 0u;
#if defined(__EMSCRIPTEN__)
            findLatestHashMatch(data, size, pos, heads, &matchLength, &matchDisp);
#else
            lz11FindMatch(data, size, pos, heads, prev, &matchLength, &matchDisp);
#endif
            if (matchLength >= CINNAMON_LZ11_MIN_MATCH) {
                out[flagPos] |= (uint8_t) (0x80u >> bit);
                if (!lz11EmitMatch(out, capacity, &outPos, matchLength, matchDisp)) {
                    free(out);
                    free(heads);
#if !defined(__EMSCRIPTEN__)
                    free(prev);
#endif
                    return NULL;
                }
#if defined(__EMSCRIPTEN__)
                for (size_t i = 0u; i < matchLength; ++i) {
                    size_t insertPos = pos + i;
                    if (insertPos + 2u < size)
                        heads[lz11Hash3(data + insertPos)] = (int32_t) insertPos;
                }
#else
                for (size_t i = 0u; i < matchLength; ++i)
                    lz11Insert(data, size, pos + i, heads, prev);
#endif
                pos += matchLength;
            } else {
                if (outPos >= capacity) {
                    free(out);
                    free(heads);
#if !defined(__EMSCRIPTEN__)
                    free(prev);
#endif
                    return NULL;
                }
                out[outPos++] = data[pos];
#if defined(__EMSCRIPTEN__)
                if (pos + 2u < size) heads[lz11Hash3(data + pos)] = (int32_t) pos;
#else
                lz11Insert(data, size, pos, heads, prev);
#endif
                pos++;
            }
        }
    }

    free(heads);
#if !defined(__EMSCRIPTEN__)
    free(prev);
#endif
    if (outPos >= maxOutputSize) {
        free(out);
        return NULL;
    }
    *outSize = outPos;
    return out;
}
