#include "texture_convert_internal.h"
#include "common/path_utils.h"

typedef struct {
    const PreprocessRunConfig* options;
    DataWin* dataWin;
    const TexturePreprocessPipeline* pipeline;
    bool gm2022_5;
    AtlasPacking atlas;
    OutputItem* items;
    OutputFragment* fragments;
    TileLookupKey* tileRequests;
    size_t tileRequestCount;
    RoomAtlasAffinity roomAtlasAffinity;
    OutputTileEntry* tileEntriesByRequest;
    bool* tileEntryWritten;
    PreprocessIndexList* pageItemLists;
    PreprocessIndexList* pageTileRequestLists;
    OutputTileEntry* packedTileEntries;
    bool* usedTPAGs;
    bool* fontTPAGs;
    bool* backgroundTPAGs;
    char** tpagDebugNames;
    int32_t* tpagToSpriteIndex;
    uint8_t* tpagBindingRows;
    uint32_t tpagBindingCount;
} TextureConversionContext;

bool texConvert(const PreprocessRunConfig* options, DataWin* dataWin) {
    if (options == NULL) return false;
    const PreprocessTargetDescriptor* desc = targetInfo(options->user.target);
    return desc != NULL && texConvertPipeline(options, dataWin, &desc->texturePipeline);
}

static void destroyTextureContext(TextureConversionContext* ctx) {
    free(ctx->items);
    free(ctx->atlas.outputPages);
    atlasFreeAffinity(&ctx->roomAtlasAffinity);
    arrfree(ctx->fragments);
    arrfree(ctx->tileRequests);
    free(ctx->tileEntriesByRequest);
    free(ctx->tileEntryWritten);
    texFreeIndexLists(ctx->pageItemLists, ctx->dataWin->txtr.count);
    texFreeIndexLists(ctx->pageTileRequestLists, ctx->dataWin->txtr.count);
    free(ctx->tpagBindingRows);
    free(ctx->usedTPAGs);
    free(ctx->fontTPAGs);
    free(ctx->backgroundTPAGs);
    freeTpagNames(ctx->tpagDebugNames, ctx->dataWin->tpag.count);
    free(ctx->tpagToSpriteIndex);
    arrfree(ctx->packedTileEntries);
    atlasFreePages(ctx->atlas.packedPages, ctx->atlas.packedPageCount);
}

static bool initTextureContext(TextureConversionContext* ctx) {
    ctx->gm2022_5 = DataWin_isVersionAtLeast(ctx->dataWin, 2022, 5, 0, 0);

    ctx->items = safeCalloc(ctx->dataWin->tpag.count, sizeof(OutputItem));

    ctx->tileRequests = collectLegacyTiles(ctx->dataWin);
    ctx->tileRequestCount = arrlen(ctx->tileRequests);

    ctx->tileEntriesByRequest =
        safeCalloc(ctx->tileRequestCount > 0 ? ctx->tileRequestCount : 1u, sizeof(OutputTileEntry));
    ctx->tileEntryWritten =
        safeCalloc(ctx->tileRequestCount > 0 ? ctx->tileRequestCount : 1u, sizeof(bool));

    ctx->usedTPAGs = collectUsedTpags(ctx->dataWin);
    ctx->fontTPAGs = collectFontTpags(ctx->dataWin);
    ctx->backgroundTPAGs = collectBackgroundTpags(ctx->dataWin);
    ctx->tpagDebugNames = collectTpagNames(ctx->dataWin);
    ctx->tpagToSpriteIndex = mapTpagSprites(ctx->dataWin);
    if (!texBuildWorkLists(ctx->dataWin, ctx->usedTPAGs, ctx->tileRequests, ctx->tileRequestCount,
            &ctx->pageItemLists, &ctx->pageTileRequestLists)) {
        return false;
    }
    if (!atlasBuildAffinity(
            ctx->dataWin, ctx->tileRequests, ctx->tileRequestCount, &ctx->roomAtlasAffinity)) {
        return false;
    }
    if (!texSortWorkLists(ctx->dataWin, ctx->tileRequests, ctx->pageItemLists,
            ctx->pageTileRequestLists, &ctx->roomAtlasAffinity)) {
        return false;
    }

    return true;
}

static bool flushTexturePagesForRect(TextureConversionContext* ctx, uint32_t width, uint32_t height,
    CinnamonTextureFormat format, bool isFont, uint32_t minAutoPageSize) {
    uint32_t padding = atlasPadding(format);
    uint32_t nextPageWidth = 0u;
    uint32_t nextPageHeight = 0u;
    atlasPageDimensions(ctx->options, width + padding * 2u, height + padding * 2u, isFont,
        minAutoPageSize, &nextPageWidth, &nextPageHeight);
    return texMaybeFlushPages(
        ctx->options, ctx->pipeline, &ctx->atlas, nextPageWidth, nextPageHeight);
}

