#ifndef PM_SHA256_H
#define PM_SHA256_H

#include <stddef.h>

int pm_sha256_file_hex(const char *path, char out_hex[65], char *err, size_t err_size);

#endif /* PM_SHA256_H */

