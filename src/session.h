#ifndef JRUN_SESSION_H_
#define JRUN_SESSION_H_

#include <stdbool.h>
#include <stddef.h>

// Opens a tmux session rooted at `dir`: an existing session already rooted
// there is reused, otherwise one is created and named after the directory.
// Inside tmux the client switches to it; outside, jrun is replaced by
// `tmux attach`, so this only returns on failure or after a switch.
// Returns an exit status.
int session_open(const char *dir);

// Turns a directory name into a tmux session name. tmux reserves '.' and ':'
// in target names, so they become '_'.
void session_name_from_dir(const char *dir, char *out, size_t out_size);

#endif // JRUN_SESSION_H_
