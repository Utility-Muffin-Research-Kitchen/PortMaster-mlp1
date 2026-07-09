#include "pm_self_heal.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PM_SELF_HEAL_JSON_MAX_BYTES (2 * 1024 * 1024)

static void pm_self_heal_timespecs(const struct stat *st, struct timespec times[2])
{
#if defined(__APPLE__)
    times[0] = st->st_atimespec;
    times[1] = st->st_mtimespec;
#else
    times[0] = st->st_atim;
    times[1] = st->st_mtim;
#endif
}

static bool pm_self_heal_path_under(const char *path, const char *root)
{
    if (!path || !path[0] || !root || !root[0]) {
        return false;
    }
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') {
        root_len--;
    }
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static int pm_self_heal_platform_root(const pm_context *ctx,
                                      char *platform_root,
                                      size_t platform_root_size,
                                      char *detail,
                                      size_t detail_size)
{
    const char *env_platform = getenv("UMRK_PLATFORM_PATH");
    if (env_platform && env_platform[0] &&
        pm_self_heal_path_under(env_platform, ctx->sdcard_path)) {
        if (pm_copy(platform_root, platform_root_size, env_platform) != 0) {
            snprintf(detail, detail_size, "%s", "UMRK_PLATFORM_PATH is too long");
            return -1;
        }
        return 0;
    }

    if (pm_format(platform_root, platform_root_size,
                  "%s/.system/leaf/platforms/%s",
                  ctx->sdcard_path, ctx->platform) != 0) {
        snprintf(detail, detail_size, "%s", "Leaf platform path too long");
        return -1;
    }
    return 0;
}

static int pm_self_heal_stamp_times(const char *path, const struct stat *src_st)
{
    struct timespec times[2];
    pm_self_heal_timespecs(src_st, times);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    int rc = futimens(fd, times);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return rc;
}

static int pm_self_heal_files_equal(const char *src,
                                    const struct stat *src_st,
                                    const char *dst,
                                    const struct stat *dst_st,
                                    bool *equal,
                                    char *detail,
                                    size_t detail_size)
{
    if (!src || !src_st || !dst || !dst_st || !equal) {
        snprintf(detail, detail_size, "%s", "invalid self-heal comparison");
        return -1;
    }

    *equal = false;
    if (src_st->st_size != dst_st->st_size) {
        return 0;
    }

    FILE *src_fp = fopen(src, "rb");
    if (!src_fp) {
        snprintf(detail, detail_size, "cannot open %s: %s", src, strerror(errno));
        return -1;
    }
    FILE *dst_fp = fopen(dst, "rb");
    if (!dst_fp) {
        int saved_errno = errno;
        fclose(src_fp);
        snprintf(detail, detail_size, "cannot open %s: %s", dst, strerror(saved_errno));
        return -1;
    }

    unsigned char src_buf[64 * 1024];
    unsigned char dst_buf[64 * 1024];
    bool same = true;
    bool read_failed = false;
    for (;;) {
        size_t src_count = fread(src_buf, 1, sizeof(src_buf), src_fp);
        size_t dst_count = fread(dst_buf, 1, sizeof(dst_buf), dst_fp);
        if (src_count != dst_count ||
            (src_count > 0 && memcmp(src_buf, dst_buf, src_count) != 0)) {
            same = false;
            break;
        }
        if (src_count == 0) {
            break;
        }
    }
    if (ferror(src_fp) || ferror(dst_fp)) {
        read_failed = true;
        snprintf(detail, detail_size, "cannot compare %s and %s", src, dst);
    }

    int src_close = fclose(src_fp);
    int dst_close = fclose(dst_fp);
    if (src_close != 0 || dst_close != 0) {
        snprintf(detail, detail_size, "cannot close comparison files for %s", dst);
        return -1;
    }
    if (read_failed) {
        return -1;
    }

    *equal = same;
    return 0;
}

static bool pm_json_string_matches(cJSON *object, const char *key, const char *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, value) == 0;
}

