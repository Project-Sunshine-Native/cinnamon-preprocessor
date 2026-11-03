#include "cinnamon_core.h"
#include "common/path_utils.h"
#include "preprocess_config.h"
#include "preprocess_run.h"
#include "preprocess_staging.h"

bool processGame(PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles) {
    PreprocessStaging staging = {0};
    progressReset();
    if (!resolveRunPaths(options, gameProfiles)) return false;

    bool ok = prepareStaging(&staging, options);
    if (ok) ok = runGame(options, gameProfiles);
    if (ok) ok = installStaging(&staging, options);
    cleanupStaging(&staging, options);
    if (staging.prepared)
        pathCopy(options->outputDir, sizeof(options->outputDir), staging.finalOutputDir);
    return ok;
}
