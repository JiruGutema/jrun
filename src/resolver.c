#include "resolver.h"
#include "database.h"
#include "scanner.h"
#include "matcher.h"
#include "path_util.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

// Checking every candidate for project markers costs a handful of stat() calls
// each. Worth it for a normal result set; skipped when a very loose query
// matches an implausible number of directories.
#define PROJECT_PROBE_LIMIT 256

typedef struct {
    char *path;
    size_t index;
    bool used;
} Path_Hash_Slot;

typedef struct {
    Path_Hash_Slot *slots;
    size_t capacity;
    size_t count;
    bool failed;
} Path_Hash_Set;

static bool target_looks_like_path(const char *target)
{
    if (!target || target[0] == '\0') return false;
    if (target[0] == '/' || target[0] == '~') return true;
    if (target[0] == '.') {
        if (target[1] == '\0' || target[1] == '/') return true;
        if (target[1] == '.' && (target[2] == '\0' || target[2] == '/')) return true;
    }
    return strchr(target, '/') != NULL;
}

static inline uint64_t fnv1a_hash(const char *str)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *str; str++) {
        h ^= (uint8_t)*str;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool path_set_init(Path_Hash_Set *set)
{
    set->capacity = 64;
    set->count = 0;
    set->failed = false;
    set->slots = (Path_Hash_Slot *)calloc(set->capacity, sizeof(Path_Hash_Slot));
    if (!set->slots) {
        set->capacity = 0;
        set->failed = true;
        return false;
    }
    return true;
}

static void path_set_free(Path_Hash_Set *set)
{
    if (!set->slots) return;
    for (size_t i = 0; i < set->capacity; i++) {
        if (set->slots[i].used) {
            free(set->slots[i].path);
        }
    }
    free(set->slots);
    set->slots = NULL;
    set->capacity = 0;
    set->count = 0;
}

static bool path_set_grow(Path_Hash_Set *set)
{
    size_t new_cap = set->capacity * 2;
    Path_Hash_Slot *new_slots = (Path_Hash_Slot *)calloc(new_cap, sizeof(Path_Hash_Slot));
    if (!new_slots) {
        // Refusing to grow would leave insert probing a full table forever.
        set->failed = true;
        return false;
    }

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < set->capacity; i++) {
        if (set->slots[i].used) {
            uint64_t h = fnv1a_hash(set->slots[i].path);
            size_t idx = (size_t)(h & new_mask);
            while (new_slots[idx].used) {
                idx = (idx + 1) & new_mask;
            }
            new_slots[idx] = set->slots[i];
        }
    }
    free(set->slots);
    set->slots = new_slots;
    set->capacity = new_cap;
    return true;
}

