#ifndef CINNAMON_PREPROCESSOR_STRING_UTILS_H
#define CINNAMON_PREPROCESSOR_STRING_UTILS_H

#include <stdbool.h>
bool strEqualNoCase(const char* first, const char* second);
bool strEndsWithNoCase(const char* value, const char* suffix);
bool strContainsNoCase(const char* value, const char* fragment);

#endif
