#include "common/preprocess_common.h"
#include "texture_atlas_packer.h"

#include "core/preprocess_options.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb/ds/stb_ds.h>

static uint32_t maskPopcount(uint64_t value) {
    uint32_t count = 0;
    while (value != 0u) {
        value &= value - 1u;
        count++;
    }
    return count;
}

uint64_t* atlasTpagMask(RoomAtlasAffinity* affinity, uint32_t tpagIndex) {
    if (affinity == NULL || affinity->tpagRoomMasks == NULL || affinity->roomMaskWords == 0u ||
        tpagIndex >= affinity->tpagCount)
        return NULL;
    return affinity->tpagRoomMasks + ((size_t) tpagIndex * affinity->roomMaskWords);
}

uint64_t* atlasTileMask(RoomAtlasAffinity* affinity, uint32_t requestIndex) {
    if (affinity == NULL || affinity->tileRequestRoomMasks == NULL ||
        affinity->roomMaskWords == 0u || requestIndex >= affinity->tileRequestCount)
        return NULL;
    return affinity->tileRequestRoomMasks + ((size_t) requestIndex * affinity->roomMaskWords);
}

static void maskSet(uint64_t* mask, uint32_t roomMaskWords, uint32_t roomIndex) {
    if (mask == NULL || roomMaskWords == 0u) return;
    uint32_t wordIndex = roomIndex / 64u;
    if (wordIndex >= roomMaskWords) return;
    mask[wordIndex] |= 1ull << (roomIndex % 64u);
}

static bool maskHasBits(const uint64_t* mask, uint32_t roomMaskWords) {
    if (mask == NULL || roomMaskWords == 0u) return false;
    preprocessRepeat(roomMaskWords, i) {
        if (mask[i] != 0u) return true;
    }
    return false;
}

static uint32_t maskCount(const uint64_t* mask, uint32_t roomMaskWords) {
    uint32_t count = 0;
    if (mask == NULL || roomMaskWords == 0u) return 0;
    preprocessRepeat(roomMaskWords, i) count += maskPopcount(mask[i]);
    return count;
}

static uint32_t maskOverlapCount(const uint64_t* a, const uint64_t* b, uint32_t roomMaskWords) {
    uint32_t count = 0;
    if (a == NULL || b == NULL || roomMaskWords == 0u) return 0;
    preprocessRepeat(roomMaskWords, i) count += maskPopcount(a[i] & b[i]);
    return count;
}

static bool maskEqual(const uint64_t* a, const uint64_t* b, uint32_t roomMaskWords) {
    if (roomMaskWords == 0u) return true;
    preprocessRepeat(roomMaskWords, i) {
        uint64_t aWord = a != NULL ? a[i] : 0u;
        uint64_t bWord = b != NULL ? b[i] : 0u;
        if (aWord != bWord) return false;
    }
    return true;
}

static void maskMerge(uint64_t* dst, const uint64_t* src, uint32_t roomMaskWords) {
    if (dst == NULL || src == NULL || roomMaskWords == 0u) return;
    preprocessRepeat(roomMaskWords, i) dst[i] |= src[i];
}

static void packedPageMergeRoomMask(
    PackedPage* page, const uint64_t* roomMask, uint32_t roomMaskWords) {
    if (page == NULL || page->roomMask == NULL || roomMask == NULL || roomMaskWords == 0u) return;
    uint32_t words = page->roomMaskWords < roomMaskWords ? page->roomMaskWords : roomMaskWords;
    maskMerge(page->roomMask, roomMask, words);
}

void atlasFreeAffinity(RoomAtlasAffinity* affinity) {
    if (affinity == NULL) return;
    free(affinity->tpagRoomMasks);
    free(affinity->tileRequestRoomMasks);
    memset(affinity, 0, sizeof(*affinity));
}

static void markRoomAffinityTPAG(
    RoomAtlasAffinity* affinity, int32_t tpagIndex, uint32_t roomIndex) {
    if (tpagIndex < 0) return;
    uint64_t* mask = atlasTpagMask(affinity, (uint32_t) tpagIndex);
    maskSet(mask, affinity != NULL ? affinity->roomMaskWords : 0u, roomIndex);
}

