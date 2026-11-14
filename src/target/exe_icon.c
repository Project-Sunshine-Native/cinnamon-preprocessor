#include "exe_icon.h"
#include "common/byteio.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "core/game_profiles.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PREPROCESS_HOST_WINDOWS
#include <windows.h>
#else
#include <dirent.h>
#endif

typedef struct {
    uint32_t address;
    uint32_t size;
    uint32_t offset;
} IconSection;

typedef struct {
    uint32_t offset;
    uint32_t size;
} IconImage;

typedef struct {
    FILE* file;
    uint32_t fileSize;
    uint32_t headerSize;
    IconSection* sections;
    uint16_t sectionCount;
    uint32_t resourceOffset;
    uint32_t resourceSize;
} IconExecutable;

typedef struct {
    char path[PREPROCESS_SEARCH_PATH_MAX];
    int rank;
    uint32_t matches;
} IconCandidate;

static bool readExe(const IconExecutable* exe, uint32_t offset, void* bytes, uint32_t size) {
    return offset <= exe->fileSize && size <= exe->fileSize - offset &&
        fseek(exe->file, (long)offset, SEEK_SET) == 0 && fread(bytes, 1, size, exe->file) == size;
}

static bool mapAddress(const IconExecutable* exe, uint32_t address, uint32_t size, uint32_t* offset) {
    if (address < exe->headerSize && size <= exe->headerSize - address &&
        address <= exe->fileSize && size <= exe->fileSize - address) {
        *offset = address;
        return true;
    }
    for (uint16_t i = 0; i < exe->sectionCount; ++i) {
        const IconSection* section = &exe->sections[i];
        if (address < section->address) continue;
        uint32_t relative = address - section->address;
        if (relative > section->size || size > section->size - relative) continue;
        uint64_t position = (uint64_t)section->offset + relative;
        if (position > exe->fileSize || size > exe->fileSize - position) continue;
        *offset = (uint32_t)position;
        return true;
    }
    return false;
}

static bool readHeaders(IconExecutable* exe) {
    uint8_t dos[64];
    uint8_t coff[24];
    if (!readExe(exe, 0, dos, sizeof(dos)) || memcmp(dos, "MZ", 2) != 0) return false;
    uint32_t peOffset = readLe32(dos + 60);
    if (!readExe(exe, peOffset, coff, sizeof(coff)) || memcmp(coff, "PE\0\0", 4) != 0) return false;
    exe->sectionCount = readLe16(coff + 6);
    uint32_t optionalSize = readLe16(coff + 20);
    uint32_t optionalOffset = peOffset + sizeof(coff);
    uint8_t optional[136];
    if (optionalSize < 2 || !readExe(exe, optionalOffset, optional, 2)) return false;
    uint16_t magic = readLe16(optional);
    uint32_t directories;
    if (magic == 0x10b) directories = 96;
    else if (magic == 0x20b) directories = 112;
    else return false;
    uint32_t required = directories + 24;
    if (optionalSize < required || !readExe(exe, optionalOffset, optional, required) ||
        readLe32(optional + directories - 4) < 3)
        return false;
    uint32_t resourceAddress = readLe32(optional + directories + 16);
    exe->resourceSize = readLe32(optional + directories + 20);
    exe->headerSize = readLe32(optional + 60);
    if (exe->sectionCount == 0 || resourceAddress == 0 || exe->resourceSize < 16 ||
        optionalSize > exe->fileSize - optionalOffset)
        return false;
    uint32_t sectionOffset = optionalOffset + optionalSize;
    if ((uint32_t)exe->sectionCount > (exe->fileSize - sectionOffset) / 40) return false;
    exe->sections = calloc(exe->sectionCount, sizeof(*exe->sections));
    if (exe->sections == NULL) return false;
    for (uint16_t i = 0; i < exe->sectionCount; ++i) {
        uint8_t section[40];
        if (!readExe(exe, sectionOffset + (uint32_t)i * 40, section, sizeof(section))) return false;
        exe->sections[i].address = readLe32(section + 12);
        exe->sections[i].size = readLe32(section + 16);
        exe->sections[i].offset = readLe32(section + 20);
    }
    return mapAddress(exe, resourceAddress, exe->resourceSize, &exe->resourceOffset);
}

static bool readResource(const IconExecutable* exe, uint32_t relative, void* bytes, uint32_t size) {
    return relative <= exe->resourceSize && size <= exe->resourceSize - relative &&
        readExe(exe, exe->resourceOffset + relative, bytes, size);
}

