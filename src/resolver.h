#ifndef JRUN_RESOLVER_H_
#define JRUN_RESOLVER_H_

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

typedef struct {
    char *path;
    double score;
    double frecency;
    double match_quality;
    bool is_exact_basename;
    bool from_db;
    // Short label such as "rust" or "git" when the directory holds a project
    // marker, else NULL. Points at a string literal; never freed.
    const char *project_kind;
} Resolve_Candidate;

typedef enum {
    RESOLVE_NO_MATCH = 0,
    RESOLVE_SINGLE_MATCH = 1,
    RESOLVE_HIGH_CONFIDENCE = 2,
    RESOLVE_AMBIGUOUS = 3
} Resolve_Status;

typedef struct {
    Resolve_Candidate *candidates;
    size_t count;
    Resolve_Status status;
} Resolve_Result;

Resolve_Result resolver_resolve(const char *target, const Jrun_Config *config, bool force_scan);
void resolver_free_result(Resolve_Result *res);

// Rebuilds the cached filesystem index up front. Returns the number of
// directories indexed via *out_count.
bool resolver_reindex(const Jrun_Config *config, size_t *out_count);

#endif // JRUN_RESOLVER_H_
