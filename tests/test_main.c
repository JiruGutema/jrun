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
#include <ftw.h>

Log_Level g_log_level = LOG_LEVEL_QUIET;

static int g_tests_run = 0;
static int g_tests_passed = 0;

// Every test works inside this directory, created fresh per run by mkdtemp and
// removed at the end. Hard-coded /tmp paths used to collide between concurrent
// runs and failed outright when another user owned the leftovers.
// Sized to the mkdtemp template below, which also lets the compiler prove
// the snprintf() calls that build paths from it cannot truncate.
static char g_sandbox[64];

#define ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "\n  FAIL: %s:%d: %s (%s)\n", __FILE__, __LINE__, #expr, msg); \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        g_tests_run++; \
        printf("  %-38s ", #fn); \
        fflush(stdout); \
        if (fn()) { \
            g_tests_passed++; \
            printf("\x1b[32mok\x1b[0m\n"); \
        } else { \
            printf("\x1b[31mFAILED\x1b[0m\n"); \
        } \
    } while (0)

// --- sandbox helpers ---------------------------------------------------------

// Builds an absolute path inside the sandbox.
//
// Rotates through a small ring of buffers so several results can be live at
// once -- `f(sb("a"), sb("b"))` with a single shared buffer would silently
// hand the same string to both parameters.
static const char *sb(const char *relative)
{
    static char ring[8][PATH_MAX];
    static size_t next = 0;
    char *buf = ring[next];
    next = (next + 1) % 8;
    snprintf(buf, sizeof(ring[0]), "%s/%s", g_sandbox, relative);
    return buf;
}

static int unlink_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

