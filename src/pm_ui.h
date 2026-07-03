#ifndef PM_UI_H
#define PM_UI_H

#include "pm_context.h"

#include <stddef.h>

void pm_ui_run(pm_context *ctx);
int pm_ui_menu_state_text(pm_context *ctx, char *out, size_t out_size);

#endif /* PM_UI_H */
