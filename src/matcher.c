#include "common.h"
#include "matcher.h"
#include "path_util.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static bool is_word_boundary(char prev)
{
    return (prev == '/' || prev == '_' || prev == '-' || prev == '.' || prev == ' ');
}

static const char *strcasestr_custom(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (needle[0] == '\0') return haystack;

    size_t needle_len = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }
    return NULL;
}

// Check if pattern matches as an acronym / initials of words in text
// e.g. "pms" matches "PMS.CatalogService" or "Property_Management_System"
static bool match_acronym(const char *pattern, const char *text)
{
    if (!pattern || !text) return false;
    size_t plen = strlen(pattern);
    if (plen == 0) return true;

    size_t pi = 0;
    for (size_t ti = 0; text[ti] && pi < plen; ++ti) {
        bool is_start = (ti == 0) || is_word_boundary(text[ti - 1]) ||
                        (isupper((unsigned char)text[ti]) && !isupper((unsigned char)text[ti - 1]));
        if (is_start) {
            char p_char = (char)tolower((unsigned char)pattern[pi]);
            char t_char = (char)tolower((unsigned char)text[ti]);
            if (p_char == t_char) {
                pi++;
            }
        }
    }
    return (pi == plen);
}

bool matcher_fuzzy_subsequence(const char *pattern, const char *text, double *score_out)
{
    if (!pattern || !text) return false;
    if (pattern[0] == '\0') {
        if (score_out) *score_out = 1.0;
        return true;
    }

    size_t plen = strlen(pattern);
    size_t tlen = strlen(text);
    if (plen > tlen) return false;

    size_t pi = 0;
    size_t first_match = 0;
    size_t last_match = 0;
    double bonus = 0.0;
    bool prev_matched = false;

    for (size_t ti = 0; ti < tlen && pi < plen; ++ti) {
        char p_char = (char)tolower((unsigned char)pattern[pi]);
        char t_char = (char)tolower((unsigned char)text[ti]);

        if (p_char == t_char) {
            if (pi == 0) {
                first_match = ti;
            }
            last_match = ti;

            // Word boundary bonus
            if (ti == 0 || is_word_boundary(text[ti - 1]) || (isupper((unsigned char)text[ti]) && !isupper((unsigned char)text[ti - 1]))) {
                bonus += 5.0;
            }
            // Consecutive match bonus
            if (prev_matched) {
                bonus += 3.0;
            }

            prev_matched = true;
            pi++;
        } else {
            prev_matched = false;
        }
    }

    if (pi == plen) {
        size_t span = last_match - first_match + 1;
        double compactness = (span > 0) ? ((double)plen / (double)span) : 1.0;

        // Strictness filter to avoid accidental loose matches:
        // Either the characters are reasonably compact (span <= plen * 2)
        // OR at least half the characters matched at word boundaries
        if (compactness < 0.5 && bonus < (plen * 2.5)) {
            return false;
        }

        if (score_out) {
            *score_out = compactness * 10.0 + bonus;
        }
        return true;
    }

    return false;
}

// Multi-component match when target contains '/', e.g. "thirdparty/tatr" or "mereb/const"
static bool match_multicomponent(const char *target, const char *path, double *quality_out)
{
    char target_copy[256];
    strncpy(target_copy, target, sizeof(target_copy) - 1);
    target_copy[sizeof(target_copy) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(target_copy, "/", &saveptr);
    const char *search_in = path;
    int matched_parts = 0;
    int total_parts = 0;

    while (token) {
        total_parts++;
        const char *found = strcasestr_custom(search_in, token);
        if (found) {
            matched_parts++;
            search_in = found + strlen(token);
        }
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (total_parts > 0 && matched_parts == total_parts) {
        if (quality_out) {
            *quality_out = 70.0 + ((double)matched_parts / (double)total_parts) * 20.0;
        }
        return true;
    }
    return false;
}

Match_Result matcher_evaluate_opts(const char *target, const char *path, bool enable_fuzzy)
{
    Match_Result res = {0};
    if (!target || !path || target[0] == '\0' || path[0] == '\0') {
        return res;
    }

    const char *bname = path_basename(path);
    size_t target_len = strlen(target);
    size_t bname_len = strlen(bname);

    // 1. Exact full path match
    if (strcasecmp(target, path) == 0) {
        res.is_match = true;
        res.is_exact_path = true;
        res.is_exact_basename = true;
        res.quality_score = 150.0;
        return res;
    }

    // 2. Exact basename match (highest precedence for project/folder matching)
    if (strcasecmp(target, bname) == 0) {
        res.is_match = true;
        res.is_exact_basename = true;
        res.quality_score = 100.0;
        if (strcmp(target, bname) == 0) {
            res.quality_score += 10.0; // Exact case bonus
        }
        return res;
    }

    // 3. Multi-component path matching (e.g. "foo/bar" matching ".../foo/bar")
    if (strchr(target, '/') != NULL) {
        double multi_quality = 0.0;
        if (match_multicomponent(target, path, &multi_quality)) {
            res.is_match = true;
            res.quality_score = multi_quality;
            const char *last_slash = strrchr(target, '/');
            if (last_slash && strcasecmp(last_slash + 1, bname) == 0) {
                res.is_exact_basename = true;
                res.quality_score += 20.0;
            }
            return res;
        }
        return res;
    }

    // 4. Prefix match on basename (e.g. "const" matches "constituent")
    if (bname_len >= target_len && strncasecmp(bname, target, target_len) == 0) {
        res.is_match = true;
        double ratio = (double)target_len / (double)bname_len;
        res.quality_score = 60.0 + (ratio * 20.0);
        return res;
    }

    // 5. Substring match on basename (e.g. "tatr" in "my-tatr-project")
    const char *sub_bname = strcasestr_custom(bname, target);
    if (sub_bname) {
        res.is_match = true;
        double ratio = (double)target_len / (double)bname_len;
        res.quality_score = 40.0 + (ratio * 15.0);
        return res;
    }

    // 6. Word acronym match on basename (e.g. "pms" matches "PMS.CatalogService")
    if (target_len >= 2 && match_acronym(target, bname)) {
        res.is_match = true;
        res.quality_score = 45.0;
        return res;
    }

    // 7. Compact fuzzy subsequence on basename ONLY
    // Target characters must appear in order in the basename with high compactness or word boundaries
    if (enable_fuzzy) {
        double fuzzy_score = 0.0;
        if (matcher_fuzzy_subsequence(target, bname, &fuzzy_score)) {
            res.is_match = true;
            res.quality_score = 15.0 + fuzzy_score;
            return res;
        }
    }

    // NOTE: Matching across arbitrary directory boundary slashes on the full path
    // is intentionally NOT permitted when target has no slashes.
    // This prevents accidental substring collisions across parent folder names.

    return res;
}

Match_Result matcher_evaluate(const char *target, const char *path)
{
    return matcher_evaluate_opts(target, path, true);
}
