#define NOB_IMPLEMENTATION
#include "thirdparty/nob.h"

#define BUILD_FOLDER "build/"
#define SRC_FOLDER "src/"
#define TESTS_FOLDER "tests/"

static const char *get_sqlite_lib(void)
{
    return "-lsqlite3";
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    bool run_tests = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-test") == 0 || strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "test") == 0) {
            run_tests = true;
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
        SRC_FOLDER"path_util.c",
    };
    size_t common_count = sizeof(common_srcs) / sizeof(common_srcs[0]);

    // Build jrun
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc");
    nob_cmd_append(&cmd, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-unused-parameter");
    nob_cmd_append(&cmd, "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE");
    nob_cmd_append(&cmd, "-O3");
    nob_cmd_append(&cmd, "-Isrc", "-Ithirdparty", "-Ithirdparty/sqlite3");
    nob_cmd_append(&cmd, "-o", BUILD_FOLDER"jrun");
    nob_cmd_append(&cmd, SRC_FOLDER"main.c");
    for (size_t i = 0; i < common_count; ++i) {
        nob_cmd_append(&cmd, common_srcs[i]);
    }
    nob_cmd_append(&cmd, get_sqlite_lib(), "-lm", "-lpthread");

    if (!nob_cmd_run_sync(cmd)) return 1;

    if (run_tests) {
        cmd.count = 0;
        nob_cmd_append(&cmd, "cc");
        nob_cmd_append(&cmd, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-unused-parameter");
        nob_cmd_append(&cmd, "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE");
        nob_cmd_append(&cmd, "-Isrc", "-Ithirdparty", "-Ithirdparty/sqlite3");
        nob_cmd_append(&cmd, "-o", BUILD_FOLDER"test_jrun");
        nob_cmd_append(&cmd, TESTS_FOLDER"test_main.c");
        for (size_t i = 0; i < common_count; ++i) {
            nob_cmd_append(&cmd, common_srcs[i]);
        }
        nob_cmd_append(&cmd, get_sqlite_lib(), "-lm", "-lpthread");

        if (!nob_cmd_run_sync(cmd)) return 1;

        cmd.count = 0;
        nob_cmd_append(&cmd, BUILD_FOLDER"test_jrun");
        if (!nob_cmd_run_sync(cmd)) return 1;
    }

    return 0;
}
