#include "pm_preferences.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool pm_install_source_valid(const char *source_id)
{
    return source_id &&
           (strcmp(source_id, PM_INSTALL_SOURCE_PRIMARY) == 0 ||
            strcmp(source_id, PM_INSTALL_SOURCE_SECONDARY) == 0);
}

static int preference_path(const pm_context *ctx, char *out, size_t out_size)
{
    return ctx ? pm_join(out, out_size, ctx->leaf_dir, "preferences.json") : -1;
}

const char *pm_install_source_label(const char *source_id)
{
    return source_id && strcmp(source_id, PM_INSTALL_SOURCE_SECONDARY) == 0
        ? "Secondary SD" : "Primary SD";
}

int pm_install_source_preference_load(pm_context *ctx, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        return -1;
    }
    (void)pm_copy(ctx->preferred_install_source,
                  sizeof(ctx->preferred_install_source),
                  PM_INSTALL_SOURCE_PRIMARY);
    ctx->effective_install_source[0] = '\0';

    char path[PM_PATH_MAX];
    if (preference_path(ctx, path, sizeof(path)) != 0) {
        snprintf(err, err_size, "preference path is too long");
        return -1;
    }
    if (!pm_file_exists(path)) {
        const pm_source *primary =
            pm_sources_by_id(&ctx->sources, PM_INSTALL_SOURCE_PRIMARY);
        if (primary && primary->available) {
            (void)pm_copy(ctx->effective_install_source,
                          sizeof(ctx->effective_install_source),
                          PM_INSTALL_SOURCE_PRIMARY);
        }
        return 0;
    }

    char read_err[128];
    char *text = pm_read_text_file(path, 64 * 1024, read_err, sizeof(read_err));
    if (!text) {
        snprintf(err, err_size, "cannot read install-card preference: %s",
                 read_err[0] ? read_err : path);
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    cJSON *value = root
        ? cJSON_GetObjectItemCaseSensitive(root, "install_source") : NULL;
    if (!cJSON_IsObject(root) || !cJSON_IsString(value) ||
        !pm_install_source_valid(value->valuestring)) {
        cJSON_Delete(root);
        snprintf(err, err_size,
                 "invalid install-card preference; expected primary or secondary_sd");
        return -1;
    }
    int rc = pm_copy(ctx->preferred_install_source,
                     sizeof(ctx->preferred_install_source),
                     value->valuestring);
    cJSON_Delete(root);
    const pm_source *preferred =
        pm_sources_by_id(&ctx->sources, ctx->preferred_install_source);
    if (rc == 0 && preferred && preferred->available) {
        rc = pm_copy(ctx->effective_install_source,
                     sizeof(ctx->effective_install_source),
                     ctx->preferred_install_source);
    }
    return rc;
}

int pm_install_source_preference_save(pm_context *ctx, const char *source_id,
                                      char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx || !pm_install_source_valid(source_id)) {
        snprintf(err, err_size, "unknown install source: %s",
                 source_id ? source_id : "(null)");
        return -1;
    }
    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        return -1;
    }

    char path[PM_PATH_MAX];
    char tmp[PM_PATH_MAX];
    if (preference_path(ctx, path, sizeof(path)) != 0 ||
        pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0) {
        snprintf(err, err_size, "preference path is too long");
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "install_source", source_id)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "cannot create install-card preference");
        return -1;
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(err, err_size, "cannot encode install-card preference");
        return -1;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t size = strlen(text);
    size_t used = 0;
    int saved_errno = fd >= 0 ? 0 : errno;
    while (fd >= 0 && used < size) {
        ssize_t written = write(fd, text + used, size - used);
        if (written > 0) {
            used += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        saved_errno = errno;
        break;
    }
    int flush_rc = used == size ? fsync(fd) : -1;
    if (flush_rc != 0 && saved_errno == 0) {
        saved_errno = errno;
    }
    if (fd >= 0) {
        close(fd);
    }
    free(text);
    if (used != size || flush_rc != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot write install-card preference: %s",
                 strerror(saved_errno));
        return -1;
    }
    if (rename(tmp, path) != 0) {
        saved_errno = errno;
        unlink(tmp);
        snprintf(err, err_size, "cannot commit install-card preference: %s",
                 strerror(saved_errno));
        return -1;
    }
    if (pm_copy(ctx->preferred_install_source,
                sizeof(ctx->preferred_install_source), source_id) != 0) {
        return -1;
    }
    return pm_context_refresh_sources(ctx, err, err_size);
}

int pm_install_source_resolve(pm_context *ctx, const char *session_source_id,
                              const pm_source **source,
                              char *err, size_t err_size)
{
    if (source) {
        *source = NULL;
    }
    if (!ctx) {
        snprintf(err, err_size, "missing PortMaster context");
        return -1;
    }
    if (pm_context_refresh_sources(ctx, err, err_size) != 0 ||
        pm_install_source_preference_load(ctx, err, err_size) != 0) {
        return -1;
    }
    const char *effective = session_source_id && session_source_id[0]
        ? session_source_id : ctx->preferred_install_source;
    if (!pm_install_source_valid(effective)) {
        snprintf(err, err_size, "unknown session install source: %s", effective);
        return -1;
    }
    const pm_source *match = pm_sources_by_id(&ctx->sources, effective);
    if (!match || !match->configured) {
        snprintf(err, err_size, "%s is not configured",
                 pm_install_source_label(effective));
        return -1;
    }
    if (!match->available) {
        snprintf(err, err_size,
                 "%s is preferred but is not mounted. Insert that card or choose Primary for this session.",
                 pm_install_source_label(effective));
        return -1;
    }
    (void)pm_copy(ctx->effective_install_source,
                  sizeof(ctx->effective_install_source), effective);
    if (source) {
        *source = match;
    }
    return 0;
}
