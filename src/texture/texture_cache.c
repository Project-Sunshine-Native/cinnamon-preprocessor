#include "texture_convert_internal.h"
#include "common/path_utils.h"
#include "common/byteio.h"

static uint32_t hashU16(uint32_t hash, uint16_t value) {
    hash ^= (uint8_t) (value & 0xFFu);
    hash *= 16777619u;
    hash ^= (uint8_t) ((value >> 8) & 0xFFu);
    hash *= 16777619u;
    return hash;
}

static uint32_t hashI16(uint32_t hash, int16_t value) {
    return hashU16(hash, (uint16_t) value);
}

uint32_t texHashTpag(const DataWin* dataWin) {
    uint32_t hash = 2166136261u;
    if (dataWin == NULL) return hash;
    hash ^= dataWin->tpag.count;
    hash *= 16777619u;
    preprocessRepeat(dataWin->tpag.count, i) {
        const TexturePageItem* item = &dataWin->tpag.items[i];
        hash = hashU16(hash, item->sourceX);
        hash = hashU16(hash, item->sourceY);
        hash = hashU16(hash, item->sourceWidth);
        hash = hashU16(hash, item->sourceHeight);
        hash = hashU16(hash, item->targetX);
        hash = hashU16(hash, item->targetY);
        hash = hashU16(hash, item->targetWidth);
        hash = hashU16(hash, item->targetHeight);
        hash = hashU16(hash, item->boundingWidth);
        hash = hashU16(hash, item->boundingHeight);
        hash = hashI16(hash, item->texturePageId);
    }
    return hash != 0u ? hash : 1u;
}

static bool getCacheMarkerPath(const char* outputDir, char* outPath, size_t outPathSize) {
    if (!strNotEmpty(outputDir) || outPath == NULL || outPathSize == 0u) return false;
    char gfxDir[PREPROCESS_PATH_MAX];
    return pathJoin(outputDir, "gfx", gfxDir, sizeof(gfxDir)) &&
        pathJoin(gfxDir, CINNAMON_PREPROCESSOR_READY_FILE, outPath, outPathSize);
}

void texRemoveCache(const char* outputDir) {
    char path[PREPROCESS_TEMP_PATH_MAX];
    if (getCacheMarkerPath(outputDir, path, sizeof(path))) remove(path);
}

static bool atlasCacheIsValid(const char* path, const DataWin* dataWin,
    CinnamonTextureFormat textureFormat, uint32_t tpagHash) {
    if (path == NULL || dataWin == NULL) return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t header[CINNAMON_ATLAS_HEADER_SIZE];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
    fclose(file);
    if (!ok) return false;
    if (readLe32(header + 0u) != CINNAMON_ATLAS_MAGIC) return false;
    uint16_t version = (uint16_t) header[4] | (uint16_t) ((uint16_t) header[5] << 8u);
    if (version != CINNAMON_ATLAS_VERSION) return false;
    if (readLe32(header + 8u) != dataWin->tpag.count) return false;
    if (readLe32(header + 20u) != (uint32_t) textureFormat) return false;
    if (readLe32(header + 24u) != tpagHash) return false;
    return true;
}

static bool atlasFormatsAreSupported(
    const char* path, CinnamonPreprocessorTarget target, bool allowCmprPages) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t header[CINNAMON_ATLAS_HEADER_SIZE];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
    if (!ok || readLe32(header + 0u) != CINNAMON_ATLAS_MAGIC) {
        fclose(file);
        return false;
    }

    uint32_t pageCount = (uint32_t) header[6] | ((uint32_t) header[7] << 8u);
    for (uint32_t i = 0u; i < pageCount; ++i) {
        uint8_t row[16];
        if (fread(row, 1, sizeof(row), file) != sizeof(row)) {
            fclose(file);
            return false;
        }
        uint32_t textureFormat = readLe32(row + 4u);
        if (textureFormat >= 32u ||
            !targetAllowsFormat(target, (CinnamonTextureFormat) textureFormat)) {
            fclose(file);
            return false;
        }
        if (!allowCmprPages && textureFormat == (uint32_t) CINNAMON_TEXFMT_CMPR) {
            fclose(file);
            return false;
        }
    }

    fclose(file);
    return true;
}