static bool pm_json_null_matches(cJSON *object, const char *key)
{
    return cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(object, key));
}

static bool pm_json_bool_matches(cJSON *object, const char *key, bool value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) &&
           ((value && cJSON_IsTrue(item)) || (!value && cJSON_IsFalse(item)));
}

static bool pm_json_string_array_matches(cJSON *object,
                                         const char *key,
                                         const char *const *values,
                                         size_t count)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(array, (int)i);
        if (!cJSON_IsString(item) || !item->valuestring ||
            strcmp(item->valuestring, values[i]) != 0) {
            return false;
        }
    }
    return true;
}

static int pm_json_set_item(cJSON *object, const char *key, cJSON *item)
{
    if (!item) {
        return -1;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    if (!cJSON_AddItemToObject(object, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int pm_json_set_string(cJSON *object, const char *key, const char *value)
{
    return pm_json_set_item(object, key, cJSON_CreateString(value));
}

static int pm_json_set_null(cJSON *object, const char *key)
{
    return pm_json_set_item(object, key, cJSON_CreateNull());
}

static int pm_json_set_bool(cJSON *object, const char *key, bool value)
{
    return pm_json_set_item(object, key, cJSON_CreateBool(value ? 1 : 0));
}

static int pm_json_set_string_array(cJSON *object,
                                    const char *key,
                                    const char *const *values,
                                    size_t count)
{
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateString(values[i]);
        if (!item || !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return -1;
        }
    }
    return pm_json_set_item(object, key, array);
}

static cJSON *pm_json_find_object_by_id(cJSON *array, const char *id)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        if (pm_json_string_matches(item, "id", id)) {
            return item;
        }
    }
    return NULL;
}

static bool pm_json_ports_core_matches(cJSON *core, const char *platform)
{
    const char *platforms[] = { platform };
    return pm_json_string_matches(core, "id", "ports") &&
           pm_json_string_matches(core, "display_name", "Ports") &&
           pm_json_string_matches(core, "type", "path") &&
           pm_json_null_matches(core, "libretro_name") &&
           pm_json_null_matches(core, "file_name") &&
           pm_json_null_matches(core, "config_folder") &&
           pm_json_null_matches(core, "info_name") &&
           pm_json_string_matches(core, "path", "emulators/ports/launch.sh") &&
           pm_json_bool_matches(core, "supports_menu", false) &&
           pm_json_bool_matches(core, "supports_savestate", false) &&
           pm_json_bool_matches(core, "supports_disk_control", false) &&
           pm_json_bool_matches(core, "needs_swap", false) &&
           pm_json_string_array_matches(core, "platforms", platforms, 1) &&
           pm_json_string_matches(core, "status", "packaged");
}

static int pm_json_set_ports_core(cJSON *core, const char *platform)
{
    const char *platforms[] = { platform };
    return pm_json_set_string(core, "id", "ports") ||
           pm_json_set_string(core, "display_name", "Ports") ||
           pm_json_set_string(core, "type", "path") ||
           pm_json_set_null(core, "libretro_name") ||
           pm_json_set_null(core, "file_name") ||
           pm_json_set_null(core, "config_folder") ||
           pm_json_set_null(core, "info_name") ||
           pm_json_set_string(core, "path", "emulators/ports/launch.sh") ||
           pm_json_set_bool(core, "supports_menu", false) ||
           pm_json_set_bool(core, "supports_savestate", false) ||
           pm_json_set_bool(core, "supports_disk_control", false) ||
           pm_json_set_bool(core, "needs_swap", false) ||
           pm_json_set_string_array(core, "platforms", platforms, 1) ||
           pm_json_set_string(core, "status", "packaged");
}

static bool pm_json_ports_system_matches(cJSON *system)
{
    const char *patterns[] = { "PORTS", "ports" };
    const char *extensions[] = { "sh" };
    return pm_json_string_matches(system, "id", "PORTS") &&
           pm_json_string_matches(system, "name", "Ports") &&
           pm_json_string_array_matches(system, "patterns", patterns, 2) &&
           pm_json_string_array_matches(system, "extensions", extensions, 1) &&
           pm_json_string_array_matches(system, "archive_extensions", NULL, 0) &&
           pm_json_string_array_matches(system, "archive_inner_extensions", NULL, 0) &&
           pm_json_string_matches(system, "archive_mode", "pass_through") &&
           pm_json_string_array_matches(system, "file_names", NULL, 0) &&
           pm_json_string_array_matches(system, "ignore_file_names", NULL, 0) &&
           pm_json_string_array_matches(system, "playlist_extensions", NULL, 0) &&
           pm_json_string_matches(system, "m3u_generation", "none") &&
           pm_json_string_matches(system, "default_core", "ports") &&
           pm_json_string_array_matches(system, "alternate_cores", NULL, 0) &&
           pm_json_string_matches(system, "rom_root", "Roms/PORTS") &&
           pm_json_string_matches(system, "image_root", "Images/PORTS") &&
           pm_json_string_array_matches(system, "bios_notes", NULL, 0);
}

static int pm_json_set_ports_system(cJSON *system)
{
    const char *patterns[] = { "PORTS", "ports" };
    const char *extensions[] = { "sh" };
    return pm_json_set_string(system, "id", "PORTS") ||
           pm_json_set_string(system, "name", "Ports") ||
           pm_json_set_string_array(system, "patterns", patterns, 2) ||
           pm_json_set_string_array(system, "extensions", extensions, 1) ||
           pm_json_set_string_array(system, "archive_extensions", NULL, 0) ||
           pm_json_set_string_array(system, "archive_inner_extensions", NULL, 0) ||
           pm_json_set_string(system, "archive_mode", "pass_through") ||
           pm_json_set_string_array(system, "file_names", NULL, 0) ||
           pm_json_set_string_array(system, "ignore_file_names", NULL, 0) ||
           pm_json_set_string_array(system, "playlist_extensions", NULL, 0) ||
           pm_json_set_string(system, "m3u_generation", "none") ||
           pm_json_set_string(system, "default_core", "ports") ||
           pm_json_set_string_array(system, "alternate_cores", NULL, 0) ||
           pm_json_set_string(system, "rom_root", "Roms/PORTS") ||
           pm_json_set_string(system, "image_root", "Images/PORTS") ||
           pm_json_set_string_array(system, "bios_notes", NULL, 0);
}

static int pm_self_heal_write_json_atomic(const char *path,
                                          cJSON *root,
                                          char *detail,
                                          size_t detail_size)
{
    struct stat st;
    mode_t mode = 0644;
    if (stat(path, &st) == 0) {
        mode = st.st_mode & 0777;
    }

    char *printed = cJSON_Print(root);
    if (!printed) {
        snprintf(detail, detail_size, "cannot render %s", path);
        return -1;
    }

    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) != 0) {
        free(printed);
        snprintf(detail, detail_size, "%s", "Leaf defaults path too long");
        return -1;
    }

    FILE *out = fopen(tmp, "wb");
    if (!out) {
        free(printed);
        snprintf(detail, detail_size, "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }

    size_t len = strlen(printed);
    bool ok = fwrite(printed, 1, len, out) == len;
    if (ok && (len == 0 || printed[len - 1] != '\n')) {
        ok = fputc('\n', out) != EOF;
    }
    free(printed);

    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
        snprintf(detail, detail_size, "cannot write %s", tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        int saved_errno = errno;
        unlink(tmp);
        snprintf(detail, detail_size, "cannot promote %s: %s",
                 path, strerror(saved_errno));
        return -1;
    }
    chmod(path, mode);
    return 0;
}

static int pm_self_heal_patch_ports_core(const char *path,
                                         const char *platform,
                                         bool *changed,
                                         char *detail,
                                         size_t detail_size)
{
    *changed = false;

    char read_err[128];
    char *text = pm_read_text_file(path, PM_SELF_HEAL_JSON_MAX_BYTES,
                                   read_err, sizeof(read_err));
    if (!text) {
        snprintf(detail, detail_size, "%s", read_err[0] ? read_err : path);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        snprintf(detail, detail_size, "cannot parse %s", path);
        return -1;
    }

    cJSON *cores = cJSON_GetObjectItemCaseSensitive(root, "cores");
    if (!cJSON_IsArray(cores)) {
        cJSON_Delete(root);
        snprintf(detail, detail_size, "missing cores array in %s", path);
        return -1;
    }

    cJSON *core = pm_json_find_object_by_id(cores, "ports");
    if (!core) {
        core = cJSON_CreateObject();
        if (!core || pm_json_set_ports_core(core, platform) != 0 ||
            !cJSON_AddItemToArray(cores, core)) {
            cJSON_Delete(core);
            cJSON_Delete(root);
            snprintf(detail, detail_size, "cannot add ports core to %s", path);
            return -1;
        }
        *changed = true;
    } else if (!pm_json_ports_core_matches(core, platform)) {
        if (pm_json_set_ports_core(core, platform) != 0) {
            cJSON_Delete(root);
            snprintf(detail, detail_size, "cannot update ports core in %s", path);
            return -1;
        }
        *changed = true;
    }

    if (*changed && pm_self_heal_write_json_atomic(path, root, detail, detail_size) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}

static int pm_self_heal_patch_ports_system(const char *path,
                                           bool *changed,
                                           char *detail,
                                           size_t detail_size)
{
    *changed = false;

    char read_err[128];
    char *text = pm_read_text_file(path, PM_SELF_HEAL_JSON_MAX_BYTES,
                                   read_err, sizeof(read_err));
    if (!text) {
        snprintf(detail, detail_size, "%s", read_err[0] ? read_err : path);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        snprintf(detail, detail_size, "cannot parse %s", path);
        return -1;
    }

    cJSON *systems = cJSON_GetObjectItemCaseSensitive(root, "systems");
    if (!cJSON_IsArray(systems)) {
        cJSON_Delete(root);
        snprintf(detail, detail_size, "missing systems array in %s", path);
        return -1;
    }

    cJSON *system = pm_json_find_object_by_id(systems, "PORTS");
    if (!system) {
        system = cJSON_CreateObject();
        if (!system || pm_json_set_ports_system(system) != 0 ||
            !cJSON_AddItemToArray(systems, system)) {
            cJSON_Delete(system);
            cJSON_Delete(root);
            snprintf(detail, detail_size, "cannot add PORTS system to %s", path);
            return -1;
        }
        *changed = true;
    } else if (!pm_json_ports_system_matches(system)) {
        if (pm_json_set_ports_system(system) != 0) {
            cJSON_Delete(root);
            snprintf(detail, detail_size, "cannot update PORTS system in %s", path);
            return -1;
        }
        *changed = true;
    }

    if (*changed && pm_self_heal_write_json_atomic(path, root, detail, detail_size) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}

static int pm_self_heal_copy_atomic(const char *src,
                                    const char *dst,
                                    const struct stat *src_st,
                                    mode_t dst_mode,
                                    const char *asset_label,
                                    char *detail,
                                    size_t detail_size)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(detail, detail_size, "cannot open packaged %s: %s",
                 asset_label ? asset_label : "asset",
                 strerror(errno));
        return -1;
    }

    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", dst, (long)getpid()) != 0) {
        fclose(in);
        snprintf(detail, detail_size, "Leaf %s path too long",
                 asset_label ? asset_label : "asset");
        return -1;
    }

    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        snprintf(detail, detail_size, "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }

    unsigned char buf[64 * 1024];
    bool ok = true;
    while (!feof(in)) {
        size_t got = fread(buf, 1, sizeof(buf), in);
        if (got > 0 && fwrite(buf, 1, got, out) != got) {
            ok = false;
            break;
        }
        if (ferror(in)) {
            ok = false;
            break;
        }
    }

    if (fclose(in) != 0) {
        ok = false;
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
        snprintf(detail, detail_size, "cannot copy %s to %s", src, dst);
        return -1;
    }

    if (rename(tmp, dst) != 0) {
        int saved_errno = errno;
        unlink(tmp);
        snprintf(detail, detail_size, "cannot promote Leaf %s: %s",
                 asset_label ? asset_label : "asset",
                 strerror(saved_errno));
        return -1;
    }

    chmod(dst, dst_mode);
    if (pm_self_heal_stamp_times(dst, src_st) != 0) {
        snprintf(detail, detail_size,
                 "updated %s but could not preserve timestamp: %s",
                 dst, strerror(errno));
        return 1;
    }

    snprintf(detail, detail_size, "updated %s from packaged %s",
             dst, asset_label ? asset_label : "asset");
    return 1;
}

int pm_self_heal_leaf_ports_launcher(const pm_context *ctx,
                                     char *detail,
                                     size_t detail_size)
{
    if (detail && detail_size > 0) {
        detail[0] = '\0';
    }
    if (!ctx) {
        return 0;
    }

    char src_platform[PM_PATH_MAX];
    char src_ports_dir[PM_PATH_MAX];
    char src[PM_PATH_MAX];
    if (pm_join3(src_platform, sizeof(src_platform),
                 ctx->pak_dir, "leaf-platforms", ctx->platform) != 0 ||
        pm_join3(src_ports_dir, sizeof(src_ports_dir),
                 src_platform, "emulators", "ports") != 0 ||
        pm_join(src, sizeof(src), src_ports_dir, "launch.sh") != 0) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "%s", "packaged ports launcher path too long");
        }
        return -1;
    }

    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        return 0;
    }
    if (!S_ISREG(src_st.st_mode)) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "packaged ports launcher is not a file: %s", src);
        }
        return -1;
    }

    char platform_root[PM_PATH_MAX];
    if (pm_self_heal_platform_root(ctx, platform_root, sizeof(platform_root),
                                   detail, detail_size) != 0) {
        return -1;
    }

    char dst_dir[PM_PATH_MAX];
    char dst[PM_PATH_MAX];
    if (pm_join3(dst_dir, sizeof(dst_dir),
                 platform_root, "emulators", "ports") != 0 ||
        pm_join(dst, sizeof(dst), dst_dir, "launch.sh") != 0) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "%s", "Leaf ports launcher path too long");
        }
        return -1;
    }

    bool should_update = false;
    struct stat dst_st;
    if (stat(dst, &dst_st) != 0) {
        if (errno != ENOENT) {
            if (detail && detail_size > 0) {
                snprintf(detail, detail_size, "cannot inspect %s: %s",
                         dst, strerror(errno));
            }
            return -1;
        }
        should_update = true;
    } else if (!S_ISREG(dst_st.st_mode)) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "Leaf ports launcher is not a file: %s", dst);
        }
        return -1;
    } else {
        bool equal = false;
        if (pm_self_heal_files_equal(src, &src_st, dst, &dst_st,
                                     &equal, detail, detail_size) != 0) {
            return -1;
        }
        should_update = !equal;
    }

    if (!should_update) {
        return 0;
    }

    if (pm_mkdir_p(dst_dir, detail, detail_size) != 0) {
        return -1;
    }

    return pm_self_heal_copy_atomic(src, dst, &src_st, 0755, "ports launcher",
                                    detail, detail_size);
}

