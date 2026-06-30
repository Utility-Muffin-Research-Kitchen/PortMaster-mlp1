#ifndef PM_LAUNCHER_H
#define PM_LAUNCHER_H

#include "pm_context.h"

#include <stddef.h>

int pm_launch_portmaster(pm_context *ctx, char *err, size_t err_size);
void pm_request_jawaka_library_rescan(pm_context *ctx);

#endif /* PM_LAUNCHER_H */
