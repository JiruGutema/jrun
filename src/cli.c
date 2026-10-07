#include "cli.h"
#include "common.h"
#include "path_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define dup_str jrun_strdup

void cli_print_version(void)
{
    printf("%s %s\n", JRUN_PROGRAM_NAME, JRUN_VERSION);
}

void cli_print_help(const char *invoked_as)
{
    // argv[0] can be an absolute path; showing it verbatim makes every line of
    // the help text unreadable.
    char prog_buf[64];
    const char *prog_name = path_basename_r(invoked_as ? invoked_as : JRUN_PROGRAM_NAME,
                                            prog_buf, sizeof(prog_buf));
    if (prog_name[0] == '\0') prog_name = JRUN_PROGRAM_NAME;

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
    printf("  -y, --yes                             Skip confirmation for destructive commands\n");
    printf("  -s, --session                         Open a tmux session in the directory\n");
    printf("  -a, --all                             Run the command in every directory with that exact name\n");
    printf("  -m, --multi                           Pick directories to run the command in (Tab marks)\n");
    printf("\n");
    printf("Targets:\n");
    printf("  <name>                                Best match by name, ranked by frecency\n");
    printf("  <part>/<name>                         Match path parts in order, e.g. dev/api\n");
    printf("  @, @/<sub>                            Root of the current project (git root or nearest marker)\n");
    printf("  <bookmark>, <bookmark>/<sub>          A directory saved with `%s mark`\n", prog_name);
    printf("\n");
    printf("Database & Maintenance Commands:\n");
    printf("  %s add <path>                         Add or update directory in database\n", prog_name);
    printf("  %s remove <path>                      Remove directory from database\n", prog_name);
    printf("  %s list                               List tracked directories with frecency\n", prog_name);
    printf("  %s query <target>                     Query matches and display ranking scores\n", prog_name);
    printf("  %s prune                              Remove non-existent directories from database\n", prog_name);
    printf("  %s reindex                            Rebuild the cached directory index now\n", prog_name);
    printf("  %s doctor                             Check environment, database, and search roots\n", prog_name);
    printf("\n");
    printf("Bookmarks:\n");
    printf("  %s mark                               List bookmarks\n", prog_name);
    printf("  %s mark <name> [path]                 Bookmark a directory (default: current one)\n", prog_name);
    printf("  %s unmark <name>                      Remove a bookmark\n", prog_name);
    printf("\n");
    printf("Configuration Commands:\n");
    printf("  %s config                             Open interactive config TUI (edits TOML)\n", prog_name);
    printf("  %s config show                        Print configuration as TOML\n", prog_name);
    printf("  %s config edit                        Open interactive config TUI\n", prog_name);
    printf("  %s root list                          List configured search roots\n", prog_name);
    printf("  %s root add <path>                    Add a filesystem search root\n", prog_name);
    printf("  %s root remove <path>                 Remove a filesystem search root\n", prog_name);
    printf("\n");
    printf("Shell Integration:\n");
    printf("  %s --init <bash|zsh|fish>             Generate shell integration script\n", prog_name);
}

bool cli_is_reserved_word(const char *word)
{
    static const char *const words[] = {
        "help", "version", "add", "remove", "list", "query", "prune", "reindex",
        "doctor", "config", "shell", "root", "cd", "mark", "unmark",
    };
    if (!word) return false;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        if (strcmp(word, words[i]) == 0) return true;
    }
    return false;
}

void cli_free_args(Cli_Args *args)
{
    if (!args) return;
    free(args->target);
    free(args->extra_arg);
    free(args->cmd_argv);
    memset(args, 0, sizeof(*args));
}

// Flags that only modify how a target is resolved. They are accepted before
// the target and directly after it, so `jrun proj -i` works like `jrun -i proj`.
static bool parse_modifier_flag(const char *arg, Cli_Args *args)
{
    if (strcmp(arg, "-d") == 0 || strcmp(arg, "--debug") == 0) {
        args->debug = true;
    } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
        args->quiet = true;
    } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--interactive") == 0) {
        args->interactive = true;
    } else if (strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0) {
        args->yes = true;
    } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--session") == 0) {
        args->session = true;
    } else if (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0) {
        args->all = true;
    } else if (strcmp(arg, "-m") == 0 || strcmp(arg, "--multi") == 0) {
        args->multi = true;
    } else {
        return false;
    }
    return true;
}

