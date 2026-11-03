#ifndef CINNAMON_PREPROCESSOR_TEXTURE_LZ11_H
#define CINNAMON_PREPROCESSOR_TEXTURE_LZ11_H

#include <stddef.h>
#include <stdint.h>

uint8_t* lz11CompressBounded(
    const uint8_t* data, size_t size, size_t maxOutputSize, size_t* outSize);

#endif