static ssize_t path_set_find(const Path_Hash_Set *set, const char *path)
{
    if (!set->slots || set->capacity == 0) return -1;

    size_t mask = set->capacity - 1;
    uint64_t h = fnv1a_hash(path);
    size_t idx = (size_t)(h & mask);

    while (set->slots[idx].used) {
        if (strcmp(set->slots[idx].path, path) == 0) {
            return (ssize_t)set->slots[idx].index;
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

static bool path_set_insert(Path_Hash_Set *set, const char *path, size_t index)
{
    if (set->failed || !set->slots) return false;

    if (set->count * 10 >= set->capacity * 7) {
        if (!path_set_grow(set)) return false;
    }

    char *copy = jrun_strdup(path);
    if (!copy) {
        set->failed = true;
        return false;
    }

    size_t mask = set->capacity - 1;
    uint64_t h = fnv1a_hash(path);
    size_t idx = (size_t)(h & mask);

    while (set->slots[idx].used) {
        idx = (idx + 1) & mask;
    }

    set->slots[idx].path = copy;
    set->slots[idx].index = index;
    set->slots[idx].used = true;
    set->count++;
    return true;
}

static int compare_candidates_desc(const void *a, const void *b)
{
    const Resolve_Candidate *ca = (const Resolve_Candidate *)a;
    const Resolve_Candidate *cb = (const Resolve_Candidate *)b;
    if (ca->is_exact_basename != cb->is_exact_basename) {
        return ca->is_exact_basename ? -1 : 1;
    }
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    if (cb->match_quality > ca->match_quality) return 1;
    if (cb->match_quality < ca->match_quality) return -1;
    size_t la = strlen(ca->path);
    size_t lb = strlen(cb->path);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return strcmp(ca->path, cb->path);
}

// Turns match quality and visit history into a single ranking number.
//
// Quality alone sets the base; having actually visited a directory can only
// ever raise it. An earlier version reweighted the two inputs based on
// from_db, which meant a directory you had visited once scored *lower* than an
// identical one you had never opened, because the heavier frecency weight was
// applied to a near-zero frecency.
static double compute_score(double frecency, double match_quality, bool from_db)
{
    double norm_qual = match_quality / 150.0;
    if (norm_qual < 0.0) norm_qual = 0.0;
    if (norm_qual > 1.0) norm_qual = 1.0;

    double base = 25.0 + norm_qual * 75.0;  // 25 .. 100
    if (!from_db) return base;

    // norm_freq saturates towards 1, so the boost is generous for the first
    // few visits and then flattens out instead of running away.
    double norm_freq = frecency / (frecency + 1.0);
    double boost = 1.0 + 0.60 * norm_freq;
    if (frecency > 8.0) {
        boost += log2(frecency / 8.0) * 0.08;
    }
    return base * boost;
}

// Returns the directory list to match against: the cached index when it is
// still valid, otherwise a fresh scan which is then cached for next time.
static bool gather_search_paths(const Jrun_Config *config, bool force_rescan,
                                char ***out_paths, size_t *out_count)
{
    *out_paths = NULL;
    *out_count = 0;

    char fingerprint[64];
    config_scan_fingerprint(config, fingerprint, sizeof(fingerprint));

    if (!force_rescan && db_cache_is_fresh(fingerprint, config->cache_ttl)) {
        if (db_cache_load(out_paths, out_count)) {
            return true;
        }
        jrun_log_debug("scan cache: load failed, falling back to a scan");
    }

    if (!scanner_scan_roots(config, out_paths, out_count)) {
        return false;
    }

    if (config->cache_ttl > 0) {
        db_cache_store(*out_paths, *out_count, fingerprint);
    }
    return true;
}

bool resolver_reindex(const Jrun_Config *config, size_t *out_count)
{
    if (!config) return false;

    char **paths = NULL;
    size_t count = 0;
    if (!gather_search_paths(config, true, &paths, &count)) {
        return false;
    }
    scanner_free_paths(paths, count);
    if (out_count) *out_count = count;
    return true;
}

// Directories holding a project marker are what people almost always mean, so
// give them a nudge that can break a tie without overriding a strong frecency
// or an exact-name match.
static void apply_project_bonus(Resolve_Candidate *candidates, size_t count)
{
    if (count > PROJECT_PROBE_LIMIT) {
        jrun_log_debug("skipping project detection for %zu candidates", count);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        candidates[i].project_kind = path_project_kind(candidates[i].path);
        if (candidates[i].project_kind) {
            candidates[i].score *= 1.15;
        }
    }
}

bool resolver_is_project_target(const char *target)
{
    return target && target[0] == '@' && (target[1] == '\0' || target[1] == '/');
}

static bool is_vcs_root(const char *path)
{
    static const char *const markers[] = { ".git", ".hg", ".svn" };
    char probe[PATH_MAX];
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        int n = snprintf(probe, sizeof(probe), "%s/%s", path, markers[i]);
        if (n > 0 && (size_t)n < sizeof(probe) && path_exists(probe)) return true;
    }
    return false;
}

// Cuts the last component off `path` in place. False once at "/".
static bool path_to_parent(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || (slash == path && path[1] == '\0')) return false;
    if (slash == path) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

// Walks up from `start`, inclusive, to the first directory that is a
// repository root, or with any_marker, that holds any project marker.
static bool find_enclosing(const char *start, bool any_marker, char *out, size_t out_size)
{
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", start);
    do {
        if (any_marker ? path_is_project_dir(cur) : is_vcs_root(cur)) {
            int n = snprintf(out, out_size, "%s", cur);
            return n > 0 && (size_t)n < out_size;
        }
    } while (path_to_parent(cur));
    return false;
}

bool resolver_project_root(const char *from, char *out, size_t out_size)
{
    char here[PATH_MAX];
    if (!from || !path_normalize(from, here, sizeof(here))) return false;

    // A directory with its own marker is a root unless it sits inside a
    // repository; a Makefile in src/ should not stop the climb.
    char scratch[PATH_MAX];
    bool at_root = is_vcs_root(here) ||
                   (path_is_project_dir(here) && !find_enclosing(here, false, scratch, sizeof(scratch)));

    char start[PATH_MAX];
    snprintf(start, sizeof(start), "%s", here);
    if (!at_root || path_to_parent(start)) {
        if (find_enclosing(start, false, out, out_size)) return true;
        if (find_enclosing(start, true, out, out_size)) return true;
    }

    // Nothing encloses this root, so it is as far up as `@` goes.
    if (at_root) {
        int n = snprintf(out, out_size, "%s", here);
        return n > 0 && (size_t)n < out_size;
    }
    return false;
}

// Appends "/rest" to `base` when `rest` is non-empty, normalizes the result
// and checks it is a directory.
static bool join_existing_dir(const char *base, const char *rest, char *out, size_t out_size)
{
    char joined[PATH_MAX];
    int n = (rest && rest[0])
          ? snprintf(joined, sizeof(joined), "%s/%s", base, rest)
          : snprintf(joined, sizeof(joined), "%s", base);
    if (n <= 0 || (size_t)n >= sizeof(joined)) return false;
    return path_normalize(joined, out, out_size) && path_is_dir(out);
}

// "@" or "@/sub" relative to the working directory's project root.
static bool project_target_path(const char *target, char *out, size_t out_size)
{
    char cwd[PATH_MAX];
    char root[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    if (!resolver_project_root(cwd, root, sizeof(root))) return false;
    return join_existing_dir(root, target[1] == '/' ? target + 2 : "", out, out_size);
}

// "name" or "name/sub" where name is a bookmark. A relative path that exists
// from here is left alone, as `cd` would take it.
static bool bookmark_target_path(const char *target, char *out, size_t out_size)
{
    char direct[PATH_MAX];
    if (target_looks_like_path(target) &&
        path_normalize(target, direct, sizeof(direct)) && path_is_dir(direct)) {
        return false;
    }

    char name[256];
    const char *slash = strchr(target, '/');
    size_t name_len = slash ? (size_t)(slash - target) : strlen(target);
    if (name_len == 0 || name_len >= sizeof(name)) return false;
    memcpy(name, target, name_len);
    name[name_len] = '\0';

    char *marked = NULL;
    if (!db_bookmark_get(name, &marked) || !marked) return false;

    bool ok = join_existing_dir(marked, slash ? slash + 1 : "", out, out_size);
    if (!ok) {
        jrun_log_error("bookmark '%s' points to %s, which is not a directory%s", name, marked,
                       slash ? " (or has no such subdirectory)" : "");
    }
    free(marked);
    return ok;
}

static Resolve_Result single_result(const char *path, bool is_bookmark)
{
    Resolve_Result result = {0};
    result.status = RESOLVE_NO_MATCH;
    Resolve_Candidate *cand = (Resolve_Candidate *)calloc(1, sizeof(Resolve_Candidate));
    char *copy = jrun_strdup(path);
    if (!cand || !copy) {
        free(cand);
        free(copy);
        return result;
    }
    *cand = (Resolve_Candidate){
        .path = copy,
        .score = 1000.0,
        .match_quality = 150.0,
        .is_exact_basename = true,
        .is_bookmark = is_bookmark,
        .project_kind = path_project_kind(copy),
    };
    result.candidates = cand;
    result.count = 1;
    result.status = RESOLVE_SINGLE_MATCH;
    return result;
}

// `shortcuts` enables bookmarks and "@". Completion turns them off: a prefix
// that happens to spell a bookmark must still complete to other names.
static Resolve_Result resolve_impl(const char *target, const Jrun_Config *config,
                                   bool force_scan, bool shortcuts)
{
    Resolve_Result result = {0};
    if (!target || target[0] == '\0') {
        result.status = RESOLVE_NO_MATCH;
        return result;
    }

    // Targets that name one directory outright skip matching altogether.
    char exact[PATH_MAX];
    if (shortcuts && resolver_is_project_target(target)) {
        if (!project_target_path(target, exact, sizeof(exact))) {
            result.status = RESOLVE_NO_MATCH;
            return result;
        }
        return single_result(exact, false);
    }
    if (shortcuts && bookmark_target_path(target, exact, sizeof(exact))) {
        return single_result(exact, true);
    }

    bool enable_fuzzy = config ? config->fuzzy : true;
    size_t capacity = 32;
    Resolve_Candidate *candidates = (Resolve_Candidate *)calloc(capacity, sizeof(Resolve_Candidate));
    if (!candidates) {
        result.status = RESOLVE_NO_MATCH;
        return result;
    }
    size_t count = 0;

    Path_Hash_Set candidate_set;
    path_set_init(&candidate_set);

    char norm_direct[PATH_MAX];
    if (target_looks_like_path(target) &&
        path_normalize(target, norm_direct, sizeof(norm_direct)) &&
        path_is_dir(norm_direct)) {
        char *path_copy = jrun_strdup(norm_direct);
        if (path_copy) {
            candidates[count].path = path_copy;
            candidates[count].score = 1000.0;
            candidates[count].frecency = 10.0;
            candidates[count].match_quality = 150.0;
            candidates[count].is_exact_basename = true;
            candidates[count].from_db = false;
            path_set_insert(&candidate_set, path_copy, count);
            count++;
        }
    }

    Db_Entry *db_entries = NULL;
    size_t db_count = 0;
    if (db_get_all(&db_entries, &db_count)) {
        for (size_t i = 0; i < db_count; ++i) {
            Match_Result match = matcher_evaluate_opts(target, db_entries[i].path, enable_fuzzy);
            if (!match.is_match) continue;

            // Matching is in-memory; only pay for a stat() on paths that match.
            if (!path_is_dir(db_entries[i].path)) continue;

            ssize_t existing_idx = path_set_find(&candidate_set, db_entries[i].path);
            if (existing_idx >= 0) {
                candidates[existing_idx].from_db = true;
                candidates[existing_idx].frecency = db_entries[i].frecency;
                candidates[existing_idx].score = compute_score(
                    db_entries[i].frecency, candidates[existing_idx].match_quality, true);
            } else {
                bool ok = true;
                JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                if (!ok) break;

                char *path_copy = jrun_strdup(db_entries[i].path);
                if (!path_copy) break;

                candidates[count] = (Resolve_Candidate){
                    .path = path_copy,
                    .frecency = db_entries[i].frecency,
                    .match_quality = match.quality_score,
                    .is_exact_basename = match.is_exact_basename,
                    .from_db = true,
                    .score = compute_score(db_entries[i].frecency, match.quality_score, true),
                };
                path_set_insert(&candidate_set, path_copy, count);
                count++;
            }
        }
        db_free_entries(db_entries, db_count);
    }

    // The filesystem index is always consulted, not just when the database
    // comes up short: two directories sharing a name are exactly the case the
    // interactive selector exists for, and skipping the scan would hide one.
    // Serving it from the cache is what keeps that affordable.
    if (config) {
        char **scanned_paths = NULL;
        size_t scanned_count = 0;
        if (gather_search_paths(config, force_scan, &scanned_paths, &scanned_count)) {
            for (size_t i = 0; i < scanned_count; ++i) {
                const char *p = scanned_paths[i];
                if (path_set_find(&candidate_set, p) >= 0) continue;

                Match_Result match = matcher_evaluate_opts(target, p, enable_fuzzy);
                if (!match.is_match) continue;

                // A cached path may have been deleted since the index was
                // built; never offer a directory that is no longer there.
                if (!path_is_dir(p)) continue;

                bool ok = true;
                JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                if (!ok) break;

                char *path_copy = jrun_strdup(p);
                if (!path_copy) break;

                candidates[count] = (Resolve_Candidate){
                    .path = path_copy,
                    .frecency = 1.0,
                    .match_quality = match.quality_score,
                    .is_exact_basename = match.is_exact_basename,
                    .from_db = false,
                    .score = compute_score(1.0, match.quality_score, false),
                };
                path_set_insert(&candidate_set, path_copy, count);
                count++;
            }
            scanner_free_paths(scanned_paths, scanned_count);
        }
    }

    path_set_free(&candidate_set);

    if (count == 0) {
        free(candidates);
        result.candidates = NULL;
        result.count = 0;
        result.status = RESOLVE_NO_MATCH;
        return result;
    }

    if (!config || config->prefer_projects) {
        apply_project_bonus(candidates, count);
    }

    qsort(candidates, count, sizeof(Resolve_Candidate), compare_candidates_desc);

    result.candidates = candidates;
    result.count = count;

    if (count == 1) {
        result.status = RESOLVE_SINGLE_MATCH;
        return result;
    }

    double threshold = (config && config->frecency_threshold > 0.0)
                     ? config->frecency_threshold : DEFAULT_FRECENCY_THRESHOLD;

    // Identical basenames in different places are always a TUI choice.
    if (candidates[0].is_exact_basename && candidates[1].is_exact_basename) {
        result.status = RESOLVE_AMBIGUOUS;
    } else if (candidates[0].is_exact_basename && !candidates[1].is_exact_basename) {
        result.status = RESOLVE_HIGH_CONFIDENCE;
    } else if (candidates[0].score >= threshold * candidates[1].score &&
               candidates[0].match_quality >= 50.0) {
        result.status = RESOLVE_HIGH_CONFIDENCE;
    } else {
        result.status = RESOLVE_AMBIGUOUS;
    }

    return result;
}

Resolve_Result resolver_resolve(const char *target, const Jrun_Config *config, bool force_scan)
{
    return resolve_impl(target, config, force_scan, true);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} Name_List;

static bool name_list_contains(const Name_List *list, const char *name)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], name) == 0) return true;
    }
    return false;
}

