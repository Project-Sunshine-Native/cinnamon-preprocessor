#include "texture_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

typedef struct {
    const PreprocessRunConfig* options;
    const TexturePreprocessPipeline* pipeline;
    OutputPage* outputPages;
    uint32_t totalPageCount;
    uint32_t packedPageCount;
    uint32_t rgb565Pages;
    uint32_t rgba4444Pages;
    uint32_t etc1Pages;
    uint32_t etc1a4Pages;
    uint32_t l4Pages;
    uint32_t la4Pages;
    uint32_t ci4Pages;
    uint32_t ci8Pages;
    uint32_t cmprPages;
    uint32_t rgb5a3Pages;
    uint32_t monoRejectCount;
    uint32_t monoRejectFontCount;
    uint32_t monoRejectBackgroundCount;
    uint32_t monoRejectNonMonoCount;
    uint32_t monoRejectSamplePages[8];
    uint32_t monoRejectSampleCount;
} PageFlushContext;

static bool isAtlasPageSidecar(const char* name) {
    if (name == NULL || strncmp(name, "page_", 5u) != 0) return false;
    const char* dot = strrchr(name, '.');
    if (dot == NULL) return false;
    const char* allowed[] = {".t3x", ".ctx", ".ctex", ".png"};
    preprocessRepeat(sizeof(allowed) / sizeof(allowed[0]), i) {
        if (strEndsWithNoCase(name, allowed[i])) return true;
    }
    return false;
}

void texRemovePageFiles(const char* gfxDir) {
    if (gfxDir == NULL || !fsDirExists(gfxDir)) return;
#if PREPROCESS_HOST_WINDOWS
    char searchPath[PREPROCESS_SEARCH_PATH_MAX];
    WIN32_FIND_DATAA findData;
    if (!pathJoin(gfxDir, "page_*", searchPath, sizeof(searchPath))) return;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return;
    do {
        const char* name = findData.cFileName;
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (!isAtlasPageSidecar(name)) continue;
        char path[PREPROCESS_SEARCH_PATH_MAX];
        if (!pathJoin(gfxDir, name, path, sizeof(path))) continue;
        if (remove(path) != 0)
            fprintf(stderr, "Warning: failed to remove stale atlas page sidecar: %s\n", path);
    } while (FindNextFileA(findHandle, &findData));
    FindClose(findHandle);
#else
    DIR* dir = opendir(gfxDir);
    if (dir == NULL) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!isAtlasPageSidecar(entry->d_name)) continue;
        char* path = pathJoinAlloc(gfxDir, entry->d_name);
        if (!fsDirExists(path) && remove(path) != 0)
            fprintf(stderr, "Warning: failed to remove stale atlas page sidecar: %s\n", path);
        free(path);
    }
    closedir(dir);
#endif
}

static uint32_t freeMemoryBytes(void) {
    return UINT32_MAX;
}

static uint64_t residentPageBytes(PackedPage** packedPages, uint32_t packedPageCount) {
    uint64_t totalBytes = 0;
    preprocessRepeat(packedPageCount, i) {
        PackedPage* page = packedPages[i];
        if (page == NULL) continue;
        totalBytes += (uint64_t) page->width * (uint64_t) page->height * 4u;
    }
    return totalBytes;
}

