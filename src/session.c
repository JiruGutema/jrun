#include "session.h"
#include "common.h"
#include "executor.h"
#include "path_util.h"

#include <fcntl.h>
#include <sys/wait.h>

// tmux exits 127-style when it cannot be run at all, which is how a missing
// binary is told apart from "no server running".
#define TMUX_NOT_FOUND 127

void session_name_from_dir(const char *dir, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    path_basename_r(dir ? dir : "", out, out_size);
    if (out[0] == '\0' || strcmp(out, "/") == 0) {
        snprintf(out, out_size, "root");
        return;
    }
    for (char *p = out; *p; ++p) {
        if (*p == '.' || *p == ':') *p = '_';
    }
}

// Runs tmux with `argv`, collecting its stdout into a malloc'd string.
// Returns tmux's exit status, or TMUX_NOT_FOUND when it could not be run.
static int tmux_capture(char *const argv[], char **out)
{
    *out = NULL;
    int fds[2];
    if (pipe(fds) != 0) return 1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execvp(argv[0], argv);
        _exit(TMUX_NOT_FOUND);
    }

    close(fds[1]);
    size_t len = 0, cap = 0;
    char *buf = NULL;
    for (;;) {
        bool ok = true;
        if (len + 512 >= cap) {
            size_t new_cap = cap ? cap * 2 : 1024;
            char *grown = (char *)realloc(buf, new_cap);
            if (!grown) ok = false;
            else { buf = grown; cap = new_cap; }
        }
        if (!ok) break;
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fds[0]);
    if (buf) buf[len] = '\0';

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *out = buf;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

// Looks `name` up in list-sessions output ("name\tpath" per line). Returns
// true when it exists, copying its path into path_out.
static bool find_session(const char *listing, const char *name, char *path_out, size_t path_size)
{
    size_t name_len = strlen(name);
    for (const char *line = listing; line && *line; ) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        const char *tab = memchr(line, '\t', line_len);
        if (tab && (size_t)(tab - line) == name_len && strncmp(line, name, name_len) == 0) {
            size_t plen = line_len - name_len - 1;
            if (plen >= path_size) plen = path_size - 1;
            memcpy(path_out, tab + 1, plen);
            path_out[plen] = '\0';
            return true;
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

// Finds a session already rooted at `dir`, copying its name into name_out.
static bool find_session_at(const char *listing, const char *dir, char *name_out, size_t name_size)
{
    size_t dir_len = strlen(dir);
    for (const char *line = listing; line && *line; ) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        const char *tab = memchr(line, '\t', line_len);
        if (tab) {
            size_t nlen = (size_t)(tab - line);
            size_t plen = line_len - nlen - 1;
            if (plen == dir_len && strncmp(tab + 1, dir, dir_len) == 0 && nlen < name_size) {
                memcpy(name_out, line, nlen);
                name_out[nlen] = '\0';
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

// Picks a name no other session uses: the directory's own name, then
// "<parent>_<name>", then "<name>-2", "<name>-3", ...
static bool pick_free_name(const char *listing, const char *dir, char *out, size_t out_size)
{
    char base[128];
    char other_path[PATH_MAX];
    session_name_from_dir(dir, base, sizeof(base));
    snprintf(out, out_size, "%s", base);
    if (!find_session(listing, out, other_path, sizeof(other_path))) return true;

    char parent_dir[PATH_MAX];
    snprintf(parent_dir, sizeof(parent_dir), "%s", dir);
    char *slash = strrchr(parent_dir, '/');
    if (slash && slash != parent_dir) {
        *slash = '\0';
        char parent[128];
        session_name_from_dir(parent_dir, parent, sizeof(parent));
        snprintf(out, out_size, "%s_%s", parent, base);
        if (!find_session(listing, out, other_path, sizeof(other_path))) return true;
    }

    for (int i = 2; i < 100; ++i) {
        snprintf(out, out_size, "%s-%d", base, i);
        if (!find_session(listing, out, other_path, sizeof(other_path))) return true;
    }
    return false;
}

int session_open(const char *dir)
{
    if (!dir || dir[0] == '\0') return 1;

    char *list_argv[] = {"tmux", "list-sessions", "-F", "#{session_name}\t#{session_path}", NULL};
    char *listing = NULL;
    int rc = tmux_capture(list_argv, &listing);
    if (rc == TMUX_NOT_FOUND) {
        free(listing);
        jrun_log_error("tmux is not installed");
        return 127;
    }
    // Any other failure means no server is running, so no sessions exist.
    if (rc != 0) {
        free(listing);
        listing = NULL;
    }

    char name[300];
    bool exists = find_session_at(listing, dir, name, sizeof(name));
    if (!exists && !pick_free_name(listing, dir, name, sizeof(name))) {
        free(listing);
        jrun_log_error("could not find a free tmux session name for %s", dir);
        return 1;
    }
    free(listing);

    // "=" makes tmux match the name exactly rather than as a prefix.
    char target[304];
    snprintf(target, sizeof(target), "=%s", name);

    const char *in_tmux = getenv("TMUX");
    if (in_tmux && in_tmux[0] != '\0') {
        // A client cannot attach from inside tmux, so create the session
        // detached and move this client over to it.
        if (!exists) {
            char *new_argv[] = {"tmux", "new-session", "-d", "-s", name, "-c", (char *)dir, NULL};
            rc = executor_run(dir, new_argv);
            if (rc != 0) return rc;
        }
        char *switch_argv[] = {"tmux", "switch-client", "-t", target, NULL};
        return executor_run(dir, switch_argv);
    }

    if (chdir(dir) != 0) {
        jrun_log_error("failed to change directory to '%s': %s", dir, strerror(errno));
        return 1;
    }
    if (exists) {
        char *attach_argv[] = {"tmux", "attach-session", "-t", target, NULL};
        execvp(attach_argv[0], attach_argv);
    } else {
        char *new_argv[] = {"tmux", "new-session", "-s", name, "-c", (char *)dir, NULL};
        execvp(new_argv[0], new_argv);
    }
    jrun_log_error("failed to run tmux: %s", strerror(errno));
    return errno == ENOENT ? 127 : 1;
}
