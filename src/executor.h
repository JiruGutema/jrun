#ifndef JRUN_EXECUTOR_H_
#define JRUN_EXECUTOR_H_

#include <stdbool.h>

// Safely executes command with argv[] in working_dir.
// Returns the exit code of the command process.
int executor_run(const char *working_dir, char *const argv[]);

#endif // JRUN_EXECUTOR_H_
