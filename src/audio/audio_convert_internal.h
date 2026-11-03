#ifndef CINNAMON_PREPROCESSOR_AUDIO_CONVERT_INTERNAL_H
#define CINNAMON_PREPROCESSOR_AUDIO_CONVERT_INTERNAL_H

#include "common/preprocess_common.h"
#include "audio_convert.h"
#include "streamed_bank.h"

#include "data_win.h"
#include "audio/common/sound_bank.h"
#include "utils.h"
#include "core/game_profiles.h"
#include "core/preprocess_context.h"
#include "core/preprocess_options.h"
#include "target/preprocess_target.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb/ds/stb_ds.h>

#if PREPROCESS_HOST_WINDOWS
#include <windows.h>
#else
#include <dirent.h>
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define CINNAMON_AUDIO_CONVERT_OPTIMIZE_FAST __attribute__((optimize("O2")))
#else
#define CINNAMON_AUDIO_CONVERT_OPTIMIZE_FAST
#endif

#define STB_VORBIS_HEADER_ONLY
#include "stb/vorbis/stb_vorbis.c"

#define CINNAMON_VORBIS_STREAM_CHUNK_FRAMES 4096u
#define CINNAMON_VORBIS_STREAM_BUFFER_FRAMES (CINNAMON_VORBIS_STREAM_CHUNK_FRAMES * 2u + 2u)
#define CINNAMON_DSP_MAX_CHANNELS 2u
#define CINNAMON_DSP_PREDICTOR_COUNT 8u
#define CINNAMON_DSP_COEFFICIENT_COUNT (CINNAMON_DSP_PREDICTOR_COUNT * 2u)
#define CINNAMON_DSP_SAMPLES_PER_FRAME 14u
#define CINNAMON_DSP_BYTES_PER_FRAME 8u
#define CINNAMON_DSP_MAX_SHIFT 13u
#define CINNAMON_DSP_FRACTIONAL_BITS 11u
#define CINNAMON_DSP_ROUNDING_BIAS (1 << (CINNAMON_DSP_FRACTIONAL_BITS - 1u))
#define CINNAMON_DSP_STREAM_WRITE_BUFFER_FRAMES 1024u
#define CINNAMON_DSP_STREAM_WRITE_BUFFER_BYTES \
    (CINNAMON_DSP_STREAM_WRITE_BUFFER_FRAMES * CINNAMON_DSP_BYTES_PER_FRAME)

static inline int audioScaleNibble(int nibble, uint8_t shift) {
    int magnitude = nibble < 0 ? -nibble : nibble;
    int scaled = magnitude << (shift + CINNAMON_DSP_FRACTIONAL_BITS);
    return nibble < 0 ? -scaled : scaled;
}

typedef struct {
    stb_vorbis* vorbis;
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t totalSampleCount;
} VorbisStreamDecoder;

#define CINNAMON_SOUND_BANK_MAGIC 0x314B4253u /* SBK1 */
#define CINNAMON_SOUND_BANK_VERSION 2u
#define CINNAMON_SOUND_BANK_ENTRY_CHANNEL_MASK 0x000000FFu
#define CINNAMON_SOUND_BANK_ENTRY_FLAG_LOOP 0x00000100u
#define CINNAMON_SOUND_BANK_ENTRY_FLAG_PCM16 0x00000200u
#define CINNAMON_SOUND_BANK_ENTRY_FLAG_DSP_ADPCM 0x00000800u
#define CINNAMON_SOUND_BANK_MAX_PACKED_SECONDS 15u

typedef struct {
    int32_t groupIndex;
    DataWin* dataWin;
    char path[PREPROCESS_TEMP_PATH_MAX];
    uint32_t usedBySoundCount;
    bool loggedSummary;
} ExternalAudioGroupCacheEntry;

bool audioCheckedAddU32(uint32_t a, uint32_t b, uint32_t* out);
bool audioRoundSamples(uint32_t sampleCount, uint32_t* outEncodedSampleCount);
bool audioBuildDsp(const WavPcm16* wav, uint8_t** outData, uint32_t* outSize);
bool audioOpenVorbis(const char* path, VorbisStreamDecoder* out);
int32_t audioReadVorbis(VorbisStreamDecoder* decoder, int16_t* outSamples, uint32_t maxFrames);
void audioCloseVorbis(VorbisStreamDecoder* decoder);
bool audioProbe(const uint8_t* data, uint32_t size, WavPcm16* out);
bool audioDownmixMono(WavPcm16* wav);
void audioClearIndex(void);
char* audioFindSource(const PreprocessRunConfig* options, const Sound* sound);
void audioFreeGroupCache(ExternalAudioGroupCacheEntry* cache);
uint8_t* audioReadSource(const PreprocessRunConfig* options, DataWin* mainDataWin,
    const Sound* sound, ExternalAudioGroupCacheEntry** inOutGroupCache, uint32_t* outSize);
bool audioBankIsComplete(
    const char* path, uint32_t expectedSoundCount, uint32_t requiredFormatFlag);
bool audioBuildStreamBank(PreprocessRunConfig* options, DataWin* dataWin, SbkEntry** outEntries);
bool audioBuildBank(
    PreprocessRunConfig* options, DataWin* dataWin, const SbkEntry* streamedEntries);

#endif