// Recursive delete without shelling out. The old suite called
// system("rm -rf ...") — the very thing jrun asks for confirmation before
// running, and a command injection risk the moment a path is not a literal.
static void rm_rf(const char *path)
{
    nftw(path, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static bool mkdirs(const char *path)
{
    return path_mkdir_p(path);
}

// Creates an empty file, making parent directories as needed.
static bool touch(const char *path)
{
    if (!path_ensure_parent_dir(path)) return false;
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fclose(fp);
    return true;
}

// Replaces a config's roots with exactly the ones given.
static void set_roots(Jrun_Config *cfg, const char *const *roots, size_t n)
{
    for (size_t i = 0; i < cfg->roots_count; ++i) free(cfg->roots[i]);
    cfg->roots_count = 0;
    for (size_t i = 0; i < n; ++i) config_add_root(cfg, roots[i]);
    // Tests want deterministic results, not whatever a previous run cached.
    cfg->cache_ttl = 0;
}

// --- 1. path utilities -------------------------------------------------------

static bool test_path_utils(void)
{
    char out[PATH_MAX];

    ASSERT(path_expand_tilde("~", out, sizeof(out)), "expand ~");
    ASSERT(out[0] == '/', "~ should expand to an absolute path");

    ASSERT(path_expand_tilde("~/development", out, sizeof(out)), "expand ~/development");
    ASSERT(strstr(out, "/development") != NULL, "~/development should contain /development");

    ASSERT(path_expand_tilde("/already/abs", out, sizeof(out)), "abs path");
    ASSERT(strcmp(out, "/already/abs") == 0, "abs path unchanged");

    // HOME is pinned by main(), so this is exact rather than best-effort.
    const char *home = getenv("HOME");
    ASSERT(home != NULL, "HOME is set");
    char full_home_path[PATH_MAX];
    snprintf(full_home_path, sizeof(full_home_path), "%s/projects/foo", home);
    ASSERT(path_shorten_tilde(full_home_path, out, sizeof(out)), "shorten tilde");
    ASSERT(strcmp(out, "~/projects/foo") == 0, "should shorten to ~/projects/foo");

    ASSERT(path_normalize("/a/b/../c/./d/", out, sizeof(out)), "normalize path");
    ASSERT(strcmp(out, "/a/c/d") == 0, "resolved . and ..");

    ASSERT(path_normalize("///var///log///", out, sizeof(out)), "normalize multiple slashes");
    ASSERT(strcmp(out, "/var/log") == 0, "multiple slashes collapsed");

    ASSERT(path_normalize("/..", out, sizeof(out)), "normalize above root");
    ASSERT(strcmp(out, "/") == 0, ".. cannot escape /");

    char b1[PATH_MAX];
    char b2[PATH_MAX];
    ASSERT(strcmp(path_basename_r("/home/user/development/hypr", b1, sizeof(b1)), "hypr") == 0,
           "basename hypr");
    ASSERT(strcmp(path_basename_r("/home/user/development/hypr/", b1, sizeof(b1)), "hypr") == 0,
           "basename with trailing slash");
    ASSERT(strcmp(path_basename_r("constituent", b1, sizeof(b1)), "constituent") == 0,
           "basename single name");
    ASSERT(strcmp(path_basename_r("/", b1, sizeof(b1)), "") == 0, "basename of root");

    // The reentrant form is the whole point: two live results at once.
    path_basename_r("/a/alpha", b1, sizeof(b1));
    path_basename_r("/b/beta", b2, sizeof(b2));
    ASSERT(strcmp(b1, "alpha") == 0 && strcmp(b2, "beta") == 0,
           "two basenames can coexist");

    ASSERT(path_normalize("/home/user/My Documents/Project A/..", out, sizeof(out)),
           "normalize path with spaces");
    ASSERT(strcmp(out, "/home/user/My Documents") == 0, "spaces in path preserved");

    return true;
}

static bool test_path_atomic_write(void)
{
    const char *target = sb("atomic/file.txt");
    ASSERT(path_ensure_parent_dir(target), "create parent dir");

    ASSERT(path_write_atomic(target, "hello", 5), "write atomically");

    FILE *fp = fopen(target, "r");
    ASSERT(fp != NULL, "file exists");
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    ASSERT(n == 5 && strcmp(buf, "hello") == 0, "content written");

    // Overwriting must replace, never append or leave a stray temp file.
    ASSERT(path_write_atomic(target, "bye", 3), "overwrite atomically");
    fp = fopen(target, "r");
    ASSERT(fp != NULL, "file still exists");
    memset(buf, 0, sizeof(buf));
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    ASSERT(n == 3 && strcmp(buf, "bye") == 0, "content replaced");

    return true;
}

static bool test_project_detection(void)
{
    ASSERT(mkdirs(sb("proj/rustapp")), "make rust dir");
    ASSERT(touch(sb("proj/rustapp/Cargo.toml")), "make Cargo.toml");
    ASSERT(mkdirs(sb("proj/plain")), "make plain dir");
    ASSERT(mkdirs(sb("proj/repo/.git")), "make git dir");

    const char *kind = path_project_kind(sb("proj/rustapp"));
    ASSERT(kind != NULL && strcmp(kind, "rust") == 0, "Cargo.toml reports rust");

    ASSERT(path_project_kind(sb("proj/plain")) == NULL, "plain directory has no marker");

    kind = path_project_kind(sb("proj/repo"));
    ASSERT(kind != NULL && strcmp(kind, "git") == 0, ".git reports git");

    ASSERT(path_is_project_dir(sb("proj/repo")), "repo is a project");
    ASSERT(!path_is_project_dir(sb("proj/plain")), "plain is not a project");

    return true;
}

// --- 2. matcher --------------------------------------------------------------

static bool test_matcher(void)
{
    Match_Result m1 = matcher_evaluate("hypr", "/home/user/dotfiles/hypr");
    ASSERT(m1.is_match, "exact basename match");
    ASSERT(m1.is_exact_basename, "flag exact basename");
    ASSERT(m1.quality_score >= 100.0, "high score for exact match");

    Match_Result m2 = matcher_evaluate("HYPR", "/home/user/dotfiles/hypr");
    ASSERT(m2.is_match, "case insensitive exact match");
    ASSERT(m2.is_exact_basename, "flag exact basename case insensitive");

    Match_Result m3 = matcher_evaluate("const", "/home/user/development/constituent");
    ASSERT(m3.is_match, "prefix match const -> constituent");
    ASSERT(m3.quality_score >= 60.0, "prefix score");

    Match_Result m4 = matcher_evaluate("stitu", "/home/user/development/constituent");
    ASSERT(m4.is_match, "substring match");
    ASSERT(m4.quality_score >= 40.0, "substring score");

    Match_Result m5 = matcher_evaluate("cstit", "/home/user/development/constituent");
    ASSERT(m5.is_match, "fuzzy subsequence cstit -> constituent");
    ASSERT(m5.quality_score >= 10.0, "fuzzy score");

    double score_boundary = 0.0;
    matcher_fuzzy_subsequence("op", "osta_pms", &score_boundary);
    ASSERT(score_boundary > 10.0, "boundary bonus for _pms");

    Match_Result m_b = matcher_evaluate("op", "/home/user/osta_pms");
    ASSERT(m_b.is_match, "fuzzy match on path with word boundaries");
    ASSERT(m_b.quality_score > 20.0, "quality score includes boundary bonus");

    Match_Result m6 = matcher_evaluate("completelyunrelated", "/home/user/development/constituent");
    ASSERT(!m6.is_match, "unrelated should not match");

    Match_Result m_ac = matcher_evaluate("pcs", "/home/user/PMS.CatalogService");
    ASSERT(m_ac.is_match, "acronym pcs -> PMS.CatalogService");

    Match_Result m_multi = matcher_evaluate("dev/jrun", "/home/user/development/jrun");
    ASSERT(m_multi.is_match, "multi-component prefix match");

    // An over-long multi-component target must be refused, not silently
    // truncated into a different query that then "matches".
    char huge[1024];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    huge[3] = '/';
    Match_Result m_huge = matcher_evaluate(huge, "/home/user/aaa/whatever");
    ASSERT(!m_huge.is_match, "an over-long target is refused rather than truncated");

    char many[512];
    size_t p = 0;
    for (int i = 0; i < 64 && p + 2 < sizeof(many); ++i) { many[p++] = 'x'; many[p++] = '/'; }
    many[p] = '\0';
    Match_Result m_many = matcher_evaluate(many, "/home/user/x/x/x");
    ASSERT(!m_many.is_match, "too many target components is refused");

    // Fuzzy off must still allow exact and prefix matches.
    Match_Result m_nofuzzy = matcher_evaluate_opts("cstit", "/home/user/constituent", false);
    ASSERT(!m_nofuzzy.is_match, "fuzzy disabled rejects loose subsequence");
    Match_Result m_nofuzzy2 = matcher_evaluate_opts("const", "/home/user/constituent", false);
    ASSERT(m_nofuzzy2.is_match, "fuzzy disabled still allows prefix");

    return true;
}

static bool test_matcher_highlight(void)
{
    const char *text = "~/development/constituent";
    unsigned char mask[64];
    size_t len = strlen(text);
    ASSERT(len < sizeof(mask), "fixture fits the mask");

    size_t marked = matcher_highlight("const", text, mask, len);
    ASSERT(marked == 5, "five bytes marked for a five-character query");

    // The run must land on the basename, not on "development".
    size_t first = 0;
    while (first < len && !mask[first]) first++;
    ASSERT(first == 14, "highlight starts at the basename");
    ASSERT(mask[14] && mask[15] && mask[16] && mask[17] && mask[18], "contiguous run marked");

    // A non-match marks nothing rather than leaving stale state.
    memset(mask, 1, len);
    marked = matcher_highlight("zzzz", text, mask, len);
    ASSERT(marked == 0, "no marks for a non-match");
    for (size_t i = 0; i < len; ++i) ASSERT(mask[i] == 0, "mask cleared on a non-match");

    marked = matcher_highlight("", text, mask, len);
    ASSERT(marked == 0, "empty query marks nothing");

    return true;
}

// --- 3. config ---------------------------------------------------------------

static bool test_config(void)
{
    Jrun_Config cfg = {0};
    config_init_default(&cfg);

    ASSERT(cfg.roots_count >= 4, "default config should have roots");
    ASSERT(cfg.max_depth == DEFAULT_MAX_DEPTH, "default max depth");
    ASSERT(cfg.fuzzy == true, "default fuzzy true");
    ASSERT(cfg.interactive == true, "default interactive true");
    ASSERT(cfg.prefer_projects == true, "default prefer_projects true");
    ASSERT(cfg.cache_ttl == DEFAULT_CACHE_TTL, "default cache ttl");
    ASSERT(cfg.ignore_count > 0, "default ignore list is populated");
    ASSERT(config_is_ignored(&cfg, "node_modules"), "node_modules ignored by default");
    ASSERT(!config_is_ignored(&cfg, "src"), "src not ignored");

    ASSERT(config_add_root(&cfg, "~/custom_projects"), "add root");
    ASSERT(config_has_root(&cfg, "~/custom_projects"), "has root");

    size_t count_before = cfg.roots_count;
    config_add_root(&cfg, "~/custom_projects");
    ASSERT(cfg.roots_count == count_before, "no duplicate roots");

    ASSERT(config_remove_root(&cfg, "~/custom_projects"), "remove root");
    ASSERT(!config_has_root(&cfg, "~/custom_projects"), "root removed");

    const char *tmp_cfg_path = sb("config/config.toml");
    ASSERT(config_save(&cfg, tmp_cfg_path), "save config");

    Jrun_Config loaded = {0};
    ASSERT(config_load(&loaded, tmp_cfg_path), "load config");
    ASSERT(loaded.roots_count == cfg.roots_count, "roots count match");
    ASSERT(loaded.ignore_count == cfg.ignore_count, "ignore count match");
    ASSERT(loaded.max_depth == cfg.max_depth, "max depth match");
    ASSERT(loaded.cache_ttl == cfg.cache_ttl, "cache ttl match");
    ASSERT(loaded.prefer_projects == cfg.prefer_projects, "prefer_projects match");
    ASSERT(loaded.confirm_destructive == cfg.confirm_destructive, "confirm flag match");
    ASSERT(loaded.confirm_commands_count == cfg.confirm_commands_count, "confirm commands match");
    ASSERT(config_needs_confirm(&loaded, "rm"), "rm requires confirm");
    ASSERT(config_needs_confirm(&loaded, "/bin/mv"), "mv path requires confirm");
    ASSERT(!config_needs_confirm(&loaded, "nvim"), "nvim does not require confirm");

    config_free(&cfg);
    config_free(&loaded);
    return true;
}

// A path containing a quote used to be written unescaped, producing a file
// that parsed back as a different, truncated path.
static bool test_config_quoting_roundtrip(void)
{
    static const char *nasty[] = {
        "/tmp/we\"ird",
        "/tmp/back\\slash",
        "/tmp/with spaces",
        "/tmp/single'quote",
        "/tmp/hash#comment",
    };

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    for (size_t i = 0; i < cfg.roots_count; ++i) free(cfg.roots[i]);
    cfg.roots_count = 0;
    for (size_t i = 0; i < sizeof(nasty) / sizeof(nasty[0]); ++i) {
        ASSERT(config_add_root(&cfg, nasty[i]), "add awkward root");
    }

    const char *path = sb("config/quoting.toml");
    ASSERT(config_save(&cfg, path), "save config with awkward roots");

    Jrun_Config loaded = {0};
    ASSERT(config_load(&loaded, path), "load config with awkward roots");
    ASSERT(loaded.roots_count == cfg.roots_count, "all awkward roots survived");
    for (size_t i = 0; i < cfg.roots_count; ++i) {
        ASSERT(strcmp(loaded.roots[i], cfg.roots[i]) == 0, "root round-trips byte for byte");
    }

    config_free(&cfg);
    config_free(&loaded);
    return true;
}

static bool test_config_sections(void)
{
    const char *path = sb("config/sections.toml");
    ASSERT(path_ensure_parent_dir(path), "parent dir");
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "write sectioned config");
    fputs("[search]\n"
          "roots = [\"/tmp/a\", \"/tmp/b\"]\n"
          "ignore = [\"target\", \"dist\"]\n"
          "max_depth = 7\n"
          "# a comment with \"quotes\" and a ] bracket\n"
          "[behavior]\n"
          "fuzzy = false\n"
          "prefer_projects = false\n"
          "[safety]\n"
          "confirm = true\n"
          "commands = [\"rm\", \"mv\"]\n", fp);
    fclose(fp);

    Jrun_Config cfg = {0};
    ASSERT(config_load(&cfg, path), "load sectioned config");
    ASSERT(cfg.roots_count == 2, "two roots");
    ASSERT(cfg.max_depth == 7, "max_depth read from [search]");
    ASSERT(cfg.fuzzy == false, "fuzzy read from [behavior]");
    ASSERT(cfg.prefer_projects == false, "prefer_projects read from [behavior]");
    ASSERT(cfg.ignore_count == 2, "explicit ignore list replaces defaults");
    ASSERT(config_is_ignored(&cfg, "target"), "target ignored");
    ASSERT(!config_is_ignored(&cfg, "node_modules"), "defaults not merged in");
    ASSERT(cfg.confirm_commands_count == 2, "two confirm commands");
    ASSERT(config_needs_confirm(&cfg, "rm"), "rm listed");
    ASSERT(!config_needs_confirm(&cfg, "chmod"), "chmod not listed");
    ASSERT(config_remove_confirm_command(&cfg, "rm"), "remove confirm command");
    ASSERT(!config_needs_confirm(&cfg, "rm"), "rm removed from confirm list");
    config_free(&cfg);

    return true;
}

static bool test_config_fingerprint(void)
{
    Jrun_Config a = {0};
    Jrun_Config b = {0};
    config_init_default(&a);
    config_init_default(&b);

    char fa[64], fb[64];
    config_scan_fingerprint(&a, fa, sizeof(fa));
    config_scan_fingerprint(&b, fb, sizeof(fb));
    ASSERT(strcmp(fa, fb) == 0, "identical configs fingerprint identically");

    config_add_root(&b, "/tmp/extra_root");
    config_scan_fingerprint(&b, fb, sizeof(fb));
    ASSERT(strcmp(fa, fb) != 0, "adding a root changes the fingerprint");

    config_free(&b);
    config_init_default(&b);
    b.max_depth = a.max_depth + 1;
    config_scan_fingerprint(&b, fb, sizeof(fb));
    ASSERT(strcmp(fa, fb) != 0, "changing max_depth changes the fingerprint");

    config_free(&b);
    config_init_default(&b);
    config_add_ignore(&b, "some_new_ignore");
    config_scan_fingerprint(&b, fb, sizeof(fb));
    ASSERT(strcmp(fa, fb) != 0, "adding an ignore changes the fingerprint");

    config_free(&a);
    config_free(&b);
    return true;
}

// --- 4. database -------------------------------------------------------------

static bool test_database(void)
{
    const char *tmp_db = sb("db/frecency.sqlite");
    ASSERT(db_init(tmp_db), "init db");

    char d1[PATH_MAX], d2[PATH_MAX];
    snprintf(d1, sizeof(d1), "%s", sb("db/d1"));
    snprintf(d2, sizeof(d2), "%s", sb("db/d2"));
    ASSERT(strcmp(d1, d2) != 0, "the two fixture paths are distinct");
    ASSERT(mkdirs(d1) && mkdirs(d2), "create test dirs");

    ASSERT(db_add_or_update(d1), "add d1");
    ASSERT(db_add_or_update(d1), "increment d1 frequency");
    ASSERT(db_add_or_update(d2), "add d2");

    Db_Entry *entries = NULL;
    size_t count = 0;
    ASSERT(db_get_all(&entries, &count), "get all entries");
    ASSERT(count == 2, "2 entries in db");
    ASSERT(strcmp(entries[0].path, d1) == 0, "d1 has higher frequency so ranks first");
    ASSERT(entries[0].frequency >= 2.0, "frequency updated to 2");
    db_free_entries(entries, count);

    rmdir(d2);
    size_t pruned = 0;
    ASSERT(db_prune(&pruned), "prune deleted dirs");
    ASSERT(pruned == 1, "1 directory pruned");

    ASSERT(db_get_all(&entries, &count), "get entries after prune");
    ASSERT(count == 1, "only 1 entry remains");
    ASSERT(strcmp(entries[0].path, d1) == 0, "d1 remains");
    db_free_entries(entries, count);

    ASSERT(db_remove(d1), "remove entry");
    ASSERT(db_get_all(&entries, &count), "get entries after remove");
    ASSERT(count == 0, "0 entries remain");
    db_free_entries(entries, count);

    db_close();
    return true;
}

static bool test_database_scan_cache(void)
{
    const char *tmp_db = sb("db/cache.sqlite");
    ASSERT(db_init(tmp_db), "init cache db");

    char *paths[3];
    paths[0] = jrun_strdup("/tmp/one");
    paths[1] = jrun_strdup("/tmp/two");
    paths[2] = jrun_strdup("/tmp/three");

    ASSERT(!db_cache_is_fresh("fp1", 300), "empty cache is not fresh");

    ASSERT(db_cache_store(paths, 3, "fp1"), "store cache");
    ASSERT(db_cache_is_fresh("fp1", 300), "stored cache is fresh");
    ASSERT(!db_cache_is_fresh("fp2", 300), "a different fingerprint invalidates");
    ASSERT(!db_cache_is_fresh("fp1", 0), "a zero ttl disables the cache");

    char **loaded = NULL;
    size_t loaded_count = 0;
    ASSERT(db_cache_load(&loaded, &loaded_count), "load cache");
    ASSERT(loaded_count == 3, "three paths came back");
    scanner_free_paths(loaded, loaded_count);

    size_t stat_count = 0;
    int64_t built_at = 0;
    ASSERT(db_cache_stats(&stat_count, &built_at), "cache stats");
    ASSERT(stat_count == 3, "stats report three paths");
    ASSERT(built_at > 0, "stats report a build time");

    // Storing again must replace, not accumulate.
    ASSERT(db_cache_store(paths, 2, "fp1"), "store a smaller cache");
    ASSERT(db_cache_load(&loaded, &loaded_count), "reload cache");
    ASSERT(loaded_count == 2, "cache was replaced, not appended to");
    scanner_free_paths(loaded, loaded_count);

    ASSERT(db_cache_invalidate(), "invalidate cache");
    ASSERT(!db_cache_is_fresh("fp1", 300), "invalidated cache is not fresh");

    for (size_t i = 0; i < 3; ++i) free(paths[i]);
    db_close();
    return true;
}

// --- 5. scanner --------------------------------------------------------------

static bool test_scanner(void)
{
    ASSERT(mkdirs(sb("scan/projA")), "mk projA");
    ASSERT(mkdirs(sb("scan/projB/subproj")), "mk subproj");
    ASSERT(mkdirs(sb("scan/.git/ignored")), "mk .git");
    ASSERT(mkdirs(sb("scan/node_modules/pkg")), "mk node_modules");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", sb("scan"));
    const char *roots[] = { root };
    set_roots(&cfg, roots, 1);

    char **paths = NULL;
    size_t count = 0;
    ASSERT(scanner_scan_roots(&cfg, &paths, &count), "scan roots");

    bool found_projA = false, found_subproj = false;
    bool found_git = false, found_node_modules = false;
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
    return true;
}

// The ignore list is configuration now, so a directory literally named "build"
// or "target" is reachable once the user drops it from the list.
static bool test_scanner_configurable_ignores(void)
{
    ASSERT(mkdirs(sb("ign/build")), "mk build");
    ASSERT(mkdirs(sb("ign/keepme")), "mk keepme");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", sb("ign"));
    const char *roots[] = { root };
    set_roots(&cfg, roots, 1);

    // Default list no longer contains "build", so it is found.
    char **paths = NULL;
    size_t count = 0;
    ASSERT(scanner_scan_roots(&cfg, &paths, &count), "scan with default ignores");
    bool found_build = false;
    for (size_t i = 0; i < count; ++i) {
        if (strstr(paths[i], "/build")) found_build = true;
    }
    ASSERT(found_build, "a directory named build is reachable by default");
    scanner_free_paths(paths, count);

    // Adding it to the ignore list hides it again.
    ASSERT(config_add_ignore(&cfg, "build"), "ignore build");
    ASSERT(scanner_scan_roots(&cfg, &paths, &count), "scan with build ignored");
    found_build = false;
    bool found_keepme = false;
    for (size_t i = 0; i < count; ++i) {
        if (strstr(paths[i], "/build")) found_build = true;
        if (strstr(paths[i], "keepme")) found_keepme = true;
    }
    ASSERT(!found_build, "build is ignored once configured");
    ASSERT(found_keepme, "other directories still found");
    scanner_free_paths(paths, count);

    config_free(&cfg);
    return true;
}

// Configuring both "~" and "~/development" used to walk the shared subtree
// twice and return every path in it twice.
static bool test_scanner_overlapping_roots(void)
{
    ASSERT(mkdirs(sb("ovl/outer/inner/leaf")), "mk nested tree");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char outer[PATH_MAX], inner[PATH_MAX];
    snprintf(outer, sizeof(outer), "%s", sb("ovl/outer"));
    snprintf(inner, sizeof(inner), "%s", sb("ovl/outer/inner"));
    const char *roots[] = { outer, inner };
    set_roots(&cfg, roots, 2);

    char **paths = NULL;
    size_t count = 0;
    ASSERT(scanner_scan_roots(&cfg, &paths, &count), "scan overlapping roots");

    size_t inner_hits = 0;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(paths[i], inner) == 0) inner_hits++;
    }
    ASSERT(inner_hits == 1, "the shared subtree is visited exactly once");

    scanner_free_paths(paths, count);
    config_free(&cfg);
    return true;
}

