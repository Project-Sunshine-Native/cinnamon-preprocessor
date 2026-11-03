#include "common/preprocess_common.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "core/preprocess_config.h"
#include "target/target_paths.h"

#include "utils.h"
#include "preprocess_target.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//this is the jankiest code i've probably written, but it works. definitely will refactor this at some point.
static void makeGameFolderName(const char* gameInputPath, char* outName, size_t outNameSize) {
    if (outName == NULL || outNameSize == 0u) return;
    outName[0] = '\0';
    const char* fallback = "game";
    const char* source = strNotEmpty(gameInputPath) ? gameInputPath : fallback;
    char temp[PREPROCESS_PATH_MAX];
    if (!pathCopy(temp, sizeof(temp), source)) pathCopy(temp, sizeof(temp), fallback);
    pathUnquote(temp);

    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '/' || temp[len - 1] == '\\')) temp[--len] = '\0';
    const char* base = pathBaseName(temp);
    if (base != NULL && isDataFileName(base)) {
        char* parent = pathDupParent(temp);
        if (parent != NULL) {
            if (!pathCopy(temp, sizeof(temp), parent)) temp[0] = '\0';
            free(parent);
            len = strlen(temp);
            while (len > 0 && (temp[len - 1] == '/' || temp[len - 1] == '\\')) temp[--len] = '\0';
            base = pathBaseName(temp);
        }
    }
    if (base == NULL || base[0] == '\0') base = fallback;

    size_t cursor = 0u;
    for (size_t i = 0u; base[i] != '\0' && cursor + 1u < outNameSize; ++i) {
        unsigned char ch = (unsigned char) base[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-') {
            outName[cursor++] = (char) ch;
        } else if (ch == ' ' || ch == '.') {
            if (cursor > 0u && outName[cursor - 1u] != '_') outName[cursor++] = '_';
        }
    }
    while (cursor > 0u && outName[cursor - 1u] == '_') cursor--;
    outName[cursor] = '\0';
    if (outName[0] == '\0') pathCopy(outName, outNameSize, fallback);
}

bool buildSdOutputDir(const char* sdInputPath, const char* gameInputPath,
    CinnamonPreprocessorTarget target, char* outOutputDir, size_t outOutputDirSize) {
    if (sdInputPath == NULL || sdInputPath[0] == '\0') return false;
    char gameFolder[128];
    makeGameFolderName(gameInputPath, gameFolder, sizeof(gameFolder));
    const PreprocessTargetDescriptor* desc = targetInfo(target);
    if (desc == NULL || desc->sdRootName == NULL) return false;
    const char* platformRoot = targetSdRoot(target);

    char temp[PREPROCESS_PATH_MAX];
    if (!pathCopy(temp, sizeof(temp), sdInputPath)) return false;
    pathUnquote(temp);

    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '/' || temp[len - 1] == '\\')) {
        if (pathIsDriveRoot(temp) && len <= 3) break;
        temp[--len] = '\0';
    }

    if (pathIsDriveRoot(temp)) {
        char driveRoot[3] = {temp[0], ':', '\0'};
        char platformDir[PREPROCESS_PATH_MAX];
        char cinnamonDir[PREPROCESS_PATH_MAX];
        return pathJoin(driveRoot, platformRoot, platformDir, sizeof(platformDir)) &&
            pathJoin(platformDir, "cinnamon", cinnamonDir, sizeof(cinnamonDir)) &&
            pathJoin(cinnamonDir, gameFolder, outOutputDir, outOutputDirSize);
    }

    const char* lastSlash = strrchr(temp, '/');
    const char* lastBackslash = strrchr(temp, '\\');
    const char* base = lastSlash;
    if (lastBackslash != NULL && (base == NULL || lastBackslash > base)) base = lastBackslash;
    base = (base != NULL) ? base + 1 : temp;

    const char* installComponents[] = {desc->sdRootName, "cinnamon"};
    bool containsInstallPath = pathHasComponentsNoCase(
        temp, installComponents, sizeof(installComponents) / sizeof(installComponents[0]));
    if (containsInstallPath) {
        return strEqualNoCase(base, "cinnamon")
            ? pathJoin(temp, gameFolder, outOutputDir, outOutputDirSize)
            : pathCopy(outOutputDir, outOutputDirSize, temp);
    }
    if (strEqualNoCase(base, "cinnamon")) {
        return pathJoin(temp, gameFolder, outOutputDir, outOutputDirSize);
    }
    if (strEqualNoCase(base, desc->sdRootName)) {
        char cinnamonDir[PREPROCESS_PATH_MAX];
        return pathJoin(temp, "cinnamon", cinnamonDir, sizeof(cinnamonDir)) &&
            pathJoin(cinnamonDir, gameFolder, outOutputDir, outOutputDirSize);
    }
    char platformDir[PREPROCESS_PATH_MAX];
    char cinnamonDir[PREPROCESS_PATH_MAX];
    return pathJoin(temp, platformRoot, platformDir, sizeof(platformDir)) &&
        pathJoin(platformDir, "cinnamon", cinnamonDir, sizeof(cinnamonDir)) &&
        pathJoin(cinnamonDir, gameFolder, outOutputDir, outOutputDirSize);
}
