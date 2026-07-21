#ifndef PM_PORTS_H
#define PM_PORTS_H

#include "pm_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *relpath;
    bool directory;
    uint64_t bytes;
} pm_port_owned_item;

typedef struct {
    char *relpath;
    char *artwork_name;
    bool artwork_present;
} pm_port_launcher;

typedef struct {
    char source_id[32];
    char slug[256];
    char display_name[256];
    char metadata_relpath[PM_PATH_MAX];
    bool authoritative;
    bool movable;
    char blocked_reason[256];
    pm_port_owned_item *items;
    size_t item_count;
    pm_port_launcher *launchers;
    size_t launcher_count;
    uint64_t bytes;
} pm_port_package;

typedef struct {
    pm_port_package *packages;
    size_t package_count;
    char **unmanaged_launchers;
    size_t unmanaged_count;
} pm_port_inventory;

int pm_port_inventory_load(pm_context *ctx,
                           pm_port_inventory *out,
                           char *err,
                           size_t err_size);
void pm_port_inventory_free(pm_port_inventory *inventory);

const pm_port_package *pm_port_inventory_find(const pm_port_inventory *inventory,
                                              const char *source_id,
                                              const char *slug);

int pm_port_inventory_text(pm_context *ctx,
                           char *out,
                           size_t out_size,
                           char *err,
                           size_t err_size);

/* FAT32-safe relative path normalizer shared with the move journal. */
int pm_port_normalize_relative(const char *input, char *out, size_t out_size);

#endif /* PM_PORTS_H */
