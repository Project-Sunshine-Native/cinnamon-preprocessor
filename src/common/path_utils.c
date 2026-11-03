#include "preprocess_common.h"
#include "common/path_utils.h"
#include "common/string_utils.h"

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool pathIsSeparator(char value) {
    return value == '/' || value == '\\';
}

const char* pathBaseName(const char* path) {
    if (path == NULL) return NULL;
    const char* base = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    return base != NULL ? base + 1 : path;
}

static void copyComparablePath(const char* path, char* out, size_t outSize) {
    if (out == NULL || outSize == 0u) return;
    out[0] = '\0';
    if (path == NULL) return;
    size_t writeIndex = 0u;
    for (size_t i = 0u; path[i] != '\0' && writeIndex + 1u < outSize; ++i) {
        char c = path[i] == '\\' ? '/' : path[i];
        out[writeIndex++] = c;
    }
    while (writeIndex > 0u && out[writeIndex - 1u] == '/') writeIndex--;
#if PREPROCESS_HOST_WINDOWS
    for (size_t i = 0u; i < writeIndex; ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char) (out[i] - 'A' + 'a');
    }
#endif
    out[writeIndex] = '\0';
}

bool pathsEqual(const char* first, const char* second) {
    char normalizedA[PREPROCESS_SEARCH_PATH_MAX];
    char normalizedB[PREPROCESS_SEARCH_PATH_MAX];
    copyComparablePath(first, normalizedA, sizeof(normalizedA));
    copyComparablePath(second, normalizedB, sizeof(normalizedB));
    return normalizedA[0] != '\0' && strcmp(normalizedA, normalizedB) == 0;
}

static bool pathComponentEquals(const char* start, size_t length, const char* expected) {
    if (expected == NULL || strlen(expected) != length) return false;
    for (size_t i = 0u; i < length; ++i) {
        char actualChar = start[i];
        char expectedChar = expected[i];
        if (actualChar >= 'A' && actualChar <= 'Z') actualChar = (char) (actualChar - 'A' + 'a');
        if (expectedChar >= 'A' && expectedChar <= 'Z')
            expectedChar = (char) (expectedChar - 'A' + 'a');
        if (actualChar != expectedChar) return false;
    }
    return true;
}

bool pathHasComponentsNoCase(
    const char* path, const char* const* components, size_t componentCount) {
    if (path == NULL || components == NULL || componentCount == 0u) return false;
    for (size_t i = 0u; i < componentCount; ++i) {
        if (components[i] == NULL || components[i][0] == '\0' ||
            strpbrk(components[i], "/\\") != NULL) {
            return false;
        }
    }

    const char* candidate = path;
    while (*candidate != '\0') {
        while (pathIsSeparator(*candidate)) ++candidate;
        if (*candidate == '\0') break;

        const char* current = candidate;
        size_t componentIndex = 0u;
        while (componentIndex < componentCount) {
            const char* end = current;
            while (*end != '\0' && !pathIsSeparator(*end)) ++end;
            if (!pathComponentEquals(
                    current, (size_t) (end - current), components[componentIndex])) {
                break;
            }
            if (++componentIndex == componentCount) return true;
            current = end;
            while (pathIsSeparator(*current)) ++current;
            if (*current == '\0') break;
        }

        while (*candidate != '\0' && !pathIsSeparator(*candidate)) ++candidate;
    }
    return false;
}

static void trimInPlace(char* value, const char* whitespace) {
    if (value == NULL) return;
    size_t start = strspn(value, whitespace);
    size_t len = strlen(value);
    while (len > start && strchr(whitespace, value[len - 1u]) != NULL) len--;
    memmove(value, value + start, len - start);
    value[len - start] = '\0';
}

void pathTrim(char* value) {
    trimInPlace(value, " \t\r\n\v\f");
}

void pathUnquote(char* value) {
    if (value == NULL) return;
    pathTrim(value);
    size_t len = strlen(value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

void pathSanitizeLabel(char* dst, size_t dstSize, const char* src) {
    if (dst == NULL || dstSize == 0) return;
    dst[0] = '\0';
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, dstSize, "unnamed");
        return;
    }

    size_t written = 0;
    for (size_t i = 0; src[i] != '\0' && written + 1 < dstSize; ++i) {
        char c = src[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            dst[written++] = c;
        } else if (written > 0 && dst[written - 1] != '_') {
            dst[written++] = '_';
        }
        if (written >= 40) break;
    }
    while (written > 0 && dst[written - 1] == '_') written--;
    if (written == 0)
        snprintf(dst, dstSize, "unnamed");
    else
        dst[written] = '\0';
}

