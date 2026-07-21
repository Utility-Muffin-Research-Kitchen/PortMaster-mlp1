#include "pm_sources.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#include <sys/sysmacros.h>
#endif

typedef struct {
    unsigned long mount_id;
    unsigned int major;
    unsigned int minor;
    char filesystem[32];
    char device[PM_PATH_MAX];
} pm_mount_identity;

static int pm__source_error(char *err, size_t err_size, const char *message,
                            const char *name)
{
    if (err && err_size > 0) {
        snprintf(err, err_size, message, name ? name : "");
    }
    return -1;
}

static int pm__clean_absolute_path(char *out, size_t out_size, const char *raw)
{
    if (!raw || raw[0] != '/' || !out || out_size == 0) {
        return -1;
    }
    size_t used = 0;
    out[used++] = '/';
    const char *cursor = raw + 1;
    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            unsigned char c = (unsigned char)*cursor;
            if (c < 0x20u || c == 0x7fu || c == ':') {
                return -1;
            }
            cursor++;
        }
        size_t len = (size_t)(cursor - start);
        if (len == 0 || (len == 1 && start[0] == '.')) {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return -1;
        }
        if (used > 1 && used + 1 >= out_size) {
            return -1;
        }
        if (used > 1) {
            out[used++] = '/';
        }
        if (used + len >= out_size) {
            return -1;
        }
        memcpy(out + used, start, len);
        used += len;
    }
    out[used] = '\0';
    return 0;
}

static int pm__parse_path_list(const char *name, const char *raw,
                               char out[][PM_PATH_MAX], size_t *count,
                               char *err, size_t err_size)
{
    if (!raw || !raw[0]) {
        return pm__source_error(err, err_size, "%s is empty", name);
    }
    char *copy = strdup(raw);
    if (!copy) {
        return pm__source_error(err, err_size, "cannot parse %s", name);
    }
    size_t used = 0;
    char *cursor = copy;
    for (;;) {
        char *separator = strchr(cursor, ':');
        if (separator) {
            *separator = '\0';
        }
        if (used >= PM_MAX_SOURCES || !cursor[0] ||
            pm__clean_absolute_path(out[used], PM_PATH_MAX, cursor) != 0) {
            free(copy);
            return pm__source_error(err, err_size,
                                    "%s is malformed or has too many items", name);
        }
        for (size_t i = 0; i < used; i++) {
            if (strcmp(out[i], out[used]) == 0) {
                free(copy);
                return pm__source_error(err, err_size,
                                        "%s contains a duplicate path", name);
            }
        }
        used++;
        if (!separator) {
            break;
        }
        cursor = separator + 1;
    }
    free(copy);
    *count = used;
    return 0;
}

static int pm__unescape_mount_field(char *out, size_t out_size, const char *raw)
{
    size_t used = 0;
    for (size_t i = 0; raw && raw[i]; i++) {
        unsigned char value = (unsigned char)raw[i];
        if (raw[i] == '\\' &&
            raw[i + 1] >= '0' && raw[i + 1] <= '7' &&
            raw[i + 2] >= '0' && raw[i + 2] <= '7' &&
            raw[i + 3] >= '0' && raw[i + 3] <= '7') {
            value = (unsigned char)(((raw[i + 1] - '0') << 6) |
                                    ((raw[i + 2] - '0') << 3) |
                                    (raw[i + 3] - '0'));
            i += 3;
        }
        if (used + 1 >= out_size) {
            return -1;
        }
        out[used++] = (char)value;
    }
    out[used] = '\0';
    return 0;
}

