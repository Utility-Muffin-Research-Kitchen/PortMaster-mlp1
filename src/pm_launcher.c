#include "pm_launcher.h"

#include "pm_artwork.h"
#include "pm_controller_layout.h"
#include "pm_env_snapshot.h"
#include "pm_installer.h"
#include "pm_preferences.h"
#include "pm_util.h"

#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool pm_env_truthy(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
}

static bool pm_valid_cfw_version(const char *value)
{
    return value && value[0] && strcmp(value, "Unknown") != 0;
}

static bool pm_json_version_from_file(const char *path, char *out, size_t out_size)
{
    if (!path || !pm_file_exists(path)) {
        return false;
    }

    char read_err[128];
    char *text = pm_read_text_file(path, 256 * 1024, read_err, sizeof(read_err));
    if (!text) {
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return false;
    }

    const char *keys[] = { "version", "release_id", NULL };
    bool ok = false;
    for (size_t i = 0; keys[i]; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsString(item) && pm_valid_cfw_version(item->valuestring) &&
            pm_copy(out, out_size, item->valuestring) == 0) {
            ok = true;
            break;
        }
    }

    cJSON_Delete(root);
    return ok;
}

static void pm_resolve_cfw_version(pm_context *ctx, char *out, size_t out_size)
{
    const char *env_version = getenv("CFW_VERSION");
    if (pm_valid_cfw_version(env_version) &&
        pm_copy(out, out_size, env_version) == 0) {
        return;
    }

    char path[PM_PATH_MAX];
    if (ctx &&
        pm_format(path, sizeof(path), "%s/.system/leaf/platforms/%s/release.json",
                  ctx->sdcard_path, ctx->platform) == 0 &&
        pm_json_version_from_file(path, out, out_size)) {
        return;
    }
    if (ctx &&
        pm_format(path, sizeof(path), "%s/.system/leaf/platforms/%s/manifest.json",
                  ctx->sdcard_path, ctx->platform) == 0 &&
        pm_json_version_from_file(path, out, out_size)) {
        return;
    }
    if (ctx &&
        pm_format(path, sizeof(path), "%s/.umrk/%s/release.json",
                  ctx->sdcard_path, ctx->platform) == 0 &&
        pm_json_version_from_file(path, out, out_size)) {
        return;
    }

    (void)pm_copy(out, out_size, PM_VERSION);
}

static bool pm_taskset_available(void)
{
    char *argv[] = {
        "sh",
        "-c",
        "command -v taskset >/dev/null 2>&1 && taskset 0xF true",
        NULL,
    };
    return pm_run_argv(argv, NULL, 0) == 0;
}

bool pm_portmaster_is_installed(const pm_context *ctx)
{
    char script[PM_PATH_MAX];
    return ctx &&
           pm_join(script, sizeof(script), ctx->portmaster_dir, "PortMaster.sh") == 0 &&
           pm_file_exists(script);
}

bool pm_portmaster_runtime_available(const pm_context *ctx,
                                     char *runtime_path,
                                     size_t runtime_path_size,
                                     bool *uses_system_python)
{
    if (runtime_path && runtime_path_size > 0) {
        runtime_path[0] = '\0';
    }
    if (uses_system_python) {
        *uses_system_python = false;
    }
    if (!ctx) {
        return false;
    }

    char managed[PM_PATH_MAX];
    if (pm_join3(managed, sizeof(managed), ctx->runtime_dir, "bin", "python3") == 0 &&
        pm_file_exists(managed)) {
        if (runtime_path && runtime_path_size > 0) {
            pm_copy(runtime_path, runtime_path_size, managed);
        }
        return true;
    }

    const char *system_python = NULL;
    if (pm_file_exists("/usr/bin/python3")) {
        system_python = "/usr/bin/python3";
    } else if (pm_file_exists("/bin/python3")) {
        system_python = "/bin/python3";
    }
    if (!system_python) {
        return false;
    }

    if (runtime_path && runtime_path_size > 0) {
        pm_copy(runtime_path, runtime_path_size, system_python);
    }
    if (uses_system_python) {
        *uses_system_python = true;
    }
    return true;
}

bool pm_portmaster_launch_ready(const pm_context *ctx, char *reason, size_t reason_size)
{
    if (reason && reason_size > 0) {
        reason[0] = '\0';
    }
    if (!ctx) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size, "PortMaster setup is unavailable.");
        }
        return false;
    }
    if (!pm_portmaster_is_installed(ctx)) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size,
                     "PortMaster is not installed yet.\n\nUse Install PortMaster to set it up.");
        }
        return false;
    }
    if (!pm_portmaster_runtime_available(ctx, NULL, 0, NULL)) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size,
                     "PortMaster setup is incomplete.\n\nUse Repair PortMaster to finish setup.");
        }
        return false;
    }
    if (!pm_armhf_compat_current(ctx)) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size,
                     "PortMaster armhf support is missing or outdated.\n\nUse Repair PortMaster to finish setup.");
        }
        return false;
    }
    return true;
}

static uint64_t pm_hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t pm_hash_u64(uint64_t hash, uint64_t value)
{
    return pm_hash_bytes(hash, &value, sizeof(value));
}

static uint64_t pm_stat_mtime_nsec(const struct stat *st)
{
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_nsec;
#else
    return (uint64_t)st->st_mtim.tv_nsec;
#endif
}

