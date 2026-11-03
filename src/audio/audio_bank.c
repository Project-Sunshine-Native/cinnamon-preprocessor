#include "audio_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/byteio.h"

typedef struct {
    PreprocessRunConfig* options;
    DataWin* dataWin;
    char bankPath[PREPROCESS_TEMP_PATH_MAX];
    char tempBankPath[PREPROCESS_TEMP_PATH_MAX + 4];
    ExternalAudioGroupCacheEntry* externalGroupCache;
    uint32_t soundCount;
    uint32_t headerSize;
    uint8_t* header;
    SbkEntry* entries;
    FILE* bankFile;
    bool preservedExisting;
    const SbkEntry* streamedEntries;
    uint32_t packedCount;
    uint32_t placeholderCount;
    uint32_t omittedLongCount;
    uint32_t packedBytes;
    uint8_t* silentBlob;
    SbkEntry silentEntry;
    uint32_t cursor;
} SoundBankContext;

#define CINNAMON_SOUND_BANK_LEGACY_STREAMED_MUSIC 0x00001000u

bool audioBankIsComplete(
    const char* path, uint32_t expectedSoundCount, uint32_t requiredFormatFlag) {
    if (path == NULL || path[0] == '\0') return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    bool ok = false;
    uint8_t header[16];
    uint8_t* tableData = NULL;
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) goto cleanup;

    uint32_t magic = readLe32(header + 0u);
    uint32_t version = readLe32(header + 4u);
    uint32_t entryCount = readLe32(header + 8u);
    uint32_t dataOffset = readLe32(header + 12u);
    if (magic != CINNAMON_SOUND_BANK_MAGIC || version == 0u ||
        version > CINNAMON_SOUND_BANK_VERSION)
        goto cleanup;
    if (requiredFormatFlag == CINNAMON_SOUND_BANK_ENTRY_FLAG_DSP_ADPCM &&
        version < CINNAMON_SOUND_BANK_VERSION)
        goto cleanup;
    if (entryCount != expectedSoundCount) goto cleanup;

    if (fseek(file, 0, SEEK_END) != 0) goto cleanup;
    long fileSizeLong = ftell(file);
    if (fileSizeLong < 0) goto cleanup;
    if ((uint64_t) fileSizeLong > UINT32_MAX) goto cleanup;
    uint32_t fileSize = (uint32_t) fileSizeLong;

    uint32_t entryStride = 20u;
    if (entryCount > (UINT32_MAX - 16u) / entryStride) goto cleanup;
    if (dataOffset < 16u + entryCount * entryStride) entryStride = 8u;
    uint32_t tableBytes = entryCount * entryStride;
    if (dataOffset < 16u || dataOffset > fileSize || 16u + tableBytes > dataOffset) goto cleanup;
    if (fseek(file, 16, SEEK_SET) != 0) goto cleanup;

    if (entryCount > 0u) {
        tableData = safeMalloc(tableBytes);
        if (fread(tableData, 1, tableBytes, file) != tableBytes) goto cleanup;
    }

    preprocessRepeat(entryCount, soundIndex) {
        uint32_t tableOffset = (uint32_t) soundIndex * entryStride;
        uint32_t offset = readLe32(tableData + tableOffset + 0u);
        uint32_t size = readLe32(tableData + tableOffset + 4u);
        if (requiredFormatFlag != 0u && entryStride < 20u) goto cleanup;
        if (requiredFormatFlag != 0u && size != 0u) {
            uint32_t flags = readLe32(tableData + tableOffset + 16u);
            if ((flags & requiredFormatFlag) == 0u) goto cleanup;
        }
        if (size == 0u) continue;
        if (offset < dataOffset || offset >= fileSize) goto cleanup;
        if (size > fileSize - offset) goto cleanup;
    }

    ok = true;

cleanup:
    free(tableData);
    fclose(file);
    return ok;
}

