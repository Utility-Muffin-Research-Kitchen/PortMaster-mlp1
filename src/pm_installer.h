#ifndef PM_INSTALLER_H
#define PM_INSTALLER_H

#include "pm_context.h"

#include <stddef.h>

int pm_install_portmaster(pm_context *ctx, char *err, size_t err_size);
int pm_repatch_portmaster(pm_context *ctx, char *err, size_t err_size);
int pm_install_runtime_archive(pm_context *ctx, const char *archive_path, char *err, size_t err_size);
int pm_install_ui_runtime(pm_context *ctx, char *err, size_t err_size);

#endif /* PM_INSTALLER_H */
