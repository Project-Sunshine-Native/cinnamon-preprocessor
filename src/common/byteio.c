#include "common/byteio.h"

#include <limits.h>
#include <stdio.h>

void writeLe16(uint8_t* ptr, uint16_t value) {
    ptr[0] = (uint8_t) (value & 0xFFu);
    ptr[1] = (uint8_t) ((value >> 8u) & 0xFFu);
}

uint16_t readLe16(const uint8_t* ptr) {
    return (uint16_t) ptr[0] | (uint16_t) ((uint16_t) ptr[1] << 8u);
}

void writeLe32(uint8_t* ptr, uint32_t value) {
    ptr[0] = (uint8_t) (value & 0xFFu);
    ptr[1] = (uint8_t) ((value >> 8) & 0xFFu);
    ptr[2] = (uint8_t) ((value >> 16) & 0xFFu);
    ptr[3] = (uint8_t) ((value >> 24) & 0xFFu);
}

uint32_t readLe32(const uint8_t* ptr) {
    return (uint32_t) ptr[0] | ((uint32_t) ptr[1] << 8u) | ((uint32_t) ptr[2] << 16u) |
        ((uint32_t) ptr[3] << 24u);
}

void writeLe64(uint8_t* ptr, uint64_t value) {
    writeLe32(ptr + 0u, (uint32_t) (value & 0xFFFFFFFFull));
    writeLe32(ptr + 4u, (uint32_t) (value >> 32u));
}

uint64_t readLe64(const uint8_t* ptr) {
    return (uint64_t) readLe32(ptr + 0u) | ((uint64_t) readLe32(ptr + 4u) << 32u);
}

uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

bool fingerprintFile(const char* path, CinnamonSourceFingerprint* out) {
    if (path == NULL || out == NULL) return false;
    out->size = 0u;
    out->sampleHash = 0u;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long fileSizeLong = ftell(file);
    if (fileSizeLong <= 0) {
        fclose(file);
        return false;
    }

    uint64_t fileSize = (uint64_t) fileSizeLong;
    out->size = fileSize;

    uint8_t sizeBytes[8];
    writeLe64(sizeBytes, fileSize);
    uint32_t hash = fnv1aUpdate(2166136261u, sizeBytes, sizeof(sizeBytes));
    static const uint32_t sampleCount = 5u;
    static const uint32_t sampleSize = 1024u;
    uint8_t buffer[1024];

    for (uint32_t i = 0u; i < sampleCount; ++i) {
        uint64_t target = 0u;
        if (i == 1u)
            target = fileSize / 4u;
        else if (i == 2u)
            target = fileSize / 2u;
        else if (i == 3u)
            target = (fileSize * 3u) / 4u;
        else if (i == 4u)
            target = fileSize > sampleSize ? fileSize - sampleSize : 0u;
        if (target > (uint64_t) LONG_MAX) target = (uint64_t) LONG_MAX;
        if (fseek(file, (long) target, SEEK_SET) != 0) break;
        size_t bytesRead = fread(buffer, 1, sizeof(buffer), file);
        if (bytesRead == 0u) break;
        hash = fnv1aUpdate(hash, buffer, bytesRead);
    }

    fclose(file);
    out->sampleHash = hash != 0u ? hash : 1u;
    return true;
}
