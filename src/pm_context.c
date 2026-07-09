#include "pm_context.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int dir_from_argv0(char *out, size_t out_size, const char *argv0)
{
    const char *env_dir = getenv("PORTMASTER_MLP1_PAK_DIR");
    if (env_dir && env_dir[0]) {
        return pm_copy(out, out_size, env_dir);
    }

    char cwd[PM_PATH_MAX];
    if (!argv0 || !argv0[0]) {
        return getcwd(out, out_size) ? 0 : -1;
    }

    char candidate[PM_PATH_MAX];
    if (argv0[0] != '/') {
        if (!getcwd(cwd, sizeof(cwd))) {
            return -1;
        }
        char joined[PM_PATH_MAX];
        if (pm_join(joined, sizeof(joined), cwd, argv0) != 0) {
            return -1;
        }
        argv0 = joined;
        char tmp[PM_PATH_MAX];
        if (pm_copy(tmp, sizeof(tmp), argv0) != 0) {
            return -1;
        }
        if (pm_copy(candidate, sizeof(candidate), dirname(tmp)) != 0) {
            return -1;
        }
    } else {
        char tmp[PM_PATH_MAX];
        if (pm_copy(tmp, sizeof(tmp), argv0) != 0) {
            return -1;
        }
        if (pm_copy(candidate, sizeof(candidate), dirname(tmp)) != 0) {
            return -1;
        }
    }

    const char *last = strrchr(candidate, '/');
    last = last ? last + 1 : candidate;
    if (strcmp(last, "bin") == 0) {
        char tmp[PM_PATH_MAX];
        char parent[PM_PATH_MAX];
        char lock_path[PM_PATH_MAX];
        if (pm_copy(tmp, sizeof(tmp), candidate) == 0 &&
            pm_copy(parent, sizeof(parent), dirname(tmp)) == 0 &&
            pm_join3(lock_path, sizeof(lock_path), parent, "locks", "portmaster-gui-stable.lock.json") == 0 &&
            pm_file_exists(lock_path)) {
            return pm_copy(out, out_size, parent);
        }
    }

    return pm_copy(out, out_size, candidate);
}

static bool path_ends_with(const char *path, const char *suffix)
{
    if (!path || !suffix) {
        return false;
    }
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int parent_dir(char *out, size_t out_size, const char *path)
{
    char tmp[PM_PATH_MAX];
    if (pm_copy(tmp, sizeof(tmp), path) != 0) {
        return -1;
    }
    return pm_copy(out, out_size, dirname(tmp));
}

static bool basename_equals(const char *path, const char *name)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, name) == 0;
}

static bool has_leaf_sd_marker(const char *root, const char *platform)
{
    char marker[PM_PATH_MAX];
    char launcher[PM_PATH_MAX];
    if (!root || !platform) {
        return false;
    }
    return (pm_format(marker, sizeof(marker), "%s/.system/leaf/platforms/%s/enabled",
                      root, platform) == 0 &&
            pm_file_exists(marker)) ||
           (pm_format(launcher, sizeof(launcher),
                      "%s/.system/leaf/platforms/%s/launcher/bin/loong_pangu",
                      root, platform) == 0 &&
            pm_file_exists(launcher));
}

static int sdcard_from_pak_dir(char *out, size_t out_size, const char *pak_dir)
{
    if (!pak_dir || !path_ends_with(pak_dir, ".pak")) {
        return -1;
    }

    char platform_dir[PM_PATH_MAX];
    char apps_dir[PM_PATH_MAX];
    char root[PM_PATH_MAX];
    if (parent_dir(platform_dir, sizeof(platform_dir), pak_dir) != 0 ||
        parent_dir(apps_dir, sizeof(apps_dir), platform_dir) != 0 ||
        parent_dir(root, sizeof(root), apps_dir) != 0 ||
        !basename_equals(apps_dir, "Apps") ||
        !pm_dir_exists(root)) {
        return -1;
    }
    return pm_copy(out, out_size, root);
}

static int resolve_sdcard_path(pm_context *ctx, char *err, size_t err_size)
{
    const char *explicit_sd = getenv("SDCARD_PATH");
    if (explicit_sd && explicit_sd[0]) {
        if (!pm_dir_exists(explicit_sd)) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "SDCARD_PATH is not a directory: %s", explicit_sd);
            }
            return -1;
        }
        return pm_copy(ctx->sdcard_path, sizeof(ctx->sdcard_path), explicit_sd);
    }

    const char *jawaka_sd = getenv("JAWAKA_SDCARD_ROOT");
    if (jawaka_sd && jawaka_sd[0]) {
        if (!pm_dir_exists(jawaka_sd)) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "JAWAKA_SDCARD_ROOT is not a directory: %s", jawaka_sd);
            }
            return -1;
        }
        return pm_copy(ctx->sdcard_path, sizeof(ctx->sdcard_path), jawaka_sd);
    }

    if (sdcard_from_pak_dir(ctx->sdcard_path, sizeof(ctx->sdcard_path),
                            ctx->pak_dir) == 0) {
        return 0;
    }

    const char *candidates[] = { "/mnt/sdcard", "/media/sdcard1", NULL };
    const char *match = NULL;
    int match_count = 0;
    for (size_t i = 0; candidates[i]; i++) {
        if (!pm_dir_exists(candidates[i]) ||
            !has_leaf_sd_marker(candidates[i], ctx->platform)) {
            continue;
        }
        match = candidates[i];
        match_count++;
    }

    if (match_count == 1) {
        return pm_copy(ctx->sdcard_path, sizeof(ctx->sdcard_path), match);
    }
    if (err && err_size > 0) {
        snprintf(err, err_size, "%s",
                 match_count > 1
                     ? "ambiguous Leaf SD root; set SDCARD_PATH explicitly"
                     : "cannot resolve Leaf SD root; set SDCARD_PATH");
    }
    return -1;
}

