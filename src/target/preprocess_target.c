#include "common/preprocess_common.h"
#include "preprocess_target.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "target/target_paths.h"

#include <stdlib.h>
#include <string.h>

#define FORMAT_BIT(format) (1u << (format))
#define WII_FORMATS \
    (FORMAT_BIT(CINNAMON_TEXFMT_CI4) | FORMAT_BIT(CINNAMON_TEXFMT_CI8) | \
        FORMAT_BIT(CINNAMON_TEXFMT_CMPR) | FORMAT_BIT(CINNAMON_TEXFMT_RGB5A3))
#define N3DS_FORMATS \
    (FORMAT_BIT(CINNAMON_TEXFMT_ETC1A4) | FORMAT_BIT(CINNAMON_TEXFMT_L4) | \
        FORMAT_BIT(CINNAMON_TEXFMT_LA4) | FORMAT_BIT(CINNAMON_TEXFMT_RGBA4444) | \
        FORMAT_BIT(CINNAMON_TEXFMT_ETC1))

static const PreprocessTargetDescriptor targets[] = {
    {
        .target = CINNAMON_PREPROCESSOR_TARGET_3DS,
        .id = "3ds",
        .alias = "n3ds",
        .displayName = "3DS",
        .audioBankLabel = "3DS DSP",
        .textureFormats = N3DS_FORMATS,
        .defaultTextureFormat = CINNAMON_TEXFMT_RGBA4444,
        .texturePipeline =
            {
                .name = "3DS",
                .atlasLogPrefix = "",
                .useLa4ForNormalFonts = true,
            },
    },
    {
        .target = CINNAMON_PREPROCESSOR_TARGET_WII,
        .id = "wii",
        .displayName = "Wii",
        .sdRootName = "apps",
        .audioBankLabel = "Wii DSP",
        .textureFormats = WII_FORMATS,
        .defaultTextureFormat = CINNAMON_TEXFMT_CI8,
        .forceAutoAtlas = true,
        .appRootCompanions = true,
        .texturePipeline =
            {
                .name = "Wii",
                .atlasLogPrefix = "seekable Wii ",
                .promoteCi8OverflowToRgb5a3 = true,
                .demoteCi8ToCi4 = true,
                .validatePageFormats = true,
            },
    },
};

const PreprocessTargetDescriptor* targetInfo(CinnamonPreprocessorTarget target) {
    if ((unsigned) target >= sizeof(targets) / sizeof(targets[0])) return NULL;
    return &targets[target];
}

const char* targetName(CinnamonPreprocessorTarget target) {
    const PreprocessTargetDescriptor* desc = targetInfo(target);
    return desc != NULL ? desc->displayName : "unknown";
}

const char* targetSdRoot(CinnamonPreprocessorTarget target) {
    const PreprocessTargetDescriptor* desc = targetInfo(target);
    return desc != NULL ? desc->sdRootName : NULL;
}

bool parseTarget(const char* label, CinnamonPreprocessorTarget* outTarget) {
    if (label == NULL || outTarget == NULL) return false;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (strcmp(label, targets[i].id) == 0 ||
            (targets[i].alias != NULL && strcmp(label, targets[i].alias) == 0)) {
            *outTarget = targets[i].target;
            return true;
        }
    }
    return false;
}

void applyTargetRules(PreprocessRunConfig* options) {
    if (options == NULL) return;
    const PreprocessTargetDescriptor* desc = targetInfo(options->user.target);
    if (desc == NULL) return;
    if (desc->forceAutoAtlas) options->atlasPageMode = CINNAMON_ATLAS_PAGE_MODE_AUTO;
    if (!targetAllowsFormat(options->user.target, options->textureFormat)) {
        options->textureFormat = desc->defaultTextureFormat;
    }
}

bool targetAllowsFormat(CinnamonPreprocessorTarget target, CinnamonTextureFormat format) {
    const PreprocessTargetDescriptor* desc = targetInfo(target);
    return desc != NULL && (unsigned) format < 32u &&
        (desc->textureFormats & FORMAT_BIT(format)) != 0u;
}

bool isSdTargetPath(const PreprocessRunConfig* options, const char* outputDir) {
    if (options == NULL || outputDir == NULL || outputDir[0] == '\0') return false;

    char normalized[PREPROCESS_PATH_MAX];
    if (!pathCopy(normalized, sizeof(normalized), outputDir)) return false;
    pathUnquote(normalized);
    size_t len = strlen(normalized);
    while (len > 0u && (normalized[len - 1u] == '/' || normalized[len - 1u] == '\\'))
        normalized[--len] = '\0';

    if (pathIsDriveRoot(normalized)) return true;

    const PreprocessTargetDescriptor* desc = targetInfo(options->user.target);
    if (desc == NULL || desc->sdRootName == NULL) return false;
    const char* installComponents[] = {desc->sdRootName, "cinnamon"};
    if (pathHasComponentsNoCase(normalized, installComponents,
            sizeof(installComponents) / sizeof(installComponents[0])))
        return true;
    const char* base = pathBaseName(normalized);
    if (strEqualNoCase(base, desc->sdRootName) || strEqualNoCase(base, "cinnamon")) return true;
    char* rootDir = pathJoinAlloc(normalized, desc->sdRootName);
    bool hasRootDir = rootDir != NULL && fsDirExists(rootDir);
    free(rootDir);
    return hasRootDir || strEqualNoCase(base, "SD");
}

bool normalizeOutputDir(PreprocessRunConfig* options) {
    if (options == NULL || options->user.outputOrigin != PREPROCESS_PATH_EXPLICIT)
        return true;
    if (!isSdTargetPath(options, options->outputDir)) return true;

    char normalizedOutputDir[sizeof(options->outputDir)];
    if (!buildSdOutputDir(options->outputDir, options->inputPath, options->user.target,
            normalizedOutputDir, sizeof(normalizedOutputDir))) {
        return false;
    }
    if (strcmp(normalizedOutputDir, options->outputDir) != 0) {
        fprintf(stderr, "cinnamon-preprocessor: normalized SD output %s -> %s\n",
            options->outputDir, normalizedOutputDir);
    }
    return pathCopy(options->outputDir, sizeof(options->outputDir), normalizedOutputDir);
}
