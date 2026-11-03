#include "common/preprocess_common.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "target/companion_files.h"

#include "data_win.h"
#include "json_writer.h"
#include "utils.h"
#include "core/game_profiles.h"
#include "core/preprocess_options.h"
#include "preprocess_target.h"

#include <errno.h>
#include <ctype.h>
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

static bool wildcardMatch(const char* pattern, const char* value) {
    if (pattern == NULL || value == NULL) return false;
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (*pattern == '\0') return true;
            while (*value != '\0') {
                if (wildcardMatch(pattern, value)) return true;
                value++;
            }
            return false;
        }
        if (*value == '\0') return false;
        if (*pattern != '?') {
            char a = *pattern;
            char b = *value;
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) return false;
        }
        pattern++;
        value++;
    }
    return *value == '\0';
}

static bool hasWildcard(const char* value) {
    return value != NULL && (strchr(value, '*') != NULL || strchr(value, '?') != NULL);
}

static bool splitPattern(const char* relativePattern, char* outDir, size_t outDirSize,
    char* outPattern, size_t outPatternSize) {
    if (relativePattern == NULL || outDir == NULL || outPattern == NULL || outDirSize == 0 ||
        outPatternSize == 0)
        return false;
    const char* slash = strrchr(relativePattern, '/');
    const char* backslash = strrchr(relativePattern, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) {
        if (!pathCopy(outDir, outDirSize, ".") ||
            !pathCopy(outPattern, outPatternSize, relativePattern))
            return false;
    } else {
        size_t dirLen = (size_t) (slash - relativePattern);
        if (dirLen >= outDirSize) return false;
        memcpy(outDir, relativePattern, dirLen);
        outDir[dirLen] = '\0';
        if (!pathCopy(outPattern, outPatternSize, slash + 1)) return false;
    }
    return outPattern[0] != '\0';
}

static bool normalizeCompanionPath(const char* value, char* out, size_t outSize) {
    if (value == NULL || out == NULL || outSize == 0) return false;
    size_t len = strlen(value);
    if (len == 0 || len + 1 > outSize) return false;
    if (value[0] == '/' || value[0] == '\\' || pathIsDriveRoot(value) || strchr(value, ':') != NULL)
        return false;

    size_t writeIndex = 0;
    size_t segmentStart = 0;
    for (size_t i = 0; i <= len; ++i) {
        char c = value[i];
        bool atSeparator = c == '/' || c == '\\' || c == '\0';
        if (atSeparator) {
            size_t segmentLen = i - segmentStart;
            if (segmentLen == 2 && value[segmentStart] == '.' && value[segmentStart + 1u] == '.')
                return false;
            segmentStart = i + 1u;
            if (c == '\0') break;
            c = '/';
        }
        out[writeIndex++] = c;
        if (writeIndex >= outSize) return false;
    }
    out[writeIndex] = '\0';
    return out[0] != '\0';
}

static bool copyMatchingFiles(const char* srcDir, const char* dstDir, const char* pattern,
    uint32_t* outUpdatedCount, uint32_t* outSkippedCount) {
    if (srcDir == NULL || dstDir == NULL || pattern == NULL) return false;
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
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (!wildcardMatch(pattern, name)) continue;
        char srcChild[PREPROCESS_SEARCH_PATH_MAX];
        char dstChild[PREPROCESS_SEARCH_PATH_MAX];
        if (!pathJoin(srcDir, name, srcChild, sizeof(srcChild)) ||
            !pathJoin(dstDir, name, dstChild, sizeof(dstChild))) {
            ok = false;
            break;
        }
        if (!fsFileExists(dstChild) || !fsFilesEqual(srcChild, dstChild)) {
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
        if (!fsDirExists(srcChild) && wildcardMatch(pattern, name)) {
            char* dstChild = pathJoinAlloc(dstDir, name);
            if (!fsFileExists(dstChild) || !fsFilesEqual(srcChild, dstChild)) {
                ok = fsCopyAtomic(srcChild, dstChild);
                if (ok && outUpdatedCount != NULL) (*outUpdatedCount)++;
            } else if (outSkippedCount != NULL) {
                (*outSkippedCount)++;
            }
            free(dstChild);
        }
        free(srcChild);
        if (!ok) break;
    }
    closedir(dir);
    return ok;
#endif
}

