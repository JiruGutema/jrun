#include "database.h"
#include "common.h"
#include "path_util.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

static sqlite3 *g_db = NULL;

double db_calculate_frecency(double frequency, int64_t last_access, int64_t current_time)
{
    if (frequency <= 0.0) frequency = 1.0;
    int64_t delta = current_time - last_access;
    if (delta < 0) delta = 0;

    double recency_weight;
    if (delta < 3600) {            
        recency_weight = 4.0;
    } else if (delta < 86400) {    
        recency_weight = 2.0;
    } else if (delta < 604800) {   
        recency_weight = 1.0;
    } else if (delta < 2592000) {  
        recency_weight = 0.5;
    } else {                       
        recency_weight = 0.25;
    }

    return frequency * recency_weight;
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
        "CREATE INDEX IF NOT EXISTS idx_last_access ON directories(last_access);";

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

    size_t capacity = 16;
    Db_Entry *list = (Db_Entry *)malloc(capacity * sizeof(Db_Entry));
    if (!list) {
        sqlite3_finalize(stmt);
        return false;
    }

    int64_t now = (int64_t)time(NULL);
    size_t n = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= capacity) {
            capacity *= 2;
            Db_Entry *new_list = (Db_Entry *)realloc(list, capacity * sizeof(Db_Entry));
            if (!new_list) {
                db_free_entries(list, n);
                sqlite3_finalize(stmt);
                return false;
            }
            list = new_list;
        }

        int64_t id = sqlite3_column_int64(stmt, 0);
        const char *p = (const char *)sqlite3_column_text(stmt, 1);
        double freq = sqlite3_column_double(stmt, 2);
        int64_t last_acc = sqlite3_column_int64(stmt, 3);

        list[n].id = id;
        list[n].path = strdup(p ? p : "");
        list[n].frequency = freq;
        list[n].last_access = last_acc;
        list[n].frecency = db_calculate_frecency(freq, last_acc, now);
        n++;
    }

    sqlite3_finalize(stmt);

    if (n > 1) {
        qsort(list, n, sizeof(Db_Entry), compare_entries_desc);
    }

    *entries = list;
    *count = n;
    return true;
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

    // If total frequency exceeds 10,000, scale down all frequencies by 0.90
    if (total_freq > 10000.0) {
        const char *age_sql = "UPDATE directories SET frequency = frequency * 0.90;";
        sqlite3_exec(g_db, age_sql, NULL, NULL, NULL);
        // Also delete entries that drop below 0.1
        const char *cleanup_sql = "DELETE FROM directories WHERE frequency < 0.1;";
        sqlite3_exec(g_db, cleanup_sql, NULL, NULL, NULL);
    }

    return true;
}