// --- 6. resolver -------------------------------------------------------------

static bool test_duplicate_disambiguation(void)
{
    ASSERT(mkdirs(sb("dup/rootA/constituent")), "mk rootA");
    ASSERT(mkdirs(sb("dup/rootB/constituent")), "mk rootB");
    ASSERT(mkdirs(sb("dup/rootC/constituent")), "mk rootC");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX];
    snprintf(a, sizeof(a), "%s", sb("dup/rootA"));
    snprintf(b, sizeof(b), "%s", sb("dup/rootB"));
    snprintf(c, sizeof(c), "%s", sb("dup/rootC"));
    const char *roots[] = { a, b, c };
    set_roots(&cfg, roots, 3);

    Resolve_Result res = resolver_resolve("constituent", &cfg, true);
    ASSERT(res.count == 3, "found all 3 duplicate constituent directories");
    ASSERT(res.status == RESOLVE_AMBIGUOUS,
           "multiple duplicate directories must trigger AMBIGUOUS / TUI");

    resolver_free_result(&res);
    config_free(&cfg);
    return true;
}

static bool test_bare_name_not_cwd_relative(void)
{
    ASSERT(mkdirs(sb("cwd/home/thirdparty")), "mk home/thirdparty");
    ASSERT(mkdirs(sb("cwd/dev/jrun/thirdparty")), "mk dev/jrun/thirdparty");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char home[PATH_MAX], dev[PATH_MAX], nested[PATH_MAX];
    snprintf(home, sizeof(home), "%s", sb("cwd/home"));
    snprintf(dev, sizeof(dev), "%s", sb("cwd/dev"));
    snprintf(nested, sizeof(nested), "%s", sb("cwd/dev/jrun"));
    const char *roots[] = { home, dev };
    set_roots(&cfg, roots, 2);

    char old_cwd[PATH_MAX];
    ASSERT(getcwd(old_cwd, sizeof(old_cwd)) != NULL, "save cwd");
    ASSERT(chdir(nested) == 0, "chdir into nested project");

    Resolve_Result res = resolver_resolve("thirdparty", &cfg, false);
    bool ok = (res.count == 2) && (res.status == RESOLVE_AMBIGUOUS);

    bool found_home = false, found_nested = false;
    for (size_t i = 0; i < res.count; ++i) {
        if (strstr(res.candidates[i].path, "/home/thirdparty")) found_home = true;
        if (strstr(res.candidates[i].path, "/dev/jrun/thirdparty")) found_nested = true;
    }

    resolver_free_result(&res);
    // Restore before asserting so a failure cannot strand the whole suite in
    // a directory that is about to be deleted.
    ASSERT(chdir(old_cwd) == 0, "restore cwd");
    config_free(&cfg);

    ASSERT(ok, "same basename must stay ambiguous even from a nested cwd");
    ASSERT(found_home, "found the home thirdparty");
    ASSERT(found_nested, "found the nested jrun/thirdparty");
    return true;
}

