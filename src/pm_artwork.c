#include "pm_artwork.h"

#include "cJSON.h"
#include "pm_sha256.h"
#include "pm_util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define PM_ARTWORK_XML_LIMIT (256u * 1024u)
#define PM_ARTWORK_MAX_OWNED 128
#define PM_ARTWORK_MANIFEST_VERSION 1

typedef struct {
    bool found;
    bool authoritative;
    char slug[256];
    char screenshot[PM_PATH_MAX];
    char metadata_relpath[PM_PATH_MAX];
    char owned[PM_ARTWORK_MAX_OWNED][PM_PATH_MAX];
    size_t owned_count;
} pm_artwork_package;

typedef enum {
    PM_ART_SOURCE_INSTALLED = 0,
    PM_ART_SOURCE_CACHE,
} pm_art_source_kind;

typedef struct {
    bool found;
    pm_art_source_kind kind;
    char path[PM_PATH_MAX];
    char relpath[PM_PATH_MAX];
} pm_artwork_candidate;

typedef struct {
    char paths[8][PM_PATH_MAX];
    size_t count;
    bool overflow;
} pm_artwork_matches;

typedef struct {
    int fd;
    char manifest_path[PM_PATH_MAX];
    char pending_path[PM_PATH_MAX];
    char dir[PM_PATH_MAX];
    cJSON *root;
    cJSON *entries;
} pm_artwork_manifest;

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

static bool pm__is_image_path(const char *path)
{
    return pm__has_suffix_casefold(path, ".png") ||
           pm__has_suffix_casefold(path, ".jpg") ||
           pm__has_suffix_casefold(path, ".jpeg");
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
    if (pm_copy(tmp, sizeof(tmp), pm__basename_const(name)) != 0) {
        out[0] = '\0';
        return;
    }
    if (pm__has_suffix_casefold(tmp, ".zip")) {
        tmp[strlen(tmp) - 4] = '\0';
    }
    pm__lower_copy(tmp, out, out_size);
}

static bool pm__component_invalid(const char *start, size_t len)
{
    if (len == 0 || (len == 2 && start[0] == '.' && start[1] == '.')) {
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c < 0x20u || c == 0x7fu || c == ':') {
            return true;
        }
    }
    return false;
}

/*
 * Normalize a PortMaster path relative to Roms/PORTS. Leading and embedded
 * current-directory components are removed. Empty components and parent
 * traversal are rejected so the result is also safe to store in provenance.
 */
static int pm__normalize_relative(const char *input, char *out, size_t out_size)
{
    if (!input || !out || out_size == 0 || !input[0] ||
        input[0] == '/' || input[0] == '\\') {
        return -1;
    }

    size_t used = 0;
    const char *p = input;
    bool saw_component = false;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            /* Accepted and removed. */
        } else {
            if (pm__component_invalid(start, len)) {
                return -1;
            }
            if (used > 0) {
                if (used + 1u >= out_size) {
                    return -1;
                }
                out[used++] = '/';
            }
            if (used + len >= out_size) {
                return -1;
            }
            memcpy(out + used, start, len);
            used += len;
            saw_component = true;
        }
        if (*p) {
            p++;
            if (!*p) {
                /*
                 * PortMaster's canonical ownership metadata marks directories
                 * with one trailing slash (for example "nonny/"). Preserve
                 * the safe normalized component while still rejecting empty
                 * components anywhere else in the path.
                 */
                break;
            }
        }
    }
    if (!saw_component) {
        return -1;
    }
    out[used] = '\0';
    return 0;
}

static bool pm__relative_is_same_or_child(const char *path, const char *root)
{
    size_t root_len = strlen(root);
    return strcasecmp(path, root) == 0 ||
           (strncasecmp(path, root, root_len) == 0 && path[root_len] == '/');
}

