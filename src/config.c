#include "common.h"
#include "config.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

static const char *DEFAULT_CONFIRM_COMMANDS[] = {
    "rm",
    "rmdir",
    "mv",
    "unlink",
    "shred",
    "dd",
    "mkfs",
    "wipefs",
    "chmod",
    "chown",
    "truncate",
    NULL
};

static char *trim_whitespace(char *str)
{
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void strip_unquoted_comment(char *str)
{
    bool in_squote = false;
    bool in_dquote = false;
    for (char *p = str; *p; ++p) {
        if (*p == '\'' && !in_dquote) in_squote = !in_squote;
        else if (*p == '"' && !in_squote) in_dquote = !in_dquote;
        else if (*p == '#' && !in_squote && !in_dquote) {
            *p = '\0';
            return;
        }
    }
}

static bool parse_bool(const char *val, bool *out)
{
    if (!val || !out) return false;
    if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

#define dup_str jrun_strdup

static bool da_add_unique(char ***arr, size_t *count, size_t *cap, const char *item)
{
    if (!arr || !item || item[0] == '\0') return false;
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp((*arr)[i], item) == 0) return true;
    }
    if (*count >= *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        char **grown = (char **)realloc(*arr, new_cap * sizeof(char *));
        if (!grown) return false;
        *arr = grown;
        *cap = new_cap;
    }
    char *copy = dup_str(item);
    if (!copy) return false;
    (*arr)[(*count)++] = copy;
    return true;
}

static void da_clear(char **arr, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(arr[i]);
    }
}

static void add_default_confirm_commands(Jrun_Config *config)
{
    for (size_t i = 0; DEFAULT_CONFIRM_COMMANDS[i] != NULL; ++i) {
        config_add_confirm_command(config, DEFAULT_CONFIRM_COMMANDS[i]);
    }
}

void config_init_default(Jrun_Config *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->max_depth = DEFAULT_MAX_DEPTH;
    config->follow_symlinks = false;
    config->fuzzy = true;
    config->interactive = true;
    config->frecency_threshold = DEFAULT_FRECENCY_THRESHOLD;
    config->confirm_destructive = true;

    config_add_root(config, "~/development");
    config_add_root(config, "~/thirdparty");
    config_add_root(config, "~/Documents");
    config_add_root(config, "~/projects");
    config_add_root(config, "~/dotfiles");
    add_default_confirm_commands(config);
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
        return true;
    }
    return da_add_unique(&config->roots, &config->roots_count, &config->roots_capacity, root_path);
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

bool config_remove_root_at(Jrun_Config *config, size_t index)
{
    if (!config || index >= config->roots_count) return false;
    free(config->roots[index]);
    for (size_t j = index; j + 1 < config->roots_count; ++j) {
        config->roots[j] = config->roots[j + 1];
    }
    config->roots_count--;
    return true;
}

bool config_add_confirm_command(Jrun_Config *config, const char *command)
{
    if (!config || !command || command[0] == '\0') return false;
    const char *base = path_basename(command);
    if (!base || base[0] == '\0') base = command;
    return da_add_unique(&config->confirm_commands, &config->confirm_commands_count,
                         &config->confirm_commands_capacity, base);
}

bool config_remove_confirm_command(Jrun_Config *config, const char *command)
{
    if (!config || !command) return false;
    const char *base = path_basename(command);
    if (!base || base[0] == '\0') base = command;
    for (size_t i = 0; i < config->confirm_commands_count; ++i) {
        if (strcmp(config->confirm_commands[i], base) == 0) {
            return config_remove_confirm_command_at(config, i);
        }
    }
    return false;
}

bool config_remove_confirm_command_at(Jrun_Config *config, size_t index)
{
    if (!config || index >= config->confirm_commands_count) return false;
    free(config->confirm_commands[index]);
    for (size_t j = index; j + 1 < config->confirm_commands_count; ++j) {
        config->confirm_commands[j] = config->confirm_commands[j + 1];
    }
    config->confirm_commands_count--;
    return true;
}

