#include "preprocess_staging.h"
#include "preprocess_context.h"
#include "target/preprocess_target.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "core/preprocess_config.h"
#include "target/companion_files.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool appendStagingPath(
    char* out, size_t outSize, size_t* inOutLen, const char* segment, size_t segmentLen) {
    if (out == NULL || inOutLen == NULL || segment == NULL) return false;
    if (segmentLen == 0u || (segmentLen == 1u && segment[0] == '.')) return true;

    bool parentSegment = segmentLen == 2u && segment[0] == '.' && segment[1] == '.';
    size_t len = *inOutLen;
    if (len > 0u) {
        if (len + 1u >= outSize) return false;
        out[len++] = '/';
    }

    if (parentSegment) {
        if (len + 1u >= outSize) return false;
        out[len++] = '_';
    } else {
        for (size_t i = 0u; i < segmentLen; ++i) {
            unsigned char c = (unsigned char) segment[i];
            char outChar = (isalnum(c) || c == '_' || c == '-' || c == '.') ? (char) c : '_';
            if (len + 1u >= outSize) return false;
            out[len++] = outChar;
        }
    }

    out[len] = '\0';
    *inOutLen = len;
    return true;
}

static bool makeStagingRelativePath(const char* finalOutputDir, char* out, size_t outSize) {
    if (finalOutputDir == NULL || out == NULL || outSize == 0u) return false;
    out[0] = '\0';

    size_t outLen = 0u;
    size_t segmentStart = 0u;
    size_t len = strlen(finalOutputDir);
    for (size_t i = 0u; i <= len; ++i) {
        char c = finalOutputDir[i];
        if (c != '/' && c != '\\' && c != '\0') continue;
        if (!appendStagingPath(
                out, outSize, &outLen, finalOutputDir + segmentStart, i - segmentStart))
            return false;
        segmentStart = i + 1u;
    }

    if (out[0] == '\0') snprintf(out, outSize, "output");
    return out[0] != '\0';
}

static bool buildStagingPaths(const PreprocessRunConfig* options, const char* finalOutputDir,
    char* outTmpRoot, size_t tmpRootSize, char* outStagingOutputDir, size_t stagingOutputDirSize) {
    if (options == NULL || finalOutputDir == NULL || outTmpRoot == NULL ||
        outStagingOutputDir == NULL)
        return false;
    const char* toolDir = options->toolDir[0] != '\0' ? options->toolDir : ".";
    if (!pathJoin(toolDir, "tmp", outTmpRoot, tmpRootSize)) return false;

    char relative[PREPROCESS_PATH_MAX];
    if (!makeStagingRelativePath(finalOutputDir, relative, sizeof(relative))) return false;
    return pathJoin(outTmpRoot, relative, outStagingOutputDir, stagingOutputDirSize);
}

static bool pathIsInStagingTmpRoot(const char* path, const char* stagingTmpRoot) {
    if (path == NULL || stagingTmpRoot == NULL) return false;
    size_t rootLen = strlen(stagingTmpRoot);
    if (rootLen == 0u || strncmp(path, stagingTmpRoot, rootLen) != 0) return false;
    char next = path[rootLen];
    return next == '\0' || next == '/' || next == '\\';
}

static bool cleanupStagingOutput(const PreprocessRunConfig* options, const char* stagingTmpRoot,
    const char* stagingOutputDir, bool tmpRootExisted) {
    if (stagingTmpRoot == NULL || stagingOutputDir == NULL) return false;
    if (!tmpRootExisted) return fsDeleteTree(stagingTmpRoot);

    if (options != NULL && targetInfo(options->user.target)->appRootCompanions) {
        char* stagingAppRootDir = pathDupParent(stagingOutputDir);
        bool ok = stagingAppRootDir != NULL &&
            pathIsInStagingTmpRoot(stagingAppRootDir, stagingTmpRoot) &&
            fsDeleteTree(stagingAppRootDir);
        free(stagingAppRootDir);
        if (ok) return true;
    }

    return fsDeleteTree(stagingOutputDir);
}

