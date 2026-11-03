#include "audio_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

typedef struct {
    char name[256];
    char path[PREPROCESS_TEMP_PATH_MAX];
} ExternalAudioFileIndexEntry;

static ExternalAudioFileIndexEntry* gExternalAudioFileIndex = NULL;
static char gExternalAudioFileIndexRoot[PREPROCESS_PATH_MAX] = "";
static const char* const AUDIO_EXTENSIONS[] = {".ogg", ".wav", ".wdsp", ".mp3"};

static bool hasAudioExtension(const char* path) {
    preprocessRepeat(sizeof(AUDIO_EXTENSIONS) / sizeof(AUDIO_EXTENSIONS[0]), i) {
        if (strEndsWithNoCase(path, AUDIO_EXTENSIONS[i])) return true;
    }
    return false;
}

static char* resolveRelativeSound(
    const char* const* searchRoots, size_t searchRootCount, const char* relativePath) {
    if (searchRoots == NULL || !strNotEmpty(relativePath)) return NULL;
    preprocessRepeat(searchRootCount, rootIndex) {
        char* candidate = pathJoinAlloc(searchRoots[rootIndex], relativePath);
        if (fsFileExists(candidate)) return candidate;
        free(candidate);
    }
    if (hasAudioExtension(pathBaseName(relativePath))) return NULL;
    preprocessRepeat(sizeof(AUDIO_EXTENSIONS) / sizeof(AUDIO_EXTENSIONS[0]), extIndex) {
        char withExt[PREPROCESS_PATH_MAX];
        if (!pathWithSuffix(relativePath, AUDIO_EXTENSIONS[extIndex], withExt, sizeof(withExt))) {
            continue;
        }
        preprocessRepeat(searchRootCount, rootIndex) {
            char* candidate = pathJoinAlloc(searchRoots[rootIndex], withExt);
            if (fsFileExists(candidate)) return candidate;
            free(candidate);
        }
    }
    return NULL;
}

void audioClearIndex(void) {
    arrfree(gExternalAudioFileIndex);
    gExternalAudioFileIndex = NULL;
    gExternalAudioFileIndexRoot[0] = '\0';
}

static bool indexAudioFile(const char* path) {
    const char* name = pathBaseName(path);
    if (name == NULL || !hasAudioExtension(name)) return true;

    ExternalAudioFileIndexEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (!pathCopy(entry.name, sizeof(entry.name), name) ||
        !pathCopy(entry.path, sizeof(entry.path), path)) {
        return false;
    }
    arrput(gExternalAudioFileIndex, entry);
    return true;
}

static bool indexExternalAudioDir(const char* srcDir) {
    if (srcDir == NULL || !fsDirExists(srcDir)) return true;

#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_TEMP_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(srcDir, "*", searchPath, sizeof(searchPath))) return false;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char childPath[PREPROCESS_TEMP_PATH_MAX];
        if (!pathJoin(srcDir, name, childPath, sizeof(childPath))) {
            ok = false;
            break;
        }
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!indexExternalAudioDir(childPath)) {
                ok = false;
                break;
            }
        } else if (!indexAudioFile(childPath)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return ok;
#else
    DIR* dir = opendir(srcDir);
    if (dir == NULL) return false;

    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char* childPath = pathJoinAlloc(srcDir, name);
        if (fsDirExists(childPath)) {
            ok = indexExternalAudioDir(childPath);
        } else {
            ok = indexAudioFile(childPath);
        }
        free(childPath);
        if (!ok) break;
    }

    closedir(dir);
    return ok;
#endif
}

static bool ensureAudioIndex(const char* inputDir) {
    if (!strNotEmpty(inputDir)) return false;
    if (gExternalAudioFileIndexRoot[0] != '\0' &&
        strcmp(gExternalAudioFileIndexRoot, inputDir) == 0)
        return true;

    audioClearIndex();
    if (!pathCopy(gExternalAudioFileIndexRoot, sizeof(gExternalAudioFileIndexRoot), inputDir)) {
        return false;
    }
    bool ok = indexExternalAudioDir(inputDir);
    fprintf(stderr, "cinnamon-preprocessor: indexed %lu external audio files under %s\n",
        (unsigned long) arrlen(gExternalAudioFileIndex), inputDir);
    return ok;
}

