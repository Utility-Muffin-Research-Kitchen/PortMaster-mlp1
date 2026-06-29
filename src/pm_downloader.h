#ifndef PM_DOWNLOADER_H
#define PM_DOWNLOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *url;
    const char *dest_path;
    uint64_t expected_size;
    const char *expected_sha256;
    bool allow_http;
} pm_download_spec;

bool pm_download_url_allowed(const char *url, bool allow_http);
int pm_download_partial_path(const char *dest_path, char *out, size_t out_size);
int pm_download_file(const pm_download_spec *spec, char *err, size_t err_size);

#endif /* PM_DOWNLOADER_H */

