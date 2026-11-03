#include "common/preprocess_common.h"
#include "preprocess_datawin.h"
#include "common/path_utils.h"

#include "texture/texture_atlas_packer.h"

#include "utils.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stb/ds/stb_ds.h>

#define GMS2_TILE_INDEX_MASK 0x0007FFFFu

static int32_t* allocIndexMap(size_t count) {
    if (count == 0u) count = 1u;
    if (count > SIZE_MAX / sizeof(int32_t)) return NULL;
    int32_t* map = safeMalloc(count * sizeof(int32_t));
    preprocessRepeat(count, i) map[i] = -1;
    return map;
}

bool* collectUsedTpags(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* used = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    preprocessRepeat(dataWin->sprt.count, i) {
        const Sprite* sprite = &dataWin->sprt.sprites[i];
        preprocessRepeat(sprite->textureCount, frame) {
            int32_t index = sprite->tpagIndices[frame];
            if (index >= 0 && (uint32_t) index < dataWin->tpag.count) used[index] = true;
        }
    }
    preprocessRepeat(dataWin->bgnd.count, i) {
        int32_t index = dataWin->bgnd.backgrounds[i].tpagIndex;
        if (index >= 0 && (uint32_t) index < dataWin->tpag.count) used[index] = true;
    }
    preprocessRepeat(dataWin->font.count, i) {
        const Font* font = &dataWin->font.fonts[i];
        if (font->isSpriteFont) continue;
        int32_t index = font->tpagIndex;
        if (index >= 0 && (uint32_t) index < dataWin->tpag.count) used[index] = true;
    }
    return used;
}

bool* collectFontTpags(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* fontTPAGs = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    preprocessRepeat(dataWin->font.count, i) {
        int32_t tpagIndex = dataWin->font.fonts[i].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        fontTPAGs[tpagIndex] = true;
    }
    return fontTPAGs;
}

bool* collectBackgroundTpags(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    bool* backgroundTPAGs =
        safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(bool));
    preprocessRepeat(dataWin->bgnd.count, i) {
        int32_t tpagIndex = dataWin->bgnd.backgrounds[i].tpagIndex;
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        backgroundTPAGs[tpagIndex] = true;
    }
    return backgroundTPAGs;
}

char** collectTpagNames(DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    char** names = safeCalloc(dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u, sizeof(char*));

    preprocessRepeat(dataWin->sprt.count, i) {
        Sprite* sprite = &dataWin->sprt.sprites[i];
        preprocessRepeat(sprite->textureCount, j) {
            int32_t tpagIndex = sprite->tpagIndices[j];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
            if (names[tpagIndex] == NULL) names[tpagIndex] = safeCalloc(1u, 2048u);
            pathAddDebugName(names[tpagIndex], 2048u, sprite->name);
        }
    }

    preprocessRepeat(dataWin->bgnd.count, i) {
        Background* bg = &dataWin->bgnd.backgrounds[i];
        if (bg->tpagIndex < 0 || (uint32_t) bg->tpagIndex >= dataWin->tpag.count) continue;
        if (names[bg->tpagIndex] == NULL) names[bg->tpagIndex] = safeCalloc(1u, 2048u);
        pathAddDebugName(names[bg->tpagIndex], 2048u, bg->name);
    }

    return names;
}

void freeTpagNames(char** names, uint32_t count) {
    if (names == NULL) return;
    preprocessRepeat(count, i) free(names[i]);
    free(names);
}

int32_t* mapTpagSprites(const DataWin* dataWin) {
    if (dataWin == NULL) return NULL;
    size_t mapCount = dataWin->tpag.count > 0 ? dataWin->tpag.count : 1u;
    int32_t* map = allocIndexMap(mapCount);
    if (map == NULL) return NULL;

    preprocessRepeat(dataWin->sprt.count, spriteIndex) {
        const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
        preprocessRepeat(sprite->textureCount, frameIndex) {
            int32_t tpagIndex = sprite->tpagIndices[frameIndex];
            if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
            if (map[tpagIndex] < 0) map[tpagIndex] = (int32_t) spriteIndex;
        }
    }
    return map;
}

static void collectLegacyTileRequest(
    TileLookupKey key, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    ptrdiff_t existing = hmgeti(*dedupe, key);
    if (existing >= 0) return;
    hmput(*dedupe, key, (uint32_t) arrlen(*outKeys));
    arrput(*outKeys, key);
}

static void collectTileLayerRequests(RoomLayerTilesData* tilesData, DataWin* dataWin,
    TileLookupKey** outKeys, TileRequestMap** dedupe) {
    if (tilesData == NULL || tilesData->tileData == NULL) return;
    if (tilesData->backgroundIndex < 0 ||
        (uint32_t) tilesData->backgroundIndex >= dataWin->bgnd.count)
        return;

    Background* tileset = &dataWin->bgnd.backgrounds[tilesData->backgroundIndex];
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 ||
        tileset->gms2TileColumns == 0)
        return;

    uint32_t tileW = tileset->gms2TileWidth;
    uint32_t tileH = tileset->gms2TileHeight;
    uint32_t borderX = tileset->gms2OutputBorderX;
    uint32_t borderY = tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;
    uint32_t totalTiles = tilesData->tilesX * tilesData->tilesY;

    preprocessRepeat(totalTiles, i) {
        uint32_t cell = tilesData->tileData[i];
        uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
        if (tileIndex == 0) continue;

        uint32_t col = tileIndex % columns;
        uint32_t row = tileIndex / columns;
        collectLegacyTileRequest(
            (TileLookupKey){
                .bgDef = (int16_t) tilesData->backgroundIndex,
                .srcX = (uint16_t) (col * (tileW + 2u * borderX) + borderX),
                .srcY = (uint16_t) (row * (tileH + 2u * borderY) + borderY),
                .srcW = (uint16_t) tileW,
                .srcH = (uint16_t) tileH,
            },
            outKeys, dedupe);
    }
}

