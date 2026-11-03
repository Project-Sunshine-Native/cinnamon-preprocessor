#include "audio_convert_internal.h"

bool audioConvert(PreprocessRunConfig* options, DataWin* dataWin) {
    fprintf(stderr, "cinnamon-preprocessor: processing audio\n");
    if (options->existingAudioAction == CINNAMON_AUDIO_EXISTING_ACTION_UNSET)
        options->existingAudioAction = CINNAMON_AUDIO_EXISTING_ACTION_SKIP;

    SbkEntry* streamedEntries = NULL;
    bool ok = audioBuildStreamBank(options, dataWin, &streamedEntries);
    if (ok) ok = audioBuildBank(options, dataWin, streamedEntries);
    free(streamedEntries);
    if (ok)
        fprintf(stderr, "Packed SOND audio into %s ADPCM sound_bank.bin at %u Hz\n",
            targetInfo(options->user.target)->audioBankLabel, CINNAMON_AUDIO_SAMPLE_RATE);
    audioClearIndex();
    return ok;
}
