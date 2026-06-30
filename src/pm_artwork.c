#include "pm_artwork.h"

#include "cJSON.h"
#include "pm_util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    bool found;
    char dir_name[256];
    char slug[256];
    char screenshot[256];
} pm_artwork_meta;

static bool pm__has_suffix_casefold(const char *value, const char *suffix)
{
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcasecmp(value + value_len - suffix_len, suffix) == 0;
}

static const char *pm__basename_const(const char *path)
{
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static int pm__stem_from_script(const char *script_name, char *out, size_t out_size)
{
    if (pm_copy(out, out_size, script_name) != 0) {
        return -1;
    }
    char *dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".sh") == 0) {
        *dot = '\0';
    }
    return 0;
}

static void pm__lower_copy(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1u < out_size; i++) {
            out[i] = (char)tolower((unsigned char)src[i]);
        }
    }
    out[i] = '\0';
}

static void pm__slug_from_archive_name(const char *name, char *out, size_t out_size)
{
    char tmp[256];
    pm_copy(tmp, sizeof(tmp), pm__basename_const(name));
    if (pm__has_suffix_casefold(tmp, ".zip")) {
        tmp[strlen(tmp) - 4] = '\0';
    }
    pm__lower_copy(tmp, out, out_size);
}

static bool pm__safe_relative_path(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\') {
            p++;
        }
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return false;
        }
    }
    return true;
}

static bool pm__json_string_matches_script(const cJSON *item, const char *script_name)
{
    if (!cJSON_IsString(item) || !item->valuestring || !script_name) {
        return false;
    }
    return strcasecmp(pm__basename_const(item->valuestring), script_name) == 0;
}

static bool pm__json_array_has_script(const cJSON *array, const char *script_name)
{
    if (!cJSON_IsArray(array)) {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (pm__json_string_matches_script(item, script_name)) {
            return true;
        }
    }
    return false;
}

static bool pm__json_object_has_script(const cJSON *object, const char *script_name)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if ((item->string && strcasecmp(pm__basename_const(item->string), script_name) == 0) ||
            pm__json_string_matches_script(item, script_name)) {
            return true;
        }
    }
    return false;
}

static bool pm__read_meta_file(const char *path,
                               const char *dir_name,
                               const char *script_name,
                               bool allow_unmatched,
                               pm_artwork_meta *out)
{
    char json_err[256];
    char *text = pm_read_text_file(path, 256 * 1024, json_err, sizeof(json_err));
    if (!text) {
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return false;
    }

    bool matches = pm__json_array_has_script(cJSON_GetObjectItemCaseSensitive(root, "items"),
                                             script_name) ||
                   pm__json_object_has_script(cJSON_GetObjectItemCaseSensitive(root, "files"),
                                              script_name);
    if (!matches && !allow_unmatched) {
        cJSON_Delete(root);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->found = true;
    pm_copy(out->dir_name, sizeof(out->dir_name), dir_name);

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) {
        pm__slug_from_archive_name(name->valuestring, out->slug, sizeof(out->slug));
    } else {
        pm__lower_copy(dir_name, out->slug, sizeof(out->slug));
    }

    const cJSON *attr = cJSON_GetObjectItemCaseSensitive(root, "attr");
    const cJSON *image = cJSON_IsObject(attr) ? cJSON_GetObjectItemCaseSensitive(attr, "image") : NULL;
    const cJSON *screenshot = cJSON_IsObject(image)
        ? cJSON_GetObjectItemCaseSensitive(image, "screenshot")
        : NULL;
    if (cJSON_IsString(screenshot) && screenshot->valuestring &&
        pm__safe_relative_path(screenshot->valuestring)) {
        pm_copy(out->screenshot, sizeof(out->screenshot), screenshot->valuestring);
    }

    cJSON_Delete(root);
    return true;
}

static bool pm__load_meta_from_dir(const pm_context *ctx,
                                   const char *dir_name,
                                   const char *script_name,
                                   bool allow_unmatched,
                                   pm_artwork_meta *out)
{
    char path[PM_PATH_MAX];
    if (pm_join3(path, sizeof(path), ctx->ports_dir, dir_name, "port.json") != 0 ||
        !pm_file_exists(path)) {
        return false;
    }
    return pm__read_meta_file(path, dir_name, script_name, allow_unmatched, out);
}

