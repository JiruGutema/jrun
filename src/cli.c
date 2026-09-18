#include "cli.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define dup_str jrun_strdup

void cli_print_version(void)
{
    printf("%s %s\n", JRUN_PROGRAM_NAME, JRUN_VERSION);
}

void cli_print_help(const char *prog_name)
{
    printf("Usage:\n");
    printf("  %s <target> <command> [args...]       Execute command in resolved directory\n", prog_name);
    printf("  %s <target> -- <command> [args...]    Unambiguous target and command syntax\n", prog_name);
    printf("  %s <target>                           Resolve directory and print path\n", prog_name);
    printf("  %s --cd <target>                      Resolve directory and print path\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                            Print this help message\n");
    printf("  -v, --version                         Print version information\n");
    printf("  -d, --debug                           Enable debug output\n");
    printf("  -q, --quiet                           Suppress non-essential messages\n");
    printf("  -i, --interactive                     Always prompt with TUI selector\n");
    printf("\n");
    printf("Database & Maintenance Commands:\n");
    printf("  %s add <path>                         Add or update directory in database\n", prog_name);
    printf("  %s remove <path>                      Remove directory from database\n", prog_name);
    printf("  %s list                               List tracked directories with frecency\n", prog_name);
    printf("  %s query <target>                     Query matches and display ranking scores\n", prog_name);
    printf("  %s prune                              Remove non-existent directories from database\n", prog_name);
    printf("  %s doctor                             Check environment, database, and search roots\n", prog_name);
    printf("\n");
    printf("Configuration Commands:\n");
    printf("  %s config                             Display current configuration\n", prog_name);
    printf("  %s root list                          List configured search roots\n", prog_name);
    printf("  %s root add <path>                    Add a filesystem search root\n", prog_name);
    printf("  %s root remove <path>                 Remove a filesystem search root\n", prog_name);
    printf("\n");
    printf("Shell Integration:\n");
    printf("  %s --init <bash|zsh|fish>             Generate shell integration script\n", prog_name);
}

void cli_free_args(Cli_Args *args)
{
    if (!args) return;
    free(args->target);
    free(args->extra_arg);
    free(args->cmd_argv);
    memset(args, 0, sizeof(*args));
}

bool cli_parse(int argc, char **argv, Cli_Args *args)
{
    if (!args) return false;
    memset(args, 0, sizeof(*args));

    if (argc <= 1) {
        args->action = CLI_ACTION_HELP;
        return true;
    }

    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            args->action = CLI_ACTION_HELP;
            return true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            args->action = CLI_ACTION_VERSION;
            return true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            args->debug = true;
            i++;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            args->quiet = true;
            i++;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            args->interactive = true;
            i++;
        } else if (strcmp(argv[i], "--cd") == 0) {
            args->action = CLI_ACTION_CD;
            i++;
            if (i < argc) {
                args->target = dup_str(argv[i++]);
            }
            return true;
        } else if (strcmp(argv[i], "--add") == 0) {
            args->action = CLI_ACTION_ADD;
            i++;
            if (i < argc) {
                args->extra_arg = dup_str(argv[i++]);
            }
            return true;
        } else if (strcmp(argv[i], "--init") == 0) {
            args->action = CLI_ACTION_INIT_SHELL;
            i++;
            if (i < argc) {
                args->extra_arg = dup_str(argv[i++]);
            }
            return true;
        } else {
            jrun_log_error("unknown option '%s'", argv[i]);
            return false;
        }
    }

    if (i >= argc) {
        if (args->action == CLI_ACTION_CD && args->target) {
            return true;
        }
        args->action = CLI_ACTION_HELP;
        return true;
    }

    // Check subcommands
    const char *first = argv[i];

    if (strcmp(first, "help") == 0) {
        args->action = CLI_ACTION_HELP;
        return true;
    } else if (strcmp(first, "version") == 0) {
        args->action = CLI_ACTION_VERSION;
        return true;
    } else if (strcmp(first, "add") == 0) {
        args->action = CLI_ACTION_ADD;
        i++;
        if (i < argc) {
            args->extra_arg = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "remove") == 0) {
        args->action = CLI_ACTION_REMOVE;
        i++;
        if (i < argc) {
            args->extra_arg = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "list") == 0) {
        args->action = CLI_ACTION_LIST;
        return true;
    } else if (strcmp(first, "query") == 0) {
        args->action = CLI_ACTION_QUERY;
        i++;
        if (i < argc) {
            args->target = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "prune") == 0) {
        args->action = CLI_ACTION_PRUNE;
        return true;
    } else if (strcmp(first, "doctor") == 0) {
        args->action = CLI_ACTION_DOCTOR;
        return true;
    } else if (strcmp(first, "config") == 0) {
        args->action = CLI_ACTION_CONFIG_SHOW;
        return true;
    } else if (strcmp(first, "shell") == 0) {
        args->action = CLI_ACTION_INIT_SHELL;
        i++;
        if (i < argc) {
            args->extra_arg = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "root") == 0) {
        i++;
        if (i >= argc || strcmp(argv[i], "list") == 0) {
            args->action = CLI_ACTION_ROOT_LIST;
        } else if (strcmp(argv[i], "add") == 0) {
            args->action = CLI_ACTION_ROOT_ADD;
            i++;
            if (i < argc) {
                args->extra_arg = dup_str(argv[i]);
            }
        } else if (strcmp(argv[i], "remove") == 0) {
            args->action = CLI_ACTION_ROOT_REMOVE;
            i++;
            if (i < argc) {
                args->extra_arg = dup_str(argv[i]);
            }
        } else {
            jrun_log_error("unknown root subcommand '%s'", argv[i]);
            return false;
        }
        return true;
    } else if (strcmp(first, "cd") == 0) {
        args->action = CLI_ACTION_CD;
        i++;
        if (i < argc) {
            args->target = dup_str(argv[i]);
        }
        return true;
    }

    // Otherwise, first argument is the target
    args->target = dup_str(argv[i++]);

    // Check if next argument is "--"
    if (i < argc && strcmp(argv[i], "--") == 0) {
        i++;
    }

    // Remaining arguments are the command to execute
    if (i < argc) {
        // If the command is "cd", it's directory jump mode!
        if (argc - i == 1 && strcmp(argv[i], "cd") == 0) {
            args->action = CLI_ACTION_CD;
        } else {
            args->action = CLI_ACTION_EXECUTE;
            args->cmd_argc = argc - i;
            args->cmd_argv = (char **)malloc((args->cmd_argc + 2) * sizeof(char *));
            if (!args->cmd_argv) return false;
            for (int j = 0; j < args->cmd_argc; ++j) {
                args->cmd_argv[j] = argv[i + j];
            }
            // If command is "open" or "xdg-open" with no arguments, default to "."
            if (args->cmd_argc == 1 && (strcmp(args->cmd_argv[0], "open") == 0 || strcmp(args->cmd_argv[0], "xdg-open") == 0)) {
                args->cmd_argv[1] = ".";
                args->cmd_argc = 2;
            }
            args->cmd_argv[args->cmd_argc] = NULL;
        }
    } else {
        // No command provided -> directory jump mode
        args->action = CLI_ACTION_CD;
    }

    return true;
}
