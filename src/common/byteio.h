#ifndef CINNAMON_PREPROCESSOR_BYTEIO_H
#define CINNAMON_PREPROCESSOR_BYTEIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t size;
    uint32_t sampleHash;
} CinnamonSourceFingerprint;

void writeLe16(uint8_t* ptr, uint16_t value);
uint16_t readLe16(const uint8_t* ptr);
void writeLe32(uint8_t* ptr, uint32_t value);
uint32_t readLe32(const uint8_t* ptr);
void writeLe64(uint8_t* ptr, uint64_t value);
uint64_t readLe64(const uint8_t* ptr);
uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, size_t length);
bool fingerprintFile(const char* path, CinnamonSourceFingerprint* out);

#endif
