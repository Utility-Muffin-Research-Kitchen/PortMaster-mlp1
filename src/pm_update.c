#include "pm_update.h"

#include "pm_downloader.h"
#include "pm_util.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PM_GUI_VERSION_URL "https://github.com/PortsMaster/PortMaster-GUI/releases/latest/download/version.json"
#define PM_UPDATE_CHECK_INTERVAL_SECONDS (24 * 60 * 60)

typedef struct {
    long long last_checked_unix;
    char latest_version[64];
    char url[1024];
    char md5[33];
    char release_url[1024];
    char manager_version[64];
    char patch_set[64];
    char patch_fingerprint[65];
    char declined_version[64];
    char failed_version[64];
    char failed_reason[256];
    char failed_manager_version[64];
    char failed_patch_set[64];
    char failed_patch_fingerprint[65];
} pm_update_state;

static int version_compare(const char *a, const char *b);
static int read_installed_version(pm_context *ctx, char *out, size_t out_size);

static int json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return -1;
    }
    return pm_copy(dst, dst_size, item->valuestring);
}

static bool env_truthy(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
}

static int state_path(pm_context *ctx, char *out, size_t out_size)
{
    return pm_join(out, out_size, ctx->leaf_dir, "gui-update-state.json");
}

static int update_log_path(pm_context *ctx, char *out, size_t out_size)
{
    char log_dir[PM_PATH_MAX];
    if (pm_join(log_dir, sizeof(log_dir), ctx->leaf_dir, "logs") != 0) {
        return -1;
    }
    return pm_join(out, out_size, log_dir, "update.log");
}

