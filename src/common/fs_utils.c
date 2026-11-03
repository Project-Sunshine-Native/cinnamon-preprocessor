#include "preprocess_common.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PREPROCESS_HOST_WINDOWS
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#define MKDIR(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0777)
#endif

#define CINNAMON_FS_IO_BUFFER_SIZE (64u * 1024u)

static bool ensureDir(const char* path) {
    if (MKDIR(path) == 0) return true;
    return errno == EEXIST && fsDirExists(path);
}

bool fsEnsureDir(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (ensureDir(path)) return true;

    char temp[PREPROCESS_PATH_MAX];
    if (!pathCopy(temp, sizeof(temp), path)) return false;
    size_t len = strlen(temp);
    if (len == 0) return false;

    for (size_t i = 1; i < len; ++i) {
        char c = temp[i];
        if (c != '/' && c != '\\') continue;
        temp[i] = '\0';
        bool isDrivePrefix = temp[i - 1] == ':';
        if (temp[0] != '\0' && !isDrivePrefix && !ensureDir(temp)) {
            return false;
        }
        temp[i] = c;
    }

    return ensureDir(temp);
}

bool fsFileExists(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
#if PREPROCESS_HOST_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

bool fsDirExists(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
#if PREPROCESS_HOST_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

#if PREPROCESS_HOST_WINDOWS
bool fsDeleteTree(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (!fsDirExists(path)) return true;

    char searchPath[PREPROCESS_TEMP_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(path, "*", searchPath, sizeof(searchPath))) return false;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle != INVALID_HANDLE_VALUE) {
        do {
            const char* name = findData.cFileName;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            char childPath[PREPROCESS_TEMP_PATH_MAX];
            if (!pathJoin(path, name, childPath, sizeof(childPath))) {
                FindClose(findHandle);
                return false;
            }
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!fsDeleteTree(childPath)) {
                    FindClose(findHandle);
                    return false;
                }
            } else {
                if (!DeleteFileA(childPath)) {
                    FindClose(findHandle);
                    return false;
                }
            }
        } while (FindNextFileA(findHandle, &findData));
        FindClose(findHandle);
    }

    return RemoveDirectoryA(path) != 0;
}
#else
bool fsDeleteTree(const char* path) {
    if (path == NULL || path[0] == '\0') return false;
    if (!fsDirExists(path)) return true;

    DIR* dir = opendir(path);
    if (dir == NULL) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char childPath[PREPROCESS_TEMP_PATH_MAX];
        if (!pathJoin(path, name, childPath, sizeof(childPath))) {
            closedir(dir);
            return false;
        }
        if (fsDirExists(childPath)) {
            if (!fsDeleteTree(childPath)) {
                closedir(dir);
                return false;
            }
        } else if (remove(childPath) != 0) {
            closedir(dir);
            return false;
        }
    }

    closedir(dir);
    return rmdir(path) == 0;
}
#endif

bool fsPruneTree(const char* srcDir, const char* dstDir) {
    if (srcDir == NULL || dstDir == NULL) return false;
    if (!fsDirExists(dstDir)) return true;
    if (!fsDirExists(srcDir)) return false;

#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_SEARCH_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(dstDir, "*", searchPath, sizeof(searchPath))) return false;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return true;

    bool ok = true;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char srcChild[PREPROCESS_SEARCH_PATH_MAX];
        char dstChild[PREPROCESS_SEARCH_PATH_MAX];
        if (!pathJoin(srcDir, name, srcChild, sizeof(srcChild)) ||
            !pathJoin(dstDir, name, dstChild, sizeof(dstChild))) {
            ok = false;
            break;
        }

        bool dstIsDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dstIsDir) {
            if (!fsDirExists(srcChild))
                ok = fsDeleteTree(dstChild);
            else
                ok = fsPruneTree(srcChild, dstChild);
        } else if (!fsFileExists(srcChild)) {
            ok = DeleteFileA(dstChild) != 0;
        }
        if (!ok) break;
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return ok;
#else
    DIR* dir = opendir(dstDir);
    if (dir == NULL) return true;

    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char* srcChild = pathJoinAlloc(srcDir, name);
        char* dstChild = pathJoinAlloc(dstDir, name);
        if (srcChild == NULL || dstChild == NULL) {
            free(srcChild);
            free(dstChild);
            ok = false;
            break;
        }

        if (fsDirExists(dstChild)) {
            if (!fsDirExists(srcChild))
                ok = fsDeleteTree(dstChild);
            else
                ok = fsPruneTree(srcChild, dstChild);
        } else if (!fsFileExists(srcChild)) {
            ok = remove(dstChild) == 0;
        }

        free(srcChild);
        free(dstChild);
        if (!ok) break;
    }

    closedir(dir);
    return ok;
#endif
}

