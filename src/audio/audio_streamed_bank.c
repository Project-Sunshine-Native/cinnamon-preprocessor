#include "audio_convert_internal.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "common/string_utils.h"
#include "common/byteio.h"

static bool copyRelativeOggPath(const char* value, char* out, size_t outSize) {
    if (value == NULL || value[0] == '\0' || value[0] == '/' || value[0] == '\\' ||
        strchr(value, ':') != NULL || !strEndsWithNoCase(value, ".ogg"))
        return false;
    size_t len = strlen(value);
    if (len + 1u > outSize) return false;
    size_t segmentStart = 0u;
    for (size_t i = 0u; i <= len; ++i) {
        char c = value[i];
        if (c == '\\') c = '/';
        out[i] = c;
        if (c != '/' && c != '\0') continue;
        size_t segmentLength = i - segmentStart;
        if (segmentLength == 0u || (segmentLength == 1u && out[segmentStart] == '.') ||
            (segmentLength == 2u && out[segmentStart] == '.' && out[segmentStart + 1u] == '.'))
            return false;
        segmentStart = i + 1u;
    }
    return true;
}

static bool copyRelativePath(const char* root, const char* path, char* out, size_t outSize) {
    size_t rootLength = strlen(root);
    while (rootLength > 0u && (root[rootLength - 1u] == '/' || root[rootLength - 1u] == '\\'))
        --rootLength;
    if (strncmp(root, path, rootLength) != 0 ||
        (path[rootLength] != '/' && path[rootLength] != '\\'))
        return false;
    const char* relative = path + rootLength + 1u;
    size_t length = strlen(relative);
    if (length == 0u || length + 1u > outSize) return false;
    for (size_t i = 0u; i <= length; ++i) out[i] = relative[i] == '\\' ? '/' : relative[i];
    return true;
}

static uint8_t* makeExternalPathRecord(const PreprocessRunConfig* options, const Sound* sound,
    const char* sourcePath, uint32_t* outSize) {
    char relative[PREPROCESS_PATH_MAX];
    char sourceRelative[PREPROCESS_PATH_MAX];
    const char* outputDir = options->outputDir;
    const char* prefix = "../";
    char* sourceRoot = pathDupParent(options->inputPath);
    bool haveRelative = sourceRoot != NULL &&
        copyRelativePath(sourceRoot, sourcePath, sourceRelative, sizeof(sourceRelative));
    if (!haveRelative) {
        free(sourceRoot);
        sourceRoot = resolveInstallRoot(options->gameProfiles, options->inputPath);
        haveRelative = sourceRoot != NULL &&
            copyRelativePath(sourceRoot, sourcePath, sourceRelative, sizeof(sourceRelative));
        if (strNotEmpty(options->sharedOutputDir)) {
            outputDir = options->sharedOutputDir;
            prefix = "../../";
        }
    }
    free(sourceRoot);
    haveRelative = haveRelative && copyRelativeOggPath(sourceRelative, relative, sizeof(relative));
    const char* sourceName = pathBaseName(sourcePath);
    if (!haveRelative) {
        haveRelative = copyRelativeOggPath(sound->file, relative, sizeof(relative));
        if (!haveRelative)
            haveRelative = copyRelativeOggPath(sourceName, relative, sizeof(relative));
    }
    if (!haveRelative) return NULL;

    char* destination = pathJoinAlloc(outputDir, relative);
    if (destination == NULL) return NULL;
    bool ok = fsFileExists(destination) && fsFilesEqual(sourcePath, destination);
    if (!ok) ok = fsCopyAtomic(sourcePath, destination);
    free(destination);
    if (!ok) return NULL;

    size_t relativeLength = strlen(relative);
    size_t prefixLength = strlen(prefix);
    if (relativeLength > UINT32_MAX - prefixLength) return NULL;
    *outSize = (uint32_t) (relativeLength + prefixLength);
    uint8_t* record = safeMalloc(*outSize);
    memcpy(record, prefix, prefixLength);
    memcpy(record + prefixLength, relative, relativeLength);
    return record;
}

