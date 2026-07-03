#ifndef PM_UPDATE_H
#define PM_UPDATE_H

#include "pm_context.h"
#include "pm_installer.h"

#include <stdbool.h>

typedef struct {
    pm_portmaster_source source;
    char installed_version[64];
    bool update_available;
    bool from_cache;
} pm_portmaster_update_status;

typedef enum {
    PM_UPDATE_CHECK_INTERACTIVE = 0,
    PM_UPDATE_CHECK_STARTUP,
} pm_update_check_policy;

int pm_portmaster_check_update(pm_context *ctx, pm_portmaster_update_status *out,
                               char *err, size_t err_size);
int pm_portmaster_check_update_cached(pm_context *ctx, pm_portmaster_update_status *out,
                                      char *err, size_t err_size);
int pm_portmaster_check_update_cached_policy(pm_context *ctx,
                                             pm_portmaster_update_status *out,
                                             pm_update_check_policy policy,
                                             char *err,
                                             size_t err_size);
int pm_portmaster_apply_update(pm_context *ctx, const pm_portmaster_update_status *status,
                               char *err, size_t err_size);
int pm_portmaster_record_update_declined(pm_context *ctx,
                                         const pm_portmaster_update_status *status,
                                         char *err, size_t err_size);
int pm_portmaster_record_update_failed(pm_context *ctx,
                                       const pm_portmaster_update_status *status,
                                       const char *reason,
                                       char *err, size_t err_size);
bool pm_portmaster_should_prompt_update(pm_context *ctx,
                                        const pm_portmaster_update_status *status);
void pm_portmaster_update_summary(const pm_portmaster_update_status *status,
                                  char *out, size_t out_size);

#endif /* PM_UPDATE_H */
