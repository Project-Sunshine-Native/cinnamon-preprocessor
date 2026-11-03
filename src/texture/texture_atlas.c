#include "texture_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/byteio.h"

typedef struct {
    uint32_t roomIndex;
    uint32_t pageStart;
    uint32_t pageCount;
    uint32_t tileStart;
    uint32_t tileCount;
} CinnamonRoomManifestEntry;

typedef struct {
    uint16_t pageIndex;
    uint8_t flags;
    uint8_t reserved;
} CinnamonRoomManifestPageRef;

typedef struct {
    uint32_t tileEntryIndex;
    int32_t tpagIndex;
} CinnamonRoomManifestTileRef;

static bool fileOffsetFitsU32(long offset) {
    if (offset < 0) return false;
#if LONG_MAX > UINT32_MAX
    if ((unsigned long) offset > UINT32_MAX) return false;
#endif
    return true;
}

static bool getFileSize32(const char* path, uint32_t* outSize) {
    if (path == NULL || outSize == NULL) return false;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    fclose(file);
    if (size < 0 || (uint64_t) size > UINT32_MAX) return false;

    *outSize = (uint32_t) size;
    return true;
}

static bool appendFileToStream(FILE* dst, const char* srcPath) {
    if (dst == NULL || srcPath == NULL) return false;

    FILE* src = fopen(srcPath, "rb");
    if (src == NULL) return false;

    uint8_t* buffer = safeMalloc(64u * 1024u);
    bool ok = true;
    while (ok) {
        size_t bytesRead = fread(buffer, 1, 64u * 1024u, src);
        if (bytesRead > 0 && fwrite(buffer, 1, bytesRead, dst) != bytesRead) ok = false;
        if (bytesRead < 64u * 1024u) {
            if (ferror(src)) ok = false;
            break;
        }
    }

    free(buffer);
    fclose(src);
    return ok;
}

bool texTpagIsFont(const DataWin* dataWin, uint32_t tpagIndex) {
    if (dataWin == NULL || tpagIndex >= dataWin->tpag.count) return false;
    preprocessRepeat(dataWin->font.count, fontIndex) {
        const Font* font = &dataWin->font.fonts[fontIndex];
        if (font->isSpriteFont) continue;
        if (font->tpagIndex == (int32_t) tpagIndex) return true;
    }
    return false;
}

static bool tpagItemIsDrawable(const OutputItem* items, uint32_t itemCount, uint32_t itemIndex) {
    if (items == NULL || itemIndex >= itemCount) return false;
    return items[itemIndex].fragmentCount > 0u && items[itemIndex].fragmentStart != UINT32_MAX;
}

static void writeBindingRow(uint8_t* row, uint8_t source, uint32_t atlasItemIndex) {
    memset(row, 0, CINNAMON_TPAG_BINDINGS_ROW_SIZE);
    row[0] = source;
    /* The runtime's atlas layout retains a scale field for unscaled items. */
    writeLe16(row + 4u, 1u);
    writeLe32(row + 8u, atlasItemIndex);
    writeLe32(row + 12u, UINT32_MAX);
}

bool texBuildBindings(
    const DataWin* dataWin, const OutputItem* items, uint8_t** outRows, uint32_t* outTpagCount) {
    if (outRows != NULL) *outRows = NULL;
    if (outTpagCount != NULL) *outTpagCount = 0u;
    if (dataWin == NULL || (dataWin->tpag.count > 0u && items == NULL) || outRows == NULL ||
        outTpagCount == NULL)
        return false;

    uint32_t tpagCount = dataWin->tpag.count;
    size_t rowBytes = CINNAMON_TPAG_BINDINGS_ROW_SIZE;
    if (tpagCount > 0u &&
        !checkedMulSize((size_t) tpagCount, CINNAMON_TPAG_BINDINGS_ROW_SIZE, &rowBytes))
        return false;
    uint8_t* rows = safeCalloc(1u, rowBytes);
    preprocessRepeat(tpagCount, i) {
        if (tpagItemIsDrawable(items, tpagCount, (uint32_t) i)) {
            writeBindingRow(rows + i * CINNAMON_TPAG_BINDINGS_ROW_SIZE, CINNAMON_TPAG_SOURCE_ATLAS,
                (uint32_t) i);
        } else {
            writeBindingRow(
                rows + i * CINNAMON_TPAG_BINDINGS_ROW_SIZE, CINNAMON_TPAG_SOURCE_NONE, UINT32_MAX);
        }
    }

    *outRows = rows;
    *outTpagCount = tpagCount;
    return true;
}

static bool writeTpagBindingBlock(
    FILE* file, const uint8_t* rows, uint32_t tpagCount, uint32_t tpagHash) {
    if (file == NULL || (rows == NULL && tpagCount > 0u)) return false;
    uint8_t header[16];
    writeLe32(header + 0u, CINNAMON_TPAG_BINDINGS_VERSION);
    writeLe32(header + 4u, tpagCount);
    writeLe32(header + 8u, tpagHash);
    writeLe32(header + 12u, CINNAMON_TPAG_BINDINGS_ROW_SIZE);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    if (ok && tpagCount > 0u)
        ok = fwrite(rows, CINNAMON_TPAG_BINDINGS_ROW_SIZE, tpagCount, file) == tpagCount;
    return ok;
}

