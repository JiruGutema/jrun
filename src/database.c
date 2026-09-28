#include "database.h"
#include "common.h"
#include "path_util.h"
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>

static sqlite3 *g_db = NULL;

double db_calculate_frecency(double frequency, int64_t last_access, int64_t current_time)
{
    if (frequency <= 0.0) frequency = 1.0;
    int64_t delta = current_time - last_access;
    if (delta < 0) delta = 0;

    // Half-life of 14 days, with a short-term boost for the last hour.
    double days = (double)delta / 86400.0;
    double recency = pow(0.5, days / 14.0);
    if (delta < 3600) {
        recency *= 1.25;
    }
    return frequency * recency;
}

bool db_init(const char *db_path)
{
    if (g_db) return true;

    if (!path_ensure_parent_dir(db_path)) {
        jrun_log_error("failed to create directory for database: %s", db_path);
        return false;
    }

    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        jrun_log_error("failed to open database '%s': %s", db_path, sqlite3_errmsg(g_db));
        if (g_db) {
            sqlite3_close(g_db);
            g_db = NULL;
        }
        return false;
    }

    // The header may be the vendored copy while the library comes from the
    // system, so make any skew visible instead of letting a missing symbol or a
    // changed default surface as mysterious behaviour later.
    if (sqlite3_libversion_number() < SQLITE_VERSION_NUMBER) {
        jrun_log_debug("sqlite runtime %s is older than the headers (%s)",
                       sqlite3_libversion(), SQLITE_VERSION);
    }

    // Set busy timeout so concurrent shell hooks don't fail immediately with SQLITE_BUSY
    sqlite3_busy_timeout(g_db, 2000);

    // Set performance pragmas
    sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS directories ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    path TEXT UNIQUE NOT NULL,"
        "    frequency REAL NOT NULL DEFAULT 1.0,"
        "    last_access INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_path ON directories(path);"
        "CREATE INDEX IF NOT EXISTS idx_last_access ON directories(last_access);"
        // Cached filesystem index; see db_cache_* below. Created lazily on
        // existing databases by the IF NOT EXISTS clauses.
        "CREATE TABLE IF NOT EXISTS scan_cache ("
        "    path TEXT PRIMARY KEY"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS meta ("
        "    key TEXT PRIMARY KEY,"
        "    value TEXT NOT NULL"
        ");";

    char *err_msg = NULL;
    rc = sqlite3_exec(g_db, schema, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        jrun_log_error("failed to initialize database schema: %s", err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }

    return true;
}

void db_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

bool db_add_or_update(const char *path)
{
    if (!g_db || !path || path[0] == '\0') return false;

    char normalized[PATH_MAX];
    if (!path_normalize(path, normalized, sizeof(normalized))) {
        jrun_log_error("failed to normalize path '%s'", path);
        return false;
    }

    // Must be an existing directory
    if (!path_is_dir(normalized)) {
        jrun_log_debug("not adding non-directory: %s", normalized);
        return false;
    }

    int64_t now = (int64_t)time(NULL);

    const char *upsert_sql =
        "INSERT INTO directories (path, frequency, last_access) "
        "VALUES (?1, 1.0, ?2) "
        "ON CONFLICT(path) DO UPDATE SET "
        "frequency = frequency + 1.0, "
        "last_access = ?2;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, upsert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        jrun_log_error("sqlite prepare failed: %s", sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, normalized, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        jrun_log_error("sqlite step failed: %s", sqlite3_errmsg(g_db));
        return false;
    }

    jrun_log_debug("added/updated directory in database: %s", normalized);
    return true;
}

