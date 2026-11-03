#ifndef CINNAMON_PREPROCESSOR_TEXTURE_CONVERT_INTERNAL_H
#define CINNAMON_PREPROCESSOR_TEXTURE_CONVERT_INTERNAL_H

#include "common/preprocess_common.h"
#include "texture_convert.h"
#include "native_texture.h"
#include "core/preprocess_context.h"
#include "core/preprocess_options.h"
#include "core/preprocess_datawin.h"
#include "target/preprocess_target.h"

#include "texture_atlas_packer.h"
#include "texture_lz11.h"
#include "texture_pipeline.h"

#include "image/image_decoder.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb/ds/stb_ds.h>
#include <stb/image/stb_image.h>
#include <stb/image/stb_image_write.h>

#if PREPROCESS_HOST_WINDOWS
#include <windows.h>
#else
#include <dirent.h>
#endif

#define CINNAMON_ATLAS_MAGIC 0x5441334Eu
#define CINNAMON_ATLAS_VERSION 2u
#define CINNAMON_ATLAS_HEADER_SIZE 28u
#define CINNAMON_ATLAS_PAGE_ROW_SIZE 16u
#define CINNAMON_ATLAS_ITEM_ROW_SIZE 12u
#define CINNAMON_ATLAS_FRAGMENT_ROW_SIZE 14u
#define CINNAMON_ATLAS_TILE_ROW_SIZE 26u
#define CINNAMON_ATLAS_DIRECTORY_MAGIC 0x4441334Eu
#define CINNAMON_ATLAS_DIRECTORY_VERSION 2u
#define CINNAMON_ATLAS_BLOCK_TPAG_BINDINGS 0x47415054u
#define CINNAMON_ATLAS_BLOCK_ROOM_MANIFEST 0x4D4F4F52u
#define CINNAMON_TPAG_BINDINGS_VERSION 2u
#define CINNAMON_TPAG_BINDINGS_ROW_SIZE 16u
#define CINNAMON_PREPROCESSOR_READY_MAGIC 0x5250334Eu
#define CINNAMON_PREPROCESSOR_READY_VERSION 20u
#define CINNAMON_TEXTURE_CACHE_REVISION 11u
#define CINNAMON_PREPROCESSOR_READY_MARKER_SIZE 52u
#define CINNAMON_PREPROCESSOR_READY_FILE "preprocess_ready_v1.bin"
#define CINNAMON_ROOM_MANIFEST_MAGIC 0x4D52334Eu
#define CINNAMON_ROOM_MANIFEST_VERSION 2u
#define CINNAMON_ROOM_MANIFEST_HEADER_SIZE 32u
#define CINNAMON_ROOM_MANIFEST_ENTRY_SIZE 36u
#define CINNAMON_ROOM_MANIFEST_PAGE_REF_SIZE 4u
#define CINNAMON_ROOM_MANIFEST_TILE_REF_SIZE 8u
#define WII_RGB5A3_COLOR_COUNT 65536u
#define WII_CI4_PALETTE_COLORS 16u
#define WII_CI8_PALETTE_COLORS 256u
#define WII_CI4_TLUT_BYTES (WII_CI4_PALETTE_COLORS * 2u)
#define WII_CI8_TLUT_BYTES (WII_CI8_PALETTE_COLORS * 2u)
#define WII_CMPR_BLOCK_DIM 4u
#define WII_CMPR_TILE_DIM 8u
#define WII_CMPR_BLOCK_BYTES 8u
#define WII_CMPR_TILE_BYTES 32u
#define WII_RGB5A3_TILE_DIM 4u
#define CINNAMON_TPAG_SOURCE_NONE 0u
#define CINNAMON_TPAG_SOURCE_ATLAS 2u

typedef struct {
    uint32_t type;
    uint32_t version;
    uint32_t offset;
    uint32_t size;
} CinnamonAtlasContainerBlock;

typedef struct {
    int16_t bgDef;
    uint16_t srcX;
    uint16_t srcY;
    uint16_t srcW;
    uint16_t srcH;
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} OutputTileEntry;

typedef struct {
    uint32_t* indices;
} PreprocessIndexList;

#if defined(__EMSCRIPTEN__)
#define CINNAMON_PREPROCESSOR_MEMORY_RESERVE (8u * 1024u * 1024u)
#define CINNAMON_PREPROCESSOR_MAX_RESIDENT_PACKED_BYTES (8u * 1024u * 1024u)
#else
#define CINNAMON_PREPROCESSOR_MEMORY_RESERVE (64u * 1024u * 1024u)
#define CINNAMON_PREPROCESSOR_MAX_RESIDENT_PACKED_BYTES (64u * 1024u * 1024u)
#endif

