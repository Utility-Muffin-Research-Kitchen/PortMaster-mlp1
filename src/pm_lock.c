#include "pm_lock.h"

#include "pm_util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return -1;
    }
    return pm_copy(dst, dst_size, item->valuestring);
}

int pm_lock_load(const char *path, pm_portmaster_lock *out, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char read_err[256];
    char *text = pm_read_text_file(path, 128 * 1024, read_err, sizeof(read_err));
    if (!text) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", read_err);
        }
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "invalid JSON in %s", path);
        }
        return -1;
    }

    int ok = 0;
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (json_string(root, "product", out->product, sizeof(out->product)) != 0 ||
        json_string(root, "channel", out->channel, sizeof(out->channel)) != 0 ||
        json_string(root, "repo", out->repo, sizeof(out->repo)) != 0 ||
        json_string(root, "tag", out->tag, sizeof(out->tag)) != 0 ||
        json_string(root, "published_at", out->published_at, sizeof(out->published_at)) != 0 ||
        json_string(root, "asset", out->asset, sizeof(out->asset)) != 0 ||
        json_string(root, "url", out->url, sizeof(out->url)) != 0 ||
        json_string(root, "release_url", out->release_url, sizeof(out->release_url)) != 0 ||
        json_string(root, "md5", out->md5, sizeof(out->md5)) != 0 ||
        json_string(root, "sha256", out->sha256, sizeof(out->sha256)) != 0 ||
        json_string(root, "checked_at", out->checked_at, sizeof(out->checked_at)) != 0 ||
        !cJSON_IsNumber(size) || size->valuedouble <= 0.0) {
        ok = -1;
    } else {
        out->size = (uint64_t)size->valuedouble;
    }

    cJSON_Delete(root);
    if (ok != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "missing required lock fields in %s", path);
        }
        return -1;
    }
    return 0;
}

void pm_lock_summary(const pm_portmaster_lock *lock, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!lock) {
        snprintf(out, out_size, "%s", "No lock loaded.");
        return;
    }
    snprintf(out, out_size,
             "Channel: %s\nTag: %s\nAsset: %s\nSize: %llu bytes\nSHA-256: %.16s...\nChecked: %s\n\n%s",
             lock->channel,
             lock->tag,
             lock->asset,
             (unsigned long long)lock->size,
             lock->sha256,
             lock->checked_at,
             lock->release_url);
}

int pm_ui_runtime_lock_load(const char *path, pm_ui_runtime_lock *out, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char read_err[256];
    char *text = pm_read_text_file(path, 256 * 1024, read_err, sizeof(read_err));
    if (!text) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", read_err);
        }
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "invalid JSON in %s", path);
        }
        return -1;
    }

    int ok = -1;
    cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(root, "artifacts");
    if (json_string(root, "product", out->product, sizeof(out->product)) != 0 ||
        !cJSON_IsArray(artifacts)) {
        goto done;
    }

    cJSON *artifact = NULL;
    cJSON_ArrayForEach(artifact, artifacts) {
        if (!cJSON_IsObject(artifact)) {
            continue;
        }
        const char *kind = json_string(artifact, "kind", out->kind, sizeof(out->kind)) == 0 ?
            out->kind : "";
        if (strcmp(kind, "cpython-runtime") != 0) {
            continue;
        }

        cJSON *size = cJSON_GetObjectItemCaseSensitive(artifact, "size");
        cJSON *installed_size = cJSON_GetObjectItemCaseSensitive(artifact, "installed_size");
        cJSON *installed_file_count = cJSON_GetObjectItemCaseSensitive(artifact, "installed_file_count");
        cJSON *manifest = cJSON_GetObjectItemCaseSensitive(artifact, "manifest");
        if (json_string(artifact, "version", out->version, sizeof(out->version)) != 0 ||
            json_string(artifact, "filename", out->filename, sizeof(out->filename)) != 0 ||
            json_string(artifact, "url", out->url, sizeof(out->url)) != 0 ||
            json_string(artifact, "sha256", out->sha256, sizeof(out->sha256)) != 0 ||
            !cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
            !cJSON_IsNumber(installed_size) || installed_size->valuedouble <= 0.0 ||
            !cJSON_IsNumber(installed_file_count) || installed_file_count->valuedouble <= 0.0 ||
            !cJSON_IsObject(manifest)) {
            goto done;
        }

        cJSON *manifest_size = cJSON_GetObjectItemCaseSensitive(manifest, "size");
        if (json_string(manifest, "filename", out->manifest_filename, sizeof(out->manifest_filename)) != 0 ||
            json_string(manifest, "url", out->manifest_url, sizeof(out->manifest_url)) != 0 ||
            json_string(manifest, "sha256", out->manifest_sha256, sizeof(out->manifest_sha256)) != 0 ||
            !cJSON_IsNumber(manifest_size) || manifest_size->valuedouble <= 0.0) {
            goto done;
        }

        out->size = (uint64_t)size->valuedouble;
        out->installed_size = (uint64_t)installed_size->valuedouble;
        out->installed_file_count = installed_file_count->valueint;
        out->manifest_size = (uint64_t)manifest_size->valuedouble;
        ok = 0;
        break;
    }

