#ifndef JRUN_CONFIG_H_
#define JRUN_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

#define DEFAULT_MAX_DEPTH 4
#define DEFAULT_FRECENCY_THRESHOLD 2.0
// How long a cached filesystem index stays usable before jrun rescans.
// Zero disables the cache and forces a scan on every lookup.
#define DEFAULT_CACHE_TTL 300

typedef struct {
    char **roots;
    size_t roots_count;
    size_t roots_capacity;

    // Directory names never descended into, e.g. "node_modules".
    char **ignore;
    size_t ignore_count;
    size_t ignore_capacity;

    int max_depth;
    bool follow_symlinks;
    bool fuzzy;
    bool interactive;
    bool prefer_projects;
    double frecency_threshold;
    int cache_ttl;

    bool confirm_destructive;
    char **confirm_commands;
    size_t confirm_commands_count;
    size_t confirm_commands_capacity;
} Jrun_Config;

void config_init_default(Jrun_Config *config);
bool config_load(Jrun_Config *config, const char *filepath);
bool config_save(const Jrun_Config *config, const char *filepath);

bool config_add_root(Jrun_Config *config, const char *root_path);
bool config_remove_root(Jrun_Config *config, const char *root_path);
bool config_has_root(const Jrun_Config *config, const char *root_path);
bool config_remove_root_at(Jrun_Config *config, size_t index);

bool config_add_ignore(Jrun_Config *config, const char *name);
bool config_remove_ignore_at(Jrun_Config *config, size_t index);
bool config_is_ignored(const Jrun_Config *config, const char *name);

bool config_add_confirm_command(Jrun_Config *config, const char *command);
bool config_remove_confirm_command(Jrun_Config *config, const char *command);
bool config_remove_confirm_command_at(Jrun_Config *config, size_t index);
bool config_needs_confirm(const Jrun_Config *config, const char *argv0);

// Stable digest of everything that affects which paths a scan produces. The
// cached index is discarded whenever this changes.
void config_scan_fingerprint(const Jrun_Config *config, char *out, size_t out_size);

void config_print(const Jrun_Config *config);
void config_free(Jrun_Config *config);

#endif // JRUN_CONFIG_H_
