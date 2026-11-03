#include "audio_convert_internal.h"
#include "common/byteio.h"

typedef struct {
    uint8_t predictorScale;
    int16_t yn1;
    int16_t yn2;
} EncodedContext;

typedef struct {
    uint8_t* data;
    uint32_t dataSize;
    uint16_t coefs[CINNAMON_DSP_COEFFICIENT_COUNT];
    EncodedContext startContext;
    EncodedContext loopContext;
} EncodedDspAdpcmChannel;

typedef struct {
    int16_t coef1;
    int16_t coef2;
} DspPredictorPair;

typedef struct {
    FILE* file;
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t emittedSampleCount;
    uint32_t encodedSampleCount;
    uint32_t frameFill;
    uint32_t frameIndex;
    uint32_t channelDataOffset[CINNAMON_DSP_MAX_CHANNELS];
    EncodedDspAdpcmChannel channels[CINNAMON_DSP_MAX_CHANNELS];
    uint32_t channelWriteOffset[CINNAMON_DSP_MAX_CHANNELS];
    uint32_t channelWriteBufferBytes[CINNAMON_DSP_MAX_CHANNELS];
    uint8_t channelWriteBuffer[CINNAMON_DSP_MAX_CHANNELS][CINNAMON_DSP_STREAM_WRITE_BUFFER_BYTES];
    int16_t hist1[CINNAMON_DSP_MAX_CHANNELS];
    int16_t hist2[CINNAMON_DSP_MAX_CHANNELS];
    int16_t frameSamples[CINNAMON_DSP_MAX_CHANNELS][CINNAMON_DSP_SAMPLES_PER_FRAME];
} StreamingDspAdpcmWriter;

static const DspPredictorPair CINNAMON_DSP_PREDICTORS[CINNAMON_DSP_PREDICTOR_COUNT] = {
    {0, 0},
    {2048, 0},
    {1024, 0},
    {3072, -1024},
    {4096, -2048},
    {3584, -1536},
    {1536, 512},
    {2560, -512},
};

static bool streamVorbisToDsp(const char* inputPath, const char* outputPath);

bool audioCheckedAddU32(uint32_t a, uint32_t b, uint32_t* out) {
    if (out == NULL || UINT32_MAX - a < b) return false;
    *out = a + b;
    return true;
}

bool audioRoundSamples(uint32_t sampleCount, uint32_t* outEncodedSampleCount) {
    if (outEncodedSampleCount == NULL ||
        sampleCount > UINT32_MAX - (CINNAMON_DSP_SAMPLES_PER_FRAME - 1u))
        return false;
    uint32_t encoded =
        ((sampleCount + CINNAMON_DSP_SAMPLES_PER_FRAME - 1u) / CINNAMON_DSP_SAMPLES_PER_FRAME) *
        CINNAMON_DSP_SAMPLES_PER_FRAME;
    if (encoded == 0u) return false;
    *outEncodedSampleCount = encoded;
    return true;
}

static int clampNibble(int value) {
    if (value < -8) return -8;
    if (value > 7) return 7;
    return value;
}

static int roundShiftSigned(int numerator, uint8_t shift) {
    if (shift == 0) return numerator;
    int bias = 1 << (shift - 1u);
    if (numerator >= 0) return (numerator + bias) >> shift;
    return -(((-numerator) + bias) >> shift);
}

static uint8_t estimateAdpcmShift(int maxAbsResidual) {
    uint32_t scale = 1u;
    uint32_t target = (uint32_t) (maxAbsResidual > 0 ? maxAbsResidual : 0);
    uint8_t shift = 0u;
    while (shift < CINNAMON_DSP_MAX_SHIFT && scale * 7u < target) {
        scale <<= 1u;
        shift++;
    }
    return shift;
}

static void initEncodedChannelInfo(EncodedDspAdpcmChannel* channel) {
    memset(channel, 0, sizeof(*channel));
    channel->startContext.predictorScale = 0;
    channel->startContext.yn1 = 0;
    channel->startContext.yn2 = 0;
    channel->loopContext = channel->startContext;

    preprocessRepeat(CINNAMON_DSP_PREDICTOR_COUNT, i) {
        channel->coefs[i * 2u + 0u] = (uint16_t) CINNAMON_DSP_PREDICTORS[i].coef1;
        channel->coefs[i * 2u + 1u] = (uint16_t) CINNAMON_DSP_PREDICTORS[i].coef2;
    }
}