static bool pm__find_meta_for_script(const pm_context *ctx,
                                     const char *script_name,
                                     const char *stem,
                                     pm_artwork_meta *out)
{
    char lower_stem[256];
    pm__lower_copy(stem, lower_stem, sizeof(lower_stem));

    if (pm__load_meta_from_dir(ctx, stem, script_name, true, out)) {
        return true;
    }
    if (strcasecmp(stem, lower_stem) != 0 &&
        pm__load_meta_from_dir(ctx, lower_stem, script_name, true, out)) {
        return true;
    }

    DIR *dir = opendir(ctx->ports_dir);
    if (!dir) {
        return false;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char child[PM_PATH_MAX];
        if (pm_join(child, sizeof(child), ctx->ports_dir, ent->d_name) != 0 ||
            !pm_dir_exists(child)) {
            continue;
        }
        if (pm__load_meta_from_dir(ctx, ent->d_name, script_name, false, out)) {
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static bool pm__first_existing(const char *dir,
                               const char *base,
                               const char *const *suffixes,
                               size_t suffix_count,
                               char *out,
                               size_t out_size)
{
    for (size_t i = 0; i < suffix_count; i++) {
        char name[512];
        if (pm_format(name, sizeof(name), "%s%s", base, suffixes[i]) != 0 ||
            pm_join(out, out_size, dir, name) != 0) {
            continue;
        }
        if (pm_file_exists(out) && pm_file_size(out) > 0) {
            return true;
        }
    }
    return false;
}

static bool pm__find_cache_source(const pm_context *ctx,
                                  const pm_artwork_meta *meta,
                                  const char *stem,
                                  char *out,
                                  size_t out_size)
{
    char cache_dir[PM_PATH_MAX];
    if (pm_join3(cache_dir, sizeof(cache_dir), ctx->portmaster_dir,
                 "config", "images_pm") != 0) {
        return false;
    }

    char lower_stem[256];
    pm__lower_copy(stem, lower_stem, sizeof(lower_stem));

    const char *suffixes[] = {
        ".screenshot.png",
        ".screenshot.jpg",
        ".screenshot.jpeg",
    };

    if (meta && meta->found && meta->slug[0] &&
        pm__first_existing(cache_dir, meta->slug, suffixes,
                           sizeof(suffixes) / sizeof(suffixes[0]),
                           out, out_size)) {
        return true;
    }
    if (lower_stem[0] &&
        (!meta || strcasecmp(meta->slug, lower_stem) != 0) &&
        pm__first_existing(cache_dir, lower_stem, suffixes,
                           sizeof(suffixes) / sizeof(suffixes[0]),
                           out, out_size)) {
        return true;
    }
    return false;
}

static bool pm__find_named_installed_source(const char *port_dir,
                                            const char *name,
                                            char *out,
                                            size_t out_size)
{
    const char *exts[] = { ".png", ".jpg", ".jpeg" };
    return pm__first_existing(port_dir, name, exts, sizeof(exts) / sizeof(exts[0]),
                              out, out_size);
}

static bool pm__find_installed_source(const pm_context *ctx,
                                      const pm_artwork_meta *meta,
                                      const char *stem,
                                      char *out,
                                      size_t out_size)
{
    char lower_stem[256];
    pm__lower_copy(stem, lower_stem, sizeof(lower_stem));

    const char *dirs[3] = { NULL, NULL, NULL };
    int dir_count = 0;
    if (meta && meta->found && meta->dir_name[0]) {
        dirs[dir_count++] = meta->dir_name;
    }
    if (stem && stem[0] && (!meta || strcasecmp(meta->dir_name, stem) != 0)) {
        dirs[dir_count++] = stem;
    }
    if (lower_stem[0] && strcasecmp(stem, lower_stem) != 0 &&
        (!meta || strcasecmp(meta->dir_name, lower_stem) != 0)) {
        dirs[dir_count++] = lower_stem;
    }

    for (int i = 0; i < dir_count; i++) {
        char port_dir[PM_PATH_MAX];
        if (pm_join(port_dir, sizeof(port_dir), ctx->ports_dir, dirs[i]) != 0 ||
            !pm_dir_exists(port_dir)) {
            continue;
        }
        if (meta && meta->found && meta->screenshot[0]) {
            char screenshot[PM_PATH_MAX];
            if (pm_join(screenshot, sizeof(screenshot), port_dir, meta->screenshot) == 0 &&
                pm_file_exists(screenshot) && pm_file_size(screenshot) > 0) {
                pm_copy(out, out_size, screenshot);
                return true;
            }
        }
        const char *names[] = { "cover", "screenshot", "splash" };
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            if (pm__find_named_installed_source(port_dir, names[n], out, out_size)) {
                return true;
            }
        }
    }
    return false;
}

static int pm__copy_file_atomic(const char *src, const char *dst, char *err, size_t err_size)
{
    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.partial", dst) != 0) {
        snprintf(err, err_size, "target path too long");
        return -1;
    }

    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(err, err_size, "cannot open artwork source: %s", strerror(errno));
        return -1;
    }
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        snprintf(err, err_size, "cannot create artwork temp: %s", strerror(errno));
        fclose(in);
        return -1;
    }

    char buf[64 * 1024];
    int rc = 0;
    while (true) {
        size_t got = fread(buf, 1, sizeof(buf), in);
        if (got > 0 && fwrite(buf, 1, got, out) != got) {
            snprintf(err, err_size, "cannot write artwork temp: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (got < sizeof(buf)) {
            if (ferror(in)) {
                snprintf(err, err_size, "cannot read artwork source: %s", strerror(errno));
                rc = -1;
            }
            break;
        }
    }

    if (fclose(out) != 0 && rc == 0) {
        snprintf(err, err_size, "cannot close artwork temp: %s", strerror(errno));
        rc = -1;
    }
    fclose(in);

    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        snprintf(err, err_size, "cannot promote artwork temp: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int pm__convert_image_to_png_atomic(const char *src,
                                           const char *dst,
                                           char *err,
                                           size_t err_size)
{
    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.partial", dst) != 0) {
        snprintf(err, err_size, "target path too long");
        return -1;
    }

    if (SDL_Init(0) != 0) {
        snprintf(err, err_size, "SDL init failed: %s", SDL_GetError());
        return -1;
    }

    int flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int ready = IMG_Init(flags);
    if ((ready & IMG_INIT_PNG) == 0 ||
        ((pm__has_suffix_casefold(src, ".jpg") || pm__has_suffix_casefold(src, ".jpeg")) &&
         (ready & IMG_INIT_JPG) == 0)) {
        snprintf(err, err_size, "SDL_image init failed: %s", IMG_GetError());
        return -1;
    }

    SDL_Surface *surface = IMG_Load(src);
    if (!surface) {
        snprintf(err, err_size, "cannot load artwork source: %s", IMG_GetError());
        return -1;
    }
    int save_rc = IMG_SavePNG(surface, tmp);
    SDL_FreeSurface(surface);
    if (save_rc != 0) {
        snprintf(err, err_size, "cannot write artwork PNG: %s", IMG_GetError());
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        snprintf(err, err_size, "cannot promote artwork PNG: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int pm__write_png_art(const char *src, const char *dst, char *err, size_t err_size)
{
    if (pm__has_suffix_casefold(src, ".png")) {
        return pm__copy_file_atomic(src, dst, err, err_size);
    }
    return pm__convert_image_to_png_atomic(src, dst, err, err_size);
}

static int pm__sync_one_script(const pm_context *ctx,
                               const char *script_name,
                               pm_artwork_sync_result *out)
{
    char stem[256];
    if (pm__stem_from_script(script_name, stem, sizeof(stem)) != 0 || !stem[0]) {
        out->failed++;
        return -1;
    }

    char target_name[512];
    char target[PM_PATH_MAX];
    if (pm_format(target_name, sizeof(target_name), "%s.png", stem) != 0 ||
        pm_join(target, sizeof(target), ctx->port_images_dir, target_name) != 0) {
        out->failed++;
        return -1;
    }

    if (pm_file_exists(target) && pm_file_size(target) > 0) {
        out->skipped_existing++;
        return 0;
    }

    pm_artwork_meta meta;
    memset(&meta, 0, sizeof(meta));
    (void)pm__find_meta_for_script(ctx, script_name, stem, &meta);

    char source[PM_PATH_MAX];
    if (!pm__find_cache_source(ctx, &meta, stem, source, sizeof(source)) &&
        !pm__find_installed_source(ctx, &meta, stem, source, sizeof(source))) {
        out->missing_source++;
        return 0;
    }

    char item_err[512];
    if (pm__write_png_art(source, target, item_err, sizeof(item_err)) != 0) {
        out->failed++;
        fprintf(stderr, "PortMaster artwork sync warning: %s -> %s: %s\n",
                source, target, item_err);
        return -1;
    }

    out->synced++;
    return 0;
}

int pm_artwork_sync(const pm_context *ctx,
                    pm_artwork_sync_result *out,
                    char *err,
                    size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!ctx || !out) {
        snprintf(err, err_size, "missing artwork sync context");
        return -1;
    }

    if (pm_mkdir_p(ctx->port_images_dir, err, err_size) != 0) {
        return -1;
    }

    DIR *dir = opendir(ctx->ports_dir);
    if (!dir) {
        if (errno == ENOENT) {
            return 0;
        }
        snprintf(err, err_size, "cannot open %s: %s", ctx->ports_dir, strerror(errno));
        return -1;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || !pm__has_suffix_casefold(ent->d_name, ".sh")) {
            continue;
        }
        char script_path[PM_PATH_MAX];
        if (pm_join(script_path, sizeof(script_path), ctx->ports_dir, ent->d_name) != 0 ||
            !pm_file_exists(script_path)) {
            continue;
        }
        out->scanned++;
        (void)pm__sync_one_script(ctx, ent->d_name, out);
    }

    closedir(dir);
    return 0;
}