static bool writeAtlasDirectory(
    FILE* file, const CinnamonAtlasContainerBlock* blocks, uint32_t blockCount) {
    if (file == NULL || blocks == NULL || blockCount == 0u) return false;
    long directoryOffsetLong = ftell(file);
    if (!fileOffsetFitsU32(directoryOffsetLong)) return false;
    uint32_t directoryOffset = (uint32_t) directoryOffsetLong;

    for (uint32_t i = 0u; i < blockCount; ++i) {
        uint8_t row[16];
        writeLe32(row + 0u, blocks[i].type);
        writeLe32(row + 4u, blocks[i].version);
        writeLe32(row + 8u, blocks[i].offset);
        writeLe32(row + 12u, blocks[i].size);
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) return false;
    }

    uint8_t footer[16];
    writeLe32(footer + 0u, CINNAMON_ATLAS_DIRECTORY_MAGIC);
    writeLe32(footer + 4u, CINNAMON_ATLAS_DIRECTORY_VERSION);
    writeLe32(footer + 8u, blockCount);
    writeLe32(footer + 12u, directoryOffset);
    return fwrite(footer, 1, sizeof(footer), file) == sizeof(footer);
}

static bool beginAtlasBlock(
    FILE* file, CinnamonAtlasContainerBlock* block, uint32_t type, uint32_t version) {
    if (file == NULL || block == NULL) return false;
    long offset = ftell(file);
    if (!fileOffsetFitsU32(offset)) return false;
    block->type = type;
    block->version = version;
    block->offset = (uint32_t) offset;
    block->size = 0u;
    return true;
}

static bool finishAtlasBlock(FILE* file, CinnamonAtlasContainerBlock* block) {
    if (file == NULL || block == NULL) return false;
    long endOffset = ftell(file);
    if (!fileOffsetFitsU32(endOffset) || (uint32_t) endOffset < block->offset) return false;
    block->size = (uint32_t) endOffset - block->offset;
    return block->size > 0u;
}

void texRemoveOldBindings(const char* outputDir) {
    if (outputDir == NULL) return;
    char path[PREPROCESS_PATH_MAX];
    char tmpPath[PREPROCESS_TEMP_PATH_MAX + 4];
    if (!pathJoin(outputDir, "gfx/tpag_bindings.bin", path, sizeof(path)) ||
        !pathWithSuffix(path, ".tmp", tmpPath, sizeof(tmpPath))) {
        return;
    }
    remove(path);
    remove(tmpPath);
}

void texRemoveManifestFiles(const char* outputDir) {
    if (outputDir == NULL) return;
    char path[PREPROCESS_PATH_MAX];
    char tmpPath[PREPROCESS_TEMP_PATH_MAX + 4];

    if (!pathJoin(outputDir, "gfx/room_manifest.bin", path, sizeof(path)) ||
        !pathWithSuffix(path, ".tmp", tmpPath, sizeof(tmpPath))) {
        return;
    }
    remove(path);
    remove(tmpPath);

    if (!pathJoin(outputDir, "gfx/sprite_manifest.bin", path, sizeof(path)) ||
        !pathWithSuffix(path, ".tmp", tmpPath, sizeof(tmpPath))) {
        return;
    }
    remove(path);
    remove(tmpPath);
}

void texRemoveOldAssets(const char* outputDir) {
    if (outputDir == NULL) return;
    char path[PREPROCESS_PATH_MAX];
    char tmpPath[PREPROCESS_TEMP_PATH_MAX + 4];
    char gfxDir[PREPROCESS_PATH_MAX];
    if (!pathJoin(outputDir, "gfx", gfxDir, sizeof(gfxDir))) return;

    if (!pathJoin(gfxDir, "page_formats.txt", path, sizeof(path)) ||
        !pathWithSuffix(path, ".tmp", tmpPath, sizeof(tmpPath))) {
        return;
    }
    remove(path);
    remove(tmpPath);

    static const char* legacyFiles[] = {
        "debugTinyFont.png",
        "debugTinyFont.t3x",
        "loadingDog.png",
        "loadingDog.t3x",
    };
    preprocessRepeat(sizeof(legacyFiles) / sizeof(legacyFiles[0]), i) {
        if (!pathJoin(gfxDir, legacyFiles[i], path, sizeof(path)) ||
            !pathWithSuffix(path, ".tmp", tmpPath, sizeof(tmpPath))) {
            continue;
        }
        remove(path);
        remove(tmpPath);
    }

    static const char* legacyDirs[] = {
        "sprites",
        "backgrounds",
        "borders",
        "fonts",
    };
    preprocessRepeat(sizeof(legacyDirs) / sizeof(legacyDirs[0]), i) {
        if (!pathJoin(gfxDir, legacyDirs[i], path, sizeof(path))) continue;
        if (fsDirExists(path) && !fsDeleteTree(path)) {
            fprintf(stderr, "Warning: failed to remove legacy direct asset output: %s\n", path);
        }
    }
}