int pm_self_heal_leaf_ports_system_icon(const pm_context *ctx,
                                        char *detail,
                                        size_t detail_size)
{
    if (detail && detail_size > 0) {
        detail[0] = '\0';
    }
    if (!ctx) {
        return 0;
    }

    char res_dir[PM_PATH_MAX];
    char src[PM_PATH_MAX];
    if (pm_join(res_dir, sizeof(res_dir), ctx->pak_dir, "res") != 0 ||
        pm_join(src, sizeof(src), res_dir, "icon.png") != 0) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "%s", "packaged PortMaster icon path too long");
        }
        return -1;
    }

    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        return 0;
    }
    if (!S_ISREG(src_st.st_mode)) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "packaged PortMaster icon is not a file: %s", src);
        }
        return -1;
    }

    char platform_root[PM_PATH_MAX];
    if (pm_self_heal_platform_root(ctx, platform_root, sizeof(platform_root),
                                   detail, detail_size) != 0) {
        return -1;
    }

    char launcher_dir[PM_PATH_MAX];
    char res_root[PM_PATH_MAX];
    char dst_dir[PM_PATH_MAX];
    char dst[PM_PATH_MAX];
    if (pm_join(launcher_dir, sizeof(launcher_dir), platform_root, "launcher") != 0 ||
        pm_join(res_root, sizeof(res_root), launcher_dir, "res") != 0 ||
        pm_join(dst_dir, sizeof(dst_dir), res_root, "system_icons") != 0 ||
        pm_join(dst, sizeof(dst), dst_dir, "PORTS.png") != 0) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "%s", "Leaf ports system icon path too long");
        }
        return -1;
    }

    bool should_update = false;
    struct stat dst_st;
    if (stat(dst, &dst_st) != 0) {
        if (errno != ENOENT) {
            if (detail && detail_size > 0) {
                snprintf(detail, detail_size, "cannot inspect %s: %s",
                         dst, strerror(errno));
            }
            return -1;
        }
        should_update = true;
    } else if (!S_ISREG(dst_st.st_mode)) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "Leaf ports system icon is not a file: %s", dst);
        }
        return -1;
    } else {
        bool equal = false;
        if (pm_self_heal_files_equal(src, &src_st, dst, &dst_st,
                                     &equal, detail, detail_size) != 0) {
            return -1;
        }
        should_update = !equal;
    }

    if (!should_update) {
        return 0;
    }

    if (pm_mkdir_p(dst_dir, detail, detail_size) != 0) {
        return -1;
    }

    return pm_self_heal_copy_atomic(src, dst, &src_st, 0644, "PortMaster icon",
                                    detail, detail_size);
}

