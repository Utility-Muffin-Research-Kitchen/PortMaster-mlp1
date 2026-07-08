#ifndef PM_INSTALLER_H
#define PM_INSTALLER_H

#include "pm_context.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char repo[128];
    char channel[32];
    char tag[64];
    char published_at[64];
    char asset[128];
    char url[1024];
    char release_url[1024];
    uint64_t size;
    char md5[33];
    char sha256[65];
    char checked_at[64];
} pm_portmaster_source;

const char *pm_portmaster_patch_set_id(void);
int pm_portmaster_patch_set_fingerprint(const pm_context *ctx, char out_hex[65],
                                        char *err, size_t err_size);
int pm_install_portmaster(pm_context *ctx, char *err, size_t err_size);
int pm_install_portmaster_source(pm_context *ctx, const pm_portmaster_source *source,
                                 char *err, size_t err_size);
int pm_repatch_portmaster(pm_context *ctx, char *err, size_t err_size);
int pm_repatch_portmaster_repair(pm_context *ctx, char *err, size_t err_size);
int pm_install_runtime_archive(pm_context *ctx, const char *archive_path, char *err, size_t err_size);
int pm_install_ui_runtime(pm_context *ctx, char *err, size_t err_size);

#endif /* PM_INSTALLER_H */