// Consumes modifier flags following a target. Stops at "--" or the first
// argument that is not an option, which is where the command begins; no
// command starts with '-', so this never swallows part of one.
static bool parse_trailing_flags(int argc, char **argv, int *i, Cli_Args *args)
{
    while (*i < argc && argv[*i][0] == '-' && argv[*i][1] != '\0' &&
           strcmp(argv[*i], "--") != 0) {
        if (!parse_modifier_flag(argv[*i], args)) {
            jrun_log_error("unknown option '%s'", argv[*i]);
            return false;
        }
        (*i)++;
    }
    return true;
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
        } else if (parse_modifier_flag(argv[i], args)) {
            i++;
        } else if (strcmp(argv[i], "--cd") == 0) {
            args->action = CLI_ACTION_CD;
            i++;
            if (i < argc) {
                args->target = dup_str(argv[i++]);
            }
            return parse_trailing_flags(argc, argv, &i, args);
        } else if (strcmp(argv[i], "--add") == 0) {
            args->action = CLI_ACTION_ADD;
            i++;
            if (i < argc) {
                args->extra_arg = dup_str(argv[i++]);
            }
            return true;
        } else if (strcmp(argv[i], "--complete-target") == 0) {
            // Called by the shell completion scripts; not listed in --help.
            args->action = CLI_ACTION_COMPLETE_TARGET;
            i++;
            args->target = dup_str(i < argc ? argv[i] : "");
            return true;
        } else if (strcmp(argv[i], "--complete-mark") == 0) {
            // Called by the shell completion scripts; not listed in --help.
            args->action = CLI_ACTION_COMPLETE_MARK;
            return true;
        } else if (strcmp(argv[i], "--resolve") == 0) {
            // Called by the shell completion scripts; not listed in --help.
            args->action = CLI_ACTION_RESOLVE;
            i++;
            if (i < argc) {
                args->target = dup_str(argv[i++]);
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
    } else if (strcmp(first, "reindex") == 0) {
        args->action = CLI_ACTION_REINDEX;
        return true;
    } else if (strcmp(first, "doctor") == 0) {
        args->action = CLI_ACTION_DOCTOR;
        return true;
    } else if (strcmp(first, "config") == 0) {
        i++;
        if (i >= argc || strcmp(argv[i], "edit") == 0 || strcmp(argv[i], "tui") == 0) {
            args->action = CLI_ACTION_CONFIG_EDIT;
        } else if (strcmp(argv[i], "show") == 0 || strcmp(argv[i], "print") == 0) {
            args->action = CLI_ACTION_CONFIG_SHOW;
        } else {
            jrun_log_error("unknown config subcommand '%s' (use show or edit)", argv[i]);
            return false;
        }
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
    } else if (strcmp(first, "mark") == 0) {
        i++;
        if (i >= argc || strcmp(argv[i], "list") == 0) {
            args->action = CLI_ACTION_MARK_LIST;
            return true;
        }
        args->action = CLI_ACTION_MARK_ADD;
        args->target = dup_str(argv[i++]);
        if (i < argc) {
            args->extra_arg = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "unmark") == 0) {
        args->action = CLI_ACTION_MARK_REMOVE;
        i++;
        if (i < argc) {
            args->target = dup_str(argv[i]);
        }
        return true;
    } else if (strcmp(first, "cd") == 0) {
        args->action = CLI_ACTION_CD;
        i++;
        if (i < argc) {
            args->target = dup_str(argv[i++]);
        }
        return parse_trailing_flags(argc, argv, &i, args);
    }

    // Otherwise, first argument is the target
    args->target = dup_str(argv[i++]);

    if (!parse_trailing_flags(argc, argv, &i, args)) return false;

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
