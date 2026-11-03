#ifndef PREPROCESS_GAME_PROFILES_H
#define PREPROCESS_GAME_PROFILES_H

#include "common/preprocess_common.h"

#include "data_win.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PREPROCESS_GAME_PROFILE_MAX_PROFILES 32u
#define PREPROCESS_GAME_PROFILE_MAX_CHAPTER_MARKERS 4u
#define PREPROCESS_GAME_PROFILE_MAX_COMPANION_COPIES 16u

typedef struct PreprocessGameProfileCompanionCopy {
    char source[256];
    char destination[256];
} PreprocessGameProfileCompanionCopy;

typedef struct PreprocessGameProfile {
    char id[64];
    char label[128];
    char matchName[128];
    int32_t minBytecode;
    int32_t maxBytecode;
    char chapterDataWinPattern[256];
    int32_t chapterMin;
    int32_t chapterMax;
    char chapterDirContains[PREPROCESS_GAME_PROFILE_MAX_CHAPTER_MARKERS][64];
    size_t chapterDirContainsCount;
    PreprocessGameProfileCompanionCopy
        companionCopies[PREPROCESS_GAME_PROFILE_MAX_COMPANION_COPIES];
    size_t companionCopyCount;
    bool chapterParentAudioRoot;
    int32_t atlasPageMode;
} PreprocessGameProfile;

typedef struct PreprocessGameProfileConfig {
    PreprocessGameProfile profiles[PREPROCESS_GAME_PROFILE_MAX_PROFILES];
    size_t profileCount;
    char loadedPath[PREPROCESS_PATH_MAX];
} PreprocessGameProfileConfig;

void initProfiles(PreprocessGameProfileConfig* config);
bool loadProfiles(const char* argv0, PreprocessGameProfileConfig* config);
const PreprocessGameProfile* detectProfile(
    const PreprocessGameProfileConfig* config, const DataWin* dataWin);
const char* profileLabel(const PreprocessGameProfile* profile);
bool profileHasChapters(const PreprocessGameProfile* profile);
bool resolveChapterData(const PreprocessGameProfile* profile, const char* rootDir, int32_t chapter,
    char* dataWinPath, size_t dataWinPathSize);
bool isChapterDir(const PreprocessGameProfile* profile, const char* path);
const PreprocessGameProfile* findChapterProfile(
    const PreprocessGameProfileConfig* config, const char* path);
char* resolveInstallRoot(const PreprocessGameProfileConfig* config, const char* inputPath);

#endif