static bool findResource(const IconExecutable* exe, uint32_t directory, uint32_t id,
    uint32_t* target, uint32_t* foundId) {
    uint8_t header[16];
    if (!readResource(exe, directory, header, sizeof(header))) return false;
    uint32_t count = (uint32_t)readLe16(header + 12) + readLe16(header + 14);
    if (count > (exe->resourceSize - directory - sizeof(header)) / 8) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t entry[8];
        if (!readResource(exe, directory + 16 + i * 8, entry, sizeof(entry))) return false;
        uint32_t name = readLe32(entry);
        if (id != UINT32_MAX && name != id) continue;
        *target = readLe32(entry + 4);
        if (foundId != NULL) *foundId = name;
        return true;
    }
    return false;
}

static bool findDirectory(const IconExecutable* exe, uint32_t parent, uint32_t id, uint32_t* directory) {
    uint32_t entry;
    if (!findResource(exe, parent, id, &entry, NULL) || !(entry & 0x80000000u)) return false;
    *directory = entry & 0x7fffffffu;
    return true;
}

static bool findPayload(const IconExecutable* exe, uint32_t directory, uint32_t preferredLanguage,
    uint32_t* language, uint32_t* offset, uint32_t* size) {
    uint32_t entry;
    bool found = preferredLanguage != UINT32_MAX &&
        findResource(exe, directory, preferredLanguage, &entry, language);
    if (!found) found = findResource(exe, directory, 0, &entry, language);
    if (!found) found = findResource(exe, directory, 1033, &entry, language);
    if (!found) found = findResource(exe, directory, UINT32_MAX, &entry, language);
    if (!found || (entry & 0x80000000u)) return false;
    uint8_t data[16];
    if (!readResource(exe, entry, data, sizeof(data))) return false;
    *size = readLe32(data + 4);
    return *size > 0 && *size <= exe->resourceSize && mapAddress(exe, readLe32(data), *size, offset);
}

static uint8_t* readIcon(IconExecutable* exe, uint32_t* outSize) {
    uint32_t groups, icons, group, groupOffset, groupSize, language;
    if (!readHeaders(exe) || !findDirectory(exe, 0, 14, &groups) ||
        !findDirectory(exe, 0, 3, &icons))
        return NULL;
    if (!findDirectory(exe, groups, 1, &group) &&
        !findDirectory(exe, groups, UINT32_MAX, &group))
        return NULL;
    if (!findPayload(exe, group, UINT32_MAX, &language, &groupOffset, &groupSize)) return NULL;
    uint8_t header[6];
    if (groupSize < sizeof(header) || !readExe(exe, groupOffset, header, sizeof(header)) ||
        readLe16(header) != 0 || readLe16(header + 2) != 1)
        return NULL;
    uint32_t count = readLe16(header + 4);
    if (count == 0 || count > (groupSize - 6) / 14) return NULL;
    uint32_t directorySize = 6 + count * 16;
    uint8_t* entries = malloc(count * 14);
    IconImage* images = calloc(count, sizeof(*images));
    uint8_t* ico = NULL;
    if (entries == NULL || images == NULL || !readExe(exe, groupOffset + 6, entries, count * 14)) goto done;
    uint32_t totalSize = directorySize;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t imageDirectory;
        if (!findDirectory(exe, icons, readLe16(entries + i * 14 + 12), &imageDirectory) ||
            !findPayload(exe, imageDirectory, language, NULL, &images[i].offset, &images[i].size) ||
            images[i].size != readLe32(entries + i * 14 + 8) ||
            images[i].size > UINT32_MAX - totalSize)
            goto done;
        totalSize += images[i].size;
    }
    ico = malloc(totalSize);
    if (ico == NULL) goto done;
    memcpy(ico, header, sizeof(header));
    uint32_t position = directorySize;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* entry = ico + 6 + i * 16;
        memcpy(entry, entries + i * 14, 12);
        writeLe32(entry + 12, position);
        if (!readExe(exe, images[i].offset, ico + position, images[i].size)) {
            free(ico);
            ico = NULL;
            goto done;
        }
        position += images[i].size;
    }
    *outSize = totalSize;
done:
    free(images);
    free(entries);
    return ico;
}

