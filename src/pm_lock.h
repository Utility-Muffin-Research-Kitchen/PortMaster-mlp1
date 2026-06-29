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

int pm_lock_load(const char *path, pm_portmaster_lock *out, char *err, size_t err_size);
void pm_lock_summary(const pm_portmaster_lock *lock, char *out, size_t out_size);

#endif /* PM_LOCK_H */