typedef struct {
    uint32_t itemIndex;
    const TexturePageItem* item;
    const uint8_t* pixels;
    uint32_t pixelsWidth;
    uint32_t pixelsHeight;
    uint32_t width;
    uint32_t height;
    CinnamonTextureFormat pageFormat;
    bool isFont;
    bool isBackground;
    bool monoSafe;
    const uint64_t* roomMask;
} TextureItemPacking;

static uint32_t itemChunkSize(
    const TextureConversionContext* ctx, const TextureItemPacking* work, uint32_t minAutoPageSize) {
    uint32_t padding = atlasPadding(work->pageFormat);
    uint32_t chunkSize = atlasPageSize(ctx->options, work->width + padding * 2u,
        work->height + padding * 2u, work->isFont, minAutoPageSize);
    if (padding > 0u && chunkSize > padding * 2u) chunkSize -= padding * 2u;
    if (chunkSize == 0u) chunkSize = 1u;
    return chunkSize;
}

static bool fragmentTextureItem(TextureConversionContext* ctx, const TextureItemPacking* work) {
    uint32_t minAutoPageSize = (work->isFont || work->isBackground)
        ? CINNAMON_SMALL_TEXTURE_SIZE
        : CINNAMON_MIN_ATLAS_TEXTURE_SIZE;
    uint32_t fragmentStart = arrlen(ctx->fragments);
    uint32_t chunkSize = itemChunkSize(ctx, work, minAutoPageSize);

    for (uint32_t chunkY = 0; chunkY < work->height; chunkY += chunkSize) {
        for (uint32_t chunkX = 0; chunkX < work->width; chunkX += chunkSize) {
            uint32_t fragWidth = work->width - chunkX;
            uint32_t fragHeight = work->height - chunkY;
            if (fragWidth > chunkSize) fragWidth = chunkSize;
            if (fragHeight > chunkSize) fragHeight = chunkSize;

            AtlasPackRequest request = {
                .width = fragWidth,
                .height = fragHeight,
                .format = work->pageFormat,
                .allowLargePage = work->isFont,
                .minAutoPageSize = minAutoPageSize,
                .roomMask = work->roomMask,
                .roomMaskWords = ctx->roomAtlasAffinity.roomMaskWords,
            };
            AtlasPlacement placement;
            if (!flushTexturePagesForRect(ctx, request.width, request.height, request.format,
                    request.allowLargePage, request.minAutoPageSize) ||
                !atlasPackRect(ctx->options, &ctx->atlas, &request, &placement)) {
                fprintf(stderr, "Fragment pack failed for TPAG item %u\n", work->itemIndex);
                return false;
            }

            PackedPage* dstPage = atlasFindPage(
                ctx->atlas.packedPages, ctx->atlas.packedPageCount, placement.pageIndex);
            if (dstPage == NULL) {
                fprintf(stderr, "Packed page lookup failed for TPAG item %u page %u\n",
                    work->itemIndex, placement.pageIndex);
                return false;
            }
            OutputPage* outputPage = &ctx->atlas.outputPages[placement.pageIndex];
            if (ctx->tpagDebugNames != NULL && ctx->tpagDebugNames[work->itemIndex] != NULL) {
                texAddDebugName(outputPage, ctx->tpagDebugNames[work->itemIndex]);
            }
            if (ctx->tpagToSpriteIndex != NULL && ctx->tpagToSpriteIndex[work->itemIndex] >= 0) {
                outputPage->containsSprite = true;
            }
            if (work->isFont) outputPage->containsFont = true;
            if (work->isBackground) outputPage->containsBackground = true;
            if (!work->monoSafe) outputPage->containsNonMonoContent = true;

            uint32_t copySrcX = (uint32_t) work->item->sourceX + chunkX;
            uint32_t copySrcY = (uint32_t) work->item->sourceY + chunkY;
            if (!texBlitClipped(dstPage->pixels, dstPage->width, dstPage->height, placement.x,
                    placement.y, work->pixels, work->pixelsWidth, work->pixelsHeight,
                    (int32_t) copySrcX, (int32_t) copySrcY, (int32_t) fragWidth,
                    (int32_t) fragHeight)) {
                fprintf(stderr,
                    "Failed to copy TPAG item %u fragment (%u,%u %ux%u) to page %u at (%u,%u).\n",
                    work->itemIndex, copySrcX, copySrcY, fragWidth, fragHeight, placement.pageIndex,
                    placement.x, placement.y);
                return false;
            }
            texPadBorder(dstPage->pixels, dstPage->width, dstPage->height, placement.x, placement.y,
                fragWidth, fragHeight, placement.padding);

            OutputFragment fragment = {
                .atlasId = placement.pageIndex,
                .x = placement.x,
                .y = placement.y,
                .width = (uint16_t) fragWidth,
                .height = (uint16_t) fragHeight,
                .sourceX = (uint16_t) chunkX,
                .sourceY = (uint16_t) chunkY,
            };
            arrput(ctx->fragments, fragment);
        }
    }

    ctx->items[work->itemIndex].width = (uint16_t) work->width;
    ctx->items[work->itemIndex].height = (uint16_t) work->height;
    ctx->items[work->itemIndex].fragmentStart = fragmentStart;
    ctx->items[work->itemIndex].fragmentCount = (uint16_t) (arrlen(ctx->fragments) - fragmentStart);
    return true;
}

