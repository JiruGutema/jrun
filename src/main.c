#include "common.h"
#include "cli.h"
#include "config.h"
#include "database.h"
#include "resolver.h"
#include "tui.h"
#include "executor.h"
#include "shell.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

Log_Level g_log_level = LOG_LEVEL_NORMAL;

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
    if (path_exists(db_path)) {
        printf("    Status: Found\n");
        Db_Entry *entries = NULL;
        size_t count = 0;
        if (db_get_all(&entries, &count)) {
            printf("    Tracked directories: %zu\n", count);
            db_free_entries(entries, count);
        }
    } else {
        printf("    Status: Not created yet (will be created on first add)\n");
    }

    // 4. Search Roots
    printf("[4] Configured Search Roots (%zu):\n", config->roots_count);
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

    // 5. Terminal & Environment
    printf("[5] Terminal & Environment:\n");
    printf("    isatty(STDIN):  %s\n", isatty(STDIN_FILENO) ? "yes" : "no");
    printf("    isatty(STDOUT): %s\n", isatty(STDOUT_FILENO) ? "yes" : "no");
    const char *term = getenv("TERM");
    printf("    TERM:           %s\n", term ? term : "unset");
    const char *shell = getenv("SHELL");
    printf("    SHELL:          %s\n", shell ? shell : "unset");

    printf("\nAll diagnostic checks completed.\n");
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
            printf("%-10.2f  %-8.1f  %-10.2f  %s %s\n",
                   res.candidates[i].score,
                   res.candidates[i].match_quality,
                   res.candidates[i].frecency,
                   short_path,
                   res.candidates[i].from_db ? "(db)" : "(root)");
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
        if (res.status == RESOLVE_NO_MATCH) {
            fprintf(stderr, "jrun: no directory found for: %s\n\nSearched:\n", args.target);
            for (size_t i = 0; i < config.roots_count; ++i) {
                fprintf(stderr, "  %s\n", config.roots[i]);
            }
            resolver_free_result(&res);
            ret_code = 1;
            break;
        }

        char *selected_path = NULL;

        // Disambiguate if ambiguous (and config.interactive is true) or -i flag passed
        bool need_tui = (args.interactive || (config.interactive && res.status == RESOLVE_AMBIGUOUS));
        if (need_tui && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            selected_path = tui_select(res.candidates, res.count, args.target);
            if (!selected_path) {
                // User cancelled TUI
                resolver_free_result(&res);
                ret_code = 130;
                break;
            }
        } else {
            // Pick top candidate
            selected_path = strdup(res.candidates[0].path);
        }

        // Record directory access in database
        db_add_or_update(selected_path);
        db_age_if_needed();

        if (args.action == CLI_ACTION_CD) {
            // Directory jump mode: output resolved path
            printf("%s\n", selected_path);
            ret_code = 0;
        } else {
            // Echo the target path before running the command
            if (!args.quiet) {
                if (isatty(STDOUT_FILENO)) {
                    printf("%s\n", selected_path);
                    fflush(stdout);
                } else {
                    fprintf(stderr, "%s\n", selected_path);
                    fflush(stderr);
                }
            }
            // Execute command in resolved directory
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