void pathAddDebugName(char* dst, size_t dstSize, const char* name) {
    if (dst == NULL || dstSize == 0 || name == NULL || name[0] == '\0') return;
    if (strstr(dst, name) != NULL) return;
    size_t len = strnlen(dst, dstSize);
    if (len >= dstSize) {
        dst[dstSize - 1u] = '\0';
        return;
    }
    if (len > 0) {
        if (dstSize - len <= 1u) return;
        snprintf(dst + len, dstSize - len, ", %s", name);
    } else {
        snprintf(dst, dstSize, "%s", name);
    }
}

bool pathIsDriveRoot(const char* path) {
    return path != NULL &&
        ((strlen(path) == 2 && path[1] == ':') ||
            (strlen(path) == 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')));
}

bool pathCopy(char* destination, size_t destinationSize, const char* source) {
    if (destination == NULL || destinationSize == 0u || source == NULL) return false;
    size_t length = strlen(source);
    if (length >= destinationSize) return false;
    memmove(destination, source, length + 1u);
    return true;
}

char* pathDupParent(const char* path) {
    if (path == NULL || path[0] == '\0') return safeStrdup(".");
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) return safeStrdup(".");
    size_t length = (size_t) (slash - path);
    if (length == 0u || (length == 2u && path[1] == ':')) length++;
    char* result = safeMalloc(length + 1);
    memcpy(result, path, length);
    result[length] = '\0';
    return result;
}

static bool pathChildIsAbsolute(const char* child) {
    return pathIsSeparator(child[0]) ||
        (((child[0] >= 'A' && child[0] <= 'Z') || (child[0] >= 'a' && child[0] <= 'z')) &&
            child[1] == ':');
}

bool pathJoin(const char* parent, const char* child, char* outPath, size_t outPathSize) {
    if (outPath == NULL || outPathSize == 0u) return false;
    outPath[0] = '\0';
    if (parent == NULL || child == NULL || parent[0] == '\0' || child[0] == '\0' ||
        pathChildIsAbsolute(child)) {
        return false;
    }

    size_t parentLength = strlen(parent);
    size_t childLength = strlen(child);
    bool needsSeparator = !pathIsSeparator(parent[parentLength - 1u]);
    size_t required = 0u;
    if (!checkedAddSize(parentLength, childLength, &required) ||
        !checkedAddSize(required, needsSeparator ? 2u : 1u, &required) || required > outPathSize) {
        return false;
    }

    memcpy(outPath, parent, parentLength);
    size_t cursor = parentLength;
    if (needsSeparator) outPath[cursor++] = CINNAMON_PREPROCESSOR_PATH_SEP[0];
    for (size_t i = 0u; i < childLength; ++i) {
        outPath[cursor++] =
            pathIsSeparator(child[i]) ? CINNAMON_PREPROCESSOR_PATH_SEP[0] : child[i];
    }
    outPath[cursor] = '\0';
    return true;
}

char* pathJoinAlloc(const char* parent, const char* child) {
    if (parent == NULL || child == NULL) return NULL;
    size_t capacity = 0u;
    if (!checkedAddSize(strlen(parent), strlen(child), &capacity) ||
        !checkedAddSize(capacity, 2u, &capacity)) {
        return NULL;
    }
    char* result = safeMalloc(capacity);
    if (!pathJoin(parent, child, result, capacity)) {
        free(result);
        return NULL;
    }
    return result;
}

bool pathWithSuffix(const char* path, const char* suffix, char* outPath, size_t outPathSize) {
    if (outPath == NULL || outPathSize == 0u) return false;
    outPath[0] = '\0';
    if (path == NULL || suffix == NULL || path[0] == '\0') return false;

    size_t pathLength = strlen(path);
    size_t suffixLength = strlen(suffix);
    size_t required = 0u;
    if (!checkedAddSize(pathLength, suffixLength, &required) ||
        !checkedAddSize(required, 1u, &required) || required > outPathSize) {
        return false;
    }
    memcpy(outPath, path, pathLength);
    memcpy(outPath + pathLength, suffix, suffixLength + 1u);
    return true;
}
