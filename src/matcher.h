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

#endif // JRUN_MATCHER_H_