// Adds `name` unless it is already listed or the list is full. Returns false
// only on allocation failure.
static bool name_list_add(Name_List *list, const char *name, size_t limit)
{
    if (name[0] == '\0' || strcmp(name, "/") == 0) return true;
    if (list->count >= limit || name_list_contains(list, name)) return true;

    bool ok = true;
    JRUN_DA_GROW(list->items, list->count, list->capacity, 16, ok);
    if (!ok) return false;
    char *copy = jrun_strdup(name);
    if (!copy) return false;
    list->items[list->count++] = copy;
    return true;
}

// Adds the final component of `path`.
static bool name_list_add_basename(Name_List *list, const char *path, size_t limit)
{
    char name[PATH_MAX];
    path_basename_r(path, name, sizeof(name));
    return name_list_add(list, name, limit);
}

static int compare_entries_by_frecency_desc(const void *a, const void *b)
{
    const Db_Entry *ea = (const Db_Entry *)a;
    const Db_Entry *eb = (const Db_Entry *)b;
    if (eb->frecency > ea->frecency) return 1;
    if (eb->frecency < ea->frecency) return -1;
    return 0;
}

bool resolver_complete(const char *prefix, const Jrun_Config *config, size_t limit,
                       char ***out_names, size_t *out_count)
{
    if (!out_names || !out_count) return false;
    *out_names = NULL;
    *out_count = 0;
    if (limit == 0) return true;
    if (!prefix) prefix = "";

    // Paths are completed by the shell's own directory completion, and "@"
    // already says exactly where to go.
    if (target_looks_like_path(prefix) || prefix[0] == '@') return true;

    Name_List prefixed = {0};
    Name_List others = {0};
    bool ok = true;

    // Bookmarks are the names the user chose, so they come first.
    size_t prefix_len = strlen(prefix);
    Db_Bookmark *marks = NULL;
    size_t mark_count = 0;
    if (db_bookmark_list(&marks, &mark_count)) {
        for (size_t i = 0; i < mark_count && ok; ++i) {
            if (strncmp(marks[i].name, prefix, prefix_len) == 0) {
                ok = name_list_add(&prefixed, marks[i].name, limit);
            }
        }
        db_free_bookmarks(marks, mark_count);
    }

    if (prefix[0] == '\0') {
        Db_Entry *entries = NULL;
        size_t count = 0;
        if (db_get_all(&entries, &count)) {
            qsort(entries, count, sizeof(Db_Entry), compare_entries_by_frecency_desc);
            for (size_t i = 0; i < count && ok; ++i) {
                if (!path_is_dir(entries[i].path)) continue;
                ok = name_list_add_basename(&prefixed, entries[i].path, limit);
            }
            db_free_entries(entries, count);
        }
    } else {
        Resolve_Result res = resolve_impl(prefix, config, false, false);
        for (size_t i = 0; i < res.count && ok; ++i) {
            char name[PATH_MAX];
            path_basename_r(res.candidates[i].path, name, sizeof(name));
            Name_List *list = strncmp(name, prefix, prefix_len) == 0 ? &prefixed : &others;
            ok = name_list_add_basename(list, res.candidates[i].path, limit);
        }
        resolver_free_result(&res);
    }

    Name_List *keep = prefixed.count > 0 ? &prefixed : &others;
    Name_List *drop = keep == &prefixed ? &others : &prefixed;
    resolver_free_names(drop->items, drop->count);
    if (!ok) {
        resolver_free_names(keep->items, keep->count);
        return false;
    }
    *out_names = keep->items;
    *out_count = keep->count;
    return true;
}

void resolver_free_names(char **names, size_t count)
{
    if (!names) return;
    for (size_t i = 0; i < count; ++i) {
        free(names[i]);
    }
    free(names);
}

void resolver_free_result(Resolve_Result *res)
{
    if (!res || !res->candidates) return;
    for (size_t i = 0; i < res->count; ++i) {
        free(res->candidates[i].path);
    }
    free(res->candidates);
    res->candidates = NULL;
    res->count = 0;
}