static char* findIndexedSound(const char* inputDir, const char* fileName) {
    if (!strNotEmpty(inputDir) || !strNotEmpty(fileName)) return NULL;
    if (!ensureAudioIndex(inputDir)) return NULL;

    preprocessRepeat(arrlen(gExternalAudioFileIndex), i) {
        if (strEqualNoCase(gExternalAudioFileIndex[i].name, fileName)) {
            return safeStrdup(gExternalAudioFileIndex[i].path);
        }
    }

    if (hasAudioExtension(fileName)) return NULL;
    preprocessRepeat(sizeof(AUDIO_EXTENSIONS) / sizeof(AUDIO_EXTENSIONS[0]), extIndex) {
        char targetName[PREPROCESS_PATH_MAX];
        if (!pathWithSuffix(fileName, AUDIO_EXTENSIONS[extIndex], targetName, sizeof(targetName))) {
            continue;
        }
        preprocessRepeat(arrlen(gExternalAudioFileIndex), i) {
            if (strEqualNoCase(gExternalAudioFileIndex[i].name, targetName)) {
                return safeStrdup(gExternalAudioFileIndex[i].path);
            }
        }
    }
    return NULL;
}

static char* findExternalSound(const char* srcDir, const char* fileName) {
    if (!strNotEmpty(srcDir) || !strNotEmpty(fileName)) return NULL;
    if (hasAudioExtension(fileName)) return fsFindFile(srcDir, fileName);

    preprocessRepeat(sizeof(AUDIO_EXTENSIONS) / sizeof(AUDIO_EXTENSIONS[0]), i) {
        char targetName[PREPROCESS_PATH_MAX];
        if (!pathWithSuffix(fileName, AUDIO_EXTENSIONS[i], targetName, sizeof(targetName))) {
            continue;
        }
        char* resolved = fsFindFile(srcDir, targetName);
        if (resolved != NULL) return resolved;
    }
    return NULL;
}

char* audioFindSource(const PreprocessRunConfig* options, const Sound* sound) {
    if (options == NULL || sound == NULL) return NULL;
    char* inputDir = pathDupParent(options->inputPath);
    char* installRoot = resolveInstallRoot(options->gameProfiles, options->inputPath);
    const char* searchRoots[2] = {inputDir, NULL};
    size_t searchRootCount = inputDir != NULL ? 1u : 0u;
    if (installRoot != NULL && (inputDir == NULL || strcmp(installRoot, inputDir) != 0)) {
        searchRoots[searchRootCount++] = installRoot;
    }
    const char* candidates[2] = {sound->file, sound->name};
    char baseName[2][512];

    preprocessRepeat(2, candidateIndex) {
        const char* base = candidates[candidateIndex];
        if (!strNotEmpty(base)) continue;

        char* resolved = resolveRelativeSound(searchRoots, searchRootCount, base);
        if (resolved != NULL) {
            free(installRoot);
            free(inputDir);
            return resolved;
        }

        const char* fileName = pathBaseName(base);
        if (fileName != base && strNotEmpty(fileName)) {
            if (!pathCopy(baseName[candidateIndex], sizeof(baseName[candidateIndex]), fileName)) {
                continue;
            }
            resolved = resolveRelativeSound(searchRoots, searchRootCount, baseName[candidateIndex]);
            if (resolved != NULL) {
                free(installRoot);
                free(inputDir);
                return resolved;
            }
        }

        const char* fileNameForSearch = pathBaseName(base);
        preprocessRepeat(searchRootCount, rootIndex) {
            resolved = findIndexedSound(searchRoots[rootIndex], fileNameForSearch);
            if (resolved == NULL)
                resolved = findExternalSound(searchRoots[rootIndex], fileNameForSearch);
            if (resolved != NULL) break;
        }
        if (resolved != NULL) {
            free(installRoot);
            free(inputDir);
            return resolved;
        }
    }

    free(installRoot);
    free(inputDir);
    return NULL;
}

static char* resolveAudioGroupPath(
    const PreprocessRunConfig* options, const DataWin* dataWin, int32_t groupIndex) {
    if (options == NULL || options->inputPath[0] == '\0' || groupIndex <= 0) return NULL;

    char fileName[PREPROCESS_PATH_MAX];
    if (dataWin != NULL && dataWin->agrp.audioGroups != NULL &&
        (uint32_t) groupIndex < dataWin->agrp.count) {
        const AudioGroup* group = &dataWin->agrp.audioGroups[groupIndex];
        if (strNotEmpty(group->path)) {
            if (!pathCopy(fileName, sizeof(fileName), group->path)) return NULL;
        } else {
            snprintf(fileName, sizeof(fileName), "audiogroup%ld.dat", (long) groupIndex);
        }
    } else {
        snprintf(fileName, sizeof(fileName), "audiogroup%ld.dat", (long) groupIndex);
    }

    char* inputDir = pathDupParent(options->inputPath);
    char* candidate = pathJoinAlloc(inputDir, fileName);
    if (fsFileExists(candidate)) {
        free(inputDir);
        return candidate;
    }
    free(candidate);

    char* resolved = fsFindFile(inputDir, pathBaseName(fileName));
    free(inputDir);
    return resolved;
}

