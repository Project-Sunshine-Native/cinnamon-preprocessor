#include "string_utils.h"

#include <string.h>

static char asciiLower(char value) {
    return value >= 'A' && value <= 'Z' ? (char) (value - 'A' + 'a') : value;
}

bool strEqualNoCase(const char* first, const char* second) {
    if (first == NULL || second == NULL) return false;
    while (*first != '\0' && *second != '\0') {
        if (asciiLower(*first++) != asciiLower(*second++)) return false;
    }
    return *first == '\0' && *second == '\0';
}

bool strEndsWithNoCase(const char* value, const char* suffix) {
    if (value == NULL || suffix == NULL || suffix[0] == '\0') return false;
    size_t valueLength = strlen(value);
    size_t suffixLength = strlen(suffix);
    if (valueLength < suffixLength) return false;
    return strEqualNoCase(value + valueLength - suffixLength, suffix);
}

bool strContainsNoCase(const char* value, const char* fragment) {
    if (value == NULL || fragment == NULL || fragment[0] == '\0') return false;
    size_t valueLength = strlen(value);
    size_t fragmentLength = strlen(fragment);
    if (fragmentLength > valueLength) return false;
    for (size_t i = 0u; i + fragmentLength <= valueLength; ++i) {
        size_t j = 0u;
        while (j < fragmentLength && asciiLower(value[i + j]) == asciiLower(fragment[j])) ++j;
        if (j == fragmentLength) return true;
    }
    return false;
}