static bool pm_ports_tree_stamp(pm_context *ctx, uint64_t *out)
{
    if (!ctx || !out || !ctx->ports_dir[0]) {
        return false;
    }

    DIR *dir = opendir(ctx->ports_dir);
    if (!dir) {
        return false;
    }

    uint64_t acc = 1469598103934665603ULL;
    uint64_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0 ||
            strcmp(ent->d_name, ".leaf-armhf") == 0) {
            continue;
        }

        char path[PM_PATH_MAX];
        if (pm_join(path, sizeof(path), ctx->ports_dir, ent->d_name) != 0) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        uint64_t entry = 1469598103934665603ULL;
        entry = pm_hash_bytes(entry, ent->d_name, strlen(ent->d_name));
        entry = pm_hash_u64(entry, (uint64_t)st.st_mode);
        entry = pm_hash_u64(entry, (uint64_t)st.st_size);
        entry = pm_hash_u64(entry, (uint64_t)st.st_mtime);
        entry = pm_hash_u64(entry, pm_stat_mtime_nsec(&st));
        acc ^= entry;
        count++;
    }
    closedir(dir);

    acc = pm_hash_u64(acc, count);
    *out = acc;
    return true;
}

typedef struct {
    char source_id[32];
    bool available;
    bool stamp_valid;
    uint64_t stamp;
} pm_source_tree_stamp;

typedef struct {
    pm_source_tree_stamp items[PM_MAX_SOURCES];
    size_t count;
} pm_source_tree_snapshot;

