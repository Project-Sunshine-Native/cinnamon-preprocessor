#include "log.h"

void platformLog(const logType type, const char* format, va_list va) {
    FILE* out = type == LOG_TYPE_NORMAL ? stdout : stderr;
    vfprintf(out, format, va);
}
