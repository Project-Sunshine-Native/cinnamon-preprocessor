#ifndef CINNAMON_PREPROCESSOR_COMPANION_FILES_H
#define CINNAMON_PREPROCESSOR_COMPANION_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/game_profiles.h"
#include "core/preprocess_options.h"

bool copyCompanions(const PreprocessRunConfig* options, const PreprocessGameProfile* gameProfile);
bool copyWiiCompanions(const PreprocessRunConfig* options, const char* outputDir);

#endif