static bool validateAtlasRefs(const TextureAtlasData* atlas) {
    if ((atlas->pageCount > 0u && atlas->pages == NULL) ||
        (atlas->itemCount > 0u && atlas->items == NULL) ||
        (atlas->fragmentCount > 0u && atlas->fragments == NULL) ||
        (atlas->tileEntryCount > 0u && atlas->tileEntries == NULL)) {
        return false;
    }

    preprocessRepeat(atlas->itemCount, i) {
        if (atlas->items[i].fragmentCount == 0u) continue;
        if (atlas->items[i].fragmentStart == UINT32_MAX ||
            atlas->items[i].fragmentStart > atlas->fragmentCount ||
            atlas->items[i].fragmentCount > atlas->fragmentCount - atlas->items[i].fragmentStart) {
            fprintf(stderr, "Invalid atlas item fragment range: %zu\n", i);
            return false;
        }
        for (uint32_t j = 0u; j < atlas->items[i].fragmentCount; ++j) {
            const OutputFragment* fragment = &atlas->fragments[atlas->items[i].fragmentStart + j];
            if (fragment->sourceX > atlas->items[i].width ||
                fragment->sourceY > atlas->items[i].height ||
                fragment->width > atlas->items[i].width - fragment->sourceX ||
                fragment->height > atlas->items[i].height - fragment->sourceY) {
                fprintf(stderr,
                    "Invalid atlas item fragment source bounds: item=%zu fragment=%lu\n", i,
                    (unsigned long) j);
                return false;
            }
            if (fragment->atlasId >= atlas->pageCount ||
                !texRectFits(atlas->pages[fragment->atlasId].width,
                    atlas->pages[fragment->atlasId].height, fragment->x, fragment->y,
                    fragment->width, fragment->height)) {
                fprintf(stderr, "Invalid atlas item fragment bounds: item=%zu fragment=%lu\n", i,
                    (unsigned long) j);
                return false;
            }
        }
    }

    preprocessRepeat(atlas->fragmentCount, i) {
        if (atlas->fragments[i].atlasId >= atlas->pageCount) {
            fprintf(stderr, "Invalid atlas fragment page index: %zu\n", i);
            return false;
        }
    }

    preprocessRepeat(atlas->tileEntryCount, i) {
        if (atlas->tileEntries[i].atlasId != UINT16_MAX &&
            atlas->tileEntries[i].atlasId >= atlas->pageCount) {
            fprintf(stderr, "Invalid atlas tile page index: %zu\n", i);
            return false;
        }
        if (atlas->tileEntries[i].atlasId != UINT16_MAX &&
            !texRectFits(atlas->pages[atlas->tileEntries[i].atlasId].width,
                atlas->pages[atlas->tileEntries[i].atlasId].height, atlas->tileEntries[i].x,
                atlas->tileEntries[i].y, atlas->tileEntries[i].width,
                atlas->tileEntries[i].height)) {
            fprintf(stderr, "Invalid atlas tile bounds: %zu\n", i);
            return false;
        }
        if (atlas->tileEntries[i].fragmentCount == 0u) continue;
        if (atlas->tileEntries[i].fragmentStart == UINT32_MAX ||
            atlas->tileEntries[i].fragmentStart > atlas->fragmentCount ||
            atlas->tileEntries[i].fragmentCount >
                atlas->fragmentCount - atlas->tileEntries[i].fragmentStart) {
            fprintf(stderr, "Invalid atlas tile fragment range: %zu\n", i);
            return false;
        }
        for (uint32_t j = 0u; j < atlas->tileEntries[i].fragmentCount; ++j) {
            const OutputFragment* fragment =
                &atlas->fragments[atlas->tileEntries[i].fragmentStart + j];
            if (fragment->sourceX > atlas->tileEntries[i].srcW ||
                fragment->sourceY > atlas->tileEntries[i].srcH ||
                fragment->width > atlas->tileEntries[i].srcW - fragment->sourceX ||
                fragment->height > atlas->tileEntries[i].srcH - fragment->sourceY) {
                fprintf(stderr,
                    "Invalid atlas tile fragment source bounds: tile=%zu fragment=%lu\n", i,
                    (unsigned long) j);
                return false;
            }
            if (fragment->atlasId >= atlas->pageCount ||
                !texRectFits(atlas->pages[fragment->atlasId].width,
                    atlas->pages[fragment->atlasId].height, fragment->x, fragment->y,
                    fragment->width, fragment->height)) {
                fprintf(stderr, "Invalid atlas tile fragment bounds: tile=%zu fragment=%lu\n", i,
                    (unsigned long) j);
                return false;
            }
        }
    }

    return true;
}