static bool copyCompanionIfPresent(const char* sourcePath, const char* outputDir,
    const char* fileName, uint32_t* updated, uint32_t* skipped) {
    if (sourcePath == NULL || outputDir == NULL || fileName == NULL) return false;
    if (!fsFileExists(sourcePath)) return true;

    char* destPath = pathJoinAlloc(outputDir, fileName);
    if (destPath == NULL) return false;

    bool ok = true;
    if (!fsFileExists(destPath) || !fsFilesEqual(sourcePath, destPath)) {
        ok = fsCopyAtomic(sourcePath, destPath);
        if (ok && updated != NULL) (*updated)++;
    } else if (skipped != NULL) {
        (*skipped)++;
    }
    if (!ok)
        fprintf(stderr, "Failed to copy runtime companion file %s -> %s\n", sourcePath, destPath);
    free(destPath);
    return ok;
}

/* Values written to cinnamon_settings.json; these are part of GameMaker's OS enum. */
enum {
    PREPROCESS_RUNNER_OS_WINDOWS = 0,
    PREPROCESS_RUNNER_OS_LINUX = 8,
};

static int32_t getRunnerOsType(const char* inputPath) {
    const char* name = pathBaseName(inputPath);
    return name != NULL && strEqualNoCase(name, "game.unx") ? PREPROCESS_RUNNER_OS_LINUX
                                                            : PREPROCESS_RUNNER_OS_WINDOWS;
}

static void trimTrailingSeparators(char* path) {
    if (path == NULL) return;
    size_t len = strlen(path);
    while (len > 0u && (path[len - 1u] == '/' || path[len - 1u] == '\\')) {
        if (pathIsDriveRoot(path) && len <= 3u) break;
        path[--len] = '\0';
    }
}

static void normalizeSeparators(char* path) {
    if (path == NULL) return;
    for (char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\') *cursor = '/';
    }
}

static char* dupWiiAppRoot(const char* outputDir) {
    if (!strNotEmpty(outputDir)) return NULL;

    char* current = safeStrdup(outputDir);
    trimTrailingSeparators(current);

    for (uint32_t depth = 0u; depth < 8u; ++depth) {
        const char* base = pathBaseName(current);
        if (strEqualNoCase(base, "cinnamon")) {
            char* parent = pathDupParent(current);
            bool parentIsApps = parent != NULL && strEqualNoCase(pathBaseName(parent), "apps");
            free(parent);
            if (parentIsApps) return current;
        }

        char* parent = pathDupParent(current);
        if (!strNotEmpty(parent) || strcmp(parent, current) == 0 || strcmp(parent, ".") == 0) {
            free(parent);
            break;
        }
        free(current);
        current = parent;
        trimTrailingSeparators(current);
    }

    free(current);
    return safeStrdup(outputDir);
}

static char* dupWiiAppsRoot(const char* wiiAppRootDir) {
    char* appsDir = pathDupParent(wiiAppRootDir);
    if (!strNotEmpty(appsDir)) {
        free(appsDir);
        return NULL;
    }
    trimTrailingSeparators(appsDir);
    normalizeSeparators(appsDir);
    return appsDir;
}

