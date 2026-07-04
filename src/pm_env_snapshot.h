#ifndef PM_ENV_SNAPSHOT_H
#define PM_ENV_SNAPSHOT_H

#include "pm_context.h"

typedef struct {
    const char *name;
    const char *value;
    const char *source;
} pm_env_snapshot_entry;

int pm_env_snapshot_path(const pm_context *ctx,
                         const char *mode,
                         const char *ext,
                         char *out,
                         size_t out_size);
int pm_env_snapshot_write(const pm_context *ctx,
                          const char *mode,
                          const pm_env_snapshot_entry *entries,
                          char *err,
                          size_t err_size);

#endif /* PM_ENV_SNAPSHOT_H */
