#ifndef JRUN_PATH_UTIL_H_
#define JRUN_PATH_UTIL_H_

#include <stdbool.h>
#include <stddef.h>

bool path_expand_tilde(const char *path, char *out, size_t out_size);
bool path_shorten_tilde(const char *path, char *out, size_t out_size);
bool path_normalize(const char *path, char *out, size_t out_size);
bool path_is_dir(const char *path);
bool path_exists(const char *path);
const char *path_basename(const char *path);
bool path_get_config_dir(char *out, size_t out_size);
bool path_get_data_dir(char *out, size_t out_size);
bool path_get_db_path(char *out, size_t out_size);
bool path_get_config_path(char *out, size_t out_size);
bool path_ensure_parent_dir(const char *filepath);
bool path_mkdir_p(const char *dir_path);

#endif // JRUN_PATH_UTIL_H_