static bool writeAtlasMetadata(FILE* file, const TextureAtlasData* atlas,
    const uint32_t* pageDataOffsets, const uint32_t* pageDataSizes) {
    uint8_t header[CINNAMON_ATLAS_HEADER_SIZE];
    writeLe32(header + 0u, CINNAMON_ATLAS_MAGIC);
    writeLe16(header + 4u, CINNAMON_ATLAS_VERSION);
    writeLe16(header + 6u, (uint16_t) atlas->pageCount);
    writeLe32(header + 8u, atlas->itemCount);
    writeLe32(header + 12u, atlas->fragmentCount);
    writeLe32(header + 16u, atlas->tileEntryCount);
    writeLe32(header + 20u, (uint32_t) atlas->textureFormat);
    writeLe32(header + 24u, atlas->tpagHash);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) return false;

    preprocessRepeat(atlas->pageCount, i) {
        uint8_t row[CINNAMON_ATLAS_PAGE_ROW_SIZE];
        writeLe16(row + 0u, atlas->pages[i].width);
        writeLe16(row + 2u, atlas->pages[i].height);
        writeLe32(row + 4u, atlas->pages[i].textureFormat);
        writeLe32(row + 8u, pageDataOffsets[i]);
        writeLe32(row + 12u, pageDataSizes[i]);
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) return false;
    }
    preprocessRepeat(atlas->itemCount, i) {
        uint8_t row[CINNAMON_ATLAS_ITEM_ROW_SIZE];
        writeLe16(row + 0u, atlas->items[i].width);
        writeLe16(row + 2u, atlas->items[i].height);
        writeLe32(row + 4u, atlas->items[i].fragmentStart);
        writeLe16(row + 8u, atlas->items[i].fragmentCount);
        writeLe16(row + 10u, 1u);
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) return false;
    }
    preprocessRepeat(atlas->fragmentCount, i) {
        uint8_t row[CINNAMON_ATLAS_FRAGMENT_ROW_SIZE];
        writeLe16(row + 0u, atlas->fragments[i].atlasId);
        writeLe16(row + 2u, atlas->fragments[i].x);
        writeLe16(row + 4u, atlas->fragments[i].y);
        writeLe16(row + 6u, atlas->fragments[i].width);
        writeLe16(row + 8u, atlas->fragments[i].height);
        writeLe16(row + 10u, atlas->fragments[i].sourceX);
        writeLe16(row + 12u, atlas->fragments[i].sourceY);
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) return false;
    }
    preprocessRepeat(atlas->tileEntryCount, i) {
        uint8_t row[CINNAMON_ATLAS_TILE_ROW_SIZE];
        writeLe16(row + 0u, (uint16_t) atlas->tileEntries[i].bgDef);
        writeLe16(row + 2u, atlas->tileEntries[i].srcX);
        writeLe16(row + 4u, atlas->tileEntries[i].srcY);
        writeLe16(row + 6u, atlas->tileEntries[i].srcW);
        writeLe16(row + 8u, atlas->tileEntries[i].srcH);
        writeLe16(row + 10u, atlas->tileEntries[i].atlasId);
        writeLe16(row + 12u, atlas->tileEntries[i].x);
        writeLe16(row + 14u, atlas->tileEntries[i].y);
        writeLe16(row + 16u, atlas->tileEntries[i].width);
        writeLe16(row + 18u, atlas->tileEntries[i].height);
        writeLe32(row + 20u, atlas->tileEntries[i].fragmentStart);
        writeLe16(row + 24u, atlas->tileEntries[i].fragmentCount);
        if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) return false;
    }
    return true;
}