bool audioWriteDsp(const char* inputPath, const char* outputPath) {
    if (streamVorbisToDsp(inputPath, outputPath)) return true;

    WavPcm16 wav;
    if (!audioLoad(inputPath, &wav)) {
        fprintf(stderr, "DSP ADPCM writer could not decode audio: %s\n", inputPath);
        return false;
    }

    if (!audioResample(&wav, CINNAMON_AUDIO_SAMPLE_RATE)) {
        audioFree(&wav);
        fprintf(stderr, "DSP ADPCM writer could not resample audio: %s\n", inputPath);
        return false;
    }

    bool ok = audioWriteDspStream(outputPath, &wav);
    audioFree(&wav);
    if (!ok) {
        fprintf(stderr, "DSP ADPCM writer failed to write %s\n", outputPath);
        return false;
    }
    return true;
}

static int16_t getEncodeSample(const int16_t* interleavedSamples, uint32_t sourceSampleCount,
    uint32_t channelCount, uint32_t channelIndex, uint32_t sampleIndex) {
    if (sampleIndex >= sourceSampleCount) return 0;
    return interleavedSamples[(size_t) sampleIndex * channelCount + channelIndex];
}

typedef struct {
    int16_t hist1;
    int16_t hist2;
} DspHistory;

typedef struct {
    uint8_t data[CINNAMON_DSP_BYTES_PER_FRAME];
    DspHistory history;
    uint64_t error;
} DspFrame;

static uint8_t CINNAMON_AUDIO_CONVERT_OPTIMIZE_FAST estimateShiftFromSamples(
    const int16_t samples[CINNAMON_DSP_SAMPLES_PER_FRAME], const DspPredictorPair* predictor,
    DspHistory history) {
    int16_t hist1 = history.hist1;
    int16_t hist2 = history.hist2;
    int maxAbsResidual = 0;

    preprocessRepeat(CINNAMON_DSP_SAMPLES_PER_FRAME, i) {
        int target = samples[i];
        int predicted =
            (CINNAMON_DSP_ROUNDING_BIAS + predictor->coef1 * hist1 + predictor->coef2 * hist2) >>
            CINNAMON_DSP_FRACTIONAL_BITS;
        int residual = target - predicted;
        int absResidual = abs(residual);
        if (absResidual > maxAbsResidual) maxAbsResidual = absResidual;
        hist2 = hist1;
        hist1 = (int16_t) target;
    }

    return estimateAdpcmShift(maxAbsResidual);
}

static void CINNAMON_AUDIO_CONVERT_OPTIMIZE_FAST encodeFrameSamples(
    const int16_t samples[CINNAMON_DSP_SAMPLES_PER_FRAME], const DspPredictorPair* predictor,
    uint8_t shift, DspHistory history, DspFrame* out) {
    int16_t hist1 = history.hist1;
    int16_t hist2 = history.hist2;
    uint8_t frame[CINNAMON_DSP_BYTES_PER_FRAME];
    frame[0] = (uint8_t) (((int) (predictor - CINNAMON_DSP_PREDICTORS) << 4) | (shift & 0x0F));
    uint64_t totalError = 0;

    preprocessRepeat(CINNAMON_DSP_SAMPLES_PER_FRAME, i) {
        int target = samples[i];
        int predicted =
            (CINNAMON_DSP_ROUNDING_BIAS + predictor->coef1 * hist1 + predictor->coef2 * hist2) >>
            CINNAMON_DSP_FRACTIONAL_BITS;
        int residual = target - predicted;
        int nibble = clampNibble(roundShiftSigned(residual, shift));
        int decoded = audioScaleNibble(nibble, shift) + CINNAMON_DSP_ROUNDING_BIAS +
            predictor->coef1 * hist1 + predictor->coef2 * hist2;
        decoded >>= CINNAMON_DSP_FRACTIONAL_BITS;
        int error = target - decoded;
        int64_t wideError = (int64_t) error;
        totalError += (uint64_t) (wideError * wideError);

        hist2 = hist1;
        hist1 = (int16_t) decoded;

        uint8_t encodedNibble = (uint8_t) (nibble < 0 ? nibble + 16 : nibble);
        uint32_t byteIndex = 1u + (uint32_t) i / 2u;
        if ((i & 1) == 0) {
            frame[byteIndex] = (uint8_t) (encodedNibble << 4);
        } else {
            frame[byteIndex] |= encodedNibble;
        }
    }

    memcpy(out->data, frame, sizeof(frame));
    out->history = (DspHistory){hist1, hist2};
    out->error = totalError;
}