bool db_remove(const char *path)
{
    if (!g_db || !path) return false;

    char normalized[PATH_MAX];
    if (!path_normalize(path, normalized, sizeof(normalized))) {
        strncpy(normalized, path, sizeof(normalized) - 1);
        normalized[sizeof(normalized) - 1] = '\0';
    }

    const char *sql = "DELETE FROM directories WHERE path = ?1;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, normalized, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

static int compare_entries_desc(const void *a, const void *b)
{
    const Db_Entry *ea = (const Db_Entry *)a;
    const Db_Entry *eb = (const Db_Entry *)b;
    if (eb->frecency > ea->frecency) return 1;
    if (eb->frecency < ea->frecency) return -1;
    return 0;
}

static bool db_load_entries(sqlite3_stmt *stmt, Db_Entry **entries, size_t *count)
{
    size_t capacity = 16;
    Db_Entry *list = (Db_Entry *)malloc(capacity * sizeof(Db_Entry));
    if (!list) return false;

    int64_t now = (int64_t)time(NULL);
    size_t n = 0;

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= capacity) {
            capacity *= 2;
            Db_Entry *new_list = (Db_Entry *)realloc(list, capacity * sizeof(Db_Entry));
            if (!new_list) {
                db_free_entries(list, n);
                return false;
            }
            list = new_list;
        }

        int64_t id = sqlite3_column_int64(stmt, 0);
        const char *p = (const char *)sqlite3_column_text(stmt, 1);
        double freq = sqlite3_column_double(stmt, 2);
        int64_t last_acc = sqlite3_column_int64(stmt, 3);

        list[n].id = id;
        list[n].path = jrun_strdup(p ? p : "");
        list[n].frequency = freq;
        list[n].last_access = last_acc;
        list[n].frecency = db_calculate_frecency(freq, last_acc, now);
        n++;
    }

    if (n > 1) {
        qsort(list, n, sizeof(Db_Entry), compare_entries_desc);
    }

    *entries = list;
    *count = n;
    return true;
}

bool db_get_all(Db_Entry **entries, size_t *count)
{
    if (!g_db || !entries || !count) return false;

    *entries = NULL;
    *count = 0;

    const char *sql = "SELECT id, path, frequency, last_access FROM directories;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        jrun_log_error("sqlite prepare failed: %s", sqlite3_errmsg(g_db));
        return false;
    }

    bool ok = db_load_entries(stmt, entries, count);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_get_top_k(Db_Entry **entries, size_t *count, size_t k)
{
    if (!g_db || !entries || !count || k == 0) return false;

    *entries = NULL;
    *count = 0;

    char sql[128];
    snprintf(sql, sizeof(sql),
        "SELECT id, path, frequency, last_access FROM directories "
        "ORDER BY frequency DESC, last_access DESC LIMIT %zu;", k);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        jrun_log_error("sqlite prepare failed: %s", sqlite3_errmsg(g_db));
        return false;
    }

    bool ok = db_load_entries(stmt, entries, count);
    sqlite3_finalize(stmt);
    return ok;
}

void db_free_entries(Db_Entry *entries, size_t count)
{
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].path);
    }
    free(entries);
}

bool db_prune(size_t *pruned_count)
{
    if (!g_db) return false;
    if (pruned_count) *pruned_count = 0;

    Db_Entry *entries = NULL;
    size_t count = 0;
    if (!db_get_all(&entries, &count)) return false;

    sqlite3_stmt *stmt = NULL;
    const char *del_sql = "DELETE FROM directories WHERE id = ?1;";
    int rc = sqlite3_prepare_v2(g_db, del_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || !stmt) {
        jrun_log_error("failed to prepare prune delete statement: %s", sqlite3_errmsg(g_db));
        db_free_entries(entries, count);
        return false;
    }

    sqlite3_exec(g_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    size_t pruned = 0;
    bool all_ok = true;

    for (size_t i = 0; i < count; ++i) {
        if (!path_is_dir(entries[i].path)) {
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, entries[i].id);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                jrun_log_error("failed to execute prune delete: %s", sqlite3_errmsg(g_db));
                all_ok = false;
                break;
            }
            pruned++;
            jrun_log_debug("pruned non-existent path: %s", entries[i].path);
        }
    }

    sqlite3_finalize(stmt);

    if (all_ok) {
        sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    } else {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        db_free_entries(entries, count);
        return false;
    }

    db_free_entries(entries, count);

    if (pruned_count) *pruned_count = pruned;
    return true;
}

