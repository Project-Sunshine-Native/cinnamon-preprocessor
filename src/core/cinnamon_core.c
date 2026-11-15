#include "cinnamon_core.h"
#include "common/path_utils.h"
#include "preprocess_config.h"
#include "preprocess_run.h"
#include "preprocess_staging.h"
#include "target/companion_files.h"

bool processGame(PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles) {
    progressReset();
    if (!resolveRunPaths(options, gameProfiles)) return false;

#if defined(__EMSCRIPTEN__)
    progressPhase(PREPROCESS_PHASE_OUTPUT_DIRS);
    bool ok = ensureOutputDirs(options->outputDir) && runGame(options, gameProfiles);
    if (ok) {
        progressPhase(PREPROCESS_PHASE_INSTALL);
        ok = copyWiiCompanions(options, options->outputDir);
    }
    return ok;
#else
    PreprocessStaging staging = {0};
    bool ok = prepareStaging(&staging, options);
    if (ok) ok = runGame(options, gameProfiles);
    if (ok) ok = installStaging(&staging, options);
    cleanupStaging(&staging, options);
    if (staging.prepared)
        pathCopy(options->outputDir, sizeof(options->outputDir), staging.finalOutputDir);
    return ok;
#endif
}
