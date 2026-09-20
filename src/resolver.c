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

typedef struct {
    char *path;
    size_t index;
    bool used;
} Path_Hash_Slot;

typedef struct {
    Path_Hash_Slot *slots;
    size_t capacity;
    size_t count;
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

static void path_set_init(Path_Hash_Set *set)
{
    set->capacity = 64;
    set->count = 0;
    set->slots = (Path_Hash_Slot *)calloc(set->capacity, sizeof(Path_Hash_Slot));
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

static void path_set_grow(Path_Hash_Set *set)
{
    size_t new_cap = set->capacity * 2;
    Path_Hash_Slot *new_slots = (Path_Hash_Slot *)calloc(new_cap, sizeof(Path_Hash_Slot));
    if (!new_slots) return;

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

static void path_set_insert(Path_Hash_Set *set, const char *path, size_t index)
{
    if (set->count * 10 >= set->capacity * 7) {
        path_set_grow(set);
    }

    size_t mask = set->capacity - 1;
    uint64_t h = fnv1a_hash(path);
    size_t idx = (size_t)(h & mask);

    while (set->slots[idx].used) {
        idx = (idx + 1) & mask;
    }

    set->slots[idx].path = jrun_strdup(path);
    set->slots[idx].index = index;
    set->slots[idx].used = true;
    set->count++;
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

static double compute_score(double frecency, double match_quality, bool from_db)
{
    double norm_freq = frecency / (frecency + 1.0);
    double norm_qual = match_quality / 150.0;
    if (norm_qual < 0.0) norm_qual = 0.0;
    if (norm_qual > 1.0) norm_qual = 1.0;

    double w_freq = from_db ? 0.55 : 0.25;
    double w_qual = 1.0 - w_freq;
    double score = (w_freq * norm_freq + w_qual * norm_qual) * 100.0;

    if (from_db && frecency > 8.0) {
        score *= 1.0 + log2(frecency / 8.0) * 0.08;
    }

    return score;
}

Resolve_Result resolver_resolve(const char *target, const Jrun_Config *config, bool force_scan)
{
    Resolve_Result result = {0};
    if (!target || target[0] == '\0') {
        result.status = RESOLVE_NO_MATCH;
        return result;
    }

    bool enable_fuzzy = config ? config->fuzzy : true;
    size_t capacity = 32;
    Resolve_Candidate *candidates = (Resolve_Candidate *)malloc(capacity * sizeof(Resolve_Candidate));
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
            if (!path_is_dir(db_entries[i].path)) {
                continue;
            }

            Match_Result match = matcher_evaluate_opts(target, db_entries[i].path, enable_fuzzy);
            if (match.is_match) {
                ssize_t existing_idx = path_set_find(&candidate_set, db_entries[i].path);
                if (existing_idx >= 0) {
                    candidates[existing_idx].from_db = true;
                    candidates[existing_idx].frecency = db_entries[i].frecency;
                    candidates[existing_idx].score = compute_score(
                        db_entries[i].frecency, match.quality_score, true);
                } else {
                    bool ok = true;
                    JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                    if (!ok) break;

                    char *path_copy = jrun_strdup(db_entries[i].path);
                    if (!path_copy) break;

                    candidates[count].path = path_copy;
                    candidates[count].frecency = db_entries[i].frecency;
                    candidates[count].match_quality = match.quality_score;
                    candidates[count].is_exact_basename = match.is_exact_basename;
                    candidates[count].from_db = true;
                    candidates[count].score = compute_score(
                        db_entries[i].frecency, match.quality_score, true);
                    path_set_insert(&candidate_set, path_copy, count);
                    count++;
                }
            }
        }
        db_free_entries(db_entries, db_count);
    }

    bool has_high_confidence_match = false;
    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].is_exact_basename || candidates[i].match_quality >= 80.0) {
            has_high_confidence_match = true;
            break;
        }
    }

    bool unique_exact = (count == 1 && candidates[0].is_exact_basename);
    bool need_scan = force_scan || (!has_high_confidence_match) || unique_exact;
    if (need_scan && config) {
        char **scanned_paths = NULL;
        size_t scanned_count = 0;
        if (scanner_scan_roots(config, &scanned_paths, &scanned_count)) {
            for (size_t i = 0; i < scanned_count; ++i) {
                const char *p = scanned_paths[i];
                Match_Result match = matcher_evaluate_opts(target, p, enable_fuzzy);
                if (match.is_match) {
                    ssize_t existing_idx = path_set_find(&candidate_set, p);
                    if (existing_idx < 0) {
                        bool ok = true;
                        JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                        if (!ok) break;

                        char *path_copy = jrun_strdup(p);
                        if (!path_copy) break;

                        candidates[count].path = path_copy;
                        candidates[count].frecency = 1.0;
                        candidates[count].match_quality = match.quality_score;
                        candidates[count].is_exact_basename = match.is_exact_basename;
                        candidates[count].from_db = false;
                        candidates[count].score = compute_score(
                            1.0, match.quality_score, false);
                        path_set_insert(&candidate_set, path_copy, count);
                        count++;
                    }
                }
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

    // Sort descending by score
    qsort(candidates, count, sizeof(Resolve_Candidate), compare_candidates_desc);

    result.candidates = candidates;
    result.count = count;

    if (count == 1) {
        result.status = RESOLVE_SINGLE_MATCH;
        return result;
    }

    // Check for ambiguity vs high confidence
    double threshold = (config && config->frecency_threshold > 0.0) ? config->frecency_threshold : DEFAULT_FRECENCY_THRESHOLD;

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
