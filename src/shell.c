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

static const char *BASH_HOOK =
"# jrun shell integration for bash\n"
"__jrun_prompt_command() {\n"
"    local status=$?\n"
"    command jrun add \"$PWD\" >/dev/null 2>&1\n"
"    return $status\n"
"}\n"
"\n"
"if [[ ! \"$PROMPT_COMMAND\" =~ __jrun_prompt_command ]]; then\n"
"    PROMPT_COMMAND=\"__jrun_prompt_command;${PROMPT_COMMAND:-}\"\n"
"fi\n";

static const char *ZSH_HOOK =
"# jrun shell integration for zsh\n"
"__jrun_chpwd() {\n"
"    command jrun add \"$PWD\" >/dev/null 2>&1\n"
"}\n"
"\n"
"autoload -U add-zsh-hook 2>/dev/null\n"
"if typeset -f add-zsh-hook >/dev/null; then\n"
"    add-zsh-hook chpwd __jrun_chpwd\n"
"else\n"
"    chpwd_functions+=(\"__jrun_chpwd\")\n"
"fi\n";

static const char *COMMON_POSIX_WRAPPER =
"\n"
"__jrun_wrapper() {\n"
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
"    if [ \"$1\" = \"cd\" ]; then\n"
"        shift\n"
"        local target_dir\n"
"        target_dir=\"$(command jrun --cd \"$@\")\"\n"
"        local ret=$?\n"
"        if [ $ret -eq 0 ] && [ -n \"$target_dir\" ]; then\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\" || return\n"
"        fi\n"
"        return $ret\n"
"    fi\n"
"\n"
"    if [ \"$#\" -eq 2 ] && [ \"$2\" = \"cd\" ]; then\n"
"        local target_dir\n"
"        target_dir=\"$(command jrun --cd \"$1\")\"\n"
"        local ret=$?\n"
"        if [ $ret -eq 0 ] && [ -n \"$target_dir\" ]; then\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\" || return\n"
"        fi\n"
"        return $ret\n"
"    fi\n"
"\n"
"    if [ \"$1\" = \"--cd\" ]; then\n"
"        shift\n"
"        local target_dir\n"
"        target_dir=\"$(command jrun --cd \"$@\")\"\n"
"        local ret=$?\n"
"        if [ $ret -eq 0 ] && [ -n \"$target_dir\" ]; then\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\" || return\n"
"        fi\n"
"        return $ret\n"
"    fi\n"
"\n"
"    if [ \"$1\" = \"-i\" ] || [ \"$1\" = \"--interactive\" ]; then\n"
"        if [ \"$#\" -eq 2 ]; then\n"
"            local target_dir\n"
"            target_dir=\"$(command jrun -i --cd \"$2\")\"\n"
"            local ret=$?\n"
"            if [ $ret -eq 0 ] && [ -n \"$target_dir\" ]; then\n"
"                echo \"$target_dir\"\n"
"                builtin cd \"$target_dir\" || return\n"
"            fi\n"
"            return $ret\n"
"        fi\n"
"    fi\n"
"\n"
"    if [ \"$#\" -eq 1 ]; then\n"
"        case \"$1\" in\n"
"            -h|--help|-v|--version|list|prune|doctor|config|root|add|remove|query|help|version)\n"
"                command jrun \"$@\"\n"
"                return $?\n"
"                ;;\n"
"            *)\n"
"                local target_dir\n"
"                target_dir=\"$(command jrun --cd \"$1\")\"\n"
"                local ret=$?\n"
"                if [ $ret -eq 0 ] && [ -n \"$target_dir\" ]; then\n"
"                    echo \"$target_dir\"\n"
"                    builtin cd \"$target_dir\" || return\n"
"                fi\n"
"                return $ret\n"
"                ;;\n"
"        esac\n"
"    fi\n"
"\n"
"    command jrun \"$@\"\n"
"}\n"
"\n"
"jrun() {\n"
"    __jrun_wrapper \"$@\"\n"
"}\n"
"\n"
"j() {\n"
"    __jrun_wrapper \"$@\"\n"
"}\n";

static const char *FISH_INIT =
"# jrun shell integration for fish\n"
"function __jrun_hook --on-variable PWD\n"
"    test -d \"$PWD\"; and command jrun add \"$PWD\" >/dev/null 2>&1\n"
"end\n"
"\n"
"function __jrun_wrapper\n"
"    set -l argc (count $argv)\n"
"    if test $argc -eq 0\n"
"        builtin cd $HOME\n"
"        return 0\n"
"    else if test $argc -eq 1 -a \"$argv[1]\" = \"-\"\n"
"        builtin cd -\n"
"        return 0\n"
"    else if test $argc -eq 2 -a \"$argv[2]\" = \"cd\"\n"
"        set -l target_dir (command jrun --cd \"$argv[1]\")\n"
"        set -l ret $status\n"
"        if test $ret -eq 0 -a -n \"$target_dir\"\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\"\n"
"        end\n"
"        return $ret\n"
"    else if test $argc -ge 1 -a \"$argv[1]\" = \"cd\"\n"
"        set -e argv[1]\n"
"        set -l target_dir (command jrun --cd $argv)\n"
"        set -l ret $status\n"
"        if test $ret -eq 0 -a -n \"$target_dir\"\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\"\n"
"        end\n"
"        return $ret\n"
"    else if test $argc -ge 1 -a \"$argv[1]\" = \"--cd\"\n"
"        set -e argv[1]\n"
"        set -l target_dir (command jrun --cd $argv)\n"
"        set -l ret $status\n"
"        if test $ret -eq 0 -a -n \"$target_dir\"\n"
"            echo \"$target_dir\"\n"
"            builtin cd \"$target_dir\"\n"
"        end\n"
"        return $ret\n"
"    else if test $argc -eq 1\n"
"        switch \"$argv[1]\"\n"
"            case -h --help -v --version list prune doctor config root add remove query help version\n"
"                command jrun $argv\n"
"                return $status\n"
"            case '*'\n"
"                set -l target_dir (command jrun --cd \"$argv[1]\")\n"
"                set -l ret $status\n"
"                if test $ret -eq 0 -a -n \"$target_dir\"\n"
"                    echo \"$target_dir\"\n"
"                    builtin cd \"$target_dir\"\n"
"                end\n"
"                return $ret\n"
"        end\n"
"    else\n"
"        command jrun $argv\n"
"    end\n"
"end\n"
"\n"
"function jrun\n"
"    __jrun_wrapper $argv\n"
"end\n"
"\n"
"function j\n"
"    __jrun_wrapper $argv\n"
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