static void markRoomAffinitySprite(
    DataWin* dataWin, RoomAtlasAffinity* affinity, int32_t spriteIndex, uint32_t roomIndex) {
    if (dataWin == NULL || spriteIndex < 0 || (uint32_t) spriteIndex >= dataWin->sprt.count) return;
    const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    preprocessRepeat(sprite->textureCount, frameIndex) {
        markRoomAffinityTPAG(affinity, sprite->tpagIndices[frameIndex], roomIndex);
    }
}

static void markBackgroundRooms(
    DataWin* dataWin, RoomAtlasAffinity* affinity, int32_t backgroundIndex, uint32_t roomIndex) {
    if (dataWin == NULL || backgroundIndex < 0 || (uint32_t) backgroundIndex >= dataWin->bgnd.count)
        return;
    markRoomAffinityTPAG(affinity, dataWin->bgnd.backgrounds[backgroundIndex].tpagIndex, roomIndex);
}

bool atlasBuildAffinity(DataWin* dataWin, const TileLookupKey* tileRequests,
    size_t tileRequestCount, RoomAtlasAffinity* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (dataWin == NULL || dataWin->room.count == 0u) return true;
    if (dataWin->room.count > UINT32_MAX - 63u) return false;

    uint32_t roomMaskWords = (dataWin->room.count + 63u) / 64u;
    out->roomMaskWords = roomMaskWords;
    out->tpagCount = dataWin->tpag.count;
    out->tileRequestCount =
        tileRequestCount > UINT32_MAX ? UINT32_MAX : (uint32_t) tileRequestCount;
    size_t tpagMaskCount = 0u;
    size_t tileRequestMaskCount = 0u;
    if (dataWin->tpag.count > 0u &&
        !checkedMulSize((size_t) dataWin->tpag.count, roomMaskWords, &tpagMaskCount))
        return false;
    if (tileRequestCount > 0u && tileRequestCount <= UINT32_MAX &&
        !checkedMulSize(tileRequestCount, roomMaskWords, &tileRequestMaskCount)) {
        return false;
    }
    if (dataWin->tpag.count > 0u) {
        out->tpagRoomMasks = safeCalloc(tpagMaskCount, sizeof(uint64_t));
    }
    if (tileRequestCount > 0u && tileRequestCount <= UINT32_MAX) {
        out->tileRequestRoomMasks = safeCalloc(tileRequestMaskCount, sizeof(uint64_t));
    }

    TileRequestMap* tileRequestMap = NULL;
    preprocessRepeat(tileRequestCount, requestIndex) {
        if (requestIndex > UINT32_MAX) break;
        hmput(tileRequestMap, tileRequests[requestIndex], (uint32_t) requestIndex);
    }

    preprocessRepeat(dataWin->room.count, roomIndex) {
        DataWin_loadRoomPayload(dataWin, (int32_t) roomIndex);
        Room* room = &dataWin->room.rooms[roomIndex];

        preprocessRepeat(8u, bgSlot) {
            if (room->backgrounds == NULL || !room->backgrounds[bgSlot].enabled) continue;
            markBackgroundRooms(
                dataWin, out, room->backgrounds[bgSlot].backgroundDefinition, (uint32_t) roomIndex);
        }

        preprocessRepeat(room->gameObjectCount, objIndex) {
            const RoomGameObject* roomObject = &room->gameObjects[objIndex];
            if (roomObject->objectDefinition < 0 ||
                (uint32_t) roomObject->objectDefinition >= dataWin->objt.count)
                continue;
            markRoomAffinitySprite(dataWin, out,
                dataWin->objt.objects[roomObject->objectDefinition].spriteId, (uint32_t) roomIndex);
        }

        preprocessRepeat(room->layerCount, layerIndex) {
            RoomLayer* layer = &room->layers[layerIndex];
            if (layer->backgroundData != NULL) {
                markRoomAffinitySprite(
                    dataWin, out, layer->backgroundData->spriteIndex, (uint32_t) roomIndex);
            }
            if (layer->assetsData == NULL) continue;
            preprocessRepeat(layer->assetsData->spriteCount, spriteSlot) {
                markRoomAffinitySprite(dataWin, out,
                    layer->assetsData->sprites[spriteSlot].spriteIndex, (uint32_t) roomIndex);
            }
        }

        TileLookupKey* roomTileKeys = NULL;
        TileRequestMap* roomTileDedupe = NULL;
        collectRoomTiles(room, dataWin, &roomTileKeys, &roomTileDedupe);
        preprocessRepeat(arrlen(roomTileKeys), tileKeyIndex) {
            ptrdiff_t packedTileIndex = hmgeti(tileRequestMap, roomTileKeys[tileKeyIndex]);
            if (packedTileIndex < 0) continue;
            uint32_t requestIndex = tileRequestMap[packedTileIndex].value;
            uint64_t* mask = atlasTileMask(out, requestIndex);
            maskSet(mask, out->roomMaskWords, (uint32_t) roomIndex);
        }
        arrfree(roomTileKeys);
        hmfree(roomTileDedupe);
    }

    hmfree(tileRequestMap);

    uint32_t affinedTPAGs = 0;
    uint32_t affinedTileRequests = 0;
    preprocessRepeat(dataWin->tpag.count, i) {
        if (maskHasBits(atlasTpagMask(out, (uint32_t) i), out->roomMaskWords)) affinedTPAGs++;
    }
    preprocessRepeat(out->tileRequestCount, i) {
        if (maskHasBits(atlasTileMask(out, (uint32_t) i), out->roomMaskWords))
            affinedTileRequests++;
    }
    fprintf(stderr,
        "cinnamon-preprocessor: room atlas affinity rooms=%u tpag=%u/%u tileRects=%u/%u\n",
        dataWin->room.count, affinedTPAGs, dataWin->tpag.count, affinedTileRequests,
        out->tileRequestCount);
    return true;
}

