#ifndef PM_ARTWORK_H
#define PM_ARTWORK_H

#include "pm_context.h"

#include <stddef.h>

typedef enum {
    PM_ARTWORK_MISSING_ONLY = 0,
    PM_ARTWORK_MANAGED_REFRESH,
    PM_ARTWORK_REPLACE_ALL,
} pm_artwork_policy;

typedef struct {
    int scanned;
    int synced;
    int skipped_existing;
    int preserved_custom;
    int missing_source;
    int failed;
} pm_artwork_sync_result;

int pm_artwork_sync(const pm_context *ctx,
                    pm_artwork_sync_result *out,
                    char *err,
                    size_t err_size);
int pm_artwork_sync_with_policy(const pm_context *ctx,
                                pm_artwork_policy policy,
                                pm_artwork_sync_result *out,
                                char *err,
                                size_t err_size);

#endif /* PM_ARTWORK_H */