static bool processTextureItem(
    TextureConversionContext* ctx, uint32_t itemIndex, const uint8_t* rgba, int width, int height) {
    const TexturePageItem* item = &ctx->dataWin->tpag.items[itemIndex];
    if (item->sourceWidth == 0u || item->sourceHeight == 0u) {
        ctx->items[itemIndex] = (OutputItem){0};
        return true;
    }

    bool isFont = ctx->fontTPAGs != NULL && ctx->fontTPAGs[itemIndex];
    bool isBackground = ctx->backgroundTPAGs != NULL && ctx->backgroundTPAGs[itemIndex];
    const TextureRgbaRect source = {
        .rgba = rgba,
        .sourceWidth = (uint32_t) width,
        .sourceHeight = (uint32_t) height,
        .sourceX = item->sourceX,
        .sourceY = item->sourceY,
        .width = item->sourceWidth,
        .height = item->sourceHeight,
    };
    bool monoSafe = texRectIsMono(&source);
    CinnamonTextureFormat pageFormat = ctx->options->textureFormat;
    if (isFont && ctx->pipeline->useLa4ForNormalFonts && texTpagIsFont(ctx->dataWin, itemIndex)) {
        pageFormat = CINNAMON_TEXFMT_LA4;
        monoSafe = true;
    }

    const TextureItemPacking packing = {
        .itemIndex = itemIndex,
        .item = item,
        .pixels = rgba,
        .pixelsWidth = (uint32_t) width,
        .pixelsHeight = (uint32_t) height,
        .width = item->sourceWidth,
        .height = item->sourceHeight,
        .pageFormat = pageFormat,
        .isFont = isFont,
        .isBackground = isBackground,
        .monoSafe = monoSafe,
        .roomMask = atlasTpagMask(&ctx->roomAtlasAffinity, itemIndex),
    };
    return fragmentTextureItem(ctx, &packing);
}