// A directory carrying a project marker should outrank an equally-named plain
// one, and the marker is reported so the selector can label it.
static bool test_resolver_project_preference(void)
{
    ASSERT(mkdirs(sb("pref/rootA/widget")), "mk plain widget");
    ASSERT(mkdirs(sb("pref/rootB/widget")), "mk project widget");
    ASSERT(touch(sb("pref/rootB/widget/go.mod")), "mark rootB widget as a project");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char a[PATH_MAX], b[PATH_MAX];
    snprintf(a, sizeof(a), "%s", sb("pref/rootA"));
    snprintf(b, sizeof(b), "%s", sb("pref/rootB"));
    const char *roots[] = { a, b };
    set_roots(&cfg, roots, 2);
    cfg.prefer_projects = true;

    Resolve_Result res = resolver_resolve("widget", &cfg, true);
    ASSERT(res.count == 2, "both widgets found");
    ASSERT(strstr(res.candidates[0].path, "rootB") != NULL,
           "the directory with go.mod ranks first");
    ASSERT(res.candidates[0].project_kind != NULL &&
           strcmp(res.candidates[0].project_kind, "go") == 0,
           "project kind reported as go");
    resolver_free_result(&res);

    // With the preference off, the tie-break falls back to path ordering.
    cfg.prefer_projects = false;
    res = resolver_resolve("widget", &cfg, true);
    ASSERT(res.count == 2, "both widgets still found");
    ASSERT(res.candidates[0].score == res.candidates[1].score,
           "scores tie without the project bonus");
    resolver_free_result(&res);

    config_free(&cfg);
    return true;
}

