#include "common.h"
#include "config.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

static const char *DEFAULT_ROOTS[] = {
    "~/development",
    "~/thirdparty",
    "~/Documents",
    "~/projects",
    "~/dotfiles",
    NULL
};

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

static const char *DEFAULT_IGNORE[] = {
    ".git",
    ".hg",
    ".svn",
    "node_modules",
    "__pycache__",
    ".cache",
    ".cargo",
    ".rustup",
    ".npm",
    ".venv",
    "vendor",
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
    bool escaped = false;
    for (char *p = str; *p; ++p) {
        if (escaped) { escaped = false; continue; }
        if (*p == '\\' && in_dquote) escaped = true;
        else if (*p == '\'' && !in_dquote) in_squote = !in_squote;
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

static bool da_remove_at(char **arr, size_t *count, size_t index)
{
    if (index >= *count) return false;
    free(arr[index]);
    for (size_t j = index; j + 1 < *count; ++j) {
        arr[j] = arr[j + 1];
    }
    (*count)--;
    return true;
}

static void add_defaults_from(const char *const *list, Jrun_Config *config,
                              bool (*add_fn)(Jrun_Config *, const char *))
{
    for (size_t i = 0; list[i] != NULL; ++i) {
        add_fn(config, list[i]);
    }
}

static void config_set_builtin_defaults(Jrun_Config *config)
{
    memset(config, 0, sizeof(*config));
    config->max_depth = DEFAULT_MAX_DEPTH;
    config->follow_symlinks = false;
    config->fuzzy = true;
    config->interactive = true;
    config->prefer_projects = true;
    config->frecency_threshold = DEFAULT_FRECENCY_THRESHOLD;
    config->cache_ttl = DEFAULT_CACHE_TTL;
    config->confirm_destructive = true;
}

void config_init_default(Jrun_Config *config)
{
    if (!config) return;
    config_set_builtin_defaults(config);
    add_defaults_from(DEFAULT_ROOTS, config, config_add_root);
    add_defaults_from(DEFAULT_IGNORE, config, config_add_ignore);
    add_defaults_from(DEFAULT_CONFIRM_COMMANDS, config, config_add_confirm_command);
}

// Normalizing can fail (getcwd failure, over-long path). Fall back to the
// literal text rather than comparing against an uninitialised buffer.
static void normalize_or_copy(const char *path, char *out, size_t out_size)
{
    if (path_normalize(path, out, out_size)) return;
    snprintf(out, out_size, "%s", path ? path : "");
}

bool config_has_root(const Jrun_Config *config, const char *root_path)
{
    if (!config || !root_path) return false;
    char norm_input[PATH_MAX];
    normalize_or_copy(root_path, norm_input, sizeof(norm_input));

    for (size_t i = 0; i < config->roots_count; ++i) {
        char norm_root[PATH_MAX];
        normalize_or_copy(config->roots[i], norm_root, sizeof(norm_root));
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
    normalize_or_copy(root_path, norm_input, sizeof(norm_input));

    for (size_t i = 0; i < config->roots_count; ++i) {
        char norm_root[PATH_MAX];
        normalize_or_copy(config->roots[i], norm_root, sizeof(norm_root));
        if (strcmp(norm_input, norm_root) == 0) {
            return da_remove_at(config->roots, &config->roots_count, i);
        }
    }
    return false;
}

bool config_remove_root_at(Jrun_Config *config, size_t index)
{
    if (!config) return false;
    return da_remove_at(config->roots, &config->roots_count, index);
}

bool config_add_ignore(Jrun_Config *config, const char *name)
{
    if (!config || !name || name[0] == '\0') return false;
    // Ignore entries are matched against single path components, so a caller
    // passing a path means a whole subtree; keep only the final component.
    char bname[PATH_MAX];
    const char *base = path_basename_r(name, bname, sizeof(bname));
    if (base[0] == '\0') base = name;
    return da_add_unique(&config->ignore, &config->ignore_count, &config->ignore_capacity, base);
}

bool config_remove_ignore_at(Jrun_Config *config, size_t index)
{
    if (!config) return false;
    return da_remove_at(config->ignore, &config->ignore_count, index);
}

bool config_is_ignored(const Jrun_Config *config, const char *name)
{
    if (!config || !name) return false;
    for (size_t i = 0; i < config->ignore_count; ++i) {
        if (strcmp(config->ignore[i], name) == 0) return true;
    }
    return false;
}

bool config_add_confirm_command(Jrun_Config *config, const char *command)
{
    if (!config || !command || command[0] == '\0') return false;
    char bname[PATH_MAX];
    const char *base = path_basename_r(command, bname, sizeof(bname));
    if (base[0] == '\0') base = command;
    return da_add_unique(&config->confirm_commands, &config->confirm_commands_count,
                         &config->confirm_commands_capacity, base);
}

bool config_remove_confirm_command(Jrun_Config *config, const char *command)
{
    if (!config || !command) return false;
    char bname[PATH_MAX];
    const char *base = path_basename_r(command, bname, sizeof(bname));
    if (base[0] == '\0') base = command;
    for (size_t i = 0; i < config->confirm_commands_count; ++i) {
        if (strcmp(config->confirm_commands[i], base) == 0) {
            return config_remove_confirm_command_at(config, i);
        }
    }
    return false;
}

bool config_remove_confirm_command_at(Jrun_Config *config, size_t index)
{
    if (!config) return false;
    return da_remove_at(config->confirm_commands, &config->confirm_commands_count, index);
}

bool config_needs_confirm(const Jrun_Config *config, const char *argv0)
{
    if (!config || !config->confirm_destructive || !argv0 || argv0[0] == '\0') {
        return false;
    }
    char bname[PATH_MAX];
    const char *base = path_basename_r(argv0, bname, sizeof(bname));
    if (base[0] == '\0') base = argv0;
    for (size_t i = 0; i < config->confirm_commands_count; ++i) {
        if (strcmp(config->confirm_commands[i], base) == 0) {
            return true;
        }
    }
    return false;
}

void config_scan_fingerprint(const Jrun_Config *config, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!config) { out[0] = '\0'; return; }

    // FNV-1a over everything a scan depends on. A digest keeps the value short
    // enough to store as a single row, and collisions only cost a rescan.
    uint64_t h = 14695981039346656037ULL;
    #define FP_MIX(str) do { \
        for (const char *_p = (str); *_p; ++_p) { \
            h ^= (uint8_t)*_p; h *= 1099511628211ULL; \
        } \
        h ^= 0xff; h *= 1099511628211ULL; \
    } while (0)

    for (size_t i = 0; i < config->roots_count; ++i) FP_MIX(config->roots[i]);
    for (size_t i = 0; i < config->ignore_count; ++i) FP_MIX(config->ignore[i]);

    char scalars[64];
    snprintf(scalars, sizeof(scalars), "d%d|s%d", config->max_depth, config->follow_symlinks ? 1 : 0);
    FP_MIX(scalars);
    #undef FP_MIX

    snprintf(out, out_size, "%016llx", (unsigned long long)h);
}

static void parse_string_array(const char *line, FILE *fp,
                               bool (*add_fn)(Jrun_Config *, const char *),
                               Jrun_Config *config)
{
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s", line);

    bool closed = (strchr(buf, ']') != NULL);
    while (!closed && fp) {
        size_t cur_len = strlen(buf);
        if (cur_len + 2 >= sizeof(buf)) break;
        char *tail = buf + cur_len;
        if (!fgets(tail, (int)(sizeof(buf) - cur_len), fp)) break;
        // Continuation lines get the same comment handling as the first line,
        // otherwise a "# see the \"docs\"" comment injects bogus entries.
        strip_unquoted_comment(tail);
        if (strchr(tail, ']')) {
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
        char *s1 = strchr(p, '\'');
        if (!q1 || (s1 && s1 < q1)) q1 = s1;
        if (!q1) break;

        char quote_char = *q1;
        char item[PATH_MAX];
        size_t n = 0;
        char *r = q1 + 1;
        bool terminated = false;
        while (*r) {
            if (*r == '\\' && quote_char == '"' && r[1]) {
                // TOML basic-string escapes, mirroring what config_save writes.
                r++;
                char c = *r;
                switch (c) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '0':  c = '\0'; break;
                default: break;
                }
                if (n + 1 < sizeof(item)) item[n++] = c;
                r++;
                continue;
            }
            if (*r == quote_char) { terminated = true; break; }
            if (n + 1 < sizeof(item)) item[n++] = *r;
            r++;
        }
        item[n] = '\0';
        if (!terminated) break;

        char *trimmed = trim_whitespace(item);
        if (trimmed[0] != '\0') {
            add_fn(config, trimmed);
        }
        p = r + 1;
    }
}

typedef enum {
    SEC_NONE = 0,
    SEC_SEARCH,
    SEC_BEHAVIOR,
    SEC_SAFETY,
    SEC_UNKNOWN
} Toml_Section;

// A key is honoured when it appears in its own section, or at top level (no
// section header yet) so hand-written flat configs keep working.
static bool key_in_section(Toml_Section actual, Toml_Section expected)
{
    return actual == expected || actual == SEC_NONE;
}

bool config_load(Jrun_Config *config, const char *filepath)
{
    if (!config) return false;
    config_set_builtin_defaults(config);

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        config_init_default(config);
        return true;
    }

    Toml_Section section = SEC_NONE;
    bool roots_specified = false;
    bool ignore_specified = false;
    bool commands_specified = false;
    char line[4096];

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
            else {
                section = SEC_UNKNOWN;
                jrun_log_debug("config: ignoring unknown section [%s]", name);
            }
            continue;
        }

        strip_unquoted_comment(trimmed);
        trimmed = trim_whitespace(trimmed);

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcmp(key, "roots") == 0 && key_in_section(section, SEC_SEARCH)) {
            if (!roots_specified) {
                da_clear(config->roots, config->roots_count);
                config->roots_count = 0;
                roots_specified = true;
            }
            parse_string_array(val, fp, config_add_root, config);
        } else if (strcmp(key, "ignore") == 0 && key_in_section(section, SEC_SEARCH)) {
            if (!ignore_specified) {
                da_clear(config->ignore, config->ignore_count);
                config->ignore_count = 0;
                ignore_specified = true;
            }
            parse_string_array(val, fp, config_add_ignore, config);
        } else if ((strcmp(key, "commands") == 0 || strcmp(key, "confirm_commands") == 0) &&
                   key_in_section(section, SEC_SAFETY)) {
            if (!commands_specified) {
                da_clear(config->confirm_commands, config->confirm_commands_count);
                config->confirm_commands_count = 0;
                commands_specified = true;
            }
            parse_string_array(val, fp, config_add_confirm_command, config);
        } else if (strcmp(key, "max_depth") == 0 && key_in_section(section, SEC_SEARCH)) {
            int d = atoi(val);
            if (d > 0 && d <= 32) config->max_depth = d;
            else jrun_log_debug("config: max_depth out of range (1-32): %s", val);
        } else if (strcmp(key, "follow_symlinks") == 0 && key_in_section(section, SEC_SEARCH)) {
            parse_bool(val, &config->follow_symlinks);
        } else if (strcmp(key, "cache_ttl") == 0 && key_in_section(section, SEC_SEARCH)) {
            int t = atoi(val);
            if (t >= 0) config->cache_ttl = t;
        } else if (strcmp(key, "fuzzy") == 0 && key_in_section(section, SEC_BEHAVIOR)) {
            parse_bool(val, &config->fuzzy);
        } else if (strcmp(key, "interactive") == 0 && key_in_section(section, SEC_BEHAVIOR)) {
            parse_bool(val, &config->interactive);
        } else if (strcmp(key, "prefer_projects") == 0 && key_in_section(section, SEC_BEHAVIOR)) {
            parse_bool(val, &config->prefer_projects);
        } else if (strcmp(key, "frecency_threshold") == 0 && key_in_section(section, SEC_BEHAVIOR)) {
            double t = atof(val);
            if (t > 0.0) config->frecency_threshold = t;
        } else if ((strcmp(key, "confirm") == 0 || strcmp(key, "confirm_destructive") == 0) &&
                   key_in_section(section, SEC_SAFETY)) {
            parse_bool(val, &config->confirm_destructive);
        } else {
            jrun_log_debug("config: ignoring unrecognised key '%s'", key);
        }
    }

    fclose(fp);

    if (!roots_specified && config->roots_count == 0) {
        add_defaults_from(DEFAULT_ROOTS, config, config_add_root);
    }
    if (!ignore_specified) {
        add_defaults_from(DEFAULT_IGNORE, config, config_add_ignore);
    }
    if (!commands_specified) {
        add_defaults_from(DEFAULT_CONFIRM_COMMANDS, config, config_add_confirm_command);
    }

    return true;
}

