/* Forced include for vendored LASRecomp backend files on mingw (zig cc):
 * POSIX bits mingw-w64 lacks, after LASRecomp runtime/platform/win_compat.h. */
#pragma once
#include <direct.h>
#include <string.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k); d[k] = 0; }
    return len;
}