static void makeWiiGameDir(
    const PreprocessRunConfig* options, const char* wiiAppRootDir, char* out, size_t outSize) {
    if (out == NULL || outSize == 0u) return;
    out[0] = '\0';
    const char* fallback = pathBaseName(wiiAppRootDir);
    if (!strNotEmpty(fallback)) fallback = "cinnamon";

    char outputDir[PREPROCESS_SEARCH_PATH_MAX];
    const char* configuredOutput =
        options != NULL && options->outputDir[0] != '\0' ? options->outputDir : wiiAppRootDir;
    if (!pathCopy(outputDir, sizeof(outputDir), configuredOutput)) return;
    trimTrailingSeparators(outputDir);
    normalizeSeparators(outputDir);

    char* appsDir = dupWiiAppsRoot(wiiAppRootDir);
    if (appsDir != NULL) {
        size_t appsLen = strlen(appsDir);
        if (strncmp(outputDir, appsDir, appsLen) == 0 &&
            (outputDir[appsLen] == '/' || outputDir[appsLen] == '\0')) {
            const char* relative = outputDir + appsLen;
            while (*relative == '/') ++relative;
            if (relative[0] != '\0') {
                if (pathCopy(out, outSize, relative)) {
                    free(appsDir);
                    return;
                }
            }
        }
    }

    pathCopy(out, outSize, fallback);
    free(appsDir);
}

static bool writeWiiSettings(const PreprocessRunConfig* options) {
    if (options == NULL || !targetInfo(options->user.target)->appRootCompanions) return true;

    char* wiiAppRootDir = dupWiiAppRoot(options->outputDir);
    if (wiiAppRootDir == NULL) return false;
    bool ok = fsEnsureDir(wiiAppRootDir);

    char* settingsPath = ok ? pathJoinAlloc(wiiAppRootDir, "cinnamon_settings.json") : NULL;
    if (settingsPath != NULL) {
        int32_t osType = getRunnerOsType(options->inputPath);
        char gameDirectory[512];
        makeWiiGameDir(options, wiiAppRootDir, gameDirectory, sizeof(gameDirectory));
        JsonWriter writer = JsonWriter_create();
        JsonWriter_beginObject(&writer);
        JsonWriter_key(&writer, "runner");
        JsonWriter_beginObject(&writer);
        JsonWriter_propertyInt(&writer, "p240Mode", 0);
        JsonWriter_propertyInt(&writer, "aspectRatio", 0);
        JsonWriter_propertyBool(&writer, "debugStats", false);
        JsonWriter_propertyBool(&writer, "debugLog", false);
        JsonWriter_propertyBool(&writer, "gamepadApi", true);
        JsonWriter_propertyBool(&writer, "cacheAtlas", true);
        JsonWriter_propertyInt(&writer, "atlasTextureCacheMb", 0);
        JsonWriter_propertyString(&writer, "gameDirectory", gameDirectory);
        JsonWriter_propertyInt(&writer, "osType", osType);
        JsonWriter_endObject(&writer);
        JsonWriter_endObject(&writer);
        ok = fsWriteFile(settingsPath, (const uint8_t*) JsonWriter_getOutput(&writer),
            (uint32_t) JsonWriter_getLength(&writer));
        JsonWriter_free(&writer);
        if (ok) {
            fprintf(stderr, "%s runner settings: os_type=%d path=%s\n",
                targetName(options->user.target), osType, settingsPath);
        } else {
            fprintf(stderr, "Failed to write Wii runner settings to %s\n", settingsPath);
        }
    } else {
        ok = false;
    }

    free(settingsPath);
    free(wiiAppRootDir);
    return ok;
}

static bool copyRuntimeFile(
    const char* srcPath, const char* dstPath, uint32_t* updated, uint32_t* skipped) {
    if (srcPath == NULL || dstPath == NULL || pathsEqual(srcPath, dstPath)) return true;
    if (!fsFileExists(dstPath) || !fsFilesEqual(srcPath, dstPath)) {
        bool ok = fsCopyAtomic(srcPath, dstPath);
        if (ok && updated != NULL) (*updated)++;
        if (!ok) fprintf(stderr, "Failed to copy runtime file %s -> %s\n", srcPath, dstPath);
        return ok;
    }
    if (skipped != NULL) (*skipped)++;
    return true;
}