static uint8_t* buildTextureTilePixels(TileLookupKey key, const TexturePageItem* item,
    const uint8_t* tileRgba, int tileWidth, int tileHeight) {
    size_t tileBytes = 0u;
    if (!texRgbaBytes(key.srcW, key.srcH, &tileBytes)) {
        fprintf(stderr, "Failed to size tile scratch bg=%d src=(%u,%u %ux%u)\n", key.bgDef,
            key.srcX, key.srcY, key.srcW, key.srcH);
        return NULL;
    }
    uint8_t* tilePixels = calloc(1u, tileBytes);
    if (tilePixels == NULL) {
        fprintf(stderr, "Failed to allocate tile scratch bg=%d src=(%u,%u %ux%u)\n", key.bgDef,
            key.srcX, key.srcY, key.srcW, key.srcH);
        return NULL;
    }

    uint32_t bgLogicalW = item->boundingWidth > 0 ? item->boundingWidth : item->sourceWidth;
    uint32_t bgLogicalH = item->boundingHeight > 0 ? item->boundingHeight : item->sourceHeight;
    if (bgLogicalW > 0 && bgLogicalH > 0 && item->sourceWidth > 0 && item->sourceHeight > 0) {
        int32_t reqLeft = (int32_t) key.srcX;
        int32_t reqTop = (int32_t) key.srcY;
        int32_t reqRight = reqLeft + (int32_t) key.srcW;
        int32_t reqBottom = reqTop + (int32_t) key.srcH;
        int32_t itemLeft = (int32_t) item->targetX;
        int32_t itemTop = (int32_t) item->targetY;
        int32_t itemRight = itemLeft + (int32_t) item->sourceWidth;
        int32_t itemBottom = itemTop + (int32_t) item->sourceHeight;
        int32_t overlapLeft = reqLeft > itemLeft ? reqLeft : itemLeft;
        int32_t overlapTop = reqTop > itemTop ? reqTop : itemTop;
        int32_t overlapRight = reqRight < itemRight ? reqRight : itemRight;
        int32_t overlapBottom = reqBottom < itemBottom ? reqBottom : itemBottom;
        if (overlapLeft < overlapRight && overlapTop < overlapBottom &&
            !texBlitClipped(tilePixels, key.srcW, key.srcH, overlapLeft - reqLeft,
                overlapTop - reqTop, tileRgba, (uint32_t) tileWidth, (uint32_t) tileHeight,
                (int32_t) item->sourceX + (overlapLeft - itemLeft),
                (int32_t) item->sourceY + (overlapTop - itemTop), overlapRight - overlapLeft,
                overlapBottom - overlapTop)) {
            fprintf(stderr, "Failed to copy tile overlap bg=%d src=(%u,%u %ux%u).\n", key.bgDef,
                key.srcX, key.srcY, key.srcW, key.srcH);
            free(tilePixels);
            return NULL;
        }
    }

    if ((uint32_t) key.srcX < item->sourceWidth && (uint32_t) key.srcY < item->sourceHeight) {
        uint32_t copyW = key.srcW;
        uint32_t copyH = key.srcH;
        if ((uint32_t) key.srcX + copyW > item->sourceWidth)
            copyW = item->sourceWidth - (uint32_t) key.srcX;
        if ((uint32_t) key.srcY + copyH > item->sourceHeight)
            copyH = item->sourceHeight - (uint32_t) key.srcY;
        uint32_t logicalCoverage = texCountOpaque(tilePixels, key.srcW, key.srcH);
        uint32_t contentCoverage = texCountOpaqueRect(&(TextureRgbaRect){.rgba = tileRgba,
            .sourceWidth = (uint32_t) tileWidth,
            .sourceHeight = (uint32_t) tileHeight,
            .sourceX = (uint32_t) item->sourceX + (uint32_t) key.srcX,
            .sourceY = (uint32_t) item->sourceY + (uint32_t) key.srcY,
            .width = copyW,
            .height = copyH});
        if (contentCoverage > logicalCoverage) {
            memset(tilePixels, 0, tileBytes);
            if (!texBlitClipped(tilePixels, key.srcW, key.srcH, 0, 0, tileRgba,
                    (uint32_t) tileWidth, (uint32_t) tileHeight,
                    (int32_t) item->sourceX + (int32_t) key.srcX,
                    (int32_t) item->sourceY + (int32_t) key.srcY, (int32_t) copyW,
                    (int32_t) copyH)) {
                fprintf(stderr, "Failed to copy tile source correction bg=%d src=(%u,%u %ux%u).\n",
                    key.bgDef, key.srcX, key.srcY, copyW, copyH);
                free(tilePixels);
                return NULL;
            }
        }
    }
    return tilePixels;
}

static void markTextureTilePage(
    TextureConversionContext* ctx, uint16_t pageIndex, const Background* background) {
    texAddDebugName(&ctx->atlas.outputPages[pageIndex], background->name);
    ctx->atlas.outputPages[pageIndex].containsBackground = true;
    ctx->atlas.outputPages[pageIndex].containsNonMonoContent = true;
}

