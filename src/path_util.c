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

const char *path_basename_r(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) return "";
    out[0] = '\0';
    if (!path || path[0] == '\0') return out;

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
    if (bname_len >= out_size) bname_len = out_size - 1;
    memcpy(out, path + start, bname_len);
    out[bname_len] = '\0';
    return out;
}

// Ordered most- to least-specific: the first hit wins, so a Rust workspace
// that also has a .git reports "rust" rather than the generic "git".
static const struct { const char *file; const char *kind; } PROJECT_MARKERS[] = {
    { "Cargo.toml",       "rust"   },
    { "go.mod",           "go"     },
    { "package.json",     "node"   },
    { "pyproject.toml",   "python" },
    { "setup.py",         "python" },
    { "requirements.txt", "python" },
    { "CMakeLists.txt",   "cmake"  },
    { "Makefile",         "make"   },
    { "nob.c",            "nob"    },
    { "build.zig",        "zig"    },
    { "pom.xml",          "java"   },
    { "build.gradle",     "java"   },
    { "Gemfile",          "ruby"   },
    { "composer.json",    "php"    },
    { ".git",             "git"    },
    { ".hg",              "git"    },
    { ".svn",             "git"    },
    { NULL, NULL }
};

const char *path_project_kind(const char *path)
{
    if (!path || path[0] == '\0') return NULL;

    char probe[PATH_MAX];
    for (size_t i = 0; PROJECT_MARKERS[i].file != NULL; ++i) {
        int n = snprintf(probe, sizeof(probe), "%s/%s", path, PROJECT_MARKERS[i].file);
        if (n <= 0 || (size_t)n >= sizeof(probe)) continue;
        if (path_exists(probe)) {
            return PROJECT_MARKERS[i].kind;
        }
    }
    return NULL;
}

bool path_is_project_dir(const char *path)
{
    return path_project_kind(path) != NULL;
}

bool path_write_atomic(const char *filepath, const char *content, size_t len)
{
    if (!filepath || (!content && len > 0)) return false;

    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", filepath, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *fp = fopen(tmp, "w");
    if (!fp) return false;

    bool ok = (len == 0) || (fwrite(content, 1, len, fp) == len);
    if (ok && fflush(fp) != 0) ok = false;
    // fsync before rename: rename is atomic with respect to the directory
    // entry, but without the fsync the new contents may not have reached disk.
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;

    if (!ok || rename(tmp, filepath) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
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