static bool pm_source_id_is_safe(const char *source_id)
{
    if (!source_id || !source_id[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)source_id; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

static bool pm_source_artifact_path(pm_context *ctx,
                                    const char *stem,
                                    const char *source_id,
                                    const char *extension,
                                    char *out,
                                    size_t out_size)
{
    char filename[128];
    return ctx && stem && extension && pm_source_id_is_safe(source_id) &&
           pm_format(filename, sizeof(filename), "%s.%s.%s",
                     stem, source_id, extension) == 0 &&
           pm_join(out, out_size, ctx->leaf_dir, filename) == 0;
}

static bool pm_capture_source_tree_snapshot(pm_context *ctx,
                                            pm_source_tree_snapshot *out)
{
    if (!ctx || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < ctx->sources.count; i++) {
        const pm_source *source = &ctx->sources.items[i];
        if (!source->configured || out->count >= PM_MAX_SOURCES) {
            continue;
        }
        pm_source_tree_stamp *item = &out->items[out->count++];
        if (pm_copy(item->source_id, sizeof(item->source_id), source->id) != 0) {
            return false;
        }
        item->available = source->available;
        if (!source->available) {
            continue;
        }
        pm_context source_ctx;
        char context_err[128];
        if (pm_context_for_source(ctx, source, &source_ctx,
                                  context_err, sizeof(context_err)) != 0) {
            return false;
        }
        item->stamp_valid = pm_ports_tree_stamp(&source_ctx, &item->stamp);
    }
    return out->count > 0;
}

static const pm_source_tree_stamp *
pm_snapshot_source(const pm_source_tree_snapshot *snapshot, const char *source_id)
{
    if (!snapshot || !source_id) {
        return NULL;
    }
    for (size_t i = 0; i < snapshot->count; i++) {
        if (strcmp(snapshot->items[i].source_id, source_id) == 0) {
            return &snapshot->items[i];
        }
    }
    return NULL;
}

static bool pm_snapshot_union_stamp(const pm_source_tree_snapshot *snapshot,
                                    uint64_t *out)
{
    if (!snapshot || !out) {
        return false;
    }
    uint64_t acc = 1469598103934665603ULL;
    size_t available_count = 0;
    for (size_t i = 0; i < snapshot->count; i++) {
        const pm_source_tree_stamp *item = &snapshot->items[i];
        if (!item->available) {
            continue;
        }
        acc = pm_hash_bytes(acc, item->source_id, strlen(item->source_id));
        acc = pm_hash_u64(acc, item->stamp_valid ? 1u : 0u);
        acc = pm_hash_u64(acc, item->stamp);
        available_count++;
    }
    *out = pm_hash_u64(acc, available_count);
    return available_count > 0;
}

static bool pm_source_snapshots_equal(const pm_source_tree_snapshot *left,
                                      const pm_source_tree_snapshot *right)
{
    if (!left || !right || left->count != right->count) {
        return false;
    }
    for (size_t i = 0; i < left->count; i++) {
        const pm_source_tree_stamp *a = &left->items[i];
        const pm_source_tree_stamp *b =
            pm_snapshot_source(right, a->source_id);
        if (!b ||
            a->available != b->available ||
            a->stamp_valid != b->stamp_valid ||
            (a->stamp_valid && a->stamp != b->stamp)) {
            return false;
        }
    }
    return true;
}

static bool pm_port_scan_stamp_path(pm_context *ctx,
                                    const char *source_id,
                                    char *out,
                                    size_t out_size)
{
    if (source_id) {
        return pm_source_artifact_path(ctx, "port-tree", source_id,
                                       "stamp", out, out_size);
    }
    return ctx && pm_join(out, out_size, ctx->leaf_dir,
                          "port-tree.stamp") == 0;
}

static bool pm_read_port_scan_stamp(pm_context *ctx,
                                    const char *source_id,
                                    uint64_t *out)
{
    char path[PM_PATH_MAX];
    if (!out ||
        !pm_port_scan_stamp_path(ctx, source_id, path, sizeof(path))) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    unsigned long long value = 0;
    int matched = fscanf(fp, "%llx", &value);
    fclose(fp);
    if (matched != 1) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static void pm_write_port_scan_stamp(pm_context *ctx,
                                     const char *source_id,
                                     uint64_t stamp)
{
    char path[PM_PATH_MAX];
    char tmp[PM_PATH_MAX];
    if (!pm_port_scan_stamp_path(ctx, source_id, path, sizeof(path)) ||
        pm_format(tmp, sizeof(tmp), "%s.tmp", path) != 0) {
        return;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return;
    }
    fprintf(fp, "%016llx\n", (unsigned long long)stamp);
    if (fclose(fp) != 0) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
    }
}

static bool pm_text_file_contains(const char *path, const char *needle, size_t max_bytes)
{
    if (!path || !needle || !needle[0] || !pm_file_exists(path)) {
        return false;
    }

    char err[128];
    char *text = pm_read_text_file(path, max_bytes, err, sizeof(err));
    if (!text) {
        return false;
    }
    bool found = strstr(text, needle) != NULL;
    free(text);
    return found;
}

static bool pm_armhf_scan_artifacts_match_context(pm_context *ctx)
{
    if (!ctx || !pm_source_id_is_safe(ctx->source_id)) {
        return false;
    }

    char scan_path[PM_PATH_MAX];
    char hook_path[PM_PATH_MAX];
    if (!pm_source_artifact_path(ctx, "armhf-scan", ctx->source_id,
                                 "json", scan_path, sizeof(scan_path)) ||
        pm_join(hook_path, sizeof(hook_path), ctx->portmaster_dir, "leaf-armhf-env.sh") != 0) {
        return false;
    }

    return pm_text_file_contains(scan_path, ctx->ports_dir, 256 * 1024) &&
           pm_text_file_contains(scan_path, ctx->portmaster_dir, 256 * 1024) &&
           pm_text_file_contains(hook_path, ctx->data_dir, 1024 * 1024);
}

bool pm_refresh_armhf_port_wrappers(pm_context *ctx)
{
    char script[PM_PATH_MAX];
    char report_tsv[PM_PATH_MAX];
    char report_json[PM_PATH_MAX];
    char manifest[PM_PATH_MAX];
    char latest_tsv[PM_PATH_MAX];
    char latest_json[PM_PATH_MAX];
    char latest_manifest[PM_PATH_MAX];
    if (!ctx ||
        !pm_source_id_is_safe(ctx->source_id) ||
        pm_join3(script, sizeof(script), ctx->pak_dir,
                 "scripts", "scan-and-fix-port-elfs.sh") != 0 ||
        !pm_source_artifact_path(ctx, "armhf-scan", ctx->source_id,
                                 "tsv", report_tsv, sizeof(report_tsv)) ||
        !pm_source_artifact_path(ctx, "armhf-scan", ctx->source_id,
                                 "json", report_json, sizeof(report_json)) ||
        !pm_source_artifact_path(ctx, "armhf-scan", ctx->source_id,
                                 "manifest", manifest, sizeof(manifest)) ||
        pm_join(latest_tsv, sizeof(latest_tsv), ctx->leaf_dir,
                "armhf-scan.tsv") != 0 ||
        pm_join(latest_json, sizeof(latest_json), ctx->leaf_dir,
                "armhf-scan.json") != 0 ||
        pm_join(latest_manifest, sizeof(latest_manifest), ctx->leaf_dir,
                "armhf-scan.manifest") != 0 ||
        !pm_file_exists(script)) {
        return false;
    }

    char mkdir_err[128];
    if (pm_mkdir_p(ctx->ports_dir, mkdir_err, sizeof(mkdir_err)) != 0) {
        fprintf(stderr, "PortMaster armhf scan warning: %s\n", mkdir_err);
        return false;
    }

    pm_env_override env[] = {
        { "PLATFORM", ctx->platform },
        { "SDCARD_PATH", ctx->sdcard_path },
        { "USERDATA_PATH", ctx->userdata_path },
        { "ROMS_PATH", ctx->roms_path },
        { "IMAGES_PATH", ctx->images_path },
        { "PORTMASTER_MLP1_DATA_DIR", ctx->data_dir },
        { "PORTMASTER_CONTROLFOLDER", ctx->portmaster_dir },
        { "LEAF_PM_ARMHF_SCAN_TSV", report_tsv },
        { "LEAF_PM_ARMHF_SCAN_JSON", report_json },
        { "LEAF_PM_ARMHF_SCAN_MANIFEST", manifest },
        { "LEAF_PM_ARMHF_SCAN_LATEST_TSV", latest_tsv },
        { "LEAF_PM_ARMHF_SCAN_LATEST_JSON", latest_json },
        { "LEAF_PM_ARMHF_SCAN_LATEST_MANIFEST", latest_manifest },
        { NULL, NULL },
    };
    char *argv[] = { "bash", script, ctx->ports_dir, NULL };
    char scan_err[512];
    if (pm_run_argv_env_in_dir(ctx->pak_dir, argv, env,
                               scan_err, sizeof(scan_err)) != 0) {
        fprintf(stderr, "PortMaster armhf scan warning: %s\n", scan_err);
        return false;
    }
    uint64_t stamp = 0;
    if (pm_ports_tree_stamp(ctx, &stamp)) {
        pm_write_port_scan_stamp(ctx, ctx->source_id, stamp);
    }
    return true;
}

static bool pm_refresh_armhf_source_mask(pm_context *ctx,
                                         unsigned int source_mask,
                                         const char *selected_source_id,
                                         size_t *scanned_count)
{
    bool all_ok = true;
    size_t scanned = 0;
    if (!ctx) {
        return false;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < ctx->sources.count; i++) {
            const pm_source *source = &ctx->sources.items[i];
            bool selected = selected_source_id &&
                            strcmp(source->id, selected_source_id) == 0;
            if (!(source_mask & (1u << i)) ||
                !source->configured || !source->available ||
                (pass == 0 && selected) || (pass == 1 && !selected)) {
                continue;
            }
            pm_context source_ctx;
            char context_err[128];
            if (pm_context_for_source(ctx, source, &source_ctx,
                                      context_err, sizeof(context_err)) != 0 ||
                !pm_refresh_armhf_port_wrappers(&source_ctx)) {
                all_ok = false;
                continue;
            }
            scanned++;
        }
    }
    if (scanned_count) {
        *scanned_count = scanned;
    }
    return all_ok;
}

static unsigned int
pm_stale_armhf_source_mask(pm_context *ctx,
                           const pm_source_tree_snapshot *snapshot,
                           bool force)
{
    unsigned int mask = 0;
    if (!ctx || !snapshot) {
        return 0;
    }
    for (size_t i = 0; i < ctx->sources.count; i++) {
        const pm_source *source = &ctx->sources.items[i];
        const pm_source_tree_stamp *current =
            pm_snapshot_source(snapshot, source->id);
        if (!source->configured || !source->available) {
            continue;
        }
        if (!current) {
            mask |= 1u << i;
            continue;
        }
        pm_context source_ctx;
        char context_err[128];
        uint64_t stored_stamp = 0;
        bool context_ok =
            pm_context_for_source(ctx, source, &source_ctx,
                                  context_err, sizeof(context_err)) == 0;
        bool stored_ok = context_ok &&
                         pm_read_port_scan_stamp(&source_ctx, source->id,
                                                 &stored_stamp);
        bool artifacts_ok = context_ok &&
                            pm_armhf_scan_artifacts_match_context(&source_ctx);
        if (force || !current->stamp_valid || !stored_ok ||
            current->stamp != stored_stamp || !artifacts_ok) {
            mask |= 1u << i;
        }
    }
    return mask;
}

static unsigned int
pm_changed_armhf_source_mask(pm_context *ctx,
                             const pm_source_tree_snapshot *before,
                             const pm_source_tree_snapshot *after)
{
    unsigned int mask = 0;
    if (!ctx || !after) {
        return 0;
    }
    for (size_t i = 0; i < ctx->sources.count; i++) {
        const pm_source *source = &ctx->sources.items[i];
        const pm_source_tree_stamp *old =
            pm_snapshot_source(before, source->id);
        const pm_source_tree_stamp *current =
            pm_snapshot_source(after, source->id);
        if (!source->configured || !source->available || !current) {
            continue;
        }
        if (!old || !old->available ||
            old->stamp_valid != current->stamp_valid ||
            (current->stamp_valid && old->stamp != current->stamp)) {
            mask |= 1u << i;
        }
    }
    return mask;
}

static bool pm_write_union_port_scan_stamp(pm_context *ctx,
                                           const pm_source_tree_snapshot *snapshot)
{
    uint64_t union_stamp = 0;
    if (!pm_snapshot_union_stamp(snapshot, &union_stamp)) {
        return false;
    }
    pm_write_port_scan_stamp(ctx, NULL, union_stamp);
    return true;
}

int pm_refresh_stale_armhf_port_wrappers(pm_context *ctx,
                                         const char *selected_source_id,
                                         bool force,
                                         size_t *scanned_count)
{
    pm_source_tree_snapshot snapshot;
    if (scanned_count) {
        *scanned_count = 0;
    }
    if (!pm_capture_source_tree_snapshot(ctx, &snapshot)) {
        return -1;
    }
    unsigned int source_mask =
        pm_stale_armhf_source_mask(ctx, &snapshot, force);
    if (!pm_refresh_armhf_source_mask(ctx, source_mask,
                                      selected_source_id, scanned_count)) {
        return -1;
    }
    if (!pm_capture_source_tree_snapshot(ctx, &snapshot) ||
        !pm_write_union_port_scan_stamp(ctx, &snapshot)) {
        return -1;
    }
    return 0;
}

static bool pm_resolve_jawaka_platformctl(pm_context *ctx, char *out, size_t out_size)
{
    const char *explicit_ctl = getenv("JAWAKA_PLATFORMCTL");
    if (explicit_ctl && explicit_ctl[0] && pm_copy(out, out_size, explicit_ctl) == 0) {
        return true;
    }

    const char *bin_dir = getenv("UMRK_BIN_PATH");
    if (bin_dir && bin_dir[0] && pm_join(out, out_size, bin_dir, "jawaka-platformctl") == 0 &&
        pm_file_exists(out)) {
        return true;
    }

    const char *launcher_dir = getenv("UMRK_LAUNCHER_PATH");
    if (launcher_dir && launcher_dir[0] &&
        pm_join3(out, out_size, launcher_dir, "bin", "jawaka-platformctl") == 0 &&
        pm_file_exists(out)) {
        return true;
    }

    if (ctx && ctx->sdcard_path[0] && ctx->platform[0]) {
        char platform_root[PM_PATH_MAX];
        if (pm_format(platform_root, sizeof(platform_root),
                      "%s/.system/leaf/platforms/%s/launcher/bin",
                      ctx->sdcard_path, ctx->platform) == 0 &&
            pm_join(out, out_size, platform_root, "jawaka-platformctl") == 0 &&
            pm_file_exists(out)) {
            return true;
        }
    }

    return pm_copy(out, out_size, "jawaka-platformctl") == 0;
}

void pm_request_jawaka_library_rescan(pm_context *ctx)
{
    char ctl[PM_PATH_MAX];
    if (!pm_resolve_jawaka_platformctl(ctx, ctl, sizeof(ctl))) {
        return;
    }

    char socket[PM_PATH_MAX];
    const char *socket_env = getenv("JAWAKA_SOCKET_PATH");
    if (socket_env && socket_env[0]) {
        if (pm_copy(socket, sizeof(socket), socket_env) != 0) {
            return;
        }
    } else {
        const char *runtime_dir = pm_env("UMRK_RUNTIME_PATH", "/tmp/jawaka-runtime");
        if (pm_join(socket, sizeof(socket), runtime_dir, "jawakad.sock") != 0) {
            return;
        }
    }

    char *argv[] = {
        ctl,
        "--socket",
        socket,
        "request",
        "{\"type\":\"scan-library\"}",
        NULL,
    };
    char scan_err[256];
    if (pm_run_argv(argv, scan_err, sizeof(scan_err)) != 0) {
        fprintf(stderr, "Leaf launcher library rescan warning: %s\n", scan_err);
    }
}

static void pm_launch_status(pm_launch_status_fn status_fn,
                             void *status_userdata,
                             const char *message)
{
    if (status_fn) {
        status_fn(status_userdata, message);
    }
}

static int pm_append_source_field(char *out, size_t out_size,
                                  const char *value)
{
    size_t used = strlen(out);
    int written = snprintf(out + used, out_size - used, "%s%s",
                           used ? ":" : "", value ? value : "");
    return written >= 0 && (size_t)written < out_size - used ? 0 : -1;
}

int pm_launch_portmaster_from_source_with_status(pm_context *ctx,
                                                 const char *session_source_id,
                                                 pm_launch_status_fn status_fn,
                                                 void *status_userdata,
                                                 char *err,
                                                 size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        snprintf(err, err_size, "missing PortMaster context");
        return -1;
    }
    pm_context *base_ctx = ctx;
    const pm_source *selected_source = NULL;
    if (pm_install_source_resolve(base_ctx, session_source_id,
                                  &selected_source, err, err_size) != 0) {
        return -1;
    }
    char selected_source_id[32];
    if (pm_copy(selected_source_id, sizeof(selected_source_id),
                selected_source->id) != 0) {
        snprintf(err, err_size, "selected source ID is too long");
        return -1;
    }
    const pm_source *primary_source =
        pm_sources_by_id(&base_ctx->sources, "primary");
    if (!primary_source || !primary_source->available) {
        snprintf(err, err_size, "Primary SD source is no longer available");
        return -1;
    }
    pm_context launch_ctx;
    if (pm_context_for_source(base_ctx, selected_source, &launch_ctx,
                              err, err_size) != 0) {
        return -1;
    }
    ctx = &launch_ctx;

    char read_ports[PM_PATH_MAX * PM_MAX_SOURCES + PM_MAX_SOURCES];
    char read_scripts[sizeof(read_ports)];
    char read_ids[128];
    char read_roots[PM_PATH_MAX * PM_MAX_SOURCES + PM_MAX_SOURCES];
    char read_mount_ids[128];
    char read_devices[128];
    char read_st_devs[128];
    char read_ports_st_devs[128];
    char read_fingerprints[512];
    read_ports[0] = read_scripts[0] = read_ids[0] = read_roots[0] = '\0';
    read_mount_ids[0] = read_devices[0] = read_st_devs[0] = '\0';
    read_ports_st_devs[0] = '\0';
    read_fingerprints[0] = '\0';
    for (size_t i = 0; i < base_ctx->sources.count; i++) {
        const pm_source *source = &base_ctx->sources.items[i];
        if (!source->configured || !source->available) {
            continue;
        }
        char mount_id[32];
        char st_dev[32];
        char ports_st_dev[32];
        char device[32];
        char fingerprint[128];
        snprintf(mount_id, sizeof(mount_id), "%lu", source->mount_id);
        snprintf(st_dev, sizeof(st_dev), "%llu",
                 (unsigned long long)source->st_dev);
        snprintf(ports_st_dev, sizeof(ports_st_dev), "%llu",
                 (unsigned long long)source->roms_st_dev);
        (void)pm_copy(device, sizeof(device), source->device_id);
        if (!device[0]) {
            (void)pm_copy(device, sizeof(device), "-");
        }
        char *device_separator = strchr(device, ':');
        if (device_separator) {
            *device_separator = ',';
        }
        (void)pm_copy(fingerprint, sizeof(fingerprint),
                      source->filesystem_fingerprint);
        if (!fingerprint[0]) {
            (void)pm_copy(fingerprint, sizeof(fingerprint), "-");
        }
        for (char *p = fingerprint; *p; p++) {
            if (*p == ':') {
                *p = ',';
            }
        }
        if (pm_append_source_field(read_ports, sizeof(read_ports),
                                   source->ports_path) != 0 ||
            pm_append_source_field(read_scripts, sizeof(read_scripts),
                                   source->ports_path) != 0 ||
            pm_append_source_field(read_ids, sizeof(read_ids), source->id) != 0 ||
            pm_append_source_field(read_roots, sizeof(read_roots),
                                   source->root) != 0 ||
            pm_append_source_field(read_mount_ids, sizeof(read_mount_ids),
                                   mount_id) != 0 ||
            pm_append_source_field(read_devices, sizeof(read_devices),
                                   device) != 0 ||
            pm_append_source_field(read_st_devs, sizeof(read_st_devs),
                                   st_dev) != 0 ||
            pm_append_source_field(read_ports_st_devs,
                                   sizeof(read_ports_st_devs),
                                   ports_st_dev) != 0 ||
            pm_append_source_field(read_fingerprints,
                                   sizeof(read_fingerprints),
                                   fingerprint) != 0) {
            snprintf(err, err_size, "multi-source launch environment is too long");
            return -1;
        }
    }
    char last_known_inventory[PM_PATH_MAX];
    if (pm_join(last_known_inventory, sizeof(last_known_inventory),
                base_ctx->leaf_dir, "installed-inventory.json") != 0) {
        snprintf(err, err_size, "inventory path is too long");
        return -1;
    }

    if (!pm_portmaster_is_installed(ctx)) {
        snprintf(err, err_size, "PortMaster is not installed: %s", ctx->portmaster_dir);
        return -1;
    }

    pm_launch_status(status_fn, status_userdata, "Preparing PortMaster...");
    if (pm_mkdir_p(ctx->ports_dir, err, err_size) != 0 ||
        pm_mkdir_p(ctx->port_images_dir, err, err_size) != 0) {
        return -1;
    }
    if (pm_repatch_portmaster(ctx, err, err_size) != 0) {
        return -1;
    }

    pm_source_tree_snapshot ports_before;
    bool have_ports_before =
        pm_capture_source_tree_snapshot(base_ctx, &ports_before);
    bool force_scan = pm_env_truthy("LEAF_PM_SCAN_BEFORE_LAUNCH");
    unsigned int stale_source_mask =
        pm_stale_armhf_source_mask(base_ctx, &ports_before, force_scan);
    bool scanned_before_launch = false;
    if (!have_ports_before || stale_source_mask != 0) {
        pm_launch_status(status_fn, status_userdata, "Scanning ports...");
        size_t scanned_count = 0;
        bool scans_ok = pm_refresh_armhf_source_mask(
            base_ctx, stale_source_mask, selected_source_id, &scanned_count);
        scanned_before_launch = scanned_count > 0;
        have_ports_before =
            pm_capture_source_tree_snapshot(base_ctx, &ports_before);
        if (scans_ok && have_ports_before) {
            (void)pm_write_union_port_scan_stamp(base_ctx, &ports_before);
        }
    }

    pm_launch_status(status_fn, status_userdata, "Preparing runtime...");
    if (pm_controller_layout_sync_hook(ctx, err, err_size) != 0) {
        return -1;
    }

    char runtime_python[PM_PATH_MAX];
    bool uses_system_python = false;
    bool has_runtime = pm_portmaster_runtime_available(ctx, runtime_python, sizeof(runtime_python),
                                                       &uses_system_python);
    if (!has_runtime) {
        snprintf(err, err_size, "Python runtime missing; install a managed runtime archive first");
        return -1;
    }

    const char *base_path = pm_env("PATH", "/usr/bin:/usr/sbin:/bin");
    const char *base_ld = pm_env("LD_LIBRARY_PATH", "");
    char tools_bin[PM_PATH_MAX];
    bool has_tools_bin = pm_join3(tools_bin, sizeof(tools_bin),
                                  ctx->data_dir, "compat/tools", "aarch64/bin") == 0 &&
                         pm_dir_exists(tools_bin);
    char compat_lib_dir[PM_PATH_MAX];
    bool has_compat_libs = pm_join3(compat_lib_dir, sizeof(compat_lib_dir),
                                    ctx->data_dir, "compat/libs", "aarch64") == 0 &&
                           pm_dir_exists(compat_lib_dir);
    bool enable_compat_libs = has_compat_libs && pm_env_truthy("LEAF_PM_ENABLE_AARCH64_COMPAT_LIBS");
    char path_env[PM_PATH_MAX * 2];
    char ld_env[PM_PATH_MAX * 2];
    char python_path[PM_PATH_MAX * 2];
    if (!uses_system_python) {
        int path_rc = has_tools_bin
            ? pm_format(path_env, sizeof(path_env), "%s/bin:%s:%s",
                        ctx->runtime_dir, tools_bin, base_path)
            : pm_format(path_env, sizeof(path_env), "%s/bin:%s",
                        ctx->runtime_dir, base_path);
        int ld_rc = enable_compat_libs
            ? pm_format(ld_env, sizeof(ld_env), "%s/lib:%s:%s",
                        ctx->runtime_dir, compat_lib_dir, base_ld)
            : pm_format(ld_env, sizeof(ld_env), "%s/lib:%s",
                        ctx->runtime_dir, base_ld);
        if (path_rc != 0 ||
            ld_rc != 0 ||
            pm_format(python_path, sizeof(python_path),
                      "%s/lib/python3.10:%s/lib/python3.10/site-packages:%s/lib",
                      ctx->runtime_dir, ctx->runtime_dir, ctx->runtime_dir) != 0) {
            snprintf(err, err_size, "runtime environment path too long");
            return -1;
        }
    } else {
        if (has_tools_bin) {
            if (pm_format(path_env, sizeof(path_env), "%s:%s", tools_bin, base_path) != 0) {
                snprintf(err, err_size, "runtime environment path too long");
                return -1;
            }
        } else {
            pm_copy(path_env, sizeof(path_env), base_path);
        }
        if (enable_compat_libs) {
            if (pm_format(ld_env, sizeof(ld_env), "%s:%s", compat_lib_dir, base_ld) != 0) {
                snprintf(err, err_size, "runtime environment path too long");
                return -1;
            }
        } else {
            pm_copy(ld_env, sizeof(ld_env), base_ld);
        }
        pm_copy(python_path, sizeof(python_path), "");
    }

    char armhf_root[PM_PATH_MAX];
    bool has_armhf = pm_armhf_compat_available(ctx, armhf_root, sizeof(armhf_root));
    const char *controller_config = pm_controller_layout_gui_sdl_config();
    char cfw_version[128];
    pm_resolve_cfw_version(ctx, cfw_version, sizeof(cfw_version));
    const char *pm_can_mount = pm_file_exists("/tmp/leaf-pm-mount-probe-failed") ? "N" : "Y";
    char taskset_value[32];
    if (pm_taskset_available()) {
        pm_copy(taskset_value, sizeof(taskset_value), "taskset 0xF");
    } else {
        pm_copy(taskset_value, sizeof(taskset_value), "");
    }
    bool allow_upstream_self_update = pm_env_truthy("LEAF_PM_ALLOW_UPSTREAM_SELF_UPDATE");
    if (allow_upstream_self_update) {
        fprintf(stderr, "PortMaster warning: upstream GUI self-update is enabled by developer override\n");
    }

    pm_env_override env[] = {
        { "PLATFORM", ctx->platform },
        { "SDCARD_PATH", ctx->sdcard_path },
        { "USERDATA_PATH", ctx->userdata_path },
        { "LOGS_PATH", ctx->logs_path },
        { "ROMS_PATH", ctx->roms_path },
        { "IMAGES_PATH", ctx->images_path },
        { "HOME", ctx->data_dir },
        { "PATH", path_env },
        { "LD_LIBRARY_PATH", ld_env },
        { "PYTHONHOME", !uses_system_python ? ctx->runtime_dir : "" },
        { "PYTHONPATH", python_path },
        { "PYTHONDONTWRITEBYTECODE", "1" },
        { "XDG_DATA_HOME", ctx->data_dir },
        { "HM_TOOLS_DIR", ctx->data_dir },
        { "LEAF_PM_TOOLS_DIR", has_tools_bin ? tools_bin : "" },
        { "LEAF_PM_AARCH64_COMPAT_LIB_DIR", has_compat_libs ? compat_lib_dir : "" },
        { "LEAF_PM_NATIVE_COMPAT_LIB_DIR", has_compat_libs ? compat_lib_dir : "" },
        { "HM_PORTS_DIR", ctx->ports_dir },
        { "HM_SCRIPTS_DIR", ctx->ports_dir },
        { "HM_PORTS_READ_DIRS", read_ports },
        { "HM_SCRIPTS_READ_DIRS", read_scripts },
        { "HM_PORTS_READ_SOURCE_IDS", read_ids },
        { "LEAF_PM_SOURCE_ROOTS", read_roots },
        { "LEAF_PM_SOURCE_MOUNT_IDS", read_mount_ids },
        { "LEAF_PM_SOURCE_DEVICES", read_devices },
        { "LEAF_PM_SOURCE_ST_DEVS", read_st_devs },
        { "LEAF_PM_SOURCE_PORTS_ST_DEVS", read_ports_st_devs },
        { "LEAF_PM_SOURCE_FINGERPRINTS", read_fingerprints },
        { "LEAF_PM_SELECTED_SOURCE_ID", selected_source_id },
        { "LEAF_PM_LAST_KNOWN_INVENTORY", last_known_inventory },
        { "PORTMASTER_CONTROLFOLDER", ctx->portmaster_dir },
        { "PORTMASTER_LEAF_DEVICE_INFO", "1" },
        { "LEAF_PM_DISABLE_SELF_UPDATE", allow_upstream_self_update ? "0" : "1" },
        { "LEAF_PM_LAUNCH_MODE", "gui" },
        { "CFW_NAME", "Leaf" },
        { "CFW_VERSION", cfw_version },
        { "DEVICE_NAME", "Miniloong Pocket 1" },
        { "DEVICE_CPU", "RK3566" },
        { "DEVICE_ARCH", "aarch64" },
        { "DEVICE_RAM", "1" },
        { "DEVICE_HAS_ARMHF", has_armhf ? "Y" : "N" },
        { "DEVICE_HAS_AARCH64", "Y" },
        { "DEVICE_HAS_X86", "N" },
        { "DEVICE_HAS_X86_64", "N" },
        { "LEAF_PM_ARMHF_ROOT", has_armhf ? armhf_root : "" },
        { "DISPLAY_WIDTH", "960" },
        { "DISPLAY_HEIGHT", "720" },
        { "DISPLAY_ORIENTATION", "0" },
        { "ASPECT_X", "4" },
        { "ASPECT_Y", "3" },
        { "ANALOG_STICKS", "2" },
        { "ANALOGSTICKS", "2" },
        { "PM_CAN_MOUNT", pm_can_mount },
        { "TASKSET", taskset_value },
        { "PORTMASTER_LEAF_PORT_LAYOUT_SCOPE", "gui" },
        { "PORTMASTER_LEAF_CONTROLLER_LAYOUT", "gui" },
        { "SDL_GAMECONTROLLERCONFIG", controller_config },
        { "sdl_controllerconfig", controller_config },
        { NULL, NULL },
    };

    pm_env_snapshot_entry snapshot[] = {
        { "PLATFORM", ctx->platform, "leaf-session-env" },
        { "SDCARD_PATH", ctx->sdcard_path, "leaf-session-env" },
        { "USERDATA_PATH", ctx->userdata_path, "leaf-session-env" },
        { "LOGS_PATH", ctx->logs_path, "leaf-session-env" },
        { "ROMS_PATH", ctx->roms_path, "leaf-session-env" },
        { "IMAGES_PATH", ctx->images_path, "leaf-session-env" },
        { "HOME", ctx->data_dir, "manager" },
        { "XDG_DATA_HOME", ctx->data_dir, "manager" },
        { "HM_TOOLS_DIR", ctx->data_dir, "manager" },
        { "HM_PORTS_DIR", ctx->ports_dir, "manager" },
        { "HM_SCRIPTS_DIR", ctx->ports_dir, "manager" },
        { "HM_PORTS_READ_DIRS", read_ports, "manager" },
        { "HM_SCRIPTS_READ_DIRS", read_scripts, "manager" },
        { "HM_PORTS_READ_SOURCE_IDS", read_ids, "manager" },
        { "LEAF_PM_SOURCE_ROOTS", read_roots, "manager" },
        { "LEAF_PM_SOURCE_MOUNT_IDS", read_mount_ids, "manager" },
        { "LEAF_PM_SOURCE_DEVICES", read_devices, "manager" },
        { "LEAF_PM_SOURCE_ST_DEVS", read_st_devs, "manager" },
        { "LEAF_PM_SOURCE_PORTS_ST_DEVS", read_ports_st_devs, "manager" },
        { "LEAF_PM_SOURCE_FINGERPRINTS", read_fingerprints, "manager" },
        { "LEAF_PM_SELECTED_SOURCE_ID", selected_source_id, "manager" },
        { "LEAF_PM_LAST_KNOWN_INVENTORY", last_known_inventory, "manager" },
        { "PORTMASTER_CONTROLFOLDER", ctx->portmaster_dir, "manager" },
        { "PATH", path_env, "shim" },
        { "LD_LIBRARY_PATH", ld_env, "shim" },
        { "PYTHONHOME", !uses_system_python ? ctx->runtime_dir : "", "shim" },
        { "PYTHONPATH", python_path, "shim" },
        { "LEAF_PM_TOOLS_DIR", has_tools_bin ? tools_bin : "", "shim" },
        { "LEAF_PM_AARCH64_COMPAT_LIB_DIR", has_compat_libs ? compat_lib_dir : "", "shim" },
        { "LEAF_PM_NATIVE_COMPAT_LIB_DIR", has_compat_libs ? compat_lib_dir : "", "shim" },
        { "LEAF_PM_DISABLE_SELF_UPDATE", allow_upstream_self_update ? "0" : "1", "manager" },
        { "LEAF_PM_LAUNCH_MODE", "gui", "manager" },
        { "CFW_NAME", "Leaf", "manager" },
        { "CFW_VERSION", cfw_version, "manager" },
        { "DEVICE_NAME", "Miniloong Pocket 1", "manager" },
        { "DEVICE_CPU", "RK3566", "manager" },
        { "DEVICE_ARCH", "aarch64", "manager" },
        { "DEVICE_RAM", "1", "manager" },
        { "DEVICE_HAS_ARMHF", has_armhf ? "Y" : "N", "manager" },
        { "DEVICE_HAS_AARCH64", "Y", "manager" },
        { "LEAF_PM_ARMHF_ROOT", has_armhf ? armhf_root : "", "shim" },
        { "DISPLAY_WIDTH", "960", "manager" },
        { "DISPLAY_HEIGHT", "720", "manager" },
        { "DISPLAY_ORIENTATION", "0", "manager" },
        { "ASPECT_X", "4", "manager" },
        { "ASPECT_Y", "3", "manager" },
        { "ANALOG_STICKS", "2", "manager" },
        { "ANALOGSTICKS", "2", "manager" },
        { "PM_CAN_MOUNT", pm_can_mount, "manager" },
        { "TASKSET", taskset_value, "manager" },
        { "ESUDO", pm_env("ESUDO", ""), "upstream-control" },
        { "GPTOKEYB", pm_env("GPTOKEYB", ""), "upstream-control" },
        { "GPTOKEYB2", pm_env("GPTOKEYB2", ""), "upstream-control" },
        { "directory", pm_env("directory", ""), "upstream-control" },
        { "PORTMASTER_LEAF_PORT_LAYOUT_SCOPE", "gui", "manager" },
        { "PORTMASTER_LEAF_CONTROLLER_LAYOUT", "gui", "manager" },
        { "SDL_GAMECONTROLLERCONFIG", controller_config, "manager" },
        { "sdl_controllerconfig", controller_config, "manager" },
        { NULL, NULL, NULL },
    };
    char snapshot_err[256];
    if (pm_env_snapshot_write(ctx, "manager", snapshot,
                              snapshot_err, sizeof(snapshot_err)) != 0) {
        fprintf(stderr, "PortMaster env snapshot warning: %s\n", snapshot_err);
    }

    char *argv[] = { "bash", "./PortMaster.sh", NULL };
    pm_launch_status(status_fn, status_userdata, "PortMaster is running");
    int rc = pm_run_argv_env_in_dir(ctx->portmaster_dir, argv, env, err, err_size);

    pm_launch_status(status_fn, status_userdata, "Closing PortMaster...");
    char repair_err[512];
    if (pm_repatch_portmaster(ctx, repair_err, sizeof(repair_err)) != 0) {
        fprintf(stderr, "PortMaster post-exit repair warning: %s\n",
                repair_err[0] ? repair_err : "unknown error");
    }

    char refresh_sources_err[256];
    if (pm_context_refresh_sources(base_ctx, refresh_sources_err,
                                   sizeof(refresh_sources_err)) != 0) {
        fprintf(stderr, "PortMaster source refresh warning: %s\n",
                refresh_sources_err);
    }
    pm_source_tree_snapshot ports_after;
    bool have_ports_after =
        pm_capture_source_tree_snapshot(base_ctx, &ports_after);
    bool ports_changed_during_session =
        !have_ports_before || !have_ports_after ||
        !pm_source_snapshots_equal(&ports_before, &ports_after);
    if (ports_changed_during_session && have_ports_after) {
        unsigned int changed_source_mask =
            pm_changed_armhf_source_mask(base_ctx, &ports_before, &ports_after);
        size_t scanned_count = 0;
        bool scans_ok = pm_refresh_armhf_source_mask(
            base_ctx, changed_source_mask, selected_source_id, &scanned_count);
        if (scans_ok &&
            pm_capture_source_tree_snapshot(base_ctx, &ports_after)) {
            (void)pm_write_union_port_scan_stamp(base_ctx, &ports_after);
        }
    }
    (void)pm_controller_layout_sync_hook(ctx, NULL, 0);
    if (scanned_before_launch || ports_changed_during_session) {
        pm_artwork_sync_result art = {0};
        char art_err[512];
        if (pm_artwork_sync(base_ctx, &art, art_err, sizeof(art_err)) != 0) {
            fprintf(stderr, "PortMaster artwork sync warning: %s\n", art_err);
        } else if (art.failed > 0) {
            fprintf(stderr,
                    "PortMaster artwork sync completed with warnings: scanned=%d synced=%d "
                    "skipped=%d preserved_custom=%d missing=%d failed=%d\n",
                    art.scanned, art.synced, art.skipped_existing,
                    art.preserved_custom, art.missing_source, art.failed);
        }
        pm_request_jawaka_library_rescan(ctx);
    }
    return rc;
}

int pm_launch_portmaster_with_status(pm_context *ctx,
                                     pm_launch_status_fn status_fn,
                                     void *status_userdata,
                                     char *err,
                                     size_t err_size)
{
    return pm_launch_portmaster_from_source_with_status(
        ctx, NULL, status_fn, status_userdata, err, err_size);
}

int pm_launch_portmaster(pm_context *ctx, char *err, size_t err_size)
{
    return pm_launch_portmaster_with_status(ctx, NULL, NULL, err, err_size);
}
