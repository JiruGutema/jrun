#include "scanner.h"
#include "common.h"
#include "path_util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <pthread.h>

typedef struct {
    dev_t dev;
    ino_t ino;
    bool used;
} Visited_Slot;

typedef struct {
    Visited_Slot *slots;
    size_t capacity;
    size_t count;
} Visited_Set;

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;

    Visited_Set visited;

    const Jrun_Config *config;
    int max_depth;
    bool follow_symlinks;
    bool oom;
} Scanner_Context;

typedef struct {
    Scanner_Context ctx;
    char root_path[PATH_MAX];
} Scan_Thread_Arg;

static bool should_ignore_dir_name(const Scanner_Context *ctx, const char *name)
{
    if (!name || name[0] == '\0') return true;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;

    // Hidden directories are skipped unless the user listed them as a root,
    // which is handled by the caller passing the root path in directly.
    // ".config" stays walkable because dotfile repos live under it.
    if (name[0] == '.' && strcmp(name, ".config") != 0) {
        return true;
    }

    return config_is_ignored(ctx->config, name);
}

static bool scanner_add_path(Scanner_Context *ctx, const char *path)
{
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? 64 : ctx->capacity * 2;
        char **new_paths = (char **)realloc(ctx->paths, new_cap * sizeof(char *));
        if (!new_paths) {
            ctx->oom = true;
            return false;
        }
        ctx->paths = new_paths;
        ctx->capacity = new_cap;
    }

    char *copy = jrun_strdup(path);
    if (!copy) {
        ctx->oom = true;
        return false;
    }
    ctx->paths[ctx->count++] = copy;
    return true;
}

static inline uint64_t hash_dev_ino(dev_t dev, ino_t ino)
{
    uint64_t h = ((uint64_t)dev << 32) ^ (uint64_t)ino;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static bool scanner_has_visited(Scanner_Context *ctx, dev_t dev, ino_t ino)
{
    if (!ctx->visited.slots || ctx->visited.capacity == 0) return false;
    size_t mask = ctx->visited.capacity - 1;
    size_t idx = (size_t)(hash_dev_ino(dev, ino) & mask);
    while (ctx->visited.slots[idx].used) {
        if (ctx->visited.slots[idx].dev == dev && ctx->visited.slots[idx].ino == ino) {
            return true;
        }
        idx = (idx + 1) & mask;
    }
    return false;
}

static bool scanner_mark_visited(Scanner_Context *ctx, dev_t dev, ino_t ino)
{
    Visited_Set *set = &ctx->visited;
    if (set->capacity == 0 || (set->count * 10 >= set->capacity * 7)) {
        size_t new_cap = set->capacity == 0 ? 512 : set->capacity * 2;
        Visited_Slot *new_slots = (Visited_Slot *)calloc(new_cap, sizeof(Visited_Slot));
        if (!new_slots) {
            ctx->oom = true;
            return false;
        }

        size_t new_mask = new_cap - 1;
        for (size_t i = 0; i < set->capacity; ++i) {
            if (set->slots[i].used) {
                size_t idx = (size_t)(hash_dev_ino(set->slots[i].dev, set->slots[i].ino) & new_mask);
                while (new_slots[idx].used) {
                    idx = (idx + 1) & new_mask;
                }
                new_slots[idx] = set->slots[i];
            }
        }
        free(set->slots);
        set->slots = new_slots;
        set->capacity = new_cap;
    }

    size_t mask = set->capacity - 1;
    size_t idx = (size_t)(hash_dev_ino(dev, ino) & mask);
    while (set->slots[idx].used) {
        if (set->slots[idx].dev == dev && set->slots[idx].ino == ino) {
            return true;
        }
        idx = (idx + 1) & mask;
    }
    set->slots[idx].dev = dev;
    set->slots[idx].ino = ino;
    set->slots[idx].used = true;
    set->count++;
    return true;
}

static void scanner_recurse(Scanner_Context *ctx, const char *dir_path, int depth)
{
    if (depth > ctx->max_depth || ctx->oom) return;

    struct stat st;
    if (stat(dir_path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) return;

    if (scanner_has_visited(ctx, st.st_dev, st.st_ino)) {
        return; // Symlink or bind-mount loop.
    }
    if (!scanner_mark_visited(ctx, st.st_dev, st.st_ino)) return;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ctx->oom) break;
        if (should_ignore_dir_name(ctx, entry->d_name)) {
            continue;
        }

        char child_path[PATH_MAX];
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child_path)) continue;

#ifdef _DIRENT_HAVE_D_TYPE
        // d_type lets us skip the stat() for the overwhelmingly common cases.
        if (!ctx->follow_symlinks && entry->d_type != DT_UNKNOWN &&
            entry->d_type != DT_DIR && entry->d_type != DT_LNK) {
            continue;
        }
        if (entry->d_type == DT_DIR) {
            if (!scanner_add_path(ctx, child_path)) break;
            scanner_recurse(ctx, child_path, depth + 1);
            continue;
        }
#endif

        struct stat child_st;
        int stat_res = ctx->follow_symlinks ? stat(child_path, &child_st) : lstat(child_path, &child_st);
        if (stat_res != 0) continue;

        if (S_ISDIR(child_st.st_mode)) {
            if (!scanner_add_path(ctx, child_path)) break;
            scanner_recurse(ctx, child_path, depth + 1);
        }
    }

    closedir(dir);
}

void scanner_free_paths(char **paths, size_t count)
{
    if (!paths) return;
    for (size_t i = 0; i < count; ++i) {
        free(paths[i]);
    }
    free(paths);
}

