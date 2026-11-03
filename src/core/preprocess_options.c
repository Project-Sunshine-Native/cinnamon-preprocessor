#include "preprocess_options.h"
#include "target/preprocess_target.h"

#include <string.h>

void initOptions(PreprocessUserOptions* out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->outputDir, "output", sizeof("output"));
    out->target = CINNAMON_PREPROCESSOR_TARGET_3DS;
    out->textureFormat = targetInfo(out->target)->defaultTextureFormat;
}

void resolveOptions(const PreprocessUserOptions* user, PreprocessRunConfig* out) {
    memset(out, 0, sizeof(*out));
    out->user = *user;
    memcpy(out->inputPath, user->inputPath, sizeof(out->inputPath));
    memcpy(out->outputDir, user->outputDir, sizeof(out->outputDir));
    memcpy(out->debugOutputDir, user->debugOutputDir, sizeof(out->debugOutputDir));
    out->atlasPageMode = user->atlasPageMode;
    out->textureFormat = user->textureFormat;
    out->existingAudioAction = user->existingAudioAction;
}
