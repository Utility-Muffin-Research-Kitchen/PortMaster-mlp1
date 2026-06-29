#ifndef PM_CONTEXT_H
#define PM_CONTEXT_H

#include "pm_lock.h"
#include "pm_util.h"

typedef struct {
    char pak_dir[PM_PATH_MAX];
    char platform[32];
    char sdcard_path[PM_PATH_MAX];
    char userdata_path[PM_PATH_MAX];
    char logs_path[PM_PATH_MAX];
    char roms_path[PM_PATH_MAX];
    char images_path[PM_PATH_MAX];
    char data_dir[PM_PATH_MAX];
    char leaf_dir[PM_PATH_MAX];
    char downloads_dir[PM_PATH_MAX];
    char staging_dir[PM_PATH_MAX];
    char backups_dir[PM_PATH_MAX];
    char runtime_dir[PM_PATH_MAX];
    char portmaster_dir[PM_PATH_MAX];
    char ports_dir[PM_PATH_MAX];
    char port_images_dir[PM_PATH_MAX];
    char lock_path[PM_PATH_MAX];
    char runtime_lock_path[PM_PATH_MAX];
    char manifest_path[PM_PATH_MAX];
    pm_portmaster_lock lock;
    pm_ui_runtime_lock runtime_lock;
    bool lock_loaded;
    bool runtime_lock_loaded;
} pm_context;

int pm_context_init(pm_context *ctx, const char *argv0, char *err, size_t err_size);
int pm_context_ensure_manager_dirs(const pm_context *ctx, char *err, size_t err_size);

#endif /* PM_CONTEXT_H */
