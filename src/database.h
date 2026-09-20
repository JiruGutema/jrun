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

#endif // JRUN_DATABASE_H_
