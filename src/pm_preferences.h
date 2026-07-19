#ifndef PM_PREFERENCES_H
#define PM_PREFERENCES_H

#include "pm_context.h"

#include <stddef.h>

#define PM_INSTALL_SOURCE_PRIMARY "primary"
#define PM_INSTALL_SOURCE_SECONDARY "secondary_sd"

int pm_install_source_preference_load(pm_context *ctx, char *err, size_t err_size);
int pm_install_source_preference_save(pm_context *ctx, const char *source_id,
                                      char *err, size_t err_size);
int pm_install_source_resolve(pm_context *ctx, const char *session_source_id,
                              const pm_source **source,
                              char *err, size_t err_size);
const char *pm_install_source_label(const char *source_id);

#endif /* PM_PREFERENCES_H */
