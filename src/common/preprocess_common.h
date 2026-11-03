#ifndef CINNAMON_PREPROCESSOR_COMMON_H
#define CINNAMON_PREPROCESSOR_COMMON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define PREPROCESS_PATH_MAX 1024
#define PREPROCESS_TEMP_PATH_MAX (PREPROCESS_PATH_MAX + 36)
#define PREPROCESS_SEARCH_PATH_MAX (PREPROCESS_PATH_MAX + 176)
#define PREPROCESS_EXTENDED_PATH_MAX (PREPROCESS_PATH_MAX + 376)
#define PREPROCESS_JOINED_PATH_MAX (2 * PREPROCESS_PATH_MAX + 32)

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define PREPROCESS_HOST_WINDOWS 1
#define CINNAMON_PREPROCESSOR_PATH_SEP "\\"
#else
#define PREPROCESS_HOST_WINDOWS 0
#define CINNAMON_PREPROCESSOR_PATH_SEP "/"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CINNAMON_PREPROCESSOR_MAYBE_UNUSED __attribute__((unused))
#else
#define CINNAMON_PREPROCESSOR_MAYBE_UNUSED
#endif

#define preprocessRepeat(n, it) \
    for (size_t it = 0, it##_count = (size_t) (n); it < it##_count; ++it)

static inline bool strNotEmpty(const char* value) {
    return value != NULL && value[0] != '\0';
}

static inline bool checkedAddSize(size_t a, size_t b, size_t* out) {
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static inline bool checkedMulSize(size_t a, size_t b, size_t* out) {
    if (a != 0u && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

#endif