static bool writeTileFragments(TextureConversionContext* ctx, uint32_t reqIndex, TileLookupKey key,
    const Background* background, const uint8_t* tilePixels, const uint64_t* tileRoomMask) {
    uint32_t fragmentStart = arrlen(ctx->fragments);
    uint32_t tileAtlasPadding = atlasPadding(ctx->options->textureFormat);
    uint32_t tileChunkSize = atlasPageSize(ctx->options, key.srcW + tileAtlasPadding * 2u,
        key.srcH + tileAtlasPadding * 2u, false, CINNAMON_SMALL_TEXTURE_SIZE);
    if (tileAtlasPadding > 0u && tileChunkSize > tileAtlasPadding * 2u)
        tileChunkSize -= tileAtlasPadding * 2u;
    if (tileChunkSize == 0u) tileChunkSize = 1u;

    for (uint32_t chunkY = 0; chunkY < key.srcH; chunkY += tileChunkSize) {
        uint32_t fragHeight = key.srcH - chunkY;
        if (fragHeight > tileChunkSize) fragHeight = tileChunkSize;
        for (uint32_t chunkX = 0; chunkX < key.srcW; chunkX += tileChunkSize) {
            uint32_t fragWidth = key.srcW - chunkX;
            if (fragWidth > tileChunkSize) fragWidth = tileChunkSize;
            if (!flushTexturePagesForRect(ctx, fragWidth, fragHeight, ctx->options->textureFormat,
                    false, CINNAMON_SMALL_TEXTURE_SIZE)) {
                fprintf(stderr,
                    "Failed to flush packed pages before tile fragment bg=%d src=(%u,%u %ux%u)\n",
                    key.bgDef, key.srcX, key.srcY, key.srcW, key.srcH);
                return false;
            }

            AtlasPackRequest request = {
                .width = fragWidth,
                .height = fragHeight,
                .format = ctx->options->textureFormat,
                .minAutoPageSize = CINNAMON_SMALL_TEXTURE_SIZE,
                .roomMask = tileRoomMask,
                .roomMaskWords = ctx->roomAtlasAffinity.roomMaskWords,
            };
            AtlasPlacement placement;
            if (!atlasPackRect(ctx->options, &ctx->atlas, &request, &placement)) {
                fprintf(stderr, "Failed to pack tile fragment bg=%d at (%u,%u %ux%u)\n", key.bgDef,
                    chunkX, chunkY, fragWidth, fragHeight);
                return false;
            }

            PackedPage* dstPage = atlasFindPage(
                ctx->atlas.packedPages, ctx->atlas.packedPageCount, placement.pageIndex);
            if (dstPage == NULL) {
                fprintf(stderr, "Packed page lookup failed for tile fragment bg=%d page %u\n",
                    key.bgDef, placement.pageIndex);
                return false;
            }
            markTextureTilePage(ctx, placement.pageIndex, background);
            if (!texRectFits(dstPage->width, dstPage->height, placement.x, placement.y, fragWidth,
                    fragHeight) ||
                !texRectFits(key.srcW, key.srcH, chunkX, chunkY, fragWidth, fragHeight)) {
                fprintf(stderr, "Failed to copy tile fragment bg=%d to page %u at (%u,%u %ux%u).\n",
                    key.bgDef, placement.pageIndex, placement.x, placement.y, fragWidth,
                    fragHeight);
                return false;
            }
            texBlit(dstPage->pixels, dstPage->width, placement.x, placement.y, tilePixels, key.srcW,
                chunkX, chunkY, fragWidth, fragHeight);
            texPadBorder(dstPage->pixels, dstPage->width, dstPage->height, placement.x, placement.y,
                fragWidth, fragHeight, placement.padding);

            OutputFragment fragment = {
                .atlasId = placement.pageIndex,
                .x = placement.x,
                .y = placement.y,
                .width = (uint16_t) fragWidth,
                .height = (uint16_t) fragHeight,
                .sourceX = (uint16_t) chunkX,
                .sourceY = (uint16_t) chunkY,
            };
            arrput(ctx->fragments, fragment);
        }
    }

    ctx->tileEntriesByRequest[reqIndex] = (OutputTileEntry){
        .bgDef = key.bgDef,
        .srcX = key.srcX,
        .srcY = key.srcY,
        .srcW = key.srcW,
        .srcH = key.srcH,
        .atlasId = UINT16_MAX,
        .fragmentStart = fragmentStart,
        .fragmentCount = (uint16_t) (arrlen(ctx->fragments) - fragmentStart),
    };
    return true;
}