static void checked_at_now(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    gmtime_r(&now, &tmv);
#else
    tmv = *gmtime(&now);
#endif
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void append_sanitized(FILE *fp, const char *text)
{
    for (const char *p = text ? text : ""; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        fputc((ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch, fp);
    }
}

static int current_update_identity(pm_context *ctx,
                                   char *manager_version,
                                   size_t manager_size,
                                   char *patch_set,
                                   size_t patch_set_size,
                                   char patch_fingerprint[65],
                                   char *err,
                                   size_t err_size)
{
    if (pm_copy(manager_version, manager_size, PM_VERSION) != 0 ||
        pm_copy(patch_set, patch_set_size, pm_portmaster_patch_set_id()) != 0) {
        snprintf(err, err_size, "update identity path too long");
        return -1;
    }
    return pm_portmaster_patch_set_fingerprint(ctx, patch_fingerprint, err, err_size);
}

static void update_log(pm_context *ctx,
                       const char *event,
                       const pm_portmaster_update_status *status,
                       const char *detail)
{
    if (!ctx || !event || pm_context_ensure_manager_dirs(ctx, NULL, 0) != 0) {
        return;
    }

    char log_dir[PM_PATH_MAX];
    char path[PM_PATH_MAX];
    if (pm_join(log_dir, sizeof(log_dir), ctx->leaf_dir, "logs") != 0 ||
        pm_mkdir_p(log_dir, NULL, 0) != 0 ||
        update_log_path(ctx, path, sizeof(path)) != 0) {
        return;
    }

    FILE *fp = fopen(path, "ab");
    if (!fp) {
        return;
    }
    char ts[64];
    checked_at_now(ts, sizeof(ts));
    fprintf(fp, "%s event=%s manager=%s patch_set=%s",
            ts, event, PM_VERSION, pm_portmaster_patch_set_id());
    if (status) {
        fprintf(fp, " installed=%s latest=%s source=%s",
                status->installed_version[0] ? status->installed_version : "-",
                status->source.tag[0] ? status->source.tag : "-",
                status->from_cache ? "cache" : "network");
    }
    if (detail && detail[0]) {
        fputs(" detail=", fp);
        append_sanitized(fp, detail);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_state(pm_context *ctx, pm_update_state *state)
{
    memset(state, 0, sizeof(*state));
    char path[PM_PATH_MAX];
    if (!ctx || state_path(ctx, path, sizeof(path)) != 0 || !pm_file_exists(path)) {
        return -1;
    }

    char read_err[128];
    char *text = pm_read_text_file(path, 256 * 1024, read_err, sizeof(read_err));
    if (!text) {
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return -1;
    }

    cJSON *last = cJSON_GetObjectItemCaseSensitive(root, "last_checked_unix");
    if (cJSON_IsNumber(last)) {
        state->last_checked_unix = (long long)last->valuedouble;
    }
    (void)json_string(root, "latest_version", state->latest_version, sizeof(state->latest_version));
    (void)json_string(root, "url", state->url, sizeof(state->url));
    (void)json_string(root, "md5", state->md5, sizeof(state->md5));
    (void)json_string(root, "release_url", state->release_url, sizeof(state->release_url));
    (void)json_string(root, "manager_version", state->manager_version, sizeof(state->manager_version));
    (void)json_string(root, "patch_set", state->patch_set, sizeof(state->patch_set));
    (void)json_string(root, "patch_fingerprint", state->patch_fingerprint, sizeof(state->patch_fingerprint));
    (void)json_string(root, "declined_version", state->declined_version, sizeof(state->declined_version));
    (void)json_string(root, "failed_version", state->failed_version, sizeof(state->failed_version));
    (void)json_string(root, "failed_reason", state->failed_reason, sizeof(state->failed_reason));
    (void)json_string(root, "failed_manager_version", state->failed_manager_version, sizeof(state->failed_manager_version));
    (void)json_string(root, "failed_patch_set", state->failed_patch_set, sizeof(state->failed_patch_set));
    (void)json_string(root, "failed_patch_fingerprint", state->failed_patch_fingerprint, sizeof(state->failed_patch_fingerprint));
    cJSON_Delete(root);
    return state->latest_version[0] && state->url[0] && state->md5[0] ? 0 : -1;
}

static int write_state(pm_context *ctx, const pm_update_state *state, char *err, size_t err_size)
{
    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        return -1;
    }

    char path[PM_PATH_MAX];
    char tmp[PM_PATH_MAX];
    if (state_path(ctx, path, sizeof(path)) != 0 ||
        pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0) {
        snprintf(err, err_size, "update state path too long");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(err, err_size, "cannot create update state JSON");
        return -1;
    }
    cJSON_AddNumberToObject(root, "last_checked_unix", (double)state->last_checked_unix);
    cJSON_AddStringToObject(root, "latest_version", state->latest_version);
    cJSON_AddStringToObject(root, "url", state->url);
    cJSON_AddStringToObject(root, "md5", state->md5);
    cJSON_AddStringToObject(root, "release_url", state->release_url);
    cJSON_AddStringToObject(root, "manager_version", state->manager_version);
    cJSON_AddStringToObject(root, "patch_set", state->patch_set);
    cJSON_AddStringToObject(root, "patch_fingerprint", state->patch_fingerprint);
    cJSON_AddStringToObject(root, "declined_version", state->declined_version);
    cJSON_AddStringToObject(root, "failed_version", state->failed_version);
    cJSON_AddStringToObject(root, "failed_reason", state->failed_reason);
    cJSON_AddStringToObject(root, "failed_manager_version", state->failed_manager_version);
    cJSON_AddStringToObject(root, "failed_patch_set", state->failed_patch_set);
    cJSON_AddStringToObject(root, "failed_patch_fingerprint", state->failed_patch_fingerprint);

    char *printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        snprintf(err, err_size, "cannot print update state JSON");
        return -1;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        snprintf(err, err_size, "cannot write %s", tmp);
        free(printed);
        return -1;
    }
    fputs(printed, fp);
    fputc('\n', fp);
    free(printed);
    if (fclose(fp) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot finish %s", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot promote update state");
        return -1;
    }
    return 0;
}

static void state_from_status(pm_context *ctx,
                              const pm_portmaster_update_status *status,
                              pm_update_state *state)
{
    state->last_checked_unix = (long long)time(NULL);
    pm_copy(state->latest_version, sizeof(state->latest_version), status->source.tag);
    pm_copy(state->url, sizeof(state->url), status->source.url);
    pm_copy(state->md5, sizeof(state->md5), status->source.md5);
    pm_copy(state->release_url, sizeof(state->release_url), status->source.release_url);
    char identity_err[128];
    if (current_update_identity(ctx,
                                state->manager_version, sizeof(state->manager_version),
                                state->patch_set, sizeof(state->patch_set),
                                state->patch_fingerprint,
                                identity_err, sizeof(identity_err)) != 0) {
        pm_copy(state->manager_version, sizeof(state->manager_version), PM_VERSION);
        pm_copy(state->patch_set, sizeof(state->patch_set), pm_portmaster_patch_set_id());
        state->patch_fingerprint[0] = '\0';
    }
}

static int status_from_state(pm_context *ctx, const pm_update_state *state,
                             pm_portmaster_update_status *out, char *err, size_t err_size)
{
    memset(out, 0, sizeof(*out));
    if (read_installed_version(ctx, out->installed_version, sizeof(out->installed_version)) != 0) {
        snprintf(err, err_size, "cannot determine installed PortMaster version");
        return -1;
    }
    pm_portmaster_source *source = &out->source;
    pm_copy(source->repo, sizeof(source->repo), "PortsMaster/PortMaster-GUI");
    pm_copy(source->channel, sizeof(source->channel), "stable");
    pm_copy(source->asset, sizeof(source->asset), "PortMaster.zip");
    pm_copy(source->tag, sizeof(source->tag), state->latest_version);
    pm_copy(source->url, sizeof(source->url), state->url);
    pm_copy(source->md5, sizeof(source->md5), state->md5);
    pm_copy(source->release_url, sizeof(source->release_url), state->release_url);
    checked_at_now(source->checked_at, sizeof(source->checked_at));
    out->update_available = version_compare(source->tag, out->installed_version) > 0;
    out->from_cache = true;
    return 0;
}

static int parse_next_number(const char **cursor, long *out)
{
    const char *p = *cursor;
    while (*p && !isdigit((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return 0;
    }
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) {
        *cursor = p;
        return 0;
    }
    *out = value;
    *cursor = end;
    return 1;
}

static int version_compare(const char *a, const char *b)
{
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";
    int saw_number = 0;
    while (*pa || *pb) {
        long na = 0;
        long nb = 0;
        int ha = parse_next_number(&pa, &na);
        int hb = parse_next_number(&pb, &nb);
        if (!ha && !hb) {
            break;
        }
        saw_number = 1;
        if (na < nb) {
            return -1;
        }
        if (na > nb) {
            return 1;
        }
    }
    return saw_number ? 0 : strcmp(a ? a : "", b ? b : "");
}

static int read_installed_version(pm_context *ctx, char *out, size_t out_size)
{
    if (!ctx || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    char pugwash[PM_PATH_MAX];
    if (pm_join(pugwash, sizeof(pugwash), ctx->portmaster_dir, "pugwash") == 0 &&
        pm_file_exists(pugwash)) {
        char read_err[128];
        char *text = pm_read_text_file(pugwash, 256 * 1024, read_err, sizeof(read_err));
        if (text) {
            const char *needle = "PORTMASTER_VERSION";
            char *p = strstr(text, needle);
            if (p) {
                p = strchr(p, '=');
            }
            if (p) {
                p++;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                if (*p == '\'' || *p == '"') {
                    char quote = *p++;
                    char *end = strchr(p, quote);
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len < out_size) {
                            memcpy(out, p, len);
                            out[len] = '\0';
                            free(text);
                            return 0;
                        }
                    }
                }
            }
            free(text);
        }
    }

    if (ctx->lock_loaded && ctx->lock.tag[0]) {
        return pm_copy(out, out_size, ctx->lock.tag);
    }
    return -1;
}

int pm_portmaster_check_update(pm_context *ctx, pm_portmaster_update_status *out,
                               char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !out) {
        snprintf(err, err_size, "missing update context");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    if (read_installed_version(ctx, out->installed_version, sizeof(out->installed_version)) != 0) {
        snprintf(err, err_size, "cannot determine installed PortMaster version");
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    update_log(ctx, "check-start", out, NULL);
    const char *url = pm_env("LEAF_PM_UPDATE_VERSION_URL", PM_GUI_VERSION_URL);
    char *text = NULL;
    bool allow_http_metadata = env_truthy("LEAF_PM_ALLOW_HTTP_UPDATE_METADATA");
    if (pm_download_text(url, 1024 * 1024, allow_http_metadata, &text, err, err_size) != 0) {
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        snprintf(err, err_size, "invalid PortMaster version metadata");
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    cJSON *stable = cJSON_GetObjectItemCaseSensitive(root, "stable");
    if (!cJSON_IsObject(stable)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "PortMaster version metadata has no stable channel");
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    pm_portmaster_source *source = &out->source;
    pm_copy(source->repo, sizeof(source->repo), "PortsMaster/PortMaster-GUI");
    pm_copy(source->channel, sizeof(source->channel), "stable");
    pm_copy(source->asset, sizeof(source->asset), "PortMaster.zip");
    checked_at_now(source->checked_at, sizeof(source->checked_at));

    if (json_string(stable, "version", source->tag, sizeof(source->tag)) != 0 ||
        json_string(stable, "url", source->url, sizeof(source->url)) != 0 ||
        json_string(stable, "md5", source->md5, sizeof(source->md5)) != 0) {
        cJSON_Delete(root);
        snprintf(err, err_size, "stable PortMaster metadata is incomplete");
        update_log(ctx, "check-failed", out, err);
        return -1;
    }
    if (pm_format(source->release_url, sizeof(source->release_url),
                  "https://github.com/PortsMaster/PortMaster-GUI/releases/tag/%s",
                  source->tag) != 0) {
        cJSON_Delete(root);
        snprintf(err, err_size, "release URL path too long");
        update_log(ctx, "check-failed", out, err);
        return -1;
    }

    cJSON_Delete(root);
    out->update_available = version_compare(source->tag, out->installed_version) > 0;

    pm_update_state state;
    if (load_state(ctx, &state) != 0) {
        memset(&state, 0, sizeof(state));
    }
    state_from_status(ctx, out, &state);
    char state_err[256];
    if (write_state(ctx, &state, state_err, sizeof(state_err)) != 0) {
        fprintf(stderr, "PortMaster update state warning: %s\n", state_err);
    }
    update_log(ctx, out->update_available ? "check-update-available" : "check-current",
               out, NULL);
    return 0;
}

int pm_portmaster_check_update_cached(pm_context *ctx, pm_portmaster_update_status *out,
                                      char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !out) {
        snprintf(err, err_size, "missing update context");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (env_truthy("LEAF_PM_SKIP_UPDATE_CHECK")) {
        if (read_installed_version(ctx, out->installed_version, sizeof(out->installed_version)) != 0) {
            pm_copy(out->installed_version, sizeof(out->installed_version), "unknown");
        }
        return 0;
    }

    pm_update_state state;
    bool force = env_truthy("LEAF_PM_FORCE_UPDATE_CHECK");
    long long now = (long long)time(NULL);
    if (!force && load_state(ctx, &state) == 0 &&
        state.last_checked_unix > 0 &&
        now >= state.last_checked_unix &&
        now - state.last_checked_unix < PM_UPDATE_CHECK_INTERVAL_SECONDS) {
        return status_from_state(ctx, &state, out, err, err_size);
    }

    return pm_portmaster_check_update(ctx, out, err, err_size);
}

int pm_portmaster_apply_update(pm_context *ctx, const pm_portmaster_update_status *status,
                               char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !status || !status->update_available) {
        snprintf(err, err_size, "no PortMaster update is available");
        return -1;
    }
    update_log(ctx, "apply-start", status, NULL);
    if (pm_install_portmaster_source(ctx, &status->source, err, err_size) != 0) {
        update_log(ctx, "apply-failed", status, err);
        return -1;
    }

    pm_update_state state;
    if (load_state(ctx, &state) == 0 &&
        strcmp(state.latest_version, status->source.tag) == 0) {
        state.declined_version[0] = '\0';
        state.failed_version[0] = '\0';
        state.failed_reason[0] = '\0';
        char state_err[256];
        if (write_state(ctx, &state, state_err, sizeof(state_err)) != 0) {
            fprintf(stderr, "PortMaster update state warning: %s\n", state_err);
        }
    }
    update_log(ctx, "apply-success", status, NULL);
    return 0;
}

int pm_portmaster_record_update_declined(pm_context *ctx,
                                         const pm_portmaster_update_status *status,
                                         char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !status || !status->source.tag[0]) {
        snprintf(err, err_size, "missing declined update status");
        return -1;
    }
    pm_update_state state;
    if (load_state(ctx, &state) != 0) {
        memset(&state, 0, sizeof(state));
        state_from_status(ctx, status, &state);
    }
    pm_copy(state.declined_version, sizeof(state.declined_version), status->source.tag);
    int rc = write_state(ctx, &state, err, err_size);
    update_log(ctx, rc == 0 ? "declined" : "decline-record-failed",
               status, rc == 0 ? NULL : err);
    return rc;
}

int pm_portmaster_record_update_failed(pm_context *ctx,
                                       const pm_portmaster_update_status *status,
                                       const char *reason,
                                       char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !status || !status->source.tag[0]) {
        snprintf(err, err_size, "missing failed update status");
        return -1;
    }
    pm_update_state state;
    if (load_state(ctx, &state) != 0) {
        memset(&state, 0, sizeof(state));
        state_from_status(ctx, status, &state);
    }
    pm_copy(state.failed_version, sizeof(state.failed_version), status->source.tag);
    pm_copy(state.failed_reason, sizeof(state.failed_reason), reason ? reason : "");
    char identity_err[128];
    if (current_update_identity(ctx,
                                state.failed_manager_version,
                                sizeof(state.failed_manager_version),
                                state.failed_patch_set,
                                sizeof(state.failed_patch_set),
                                state.failed_patch_fingerprint,
                                identity_err,
                                sizeof(identity_err)) != 0) {
        pm_copy(state.failed_manager_version, sizeof(state.failed_manager_version), PM_VERSION);
        pm_copy(state.failed_patch_set, sizeof(state.failed_patch_set),
                pm_portmaster_patch_set_id());
        state.failed_patch_fingerprint[0] = '\0';
    }
    int rc = write_state(ctx, &state, err, err_size);
    update_log(ctx, rc == 0 ? "failure-recorded" : "failure-record-failed",
               status, reason ? reason : (rc == 0 ? NULL : err));
    return rc;
}

