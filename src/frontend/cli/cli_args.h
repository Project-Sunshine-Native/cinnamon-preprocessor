#ifndef CINNAMON_PREPROCESSOR_CLI_ARGS_H
#define CINNAMON_PREPROCESSOR_CLI_ARGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/game_profiles.h"
#include "core/preprocess_options.h"

void printUsage(const char* argv0);
bool parseArgs(int argc, char** argv, PreprocessUserOptions* out);

#endif