static bool matchesExeName(const char* executable, const char* reference) {
    if (reference == NULL || reference[0] == '\0') return false;
    const char* base = pathBaseName(reference);
    size_t length = strlen(base);
    if (strEndsWithNoCase(base, ".exe")) length -= 4;
    size_t exeLength = strlen(executable);
    if (exeLength < 4 || exeLength - 4 != length) return false;
    char stem[PREPROCESS_PATH_MAX];
    if (length >= sizeof(stem)) return false;
    memcpy(stem, executable, length);
    stem[length] = '\0';
    return strEqualNoCase(stem, base) ||
        (length + 4 == strlen(base) && strEqualNoCase(executable, base));
}

static void considerExecutable(const char* directory, const char* name,
    const DataWin* dataWin, int rootIndex, IconCandidate* candidate) {
    if (!strEndsWithNoCase(name, ".exe")) return;
    char path[PREPROCESS_SEARCH_PATH_MAX];
    if (!pathJoin(directory, name, path, sizeof(path)) || !fsFileExists(path)) return;
    const char* references[] = {dataWin->gen8.fileName, dataWin->gen8.name,
        dataWin->gen8.displayName, pathBaseName(directory)};
    int rank = 100;
    for (int i = 0; i < 4; ++i) {
        if (matchesExeName(name, references[i])) {
            rank = i * 2 + rootIndex;
            break;
        }
    }
    if (rank > candidate->rank) return;
    if (rank < candidate->rank) {
        pathCopy(candidate->path, sizeof(candidate->path), path);
        candidate->rank = rank;
        candidate->matches = 1;
    } else ++candidate->matches;
}

static void findExecutable(const char* directory, const DataWin* dataWin,
    int rootIndex, IconCandidate* candidate) {
    if (directory == NULL || !fsDirExists(directory)) return;
#if PREPROCESS_HOST_WINDOWS
    char search[PREPROCESS_SEARCH_PATH_MAX];
    if (!pathJoin(directory, "*", search, sizeof(search))) return;
    WIN32_FIND_DATAA entry;
    HANDLE handle = FindFirstFileA(search, &entry);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            considerExecutable(directory, entry.cFileName, dataWin, rootIndex, candidate);
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
#else
    DIR* dir = opendir(directory);
    if (dir == NULL) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL)
        considerExecutable(directory, entry->d_name, dataWin, rootIndex, candidate);
    closedir(dir);
#endif
}

bool extractGameIcon(const PreprocessRunConfig* options, const DataWin* dataWin) {
    char* inputDir = pathDupParent(options->inputPath);
    char* installRoot = resolveInstallRoot(options->gameProfiles, options->inputPath);
    IconCandidate candidate = {.rank = INT_MAX};
    findExecutable(inputDir, dataWin, 0, &candidate);
    if (installRoot != NULL && (inputDir == NULL || !pathsEqual(inputDir, installRoot)))
        findExecutable(installRoot, dataWin, 1, &candidate);
    free(installRoot);
    free(inputDir);
    if (candidate.matches == 0) {
        fprintf(stderr, "cinnamon-preprocessor: no game EXE found; skipping icon extraction\n");
        return true;
    }
    if (candidate.matches > 1) {
        fprintf(stderr, "cinnamon-preprocessor: multiple EXEs match; skipping ambiguous game icon\n");
        return true;
    }
    IconExecutable exe = {0};
    exe.file = fopen(candidate.path, "rb");
    uint8_t* ico = NULL;
    uint32_t size = 0;
    if (exe.file != NULL) {
        if (fseek(exe.file, 0, SEEK_END) == 0) {
            long length = ftell(exe.file);
            if (length > 0 && (uint64_t)length <= UINT32_MAX) {
                exe.fileSize = (uint32_t)length;
                ico = readIcon(&exe, &size);
            }
        }
        fclose(exe.file);
    }
    free(exe.sections);
    if (ico == NULL) {
        fprintf(stderr, "cinnamon-preprocessor: no readable PE icon in %s; skipping\n", candidate.path);
        return true;
    }
    char output[PREPROCESS_SEARCH_PATH_MAX];
    char temporary[PREPROCESS_TEMP_PATH_MAX];
    bool ok = pathJoin(options->outputDir, "icon.ico", output, sizeof(output)) &&
        pathWithSuffix(output, ".tmp", temporary, sizeof(temporary));
    if (ok) {
        ok = fsWriteFile(temporary, ico, size) && fsCommitTemp(temporary, output);
        if (!ok) remove(temporary);
    }
    free(ico);
    if (ok) fprintf(stderr, "cinnamon-preprocessor: extracted %s -> %s\n", candidate.path, output);
    else fprintf(stderr, "Failed to write extracted game icon to %s\n", options->outputDir);
    return ok;
}