static bool appendSoundBankEntry(
    SoundBankContext* ctx, size_t soundIndex, const uint8_t* blob, SbkEntry entry) {
    uint32_t channelCount = entry.flags & CINNAMON_SOUND_BANK_ENTRY_CHANNEL_MASK;
    if (ctx == NULL || ctx->bankFile == NULL || ctx->entries == NULL || blob == NULL ||
        soundIndex >= ctx->soundCount || entry.size == 0u || entry.sampleRate == 0u ||
        channelCount == 0u || channelCount > CINNAMON_DSP_MAX_CHANNELS ||
        entry.size > UINT32_MAX - ctx->cursor) {
        return false;
    }
    if (fwrite(blob, 1, entry.size, ctx->bankFile) != entry.size) return false;

    entry.offset = ctx->cursor;
    ctx->entries[soundIndex] = entry;
    ctx->cursor += entry.size;
    return true;
}

static bool appendSilentPlaceholder(SoundBankContext* ctx, size_t soundIndex, const Sound* sound,
    uint32_t entryFlags, const char* reason) {
    fprintf(stderr, "Using silent sound bank placeholder for entry %zu (%s): %s\n", soundIndex,
        sound->name != NULL ? sound->name : "<unnamed>", reason);
    SbkEntry entry = ctx->silentEntry;
    entry.flags |= entryFlags;
    if (!appendSoundBankEntry(ctx, soundIndex, ctx->silentBlob, entry)) return false;
    ctx->placeholderCount++;
    ctx->packedBytes += entry.size;
    return true;
}

static bool packSoundBankEntry(SoundBankContext* ctx, size_t soundIndex) {
    const Sound* sound = &ctx->dataWin->sond.sounds[soundIndex];
    uint32_t entryFlags = CINNAMON_SOUND_BANK_ENTRY_FLAG_DSP_ADPCM;
    const SbkEntry* streamed =
        ctx->streamedEntries != NULL ? &ctx->streamedEntries[soundIndex] : NULL;
    if (streamed != NULL && streamed->size != 0u) {
        ctx->entries[soundIndex] = (SbkEntry){0};
        return true;
    }
    WavPcm16 pcm = {0};
    uint8_t* blob = NULL;
    uint32_t size = 0;
    bool ok = false;

    fprintf(stderr, "Packing sound bank entry %zu (%s)\n", soundIndex,
        sound->name != NULL ? sound->name : "<unnamed>");
    fflush(stderr);
    char* externalSourcePath = audioFindSource(ctx->options, sound);
    bool loaded;
    if (externalSourcePath != NULL) {
        loaded = audioLoad(externalSourcePath, &pcm);
    } else {
        uint32_t sourceSize = 0;
        uint8_t* source = audioReadSource(
            ctx->options, ctx->dataWin, sound, &ctx->externalGroupCache, &sourceSize);
        loaded = audioDecode(source, sourceSize, &pcm);
        free(source);
    }
    free(externalSourcePath);
    if (!loaded) {
        ok = appendSilentPlaceholder(
            ctx, soundIndex, sound, entryFlags, "unsupported or missing audio source");
        goto cleanup;
    }
    if (!audioDownmixMono(&pcm)) {
        ok = appendSilentPlaceholder(
            ctx, soundIndex, sound, entryFlags, "failed to downmix audio to mono");
        goto cleanup;
    }
    if (!audioResample(&pcm, CINNAMON_AUDIO_SAMPLE_RATE)) {
        ok =
            appendSilentPlaceholder(ctx, soundIndex, sound, entryFlags, "failed to resample audio");
        goto cleanup;
    }
    uint32_t rate = pcm.sampleRate != 0 ? pcm.sampleRate : CINNAMON_AUDIO_SAMPLE_RATE;
    uint32_t samples = pcm.sampleCount;
    uint32_t channels = pcm.channelCount;
    if (rate != 0u &&
        (uint64_t) samples > (uint64_t) rate * CINNAMON_SOUND_BANK_MAX_PACKED_SECONDS) {
        fprintf(stderr,
            "Omitting sound bank entry %zu (%s): duration %.2f seconds exceeds %u second "
            "packed-bank limit\n",
            soundIndex, sound->name != NULL ? sound->name : "<unnamed>",
            (double) samples / (double) rate, CINNAMON_SOUND_BANK_MAX_PACKED_SECONDS);
        ctx->entries[soundIndex] = (SbkEntry){0};
        ctx->omittedLongCount++;
        ok = true;
        goto cleanup;
    }
    if (!audioBuildDsp(&pcm, &blob, &size)) {
        ok = appendSilentPlaceholder(ctx, soundIndex, sound, entryFlags, "failed to encode ADPCM");
        goto cleanup;
    }
    SbkEntry entry = {
        .size = size,
        .sampleRate = rate,
        .sampleCount = samples,
        .flags = channels | entryFlags,
    };
    ok = appendSoundBankEntry(ctx, soundIndex, blob, entry);
    if (ok) {
        ctx->packedCount++;
        ctx->packedBytes += size;
    }
cleanup:
    free(blob);
    audioFree(&pcm);
    return ok;
}