static bool pm__mountinfo_identity(const char *root,
                                   pm_mount_identity *identity,
                                   bool exact)
{
    const char *fixture_path = getenv("PORTMASTER_SOURCE_TEST_MOUNTINFO");
#if defined(__linux__)
    const char *path = fixture_path && fixture_path[0]
        ? fixture_path : "/proc/self/mountinfo";
#else
    if (!fixture_path || !fixture_path[0]) {
        return false;
    }
    const char *path = fixture_path;
#endif
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;
    size_t best_length = 0;
    while (getline(&line, &capacity, fp) >= 0) {
        char *fields[128];
        size_t field_count = 0;
        char *save = NULL;
        for (char *field = strtok_r(line, " \n", &save);
             field && field_count < sizeof(fields) / sizeof(fields[0]);
             field = strtok_r(NULL, " \n", &save)) {
            fields[field_count++] = field;
        }
        if (field_count < 10) {
            continue;
        }
        size_t separator = 0;
        while (separator < field_count && strcmp(fields[separator], "-") != 0) {
            separator++;
        }
        if (separator + 2 >= field_count) {
            continue;
        }
        char mountpoint[PM_PATH_MAX];
        if (pm__unescape_mount_field(mountpoint, sizeof(mountpoint), fields[4]) != 0) {
            continue;
        }
        size_t mountpoint_length = strlen(mountpoint);
        bool matches = exact
            ? strcmp(mountpoint, root) == 0
            : (!strncmp(root, mountpoint, mountpoint_length) &&
               (mountpoint_length == 1 || root[mountpoint_length] == '/' ||
                root[mountpoint_length] == '\0'));
        if (!matches || (!exact && mountpoint_length <= best_length)) {
            continue;
        }
        unsigned long mount_id = 0;
        unsigned int major = 0;
        unsigned int minor = 0;
        if (sscanf(fields[0], "%lu", &mount_id) != 1 ||
            sscanf(fields[2], "%u:%u", &major, &minor) != 2 ||
            pm__unescape_mount_field(identity->device, sizeof(identity->device),
                                     fields[separator + 2]) != 0 ||
            pm_copy(identity->filesystem, sizeof(identity->filesystem),
                    fields[separator + 1]) != 0) {
            continue;
        }
        identity->mount_id = mount_id;
        identity->major = major;
        identity->minor = minor;
        found = true;
        best_length = mountpoint_length;
        if (exact) {
            break;
        }
    }
    free(line);
    fclose(fp);
    return found;
}

static bool pm__fixture_available(const char *id)
{
    const char *raw = getenv("PORTMASTER_SOURCE_TEST_AVAILABLE");
    if (!raw || !raw[0]) {
        return false;
    }
    size_t id_len = strlen(id);
    const char *cursor = raw;
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            cursor++;
        }
        const char *end = cursor;
        while (*end && *end != ',' && !isspace((unsigned char)*end)) {
            end++;
        }
        if ((size_t)(end - cursor) == id_len &&
            strncmp(cursor, id, id_len) == 0) {
            return true;
        }
        cursor = end;
    }
    return false;
}

#if defined(__linux__)
static uint32_t pm__le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
#endif

static void pm__filesystem_fingerprint(pm_source *source,
                                       const pm_mount_identity *identity)
{
    const char *fixture = getenv("PORTMASTER_SOURCE_TEST_FINGERPRINT");
    if (fixture && fixture[0]) {
        (void)pm_format(source->filesystem_fingerprint,
                        sizeof(source->filesystem_fingerprint),
                        "fixture:%s:%s", source->id, fixture);
        return;
    }
#if defined(__linux__)
    DIR *uuid_dir = opendir("/dev/disk/by-uuid");
    if (uuid_dir) {
        struct dirent *entry = NULL;
        while ((entry = readdir(uuid_dir)) != NULL) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            char path[PM_PATH_MAX];
            struct stat st;
            if (pm_join(path, sizeof(path), "/dev/disk/by-uuid",
                        entry->d_name) == 0 &&
                stat(path, &st) == 0 && S_ISBLK(st.st_mode) &&
                major(st.st_rdev) == identity->major &&
                minor(st.st_rdev) == identity->minor) {
                (void)pm_format(source->filesystem_fingerprint,
                                sizeof(source->filesystem_fingerprint),
                                "uuid:%s", entry->d_name);
                break;
            }
        }
        closedir(uuid_dir);
        if (source->filesystem_fingerprint[0]) {
            return;
        }
    }
    if ((!strcmp(identity->filesystem, "vfat") ||
         !strcmp(identity->filesystem, "msdos") ||
         !strcmp(identity->filesystem, "fat")) &&
        identity->device[0] == '/') {
        unsigned char sector[512];
        int fd = open(identity->device, O_RDONLY | O_CLOEXEC);
        ssize_t got = fd >= 0 ? pread(fd, sector, sizeof(sector), 0) : -1;
        if (fd >= 0) {
            close(fd);
        }
        if (got == (ssize_t)sizeof(sector) &&
            sector[510] == 0x55 && sector[511] == 0xaa) {
            size_t offset = (sector[17] == 0 && sector[18] == 0 &&
                             sector[22] == 0 && sector[23] == 0) ? 67 : 39;
            uint32_t serial = pm__le32(&sector[offset]);
            if (serial != 0) {
                (void)pm_format(source->filesystem_fingerprint,
                                sizeof(source->filesystem_fingerprint),
                                "fat:%08x", serial);
            }
        }
    }
#else
    (void)identity;
#endif
}

