#ifndef JRUN_TUI_H_
#define JRUN_TUI_H_

#include <stdbool.h>
#include <stddef.h>
#include "resolver.h"
#include "config.h"

char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query);
bool tui_confirm_command(const char *working_dir, char *const argv[]);
bool tui_edit_config(Jrun_Config *config, const char *filepath);

#endif // JRUN_TUI_H_