bool texMaybeFlushPages(const PreprocessRunConfig* options,
    const TexturePreprocessPipeline* pipeline, AtlasPacking* atlas, uint32_t nextPageWidth,
    uint32_t nextPageHeight) {
    if (atlas->packedPages == NULL || atlas->packedPageCount == 0) return true;
    uint64_t residentBytes = residentPageBytes(atlas->packedPages, atlas->packedPageCount);
    uint64_t nextPageBytes = (uint64_t) nextPageWidth * (uint64_t) nextPageHeight * 4u;
    uint64_t residentLimit = CINNAMON_PREPROCESSOR_MAX_RESIDENT_PACKED_BYTES;
    uint64_t freeBytes = freeMemoryBytes();
    bool overResidentLimit = residentBytes + nextPageBytes > residentLimit;
    bool overMemoryBudget =
        freeBytes < residentBytes + nextPageBytes + CINNAMON_PREPROCESSOR_MEMORY_RESERVE;
    if (!overResidentLimit && !overMemoryBudget) return true;

    fprintf(stderr,
        "cinnamon-preprocessor: flushing packed pages early resident=%lu next=%lu free=%lu "
        "limit=%lu count=%lu\n",
        (unsigned long) (residentBytes > ULONG_MAX ? ULONG_MAX : residentBytes),
        (unsigned long) (nextPageBytes > ULONG_MAX ? ULONG_MAX : nextPageBytes),
        (unsigned long) (freeBytes > ULONG_MAX ? ULONG_MAX : freeBytes),
        (unsigned long) (residentLimit > ULONG_MAX ? ULONG_MAX : residentLimit),
        (unsigned long) atlas->packedPageCount);
    if (!texFlushPages(options, pipeline, atlas)) return false;
    atlasFreePages(atlas->packedPages, atlas->packedPageCount);
    atlas->packedPages = NULL;
    atlas->packedPageCount = 0;
    return true;
}

void texAddDebugName(OutputPage* page, const char* name) {
    if (page == NULL || name == NULL || name[0] == '\0') return;
    pathAddDebugName(page->debugNames, sizeof(page->debugNames), name);
    if (page->previewLabel[0] == '\0') {
        pathSanitizeLabel(page->previewLabel, sizeof(page->previewLabel), name);
    }
}

static bool pageCanUseMonoFormat(const OutputPage* page) {
    if (page == NULL) return false;
    return !page->containsBackground && !page->containsNonMonoContent;
}

static CinnamonTextureFormat choosePackedPageFormat(PageFlushContext* ctx,
    const PackedPage* packedPage, uint32_t pageIndex, OutputPage* outputPage) {
    const TextureRgbaRect source = {
        .rgba = packedPage->pixels,
        .sourceWidth = packedPage->width,
        .sourceHeight = packedPage->height,
        .width = packedPage->width,
        .height = packedPage->height,
    };
    CinnamonTextureFormat pageFormat = (CinnamonTextureFormat) outputPage->textureFormat;
    if (ctx->pipeline != NULL && ctx->pipeline->promoteCi8OverflowToRgb5a3 &&
        pageFormat == CINNAMON_TEXFMT_CI8 && !texCi8PaletteFits(&source)) {
        pageFormat = CINNAMON_TEXFMT_RGB5A3;
    } else if (ctx->pipeline != NULL && ctx->pipeline->demoteCi8ToCi4 &&
        pageFormat == CINNAMON_TEXFMT_CI8 && texCi4PaletteFits(&source)) {
        pageFormat = CINNAMON_TEXFMT_CI4;
    } else if ((pageFormat == CINNAMON_TEXFMT_L4 || pageFormat == CINNAMON_TEXFMT_LA4) &&
        !pageCanUseMonoFormat(outputPage)) {
        ctx->monoRejectCount++;
        if (outputPage->containsFont) ctx->monoRejectFontCount++;
        if (outputPage->containsBackground) ctx->monoRejectBackgroundCount++;
        if (outputPage->containsNonMonoContent) ctx->monoRejectNonMonoCount++;
        if (ctx->monoRejectSampleCount < 8u)
            ctx->monoRejectSamplePages[ctx->monoRejectSampleCount++] = pageIndex;
        pageFormat = CINNAMON_TEXFMT_RGBA4444;
    }

    outputPage->textureFormat = (uint32_t) pageFormat;
    snprintf(outputPage->path, sizeof(outputPage->path), "page_%03u.%s", pageIndex,
        texExtension(pageFormat));
    return pageFormat;
}

