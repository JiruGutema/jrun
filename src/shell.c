#include "shell.h"
#include "common.h"

#include <stdio.h>
#include <string.h>

Shell_Type shell_parse_type(const char *name)
{
    if (!name) return SHELL_UNKNOWN;
    if (strcasecmp(name, "bash") == 0) return SHELL_BASH;
    if (strcasecmp(name, "zsh") == 0) return SHELL_ZSH;
    if (strcasecmp(name, "fish") == 0) return SHELL_FISH;
    return SHELL_UNKNOWN;
}

// The hooks record the working directory only when it actually changed.
// Firing `jrun add` from every prompt spawns a process and opens the database
// on each keystroke-to-prompt cycle, which is pure overhead when the user has
// not moved.
static const char *BASH_HOOK =
"# jrun shell integration for bash\n"
"__jrun_last_pwd=\"$PWD\"\n"
"__jrun_track() {\n"
"    local __jrun_status=$?\n"
"    if [ \"$PWD\" != \"$__jrun_last_pwd\" ]; then\n"
"        __jrun_last_pwd=\"$PWD\"\n"
"        command jrun add \"$PWD\" >/dev/null 2>&1\n"
"    fi\n"
"    return $__jrun_status\n"
"}\n"
"\n"
"# bash 5.1+ allows PROMPT_COMMAND to be an array; append to it properly so we\n"
"# neither clobber another tool's hook nor corrupt the array form.\n"
"if [ -n \"${BASH_VERSION:-}\" ]; then\n"
"    if [[ \"$(declare -p PROMPT_COMMAND 2>/dev/null)\" == \"declare -a\"* ]]; then\n"
"        for __jrun_i in \"${PROMPT_COMMAND[@]}\"; do\n"
"            [ \"$__jrun_i\" = \"__jrun_track\" ] && __jrun_found=1\n"
"        done\n"
"        [ -z \"${__jrun_found:-}\" ] && PROMPT_COMMAND+=(__jrun_track)\n"
"        unset __jrun_i __jrun_found\n"
"    else\n"
"        case \"${PROMPT_COMMAND:-}\" in\n"
"            *__jrun_track*) ;;\n"
"            \"\") PROMPT_COMMAND=\"__jrun_track\" ;;\n"
"            *)  PROMPT_COMMAND=\"__jrun_track;${PROMPT_COMMAND}\" ;;\n"
"        esac\n"
"    fi\n"
"fi\n";

static const char *ZSH_HOOK =
"# jrun shell integration for zsh\n"
"__jrun_track() {\n"
"    command jrun add \"$PWD\" >/dev/null 2>&1\n"
"}\n"
"\n"
"autoload -Uz add-zsh-hook 2>/dev/null\n"
"if typeset -f add-zsh-hook >/dev/null; then\n"
"    add-zsh-hook chpwd __jrun_track\n"
"else\n"
"    chpwd_functions+=(\"__jrun_track\")\n"
"fi\n";

// Only `j` is defined. Shadowing `jrun` itself with a function was surprising:
// it silently changed what `jrun <target>` did compared to running the binary,
// and made the documented "print the resolved path" behaviour unreachable.
static const char *COMMON_POSIX_WRAPPER =
"\n"
"# j <target>           cd to the best match\n"
"# j <target> <cmd>...  run a command there (delegates to the jrun binary)\n"
"# j -                  cd to the previous directory\n"
"# j                    cd to $HOME\n"
"j() {\n"
"    if [ \"$#\" -eq 0 ]; then\n"
"        builtin cd \"$HOME\" || return\n"
"        return 0\n"
"    fi\n"
"\n"
"    if [ \"$#\" -eq 1 ] && [ \"$1\" = \"-\" ]; then\n"
"        builtin cd - || return\n"
"        return 0\n"
"    fi\n"
"\n"
"    # Subcommands and flags go straight through to the binary.\n"
"    case \"$1\" in\n"
"        -h|--help|-v|--version|add|remove|list|query|prune|reindex|doctor|config|root|help|version)\n"
"            command jrun \"$@\"\n"
"            return $?\n"
"            ;;\n"
"    esac\n"
"\n"
"    __jrun_cd() {\n"
"        local __jrun_dir __jrun_rc\n"
"        # The selector renders on /dev/tty, so it still appears even though\n"
"        # stdout is captured here.\n"
"        __jrun_dir=$(command jrun --cd \"$@\") || return $?\n"
"        [ -n \"$__jrun_dir\" ] || return 1\n"
"        builtin cd \"$__jrun_dir\" || return $?\n"
"        __jrun_last_pwd=\"$PWD\"\n"
"        command jrun add \"$PWD\" >/dev/null 2>&1\n"
"        return 0\n"
"    }\n"
"\n"
"    if [ \"$1\" = \"cd\" ] || [ \"$1\" = \"--cd\" ]; then\n"
"        shift\n"
"        __jrun_cd \"$@\"\n"
"        return $?\n"
"    fi\n"
"\n"
"    if [ \"$1\" = \"-i\" ] || [ \"$1\" = \"--interactive\" ]; then\n"
"        shift\n"
"        if [ \"$#\" -eq 1 ]; then\n"
"            local __jrun_dir\n"
"            __jrun_dir=$(command jrun -i --cd \"$1\") || return $?\n"
"            [ -n \"$__jrun_dir\" ] || return 1\n"
"            builtin cd \"$__jrun_dir\" || return $?\n"
"            __jrun_last_pwd=\"$PWD\"\n"
"            command jrun add \"$PWD\" >/dev/null 2>&1\n"
"            return 0\n"
"        fi\n"
"        command jrun -i \"$@\"\n"
"        return $?\n"
"    fi\n"
"\n"
"    if [ \"$#\" -eq 2 ] && [ \"$2\" = \"cd\" ]; then\n"
"        __jrun_cd \"$1\"\n"
"        return $?\n"
"    fi\n"
"\n"
"    if [ \"$#\" -eq 1 ]; then\n"
"        __jrun_cd \"$1\"\n"
"        return $?\n"
"    fi\n"
"\n"
"    # Two or more arguments: target plus a command to run there.\n"
"    command jrun \"$@\"\n"
"}\n";

