#ifndef PM_ARTWORK_H
#define PM_ARTWORK_H

#include "pm_context.h"

#include <stddef.h>

typedef struct {
    int scanned;
    int synced;
    int skipped_existing;
    int missing_source;
    int failed;
} pm_artwork_sync_result;

int pm_artwork_sync(const pm_context *ctx,
                    pm_artwork_sync_result *out,
                    char *err,
                    size_t err_size);

#endif /* PM_ARTWORK_H */
