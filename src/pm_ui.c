#include "pm_ui.h"

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include "pm_artwork.h"
#include "pm_controller_layout.h"
#include "pm_doctor.h"
#include "pm_installer.h"
#include "pm_launcher.h"
#include "pm_update.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PM_ACTION_NONE = 0,
    PM_ACTION_LAUNCH,
    PM_ACTION_INSTALL,
    PM_ACTION_UPDATE,
    PM_ACTION_CONTROLLER_LAYOUT,
    PM_ACTION_REPAIR,
    PM_ACTION_TROUBLESHOOTING,
    PM_ACTION_QUIT,
} pm_action;

typedef enum {
    PM_TROUBLE_ACTION_NONE = 0,
    PM_TROUBLE_ACTION_QUICK_DIAGNOSTICS,
    PM_TROUBLE_ACTION_HEALTH,
    PM_TROUBLE_ACTION_CHECK_UPDATE,
    PM_TROUBLE_ACTION_LOGS,
    PM_TROUBLE_ACTION_PATHS,
    PM_TROUBLE_ACTION_BACK,
} pm_trouble_action;

typedef enum {
    PM_UI_SETUP_NOT_INSTALLED = 0,
    PM_UI_SETUP_INCOMPLETE,
    PM_UI_SETUP_READY,
} pm_ui_setup_state;

typedef struct {
    bool installed;
    bool has_runtime;
    bool uses_system_python;
    bool lock_loaded;
    bool runtime_lock_loaded;
    bool manifest_exists;
    bool launchable;
    pm_ui_setup_state setup_state;
    pm_portmaster_update_status update;
    bool update_status_valid;
    char runtime_path[PM_PATH_MAX];
    char launch_disabled_reason[256];
    char setup_summary[128];
    char update_summary[128];
} pm_ui_state;

typedef struct {
    bool startup_update_check_attempted;
    bool startup_update_check_timed_out;
    bool update_status_valid;
    pm_portmaster_update_status update;
    char update_error[256];
} pm_ui_session_state;

typedef struct {
    pm_action action;
    bool disabled;
    const char *disabled_message;
} pm_menu_row;

static const char *PM_UNOFFICIAL_NOTICE_MARKER = "unofficial-support-notice.seen";
static const char *PM_UNOFFICIAL_NOTICE_TEXT =
    "This PortMaster manager is an unofficial Leaf/UMRK wrapper for MLP1.\n\n"
    "Please do not use the official PortMaster Discord for support with this wrapper, "
    "Leaf-specific setup, MLP1 runtime issues, or compatibility patches.\n\n"
    "For help, use Leaf/UMRK support channels or this package's issue tracker. "
    "The official PortMaster project is not responsible for this implementation.";

static const char *setup_state_slug(pm_ui_setup_state setup_state)
{
    switch (setup_state) {
        case PM_UI_SETUP_NOT_INSTALLED:
            return "not_installed";
        case PM_UI_SETUP_INCOMPLETE:
            return "incomplete";
        case PM_UI_SETUP_READY:
            return "ready";
        default:
            return "unknown";
    }
}

static const char *action_slug(pm_action action)
{
    switch (action) {
        case PM_ACTION_LAUNCH:
            return "launch";
        case PM_ACTION_INSTALL:
            return "install";
        case PM_ACTION_UPDATE:
            return "update";
        case PM_ACTION_CONTROLLER_LAYOUT:
            return "controller_layout";
        case PM_ACTION_REPAIR:
            return "repair";
        case PM_ACTION_TROUBLESHOOTING:
            return "troubleshooting";
        case PM_ACTION_QUIT:
            return "quit";
        case PM_ACTION_NONE:
        default:
            return "none";
    }
}

static void append_text_escaped(char *out, size_t out_size, size_t *used, const char *text)
{
    if (!out || out_size == 0 || !used || *used >= out_size) {
        return;
    }
    for (const char *p = text ? text : ""; *p && *used + 1 < out_size; p++) {
        if (*p == '\n') {
            if (*used + 2 >= out_size) {
                break;
            }
            out[(*used)++] = '\\';
            out[(*used)++] = 'n';
        } else if (*p == '\t') {
            if (*used + 2 >= out_size) {
                break;
            }
            out[(*used)++] = '\\';
            out[(*used)++] = 't';
        } else {
            out[(*used)++] = *p;
        }
    }
    out[*used] = '\0';
}

