#include "pm_env_snapshot.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void snapshot_mode_slug(const char *mode, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *src = (mode && mode[0]) ? mode : "latest";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < out_size; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_') {
            out[used++] = (char)*p;
        } else {
            out[used++] = '_';
        }
    }
    if (used == 0) {
        out[used++] = 'x';
    }
    out[used] = '\0';
}

int pm_env_snapshot_path(const pm_context *ctx,
                         const char *mode,
                         const char *ext,
                         char *out,
                         size_t out_size)
{
    if (!ctx || !ext || !ext[0]) {
        return -1;
    }

    char file[128];
    if (!mode || !mode[0] || strcmp(mode, "latest") == 0) {
        if (pm_format(file, sizeof(file), "launch-env.%s", ext) != 0) {
            return -1;
        }
    } else {
        char slug[64];
        snapshot_mode_slug(mode, slug, sizeof(slug));
        if (pm_format(file, sizeof(file), "launch-env-%s.%s", slug, ext) != 0) {
            return -1;
        }
    }
    return pm_join(out, out_size, ctx->leaf_dir, file);
}

static int previous_snapshot_path(const pm_context *ctx,
                                  const char *mode,
                                  const char *ext,
                                  char *out,
                                  size_t out_size)
{
    if (!ctx || !ext || !ext[0]) {
        return -1;
    }

    char file[160];
    if (!mode || !mode[0] || strcmp(mode, "latest") == 0) {
        if (pm_format(file, sizeof(file), "launch-env.previous.%s", ext) != 0) {
            return -1;
        }
    } else {
        char slug[64];
        snapshot_mode_slug(mode, slug, sizeof(slug));
        if (pm_format(file, sizeof(file), "launch-env-%s.previous.%s", slug, ext) != 0) {
            return -1;
        }
    }
    return pm_join(out, out_size, ctx->leaf_dir, file);
}

static void rotate_previous(const char *path, const char *previous)
{
    if (!path || !previous || !pm_file_exists(path)) {
        return;
    }
    unlink(previous);
    (void)rename(path, previous);
}

static int write_bytes_atomic(const char *path, const char *bytes, char *err, size_t err_size)
{
    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0) {
        snprintf(err, err_size, "snapshot tmp path too long");
        return -1;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        snprintf(err, err_size, "cannot write %s", tmp);
        return -1;
    }
    if (bytes && bytes[0]) {
        fputs(bytes, fp);
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot close %s", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot replace %s", path);
        return -1;
    }
    return 0;
}

static int write_json_snapshot(const pm_context *ctx,
                               const char *mode,
                               const pm_env_snapshot_entry *entries,
                               const char *path,
                               char *err,
                               size_t err_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        snprintf(err, err_size, "cannot allocate snapshot json");
        return -1;
    }

    time_t now = time(NULL);
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "kind", "portmaster-launch-env");
    cJSON_AddStringToObject(root, "mode", mode && mode[0] ? mode : "latest");
    cJSON_AddNumberToObject(root, "generated_at_unix", (double)now);
    cJSON_AddStringToObject(root, "manager_version", PM_VERSION);
    cJSON_AddStringToObject(root, "platform", ctx->platform);
    cJSON_AddStringToObject(root, "sdcard_path", ctx->sdcard_path);
    cJSON_AddItemToObject(root, "entries", items);

    for (size_t i = 0; entries && entries[i].name; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            snprintf(err, err_size, "cannot allocate snapshot entry");
            return -1;
        }
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddStringToObject(item, "value", entries[i].value ? entries[i].value : "");
        cJSON_AddStringToObject(item, "source", entries[i].source ? entries[i].source : "manager");
        cJSON_AddItemToArray(items, item);
    }

    char *printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        snprintf(err, err_size, "cannot render snapshot json");
        return -1;
    }

    int rc = write_bytes_atomic(path, printed, err, err_size);
    free(printed);
    return rc;
}

static int write_text_snapshot(const pm_context *ctx,
                               const char *mode,
                               const pm_env_snapshot_entry *entries,
                               const char *path,
                               char *err,
                               size_t err_size)
{
    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0) {
        snprintf(err, err_size, "snapshot tmp path too long");
        return -1;
    }

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        snprintf(err, err_size, "cannot write %s", tmp);
        return -1;
    }

    time_t now = time(NULL);
    fprintf(fp, "schema=1\n");
    fprintf(fp, "kind=portmaster-launch-env\n");
    fprintf(fp, "mode=%s\n", mode && mode[0] ? mode : "latest");
    fprintf(fp, "generated_at_unix=%lld\n", (long long)now);
    fprintf(fp, "manager_version=%s\n", PM_VERSION);
    fprintf(fp, "platform=%s\n", ctx->platform);
    fprintf(fp, "sdcard_path=%s\n", ctx->sdcard_path);
    fprintf(fp, "\n# name\tsource\tvalue\n");
    for (size_t i = 0; entries && entries[i].name; i++) {
        fprintf(fp, "%s\t%s\t%s\n",
                entries[i].name,
                entries[i].source ? entries[i].source : "manager",
                entries[i].value ? entries[i].value : "");
    }

    if (fclose(fp) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot close %s", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, err_size, "cannot replace %s", path);
        return -1;
    }
    return 0;
}

static int write_snapshot_pair(const pm_context *ctx,
                               const char *mode,
                               const pm_env_snapshot_entry *entries,
                               char *err,
                               size_t err_size)
{
    char json_path[PM_PATH_MAX];
    char text_path[PM_PATH_MAX];
    char json_prev[PM_PATH_MAX];
    char text_prev[PM_PATH_MAX];
    if (pm_env_snapshot_path(ctx, mode, "json", json_path, sizeof(json_path)) != 0 ||
        pm_env_snapshot_path(ctx, mode, "txt", text_path, sizeof(text_path)) != 0 ||
        previous_snapshot_path(ctx, mode, "json", json_prev, sizeof(json_prev)) != 0 ||
        previous_snapshot_path(ctx, mode, "txt", text_prev, sizeof(text_prev)) != 0) {
        snprintf(err, err_size, "snapshot path too long");
        return -1;
    }

    rotate_previous(json_path, json_prev);
    rotate_previous(text_path, text_prev);
    if (write_json_snapshot(ctx, mode, entries, json_path, err, err_size) != 0 ||
        write_text_snapshot(ctx, mode, entries, text_path, err, err_size) != 0) {
        return -1;
    }
    return 0;
}

int pm_env_snapshot_write(const pm_context *ctx,
                          const char *mode,
                          const pm_env_snapshot_entry *entries,
                          char *err,
                          size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!ctx) {
        snprintf(err, err_size, "missing context");
        return -1;
    }
    if (pm_context_ensure_manager_dirs(ctx, err, err_size) != 0) {
        return -1;
    }

    char slug[64];
    snapshot_mode_slug(mode, slug, sizeof(slug));
    if (write_snapshot_pair(ctx, slug, entries, err, err_size) != 0) {
        return -1;
    }
    return write_snapshot_pair(ctx, "latest", entries, err, err_size);
}
