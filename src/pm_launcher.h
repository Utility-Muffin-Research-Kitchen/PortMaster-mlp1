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
int pm_launch_portmaster(pm_context *ctx, char *err, size_t err_size);
void pm_request_jawaka_library_rescan(pm_context *ctx);

#endif /* PM_LAUNCHER_H */
