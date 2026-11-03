#include "audio_convert_internal.h"
#include "common/fs_utils.h"
#include "common/byteio.h"

static bool decodeDspAdpcmToPcm(const uint8_t* fileData, const WdspSound* wdsp, WavPcm16* out);

static bool checkedPcmSampleBytes(uint32_t frames, uint32_t channels, size_t* outBytes) {
    size_t samples = 0u;
    return channels > 0u && checkedMulSize((size_t) frames, (size_t) channels, &samples) &&
        checkedMulSize(samples, sizeof(int16_t), outBytes);
}

static int signExtend4(int nibble) {
    return (nibble & 0x8) ? (nibble - 16) : nibble;
}

static bool parseWavPcm16FromMemory(
    const uint8_t* blob, uint32_t blobSize, WavPcm16* out, bool decode) {
    if (blob == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if (blobSize < 44 || memcmp(blob, "RIFF", 4) != 0 || memcmp(blob + 8, "WAVE", 4) != 0)
        return false;

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* sampleData = NULL;
    uint32_t sampleDataSize = 0;
    size_t offset = 12;
    while (offset <= (size_t) blobSize - 8u) {
        const uint8_t* chunk = blob + offset;
        uint32_t chunkSize = readLe32(chunk + 4);
        size_t payloadOffset = offset + 8;
        if ((size_t) chunkSize > (size_t) blobSize - payloadOffset) return false;
        size_t payloadEnd = payloadOffset + chunkSize;
        if (payloadEnd > SIZE_MAX - (chunkSize & 1u)) return false;
        size_t nextOffset = payloadEnd + (chunkSize & 1u);
        if (nextOffset <= offset) return false;

        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            formatTag = readLe16(blob + payloadOffset + 0);
            channels = readLe16(blob + payloadOffset + 2);
            sampleRate = readLe32(blob + payloadOffset + 4);
            bitsPerSample = readLe16(blob + payloadOffset + 14);
        } else if (memcmp(chunk, "data", 4) == 0) {
            sampleData = blob + payloadOffset;
            sampleDataSize = chunkSize;
        }

        offset = nextOffset;
    }

    if (formatTag != 1 || channels == 0 || channels > 2 || sampleRate == 0 || bitsPerSample != 16 ||
        sampleData == NULL || sampleDataSize == 0) {
        return false;
    }

    if (sampleDataSize % (channels * (uint32_t) sizeof(int16_t)) != 0u) return false;
    size_t sampleCount = sampleDataSize / sizeof(int16_t);
    if (sampleCount / channels > UINT32_MAX) return false;
    if (decode) {
        out->interleavedPcm = safeMalloc(sampleDataSize);
        for (size_t i = 0; i < sampleCount; ++i)
            out->interleavedPcm[i] = (int16_t) readLe16(sampleData + i * 2u);
    }
    out->sampleRate = sampleRate;
    out->channelCount = channels;
    out->sampleCount = (uint32_t) (sampleCount / channels);
    return true;
}

/* Read duration without expanding an entire music track into PCM. */
bool audioProbe(const uint8_t* data, uint32_t size, WavPcm16* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (data == NULL || size < 4u) return false;
    if (memcmp(data, "RIFF", 4u) == 0) return parseWavPcm16FromMemory(data, size, out, false);
    if (memcmp(data, "WDSP", 4u) == 0) {
        WdspSound info;
        if (!Wdsp_parse(data, size, &info)) return false;
        out->sampleRate = info.sampleRate;
        out->sampleCount = info.sampleCount;
        out->channelCount = info.channelCount;
        return true;
    }
    if (memcmp(data, "OggS", 4u) != 0 || size > INT_MAX) return false;
    int error = 0;
    stb_vorbis* decoder = stb_vorbis_open_memory(data, (int) size, &error, NULL);
    if (decoder == NULL) return false;
    stb_vorbis_info info = stb_vorbis_get_info(decoder);
    out->sampleRate = info.sample_rate;
    out->sampleCount = stb_vorbis_stream_length_in_samples(decoder);
    out->channelCount = (uint32_t) info.channels;
    stb_vorbis_close(decoder);
    return out->sampleRate > 0u && out->sampleCount > 0u && out->channelCount > 0u &&
        out->channelCount <= CINNAMON_DSP_MAX_CHANNELS;
}