bool texWriteAtlas(const PreprocessRunConfig* options, const TexturePreprocessPipeline* pipeline,
    const TextureAtlasData* atlas, const char* roomManifestPath) {
    progressPhase(PREPROCESS_PHASE_ATLAS);
    if (atlas->pageCount > UINT16_MAX) {
        fprintf(stderr, "Too many atlas pages for atlas metadata: %u\n", atlas->pageCount);
        return false;
    }
    if (!validateAtlasRefs(atlas)) {
        return false;
    }

    char atlasPath[PREPROCESS_TEMP_PATH_MAX];
    char tmpPath[PREPROCESS_TEMP_PATH_MAX + 4];
    char gfxDir[PREPROCESS_TEMP_PATH_MAX];
    if (!pathJoin(options->outputDir, "gfx", gfxDir, sizeof(gfxDir)) ||
        !pathJoin(gfxDir, "atlas.bin", atlasPath, sizeof(atlasPath)) ||
        !pathWithSuffix(atlasPath, ".tmp", tmpPath, sizeof(tmpPath))) {
        fprintf(stderr, "Atlas output path is too long\n");
        return false;
    }

    uint32_t* pageDataOffsets =
        safeCalloc(atlas->pageCount > 0 ? atlas->pageCount : 1u, sizeof(uint32_t));
    uint32_t* pageDataSizes =
        safeCalloc(atlas->pageCount > 0 ? atlas->pageCount : 1u, sizeof(uint32_t));
    FILE* file = NULL;
    bool ok = false;
    size_t metadataSize = CINNAMON_ATLAS_HEADER_SIZE +
        ((size_t) atlas->pageCount * CINNAMON_ATLAS_PAGE_ROW_SIZE) +
        ((size_t) atlas->itemCount * CINNAMON_ATLAS_ITEM_ROW_SIZE) +
        ((size_t) atlas->fragmentCount * CINNAMON_ATLAS_FRAGMENT_ROW_SIZE) +
        ((size_t) atlas->tileEntryCount * CINNAMON_ATLAS_TILE_ROW_SIZE);
    if (metadataSize > UINT32_MAX) {
        goto cleanup;
    }
    uint32_t currentOffset = (uint32_t) metadataSize;

    preprocessRepeat(atlas->pageCount, i) {
        char pagePath[PREPROCESS_JOINED_PATH_MAX];
        if (!pathJoin(gfxDir, atlas->pages[i].path, pagePath, sizeof(pagePath))) goto cleanup;
        if (!getFileSize32(pagePath, &pageDataSizes[i])) {
            fprintf(stderr, "Failed to determine atlas page size: %s\n", pagePath);
            goto cleanup;
        }
        pageDataOffsets[i] = currentOffset;
        if (UINT32_MAX - currentOffset < pageDataSizes[i]) {
            goto cleanup;
        }
        currentOffset += pageDataSizes[i];
    }

    remove(tmpPath);
    file = fopen(tmpPath, "wb");
    if (file == NULL) {
        fprintf(stderr, "Failed to open atlas output: %s\n", tmpPath);
        goto cleanup;
    }

    if (!writeAtlasMetadata(file, atlas, pageDataOffsets, pageDataSizes)) {
        goto cleanup;
    }

    preprocessRepeat(atlas->pageCount, i) {
        char pagePath[PREPROCESS_JOINED_PATH_MAX];
        if (!pathJoin(gfxDir, atlas->pages[i].path, pagePath, sizeof(pagePath))) goto cleanup;
        if (!appendFileToStream(file, pagePath)) {
            fprintf(stderr, "Failed to append packed atlas page: %s\n", pagePath);
            goto cleanup;
        }
    }

    CinnamonAtlasContainerBlock blocks[2];
    uint32_t blockCount = 0u;

    if (!beginAtlasBlock(file, &blocks[blockCount], CINNAMON_ATLAS_BLOCK_TPAG_BINDINGS,
            CINNAMON_TPAG_BINDINGS_VERSION) ||
        !writeTpagBindingBlock(
            file, atlas->tpagBindingRows, atlas->tpagBindingCount, atlas->tpagHash) ||
        !finishAtlasBlock(file, &blocks[blockCount])) {
        fprintf(stderr, "Failed to append %s TPAG binding map to atlas.\n",
            pipeline != NULL && pipeline->name != NULL ? pipeline->name : "target");
        goto cleanup;
    }
    blockCount++;

    if (roomManifestPath != NULL) {
        if (!beginAtlasBlock(file, &blocks[blockCount], CINNAMON_ATLAS_BLOCK_ROOM_MANIFEST,
                CINNAMON_ROOM_MANIFEST_VERSION) ||
            !appendFileToStream(file, roomManifestPath) ||
            !finishAtlasBlock(file, &blocks[blockCount])) {
            fprintf(stderr, "Failed to append room manifest to atlas: %s\n", roomManifestPath);
            goto cleanup;
        }
        blockCount++;
    }

    if (!writeAtlasDirectory(file, blocks, blockCount)) {
        fprintf(stderr, "Failed to write atlas container directory: %s\n", atlasPath);
        goto cleanup;
    }

    bool closed = fclose(file) == 0;
    file = NULL;
    if (!closed) goto cleanup;

    if (!fsCommitTemp(tmpPath, atlasPath)) {
        fprintf(stderr, "Failed to commit atlas output: %s\n", atlasPath);
        goto cleanup;
    }
    ok = true;

cleanup:
    if (file != NULL) fclose(file);
    if (!ok) remove(tmpPath);
    free(pageDataOffsets);
    free(pageDataSizes);
    if (!ok) return false;

    preprocessRepeat(atlas->pageCount, i) {
        char pagePath[PREPROCESS_JOINED_PATH_MAX];
        if (!pathJoin(gfxDir, atlas->pages[i].path, pagePath, sizeof(pagePath))) continue;
        if (fsFileExists(pagePath) && remove(pagePath) != 0) {
            fprintf(
                stderr, "Warning: failed to remove embedded atlas page sidecar: %s\n", pagePath);
        }
    }
    fprintf(stderr,
        "cinnamon-preprocessor: wrote %satlas (%u pages, %u items, %u fragments) -> %s\n",
        pipeline != NULL && pipeline->atlasLogPrefix != NULL ? pipeline->atlasLogPrefix : "",
        atlas->pageCount, atlas->itemCount, atlas->fragmentCount, atlasPath);
    return true;
}