static void scanner_context_free(Scanner_Context *ctx)
{
    scanner_free_paths(ctx->paths, ctx->count);
    ctx->paths = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    free(ctx->visited.slots);
    ctx->visited.slots = NULL;
    ctx->visited.capacity = 0;
    ctx->visited.count = 0;
}

static void *scan_root_thread(void *arg)
{
    Scan_Thread_Arg *targ = (Scan_Thread_Arg *)arg;
    Scanner_Context *ctx = &targ->ctx;

    if (scanner_add_path(ctx, targ->root_path)) {
        scanner_recurse(ctx, targ->root_path, 1);
    }
    return NULL;
}

// True when `inner` is the same directory as, or lives beneath, `outer`.
static bool path_is_within(const char *inner, const char *outer)
{
    size_t outer_len = strlen(outer);
    if (outer_len == 0) return false;
    if (strcmp(outer, "/") == 0) return true;
    if (strncmp(inner, outer, outer_len) != 0) return false;
    return inner[outer_len] == '\0' || inner[outer_len] == '/';
}

// Expands and normalizes each configured root, dropping ones that do not
// exist and ones already covered by another root.
static size_t collect_roots(const Jrun_Config *config, char (*out)[PATH_MAX], size_t out_cap)
{
    size_t n = 0;
    for (size_t i = 0; i < config->roots_count && n < out_cap; ++i) {
        char expanded[PATH_MAX];
        if (!path_expand_tilde(config->roots[i], expanded, sizeof(expanded))) continue;
        char normalized[PATH_MAX];
        if (!path_normalize(expanded, normalized, sizeof(normalized))) continue;
        if (!path_is_dir(normalized)) {
            jrun_log_debug("search root does not exist or is not a directory: %s", normalized);
            continue;
        }

        bool covered = false;
        for (size_t j = 0; j < n; ++j) {
            if (path_is_within(normalized, out[j])) {
                jrun_log_debug("skipping root '%s': already covered by '%s'", normalized, out[j]);
                covered = true;
                break;
            }
        }
        if (covered) continue;

        // The new root may itself subsume roots already collected.
        for (size_t j = 0; j < n;) {
            if (path_is_within(out[j], normalized)) {
                jrun_log_debug("dropping root '%s': subsumed by '%s'", out[j], normalized);
                memmove(out[j], out[j + 1], (n - j - 1) * sizeof(out[0]));
                n--;
            } else {
                j++;
            }
        }

        snprintf(out[n], PATH_MAX, "%s", normalized);
        n++;
    }
    return n;
}

#define SCANNER_MAX_ROOTS 64

bool scanner_scan_roots(const Jrun_Config *config, char ***out_paths, size_t *out_count)
{
    if (!config || !out_paths || !out_count) return false;

    *out_paths = NULL;
    *out_count = 0;

    char (*roots)[PATH_MAX] = malloc(SCANNER_MAX_ROOTS * PATH_MAX);
    if (!roots) return false;
    size_t root_count = collect_roots(config, roots, SCANNER_MAX_ROOTS);
    if (root_count == 0) {
        free(roots);
        return true;
    }

    int max_depth = config->max_depth > 0 ? config->max_depth : DEFAULT_MAX_DEPTH;

    Scan_Thread_Arg *args = (Scan_Thread_Arg *)calloc(root_count, sizeof(Scan_Thread_Arg));
    pthread_t *threads = (pthread_t *)calloc(root_count, sizeof(pthread_t));
    bool *joinable = (bool *)calloc(root_count, sizeof(bool));
    if (!args || !threads || !joinable) {
        free(roots);
        free(args);
        free(threads);
        free(joinable);
        return false;
    }

    for (size_t i = 0; i < root_count; ++i) {
        args[i].ctx.config = config;
        args[i].ctx.max_depth = max_depth;
        args[i].ctx.follow_symlinks = config->follow_symlinks;
        snprintf(args[i].root_path, PATH_MAX, "%s", roots[i]);

        // One root needs no thread at all; more than one, and a failed spawn
        // falls back to scanning inline so results stay complete.
        if (root_count == 1) {
            scan_root_thread(&args[i]);
        } else if (pthread_create(&threads[i], NULL, scan_root_thread, &args[i]) == 0) {
            joinable[i] = true;
        } else {
            jrun_log_debug("pthread_create failed for root '%s', scanning inline", roots[i]);
            scan_root_thread(&args[i]);
        }
    }

    free(roots);

    bool oom = false;
    size_t total = 0;
    for (size_t i = 0; i < root_count; ++i) {
        if (joinable[i]) pthread_join(threads[i], NULL);
        if (args[i].ctx.oom) oom = true;
        total += args[i].ctx.count;
    }

    char **merged = NULL;
    if (total > 0 && !oom) {
        merged = (char **)malloc(total * sizeof(char *));
        if (!merged) oom = true;
    }

    if (oom) {
        for (size_t i = 0; i < root_count; ++i) {
            scanner_context_free(&args[i].ctx);
        }
        free(merged);
        free(args);
        free(threads);
        free(joinable);
        jrun_log_error("ran out of memory while scanning search roots");
        return false;
    }

    size_t idx = 0;
    for (size_t i = 0; i < root_count; ++i) {
        for (size_t j = 0; j < args[i].ctx.count; ++j) {
            merged[idx++] = args[i].ctx.paths[j];
        }
        // The strings moved into `merged`; only the per-thread bookkeeping
        // is ours to release here.
        free(args[i].ctx.paths);
        free(args[i].ctx.visited.slots);
    }

    free(args);
    free(threads);
    free(joinable);

    *out_paths = merged;
    *out_count = idx;
    return true;
}
