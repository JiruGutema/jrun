#ifndef JRUN_SHELL_H_
#define JRUN_SHELL_H_

#include <stdbool.h>

typedef enum {
    SHELL_UNKNOWN = 0,
    SHELL_BASH,
    SHELL_ZSH,
    SHELL_FISH
} Shell_Type;

Shell_Type shell_parse_type(const char *name);
bool shell_generate_init(Shell_Type shell);

#endif // JRUN_SHELL_H_