static bool processTextureTile(TextureConversionContext* ctx, uint32_t reqIndex,
    const uint8_t* tileRgba, int tileWidth, int tileHeight) {
    uint8_t* tilePixels = NULL;
    bool ok = false;
    if (ctx->tileEntryWritten[reqIndex]) return true;
    TileLookupKey key = ctx->tileRequests[reqIndex];
    if (key.bgDef < 0 || (uint32_t) key.bgDef >= ctx->dataWin->bgnd.count) return true;

    Background* bg = &ctx->dataWin->bgnd.backgrounds[key.bgDef];
    if (bg->tpagIndex < 0 || (uint32_t) bg->tpagIndex >= ctx->dataWin->tpag.count) return true;

    TexturePageItem* item = &ctx->dataWin->tpag.items[bg->tpagIndex];

    if (key.srcW == 0 || key.srcH == 0 || key.srcW > CINNAMON_LARGE_TEXTURE_SIZE ||
        key.srcH > CINNAMON_LARGE_TEXTURE_SIZE) {
        fprintf(stderr,
            "Warning: skipping oversized tile rect bg=%d src=(%u,%u %ux%u) "
            "(exceeds max atlas page size %u)\n",
            key.bgDef, key.srcX, key.srcY, key.srcW, key.srcH, CINNAMON_LARGE_TEXTURE_SIZE);
        ctx->tileEntryWritten[reqIndex] = true;
        ok = true;
        goto cleanup;
    }

    tilePixels = buildTextureTilePixels(key, item, tileRgba, tileWidth, tileHeight);
    if (tilePixels == NULL) goto cleanup;

    AtlasPlacement placement;
    const uint64_t* tileRoomMask = atlasTileMask(&ctx->roomAtlasAffinity, (uint32_t) reqIndex);
    if (!flushTexturePagesForRect(ctx, key.srcW, key.srcH, ctx->options->textureFormat, false,
            CINNAMON_SMALL_TEXTURE_SIZE)) {
        fprintf(stderr, "Failed to flush packed pages before tile rect bg=%d src=(%u,%u %ux%u)\n",
            key.bgDef, key.srcX, key.srcY, key.srcW, key.srcH);
        goto cleanup;
    }
    AtlasPackRequest request = {
        .width = key.srcW,
        .height = key.srcH,
        .format = ctx->options->textureFormat,
        .minAutoPageSize = CINNAMON_SMALL_TEXTURE_SIZE,
        .roomMask = tileRoomMask,
        .roomMaskWords = ctx->roomAtlasAffinity.roomMaskWords,
    };
    if (atlasPackRect(ctx->options, &ctx->atlas, &request, &placement)) {
        PackedPage* dstPage =
            atlasFindPage(ctx->atlas.packedPages, ctx->atlas.packedPageCount, placement.pageIndex);
        if (dstPage == NULL) {
            fprintf(stderr, "Packed page lookup failed for tile bg=%d page %u\n", key.bgDef,
                placement.pageIndex);
            goto cleanup;
        }
        markTextureTilePage(ctx, placement.pageIndex, bg);
        if (!texRectFits(
                dstPage->width, dstPage->height, placement.x, placement.y, key.srcW, key.srcH)) {
            fprintf(stderr, "Failed to copy tile bg=%d to page %u at (%u,%u %ux%u).\n", key.bgDef,
                placement.pageIndex, placement.x, placement.y, key.srcW, key.srcH);
            goto cleanup;
        }
        texBlit(dstPage->pixels, dstPage->width, placement.x, placement.y, tilePixels, key.srcW, 0,
            0, key.srcW, key.srcH);
        texPadBorder(dstPage->pixels, dstPage->width, dstPage->height, placement.x, placement.y,
            key.srcW, key.srcH, placement.padding);

        ctx->tileEntriesByRequest[reqIndex] = (OutputTileEntry){
            .bgDef = key.bgDef,
            .srcX = key.srcX,
            .srcY = key.srcY,
            .srcW = key.srcW,
            .srcH = key.srcH,
            .atlasId = placement.pageIndex,
            .x = placement.x,
            .y = placement.y,
            .width = key.srcW,
            .height = key.srcH,
            .fragmentStart = UINT32_MAX,
            .fragmentCount = 0,
        };
    } else if (!writeTileFragments(ctx, reqIndex, key, bg, tilePixels, tileRoomMask)) {
        goto cleanup;
    }

    ctx->tileEntryWritten[reqIndex] = true;
    ok = true;
cleanup:
    free(tilePixels);
    return ok;
}