bool pm_portmaster_should_prompt_update(pm_context *ctx,
                                        const pm_portmaster_update_status *status)
{
    if (!ctx || !status || !status->update_available || !status->source.tag[0]) {
        return false;
    }
    pm_update_state state;
    if (load_state(ctx, &state) != 0) {
        return true;
    }
    if (strcmp(state.declined_version, status->source.tag) == 0) {
        return false;
    }
    if (strcmp(state.failed_version, status->source.tag) == 0) {
        char manager_version[64];
        char patch_set[64];
        char patch_fingerprint[65];
        char identity_err[128];
        if (current_update_identity(ctx,
                                    manager_version, sizeof(manager_version),
                                    patch_set, sizeof(patch_set),
                                    patch_fingerprint,
                                    identity_err, sizeof(identity_err)) != 0) {
            return true;
        }
        return !(strcmp(state.failed_manager_version, manager_version) == 0 &&
                 strcmp(state.failed_patch_set, patch_set) == 0 &&
                 strcmp(state.failed_patch_fingerprint, patch_fingerprint) == 0);
    }
    return true;
}

void pm_portmaster_update_summary(const pm_portmaster_update_status *status,
                                  char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!status) {
        snprintf(out, out_size, "%s", "No update status.");
        return;
    }
    snprintf(out, out_size,
             "Installed: %s\nLatest stable: %s\nUpdate: %s\n\n%s",
             status->installed_version[0] ? status->installed_version : "unknown",
             status->source.tag[0] ? status->source.tag : "unknown",
             status->update_available ? "available" : "not needed",
             status->source.release_url);
}