// The resolver serves the cached index when it is fresh, and rebuilds it when
// the configuration changes.
static bool test_resolver_uses_cache(void)
{
    const char *tmp_db = sb("db/resolver_cache.sqlite");
    ASSERT(db_init(tmp_db), "init db for cache test");

    ASSERT(mkdirs(sb("rcache/root/alpha")), "mk alpha");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", sb("rcache/root"));
    const char *roots[] = { root };
    set_roots(&cfg, roots, 1);
    cfg.cache_ttl = 300;  // set_roots zeroes it; the cache is the point here

    Resolve_Result res = resolver_resolve("alpha", &cfg, false);
    ASSERT(res.count == 1, "alpha resolved on the first pass");
    resolver_free_result(&res);

    size_t cached = 0;
    ASSERT(db_cache_stats(&cached, NULL), "cache stats after resolve");
    ASSERT(cached > 0, "the first resolve populated the cache");

    char fingerprint[64];
    config_scan_fingerprint(&cfg, fingerprint, sizeof(fingerprint));
    ASSERT(db_cache_is_fresh(fingerprint, cfg.cache_ttl), "cache is fresh after a resolve");

    // A directory created after the index was built is invisible until the
    // cache expires. That is the documented trade-off, so pin it down.
    ASSERT(mkdirs(sb("rcache/root/beta")), "mk beta after indexing");
    res = resolver_resolve("beta", &cfg, false);
    ASSERT(res.count == 0, "a brand new directory is not in the fresh cache");
    resolver_free_result(&res);

    // Changing the configuration must invalidate immediately, not after a TTL.
    ASSERT(config_add_ignore(&cfg, "some_other_name"), "change the scan configuration");
    config_scan_fingerprint(&cfg, fingerprint, sizeof(fingerprint));
    ASSERT(!db_cache_is_fresh(fingerprint, cfg.cache_ttl),
           "changing the configuration invalidates the cache");

    res = resolver_resolve("beta", &cfg, false);
    ASSERT(res.count == 1, "beta is found once the cache is rebuilt");
    resolver_free_result(&res);

    // An explicit reindex always rescans.
    size_t indexed = 0;
    ASSERT(resolver_reindex(&cfg, &indexed), "reindex");
    ASSERT(indexed >= 3, "reindex reported the directories it walked");

    config_free(&cfg);
    db_close();
    return true;
}

