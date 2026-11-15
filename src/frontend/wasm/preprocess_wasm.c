#include "preprocess_wasm.h"
#include "core/preprocess_context.h"

#include <stdio.h>
#include <emscripten.h>

static int s_wasmProgress = -1;
static uint32_t s_gameIndex;
static uint32_t s_gameCount = 1u;
static char s_gameName[128];

EM_JS(void, emitJsProgress, (int pct, const char* phase), {
    if (Module["preprocessProgress"]) Module["preprocessProgress"](pct, UTF8ToString(phase));
});

static void emitProgress(int pct, const char* phase) {
    if (phase == NULL) phase = "";
    bool inGame = pct >= 50 && pct <= 87;
    if (inGame) pct = 50 + (int) ((s_gameIndex * 38u + (uint32_t) (pct - 50)) / s_gameCount);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct <= s_wasmProgress) return;
    s_wasmProgress = pct;
    if (inGame && s_gameCount > 1u) {
        char labeled[256];
        snprintf(labeled, sizeof(labeled), "%s (%u/%u): %s", s_gameName,
            s_gameIndex + 1u, s_gameCount, phase);
        emitJsProgress(pct, labeled);
    } else emitJsProgress(pct, phase);
}

static void onReset(void* userData) {
    (void) userData;
    s_wasmProgress = -1;
    s_gameIndex = 0u;
    s_gameCount = 1u;
    s_gameName[0] = '\0';
}

static void onGame(uint32_t gameIndex, uint32_t gameCount, const char* name, void* userData) {
    (void) userData;
    s_gameIndex = gameIndex;
    s_gameCount = gameCount > 0u ? gameCount : 1u;
    snprintf(s_gameName, sizeof(s_gameName), "%s", name != NULL ? name : "game");
    if (s_gameCount > 1u) {
        char phase[192];
        snprintf(phase, sizeof(phase), "Processing %s (%u/%u)", s_gameName,
            gameIndex + 1u, gameCount);
        emitJsProgress(s_wasmProgress > 45 ? s_wasmProgress : 45, phase);
    }
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
        .game = onGame,
    };
    progressSetCallbacks(&callbacks);
}
