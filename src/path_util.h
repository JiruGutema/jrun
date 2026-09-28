#ifndef JRUN_PATH_UTIL_H_
#define JRUN_PATH_UTIL_H_

#include <stdbool.h>
#include <stddef.h>

bool path_expand_tilde(const char *path, char *out, size_t out_size);
bool path_shorten_tilde(const char *path, char *out, size_t out_size);
bool path_normalize(const char *path, char *out, size_t out_size);
bool path_is_dir(const char *path);
bool path_exists(const char *path);

// Writes the final component of `path` into `out`. Returns `out` for
// convenience, or "" written into `out` when the path is empty.
//
// There is deliberately no buffer-less variant: a shared static return buffer
// makes `strcmp(basename(a), basename(b))` silently compare a string to itself.
const char *path_basename_r(const char *path, char *out, size_t out_size);

// Recognised project markers (.git, Cargo.toml, package.json, ...). Returns a
// short static label such as "git" or "rust", or NULL when `path` holds none.
// The returned pointer is a string literal and is always safe to keep.
const char *path_project_kind(const char *path);
bool path_is_project_dir(const char *path);

bool path_get_config_dir(char *out, size_t out_size);
bool path_get_data_dir(char *out, size_t out_size);
bool path_get_db_path(char *out, size_t out_size);
bool path_get_config_path(char *out, size_t out_size);
bool path_mkdir_p(const char *dir_path);
bool path_ensure_parent_dir(const char *filepath);

// Writes `content` to `filepath` atomically: a sibling temp file is written,
// fsynced and renamed into place, so a crash can never leave a half-written
// file where a valid one used to be.
bool path_write_atomic(const char *filepath, const char *content, size_t len);

#endif // JRUN_PATH_UTIL_H_
