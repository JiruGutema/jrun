#define NOB_IMPLEMENTATION
#include "thirdparty/nob.h"

#define BUILD_FOLDER "build/"
#define SRC_FOLDER "src/"
#define TESTS_FOLDER "tests/"

// Kept in step with the Makefile: same standard, same warnings, same -Werror.
// A build that only fails under one of the two build systems is worse than no
// second build system at all.
static void common_flags(Nob_Cmd *cmd)
{
    nob_cmd_append(cmd, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   "-Wno-unused-parameter");
    nob_cmd_append(cmd, "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE");
    nob_cmd_append(cmd, "-Isrc", "-Ithirdparty");
    // Fall back to the vendored sqlite3.h when the system ships no dev headers.
    // Mirrors the probe in the Makefile.
    if (!nob_file_exists("/usr/include/sqlite3.h")) {
        nob_cmd_append(cmd, "-Ithirdparty/sqlite3");
    }
}

// Distributions increasingly ship libsqlite3.so.0 without the -dev symlink
// that plain -lsqlite3 needs, so probe before trusting it.
static const char *get_sqlite_lib(void)
{
    static const char *candidates[] = {
        "/usr/lib/x86_64-linux-gnu/libsqlite3.so.0",
        "/usr/lib/aarch64-linux-gnu/libsqlite3.so.0",
        "/usr/lib/libsqlite3.so.0",
        "/usr/lib64/libsqlite3.so.0",
        NULL,
    };

    if (nob_file_exists("/usr/lib/x86_64-linux-gnu/libsqlite3.so") ||
        nob_file_exists("/usr/lib/libsqlite3.so") ||
        nob_file_exists("/usr/lib64/libsqlite3.so")) {
        return "-lsqlite3";
    }

    for (size_t i = 0; candidates[i]; ++i) {
        if (nob_file_exists(candidates[i])) return candidates[i];
    }
    return "-lsqlite3";
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    bool run_tests = false;
    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-test") == 0 || strcmp(argv[i], "--test") == 0 ||
            strcmp(argv[i], "test") == 0) {
            run_tests = true;
        } else if (strcmp(argv[i], "-debug") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug = true;
        }
    }

    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) return 1;

    const char *common_srcs[] = {
        SRC_FOLDER"cli.c",
        SRC_FOLDER"config.c",
        SRC_FOLDER"database.c",
        SRC_FOLDER"scanner.c",
        SRC_FOLDER"matcher.c",
        SRC_FOLDER"resolver.c",
        SRC_FOLDER"tui.c",
        SRC_FOLDER"executor.c",
        SRC_FOLDER"shell.c",
        SRC_FOLDER"session.c",
        SRC_FOLDER"path_util.c",
    };
    size_t common_count = sizeof(common_srcs) / sizeof(common_srcs[0]);
    const char *sqlite_lib = get_sqlite_lib();

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc");
    common_flags(&cmd);
    if (debug) nob_cmd_append(&cmd, "-O0", "-g3", "-DDEBUG");
    else nob_cmd_append(&cmd, "-O2", "-DNDEBUG");
    nob_cmd_append(&cmd, "-o", BUILD_FOLDER"jrun");
    nob_cmd_append(&cmd, SRC_FOLDER"main.c");
    for (size_t i = 0; i < common_count; ++i) {
        nob_cmd_append(&cmd, common_srcs[i]);
    }
    nob_cmd_append(&cmd, sqlite_lib, "-lm", "-lpthread");

    if (!nob_cmd_run_sync(cmd)) return 1;

    if (run_tests) {
        cmd.count = 0;
        nob_cmd_append(&cmd, "cc");
        common_flags(&cmd);
        nob_cmd_append(&cmd, "-O1", "-g");
        nob_cmd_append(&cmd, "-o", BUILD_FOLDER"test_jrun");
        nob_cmd_append(&cmd, TESTS_FOLDER"test_main.c");
        for (size_t i = 0; i < common_count; ++i) {
            nob_cmd_append(&cmd, common_srcs[i]);
        }
        nob_cmd_append(&cmd, sqlite_lib, "-lm", "-lpthread");

        if (!nob_cmd_run_sync(cmd)) return 1;

        cmd.count = 0;
        nob_cmd_append(&cmd, BUILD_FOLDER"test_jrun");
        if (!nob_cmd_run_sync(cmd)) return 1;
    }

    return 0;
}
