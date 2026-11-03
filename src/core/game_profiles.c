#include "common/preprocess_common.h"
#include "game_profiles.h"

#include "utils.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PREPROCESS_GAME_PROFILE_UNKNOWN_LABEL "Unknown/Generic GameMaker game"

static bool parseBoolValue(const char* value) {
    return strEqualNoCase(value, "true") || strEqualNoCase(value, "yes") ||
        strEqualNoCase(value, "1") || strEqualNoCase(value, "on");
}

static bool parseInt32Value(const char* value, int32_t* out) {
    if (value == NULL || out == NULL) return false;
    char* end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || parsed < INT32_MIN || parsed > INT32_MAX) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    *out = (int32_t) parsed;
    return true;
}

static bool parseAtlasPageModeValue(const char* value, int32_t* out) {
    if (value == NULL || out == NULL) return false;
    if (strEqualNoCase(value, "auto")) {
        *out = 0;
        return true;
    }
    if (strEqualNoCase(value, "256")) {
        *out = 1;
        return true;
    }
    if (strEqualNoCase(value, "512")) {
        *out = 2;
        return true;
    }
    if (strEqualNoCase(value, "1024")) {
        *out = 3;
        return true;
    }
    return false;
}

static bool splitPipeField(char** cursor, char* out, size_t outSize) {
    if (cursor == NULL || *cursor == NULL || out == NULL || outSize == 0) return false;
    char* start = *cursor;
    char* pipe = strchr(start, '|');
    if (pipe != NULL) {
        *pipe = '\0';
        *cursor = pipe + 1;
    } else {
        *cursor = start + strlen(start);
    }
    pathTrim(start);
    snprintf(out, outSize, "%s", start);
    return out[0] != '\0';
}

static PreprocessGameProfile* appendProfile(PreprocessGameProfileConfig* config) {
    if (config == NULL || config->profileCount >= PREPROCESS_GAME_PROFILE_MAX_PROFILES) return NULL;
    PreprocessGameProfile* profile = &config->profiles[config->profileCount++];
    memset(profile, 0, sizeof(*profile));
    profile->minBytecode = 0;
    profile->maxBytecode = 0;
    profile->chapterMin = 1;
    profile->chapterMax = 0;
    profile->atlasPageMode = -1;
    return profile;
}

static bool appendChapterDirMarker(PreprocessGameProfile* profile, const char* value) {
    if (profile == NULL || value == NULL ||
        profile->chapterDirContainsCount >= PREPROCESS_GAME_PROFILE_MAX_CHAPTER_MARKERS)
        return false;
    snprintf(profile->chapterDirContains[profile->chapterDirContainsCount++],
        sizeof(profile->chapterDirContains[0]), "%s", value);
    return true;
}

static bool appendCompanionCopy(PreprocessGameProfile* profile, const char* value) {
    if (profile == NULL || value == NULL ||
        profile->companionCopyCount >= PREPROCESS_GAME_PROFILE_MAX_COMPANION_COPIES)
        return false;
    char buffer[PREPROCESS_PATH_MAX];
    snprintf(buffer, sizeof(buffer), "%s", value);
    char* cursor = buffer;

    PreprocessGameProfileCompanionCopy copy;
    memset(&copy, 0, sizeof(copy));
    if (!splitPipeField(&cursor, copy.source, sizeof(copy.source))) return false;
    if (!splitPipeField(&cursor, copy.destination, sizeof(copy.destination))) {
        snprintf(copy.destination, sizeof(copy.destination), "%s", copy.source);
    }

    profile->companionCopies[profile->companionCopyCount++] = copy;
    return true;
}

void initProfiles(PreprocessGameProfileConfig* config) {
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
}

