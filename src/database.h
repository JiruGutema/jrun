#ifndef JRUN_DATABASE_H_
#define JRUN_DATABASE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t id;
    char *path;
    double frequency;
    int64_t last_access;
    double frecency;
} Db_Entry;

bool db_init(const char *db_path);
void db_close(void);

bool db_add_or_update(const char *path);
bool db_remove(const char *path);
bool db_get_all(Db_Entry **entries, size_t *count);
bool db_get_top_k(Db_Entry **entries, size_t *count, size_t k);
void db_free_entries(Db_Entry *entries, size_t count);
bool db_prune(size_t *pruned_count);
bool db_age_if_needed(void);

double db_calculate_frecency(double frequency, int64_t last_access, int64_t current_time);

// --- Cached filesystem index -------------------------------------------------
//
// Walking every search root on each lookup dominates jrun's runtime, so the
// result is cached here and replayed until it goes stale. `fingerprint` comes
// from config_scan_fingerprint(): any change to roots, ignores, depth or
// symlink handling invalidates the cache immediately rather than after the TTL.

// True when a stored index exists, was built with this fingerprint, and is
// younger than ttl_seconds. A ttl of 0 always reports false.
bool db_cache_is_fresh(const char *fingerprint, int ttl_seconds);

// Loads the cached paths. Ownership transfers to the caller, which should
// release them with scanner_free_paths().
bool db_cache_load(char ***paths, size_t *count);

// Replaces the stored index in a single transaction.
bool db_cache_store(char **paths, size_t count, const char *fingerprint);

// Drops the stored index so the next lookup rescans.
bool db_cache_invalidate(void);

// Number of paths currently cached, for `jrun doctor`.
bool db_cache_stats(size_t *count, int64_t *built_at);

// --- Bookmarks ---------------------------------------------------------------
//
// A bookmark pins a name to one directory. It is checked before any matching,
// so `jrun <name>` always lands there whatever the frecency says.

typedef struct {
    char *name;
    char *path;
} Db_Bookmark;

// Creates or repoints a bookmark. `path` is normalized and must be an
// existing directory.
bool db_bookmark_set(const char *name, const char *path);
// *removed reports whether a bookmark by that name existed.
bool db_bookmark_remove(const char *name, bool *removed);
// *out_path is NULL when there is no such bookmark; otherwise caller frees.
bool db_bookmark_get(const char *name, char **out_path);
// All bookmarks, sorted by name. Release with db_free_bookmarks().
bool db_bookmark_list(Db_Bookmark **out, size_t *count);
void db_free_bookmarks(Db_Bookmark *bookmarks, size_t count);

#endif // JRUN_DATABASE_H_
