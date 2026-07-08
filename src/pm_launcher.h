#ifndef PM_LAUNCHER_H
#define PM_LAUNCHER_H

#include "pm_context.h"

#include <stdbool.h>
#include <stddef.h>

bool pm_portmaster_is_installed(const pm_context *ctx);
bool pm_portmaster_runtime_available(const pm_context *ctx,
                                     char *runtime_path,
                                     size_t runtime_path_size,
                                     bool *uses_system_python);
bool pm_portmaster_launch_ready(const pm_context *ctx, char *reason, size_t reason_size);
bool pm_refresh_armhf_port_wrappers(pm_context *ctx);
typedef void (*pm_launch_status_fn)(void *userdata, const char *message);
int pm_launch_portmaster(pm_context *ctx, char *err, size_t err_size);
int pm_launch_portmaster_with_status(pm_context *ctx,
                                     pm_launch_status_fn status_fn,
                                     void *status_userdata,
                                     char *err,
                                     size_t err_size);
void pm_request_jawaka_library_rescan(pm_context *ctx);

#endif /* PM_LAUNCHER_H */
