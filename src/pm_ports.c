#include "pm_ports.h"

#include "cJSON.h"
#include "pm_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PM_PORT_METADATA_LIMIT (512u * 1024u)
#define PM_PORT_MAX_PACKAGES 256u
#define PM_PORT_MAX_ITEMS 512u
#define PM_PORT_MAX_LAUNCHERS 64u

static bool has_suffix_casefold(const char *value, const char *suffix)
{
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcasecmp(value + value_len - suffix_len, suffix) == 0;
}

static bool invalid_component(const char *start, size_t len)
{
    if (len == 0 || (len == 2 && start[0] == '.' && start[1] == '.')) {
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c < 0x20u || c == 0x7fu || c == ':') {
            return true;
        }
    }
    return false;
}

int pm_port_normalize_relative(const char *input, char *out, size_t out_size)
{
    if (!input || !input[0] || !out || out_size == 0 ||
        input[0] == '/' || input[0] == '\\') {
        return -1;
    }
    size_t used = 0;
    const char *cursor = input;
    bool saw = false;
    while (*cursor) {
        const char *start = cursor;
        while (*cursor && *cursor != '/' && *cursor != '\\') {
            cursor++;
        }
        size_t len = (size_t)(cursor - start);
        if (len == 1 && start[0] == '.') {
            /* Normalize current-directory components away. */
        } else {
            if (invalid_component(start, len)) {
                return -1;
            }
            if (used && used + 1 >= out_size) {
                return -1;
            }
            if (used) {
                out[used++] = '/';
            }
            if (used + len >= out_size) {
                return -1;
            }
            memcpy(out + used, start, len);
            used += len;
            saw = true;
        }
        if (*cursor) {
            cursor++;
            if (!*cursor) {
                break;
            }
        }
    }
    if (!saw) {
        return -1;
    }
    out[used] = '\0';
    return 0;
}

static bool same_or_child(const char *path, const char *root)
{
    size_t root_len = strlen(root);
    return strcasecmp(path, root) == 0 ||
           (strncasecmp(path, root, root_len) == 0 && path[root_len] == '/');
}

static const char *basename_const(const char *path)
{
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static void slug_from_name(const char *name, char *out, size_t out_size)
{
    const char *base = basename_const(name);
    size_t len = strlen(base);
    if (len >= 4 && strcasecmp(base + len - 4, ".zip") == 0) {
        len -= 4;
    }
    if (len >= out_size) {
        len = out_size - 1;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)tolower((unsigned char)base[i]);
    }
    out[len] = '\0';
}

static void block_package(pm_port_package *package, const char *reason)
{
    if (!package || package->blocked_reason[0]) {
        return;
    }
    package->movable = false;
    (void)pm_copy(package->blocked_reason, sizeof(package->blocked_reason), reason);
}

