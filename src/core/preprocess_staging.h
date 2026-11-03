#ifndef CINNAMON_PREPROCESSOR_STAGING_H
#define CINNAMON_PREPROCESSOR_STAGING_H

#include "common/preprocess_common.h"

#include "preprocess_options.h"

typedef struct {
    char finalOutputDir[PREPROCESS_PATH_MAX];
    char tmpRoot[PREPROCESS_PATH_MAX];
    char outputDir[PREPROCESS_PATH_MAX];
    bool tmpRootExisted;
    bool prepared;
} PreprocessStaging;

bool prepareStaging(PreprocessStaging* staging, PreprocessRunConfig* options);
bool installStaging(const PreprocessStaging* staging, const PreprocessRunConfig* options);
void cleanupStaging(const PreprocessStaging* staging, const PreprocessRunConfig* options);

#endif