// A cached path that has since been deleted must never be offered.
static bool test_resolver_skips_stale_cache_entries(void)
{
    const char *tmp_db = sb("db/stale.sqlite");
    ASSERT(db_init(tmp_db), "init db for stale test");

    ASSERT(mkdirs(sb("stale/root/gone")), "mk gone");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", sb("stale/root"));
    const char *roots[] = { root };
    set_roots(&cfg, roots, 1);
    cfg.cache_ttl = 300;

    Resolve_Result res = resolver_resolve("gone", &cfg, false);
    ASSERT(res.count == 1, "gone resolves while it exists");
    resolver_free_result(&res);

    rm_rf(sb("stale/root/gone"));

    res = resolver_resolve("gone", &cfg, false);
    ASSERT(res.count == 0, "a deleted directory is not offered from the cache");
    resolver_free_result(&res);

    config_free(&cfg);
    db_close();
    return true;
}

// Visiting a directory must never make it rank lower. The original scoring
// reweighted quality against frecency when an entry came from the database,
// so a directory visited once scored below an identical never-visited one.
static bool test_resolver_visiting_never_demotes(void)
{
    const char *tmp_db = sb("db/demote.sqlite");
    ASSERT(db_init(tmp_db), "init db for demotion test");

    ASSERT(mkdirs(sb("demote/rootA/shared")), "mk rootA/shared");
    ASSERT(mkdirs(sb("demote/rootB/shared")), "mk rootB/shared");

    Jrun_Config cfg = {0};
    config_init_default(&cfg);
    char a[PATH_MAX], b[PATH_MAX], visited[PATH_MAX];
    snprintf(a, sizeof(a), "%s", sb("demote/rootA"));
    snprintf(b, sizeof(b), "%s", sb("demote/rootB"));
    snprintf(visited, sizeof(visited), "%s", sb("demote/rootB/shared"));
    const char *roots[] = { a, b };
    set_roots(&cfg, roots, 2);

    // Neither has been visited: the two must tie.
    Resolve_Result res = resolver_resolve("shared", &cfg, true);
    ASSERT(res.count == 2, "both shared directories found");
    ASSERT(res.candidates[0].score == res.candidates[1].score,
           "two unvisited directories score identically");
    double unvisited_score = res.candidates[0].score;
    resolver_free_result(&res);

    // Record a single visit to rootB/shared.
    ASSERT(db_add_or_update(visited), "record a visit");

    res = resolver_resolve("shared", &cfg, true);
    ASSERT(res.count == 2, "both still found after a visit");
    ASSERT(strcmp(res.candidates[0].path, visited) == 0,
           "the visited directory ranks first");
    ASSERT(res.candidates[0].score > unvisited_score,
           "one visit raises the score rather than lowering it");
    ASSERT(res.candidates[1].score == unvisited_score,
           "the untouched directory keeps its score");
    resolver_free_result(&res);

    config_free(&cfg);
    db_close();
    return true;
}