static bool buildPackedPagePaths(const PageFlushContext* ctx, const OutputPage* outputPage,
    uint32_t pageIndex, char* pagePath, size_t pagePathSize, char* previewPath,
    size_t previewPathSize) {
    char gfxDir[PREPROCESS_PATH_MAX];
    if (!pathJoin(ctx->options->outputDir, "gfx", gfxDir, sizeof(gfxDir)) ||
        !pathJoin(gfxDir, outputPage->path, pagePath, pagePathSize)) {
        fprintf(
            stderr, "cinnamon-preprocessor: packed page %u output path is too long.\n", pageIndex);
        return false;
    }

    const char* previewLabel =
        outputPage->previewLabel[0] != '\0' ? outputPage->previewLabel : "unnamed";
    char previewName[PREPROCESS_PATH_MAX];
    int previewNameLen =
        snprintf(previewName, sizeof(previewName), "page_%03u__%s.png", pageIndex, previewLabel);
    if (previewNameLen < 0 || (size_t) previewNameLen >= sizeof(previewName)) return false;

    const char* previewParent = gfxDir;
    if (strNotEmpty(ctx->options->debugOutputDir)) {
        char previewDir[PREPROCESS_PATH_MAX];
        if (!pathJoin(
                ctx->options->debugOutputDir, "atlas_debug", previewDir, sizeof(previewDir))) {
            fprintf(stderr,
                "cinnamon-preprocessor: packed page %u preview directory path is too long.\n",
                pageIndex);
            return false;
        }
        fsEnsureDir(previewDir);
        if (!pathJoin(previewDir, previewName, previewPath, previewPathSize)) {
            fprintf(stderr, "cinnamon-preprocessor: packed page %u preview path is too long.\n",
                pageIndex);
            return false;
        }
        return true;
    }
    return pathJoin(previewParent, previewName, previewPath, previewPathSize);
}

static void countPackedPageFormat(PageFlushContext* ctx, CinnamonTextureFormat pageFormat) {
    switch (pageFormat) {
    case CINNAMON_TEXFMT_RGB565:
        ctx->rgb565Pages++;
        break;
    case CINNAMON_TEXFMT_RGBA4444:
        ctx->rgba4444Pages++;
        break;
    case CINNAMON_TEXFMT_ETC1:
        ctx->etc1Pages++;
        break;
    case CINNAMON_TEXFMT_ETC1A4:
        ctx->etc1a4Pages++;
        break;
    case CINNAMON_TEXFMT_L4:
        ctx->l4Pages++;
        break;
    case CINNAMON_TEXFMT_LA4:
        ctx->la4Pages++;
        break;
    case CINNAMON_TEXFMT_CI4:
        ctx->ci4Pages++;
        break;
    case CINNAMON_TEXFMT_CI8:
        ctx->ci8Pages++;
        break;
    case CINNAMON_TEXFMT_CMPR:
        ctx->cmprPages++;
        break;
    case CINNAMON_TEXFMT_RGB5A3:
        ctx->rgb5a3Pages++;
        break;
    default:
        break;
    }
}

