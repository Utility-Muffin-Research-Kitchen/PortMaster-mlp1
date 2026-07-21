#ifndef PM_MOVE_H
#define PM_MOVE_H

#include "pm_context.h"
#include "pm_ports.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*pm_move_status_fn)(const char *message, void *userdata);

typedef struct {
    bool supported;
    char detail[256];
} pm_move_capability;

int pm_move_probe_capability(pm_context *ctx,
                             pm_move_capability *out,
                             char *err,
                             size_t err_size);

int pm_move_package(pm_context *ctx,
                    const pm_port_package *package,
                    const char *destination_source_id,
                    pm_move_status_fn status_fn,
                    void *status_userdata,
                    char *err,
                    size_t err_size);

int pm_move_recover_all(pm_context *ctx,
                        pm_move_status_fn status_fn,
                        void *status_userdata,
                        char *summary,
                        size_t summary_size);

int pm_move_pending_count(const pm_context *ctx);

#endif /* PM_MOVE_H */
