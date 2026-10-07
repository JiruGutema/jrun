#include "common.h"
#include "cli.h"
#include "config.h"
#include "database.h"
#include "resolver.h"
#include "tui.h"
#include "executor.h"
#include "shell.h"
#include "session.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

Log_Level g_log_level = LOG_LEVEL_NORMAL;

// More than this scrolls off the screen when the shell lists them.
#define COMPLETION_LIMIT 50

static void run_doctor(const Jrun_Config *config, const char *config_path, const char *db_path)
{
    printf("=== Jrun Doctor Diagnostics ===\n\n");

    // 1. Version
    printf("[1] Version: %s\n", JRUN_VERSION);

    // 2. Config
    printf("[2] Config File: %s\n", config_path);
    if (path_exists(config_path)) {
        printf("    Status: Found\n");
    } else {
        printf("    Status: Not found (using defaults)\n");
    }

    // 3. Database
    printf("[3] Database: %s\n", db_path);
    {
        Db_Entry *entries = NULL;
        size_t count = 0;
        if (db_get_all(&entries, &count)) {
            printf("    Status: OK\n");
            printf("    Tracked directories: %zu\n", count);
            db_free_entries(entries, count);
        } else {
            printf("    Status: UNREADABLE\n");
        }
    }

    // 4. Cached directory index
    {
        size_t cached = 0;
        int64_t built_at = 0;
        db_cache_stats(&cached, &built_at);
        char fingerprint[64];
        config_scan_fingerprint(config, fingerprint, sizeof(fingerprint));
        bool fresh = db_cache_is_fresh(fingerprint, config->cache_ttl);

        printf("[4] Directory index: %zu paths cached\n", cached);
        if (config->cache_ttl <= 0) {
            printf("    Status: disabled (cache_ttl = 0, every lookup rescans)\n");
        } else if (built_at > 0) {
            long long age = (long long)time(NULL) - (long long)built_at;
            printf("    Status: %s (%llds old, ttl %ds)\n",
                   fresh ? "fresh" : "stale, will rebuild on next lookup",
                   age, config->cache_ttl);
        } else {
            printf("    Status: not built yet (run `jrun reindex`)\n");
        }
    }

    // 5. Search Roots
    printf("[5] Configured Search Roots (%zu):\n", config->roots_count);
    for (size_t i = 0; i < config->roots_count; ++i) {
        char expanded[PATH_MAX];
        memset(expanded, 0, sizeof(expanded));
        if (path_expand_tilde(config->roots[i], expanded, sizeof(expanded))) {
            bool is_dir = path_is_dir(expanded);
            printf("    - %s (%s): %s\n", config->roots[i], expanded, is_dir ? "OK (accessible)" : "MISSING or inaccessible");
        } else {
            printf("    - %s: FAILED to expand path\n", config->roots[i]);
        }
    }

    // 6. Terminal & Environment
    printf("[6] Terminal & Environment:\n");
    printf("    isatty(STDIN):  %s\n", isatty(STDIN_FILENO) ? "yes" : "no");
    printf("    isatty(STDOUT): %s\n", isatty(STDOUT_FILENO) ? "yes" : "no");
    printf("    interactive UI: %s\n",
           tui_available() ? "available" : "unavailable (no controlling terminal)");
    const char *term = getenv("TERM");
    printf("    TERM:           %s\n", term ? term : "unset");
    const char *shell = getenv("SHELL");
    printf("    SHELL:          %s\n", shell ? shell : "unset");

    printf("\nAll diagnostic checks completed.\n");
}

// Bookmark names must read as a target and nothing else: no path
// separators, nothing an option or a path or "@" could start with, and no
// subcommand names, which would never reach the bookmark.
static bool bookmark_name_ok(const char *name)
{
    if (!name || name[0] == '\0') {
        jrun_log_error("missing bookmark name");
        return false;
    }
    if (strchr("-@.~", name[0]) || strpbrk(name, "/ \t\n")) {
        jrun_log_error("invalid bookmark name '%s': it cannot contain '/' or spaces, "
                       "or start with '-', '@', '.' or '~'", name);
        return false;
    }
    if (cli_is_reserved_word(name)) {
        jrun_log_error("'%s' is a jrun subcommand and cannot be a bookmark name", name);
        return false;
    }
    return true;
}

