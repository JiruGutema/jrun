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

    int max_depth;
    bool follow_symlinks;
} Scanner_Context;

typedef struct {
    Scanner_Context ctx;
    char root_path[PATH_MAX];
    int max_depth;
    bool follow_symlinks;
    bool success;
} Scan_Thread_Arg;

static const char *IGNORED_DIR_NAMES[] = {
    ".git",
    ".hg",
    ".svn",
    "node_modules",
    "build",
    "dist",
    "target",
    "vendor",
    "__pycache__",
    ".cache",
    ".cargo",
    ".rustup",
    ".npm",
    ".local",
    NULL
};

static bool should_ignore_dir_name(const char *name)
{
    if (!name || name[0] == '\0') return true;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;

    // Skip hidden files/directories except .config
    if (name[0] == '.' && strcmp(name, ".config") != 0) {
        return true;
    }

    for (size_t i = 0; IGNORED_DIR_NAMES[i] != NULL; ++i) {
        if (strcmp(name, IGNORED_DIR_NAMES[i]) == 0) {
            return true;
        }
    }

    return false;
}

static bool scanner_add_path(Scanner_Context *ctx, const char *path)
{
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? 64 : ctx->capacity * 2;
        char **new_paths = (char **)realloc(ctx->paths, new_cap * sizeof(char *));
        if (!new_paths) return false;
        ctx->paths = new_paths;
        ctx->capacity = new_cap;
    }

    char *copy = jrun_strdup(path);
    if (!copy) return false;
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
        if (!new_slots) return false;

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
    if (depth > ctx->max_depth) return;

    struct stat st;
    if (stat(dir_path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) return;

    if (scanner_has_visited(ctx, st.st_dev, st.st_ino)) {
        return; // Avoid loop
    }
    scanner_mark_visited(ctx, st.st_dev, st.st_ino);

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (should_ignore_dir_name(entry->d_name)) {
            continue;
        }

        char child_path[PATH_MAX];
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child_path)) continue;

#ifdef _DIRENT_HAVE_D_TYPE
        if (!ctx->follow_symlinks && entry->d_type != DT_UNKNOWN &&
            entry->d_type != DT_DIR && entry->d_type != DT_LNK) {
            continue;
        }
        if (entry->d_type == DT_DIR) {
            scanner_add_path(ctx, child_path);
            scanner_recurse(ctx, child_path, depth + 1);
            continue;
        }
#endif

        struct stat child_st;
        int stat_res = ctx->follow_symlinks ? stat(child_path, &child_st) : lstat(child_path, &child_st);
        if (stat_res != 0) continue;

        if (S_ISDIR(child_st.st_mode)) {
            scanner_add_path(ctx, child_path);
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

static void *scan_root_thread(void *arg)
{
    Scan_Thread_Arg *targ = (Scan_Thread_Arg *)arg;
    Scanner_Context *ctx = &targ->ctx;

    scanner_add_path(ctx, targ->root_path);
    scanner_recurse(ctx, targ->root_path, 1);
    targ->success = true;
    return NULL;
}

static bool scanner_merge_results(char ***out_paths, size_t *out_count,
                                  Scanner_Context *results, size_t root_count)
{
    size_t total = 0;
    for (size_t i = 0; i < root_count; i++) {
        total += results[i].count;
    }

    if (total == 0) {
        *out_paths = NULL;
        *out_count = 0;
        return true;
    }

    char **merged = (char **)malloc(total * sizeof(char *));
    if (!merged) return false;

    size_t idx = 0;
    for (size_t i = 0; i < root_count; i++) {
        for (size_t j = 0; j < results[i].count; j++) {
            merged[idx++] = results[i].paths[j];
            results[i].paths[j] = NULL;
        }
    }

    *out_paths = merged;
    *out_count = idx;
    return true;
}

bool scanner_scan_roots(const Jrun_Config *config, char ***out_paths, size_t *out_count)
{
    if (!config || !out_paths || !out_count) return false;

    size_t valid_roots = 0;
    for (size_t i = 0; i < config->roots_count; ++i) {
        char expanded[PATH_MAX];
        if (!path_expand_tilde(config->roots[i], expanded, sizeof(expanded))) continue;
        char normalized[PATH_MAX];
        if (!path_normalize(expanded, normalized, sizeof(normalized))) continue;
        if (!path_is_dir(normalized)) {
            jrun_log_debug("search root does not exist or is not a directory: %s", normalized);
            continue;
        }
        valid_roots++;
    }

    if (valid_roots == 0) {
        *out_paths = NULL;
        *out_count = 0;
        return true;
    }

    int max_depth = config->max_depth > 0 ? config->max_depth : DEFAULT_MAX_DEPTH;
    bool follow_symlinks = config->follow_symlinks;

    if (valid_roots == 1) {
        Scanner_Context ctx = {0};
        ctx.max_depth = max_depth;
        ctx.follow_symlinks = follow_symlinks;

        for (size_t i = 0; i < config->roots_count; ++i) {
            char expanded[PATH_MAX];
            if (!path_expand_tilde(config->roots[i], expanded, sizeof(expanded))) continue;
            char normalized[PATH_MAX];
            if (!path_normalize(expanded, normalized, sizeof(normalized))) continue;
            if (!path_is_dir(normalized)) continue;

            scanner_add_path(&ctx, normalized);
            scanner_recurse(&ctx, normalized, 1);
            break;
        }

        free(ctx.visited.slots);
        *out_paths = ctx.paths;
        *out_count = ctx.count;
        return true;
    }

    pthread_t *threads = (pthread_t *)malloc(valid_roots * sizeof(pthread_t));
    Scan_Thread_Arg *args = (Scan_Thread_Arg *)malloc(valid_roots * sizeof(Scan_Thread_Arg));
    Scanner_Context *results = (Scanner_Context *)calloc(valid_roots, sizeof(Scanner_Context));

    if (!threads || !args || !results) {
        free(threads);
        free(args);
        free(results);
        return false;
    }

    size_t thread_idx = 0;
    for (size_t i = 0; i < config->roots_count; ++i) {
        char expanded[PATH_MAX];
        if (!path_expand_tilde(config->roots[i], expanded, sizeof(expanded))) continue;
        char normalized[PATH_MAX];
        if (!path_normalize(expanded, normalized, sizeof(normalized))) continue;
        if (!path_is_dir(normalized)) continue;

        results[thread_idx].max_depth = max_depth;
        results[thread_idx].follow_symlinks = follow_symlinks;

        args[thread_idx].ctx = results[thread_idx];
        strncpy(args[thread_idx].root_path, normalized, PATH_MAX - 1);
        args[thread_idx].root_path[PATH_MAX - 1] = '\0';
        args[thread_idx].max_depth = max_depth;
        args[thread_idx].follow_symlinks = follow_symlinks;
        args[thread_idx].success = false;

        pthread_create(&threads[thread_idx], NULL, scan_root_thread, &args[thread_idx]);
        thread_idx++;
    }

    for (size_t i = 0; i < valid_roots; i++) {
        pthread_join(threads[i], NULL);
        results[i] = args[i].ctx;
    }

    bool ok = scanner_merge_results(out_paths, out_count, results, valid_roots);

    for (size_t i = 0; i < valid_roots; i++) {
        free(results[i].visited.slots);
    }
    free(threads);
    free(args);
    free(results);

    return ok;
}
