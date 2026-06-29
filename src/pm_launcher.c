#include "pm_launcher.h"

#include "pm_installer.h"
#include "pm_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char ld_env[PM_PATH_MAX * 4];
    char python_path[PM_PATH_MAX * 2];
    char pydll_path[PM_PATH_MAX];
    if (has_runtime) {
        char site_pydll_path[PM_PATH_MAX];
        char legacy_pydll_path[PM_PATH_MAX];
        if (pm_format(site_pydll_path, sizeof(site_pydll_path),
                      "%s/lib/python3.10/site-packages/sdl2dll/dll", ctx->runtime_dir) != 0 ||
            pm_format(legacy_pydll_path, sizeof(legacy_pydll_path),
                      "%s/lib/sdl2dll/dll", ctx->runtime_dir) != 0) {
            snprintf(err, err_size, "runtime SDL DLL path too long");
            return -1;
        }
        const char *primary_pydll_path = pm_dir_exists(site_pydll_path) ? site_pydll_path : legacy_pydll_path;
        if (pm_format(path_env, sizeof(path_env), "%s/bin:%s", ctx->runtime_dir, base_path) != 0 ||
            pm_format(ld_env, sizeof(ld_env), "%s/lib:%s:%s:%s",
                      ctx->runtime_dir, site_pydll_path, legacy_pydll_path, base_ld) != 0 ||
            pm_format(python_path, sizeof(python_path),
                      "%s/lib/python3.10:%s/lib/python3.10/site-packages:%s/lib",
                      ctx->runtime_dir, ctx->runtime_dir, ctx->runtime_dir) != 0 ||
            pm_copy(pydll_path, sizeof(pydll_path), primary_pydll_path) != 0) {
            snprintf(err, err_size, "runtime environment path too long");
            return -1;
        }
    } else {
        pm_copy(path_env, sizeof(path_env), base_path);
        pm_copy(ld_env, sizeof(ld_env), base_ld);
        pm_copy(python_path, sizeof(python_path), "");
        pm_copy(pydll_path, sizeof(pydll_path), "/usr/lib");
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
        { "PYSDL2_DLL_PATH", pydll_path },
        { NULL, NULL },
    };

    char *argv[] = { "bash", "./PortMaster.sh", NULL };
    return pm_run_argv_env_in_dir(ctx->portmaster_dir, argv, env, err, err_size);
}
