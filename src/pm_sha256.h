#ifndef PM_SHA256_H
#define PM_SHA256_H

#include <stddef.h>

int pm_sha256_file_hex(const char *path, char out_hex[65], char *err, size_t err_size);
int pm_sha256_buffer_hex(const void *data, size_t size, char out_hex[65], char *err, size_t err_size);
int pm_md5_file_hex(const char *path, char out_hex[33], char *err, size_t err_size);

#endif /* PM_SHA256_H */