// --- 7. CLI ------------------------------------------------------------------

static bool test_cli_parsing(void)
{
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

    {
        char *argv[] = {"jrun", "hypr", "grep", "blur_passes", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(4, argv, &args), "parse jrun hypr grep blur_passes");
        ASSERT(args.cmd_argc == 2, "2 cmd args");
        ASSERT(strcmp(args.cmd_argv[1], "blur_passes") == 0, "arg is blur_passes");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "hypr", "--", "nvim", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(4, argv, &args), "parse jrun hypr -- nvim");
        ASSERT(args.action == CLI_ACTION_EXECUTE, "execute action");
        ASSERT(args.cmd_argc == 1, "1 cmd arg");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "constituent", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(2, argv, &args), "parse jrun constituent");
        ASSERT(args.action == CLI_ACTION_CD, "cd action");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "--cd", "constituent", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun --cd constituent");
        ASSERT(args.action == CLI_ACTION_CD, "cd action");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "-i", "-d", "hypr", "nvim", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(5, argv, &args), "parse flags");
        ASSERT(args.interactive == true, "interactive set");
        ASSERT(args.debug == true, "debug set");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "-y", "hypr", "rm", "-rf", "build", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(6, argv, &args), "parse -y");
        ASSERT(args.yes == true, "yes set");
        ASSERT(strcmp(args.cmd_argv[0], "rm") == 0, "cmd is rm");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "config", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(2, argv, &args), "parse jrun config");
        ASSERT(args.action == CLI_ACTION_CONFIG_EDIT, "config opens TUI");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "config", "show", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun config show");
        ASSERT(args.action == CLI_ACTION_CONFIG_SHOW, "config show prints TOML");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "reindex", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(2, argv, &args), "parse jrun reindex");
        ASSERT(args.action == CLI_ACTION_REINDEX, "reindex action");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "tatr", "cd", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun tatr cd");
        ASSERT(args.action == CLI_ACTION_CD, "cd suffix sets CD action");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "tatr", "open", NULL};
        Cli_Args args = {0};
        ASSERT(cli_parse(3, argv, &args), "parse jrun tatr open");
        ASSERT(args.cmd_argc == 2, "2 cmd args with '.' appended");
        ASSERT(strcmp(args.cmd_argv[1], ".") == 0, "arg is '.'");
        cli_free_args(&args);
    }

    {
        char *argv[] = {"jrun", "--bogus", NULL};
        Cli_Args args = {0};
        ASSERT(!cli_parse(2, argv, &args), "unknown option is rejected");
        cli_free_args(&args);
    }

    return true;
}