static bool loadGameProfiles(const char* path, PreprocessGameProfileConfig* config) {
    if (path == NULL || config == NULL || !fsFileExists(path)) return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    PreprocessGameProfileConfig loaded;
    initProfiles(&loaded);
    PreprocessGameProfile* current = NULL;
    char line[PREPROCESS_PATH_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        pathTrim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;
        if (strcmp(line, "[profile]") == 0) {
            current = appendProfile(&loaded);
            continue;
        }
        if (current == NULL) continue;

        char* equals = strchr(line, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        char* key = line;
        char* value = equals + 1;
        pathTrim(key);
        pathTrim(value);
        pathUnquote(value);

        if (strEqualNoCase(key, "id")) {
            snprintf(current->id, sizeof(current->id), "%s", value);
        } else if (strEqualNoCase(key, "label")) {
            snprintf(current->label, sizeof(current->label), "%s", value);
        } else if (strEqualNoCase(key, "match_name")) {
            snprintf(current->matchName, sizeof(current->matchName), "%s", value);
        } else if (strEqualNoCase(key, "min_bytecode")) {
            parseInt32Value(value, &current->minBytecode);
        } else if (strEqualNoCase(key, "max_bytecode")) {
            parseInt32Value(value, &current->maxBytecode);
        } else if (strEqualNoCase(key, "chapter_data_win")) {
            snprintf(current->chapterDataWinPattern, sizeof(current->chapterDataWinPattern), "%s",
                value);
        } else if (strEqualNoCase(key, "chapter_min")) {
            parseInt32Value(value, &current->chapterMin);
        } else if (strEqualNoCase(key, "chapter_max")) {
            parseInt32Value(value, &current->chapterMax);
        } else if (strEqualNoCase(key, "chapter_dir_contains")) {
            appendChapterDirMarker(current, value);
        } else if (strEqualNoCase(key, "companion_copy") || strEqualNoCase(key, "runtime_copy") ||
            strEqualNoCase(key, "copy_runtime")) {
            appendCompanionCopy(current, value);
        } else if (strEqualNoCase(key, "chapter_parent_audio_root")) {
            current->chapterParentAudioRoot = parseBoolValue(value);
        } else if (strEqualNoCase(key, "atlas_page_mode") || strEqualNoCase(key, "atlas_pages") ||
            strEqualNoCase(key, "page_mode")) {
            parseAtlasPageModeValue(value, &current->atlasPageMode);
        }
    }

    fclose(file);
    if (!pathCopy(loaded.loadedPath, sizeof(loaded.loadedPath), path)) return false;
    *config = loaded;
    return config->profileCount > 0;
}

bool loadProfiles(const char* argv0, PreprocessGameProfileConfig* config) {
    if (config == NULL) return false;
    initProfiles(config);

    char path[PREPROCESS_SEARCH_PATH_MAX];
    char programDir[PREPROCESS_PATH_MAX];
    if (fsProgramDir(argv0, programDir, sizeof(programDir))) {
        if (pathJoin(programDir, "game_profiles.cfg", path, sizeof(path)) &&
            loadGameProfiles(path, config)) {
            return true;
        }
    }

#if defined(CINNAMON_PREPROCESSOR_SOURCE_DIR)
    if (pathJoin(CINNAMON_PREPROCESSOR_SOURCE_DIR, "game_profiles.cfg", path, sizeof(path)) &&
        loadGameProfiles(path, config)) {
        return true;
    }
#endif

    if (loadGameProfiles("game_profiles.cfg", config)) return true;
    return false;
}

static bool dataWinMatchesProfile(const DataWin* dataWin, const PreprocessGameProfile* profile) {
    if (dataWin == NULL || profile == NULL) return false;
    if (profile->matchName[0] != '\0' &&
        !strContainsNoCase(dataWin->gen8.name, profile->matchName) &&
        !strContainsNoCase(dataWin->gen8.displayName, profile->matchName)) {
        return false;
    }
    if (profile->minBytecode > 0 && (int32_t) dataWin->gen8.wadVersion < profile->minBytecode)
        return false;
    if (profile->maxBytecode > 0 && (int32_t) dataWin->gen8.wadVersion > profile->maxBytecode)
        return false;
    return true;
}

const PreprocessGameProfile* detectProfile(
    const PreprocessGameProfileConfig* config, const DataWin* dataWin) {
    if (config == NULL || dataWin == NULL) return NULL;
    for (size_t i = 0; i < config->profileCount; ++i) {
        if (dataWinMatchesProfile(dataWin, &config->profiles[i])) return &config->profiles[i];
    }
    return NULL;
}

const char* profileLabel(const PreprocessGameProfile* profile) {
    if (profile == NULL) return PREPROCESS_GAME_PROFILE_UNKNOWN_LABEL;
    if (profile->label[0] != '\0') return profile->label;
    if (profile->id[0] != '\0') return profile->id;
    return PREPROCESS_GAME_PROFILE_UNKNOWN_LABEL;
}

bool profileHasChapters(const PreprocessGameProfile* profile) {
    return profile != NULL && profile->chapterDataWinPattern[0] != '\0' &&
        profile->chapterMax >= profile->chapterMin;
}

static bool formatChapterPath(const char* pattern, int32_t chapter, char* out, size_t outSize) {
    if (pattern == NULL || out == NULL || outSize == 0) return false;
    out[0] = '\0';
    char chapterText[32];
    snprintf(chapterText, sizeof(chapterText), "%ld", (long) chapter);
    size_t writeIndex = 0;
    for (size_t readIndex = 0; pattern[readIndex] != '\0' && writeIndex + 1 < outSize;) {
        if (strncmp(pattern + readIndex, "{chapter}", 9) == 0) {
            size_t chapterLen = strlen(chapterText);
            if (writeIndex + chapterLen >= outSize) return false;
            memcpy(out + writeIndex, chapterText, chapterLen);
            writeIndex += chapterLen;
            readIndex += 9;
        } else if (strncmp(pattern + readIndex, "%ld", 3) == 0) {
            size_t chapterLen = strlen(chapterText);
            if (writeIndex + chapterLen >= outSize) return false;
            memcpy(out + writeIndex, chapterText, chapterLen);
            writeIndex += chapterLen;
            readIndex += 3;
        } else if (strncmp(pattern + readIndex, "%d", 2) == 0) {
            size_t chapterLen = strlen(chapterText);
            if (writeIndex + chapterLen >= outSize) return false;
            memcpy(out + writeIndex, chapterText, chapterLen);
            writeIndex += chapterLen;
            readIndex += 2;
        } else {
            out[writeIndex++] = pattern[readIndex++];
        }
    }
    out[writeIndex] = '\0';
    return out[0] != '\0';
}

bool resolveChapterData(const PreprocessGameProfile* profile, const char* rootDir, int32_t chapter,
    char* dataWinPath, size_t dataWinPathSize) {
    if (!profileHasChapters(profile) || rootDir == NULL || rootDir[0] == '\0' ||
        dataWinPath == NULL || dataWinPathSize == 0)
        return false;
    if (chapter < profile->chapterMin || chapter > profile->chapterMax) return false;
    char relativePath[512];
    if (!formatChapterPath(
            profile->chapterDataWinPattern, chapter, relativePath, sizeof(relativePath)))
        return false;
    char* candidate = pathJoinAlloc(rootDir, relativePath);
    bool found = fsFileExists(candidate);
    if (found) found = pathCopy(dataWinPath, dataWinPathSize, candidate);
    free(candidate);
    return found;
}

bool isChapterDir(const PreprocessGameProfile* profile, const char* path) {
    if (profile == NULL || path == NULL || path[0] == '\0' || profile->chapterDirContainsCount == 0)
        return false;
    const char* base = pathBaseName(path);
    if (base == NULL || base[0] == '\0') return false;
    for (size_t i = 0; i < profile->chapterDirContainsCount; ++i) {
        if (!strContainsNoCase(base, profile->chapterDirContains[i])) return false;
    }
    return true;
}

const PreprocessGameProfile* findChapterProfile(
    const PreprocessGameProfileConfig* config, const char* path) {
    if (config == NULL || path == NULL) return NULL;
    for (size_t i = 0; i < config->profileCount; ++i) {
        if (isChapterDir(&config->profiles[i], path)) return &config->profiles[i];
    }
    return NULL;
}

char* resolveInstallRoot(const PreprocessGameProfileConfig* config, const char* inputPath) {
    if (inputPath == NULL) return NULL;
    char* inputDir = pathDupParent(inputPath);
    if (inputDir == NULL) return NULL;
    const PreprocessGameProfile* profile = findChapterProfile(config, inputDir);
    if (profile == NULL || !profile->chapterParentAudioRoot) return inputDir;
    char* root = pathDupParent(inputDir);
    free(inputDir);
    return root;
}
