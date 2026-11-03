#ifndef CINNAMON_PREPROCESSOR_TARGET_H
#define CINNAMON_PREPROCESSOR_TARGET_H

#include <stdbool.h>

#include "core/preprocess_options.h"
#include "texture/texture_pipeline.h"

typedef struct {
    CinnamonPreprocessorTarget target;
    const char* id;
    const char* alias;
    const char* displayName;
    const char* sdRootName;
    const char* audioBankLabel;
    uint32_t textureFormats;
    CinnamonTextureFormat defaultTextureFormat;
    bool forceAutoAtlas;
    bool appRootCompanions;
    TexturePreprocessPipeline texturePipeline;
} PreprocessTargetDescriptor;

const PreprocessTargetDescriptor* targetInfo(CinnamonPreprocessorTarget target);
bool isSdTargetPath(const PreprocessRunConfig* options, const char* outputDir);
bool normalizeOutputDir(PreprocessRunConfig* options);

const char* targetName(CinnamonPreprocessorTarget target);
const char* targetSdRoot(CinnamonPreprocessorTarget target);
bool parseTarget(const char* label, CinnamonPreprocessorTarget* outTarget);
void applyTargetRules(PreprocessRunConfig* options);
bool targetAllowsFormat(CinnamonPreprocessorTarget target, CinnamonTextureFormat format);

#endif