int pm_context_init(pm_context *ctx, const char *argv0, char *err, size_t err_size)
{
    if (!ctx) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (err && err_size > 0) {
        err[0] = '\0';
    }

    if (dir_from_argv0(ctx->pak_dir, sizeof(ctx->pak_dir), argv0) != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "cannot resolve pak directory");
        }
        return -1;
    }

    pm_copy(ctx->platform, sizeof(ctx->platform), pm_env("PLATFORM", "mlp1"));
    if (resolve_sdcard_path(ctx, err, err_size) != 0) {
        return -1;
    }

    const char *userdata = getenv("USERDATA_PATH");
    if (userdata && userdata[0]) {
        pm_copy(ctx->userdata_path, sizeof(ctx->userdata_path), userdata);
    } else {
        pm_format(ctx->userdata_path, sizeof(ctx->userdata_path), "%s/.userdata/%s",
                  ctx->sdcard_path, ctx->platform);
    }

    const char *logs = getenv("LOGS_PATH");
    if (logs && logs[0]) {
        pm_copy(ctx->logs_path, sizeof(ctx->logs_path), logs);
    } else {
        pm_join(ctx->logs_path, sizeof(ctx->logs_path), ctx->userdata_path, "logs");
    }

    const char *roms = getenv("ROMS_PATH");
    if (roms && roms[0]) {
        pm_copy(ctx->roms_path, sizeof(ctx->roms_path), roms);
    } else {
        pm_join(ctx->roms_path, sizeof(ctx->roms_path), ctx->sdcard_path, "Roms");
    }

    const char *images = getenv("IMAGES_PATH");
    if (images && images[0]) {
        pm_copy(ctx->images_path, sizeof(ctx->images_path), images);
    } else {
        pm_join(ctx->images_path, sizeof(ctx->images_path), ctx->sdcard_path, "Images");
    }

    const char *data = getenv("PORTMASTER_MLP1_DATA_DIR");
    if (data && data[0]) {
        pm_copy(ctx->data_dir, sizeof(ctx->data_dir), data);
    } else {
        pm_join(ctx->data_dir, sizeof(ctx->data_dir), ctx->userdata_path, "portmaster");
    }

    pm_join(ctx->leaf_dir, sizeof(ctx->leaf_dir), ctx->data_dir, ".leaf");
    pm_join(ctx->downloads_dir, sizeof(ctx->downloads_dir), ctx->leaf_dir, "downloads");
    pm_join(ctx->staging_dir, sizeof(ctx->staging_dir), ctx->leaf_dir, "staging");
    pm_join(ctx->backups_dir, sizeof(ctx->backups_dir), ctx->leaf_dir, "backups");
    pm_join(ctx->runtime_dir, sizeof(ctx->runtime_dir), ctx->data_dir, "runtime");
    pm_join(ctx->portmaster_dir, sizeof(ctx->portmaster_dir), ctx->data_dir, "PortMaster");
    pm_join(ctx->ports_dir, sizeof(ctx->ports_dir), ctx->roms_path, "PORTS");
    pm_join(ctx->port_images_dir, sizeof(ctx->port_images_dir), ctx->images_path, "PORTS");
    pm_join3(ctx->lock_path, sizeof(ctx->lock_path), ctx->pak_dir, "locks", "portmaster-gui-stable.lock.json");
    pm_join3(ctx->runtime_lock_path, sizeof(ctx->runtime_lock_path), ctx->pak_dir, "locks", "ui-runtime.lock.json");
    pm_join3(ctx->armhf_lock_path, sizeof(ctx->armhf_lock_path), ctx->pak_dir,
             "locks", "armhf-compat.lock.json");
    pm_join(ctx->manifest_path, sizeof(ctx->manifest_path), ctx->leaf_dir, "manifest.json");

    char lock_err[256];
    ctx->lock_loaded = pm_lock_load(ctx->lock_path, &ctx->lock, lock_err, sizeof(lock_err)) == 0;
    if (!ctx->lock_loaded && err && err_size > 0) {
        snprintf(err, err_size, "lock warning: %s", lock_err);
    }
    char runtime_lock_err[256];
    ctx->runtime_lock_loaded = pm_ui_runtime_lock_load(ctx->runtime_lock_path,
                                                       &ctx->runtime_lock,
                                                       runtime_lock_err,
                                                       sizeof(runtime_lock_err)) == 0;
    if (ctx->lock_loaded && !ctx->runtime_lock_loaded && err && err_size > 0) {
        snprintf(err, err_size, "runtime lock warning: %s", runtime_lock_err);
    }
    char armhf_lock_err[256];
    ctx->armhf_lock_loaded = pm_armhf_compat_lock_load(ctx->armhf_lock_path,
                                                       &ctx->armhf_lock,
                                                       armhf_lock_err,
                                                       sizeof(armhf_lock_err)) == 0;
    if (ctx->lock_loaded && !ctx->armhf_lock_loaded && err && err_size > 0) {
        snprintf(err, err_size, "armhf lock warning: %s", armhf_lock_err);
    }
    return 0;
}

int pm_context_ensure_manager_dirs(const pm_context *ctx, char *err, size_t err_size)
{
    const char *dirs[] = {
        ctx->data_dir,
        ctx->leaf_dir,
        ctx->downloads_dir,
        ctx->staging_dir,
        ctx->backups_dir,
        ctx->logs_path,
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (pm_mkdir_p(dirs[i], err, err_size) != 0) {
            return -1;
        }
    }
    return 0;
}