static bool tpagBindingsAreValid(FILE* file, const DataWin* dataWin, uint32_t tpagHash) {
    if (file == NULL || dataWin == NULL) return false;
    uint8_t header[16];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
    uint32_t version = ok ? readLe32(header + 0u) : 0u;
    uint32_t tpagCount = ok ? readLe32(header + 4u) : 0u;
    uint32_t manifestHash = ok ? readLe32(header + 8u) : 0u;
    uint32_t rowSize = ok ? readLe32(header + 12u) : 0u;
    if (!ok || version != CINNAMON_TPAG_BINDINGS_VERSION || tpagCount != dataWin->tpag.count ||
        manifestHash != tpagHash || rowSize != CINNAMON_TPAG_BINDINGS_ROW_SIZE) {
        return false;
    }

    bool* usedTPAGs = collectUsedTpags(dataWin);
    preprocessRepeat(tpagCount, i) {
        uint8_t row[CINNAMON_TPAG_BINDINGS_ROW_SIZE];
        const TexturePageItem* item = &dataWin->tpag.items[i];
        bool drawable = usedTPAGs[i] && item->sourceWidth > 0u && item->sourceHeight > 0u;
        uint8_t source = drawable ? CINNAMON_TPAG_SOURCE_ATLAS : CINNAMON_TPAG_SOURCE_NONE;
        if (fread(row, 1, sizeof(row), file) != sizeof(row) || row[0] != source) {
            ok = false;
            break;
        }
    }
    free(usedTPAGs);
    return ok;
}

static bool findAtlasContainerBlock(FILE* file, uint32_t fileSize, uint32_t pageDataEnd,
    uint32_t type, CinnamonAtlasContainerBlock* outBlock) {
    if (outBlock != NULL) memset(outBlock, 0, sizeof(*outBlock));
    if (file == NULL || outBlock == NULL || fileSize < 16u || pageDataEnd >= fileSize) return false;

    uint8_t footer[16];
    uint32_t footerOffset = fileSize - 16u;
    if (fseek(file, (long) footerOffset, SEEK_SET) != 0 ||
        fread(footer, 1, sizeof(footer), file) != sizeof(footer))
        return false;
    if (readLe32(footer + 0u) != CINNAMON_ATLAS_DIRECTORY_MAGIC ||
        readLe32(footer + 4u) != CINNAMON_ATLAS_DIRECTORY_VERSION) {
        return false;
    }

    uint32_t blockCount = readLe32(footer + 8u);
    uint32_t directoryOffset = readLe32(footer + 12u);
    uint64_t directorySize = (uint64_t) blockCount * 16u;
    if (blockCount == 0u || blockCount > 32u || directoryOffset < pageDataEnd ||
        directoryOffset > footerOffset ||
        directorySize > (uint64_t) (footerOffset - directoryOffset)) {
        return false;
    }

    if (fseek(file, (long) directoryOffset, SEEK_SET) != 0) return false;
    for (uint32_t i = 0u; i < blockCount; ++i) {
        uint8_t row[16];
        if (fread(row, 1, sizeof(row), file) != sizeof(row)) return false;
        CinnamonAtlasContainerBlock block = {
            .type = readLe32(row + 0u),
            .version = readLe32(row + 4u),
            .offset = readLe32(row + 8u),
            .size = readLe32(row + 12u),
        };
        if (block.size == 0u || block.offset < pageDataEnd || block.offset > directoryOffset ||
            block.size > directoryOffset - block.offset) {
            return false;
        }
        if (block.type == type) {
            *outBlock = block;
            return true;
        }
    }

    return false;
}

