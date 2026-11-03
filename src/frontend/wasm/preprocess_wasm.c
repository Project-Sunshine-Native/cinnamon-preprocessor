#include "preprocess_wasm.h"
#include "core/preprocess_context.h"

#include <stdio.h>
#include <emscripten.h>

static int s_wasmProgress = -1;

EM_JS(void, emitJsProgress, (int pct, const char* phase), {
    if (Module["preprocessProgress"]) Module["preprocessProgress"](pct, UTF8ToString(phase));
});

static void emitProgress(int pct, const char* phase) {
    if (phase == NULL) phase = "";
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct <= s_wasmProgress) return;
    s_wasmProgress = pct;
    emitJsProgress(pct, phase);
}

static void onReset(void* userData) {
    (void) userData;
    s_wasmProgress = -1;
}

static void onPhase(PreprocessPhase phase, void* userData) {
    (void) userData;
    static const struct {
        int pct;
        const char* name;
    } phases[] = {
        {46, "parsing arguments"},
        {47, "resolving GameMaker data path"},
        {48, "configuring output"},
        {49, "creating output directories"},
        {50, "parsing GameMaker data file"},
        {58, "copying runtime companion files"},
        {61, "converting textures"},
        {62, "processing textures"},
        {78, "writing atlas"},
        {81, "writing room manifest"},
        {83, "converting audio"},
        {87, "freeing GameMaker data file"},
        {88, "copying staged output to final destination"},
    };
    if ((unsigned) phase < sizeof(phases) / sizeof(phases[0])) {
        emitProgress(phases[phase].pct, phases[phase].name);
    }
}

static void onTexturePage(uint32_t pageIndex, uint32_t pageCount, void* userData) {
    (void) userData;
    if (pageCount > 0u) {
        char phase[64];
        uint32_t pageNumber = pageIndex + 1u;
        if (pageNumber > pageCount) pageNumber = pageCount;
        int pct = 63 + (int) (((uint64_t) pageNumber * 14u) / pageCount);
        if (pct > 77) pct = 77;
        snprintf(phase, sizeof(phase), "processing texture page %lu/%lu",
            (unsigned long) pageNumber, (unsigned long) pageCount);
        emitProgress(pct, phase);
    }
}

static void onRoom(int32_t roomIndex, void* userData) {
    (void) userData;
    if (roomIndex >= 0) {
        char phase[64];
        snprintf(phase, sizeof(phase), "writing room %ld", (long) roomIndex);
        emitProgress(82, phase);
    }
}

void initWasmProgress(void) {
    const PreprocessProgressCallbacks callbacks = {
        .reset = onReset,
        .phase = onPhase,
        .texturePage = onTexturePage,
        .room = onRoom,
    };
    progressSetCallbacks(&callbacks);
}