static int inspect_tree(const char *path, uint64_t *bytes)
{
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || UINT64_MAX - *bytes < (uint64_t)st.st_size) {
            return -1;
        }
        *bytes += (uint64_t)st.st_size;
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return -1;
    }
    int dir_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = dir_fd >= 0 ? fdopendir(dir_fd) : NULL;
    if (!dir) {
        if (dir_fd >= 0) close(dir_fd);
        return -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char child[PM_PATH_MAX];
        if (pm_join(child, sizeof(child), path, entry->d_name) != 0 ||
            inspect_tree(child, bytes) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    return rc;
}

static int add_item(pm_port_package *package, const char *raw)
{
    char relpath[PM_PATH_MAX];
    if (pm_port_normalize_relative(raw, relpath, sizeof(relpath)) != 0) {
        block_package(package, "Package metadata contains an unsafe path.");
        return -1;
    }
    for (size_t i = 0; i < package->item_count; i++) {
        if (same_or_child(relpath, package->items[i].relpath)) {
            return 0;
        }
    }
    for (size_t i = 0; i < package->item_count;) {
        if (same_or_child(package->items[i].relpath, relpath)) {
            free(package->items[i].relpath);
            memmove(&package->items[i], &package->items[i + 1],
                    (package->item_count - i - 1) * sizeof(package->items[0]));
            package->item_count--;
        } else {
            i++;
        }
    }
    if (package->item_count >= PM_PORT_MAX_ITEMS) {
        block_package(package, "Package owns too many paths for the v1 mover.");
        return -1;
    }
    pm_port_owned_item *grown = realloc(
        package->items, (package->item_count + 1) * sizeof(package->items[0]));
    if (!grown) {
        return -1;
    }
    package->items = grown;
    pm_port_owned_item *item = &package->items[package->item_count++];
    memset(item, 0, sizeof(*item));
    item->relpath = strdup(relpath);
    return item->relpath ? 0 : -1;
}

static int add_json_paths(pm_port_package *package, const cJSON *value)
{
    if (cJSON_IsString(value) && value->valuestring) {
        return add_item(package, value->valuestring);
    }
    if (!cJSON_IsArray(value)) {
        block_package(package, "Package files metadata is malformed.");
        return -1;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, value) {
        if (!cJSON_IsString(item) || !item->valuestring ||
            add_item(package, item->valuestring) != 0) {
            return -1;
        }
    }
    return 0;
}

static int add_present_items(const char *ports_root,
                             pm_port_package *package,
                             const cJSON *array,
                             bool required)
{
    if (!array || cJSON_IsNull(array)) {
        if (required) {
            block_package(package, "Package has no complete ownership metadata.");
            return -1;
        }
        return 0;
    }
    if (!cJSON_IsArray(array)) {
        block_package(package, "Package items metadata is malformed.");
        return -1;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        char relpath[PM_PATH_MAX];
        char path[PM_PATH_MAX];
        struct stat st;
        if (!cJSON_IsString(item) || !item->valuestring ||
            pm_port_normalize_relative(item->valuestring,
                                       relpath, sizeof(relpath)) != 0 ||
            pm_join(path, sizeof(path), ports_root, relpath) != 0) {
            block_package(package, "Package items metadata contains an unsafe path.");
            return -1;
        }
        if (lstat(path, &st) == 0 && !S_ISLNK(st.st_mode) &&
            (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
            if (add_item(package, relpath) != 0) {
                return -1;
            }
        } else if (required) {
            block_package(package, "A required package item is missing or unsafe.");
            return -1;
        }
    }
    return 0;
}

static bool package_owns(const pm_port_package *package, const char *relpath)
{
    for (size_t i = 0; i < package->item_count; i++) {
        if (same_or_child(relpath, package->items[i].relpath)) {
            return true;
        }
    }
    return false;
}

static int add_launcher(pm_port_package *package, const char *relpath)
{
    if (package->launcher_count >= PM_PORT_MAX_LAUNCHERS) {
        block_package(package, "Package has too many launchers for the v1 mover.");
        return -1;
    }
    pm_port_launcher *grown = realloc(
        package->launchers,
        (package->launcher_count + 1) * sizeof(package->launchers[0]));
    if (!grown) {
        return -1;
    }
    package->launchers = grown;
    pm_port_launcher *launcher = &package->launchers[package->launcher_count++];
    memset(launcher, 0, sizeof(*launcher));
    launcher->relpath = strdup(relpath);
    char stem[PM_PATH_MAX];
    if (!launcher->relpath || pm_copy(stem, sizeof(stem), basename_const(relpath)) != 0) {
        return -1;
    }
    char *dot = strrchr(stem, '.');
    if (dot) {
        *dot = '\0';
    }
    char artwork[PM_PATH_MAX];
    if (pm_format(artwork, sizeof(artwork), "%s.png", stem) != 0) {
        return -1;
    }
    launcher->artwork_name = strdup(artwork);
    return launcher->artwork_name ? 0 : -1;
}

static int discover_launchers(const pm_source *source, pm_port_package *package)
{
    for (size_t i = 0; i < package->item_count; i++) {
        char path[PM_PATH_MAX];
        struct stat st;
        if (pm_join(path, sizeof(path), source->ports_path,
                    package->items[i].relpath) != 0 ||
            lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
            block_package(package, "An owned package path is missing or unsafe.");
            continue;
        }
        package->items[i].directory = S_ISDIR(st.st_mode);
        uint64_t bytes = 0;
        if ((!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
            inspect_tree(path, &bytes) != 0 ||
            UINT64_MAX - package->bytes < bytes) {
            block_package(package, "Package contains a symlink or special file.");
            continue;
        }
        package->items[i].bytes = bytes;
        package->bytes += bytes;

        if (S_ISREG(st.st_mode) &&
            has_suffix_casefold(package->items[i].relpath, ".sh")) {
            if (strchr(package->items[i].relpath, '/')) {
                block_package(package, "Nested launcher scripts are unsupported by v1.");
            } else if (add_launcher(package, package->items[i].relpath) != 0) {
                return -1;
            }
        }
    }
    if (package->launcher_count == 0) {
        block_package(package, "Package has no owned launcher scripts.");
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        char artwork[PM_PATH_MAX];
        struct stat st;
        if (pm_join(artwork, sizeof(artwork), source->port_images_path,
                    package->launchers[i].artwork_name) == 0 &&
            lstat(artwork, &st) == 0) {
            if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                block_package(package, "Launcher artwork is not a safe regular file.");
            } else {
                package->launchers[i].artwork_present = true;
                if (st.st_size > 0 && UINT64_MAX - package->bytes >= (uint64_t)st.st_size) {
                    package->bytes += (uint64_t)st.st_size;
                }
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (!strcasecmp(package->launchers[i].artwork_name,
                            package->launchers[j].artwork_name)) {
                block_package(package,
                              "Two launchers share one case-folded artwork target.");
            }
        }
    }
    return 0;
}

static int load_metadata(const pm_source *source,
                         const char *metadata_relpath,
                         pm_port_package *package)
{
    char metadata_path[PM_PATH_MAX];
    char read_err[256];
    if (pm_join(metadata_path, sizeof(metadata_path), source->ports_path,
                metadata_relpath) != 0) {
        return -1;
    }
    char *text = pm_read_text_file(metadata_path, PM_PORT_METADATA_LIMIT,
                                   read_err, sizeof(read_err));
    if (!text) {
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    memset(package, 0, sizeof(*package));
    package->movable = true;
    if (pm_copy(package->source_id, sizeof(package->source_id), source->id) != 0 ||
        pm_copy(package->metadata_relpath, sizeof(package->metadata_relpath),
                metadata_relpath) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
        block_package(package, "Package metadata has no archive name.");
        slug_from_name(metadata_relpath, package->slug, sizeof(package->slug));
    } else {
        slug_from_name(name->valuestring, package->slug, sizeof(package->slug));
    }
    const cJSON *attr = cJSON_GetObjectItemCaseSensitive(root, "attr");
    const cJSON *title = cJSON_IsObject(attr)
        ? cJSON_GetObjectItemCaseSensitive(attr, "title") : NULL;
    const char *display = cJSON_IsString(title) && title->valuestring
        ? title->valuestring : package->slug;
    (void)pm_copy(package->display_name, sizeof(package->display_name), display);

    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (cJSON_IsObject(files)) {
        package->authoritative = true;
        const cJSON *value = NULL;
        cJSON_ArrayForEach(value, files) {
            if (add_json_paths(package, value) != 0) {
                break;
            }
        }
    } else if (files) {
        block_package(package, "Package files metadata is malformed.");
    } else {
        (void)add_present_items(source->ports_path, package,
                                cJSON_GetObjectItemCaseSensitive(root, "items"), true);
        (void)add_present_items(source->ports_path, package,
                                cJSON_GetObjectItemCaseSensitive(root, "items_opt"), false);
    }
    if (!package_owns(package, metadata_relpath) &&
        add_item(package, metadata_relpath) != 0) {
        block_package(package, "Package metadata file cannot be owned safely.");
    }
    cJSON_Delete(root);
    if (!package->slug[0] || package->item_count == 0) {
        block_package(package, "Package ownership is incomplete.");
    }
    return discover_launchers(source, package);
}

static int append_package(pm_port_inventory *inventory, pm_port_package *package)
{
    if (inventory->package_count >= PM_PORT_MAX_PACKAGES) {
        return -1;
    }
    pm_port_package *grown = realloc(
        inventory->packages,
        (inventory->package_count + 1) * sizeof(inventory->packages[0]));
    if (!grown) {
        return -1;
    }
    inventory->packages = grown;
    inventory->packages[inventory->package_count++] = *package;
    memset(package, 0, sizeof(*package));
    return 0;
}

static bool metadata_name(const char *name)
{
    return !strcasecmp(name, "port.json") || has_suffix_casefold(name, ".port.json");
}

static int scan_source_metadata(const pm_source *source,
                                pm_port_inventory *inventory,
                                char *err,
                                size_t err_size)
{
    DIR *root = opendir(source->ports_path);
    if (!root) {
        if (errno == ENOENT) {
            return 0;
        }
        snprintf(err, err_size, "cannot scan %s: %s",
                 source->ports_path, strerror(errno));
        return -1;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(root)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[PM_PATH_MAX];
        if (pm_join(child, sizeof(child), source->ports_path, entry->d_name) != 0) {
            closedir(root);
            return -1;
        }
        struct stat st;
        if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            continue;
        }
        DIR *dir = opendir(child);
        if (!dir) {
            continue;
        }
        struct dirent *nested = NULL;
        while ((nested = readdir(dir)) != NULL) {
            if (nested->d_name[0] == '.' || !metadata_name(nested->d_name)) {
                continue;
            }
            char relpath[PM_PATH_MAX];
            if (pm_join(relpath, sizeof(relpath), entry->d_name,
                        nested->d_name) != 0) {
                continue;
            }
            pm_port_package package;
            if (load_metadata(source, relpath, &package) == 0 &&
                append_package(inventory, &package) != 0) {
                closedir(dir);
                closedir(root);
                snprintf(err, err_size, "installed package inventory is too large");
                return -1;
            }
        }
        closedir(dir);
    }
    closedir(root);
    return 0;
}

static void mark_ownership_conflicts(pm_port_inventory *inventory)
{
    for (size_t i = 0; i < inventory->package_count; i++) {
        pm_port_package *a = &inventory->packages[i];
        for (size_t j = i + 1; j < inventory->package_count; j++) {
            pm_port_package *b = &inventory->packages[j];
            if (!strcasecmp(a->slug, b->slug)) {
                block_package(a, "Package exists on more than one card.");
                block_package(b, "Package exists on more than one card.");
            }
            if (strcmp(a->source_id, b->source_id) != 0) {
                continue;
            }
            bool shared = false;
            for (size_t ai = 0; ai < a->item_count && !shared; ai++) {
                for (size_t bi = 0; bi < b->item_count; bi++) {
                    if (same_or_child(a->items[ai].relpath, b->items[bi].relpath) ||
                        same_or_child(b->items[bi].relpath, a->items[ai].relpath)) {
                        shared = true;
                        break;
                    }
                }
            }
            if (shared) {
                block_package(a, "An owned path is shared with another package.");
                block_package(b, "An owned path is shared with another package.");
            }
        }
    }
}

static int append_unmanaged(pm_port_inventory *inventory,
                            const char *source_id,
                            const char *script)
{
    char text[PM_PATH_MAX];
    if (pm_format(text, sizeof(text), "%s:%s", source_id, script) != 0) {
        return -1;
    }
    char **grown = realloc(inventory->unmanaged_launchers,
                           (inventory->unmanaged_count + 1) *
                           sizeof(inventory->unmanaged_launchers[0]));
    if (!grown) {
        return -1;
    }
    inventory->unmanaged_launchers = grown;
    inventory->unmanaged_launchers[inventory->unmanaged_count] = strdup(text);
    if (!inventory->unmanaged_launchers[inventory->unmanaged_count]) {
        return -1;
    }
    inventory->unmanaged_count++;
    return 0;
}

static int scan_unmanaged(const pm_context *ctx, pm_port_inventory *inventory)
{
    for (size_t s = 0; s < ctx->sources.count; s++) {
        const pm_source *source = &ctx->sources.items[s];
        if (!source->available) {
            continue;
        }
        DIR *dir = opendir(source->ports_path);
        if (!dir) {
            continue;
        }
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.' ||
                !has_suffix_casefold(entry->d_name, ".sh")) {
                continue;
            }
            bool owned = false;
            for (size_t i = 0; i < inventory->package_count && !owned; i++) {
                pm_port_package *package = &inventory->packages[i];
                owned = !strcmp(package->source_id, source->id) &&
                        package_owns(package, entry->d_name);
            }
            if (!owned && append_unmanaged(inventory, source->id, entry->d_name) != 0) {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
    }
    return 0;
}

int pm_port_inventory_load(pm_context *ctx,
                           pm_port_inventory *out,
                           char *err,
                           size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    if (!ctx || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (pm_context_refresh_sources(ctx, err, err_size) != 0) {
        return -1;
    }
    for (size_t i = 0; i < ctx->sources.count; i++) {
        const pm_source *source = &ctx->sources.items[i];
        if (source->available &&
            scan_source_metadata(source, out, err, err_size) != 0) {
            pm_port_inventory_free(out);
            return -1;
        }
    }
    mark_ownership_conflicts(out);
    if (scan_unmanaged(ctx, out) != 0) {
        pm_port_inventory_free(out);
        snprintf(err, err_size, "cannot build unmanaged launcher inventory");
        return -1;
    }
    return 0;
}

void pm_port_inventory_free(pm_port_inventory *inventory)
{
    if (!inventory) {
        return;
    }
    for (size_t i = 0; i < inventory->package_count; i++) {
        pm_port_package *package = &inventory->packages[i];
        for (size_t j = 0; j < package->item_count; j++) {
            free(package->items[j].relpath);
        }
        for (size_t j = 0; j < package->launcher_count; j++) {
            free(package->launchers[j].relpath);
            free(package->launchers[j].artwork_name);
        }
        free(package->items);
        free(package->launchers);
    }
    for (size_t i = 0; i < inventory->unmanaged_count; i++) {
        free(inventory->unmanaged_launchers[i]);
    }
    free(inventory->packages);
    free(inventory->unmanaged_launchers);
    memset(inventory, 0, sizeof(*inventory));
}

const pm_port_package *pm_port_inventory_find(const pm_port_inventory *inventory,
                                              const char *source_id,
                                              const char *slug)
{
    if (!inventory || !source_id || !slug) {
        return NULL;
    }
    for (size_t i = 0; i < inventory->package_count; i++) {
        const pm_port_package *package = &inventory->packages[i];
        if (!strcmp(package->source_id, source_id) &&
            !strcasecmp(package->slug, slug)) {
            return package;
        }
    }
    return NULL;
}

static int appendf(char *out, size_t out_size, size_t *used, const char *fmt, ...)
{
    if (*used >= out_size) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(out + *used, out_size - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= out_size - *used) {
        *used = out_size - 1;
        out[*used] = '\0';
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

int pm_port_inventory_text(pm_context *ctx,
                           char *out,
                           size_t out_size,
                           char *err,
                           size_t err_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    pm_port_inventory inventory;
    if (pm_port_inventory_load(ctx, &inventory, err, err_size) != 0) {
        return -1;
    }
    size_t used = 0;
    int rc = 0;
    for (size_t i = 0; i < inventory.package_count; i++) {
        const pm_port_package *package = &inventory.packages[i];
        if (appendf(out, out_size, &used,
                    "package=%s\tsource=%s\tmovable=%d\tauthoritative=%d"
                    "\tlaunchers=%zu\titems=%zu\tbytes=%llu\treason=%s\n",
                    package->slug, package->source_id, package->movable ? 1 : 0,
                    package->authoritative ? 1 : 0, package->launcher_count,
                    package->item_count, (unsigned long long)package->bytes,
                    package->blocked_reason) != 0) {
            rc = -1;
        }
        for (size_t j = 0; j < package->launcher_count; j++) {
            if (appendf(out, out_size, &used,
                        " launcher=%s\tartwork=%s\tpresent=%d\n",
                        package->launchers[j].relpath,
                        package->launchers[j].artwork_name,
                        package->launchers[j].artwork_present ? 1 : 0) != 0) {
                rc = -1;
            }
        }
    }
    for (size_t i = 0; i < inventory.unmanaged_count; i++) {
        if (appendf(out, out_size, &used, "unmanaged=%s\n",
                    inventory.unmanaged_launchers[i]) != 0) {
            rc = -1;
        }
    }
    pm_port_inventory_free(&inventory);
    return rc;
}