done:
    cJSON_Delete(root);
    if (ok != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "missing required UI runtime lock fields in %s", path);
        }
        return -1;
    }
    return 0;
}

void pm_ui_runtime_lock_summary(const pm_ui_runtime_lock *lock, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!lock) {
        snprintf(out, out_size, "%s", "No UI runtime lock loaded.");
        return;
    }
    snprintf(out, out_size,
             "Kind: %s\nVersion: %s\nAsset: %s\nSize: %llu bytes\nInstalled: %d files, %llu bytes\nSHA-256: %.16s...\n\n%s",
             lock->kind,
             lock->version,
             lock->filename,
             (unsigned long long)lock->size,
             lock->installed_file_count,
             (unsigned long long)lock->installed_size,
             lock->sha256,
             lock->url);
}

int pm_armhf_compat_lock_load(const char *path, pm_armhf_compat_lock *out,
                              char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char read_err[256];
    char *text = pm_read_text_file(path, 512 * 1024, read_err, sizeof(read_err));
    if (!text) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", read_err);
        }
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "invalid JSON in %s", path);
        }
        return -1;
    }

    int ok = -1;
    cJSON *builder = cJSON_GetObjectItemCaseSensitive(root, "builder");
    cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(root, "artifacts");
    if (json_string(root, "status", out->status, sizeof(out->status)) != 0 ||
        !cJSON_IsObject(builder) ||
        json_string(builder, "version", out->version, sizeof(out->version)) != 0 ||
        !cJSON_IsArray(artifacts) || cJSON_GetArraySize(artifacts) != 1) {
        goto done;
    }

    cJSON *artifact = cJSON_GetArrayItem(artifacts, 0);
    cJSON *size = cJSON_GetObjectItemCaseSensitive(artifact, "size");
    cJSON *manifest = cJSON_GetObjectItemCaseSensitive(artifact, "manifest");
    if (!cJSON_IsObject(artifact) ||
        json_string(artifact, "name", out->filename, sizeof(out->filename)) != 0 ||
        json_string(artifact, "url", out->url, sizeof(out->url)) != 0 ||
        json_string(artifact, "sha256", out->sha256, sizeof(out->sha256)) != 0 ||
        !cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
        !cJSON_IsObject(manifest)) {
        goto done;
    }

    cJSON *manifest_size = cJSON_GetObjectItemCaseSensitive(manifest, "size");
    if (json_string(manifest, "filename", out->manifest_filename,
                    sizeof(out->manifest_filename)) != 0 ||
        json_string(manifest, "url", out->manifest_url,
                    sizeof(out->manifest_url)) != 0 ||
        json_string(manifest, "sha256", out->manifest_sha256,
                    sizeof(out->manifest_sha256)) != 0 ||
        !cJSON_IsNumber(manifest_size) || manifest_size->valuedouble <= 0.0) {
        goto done;
    }

    out->size = (uint64_t)size->valuedouble;
    out->manifest_size = (uint64_t)manifest_size->valuedouble;
    ok = 0;

done:
    cJSON_Delete(root);
    if (ok != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size,
                     "missing required armhf compatibility lock fields in %s", path);
        }
        return -1;
    }
    return 0;
}

void pm_armhf_compat_lock_summary(const pm_armhf_compat_lock *lock,
                                  char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!lock) {
        snprintf(out, out_size, "%s", "No armhf compatibility lock loaded.");
        return;
    }
    snprintf(out, out_size,
             "Version: %s\nAsset: %s\nSize: %llu bytes\nSHA-256: %.16s...\n\n%s",
             lock->version,
             lock->filename,
             (unsigned long long)lock->size,
             lock->sha256,
             lock->url);
}