static bool flushPackedPage(PageFlushContext* ctx, PackedPage* packedPage, size_t i) {
    if (packedPage == NULL) {
        fprintf(
            stderr, "cinnamon-preprocessor: packed page slot %lu is NULL.\n", (unsigned long) i);
        return false;
    }
    uint32_t pageIndex = packedPage->pageIndex;
    if (pageIndex >= ctx->totalPageCount) {
        fprintf(stderr,
            "cinnamon-preprocessor: packed page slot %lu has invalid page index %u (total "
            "pages=%u).\n",
            (unsigned long) i, pageIndex, ctx->totalPageCount);
        return false;
    }
    if (packedPage->pixels == NULL || packedPage->width == 0u || packedPage->height == 0u) {
        fprintf(stderr,
            "cinnamon-preprocessor: packed page %u has invalid pixels/dimensions (%ux%u "
            "pixels=%p).\n",
            pageIndex, packedPage->width, packedPage->height, (void*) packedPage->pixels);
        return false;
    }
    OutputPage* outputPage = &ctx->outputPages[pageIndex];
    CinnamonTextureFormat pageFormat =
        choosePackedPageFormat(ctx, packedPage, pageIndex, outputPage);
    if (!targetAllowsFormat(ctx->options->user.target, pageFormat)) {
        fprintf(stderr, "%s atlas page format is not supported for %s preprocessing (page_%03u).\n",
            texFormatName(pageFormat), targetName(ctx->options->user.target), pageIndex);
        return false;
    }
    char pagePath[PREPROCESS_PATH_MAX];
    char previewPath[PREPROCESS_PATH_MAX];
    if (!buildPackedPagePaths(ctx, outputPage, pageIndex, pagePath, sizeof(pagePath), previewPath,
            sizeof(previewPath)))
        return false;

    if (!texWriteRegions(packedPage->pixels, packedPage->width, packedPage->height, pagePath,
            pageFormat, NULL, 0u)) {
        return false;
    }
    if (ctx->options->user.dumpPagePreviews) {
        stbi_write_png(previewPath, packedPage->width, packedPage->height, 4, packedPage->pixels,
            packedPage->width * 4);
    }
    countPackedPageFormat(ctx, pageFormat);
    fprintf(stderr, "Wrote packed atlas page %u (%s) -> %s\n", pageIndex, texFormatName(pageFormat),
        pagePath);
    return true;
}

static void logFlushedPages(const PageFlushContext* ctx) {
    if (ctx->packedPageCount > 0) {
        fprintf(stderr,
            "cinnamon-preprocessor: atlas page formats: rgb565=%u rgba4444=%u etc1=%u "
            "etc1a4=%u l4=%u la4=%u ci4=%u ci8=%u cmpr=%u rgb5a3=%u total=%u\n",
            ctx->rgb565Pages, ctx->rgba4444Pages, ctx->etc1Pages, ctx->etc1a4Pages, ctx->l4Pages,
            ctx->la4Pages, ctx->ci4Pages, ctx->ci8Pages, ctx->cmprPages, ctx->rgb5a3Pages,
            ctx->packedPageCount);
        if (ctx->monoRejectCount > 0) {
            fprintf(stderr,
                "cinnamon-preprocessor: monochrome format fallbacks: total=%u font=%u "
                "background=%u "
                "nonMono=%u",
                ctx->monoRejectCount, ctx->monoRejectFontCount, ctx->monoRejectBackgroundCount,
                ctx->monoRejectNonMonoCount);
            if (ctx->monoRejectSampleCount > 0) {
                fprintf(stderr, " samplePages=");
                preprocessRepeat(ctx->monoRejectSampleCount, i) {
                    fprintf(stderr, "%s%03u", i == 0 ? "" : ",", ctx->monoRejectSamplePages[i]);
                }
            }
            fprintf(stderr, "\n");
        }
    }
}

bool texFlushPages(const PreprocessRunConfig* options, const TexturePreprocessPipeline* pipeline,
    const AtlasPacking* atlas) {
    if (atlas->packedPageCount == 0) return true;
    if (options == NULL || atlas->outputPages == NULL || atlas->packedPages == NULL) {
        fprintf(stderr, "cinnamon-preprocessor: invalid packed page flush state.\n");
        return false;
    }
    PageFlushContext ctx = {
        .options = options,
        .pipeline = pipeline,
        .outputPages = atlas->outputPages,
        .totalPageCount = atlas->totalPageCount,
        .packedPageCount = atlas->packedPageCount,
    };
    preprocessRepeat(atlas->packedPageCount, i) {
        if (!flushPackedPage(&ctx, atlas->packedPages[i], i)) return false;
    }
    logFlushedPages(&ctx);
    return true;
}