static char* joinRuntimePath(const char* root, const char* relativePath) {
    return strNotEmpty(relativePath) ? pathJoinAlloc(root, relativePath) : safeStrdup(root);
}

typedef struct {
    const char* srcRoot;
    const char* dstRoot;
    const char* extension;
    const PreprocessGameProfileConfig* profiles;
    bool skipChapterDirs;
    uint32_t updated;
    uint32_t skipped;
} RuntimeFileSync;

static bool syncRuntimeFilesDir(RuntimeFileSync* sync, const char* relativeDir);

static bool syncRuntimeFileEntry(
    RuntimeFileSync* sync, const char* relativeDir, const char* name, bool isDirectory) {
    char* childRel = strNotEmpty(relativeDir) ? pathJoinAlloc(relativeDir, name) : safeStrdup(name);
    char* srcChild = childRel != NULL ? pathJoinAlloc(sync->srcRoot, childRel) : NULL;
    char* dstChild = childRel != NULL ? pathJoinAlloc(sync->dstRoot, childRel) : NULL;
    bool ok = childRel != NULL && srcChild != NULL && dstChild != NULL;
    if (ok && isDirectory) {
        if (!sync->skipChapterDirs || findChapterProfile(sync->profiles, srcChild) == NULL) {
            ok = syncRuntimeFilesDir(sync, childRel);
        }
    } else if (ok && strEndsWithNoCase(name, sync->extension)) {
        ok = copyRuntimeFile(srcChild, dstChild, &sync->updated, &sync->skipped);
    }
    free(childRel);
    free(srcChild);
    free(dstChild);
    return ok;
}

static bool syncRuntimeFilesDir(RuntimeFileSync* sync, const char* relativeDir) {
    if (sync->srcRoot == NULL || sync->dstRoot == NULL || sync->extension == NULL) return false;
    char* srcDir = joinRuntimePath(sync->srcRoot, relativeDir);
    char* dstDir = joinRuntimePath(sync->dstRoot, relativeDir);
    if (srcDir == NULL || dstDir == NULL) {
        free(srcDir);
        free(dstDir);
        return false;
    }
    if (pathsEqual(srcDir, dstDir)) {
        free(srcDir);
        free(dstDir);
        return true;
    }
    if (!fsDirExists(srcDir)) {
        free(srcDir);
        free(dstDir);
        return true;
    }
#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_SEARCH_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(srcDir, "*", searchPath, sizeof(searchPath))) {
        free(srcDir);
        free(dstDir);
        return false;
    }
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        free(srcDir);
        free(dstDir);
        return true;
    }

    bool ok = true;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        ok = syncRuntimeFileEntry(
            sync, relativeDir, name, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        if (!ok) break;
    } while (FindNextFileA(findHandle, &findData));
    FindClose(findHandle);
    free(srcDir);
    free(dstDir);
    return ok;
#else
    DIR* dir = opendir(srcDir);
    if (dir == NULL) {
        free(srcDir);
        free(dstDir);
        return true;
    }

    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char* srcChild = pathJoinAlloc(srcDir, name);
        bool isDirectory = srcChild != NULL && fsDirExists(srcChild);
        free(srcChild);
        ok = syncRuntimeFileEntry(sync, relativeDir, name, isDirectory);
        if (!ok) break;
    }
    closedir(dir);
    free(srcDir);
    free(dstDir);
    return ok;
#endif
}

static bool copyRuntimeText(const PreprocessRunConfig* options) {
    if (options == NULL || options->inputPath[0] == '\0' || options->outputDir[0] == '\0')
        return false;
    char* inputDir = pathDupParent(options->inputPath);
    if (inputDir == NULL) return false;
    RuntimeFileSync sync = {
        .srcRoot = inputDir,
        .dstRoot = options->outputDir,
        .extension = ".txt",
    };
    bool ok = syncRuntimeFilesDir(&sync, "");
    if (ok && (sync.updated > 0u || sync.skipped > 0u)) {
        fprintf(stderr, "Runtime text files: updated=%u skipped=%u\n", sync.updated, sync.skipped);
    }
    free(inputDir);
    return ok;
}