static void markRoomItemPages(uint32_t tpagIndex, uint32_t totalPageCount, const OutputItem* items,
    const OutputFragment* fragments, bool* seenPages) {
    if (items == NULL || fragments == NULL || seenPages == NULL) return;
    const OutputItem* item = &items[tpagIndex];
    if (item->fragmentCount == 0 || item->fragmentStart == UINT32_MAX) return;

    preprocessRepeat(item->fragmentCount, i) {
        uint32_t fragmentIndex = item->fragmentStart + (uint32_t) i;
        uint16_t pageIndex = fragments[fragmentIndex].atlasId;
        if (pageIndex < totalPageCount) seenPages[pageIndex] = true;
    }
}

static void markRoomSpritePages(const DataWin* dataWin, int32_t spriteIndex,
    uint32_t totalPageCount, const OutputItem* items, const OutputFragment* fragments,
    bool* seenPages) {
    if (dataWin == NULL || items == NULL || fragments == NULL || seenPages == NULL) return;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dataWin->sprt.count) return;

    const Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    preprocessRepeat(sprite->textureCount, i) {
        int32_t tpagIndex = sprite->tpagIndices[i];
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) continue;
        markRoomItemPages((uint32_t) tpagIndex, totalPageCount, items, fragments, seenPages);
    }
}

static void markRoomBackgroundPages(const DataWin* dataWin, int32_t backgroundIndex,
    uint32_t totalPageCount, const OutputItem* items, const OutputFragment* fragments,
    bool* seenPages) {
    if (dataWin == NULL || items == NULL || fragments == NULL || seenPages == NULL) return;
    if (backgroundIndex < 0 || (uint32_t) backgroundIndex >= dataWin->bgnd.count) return;

    const Background* background = &dataWin->bgnd.backgrounds[backgroundIndex];
    if (background->tpagIndex < 0 || (uint32_t) background->tpagIndex >= dataWin->tpag.count)
        return;
    markRoomItemPages(
        (uint32_t) background->tpagIndex, totalPageCount, items, fragments, seenPages);
}

static void markRoomTilePages(const OutputTileEntry* tileEntry, uint32_t totalPageCount,
    const OutputFragment* fragments, bool* seenPages) {
    if (tileEntry == NULL || fragments == NULL || seenPages == NULL) return;

    if (tileEntry->atlasId != UINT16_MAX) {
        if (tileEntry->atlasId < totalPageCount) seenPages[tileEntry->atlasId] = true;
        return;
    }

    if (tileEntry->fragmentCount == 0 || tileEntry->fragmentStart == UINT32_MAX) return;
    preprocessRepeat(tileEntry->fragmentCount, i) {
        uint32_t fragmentIndex = tileEntry->fragmentStart + (uint32_t) i;
        uint16_t pageIndex = fragments[fragmentIndex].atlasId;
        if (pageIndex < totalPageCount) seenPages[pageIndex] = true;
    }
}

static CinnamonRoomManifestTileRef makeRoomManifestTileRef(
    const DataWin* dataWin, TileRequestMap* tileEntryMap, const RoomTile* tile) {
    CinnamonRoomManifestTileRef ref = {
        .tileEntryIndex = UINT32_MAX,
        .tpagIndex = -1,
    };
    if (dataWin == NULL || tile == NULL || tile->backgroundDefinition < 0) return ref;
    if (tile->useSpriteDefinition) {
        if ((uint32_t) tile->backgroundDefinition < dataWin->sprt.count) {
            const Sprite* sprite = &dataWin->sprt.sprites[tile->backgroundDefinition];
            if (sprite->textureCount > 0u && sprite->tpagIndices[0] >= 0 &&
                (uint32_t) sprite->tpagIndices[0] < dataWin->tpag.count) {
                ref.tpagIndex = sprite->tpagIndices[0];
            }
        }
        return ref;
    }
    if ((uint32_t) tile->backgroundDefinition < dataWin->bgnd.count) {
        int32_t tpagIndex = dataWin->bgnd.backgrounds[tile->backgroundDefinition].tpagIndex;
        if (tpagIndex >= 0 && (uint32_t) tpagIndex < dataWin->tpag.count) ref.tpagIndex = tpagIndex;
    }
    if (tile->sourceX < 0 || tile->sourceY < 0 || tile->width == 0u || tile->height == 0u ||
        tile->width > UINT16_MAX || tile->height > UINT16_MAX)
        return ref;
    TileLookupKey key = {
        .bgDef = (int16_t) tile->backgroundDefinition,
        .srcX = (uint16_t) tile->sourceX,
        .srcY = (uint16_t) tile->sourceY,
        .srcW = (uint16_t) tile->width,
        .srcH = (uint16_t) tile->height,
    };
    ptrdiff_t index = hmgeti(tileEntryMap, key);
    if (index >= 0) ref.tileEntryIndex = tileEntryMap[index].value;
    return ref;
}