bool audioOpenVorbis(const char* path, VorbisStreamDecoder* out) {
    if (path == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    int error = 0;
    out->vorbis = stb_vorbis_open_filename(path, &error, NULL);
    if (out->vorbis == NULL) return false;

    stb_vorbis_info info = stb_vorbis_get_info(out->vorbis);
    unsigned int totalSamples = stb_vorbis_stream_length_in_samples(out->vorbis);
    if (info.sample_rate <= 0 || info.channels <= 0 || info.channels > 2 || totalSamples == 0 ||
        totalSamples > UINT32_MAX) {
        audioCloseVorbis(out);
        return false;
    }

    out->channelCount = (uint32_t) info.channels;
    out->sampleRate = (uint32_t) info.sample_rate;
    out->totalSampleCount = (uint32_t) totalSamples;
    return true;
}

int32_t audioReadVorbis(VorbisStreamDecoder* decoder, int16_t* outSamples, uint32_t maxFrames) {
    if (decoder == NULL || decoder->vorbis == NULL || outSamples == NULL || maxFrames == 0)
        return -1;
    int requestShorts = (int) (maxFrames * decoder->channelCount);
    int framesRead = stb_vorbis_get_samples_short_interleaved(
        decoder->vorbis, (int) decoder->channelCount, outSamples, requestShorts);
    return framesRead < 0 ? -1 : framesRead;
}

void audioCloseVorbis(VorbisStreamDecoder* decoder) {
    if (decoder == NULL) return;
    if (decoder->vorbis != NULL) {
        stb_vorbis_close(decoder->vorbis);
    }
    memset(decoder, 0, sizeof(*decoder));
}

static bool loadVorbisPcm16(const uint8_t* data, uint32_t dataSize, WavPcm16* out) {
    if (data == NULL || out == NULL || dataSize > INT_MAX) return false;
    memset(out, 0, sizeof(*out));

    int channels = 0;
    int sampleRate = 0;
    short* decoded = NULL;
    int sampleCount =
        stb_vorbis_decode_memory(data, (int) dataSize, &channels, &sampleRate, &decoded);
    if (sampleCount <= 0 || decoded == NULL || channels <= 0 || channels > 2 || sampleRate <= 0) {
        free(decoded);
        return false;
    }

    out->channelCount = (uint32_t) channels;
    out->sampleRate = (uint32_t) sampleRate;
    out->sampleCount = (uint32_t) sampleCount;
    out->interleavedPcm = (int16_t*) decoded;
    return true;
}

bool audioLoad(const char* path, WavPcm16* out) {
    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    blob = fsReadFile(path, &blobSize);
    bool ok = blob != NULL && audioDecode(blob, blobSize, out);
    free(blob);
    return ok;
}

bool audioDecode(const uint8_t* data, uint32_t dataSize, WavPcm16* out) {
    if (data == NULL || out == NULL || dataSize < 4) return false;
    if (dataSize >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) {
        return parseWavPcm16FromMemory(data, dataSize, out, true);
    }
    if (memcmp(data, "WDSP", 4) == 0) {
        WdspSound wdsp;
        return Wdsp_parse(data, dataSize, &wdsp) && decodeDspAdpcmToPcm(data, &wdsp, out);
    }
    return loadVorbisPcm16(data, dataSize, out);
}

bool audioDownmixMono(WavPcm16* wav) {
    if (wav == NULL || wav->interleavedPcm == NULL || wav->sampleCount == 0u ||
        wav->channelCount == 0u)
        return false;
    if (wav->channelCount == 1u) return true;
    if (wav->channelCount != 2u) return false;

    size_t monoBytes = 0u;
    if (!checkedPcmSampleBytes(wav->sampleCount, 1u, &monoBytes)) return false;
    int16_t* mono = safeMalloc(monoBytes);
    preprocessRepeat(wav->sampleCount, frameIndex) {
        int left = wav->interleavedPcm[(size_t) frameIndex * 2u + 0u];
        int right = wav->interleavedPcm[(size_t) frameIndex * 2u + 1u];
        mono[frameIndex] = (int16_t) ((left + right) / 2);
    }

    free(wav->interleavedPcm);
    wav->interleavedPcm = mono;
    wav->channelCount = 1u;
    return true;
}

bool audioResample(WavPcm16* wav, uint32_t targetRate) {
    if (wav == NULL || wav->interleavedPcm == NULL || wav->sampleCount == 0 ||
        wav->channelCount == 0 || targetRate == 0)
        return false;
    if (wav->sampleRate == 0) wav->sampleRate = targetRate;
    if (wav->sampleRate == targetRate) return true;

    uint64_t dstFrames64 =
        ((uint64_t) wav->sampleCount * targetRate + wav->sampleRate / 2u) / wav->sampleRate;
    if (dstFrames64 == 0 || dstFrames64 > UINT32_MAX) return false;

    uint32_t dstFrames = (uint32_t) dstFrames64;
    size_t dstBytes = 0u;
    if (!checkedPcmSampleBytes(dstFrames, wav->channelCount, &dstBytes)) return false;
    int16_t* dst = safeMalloc(dstBytes);

    preprocessRepeat(dstFrames, dstFrame) {
        uint64_t srcPos = (uint64_t) dstFrame * wav->sampleRate;
        uint32_t srcFrame = (uint32_t) (srcPos / targetRate);
        uint32_t frac = (uint32_t) (srcPos % targetRate);
        if (srcFrame >= wav->sampleCount) srcFrame = wav->sampleCount - 1u;
        uint32_t nextFrame = srcFrame + 1u < wav->sampleCount ? srcFrame + 1u : srcFrame;

        preprocessRepeat(wav->channelCount, channel) {
            int a = wav->interleavedPcm[(size_t) srcFrame * wav->channelCount + channel];
            int b = wav->interleavedPcm[(size_t) nextFrame * wav->channelCount + channel];
            int value = a + (int) (((int64_t) (b - a) * frac + targetRate / 2u) / targetRate);
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
            dst[(size_t) dstFrame * wav->channelCount + channel] = (int16_t) value;
        }
    }

    free(wav->interleavedPcm);
    wav->interleavedPcm = dst;
    wav->sampleCount = dstFrames;
    wav->sampleRate = targetRate;
    return true;
}

void audioFree(WavPcm16* wav) {
    free(wav->interleavedPcm);
    memset(wav, 0, sizeof(*wav));
}

static bool decodeDspAdpcmFrame(const WdspChannel* channel, const uint8_t* frame, int16_t* hist1,
    int16_t* hist2, int16_t outSamples[CINNAMON_DSP_SAMPLES_PER_FRAME]) {
    int predictor = frame[0] >> 4;
    if (predictor >= (int) CINNAMON_DSP_PREDICTOR_COUNT) return false;
    uint8_t shift = frame[0] & 0x0F;
    int coef1 = (int16_t) channel->coefs[predictor * 2 + 0];
    int coef2 = (int16_t) channel->coefs[predictor * 2 + 1];

    int sampleIndex = 0;
    for (uint32_t byteIndex = 1u; byteIndex < CINNAMON_DSP_BYTES_PER_FRAME; ++byteIndex) {
        int hi = signExtend4(frame[byteIndex] >> 4);
        int lo = signExtend4(frame[byteIndex] & 0x0F);
        int nibbles[2] = {hi, lo};
        preprocessRepeat(2, nibbleIndex) {
            int64_t sample = audioScaleNibble(nibbles[nibbleIndex], shift);
            sample += CINNAMON_DSP_ROUNDING_BIAS + (int64_t) coef1 * (*hist1) +
                (int64_t) coef2 * (*hist2);
            sample >>= CINNAMON_DSP_FRACTIONAL_BITS;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            outSamples[sampleIndex++] = (int16_t) sample;
            *hist2 = *hist1;
            *hist1 = (int16_t) sample;
        }
    }
    return true;
}

static bool decodeDspAdpcmToPcm(const uint8_t* fileData, const WdspSound* wdsp, WavPcm16* out) {
    if (fileData == NULL || wdsp == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    size_t totalBytes = 0u;
    if (!checkedPcmSampleBytes(wdsp->sampleCount, wdsp->channelCount, &totalBytes)) return false;
    int16_t* pcm = safeMalloc(totalBytes);

    int16_t frameSamples[CINNAMON_DSP_MAX_CHANNELS][CINNAMON_DSP_SAMPLES_PER_FRAME];
    int16_t hist1[CINNAMON_DSP_MAX_CHANNELS] = {
        wdsp->channels[0].startHistory1,
        wdsp->channels[1].startHistory1,
    };
    int16_t hist2[CINNAMON_DSP_MAX_CHANNELS] = {
        wdsp->channels[0].startHistory2,
        wdsp->channels[1].startHistory2,
    };

    uint32_t frameCount = wdsp->sampleCount / CINNAMON_DSP_SAMPLES_PER_FRAME;
    if (wdsp->sampleCount % CINNAMON_DSP_SAMPLES_PER_FRAME != 0u) frameCount++;
    uint32_t sampleCursor = 0;
    preprocessRepeat(frameCount, frameIndex) {
        preprocessRepeat(wdsp->channelCount, channelIndex) {
            const uint8_t* frame = fileData + wdsp->channels[channelIndex].offset +
                frameIndex * CINNAMON_DSP_BYTES_PER_FRAME;
            if (!decodeDspAdpcmFrame(&wdsp->channels[channelIndex], frame, &hist1[channelIndex],
                    &hist2[channelIndex], frameSamples[channelIndex])) {
                free(pcm);
                return false;
            }
        }

        uint32_t samplesThisFrame = wdsp->sampleCount - sampleCursor;
        if (samplesThisFrame > CINNAMON_DSP_SAMPLES_PER_FRAME)
            samplesThisFrame = CINNAMON_DSP_SAMPLES_PER_FRAME;
        preprocessRepeat(samplesThisFrame, sampleIndex) {
            if (wdsp->channelCount == 1) {
                pcm[sampleCursor + sampleIndex] = frameSamples[0][sampleIndex];
            } else {
                size_t outIndex = ((size_t) sampleCursor + sampleIndex) * 2u;
                pcm[outIndex + 0] = frameSamples[0][sampleIndex];
                pcm[outIndex + 1] = frameSamples[1][sampleIndex];
            }
        }
        sampleCursor += samplesThisFrame;
    }

    out->sampleRate = wdsp->sampleRate;
    out->sampleCount = wdsp->sampleCount;
    out->channelCount = wdsp->channelCount;
    out->interleavedPcm = pcm;
    return true;
}