static PackedPage* createPackedPage(OutputPage** pages, uint32_t* totalPageCount,
    uint32_t pageWidth, uint32_t pageHeight, CinnamonTextureFormat pageFormat,
    uint32_t roomMaskWords) {
    if (pages == NULL || totalPageCount == NULL || *totalPageCount == UINT32_MAX) return NULL;
    size_t outputPageCount = (size_t) *totalPageCount + 1u;
    if (outputPageCount > SIZE_MAX / sizeof(OutputPage)) return NULL;
    *pages = safeRealloc(*pages, outputPageCount * sizeof(OutputPage));
    OutputPage* outputPage = &(*pages)[*totalPageCount];
    memset(outputPage, 0, sizeof(*outputPage));
    outputPage->width = (uint16_t) pageWidth;
    outputPage->height = (uint16_t) pageHeight;
    outputPage->textureFormat = (uint32_t) pageFormat;
    const char* extension = texExtension(pageFormat);
    snprintf(
        outputPage->path, sizeof(outputPage->path), "page_%03u.%s", *totalPageCount, extension);

    PackedPage* packedPage = safeCalloc(1, sizeof(PackedPage));
    packedPage->pageIndex = *totalPageCount;
    packedPage->width = pageWidth;
    packedPage->height = pageHeight;
    packedPage->roomMaskWords = roomMaskWords;
    if (roomMaskWords > 0u) packedPage->roomMask = safeCalloc(roomMaskWords, sizeof(uint64_t));
    size_t pagePixels = 0u;
    if (!checkedMulSize((size_t) pageWidth, pageHeight, &pagePixels)) {
        free(packedPage->roomMask);
        free(packedPage);
        return NULL;
    }
    packedPage->pixels = safeCalloc(pagePixels, 4);
    AtlasFreeRect initialRect = {0u, 0u, pageWidth, pageHeight};
    arrput(packedPage->freeRects, initialRect);
    (*totalPageCount)++;
    return packedPage;
}