static bool processTexturePage(TextureConversionContext* ctx, size_t i) {
    bool ok = false;
    uint8_t* blobCopy = NULL;
    uint8_t* rgba = NULL;
    uint8_t* tileRgba = NULL;

    progressPage((uint32_t) i, ctx->dataWin->txtr.count);

    bool pageHasItems = arrlen(ctx->pageItemLists[i].indices) > 0;
    bool pageHasTileRequests = arrlen(ctx->pageTileRequestLists[i].indices) > 0;
    if (!pageHasItems && !pageHasTileRequests) return true;

    Texture* texture = &ctx->dataWin->txtr.textures[i];
    fprintf(stderr, "cinnamon-preprocessor: TXTR page %zu blobOffset=0x%x blobSize=%u\n", i,
        texture->blobOffset, texture->blobSize);

    /* Reload pages on demand so only one decoded texture stays resident. */
    char textureSourcePath[PREPROCESS_TEMP_PATH_MAX];
    uint32_t blobCopySize = 0u;
    blobCopy = texReadBlob(ctx->options, ctx->dataWin, (uint32_t) i, texture, &blobCopySize,
        textureSourcePath, sizeof(textureSourcePath));
    if (blobCopy == NULL) {
        if (texture->blobOffset == 0u || texture->blobSize == 0u) {
            fprintf(stderr, "Failed to resolve external TXTR page %zu near %s\n", i,
                ctx->options->inputPath);
        } else {
            fprintf(stderr, "Failed to reload TXTR page %zu from %s at 0x%x (%u bytes)\n", i,
                ctx->options->inputPath, texture->blobOffset, texture->blobSize);
        }
        goto cleanup;
    }
    int pngInfoWidth = 0;
    int pngInfoHeight = 0;
    int pngInfoChannels = 0;
    bool blobIsPng = blobCopySize >= 8u && blobCopy[0] == 0x89u && blobCopy[1] == 0x50u &&
        blobCopy[2] == 0x4eu && blobCopy[3] == 0x47u;
    if (blobIsPng &&
        stbi_info_from_memory(
            blobCopy, (int) blobCopySize, &pngInfoWidth, &pngInfoHeight, &pngInfoChannels)) {
        uint64_t rgbaBytes = (uint64_t) pngInfoWidth * (uint64_t) pngInfoHeight * 4u;
        fprintf(stderr,
            "cinnamon-preprocessor: TXTR page %zu PNG info %dx%d channels=%d rgbaBytes=%llu\n", i,
            pngInfoWidth, pngInfoHeight, pngInfoChannels, (unsigned long long) rgbaBytes);
    }
    int width = 0;
    int height = 0;
    rgba = ImageDecoder_decodeToRgba(blobCopy, blobCopySize, ctx->gm2022_5, &width, &height);
    if (rgba == NULL) {
        const uint8_t* b = blobCopy;
        fprintf(stderr,
            "Failed to decode TXTR page %zu from %s (blobSize=%u magic=%02x%02x%02x%02x "
            "png=%dx%d/%d stbi=%s)\n",
            i, textureSourcePath[0] != '\0' ? textureSourcePath : ctx->options->inputPath,
            blobCopySize, b && blobCopySize > 0 ? b[0] : 0, b && blobCopySize > 1 ? b[1] : 0,
            b && blobCopySize > 2 ? b[2] : 0, b && blobCopySize > 3 ? b[3] : 0, pngInfoWidth,
            pngInfoHeight, pngInfoChannels, stbi_failure_reason());
        goto cleanup;
    }
    free(blobCopy);
    blobCopy = NULL;
    blobCopySize = 0;

    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        fprintf(stderr, "Invalid decoded size for TXTR page %zu: %dx%d\n", i, width, height);
        goto cleanup;
    }

    uint32_t* pageItemIndices = ctx->pageItemLists != NULL ? ctx->pageItemLists[i].indices : NULL;
    preprocessRepeat(arrlen(pageItemIndices), pageItemSlot) {
        if (!processTextureItem(ctx, pageItemIndices[pageItemSlot], rgba, width, height))
            goto cleanup;
    }

    /* Reuse the page decode for tile extraction so we do not pay for a
     * second full decode while the page is still being processed. */
    int tileWidth = 0, tileHeight = 0;
    if (pageHasTileRequests) {
        tileRgba = rgba;
        tileWidth = width;
        tileHeight = height;
        rgba = NULL; /* ownership transferred */
    } else {
        ImageDecoder_freeRgba(rgba);
        rgba = NULL;
    }

    uint32_t* pageTileRequestIndices =
        ctx->pageTileRequestLists != NULL ? ctx->pageTileRequestLists[i].indices : NULL;
    preprocessRepeat(arrlen(pageTileRequestIndices), reqSlot) {
        if (!processTextureTile(
                ctx, pageTileRequestIndices[reqSlot], tileRgba, tileWidth, tileHeight))
            goto cleanup;
    }

    ok = true;
cleanup:
    free(blobCopy);
    ImageDecoder_freeRgba(rgba);
    ImageDecoder_freeRgba(tileRgba);
    return ok;
}

static bool packTextureInputs(TextureConversionContext* ctx) {
    preprocessRepeat(ctx->dataWin->txtr.count, txtrIndex) {
        if (!ctx->dataWin->txtr.textures[txtrIndex].mapped) {
            free(ctx->dataWin->txtr.textures[txtrIndex].blobData);
        }
        ctx->dataWin->txtr.textures[txtrIndex].blobData = NULL;
        ctx->dataWin->txtr.textures[txtrIndex].mapped = false;
    }

    uint32_t usedCount = 0u;
    preprocessRepeat(ctx->dataWin->tpag.count, i) {
        if (!ctx->usedTPAGs[i]) continue;
        usedCount++;
        ctx->items[i].fragmentCount = UINT16_MAX;
        ctx->items[i].fragmentStart = UINT32_MAX;
    }

    fprintf(stderr,
        "cinnamon-preprocessor: packing %u referenced TPAG entries; skipping %u unused\n",
        usedCount, ctx->dataWin->tpag.count - usedCount);

    preprocessRepeat(ctx->dataWin->txtr.count, i) {
        if (!processTexturePage(ctx, i)) return false;
    }

    return true;
}