// --- 8. shell ----------------------------------------------------------------

static bool test_shell_integration(void)
{
    ASSERT(shell_parse_type("bash") == SHELL_BASH, "bash type");
    ASSERT(shell_parse_type("zsh") == SHELL_ZSH, "zsh type");
    ASSERT(shell_parse_type("fish") == SHELL_FISH, "fish type");
    ASSERT(shell_parse_type("invalid") == SHELL_UNKNOWN, "unknown type");
    ASSERT(shell_parse_type(NULL) == SHELL_UNKNOWN, "NULL type");
    return true;
}

// --- 9. executor -------------------------------------------------------------

static bool test_executor(void)
{
    char *argv[] = {"echo", "", NULL};
    int ret = executor_run(g_sandbox, argv);
    ASSERT(ret == 0, "executor run echo returned 0");

    char *bad_argv[] = {"jrun_no_such_command_12345", NULL};
    int bad_ret = executor_run(g_sandbox, bad_argv);
    ASSERT(bad_ret == 127, "command not found returns 127");

    char *false_argv[] = {"false", NULL};
    ASSERT(executor_run(g_sandbox, false_argv) == 1, "exit status propagates");

    ASSERT(executor_run("/no/such/directory/at/all", argv) == 1,
           "an unusable working directory fails cleanly");

    return true;
}

// --- driver ------------------------------------------------------------------

int main(void)
{
    char tmpl[] = "/tmp/jrun_test_XXXXXX";
    if (!mkdtemp(tmpl)) {
        fprintf(stderr, "could not create a sandbox directory: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_sandbox, sizeof(g_sandbox), "%s", tmpl);

    // Pin HOME and the XDG directories so nothing reads or writes the
    // developer's real configuration, and so ~-expansion is predictable.
    char fake_home[128];
    snprintf(fake_home, sizeof(fake_home), "%s/home", g_sandbox);
    path_mkdir_p(fake_home);
    setenv("HOME", fake_home, 1);
    setenv("XDG_CONFIG_HOME", sb("home/.config"), 1);
    setenv("XDG_DATA_HOME", sb("home/.local/share"), 1);

    printf("=== jrun test suite ===\n");
    printf("sandbox: %s\n\n", g_sandbox);

    RUN_TEST(test_path_utils);
    RUN_TEST(test_path_atomic_write);
    RUN_TEST(test_project_detection);
    RUN_TEST(test_matcher);
    RUN_TEST(test_matcher_highlight);
    RUN_TEST(test_config);
    RUN_TEST(test_config_quoting_roundtrip);
    RUN_TEST(test_config_sections);
    RUN_TEST(test_config_fingerprint);
    RUN_TEST(test_database);
    RUN_TEST(test_database_scan_cache);
    RUN_TEST(test_scanner);
    RUN_TEST(test_scanner_configurable_ignores);
    RUN_TEST(test_scanner_overlapping_roots);
    RUN_TEST(test_duplicate_disambiguation);
    RUN_TEST(test_bare_name_not_cwd_relative);
    RUN_TEST(test_resolver_project_preference);
    RUN_TEST(test_resolver_uses_cache);
    RUN_TEST(test_resolver_skips_stale_cache_entries);
    RUN_TEST(test_resolver_visiting_never_demotes);
    RUN_TEST(test_cli_parsing);
    RUN_TEST(test_shell_integration);
    RUN_TEST(test_executor);

    // Whatever happened above, leave nothing behind.
    db_close();
    rm_rf(g_sandbox);

    printf("\n%d/%d passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
