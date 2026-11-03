#include "cli_args.h"
#include "common/path_utils.h"
#include "target/preprocess_target.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

void printUsage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s <game-folder|data.win|game.unx> [--keep-png]\n"
        "           [-o|--output <dir>]\n"
        "           [--debug-output <dir>]\n"
        "           [--atlas-size 256|512|1024]\n"
        "           [--target 3ds|wii]\n"
        "           [--atlas-page-mode auto|256|512|1024] (auto uses independent 128..1024 "
        "power-of-two dimensions)\n"
        "           [--texture-format etc1|etc1a4|rgba4444|l4|la4] (3DS)\n"
        "           [--texture-format ci4|ci8|cmpr|rgb5a3] (Wii)\n"
        "           [--dump-page-previews]\n"
        "           [--chapter <number>]\n"
        "\n"
        "Input is required. Output defaults to ./output; use -o or --output to change it.\n",
        argv0 != NULL ? argv0 : "cinnamon-preprocessor");
}

static bool parseAtlasPageModeLabel(const char* mode, CinnamonAtlasPageMode* outMode) {
    if (mode == NULL || outMode == NULL) return false;
    if (strcmp(mode, "auto") == 0 || strcmp(mode, "0") == 0) {
        *outMode = CINNAMON_ATLAS_PAGE_MODE_AUTO;
        return true;
    }
    if (strcmp(mode, "256") == 0) {
        *outMode = CINNAMON_ATLAS_PAGE_MODE_FORCE_256;
        return true;
    }
    if (strcmp(mode, "512") == 0) {
        *outMode = CINNAMON_ATLAS_PAGE_MODE_FORCE_512;
        return true;
    }
    if (strcmp(mode, "1024") == 0) {
        *outMode = CINNAMON_ATLAS_PAGE_MODE_FORCE_1024;
        return true;
    }
    return false;
}

static void selectTarget(
    PreprocessUserOptions* out, CinnamonPreprocessorTarget target, bool textureFormatExplicit) {
    out->target = target;
    if (!out->atlasPageModeExplicit) out->atlasPageMode = CINNAMON_ATLAS_PAGE_MODE_AUTO;
    if (!textureFormatExplicit) out->textureFormat = targetInfo(target)->defaultTextureFormat;
}

typedef struct {
    PreprocessUserOptions* options;
    bool textureFormatExplicit;
} CliParseContext;

typedef bool (*CliOptionHandler)(CliParseContext* ctx, const char* value);

typedef struct {
    const char* name;
    bool requiresValue;
    bool allowsInlineValue;
    CliOptionHandler handler;
} CliOption;

static bool handleHelp(CliParseContext* ctx, const char* value) {
    (void) value;
    ctx->options->helpRequested = true;
    return true;
}

static bool handleKeepPng(CliParseContext* ctx, const char* value) {
    (void) value;
    ctx->options->keepPng = true;
    return true;
}

static bool handleOutput(CliParseContext* ctx, const char* value) {
    if (!strNotEmpty(value) ||
        !pathCopy(ctx->options->outputDir, sizeof(ctx->options->outputDir), value)) {
        return false;
    }
    ctx->options->outputOrigin = PREPROCESS_PATH_EXPLICIT;
    return true;
}

static bool handleDebugOutput(CliParseContext* ctx, const char* value) {
    if (!strNotEmpty(value) ||
        !pathCopy(ctx->options->debugOutputDir, sizeof(ctx->options->debugOutputDir), value)) {
        return false;
    }
    ctx->options->dumpPagePreviews = true;
    return true;
}

static bool handleAtlasPageMode(CliParseContext* ctx, const char* value) {
    if (!parseAtlasPageModeLabel(value, &ctx->options->atlasPageMode)) return false;
    ctx->options->atlasPageModeExplicit = true;
    return true;
}

static bool handleWiiTarget(CliParseContext* ctx, const char* value) {
    (void) value;
    selectTarget(ctx->options, CINNAMON_PREPROCESSOR_TARGET_WII, ctx->textureFormatExplicit);
    return true;
}

static bool handleTarget(CliParseContext* ctx, const char* value) {
    CinnamonPreprocessorTarget target;
    if (!parseTarget(value, &target)) return false;
    selectTarget(ctx->options, target, ctx->textureFormatExplicit);
    return true;
}

static bool handleDumpPagePreviews(CliParseContext* ctx, const char* value) {
    (void) value;
    ctx->options->dumpPagePreviews = true;
    return true;
}

static bool handleChapter(CliParseContext* ctx, const char* value) {
    char* end;
    errno = 0;
    long chapter = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || chapter <= 0 || chapter > INT32_MAX)
        return false;
    ctx->options->chapterOverride = (int32_t) chapter;
    return true;
}

static bool handleTextureFormat(CliParseContext* ctx, const char* value) {
    if (!texParseFormat(value, &ctx->options->textureFormat)) return false;
    ctx->textureFormatExplicit = true;
    return true;
}

static const CliOption CLI_OPTIONS[] = {
    {"-h", false, false, handleHelp},
    {"--help", false, false, handleHelp},
    {"--keep-png", false, false, handleKeepPng},
    {"-o", true, false, handleOutput},
    {"--output", true, true, handleOutput},
    {"--debug-output", true, true, handleDebugOutput},
    {"--atlas-size", true, true, handleAtlasPageMode},
    {"--wii", false, false, handleWiiTarget},
    {"--target", true, true, handleTarget},
    {"--dump-page-previews", false, false, handleDumpPagePreviews},
    {"--chapter", true, false, handleChapter},
    {"--atlas-page-mode", true, true, handleAtlasPageMode},
    {"--texture-format", true, true, handleTextureFormat},
};

static const CliOption* findCliOption(const char* argument, const char** inlineValue) {
    const char* equals = strchr(argument, '=');
    size_t nameLength = equals != NULL ? (size_t) (equals - argument) : strlen(argument);
    *inlineValue = equals != NULL ? equals + 1 : NULL;
    preprocessRepeat(sizeof(CLI_OPTIONS) / sizeof(CLI_OPTIONS[0]), i) {
        const CliOption* option = &CLI_OPTIONS[i];
        if (strlen(option->name) == nameLength &&
            strncmp(argument, option->name, nameLength) == 0) {
            if (equals != NULL && !option->allowsInlineValue) return NULL;
            return option;
        }
    }
    return NULL;
}

bool parseArgs(int argc, char** argv, PreprocessUserOptions* out) {
    if (out == NULL) return false;
    initOptions(out);
    if (argc <= 1) return false;
    CliParseContext ctx = {.options = out};
    uint32_t positionalCount = 0u;
    for (int i = 1; i < argc; ++i) {
        const char* inlineValue = NULL;
        const CliOption* option = findCliOption(argv[i], &inlineValue);
        if (option != NULL) {
            const char* value = inlineValue;
            if (option->requiresValue && value == NULL) {
                if (++i >= argc) return false;
                value = argv[i];
            }
            if (!option->handler(&ctx, value)) return false;
            if (out->helpRequested) return true;
        } else if (argv[i][0] != '-' && positionalCount == 0u) {
            if (!pathCopy(out->inputPath, sizeof(out->inputPath), argv[i])) return false;
            positionalCount++;
        } else {
            return false;
        }
    }
    if (!targetAllowsFormat(out->target, out->textureFormat)) {
        fprintf(stderr, "%s texture format is not supported for %s preprocessing.\n",
            texFormatName(out->textureFormat), targetName(out->target));
        return false;
    }
    return out->inputPath[0] != '\0' && out->outputDir[0] != '\0';
}
