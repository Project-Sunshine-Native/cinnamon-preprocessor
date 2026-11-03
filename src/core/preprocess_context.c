#include "preprocess_context.h"

#include <stddef.h>

static PreprocessProgressCallbacks s_callbacks;

void progressSetCallbacks(const PreprocessProgressCallbacks* callbacks) {
    s_callbacks = callbacks != NULL ? *callbacks : (PreprocessProgressCallbacks){0};
}

void progressReset(void) {
    if (s_callbacks.reset != NULL) s_callbacks.reset(s_callbacks.userData);
}

void progressPhase(PreprocessPhase phase) {
    if (s_callbacks.phase != NULL) s_callbacks.phase(phase, s_callbacks.userData);
}

void progressPage(uint32_t pageIndex, uint32_t pageCount) {
    if (s_callbacks.texturePage != NULL)
        s_callbacks.texturePage(pageIndex, pageCount, s_callbacks.userData);
}

void progressRoom(int32_t roomIndex) {
    if (s_callbacks.room != NULL) s_callbacks.room(roomIndex, s_callbacks.userData);
}