static bool writeTextureOutputs(TextureConversionContext* ctx) {
    preprocessRepeat(ctx->dataWin->tpag.count, i) {
        if (ctx->items[i].fragmentCount == UINT16_MAX) {
            fprintf(stderr, "TPAG item %zu was not mapped to any output page.\n", i);
            return false;
        }
    }

    preprocessRepeat(ctx->tileRequestCount, i) {
        if (!ctx->tileEntryWritten[i]) {
            fprintf(stderr, "Warning: no packed legacy tile entry for bg=%d src=(%u,%u %ux%u)\n",
                ctx->tileRequests[i].bgDef, ctx->tileRequests[i].srcX, ctx->tileRequests[i].srcY,
                ctx->tileRequests[i].srcW, ctx->tileRequests[i].srcH);
            continue;
        }
        arrput(ctx->packedTileEntries, ctx->tileEntriesByRequest[i]);
    }

    TextureAtlasData atlas = {
        .pages = ctx->atlas.outputPages,
        .pageCount = ctx->atlas.totalPageCount,
        .items = ctx->items,
        .itemCount = ctx->dataWin->tpag.count,
        .fragments = ctx->fragments,
        .fragmentCount = (uint32_t) arrlen(ctx->fragments),
        .tileEntries = ctx->packedTileEntries,
        .tileEntryCount = (uint32_t) arrlen(ctx->packedTileEntries),
        .textureFormat = ctx->options->textureFormat,
        .tpagHash = texHashTpag(ctx->dataWin),
    };
    bool ok = texFlushPages(ctx->options, ctx->pipeline, &ctx->atlas);
    if (ok) ok = texWriteRoomManifest(ctx->options, ctx->dataWin, &atlas);
    if (ok)
        ok = texBuildBindings(
            ctx->dataWin, ctx->items, &ctx->tpagBindingRows, &ctx->tpagBindingCount);
    atlas.tpagBindingRows = ctx->tpagBindingRows;
    atlas.tpagBindingCount = ctx->tpagBindingCount;
    char roomManifestPath[PREPROCESS_TEMP_PATH_MAX];
    if (ok)
        ok = pathJoin(ctx->options->outputDir, "gfx/room_manifest.bin", roomManifestPath,
            sizeof(roomManifestPath));
    if (ok) ok = texWriteAtlas(ctx->options, ctx->pipeline, &atlas, roomManifestPath);
    if (ok) texRemoveOldBindings(ctx->options->outputDir);
    if (ok) texRemoveManifestFiles(ctx->options->outputDir);
    if (ok) texRemoveOldAssets(ctx->options->outputDir);
    return ok;
}

bool texConvertPipeline(const PreprocessRunConfig* options, DataWin* dataWin,
    const TexturePreprocessPipeline* pipeline) {
    if (options == NULL || dataWin == NULL || pipeline == NULL || options->outputDir[0] == '\0')
        return false;
    if (!targetAllowsFormat(options->user.target, options->textureFormat)) {
        fprintf(stderr, "%s texture format is not supported for %s preprocessing.\n",
            texFormatName(options->textureFormat), targetName(options->user.target));
        return false;
    }
    fprintf(stderr, "cinnamon-preprocessor: processing textures (%s pipeline, atlas-only)\n",
        pipeline->name != NULL ? pipeline->name : "target");
    progressPhase(PREPROCESS_PHASE_TEXTURE_PAGES);
    texRemoveOldAssets(options->outputDir);
    if (texCacheIsCurrent(options, pipeline, dataWin)) {
        texRemoveOldBindings(options->outputDir);
        fprintf(stderr,
            "cinnamon-preprocessor: texture cache is current; skipping texture preprocessing\n");
        return true;
    }
    texRemoveCache(options->outputDir);
    char* gfxDir = pathJoinAlloc(options->outputDir, "gfx");
    texRemovePageFiles(gfxDir);
    free(gfxDir);

    TextureConversionContext ctx = {.options = options, .dataWin = dataWin, .pipeline = pipeline};
    bool ok = false;
    if (!initTextureContext(&ctx)) goto cleanup;
    if (!packTextureInputs(&ctx)) goto cleanup;
    ok = writeTextureOutputs(&ctx);
cleanup:
    destroyTextureContext(&ctx);
    return ok;
}