static const char *FISH_INIT =
"# jrun shell integration for fish\n"
"function __jrun_track --on-variable PWD\n"
"    status is-command-substitution; and return\n"
"    test -d \"$PWD\"; and command jrun add \"$PWD\" >/dev/null 2>&1\n"
"end\n"
"\n"
"function __jrun_cd\n"
"    set -l dir (command jrun --cd $argv)\n"
"    set -l rc $status\n"
"    test $rc -eq 0; or return $rc\n"
"    test -n \"$dir\"; or return 1\n"
"    builtin cd \"$dir\"; or return $status\n"
"    return 0\n"
"end\n"
"\n"
"function j --description 'jump to a project directory'\n"
"    set -l argc (count $argv)\n"
"    if test $argc -eq 0\n"
"        builtin cd $HOME\n"
"        return 0\n"
"    end\n"
"\n"
"    if test $argc -eq 1 -a \"$argv[1]\" = \"-\"\n"
"        builtin cd -\n"
"        return 0\n"
"    end\n"
"\n"
"    switch \"$argv[1]\"\n"
"        case -h --help -v --version add remove list query prune reindex doctor config root help version\n"
"            command jrun $argv\n"
"            return $status\n"
"    end\n"
"\n"
"    if test \"$argv[1]\" = \"cd\" -o \"$argv[1]\" = \"--cd\"\n"
"        set -e argv[1]\n"
"        __jrun_cd $argv\n"
"        return $status\n"
"    end\n"
"\n"
"    if test \"$argv[1]\" = \"-i\" -o \"$argv[1]\" = \"--interactive\"\n"
"        set -e argv[1]\n"
"        if test (count $argv) -eq 1\n"
"            set -l dir (command jrun -i --cd \"$argv[1]\")\n"
"            test $status -eq 0 -a -n \"$dir\"; and builtin cd \"$dir\"\n"
"            return $status\n"
"        end\n"
"        command jrun -i $argv\n"
"        return $status\n"
"    end\n"
"\n"
"    if test $argc -eq 2 -a \"$argv[2]\" = \"cd\"\n"
"        __jrun_cd \"$argv[1]\"\n"
"        return $status\n"
"    end\n"
"\n"
"    if test $argc -eq 1\n"
"        __jrun_cd \"$argv[1]\"\n"
"        return $status\n"
"    end\n"
"\n"
"    command jrun $argv\n"
"end\n";

bool shell_generate_init(Shell_Type shell)
{
    switch (shell) {
    case SHELL_BASH:
        printf("%s%s", BASH_HOOK, COMMON_POSIX_WRAPPER);
        return true;
    case SHELL_ZSH:
        printf("%s%s", ZSH_HOOK, COMMON_POSIX_WRAPPER);
        return true;
    case SHELL_FISH:
        printf("%s", FISH_INIT);
        return true;
    case SHELL_UNKNOWN:
    default:
        jrun_log_error("unsupported shell. Supported shells: bash, zsh, fish");
        return false;
    }
}
