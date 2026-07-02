#include "pm_installer.h"

#include "pm_controller_layout.h"
#include "pm_downloader.h"
#include "pm_sha256.h"
#include "pm_util.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PM_MAX_PATCHES 32
#define PM_PATCH_SET "leaf-mlp1-v1"

typedef struct {
    char name[128];
    char path[PM_PATH_MAX];
    char sha256[65];
} pm_patch_record;

static int pm_install_compat_assets(const pm_context *ctx, char *err, size_t err_size);

static void source_from_lock(const pm_context *ctx, pm_portmaster_source *out)
{
    memset(out, 0, sizeof(*out));
    if (!ctx) {
        return;
    }
    pm_copy(out->repo, sizeof(out->repo), ctx->lock.repo);
    pm_copy(out->channel, sizeof(out->channel), ctx->lock.channel);
    pm_copy(out->tag, sizeof(out->tag), ctx->lock.tag);
    pm_copy(out->published_at, sizeof(out->published_at), ctx->lock.published_at);
    pm_copy(out->asset, sizeof(out->asset), ctx->lock.asset);
    pm_copy(out->url, sizeof(out->url), ctx->lock.url);
    pm_copy(out->release_url, sizeof(out->release_url), ctx->lock.release_url);
    out->size = ctx->lock.size;
    pm_copy(out->md5, sizeof(out->md5), ctx->lock.md5);
    pm_copy(out->sha256, sizeof(out->sha256), ctx->lock.sha256);
    pm_copy(out->checked_at, sizeof(out->checked_at), ctx->lock.checked_at);
}

static int manifest_json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return -1;
    }
    return pm_copy(dst, dst_size, item->valuestring);
}

static int source_from_manifest(const pm_context *ctx, pm_portmaster_source *out)
{
    memset(out, 0, sizeof(*out));
    if (!ctx || !pm_file_exists(ctx->manifest_path)) {
        return -1;
    }

    char read_err[128];
    char *text = pm_read_text_file(ctx->manifest_path, 256 * 1024, read_err, sizeof(read_err));
    if (!text) {
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return -1;
    }

    cJSON *pm = cJSON_GetObjectItemCaseSensitive(root, "portmaster");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(pm, "size");
    int ok = -1;
    if (cJSON_IsObject(pm) &&
        manifest_json_string(pm, "repo", out->repo, sizeof(out->repo)) == 0 &&
        manifest_json_string(pm, "channel", out->channel, sizeof(out->channel)) == 0 &&
        manifest_json_string(pm, "tag", out->tag, sizeof(out->tag)) == 0 &&
        manifest_json_string(pm, "asset", out->asset, sizeof(out->asset)) == 0 &&
        manifest_json_string(pm, "md5", out->md5, sizeof(out->md5)) == 0 &&
        manifest_json_string(pm, "sha256", out->sha256, sizeof(out->sha256)) == 0 &&
        cJSON_IsNumber(size) && size->valuedouble >= 0.0) {
        out->size = (uint64_t)size->valuedouble;
        ok = 0;
    }
    cJSON_Delete(root);
    return ok;
}

static void source_from_manifest_or_lock(const pm_context *ctx, pm_portmaster_source *out)
{
    if (source_from_manifest(ctx, out) == 0 && out->tag[0]) {
        return;
    }
    source_from_lock(ctx, out);
}

static int verified_existing_zip(const pm_portmaster_source *source, const char *path)
{
    off_t size = pm_file_size(path);
    if (size <= 0) {
        return 0;
    }
    if (source->size > 0 && size != (off_t)source->size) {
        return 0;
    }
    if (source->sha256[0]) {
        char sha[65], err[128];
        if (pm_sha256_file_hex(path, sha, err, sizeof(err)) != 0) {
            return 0;
        }
        if (strcmp(sha, source->sha256) != 0) {
            return 0;
        }
    }
    if (source->md5[0]) {
        char md5[33], err[128];
        if (pm_md5_file_hex(path, md5, err, sizeof(err)) != 0) {
            return 0;
        }
        if (strcmp(md5, source->md5) != 0) {
            return 0;
        }
    }
    return source->sha256[0] || source->md5[0];
}

static int is_patch_name(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    return len > 6 && strcmp(name + len - 6, ".patch") == 0;
}

static int compare_patch_record(const void *a, const void *b)
{
    const pm_patch_record *pa = (const pm_patch_record *)a;
    const pm_patch_record *pb = (const pm_patch_record *)b;
    return strcmp(pa->name, pb->name);
}