/* MaxRects best-short-side-fit, without rotation: atlas records have no rotation flag. */
static bool findMaxRectsPlacement(const PackedPage* page, uint32_t width, uint32_t height,
    AtlasFreeRect* placement, uint32_t* shortSide, uint32_t* longSide) {
    if (width == 0u || height == 0u) return false;
    bool found = false;
    preprocessRepeat(arrlenu(page->freeRects), i) {
        const AtlasFreeRect* rect = &page->freeRects[i];
        if (width > rect->width || height > rect->height) continue;
        uint32_t spareWidth = rect->width - width;
        uint32_t spareHeight = rect->height - height;
        uint32_t candidateShort = spareWidth < spareHeight ? spareWidth : spareHeight;
        uint32_t candidateLong = spareWidth > spareHeight ? spareWidth : spareHeight;
        if (found && (candidateShort > *shortSide ||
                         (candidateShort == *shortSide && candidateLong > *longSide) ||
                         (candidateShort == *shortSide && candidateLong == *longSide &&
                             (rect->y > placement->y ||
                                 (rect->y == placement->y && rect->x >= placement->x)))))
            continue;
        *placement = (AtlasFreeRect) {rect->x, rect->y, width, height};
        *shortSide = candidateShort;
        *longSide = candidateLong;
        found = true;
    }
    return found;
}

static bool rectContains(const AtlasFreeRect* outer, const AtlasFreeRect* inner) {
    return inner->x >= outer->x && inner->y >= outer->y &&
        inner->x + inner->width <= outer->x + outer->width &&
        inner->y + inner->height <= outer->y + outer->height;
}

static void commitMaxRectsPlacement(PackedPage* page, AtlasFreeRect used) {
    uint32_t right = used.x + used.width;
    uint32_t bottom = used.y + used.height;
    AtlasFreeRect* freeRects = NULL;
    preprocessRepeat(arrlenu(page->freeRects), i) {
        AtlasFreeRect rect = page->freeRects[i];
        uint32_t rectRight = rect.x + rect.width;
        uint32_t rectBottom = rect.y + rect.height;
        if (used.x >= rectRight || right <= rect.x || used.y >= rectBottom || bottom <= rect.y) {
            arrput(freeRects, rect);
            continue;
        }
        if (used.x > rect.x) {
            AtlasFreeRect split = {rect.x, rect.y, used.x - rect.x, rect.height};
            arrput(freeRects, split);
        }
        if (right < rectRight) {
            AtlasFreeRect split = {right, rect.y, rectRight - right, rect.height};
            arrput(freeRects, split);
        }
        if (used.y > rect.y) {
            AtlasFreeRect split = {rect.x, rect.y, rect.width, used.y - rect.y};
            arrput(freeRects, split);
        }
        if (bottom < rectBottom) {
            AtlasFreeRect split = {rect.x, bottom, rect.width, rectBottom - bottom};
            arrput(freeRects, split);
        }
    }
    for (size_t i = 0u; i < arrlenu(freeRects);) {
        bool removed = false;
        for (size_t j = i + 1u; j < arrlenu(freeRects);) {
            if (rectContains(&freeRects[i], &freeRects[j])) {
                arrdel(freeRects, j);
            } else if (rectContains(&freeRects[j], &freeRects[i])) {
                arrdel(freeRects, i);
                removed = true;
                break;
            } else {
                j++;
            }
        }
        if (!removed) i++;
    }
    arrfree(page->freeRects);
    page->freeRects = freeRects;
}

static uint32_t normalizePageSize(uint32_t minPageSize) {
    uint32_t pageSize = CINNAMON_MIN_ATLAS_TEXTURE_SIZE;
    while (pageSize < CINNAMON_LARGE_TEXTURE_SIZE && pageSize < minPageSize) pageSize <<= 1u;
    return pageSize;
}

static uint32_t choosePageDimension(uint32_t dimension, uint32_t minPageSize) {
    uint32_t pageSize = normalizePageSize(minPageSize);
    while (pageSize < CINNAMON_LARGE_TEXTURE_SIZE && pageSize < dimension) pageSize <<= 1u;
    return pageSize;
}

