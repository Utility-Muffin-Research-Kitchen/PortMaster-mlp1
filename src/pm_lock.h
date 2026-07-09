#ifndef PM_LOCK_H
#define PM_LOCK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char product[64];
    char channel[32];
    char repo[128];
    char tag[64];
    char published_at[64];
    char asset[128];
    char url[512];
    char release_url[512];
    uint64_t size;
    char md5[33];
    char sha256[65];
    char checked_at[32];
} pm_portmaster_lock;

typedef struct {
    char product[64];
    char kind[64];
    char version[64];
    char filename[256];
    char url[1024];
    uint64_t size;
    uint64_t installed_size;
    int installed_file_count;
    char sha256[65];
    char manifest_filename[256];
    char manifest_url[1024];
    uint64_t manifest_size;
    char manifest_sha256[65];
} pm_ui_runtime_lock;

typedef struct {
    char status[64];
    char version[128];
    char filename[256];
    char url[1024];
    uint64_t size;
    char sha256[65];
    char manifest_filename[256];
    char manifest_url[1024];
    uint64_t manifest_size;
    char manifest_sha256[65];
} pm_armhf_compat_lock;

int pm_lock_load(const char *path, pm_portmaster_lock *out, char *err, size_t err_size);
void pm_lock_summary(const pm_portmaster_lock *lock, char *out, size_t out_size);
int pm_ui_runtime_lock_load(const char *path, pm_ui_runtime_lock *out, char *err, size_t err_size);
void pm_ui_runtime_lock_summary(const pm_ui_runtime_lock *lock, char *out, size_t out_size);
int pm_armhf_compat_lock_load(const char *path, pm_armhf_compat_lock *out,
                               char *err, size_t err_size);
void pm_armhf_compat_lock_summary(const pm_armhf_compat_lock *lock,
                                  char *out, size_t out_size);

#endif /* PM_LOCK_H */
