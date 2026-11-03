#include "preprocess_frontend.h"
#include "core/cinnamon_core.h"
#include "frontend/cli/cli_args.h"
#include "common/fs_utils.h"

#include <stdio.h>
#include <string.h>

int runFrontend(int argc, char** argv) {
    progressReset();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    PreprocessGameProfileConfig gameProfiles;
    if (loadProfiles(argv[0], &gameProfiles)) {
        fprintf(stderr, "preprocess: loaded game profiles from %s\n", gameProfiles.loadedPath);
    } else {
        fprintf(stderr,
            "preprocess: no game_profiles.cfg found; using generic GameMaker behavior only.\n");
    }
    progressPhase(PREPROCESS_PHASE_ARGUMENTS);
    PreprocessUserOptions user;
    if (!parseArgs(argc, argv, &user)) {
        printUsage(argv[0]);
        return 1;
    }
    if (user.helpRequested) {
        printUsage(argv[0]);
        return 0;
    }

    PreprocessRunConfig options;
    resolveOptions(&user, &options);
    if (fsProgramDir(argv[0], options.toolDir, sizeof(options.toolDir))) {
        fprintf(stderr, "cinnamon-preprocessor: tool directory %s\n", options.toolDir);
    }
    if (options.inputPath[0] == '\0' || options.outputDir[0] == '\0') {
        printUsage(argv[0]);
        return 1;
    }
    return processGame(&options, &gameProfiles) ? 0 : 1;
}