typedef struct {
    const OutputPage* pages;
    uint32_t pageCount;
    const OutputItem* items;
    uint32_t itemCount;
    const OutputFragment* fragments;
    uint32_t fragmentCount;
    const OutputTileEntry* tileEntries;
    uint32_t tileEntryCount;
    CinnamonTextureFormat textureFormat;
    uint32_t tpagHash;
    const uint8_t* tpagBindingRows;
    uint32_t tpagBindingCount;
} TextureAtlasData;

bool texTpagIsFont(const DataWin* dataWin, uint32_t tpagIndex);
bool texBuildBindings(
    const DataWin* dataWin, const OutputItem* items, uint8_t** outRows, uint32_t* outTpagCount);
void texRemoveOldBindings(const char* outputDir);
void texRemoveManifestFiles(const char* outputDir);
void texRemoveOldAssets(const char* outputDir);
bool texWriteAtlas(const PreprocessRunConfig* options, const TexturePreprocessPipeline* pipeline,
    const TextureAtlasData* atlas, const char* roomManifestPath);
bool texWriteRoomManifest(
    const PreprocessRunConfig* options, const DataWin* dataWin, const TextureAtlasData* atlas);
bool texRgbaBytes(uint32_t width, uint32_t height, size_t* outBytes);
uint32_t texCountOpaque(const uint8_t* rgba, uint32_t width, uint32_t height);
uint32_t texCountOpaqueRect(const TextureRgbaRect* source);
bool texRectFits(
    uint32_t texWidth, uint32_t texHeight, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void texPadBorder(uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t padding);
void texFreeIndexLists(PreprocessIndexList* lists, uint32_t count);
bool texBuildWorkLists(const DataWin* dataWin, const bool* usedTPAGs,
    const TileLookupKey* tileRequests, size_t tileRequestCount, PreprocessIndexList** outItemLists,
    PreprocessIndexList** outTileRequestLists);
bool texSortWorkLists(const DataWin* dataWin, const TileLookupKey* tileRequests,
    PreprocessIndexList* pageItemLists, PreprocessIndexList* pageTileRequestLists,
    RoomAtlasAffinity* affinity);
uint8_t* texReadBlob(const PreprocessRunConfig* options, const DataWin* dataWin,
    uint32_t textureIndex, const Texture* texture, uint32_t* outSize, char* outSourcePath,
    size_t outSourcePathSize);
void texRemovePageFiles(const char* gfxDir);
bool texMaybeFlushPages(const PreprocessRunConfig* options,
    const TexturePreprocessPipeline* pipeline, AtlasPacking* atlas, uint32_t nextPageWidth,
    uint32_t nextPageHeight);
void texAddDebugName(OutputPage* page, const char* name);
bool texFlushPages(const PreprocessRunConfig* options, const TexturePreprocessPipeline* pipeline,
    const AtlasPacking* atlas);
uint32_t texHashTpag(const DataWin* dataWin);
void texRemoveCache(const char* outputDir);
bool texCacheIsCurrent(const PreprocessRunConfig* options,
    const TexturePreprocessPipeline* pipeline, const DataWin* dataWin);
bool texCi4PaletteFits(const TextureRgbaRect* source);
bool texCi8PaletteFits(const TextureRgbaRect* source);

const uint8_t* texSampleRect(const TextureRgbaRect* source, uint32_t x, uint32_t y);
uint32_t texMorton8(uint32_t x, uint32_t y);
uint16_t texPackRGB565(const uint8_t* px);
uint16_t texPackRGBA4444(const uint8_t* px);
uint8_t texLuma4(const uint8_t* px);
void texEncodeEtc1a4Block(const uint8_t* rgba8888, uint32_t width, uint32_t height, uint32_t blockX,
    uint32_t blockY, uint8_t* out);
uint32_t texBuildPalette(const TextureRgbaRect* source, uint16_t* palette, uint16_t* paletteLookup,
    uint32_t paletteCapacity);
uint8_t texPaletteIndex(
    const uint8_t* px, const uint16_t* palette, uint32_t paletteCount, uint16_t* paletteLookup);
void texWriteCi4Palette(uint8_t* out, const uint16_t* palette);
void texWriteCi8Palette(uint8_t* out, const uint16_t* palette);
void texEncodeCmpr(const TextureRgbaRect* source, uint8_t* out);
void texEncodeRgb5a3(const TextureRgbaRect* source, uint8_t* out);

#endif
