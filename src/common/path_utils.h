#ifndef CINNAMON_PREPROCESSOR_PATH_UTILS_H
#define CINNAMON_PREPROCESSOR_PATH_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

char* pathDupParent(const char* path);
bool pathCopy(char* destination, size_t destinationSize, const char* source);
bool pathsEqual(const char* first, const char* second);
bool pathHasComponentsNoCase(
    const char* path, const char* const* components, size_t componentCount);
bool pathJoin(const char* parent, const char* child, char* outPath, size_t outPathSize);
char* pathJoinAlloc(const char* parent, const char* child);
bool pathWithSuffix(const char* path, const char* suffix, char* outPath, size_t outPathSize);
const char* pathBaseName(const char* path);
void pathTrim(char* value);
void pathUnquote(char* value);
void pathSanitizeLabel(char* dst, size_t dstSize, const char* src);
void pathAddDebugName(char* dst, size_t dstSize, const char* name);
bool pathIsDriveRoot(const char* path);

#endif