static int appendf(char *out, size_t out_size, size_t *used, const char *fmt, ...)
{
    if (!out || out_size == 0 || !used || !fmt || *used >= out_size) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(out + *used, out_size - *used, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= out_size - *used) {
        *used = out_size - 1;
        out[*used] = '\0';
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

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

typedef struct {
    char *text;
    TTF_Font *font;
    cat_draw_color color;
    int line_h;
} pm_text_detail;

static int text_append_char(char **out, size_t *cap, size_t *used, char ch)
{
    if (!out || !cap || !used) {
        return -1;
    }
    if (*used + 2 > *cap) {
        size_t next_cap = *cap > 0 ? *cap * 2 : 256;
        while (*used + 2 > next_cap) {
            next_cap *= 2;
        }
        char *next = realloc(*out, next_cap);
        if (!next) {
            return -1;
        }
        *out = next;
        *cap = next_cap;
    }
    (*out)[(*used)++] = ch;
    (*out)[*used] = '\0';
    return 0;
}

static char *hard_wrap_text(const char *text, TTF_Font *font, int max_w)
{
    if (!text) {
        text = "";
    }
    if (!font || max_w <= 0) {
        char *copy = malloc(strlen(text) + 1);
        if (copy) {
            strcpy(copy, text);
        }
        return copy;
    }

    size_t len = strlen(text);
    size_t cap = len * 2 + 16;
    if (cap < 256) {
        cap = 256;
    }
    char *out = calloc(1, cap);
    if (!out) {
        return NULL;
    }

    size_t used = 0;
    size_t line_start = 0;
    size_t last_space = (size_t)-1;
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (text_append_char(&out, &cap, &used, ch) != 0) {
            free(out);
            return NULL;
        }

        if (ch == '\n') {
            line_start = used;
            last_space = (size_t)-1;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            last_space = used - 1;
        }

        if (cat_measure_text(font, out + line_start) <= max_w) {
            continue;
        }

        if (last_space != (size_t)-1 && last_space >= line_start) {
            out[last_space] = '\n';
            line_start = last_space + 1;
            last_space = (size_t)-1;
        } else if (used - line_start > 1) {
            char overflow = out[used - 1];
            out[used - 1] = '\n';
            line_start = used;
            last_space = (size_t)-1;
            if (text_append_char(&out, &cap, &used, overflow) != 0) {
                free(out);
                return NULL;
            }
        }
    }

    return out;
}

static int wrapped_line_count(const char *text)
{
    if (!text || !text[0]) {
        return 1;
    }
    int lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    return lines;
}

static void draw_text_detail_content(int x, int y, int w, void *user)
{
    pm_text_detail *detail = user;
    if (!detail || !detail->text || !detail->font) {
        return;
    }

    int yy = y;
    char *line = detail->text;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (line[0]) {
            cat_draw_text(detail->font, line, x, yy, detail->color);
        }
        if (end) {
            *end = '\n';
            line = end + 1;
        } else {
            line = NULL;
        }
        yy += detail->line_h;
    }
}

static void show_text_detail(const char *title, const char *message)
{
    TTF_Font *font = cat_get_font(CAT_FONT_SMALL);
    if (!font) {
        show_message(message ? message : "");
        return;
    }

    SDL_Rect content = cat_get_content_rect(true, true, false);
    int margin = cat_get_screen_width() / 40;
    if (margin < 12) {
        margin = 12;
    } else if (margin > 32) {
        margin = 32;
    }
    int top_pad = 8;
    int bottom_pad = 8;
    int view_x = content.x + margin;
    int view_y = content.y + top_pad;
    int view_w = content.w - margin * 2;
    int view_h = content.h - top_pad - bottom_pad;
    if (view_w < 1 || view_h < 1) {
        show_message(message ? message : "");
        return;
    }

    int wrap_w = view_w - 12;
    if (wrap_w < 1) {
        wrap_w = view_w;
    }

    char *wrapped = hard_wrap_text(message ? message : "", font, wrap_w);
    if (!wrapped) {
        show_message("Could not render diagnostics.");
        return;
    }

    pm_text_detail detail = {
        .text = wrapped,
        .font = font,
        .color = cat_get_theme()->text,
        .line_h = TTF_FontLineSkip(font) + 2,
    };
    int content_h = wrapped_line_count(wrapped) * detail.line_h;
    cat_scroll_state scroll;
    cat_scroll_state_init(&scroll);
    cat_footer_item footer[] = {
        { .button = CAT_BTN_UP, .label = "Scroll" },
        { .button = CAT_BTN_DOWN, .label = "Scroll" },
        { .button = CAT_BTN_B, .label = "Back" },
        { .button = CAT_BTN_A, .label = "OK", .is_confirm = true },
    };

    bool running = true;
    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) {
                continue;
            }
            switch (ev.button) {
                case CAT_BTN_UP:
                    cat_scroll_state_move(&scroll, -detail.line_h * 3);
                    break;
                case CAT_BTN_DOWN:
                    cat_scroll_state_move(&scroll, detail.line_h * 3);
                    break;
                case CAT_BTN_A:
                case CAT_BTN_B:
                    running = false;
                    break;
                default:
                    break;
            }
        }

        cat_draw_background();
        cat_draw_screen_title(title ? title : "", NULL);
        cat_draw_scroll_view(view_x, view_y, view_w, view_h, content_h,
                             &scroll, draw_text_detail_content, &detail);
        cat_draw_footer(footer, (int)(sizeof(footer) / sizeof(footer[0])));
        cat_present();
    }

    free(wrapped);
}

