#ifndef CINNAMON_PREPROCESSOR_AUDIO_CONVERT_H
#define CINNAMON_PREPROCESSOR_AUDIO_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include "data_win.h"
#include "core/preprocess_options.h"

#define CINNAMON_AUDIO_SAMPLE_RATE 32000u

typedef struct {
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t channelCount;
    int16_t* interleavedPcm;
} WavPcm16;

bool audioWriteDsp(const char* inputPath, const char* outputPath);
bool audioWriteDspStream(const char* path, const WavPcm16* wav);
bool audioLoad(const char* path, WavPcm16* out);
bool audioDecode(const uint8_t* data, uint32_t dataSize, WavPcm16* out);
bool audioResample(WavPcm16* wav, uint32_t targetRate);
void audioFree(WavPcm16* wav);
bool audioConvert(PreprocessRunConfig* options, DataWin* dataWin);

#endif