static bool roomManifestIsValid(FILE* file, const DataWin* dataWin) {
    if (file == NULL || dataWin == NULL) return false;
    uint8_t header[32];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) return false;
    uint32_t magic = readLe32(header + 0u);
    uint32_t version = readLe32(header + 4u);
    uint32_t entryCount = readLe32(header + 8u);
    uint32_t pageRefCount = readLe32(header + 12u);
    uint32_t roomCount = readLe32(header + 16u);
    uint32_t tileRefCount = readLe32(header + 20u);
    uint32_t tileRefRowSize = readLe32(header + 24u);
    if (magic != CINNAMON_ROOM_MANIFEST_MAGIC || version != CINNAMON_ROOM_MANIFEST_VERSION ||
        roomCount != dataWin->room.count || tileRefRowSize != CINNAMON_ROOM_MANIFEST_TILE_REF_SIZE)
        return false;

    uint8_t buffer[256];
    uint64_t bytesLeft = (uint64_t) entryCount * CINNAMON_ROOM_MANIFEST_ENTRY_SIZE +
        (uint64_t) pageRefCount * CINNAMON_ROOM_MANIFEST_PAGE_REF_SIZE +
        (uint64_t) tileRefCount * CINNAMON_ROOM_MANIFEST_TILE_REF_SIZE;
    while (bytesLeft > 0u) {
        size_t chunk = bytesLeft > sizeof(buffer) ? sizeof(buffer) : (size_t) bytesLeft;
        if (fread(buffer, 1, chunk, file) != chunk) return false;
        bytesLeft -= chunk;
    }
    return true;
}

static bool atlasMetadataIsValid(const char* path, const DataWin* dataWin, uint32_t tpagHash) {
    if (path == NULL || dataWin == NULL) return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    bool ok = false;
    if (fseek(file, 0, SEEK_END) != 0) goto cleanup;
    long fileSizeLong = ftell(file);
    if (fileSizeLong < 0 || (uint64_t) fileSizeLong < CINNAMON_ATLAS_HEADER_SIZE) goto cleanup;
    if (fseek(file, 0, SEEK_SET) != 0) goto cleanup;

    uint8_t header[CINNAMON_ATLAS_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) goto cleanup;
    if (readLe32(header + 0u) != CINNAMON_ATLAS_MAGIC) goto cleanup;
    if (((uint16_t) header[4] | (uint16_t) ((uint16_t) header[5] << 8u)) != CINNAMON_ATLAS_VERSION)
        goto cleanup;
    if (readLe32(header + 8u) != dataWin->tpag.count) goto cleanup;
    if (readLe32(header + 24u) != tpagHash) goto cleanup;

    uint32_t pageCount = (uint32_t) header[6] | ((uint32_t) header[7] << 8u);
    uint32_t itemCount = readLe32(header + 8u);
    uint32_t fragmentCount = readLe32(header + 12u);
    uint32_t tileEntryCount = readLe32(header + 16u);
    size_t pageDataBase = CINNAMON_ATLAS_HEADER_SIZE +
        (size_t) pageCount * CINNAMON_ATLAS_PAGE_ROW_SIZE +
        (size_t) itemCount * CINNAMON_ATLAS_ITEM_ROW_SIZE +
        (size_t) fragmentCount * CINNAMON_ATLAS_FRAGMENT_ROW_SIZE +
        (size_t) tileEntryCount * CINNAMON_ATLAS_TILE_ROW_SIZE;
    if (pageDataBase > (size_t) fileSizeLong) goto cleanup;

    uint32_t pageDataEnd = (uint32_t) pageDataBase;
    for (uint32_t i = 0u; i < pageCount; ++i) {
        uint8_t row[CINNAMON_ATLAS_PAGE_ROW_SIZE];
        if (fread(row, 1, sizeof(row), file) != sizeof(row)) goto cleanup;
        uint32_t dataOffset = readLe32(row + 8u);
        uint32_t dataSize = readLe32(row + 12u);
        if (dataOffset < pageDataBase || dataOffset > (uint32_t) fileSizeLong ||
            dataSize > (uint32_t) fileSizeLong - dataOffset) {
            goto cleanup;
        }
        uint32_t dataEnd = dataOffset + dataSize;
        if (dataEnd > pageDataEnd) pageDataEnd = dataEnd;
    }

    CinnamonAtlasContainerBlock tpagBlock;
    if (!findAtlasContainerBlock(file, (uint32_t) fileSizeLong, pageDataEnd,
            CINNAMON_ATLAS_BLOCK_TPAG_BINDINGS, &tpagBlock))
        goto cleanup;
    if (tpagBlock.version != CINNAMON_TPAG_BINDINGS_VERSION ||
        tpagBlock.size != 16u + dataWin->tpag.count * CINNAMON_TPAG_BINDINGS_ROW_SIZE)
        goto cleanup;
    if (fseek(file, (long) tpagBlock.offset, SEEK_SET) != 0) goto cleanup;
    if (!tpagBindingsAreValid(file, dataWin, tpagHash)) goto cleanup;
    long afterBlock = ftell(file);
    if (afterBlock < 0 || (uint32_t) afterBlock != tpagBlock.offset + tpagBlock.size) goto cleanup;

    CinnamonAtlasContainerBlock roomBlock;
    if (!findAtlasContainerBlock(file, (uint32_t) fileSizeLong, pageDataEnd,
            CINNAMON_ATLAS_BLOCK_ROOM_MANIFEST, &roomBlock))
        goto cleanup;
    if (roomBlock.version != CINNAMON_ROOM_MANIFEST_VERSION ||
        roomBlock.size < CINNAMON_ROOM_MANIFEST_HEADER_SIZE)
        goto cleanup;
    if (fseek(file, (long) roomBlock.offset, SEEK_SET) != 0) goto cleanup;
    if (!roomManifestIsValid(file, dataWin)) goto cleanup;
    afterBlock = ftell(file);
    if (afterBlock < 0 || (uint32_t) afterBlock != roomBlock.offset + roomBlock.size) goto cleanup;

    ok = true;

cleanup:
    fclose(file);
    return ok;
}

