#include "common.h"
#include "config.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

static char *trim_whitespace(char *str)
{
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

#define dup_str jrun_strdup

void config_init_default(Jrun_Config *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->max_depth = DEFAULT_MAX_DEPTH;
    config->follow_symlinks = false;
    config->fuzzy = true;
    config->interactive = true;
    config->frecency_threshold = DEFAULT_FRECENCY_THRESHOLD;

    // Default standard roots
    config_add_root(config, "~/development");
    config_add_root(config, "~/thirdparty");
    config_add_root(config, "~/Documents");
    config_add_root(config, "~/projects");
    config_add_root(config, "~/dotfiles");
}

bool config_has_root(const Jrun_Config *config, const char *root_path)
{
    if (!config || !root_path) return false;
    char norm_input[PATH_MAX];
    path_normalize(root_path, norm_input, sizeof(norm_input));

    for (size_t i = 0; i < config->roots_count; ++i) {
        char norm_root[PATH_MAX];
        path_normalize(config->roots[i], norm_root, sizeof(norm_root));
        if (strcmp(norm_input, norm_root) == 0) {
            return true;
        }
    }
    return false;
}

bool config_add_root(Jrun_Config *config, const char *root_path)
{
    if (!config || !root_path || root_path[0] == '\0') return false;

    if (config_has_root(config, root_path)) {
        return true; // Already exists
    }

    if (config->roots_count >= config->roots_capacity) {
        size_t new_cap = config->roots_capacity == 0 ? 8 : config->roots_capacity * 2;
        char **new_roots = (char **)realloc(config->roots, new_cap * sizeof(char *));
        if (!new_roots) return false;
        config->roots = new_roots;
        config->roots_capacity = new_cap;
    }

    char *copy = dup_str(root_path);
    if (!copy) return false;
    config->roots[config->roots_count++] = copy;
    return true;
}

bool config_remove_root(Jrun_Config *config, const char *root_path)
{
    if (!config || !root_path) return false;

    char norm_input[PATH_MAX];
    path_normalize(root_path, norm_input, sizeof(norm_input));

    for (size_t i = 0; i < config->roots_count; ++i) {
        char norm_root[PATH_MAX];
        path_normalize(config->roots[i], norm_root, sizeof(norm_root));
        if (strcmp(norm_input, norm_root) == 0) {
            free(config->roots[i]);
            for (size_t j = i; j + 1 < config->roots_count; ++j) {
                config->roots[j] = config->roots[j + 1];
            }
            config->roots_count--;
            return true;
        }
    }
    return false;
}

static void parse_string_array(Jrun_Config *config, const char *line, FILE *fp)
{
    char buf[2048];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Check if array closes on the same line or if we need to keep reading
    bool closed = (strchr(buf, ']') != NULL);
    while (!closed && fp) {
        size_t cur_len = strlen(buf);
        if (cur_len + 2 >= sizeof(buf)) break;
        if (!fgets(buf + cur_len, (int)(sizeof(buf) - cur_len), fp)) break;
        if (strchr(buf, ']')) {
            closed = true;
        }
    }

    // Extract quoted strings inside [ ... ]
    char *start = strchr(buf, '[');
    if (!start) return;
    start++;
    char *end = strchr(start, ']');
    if (end) *end = '\0';

    char *p = start;
    while (*p) {
        char *q1 = strchr(p, '"');
        if (!q1) q1 = strchr(p, '\'');
        if (!q1) break;

        char quote_char = *q1;
        char *q2 = strchr(q1 + 1, quote_char);
        if (!q2) break;

        *q2 = '\0';
        char *item = trim_whitespace(q1 + 1);
        if (item[0] != '\0') {
            config_add_root(config, item);
        }
        p = q2 + 1;
    }
}

bool config_load(Jrun_Config *config, const char *filepath)
{
    if (!config) return false;
    memset(config, 0, sizeof(*config));
    config->max_depth = DEFAULT_MAX_DEPTH;
    config->follow_symlinks = false;
    config->fuzzy = true;
    config->interactive = true;
    config->frecency_threshold = DEFAULT_FRECENCY_THRESHOLD;

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        // If file doesn't exist, populate with defaults
        config_init_default(config);
        return true;
    }

    bool roots_specified = false;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        // Section headers like [search] or [behavior]
        if (trimmed[0] == '[') {
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcmp(key, "roots") == 0) {
            if (!roots_specified) {
                // Clear existing roots since user specified custom ones
                for (size_t i = 0; i < config->roots_count; ++i) {
                    free(config->roots[i]);
                }
                config->roots_count = 0;
                roots_specified = true;
            }
            parse_string_array(config, val, fp);
        } else if (strcmp(key, "max_depth") == 0) {
            int d = atoi(val);
            if (d > 0 && d <= 32) config->max_depth = d;
        } else if (strcmp(key, "follow_symlinks") == 0) {
            config->follow_symlinks = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "fuzzy") == 0) {
            config->fuzzy = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "interactive") == 0) {
            config->interactive = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "frecency_threshold") == 0) {
            double t = atof(val);
            if (t > 0.0) config->frecency_threshold = t;
        }
    }

    fclose(fp);

    if (!roots_specified && config->roots_count == 0) {
        config_add_root(config, "~/development");
        config_add_root(config, "~/thirdparty");
        config_add_root(config, "~/Documents");
        config_add_root(config, "~/projects");
        config_add_root(config, "~/dotfiles");
    }

    return true;
}