int pm_self_heal_leaf_ports_catalog(const pm_context *ctx,
                                    char *detail,
                                    size_t detail_size)
{
    if (detail && detail_size > 0) {
        detail[0] = '\0';
    }
    if (!ctx) {
        return 0;
    }

    char platform_root[PM_PATH_MAX];
    if (pm_self_heal_platform_root(ctx, platform_root, sizeof(platform_root),
                                   detail, detail_size) != 0) {
        return -1;
    }

    char defaults_dir[PM_PATH_MAX];
    char cores_path[PM_PATH_MAX];
    char systems_path[PM_PATH_MAX];
    if (pm_join(defaults_dir, sizeof(defaults_dir), platform_root, "defaults") != 0 ||
        pm_join(cores_path, sizeof(cores_path), defaults_dir, "cores.json") != 0 ||
        pm_join(systems_path, sizeof(systems_path), defaults_dir, "systems.json") != 0) {
        snprintf(detail, detail_size, "%s", "Leaf defaults path too long");
        return -1;
    }

    bool cores_changed = false;
    if (pm_self_heal_patch_ports_core(cores_path, ctx->platform,
                                      &cores_changed, detail, detail_size) != 0) {
        return -1;
    }

    bool systems_changed = false;
    if (pm_self_heal_patch_ports_system(systems_path,
                                        &systems_changed, detail, detail_size) != 0) {
        return -1;
    }

    if (!cores_changed && !systems_changed) {
        return 0;
    }

    if (cores_changed && systems_changed) {
        snprintf(detail, detail_size, "updated %s and %s", cores_path, systems_path);
    } else {
        snprintf(detail, detail_size, "updated %s",
                 cores_changed ? cores_path : systems_path);
    }
    return 1;
}
