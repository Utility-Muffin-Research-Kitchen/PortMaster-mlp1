#include "pm_ui.h"

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include "pm_doctor.h"
#include "pm_installer.h"
#include "pm_launcher.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    PM_ACTION_STATUS = 0,
    PM_ACTION_INSTALL,
    PM_ACTION_LAUNCH,
    PM_ACTION_REPATCH,
    PM_ACTION_RUNTIMES,
    PM_ACTION_ARMHF,
    PM_ACTION_LOGS,
    PM_ACTION_PATHS,
    PM_ACTION_QUIT,
} pm_action;

static void show_message(const char *message)
{
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "OK", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    cat_confirm_result result = {0};
    (void)cat_confirmation(&opts, &result);
}

static void show_status(pm_context *ctx)
{
    pm_doctor_report report;
    pm_doctor_run(ctx, &report);
    show_message(report.text);
}

typedef struct {
    pm_context *ctx;
    char err[512];
} pm_install_job;

static int install_worker(void *userdata)
{
    pm_install_job *job = (pm_install_job *)userdata;
    job->err[0] = '\0';
    return pm_install_portmaster(job->ctx, job->err, sizeof(job->err));
}

static void show_install(pm_context *ctx)
{
    if (!ctx->lock_loaded) {
        char summary[8192];
        snprintf(summary, sizeof(summary), "Stable PortMaster lock is missing.\n\n%s", ctx->lock_path);
        show_message(summary);
        return;
    }

    pm_install_job job = { .ctx = ctx };
    cat_process_opts opts = {
        .message = "Installing PortMaster\n\nDownloading, verifying, extracting, and writing Leaf manifest.",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
    };
    int rc = cat_process_message(&opts, install_worker, &job);
    if (rc == 0) {
        show_message("PortMaster installed and patched.\n\nLeaf manifest has been written.");
    } else {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster install failed.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

typedef struct {
    pm_context *ctx;
    char err[512];
} pm_repatch_job;

static int repatch_worker(void *userdata)
{
    pm_repatch_job *job = (pm_repatch_job *)userdata;
    job->err[0] = '\0';
    return pm_repatch_portmaster(job->ctx, job->err, sizeof(job->err));
}

static void show_repatch(pm_context *ctx)
{
    pm_repatch_job job = { .ctx = ctx };
    cat_process_opts opts = {
        .message = "Repairing PortMaster patches",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
    };
    int rc = cat_process_message(&opts, repatch_worker, &job);
    if (rc == 0) {
        show_message("PortMaster patches are applied.\n\nLeaf manifest has been refreshed.");
    } else {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster repair failed.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

typedef struct {
    pm_context *ctx;
    char err[512];
} pm_launch_job;

static int launch_worker(void *userdata)
{
    pm_launch_job *job = (pm_launch_job *)userdata;
    job->err[0] = '\0';
    return pm_launch_portmaster(job->ctx, job->err, sizeof(job->err));
}

static void show_launch(pm_context *ctx)
{
    char script[PM_PATH_MAX];
    pm_join(script, sizeof(script), ctx->portmaster_dir, "PortMaster.sh");
    if (!pm_file_exists(script)) {
        char msg[8192];
        snprintf(msg, sizeof(msg),
                 "PortMaster is not installed yet.\n\nExpected:\n%s\n\nUse Install PortMaster once Phase 1 wiring lands.",
                 script);
        show_message(msg);
        return;
    }

    pm_launch_job job = { .ctx = ctx };
    cat_process_opts opts = {
        .message = "Launching PortMaster",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
    };
    int rc = cat_process_message(&opts, launch_worker, &job);
    if (rc != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster exited with an error.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

static void show_paths(pm_context *ctx)
{
    char msg[65536];
    snprintf(msg, sizeof(msg),
             "Pak:\n%s\n\nData:\n%s\n\nRuntime:\n%s\n\nManifest:\n%s\n\nDownloads:\n%s\n\nPorts:\n%s\n\nImages:\n%s\n\nLogs:\n%s",
             ctx->pak_dir,
             ctx->data_dir,
             ctx->runtime_dir,
             ctx->manifest_path,
             ctx->downloads_dir,
             ctx->ports_dir,
             ctx->port_images_dir,
             ctx->logs_path);
    show_message(msg);
}

static void show_logs(pm_context *ctx)
{
    char msg[8192];
    snprintf(msg, sizeof(msg),
             "Logs are written under:\n%s\n\nCurrent app stderr log:\nportmaster-mlp1.log",
             ctx->logs_path);
    show_message(msg);
}

static pm_action menu(pm_context *ctx)
{
    cat_list_item items[] = {
        { .label = "Doctor / Status", .trailing_text = ctx->lock_loaded ? "locked" : "lock missing" },
        { .label = "Install PortMaster", .trailing_text = "Phase 1" },
        { .label = "Launch PortMaster", .trailing_text = pm_dir_exists(ctx->portmaster_dir) ? "ready" : "not installed" },
        { .label = "Repair / Repatch", .trailing_text = pm_dir_exists(ctx->portmaster_dir) ? "ready" : "not installed" },
        { .label = "Popular Runtimes", .trailing_text = "planned" },
        { .label = "armhf Compatibility", .trailing_text = "planned" },
        { .label = "View Logs" },
        { .label = "View Paths" },
    };
    static const pm_action map[] = {
        PM_ACTION_STATUS,
        PM_ACTION_INSTALL,
        PM_ACTION_LAUNCH,
        PM_ACTION_REPATCH,
        PM_ACTION_RUNTIMES,
        PM_ACTION_ARMHF,
        PM_ACTION_LOGS,
        PM_ACTION_PATHS,
    };

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Quit" },
        { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts("PortMaster", items, (int)(sizeof(items) / sizeof(items[0])));
    opts.footer = footer;
    opts.footer_count = 2;

    cat_list_result result = {0};
    if (cat_list(&opts, &result) != CAT_OK) {
        return PM_ACTION_QUIT;
    }
    if (result.selected_index < 0 || result.selected_index >= (int)(sizeof(map) / sizeof(map[0]))) {
        return PM_ACTION_QUIT;
    }
    return map[result.selected_index];
}

void pm_ui_run(pm_context *ctx)
{
    bool running = true;
    while (running) {
        switch (menu(ctx)) {
            case PM_ACTION_STATUS:
                show_status(ctx);
                break;
            case PM_ACTION_INSTALL:
                show_install(ctx);
                break;
            case PM_ACTION_LAUNCH:
                show_launch(ctx);
                break;
            case PM_ACTION_REPATCH:
                show_repatch(ctx);
                break;
            case PM_ACTION_RUNTIMES:
                show_message("Popular runtime install/verify/repair is planned for Phase 1.");
                break;
            case PM_ACTION_ARMHF:
                show_message("armhf compatibility pack build/install/verify is planned for Phase 1.\n\nTier 0 and Tier 1 must be smoked before support is claimed.");
                break;
            case PM_ACTION_LOGS:
                show_logs(ctx);
                break;
            case PM_ACTION_PATHS:
                show_paths(ctx);
                break;
            case PM_ACTION_QUIT:
            default:
                running = false;
                break;
        }
    }
}
