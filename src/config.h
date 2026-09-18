#ifndef JRUN_CONFIG_H_
#define JRUN_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

#define DEFAULT_MAX_DEPTH 4
#define DEFAULT_FRECENCY_THRESHOLD 2.0

typedef struct {
    char **roots;
    size_t roots_count;
    size_t roots_capacity;
    int max_depth;
    bool follow_symlinks;
    bool fuzzy;
    bool interactive;
    double frecency_threshold;
} Jrun_Config;

void config_init_default(Jrun_Config *config);
bool config_load(Jrun_Config *config, const char *filepath);
bool config_save(const Jrun_Config *config, const char *filepath);
bool config_add_root(Jrun_Config *config, const char *root_path);
bool config_remove_root(Jrun_Config *config, const char *root_path);
bool config_has_root(const Jrun_Config *config, const char *root_path);
void config_print(const Jrun_Config *config);
void config_free(Jrun_Config *config);

#endif // JRUN_CONFIG_H_