static void encodeBestFrameSamples(const int16_t samples[CINNAMON_DSP_SAMPLES_PER_FRAME],
    DspHistory* history, uint8_t outFrame[CINNAMON_DSP_BYTES_PER_FRAME]) {
    DspFrame best = {.error = UINT64_MAX};
    preprocessRepeat(CINNAMON_DSP_PREDICTOR_COUNT, predictorIndex) {
        const DspPredictorPair* predictor = &CINNAMON_DSP_PREDICTORS[predictorIndex];
        uint8_t estimatedShift = estimateShiftFromSamples(samples, predictor, *history);
        uint8_t shiftStart = estimatedShift > 1u ? (uint8_t) (estimatedShift - 1u) : 0u;
        uint8_t shiftEnd = estimatedShift + 1u < CINNAMON_DSP_MAX_SHIFT
            ? (uint8_t) (estimatedShift + 1u)
            : CINNAMON_DSP_MAX_SHIFT;
        for (uint8_t shift = shiftStart; shift <= shiftEnd; ++shift) {
            DspFrame frame;
            encodeFrameSamples(samples, predictor, shift, *history, &frame);
            if (frame.error < best.error) best = frame;
        }
    }
    memcpy(outFrame, best.data, sizeof(best.data));
    *history = best.history;
}

static bool CINNAMON_AUDIO_CONVERT_OPTIMIZE_FAST encodeChannelDspAdpcm(
    const int16_t* interleavedSamples, uint32_t sourceSampleCount, uint32_t channelCount,
    uint32_t channelIndex, uint32_t encodedSampleCount, EncodedDspAdpcmChannel* out) {
    if (interleavedSamples == NULL || out == NULL || channelCount == 0u ||
        channelIndex >= channelCount)
        return false;
    initEncodedChannelInfo(out);
    uint32_t frameCount = encodedSampleCount / CINNAMON_DSP_SAMPLES_PER_FRAME;
    if (encodedSampleCount % CINNAMON_DSP_SAMPLES_PER_FRAME != 0u) frameCount++;
    if (frameCount > UINT32_MAX / CINNAMON_DSP_BYTES_PER_FRAME) return false;
    out->dataSize = frameCount * CINNAMON_DSP_BYTES_PER_FRAME;
    out->data = safeMalloc(out->dataSize);

    DspHistory history = {0};
    preprocessRepeat(frameCount, frameIndex) {
        int16_t samples[CINNAMON_DSP_SAMPLES_PER_FRAME];
        preprocessRepeat(CINNAMON_DSP_SAMPLES_PER_FRAME, i) {
            uint32_t sampleIndex =
                (uint32_t) frameIndex * CINNAMON_DSP_SAMPLES_PER_FRAME + (uint32_t) i;
            samples[i] = getEncodeSample(
                interleavedSamples, sourceSampleCount, channelCount, channelIndex, sampleIndex);
        }
        uint8_t* frame = out->data + frameIndex * CINNAMON_DSP_BYTES_PER_FRAME;
        encodeBestFrameSamples(samples, &history, frame);
        if (frameIndex == 0) {
            out->startContext.predictorScale = frame[0];
            out->loopContext = out->startContext;
        }
    }

    return true;
}

static void freeEncodedChannel(EncodedDspAdpcmChannel* channel) {
    free(channel->data);
    memset(channel, 0, sizeof(*channel));
}

typedef struct {
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t encodedSampleCount;
    const EncodedDspAdpcmChannel* channels;
    const uint32_t* channelOffsets;
} DspAdpcmHeaderInfo;