typedef struct RoomManifestBuildContext {
    const DataWin* dataWin;
    uint32_t totalPageCount;
    const OutputItem* items;
    const OutputFragment* fragments;
    const OutputTileEntry* packedTileEntries;
    TileRequestMap* tileEntryMap;
    CinnamonRoomManifestEntry* entries;
    CinnamonRoomManifestPageRef* pageRefs;
    CinnamonRoomManifestTileRef* tileRefs;
    uint32_t roomWithPagesCount;
} RoomManifestBuildContext;

static void markRoomReferencedPages(
    RoomManifestBuildContext* context, Room* room, bool* seenPages) {
    preprocessRepeat(8u, i) {
        if (room->backgrounds == NULL || !room->backgrounds[i].enabled) continue;
        markRoomBackgroundPages(context->dataWin, room->backgrounds[i].backgroundDefinition,
            context->totalPageCount, context->items, context->fragments, seenPages);
    }

    preprocessRepeat(room->gameObjectCount, objIndex) {
        const RoomGameObject* roomObject = &room->gameObjects[objIndex];
        if (roomObject->objectDefinition < 0 ||
            (uint32_t) roomObject->objectDefinition >= context->dataWin->objt.count)
            continue;
        const GameObject* object = &context->dataWin->objt.objects[roomObject->objectDefinition];
        markRoomSpritePages(context->dataWin, object->spriteId, context->totalPageCount,
            context->items, context->fragments, seenPages);
    }

    preprocessRepeat(room->layerCount, layerIndex) {
        RoomLayer* layer = &room->layers[layerIndex];
        if (layer->backgroundData != NULL) {
            markRoomSpritePages(context->dataWin, layer->backgroundData->spriteIndex,
                context->totalPageCount, context->items, context->fragments, seenPages);
        }
        if (layer->assetsData == NULL) continue;
        preprocessRepeat(layer->assetsData->spriteCount, spriteSlot) {
            markRoomSpritePages(context->dataWin,
                layer->assetsData->sprites[spriteSlot].spriteIndex, context->totalPageCount,
                context->items, context->fragments, seenPages);
        }
    }

    TileLookupKey* roomTileKeys = NULL;
    TileRequestMap* roomTileDedupe = NULL;
    collectRoomTiles(room, (DataWin*) context->dataWin, &roomTileKeys, &roomTileDedupe);
    preprocessRepeat(arrlen(roomTileKeys), tileKeyIndex) {
        ptrdiff_t packedTileIndex = hmgeti(context->tileEntryMap, roomTileKeys[tileKeyIndex]);
        if (packedTileIndex < 0) continue;
        markRoomTilePages(&context->packedTileEntries[context->tileEntryMap[packedTileIndex].value],
            context->totalPageCount, context->fragments, seenPages);
    }
    arrfree(roomTileKeys);
    hmfree(roomTileDedupe);
}

static uint32_t appendRoomTileRefs(RoomManifestBuildContext* context, const Room* room) {
    uint32_t tileStart = (uint32_t) arrlen(context->tileRefs);
    preprocessRepeat(room->tileCount, tileIndex) {
        CinnamonRoomManifestTileRef ref = makeRoomManifestTileRef(
            context->dataWin, context->tileEntryMap, &room->tiles[tileIndex]);
        arrput(context->tileRefs, ref);
    }
    preprocessRepeat(room->layerCount, layerIndex) {
        RoomLayerAssetsData* assets = room->layers[layerIndex].assetsData;
        if (assets == NULL) continue;
        preprocessRepeat(assets->legacyTileCount, tileIndex) {
            CinnamonRoomManifestTileRef ref = makeRoomManifestTileRef(
                context->dataWin, context->tileEntryMap, &assets->legacyTiles[tileIndex]);
            arrput(context->tileRefs, ref);
        }
    }
    return tileStart;
}

static void appendRoomManifestEntry(RoomManifestBuildContext* context, uint32_t roomIndex) {
    progressRoom((int32_t) roomIndex);
    DataWin_loadRoomPayload((DataWin*) context->dataWin, (int32_t) roomIndex);
    Room* room = &context->dataWin->room.rooms[roomIndex];
    bool* seenPages = safeCalloc(context->totalPageCount, sizeof(*seenPages));
    uint32_t pageStart = (uint32_t) arrlen(context->pageRefs);

    markRoomReferencedPages(context, room, seenPages);
    uint32_t tileStart = appendRoomTileRefs(context, room);

    preprocessRepeat(context->totalPageCount, pageIndex) {
        if (!seenPages[pageIndex]) continue;
        CinnamonRoomManifestPageRef ref = {
            .pageIndex = (uint16_t) pageIndex,
            .flags = 0,
            .reserved = 0,
        };
        arrput(context->pageRefs, ref);
    }

    uint32_t pageCount = (uint32_t) arrlen(context->pageRefs) - pageStart;
    if (pageCount > 0) context->roomWithPagesCount++;
    CinnamonRoomManifestEntry entry = {
        .roomIndex = roomIndex,
        .pageStart = pageStart,
        .pageCount = pageCount,
        .tileStart = tileStart,
        .tileCount = (uint32_t) arrlen(context->tileRefs) - tileStart,
    };
    arrput(context->entries, entry);
    free(seenPages);
}

