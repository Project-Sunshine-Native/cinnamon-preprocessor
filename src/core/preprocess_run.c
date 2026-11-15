#include "preprocess_run.h"
#include "audio/audio_convert.h"
#include "preprocess_context.h"
#include "target/preprocess_target.h"
#include "common/fs_utils.h"
#include "common/path_utils.h"
#include "core/preprocess_config.h"
#include "target/companion_files.h"
#include "target/exe_icon.h"

#include <stdlib.h>
#include <string.h>

static bool runSingle(
    PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles) {
    if (options == NULL) return false;

    progressPhase(PREPROCESS_PHASE_PARSE_DATA);
    DataWin* dataWin = DataWin_parse(options->inputPath,
        (DataWinParserOptions){
            .loadType = DATAWINLOADTYPE_STREAM_FILE,
            .parseGen8 = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parseFont = true,
            .parseRoom = true,
            .parseTgin = true,
            .parseTpag = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .lazyLoadRooms = true,
            .lazyLoadTextures = true,
            .lazyLoadAudio = true,
            .skipLoadingSpriteMasks = true,
        });
    if (dataWin == NULL) {
        fprintf(stderr, "Failed to parse %s\n", options->inputPath);
        return false;
    }

    const PreprocessGameProfile* gameProfile = detectProfile(gameProfiles, dataWin);
    applyProfile(options, gameProfile);
    applyTargetRules(options);
    fprintf(stderr,
        "cinnamon-preprocessor: detected %s (GEN8 name=%s, displayName=%s, bytecode=%u)\n",
        profileLabel(gameProfile), dataWin->gen8.name != NULL ? dataWin->gen8.name : "<unnamed>",
        dataWin->gen8.displayName != NULL ? dataWin->gen8.displayName : "<unnamed>",
        dataWin->gen8.wadVersion);

    progressPhase(PREPROCESS_PHASE_COMPANIONS);
    bool ok = copyCompanions(options, gameProfile);
    if (ok) ok = extractGameIcon(options, dataWin);
    progressPhase(PREPROCESS_PHASE_TEXTURES);
    if (ok) ok = texConvert(options, dataWin);
    progressPhase(PREPROCESS_PHASE_AUDIO);
    if (ok) ok = audioConvert(options, dataWin);

    progressPhase(PREPROCESS_PHASE_FREE_DATA);
    DataWin_free(dataWin);

    if (ok)
        fprintf(stderr, "Wrote %s assets to %s/gfx and %s/audio\n",
            targetName(options->user.target), options->outputDir, options->outputDir);
    return ok;
}

static bool tryChapterBatch(PreprocessRunConfig* options,
    const PreprocessGameProfileConfig* gameProfiles, bool* outHandled) {
    if (outHandled != NULL) *outHandled = false;
    if (options == NULL || gameProfiles == NULL || options->user.chapterOverride > 0) return false;

    char* installRoot = resolveInstallRoot(gameProfiles, options->inputPath);
    if (installRoot == NULL) return false;

    const PreprocessGameProfile* batchProfile = NULL;
    char chapterDataWin[PREPROCESS_PATH_MAX];
    int32_t foundChapterCount = 0;
    for (size_t i = 0; i < gameProfiles->profileCount; ++i) {
        const PreprocessGameProfile* profile = &gameProfiles->profiles[i];
        if (!profileHasChapters(profile)) continue;
        int32_t countForProfile = 0;
        for (int32_t chapter = profile->chapterMin; chapter <= profile->chapterMax; ++chapter) {
            if (resolveChapterData(
                    profile, installRoot, chapter, chapterDataWin, sizeof(chapterDataWin))) {
                ++countForProfile;
            }
        }
        if (countForProfile > 0) {
            batchProfile = profile;
            foundChapterCount = countForProfile;
            break;
        }
    }

    if (batchProfile == NULL) {
        free(installRoot);
        return false;
    }

    char* rootDataWin = pathJoinAlloc(installRoot, "data.win");
    const bool hasRootDataWin = rootDataWin != NULL && fsFileExists(rootDataWin);
    if (!hasRootDataWin && foundChapterCount <= 1) {
        free(rootDataWin);
        free(installRoot);
        return false;
    }

    if (outHandled != NULL) *outHandled = true;
    fprintf(stderr, "cinnamon-preprocessor: preprocessing %s root plus %ld chapter folder(s)\n",
        profileLabel(batchProfile), (long) foundChapterCount);

    bool ok = true;
    uint32_t gameIndex = 0u;
    uint32_t gameCount = (uint32_t) foundChapterCount + (hasRootDataWin ? 1u : 0u);
    if (hasRootDataWin) {
        PreprocessRunConfig rootOptions = *options;
        if (!pathCopy(rootOptions.inputPath, sizeof(rootOptions.inputPath), rootDataWin)) {
            ok = false;
        }

        if (ok) {
            fprintf(stderr, "cinnamon-preprocessor: preprocessing root %s -> %s\n",
                rootOptions.inputPath, rootOptions.outputDir);
            progressGame(gameIndex++, gameCount, "game root");
            ok = runSingle(&rootOptions, gameProfiles);
        }
    }

    for (int32_t chapter = batchProfile->chapterMin; ok && chapter <= batchProfile->chapterMax;
         ++chapter) {
        if (!resolveChapterData(
                batchProfile, installRoot, chapter, chapterDataWin, sizeof(chapterDataWin)))
            continue;

        char* chapterDir = pathDupParent(chapterDataWin);
        if (chapterDir == NULL) {
            free(chapterDir);
            ok = false;
            break;
        }
        const char* chapterFolderName = pathBaseName(chapterDir);
        if (chapterFolderName == NULL || chapterFolderName[0] == '\0') {
            free(chapterDir);
            ok = false;
            break;
        }

        PreprocessRunConfig chapterOptions = *options;
        if (!pathCopy(chapterOptions.inputPath, sizeof(chapterOptions.inputPath), chapterDataWin)) {
            free(chapterDir);
            ok = false;
            break;
        }

        if (!pathCopy(chapterOptions.sharedOutputDir, sizeof(chapterOptions.sharedOutputDir),
                options->outputDir) ||
            !pathJoin(options->outputDir, chapterFolderName, chapterOptions.outputDir,
                sizeof(chapterOptions.outputDir))) {
            free(chapterDir);
            ok = false;
            break;
        }

        if (!ensureOutputDirs(chapterOptions.outputDir)) {
            free(chapterDir);
            ok = false;
            break;
        }

        fprintf(stderr, "cinnamon-preprocessor: preprocessing chapter %ld %s -> %s\n",
            (long) chapter, chapterOptions.inputPath, chapterOptions.outputDir);
        progressGame(gameIndex++, gameCount, chapterFolderName);
        ok = runSingle(&chapterOptions, gameProfiles);
        free(chapterDir);
    }

    free(rootDataWin);
    free(installRoot);
    return ok;
}

bool runGame(PreprocessRunConfig* options, const PreprocessGameProfileConfig* gameProfiles) {
    bool batchHandled = false;
    bool ok = tryChapterBatch(options, gameProfiles, &batchHandled);
    if (batchHandled) return ok;
    progressGame(0u, 1u, NULL);
    return runSingle(options, gameProfiles);
}
