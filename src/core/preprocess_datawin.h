#ifndef CINNAMON_PREPROCESSOR_DATAWIN_H
#define CINNAMON_PREPROCESSOR_DATAWIN_H

#include "data_win.h"
#include "texture/texture_convert.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
} TileLookupKey;

typedef struct {
    TileLookupKey key;
    uint32_t value;
} TileRequestMap;

bool* collectUsedTpags(const DataWin* dataWin);
bool* collectFontTpags(const DataWin* dataWin);
bool* collectBackgroundTpags(const DataWin* dataWin);
char** collectTpagNames(DataWin* dataWin);
void freeTpagNames(char** names, uint32_t count);
int32_t* mapTpagSprites(const DataWin* dataWin);
void collectRoomTiles(
    Room* room, DataWin* dataWin, TileLookupKey** outKeys, TileRequestMap** inOutDedupe);
TileLookupKey* collectLegacyTiles(DataWin* dataWin);

#endif