static void encodeDspAdpcmHeader(uint8_t header[WDSP_HEADER_SIZE], const DspAdpcmHeaderInfo* info) {
    memset(header, 0, WDSP_HEADER_SIZE);
    writeLe32(header + 0u, WDSP_MAGIC);
    writeLe32(header + 4u, WDSP_VERSION);
    writeLe32(header + 8u, info->channelCount);
    writeLe32(header + 12u, info->sampleRate);
    writeLe32(header + 16u, info->sampleCount);
    writeLe32(header + 20u, info->encodedSampleCount);
    writeLe32(header + 24u, WDSP_HEADER_SIZE);
    preprocessRepeat(info->channelCount, channelIndex) {
        uint8_t* row = header + 32u + channelIndex * WDSP_CHANNEL_ROW_SIZE;
        writeLe32(row + 0u, info->channelOffsets[channelIndex]);
        writeLe32(row + 4u, info->channels[channelIndex].dataSize);
        preprocessRepeat(CINNAMON_DSP_COEFFICIENT_COUNT, coefIndex)
            writeLe16(row + 8u + coefIndex * 2u, info->channels[channelIndex].coefs[coefIndex]);
        writeLe16(row + 40u, info->channels[channelIndex].startContext.predictorScale);
        writeLe16(row + 42u, (uint16_t) info->channels[channelIndex].startContext.yn1);
        writeLe16(row + 44u, (uint16_t) info->channels[channelIndex].startContext.yn2);
        writeLe16(row + 46u, info->channels[channelIndex].loopContext.predictorScale);
        writeLe16(row + 48u, (uint16_t) info->channels[channelIndex].loopContext.yn1);
        writeLe16(row + 50u, (uint16_t) info->channels[channelIndex].loopContext.yn2);
    }
}

static bool dspWriterFlushOutput(StreamingDspAdpcmWriter* writer) {
    if (writer == NULL || writer->file == NULL) return false;

    preprocessRepeat(writer->channelCount, channelIndex) {
        uint32_t bytes = writer->channelWriteBufferBytes[channelIndex];
        if (bytes == 0) continue;

        if (fseek(writer->file, (long) writer->channelWriteOffset[channelIndex], SEEK_SET) != 0 ||
            fwrite(writer->channelWriteBuffer[channelIndex], 1, bytes, writer->file) != bytes) {
            return false;
        }

        writer->channelWriteOffset[channelIndex] += bytes;
        writer->channelWriteBufferBytes[channelIndex] = 0;
    }

    return true;
}

static bool dspWriterFlushFrame(StreamingDspAdpcmWriter* writer) {
    if (writer == NULL || writer->file == NULL ||
        writer->frameFill != CINNAMON_DSP_SAMPLES_PER_FRAME)
        return false;

    preprocessRepeat(writer->channelCount, channelIndex) {
        if (writer->channelWriteBufferBytes[channelIndex] + CINNAMON_DSP_BYTES_PER_FRAME >
            CINNAMON_DSP_STREAM_WRITE_BUFFER_BYTES) {
            if (!dspWriterFlushOutput(writer)) return false;
        }

        uint8_t frame[CINNAMON_DSP_BYTES_PER_FRAME];
        DspHistory history = {writer->hist1[channelIndex], writer->hist2[channelIndex]};
        encodeBestFrameSamples(writer->frameSamples[channelIndex], &history, frame);

        if (writer->frameIndex == 0) {
            writer->channels[channelIndex].startContext.predictorScale = frame[0];
            writer->channels[channelIndex].loopContext =
                writer->channels[channelIndex].startContext;
        }

        memcpy(writer->channelWriteBuffer[channelIndex] +
                writer->channelWriteBufferBytes[channelIndex],
            frame, sizeof(frame));
        writer->channelWriteBufferBytes[channelIndex] += (uint32_t) sizeof(frame);
        writer->hist1[channelIndex] = history.hist1;
        writer->hist2[channelIndex] = history.hist2;
    }

    writer->frameFill = 0;
    writer->frameIndex++;
    return true;
}

static bool dspWriterEmit(
    StreamingDspAdpcmWriter* writer, const int16_t samples[CINNAMON_DSP_MAX_CHANNELS]) {
    if (writer == NULL || samples == NULL ||
        writer->emittedSampleCount >= writer->encodedSampleCount)
        return false;

    preprocessRepeat(writer->channelCount, channelIndex) {
        writer->frameSamples[channelIndex][writer->frameFill] = samples[channelIndex];
    }
    writer->frameFill++;
    writer->emittedSampleCount++;

    if (writer->frameFill == CINNAMON_DSP_SAMPLES_PER_FRAME) {
        return dspWriterFlushFrame(writer);
    }
    return true;
}