static bool writeRoomManifestFile(const char* manifestPath, const DataWin* dataWin,
    const CinnamonRoomManifestEntry* entries, const CinnamonRoomManifestPageRef* pageRefs,
    const CinnamonRoomManifestTileRef* tileRefs) {
    FILE* file = fopen(manifestPath, "wb");
    if (file == NULL) return false;

    size_t entryLen = arrlenu(entries);
    size_t pageRefLen = arrlenu(pageRefs);
    size_t tileRefLen = arrlenu(tileRefs);
    uint8_t header[CINNAMON_ROOM_MANIFEST_HEADER_SIZE] = {0};
    writeLe32(header, CINNAMON_ROOM_MANIFEST_MAGIC);
    writeLe32(header + 4u, CINNAMON_ROOM_MANIFEST_VERSION);
    writeLe32(header + 8u, (uint32_t) entryLen);
    writeLe32(header + 12u, (uint32_t) pageRefLen);
    writeLe32(header + 16u, dataWin->room.count);
    writeLe32(header + 20u, (uint32_t) tileRefLen);
    writeLe32(header + 24u, CINNAMON_ROOM_MANIFEST_TILE_REF_SIZE);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (size_t i = 0; ok && i < entryLen; ++i) {
        uint8_t row[CINNAMON_ROOM_MANIFEST_ENTRY_SIZE] = {0};
        writeLe32(row, entries[i].roomIndex);
        writeLe32(row + 4u, entries[i].pageStart);
        writeLe32(row + 8u, entries[i].pageCount);
        writeLe32(row + 12u, entries[i].tileStart);
        writeLe32(row + 16u, entries[i].tileCount);
        ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
    }
    for (size_t i = 0; ok && i < pageRefLen; ++i) {
        uint8_t row[CINNAMON_ROOM_MANIFEST_PAGE_REF_SIZE];
        writeLe16(row, pageRefs[i].pageIndex);
        row[2] = pageRefs[i].flags;
        row[3] = 0;
        ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
    }
    for (size_t i = 0; ok && i < tileRefLen; ++i) {
        uint8_t row[CINNAMON_ROOM_MANIFEST_TILE_REF_SIZE];
        writeLe32(row, tileRefs[i].tileEntryIndex);
        writeLe32(row + 4u, (uint32_t) tileRefs[i].tpagIndex);
        ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
    }
    if (fclose(file) != 0) ok = false;
    return ok;
}

bool texWriteRoomManifest(
    const PreprocessRunConfig* options, const DataWin* dataWin, const TextureAtlasData* atlas) {
    if (options == NULL || options->outputDir[0] == '\0' || dataWin == NULL ||
        (atlas->itemCount > 0u && atlas->items == NULL) ||
        (atlas->fragmentCount > 0u && atlas->fragments == NULL))
        return false;
    progressPhase(PREPROCESS_PHASE_ROOM_MANIFEST);
    progressRoom(-1);

    char manifestPath[PREPROCESS_TEMP_PATH_MAX];
    if (!pathJoin(
            options->outputDir, "gfx/room_manifest.bin", manifestPath, sizeof(manifestPath))) {
        return false;
    }

    TileRequestMap* tileEntryMap = NULL;
    preprocessRepeat(atlas->tileEntryCount, i) {
        TileLookupKey key = {
            .bgDef = atlas->tileEntries[i].bgDef,
            .srcX = atlas->tileEntries[i].srcX,
            .srcY = atlas->tileEntries[i].srcY,
            .srcW = atlas->tileEntries[i].srcW,
            .srcH = atlas->tileEntries[i].srcH,
        };
        hmput(tileEntryMap, key, i);
    }

    RoomManifestBuildContext context = {
        .dataWin = dataWin,
        .totalPageCount = atlas->pageCount,
        .items = atlas->items,
        .fragments = atlas->fragments,
        .packedTileEntries = atlas->tileEntries,
        .tileEntryMap = tileEntryMap,
    };
    preprocessRepeat(dataWin->room.count, roomIndex) {
        appendRoomManifestEntry(&context, roomIndex);
    }

    size_t pageRefLen = arrlenu(context.pageRefs);
    size_t tileRefLen = arrlenu(context.tileRefs);
    bool ok = writeRoomManifestFile(
        manifestPath, dataWin, context.entries, context.pageRefs, context.tileRefs);
    if (!ok) fprintf(stderr, "Failed to write room manifest output: %s\n", manifestPath);

    fprintf(stderr,
        "cinnamon-preprocessor: staged room manifest for atlas (%u rooms, %u non-empty, %u page "
        "refs, %u tile refs)\n",
        dataWin->room.count, context.roomWithPagesCount, (unsigned int) pageRefLen,
        (unsigned int) tileRefLen);

    hmfree(tileEntryMap);
    arrfree(context.entries);
    arrfree(context.pageRefs);
    arrfree(context.tileRefs);
    progressRoom(-1);
    return ok;
}
