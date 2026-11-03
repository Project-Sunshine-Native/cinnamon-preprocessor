#ifndef CINNAMON_PREPROCESSOR_FS_UTILS_H
#define CINNAMON_PREPROCESSOR_FS_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

bool fsEnsureDir(const char* path);
bool fsFileExists(const char* path);
bool fsDirExists(const char* path);
bool fsDeleteTree(const char* path);
bool fsPruneTree(const char* srcDir, const char* dstDir);
bool fsProgramDir(const char* argv0, char* outDir, size_t outDirSize);
uint8_t* fsReadSlice(const char* path, uint32_t offset, uint32_t size);
uint8_t* fsReadFile(const char* path, uint32_t* outSize);
bool fsFilesEqual(const char* pathA, const char* pathB);
bool fsCopyFile(const char* srcPath, const char* dstPath);
bool fsCopyAtomic(const char* srcPath, const char* dstPath);
bool fsCommitTemp(const char* tmpPath, const char* finalPath);
bool fsSyncTree(
    const char* srcDir, const char* dstDir, uint32_t* outUpdatedCount, uint32_t* outSkippedCount);
char* fsFindFile(const char* srcDir, const char* targetName);
bool fsWriteFile(const char* path, const uint8_t* data, uint32_t size);

#endif