static void collectBackgroundTiles(
    DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    preprocessRepeat(dataWin->bgnd.count, i) {
        Background* bg = &dataWin->bgnd.backgrounds[i];
        if (bg->gms2TileWidth == 0 || bg->gms2TileHeight == 0 || bg->gms2TileColumns == 0 ||
            bg->gms2TileCount == 0)
            continue;

        uint32_t tileW = bg->gms2TileWidth;
        uint32_t tileH = bg->gms2TileHeight;
        uint32_t borderX = bg->gms2OutputBorderX;
        uint32_t borderY = bg->gms2OutputBorderY;
        uint32_t columns = bg->gms2TileColumns;

        preprocessRepeat(bg->gms2TileCount, tileSlot) {
            uint32_t col = (uint32_t) tileSlot % columns;
            uint32_t row = (uint32_t) tileSlot / columns;
            collectLegacyTileRequest(
                (TileLookupKey){
                    .bgDef = (int16_t) i,
                    .srcX = (uint16_t) (col * (tileW + 2u * borderX) + borderX),
                    .srcY = (uint16_t) (row * (tileH + 2u * borderY) + borderY),
                    .srcW = (uint16_t) tileW,
                    .srcH = (uint16_t) tileH,
                },
                outKeys, dedupe);
        }
    }
}

void collectRoomTiles(
    Room* room, DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** dedupe) {
    if (room == NULL) return;

    preprocessRepeat(room->tileCount, i) {
        RoomTile* tile = &room->tiles[i];
        if (tile->useSpriteDefinition || tile->backgroundDefinition < 0 || tile->width == 0 ||
            tile->height == 0)
            continue;
        if (tile->width > CINNAMON_LARGE_TEXTURE_SIZE || tile->height > CINNAMON_LARGE_TEXTURE_SIZE)
            continue;
        collectLegacyTileRequest(
            (TileLookupKey){
                .bgDef = (int16_t) tile->backgroundDefinition,
                .srcX = (uint16_t) tile->sourceX,
                .srcY = (uint16_t) tile->sourceY,
                .srcW = (uint16_t) tile->width,
                .srcH = (uint16_t) tile->height,
            },
            outKeys, dedupe);
    }

    preprocessRepeat(room->layerCount, i) {
        RoomLayer* layer = &room->layers[i];
        if (layer->tilesData != NULL) {
            collectTileLayerRequests(layer->tilesData, dataWin, outKeys, dedupe);
        }
        if (layer->assetsData == NULL) continue;
        preprocessRepeat(layer->assetsData->legacyTileCount, j) {
            RoomTile* tile = &layer->assetsData->legacyTiles[j];
            if (tile->useSpriteDefinition || tile->backgroundDefinition < 0 || tile->width == 0 ||
                tile->height == 0)
                continue;
            if (tile->width > CINNAMON_LARGE_TEXTURE_SIZE ||
                tile->height > CINNAMON_LARGE_TEXTURE_SIZE)
                continue;
            collectLegacyTileRequest(
                (TileLookupKey){
                    .bgDef = (int16_t) tile->backgroundDefinition,
                    .srcX = (uint16_t) tile->sourceX,
                    .srcY = (uint16_t) tile->sourceY,
                    .srcW = (uint16_t) tile->width,
                    .srcH = (uint16_t) tile->height,
                },
                outKeys, dedupe);
        }
    }
}

TileLookupKey* collectLegacyTiles(DataWin* dataWin) {
    TileLookupKey* keys = NULL;
    TileRequestMap* dedupe = NULL;
    uint32_t totalRoomTiles = 0;
    uint32_t totalLegacyAssetTiles = 0;
    uint32_t totalTileLayers = 0;
    preprocessRepeat(dataWin->room.count, i) {
        DataWin_loadRoomPayload(dataWin, (int32_t) i);
        Room* room = &dataWin->room.rooms[i];
        totalRoomTiles += room->tileCount;
        preprocessRepeat(room->layerCount, layerIndex) {
            RoomLayer* layer = &room->layers[layerIndex];
            if (layer->assetsData != NULL)
                totalLegacyAssetTiles += layer->assetsData->legacyTileCount;
            if (layer->tilesData != NULL) totalTileLayers++;
        }
        collectRoomTiles(&dataWin->room.rooms[i], dataWin, &keys, &dedupe);
    }
    collectBackgroundTiles(dataWin, &keys, &dedupe);
    fprintf(stderr,
        "cinnamon-preprocessor: roomTiles=%u assetLegacyTiles=%u tileLayers=%u "
        "uniqueTileRects=%u\n",
        totalRoomTiles, totalLegacyAssetTiles, totalTileLayers, (unsigned) arrlen(keys));
    hmfree(dedupe);
    return keys;
}
