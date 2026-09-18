#include "common.h"
#include "path_util.h"
#include "matcher.h"
#include "config.h"
#include "database.h"
#include "scanner.h"
#include "resolver.h"
#include "cli.h"
#include "shell.h"
#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

Log_Level g_log_level = LOG_LEVEL_NORMAL;

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s:%d: %s (%s)\n", __FILE__, __LINE__, #expr, msg); \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        g_tests_run++; \
        printf("Running %s... ", #fn); \
        fflush(stdout); \
        if (fn()) { \
            g_tests_passed++; \
            printf("\x1b[32mOK\x1b[0m\n"); \
        } else { \
            printf("\x1b[31mFAILED\x1b[0m\n"); \
        } \
    } while (0)

// 1. Path utilities tests
static bool test_path_utils(void)
{
    char out[PATH_MAX];

    // Tilde expansion
    ASSERT(path_expand_tilde("~", out, sizeof(out)), "expand ~");
    ASSERT(out[0] == '/', "~ should expand to absolute path");

    ASSERT(path_expand_tilde("~/development", out, sizeof(out)), "expand ~/development");
    ASSERT(strstr(out, "/development") != NULL, "~/development should contain /development");

    ASSERT(path_expand_tilde("/already/abs", out, sizeof(out)), "abs path");
    ASSERT(strcmp(out, "/already/abs") == 0, "abs path unchanged");

    // Shorten tilde
    const char *home = getenv("HOME");
    if (home) {
        char full_home_path[PATH_MAX];
        snprintf(full_home_path, sizeof(full_home_path), "%s/projects/foo", home);
        ASSERT(path_shorten_tilde(full_home_path, out, sizeof(out)), "shorten tilde");
        ASSERT(strcmp(out, "~/projects/foo") == 0, "should start with ~");
    }

    // Path normalization
    ASSERT(path_normalize("/a/b/../c/./d/", out, sizeof(out)), "normalize path");
    ASSERT(strcmp(out, "/a/c/d") == 0, "resolved . and ..");

    ASSERT(path_normalize("///var///log///", out, sizeof(out)), "normalize multiple slashes");
    ASSERT(strcmp(out, "/var/log") == 0, "multiple slashes collapsed");

    // Basename
    ASSERT(strcmp(path_basename("/home/user/development/hypr"), "hypr") == 0, "basename hypr");
    ASSERT(strcmp(path_basename("/home/user/development/hypr/"), "hypr") == 0, "basename with trailing slash");
    ASSERT(strcmp(path_basename("constituent"), "constituent") == 0, "basename single name");

    // Path with spaces
    ASSERT(path_normalize("/home/user/My Documents/Project A/..", out, sizeof(out)), "normalize path with spaces");
    ASSERT(strcmp(out, "/home/user/My Documents") == 0, "spaces in path preserved");

    return true;
}

// 2. Matcher tests
static bool test_matcher(void)
{
    // Exact basename match
    Match_Result m1 = matcher_evaluate("hypr", "/home/user/dotfiles/hypr");
    ASSERT(m1.is_match, "exact basename match");
    ASSERT(m1.is_exact_basename, "flag exact basename");
    ASSERT(m1.quality_score >= 100.0, "high score for exact match");

    // Case insensitivity
    Match_Result m2 = matcher_evaluate("HYPR", "/home/user/dotfiles/hypr");
    ASSERT(m2.is_match, "case insensitive exact match");
    ASSERT(m2.is_exact_basename, "flag exact basename case insensitive");

    // Prefix match
    Match_Result m3 = matcher_evaluate("const", "/home/user/development/constituent");
    ASSERT(m3.is_match, "prefix match const -> constituent");
    ASSERT(m3.quality_score >= 60.0, "prefix score");

    // Substring match
    Match_Result m4 = matcher_evaluate("stitu", "/home/user/development/constituent");
    ASSERT(m4.is_match, "substring match");
    ASSERT(m4.quality_score >= 40.0, "substring score");

    // Fuzzy subsequence match
    Match_Result m5 = matcher_evaluate("cstit", "/home/user/development/constituent");
    ASSERT(m5.is_match, "fuzzy subsequence cstit -> constituent");
    ASSERT(m5.quality_score >= 10.0, "fuzzy score");

    // Boundary bonus
    double score_boundary = 0.0;
    matcher_fuzzy_subsequence("op", "osta_pms", &score_boundary);
    ASSERT(score_boundary > 10.0, "boundary bonus for _pms");

    Match_Result m_b = matcher_evaluate("op", "/home/user/osta_pms");
    ASSERT(m_b.is_match, "fuzzy match on path with word boundaries");
    ASSERT(m_b.quality_score > 20.0, "quality score includes boundary bonus");

    // Non-matching
    Match_Result m6 = matcher_evaluate("completelyunrelated", "/home/user/development/constituent");
    ASSERT(!m6.is_match, "unrelated should not match");

    return true;
}

// 3. Config tests
static bool test_config(void)
{
    Jrun_Config cfg = {0};
    config_init_default(&cfg);

    ASSERT(cfg.roots_count >= 4, "default config should have roots");
    ASSERT(cfg.max_depth == DEFAULT_MAX_DEPTH, "default max depth");
    ASSERT(cfg.fuzzy == true, "default fuzzy true");
    ASSERT(cfg.interactive == true, "default interactive true");

    // Add root
    ASSERT(config_add_root(&cfg, "~/custom_projects"), "add root");
    ASSERT(config_has_root(&cfg, "~/custom_projects"), "has root");

    // Adding duplicate does not duplicate
    size_t count_before = cfg.roots_count;
    config_add_root(&cfg, "~/custom_projects");
    ASSERT(cfg.roots_count == count_before, "no duplicate roots");

    // Remove root
    ASSERT(config_remove_root(&cfg, "~/custom_projects"), "remove root");
    ASSERT(!config_has_root(&cfg, "~/custom_projects"), "root removed");

    // Save and load
    const char *tmp_cfg_path = "/tmp/jrun_test_config.toml";
    unlink(tmp_cfg_path);
    ASSERT(config_save(&cfg, tmp_cfg_path), "save config");

    Jrun_Config loaded = {0};
    ASSERT(config_load(&loaded, tmp_cfg_path), "load config");
    ASSERT(loaded.roots_count == cfg.roots_count, "roots count match");
    ASSERT(loaded.max_depth == cfg.max_depth, "max depth match");

    config_free(&cfg);
    config_free(&loaded);
    unlink(tmp_cfg_path);

    return true;
}

// 4. Database tests
static bool test_database(void)
{
    const char *tmp_db = "/tmp/jrun_test_db.sqlite";
    unlink(tmp_db);

    ASSERT(db_init(tmp_db), "init db");

    // Create temporary directory for real testing
    const char *test_dir1 = "/tmp/jrun_test_d1";
    const char *test_dir2 = "/tmp/jrun_test_d2";
    mkdir(test_dir1, 0755);
    mkdir(test_dir2, 0755);

    // Add entries
    ASSERT(db_add_or_update(test_dir1), "add test_dir1");
    ASSERT(db_add_or_update(test_dir1), "increment test_dir1 frequency");
    ASSERT(db_add_or_update(test_dir2), "add test_dir2");

    Db_Entry *entries = NULL;
    size_t count = 0;
    ASSERT(db_get_all(&entries, &count), "get all entries");
    ASSERT(count == 2, "2 entries in db");
    ASSERT(strcmp(entries[0].path, test_dir1) == 0, "test_dir1 has higher frequency so ranks first");
    ASSERT(entries[0].frequency >= 2.0, "frequency updated to 2");

    db_free_entries(entries, count);

    // Prune test: delete test_dir2 from filesystem and run prune
    rmdir(test_dir2);
    size_t pruned = 0;
    ASSERT(db_prune(&pruned), "prune deleted dirs");
    ASSERT(pruned == 1, "1 directory pruned");

    ASSERT(db_get_all(&entries, &count), "get entries after prune");
    ASSERT(count == 1, "only 1 entry remains");
    ASSERT(strcmp(entries[0].path, test_dir1) == 0, "test_dir1 remains");
    db_free_entries(entries, count);

    // Remove entry
    ASSERT(db_remove(test_dir1), "remove entry");
    ASSERT(db_get_all(&entries, &count), "get entries after remove");
    ASSERT(count == 0, "0 entries remain");
    db_free_entries(entries, count);

    // Cleanup
    rmdir(test_dir1);
    db_close();
    unlink(tmp_db);

    return true;
}

// 5. Scanner tests
static bool test_scanner(void)
{
    // Create temp test directory hierarchy
    system("rm -rf /tmp/jrun_scan_test && mkdir -p /tmp/jrun_scan_test/projA /tmp/jrun_scan_test/projB/subproj /tmp/jrun_scan_test/.git/ignored /tmp/jrun_scan_test/node_modules/pkg");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    // Clear default roots and set our test root
    for (size_t i = 0; i < cfg.roots_count; ++i) free(cfg.roots[i]);
    cfg.roots_count = 0;
    config_add_root(&cfg, "/tmp/jrun_scan_test");

    char **paths = NULL;
    size_t count = 0;
    ASSERT(scanner_scan_roots(&cfg, &paths, &count), "scan roots");

    bool found_projA = false;
    bool found_subproj = false;
    bool found_git = false;
    bool found_node_modules = false;

    for (size_t i = 0; i < count; ++i) {
        if (strstr(paths[i], "projA")) found_projA = true;
        if (strstr(paths[i], "subproj")) found_subproj = true;
        if (strstr(paths[i], ".git")) found_git = true;
        if (strstr(paths[i], "node_modules")) found_node_modules = true;
    }

    ASSERT(found_projA, "found projA");
    ASSERT(found_subproj, "found subproj");
    ASSERT(!found_git, ".git ignored");
    ASSERT(!found_node_modules, "node_modules ignored");

    scanner_free_paths(paths, count);
    config_free(&cfg);
    system("rm -rf /tmp/jrun_scan_test");

    return true;
}

// 6. Duplicate directory names and resolver ambiguity test
static bool test_duplicate_disambiguation(void)
{
    system("rm -rf /tmp/jrun_dup_test && mkdir -p /tmp/jrun_dup_test/rootA/constituent /tmp/jrun_dup_test/rootB/constituent /tmp/jrun_dup_test/rootC/constituent");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    for (size_t i = 0; i < cfg.roots_count; ++i) free(cfg.roots[i]);
    cfg.roots_count = 0;
    config_add_root(&cfg, "/tmp/jrun_dup_test/rootA");
    config_add_root(&cfg, "/tmp/jrun_dup_test/rootB");
    config_add_root(&cfg, "/tmp/jrun_dup_test/rootC");

    Resolve_Result res = resolver_resolve("constituent", &cfg, true);
    ASSERT(res.count == 3, "found all 3 duplicate constituent directories");
    ASSERT(res.status == RESOLVE_AMBIGUOUS, "multiple duplicate directories must trigger AMBIGUOUS / TUI");

    resolver_free_result(&res);
    config_free(&cfg);
    system("rm -rf /tmp/jrun_dup_test");

    return true;
}

// 7. CLI argument parsing tests
static bool test_cli_parsing(void)
{
    // Test 1: jrun hypr nvim
    {
        char *argv[] = {"jrun", "hypr", "nvim", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun hypr nvim");
        ASSERT(args.action == CLI_ACTION_EXECUTE, "execute action");
        ASSERT(strcmp(args.target, "hypr") == 0, "target is hypr");
        ASSERT(args.cmd_argc == 1, "1 cmd arg");
        ASSERT(strcmp(args.cmd_argv[0], "nvim") == 0, "cmd is nvim");
        cli_free_args(&args);
    }

    // Test 2: jrun hypr grep "blur_passes"
    {
        char *argv[] = {"jrun", "hypr", "grep", "blur_passes", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(4, argv, &args), "parse jrun hypr grep blur_passes");
        ASSERT(args.action == CLI_ACTION_EXECUTE, "execute action");
        ASSERT(strcmp(args.target, "hypr") == 0, "target is hypr");
        ASSERT(args.cmd_argc == 2, "2 cmd args");
        ASSERT(strcmp(args.cmd_argv[0], "grep") == 0, "cmd is grep");
        ASSERT(strcmp(args.cmd_argv[1], "blur_passes") == 0, "arg is blur_passes");
        cli_free_args(&args);
    }

    // Test 3: jrun hypr -- nvim
    {
        char *argv[] = {"jrun", "hypr", "--", "nvim", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(4, argv, &args), "parse jrun hypr -- nvim");
        ASSERT(args.action == CLI_ACTION_EXECUTE, "execute action");
        ASSERT(strcmp(args.target, "hypr") == 0, "target is hypr");
        ASSERT(args.cmd_argc == 1, "1 cmd arg");
        ASSERT(strcmp(args.cmd_argv[0], "nvim") == 0, "cmd is nvim");
        cli_free_args(&args);
    }

    // Test 4: jrun constituent (directory jump mode)
    {
        char *argv[] = {"jrun", "constituent", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(2, argv, &args), "parse jrun constituent");
        ASSERT(args.action == CLI_ACTION_CD, "cd action");
        ASSERT(strcmp(args.target, "constituent") == 0, "target is constituent");
        cli_free_args(&args);
    }

    // Test 5: jrun --cd constituent
    {
        char *argv[] = {"jrun", "--cd", "constituent", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun --cd constituent");
        ASSERT(args.action == CLI_ACTION_CD, "cd action");
        ASSERT(strcmp(args.target, "constituent") == 0, "target is constituent");
        cli_free_args(&args);
    }

    // Test 6: flags -i -d
    {
        char *argv[] = {"jrun", "-i", "-d", "hypr", "nvim", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(5, argv, &args), "parse flags");
        ASSERT(args.interactive == true, "interactive set");
        ASSERT(args.debug == true, "debug set");
        ASSERT(strcmp(args.target, "hypr") == 0, "target is hypr");
        cli_free_args(&args);
    }

    // Test 7: jrun tatr cd (explicit cd command suffix -> CD action)
    {
        char *argv[] = {"jrun", "tatr", "cd", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun tatr cd");
        ASSERT(args.action == CLI_ACTION_CD, "cd suffix sets CD action");
        ASSERT(strcmp(args.target, "tatr") == 0, "target is tatr");
        cli_free_args(&args);
    }

    // Test 8: jrun tatr open (open command defaults target to ".")
    {
        char *argv[] = {"jrun", "tatr", "open", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun tatr open");
        ASSERT(args.action == CLI_ACTION_EXECUTE, "execute action");
        ASSERT(args.cmd_argc == 2, "2 cmd args with '.' appended");
        ASSERT(strcmp(args.cmd_argv[0], "open") == 0, "cmd is open");
        ASSERT(strcmp(args.cmd_argv[1], ".") == 0, "arg is '.'");
        cli_free_args(&args);
    }

    return true;
}

// 8. Shell integration tests
static bool test_shell_integration(void)
{
    ASSERT(shell_parse_type("bash") == SHELL_BASH, "bash type");
    ASSERT(shell_parse_type("zsh") == SHELL_ZSH, "zsh type");
    ASSERT(shell_parse_type("fish") == SHELL_FISH, "fish type");
    ASSERT(shell_parse_type("invalid") == SHELL_UNKNOWN, "unknown type");
    return true;
}

// 9. Executor test
static bool test_executor(void)
{
    const char *test_dir = "/tmp";
    char *argv[] = {"echo", "jrun_executor_ok", NULL};
    int ret = executor_run(test_dir, argv);
    ASSERT(ret == 0, "executor run echo returned 0");

    char *bad_argv[] = {"non_existent_command_12345", NULL};
    int bad_ret = executor_run(test_dir, bad_argv);
    ASSERT(bad_ret == 127, "command not found returns 127");

    return true;
}

int main(void)
{
    printf("=== Running Jrun Test Suite ===\n\n");

    RUN_TEST(test_path_utils);
    RUN_TEST(test_matcher);
    RUN_TEST(test_config);
    RUN_TEST(test_database);
    RUN_TEST(test_scanner);
    RUN_TEST(test_duplicate_disambiguation);
    RUN_TEST(test_cli_parsing);
    RUN_TEST(test_shell_integration);
    RUN_TEST(test_executor);

    printf("\nTests summary: %d/%d passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
