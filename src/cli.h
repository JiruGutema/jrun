#ifndef JRUN_CLI_H_
#define JRUN_CLI_H_

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CLI_ACTION_EXECUTE,
    CLI_ACTION_CD,
    CLI_ACTION_ADD,
    CLI_ACTION_REMOVE,
    CLI_ACTION_LIST,
    CLI_ACTION_QUERY,
    CLI_ACTION_PRUNE,
    CLI_ACTION_REINDEX,
    CLI_ACTION_DOCTOR,
    CLI_ACTION_CONFIG_SHOW,
    CLI_ACTION_CONFIG_EDIT,
    CLI_ACTION_ROOT_ADD,
    CLI_ACTION_ROOT_REMOVE,
    CLI_ACTION_ROOT_LIST,
    CLI_ACTION_MARK_LIST,
    CLI_ACTION_MARK_ADD,
    CLI_ACTION_MARK_REMOVE,
    CLI_ACTION_COMPLETE_MARK,
    CLI_ACTION_INIT_SHELL,
    CLI_ACTION_COMPLETE_TARGET,
    CLI_ACTION_RESOLVE,
    CLI_ACTION_HELP,
    CLI_ACTION_VERSION,
} Cli_Action;

typedef struct {
    Cli_Action action;
    char *target;
    char **cmd_argv;
    int cmd_argc;
    char *extra_arg;
    bool interactive;
    bool debug;
    bool quiet;
    bool yes;
    // Open a tmux session in the directory instead of jumping to it.
    bool session;
    // Run the command in every directory named exactly like the target.
    bool all;
    // Pick the directories to run the command in from the selector.
    bool multi;
} Cli_Args;

bool cli_parse(int argc, char **argv, Cli_Args *args);
void cli_print_help(const char *prog_name);
void cli_print_version(void);
void cli_free_args(Cli_Args *args);

// True for words jrun reads as a subcommand, which therefore cannot be used
// as bookmark names.
bool cli_is_reserved_word(const char *word);

#endif // JRUN_CLI_H_
