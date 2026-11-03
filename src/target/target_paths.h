#ifndef CINNAMON_PREPROCESSOR_TARGET_PATHS_H
#define CINNAMON_PREPROCESSOR_TARGET_PATHS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/game_profiles.h"
#include "core/preprocess_options.h"

bool buildSdOutputDir(const char* sdInputPath, const char* gameInputPath,
    CinnamonPreprocessorTarget target, char* outOutputDir, size_t outOutputDirSize);

#endif