static bool dspWriterInit(StreamingDspAdpcmWriter* writer, const char* path, uint32_t channelCount,
    uint32_t sampleRate, uint32_t sampleCount) {
    if (writer == NULL || path == NULL || channelCount == 0 ||
        channelCount > CINNAMON_DSP_MAX_CHANNELS || sampleRate == 0 || sampleCount == 0)
        return false;
    memset(writer, 0, sizeof(*writer));

    writer->channelCount = channelCount;
    writer->sampleRate = sampleRate;
    writer->sampleCount = sampleCount;
    if (!audioRoundSamples(sampleCount, &writer->encodedSampleCount)) return false;

    uint32_t frameCount = writer->encodedSampleCount / CINNAMON_DSP_SAMPLES_PER_FRAME;
    if (frameCount > UINT32_MAX / CINNAMON_DSP_BYTES_PER_FRAME) return false;
    preprocessRepeat(channelCount, channelIndex) {
        initEncodedChannelInfo(&writer->channels[channelIndex]);
        writer->channels[channelIndex].dataSize = frameCount * CINNAMON_DSP_BYTES_PER_FRAME;
    }

    uint32_t dataOffset = WDSP_HEADER_SIZE;
    preprocessRepeat(channelCount, channelIndex) {
        writer->channelDataOffset[channelIndex] = dataOffset;
        writer->channelWriteOffset[channelIndex] = dataOffset;
        if (!audioCheckedAddU32(dataOffset, writer->channels[channelIndex].dataSize, &dataOffset))
            return false;
    }

    writer->file = fopen(path, "wb+");
    return writer->file != NULL;
}

static bool dspWriterFinish(StreamingDspAdpcmWriter* writer) {
    if (writer == NULL || writer->file == NULL) return false;

    int16_t silence[CINNAMON_DSP_MAX_CHANNELS] = {0};
    while (writer->emittedSampleCount < writer->encodedSampleCount) {
        if (!dspWriterEmit(writer, silence)) return false;
    }
    if (writer->frameFill != 0) return false;
    if (!dspWriterFlushOutput(writer)) return false;

    preprocessRepeat(writer->channelCount, channelIndex) {
        if (writer->channelWriteOffset[channelIndex] !=
            writer->channelDataOffset[channelIndex] + writer->channels[channelIndex].dataSize) {
            return false;
        }
    }

    const DspAdpcmHeaderInfo info = {
        .channelCount = writer->channelCount,
        .sampleRate = writer->sampleRate,
        .sampleCount = writer->sampleCount,
        .encodedSampleCount = writer->encodedSampleCount,
        .channels = writer->channels,
        .channelOffsets = writer->channelDataOffset,
    };
    uint8_t header[WDSP_HEADER_SIZE];
    encodeDspAdpcmHeader(header, &info);
    if (fseek(writer->file, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), writer->file) != sizeof(header))
        return false;

    if (fflush(writer->file) != 0) return false;
    return true;
}

static bool dspWriterClose(StreamingDspAdpcmWriter* writer) {
    if (writer == NULL || writer->file == NULL) return true;
    bool ok = fclose(writer->file) == 0;
    writer->file = NULL;
    return ok;
}

static bool ensureVorbisSourceFrame(VorbisStreamDecoder* decoder, int16_t* sourceBuffer,
    uint32_t* bufferStartFrame, uint32_t* bufferFrameCount, bool* sourceEof,
    uint32_t requiredFrame) {
    if (decoder == NULL || sourceBuffer == NULL || bufferStartFrame == NULL ||
        bufferFrameCount == NULL || sourceEof == NULL)
        return false;

    if (requiredFrame < *bufferStartFrame) return true;
    while (requiredFrame >= *bufferStartFrame + *bufferFrameCount) {
        if (*sourceEof) return false;
        if (*bufferFrameCount >= CINNAMON_VORBIS_STREAM_BUFFER_FRAMES) return false;

        uint32_t writableFrames = CINNAMON_VORBIS_STREAM_BUFFER_FRAMES - *bufferFrameCount;
        if (writableFrames > CINNAMON_VORBIS_STREAM_CHUNK_FRAMES)
            writableFrames = CINNAMON_VORBIS_STREAM_CHUNK_FRAMES;
        int32_t readFrames = audioReadVorbis(decoder,
            sourceBuffer + (size_t) *bufferFrameCount * decoder->channelCount, writableFrames);
        if (readFrames < 0) return false;
        if (readFrames == 0) {
            *sourceEof = true;
            return false;
        }
        *bufferFrameCount += (uint32_t) readFrames;
    }

    return true;
}