bool audioBuildStreamBank(PreprocessRunConfig* options, DataWin* dataWin, SbkEntry** outEntries) {
    if (options == NULL || dataWin == NULL || outEntries == NULL) return false;
    *outEntries = NULL;
    uint32_t count = dataWin->sond.count;
    if (count >
        (UINT32_MAX - CINNAMON_STREAMED_BANK_HEADER_SIZE) / CINNAMON_STREAMED_BANK_ENTRY_SIZE)
        return false;
    uint32_t headerSize =
        CINNAMON_STREAMED_BANK_HEADER_SIZE + count * CINNAMON_STREAMED_BANK_ENTRY_SIZE;
    uint8_t* header = safeCalloc(1, headerSize);
    SbkEntry* entries = safeCalloc(count != 0u ? count : 1u, sizeof(*entries));
    ExternalAudioGroupCacheEntry* groupCache = NULL;
    char path[PREPROCESS_EXTENDED_PATH_MAX];
    char tempPath[PREPROCESS_EXTENDED_PATH_MAX + 4];
    if (!pathJoin(options->outputDir, "audio/streamed.bin", path, sizeof(path)) ||
        !pathWithSuffix(path, ".tmp", tempPath, sizeof(tempPath))) {
        fprintf(stderr, "Streamed audio bank output path is too long\n");
        free(entries);
        free(header);
        return false;
    }
    FILE* file = fopen(tempPath, "wb");
    bool ok = file != NULL;
    uint32_t cursor = headerSize;
    uint32_t streamedCount = 0u;
    if (ok) ok = fwrite(header, 1, headerSize, file) == headerSize;

    for (uint32_t i = 0u; ok && i < count; ++i) {
        const Sound* sound = &dataWin->sond.sounds[i];
        uint32_t size = 0u;
        char* externalPath = audioFindSource(options, sound);
        uint8_t* payload = externalPath != NULL ? fsReadFile(externalPath, &size) : NULL;
        if (payload == NULL) {
            free(externalPath);
            externalPath = NULL;
            payload = audioReadSource(options, dataWin, sound, &groupCache, &size);
        }
        WavPcm16 info = {0};
        if (!audioProbe(payload, size, &info) ||
            (uint64_t) info.sampleCount <=
                (uint64_t) info.sampleRate * CINNAMON_SOUND_BANK_MAX_PACKED_SECONDS) {
            free(payload);
            free(externalPath);
            continue;
        }

        uint32_t codec = CINNAMON_STREAMED_BANK_FLAG_DSP_ADPCM;
        if (size >= 4u && memcmp(payload, "OggS", 4u) == 0) {
            codec = CINNAMON_STREAMED_BANK_FLAG_OGG_VORBIS;
            if (externalPath != NULL) {
                uint8_t* record = makeExternalPathRecord(options, sound, externalPath, &size);
                free(payload);
                payload = record;
                if (payload != NULL)
                    codec |= CINNAMON_STREAMED_BANK_FLAG_EXTERNAL_FILE;
                else
                    ok = false;
            }
        } else if (size < 4u || memcmp(payload, "WDSP", 4u) != 0) {
            WavPcm16 pcm = {0};
            ok =
                audioDecode(payload, size, &pcm) && audioResample(&pcm, CINNAMON_AUDIO_SAMPLE_RATE);
            free(payload);
            payload = NULL;
            if (ok) ok = audioBuildDsp(&pcm, &payload, &size);
            info.sampleRate = pcm.sampleRate;
            info.sampleCount = pcm.sampleCount;
            info.channelCount = pcm.channelCount;
            audioFree(&pcm);
        }
        free(externalPath);
        if (ok) ok = size <= UINT32_MAX - cursor;
        if (ok) ok = fwrite(payload, 1, size, file) == size;
        free(payload);
        if (!ok) {
            fprintf(stderr, "Failed to archive streamed sound %u (%s)\n", i,
                sound->name != NULL ? sound->name : "<unnamed>");
            break;
        }
        entries[i] = (SbkEntry){.offset = cursor,
            .size = size,
            .sampleRate = info.sampleRate,
            .sampleCount = info.sampleCount,
            .flags = info.channelCount | codec};
        uint8_t* row =
            header + CINNAMON_STREAMED_BANK_HEADER_SIZE + i * CINNAMON_STREAMED_BANK_ENTRY_SIZE;
        writeLe32(row + 0u, cursor);
        writeLe32(row + 4u, size);
        writeLe32(row + 8u, info.sampleRate);
        writeLe32(row + 12u, info.sampleCount);
        writeLe32(row + 16u, entries[i].flags);
        cursor += size;
        ++streamedCount;
        fprintf(stderr, "Archived streamed sound %u (%s): %.2f seconds\n", i,
            sound->name != NULL ? sound->name : "<unnamed>",
            (double) info.sampleCount / info.sampleRate);
    }
    writeLe32(header + 0u, CINNAMON_STREAMED_BANK_MAGIC);
    writeLe32(header + 4u, CINNAMON_STREAMED_BANK_VERSION);
    writeLe32(header + 8u, count);
    writeLe32(header + 12u, headerSize);
    if (ok) ok = fseek(file, 0, SEEK_SET) == 0 && fwrite(header, 1, headerSize, file) == headerSize;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (ok) ok = fsCommitTemp(tempPath, path);
    if (!ok) {
        fprintf(stderr, "Failed to write streamed audio bank: %s\n", path);
        remove(tempPath);
        free(entries);
    } else {
        *outEntries = entries;
        fprintf(stderr, "Streamed audio bank: %u tracks, %u bytes in %s\n", streamedCount, cursor,
            path);
    }
    audioFreeGroupCache(groupCache);
    free(header);
    return ok;
}