bool db_age_if_needed(void)
{
    if (!g_db) return false;

    const char *sum_sql = "SELECT SUM(frequency) FROM directories;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sum_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    double total_freq = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total_freq = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (total_freq <= 10000.0) return true;

    int64_t now = (int64_t)time(NULL);
    int64_t age_threshold = now - 604800;

    const char *age_sql =
        "UPDATE directories SET frequency = frequency * 0.90 "
        "WHERE last_access < ?1;";
    sqlite3_stmt *age_stmt = NULL;
    if (sqlite3_prepare_v2(g_db, age_sql, -1, &age_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(age_stmt, 1, age_threshold);
        sqlite3_step(age_stmt);
        sqlite3_finalize(age_stmt);
    }

    const char *cleanup_sql = "DELETE FROM directories WHERE frequency < 0.1;";
    sqlite3_exec(g_db, cleanup_sql, NULL, NULL, NULL);

    return true;
}

// --- Cached filesystem index -------------------------------------------------

static bool db_meta_get(const char *key, char *out, size_t out_size)
{
    if (!g_db || !key || !out || out_size == 0) return false;
    out[0] = '\0';

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT value FROM meta WHERE key = ?1;", -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) {
            snprintf(out, out_size, "%s", v);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool db_meta_set(const char *key, const char *value)
{
    if (!g_db || !key || !value) return false;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO meta (key, value) VALUES (?1, ?2) "
                      "ON CONFLICT(key) DO UPDATE SET value = ?2;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_cache_is_fresh(const char *fingerprint, int ttl_seconds)
{
    if (!g_db || !fingerprint || ttl_seconds <= 0) return false;

    char stored_fp[64];
    if (!db_meta_get("scan_fingerprint", stored_fp, sizeof(stored_fp))) return false;
    if (strcmp(stored_fp, fingerprint) != 0) {
        jrun_log_debug("scan cache: configuration changed, rescanning");
        return false;
    }

    char stored_time[32];
    if (!db_meta_get("scan_time", stored_time, sizeof(stored_time))) return false;

    int64_t built = (int64_t)strtoll(stored_time, NULL, 10);
    int64_t age = (int64_t)time(NULL) - built;
    // A clock that moved backwards would otherwise pin the cache as fresh
    // forever, so treat a negative age as stale.
    if (age < 0 || age > (int64_t)ttl_seconds) {
        jrun_log_debug("scan cache: %lld seconds old (ttl %d), rescanning",
                       (long long)age, ttl_seconds);
        return false;
    }

    jrun_log_debug("scan cache: fresh (%lld seconds old)", (long long)age);
    return true;
}

bool db_cache_load(char ***paths, size_t *count)
{
    if (!g_db || !paths || !count) return false;
    *paths = NULL;
    *count = 0;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT path FROM scan_cache;", -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    size_t capacity = 256;
    char **list = (char **)malloc(capacity * sizeof(char *));
    if (!list) {
        sqlite3_finalize(stmt);
        return false;
    }

    size_t n = 0;
    bool ok = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= capacity) {
            size_t new_cap = capacity * 2;
            char **grown = (char **)realloc(list, new_cap * sizeof(char *));
            if (!grown) { ok = false; break; }
            list = grown;
            capacity = new_cap;
        }
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        char *copy = jrun_strdup(p ? p : "");
        if (!copy) { ok = false; break; }
        list[n++] = copy;
    }
    sqlite3_finalize(stmt);

    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(list[i]);
        free(list);
        return false;
    }

    *paths = list;
    *count = n;
    jrun_log_debug("scan cache: loaded %zu paths", n);
    return true;
}

bool db_cache_store(char **paths, size_t count, const char *fingerprint)
{
    if (!g_db || !fingerprint) return false;
    if (count > 0 && !paths) return false;

    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        // Another jrun is already rewriting the cache; its result is just as
        // good as ours, so skip rather than block the user's lookup.
        jrun_log_debug("scan cache: busy, skipping store");
        return false;
    }

    bool ok = (sqlite3_exec(g_db, "DELETE FROM scan_cache;", NULL, NULL, NULL) == SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    if (ok && sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO scan_cache (path) VALUES (?1);",
                                 -1, &stmt, NULL) != SQLITE_OK) {
        ok = false;
    }

    for (size_t i = 0; ok && i < count; ++i) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, paths[i], -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
    }
    if (stmt) sqlite3_finalize(stmt);

    if (ok) {
        char now_buf[32];
        snprintf(now_buf, sizeof(now_buf), "%lld", (long long)time(NULL));
        ok = db_meta_set("scan_fingerprint", fingerprint) && db_meta_set("scan_time", now_buf);
    }

    if (ok) {
        sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
        jrun_log_debug("scan cache: stored %zu paths", count);
    } else {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        jrun_log_debug("scan cache: store failed: %s", sqlite3_errmsg(g_db));
    }
    return ok;
}

bool db_cache_invalidate(void)
{
    if (!g_db) return false;
    sqlite3_exec(g_db, "DELETE FROM scan_cache;", NULL, NULL, NULL);
    return db_meta_set("scan_time", "0");
}

bool db_cache_stats(size_t *count, int64_t *built_at)
{
    if (!g_db) return false;

    if (count) {
        *count = 0;
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM scan_cache;", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                *count = (size_t)sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    if (built_at) {
        char buf[32];
        *built_at = db_meta_get("scan_time", buf, sizeof(buf)) ? (int64_t)strtoll(buf, NULL, 10) : 0;
    }
    return true;
}
