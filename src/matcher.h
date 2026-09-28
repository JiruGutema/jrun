#ifndef JRUN_MATCHER_H_
#define JRUN_MATCHER_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool is_match;
    double quality_score;
    bool is_exact_basename;
    bool is_exact_path;
} Match_Result;

Match_Result matcher_evaluate(const char *target, const char *path);
Match_Result matcher_evaluate_opts(const char *target, const char *path, bool enable_fuzzy);
bool matcher_fuzzy_subsequence(const char *pattern, const char *text, double *score_out);

// Marks which bytes of `text` `pattern` actually matched, for highlighting.
// `mask` must hold at least strlen(text) entries; entry i is set to 1 when
// byte i participates in the match. Returns the number of bytes marked.
//
// Prefers a contiguous run over a scattered subsequence, and prefers matching
// inside the final path component, which is what the ranking rewards too.
size_t matcher_highlight(const char *pattern, const char *text,
                         unsigned char *mask, size_t mask_len);

#endif // JRUN_MATCHER_H_