// Growable output buffer so config_save can render the whole file before
// touching the filesystem, which is what makes the write atomic.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} Str_Builder;

static void sb_ensure(Str_Builder *sb, size_t extra)
{
    if (sb->failed) return;
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t new_cap = sb->cap ? sb->cap : 1024;
    while (new_cap < sb->len + extra + 1) new_cap *= 2;
    char *grown = (char *)realloc(sb->data, new_cap);
    if (!grown) { sb->failed = true; return; }
    sb->data = grown;
    sb->cap = new_cap;
}

static void sb_append(Str_Builder *sb, const char *str)
{
    size_t len = strlen(str);
    sb_ensure(sb, len);
    if (sb->failed) return;
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(Str_Builder *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char tmp[PATH_MAX + 256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0) { sb->failed = true; return; }
    sb_append(sb, tmp);
}

// TOML basic-string escaping. Without this a path containing a quote produces
// a file that does not parse back to what was written.
static void sb_append_quoted(Str_Builder *sb, const char *str)
{
    sb_append(sb, "\"");
    for (const char *p = str; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n");  break;
        case '\t': sb_append(sb, "\\t");  break;
        case '\r': sb_append(sb, "\\r");  break;
        default:
            if (c < 0x20 || c == 0x7f) {
                sb_appendf(sb, "\\u%04x", c);
            } else {
                char one[2] = { (char)c, '\0' };
                sb_append(sb, one);
            }
            break;
        }
    }
    sb_append(sb, "\"");
}