bool config_needs_confirm(const Jrun_Config *config, const char *argv0)
{
    if (!config || !config->confirm_destructive || !argv0 || argv0[0] == '\0') {
        return false;
    }
    const char *base = path_basename(argv0);
    if (!base || base[0] == '\0') base = argv0;
    for (size_t i = 0; i < config->confirm_commands_count; ++i) {
        if (strcmp(config->confirm_commands[i], base) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_string_array(const char *line, FILE *fp,
                               bool (*add_fn)(Jrun_Config *, const char *),
                               Jrun_Config *config)
{
    char buf[4096];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    bool closed = (strchr(buf, ']') != NULL);
    while (!closed && fp) {
        size_t cur_len = strlen(buf);
        if (cur_len + 2 >= sizeof(buf)) break;
        if (!fgets(buf + cur_len, (int)(sizeof(buf) - cur_len), fp)) break;
        if (strchr(buf, ']')) {
            closed = true;
        }
    }

    char *start = strchr(buf, '[');
    if (!start) return;
    start++;
    char *end = strrchr(start, ']');
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
            add_fn(config, item);
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
    config->confirm_destructive = true;

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        config_init_default(config);
        return true;
    }

    enum {
        SEC_NONE,
        SEC_SEARCH,
        SEC_BEHAVIOR,
        SEC_SAFETY
    } section = SEC_NONE;

    bool roots_specified = false;
    bool commands_specified = false;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        if (trimmed[0] == '[') {
            char *close = strchr(trimmed, ']');
            if (close) *close = '\0';
            const char *name = trim_whitespace(trimmed + 1);
            if (strcmp(name, "search") == 0) section = SEC_SEARCH;
            else if (strcmp(name, "behavior") == 0) section = SEC_BEHAVIOR;
            else if (strcmp(name, "safety") == 0) section = SEC_SAFETY;
            else section = SEC_NONE;
            continue;
        }

        strip_unquoted_comment(trimmed);
        trimmed = trim_whitespace(trimmed);

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcmp(key, "roots") == 0) {
            if (!roots_specified) {
                da_clear(config->roots, config->roots_count);
                config->roots_count = 0;
                roots_specified = true;
            }
            parse_string_array(val, fp, config_add_root, config);
        } else if (strcmp(key, "commands") == 0 || strcmp(key, "confirm_commands") == 0) {
            if (!commands_specified) {
                da_clear(config->confirm_commands, config->confirm_commands_count);
                config->confirm_commands_count = 0;
                commands_specified = true;
            }
            parse_string_array(val, fp, config_add_confirm_command, config);
        } else if (strcmp(key, "max_depth") == 0) {
            int d = atoi(val);
            if (d > 0 && d <= 32) config->max_depth = d;
        } else if (strcmp(key, "follow_symlinks") == 0) {
            parse_bool(val, &config->follow_symlinks);
        } else if (strcmp(key, "fuzzy") == 0) {
            parse_bool(val, &config->fuzzy);
        } else if (strcmp(key, "interactive") == 0) {
            parse_bool(val, &config->interactive);
        } else if (strcmp(key, "frecency_threshold") == 0) {
            double t = atof(val);
            if (t > 0.0) config->frecency_threshold = t;
        } else if (strcmp(key, "confirm") == 0 || strcmp(key, "confirm_destructive") == 0) {
            parse_bool(val, &config->confirm_destructive);
        } else {
            (void)section;
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

    if (!commands_specified) {
        add_default_confirm_commands(config);
    }

    return true;
}

static void write_quoted_array(FILE *fp, const char *key, char **items, size_t count)
{
    fprintf(fp, "%s = [\n", key);
    for (size_t i = 0; i < count; ++i) {
        fprintf(fp, "    \"%s\"%s\n", items[i], (i + 1 < count) ? "," : "");
    }
    fprintf(fp, "]\n");
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
    write_quoted_array(fp, "roots", config->roots, config->roots_count);
    fprintf(fp, "max_depth = %d\n", config->max_depth);
    fprintf(fp, "follow_symlinks = %s\n\n", config->follow_symlinks ? "true" : "false");

    fprintf(fp, "[behavior]\n");
    fprintf(fp, "fuzzy = %s\n", config->fuzzy ? "true" : "false");
    fprintf(fp, "interactive = %s\n", config->interactive ? "true" : "false");
    fprintf(fp, "frecency_threshold = %.2f\n\n", config->frecency_threshold);

    fprintf(fp, "[safety]\n");
    fprintf(fp, "# Prompt before running these commands in a resolved directory.\n");
    fprintf(fp, "# Set confirm = false to disable prompts, or pass -y / --yes.\n");
    fprintf(fp, "confirm = %s\n", config->confirm_destructive ? "true" : "false");
    write_quoted_array(fp, "commands", config->confirm_commands, config->confirm_commands_count);

    fclose(fp);
    return true;
}

void config_print(const Jrun_Config *config)
{
    if (!config) return;
    printf("[search]\n");
    write_quoted_array(stdout, "roots", config->roots, config->roots_count);
    printf("max_depth = %d\n", config->max_depth);
    printf("follow_symlinks = %s\n\n", config->follow_symlinks ? "true" : "false");

    printf("[behavior]\n");
    printf("fuzzy = %s\n", config->fuzzy ? "true" : "false");
    printf("interactive = %s\n", config->interactive ? "true" : "false");
    printf("frecency_threshold = %.2f\n\n", config->frecency_threshold);

    printf("[safety]\n");
    printf("confirm = %s\n", config->confirm_destructive ? "true" : "false");
    write_quoted_array(stdout, "commands", config->confirm_commands, config->confirm_commands_count);
}

void config_free(Jrun_Config *config)
{
    if (!config) return;
    da_clear(config->roots, config->roots_count);
    free(config->roots);
    da_clear(config->confirm_commands, config->confirm_commands_count);
    free(config->confirm_commands);
    memset(config, 0, sizeof(*config));
}