static int load_patch_records(const pm_context *ctx, pm_patch_record *records,
                              size_t *count, char *err, size_t err_size)
{
    char patch_dir[PM_PATH_MAX];
    if (pm_join3(patch_dir, sizeof(patch_dir), ctx->pak_dir,
                 "patches/portmaster-gui", "mlp1") != 0) {
        snprintf(err, err_size, "patch directory path too long");
        return -1;
    }

    DIR *dir = opendir(patch_dir);
    if (!dir) {
        snprintf(err, err_size, "cannot open patch directory %s: %s",
                 patch_dir, strerror(errno));
        return -1;
    }

    size_t used = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_patch_name(ent->d_name)) {
            continue;
        }
        if (used >= PM_MAX_PATCHES) {
            closedir(dir);
            snprintf(err, err_size, "too many PortMaster patches");
            return -1;
        }
        if (pm_copy(records[used].name, sizeof(records[used].name), ent->d_name) != 0 ||
            pm_join(records[used].path, sizeof(records[used].path), patch_dir, ent->d_name) != 0) {
            closedir(dir);
            snprintf(err, err_size, "patch path too long");
            return -1;
        }
        char sha_err[128];
        if (pm_sha256_file_hex(records[used].path, records[used].sha256,
                               sha_err, sizeof(sha_err)) != 0) {
            closedir(dir);
            snprintf(err, err_size, "cannot hash patch %s: %s", ent->d_name, sha_err);
            return -1;
        }
        used++;
    }
    closedir(dir);

    if (used == 0) {
        snprintf(err, err_size, "no MLP1 PortMaster patches found in %s", patch_dir);
        return -1;
    }

    qsort(records, used, sizeof(records[0]), compare_patch_record);
    *count = used;
    return 0;
}

static int append_text(char *buf, size_t buf_size, size_t *used, const char *text)
{
    size_t len = strlen(text ? text : "");
    if (*used >= buf_size || len >= buf_size - *used) {
        return -1;
    }
    memcpy(buf + *used, text ? text : "", len);
    *used += len;
    buf[*used] = '\0';
    return 0;
}

static int patch_records_fingerprint(const pm_patch_record *records,
                                     size_t count,
                                     char out_hex[65],
                                     char *err,
                                     size_t err_size)
{
    char input[PM_MAX_PATCHES * 256 + 128];
    size_t used = 0;
    input[0] = '\0';
    if (append_text(input, sizeof(input), &used, PM_PATCH_SET) != 0 ||
        append_text(input, sizeof(input), &used, "\n") != 0) {
        snprintf(err, err_size, "patch fingerprint input too long");
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (append_text(input, sizeof(input), &used, records[i].name) != 0 ||
            append_text(input, sizeof(input), &used, "\t") != 0 ||
            append_text(input, sizeof(input), &used, records[i].sha256) != 0 ||
            append_text(input, sizeof(input), &used, "\n") != 0) {
            snprintf(err, err_size, "patch fingerprint input too long");
            return -1;
        }
    }
    return pm_sha256_buffer_hex(input, used, out_hex, err, err_size);
}

const char *pm_portmaster_patch_set_id(void)
{
    return PM_PATCH_SET;
}

int pm_portmaster_patch_set_fingerprint(const pm_context *ctx, char out_hex[65],
                                        char *err, size_t err_size)
{
    if (out_hex) {
        out_hex[0] = '\0';
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !out_hex) {
        snprintf(err, err_size, "missing patch fingerprint context");
        return -1;
    }
    pm_patch_record patches[PM_MAX_PATCHES];
    size_t patch_count = 0;
    if (load_patch_records(ctx, patches, &patch_count, err, err_size) != 0) {
        return -1;
    }
    return patch_records_fingerprint(patches, patch_count, out_hex, err, err_size);
}

