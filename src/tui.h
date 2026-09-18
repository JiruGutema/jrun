#ifndef JRUN_TUI_H_
#define JRUN_TUI_H_

#include <stdbool.h>
#include <stddef.h>
#include "resolver.h"

// Runs the interactive TUI selector.
// Returns a dynamically allocated copy of the chosen path, or NULL if cancelled.
char *tui_select(const Resolve_Candidate *candidates, size_t count, const char *initial_query);

#endif // JRUN_TUI_H_
