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

static int compare_candidates_desc(const void *a, const void *b)
{
    const Resolve_Candidate *ca = (const Resolve_Candidate *)a;
    const Resolve_Candidate *cb = (const Resolve_Candidate *)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return 0;
}

static ssize_t find_candidate(const Resolve_Candidate *candidates, size_t count, const char *path)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(candidates[i].path, path) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
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

    // Check if target is directly a path that exists on filesystem
    char norm_direct[PATH_MAX];
    if (path_normalize(target, norm_direct, sizeof(norm_direct)) && path_is_dir(norm_direct)) {
        char *path_copy = strdup(norm_direct);
        if (path_copy) {
            candidates[count].path = path_copy;
            candidates[count].score = 1000.0;
            candidates[count].frecency = 10.0;
            candidates[count].match_quality = 100.0;
            candidates[count].is_exact_basename = true;
            candidates[count].from_db = false;
            count++;
        }
    }

    // 1. Evaluate database entries
    Db_Entry *db_entries = NULL;
    size_t db_count = 0;
    if (db_get_all(&db_entries, &db_count)) {
        for (size_t i = 0; i < db_count; ++i) {
            if (!path_is_dir(db_entries[i].path)) {
                continue; // Skip stale paths
            }

            Match_Result match = matcher_evaluate_opts(target, db_entries[i].path, enable_fuzzy);
            if (match.is_match) {
                ssize_t existing_idx = find_candidate(candidates, count, db_entries[i].path);
                if (existing_idx >= 0) {
                    candidates[existing_idx].from_db = true;
                    candidates[existing_idx].frecency = db_entries[i].frecency;
                    candidates[existing_idx].score += db_entries[i].frecency * (match.quality_score / 10.0);
                } else {
                    bool ok = true;
                    JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                    if (!ok) break;

                    char *path_copy = strdup(db_entries[i].path);
                    if (!path_copy) break;

                    candidates[count].path = path_copy;
                    candidates[count].frecency = db_entries[i].frecency;
                    candidates[count].match_quality = match.quality_score;
                    candidates[count].is_exact_basename = match.is_exact_basename;
                    candidates[count].from_db = true;
                    candidates[count].score = (db_entries[i].frecency + 1.0) * match.quality_score;
                    count++;
                }
            }
        }
        db_free_entries(db_entries, db_count);
    }

    // 2. Scan configured filesystem roots if needed
    // Per SRS §9, do not scan roots if DB already provides high-confidence matches,
    // unless force_scan is explicitly requested.
    bool has_high_confidence_match = false;
    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].is_exact_basename || candidates[i].match_quality >= 80.0) {
            has_high_confidence_match = true;
            break;
        }
    }

    bool need_scan = force_scan || (!has_high_confidence_match);
    if (need_scan && config) {
        char **scanned_paths = NULL;
        size_t scanned_count = 0;
        if (scanner_scan_roots(config, &scanned_paths, &scanned_count)) {
            for (size_t i = 0; i < scanned_count; ++i) {
                const char *p = scanned_paths[i];
                Match_Result match = matcher_evaluate_opts(target, p, enable_fuzzy);
                if (match.is_match) {
                    ssize_t existing_idx = find_candidate(candidates, count, p);
                    if (existing_idx < 0) {
                        bool ok = true;
                        JRUN_DA_GROW(candidates, count, capacity, 32, ok);
                        if (!ok) break;

                        char *path_copy = strdup(p);
                        if (!path_copy) break;

                        candidates[count].path = path_copy;
                        candidates[count].frecency = 1.0;
                        candidates[count].match_quality = match.quality_score;
                        candidates[count].is_exact_basename = match.is_exact_basename;
                        candidates[count].from_db = false;
                        candidates[count].score = 1.0 * match.quality_score;
                        count++;
                    }
                }
            }
            scanner_free_paths(scanned_paths, scanned_count);
        }
    }

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

    // If both top 2 are exact basename matches with similar or duplicate names, they are ambiguous!
    if (candidates[0].is_exact_basename && candidates[1].is_exact_basename) {
        // If top candidate has significantly higher frecency (e.g. heavily visited vs not visited)
        if (candidates[0].score >= threshold * candidates[1].score && candidates[0].from_db && !candidates[1].from_db) {
            result.status = RESOLVE_HIGH_CONFIDENCE;
        } else {
            result.status = RESOLVE_AMBIGUOUS;
        }
    } else if (candidates[0].score >= threshold * candidates[1].score && candidates[0].match_quality >= 50.0) {
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
