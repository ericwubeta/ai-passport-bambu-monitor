#pragma once
#include <stddef.h>
#include <string.h>
#include <time.h>
static inline size_t host_strlcpy(char *out, const char *in, size_t size)
{
    size_t len = strlen(in);
    if (size) {
        size_t count = len < size - 1 ? len : size - 1;
        memcpy(out, in, count);
        out[count] = 0;
    }
    return len;
}
#define strlcpy host_strlcpy
#ifdef _WIN32
static inline struct tm *host_localtime_r(const time_t *value, struct tm *out)
{
    return localtime_s(out, value) == 0 ? out : NULL;
}
#define localtime_r host_localtime_r
#endif