static void sb_append_array(Str_Builder *sb, const char *key, char **items, size_t count)
{
    if (count == 0) {
        sb_appendf(sb, "%s = []\n", key);
        return;
    }
    sb_appendf(sb, "%s = [\n", key);
    for (size_t i = 0; i < count; ++i) {
        sb_append(sb, "    ");
        sb_append_quoted(sb, items[i]);
        sb_append(sb, (i + 1 < count) ? ",\n" : "\n");
    }
    sb_append(sb, "]\n");
}

static void config_render(const Jrun_Config *config, Str_Builder *sb)
{
    sb_append(sb, "# jrun configuration file\n");
    sb_append(sb, "# Written by jrun; hand edits are preserved in meaning, not in layout.\n\n");

    sb_append(sb, "[search]\n");
    sb_append_array(sb, "roots", config->roots, config->roots_count);
    sb_append(sb, "\n# Directory names never descended into.\n");
    sb_append_array(sb, "ignore", config->ignore, config->ignore_count);
    sb_appendf(sb, "max_depth = %d\n", config->max_depth);
    sb_appendf(sb, "follow_symlinks = %s\n", config->follow_symlinks ? "true" : "false");
    sb_append(sb, "# Seconds a cached directory index stays usable. 0 rescans every time.\n");
    sb_appendf(sb, "cache_ttl = %d\n\n", config->cache_ttl);

    sb_append(sb, "[behavior]\n");
    sb_appendf(sb, "fuzzy = %s\n", config->fuzzy ? "true" : "false");
    sb_appendf(sb, "interactive = %s\n", config->interactive ? "true" : "false");
    sb_append(sb, "# Rank directories holding a project marker (.git, Cargo.toml, ...) higher.\n");
    sb_appendf(sb, "prefer_projects = %s\n", config->prefer_projects ? "true" : "false");
    sb_appendf(sb, "frecency_threshold = %.2f\n\n", config->frecency_threshold);

    sb_append(sb, "[safety]\n");
    sb_append(sb, "# Prompt before running these commands in a resolved directory.\n");
    sb_append(sb, "# Set confirm = false to disable prompts, or pass -y / --yes.\n");
    sb_appendf(sb, "confirm = %s\n", config->confirm_destructive ? "true" : "false");
    sb_append_array(sb, "commands", config->confirm_commands, config->confirm_commands_count);
}

bool config_save(const Jrun_Config *config, const char *filepath)
{
    if (!config || !filepath) return false;
    if (!path_ensure_parent_dir(filepath)) return false;

    Str_Builder sb = {0};
    config_render(config, &sb);
    if (sb.failed || !sb.data) {
        free(sb.data);
        jrun_log_error("failed to render configuration (out of memory)");
        return false;
    }

    bool ok = path_write_atomic(filepath, sb.data, sb.len);
    free(sb.data);
    if (!ok) {
        jrun_log_error("failed to write config file '%s': %s", filepath, strerror(errno));
    }
    return ok;
}

void config_print(const Jrun_Config *config)
{
    if (!config) return;
    Str_Builder sb = {0};
    config_render(config, &sb);
    if (!sb.failed && sb.data) {
        fwrite(sb.data, 1, sb.len, stdout);
    }
    free(sb.data);
}

void config_free(Jrun_Config *config)
{
    if (!config) return;
    da_clear(config->roots, config->roots_count);
    free(config->roots);
    da_clear(config->ignore, config->ignore_count);
    free(config->ignore);
    da_clear(config->confirm_commands, config->confirm_commands_count);
    free(config->confirm_commands);
    memset(config, 0, sizeof(*config));
}