bool config_save(const Jrun_Config *config, const char *filepath)
{
    if (!config || !filepath) return false;

    if (!path_ensure_parent_dir(filepath)) {
        return false;
    }

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        jrun_log_error("failed to open config file '%s' for writing: %s", filepath, strerror(errno));
        return false;
    }

    fprintf(fp, "# Jrun configuration file\n");
    fprintf(fp, "# Created automatically by jrun\n\n");

    fprintf(fp, "[search]\n");
    fprintf(fp, "roots = [\n");
    for (size_t i = 0; i < config->roots_count; ++i) {
        fprintf(fp, "    \"%s\"%s\n", config->roots[i], (i + 1 < config->roots_count) ? "," : "");
    }
    fprintf(fp, "]\n");
    fprintf(fp, "max_depth = %d\n", config->max_depth);
    fprintf(fp, "follow_symlinks = %s\n\n", config->follow_symlinks ? "true" : "false");

    fprintf(fp, "[behavior]\n");
    fprintf(fp, "fuzzy = %s\n", config->fuzzy ? "true" : "false");
    fprintf(fp, "interactive = %s\n", config->interactive ? "true" : "false");
    fprintf(fp, "frecency_threshold = %.2f\n", config->frecency_threshold);

    fclose(fp);
    return true;
}

void config_print(const Jrun_Config *config)
{
    if (!config) return;
    printf("[search]\n");
    printf("roots = [\n");
    for (size_t i = 0; i < config->roots_count; ++i) {
        printf("    \"%s\"%s\n", config->roots[i], (i + 1 < config->roots_count) ? "," : "");
    }
    printf("]\n");
    printf("max_depth = %d\n", config->max_depth);
    printf("follow_symlinks = %s\n\n", config->follow_symlinks ? "true" : "false");

    printf("[behavior]\n");
    printf("fuzzy = %s\n", config->fuzzy ? "true" : "false");
    printf("interactive = %s\n", config->interactive ? "true" : "false");
    printf("frecency_threshold = %.2f\n", config->frecency_threshold);
}

void config_free(Jrun_Config *config)
{
    if (!config) return;
    for (size_t i = 0; i < config->roots_count; ++i) {
        free(config->roots[i]);
    }
    free(config->roots);
    config->roots = NULL;
    config->roots_count = 0;
    config->roots_capacity = 0;
}