static void discardVorbisFrames(int16_t* sourceBuffer, uint32_t channelCount,
    uint32_t* bufferStartFrame, uint32_t* bufferFrameCount, uint32_t keepFromFrame) {
    if (sourceBuffer == NULL || bufferStartFrame == NULL || bufferFrameCount == NULL ||
        keepFromFrame <= *bufferStartFrame)
        return;

    uint32_t discardFrames = keepFromFrame - *bufferStartFrame;
    if (discardFrames > *bufferFrameCount) discardFrames = *bufferFrameCount;
    uint32_t remainingFrames = *bufferFrameCount - discardFrames;
    if (remainingFrames > 0) {
        memmove(sourceBuffer, sourceBuffer + (size_t) discardFrames * channelCount,
            (size_t) remainingFrames * channelCount * sizeof(int16_t));
    }
    *bufferStartFrame += discardFrames;
    *bufferFrameCount = remainingFrames;
}

static bool streamVorbisToDsp(const char* inputPath, const char* outputPath) {
    VorbisStreamDecoder decoder;
    if (!audioOpenVorbis(inputPath, &decoder)) return false;

    bool ok = false;
    StreamingDspAdpcmWriter writer;
    memset(&writer, 0, sizeof(writer));
    int16_t* sourceBuffer = NULL;

    uint64_t dstFrames64 = ((uint64_t) decoder.totalSampleCount * CINNAMON_AUDIO_SAMPLE_RATE +
                               decoder.sampleRate / 2u) /
        decoder.sampleRate;
    if (dstFrames64 == 0 || dstFrames64 > UINT32_MAX) goto cleanup;
    uint32_t dstFrames = (uint32_t) dstFrames64;

    if (!dspWriterInit(
            &writer, outputPath, decoder.channelCount, CINNAMON_AUDIO_SAMPLE_RATE, dstFrames))
        goto cleanup;

    sourceBuffer = safeMalloc(
        (size_t) CINNAMON_VORBIS_STREAM_BUFFER_FRAMES * decoder.channelCount * sizeof(int16_t));
    uint32_t bufferStartFrame = 0;
    uint32_t bufferFrameCount = 0;
    bool sourceEof = false;

    preprocessRepeat(dstFrames, dstFrameIndex) {
        uint64_t srcPos = (uint64_t) dstFrameIndex * decoder.sampleRate;
        uint32_t srcFrame = (uint32_t) (srcPos / CINNAMON_AUDIO_SAMPLE_RATE);
        uint32_t frac = (uint32_t) (srcPos % CINNAMON_AUDIO_SAMPLE_RATE);
        if (srcFrame >= decoder.totalSampleCount) srcFrame = decoder.totalSampleCount - 1u;
        uint32_t nextFrame = srcFrame + 1u < decoder.totalSampleCount ? srcFrame + 1u : srcFrame;

        discardVorbisFrames(
            sourceBuffer, decoder.channelCount, &bufferStartFrame, &bufferFrameCount, srcFrame);
        if (!ensureVorbisSourceFrame(&decoder, sourceBuffer, &bufferStartFrame, &bufferFrameCount,
                &sourceEof, nextFrame))
            goto cleanup;

        size_t srcOffset = (size_t) (srcFrame - bufferStartFrame) * decoder.channelCount;
        size_t nextOffset = (size_t) (nextFrame - bufferStartFrame) * decoder.channelCount;
        int16_t outputSamples[CINNAMON_DSP_MAX_CHANNELS] = {0};
        preprocessRepeat(decoder.channelCount, channelIndex) {
            int a = sourceBuffer[srcOffset + channelIndex];
            int b = sourceBuffer[nextOffset + channelIndex];
            int value = a +
                (int) (((int64_t) (b - a) * frac + CINNAMON_AUDIO_SAMPLE_RATE / 2u) /
                    CINNAMON_AUDIO_SAMPLE_RATE);
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
            outputSamples[channelIndex] = (int16_t) value;
        }

        if (!dspWriterEmit(&writer, outputSamples)) goto cleanup;
    }

    ok = dspWriterFinish(&writer);

cleanup:
    if (!dspWriterClose(&writer)) ok = false;
    audioCloseVorbis(&decoder);
    free(sourceBuffer);
    if (!ok && outputPath != NULL) remove(outputPath);
    return ok;
}

