#include "pm_launcher.h"

#include "pm_installer.h"
#include "pm_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PM_MLP1_GAMECONTROLLERCONFIG =
    "1900fe3c039900001399000002010000,Loong Gamepad,"
    "a:b1,b:b0,x:b2,y:b3,back:b8,guide:b10,start:b9,"
    "leftstick:b11,leftshoulder:b4,rightshoulder:b5,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,lefttrigger:b6,righttrigger:b7,"
    "crc:3cfe,platform:Linux";

static void pm_refresh_armhf_port_wrappers(pm_context *ctx)
{
    char script[PM_PATH_MAX];
    if (!ctx ||
        pm_join3(script, sizeof(script), ctx->pak_dir,
                 "scripts", "scan-and-fix-port-elfs.sh") != 0 ||
        !pm_file_exists(script)) {
        return;
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
    pm_refresh_armhf_port_wrappers(ctx);

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
        { "DEVICE_HAS_ARMHF", "N" },
        { "DEVICE_HAS_AARCH64", "Y" },
        { "DEVICE_HAS_X86", "N" },
        { "DEVICE_HAS_X86_64", "N" },
        { "DISPLAY_WIDTH", "960" },
        { "DISPLAY_HEIGHT", "720" },
        { "DISPLAY_ORIENTATION", "0" },
        { "ASPECT_X", "4" },
        { "ASPECT_Y", "3" },
        { "ANALOG_STICKS", "2" },
        { "ANALOGSTICKS", "2" },
        { "SDL_GAMECONTROLLERCONFIG", PM_MLP1_GAMECONTROLLERCONFIG },
        { NULL, NULL },
    };

    char *argv[] = { "bash", "./PortMaster.sh", NULL };
    return pm_run_argv_env_in_dir(ctx->portmaster_dir, argv, env, err, err_size);
}