uint8_t* fsReadSlice(const char* path, uint32_t offset, uint32_t size) {
    if (path == NULL || path[0] == '\0' || size == 0) return NULL;
#if UINT32_MAX > LONG_MAX
    if (offset > (uint32_t) LONG_MAX) return NULL;
#endif
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long fileSizeLong = ftell(file);
    if (fileSizeLong < 0 || (uint64_t) offset + size > (uint64_t) fileSizeLong) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, (long) offset, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* data = safeMalloc(size);
    bool ok = fread(data, 1, size, file) == size;
    fclose(file);
    if (!ok) {
        free(data);
        return NULL;
    }
    return data;
}

uint8_t* fsReadFile(const char* path, uint32_t* outSize) {
    if (outSize != NULL) *outSize = 0u;
    if (path == NULL || path[0] == '\0') return NULL;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long sizeLong = ftell(file);
    if (sizeLong <= 0 || (uint64_t) sizeLong > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint32_t size = (uint32_t) sizeLong;
    uint8_t* data = safeMalloc(size);
    bool ok = fread(data, 1, size, file) == size;
    fclose(file);
    if (!ok) {
        free(data);
        return NULL;
    }
    if (outSize != NULL) *outSize = size;
    return data;
}

bool fsProgramDir(const char* argv0, char* outDir, size_t outDirSize) {
    if (outDir == NULL || outDirSize == 0) return false;
    outDir[0] = '\0';

#if PREPROCESS_HOST_WINDOWS
    DWORD len = GetModuleFileNameA(NULL, outDir, (DWORD) outDirSize);
    if (len > 0 && len < outDirSize) {
        char* slash = strrchr(outDir, '\\');
        char* forward = strrchr(outDir, '/');
        if (forward != NULL && (slash == NULL || forward > slash)) slash = forward;
        if (slash != NULL) {
            *slash = '\0';
            return true;
        }
    }
#else
    char exePath[PREPROCESS_PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0 && (size_t) len < sizeof(exePath)) {
        exePath[len] = '\0';
        char* slash = strrchr(exePath, '/');
        if (slash != NULL) {
            *slash = '\0';
            return pathCopy(outDir, outDirSize, exePath);
        }
    }
#endif

    if (strNotEmpty(argv0)) {
        char candidate[PREPROCESS_PATH_MAX];
        if (!pathCopy(candidate, sizeof(candidate), argv0)) return false;
        char* slash = strrchr(candidate, '/');
        char* backslash = strrchr(candidate, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
        if (slash != NULL) {
            *slash = '\0';
            if (candidate[0] != '\0') {
                return pathCopy(outDir, outDirSize, candidate);
            }
        }
    }

#if PREPROCESS_HOST_WINDOWS
    DWORD cwdLen = GetCurrentDirectoryA((DWORD) outDirSize, outDir);
    return cwdLen > 0 && cwdLen < outDirSize;
#else
    return getcwd(outDir, outDirSize) != NULL;
#endif
}

bool fsFilesEqual(const char* pathA, const char* pathB) {
    if (pathA == NULL || pathB == NULL) return false;
    FILE* fileA = fopen(pathA, "rb");
    FILE* fileB = fopen(pathB, "rb");
    if (fileA == NULL || fileB == NULL) {
        if (fileA != NULL) fclose(fileA);
        if (fileB != NULL) fclose(fileB);
        return false;
    }

    bool identical = true;
    uint8_t* bufferA = safeMalloc(CINNAMON_FS_IO_BUFFER_SIZE);
    uint8_t* bufferB = safeMalloc(CINNAMON_FS_IO_BUFFER_SIZE);

    while (identical) {
        size_t readA = fread(bufferA, 1, CINNAMON_FS_IO_BUFFER_SIZE, fileA);
        size_t readB = fread(bufferB, 1, CINNAMON_FS_IO_BUFFER_SIZE, fileB);
        if (readA != readB) {
            identical = false;
            break;
        }
        if (memcmp(bufferA, bufferB, readA) != 0) {
            identical = false;
            break;
        }
        if (readA < CINNAMON_FS_IO_BUFFER_SIZE) {
            identical = !ferror(fileA) && !ferror(fileB);
            break;
        }
    }

    free(bufferA);
    free(bufferB);
    fclose(fileA);
    fclose(fileB);
    return identical;
}

bool fsCopyFile(const char* srcPath, const char* dstPath) {
    if (srcPath == NULL || dstPath == NULL) return false;

    char* parentDir = pathDupParent(dstPath);
    bool ok = fsEnsureDir(parentDir);
    free(parentDir);
    if (!ok) return false;

    FILE* src = fopen(srcPath, "rb");
    if (src == NULL) return false;
    FILE* dst = fopen(dstPath, "wb");
    if (dst == NULL) {
        fclose(src);
        return false;
    }

    uint8_t* buffer = safeMalloc(CINNAMON_FS_IO_BUFFER_SIZE);
    ok = true;
    while (ok) {
        size_t bytesRead = fread(buffer, 1, CINNAMON_FS_IO_BUFFER_SIZE, src);
        if (bytesRead > 0 && fwrite(buffer, 1, bytesRead, dst) != bytesRead) ok = false;
        if (bytesRead < CINNAMON_FS_IO_BUFFER_SIZE) {
            if (ferror(src)) ok = false;
            break;
        }
    }

    free(buffer);
    fclose(src);
    if (fclose(dst) != 0) ok = false;
    return ok;
}

bool fsCopyAtomic(const char* srcPath, const char* dstPath) {
    if (srcPath == NULL || dstPath == NULL) return false;
    char tmpPath[PREPROCESS_TEMP_PATH_MAX];
    if (!pathWithSuffix(dstPath, ".tmp", tmpPath, sizeof(tmpPath))) return false;
    remove(tmpPath);
    if (!fsCopyFile(srcPath, tmpPath)) {
        remove(tmpPath);
        return false;
    }
    if (!fsCommitTemp(tmpPath, dstPath)) {
        remove(tmpPath);
        return false;
    }
    return true;
}

bool fsCommitTemp(const char* tmpPath, const char* finalPath) {
    if (tmpPath == NULL || finalPath == NULL) return false;
#if PREPROCESS_HOST_WINDOWS
    return MoveFileExA(tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmpPath, finalPath) == 0;
#endif
}

char* fsFindFile(const char* srcDir, const char* targetName) {
    if (srcDir == NULL || targetName == NULL || targetName[0] == '\0') return NULL;
    if (!fsDirExists(srcDir)) return NULL;

#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_TEMP_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(srcDir, "*", searchPath, sizeof(searchPath))) return NULL;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return NULL;

    char* result = NULL;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char childPath[PREPROCESS_TEMP_PATH_MAX];
        if (!pathJoin(srcDir, name, childPath, sizeof(childPath))) break;
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            result = fsFindFile(childPath, targetName);
            if (result != NULL) break;
        } else if (strEqualNoCase(name, targetName)) {
            result = safeStrdup(childPath);
            break;
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return result;
#else
    DIR* dir = opendir(srcDir);
    if (dir == NULL) return NULL;

    char* result = NULL;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char* childPath = pathJoinAlloc(srcDir, name);
        if (fsDirExists(childPath)) {
            result = fsFindFile(childPath, targetName);
            free(childPath);
            if (result != NULL) break;
        } else if (strEqualNoCase(name, targetName)) {
            result = childPath;
            break;
        } else {
            free(childPath);
        }
    }

    closedir(dir);
    return result;
#endif
}

bool fsSyncTree(
    const char* srcDir, const char* dstDir, uint32_t* outUpdatedCount, uint32_t* outSkippedCount) {
    if (srcDir == NULL || dstDir == NULL) return false;
    if (!fsDirExists(srcDir)) return true;
    if (!fsEnsureDir(dstDir)) return false;
#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_SEARCH_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(srcDir, "*", searchPath, sizeof(searchPath))) return false;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char srcChild[PREPROCESS_SEARCH_PATH_MAX];
        char dstChild[PREPROCESS_SEARCH_PATH_MAX];
        if (!pathJoin(srcDir, name, srcChild, sizeof(srcChild)) ||
            !pathJoin(dstDir, name, dstChild, sizeof(dstChild))) {
            ok = false;
            break;
        }
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = fsSyncTree(srcChild, dstChild, outUpdatedCount, outSkippedCount);
        } else if (!fsFileExists(dstChild) || !fsFilesEqual(srcChild, dstChild)) {
            ok = fsCopyAtomic(srcChild, dstChild);
            if (ok && outUpdatedCount != NULL) (*outUpdatedCount)++;
        } else if (outSkippedCount != NULL) {
            (*outSkippedCount)++;
        }
        if (!ok) break;
    } while (FindNextFileA(findHandle, &findData));
    FindClose(findHandle);
    return ok;
#else
    DIR* dir = opendir(srcDir);
    if (dir == NULL) return true;
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char* srcChild = pathJoinAlloc(srcDir, name);
        char* dstChild = pathJoinAlloc(dstDir, name);
        if (fsDirExists(srcChild)) {
            ok = fsSyncTree(srcChild, dstChild, outUpdatedCount, outSkippedCount);
        } else if (!fsFileExists(dstChild) || !fsFilesEqual(srcChild, dstChild)) {
            ok = fsCopyAtomic(srcChild, dstChild);
            if (ok && outUpdatedCount != NULL) (*outUpdatedCount)++;
        } else if (outSkippedCount != NULL) {
            (*outSkippedCount)++;
        }
        free(srcChild);
        free(dstChild);
        if (!ok) break;
    }
    closedir(dir);
    return ok;
#endif
}

bool fsWriteFile(const char* path, const uint8_t* data, uint32_t size) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}
