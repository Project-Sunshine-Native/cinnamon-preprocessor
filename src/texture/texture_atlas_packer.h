#ifndef CINNAMON_PREPROCESSOR_TEXTURE_ATLAS_PACKER_H
#define CINNAMON_PREPROCESSOR_TEXTURE_ATLAS_PACKER_H

#include "common/preprocess_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/preprocess_datawin.h"
#include "texture_convert.h"

#define CINNAMON_MIN_ATLAS_TEXTURE_SIZE 128u
#define CINNAMON_SMALL_TEXTURE_SIZE 256u
#define CINNAMON_MEDIUM_TEXTURE_SIZE 512u
#define CINNAMON_LARGE_TEXTURE_SIZE 1024u

#define CINNAMON_OUTPUT_PAGE_PATH_CHARS PREPROCESS_PATH_MAX
#define CINNAMON_OUTPUT_PAGE_PREVIEW_LABEL_CHARS 128
#define CINNAMON_OUTPUT_PAGE_DEBUG_NAMES_CHARS 2048

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t fragmentStart;
    uint16_t fragmentCount;
} OutputItem;

typedef struct {
    uint16_t atlasId;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t sourceX;
    uint16_t sourceY;
} OutputFragment;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t textureFormat;
    char path[CINNAMON_OUTPUT_PAGE_PATH_CHARS];
    char previewLabel[CINNAMON_OUTPUT_PAGE_PREVIEW_LABEL_CHARS];
    char debugNames[CINNAMON_OUTPUT_PAGE_DEBUG_NAMES_CHARS];
    bool containsSprite;
    bool containsFont;
    bool containsBackground;
    bool containsNonMonoContent;
} OutputPage;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} AtlasFreeRect;

typedef struct {
    uint32_t pageIndex;
    uint32_t width;
    uint32_t height;
    AtlasFreeRect* freeRects;
    uint32_t roomMaskWords;
    uint64_t* roomMask;
    uint8_t* pixels;
} PackedPage;

typedef struct {
    uint64_t* tpagRoomMasks;
    uint64_t* tileRequestRoomMasks;
    uint32_t roomMaskWords;
    uint32_t tpagCount;
    uint32_t tileRequestCount;
} RoomAtlasAffinity;

uint64_t* atlasTpagMask(RoomAtlasAffinity* affinity, uint32_t tpagIndex);
uint64_t* atlasTileMask(RoomAtlasAffinity* affinity, uint32_t requestIndex);
bool atlasBuildAffinity(DataWin* dataWin, const TileLookupKey* tileRequests,
    size_t tileRequestCount, RoomAtlasAffinity* out);
void atlasFreeAffinity(RoomAtlasAffinity* affinity);

uint32_t atlasPageSize(const PreprocessRunConfig* options, uint32_t width, uint32_t height,
    bool allowLargePage, uint32_t minAutoPageSize);
void atlasPageDimensions(const PreprocessRunConfig* options, uint32_t width, uint32_t height,
    bool allowLargePage, uint32_t minAutoPageSize, uint32_t* outWidth, uint32_t* outHeight);
uint32_t atlasPadding(CinnamonTextureFormat pageFormat);

typedef struct {
    OutputPage* outputPages;
    uint32_t totalPageCount;
    PackedPage** packedPages;
    uint32_t packedPageCount;
} AtlasPacking;

typedef struct {
    uint32_t width;
    uint32_t height;
    CinnamonTextureFormat format;
    bool allowLargePage;
    uint32_t minAutoPageSize;
    const uint64_t* roomMask;
    uint32_t roomMaskWords;
} AtlasPackRequest;

typedef struct {
    uint16_t pageIndex;
    uint16_t x;
    uint16_t y;
    uint32_t padding;
} AtlasPlacement;

bool atlasPackRect(const PreprocessRunConfig* options, AtlasPacking* atlas,
    const AtlasPackRequest* request, AtlasPlacement* placement);

PackedPage* atlasFindPage(PackedPage** packedPages, uint32_t packedPageCount, uint16_t pageIndex);
void atlasFreePages(PackedPage** packedPages, uint32_t packedPageCount);

#endif
