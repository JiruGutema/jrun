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

    // While the child runs it owns the terminal, so Ctrl-C and Ctrl-\ belong to
    // it. Without this, the signal kills jrun too and the child is orphaned
    // mid-edit; ignoring them here is what system(3) does for the same reason.
    struct sigaction ignore, old_int, old_quit;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGINT, &ignore, &old_int);
    sigaction(SIGQUIT, &ignore, &old_quit);

    pid_t pid = fork();
    if (pid < 0) {
        jrun_log_error("failed to fork process: %s", strerror(errno));
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        return 1;
    }

    if (pid == 0) {
        // Child process. Restore the default dispositions first so the command
        // we exec does not inherit jrun's ignored signals.
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);

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
            sigaction(SIGINT, &old_int, NULL);
            sigaction(SIGQUIT, &old_quit, NULL);
            return 1;
        }
    }

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 0;
}
