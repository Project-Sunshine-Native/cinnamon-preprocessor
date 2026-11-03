#include "texture_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

typedef struct {
    uint32_t index;
    uint32_t roomCount;
    uint64_t area;
} PreprocessWorkOrder;

void texFreeIndexLists(PreprocessIndexList* lists, uint32_t count) {
    if (lists == NULL) return;
    preprocessRepeat(count, i) {
        arrfree(lists[i].indices);
    }
    free(lists);
}

bool texBuildWorkLists(const DataWin* dataWin, const bool* usedTPAGs,
    const TileLookupKey* tileRequests, size_t tileRequestCount, PreprocessIndexList** outItemLists,
    PreprocessIndexList** outTileRequestLists) {
    if (outItemLists == NULL || outTileRequestLists == NULL) return false;
    *outItemLists = NULL;
    *outTileRequestLists = NULL;
    if (dataWin == NULL || usedTPAGs == NULL) return false;

    uint32_t texturePageCount = dataWin->txtr.count;
    PreprocessIndexList* itemLists =
        safeCalloc(texturePageCount > 0u ? texturePageCount : 1u, sizeof(PreprocessIndexList));
    PreprocessIndexList* tileRequestLists =
        safeCalloc(texturePageCount > 0u ? texturePageCount : 1u, sizeof(PreprocessIndexList));

    preprocessRepeat(dataWin->tpag.count, itemIndex) {
        if (!usedTPAGs[itemIndex]) continue;
        const TexturePageItem* item = &dataWin->tpag.items[itemIndex];
        if (item->texturePageId < 0 || (uint32_t) item->texturePageId >= texturePageCount) continue;
        arrput(itemLists[item->texturePageId].indices, (uint32_t) itemIndex);
    }

    preprocessRepeat(tileRequestCount, requestIndex) {
        TileLookupKey key = tileRequests[requestIndex];
        if (key.bgDef < 0 || (uint32_t) key.bgDef >= dataWin->bgnd.count) continue;
        const Background* bg = &dataWin->bgnd.backgrounds[key.bgDef];
        if (bg->tpagIndex < 0 || (uint32_t) bg->tpagIndex >= dataWin->tpag.count) continue;
        const TexturePageItem* item = &dataWin->tpag.items[bg->tpagIndex];
        if (item->texturePageId < 0 || (uint32_t) item->texturePageId >= texturePageCount) continue;
        arrput(tileRequestLists[item->texturePageId].indices, (uint32_t) requestIndex);
    }

    *outItemLists = itemLists;
    *outTileRequestLists = tileRequestLists;
    return true;
}

static uint32_t countRoomMaskBits(const uint64_t* mask, uint32_t wordCount) {
    uint32_t count = 0u;
    if (mask == NULL) return 0u;
    preprocessRepeat(wordCount, i) {
        uint64_t value = mask[i];
        while (value != 0u) {
            value &= value - 1u;
            count++;
        }
    }
    return count;
}

static int compareWorkOrder(const void* lhsPtr, const void* rhsPtr) {
    const PreprocessWorkOrder* lhs = lhsPtr;
    const PreprocessWorkOrder* rhs = rhsPtr;
    if (lhs->roomCount != rhs->roomCount) return lhs->roomCount > rhs->roomCount ? -1 : 1;
    if (lhs->area != rhs->area) return lhs->area > rhs->area ? -1 : 1;
    if (lhs->index != rhs->index) return lhs->index < rhs->index ? -1 : 1;
    return 0;
}

static bool sortPreprocessWorkList(uint32_t* indices, size_t count, bool tileRequestList,
    const DataWin* dataWin, const TileLookupKey* tileRequests, RoomAtlasAffinity* affinity) {
    if (count < 2u) return true;
    if (indices == NULL || dataWin == NULL || affinity == NULL) return false;

    size_t orderBytes = 0u;
    if (!checkedMulSize(count, sizeof(PreprocessWorkOrder), &orderBytes)) return false;
    PreprocessWorkOrder* orders = safeMalloc(orderBytes);
    preprocessRepeat(count, i) {
        uint32_t index = indices[i];
        const uint64_t* roomMask;
        uint64_t area;
        if (tileRequestList) {
            if (tileRequests == NULL || index >= affinity->tileRequestCount) {
                free(orders);
                return false;
            }
            roomMask = atlasTileMask(affinity, index);
            area = (uint64_t) tileRequests[index].srcW * tileRequests[index].srcH;
        } else {
            if (index >= dataWin->tpag.count) {
                free(orders);
                return false;
            }
            roomMask = atlasTpagMask(affinity, index);
            area = (uint64_t) dataWin->tpag.items[index].sourceWidth *
                dataWin->tpag.items[index].sourceHeight;
        }
        orders[i].index = index;
        orders[i].roomCount = countRoomMaskBits(roomMask, affinity->roomMaskWords);
        orders[i].area = area;
    }

    qsort(orders, count, sizeof(*orders), compareWorkOrder);
    preprocessRepeat(count, i) indices[i] = orders[i].index;
    free(orders);
    return true;
}

bool texSortWorkLists(const DataWin* dataWin, const TileLookupKey* tileRequests,
    PreprocessIndexList* pageItemLists, PreprocessIndexList* pageTileRequestLists,
    RoomAtlasAffinity* affinity) {
    if (dataWin == NULL || affinity == NULL) return false;
    preprocessRepeat(dataWin->txtr.count, i) {
        if (!sortPreprocessWorkList(pageItemLists != NULL ? pageItemLists[i].indices : NULL,
                pageItemLists != NULL ? (size_t) arrlen(pageItemLists[i].indices) : 0u, false,
                dataWin, tileRequests, affinity))
            return false;
        if (!sortPreprocessWorkList(
                pageTileRequestLists != NULL ? pageTileRequestLists[i].indices : NULL,
                pageTileRequestLists != NULL ? (size_t) arrlen(pageTileRequestLists[i].indices)
                                             : 0u,
                true, dataWin, tileRequests, affinity))
            return false;
    }
    return true;
}