static void print_bookmarks(void)
{
    Db_Bookmark *marks = NULL;
    size_t count = 0;
    if (!db_bookmark_list(&marks, &count)) return;
    if (count == 0) {
        jrun_log_info("no bookmarks yet; add one with `jrun mark <name> [path]`");
        return;
    }
    int width = 4;
    for (size_t i = 0; i < count; ++i) {
        int len = (int)strlen(marks[i].name);
        if (len > width) width = len;
    }
    for (size_t i = 0; i < count; ++i) {
        char short_path[PATH_MAX];
        path_shorten_tilde(marks[i].path, short_path, sizeof(short_path));
        printf("%-*s  %s%s\n", width, marks[i].name, short_path,
               path_is_dir(marks[i].path) ? "" : "  (missing)");
    }
    db_free_bookmarks(marks, count);
}

// Runs the command in several directories one after another, for -a and -m.
// A failure does not stop the rest, but Ctrl-C does.
static int run_in_many(const Cli_Args *args, const Jrun_Config *config, const Resolve_Result *res)
{
    const char **dirs = (const char **)malloc(res->count * sizeof(*dirs));
    if (!dirs) return 1;
    size_t n = 0;

    if (args->multi) {
        if (!tui_available()) {
            jrun_log_error("-m needs a terminal for the selector");
            free(dirs);
            return 1;
        }
        size_t *picked = NULL;
        size_t picked_count = 0;
        if (!tui_select_many(res->candidates, res->count, args->target, &picked, &picked_count)) {
            free(dirs);
            return 130;  // cancelled
        }
        for (size_t i = 0; i < picked_count; ++i) {
            dirs[n++] = res->candidates[picked[i]].path;
        }
        free(picked);
    } else {
        // Loose matches would sweep in unrelated directories, so -a only
        // takes the ones carrying exactly the name asked for.
        for (size_t i = 0; i < res->count; ++i) {
            if (res->candidates[i].is_exact_basename) dirs[n++] = res->candidates[i].path;
        }
        if (n == 0) {
            jrun_log_error("no directory is named exactly '%s'; use -m to pick from the matches",
                           args->target);
            free(dirs);
            return 1;
        }
    }

    if (!args->yes && config_needs_confirm(config, args->cmd_argv[0])) {
        char label[64];
        snprintf(label, sizeof(label), "%zu directories", n);
        if (!tui_confirm_command(n == 1 ? dirs[0] : label, args->cmd_argv)) {
            jrun_log_info("cancelled");
            free(dirs);
            return 130;
        }
    }

    // Same rule as a single run: keep stdout for the commands when piped.
    FILE *banner = isatty(STDOUT_FILENO) ? stdout : stderr;
    bool color = isatty(fileno(banner));
    size_t failures = 0;
    int ret = 0;
    for (size_t i = 0; i < n; ++i) {
        db_add_or_update(dirs[i]);
        if (!args->quiet) {
            char short_path[PATH_MAX];
            path_shorten_tilde(dirs[i], short_path, sizeof(short_path));
            fprintf(banner, color ? "%s\x1b[1;36m==> %s\x1b[0m\n" : "%s==> %s\n",
                    i > 0 ? "\n" : "", short_path);
            fflush(banner);
        }
        int rc = executor_run(dirs[i], args->cmd_argv);
        if (rc != 0) {
            failures++;
            ret = rc;
        }
        if (rc == 130) break;  // interrupted: stop here rather than carry on
    }
    db_age_if_needed();

    if (failures > 0 && n > 1) {
        jrun_log_error("failed in %zu of %zu directories", failures, n);
    }
    free(dirs);
    return ret;
}

