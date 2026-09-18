#ifndef JRUN_COMMON_H_
#define JRUN_COMMON_H_

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define JRUN_VERSION "0.1.0"
#define JRUN_PROGRAM_NAME "jrun"

typedef enum {
    LOG_LEVEL_QUIET = 0,
    LOG_LEVEL_NORMAL = 1,
    LOG_LEVEL_DEBUG = 2
} Log_Level;

extern Log_Level g_log_level;

#define jrun_log_info(...) \
    do { \
        if (g_log_level >= LOG_LEVEL_NORMAL) { \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

#define jrun_log_error(...) \
    do { \
        fprintf(stderr, "jrun: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } while (0)

#define jrun_log_debug(...) \
    do { \
        if (g_log_level >= LOG_LEVEL_DEBUG) { \
            fprintf(stderr, "[debug] "); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

static inline char *jrun_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

#define JRUN_DA_GROW(arr, count, cap, initial_cap, ok) \
    do { \
        (ok) = true; \
        if ((count) >= (cap)) { \
            size_t _new_cap = ((cap) == 0) ? (initial_cap) : ((cap) * 2); \
            void *_new_arr = realloc((arr), _new_cap * sizeof(*(arr))); \
            if (!_new_arr) { \
                (ok) = false; \
            } else { \
                (arr) = _new_arr; \
                (cap) = _new_cap; \
            } \
        } \
    } while (0)

#endif // JRUN_COMMON_H_