static bool textureDimensionsMatch(const char* path, const Texture* texture) {
    if (path == NULL || texture == NULL) return false;
    if (texture->textureWidth <= 0 || texture->textureHeight <= 0) return true;
    if (!strEndsWithNoCase(path, ".png")) return true;

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info(path, &width, &height, &channels)) return true;
    return width == texture->textureWidth && height == texture->textureHeight;
}

static char* tryRelativeTexture(
    const char* inputDir, const Texture* texture, const char* relativePath) {
    if (inputDir == NULL || relativePath == NULL || relativePath[0] == '\0') return NULL;
    char* candidate = pathJoinAlloc(inputDir, relativePath);
    if (fsFileExists(candidate) && textureDimensionsMatch(candidate, texture)) return candidate;
    free(candidate);
    return NULL;
}

static char* findExternalTexture(
    const char* inputDir, const Texture* texture, const char* fileName) {
    if (inputDir == NULL || fileName == NULL || fileName[0] == '\0') return NULL;

    char* direct = tryRelativeTexture(inputDir, texture, fileName);
    if (direct != NULL) return direct;

    char texturepagesRelative[256];
    snprintf(texturepagesRelative, sizeof(texturepagesRelative), "texturepages/%s", fileName);
    direct = tryRelativeTexture(inputDir, texture, texturepagesRelative);
    if (direct != NULL) return direct;

    snprintf(texturepagesRelative, sizeof(texturepagesRelative), "TexturePages/%s", fileName);
    direct = tryRelativeTexture(inputDir, texture, texturepagesRelative);
    if (direct != NULL) return direct;

    char* recursive = fsFindFile(inputDir, fileName);
    if (recursive != NULL && textureDimensionsMatch(recursive, texture)) return recursive;
    free(recursive);
    return NULL;
}

static char* resolveTexturePath(const PreprocessRunConfig* options, const DataWin* dataWin,
    uint32_t textureIndex, const Texture* texture) {
    (void) dataWin;
    if (options == NULL || options->inputPath[0] == '\0' || texture == NULL) return NULL;
    char* inputDir = pathDupParent(options->inputPath);
    if (inputDir == NULL) return NULL;

    char name[96];
    const char* patterns[] = {
        "texture_%lu.png",
        "texture%lu.png",
        "texture_page_%lu.png",
        "texturepage_%lu.png",
        "Texture_%lu.png",
        "Texture%lu.png",
        "TexturePage_%lu.png",
        "page_%lu.png",
        "page_%03lu.png",
        "texpage_%lu.png",
        "texture_%lu.qoi",
        "texture%lu.qoi",
    };
    uint32_t candidateIds[3];
    size_t candidateCount = 0u;
    candidateIds[candidateCount++] = textureIndex;
    if (textureIndex < UINT32_MAX) candidateIds[candidateCount++] = textureIndex + 1u;
    if (texture->indexInGroup >= 0) {
        uint32_t indexInGroup = (uint32_t) texture->indexInGroup;
        bool alreadyAdded = false;
        preprocessRepeat(candidateCount, candidateIndex) {
            if (candidateIds[candidateIndex] == indexInGroup) {
                alreadyAdded = true;
                break;
            }
        }
        if (!alreadyAdded && candidateCount < sizeof(candidateIds) / sizeof(candidateIds[0])) {
            candidateIds[candidateCount++] = indexInGroup;
        }
    }

    preprocessRepeat(candidateCount, candidateIndex) {
        preprocessRepeat(sizeof(patterns) / sizeof(patterns[0]), i) {
            snprintf(name, sizeof(name), patterns[i], (unsigned long) candidateIds[candidateIndex]);
            char* resolved = findExternalTexture(inputDir, texture, name);
            if (resolved != NULL) {
                free(inputDir);
                return resolved;
            }
        }
    }

    free(inputDir);
    return NULL;
}

uint8_t* texReadBlob(const PreprocessRunConfig* options, const DataWin* dataWin,
    uint32_t textureIndex, const Texture* texture, uint32_t* outSize, char* outSourcePath,
    size_t outSourcePathSize) {
    if (outSize != NULL) *outSize = 0u;
    if (outSourcePath != NULL && outSourcePathSize > 0u) outSourcePath[0] = '\0';
    if (options == NULL || texture == NULL) return NULL;
    if (texture->blobOffset != 0u && texture->blobSize != 0u) {
        if (outSourcePath != NULL && outSourcePathSize > 0u &&
            !pathCopy(outSourcePath, outSourcePathSize, options->inputPath)) {
            return NULL;
        }
        if (outSize != NULL) *outSize = texture->blobSize;
        return fsReadSlice(options->inputPath, texture->blobOffset, texture->blobSize);
    }

    char* externalPath = resolveTexturePath(options, dataWin, textureIndex, texture);
    if (externalPath == NULL) return NULL;
    uint8_t* data = fsReadFile(externalPath, outSize);
    if (data != NULL) {
        if (outSourcePath != NULL && outSourcePathSize > 0u &&
            !pathCopy(outSourcePath, outSourcePathSize, externalPath)) {
            free(data);
            data = NULL;
        }
    }
    if (data != NULL) {
        fprintf(stderr, "preprocess: TXTR page %lu using external texture %s\n",
            (unsigned long) textureIndex, externalPath);
    }
    free(externalPath);
    return data;
}
