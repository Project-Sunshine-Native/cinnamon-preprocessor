#ifndef CINNAMON_PREPROCESSOR_CONTEXT_H
#define CINNAMON_PREPROCESSOR_CONTEXT_H

#include <stdint.h>

typedef enum {
    PREPROCESS_PHASE_ARGUMENTS,
    PREPROCESS_PHASE_INPUT_PATH,
    PREPROCESS_PHASE_OUTPUT_CONFIG,
    PREPROCESS_PHASE_OUTPUT_DIRS,
    PREPROCESS_PHASE_PARSE_DATA,
    PREPROCESS_PHASE_COMPANIONS,
    PREPROCESS_PHASE_TEXTURES,
    PREPROCESS_PHASE_TEXTURE_PAGES,
    PREPROCESS_PHASE_ATLAS,
    PREPROCESS_PHASE_ROOM_MANIFEST,
    PREPROCESS_PHASE_AUDIO,
    PREPROCESS_PHASE_FREE_DATA,
    PREPROCESS_PHASE_INSTALL,
} PreprocessPhase;

typedef struct {
    void (*reset)(void* userData);
    void (*phase)(PreprocessPhase phase, void* userData);
    void (*texturePage)(uint32_t pageIndex, uint32_t pageCount, void* userData);
    void (*room)(int32_t roomIndex, void* userData);
    void* userData;
} PreprocessProgressCallbacks;

void progressSetCallbacks(const PreprocessProgressCallbacks* callbacks);

void progressReset(void);
void progressPhase(PreprocessPhase phase);
void progressPage(uint32_t pageIndex, uint32_t pageCount);
void progressRoom(int32_t roomIndex);

#endif