static bool seedGeneratedOutput(const char* finalOutputDir, const char* stagingOutputDir) {
    if (finalOutputDir == NULL || stagingOutputDir == NULL) return false;

    const char* generatedDirs[] = {"gfx", "audio"};
    uint32_t updated = 0u;
    uint32_t skipped = 0u;
    preprocessRepeat(sizeof(generatedDirs) / sizeof(generatedDirs[0]), i) {
        char* finalChild = pathJoinAlloc(finalOutputDir, generatedDirs[i]);
        char* stagingChild = pathJoinAlloc(stagingOutputDir, generatedDirs[i]);
        bool ok = finalChild != NULL && stagingChild != NULL &&
            (!fsDirExists(finalChild) || fsSyncTree(finalChild, stagingChild, &updated, &skipped));
        free(finalChild);
        free(stagingChild);
        if (!ok) return false;
    }

    if (updated > 0u || skipped > 0u) {
        fprintf(stderr,
            "cinnamon-preprocessor: staged existing generated output: copied=%u cached=%u\n",
            updated, skipped);
    }
    return true;
}

static bool installStagedOutput(const char* stagingOutputDir, const char* finalOutputDir) {
    if (stagingOutputDir == NULL || finalOutputDir == NULL) return false;

    uint32_t updated = 0u;
    uint32_t skipped = 0u;
    if (!fsSyncTree(stagingOutputDir, finalOutputDir, &updated, &skipped)) return false;

    const char* mirroredGeneratedDirs[] = {"gfx", "audio"};
    preprocessRepeat(sizeof(mirroredGeneratedDirs) / sizeof(mirroredGeneratedDirs[0]), i) {
        char* stagingChild = pathJoinAlloc(stagingOutputDir, mirroredGeneratedDirs[i]);
        char* finalChild = pathJoinAlloc(finalOutputDir, mirroredGeneratedDirs[i]);
        bool ok = stagingChild != NULL && finalChild != NULL &&
            (!fsDirExists(stagingChild) || fsPruneTree(stagingChild, finalChild));
        free(stagingChild);
        free(finalChild);
        if (!ok) return false;
    }

    fprintf(stderr,
        "cinnamon-preprocessor: installed staged output: updated=%u skipped=%u dir=%s\n", updated,
        skipped, finalOutputDir);
    return true;
}

bool prepareStaging(PreprocessStaging* staging, PreprocessRunConfig* options) {
    if (!pathCopy(staging->finalOutputDir, sizeof(staging->finalOutputDir), options->outputDir)) {
        return false;
    }
    if (!buildStagingPaths(options, staging->finalOutputDir, staging->tmpRoot,
            sizeof(staging->tmpRoot), staging->outputDir, sizeof(staging->outputDir))) {
        fprintf(stderr, "Failed to configure local staging directory.\n");
        return false;
    }
    staging->tmpRootExisted = fsDirExists(staging->tmpRoot);
    if (fsDirExists(staging->outputDir) && !fsDeleteTree(staging->outputDir)) {
        fprintf(stderr, "Failed to clean stale staging directory: %s\n", staging->outputDir);
        return false;
    }
    staging->prepared = true;
    if (!fsEnsureDir(staging->tmpRoot) ||
        !seedGeneratedOutput(staging->finalOutputDir, staging->outputDir)) {
        fprintf(stderr, "Failed to prepare local staging directory: %s\n", staging->outputDir);
        return false;
    }
    if (!pathCopy(options->outputDir, sizeof(options->outputDir), staging->outputDir)) return false;

    progressPhase(PREPROCESS_PHASE_OUTPUT_DIRS);
    if (!ensureOutputDirs(options->outputDir)) return false;
    fprintf(stderr, "cinnamon-preprocessor: staging output at %s\n", options->outputDir);
    fprintf(stderr, "cinnamon-preprocessor: final output will be installed to %s\n",
        staging->finalOutputDir);
    return true;
}

bool installStaging(const PreprocessStaging* staging, const PreprocessRunConfig* options) {
    progressPhase(PREPROCESS_PHASE_INSTALL);
    return installStagedOutput(staging->outputDir, staging->finalOutputDir) &&
        copyWiiCompanions(options, staging->finalOutputDir);
}

void cleanupStaging(const PreprocessStaging* staging, const PreprocessRunConfig* options) {
    if (!staging->prepared) return;
    if (!cleanupStagingOutput(
            options, staging->tmpRoot, staging->outputDir, staging->tmpRootExisted)) {
        fprintf(stderr, "Warning: failed to remove staging directory: %s\n",
            staging->tmpRootExisted ? staging->outputDir : staging->tmpRoot);
    }
}
