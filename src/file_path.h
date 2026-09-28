
#ifndef UXN_FILE_PATH_H
#define UXN_FILE_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline bool uxn_resolve_file_path(const char *source, const char *root,
                                         char *dest, size_t capacity)
{
    if (!source || !root || !dest || !capacity) return false;
    int n = snprintf(dest, capacity, "0:%s", root);
    if (n < 0 || (size_t)n >= capacity) return false;
    size_t used = (size_t)n;
    const char *p = source;
    while (*p) {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') {
            if ((unsigned char)*p < 0x20 || *p == ':' || *p == '\\') return false;
            ++p;
        }
        size_t len = (size_t)(p - start);
        if (!len || (len == 1 && start[0] == '.')) continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') return false;
        if (used + 1 + len >= capacity) return false;
        dest[used++] = '/';
        memcpy(dest + used, start, len);
        used += len;
        dest[used] = 0;
    }

    size_t len = strlen(source);
    if (len && source[len - 1] == '/' && used && dest[used - 1] != '/') {
        if (used + 1 >= capacity) return false;
        dest[used++] = '/';
        dest[used] = 0;
    }
    return true;
}
#endif
