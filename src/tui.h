#ifndef JRUN_TUI_H_
#define JRUN_TUI_H_

#include <stdbool.h>
#include <stddef.h>
#include "resolver.h"
#include "config.h"

// True when an interactive selector can be shown. This asks whether a
// controlling terminal is reachable, NOT whether stdout is a terminal: jrun is
// nearly always run inside $(...) by the shell wrapper, where stdout is a pipe
// but /dev/tty is still perfectly usable.
bool tui_available(void);

// Returns the chosen path (caller frees), or NULL when the user cancelled.
char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query);

bool tui_confirm_command(const char *working_dir, char *const argv[]);
bool tui_edit_config(Jrun_Config *config, const char *filepath);

#endif // JRUN_TUI_H_