static bool streamedCacheMatches(const SoundBankContext* ctx) {
    FILE* file = fopen(ctx->bankPath, "rb");
    if (file == NULL) return false;
    bool ok = fseek(file, 16, SEEK_SET) == 0;
    for (uint32_t i = 0u; ok && i < ctx->soundCount; ++i) {
        uint8_t row[20];
        if (fread(row, 1, sizeof(row), file) != sizeof(row)) {
            ok = false;
            break;
        }
        const SbkEntry* stream = ctx->streamedEntries != NULL ? &ctx->streamedEntries[i] : NULL;
        bool externalized = stream != NULL && stream->size != 0u;
        if (externalized) {
            for (uint32_t j = 0u; ok && j < sizeof(row); ++j) ok = row[j] == 0u;
        } else if ((readLe32(row + 16u) & CINNAMON_SOUND_BANK_LEGACY_STREAMED_MUSIC) != 0u) {
            ok = false;
        }
    }
    fclose(file);
    return ok;
}

static bool initSoundBankContext(SoundBankContext* ctx) {
    if (ctx->dataWin->sond.count > (UINT32_MAX - 16u) / 20u) return false;
    ctx->soundCount = (uint32_t) ctx->dataWin->sond.count;
    ctx->headerSize = 16u + ctx->soundCount * 20u;
    ctx->header = safeCalloc(1, ctx->headerSize);
    ctx->entries = safeCalloc(ctx->soundCount, sizeof(*ctx->entries));

    if (!pathJoin(ctx->options->outputDir, "audio/sound_bank.bin", ctx->bankPath,
            sizeof(ctx->bankPath)) ||
        !pathWithSuffix(ctx->bankPath, ".tmp", ctx->tempBankPath, sizeof(ctx->tempBankPath))) {
        fprintf(stderr, "Sound bank output path is too long\n");
        return false;
    }
    if (ctx->options->existingAudioAction == CINNAMON_AUDIO_EXISTING_ACTION_SKIP &&
        fsFileExists(ctx->bankPath)) {
        if (audioBankIsComplete(
                ctx->bankPath, ctx->soundCount, CINNAMON_SOUND_BANK_ENTRY_FLAG_DSP_ADPCM) &&
            streamedCacheMatches(ctx)) {
            fprintf(stderr, "Preserving existing packed sound bank: %s\n", ctx->bankPath);
            ctx->preservedExisting = true;
            return true;
        }
        fprintf(stderr, "Existing packed sound bank is incomplete or invalid; rebuilding: %s\n",
            ctx->bankPath);
    }
    int16_t silence[CINNAMON_DSP_SAMPLES_PER_FRAME] = {0};
    WavPcm16 silentPcm = {
        .sampleRate = CINNAMON_AUDIO_SAMPLE_RATE,
        .sampleCount = CINNAMON_DSP_SAMPLES_PER_FRAME,
        .channelCount = 1u,
        .interleavedPcm = silence,
    };
    if (!audioBuildDsp(&silentPcm, &ctx->silentBlob, &ctx->silentEntry.size)) {
        fprintf(stderr, "Failed to create silent sound bank placeholder\n");
        return false;
    }
    ctx->silentEntry.sampleRate = silentPcm.sampleRate;
    ctx->silentEntry.sampleCount = silentPcm.sampleCount;
    ctx->silentEntry.flags = silentPcm.channelCount;
    remove(ctx->tempBankPath);
    ctx->bankFile = fopen(ctx->tempBankPath, "wb");
    if (ctx->bankFile == NULL) {
        fprintf(stderr, "Failed to create packed sound bank: %s\n", ctx->tempBankPath);
        return false;
    }
    if (fseek(ctx->bankFile, (long) ctx->headerSize, SEEK_SET) != 0) return false;

    ctx->cursor = ctx->headerSize;
    return true;
}

