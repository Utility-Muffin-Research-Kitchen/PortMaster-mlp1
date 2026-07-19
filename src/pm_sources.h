#ifndef PM_SOURCES_H
#define PM_SOURCES_H

#include "pm_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM_MAX_SOURCES 2

typedef struct {
    char id[32];
    char root[PM_PATH_MAX];
    char roms_path[PM_PATH_MAX];
    char images_path[PM_PATH_MAX];
    char ports_path[PM_PATH_MAX];
    char port_images_path[PM_PATH_MAX];
    bool configured;
    bool available;
    unsigned long mount_id;
    unsigned int device_major;
    unsigned int device_minor;
    char device_id[32];
    uint64_t st_dev;
    uint64_t roms_st_dev;
    uint64_t images_st_dev;
    char filesystem_fingerprint[128];
    unsigned long roms_mount_id;
    unsigned int roms_device_major;
    unsigned int roms_device_minor;
    char roms_device_id[32];
    char roms_filesystem_fingerprint[128];
    unsigned long images_mount_id;
    unsigned int images_device_major;
    unsigned int images_device_minor;
    char images_device_id[32];
    char images_filesystem_fingerprint[128];
} pm_source;

typedef struct {
    pm_source items[PM_MAX_SOURCES];
    size_t count;
} pm_source_list;

int pm_sources_resolve(pm_source_list *out,
                       const char *platform,
                       const char *primary_root,
                       const char *primary_roms,
                       const char *primary_images,
                       char *err,
                       size_t err_size);

const pm_source *pm_sources_by_id(const pm_source_list *sources, const char *id);

#endif /* PM_SOURCES_H */