void atlasPageDimensions(const PreprocessRunConfig* options, uint32_t width, uint32_t height,
    bool allowLargePage, uint32_t minAutoPageSize, uint32_t* outWidth, uint32_t* outHeight) {
    uint32_t fixedSize = 0u;
    if (options != NULL) {
        if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_256)
            fixedSize = CINNAMON_SMALL_TEXTURE_SIZE;
        if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_512)
            fixedSize = CINNAMON_MEDIUM_TEXTURE_SIZE;
        if (options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_FORCE_1024)
            fixedSize = CINNAMON_LARGE_TEXTURE_SIZE;
    }
    if (!allowLargePage && fixedSize != 0u) {
        *outWidth = fixedSize;
        *outHeight = fixedSize;
        return;
    }
    *outWidth = choosePageDimension(width, minAutoPageSize);
    *outHeight = choosePageDimension(height, minAutoPageSize);
}

uint32_t atlasPageSize(const PreprocessRunConfig* options, uint32_t width, uint32_t height,
    bool allowLargePage, uint32_t minAutoPageSize) {
    uint32_t pageWidth = 0u;
    uint32_t pageHeight = 0u;
    atlasPageDimensions(
        options, width, height, allowLargePage, minAutoPageSize, &pageWidth, &pageHeight);
    return pageWidth > pageHeight ? pageWidth : pageHeight;
}

static bool packRect(const PreprocessRunConfig* options, AtlasPacking* atlas,
    const AtlasPackRequest* request, AtlasPlacement* placement) {
    if (request->width == 0u || request->height == 0u ||
        request->width > CINNAMON_LARGE_TEXTURE_SIZE ||
        request->height > CINNAMON_LARGE_TEXTURE_SIZE)
        return false;
    uint32_t pageWidth = 0u;
    uint32_t pageHeight = 0u;
    atlasPageDimensions(options, request->width, request->height, request->allowLargePage,
        request->minAutoPageSize, &pageWidth, &pageHeight);
    if (request->width > pageWidth || request->height > pageHeight) return false;

    PackedPage* bestPage = NULL;
    AtlasFreeRect bestPlacement = {0};
    uint32_t bestShort = UINT32_MAX;
    uint32_t bestLong = UINT32_MAX;
    uint32_t bestOverlap = 0;
    uint32_t bestExtraRooms = UINT32_MAX;
    bool useRoomAffinity = request->roomMaskWords > 0u;
    bool strictRoomAffinity =
        options == NULL || options->atlasPageMode == CINNAMON_ATLAS_PAGE_MODE_AUTO;
    uint32_t roomUseCount =
        useRoomAffinity ? maskCount(request->roomMask, request->roomMaskWords) : 0u;

    preprocessRepeat(atlas->packedPageCount, i) {
        PackedPage* page = atlas->packedPages[i];
        if (page->width != pageWidth || page->height != pageHeight) continue;
        OutputPage* outputPage = &atlas->outputPages[page->pageIndex];
        if ((CinnamonTextureFormat) outputPage->textureFormat != request->format) continue;
        if (request->format == CINNAMON_TEXFMT_LA4) {
            if (request->allowLargePage &&
                (outputPage->containsSprite || outputPage->containsBackground))
                continue;
            if (!request->allowLargePage && outputPage->containsFont) continue;
        }
        if (strictRoomAffinity && useRoomAffinity &&
            !maskEqual(page->roomMask, request->roomMask, request->roomMaskWords))
            continue;

        AtlasFreeRect candidate;
        uint32_t candidateShort = UINT32_MAX;
        uint32_t candidateLong = UINT32_MAX;
        if (!findMaxRectsPlacement(
                page, request->width, request->height, &candidate, &candidateShort, &candidateLong))
            continue;

        uint32_t overlap = useRoomAffinity
            ? maskOverlapCount(page->roomMask, request->roomMask, request->roomMaskWords)
            : 0u;
        uint32_t extraRooms =
            useRoomAffinity ? maskCount(page->roomMask, request->roomMaskWords) - overlap : 0u;
        bool better = bestPage == NULL || candidateShort < bestShort ||
            (candidateShort == bestShort && candidateLong < bestLong);
        if (bestPage != NULL && useRoomAffinity && !strictRoomAffinity) {
            if (overlap != bestOverlap)
                better = overlap > bestOverlap;
            else if (extraRooms != bestExtraRooms)
                better = extraRooms < bestExtraRooms;
        }
        if (!better) continue;

        bestPage = page;
        bestPlacement = candidate;
        bestShort = candidateShort;
        bestLong = candidateLong;
        bestOverlap = overlap;
        bestExtraRooms = extraRooms;
    }

    if (!strictRoomAffinity && useRoomAffinity && bestPage != NULL && bestOverlap == 0u &&
        bestExtraRooms > 0u && roomUseCount <= 4u) {
        bestPage = NULL;
    }

    if (bestPage != NULL) {
        placement->pageIndex = (uint16_t) bestPage->pageIndex;
        placement->x = (uint16_t) bestPlacement.x;
        placement->y = (uint16_t) bestPlacement.y;
        commitMaxRectsPlacement(bestPage, bestPlacement);
        packedPageMergeRoomMask(bestPage, request->roomMask, request->roomMaskWords);
        return true;
    }

    if (atlas->packedPageCount == UINT32_MAX || atlas->totalPageCount >= UINT16_MAX) return false;
    size_t packedPageSlots = (size_t) atlas->packedPageCount + 1u;
    if (packedPageSlots > SIZE_MAX / sizeof(PackedPage*)) return false;
    atlas->packedPages = safeRealloc(atlas->packedPages, packedPageSlots * sizeof(PackedPage*));
    PackedPage* page = createPackedPage(&atlas->outputPages, &atlas->totalPageCount, pageWidth,
        pageHeight, request->format, request->roomMaskWords);
    if (page == NULL) return false;
    atlas->packedPages[(atlas->packedPageCount)++] = page;

    placement->pageIndex = (uint16_t) page->pageIndex;
    placement->x = 0;
    placement->y = 0;
    commitMaxRectsPlacement(page, (AtlasFreeRect){0u, 0u, request->width, request->height});
    packedPageMergeRoomMask(page, request->roomMask, request->roomMaskWords);
    return true;
}