static DataWin* getAudioGroupData(ExternalAudioGroupCacheEntry** inOutCache,
    const PreprocessRunConfig* options, const DataWin* mainDataWin, int32_t groupIndex) {
    if (inOutCache == NULL || options == NULL || groupIndex <= 0) return NULL;

    preprocessRepeat(arrlen(*inOutCache), i) {
        if ((*inOutCache)[i].groupIndex == groupIndex) {
            return (*inOutCache)[i].dataWin;
        }
    }

    char* groupPath = resolveAudioGroupPath(options, mainDataWin, groupIndex);
    if (groupPath == NULL) return NULL;

    DataWin* groupDataWin = DataWin_parse(groupPath,
        (DataWinParserOptions){
            .parseAudo = true,
        });
    if (groupDataWin == NULL) {
        fprintf(stderr, "Failed to parse %s for audio group %ld\n", groupPath, (long) groupIndex);
    }
    if (strlen(groupPath) >= sizeof(((ExternalAudioGroupCacheEntry*) 0)->path)) {
        DataWin_free(groupDataWin);
        free(groupPath);
        return NULL;
    }

    arrput(*inOutCache,
        ((ExternalAudioGroupCacheEntry){
            .groupIndex = groupIndex,
            .dataWin = groupDataWin,
            .usedBySoundCount = 0,
            .loggedSummary = false,
        }));
    ExternalAudioGroupCacheEntry* entry = &(*inOutCache)[arrlen(*inOutCache) - 1];
    pathCopy(entry->path, sizeof(entry->path), groupPath);
    if (groupDataWin != NULL) {
        fprintf(stderr, "Loaded external audio group %ld from %s (AUDO entries=%u)\n",
            (long) groupIndex, groupPath, groupDataWin->audo.count);
    }
    free(groupPath);
    return groupDataWin;
}

void audioFreeGroupCache(ExternalAudioGroupCacheEntry* cache) {
    preprocessRepeat(arrlen(cache), i) {
        if (cache[i].dataWin != NULL && cache[i].usedBySoundCount > 0 && !cache[i].loggedSummary) {
            fprintf(stderr, "External audio group %ld supplied %u sound bank entr%s%s%s\n",
                (long) cache[i].groupIndex, cache[i].usedBySoundCount,
                cache[i].usedBySoundCount == 1u ? "y" : "ies",
                cache[i].path[0] != '\0' ? " from " : "",
                cache[i].path[0] != '\0' ? cache[i].path : "");
        }
        if (cache[i].dataWin != NULL) DataWin_free(cache[i].dataWin);
    }
    arrfree(cache);
}

uint8_t* audioReadSource(const PreprocessRunConfig* options, DataWin* mainDataWin,
    const Sound* sound, ExternalAudioGroupCacheEntry** inOutGroupCache, uint32_t* outSize) {
    if (outSize != NULL) *outSize = 0;
    if (options == NULL || mainDataWin == NULL || sound == NULL) return NULL;
    if (sound->audioFile < 0) return NULL;

    DataWin* sourceDataWin = mainDataWin;
    const char* sourcePath = options->inputPath;
    if (sound->audioGroup > 0) {
        sourceDataWin = getAudioGroupData(inOutGroupCache, options, mainDataWin, sound->audioGroup);
        sourcePath = NULL;
        if (sourceDataWin != NULL && inOutGroupCache != NULL) {
            preprocessRepeat(arrlen(*inOutGroupCache), i) {
                if ((*inOutGroupCache)[i].groupIndex != sound->audioGroup) continue;
                (*inOutGroupCache)[i].usedBySoundCount++;
                if ((*inOutGroupCache)[i].path[0] != '\0') sourcePath = (*inOutGroupCache)[i].path;
                break;
            }
        }
    }
    if (sourceDataWin == NULL) return NULL;
    if ((uint32_t) sound->audioFile >= sourceDataWin->audo.count) return NULL;

    AudioEntry* entry = &sourceDataWin->audo.entries[sound->audioFile];
    if (entry->dataSize == 0) return NULL;
    uint8_t* copy = NULL;
    if (entry->data != NULL) {
        copy = safeMalloc(entry->dataSize);
        memcpy(copy, entry->data, entry->dataSize);
    } else if (sourcePath != NULL) {
        copy = fsReadSlice(sourcePath, entry->dataOffset, entry->dataSize);
    }
    if (copy == NULL) return NULL;
    if (outSize != NULL) *outSize = entry->dataSize;
    return copy;
}
