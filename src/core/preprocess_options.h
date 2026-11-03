#ifndef CINNAMON_PREPROCESSOR_OPTIONS_H
#define CINNAMON_PREPROCESSOR_OPTIONS_H

#include "common/preprocess_common.h"

#include <stdbool.h>
#include <stdint.h>

#include "texture/texture_convert.h"

struct PreprocessGameProfileConfig;

typedef enum {
    CINNAMON_ATLAS_PAGE_MODE_AUTO = 0,
    CINNAMON_ATLAS_PAGE_MODE_FORCE_256,
    CINNAMON_ATLAS_PAGE_MODE_FORCE_512,
    CINNAMON_ATLAS_PAGE_MODE_FORCE_1024,
} CinnamonAtlasPageMode;

typedef enum {
    CINNAMON_AUDIO_EXISTING_ACTION_UNSET = 0,
    CINNAMON_AUDIO_EXISTING_ACTION_SKIP,
    CINNAMON_AUDIO_EXISTING_ACTION_REPROCESS,
} CinnamonAudioExistingAction;

typedef enum {
    CINNAMON_PREPROCESSOR_TARGET_3DS = 0,
    CINNAMON_PREPROCESSOR_TARGET_WII,
} CinnamonPreprocessorTarget;

typedef enum {
    PREPROCESS_PATH_UNSET = 0,
    PREPROCESS_PATH_EXPLICIT,
} PreprocessPathOrigin;

typedef struct {
    char inputPath[PREPROCESS_PATH_MAX];
    char outputDir[PREPROCESS_PATH_MAX];
    char debugOutputDir[PREPROCESS_SEARCH_PATH_MAX];
    bool keepPng;
    bool dumpPagePreviews;
    bool helpRequested;
    bool atlasPageModeExplicit;
    PreprocessPathOrigin outputOrigin;
    CinnamonAudioExistingAction existingAudioAction;
    int32_t chapterOverride;
    CinnamonPreprocessorTarget target;
    CinnamonAtlasPageMode atlasPageMode;
    CinnamonTextureFormat textureFormat;
} PreprocessUserOptions;

struct PreprocessRunConfig {
    PreprocessUserOptions user;
    char inputPath[PREPROCESS_PATH_MAX];
    char outputDir[PREPROCESS_PATH_MAX];
    char sharedOutputDir[PREPROCESS_PATH_MAX];
    char debugOutputDir[PREPROCESS_SEARCH_PATH_MAX];
    char toolDir[PREPROCESS_PATH_MAX];
    const struct PreprocessGameProfileConfig* gameProfiles;
    CinnamonAtlasPageMode atlasPageMode;
    CinnamonTextureFormat textureFormat;
    CinnamonAudioExistingAction existingAudioAction;
};

void initOptions(PreprocessUserOptions* out);
void resolveOptions(const PreprocessUserOptions* user, PreprocessRunConfig* out);

#endif