uint32_t atlasPadding(CinnamonTextureFormat pageFormat) {
    switch (pageFormat) {
        case CINNAMON_TEXFMT_ETC1:
        case CINNAMON_TEXFMT_ETC1A4:
            return 4u;
        case CINNAMON_TEXFMT_L4:
        case CINNAMON_TEXFMT_LA4:
        case CINNAMON_TEXFMT_RGB565:
        case CINNAMON_TEXFMT_RGBA4444:
        case CINNAMON_TEXFMT_CI4:
        case CINNAMON_TEXFMT_CI8:
        case CINNAMON_TEXFMT_CMPR:
            return 1u;
        default:
            return 0u;
    }
}

bool atlasPackRect(const PreprocessRunConfig* options, AtlasPacking* atlas,
    const AtlasPackRequest* request, AtlasPlacement* placement) {
    uint32_t padding = atlasPadding(request->format);
    if (request->width > UINT32_MAX - padding * 2u || request->height > UINT32_MAX - padding * 2u)
        return false;
    AtlasPackRequest padded = *request;
    padded.width += padding * 2u;
    padded.height += padding * 2u;
    if (!packRect(options, atlas, &padded, placement)) return false;
    placement->x += (uint16_t) padding;
    placement->y += (uint16_t) padding;
    placement->padding = padding;
    return true;
}

PackedPage* atlasFindPage(PackedPage** packedPages, uint32_t packedPageCount, uint16_t pageIndex) {
    if (packedPages == NULL) return NULL;
    preprocessRepeat(packedPageCount, i) {
        if (packedPages[i] == NULL) continue;
        if (packedPages[i]->pageIndex == pageIndex) return packedPages[i];
    }
    return NULL;
}

void atlasFreePages(PackedPage** packedPages, uint32_t packedPageCount) {
    if (packedPages == NULL) return;
    preprocessRepeat(packedPageCount, i) {
        if (packedPages[i] == NULL) continue;
        free(packedPages[i]->pixels);
        free(packedPages[i]->roomMask);
        arrfree(packedPages[i]->freeRects);
        free(packedPages[i]);
    }
    free(packedPages);
}
