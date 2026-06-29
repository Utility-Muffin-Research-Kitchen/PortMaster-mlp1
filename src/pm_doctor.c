#include "pm_doctor.h"

#include <stdio.h>
#include <string.h>

static void append_line(pm_doctor_report *r, const char *status, const char *label, const char *detail)
{
    size_t len = strlen(r->text);
    snprintf(r->text + len, sizeof(r->text) - len, "%s %s\n%s\n\n",
             status, label, detail ? detail : "");
}

void pm_doctor_run(const pm_context *ctx, pm_doctor_report *report)
{
    memset(report, 0, sizeof(*report));
    append_line(report, "OK", "Platform", ctx->platform);

    if (ctx->lock_loaded) {
        char summary[1024];
        pm_lock_summary(&ctx->lock, summary, sizeof(summary));
        append_line(report, "OK", "Stable PortMaster lock", summary);
    } else {
        report->issues++;
        append_line(report, "ERR", "Stable PortMaster lock", ctx->lock_path);
    }

    append_line(report, pm_dir_exists(ctx->data_dir) ? "OK" : "WARN",
                "Manager data", ctx->data_dir);
    append_line(report, pm_dir_exists(ctx->portmaster_dir) ? "OK" : "WARN",
                "Upstream PortMaster install", ctx->portmaster_dir);

    char runtime_python[PM_PATH_MAX];
    bool has_runtime = pm_join3(runtime_python, sizeof(runtime_python), ctx->runtime_dir,
                                "bin", "python3") == 0 && pm_file_exists(runtime_python);
    bool has_system_python = pm_file_exists("/usr/bin/python3") || pm_file_exists("/bin/python3");
    append_line(report, (has_runtime || has_system_python) ? "OK" : "WARN",
                "PortMaster Python runtime",
                has_runtime ? runtime_python :
                (has_system_python ? "system python3" : "install managed runtime archive before launch"));

    append_line(report, pm_dir_exists(ctx->ports_dir) ? "OK" : "WARN",
                "Ports folder", ctx->ports_dir);
    append_line(report, pm_dir_exists(ctx->port_images_dir) ? "OK" : "WARN",
                "Port artwork folder", ctx->port_images_dir);

    if (!pm_dir_exists(ctx->portmaster_dir)) {
        append_line(report, "INFO", "Next step",
                    "Phase 1 will wire Install PortMaster to the verified downloader.");
    }
}