static void pm__resolve_availability(pm_source *source, bool primary)
{
    struct stat st;
    if (stat(source->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    const char *fixture = getenv("PORTMASTER_SOURCE_TEST_AVAILABLE");
    if (fixture && fixture[0]) {
        source->available = pm__fixture_available(source->id);
        if (source->available) {
            source->mount_id = primary ? 1 : 2;
            source->device_major = 0;
            source->device_minor = primary ? 1 : 2;
            (void)pm_format(source->device_id, sizeof(source->device_id),
                            "%u:%u", source->device_major, source->device_minor);
            source->st_dev = (uint64_t)st.st_dev;
            pm_mount_identity identity = {0};
            pm__filesystem_fingerprint(source, &identity);
        }
        return;
    }

    pm_mount_identity identity = {0};
    bool mounted = pm__mountinfo_identity(source->root, &identity, true);
    if (!primary && !mounted) {
        return;
    }
    source->available = true;
    source->st_dev = (uint64_t)st.st_dev;
    if (mounted) {
        source->mount_id = identity.mount_id;
        source->device_major = identity.major;
        source->device_minor = identity.minor;
        (void)pm_format(source->device_id, sizeof(source->device_id),
                        "%u:%u", source->device_major, source->device_minor);
        pm__filesystem_fingerprint(source, &identity);
    }
}

static void pm__resolve_content_identity(pm_source *source,
                                         const char *path,
                                         bool images)
{
    struct stat st;
    if (!source->available || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    uint64_t *st_dev = images ? &source->images_st_dev : &source->roms_st_dev;
    unsigned long *mount_id =
        images ? &source->images_mount_id : &source->roms_mount_id;
    unsigned int *device_major =
        images ? &source->images_device_major : &source->roms_device_major;
    unsigned int *device_minor =
        images ? &source->images_device_minor : &source->roms_device_minor;
    char *device_id = images ? source->images_device_id : source->roms_device_id;
    char *fingerprint = images
        ? source->images_filesystem_fingerprint
        : source->roms_filesystem_fingerprint;
    *st_dev = (uint64_t)st.st_dev;
    if (*st_dev == source->st_dev) {
        *mount_id = source->mount_id;
        *device_major = source->device_major;
        *device_minor = source->device_minor;
        (void)pm_copy(device_id, 32, source->device_id);
        (void)pm_copy(fingerprint, 128, source->filesystem_fingerprint);
        return;
    }
    pm_mount_identity identity = {0};
    if (pm__mountinfo_identity(path, &identity, false)) {
        *mount_id = identity.mount_id;
        *device_major = identity.major;
        *device_minor = identity.minor;
        (void)pm_format(device_id, 32, "%u:%u",
                        identity.major, identity.minor);
        pm_source temporary = *source;
        temporary.filesystem_fingerprint[0] = '\0';
        pm__filesystem_fingerprint(&temporary, &identity);
        (void)pm_copy(fingerprint, 128, temporary.filesystem_fingerprint);
    }
}

static int pm__resolve_content_paths(const char *plural_name,
                                     const char *singular_value,
                                     const char *child,
                                     char roots[][PM_PATH_MAX],
                                     size_t root_count,
                                     bool legacy_appended,
                                     char out[][PM_PATH_MAX],
                                     char *err,
                                     size_t err_size)
{
    const char *raw = getenv(plural_name);
    size_t count = 0;
    bool supplied = raw && raw[0];
    if (supplied) {
        if (pm__parse_path_list(plural_name, raw, out, &count, err, err_size) != 0) {
            return -1;
        }
        size_t accepted = legacy_appended ? root_count - 1 : root_count;
        if (count != root_count && count != accepted) {
            return pm__source_error(err, err_size,
                                    "%s is not aligned with SDCARD_PATHS", plural_name);
        }
    }
    if (!supplied) {
        count = 0;
    }
    for (size_t i = count; i < root_count; i++) {
        if (pm_join(out[i], PM_PATH_MAX, roots[i], child) != 0) {
            return pm__source_error(err, err_size, "%s path is too long", plural_name);
        }
    }
    if (singular_value && singular_value[0]) {
        char clean_singular[PM_PATH_MAX];
        if (pm__clean_absolute_path(clean_singular, sizeof(clean_singular),
                                    singular_value) != 0) {
            return pm__source_error(err, err_size,
                                    "%s singular alias is malformed", plural_name);
        }
        if (!supplied) {
            if (pm_copy(out[0], PM_PATH_MAX, clean_singular) != 0) {
                return pm__source_error(err, err_size,
                                        "%s primary path is too long", plural_name);
            }
        } else if (strcmp(out[0], clean_singular) != 0) {
            return pm__source_error(err, err_size,
                                    "%s primary does not match its singular alias", plural_name);
        }
    }
    return 0;
}

int pm_sources_resolve(pm_source_list *out,
                       const char *platform,
                       const char *primary_root,
                       const char *primary_roms,
                       const char *primary_images,
                       char *err,
                       size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out || !primary_root || !primary_root[0]) {
        return pm__source_error(err, err_size, "invalid %s source request", "primary");
    }
    memset(out, 0, sizeof(*out));

    char roots[PM_MAX_SOURCES][PM_PATH_MAX] = {{0}};
    size_t root_count = 0;
    const char *raw_roots = getenv("SDCARD_PATHS");
    if (raw_roots && raw_roots[0]) {
        if (pm__parse_path_list("SDCARD_PATHS", raw_roots, roots, &root_count,
                                err, err_size) != 0) {
            return -1;
        }
    } else {
        if (pm__clean_absolute_path(roots[0], sizeof(roots[0]), primary_root) != 0) {
            return pm__source_error(err, err_size, "%s is malformed", "SDCARD_PATH");
        }
        root_count = 1;
    }
    char clean_primary[PM_PATH_MAX];
    if (pm__clean_absolute_path(clean_primary, sizeof(clean_primary), primary_root) != 0 ||
        strcmp(roots[0], clean_primary) != 0) {
        return pm__source_error(err, err_size,
                                "%s primary does not match SDCARD_PATH", "SDCARD_PATHS");
    }

    bool legacy_appended = false;
    const char *secondary = getenv("UMRK_SECONDARY_SDCARD_PATH");
    if (platform && strcmp(platform, "mlp1") == 0 &&
        secondary && secondary[0]) {
        char clean_secondary[PM_PATH_MAX];
        if (pm__clean_absolute_path(clean_secondary, sizeof(clean_secondary), secondary) != 0 ||
            strcmp(clean_secondary, roots[0]) == 0) {
            return pm__source_error(err, err_size,
                                    "%s is malformed or ambiguous",
                                    "UMRK_SECONDARY_SDCARD_PATH");
        }
        bool found = false;
        for (size_t i = 1; i < root_count; i++) {
            found = found || strcmp(roots[i], clean_secondary) == 0;
        }
        if (!found) {
            if (root_count >= PM_MAX_SOURCES) {
                return pm__source_error(err, err_size,
                                        "%s is ambiguous", "SDCARD_PATHS");
            }
            pm_copy(roots[root_count++], PM_PATH_MAX, clean_secondary);
            legacy_appended = true;
        }
    }
    if (platform && strcmp(platform, "mlp1") == 0 && root_count > 2) {
        return pm__source_error(err, err_size,
                                "%s has unsupported MLP1 slots", "SDCARD_PATHS");
    }

    char roms[PM_MAX_SOURCES][PM_PATH_MAX] = {{0}};
    char images[PM_MAX_SOURCES][PM_PATH_MAX] = {{0}};
    if (pm__resolve_content_paths("ROMS_PATHS", primary_roms, "Roms",
                                  roots, root_count, legacy_appended,
                                  roms, err, err_size) != 0 ||
        pm__resolve_content_paths("IMAGES_PATHS", primary_images, "Images",
                                  roots, root_count, legacy_appended,
                                  images, err, err_size) != 0) {
        return -1;
    }

    out->count = root_count;
    for (size_t i = 0; i < root_count; i++) {
        pm_source *source = &out->items[i];
        const char *id = i == 0 ? "primary" : "secondary_sd";
        if (pm_copy(source->id, sizeof(source->id), id) != 0 ||
            pm_copy(source->root, sizeof(source->root), roots[i]) != 0 ||
            pm_copy(source->roms_path, sizeof(source->roms_path), roms[i]) != 0 ||
            pm_copy(source->images_path, sizeof(source->images_path), images[i]) != 0 ||
            pm_join(source->ports_path, sizeof(source->ports_path),
                    source->roms_path, "PORTS") != 0 ||
            pm_join(source->port_images_path, sizeof(source->port_images_path),
                    source->images_path, "PORTS") != 0) {
            return pm__source_error(err, err_size, "%s path is too long", id);
        }
        source->configured = true;
        pm__resolve_availability(source, i == 0);
        pm__resolve_content_identity(source, source->roms_path, false);
        pm__resolve_content_identity(source, source->images_path, true);
    }
    return 0;
}

const pm_source *pm_sources_by_id(const pm_source_list *sources, const char *id)
{
    if (!sources || !id) {
        return NULL;
    }
    for (size_t i = 0; i < sources->count; i++) {
        if (strcmp(sources->items[i].id, id) == 0) {
            return &sources->items[i];
        }
    }
    return NULL;
}