static int write_notice_marker(const char *path, char *err, size_t err_size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        pm_format(err, err_size, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }

    if (fputs("seen\n", fp) == EOF) {
        int saved_errno = errno;
        fclose(fp);
        pm_format(err, err_size, "cannot write %s: %s", path, strerror(saved_errno));
        return -1;
    }

    if (fclose(fp) != 0) {
        pm_format(err, err_size, "cannot finish %s: %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

static void show_unofficial_support_notice_if_needed(const pm_context *ctx)
{
    char marker_path[PM_PATH_MAX];
    char err[256];

    if (!ctx) {
        return;
    }

    bool dirs_ready = pm_context_ensure_manager_dirs(ctx, err, sizeof(err)) == 0;
    if (!dirs_ready) {
        cat_log("PortMaster notice: could not prepare manager dirs: %s", err);
    }

    if (pm_join(marker_path, sizeof(marker_path), ctx->leaf_dir,
                PM_UNOFFICIAL_NOTICE_MARKER) != 0) {
        cat_log("PortMaster notice: marker path too long");
        show_text_detail("Unofficial PortMaster", PM_UNOFFICIAL_NOTICE_TEXT);
        return;
    }

    if (dirs_ready && pm_file_exists(marker_path)) {
        return;
    }

    show_text_detail("Unofficial PortMaster", PM_UNOFFICIAL_NOTICE_TEXT);

    if (!dirs_ready) {
        return;
    }

    if (write_notice_marker(marker_path, err, sizeof(err)) != 0) {
        cat_log("PortMaster notice: could not write seen marker: %s", err);
    }
}

static bool confirm_message(const char *message, const char *confirm_label)
{
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Later", .is_confirm = false },
        { .button = CAT_BTN_A, .label = confirm_label ? confirm_label : "OK", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result = {0};
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static pm_controller_layout current_controller_layout(pm_context *ctx)
{
    pm_controller_layout layout = PM_CONTROLLER_LAYOUT_NINTENDO;
    if (pm_controller_layout_load(ctx, &layout) != 0) {
        return PM_CONTROLLER_LAYOUT_NINTENDO;
    }
    return layout;
}

static void build_ui_state(pm_context *ctx,
                           const pm_ui_session_state *session,
                           pm_ui_state *state)
{
    memset(state, 0, sizeof(*state));
    state->lock_loaded = ctx->lock_loaded;
    state->runtime_lock_loaded = ctx->runtime_lock_loaded;
    state->installed = pm_portmaster_is_installed(ctx);
    state->manifest_exists = pm_file_exists(ctx->manifest_path);
    state->has_runtime = pm_portmaster_runtime_available(ctx,
                                                         state->runtime_path,
                                                         sizeof(state->runtime_path),
                                                         &state->uses_system_python);
    state->launchable = pm_portmaster_launch_ready(ctx,
                                                   state->launch_disabled_reason,
                                                   sizeof(state->launch_disabled_reason));

    if (!state->installed) {
        state->setup_state = PM_UI_SETUP_NOT_INSTALLED;
        pm_copy(state->setup_summary, sizeof(state->setup_summary), "Not installed");
    } else if (!state->launchable) {
        state->setup_state = PM_UI_SETUP_INCOMPLETE;
        if (!state->has_runtime) {
            pm_copy(state->setup_summary, sizeof(state->setup_summary), "Python runtime missing");
        } else {
            pm_copy(state->setup_summary, sizeof(state->setup_summary), "Setup needed");
        }
    } else if (!state->manifest_exists) {
        state->setup_state = PM_UI_SETUP_INCOMPLETE;
        state->launchable = false;
        pm_copy(state->setup_summary, sizeof(state->setup_summary), "Setup manifest missing");
        pm_copy(state->launch_disabled_reason,
                sizeof(state->launch_disabled_reason),
                "PortMaster setup needs repair.\n\nUse Repair PortMaster to finish setup.");
    } else {
        state->setup_state = PM_UI_SETUP_READY;
        pm_copy(state->setup_summary, sizeof(state->setup_summary), "Complete");
    }

    if (session && session->update_status_valid) {
        state->update = session->update;
        state->update_status_valid = true;
        if (state->update.update_available) {
            pm_format(state->update_summary, sizeof(state->update_summary),
                      "Stable %s", state->update.source.tag[0] ? state->update.source.tag : "available");
        } else {
            pm_copy(state->update_summary, sizeof(state->update_summary), "Current");
        }
    } else if (session && session->update_error[0]) {
        pm_copy(state->update_summary, sizeof(state->update_summary), "Could not check");
    } else {
        pm_copy(state->update_summary, sizeof(state->update_summary),
                state->installed ? "Not checked" : "Not installed");
    }
}

typedef struct {
    float progress;
    char message[128];
    char *message_ptr;
} pm_setup_progress;

static void setup_progress_init(pm_setup_progress *progress,
                                const char *message,
                                float value)
{
    if (!progress) {
        return;
    }
    progress->progress = value;
    snprintf(progress->message, sizeof(progress->message), "%s",
             message ? message : "");
    progress->message_ptr = progress->message;
}

static void setup_progress_set(pm_setup_progress *progress,
                               float value,
                               const char *message)
{
    if (!progress) {
        return;
    }
    progress->progress = value;
    if (message) {
        snprintf(progress->message, sizeof(progress->message), "%s", message);
    }
}

typedef struct {
    pm_context *ctx;
    char err[512];
    pm_artwork_sync_result artwork;
    bool runtime_installed;
    pm_setup_progress progress;
} pm_repair_job;

static int repair_core(pm_context *ctx,
                       pm_artwork_sync_result *artwork,
                       bool *runtime_installed,
                       pm_setup_progress *progress,
                       char *err,
                       size_t err_size)
{
    if (runtime_installed) {
        *runtime_installed = false;
    }
    setup_progress_set(progress, 0.36f, "Checking runtime");
    if (!pm_portmaster_runtime_available(ctx, NULL, 0, NULL)) {
        if (!ctx->runtime_lock_loaded) {
            snprintf(err, err_size, "PortMaster runtime setup metadata is missing");
            return -1;
        }
        setup_progress_set(progress, 0.46f, "Downloading Python runtime");
        if (pm_install_ui_runtime(ctx, err, err_size) != 0) {
            return -1;
        }
        if (runtime_installed) {
            *runtime_installed = true;
        }
    }

    setup_progress_set(progress, 0.76f, "Applying Leaf support files");
    if (pm_repatch_portmaster_repair(ctx, err, err_size) != 0) {
        return -1;
    }
    if (artwork) {
        setup_progress_set(progress, 0.88f, "Refreshing artwork");
        memset(artwork, 0, sizeof(*artwork));
        if (pm_artwork_sync(ctx, artwork, err, err_size) != 0) {
            return -1;
        }
    }
    setup_progress_set(progress, 0.96f, "Refreshing launcher library");
    pm_request_jawaka_library_rescan(ctx);
    setup_progress_set(progress, 1.0f, "PortMaster is ready");
    return 0;
}

static int repair_worker(void *userdata)
{
    pm_repair_job *job = (pm_repair_job *)userdata;
    job->err[0] = '\0';
    memset(&job->artwork, 0, sizeof(job->artwork));
    return repair_core(job->ctx,
                       &job->artwork,
                       &job->runtime_installed,
                       &job->progress,
                       job->err,
                       sizeof(job->err));
}

typedef struct {
    pm_context *ctx;
    char err[512];
    pm_artwork_sync_result artwork;
    bool runtime_installed;
    pm_setup_progress progress;
} pm_install_job;

static int install_worker(void *userdata)
{
    pm_install_job *job = (pm_install_job *)userdata;
    job->err[0] = '\0';
    memset(&job->artwork, 0, sizeof(job->artwork));
    setup_progress_set(&job->progress, 0.08f, "Downloading PortMaster package");
    if (pm_install_portmaster(job->ctx, job->err, sizeof(job->err)) != 0) {
        return -1;
    }
    setup_progress_set(&job->progress, 0.30f, "Preparing setup");
    return repair_core(job->ctx,
                       &job->artwork,
                       &job->runtime_installed,
                       &job->progress,
                       job->err,
                       sizeof(job->err));
}

static void show_install(pm_context *ctx)
{
    if (!ctx->lock_loaded) {
        char summary[8192];
        snprintf(summary, sizeof(summary), "PortMaster setup metadata is missing.\n\n%s", ctx->lock_path);
        show_message(summary);
        return;
    }

    pm_install_job job = { .ctx = ctx };
    setup_progress_init(&job.progress, "Starting setup", 0.02f);
    cat_process_opts opts = {
        .message = "Installing PortMaster",
        .show_progress = true,
        .progress = &job.progress.progress,
        .interrupt_button = CAT_BTN_NONE,
        .dynamic_message = &job.progress.message_ptr,
        .message_lines = 1,
    };
    int rc = cat_process_message(&opts, install_worker, &job);
    if (rc == 0) {
        show_message("PortMaster is ready.");
    } else {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster setup needs repair.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

static void show_repair(pm_context *ctx)
{
    pm_repair_job job = { .ctx = ctx };
    setup_progress_init(&job.progress, "Checking setup", 0.10f);
    cat_process_opts opts = {
        .message = "Repairing PortMaster",
        .show_progress = true,
        .progress = &job.progress.progress,
        .interrupt_button = CAT_BTN_NONE,
        .dynamic_message = &job.progress.message_ptr,
        .message_lines = 1,
    };
    int rc = cat_process_message(&opts, repair_worker, &job);
    if (rc == 0) {
        show_message("PortMaster is ready.");
    } else {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster repair failed.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

typedef struct {
    pm_context *ctx;
    pm_portmaster_update_status status;
    pm_update_check_policy policy;
    bool cached;
    char err[512];
} pm_update_job;

static int update_check_worker(void *userdata)
{
    pm_update_job *job = (pm_update_job *)userdata;
    job->err[0] = '\0';
    memset(&job->status, 0, sizeof(job->status));
    if (job->cached) {
        return pm_portmaster_check_update_cached_policy(job->ctx,
                                                        &job->status,
                                                        job->policy,
                                                        job->err,
                                                        sizeof(job->err));
    }
    return pm_portmaster_check_update(job->ctx, &job->status, job->err, sizeof(job->err));
}

static int update_apply_worker(void *userdata)
{
    pm_update_job *job = (pm_update_job *)userdata;
    job->err[0] = '\0';
    return pm_portmaster_apply_update(job->ctx, &job->status, job->err, sizeof(job->err));
}

static void store_update_status(pm_ui_session_state *session,
                                const pm_portmaster_update_status *status)
{
    session->update = *status;
    session->update_status_valid = true;
    session->update_error[0] = '\0';
}

static bool run_update_flow(pm_context *ctx,
                            pm_ui_session_state *session,
                            bool check_first,
                            bool force_prompt,
                            bool show_current)
{
    pm_update_job job = {
        .ctx = ctx,
        .policy = PM_UPDATE_CHECK_INTERACTIVE,
        .cached = false,
    };

    if (check_first || !session->update_status_valid) {
        cat_process_opts check_opts = {
            .message = "Checking PortMaster update\n\nFetching stable release metadata.",
            .show_progress = false,
            .interrupt_button = CAT_BTN_NONE,
        };
        int rc = cat_process_message(&check_opts, update_check_worker, &job);
        if (rc != 0) {
            pm_copy(session->update_error, sizeof(session->update_error),
                    job.err[0] ? job.err : "Unknown error");
            char msg[1024];
            snprintf(msg, sizeof(msg), "PortMaster update check failed.\n\n%s",
                     session->update_error);
            show_text_detail("Update Check Failed", msg);
            return false;
        }
        store_update_status(session, &job.status);
    } else {
        job.status = session->update;
    }

    if (!job.status.update_available) {
        if (show_current) {
            char summary[1024];
            pm_portmaster_update_summary(&job.status, summary, sizeof(summary));
            show_text_detail("PortMaster Update", summary);
        }
        return false;
    }

    if (!force_prompt && !pm_portmaster_should_prompt_update(ctx, &job.status)) {
        return false;
    }

    char prompt[1400];
    snprintf(prompt, sizeof(prompt),
             "Installed: %s\nLatest stable: %s\nUpdate: available\n\nUpdate now?",
             job.status.installed_version[0] ? job.status.installed_version : "unknown",
             job.status.source.tag[0] ? job.status.source.tag : "unknown");
    if (!confirm_message(prompt, "Update")) {
        char state_err[256];
        (void)pm_portmaster_record_update_declined(ctx, &job.status,
                                                   state_err, sizeof(state_err));
        return false;
    }

    cat_process_opts apply_opts = {
        .message = "Updating PortMaster\n\nDownloading, verifying, patching, and promoting the managed GUI.",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
    };
    int rc = cat_process_message(&apply_opts, update_apply_worker, &job);
    if (rc == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "PortMaster updated to %s.",
                 job.status.source.tag[0] ? job.status.source.tag : "the latest stable release");
        show_message(msg);
        session->update_status_valid = false;
        session->update_error[0] = '\0';
        return true;
    }

    char state_err[256];
    (void)pm_portmaster_record_update_failed(ctx, &job.status,
                                             job.err[0] ? job.err : "Unknown error",
                                             state_err, sizeof(state_err));
    pm_copy(session->update_error, sizeof(session->update_error),
            job.err[0] ? job.err : "Unknown error");
    char msg[1024];
    snprintf(msg, sizeof(msg), "PortMaster update failed.\n\n%s",
            session->update_error);
    show_text_detail("Update Failed", msg);
    return false;
}

static void run_startup_update_check(pm_context *ctx,
                                     const pm_ui_state *state,
                                     pm_ui_session_state *session)
{
    if (session->startup_update_check_attempted ||
        state->setup_state != PM_UI_SETUP_READY) {
        return;
    }

    session->startup_update_check_attempted = true;
    pm_update_job job = {
        .ctx = ctx,
        .policy = PM_UPDATE_CHECK_STARTUP,
        .cached = true,
    };
    cat_process_opts check_opts = {
        .message = "Checking PortMaster update",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
    };
    int rc = cat_process_message(&check_opts, update_check_worker, &job);
    if (rc != 0) {
        pm_copy(session->update_error, sizeof(session->update_error),
                job.err[0] ? job.err : "Unknown error");
        if (strstr(session->update_error, "timed out") ||
            strstr(session->update_error, "curl=28")) {
            session->startup_update_check_timed_out = true;
        }
        return;
    }

    store_update_status(session, &job.status);
    (void)run_update_flow(ctx, session, false, false, false);
}

typedef struct {
    pm_context *ctx;
    char *phase;
    char err[512];
} pm_launch_job;

static void launch_status(void *userdata, const char *message)
{
    pm_launch_job *job = (pm_launch_job *)userdata;
    if (job) {
        job->phase = (char *)message;
    }
}

static int launch_worker(void *userdata)
{
    pm_launch_job *job = (pm_launch_job *)userdata;
    job->err[0] = '\0';
    return pm_launch_portmaster_with_status(job->ctx,
                                            launch_status,
                                            job,
                                            job->err,
                                            sizeof(job->err));
}

static void show_launch(pm_context *ctx, const pm_ui_state *state)
{
    if (!state->launchable) {
        show_message(state->launch_disabled_reason[0]
                         ? state->launch_disabled_reason
                         : "PortMaster is not ready yet.");
        return;
    }

    pm_launch_job job = {
        .ctx = ctx,
        .phase = "Starting PortMaster...",
    };
    cat_process_opts opts = {
        .message = "PortMaster",
        .show_progress = false,
        .interrupt_button = CAT_BTN_NONE,
        .dynamic_message = &job.phase,
        .message_lines = 1,
    };
    int rc = cat_process_message(&opts, launch_worker, &job);
    if (rc != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "PortMaster exited with an error.\n\n%s",
                 job.err[0] ? job.err : "Unknown error");
        show_message(msg);
    }
}

static void show_controller_layout(pm_context *ctx)
{
    cat_option layout_options[] = {
        { .label = "X360", .value = "x360" },
        { .label = "Nintendo", .value = "nintendo" },
    };
    cat_options_item item = {
        .label = "Controller Layout",
        .type = CAT_OPT_STANDARD,
        .options = layout_options,
        .option_count = 2,
        .selected_option = current_controller_layout(ctx) == PM_CONTROLLER_LAYOUT_NINTENDO ? 1 : 0,
    };
    cat_footer_item footer[] = {
        { .button = CAT_BTN_LEFT, .label = "Change", .button_text = "< >" },
        { .button = CAT_BTN_B, .label = "Back" },
        { .button = CAT_BTN_START, .label = "Save", .is_confirm = true },
    };
    cat_options_list_opts opts = {
        .title = "Controller",
        .items = &item,
        .item_count = 1,
        .footer = footer,
        .footer_count = 3,
        .confirm_button = CAT_BTN_START,
        .label_font = cat_get_font(CAT_FONT_MEDIUM),
        .value_font = cat_get_font(CAT_FONT_TINY),
    };
    cat_options_list_result result = {0};
    if (cat_options_list(&opts, &result) != CAT_OK || result.action == CAT_ACTION_BACK) {
        return;
    }
    if (result.action != CAT_ACTION_CONFIRMED) {
        return;
    }

    pm_controller_layout selected = item.selected_option == 1
        ? PM_CONTROLLER_LAYOUT_NINTENDO
        : PM_CONTROLLER_LAYOUT_X360;
    char err[512];
    if (pm_controller_layout_save(ctx, selected, err, sizeof(err)) != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "Could not save controller layout.\n\n%s",
                 err[0] ? err : "Unknown error");
        show_message(msg);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Controller layout saved: %s.\n\nIt will apply to launched ports. The PortMaster GUI keeps its own A/B mapping.",
             pm_controller_layout_label(selected));
    show_message(msg);
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
    show_text_detail("Paths", msg);
}

static void show_logs(pm_context *ctx)
{
    char msg[8192];
    snprintf(msg, sizeof(msg),
             "Logs are written under:\n%s\n\nCurrent app stderr log:\nportmaster-mlp1.log",
             ctx->logs_path);
    show_text_detail("Logs", msg);
}

static void show_health(pm_context *ctx)
{
    pm_doctor_report report;
    pm_doctor_run(ctx, &report);
    show_text_detail("Health Check", report.text);
}

static void show_troubleshooting_summary(pm_context *ctx,
                                         const pm_ui_state *state,
                                         const pm_ui_session_state *session)
{
    const char *status = "Setup needed";
    if (!state->installed) {
        status = "Not installed";
    } else if (state->launchable) {
        status = "Ready to launch";
    }

    const char *updates = state->update_summary;
    if (session->startup_update_check_timed_out) {
        updates = "Check timed out";
    }

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "Status: %s\nPortMaster: %s\nUpdates: %s\nSetup: %s\nPorts folder: %s\nArtwork folder: %s",
             status,
             state->installed ? "Installed" : "Not installed",
             updates,
             state->setup_summary,
             pm_dir_exists(ctx->ports_dir) ? "Found" : "Missing",
             pm_dir_exists(ctx->port_images_dir) ? "Found" : "Missing");
    show_text_detail("Quick Diagnostics", msg);
}

static pm_trouble_action troubleshooting_menu(int *cursor)
{
    cat_list_item items[] = {
        { .label = "Quick Diagnostics" },
        { .label = "Run Health Check" },
        { .label = "Check For Updates" },
        { .label = "View Logs" },
        { .label = "View Paths" },
    };
    static const pm_trouble_action map[] = {
        PM_TROUBLE_ACTION_QUICK_DIAGNOSTICS,
        PM_TROUBLE_ACTION_HEALTH,
        PM_TROUBLE_ACTION_CHECK_UPDATE,
        PM_TROUBLE_ACTION_LOGS,
        PM_TROUBLE_ACTION_PATHS,
    };
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Back" },
        { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts("Troubleshooting", items,
                                               (int)(sizeof(items) / sizeof(items[0])));
    opts.footer = footer;
    opts.footer_count = 2;
    if (cursor && *cursor >= 0 &&
        *cursor < (int)(sizeof(items) / sizeof(items[0]))) {
        opts.initial_index = *cursor;
    }

    cat_list_result result = {0};
    if (cat_list(&opts, &result) != CAT_OK ||
        result.selected_index < 0 ||
        result.selected_index >= (int)(sizeof(map) / sizeof(map[0]))) {
        return PM_TROUBLE_ACTION_BACK;
    }
    if (cursor) {
        *cursor = result.selected_index;
    }
    return map[result.selected_index];
}

static void show_troubleshooting(pm_context *ctx,
                                 pm_ui_session_state *session)
{
    int cursor = 0;
    bool running = true;
    while (running) {
        pm_ui_state state;
        build_ui_state(ctx, session, &state);

        switch (troubleshooting_menu(&cursor)) {
            case PM_TROUBLE_ACTION_QUICK_DIAGNOSTICS:
                show_troubleshooting_summary(ctx, &state, session);
                break;
            case PM_TROUBLE_ACTION_HEALTH:
                show_health(ctx);
                break;
            case PM_TROUBLE_ACTION_CHECK_UPDATE:
                if (!state.installed) {
                    show_message("PortMaster is not installed yet.\n\nInstall PortMaster before checking for updates.");
                    break;
                }
                (void)run_update_flow(ctx, session, true, true, true);
                break;
            case PM_TROUBLE_ACTION_LOGS:
                show_logs(ctx);
                break;
            case PM_TROUBLE_ACTION_PATHS:
                show_paths(ctx);
                break;
            case PM_TROUBLE_ACTION_BACK:
            case PM_TROUBLE_ACTION_NONE:
            default:
                running = false;
                break;
        }
    }
}

static int build_menu_rows(pm_context *ctx,
                           const pm_ui_state *state,
                           cat_list_item *items,
                           pm_menu_row *rows,
                           int max_rows)
{
    int count = 0;
    if (!ctx || !state || !items || !rows || max_rows < 5) {
        return 0;
    }

    items[count] = (cat_list_item){
        .label = "Launch PortMaster",
        .trailing_text = state->launchable
            ? (state->update_status_valid && state->update.update_available ? "Update available" : "Ready")
            : (state->setup_state == PM_UI_SETUP_NOT_INSTALLED ? "Not installed" : "Setup needed"),
        .disabled = !state->launchable,
    };
    rows[count++] = (pm_menu_row){
        .action = PM_ACTION_LAUNCH,
        .disabled = !state->launchable,
        .disabled_message = state->launch_disabled_reason,
    };

    if (!state->installed) {
        bool disabled = !ctx->lock_loaded;
        items[count] = (cat_list_item){
            .label = "Install PortMaster",
            .trailing_text = disabled ? "Metadata missing" : "Ready",
            .disabled = disabled,
        };
        rows[count++] = (pm_menu_row){
            .action = PM_ACTION_INSTALL,
            .disabled = disabled,
            .disabled_message = "PortMaster setup metadata is missing.",
        };
    } else if (state->update_status_valid && state->update.update_available) {
        items[count] = (cat_list_item){
            .label = "Update PortMaster",
            .trailing_text = state->update_summary,
        };
        rows[count++] = (pm_menu_row){ .action = PM_ACTION_UPDATE };
    }

    if (state->installed) {
        items[count] = (cat_list_item){
            .label = "Repair PortMaster",
            .trailing_text = state->setup_state == PM_UI_SETUP_READY ? "Fix setup" : "Finish setup",
        };
        rows[count++] = (pm_menu_row){ .action = PM_ACTION_REPAIR };
    }

    pm_controller_layout layout = current_controller_layout(ctx);
    items[count] = (cat_list_item){
        .label = "Controller Layout",
        .trailing_text = pm_controller_layout_label(layout),
    };
    rows[count++] = (pm_menu_row){ .action = PM_ACTION_CONTROLLER_LAYOUT };

    items[count] = (cat_list_item){
        .label = "Troubleshooting",
        .trailing_text = state->setup_state == PM_UI_SETUP_READY ? state->update_summary : state->setup_summary,
    };
    rows[count++] = (pm_menu_row){ .action = PM_ACTION_TROUBLESHOOTING };

    return count;
}

static pm_action menu(pm_context *ctx, const pm_ui_state *state)
{
    cat_list_item items[6];
    pm_menu_row rows[6];
    int count = build_menu_rows(ctx, state, items, rows, (int)(sizeof(items) / sizeof(items[0])));
    if (count <= 0) {
        return PM_ACTION_QUIT;
    }

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Quit" },
        { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts("PortMaster", items, count);
    opts.footer = footer;
    opts.footer_count = 2;
    opts.initial_index = 0;

    cat_list_result result = {0};
    if (cat_list(&opts, &result) != CAT_OK) {
        return PM_ACTION_QUIT;
    }
    if (result.selected_index < 0 || result.selected_index >= count) {
        return PM_ACTION_QUIT;
    }
    if (rows[result.selected_index].disabled) {
        show_message(rows[result.selected_index].disabled_message
                         ? rows[result.selected_index].disabled_message
                         : "This action is not available yet.");
        return PM_ACTION_NONE;
    }
    return rows[result.selected_index].action;
}

int pm_ui_menu_state_text(pm_context *ctx, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    pm_ui_session_state session = {0};
    pm_ui_state state;
    build_ui_state(ctx, &session, &state);

    cat_list_item items[6];
    pm_menu_row rows[6];
    int count = build_menu_rows(ctx, &state, items, rows,
                                (int)(sizeof(items) / sizeof(items[0])));

    int rc = 0;
    size_t used = 0;
    if (appendf(out, out_size, &used,
                "setup=%s\tinstalled=%d\tlaunchable=%d\tupdate_status=%s\tupdate_available=%d\n",
                setup_state_slug(state.setup_state),
                state.installed ? 1 : 0,
                state.launchable ? 1 : 0,
                state.update_summary,
                state.update_status_valid && state.update.update_available ? 1 : 0) != 0) {
        rc = -1;
    }
    for (int i = 0; i < count; i++) {
        if (appendf(out, out_size, &used,
                    "row=%d\taction=%s\tdisabled=%d\tlabel=",
                    i,
                    action_slug(rows[i].action),
                    rows[i].disabled ? 1 : 0) != 0) {
            rc = -1;
        }
        append_text_escaped(out, out_size, &used, items[i].label);
        if (appendf(out, out_size, &used, "\ttrailing=") != 0) {
            rc = -1;
        }
        append_text_escaped(out, out_size, &used, items[i].trailing_text);
        if (appendf(out, out_size, &used, "\tdisabled_message=") != 0) {
            rc = -1;
        }
        append_text_escaped(out, out_size, &used,
                            rows[i].disabled ? rows[i].disabled_message : "");
        if (appendf(out, out_size, &used, "\n") != 0) {
            rc = -1;
        }
    }
    return rc;
}

void pm_ui_run(pm_context *ctx)
{
    pm_ui_session_state session = {0};
    pm_ui_state state;
    build_ui_state(ctx, &session, &state);
    show_unofficial_support_notice_if_needed(ctx);
    run_startup_update_check(ctx, &state, &session);

    bool running = true;
    while (running) {
        build_ui_state(ctx, &session, &state);
        switch (menu(ctx, &state)) {
            case PM_ACTION_NONE:
                break;
            case PM_ACTION_INSTALL:
                show_install(ctx);
                session.update_status_valid = false;
                session.update_error[0] = '\0';
                break;
            case PM_ACTION_UPDATE:
                (void)run_update_flow(ctx, &session, true, true, true);
                break;
            case PM_ACTION_LAUNCH:
                show_launch(ctx, &state);
                break;
            case PM_ACTION_CONTROLLER_LAYOUT:
                show_controller_layout(ctx);
                break;
            case PM_ACTION_REPAIR:
                show_repair(ctx);
                break;
            case PM_ACTION_TROUBLESHOOTING:
                show_troubleshooting(ctx, &session);
                break;
            case PM_ACTION_QUIT:
            default:
                running = false;
                break;
        }
    }
}