static bool writeSoundBankHeader(SoundBankContext* ctx) {
    writeLe32(ctx->header + 0u, CINNAMON_SOUND_BANK_MAGIC);
    writeLe32(ctx->header + 4u, CINNAMON_SOUND_BANK_VERSION);
    writeLe32(ctx->header + 8u, ctx->soundCount);
    writeLe32(ctx->header + 12u, ctx->headerSize);
    preprocessRepeat(ctx->soundCount, soundIndex) {
        uint32_t entryOffset = 16u + soundIndex * 20u;
        const SbkEntry* entry = &ctx->entries[soundIndex];
        writeLe32(ctx->header + entryOffset + 0u, entry->offset);
        writeLe32(ctx->header + entryOffset + 4u, entry->size);
        writeLe32(ctx->header + entryOffset + 8u, entry->sampleRate);
        writeLe32(ctx->header + entryOffset + 12u, entry->sampleCount);
        writeLe32(ctx->header + entryOffset + 16u, entry->flags);
    }

    if (fseek(ctx->bankFile, 0, SEEK_SET) != 0) return false;
    if (fwrite(ctx->header, 1, ctx->headerSize, ctx->bankFile) != ctx->headerSize) return false;

    fprintf(stderr,
        "Packed %u sounds and %u silent placeholders into %s (%u bytes, %u SOND entries)\n",
        ctx->packedCount, ctx->placeholderCount, ctx->bankPath, ctx->packedBytes, ctx->soundCount);
    if (ctx->omittedLongCount != 0u) {
        fprintf(stderr, "Omitted %u sound bank entr%s longer than %u seconds\n",
            ctx->omittedLongCount, ctx->omittedLongCount == 1u ? "y" : "ies",
            CINNAMON_SOUND_BANK_MAX_PACKED_SECONDS);
    }
    return true;
}

static void destroySoundBankContext(SoundBankContext* ctx) {
    audioFreeGroupCache(ctx->externalGroupCache);
    free(ctx->silentBlob);
    free(ctx->header);
    free(ctx->entries);
}

bool audioBuildBank(
    PreprocessRunConfig* options, DataWin* dataWin, const SbkEntry* streamedEntries) {
    if (options == NULL || dataWin == NULL) return false;
    SoundBankContext state = {
        .options = options, .dataWin = dataWin, .streamedEntries = streamedEntries};
    SoundBankContext* ctx = &state;
    bool ok = false;
    if (!initSoundBankContext(ctx)) goto cleanup;
    if (ctx->preservedExisting) {
        ok = true;
        goto cleanup;
    }
    preprocessRepeat(dataWin->sond.count, i) {
        if (!packSoundBankEntry(ctx, i)) goto cleanup;
    }
    ok = writeSoundBankHeader(ctx);
cleanup:
    if (ctx->bankFile != NULL) {
        if (fclose(ctx->bankFile) != 0) ok = false;
        ctx->bankFile = NULL;
    }
    if (ok && !ctx->preservedExisting) {
        if (!fsCommitTemp(ctx->tempBankPath, ctx->bankPath)) {
            fprintf(stderr, "Failed to publish packed sound bank: %s -> %s\n", ctx->tempBankPath,
                ctx->bankPath);
            ok = false;
        }
    }
    if (!ok || ctx->preservedExisting) remove(ctx->tempBankPath);
    destroySoundBankContext(ctx);
    return ok;
}
