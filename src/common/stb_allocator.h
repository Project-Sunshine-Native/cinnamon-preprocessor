#ifndef CINNAMON_PREPROCESSOR_STB_ALLOCATOR_H
#define CINNAMON_PREPROCESSOR_STB_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

void* imageAlloc(size_t size);
void imageFree(void* ptr);
uint8_t* imageLoadPngRgba(const uint8_t* blob, size_t blobSize, int* outWidth, int* outHeight);

#endif