static int prepare_pylibs_for_patches(const char *tree, char *err, size_t err_size)
{
    char hardware_path[PM_PATH_MAX];
    char pylibs_path[PM_PATH_MAX];
    char zip_path[PM_PATH_MAX];
    char md5_path[PM_PATH_MAX];
    if (pm_join3(hardware_path, sizeof(hardware_path), tree,
                 "pylibs/harbourmaster", "hardware.py") != 0 ||
        pm_join(pylibs_path, sizeof(pylibs_path), tree, "pylibs") != 0 ||
        pm_join(zip_path, sizeof(zip_path), tree, "pylibs.zip") != 0 ||
        pm_join(md5_path, sizeof(md5_path), tree, "pylibs.zip.md5") != 0) {
        snprintf(err, err_size, "pylibs path too long");
        return -1;
    }

    if (!pm_file_exists(zip_path)) {
        return 0;
    }

    if (pm_dir_exists(pylibs_path) && pm_rm_rf(pylibs_path, err, err_size) != 0) {
        char rm_err[512];
        pm_copy(rm_err, sizeof(rm_err), err ? err : "");
        snprintf(err, err_size, "replace pylibs from pylibs.zip failed: %s",
                 rm_err[0] ? rm_err : "unknown error");
        return -1;
    }

    char *argv[] = { "unzip", "-q", "-o", zip_path, "-d", (char *)tree, NULL };
    if (pm_run_argv(argv, err, err_size) != 0) {
        char run_err[512];
        pm_copy(run_err, sizeof(run_err), err ? err : "");
        snprintf(err, err_size, "extract pylibs.zip failed: %s",
                 run_err[0] ? run_err : "unknown error");
        return -1;
    }

    if (!pm_file_exists(hardware_path)) {
        snprintf(err, err_size, "pylibs.zip did not contain harbourmaster/hardware.py");
        return -1;
    }
    if (unlink(zip_path) != 0 && errno != ENOENT) {
        snprintf(err, err_size, "cannot remove extracted pylibs.zip: %s", strerror(errno));
        return -1;
    }
    if (unlink(md5_path) != 0 && errno != ENOENT) {
        snprintf(err, err_size, "cannot remove stale pylibs.zip.md5: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int apply_patch_records(const char *tree, const pm_patch_record *records,
                               size_t count, char *err, size_t err_size)
{
    if (prepare_pylibs_for_patches(tree, err, err_size) != 0) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(records[i].name, "0001-leaf-controlfolder-env.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join(target, sizeof(target), tree, "control.txt") != 0) {
                snprintf(err, err_size, "control.txt path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 256 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "PORTMASTER_CONTROLFOLDER")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0002-leaf-device-info-env.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join(target, sizeof(target), tree, "device_info.txt") != 0) {
                snprintf(err, err_size, "device_info.txt path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 256 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "PORTMASTER_LEAF_DEVICE_INFO")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0003-leaf-preserve-pysdl2-env.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join(target, sizeof(target), tree, "PortMaster.sh") != 0) {
                snprintf(err, err_size, "PortMaster.sh path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 256 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "PYSDL2_DLL_PATH:-")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0004-leaf-harbourmaster-device.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join3(target, sizeof(target), tree, "pylibs/harbourmaster", "hardware.py") != 0) {
                snprintf(err, err_size, "hardware.py path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 512 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "leaf-mlp1")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0005-leaf-armhf-hook.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join(target, sizeof(target), tree, "control.txt") != 0) {
                snprintf(err, err_size, "control.txt path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 256 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "leaf-armhf-env.sh")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0006-leaf-armhf-gui-capability.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join3(target, sizeof(target), tree, "pylibs/harbourmaster", "hardware.py") != 0) {
                snprintf(err, err_size, "hardware.py path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 512 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "LEAF_PM_ARMHF_ROOT")) {
                free(content);
                continue;
            }
            free(content);
        } else if (strcmp(records[i].name, "0007-leaf-disable-self-update.patch") == 0) {
            char target[PM_PATH_MAX];
            if (pm_join(target, sizeof(target), tree, "pugwash") != 0) {
                snprintf(err, err_size, "pugwash path too long");
                return -1;
            }
            char marker_err[128];
            char *content = pm_read_text_file(target, 1024 * 1024, marker_err, sizeof(marker_err));
            if (content && strstr(content, "LEAF_PM_DISABLE_SELF_UPDATE")) {
                free(content);
                continue;
            }
            free(content);
        }

        char patch_path[PM_PATH_MAX];
        if (pm_copy(patch_path, sizeof(patch_path), records[i].path) != 0) {
            snprintf(err, err_size, "patch path too long");
            return -1;
        }
        char *argv[] = { "patch", "-p0", "-i", patch_path, NULL };
        if (pm_run_argv_in_dir(tree, argv, err, err_size) != 0) {
            char run_err[512];
            pm_copy(run_err, sizeof(run_err), err ? err : "");
            snprintf(err, err_size, "patch %s failed: %s",
                     records[i].name, run_err[0] ? run_err : "unknown error");
            return -1;
        }
    }
    return 0;
}

static int file_contains_required(const char *path,
                                  const char *needle,
                                  const char *description,
                                  char *err,
                                  size_t err_size)
{
    char read_err[128];
    char *content = pm_read_text_file(path, 2 * 1024 * 1024, read_err, sizeof(read_err));
    if (!content) {
        snprintf(err, err_size, "candidate validation failed: cannot read %s (%s)",
                 description, read_err[0] ? read_err : path);
        return -1;
    }
    if (!strstr(content, needle)) {
        free(content);
        snprintf(err, err_size, "candidate validation failed: %s lacks %s",
                 description, needle);
        return -1;
    }
    free(content);
    return 0;
}

static int validate_patched_portmaster_tree(const char *tree, char *err, size_t err_size)
{
    char portmaster_sh[PM_PATH_MAX];
    char pugwash[PM_PATH_MAX];
    char control[PM_PATH_MAX];
    char device_info[PM_PATH_MAX];
    char hardware[PM_PATH_MAX];
    if (pm_join(portmaster_sh, sizeof(portmaster_sh), tree, "PortMaster.sh") != 0 ||
        pm_join(pugwash, sizeof(pugwash), tree, "pugwash") != 0 ||
        pm_join(control, sizeof(control), tree, "control.txt") != 0 ||
        pm_join(device_info, sizeof(device_info), tree, "device_info.txt") != 0 ||
        pm_join3(hardware, sizeof(hardware), tree, "pylibs/harbourmaster", "hardware.py") != 0) {
        snprintf(err, err_size, "candidate validation path too long");
        return -1;
    }

    if (!pm_file_exists(portmaster_sh)) {
        snprintf(err, err_size, "candidate validation failed: missing PortMaster.sh");
        return -1;
    }
    if (!pm_file_exists(pugwash)) {
        snprintf(err, err_size, "candidate validation failed: missing pugwash");
        return -1;
    }
    if (!pm_file_exists(control)) {
        snprintf(err, err_size, "candidate validation failed: missing control.txt");
        return -1;
    }
    if (!pm_file_exists(device_info)) {
        snprintf(err, err_size, "candidate validation failed: missing device_info.txt");
        return -1;
    }
    if (!pm_file_exists(hardware)) {
        snprintf(err, err_size, "candidate validation failed: missing harbourmaster hardware.py");
        return -1;
    }

    if (file_contains_required(portmaster_sh, "./pugwash $PORTMASTER_CMDS",
                               "PortMaster.sh", err, err_size) != 0 ||
        file_contains_required(pugwash, "LEAF_PM_DISABLE_SELF_UPDATE",
                               "pugwash", err, err_size) != 0 ||
        file_contains_required(control, "PORTMASTER_CONTROLFOLDER",
                               "control.txt", err, err_size) != 0 ||
        file_contains_required(control, "leaf-armhf-env.sh",
                               "control.txt", err, err_size) != 0 ||
        file_contains_required(device_info, "PORTMASTER_LEAF_DEVICE_INFO",
                               "device_info.txt", err, err_size) != 0 ||
        file_contains_required(hardware, "leaf-mlp1",
                               "harbourmaster hardware.py", err, err_size) != 0 ||
        file_contains_required(hardware, "LEAF_PM_ARMHF_ROOT",
                               "harbourmaster hardware.py", err, err_size) != 0) {
        return -1;
    }
    return 0;
}

static int write_manifest(const pm_context *ctx, const pm_portmaster_source *source,
                          const pm_patch_record *patches,
                          size_t patch_count, char *err, size_t err_size)
{
    char patch_fingerprint[65];
    if (patch_records_fingerprint(patches, patch_count, patch_fingerprint,
                                  err, err_size) != 0) {
        return -1;
    }

    FILE *fp = fopen(ctx->manifest_path, "wb");
    if (!fp) {
        snprintf(err, err_size, "cannot write manifest %s: %s",
                 ctx->manifest_path, strerror(errno));
        return -1;
    }

    int ok = fprintf(fp,
        "{\n"
        "  \"schema\": 1,\n"
        "  \"platform\": \"mlp1\",\n"
        "  \"manager_version\": \"%s\",\n"
        "  \"portmaster\": {\n"
        "    \"repo\": \"%s\",\n"
        "    \"channel\": \"%s\",\n"
        "    \"tag\": \"%s\",\n"
        "    \"asset\": \"%s\",\n"
        "    \"size\": %llu,\n"
        "    \"md5\": \"%s\",\n"
        "    \"sha256\": \"%s\"\n"
        "  },\n"
        "  \"patches\": {\n"
        "    \"set\": \"%s\",\n"
        "    \"fingerprint\": \"%s\",\n"
        "    \"files\": [\n",
        PM_VERSION,
        source->repo,
        source->channel,
        source->tag,
        source->asset,
        (unsigned long long)source->size,
        source->md5,
        source->sha256,
        PM_PATCH_SET,
        patch_fingerprint) > 0;

    for (size_t i = 0; ok && i < patch_count; i++) {
        ok = fprintf(fp,
            "      { \"path\": \"patches/portmaster-gui/mlp1/%s\", \"sha256\": \"%s\" }%s\n",
            patches[i].name,
            patches[i].sha256,
            i + 1 == patch_count ? "" : ",") > 0;
    }

    if (ok) {
        ok = fprintf(fp,
            "    ],\n"
            "    \"overlays\": []\n"
            "  },\n"
            "  \"runtimes\": {},\n"
            "  \"compat\": {\n"
            "    \"egl_gles_shim\": \"compat/egl/aarch64/libEGL.so.1\",\n"
            "    \"aarch64_sdl2_fullscreen_shim\": \"compat/sdl2/aarch64/leaf-sdl2-fullscreen.so\",\n"
            "    \"native_tools\": {\n"
            "      \"rsync\": \"compat/tools/aarch64/bin/rsync\",\n"
            "      \"zip\": \"compat/tools/aarch64/bin/zip\"\n"
            "    }\n"
            "  },\n"
            "  \"ports_scan\": {\n"
            "    \"schema\": 1,\n"
            "    \"report\": \".leaf/armhf-scan.json\",\n"
            "    \"refreshed_by\": \"scripts/scan-and-fix-port-elfs.sh\"\n"
            "  }\n"
            "}\n") > 0;
    }

    if (fclose(fp) != 0 || !ok) {
        snprintf(err, err_size, "cannot finish manifest %s", ctx->manifest_path);
        return -1;
    }
    return 0;
}

static bool path_exists_any(const char *path)
{
    struct stat st;
    return path && lstat(path, &st) == 0;
}

static int backup_existing_install(const pm_context *ctx,
                                   char *backup_out,
                                   size_t backup_size,
                                   char *err,
                                   size_t err_size)
{
    if (backup_out && backup_size > 0) {
        backup_out[0] = '\0';
    }
    if (!path_exists_any(ctx->portmaster_dir)) {
        if (errno != ENOENT) {
            snprintf(err, err_size, "cannot inspect existing PortMaster: %s",
                     strerror(errno));
            return -1;
        }
        return 0;
    }

    time_t now = time(NULL);
    struct tm tmv;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    localtime_r(&now, &tmv);
#else
    tmv = *localtime(&now);
#endif
    char base[64];
    strftime(base, sizeof(base), "PortMaster-%Y%m%d-%H%M%S", &tmv);

    for (int attempt = 0; attempt < 64; attempt++) {
        char name[128];
        if (attempt == 0) {
            if (pm_copy(name, sizeof(name), base) != 0) {
                snprintf(err, err_size, "backup name too long");
                return -1;
            }
        } else if (pm_format(name, sizeof(name), "%s-%ld-%02d",
                             base, (long)getpid(), attempt) != 0) {
            snprintf(err, err_size, "backup name too long");
            return -1;
        }

        char backup[PM_PATH_MAX];
        if (pm_join(backup, sizeof(backup), ctx->backups_dir, name) != 0) {
            snprintf(err, err_size, "backup path too long");
            return -1;
        }
        if (path_exists_any(backup)) {
            continue;
        }
        if (rename(ctx->portmaster_dir, backup) == 0) {
            if (backup_out && backup_size > 0 &&
                pm_copy(backup_out, backup_size, backup) != 0) {
                snprintf(err, err_size, "backup path too long");
                return -1;
            }
            return 0;
        }
        if (errno == EEXIST || errno == ENOTEMPTY) {
            continue;
        }
        snprintf(err, err_size, "cannot backup existing PortMaster: %s", strerror(errno));
        return -1;
    }

    snprintf(err, err_size, "cannot find unused PortMaster backup path");
    return -1;
}

static int restore_portmaster_backup(const pm_context *ctx,
                                     const char *backup_path,
                                     char *err,
                                     size_t err_size)
{
    if (!backup_path || !backup_path[0]) {
        if (pm_rm_rf(ctx->portmaster_dir, err, err_size) != 0) {
            return -1;
        }
        return 0;
    }

    if (!path_exists_any(backup_path)) {
        snprintf(err, err_size, "PortMaster backup is missing: %s", backup_path);
        return -1;
    }
    if (pm_rm_rf(ctx->portmaster_dir, err, err_size) != 0) {
        return -1;
    }
    if (rename(backup_path, ctx->portmaster_dir) != 0) {
        snprintf(err, err_size, "cannot restore PortMaster backup: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int fail_with_portmaster_restore(const pm_context *ctx,
                                        const char *backup_path,
                                        const char *failure,
                                        char *err,
                                        size_t err_size)
{
    char restore_err[256];
    if (restore_portmaster_backup(ctx, backup_path, restore_err, sizeof(restore_err)) == 0) {
        if (backup_path && backup_path[0]) {
            snprintf(err, err_size,
                     "PortMaster install failed and previous install was restored: %s",
                     failure);
        } else {
            snprintf(err, err_size,
                     "PortMaster install failed and incomplete install was removed: %s",
                     failure);
        }
        return -1;
    }

    snprintf(err, err_size, "PortMaster install failed: %s; rollback failed: %s",
             failure, restore_err);
    return -1;
}

int pm_repatch_portmaster(pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !ctx->lock_loaded) {
        snprintf(err, err_size, "stable PortMaster lock is not loaded");
        return -1;
    }
    if (!pm_dir_exists(ctx->portmaster_dir)) {
        snprintf(err, err_size, "PortMaster is not installed: %s", ctx->portmaster_dir);
        return -1;
    }

    pm_patch_record patches[PM_MAX_PATCHES];
    size_t patch_count = 0;
    pm_portmaster_source source;
    source_from_manifest_or_lock(ctx, &source);
    if (load_patch_records(ctx, patches, &patch_count, err, err_size) != 0 ||
        apply_patch_records(ctx->portmaster_dir, patches, patch_count, err, err_size) != 0 ||
        validate_patched_portmaster_tree(ctx->portmaster_dir, err, err_size) != 0 ||
        pm_install_compat_assets(ctx, err, err_size) != 0 ||
        write_manifest(ctx, &source, patches, patch_count, err, err_size) != 0 ||
        pm_controller_layout_sync_hook(ctx, err, err_size) != 0) {
        return -1;
    }
    return 0;
}

static void chmod_if_present(const char *path)
{
    if (!pm_file_exists(path)) {
        return;
    }
    char *argv[] = { "chmod", "755", (char *)path, NULL };
    (void)pm_run_argv(argv, NULL, 0);
}

static int copy_file_atomic(const char *src, const char *dst, char *err, size_t err_size)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(err, err_size, "cannot open %s: %s", src, strerror(errno));
        return -1;
    }

    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", dst, (long)getpid()) != 0) {
        fclose(in);
        snprintf(err, err_size, "compat destination path too long");
        return -1;
    }

    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        snprintf(err, err_size, "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }

    unsigned char buf[64 * 1024];
    int ok = 1;
    while (!feof(in)) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0 && fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        if (ferror(in)) {
            ok = 0;
            break;
        }
    }

    if (fclose(in) != 0) {
        ok = 0;
    }
    if (fclose(out) != 0) {
        ok = 0;
    }
    if (!ok) {
        unlink(tmp);
        snprintf(err, err_size, "cannot copy %s to %s", src, dst);
        return -1;
    }

    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot promote %s: %s", dst, strerror(errno));
        return -1;
    }
    chmod(dst, 0755);
    return 0;
}

static int copy_compat_asset_set(const pm_context *ctx,
                                 const char *src_mid,
                                 const char *src_leaf,
                                 const char *dst_mid,
                                 const char *dst_leaf,
                                 const char *const *files,
                                 size_t file_count,
                                 char *err,
                                 size_t err_size)
{
    char src_dir[PM_PATH_MAX];
    if (pm_join3(src_dir, sizeof(src_dir), ctx->pak_dir, src_mid, src_leaf) != 0) {
        snprintf(err, err_size, "compat source path too long");
        return -1;
    }
    if (!pm_dir_exists(src_dir)) {
        return 0;
    }

    char dst_dir[PM_PATH_MAX];
    if (pm_join3(dst_dir, sizeof(dst_dir), ctx->data_dir, dst_mid, dst_leaf) != 0) {
        snprintf(err, err_size, "compat destination path too long");
        return -1;
    }
    if (pm_mkdir_p(dst_dir, err, err_size) != 0) {
        return -1;
    }

    for (size_t i = 0; i < file_count; i++) {
        char src[PM_PATH_MAX];
        char dst[PM_PATH_MAX];
        if (pm_join(src, sizeof(src), src_dir, files[i]) != 0 ||
            pm_join(dst, sizeof(dst), dst_dir, files[i]) != 0) {
            snprintf(err, err_size, "compat file path too long");
            return -1;
        }
        if (pm_file_exists(src) && copy_file_atomic(src, dst, err, err_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int pm_install_compat_assets(const pm_context *ctx, char *err, size_t err_size)
{
    const char *egl_files[] = { "libEGL.so.1", "libEGL.so" };
    const char *mali_files[] = { "libmali.so.1", "libmali-hook.so.1" };
    const char *sdl2_files[] = { "leaf-sdl2-fullscreen.so", "manifest.json" };
    const char *tools_bin_files[] = { "rsync", "zip" };
    const char *tools_meta_files[] = { "manifest.json" };

    if (copy_compat_asset_set(ctx,
                              "compat/egl",
                              "aarch64",
                              "compat/egl",
                              "aarch64",
                              egl_files,
                              sizeof(egl_files) / sizeof(egl_files[0]),
                              err,
                              err_size) != 0) {
        return -1;
    }

    if (copy_compat_asset_set(ctx,
                              "compat/mali",
                              "aarch64",
                              "compat/mali",
                              "aarch64",
                              mali_files,
                              sizeof(mali_files) / sizeof(mali_files[0]),
                              err,
                              err_size) != 0) {
        return -1;
    }

    if (copy_compat_asset_set(ctx,
                              "compat/sdl2",
                              "aarch64",
                              "compat/sdl2",
                              "aarch64",
                              sdl2_files,
                              sizeof(sdl2_files) / sizeof(sdl2_files[0]),
                              err,
                              err_size) != 0) {
        return -1;
    }

    if (copy_compat_asset_set(ctx,
                              "compat/tools",
                              "aarch64/bin",
                              "compat/tools",
                              "aarch64/bin",
                              tools_bin_files,
                              sizeof(tools_bin_files) / sizeof(tools_bin_files[0]),
                              err,
                              err_size) != 0) {
        return -1;
    }

    if (copy_compat_asset_set(ctx,
                              "compat/tools",
                              "aarch64",
                              "compat/tools",
                              "aarch64",
                              tools_meta_files,
                              sizeof(tools_meta_files) / sizeof(tools_meta_files[0]),
                              err,
                              err_size) != 0) {
        return -1;
    }

    return 0;
}

int pm_install_runtime_archive(pm_context *ctx, const char *archive_path, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !archive_path || !archive_path[0]) {
        snprintf(err, err_size, "missing runtime archive path");
        return -1;
    }
    if (!pm_file_exists(archive_path)) {
        snprintf(err, err_size, "runtime archive not found: %s", archive_path);
        return -1;
    }

    char seven_zip[PM_PATH_MAX];
    if (pm_join(seven_zip, sizeof(seven_zip), ctx->portmaster_dir, "7zzs.aarch64") != 0 ||
        !pm_file_exists(seven_zip)) {
        snprintf(err, err_size, "PortMaster 7zzs.aarch64 is required; install PortMaster first");
        return -1;
    }

    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        return -1;
    }

    char extract_dir[PM_PATH_MAX];
    if (pm_join(extract_dir, sizeof(extract_dir), ctx->staging_dir, "runtime-extract") != 0) {
        snprintf(err, err_size, "runtime staging path too long");
        return -1;
    }
    if (pm_rm_rf(extract_dir, err, err_size) != 0 ||
        pm_mkdir_p(extract_dir, err, err_size) != 0) {
        return -1;
    }

    char out_arg[PM_PATH_MAX + 3];
    if (pm_format(out_arg, sizeof(out_arg), "-o%s", extract_dir) != 0) {
        snprintf(err, err_size, "runtime output path too long");
        return -1;
    }
    char *argv[] = { seven_zip, "x", (char *)archive_path, out_arg, "-y", NULL };
    if (pm_run_argv(argv, err, err_size) != 0) {
        return -1;
    }

    char extracted_runtime[PM_PATH_MAX];
    char python_path[PM_PATH_MAX];
    if (pm_join(extracted_runtime, sizeof(extracted_runtime), extract_dir, "portmaster") != 0 ||
        pm_join3(python_path, sizeof(python_path), extracted_runtime, "bin", "python3") != 0 ||
        !pm_file_exists(python_path)) {
        snprintf(err, err_size, "runtime archive did not contain portmaster/bin/python3");
        return -1;
    }

    if (pm_rm_rf(ctx->runtime_dir, err, err_size) != 0) {
        return -1;
    }
    if (rename(extracted_runtime, ctx->runtime_dir) != 0) {
        snprintf(err, err_size, "cannot promote runtime install: %s", strerror(errno));
        return -1;
    }

    char py[PM_PATH_MAX];
    if (pm_join3(py, sizeof(py), ctx->runtime_dir, "bin", "python") == 0) {
        chmod_if_present(py);
    }
    if (pm_join3(py, sizeof(py), ctx->runtime_dir, "bin", "python3") == 0) {
        chmod_if_present(py);
    }
    if (pm_join3(py, sizeof(py), ctx->runtime_dir, "bin", "python3.10") == 0) {
        chmod_if_present(py);
    }

    (void)pm_rm_rf(extract_dir, NULL, 0);
    return 0;
}

int pm_install_ui_runtime(pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !ctx->runtime_lock_loaded) {
        snprintf(err, err_size, "UI runtime lock is not loaded");
        return -1;
    }

    char seven_zip[PM_PATH_MAX];
    if (pm_join(seven_zip, sizeof(seven_zip), ctx->portmaster_dir, "7zzs.aarch64") != 0 ||
        !pm_file_exists(seven_zip)) {
        snprintf(err, err_size, "Install PortMaster first; 7zzs.aarch64 is required to extract the runtime");
        return -1;
    }

    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        return -1;
    }

    char archive_path[PM_PATH_MAX];
    if (pm_join(archive_path, sizeof(archive_path), ctx->downloads_dir,
                ctx->runtime_lock.filename) != 0) {
        snprintf(err, err_size, "runtime download path too long");
        return -1;
    }

    pm_download_spec spec = {
        .url = ctx->runtime_lock.url,
        .dest_path = archive_path,
        .expected_size = ctx->runtime_lock.size,
        .expected_sha256 = ctx->runtime_lock.sha256,
        .allow_http = false,
    };
    if (pm_download_file(&spec, err, err_size) != 0) {
        return -1;
    }

    if (pm_install_runtime_archive(ctx, archive_path, err, err_size) != 0) {
        return -1;
    }
    unlink(archive_path);
    return 0;
}

int pm_install_portmaster(pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !ctx->lock_loaded) {
        snprintf(err, err_size, "stable PortMaster lock is not loaded");
        return -1;
    }
    pm_portmaster_source source;
    source_from_lock(ctx, &source);
    return pm_install_portmaster_source(ctx, &source, err, err_size);
}

int pm_install_portmaster_source(pm_context *ctx, const pm_portmaster_source *source,
                                 char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !source || !source->tag[0] || !source->url[0] ||
        (!source->sha256[0] && !source->md5[0])) {
        snprintf(err, err_size, "PortMaster source is incomplete");
        return -1;
    }

    pm_patch_record patches[PM_MAX_PATCHES];
    size_t patch_count = 0;
    if (load_patch_records(ctx, patches, &patch_count, err, err_size) != 0) {
        return -1;
    }

    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0 ||
        pm_mkdir_p(ctx->ports_dir, err, err_size) != 0 ||
        pm_mkdir_p(ctx->port_images_dir, err, err_size) != 0) {
        return -1;
    }

    char zip_path[PM_PATH_MAX];
    if (pm_format(zip_path, sizeof(zip_path), "%s/PortMaster-%s.zip",
                  ctx->downloads_dir, source->tag) != 0) {
        snprintf(err, err_size, "download path too long");
        return -1;
    }

    if (!verified_existing_zip(source, zip_path)) {
        pm_download_spec spec = {
            .url = source->url,
            .dest_path = zip_path,
            .expected_size = source->size,
            .expected_sha256 = source->sha256,
            .expected_md5 = source->md5,
            .allow_http = false,
        };
        if (pm_download_file(&spec, err, err_size) != 0) {
            return -1;
        }
    }

    pm_portmaster_source manifest_source = *source;
    if (manifest_source.size == 0) {
        off_t size = pm_file_size(zip_path);
        if (size > 0) {
            manifest_source.size = (uint64_t)size;
        }
    }
    if (!manifest_source.sha256[0]) {
        char sha_err[128];
        if (pm_sha256_file_hex(zip_path, manifest_source.sha256,
                               sha_err, sizeof(sha_err)) != 0) {
            snprintf(err, err_size, "cannot hash PortMaster.zip: %s", sha_err);
            return -1;
        }
    }
    if (!manifest_source.asset[0]) {
        pm_copy(manifest_source.asset, sizeof(manifest_source.asset), "PortMaster.zip");
    }
    if (!manifest_source.repo[0]) {
        pm_copy(manifest_source.repo, sizeof(manifest_source.repo), "PortsMaster/PortMaster-GUI");
    }
    if (!manifest_source.channel[0]) {
        pm_copy(manifest_source.channel, sizeof(manifest_source.channel), "stable");
    }

    char extract_dir[PM_PATH_MAX];
    if (pm_join(extract_dir, sizeof(extract_dir), ctx->staging_dir, "portmaster-extract") != 0) {
        snprintf(err, err_size, "staging path too long");
        return -1;
    }
    if (pm_rm_rf(extract_dir, err, err_size) != 0 ||
        pm_mkdir_p(extract_dir, err, err_size) != 0) {
        return -1;
    }

    char *argv[] = { "unzip", "-q", zip_path, "-d", extract_dir, NULL };
    if (pm_run_argv(argv, err, err_size) != 0) {
        return -1;
    }

    char extracted_pm[PM_PATH_MAX];
    if (pm_join(extracted_pm, sizeof(extracted_pm), extract_dir, "PortMaster") != 0 ||
        !pm_dir_exists(extracted_pm)) {
        snprintf(err, err_size, "verified zip did not contain PortMaster/");
        return -1;
    }

    if (apply_patch_records(extracted_pm, patches, patch_count, err, err_size) != 0 ||
        validate_patched_portmaster_tree(extracted_pm, err, err_size) != 0) {
        return -1;
    }

    char backup_path[PM_PATH_MAX];
    if (backup_existing_install(ctx, backup_path, sizeof(backup_path), err, err_size) != 0) {
        return -1;
    }

    if (rename(extracted_pm, ctx->portmaster_dir) != 0) {
        char failure[256];
        snprintf(failure, sizeof(failure), "cannot promote PortMaster install: %s",
                 strerror(errno));
        return fail_with_portmaster_restore(ctx, backup_path, failure, err, err_size);
    }

    if (pm_install_compat_assets(ctx, err, err_size) != 0 ||
        write_manifest(ctx, &manifest_source, patches, patch_count, err, err_size) != 0 ||
        pm_controller_layout_sync_hook(ctx, err, err_size) != 0) {
        char failure[512];
        pm_copy(failure, sizeof(failure), err);
        return fail_with_portmaster_restore(ctx, backup_path, failure, err, err_size);
    }

    (void)pm_rm_rf(extract_dir, NULL, 0);
    return 0;
}
