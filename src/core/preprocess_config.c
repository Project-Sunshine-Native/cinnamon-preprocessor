#include "common/preprocess_common.h"
#include "preprocess_context.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "core/preprocess_config.h"

#include "utils.h"
#include "target/preprocess_target.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool isDataFileName(const char* name) {
    return strEqualNoCase(name, "data.win") || strEqualNoCase(name, "game.unx");
}

bool isDataFile(const char* path) {
    const char* name = pathBaseName(path);
    return name != NULL && isDataFileName(name);
}

bool ensureOutputDirs(const char* outputDir) {
    if (outputDir == NULL || outputDir[0] == '\0') return false;
    char path[PREPROCESS_SEARCH_PATH_MAX];
    if (!fsEnsureDir(outputDir)) return false;
    if (!pathJoin(outputDir, "gfx", path, sizeof(path))) return false;
    if (!fsEnsureDir(path)) return false;
    if (!pathJoin(outputDir, "audio", path, sizeof(path))) return false;
    return fsEnsureDir(path);
}

void applyProfile(PreprocessRunConfig* options, const PreprocessGameProfile* gameProfile) {
    if (options == NULL || gameProfile == NULL) return;
    if (!options->user.atlasPageModeExplicit &&
        gameProfile->atlasPageMode >= (int32_t) CINNAMON_ATLAS_PAGE_MODE_AUTO &&
        gameProfile->atlasPageMode <= (int32_t) CINNAMON_ATLAS_PAGE_MODE_FORCE_1024) {
        options->atlasPageMode = (CinnamonAtlasPageMode) gameProfile->atlasPageMode;
        const char* label = "auto";
        if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_256)
            label = "256";
        else if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_512)
            label = "512";
        else if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_1024)
            label = "1024";
        fprintf(stderr, "cinnamon-preprocessor: profile enabled atlas page mode %s\n", label);
    }
}

static bool findChapterData(const PreprocessGameProfileConfig* profiles, const char* rootDir,
    int32_t chapterOverride, char* dataWinPath, size_t dataWinPathSize) {
    if (profiles == NULL || rootDir == NULL || chapterOverride <= 0) return false;
    for (size_t i = 0; i < profiles->profileCount; ++i) {
        if (resolveChapterData(
                &profiles->profiles[i], rootDir, chapterOverride, dataWinPath, dataWinPathSize)) {
            return true;
        }
    }
    return false;
}

bool resolveGamePath(const PreprocessGameProfileConfig* profiles, const char* candidate,
    int32_t chapterOverride, char* dataWinPath, size_t dataWinPathSize) {
    if (candidate == NULL || candidate[0] == '\0') return false;

    if (fsFileExists(candidate)) {
        if (isDataFile(candidate)) {
            if (chapterOverride > 0) {
                char* inputDir = pathDupParent(candidate);
                char* installRoot = resolveInstallRoot(profiles, candidate);
                if (installRoot == NULL && inputDir != NULL) installRoot = safeStrdup(inputDir);
                bool found = findChapterData(
                    profiles, installRoot, chapterOverride, dataWinPath, dataWinPathSize);
                free(installRoot);
                free(inputDir);
                if (found) return true;
            }
            return pathCopy(dataWinPath, dataWinPathSize, candidate);
        }
        return false;
    }

    if (!fsDirExists(candidate)) return false;

    if (chapterOverride > 0 &&
        findChapterData(profiles, candidate, chapterOverride, dataWinPath, dataWinPathSize)) {
        return true;
    }

    char directPath[PREPROCESS_PATH_MAX];
    if (!pathJoin(candidate, "data.win", directPath, sizeof(directPath))) return false;
    if (fsFileExists(directPath)) {
        return pathCopy(dataWinPath, dataWinPathSize, directPath);
    }

    if (!pathJoin(candidate, "game.unx", directPath, sizeof(directPath))) return false;
    if (fsFileExists(directPath)) {
        return pathCopy(dataWinPath, dataWinPathSize, directPath);
    }

    return false;
}

bool normalizeInputPath(PreprocessRunConfig* options, const PreprocessGameProfileConfig* profiles) {
    if (options == NULL || options->inputPath[0] == '\0') return false;
    char resolvedPath[sizeof(options->inputPath)];
    if (!resolveGamePath(profiles, options->inputPath, options->user.chapterOverride, resolvedPath,
            sizeof(resolvedPath))) {
        return false;
    }
    return pathCopy(options->inputPath, sizeof(options->inputPath), resolvedPath);
}

bool resolveRunPaths(
    PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles) {
    options->gameProfiles = gameProfiles;
    if (options->inputPath[0] == '\0' || options->outputDir[0] == '\0') {
        fprintf(stderr, "Input path and output directory are required.\n");
        return false;
    }

    progressPhase(PREPROCESS_PHASE_INPUT_PATH);
    if (!normalizeInputPath(options, gameProfiles)) {
        fprintf(stderr, "Could not resolve data.win or game.unx from: %s\n", options->inputPath);
        fprintf(stderr, "Pass either the game folder or the full path to data.win/game.unx.\n");
        return false;
    }

    progressPhase(PREPROCESS_PHASE_OUTPUT_CONFIG);
    if (!normalizeOutputDir(options)) {
        fprintf(stderr, "Failed to normalize output directory.\n");
        return false;
    }

    return true;
}