bool audioWriteDspStream(const char* path, const WavPcm16* wav) {
    if (path == NULL || wav == NULL || wav->interleavedPcm == NULL || wav->channelCount == 0 ||
        wav->channelCount > CINNAMON_DSP_MAX_CHANNELS || wav->sampleCount == 0)
        return false;

    bool ok = false;
    StreamingDspAdpcmWriter writer;
    memset(&writer, 0, sizeof(writer));
    uint32_t sampleRate = wav->sampleRate != 0 ? wav->sampleRate : CINNAMON_AUDIO_SAMPLE_RATE;
    if (!dspWriterInit(&writer, path, wav->channelCount, sampleRate, wav->sampleCount))
        goto cleanup;

    preprocessRepeat(wav->sampleCount, frameIndex) {
        int16_t outputSamples[CINNAMON_DSP_MAX_CHANNELS] = {0};
        preprocessRepeat(wav->channelCount, channelIndex) {
            outputSamples[channelIndex] =
                wav->interleavedPcm[(size_t) frameIndex * wav->channelCount + channelIndex];
        }
        if (!dspWriterEmit(&writer, outputSamples)) goto cleanup;
    }

    ok = dspWriterFinish(&writer);

cleanup:
    if (!dspWriterClose(&writer)) ok = false;
    if (!ok && path != NULL) remove(path);
    return ok;
}

bool audioBuildDsp(const WavPcm16* wav, uint8_t** outData, uint32_t* outSize) {
    if (outData != NULL) *outData = NULL;
    if (outSize != NULL) *outSize = 0u;
    if (wav == NULL || outData == NULL || outSize == NULL || wav->interleavedPcm == NULL ||
        wav->channelCount == 0u || wav->channelCount > CINNAMON_DSP_MAX_CHANNELS ||
        wav->sampleCount == 0u) {
        return false;
    }

    EncodedDspAdpcmChannel channels[CINNAMON_DSP_MAX_CHANNELS];
    memset(channels, 0, sizeof(channels));
    bool ok = false;
    uint32_t encodedSampleCount = 0u;
    if (!audioRoundSamples(wav->sampleCount, &encodedSampleCount)) return false;
    uint32_t channelOffsets[CINNAMON_DSP_MAX_CHANNELS] = {
        WDSP_HEADER_SIZE,
        WDSP_HEADER_SIZE,
    };

    preprocessRepeat(wav->channelCount, channelIndex) {
        if (!encodeChannelDspAdpcm(wav->interleavedPcm, wav->sampleCount, wav->channelCount,
                (uint32_t) channelIndex, encodedSampleCount, &channels[channelIndex])) {
            goto cleanup;
        }
    }

    uint32_t totalSize = WDSP_HEADER_SIZE;
    preprocessRepeat(wav->channelCount, channelIndex) {
        channelOffsets[channelIndex] = totalSize;
        if (UINT32_MAX - totalSize < channels[channelIndex].dataSize) goto cleanup;
        totalSize += channels[channelIndex].dataSize;
    }

    uint8_t* blob = safeCalloc(1, totalSize);
    const DspAdpcmHeaderInfo info = {
        .channelCount = wav->channelCount,
        .sampleRate = wav->sampleRate != 0u ? wav->sampleRate : CINNAMON_AUDIO_SAMPLE_RATE,
        .sampleCount = wav->sampleCount,
        .encodedSampleCount = encodedSampleCount,
        .channels = channels,
        .channelOffsets = channelOffsets,
    };
    encodeDspAdpcmHeader(blob, &info);
    preprocessRepeat(wav->channelCount, channelIndex) {
        memcpy(blob + channelOffsets[channelIndex], channels[channelIndex].data,
            channels[channelIndex].dataSize);
    }

    *outData = blob;
    *outSize = totalSize;
    ok = true;

cleanup:
    preprocessRepeat(CINNAMON_DSP_MAX_CHANNELS, i) freeEncodedChannel(&channels[i]);
    return ok;
}
