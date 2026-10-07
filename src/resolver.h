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
    // Reached through a `jrun mark` bookmark rather than by matching.
    bool is_bookmark;
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

// Besides ordinary names and paths, `target` may be:
//   @, @/sub      the root of the project holding the working directory
//   name, name/sub  a bookmark made with `jrun mark`
// Both resolve to a single candidate without any matching.
Resolve_Result resolver_resolve(const char *target, const Jrun_Config *config, bool force_scan);
void resolver_free_result(Resolve_Result *res);

// Names for shell tab completion of a target: the final component of each
// matching directory, best match first, without duplicates. When any of them
// start with `prefix` only those are returned, otherwise the fuzzy matches
// are, so a loose prefix still completes to something. An empty prefix lists
// the most frecent directories. Release with resolver_free_names().
bool resolver_complete(const char *prefix, const Jrun_Config *config, size_t limit,
                       char ***out_names, size_t *out_count);
void resolver_free_names(char **names, size_t count);

// True for "@" and "@/...", the targets naming the current project root.
bool resolver_is_project_target(const char *target);

// The root of the project holding `from`: the nearest enclosing repository
// (.git, .hg, .svn), or failing that the nearest directory with any project
// marker. When `from` is itself a project root the search starts from its
// parent, so going to the root again climbs out to an enclosing project.
// Returns false when `from` is in no project.
bool resolver_project_root(const char *from, char *out, size_t out_size);

// Rebuilds the cached filesystem index up front. Returns the number of
// directories indexed via *out_count.
bool resolver_reindex(const Jrun_Config *config, size_t *out_count);

#endif // JRUN_RESOLVER_H_