bool texCacheIsCurrent(const PreprocessRunConfig* options,
    const TexturePreprocessPipeline* pipeline, const DataWin* dataWin) {
    if (options == NULL || dataWin == NULL || options->outputDir[0] == '\0' ||
        options->inputPath[0] == '\0')
        return false;
    uint32_t tpagHash = texHashTpag(dataWin);
    char readyPath[PREPROCESS_PATH_MAX];
    if (!getCacheMarkerPath(options->outputDir, readyPath, sizeof(readyPath))) return false;
    FILE* ready = fopen(readyPath, "rb");
    if (ready == NULL) return false;
    uint8_t marker[CINNAMON_PREPROCESSOR_READY_MARKER_SIZE];
    bool ok = fread(marker, 1, sizeof(marker), ready) == sizeof(marker);
    fclose(ready);
    if (!ok) return false;
    if (readLe32(marker + 0u) != CINNAMON_PREPROCESSOR_READY_MAGIC) return false;
    if (readLe32(marker + 4u) != CINNAMON_PREPROCESSOR_READY_VERSION) return false;
    if (readLe32(marker + 8u) != CINNAMON_ATLAS_VERSION) return false;
    if (readLe32(marker + 12u) != CINNAMON_TEXTURE_CACHE_REVISION) return false;
    if (readLe32(marker + 16u) != 0u) return false;
    if (readLe32(marker + 20u) != CINNAMON_ROOM_MANIFEST_VERSION) return false;
    if (readLe32(marker + 24u) != (uint32_t) options->textureFormat) return false;
    if (readLe32(marker + 28u) != tpagHash) return false;
    CinnamonSourceFingerprint fp;
    if (!fingerprintFile(options->inputPath, &fp)) return false;
    if (readLe64(marker + 32u) != fp.size) return false;
    if (readLe32(marker + 40u) != fp.sampleHash) return false;
    if (readLe32(marker + 48u) != (uint32_t) options->atlasPageMode) return false;
    char path[PREPROCESS_TEMP_PATH_MAX];
    if (!pathJoin(options->outputDir, "gfx/atlas.bin", path, sizeof(path))) return false;
    if (!atlasCacheIsValid(path, dataWin, options->textureFormat, tpagHash)) return false;
    if (pipeline != NULL && pipeline->validatePageFormats &&
        !atlasFormatsAreSupported(
            path, options->user.target, options->textureFormat == CINNAMON_TEXFMT_CMPR)) {
        return false;
    }
    if (!atlasMetadataIsValid(path, dataWin, tpagHash)) return false;
    return true;
}
