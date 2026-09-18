#include "path_util.h"
#include "common.h"

#include <pwd.h>
#include <limits.h>

static const char *get_home_directory(void)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        return home;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return pw->pw_dir;
    }
    return NULL;
}

bool path_expand_tilde(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return false;

    if (path[0] == '~') {
        const char *home = get_home_directory();
        if (!home) return false;

        if (path[1] == '\0') {
            size_t len = strlen(home);
            if (len >= out_size) return false;
            strncpy(out, home, out_size - 1);
            out[out_size - 1] = '\0';
            return true;
        } else if (path[1] == '/') {
            int n = snprintf(out, out_size, "%s%s", home, path + 1);
            return (n > 0 && (size_t)n < out_size);
        }
    }

    size_t len = strlen(path);
    if (len >= out_size) return false;
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

bool path_shorten_tilde(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return false;

    const char *home = get_home_directory();
    if (home) {
        size_t home_len = strlen(home);
        if (strncmp(path, home, home_len) == 0) {
            if (path[home_len] == '\0') {
                if (out_size < 2) return false;
                out[0] = '~';
                out[1] = '\0';
                return true;
            } else if (path[home_len] == '/') {
                int n = snprintf(out, out_size, "~%s", path + home_len);
                return (n > 0 && (size_t)n < out_size);
            }
        }
    }

    size_t len = strlen(path);
    if (len >= out_size) return false;
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

bool path_normalize(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return false;

    char expanded[PATH_MAX];
    if (!path_expand_tilde(path, expanded, sizeof(expanded))) {
        return false;
    }

    char resolved[PATH_MAX];
    bool is_abs = (expanded[0] == '/');

    // If relative, prepend current working directory
    if (!is_abs) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            return false;
        }
        int n = snprintf(resolved, sizeof(resolved), "%s/%s", cwd, expanded);
        if (n <= 0 || (size_t)n >= sizeof(resolved)) return false;
    } else {
        size_t len = strlen(expanded);
        if (len >= sizeof(resolved)) return false;
        strncpy(resolved, expanded, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    // Tokenize and resolve . and ..
    char *segments[PATH_MAX / 2];
    size_t seg_count = 0;

    char *saveptr = NULL;
    char *token = strtok_r(resolved, "/", &saveptr);
    while (token) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            // ignore
        } else if (strcmp(token, "..") == 0) {
            if (seg_count > 0) {
                seg_count--;
            }
        } else {
            segments[seg_count++] = token;
        }
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (seg_count == 0) {
        if (out_size < 2) return false;
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    size_t pos = 0;
    out[pos++] = '/';
    out[pos] = '\0';

    for (size_t i = 0; i < seg_count; ++i) {
        size_t seg_len = strlen(segments[i]);
        if (pos + seg_len + (i > 0 ? 1 : 0) >= out_size) {
            return false;
        }
        if (i > 0) {
            out[pos++] = '/';
        }
        memcpy(out + pos, segments[i], seg_len);
        pos += seg_len;
        out[pos] = '\0';
    }

    return true;
}

bool path_is_dir(const char *path)
{
    if (!path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool path_exists(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

const char *path_basename(const char *path)
{
    static _Thread_local char bname_buf[PATH_MAX];
    if (!path || path[0] == '\0') return "";

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            start = i + 1;
        }
    }

    size_t bname_len = len - start;
    if (bname_len >= sizeof(bname_buf)) bname_len = sizeof(bname_buf) - 1;
    memcpy(bname_buf, path + start, bname_len);
    bname_buf[bname_len] = '\0';
    return bname_buf;
}

bool path_get_config_dir(char *out, size_t out_size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        int n = snprintf(out, out_size, "%s/jrun", xdg);
        return (n > 0 && (size_t)n < out_size);
    }
    const char *home = get_home_directory();
    if (!home) return false;
    int n = snprintf(out, out_size, "%s/.config/jrun", home);
    return (n > 0 && (size_t)n < out_size);
}

bool path_get_data_dir(char *out, size_t out_size)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        int n = snprintf(out, out_size, "%s/jrun", xdg);
        return (n > 0 && (size_t)n < out_size);
    }
    const char *home = get_home_directory();
    if (!home) return false;
    int n = snprintf(out, out_size, "%s/.local/share/jrun", home);
    return (n > 0 && (size_t)n < out_size);
}

bool path_get_db_path(char *out, size_t out_size)
{
    char data_dir[PATH_MAX];
    if (!path_get_data_dir(data_dir, sizeof(data_dir))) return false;
    int n = snprintf(out, out_size, "%s/jrun.db", data_dir);
    return (n > 0 && (size_t)n < out_size);
}

bool path_get_config_path(char *out, size_t out_size)
{
    char config_dir[PATH_MAX];
    if (!path_get_config_dir(config_dir, sizeof(config_dir))) return false;
    int n = snprintf(out, out_size, "%s/config.toml", config_dir);
    return (n > 0 && (size_t)n < out_size);
}

bool path_mkdir_p(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0') return false;

    char tmp[PATH_MAX];
    size_t len = strlen(dir_path);
    if (len >= sizeof(tmp)) return false;
    strncpy(tmp, dir_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool path_ensure_parent_dir(const char *filepath)
{
    if (!filepath) return false;
    char tmp[PATH_MAX];
    size_t len = strlen(filepath);
    if (len >= sizeof(tmp)) return false;
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return true; // file in current directory
    *last_slash = '\0';
    return path_mkdir_p(tmp);
}