static bool copyExternalOggFiles(const PreprocessRunConfig* options) {
    if (options == NULL || options->inputPath[0] == '\0' || options->outputDir[0] == '\0')
        return false;
    char* inputDir = pathDupParent(options->inputPath);
    char* installRoot = resolveInstallRoot(options->gameProfiles, options->inputPath);
    if (inputDir == NULL || installRoot == NULL) {
        free(inputDir);
        free(installRoot);
        return false;
    }

    RuntimeFileSync sync = {
        .srcRoot = installRoot,
        .dstRoot = strNotEmpty(options->sharedOutputDir) ? options->sharedOutputDir
                                                       : options->outputDir,
        .extension = ".ogg",
        .profiles = options->gameProfiles,
        .skipChapterDirs = true,
    };
    bool sameRoot = pathsEqual(inputDir, installRoot);
    bool ok = sameRoot || syncRuntimeFilesDir(&sync, "");
    if (ok) {
        sync.srcRoot = inputDir;
        sync.dstRoot = options->outputDir;
        ok = syncRuntimeFilesDir(&sync, "");
    }
    if (ok && (sync.updated > 0u || sync.skipped > 0u)) {
        fprintf(stderr, "External Ogg files: updated=%u skipped=%u\n", sync.updated, sync.skipped);
    }
    free(inputDir);
    free(installRoot);
    return ok;
}

bool copyWiiCompanions(const PreprocessRunConfig* options, const char* outputDir) {
    if (options == NULL || !strNotEmpty(outputDir)) return false;
    if (!targetInfo(options->user.target)->appRootCompanions) return true;

    char wiiResourceDir[PREPROCESS_SEARCH_PATH_MAX] = "";
    if (options->toolDir[0] != '\0') {
        if (!pathJoin(options->toolDir, "resources/wii", wiiResourceDir, sizeof(wiiResourceDir))) {
            return false;
        }
        if (!fsDirExists(wiiResourceDir)) wiiResourceDir[0] = '\0';
    }
#if defined(CINNAMON_PREPROCESSOR_SOURCE_DIR)
    if (wiiResourceDir[0] == '\0') {
        if (!pathJoin(CINNAMON_PREPROCESSOR_SOURCE_DIR, "resources/wii", wiiResourceDir,
                sizeof(wiiResourceDir))) {
            return false;
        }
    }
#endif

    if (!fsDirExists(wiiResourceDir)) return true;

    char iconPath[1300];
    char metaPath[1300];
    if (!pathJoin(wiiResourceDir, "icon.png", iconPath, sizeof(iconPath)) ||
        !pathJoin(wiiResourceDir, "meta.xml", metaPath, sizeof(metaPath))) {
        return false;
    }

    char* wiiAppRootDir = dupWiiAppRoot(outputDir);
    if (wiiAppRootDir == NULL) return false;
    uint32_t updated = 0;
    uint32_t skipped = 0;
    if (!fsEnsureDir(wiiAppRootDir) ||
        !copyCompanionIfPresent(iconPath, wiiAppRootDir, "icon.png", &updated, &skipped) ||
        !copyCompanionIfPresent(metaPath, wiiAppRootDir, "meta.xml", &updated, &skipped)) {
        free(wiiAppRootDir);
        return false;
    }
    PreprocessRunConfig finalOptions = *options;
    if (!pathCopy(finalOptions.outputDir, sizeof(finalOptions.outputDir), outputDir)) {
        free(wiiAppRootDir);
        return false;
    }

    if (!writeWiiSettings(&finalOptions)) {
        free(wiiAppRootDir);
        return false;
    }
    fprintf(stderr, "%s runtime companion files: updated=%u skipped=%u dir=%s\n",
        targetName(options->user.target), updated, skipped, wiiAppRootDir);
    free(wiiAppRootDir);
    return true;
}