int main(int argc, char **argv)
{
    Cli_Args args = {0};
    if (!cli_parse(argc, argv, &args)) {
        cli_free_args(&args);
        return 1;
    }

    if (args.debug) {
        g_log_level = LOG_LEVEL_DEBUG;
    } else if (args.quiet) {
        g_log_level = LOG_LEVEL_QUIET;
    }

    if (args.action == CLI_ACTION_HELP) {
        cli_print_help(argv[0]);
        cli_free_args(&args);
        return 0;
    }

    if (args.action == CLI_ACTION_VERSION) {
        cli_print_version();
        cli_free_args(&args);
        return 0;
    }

    const char *bad_combo = NULL;
    if (args.session && args.action == CLI_ACTION_EXECUTE) {
        bad_combo = "-s opens a session in the directory and does not take a command";
    } else if ((args.all || args.multi) && args.action == CLI_ACTION_CD) {
        bad_combo = "-a and -m need a command to run";
    } else if (args.all && args.multi) {
        bad_combo = "use either -a or -m, not both";
    }
    if (bad_combo) {
        jrun_log_error("%s", bad_combo);
        cli_free_args(&args);
        return 1;
    }

    if (args.action == CLI_ACTION_INIT_SHELL) {
        Shell_Type st = shell_parse_type(args.extra_arg ? args.extra_arg : "bash");
        bool ok = shell_generate_init(st);
        cli_free_args(&args);
        return ok ? 0 : 1;
    }

    // Load configuration
    char config_path[PATH_MAX];
    path_get_config_path(config_path, sizeof(config_path));
    Jrun_Config config = {0};
    config_load(&config, config_path);

    // Config subcommands
    if (args.action == CLI_ACTION_CONFIG_SHOW) {
        printf("Configuration file: %s\n\n", config_path);
        config_print(&config);
        config_free(&config);
        cli_free_args(&args);
        return 0;
    }

    if (args.action == CLI_ACTION_CONFIG_EDIT) {
        int cfg_rc = 0;
        if (tui_available()) {
            if (!tui_edit_config(&config, config_path)) {
                cfg_rc = 1;
            }
        } else {
            printf("Configuration file: %s\n\n", config_path);
            config_print(&config);
            printf("\n(open a terminal to use `jrun config edit`)\n");
        }
        config_free(&config);
        cli_free_args(&args);
        return cfg_rc;
    }

    if (args.action == CLI_ACTION_ROOT_LIST) {
        printf("Configured search roots (%zu):\n", config.roots_count);
        for (size_t i = 0; i < config.roots_count; ++i) {
            printf("  %s\n", config.roots[i]);
        }
        config_free(&config);
        cli_free_args(&args);
        return 0;
    }

    if (args.action == CLI_ACTION_ROOT_ADD) {
        if (!args.extra_arg || args.extra_arg[0] == '\0') {
            jrun_log_error("missing path to add as root");
            config_free(&config);
            cli_free_args(&args);
            return 1;
        }
        if (config_add_root(&config, args.extra_arg)) {
            config_save(&config, config_path);
            jrun_log_info("added search root: %s", args.extra_arg);
        }
        config_free(&config);
        cli_free_args(&args);
        return 0;
    }

    if (args.action == CLI_ACTION_ROOT_REMOVE) {
        if (!args.extra_arg || args.extra_arg[0] == '\0') {
            jrun_log_error("missing path to remove from roots");
            config_free(&config);
            cli_free_args(&args);
            return 1;
        }
        if (config_remove_root(&config, args.extra_arg)) {
            config_save(&config, config_path);
            jrun_log_info("removed search root: %s", args.extra_arg);
        } else {
            jrun_log_error("root not found: %s", args.extra_arg);
        }
        config_free(&config);
        cli_free_args(&args);
        return 0;
    }

    // Initialize Database
    char db_path[PATH_MAX];
    path_get_db_path(db_path, sizeof(db_path));
    if (!db_init(db_path)) {
        jrun_log_error("failed to initialize database at '%s'", db_path);
        config_free(&config);
        cli_free_args(&args);
        return 1;
    }

    int ret_code = 0;

    switch (args.action) {
    case CLI_ACTION_DOCTOR: {
        run_doctor(&config, config_path, db_path);
        break;
    }

    case CLI_ACTION_ADD: {
        char path_to_add[PATH_MAX];
        if (args.extra_arg && args.extra_arg[0] != '\0') {
            strncpy(path_to_add, args.extra_arg, sizeof(path_to_add) - 1);
            path_to_add[sizeof(path_to_add) - 1] = '\0';
        } else {
            if (!getcwd(path_to_add, sizeof(path_to_add))) {
                jrun_log_error("failed to get current working directory");
                ret_code = 1;
                break;
            }
        }
        if (db_add_or_update(path_to_add)) {
            jrun_log_debug("added '%s' to database", path_to_add);
            db_age_if_needed();
        } else {
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_REMOVE: {
        if (!args.extra_arg) {
            jrun_log_error("missing directory to remove");
            ret_code = 1;
            break;
        }
        if (db_remove(args.extra_arg)) {
            jrun_log_info("removed '%s' from database", args.extra_arg);
        } else {
            jrun_log_error("failed to remove '%s'", args.extra_arg);
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_LIST: {
        Db_Entry *entries = NULL;
        size_t count = 0;
        if (db_get_all(&entries, &count)) {
            printf("%-10s  %-10s  %-20s  %s\n", "SCORE", "FREQUENCY", "LAST ACCESS", "PATH");
            printf("--------------------------------------------------------------------------------\n");
            for (size_t i = 0; i < count; ++i) {
                char time_buf[64] = {0};
                time_t t = (time_t)entries[i].last_access;
                struct tm *tm_info = localtime(&t);
                if (tm_info) {
                    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                }
                char short_path[PATH_MAX];
                path_shorten_tilde(entries[i].path, short_path, sizeof(short_path));
                printf("%-10.2f  %-10.1f  %-20s  %s\n",
                       entries[i].frecency,
                       entries[i].frequency,
                       time_buf,
                       short_path);
            }
            db_free_entries(entries, count);
        }
        break;
    }

    case CLI_ACTION_MARK_LIST:
        print_bookmarks();
        break;

    case CLI_ACTION_MARK_ADD: {
        if (!bookmark_name_ok(args.target)) {
            ret_code = 1;
            break;
        }
        char mark_path[PATH_MAX];
        if (args.extra_arg && args.extra_arg[0] != '\0') {
            snprintf(mark_path, sizeof(mark_path), "%s", args.extra_arg);
        } else if (!getcwd(mark_path, sizeof(mark_path))) {
            jrun_log_error("failed to get current working directory");
            ret_code = 1;
            break;
        }
        if (!db_bookmark_set(args.target, mark_path)) {
            ret_code = 1;
            break;
        }
        char *stored = NULL;
        db_bookmark_get(args.target, &stored);
        char short_path[PATH_MAX];
        path_shorten_tilde(stored ? stored : mark_path, short_path, sizeof(short_path));
        jrun_log_info("%s -> %s", args.target, short_path);
        free(stored);
        break;
    }

    case CLI_ACTION_MARK_REMOVE: {
        if (!args.target) {
            jrun_log_error("missing bookmark name to remove");
            ret_code = 1;
            break;
        }
        bool removed = false;
        if (!db_bookmark_remove(args.target, &removed)) {
            ret_code = 1;
        } else if (!removed) {
            jrun_log_error("no bookmark named '%s'", args.target);
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_COMPLETE_MARK: {
        Db_Bookmark *marks = NULL;
        size_t count = 0;
        if (db_bookmark_list(&marks, &count)) {
            for (size_t i = 0; i < count; ++i) printf("%s\n", marks[i].name);
            db_free_bookmarks(marks, count);
        }
        break;
    }

    case CLI_ACTION_REINDEX: {
        size_t indexed = 0;
        if (resolver_reindex(&config, &indexed)) {
            jrun_log_info("indexed %zu directories", indexed);
        } else {
            jrun_log_error("failed to rebuild the directory index");
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_PRUNE: {
        size_t pruned = 0;
        if (db_prune(&pruned)) {
            jrun_log_info("pruned %zu non-existent directories from database", pruned);
        } else {
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_QUERY: {
        if (!args.target) {
            jrun_log_error("query requires a search target");
            ret_code = 1;
            break;
        }
        Resolve_Result res = resolver_resolve(args.target, &config, true);
        const char *status_str = "UNKNOWN";
        switch (res.status) {
        case RESOLVE_NO_MATCH: status_str = "NO_MATCH"; break;
        case RESOLVE_SINGLE_MATCH: status_str = "SINGLE_MATCH"; break;
        case RESOLVE_HIGH_CONFIDENCE: status_str = "HIGH_CONFIDENCE"; break;
        case RESOLVE_AMBIGUOUS: status_str = "AMBIGUOUS"; break;
        }
        printf("Resolution Status: %s (Total matches: %zu)\n\n", status_str, res.count);
        printf("%-10s  %-8s  %-10s  %s\n", "SCORE", "QUALITY", "FRECENCY", "PATH");
        printf("--------------------------------------------------------------------------------\n");
        for (size_t i = 0; i < res.count; ++i) {
            char short_path[PATH_MAX];
            path_shorten_tilde(res.candidates[i].path, short_path, sizeof(short_path));
            const char *source = res.candidates[i].is_bookmark ? "(mark)"
                               : res.candidates[i].from_db ? "(db)" : "(root)";
            printf("%-10.2f  %-8.1f  %-10.2f  %s %s\n",
                   res.candidates[i].score,
                   res.candidates[i].match_quality,
                   res.candidates[i].frecency,
                   short_path,
                   source);
        }
        resolver_free_result(&res);
        break;
    }

    case CLI_ACTION_COMPLETE_TARGET: {
        char **names = NULL;
        size_t count = 0;
        if (resolver_complete(args.target, &config, COMPLETION_LIMIT, &names, &count)) {
            for (size_t i = 0; i < count; ++i) {
                printf("%s\n", names[i]);
            }
            resolver_free_names(names, count);
        } else {
            ret_code = 1;
        }
        break;
    }

    case CLI_ACTION_RESOLVE: {
        // Completion needs to know where a command would run. Pressing Tab
        // must never open the selector or count as a visit, so this takes the
        // top match as it stands and leaves the database alone.
        if (!args.target) {
            ret_code = 1;
            break;
        }
        Resolve_Result res = resolver_resolve(args.target, &config, false);
        if (res.count > 0) {
            printf("%s\n", res.candidates[0].path);
        } else {
            ret_code = 1;
        }
        resolver_free_result(&res);
        break;
    }

    case CLI_ACTION_EXECUTE:
    case CLI_ACTION_CD: {
        if (!args.target) {
            jrun_log_error("missing search target");
            ret_code = 1;
            break;
        }

        Resolve_Result res = resolver_resolve(args.target, &config, false);
        if (res.status == RESOLVE_NO_MATCH && resolver_is_project_target(args.target)) {
            if (args.target[1] == '\0') {
                jrun_log_error("not inside a project (no .git or project file above here)");
            } else {
                jrun_log_error("no directory %s in this project", args.target + 2);
            }
            resolver_free_result(&res);
            ret_code = 1;
            break;
        }
        if (res.status == RESOLVE_NO_MATCH) {
            fprintf(stderr, "jrun: no directory found for: %s\n\nSearched:\n", args.target);
            for (size_t i = 0; i < config.roots_count; ++i) {
                fprintf(stderr, "  %s\n", config.roots[i]);
            }
            resolver_free_result(&res);
            ret_code = 1;
            break;
        }

        if (args.all || args.multi) {
            ret_code = run_in_many(&args, &config, &res);
            resolver_free_result(&res);
            break;
        }

        char *selected_path = NULL;

        // The selector is gated on a reachable controlling terminal, not on
        // isatty(stdout): the shell wrapper always runs jrun inside $(...), so
        // stdout is a pipe even when the user is sitting at a terminal.
        bool want_tui = args.interactive ||
                        (config.interactive && res.status == RESOLVE_AMBIGUOUS);
        if (want_tui && tui_available()) {
            selected_path = tui_select(res.candidates, res.count, args.target);
            if (!selected_path) {
                resolver_free_result(&res);
                ret_code = 130;  // cancelled
                break;
            }
        } else {
            if (want_tui) {
                jrun_log_debug("%zu candidates but no terminal; taking the top match",
                               res.count);
            }
            selected_path = jrun_strdup(res.candidates[0].path);
            if (!selected_path) {
                resolver_free_result(&res);
                ret_code = 1;
                break;
            }
        }

        if (args.action == CLI_ACTION_CD && args.session) {
            db_add_or_update(selected_path);
            db_age_if_needed();
            // Outside tmux this execs tmux in jrun's place, so let go of the
            // database first.
            db_close();
            ret_code = session_open(selected_path);
        } else if (args.action == CLI_ACTION_CD) {
            db_add_or_update(selected_path);
            db_age_if_needed();
            printf("%s\n", selected_path);
            ret_code = 0;
        } else {
            // Confirm before anything else happens, so declining a destructive
            // command leaves no trace: no frecency bump, no echoed path.
            if (!args.yes && config_needs_confirm(&config, args.cmd_argv[0])) {
                if (!tui_confirm_command(selected_path, args.cmd_argv)) {
                    jrun_log_info("cancelled");
                    free(selected_path);
                    resolver_free_result(&res);
                    ret_code = 130;
                    break;
                }
            }

            db_add_or_update(selected_path);
            db_age_if_needed();

            if (!args.quiet) {
                // stdout belongs to the command we are about to run, so the
                // directory banner goes to stderr unless nobody is piping us.
                FILE *banner = isatty(STDOUT_FILENO) ? stdout : stderr;
                fprintf(banner, "%s\n", selected_path);
                fflush(banner);
            }
            ret_code = executor_run(selected_path, args.cmd_argv);
        }

        free(selected_path);
        resolver_free_result(&res);
        break;
    }

    default:
        break;
    }

    db_close();
    config_free(&config);
    cli_free_args(&args);

    return ret_code;
}
