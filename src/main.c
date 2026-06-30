#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "pm_context.h"
#include "pm_controller_layout.h"
#include "pm_doctor.h"
#include "pm_downloader.h"
#include "pm_installer.h"
#include "pm_launcher.h"
#include "pm_ui.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    pm_context ctx;
    char ctx_err[256];
    if (pm_context_init(&ctx, argv && argv[0] ? argv[0] : NULL, ctx_err, sizeof(ctx_err)) != 0) {
        fprintf(stderr, "PortMaster context error: %s\n", ctx_err);
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--doctor-text") == 0) {
        pm_doctor_report report;
        pm_doctor_run(&ctx, &report);
        fputs(report.text, stdout);
        return report.issues == 0 ? 0 : 1;
    }

    if (argc > 1 && strcmp(argv[1], "--lock-summary") == 0) {
        if (!ctx.lock_loaded) {
            fprintf(stderr, "lock not loaded: %s\n", ctx.lock_path);
            return 1;
        }
        char summary[1024];
        pm_lock_summary(&ctx.lock, summary, sizeof(summary));
        puts(summary);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--controller-layout") == 0) {
        pm_controller_layout layout = PM_CONTROLLER_LAYOUT_X360;
        if (pm_controller_layout_load(&ctx, &layout) != 0) {
            fprintf(stderr, "controller layout load failed\n");
            return 1;
        }
        puts(pm_controller_layout_slug(layout));
        return 0;
    }

    if (argc > 2 && strcmp(argv[1], "--set-controller-layout") == 0) {
        pm_controller_layout layout = PM_CONTROLLER_LAYOUT_X360;
        char err[512];
        if (pm_controller_layout_from_string(argv[2], &layout) != 0) {
            fprintf(stderr, "unknown controller layout: %s\n", argv[2]);
            return 1;
        }
        if (pm_controller_layout_save(&ctx, layout, err, sizeof(err)) != 0) {
            fprintf(stderr, "controller layout save failed: %s\n", err);
            return 1;
        }
        printf("controller layout set: %s\n", pm_controller_layout_slug(layout));
        return 0;
    }

    if (argc > 2 && strcmp(argv[1], "--download-portmaster") == 0) {
        if (!ctx.lock_loaded) {
            fprintf(stderr, "lock not loaded: %s\n", ctx.lock_path);
            return 1;
        }
        pm_download_spec spec = {
            .url = ctx.lock.url,
            .dest_path = argv[2],
            .expected_size = ctx.lock.size,
            .expected_sha256 = ctx.lock.sha256,
            .allow_http = false,
        };
        char err[512];
        if (pm_download_file(&spec, err, sizeof(err)) != 0) {
            fprintf(stderr, "download failed: %s\n", err);
            return 1;
        }
        printf("downloaded and verified: %s\n", argv[2]);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--install-portmaster") == 0) {
        char err[512];
        if (pm_install_portmaster(&ctx, err, sizeof(err)) != 0) {
            fprintf(stderr, "install failed: %s\n", err);
            return 1;
        }
        puts("PortMaster installed and manifest written");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--repatch-portmaster") == 0) {
        char err[512];
        if (pm_repatch_portmaster(&ctx, err, sizeof(err)) != 0) {
            fprintf(stderr, "repatch failed: %s\n", err);
            return 1;
        }
        puts("PortMaster patches applied and manifest written");
        return 0;
    }

    if (argc > 2 && strcmp(argv[1], "--install-runtime-archive") == 0) {
        char err[512];
        if (pm_install_runtime_archive(&ctx, argv[2], err, sizeof(err)) != 0) {
            fprintf(stderr, "runtime install failed: %s\n", err);
            return 1;
        }
        puts("PortMaster runtime installed");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--install-ui-runtime") == 0) {
        char err[512];
        if (pm_install_ui_runtime(&ctx, err, sizeof(err)) != 0) {
            fprintf(stderr, "runtime install failed: %s\n", err);
            return 1;
        }
        puts("PortMaster UI runtime installed");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--launch-portmaster") == 0) {
        char err[512];
        if (pm_launch_portmaster(&ctx, err, sizeof(err)) != 0) {
            fprintf(stderr, "launch failed: %s\n", err);
            return 1;
        }
        return 0;
    }

    cat_config cfg = {0};
    cfg.window_title = "PortMaster";
    cfg.font_path = NULL;
    cfg.log_path = cat_resolve_log_path("portmaster-mlp1");
    cfg.cpu_speed = CAT_CPU_SPEED_MENU;

    if (cat_init(&cfg) != CAT_OK) {
        fprintf(stderr, "Failed to initialise Catastrophe: %s\n", cat_get_error());
        return 1;
    }

    cat_log("portmaster-mlp1 startup: version=%s platform=%s data=%s",
            PM_VERSION, ctx.platform, ctx.data_dir);
    if (ctx_err[0]) {
        cat_log("%s", ctx_err);
    }

    pm_ui_run(&ctx);
    cat_quit();
    return 0;
}