bool copyCompanions(const PreprocessRunConfig* options, const PreprocessGameProfile* gameProfile) {
    if (options == NULL || options->inputPath[0] == '\0' || options->outputDir[0] == '\0')
        return false;

    uint32_t dataFileUpdated = 0u;
    uint32_t dataFileSkipped = 0u;
    if (!copyCompanionIfPresent(options->inputPath, options->outputDir, "data.win",
            &dataFileUpdated, &dataFileSkipped)) {
        return false;
    }
    fprintf(stderr, "GameMaker data file: updated=%u skipped=%u path=%s/data.win\n",
        dataFileUpdated, dataFileSkipped, options->outputDir);

    if (targetInfo(options->user.target)->appRootCompanions) {
        if (!copyWiiCompanions(options, options->outputDir)) return false;
    }
    if (!copyExternalOggFiles(options)) return false;
    if (gameProfile != NULL && gameProfile->companionCopyCount > 0u) {
        char* inputDir = pathDupParent(options->inputPath);
        if (inputDir == NULL) return false;
        uint32_t updated = 0;
        uint32_t skipped = 0;
        bool ok = true;
        for (size_t i = 0; ok && i < gameProfile->companionCopyCount; ++i) {
            const PreprocessGameProfileCompanionCopy* rule = &gameProfile->companionCopies[i];
            char sourceRel[512];
            char destRel[512];
            if (!normalizeCompanionPath(rule->source, sourceRel, sizeof(sourceRel)) ||
                !normalizeCompanionPath(rule->destination, destRel, sizeof(destRel))) {
                fprintf(stderr, "Invalid companion copy rule for %s: %s -> %s\n",
                    profileLabel(gameProfile), rule->source, rule->destination);
                ok = false;
                break;
            }

            if (hasWildcard(sourceRel)) {
                char sourceDirRel[512];
                char sourcePattern[256];
                if (!splitPattern(sourceRel, sourceDirRel, sizeof(sourceDirRel), sourcePattern,
                        sizeof(sourcePattern))) {
                    ok = false;
                    break;
                }
                char* sourceDir = pathJoinAlloc(inputDir, sourceDirRel);
                char* destDir = pathJoinAlloc(options->outputDir, destRel);
                ok = copyMatchingFiles(sourceDir, destDir, sourcePattern, &updated, &skipped);
                if (!ok)
                    fprintf(stderr, "Failed to copy companion files %s/%s -> %s\n", sourceDir,
                        sourcePattern, destDir);
                free(sourceDir);
                free(destDir);
            } else {
                char* sourcePath = pathJoinAlloc(inputDir, sourceRel);
                char* destPath = pathJoinAlloc(options->outputDir, destRel);
                if (fsDirExists(sourcePath)) {
                    ok = fsSyncTree(sourcePath, destPath, &updated, &skipped);
                    if (!ok)
                        fprintf(stderr, "Failed to copy companion directory %s -> %s\n", sourcePath,
                            destPath);
                } else if (fsFileExists(sourcePath)) {
                    if (!fsFileExists(destPath) || !fsFilesEqual(sourcePath, destPath)) {
                        ok = fsCopyAtomic(sourcePath, destPath);
                        if (ok) updated++;
                    } else {
                        skipped++;
                    }
                    if (!ok)
                        fprintf(stderr, "Failed to copy companion file %s -> %s\n", sourcePath,
                            destPath);
                }
                free(sourcePath);
                free(destPath);
            }
        }
        if (ok)
            fprintf(stderr, "Profile companion files: updated=%u skipped=%u profile=%s\n", updated,
                skipped, profileLabel(gameProfile));
        free(inputDir);
        if (!ok) return false;
    }

    if (!copyRuntimeText(options)) return false;
    return true;
}
