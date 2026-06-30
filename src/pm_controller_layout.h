#ifndef PM_CONTROLLER_LAYOUT_H
#define PM_CONTROLLER_LAYOUT_H

#include "pm_context.h"

typedef enum {
    PM_CONTROLLER_LAYOUT_X360 = 0,
    PM_CONTROLLER_LAYOUT_NINTENDO,
} pm_controller_layout;

const char *pm_controller_layout_slug(pm_controller_layout layout);
const char *pm_controller_layout_label(pm_controller_layout layout);
const char *pm_controller_layout_sdl_config(pm_controller_layout layout);
const char *pm_controller_layout_gui_sdl_config(void);
int pm_controller_layout_from_string(const char *value, pm_controller_layout *out);
int pm_controller_layout_load(const pm_context *ctx, pm_controller_layout *out);
int pm_controller_layout_save(const pm_context *ctx, pm_controller_layout layout,
                              char *err, size_t err_size);
int pm_controller_layout_sync_hook(const pm_context *ctx, char *err, size_t err_size);

#endif /* PM_CONTROLLER_LAYOUT_H */
