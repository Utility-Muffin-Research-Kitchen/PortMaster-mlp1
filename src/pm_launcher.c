#include "pm_launcher.h"

#include "pm_artwork.h"
#include "pm_controller_layout.h"
#include "pm_installer.h"
#include "pm_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool pm_armhf_compat_available(pm_context *ctx, char *root, size_t root_size)
{
    if (!ctx ||
        pm_join3(root, root_size, ctx->data_dir, "compat", "armhf") != 0) {
        return false;
    }

    char loader[PM_PATH_MAX];
    char runner[PM_PATH_MAX];
    return pm_join3(loader, sizeof(loader), root, "lib", "ld-linux-armhf.so.3") == 0 &&
           pm_join3(runner, sizeof(runner), root, "bin", "leaf-armhf-run") == 0 &&
           pm_file_exists(loader) &&
           pm_file_exists(runner);
}

static bool pm_env_truthy(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
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

static bool pm_port_scan_stamp_path(pm_context *ctx, char *out, size_t out_size)
{
    return ctx && pm_join(out, out_size, ctx->leaf_dir, "port-tree.stamp") == 0;
}

static bool pm_read_port_scan_stamp(pm_context *ctx, uint64_t *out)
{
    char path[PM_PATH_MAX];
    if (!out || !pm_port_scan_stamp_path(ctx, path, sizeof(path))) {
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

static void pm_write_port_scan_stamp(pm_context *ctx, uint64_t stamp)
{
    char path[PM_PATH_MAX];
    char tmp[PM_PATH_MAX];
    if (!pm_port_scan_stamp_path(ctx, path, sizeof(path)) ||
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

static bool pm_armhf_scan_report_exists(pm_context *ctx)
{
    char path[PM_PATH_MAX];
    return ctx &&
           pm_join(path, sizeof(path), ctx->leaf_dir, "armhf-scan.json") == 0 &&
           pm_file_exists(path);
}

static bool pm_refresh_armhf_port_wrappers(pm_context *ctx)
{
    char script[PM_PATH_MAX];
    if (!ctx ||
        pm_join3(script, sizeof(script), ctx->pak_dir,
                 "scripts", "scan-and-fix-port-elfs.sh") != 0 ||
        !pm_file_exists(script)) {
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
        pm_write_port_scan_stamp(ctx, stamp);
    }
    return true;
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
        fprintf(stderr, "Jawaka library rescan warning: %s\n", scan_err);
    }
}

int pm_launch_portmaster(pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        snprintf(err, err_size, "missing PortMaster context");
        return -1;
    }

    char script[PM_PATH_MAX];
    if (pm_join(script, sizeof(script), ctx->portmaster_dir, "PortMaster.sh") != 0) {
        snprintf(err, err_size, "PortMaster script path too long");
        return -1;
    }
    if (!pm_file_exists(script)) {
        snprintf(err, err_size, "PortMaster is not installed: %s", script);
        return -1;
    }

    if (pm_mkdir_p(ctx->ports_dir, err, err_size) != 0 ||
        pm_mkdir_p(ctx->port_images_dir, err, err_size) != 0) {
        return -1;
    }
    if (pm_repatch_portmaster(ctx, err, err_size) != 0) {
        return -1;
    }

    uint64_t ports_stamp_before = 0;
    uint64_t stored_ports_stamp = 0;
    bool have_ports_stamp_before = pm_ports_tree_stamp(ctx, &ports_stamp_before);
    bool have_stored_ports_stamp = pm_read_port_scan_stamp(ctx, &stored_ports_stamp);
    bool scanned_before_launch = false;
    if (pm_env_truthy("LEAF_PM_SCAN_BEFORE_LAUNCH") ||
        !pm_armhf_scan_report_exists(ctx) ||
        !have_ports_stamp_before ||
        !have_stored_ports_stamp ||
        ports_stamp_before != stored_ports_stamp) {
        scanned_before_launch = pm_refresh_armhf_port_wrappers(ctx);
        have_ports_stamp_before = pm_ports_tree_stamp(ctx, &ports_stamp_before);
    }

    if (pm_controller_layout_sync_hook(ctx, err, err_size) != 0) {
        return -1;
    }

    char runtime_python[PM_PATH_MAX];
    bool has_runtime = pm_join3(runtime_python, sizeof(runtime_python), ctx->runtime_dir,
                                "bin", "python3") == 0 && pm_file_exists(runtime_python);
    if (!has_runtime && !pm_file_exists("/usr/bin/python3") && !pm_file_exists("/bin/python3")) {
        snprintf(err, err_size, "Python runtime missing; install a managed runtime archive first");
        return -1;
    }

    const char *base_path = pm_env("PATH", "/usr/bin:/usr/sbin:/bin");
    const char *base_ld = pm_env("LD_LIBRARY_PATH", "");
    char path_env[PM_PATH_MAX * 2];
    char ld_env[PM_PATH_MAX * 2];
    char python_path[PM_PATH_MAX * 2];
    if (has_runtime) {
        if (pm_format(path_env, sizeof(path_env), "%s/bin:%s", ctx->runtime_dir, base_path) != 0 ||
            pm_format(ld_env, sizeof(ld_env), "%s/lib:%s", ctx->runtime_dir, base_ld) != 0 ||
            pm_format(python_path, sizeof(python_path),
                      "%s/lib/python3.10:%s/lib/python3.10/site-packages:%s/lib",
                      ctx->runtime_dir, ctx->runtime_dir, ctx->runtime_dir) != 0) {
            snprintf(err, err_size, "runtime environment path too long");
            return -1;
        }
    } else {
        pm_copy(path_env, sizeof(path_env), base_path);
        pm_copy(ld_env, sizeof(ld_env), base_ld);
        pm_copy(python_path, sizeof(python_path), "");
    }

    char armhf_root[PM_PATH_MAX];
    bool has_armhf = pm_armhf_compat_available(ctx, armhf_root, sizeof(armhf_root));
    const char *controller_config = pm_controller_layout_gui_sdl_config();

    pm_env_override env[] = {
        { "HOME", ctx->data_dir },
        { "PATH", path_env },
        { "LD_LIBRARY_PATH", ld_env },
        { "PYTHONHOME", has_runtime ? ctx->runtime_dir : "" },
        { "PYTHONPATH", python_path },
        { "PYTHONDONTWRITEBYTECODE", "1" },
        { "XDG_DATA_HOME", ctx->data_dir },
        { "HM_TOOLS_DIR", ctx->data_dir },
        { "HM_PORTS_DIR", ctx->ports_dir },
        { "HM_SCRIPTS_DIR", ctx->ports_dir },
        { "PORTMASTER_CONTROLFOLDER", ctx->portmaster_dir },
        { "PORTMASTER_LEAF_DEVICE_INFO", "1" },
        { "CFW_NAME", "Leaf" },
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
        { "PORTMASTER_LEAF_PORT_LAYOUT_SCOPE", "gui" },
        { "PORTMASTER_LEAF_CONTROLLER_LAYOUT", "gui" },
        { "SDL_GAMECONTROLLERCONFIG", controller_config },
        { "sdl_controllerconfig", controller_config },
        { NULL, NULL },
    };

    char *argv[] = { "bash", "./PortMaster.sh", NULL };
    int rc = pm_run_argv_env_in_dir(ctx->portmaster_dir, argv, env, err, err_size);

    uint64_t ports_stamp_after = 0;
    bool have_ports_stamp_after = pm_ports_tree_stamp(ctx, &ports_stamp_after);
    bool ports_changed_during_session = !have_ports_stamp_before ||
                                        !have_ports_stamp_after ||
                                        ports_stamp_before != ports_stamp_after;
    if (ports_changed_during_session && pm_refresh_armhf_port_wrappers(ctx)) {
        have_ports_stamp_after = pm_ports_tree_stamp(ctx, &ports_stamp_after);
        if (have_ports_stamp_after) {
            pm_write_port_scan_stamp(ctx, ports_stamp_after);
        }
    }
    (void)pm_controller_layout_sync_hook(ctx, NULL, 0);
    if (scanned_before_launch || ports_changed_during_session) {
        pm_artwork_sync_result art = {0};
        char art_err[512];
        if (pm_artwork_sync(ctx, &art, art_err, sizeof(art_err)) != 0) {
            fprintf(stderr, "PortMaster artwork sync warning: %s\n", art_err);
        } else if (art.failed > 0) {
            fprintf(stderr,
                    "PortMaster artwork sync completed with warnings: scanned=%d synced=%d skipped=%d missing=%d failed=%d\n",
                    art.scanned, art.synced, art.skipped_existing, art.missing_source, art.failed);
        }
        pm_request_jawaka_library_rescan(ctx);
    }
    return rc;
}
