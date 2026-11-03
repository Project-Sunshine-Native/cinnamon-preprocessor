#ifndef CINNAMON_PREPROCESSOR_TEXTURE_PIPELINE_H
#define CINNAMON_PREPROCESSOR_TEXTURE_PIPELINE_H

#include <stdbool.h>

#include "core/preprocess_options.h"

typedef struct TexturePreprocessPipeline {
    const char* name;
    const char* atlasLogPrefix;
    bool promoteCi8OverflowToRgb5a3;
    bool demoteCi8ToCi4;
    bool useLa4ForNormalFonts;
    bool validatePageFormats;
} TexturePreprocessPipeline;

bool texConvertPipeline(const PreprocessRunConfig* options, DataWin* dataWin,
    const TexturePreprocessPipeline* pipeline);

#endif
