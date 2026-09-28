#ifndef JRUN_SCANNER_H_
#define JRUN_SCANNER_H_

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// Walks every configured root and returns each directory found beneath them.
// Roots nested inside another root are skipped, so overlapping configuration
// such as ["~", "~/development"] does not walk the same tree twice.
//
// On success the caller owns *out_paths and must release it with
// scanner_free_paths(). Returns false only on allocation failure, in which
// case *out_paths is NULL and nothing needs freeing.
bool scanner_scan_roots(const Jrun_Config *config, char ***out_paths, size_t *out_count);
void scanner_free_paths(char **paths, size_t count);

#endif // JRUN_SCANNER_H_
