#ifndef CINNAMON_PREPROCESSOR_PREPROCESS_CONFIG_H
#define CINNAMON_PREPROCESSOR_PREPROCESS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "game_profiles.h"
#include "preprocess_options.h"

bool isDataFileName(const char* name);
bool isDataFile(const char* path);
bool resolveGamePath(const PreprocessGameProfileConfig* profiles, const char* candidate,
    int32_t chapterOverride, char* dataWinPath, size_t dataWinPathSize);

bool ensureOutputDirs(const char* outputDir);
void applyProfile(PreprocessRunConfig* options, const PreprocessGameProfile* gameProfile);
bool normalizeInputPath(PreprocessRunConfig* options, const PreprocessGameProfileConfig* profiles);

bool resolveRunPaths(PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles);

#endif
