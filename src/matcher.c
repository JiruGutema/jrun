#include "common.h"
#include "matcher.h"
#include "path_util.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static bool is_word_boundary(const char *text, size_t i)
{
    if (i == 0) return true;
    unsigned char prev = (unsigned char)text[i - 1];
    unsigned char cur = (unsigned char)text[i];
    if (prev == '/' || prev == '_' || prev == '-' || prev == '.' || prev == ' ') return true;
    if (isupper(cur) && !isupper(prev)) return true;
    if (isalpha(cur) && !isalpha(prev)) return true;
    if (isdigit(cur) && !isdigit(prev)) return true;
    return false;
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

static bool chars_eq_ci(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool match_acronym(const char *pattern, const char *text)
{
    if (!pattern || !text) return false;
    size_t plen = strlen(pattern);
    if (plen == 0) return true;

    size_t pi = 0;
    for (size_t ti = 0; text[ti]; ++ti) {
        if (!is_word_boundary(text, ti)) continue;
        if (chars_eq_ci(pattern[pi], text[ti])) {
            pi++;
            if (pi == plen) return true;
        }
    }
    return false;
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

    const size_t stack_cap = 96;
    double best_stack[96];
    size_t first_stack[96];
    size_t last_stack[96];
    size_t boundaries_stack[96];
    unsigned char got_stack[96];

    double *best = best_stack;
    size_t *first = first_stack;
    size_t *last = last_stack;
    size_t *boundaries = boundaries_stack;
    unsigned char *got = got_stack;
    bool heap = false;

    if (plen > stack_cap) {
        best = (double *)calloc(plen, sizeof(double));
        first = (size_t *)calloc(plen, sizeof(size_t));
        last = (size_t *)calloc(plen, sizeof(size_t));
        boundaries = (size_t *)calloc(plen, sizeof(size_t));
        got = (unsigned char *)calloc(plen, sizeof(unsigned char));
        heap = true;
        if (!best || !first || !last || !boundaries || !got) {
            free(best); free(first); free(last); free(boundaries); free(got);
            return false;
        }
    } else {
        memset(best, 0, plen * sizeof(double));
        memset(first, 0, plen * sizeof(size_t));
        memset(last, 0, plen * sizeof(size_t));
        memset(boundaries, 0, plen * sizeof(size_t));
        memset(got, 0, plen * sizeof(unsigned char));
    }

    for (size_t ti = 0; ti < tlen; ++ti) {
        for (size_t k = plen; k-- > 0;) {
            if (!chars_eq_ci(pattern[k], text[ti])) continue;
            if (k > 0 && !got[k - 1]) continue;

            bool boundary = is_word_boundary(text, ti);
            double incoming = (k == 0) ? 0.0 : best[k - 1];
            double bonus = 1.0;
            if (boundary) bonus += 5.0;
            if (k > 0 && last[k - 1] + 1 == ti) {
                bonus += 2.5;
            } else if (k > 0) {
                size_t gap = ti - last[k - 1] - 1;
                bonus -= (double)gap * 0.15;
            }
            if (k == 0 && ti == 0) bonus += 8.0;
            else if (k == 0 && ti <= 2) bonus += 3.0;

            double cand = incoming + bonus;
            if (!got[k] || cand > best[k]) {
                best[k] = cand;
                last[k] = ti;
                first[k] = (k == 0) ? ti : first[k - 1];
                boundaries[k] = (k == 0 ? 0 : boundaries[k - 1]) + (boundary ? 1 : 0);
                got[k] = 1;
            }
        }
    }

    bool matched = got[plen - 1] != 0;
    if (matched) {
        size_t span = last[plen - 1] - first[plen - 1] + 1;
        double compactness = (double)plen / (double)span;
        double boundary_ratio = (double)boundaries[plen - 1] / (double)plen;

        if (compactness < 0.35 && boundary_ratio < 0.3) {
            matched = false;
        } else if (score_out) {
            double coverage_penalty = 0.0;
            if (span > plen * 3) {
                coverage_penalty = (double)(span - plen * 3) * 0.4;
            }
            *score_out = compactness * 10.0 + best[plen - 1] - coverage_penalty;
        }
    }

    if (heap) {
        free(best);
        free(first);
        free(last);
        free(boundaries);
        free(got);
    }
    return matched;
}

static bool component_matches(const char *part, const char *comp, double *quality)
{
    if (strcasecmp(part, comp) == 0) {
        *quality = 20.0;
        return true;
    }
    size_t plen = strlen(part);
    size_t clen = strlen(comp);
    if (clen >= plen && strncasecmp(comp, part, plen) == 0) {
        *quality = 12.0 + 6.0 * ((double)plen / (double)clen);
        return true;
    }
    if (strcasestr_custom(comp, part)) {
        *quality = 8.0;
        return true;
    }
    double fuzzy = 0.0;
    if (matcher_fuzzy_subsequence(part, comp, &fuzzy)) {
        *quality = 5.0 + fuzzy * 0.2;
        return true;
    }
    return false;
}

static bool match_multicomponent(const char *target, const char *path, double *quality_out)
{
    char target_copy[256];
    strncpy(target_copy, target, sizeof(target_copy) - 1);
    target_copy[sizeof(target_copy) - 1] = '\0';

    char path_copy[PATH_MAX];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    const char *tparts[32];
    size_t nt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(target_copy, "/", &save); tok && nt < 32; tok = strtok_r(NULL, "/", &save)) {
        if (tok[0] != '\0') tparts[nt++] = tok;
    }
    if (nt == 0) return false;

    const char *pparts[128];
    size_t np = 0;
    save = NULL;
    for (char *tok = strtok_r(path_copy, "/", &save); tok && np < 128; tok = strtok_r(NULL, "/", &save)) {
        if (tok[0] != '\0') pparts[np++] = tok;
    }

    size_t pi = 0;
    int matched = 0;
    double total_q = 0.0;
    for (size_t ti = 0; ti < nt; ++ti) {
        bool found = false;
        while (pi < np) {
            double q = 0.0;
            if (component_matches(tparts[ti], pparts[pi], &q)) {
                total_q += q;
                matched++;
                pi++;
                found = true;
                break;
            }
            pi++;
        }
        if (!found) return false;
    }

    if (matched != (int)nt) return false;
    if (quality_out) {
        *quality_out = 70.0 + total_q;
        if (*quality_out > 95.0) *quality_out = 95.0;
    }
    return true;
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

    if (strcasecmp(target, path) == 0) {
        res.is_match = true;
        res.is_exact_path = true;
        res.is_exact_basename = true;
        res.quality_score = 150.0;
        return res;
    }

    if (strcasecmp(target, bname) == 0) {
        res.is_match = true;
        res.is_exact_basename = true;
        res.quality_score = 100.0;
        if (strcmp(target, bname) == 0) {
            res.quality_score += 10.0;
        }
        return res;
    }

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

    {
        size_t path_len = strlen(path);
        if (target_len < path_len) {
            const char *suffix = path + path_len - target_len;
            if (suffix > path && *(suffix - 1) == '/' &&
                strcasecmp(target, suffix) == 0) {
                res.is_match = true;
                res.quality_score = 55.0;
                res.is_exact_basename = (strcasecmp(target, bname) == 0);
                return res;
            }
        }
    }

    if (bname_len >= target_len && strncasecmp(bname, target, target_len) == 0) {
        res.is_match = true;
        double ratio = (double)target_len / (double)bname_len;
        res.quality_score = 60.0 + (ratio * 20.0);
        return res;
    }

    const char *sub_bname = strcasestr_custom(bname, target);
    if (sub_bname) {
        res.is_match = true;
        double ratio = (double)target_len / (double)bname_len;
        bool at_boundary = is_word_boundary(bname, (size_t)(sub_bname - bname));
        res.quality_score = (at_boundary ? 48.0 : 40.0) + (ratio * 15.0);
        return res;
    }

    if (target_len >= 2 && match_acronym(target, bname)) {
        res.is_match = true;
        res.quality_score = 45.0;
        return res;
    }

    if (enable_fuzzy) {
        double fuzzy_score = 0.0;
        if (matcher_fuzzy_subsequence(target, bname, &fuzzy_score)) {
            res.is_match = true;
            res.quality_score = 15.0 + fuzzy_score;
            return res;
        }
    }

    return res;
}

Match_Result matcher_evaluate(const char *target, const char *path)
{
    return matcher_evaluate_opts(target, path, true);
}