static bool pm__lstat_regular(const char *path)
{
    struct stat st;
    return path && lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool pm__lstat_directory(const char *path)
{
    struct stat st;
    return path && lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool pm__path_components_safe(const char *root, const char *relpath, bool require_regular)
{
    char normalized[PM_PATH_MAX];
    if (pm__normalize_relative(relpath, normalized, sizeof(normalized)) != 0) {
        return false;
    }

    char current[PM_PATH_MAX];
    if (pm_copy(current, sizeof(current), root) != 0) {
        return false;
    }
    char *save = NULL;
    char parts[PM_PATH_MAX];
    if (pm_copy(parts, sizeof(parts), normalized) != 0) {
        return false;
    }
    char *part = strtok_r(parts, "/", &save);
    while (part) {
        char next_path[PM_PATH_MAX];
        if (pm_join(next_path, sizeof(next_path), current, part) != 0 ||
            pm_copy(current, sizeof(current), next_path) != 0) {
            return false;
        }
        struct stat st;
        if (lstat(current, &st) != 0 || S_ISLNK(st.st_mode)) {
            return false;
        }
        char *next = strtok_r(NULL, "/", &save);
        if (next && !S_ISDIR(st.st_mode)) {
            return false;
        }
        if (!next && require_regular && (!S_ISREG(st.st_mode) || st.st_size <= 0)) {
            return false;
        }
        part = next;
    }
    return true;
}

static int pm__package_add_owned(pm_artwork_package *package, const char *raw)
{
    char normalized[PM_PATH_MAX];
    if (!package || pm__normalize_relative(raw, normalized, sizeof(normalized)) != 0) {
        return -1;
    }
    for (size_t i = 0; i < package->owned_count; i++) {
        if (strcasecmp(package->owned[i], normalized) == 0) {
            return 0;
        }
    }
    if (package->owned_count >= PM_ARTWORK_MAX_OWNED) {
        return -1;
    }
    return pm_copy(package->owned[package->owned_count++], PM_PATH_MAX, normalized);
}

static int pm__package_add_json_value(pm_artwork_package *package, const cJSON *value)
{
    if (cJSON_IsString(value) && value->valuestring) {
        return pm__package_add_owned(package, value->valuestring);
    }
    if (!cJSON_IsArray(value)) {
        return -1;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, value) {
        if (!cJSON_IsString(item) || !item->valuestring ||
            pm__package_add_owned(package, item->valuestring) != 0) {
            return -1;
        }
    }
    return 0;
}

static int pm__package_add_present_array(const pm_context *ctx,
                                         pm_artwork_package *package,
                                         const cJSON *array)
{
    if (cJSON_IsNull(array) || !array) {
        return 0;
    }
    if (!cJSON_IsArray(array)) {
        return -1;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        char normalized[PM_PATH_MAX];
        char full[PM_PATH_MAX];
        struct stat st;
        if (!cJSON_IsString(item) || !item->valuestring ||
            pm__normalize_relative(item->valuestring, normalized, sizeof(normalized)) != 0 ||
            pm_join(full, sizeof(full), ctx->ports_dir, normalized) != 0) {
            return -1;
        }
        if (lstat(full, &st) == 0 && !S_ISLNK(st.st_mode) &&
            (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
            if (pm__package_add_owned(package, normalized) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int pm__read_package(const pm_context *ctx,
                            const char *metadata_relpath,
                            pm_artwork_package *out)
{
    char metadata_path[PM_PATH_MAX];
    char json_err[256];
    if (pm_join(metadata_path, sizeof(metadata_path), ctx->ports_dir, metadata_relpath) != 0 ||
        !pm__path_components_safe(ctx->ports_dir, metadata_relpath, true)) {
        return -1;
    }
    char *text = pm_read_text_file(metadata_path, PM_ARTWORK_XML_LIMIT,
                                   json_err, sizeof(json_err));
    if (!text) {
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (pm_copy(out->metadata_relpath, sizeof(out->metadata_relpath),
                metadata_relpath) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name->valuestring) {
        pm__slug_from_archive_name(name->valuestring, out->slug, sizeof(out->slug));
    }

    const cJSON *attr = cJSON_GetObjectItemCaseSensitive(root, "attr");
    const cJSON *image = cJSON_IsObject(attr)
        ? cJSON_GetObjectItemCaseSensitive(attr, "image")
        : NULL;
    const cJSON *screenshot = cJSON_IsObject(image)
        ? cJSON_GetObjectItemCaseSensitive(image, "screenshot")
        : NULL;
    if (cJSON_IsString(screenshot) && screenshot->valuestring) {
        (void)pm__normalize_relative(screenshot->valuestring,
                                     out->screenshot, sizeof(out->screenshot));
    }

    int rc = 0;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (cJSON_IsObject(files)) {
        out->authoritative = true;
        const cJSON *value = NULL;
        cJSON_ArrayForEach(value, files) {
            if (pm__package_add_json_value(out, value) != 0) {
                rc = -1;
                break;
            }
        }
    } else if (files) {
        rc = -1;
    } else {
        if (pm__package_add_owned(out, metadata_relpath) != 0 ||
            pm__package_add_present_array(ctx, out,
                cJSON_GetObjectItemCaseSensitive(root, "items")) != 0 ||
            pm__package_add_present_array(ctx, out,
                cJSON_GetObjectItemCaseSensitive(root, "items_opt")) != 0) {
            rc = -1;
        }
    }
    cJSON_Delete(root);
    if (rc != 0 || out->owned_count == 0) {
        return -1;
    }
    out->found = true;
    return 0;
}

static bool pm__package_owns(const pm_artwork_package *package, const char *relpath)
{
    if (!package || !package->found || !relpath) {
        return false;
    }
    for (size_t i = 0; i < package->owned_count; i++) {
        if (pm__relative_is_same_or_child(relpath, package->owned[i])) {
            return true;
        }
    }
    return false;
}

static bool pm__package_owns_regular(const pm_context *ctx,
                                     const pm_artwork_package *package,
                                     const char *relpath)
{
    return pm__package_owns(package, relpath) &&
           pm__path_components_safe(ctx->ports_dir, relpath, true);
}

static int pm__consider_package(const pm_context *ctx,
                                const char *metadata_relpath,
                                const char *script_relpath,
                                pm_artwork_package *match,
                                int *match_count)
{
    pm_artwork_package candidate;
    if (pm__read_package(ctx, metadata_relpath, &candidate) != 0 ||
        !pm__package_owns_regular(ctx, &candidate, script_relpath)) {
        return 0;
    }
    (*match_count)++;
    if (*match_count == 1) {
        *match = candidate;
    }
    return 0;
}

static int pm__find_package_for_script(const pm_context *ctx,
                                       const char *script_name,
                                       pm_artwork_package *out)
{
    int matches = 0;
    DIR *top = opendir(ctx->ports_dir);
    if (!top) {
        return -1;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(top)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char child[PM_PATH_MAX];
        if (pm_join(child, sizeof(child), ctx->ports_dir, ent->d_name) != 0 ||
            !pm__lstat_directory(child)) {
            continue;
        }
        DIR *dir = opendir(child);
        if (!dir) {
            continue;
        }
        struct dirent *meta = NULL;
        while ((meta = readdir(dir)) != NULL) {
            if (meta->d_name[0] == '.') {
                continue;
            }
            if (strcasecmp(meta->d_name, "port.json") != 0 &&
                !pm__has_suffix_casefold(meta->d_name, ".port.json")) {
                continue;
            }
            char relpath[PM_PATH_MAX];
            if (pm_join(relpath, sizeof(relpath), ent->d_name, meta->d_name) == 0) {
                (void)pm__consider_package(ctx, relpath, script_name, out, &matches);
            }
        }
        closedir(dir);
    }
    closedir(top);
    return matches == 1 ? 0 : -1;
}

static void pm__matches_add(pm_artwork_matches *matches, const char *relpath)
{
    if (!matches || matches->overflow) {
        return;
    }
    for (size_t i = 0; i < matches->count; i++) {
        if (strcasecmp(matches->paths[i], relpath) == 0) {
            return;
        }
    }
    if (matches->count >= sizeof(matches->paths) / sizeof(matches->paths[0])) {
        matches->overflow = true;
        return;
    }
    (void)pm_copy(matches->paths[matches->count++], PM_PATH_MAX, relpath);
}

typedef bool (*pm_artwork_name_predicate)(const char *name, void *userdata);

static void pm__find_owned_named(const pm_context *ctx,
                                 const pm_artwork_package *package,
                                 pm_artwork_name_predicate predicate,
                                 void *userdata,
                                 pm_artwork_matches *matches)
{
    memset(matches, 0, sizeof(*matches));
    for (size_t i = 0; i < package->owned_count && !matches->overflow; i++) {
        char path[PM_PATH_MAX];
        struct stat st;
        if (pm_join(path, sizeof(path), ctx->ports_dir, package->owned[i]) != 0 ||
            lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISREG(st.st_mode) && st.st_size > 0 &&
            predicate(pm__basename_const(package->owned[i]), userdata)) {
            pm__matches_add(matches, package->owned[i]);
        } else if (S_ISDIR(st.st_mode)) {
            /*
             * PortMaster ownership metadata describes top-level installed
             * items. Covers and gameinfo.xml live at the root of those owned
             * package directories; do not crawl user-supplied game data.
             */
            DIR *dir = opendir(path);
            if (!dir) {
                continue;
            }
            struct dirent *ent = NULL;
            while ((ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0 ||
                    !predicate(ent->d_name, userdata)) {
                    continue;
                }
                char relpath[PM_PATH_MAX];
                if (pm_join(relpath, sizeof(relpath), package->owned[i],
                            ent->d_name) == 0 &&
                    pm__package_owns_regular(ctx, package, relpath)) {
                    pm__matches_add(matches, relpath);
                }
            }
            closedir(dir);
        }
    }
}

static bool pm__name_gameinfo(const char *name, void *userdata)
{
    return strcasecmp(name, "gameinfo.xml") == 0;
}

static bool pm__name_exact_image(const char *name, void *userdata)
{
    const char *wanted = (const char *)userdata;
    const char *extensions[] = { ".png", ".jpg", ".jpeg" };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        char exact[64];
        if (pm_format(exact, sizeof(exact), "%s%s", wanted, extensions[i]) == 0 &&
            strcasecmp(name, exact) == 0) {
            return true;
        }
    }
    return false;
}

static const char *pm__strcasestr_local(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return haystack;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}

static int pm__xml_decode(const char *start, size_t len, char *out, size_t out_size)
{
    size_t used = 0;
    for (size_t i = 0; i < len;) {
        if (start[i] != '&') {
            if (used + 1u >= out_size) {
                return -1;
            }
            out[used++] = start[i++];
            continue;
        }
        const struct {
            const char *entity;
            char value;
        } entities[] = {
            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
            { "&quot;", '"' }, { "&apos;", '\'' },
        };
        bool decoded = false;
        for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
            size_t entity_len = strlen(entities[e].entity);
            if (i + entity_len <= len &&
                memcmp(start + i, entities[e].entity, entity_len) == 0) {
                if (used + 1u >= out_size) {
                    return -1;
                }
                out[used++] = entities[e].value;
                i += entity_len;
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            return -1;
        }
    }
    out[used] = '\0';
    return 0;
}

static int pm__xml_element_text(const char *block_start,
                                const char *block_end,
                                const char *tag,
                                char *out,
                                size_t out_size)
{
    char open[64];
    char close[64];
    if (pm_format(open, sizeof(open), "<%s", tag) != 0 ||
        pm_format(close, sizeof(close), "</%s>", tag) != 0) {
        return -1;
    }
    const char *start = pm__strcasestr_local(block_start, open);
    if (!start || start >= block_end) {
        return -1;
    }
    char boundary = start[strlen(open)];
    if (boundary != '>' && !isspace((unsigned char)boundary)) {
        return -1;
    }
    start = strchr(start, '>');
    if (!start || ++start >= block_end) {
        return -1;
    }
    const char *end = pm__strcasestr_local(start, close);
    if (!end || end > block_end) {
        return -1;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return pm__xml_decode(start, (size_t)(end - start), out, out_size);
}

static int pm__gameinfo_mapping(const pm_context *ctx,
                                const pm_artwork_package *package,
                                const char *xml_relpath,
                                const char *script_relpath,
                                char *image_relpath,
                                size_t image_relpath_size)
{
    char xml_path[PM_PATH_MAX];
    char read_err[256];
    if (!pm__package_owns_regular(ctx, package, xml_relpath) ||
        pm_join(xml_path, sizeof(xml_path), ctx->ports_dir, xml_relpath) != 0) {
        return -1;
    }
    char *text = pm_read_text_file(xml_path, PM_ARTWORK_XML_LIMIT,
                                   read_err, sizeof(read_err));
    if (!text) {
        return -1;
    }
    if (pm__strcasestr_local(text, "<!DOCTYPE") ||
        pm__strcasestr_local(text, "<!ENTITY") ||
        pm__strcasestr_local(text, "<![CDATA[")) {
        free(text);
        return -1;
    }

    int matches = 0;
    char selected[PM_PATH_MAX] = "";
    const char *cursor = text;
    while ((cursor = pm__strcasestr_local(cursor, "<game")) != NULL) {
        char boundary = cursor[5];
        if (boundary != '>' && !isspace((unsigned char)boundary)) {
            cursor += 5;
            continue;
        }
        const char *open_end = strchr(cursor, '>');
        const char *block_end = open_end
            ? pm__strcasestr_local(open_end + 1, "</game>")
            : NULL;
        if (!open_end || !block_end) {
            free(text);
            return -1;
        }
        char raw_path[PM_PATH_MAX];
        char raw_image[PM_PATH_MAX];
        char normalized_path[PM_PATH_MAX];
        char normalized_image[PM_PATH_MAX];
        if (pm__xml_element_text(open_end + 1, block_end, "path",
                                 raw_path, sizeof(raw_path)) == 0 &&
            pm__xml_element_text(open_end + 1, block_end, "image",
                                 raw_image, sizeof(raw_image)) == 0 &&
            pm__normalize_relative(raw_path, normalized_path,
                                   sizeof(normalized_path)) == 0 &&
            strcasecmp(normalized_path, script_relpath) == 0 &&
            pm__normalize_relative(raw_image, normalized_image,
                                   sizeof(normalized_image)) == 0) {
            matches++;
            if (matches == 1) {
                (void)pm_copy(selected, sizeof(selected), normalized_image);
            }
        }
        cursor = block_end + strlen("</game>");
    }
    free(text);
    if (matches != 1 || !pm__is_image_path(selected) ||
        !pm__package_owns_regular(ctx, package, selected)) {
        return -1;
    }
    return pm_copy(image_relpath, image_relpath_size, selected);
}

static bool pm__candidate_from_installed(const pm_context *ctx,
                                         const char *relpath,
                                         pm_artwork_candidate *out)
{
    char path[PM_PATH_MAX];
    if (pm_join(path, sizeof(path), ctx->ports_dir, relpath) != 0 ||
        !pm__lstat_regular(path)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->found = true;
    out->kind = PM_ART_SOURCE_INSTALLED;
    if (pm_copy(out->path, sizeof(out->path), path) != 0 ||
        pm_format(out->relpath, sizeof(out->relpath), "PORTS/%s", relpath) != 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool pm__find_mapped_cover(const pm_context *ctx,
                                  const pm_artwork_package *package,
                                  const char *script_relpath,
                                  pm_artwork_candidate *out)
{
    pm_artwork_matches xml_files;
    pm__find_owned_named(ctx, package, pm__name_gameinfo, NULL, &xml_files);
    if (xml_files.overflow) {
        return false;
    }
    char selected[PM_PATH_MAX] = "";
    int matches = 0;
    for (size_t i = 0; i < xml_files.count; i++) {
        char image_relpath[PM_PATH_MAX];
        if (pm__gameinfo_mapping(ctx, package, xml_files.paths[i], script_relpath,
                                 image_relpath, sizeof(image_relpath)) == 0) {
            matches++;
            if (matches == 1) {
                (void)pm_copy(selected, sizeof(selected), image_relpath);
            }
        }
    }
    return matches == 1 && pm__candidate_from_installed(ctx, selected, out);
}

static bool pm__find_unique_named(const pm_context *ctx,
                                  const pm_artwork_package *package,
                                  const char *name,
                                  pm_artwork_candidate *out)
{
    pm_artwork_matches matches;
    pm__find_owned_named(ctx, package, pm__name_exact_image, (void *)name, &matches);
    return !matches.overflow && matches.count == 1 &&
           pm__candidate_from_installed(ctx, matches.paths[0], out);
}

static bool pm__first_existing(const char *dir,
                               const char *base,
                               const char *const *suffixes,
                               size_t suffix_count,
                               char *out,
                               size_t out_size,
                               char *relpath,
                               size_t relpath_size)
{
    for (size_t i = 0; i < suffix_count; i++) {
        char name[512];
        if (pm_format(name, sizeof(name), "%s%s", base, suffixes[i]) != 0 ||
            pm_join(out, out_size, dir, name) != 0) {
            continue;
        }
        if (pm__lstat_regular(out)) {
            return pm_copy(relpath, relpath_size, name) == 0;
        }
    }
    return false;
}

static bool pm__find_cache_screenshot(const pm_context *ctx,
                                      const pm_artwork_package *package,
                                      const char *stem,
                                      pm_artwork_candidate *out)
{
    char cache_dir[PM_PATH_MAX];
    if (pm_join3(cache_dir, sizeof(cache_dir), ctx->portmaster_dir,
                 "config", "images_pm") != 0) {
        return false;
    }
    const char *suffixes[] = {
        ".screenshot.png", ".screenshot.jpg", ".screenshot.jpeg",
    };
    char lower_stem[256];
    pm__lower_copy(stem, lower_stem, sizeof(lower_stem));
    memset(out, 0, sizeof(*out));
    out->kind = PM_ART_SOURCE_CACHE;
    if (package->slug[0] &&
        pm__first_existing(cache_dir, package->slug, suffixes,
                           sizeof(suffixes) / sizeof(suffixes[0]),
                           out->path, sizeof(out->path),
                           out->relpath, sizeof(out->relpath))) {
        out->found = true;
        return true;
    }
    if (lower_stem[0] && strcasecmp(package->slug, lower_stem) != 0 &&
        pm__first_existing(cache_dir, lower_stem, suffixes,
                           sizeof(suffixes) / sizeof(suffixes[0]),
                           out->path, sizeof(out->path),
                           out->relpath, sizeof(out->relpath))) {
        out->found = true;
        return true;
    }
    return false;
}

static bool pm__find_attr_screenshot(const pm_context *ctx,
                                     const pm_artwork_package *package,
                                     pm_artwork_candidate *out)
{
    if (!package->screenshot[0]) {
        return false;
    }
    if (pm__package_owns_regular(ctx, package, package->screenshot) &&
        pm__is_image_path(package->screenshot)) {
        return pm__candidate_from_installed(ctx, package->screenshot, out);
    }

    char selected[PM_PATH_MAX] = "";
    int matches = 0;
    for (size_t i = 0; i < package->owned_count; i++) {
        char owned_path[PM_PATH_MAX];
        struct stat st;
        if (pm_join(owned_path, sizeof(owned_path), ctx->ports_dir,
                    package->owned[i]) != 0 ||
            lstat(owned_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        char candidate[PM_PATH_MAX];
        if (pm_join(candidate, sizeof(candidate), package->owned[i],
                    package->screenshot) == 0 &&
            pm__package_owns_regular(ctx, package, candidate) &&
            pm__is_image_path(candidate)) {
            matches++;
            if (matches == 1) {
                (void)pm_copy(selected, sizeof(selected), candidate);
            }
        }
    }
    return matches == 1 && pm__candidate_from_installed(ctx, selected, out);
}

static bool pm__resolve_candidate(const pm_context *ctx,
                                  const char *script_name,
                                  const char *stem,
                                  pm_artwork_candidate *out)
{
    pm_artwork_package package;
    memset(&package, 0, sizeof(package));
    if (pm__find_package_for_script(ctx, script_name, &package) != 0) {
        return false;
    }

    /*
     * The precedence deliberately keeps installed cover data ahead of the
     * shared images_pm screenshot cache.
     */
    pm_artwork_candidate candidates[6];
    memset(candidates, 0, sizeof(candidates));
    bool found[] = {
        pm__find_mapped_cover(ctx, &package, script_name, &candidates[0]),
        pm__find_unique_named(ctx, &package, "cover", &candidates[1]),
        pm__find_cache_screenshot(ctx, &package, stem, &candidates[2]),
        pm__find_attr_screenshot(ctx, &package, &candidates[3]),
        pm__find_unique_named(ctx, &package, "screenshot", &candidates[4]),
        pm__find_unique_named(ctx, &package, "splash", &candidates[5]),
    };
    for (size_t i = 0; i < sizeof(found) / sizeof(found[0]); i++) {
        if (!found[i]) {
            continue;
        }
        if (SDL_Init(0) != 0) {
            continue;
        }
        (void)IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
        SDL_Surface *surface = IMG_Load(candidates[i].path);
        if (surface) {
            SDL_FreeSurface(surface);
            IMG_Quit();
            *out = candidates[i];
            return true;
        }
        IMG_Quit();
    }
    return false;
}

static int pm__fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int rc = fsync(fd);
    int saved = errno;
    close(fd);
    if (rc != 0 && saved != EINVAL && saved != ENOTSUP) {
        errno = saved;
        return -1;
    }
    return 0;
}

static int pm__write_bytes_atomic(const char *path,
                                  const char *bytes,
                                  size_t size,
                                  char *err,
                                  size_t err_size)
{
    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.partial", path) != 0) {
        snprintf(err, err_size, "path too long: %s", path);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, err_size, "cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, bytes + written, size - written);
        if (rc <= 0) {
            snprintf(err, err_size, "cannot write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        written += (size_t)rc;
    }
    int flush_rc = fsync(fd);
    int flush_errno = errno;
    int close_rc = close(fd);
    if (flush_rc != 0 || close_rc != 0) {
        if (flush_rc != 0) {
            errno = flush_errno;
        }
        snprintf(err, err_size, "cannot flush %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, err_size, "cannot promote %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int pm__write_json_atomic(const char *path,
                                 const cJSON *root,
                                 char *err,
                                 size_t err_size)
{
    char *printed = cJSON_Print(root);
    if (!printed) {
        snprintf(err, err_size, "cannot encode artwork state");
        return -1;
    }
    int rc = pm__write_bytes_atomic(path, printed, strlen(printed), err, err_size);
    free(printed);
    return rc;
}

static cJSON *pm__new_manifest(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateObject();
    if (!root || !entries ||
        !cJSON_AddNumberToObject(root, "version", PM_ARTWORK_MANIFEST_VERSION) ||
        !cJSON_AddItemToObject(root, "entries", entries)) {
        if (entries && (!root || !cJSON_GetObjectItemCaseSensitive(root, "entries"))) {
            cJSON_Delete(entries);
        }
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *pm__load_json_object(const char *path, size_t limit)
{
    char read_err[256];
    char *text = pm_read_text_file(path, limit, read_err, sizeof(read_err));
    if (!text) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static int pm__manifest_store(pm_artwork_manifest *manifest,
                              char *err,
                              size_t err_size)
{
    if (pm__write_json_atomic(manifest->manifest_path, manifest->root,
                              err, err_size) != 0 ||
        pm__fsync_dir(manifest->dir) != 0) {
        if (!err[0]) {
            snprintf(err, err_size, "cannot flush artwork manifest directory");
        }
        return -1;
    }
    return 0;
}

static int pm__manifest_merge_entry(pm_artwork_manifest *manifest,
                                    const char *target_name,
                                    const cJSON *entry,
                                    char *err,
                                    size_t err_size)
{
    cJSON *copy = cJSON_Duplicate(entry, true);
    if (!copy) {
        snprintf(err, err_size, "cannot duplicate artwork manifest entry");
        return -1;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(manifest->entries, target_name);
    if (!cJSON_AddItemToObject(manifest->entries, target_name, copy)) {
        cJSON_Delete(copy);
        snprintf(err, err_size, "cannot merge artwork manifest entry");
        return -1;
    }
    return pm__manifest_store(manifest, err, err_size);
}

static bool pm__safe_target_name(const char *name)
{
    return name && name[0] && !strchr(name, '/') && !strchr(name, '\\') &&
           !strchr(name, ':') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int pm__manifest_recover(pm_artwork_manifest *manifest,
                                char *err,
                                size_t err_size)
{
    if (!pm_file_exists(manifest->pending_path)) {
        return 0;
    }
    cJSON *pending = pm__load_json_object(manifest->pending_path, 64u * 1024u);
    if (!pending) {
        snprintf(err, err_size, "artwork pending record is malformed");
        return -1;
    }
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(pending, "target");
    const cJSON *output_sha = cJSON_GetObjectItemCaseSensitive(pending, "output_sha256");
    if (!cJSON_IsString(target) || !target->valuestring ||
        !pm__safe_target_name(target->valuestring) ||
        !cJSON_IsString(output_sha) || !output_sha->valuestring ||
        strlen(output_sha->valuestring) != 64u) {
        cJSON_Delete(pending);
        snprintf(err, err_size, "artwork pending record is invalid");
        return -1;
    }

    char target_path[PM_PATH_MAX] = "";
    char digest[65];
    char hash_err[256];
    bool have_target_path = pm_join(target_path, sizeof(target_path), manifest->dir,
                                    target->valuestring) == 0;
    bool matches = have_target_path &&
                   pm_sha256_file_hex(target_path, digest,
                                      hash_err, sizeof(hash_err)) == 0 &&
                   strcasecmp(digest, output_sha->valuestring) == 0;
    if (matches &&
        pm__manifest_merge_entry(manifest, target->valuestring, pending,
                                 err, err_size) != 0) {
        cJSON_Delete(pending);
        return -1;
    }
    char stage_path[PM_PATH_MAX];
    if (have_target_path &&
        pm_format(stage_path, sizeof(stage_path), "%s.portmaster-partial",
                  target_path) == 0) {
        (void)unlink(stage_path);
    }
    cJSON_Delete(pending);
    if (unlink(manifest->pending_path) != 0 && errno != ENOENT) {
        snprintf(err, err_size, "cannot remove recovered artwork pending record: %s",
                 strerror(errno));
        return -1;
    }
    (void)pm__fsync_dir(manifest->dir);
    return 0;
}

static int pm__manifest_open(const pm_context *ctx,
                             pm_artwork_manifest *manifest,
                             char *err,
                             size_t err_size)
{
    memset(manifest, 0, sizeof(*manifest));
    manifest->fd = -1;
    if (pm_copy(manifest->dir, sizeof(manifest->dir), ctx->port_images_dir) != 0 ||
        pm_join(manifest->manifest_path, sizeof(manifest->manifest_path),
                manifest->dir, ".portmaster-artwork.json") != 0 ||
        pm_join(manifest->pending_path, sizeof(manifest->pending_path),
                manifest->dir, ".portmaster-artwork.pending.json") != 0) {
        snprintf(err, err_size, "artwork state path too long");
        return -1;
    }
    char lock_path[PM_PATH_MAX];
    if (pm_join(lock_path, sizeof(lock_path), manifest->dir,
                ".portmaster-artwork.lock") != 0) {
        snprintf(err, err_size, "artwork lock path too long");
        return -1;
    }
    manifest->fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (manifest->fd < 0 || flock(manifest->fd, LOCK_EX) != 0) {
        snprintf(err, err_size, "cannot lock artwork state: %s", strerror(errno));
        if (manifest->fd >= 0) {
            close(manifest->fd);
            manifest->fd = -1;
        }
        return -1;
    }

    if (pm_file_exists(manifest->manifest_path)) {
        manifest->root = pm__load_json_object(manifest->manifest_path, 1024u * 1024u);
        cJSON *version = manifest->root
            ? cJSON_GetObjectItemCaseSensitive(manifest->root, "version")
            : NULL;
        manifest->entries = manifest->root
            ? cJSON_GetObjectItemCaseSensitive(manifest->root, "entries")
            : NULL;
        if (!cJSON_IsNumber(version) ||
            version->valueint != PM_ARTWORK_MANIFEST_VERSION ||
            !cJSON_IsObject(manifest->entries)) {
            snprintf(err, err_size, "artwork manifest is malformed or unsupported");
            return -1;
        }
    } else {
        manifest->root = pm__new_manifest();
        manifest->entries = manifest->root
            ? cJSON_GetObjectItemCaseSensitive(manifest->root, "entries")
            : NULL;
        if (!manifest->root || !manifest->entries) {
            snprintf(err, err_size, "cannot create artwork manifest");
            return -1;
        }
    }
    return pm__manifest_recover(manifest, err, err_size);
}

static void pm__manifest_close(pm_artwork_manifest *manifest)
{
    if (!manifest) {
        return;
    }
    cJSON_Delete(manifest->root);
    manifest->root = NULL;
    manifest->entries = NULL;
    if (manifest->fd >= 0) {
        (void)flock(manifest->fd, LOCK_UN);
        close(manifest->fd);
        manifest->fd = -1;
    }
}

static bool pm__manifest_target_managed(pm_artwork_manifest *manifest,
                                        const char *target_name,
                                        const char *target_path)
{
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(manifest->entries, target_name);
    cJSON *generated = cJSON_IsObject(entry)
        ? cJSON_GetObjectItemCaseSensitive(entry, "output_sha256")
        : NULL;
    if (!cJSON_IsString(generated) || !generated->valuestring ||
        strlen(generated->valuestring) != 64u) {
        return false;
    }
    char digest[65];
    char hash_err[256];
    return pm_sha256_file_hex(target_path, digest, hash_err, sizeof(hash_err)) == 0 &&
           strcasecmp(digest, generated->valuestring) == 0;
}

static int pm__copy_file(const char *src, const char *dst, char *err, size_t err_size)
{
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        snprintf(err, err_size, "cannot open artwork source: %s", strerror(errno));
        return -1;
    }
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        snprintf(err, err_size, "cannot create artwork staging file: %s", strerror(errno));
        close(in_fd);
        return -1;
    }
    unsigned char buf[64u * 1024u];
    int rc = 0;
    while (true) {
        ssize_t got = read(in_fd, buf, sizeof(buf));
        if (got < 0) {
            snprintf(err, err_size, "cannot read artwork source: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (got == 0) {
            break;
        }
        size_t written = 0;
        while (written < (size_t)got) {
            ssize_t put = write(out_fd, buf + written, (size_t)got - written);
            if (put <= 0) {
                snprintf(err, err_size, "cannot write artwork staging file: %s",
                         strerror(errno));
                rc = -1;
                break;
            }
            written += (size_t)put;
        }
        if (rc != 0) {
            break;
        }
    }
    if (rc == 0 && fsync(out_fd) != 0) {
        snprintf(err, err_size, "cannot flush artwork staging file: %s", strerror(errno));
        rc = -1;
    }
    close(in_fd);
    if (close(out_fd) != 0 && rc == 0) {
        snprintf(err, err_size, "cannot close artwork staging file: %s", strerror(errno));
        rc = -1;
    }
    if (rc != 0) {
        unlink(dst);
    }
    return rc;
}

static int pm__write_png_staging(const char *src,
                                 const char *dst,
                                 char *err,
                                 size_t err_size)
{
    unlink(dst);
    if (pm__has_suffix_casefold(src, ".png")) {
        return pm__copy_file(src, dst, err, err_size);
    }
    if (SDL_Init(0) != 0) {
        snprintf(err, err_size, "SDL init failed: %s", SDL_GetError());
        return -1;
    }
    int ready = IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    if ((ready & IMG_INIT_PNG) == 0 || (ready & IMG_INIT_JPG) == 0) {
        snprintf(err, err_size, "SDL_image init failed: %s", IMG_GetError());
        IMG_Quit();
        return -1;
    }
    SDL_Surface *surface = IMG_Load(src);
    if (!surface) {
        snprintf(err, err_size, "cannot load artwork source: %s", IMG_GetError());
        IMG_Quit();
        return -1;
    }
    int save_rc = IMG_SavePNG(surface, dst);
    SDL_FreeSurface(surface);
    IMG_Quit();
    if (save_rc != 0) {
        snprintf(err, err_size, "cannot write artwork PNG: %s", IMG_GetError());
        unlink(dst);
        return -1;
    }
    int fd = open(dst, O_RDONLY);
    if (fd < 0 || fsync(fd) != 0) {
        snprintf(err, err_size, "cannot flush artwork PNG: %s", strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        unlink(dst);
        return -1;
    }
    close(fd);
    return 0;
}

static cJSON *pm__build_manifest_entry(const pm_context *ctx,
                                       const char *target_name,
                                       const pm_artwork_candidate *source,
                                       const char *source_sha,
                                       const char *output_sha)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry ||
        !cJSON_AddStringToObject(entry, "target", target_name) ||
        !cJSON_AddStringToObject(entry, "source_kind",
            source->kind == PM_ART_SOURCE_INSTALLED ? "installed" : "cache") ||
        !cJSON_AddStringToObject(entry, "source_id",
            source->kind == PM_ART_SOURCE_INSTALLED ? ctx->source_id : "primary") ||
        !cJSON_AddStringToObject(entry, "source_root",
            source->kind == PM_ART_SOURCE_INSTALLED ? "roms" : "cache") ||
        !cJSON_AddStringToObject(entry, "source_relpath", source->relpath) ||
        !cJSON_AddStringToObject(entry, "source_sha256", source_sha) ||
        !cJSON_AddStringToObject(entry, "output_sha256", output_sha)) {
        cJSON_Delete(entry);
        return NULL;
    }
    return entry;
}

static int pm__publish_artwork(const pm_context *ctx,
                               pm_artwork_manifest *manifest,
                               const pm_artwork_candidate *source,
                               const char *target_name,
                               const char *target_path,
                               char *err,
                               size_t err_size)
{
    char stage[PM_PATH_MAX];
    if (pm_format(stage, sizeof(stage), "%s.portmaster-partial", target_path) != 0) {
        snprintf(err, err_size, "artwork staging path too long");
        return -1;
    }
    char source_sha[65];
    char source_sha_after[65];
    if (pm_sha256_file_hex(source->path, source_sha, err, err_size) != 0) {
        return -1;
    }
    if (pm__write_png_staging(source->path, stage, err, err_size) != 0) {
        return -1;
    }
    char output_sha[65];
    if (pm_sha256_file_hex(source->path, source_sha_after, err, err_size) != 0 ||
        strcasecmp(source_sha, source_sha_after) != 0 ||
        pm_sha256_file_hex(stage, output_sha, err, err_size) != 0) {
        if (!err[0]) {
            snprintf(err, err_size, "artwork source changed while it was read");
        }
        unlink(stage);
        return -1;
    }
    cJSON *entry = pm__build_manifest_entry(ctx, target_name, source,
                                            source_sha, output_sha);
    if (!entry) {
        unlink(stage);
        snprintf(err, err_size, "cannot create artwork provenance entry");
        return -1;
    }
    if (pm__write_json_atomic(manifest->pending_path, entry, err, err_size) != 0 ||
        pm__fsync_dir(manifest->dir) != 0) {
        cJSON_Delete(entry);
        unlink(stage);
        return -1;
    }
    if (rename(stage, target_path) != 0 || pm__fsync_dir(manifest->dir) != 0) {
        snprintf(err, err_size, "cannot publish artwork target: %s", strerror(errno));
        cJSON_Delete(entry);
        return -1;
    }
    if (pm__manifest_merge_entry(manifest, target_name, entry, err, err_size) != 0) {
        cJSON_Delete(entry);
        return -1;
    }
    cJSON_Delete(entry);
    if (unlink(manifest->pending_path) != 0 && errno != ENOENT) {
        snprintf(err, err_size, "cannot remove artwork pending record: %s",
                 strerror(errno));
        return -1;
    }
    (void)pm__fsync_dir(manifest->dir);
    return 0;
}

static int pm__sync_one_script(const pm_context *ctx,
                               pm_artwork_manifest *manifest,
                               pm_artwork_policy policy,
                               const char *script_name,
                               pm_artwork_sync_result *out,
                               char *err,
                               size_t err_size)
{
    char stem[256];
    if (pm__stem_from_script(script_name, stem, sizeof(stem)) != 0 || !stem[0]) {
        out->failed++;
        return 0;
    }
    char target_name[512];
    char target[PM_PATH_MAX];
    if (pm_format(target_name, sizeof(target_name), "%s.png", stem) != 0 ||
        !pm__safe_target_name(target_name) ||
        pm_join(target, sizeof(target), ctx->port_images_dir, target_name) != 0) {
        out->failed++;
        return 0;
    }

    bool target_exists = pm__lstat_regular(target);
    if (target_exists && policy == PM_ARTWORK_MISSING_ONLY) {
        out->skipped_existing++;
        return 0;
    }
    if (target_exists && policy == PM_ARTWORK_MANAGED_REFRESH &&
        !pm__manifest_target_managed(manifest, target_name, target)) {
        out->preserved_custom++;
        return 0;
    }

    pm_artwork_candidate source;
    memset(&source, 0, sizeof(source));
    if (!pm__resolve_candidate(ctx, script_name, stem, &source)) {
        out->missing_source++;
        return 0;
    }
    char item_err[512] = "";
    if (pm__publish_artwork(ctx, manifest, &source, target_name, target,
                            item_err, sizeof(item_err)) != 0) {
        out->failed++;
        snprintf(err, err_size, "%s", item_err[0] ? item_err : "artwork publish failed");
        fprintf(stderr, "PortMaster artwork sync warning: %s -> %s: %s\n",
                source.path, target, err);
        return -1;
    }
    out->synced++;
    return 0;
}

static int pm__artwork_sync_one_source(const pm_context *ctx,
                                       pm_artwork_policy policy,
                                       pm_artwork_sync_result *out,
                                       char *err,
                                       size_t err_size)
{
    if (pm_mkdir_p(ctx->port_images_dir, err, err_size) != 0) {
        return -1;
    }

    pm_artwork_manifest manifest;
    if (pm__manifest_open(ctx, &manifest, err, err_size) != 0) {
        pm__manifest_close(&manifest);
        return -1;
    }

    DIR *dir = opendir(ctx->ports_dir);
    if (!dir) {
        int saved = errno;
        pm__manifest_close(&manifest);
        if (saved == ENOENT) {
            return 0;
        }
        snprintf(err, err_size, "cannot open %s: %s",
                 ctx->ports_dir, strerror(saved));
        return -1;
    }

    int rc = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || !pm__has_suffix_casefold(ent->d_name, ".sh")) {
            continue;
        }
        char script_path[PM_PATH_MAX];
        if (pm_join(script_path, sizeof(script_path), ctx->ports_dir, ent->d_name) != 0 ||
            !pm__lstat_regular(script_path)) {
            continue;
        }
        out->scanned++;
        if (pm__sync_one_script(ctx, &manifest, policy, ent->d_name,
                                out, err, err_size) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    pm__manifest_close(&manifest);
    return rc;
}

int pm_artwork_sync_with_policy(const pm_context *ctx,
                                pm_artwork_policy policy,
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
    if (!ctx || !out ||
        policy < PM_ARTWORK_MISSING_ONLY || policy > PM_ARTWORK_REPLACE_ALL) {
        snprintf(err, err_size, "invalid artwork sync request");
        return -1;
    }

    pm_source_list sources;
    if (pm_sources_resolve(&sources, ctx->platform,
                           ctx->sdcard_path, ctx->roms_path, ctx->images_path,
                           err, err_size) != 0) {
        return -1;
    }
    for (size_t i = 0; i < sources.count; i++) {
        if (!sources.items[i].configured || !sources.items[i].available) {
            continue;
        }
        pm_context source_ctx;
        if (pm_context_for_source(ctx, &sources.items[i], &source_ctx,
                                  err, err_size) != 0 ||
            pm__artwork_sync_one_source(&source_ctx, policy, out,
                                        err, err_size) != 0) {
            return -1;
        }
    }
    return 0;
}

int pm_artwork_sync(const pm_context *ctx,
                    pm_artwork_sync_result *out,
                    char *err,
                    size_t err_size)
{
    return pm_artwork_sync_with_policy(ctx, PM_ARTWORK_MISSING_ONLY,
                                       out, err, err_size);
}
