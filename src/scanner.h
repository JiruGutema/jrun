#ifndef JRUN_SCANNER_H_
#define JRUN_SCANNER_H_

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

bool scanner_scan_roots(const Jrun_Config *config, char ***out_paths, size_t *out_count);
void scanner_free_paths(char **paths, size_t count);

#endif // JRUN_SCANNER_H_
