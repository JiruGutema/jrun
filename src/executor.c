#include "executor.h"
#include "common.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

int executor_run(const char *working_dir, char *const argv[])
{
    if (!working_dir || !argv || !argv[0]) {
        jrun_log_error("invalid arguments to executor_run");
        return 1;
    }

    jrun_log_debug("changing directory to '%s' and executing '%s'", working_dir, argv[0]);

    pid_t pid = fork();
    if (pid < 0) {
        jrun_log_error("failed to fork process: %s", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        // Child process
        if (chdir(working_dir) != 0) {
            fprintf(stderr, "jrun: failed to change working directory to '%s': %s\n", working_dir, strerror(errno));
            _exit(1);
        }

        execvp(argv[0], argv);

        // If execvp returns, an error occurred
        if (errno == ENOENT) {
            fprintf(stderr, "jrun: command not found: %s\n", argv[0]);
            _exit(127);
        } else if (errno == EACCES) {
            fprintf(stderr, "jrun: permission denied: %s\n", argv[0]);
            _exit(126);
        } else {
            fprintf(stderr, "jrun: failed to execute '%s': %s\n", argv[0], strerror(errno));
            _exit(1);
        }
    }

    // Parent process
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            jrun_log_error("waitpid error: %s", strerror(errno));
            return 1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 0;
}
