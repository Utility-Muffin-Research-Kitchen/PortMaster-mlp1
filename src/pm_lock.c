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

