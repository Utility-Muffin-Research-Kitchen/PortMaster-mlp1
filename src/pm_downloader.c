#include "pm_downloader.h"

#include "pm_sha256.h"
#include "pm_util.h"

#include <curl/curl.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static int curl_ready;

#define PM_FAT32_SINGLE_FILE_LIMIT (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define PM_DOWNLOAD_FREE_SPACE_MARGIN (1024ULL * 1024ULL)

bool pm_download_url_allowed(const char *url, bool allow_http)
{
    if (!url) {
        return false;
    }
    if (strncmp(url, "https://", 8) == 0) {
        return true;
    }
    return allow_http && strncmp(url, "http://", 7) == 0;
}

int pm_download_partial_path(const char *dest_path, char *out, size_t out_size)
{
    return pm_format(out, out_size, "%s.partial", dest_path ? dest_path : "");
}

static int ensure_curl(void)
{
    if (curl_ready) {
        return 0;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return -1;
    }
    curl_ready = 1;
    return 0;
}

static void configure_ca(CURL *curl)
{
    const char *env = getenv("PM_CAINFO");
    if (env && env[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, env);
        return;
    }

    const char *launcher = getenv("UMRK_LAUNCHER_PATH");
    char path[PM_PATH_MAX];
    if (launcher && launcher[0] &&
        pm_join3(path, sizeof(path), launcher, "res", "certs/cacert.pem") == 0 &&
        pm_file_exists(path)) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, path);
        return;
    }

    const char *ca_files[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/cert.pem",
        "/usr/share/ssl/certs/ca-bundle.crt",
    };
    for (size_t i = 0; i < sizeof(ca_files) / sizeof(ca_files[0]); i++) {
        if (pm_file_exists(ca_files[i])) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_files[i]);
            return;
        }
    }
    if (pm_dir_exists("/etc/ssl/certs")) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs");
    }
}

typedef struct {
    FILE *fp;
    uint64_t written;
    uint64_t single_file_limit;
    bool hit_single_file_limit;
    int write_errno;
} pm_file_download;

static uint64_t fat32_single_file_limit(void)
{
    const char *env = getenv("LEAF_PM_FAT32_FILE_LIMIT_BYTES");
    if (env && env[0]) {
        char *end = NULL;
        unsigned long long value = strtoull(env, &end, 10);
        if (end && *end == '\0' && value > 0) {
            return (uint64_t)value;
        }
    }
    return PM_FAT32_SINGLE_FILE_LIMIT;
}

static void parent_path_for(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    if (pm_copy(out, out_size, path) != 0) {
        out[0] = '\0';
        return;
    }
    char *slash = strrchr(out, '/');
    if (!slash) {
        pm_copy(out, out_size, ".");
    } else if (slash == out) {
        out[1] = '\0';
    } else {
        *slash = '\0';
    }
}

static bool mount_type_for_path(const char *path, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    FILE *fp = fopen("/proc/mounts", "rb");
    if (!fp) {
        return false;
    }

    char best_mount[PM_PATH_MAX] = "";
    char best_type[64] = "";
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        char dev[PM_PATH_MAX], mountpoint[PM_PATH_MAX], type[64];
        if (sscanf(line, "%4095s %4095s %63s", dev, mountpoint, type) != 3) {
            continue;
        }
        size_t ml = strlen(mountpoint);
        if (ml == 0) {
            continue;
        }
        if (strncmp(path, mountpoint, ml) == 0 &&
            (path[ml] == '\0' || mountpoint[ml - 1] == '/' || path[ml] == '/') &&
            ml > strlen(best_mount)) {
            pm_copy(best_mount, sizeof(best_mount), mountpoint);
            pm_copy(best_type, sizeof(best_type), type);
        }
    }
    fclose(fp);

    if (!best_type[0]) {
        return false;
    }
    pm_copy(out, out_size, best_type);
    return true;
}

static bool path_is_vfat(const char *path)
{
    char type[64];
    return mount_type_for_path(path, type, sizeof(type)) && strcmp(type, "vfat") == 0;
}

static int available_bytes_for_path(const char *path, uint64_t *out)
{
    if (out) {
        *out = 0;
    }
    char parent[PM_PATH_MAX];
    parent_path_for(path, parent, sizeof(parent));
    if (!parent[0]) {
        return -1;
    }
    struct statvfs st;
    if (statvfs(parent, &st) != 0) {
        return -1;
    }
    uint64_t block = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    if (block == 0 || st.f_bavail > UINT64_MAX / block) {
        return -1;
    }
    if (out) {
        *out = (uint64_t)st.f_bavail * block;
    }
    return 0;
}

static void log_download_detail(const char *dest_path, const char *detail)
{
    const char *leaf = dest_path ? strstr(dest_path, "/.leaf/") : NULL;
    if (!leaf || !detail || !detail[0]) {
        return;
    }

    size_t leaf_len = (size_t)(leaf - dest_path) + strlen("/.leaf");
    char leaf_dir[PM_PATH_MAX];
    char log_dir[PM_PATH_MAX];
    char log_path[PM_PATH_MAX];
    if (leaf_len >= sizeof(leaf_dir)) {
        return;
    }
    memcpy(leaf_dir, dest_path, leaf_len);
    leaf_dir[leaf_len] = '\0';
    if (pm_join(log_dir, sizeof(log_dir), leaf_dir, "logs") != 0 ||
        pm_join(log_path, sizeof(log_path), log_dir, "download.log") != 0 ||
        pm_mkdir_p(log_dir, NULL, 0) != 0) {
        return;
    }

    FILE *fp = fopen(log_path, "ab");
    if (!fp) {
        return;
    }
    time_t now = time(NULL);
    fprintf(fp, "%lld ", (long long)now);
    for (const char *p = detail; *p; p++) {
        fputc((*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p, fp);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int preflight_download_target(const pm_download_spec *spec, char *err, size_t err_size)
{
    if (!spec || !spec->dest_path || spec->expected_size == 0) {
        return 0;
    }

    uint64_t limit = fat32_single_file_limit();
    bool vfat = path_is_vfat(spec->dest_path);
    if (vfat && spec->expected_size >= limit) {
        snprintf(err, err_size,
                 "download target is vfat/FAT32 and expected file is %llu bytes; single files >= %llu bytes are unsupported",
                 (unsigned long long)spec->expected_size,
                 (unsigned long long)limit);
        return -1;
    }

    uint64_t available = 0;
    uint64_t needed = spec->expected_size;
    if (needed <= UINT64_MAX - PM_DOWNLOAD_FREE_SPACE_MARGIN) {
        needed += PM_DOWNLOAD_FREE_SPACE_MARGIN;
    }
    if (available_bytes_for_path(spec->dest_path, &available) == 0 &&
        needed > available) {
        snprintf(err, err_size,
                 "not enough free space for download: need %llu bytes plus margin, available %llu bytes",
                 (unsigned long long)spec->expected_size,
                 (unsigned long long)available);
        return -1;
    }
    return 0;
}

static void classify_write_failure(const pm_download_spec *spec,
                                   const pm_file_download *download,
                                   char *err,
                                   size_t err_size)
{
    uint64_t limit = fat32_single_file_limit();
    if (download && download->hit_single_file_limit) {
        snprintf(err, err_size,
                 "download stopped at the FAT32 single-file limit (%llu bytes); this port/runtime needs a larger single file than stock MLP1 vfat supports",
                 (unsigned long long)limit);
        return;
    }
    if (download && download->write_errno == EFBIG) {
        snprintf(err, err_size,
                 "download failed because the target filesystem rejected a large file (EFBIG); FAT32/vfat cannot store single files >= %llu bytes",
                 (unsigned long long)limit);
        return;
    }
    if (download && download->write_errno == ENOSPC) {
        snprintf(err, err_size,
                 "download failed because the SD card ran out of free space (ENOSPC)");
        return;
    }
    if (download && download->write_errno) {
        snprintf(err, err_size, "download write failed: %s", strerror(download->write_errno));
        return;
    }
    snprintf(err, err_size, "download failed: %s", spec && spec->url ? spec->url : "(unknown)");
}

static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    pm_file_download *dl = (pm_file_download *)userdata;
    if (!dl || !dl->fp || (size != 0 && nmemb > SIZE_MAX / size)) {
        return 0;
    }
    size_t len = size * nmemb;
    if (len == 0) {
        return 0;
    }
    if (dl->single_file_limit > 0) {
        uint64_t max_allowed = dl->single_file_limit - 1;
        if (dl->written > max_allowed || len > max_allowed - dl->written) {
            dl->hit_single_file_limit = true;
            dl->write_errno = EFBIG;
            return 0;
        }
    }
    size_t wrote = fwrite(ptr, 1, len, dl->fp);
    dl->written += (uint64_t)wrote;
    if (wrote != len && ferror(dl->fp)) {
        dl->write_errno = errno ? errno : EIO;
    }
    return wrote;
}

typedef struct {
    char *data;
    size_t size;
    size_t cap;
    size_t max;
    bool too_large;
} pm_text_download;

static size_t write_text_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    pm_text_download *dl = (pm_text_download *)userdata;
    size_t len = size * nmemb;
    if (!dl || dl->too_large) {
        return 0;
    }
    if (len > dl->max || dl->size > dl->max - len) {
        dl->too_large = true;
        return 0;
    }
    if (dl->size + len + 1 > dl->cap) {
        size_t next = dl->cap ? dl->cap : 4096;
        while (next < dl->size + len + 1) {
            next *= 2;
            if (next > dl->max + 1) {
                next = dl->max + 1;
                break;
            }
        }
        char *new_data = (char *)realloc(dl->data, next);
        if (!new_data) {
            return 0;
        }
        dl->data = new_data;
        dl->cap = next;
    }
    memcpy(dl->data + dl->size, ptr, len);
    dl->size += len;
    dl->data[dl->size] = '\0';
    return len;
}

int pm_download_file(const pm_download_spec *spec, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    bool want_sha = spec && spec->expected_sha256 && spec->expected_sha256[0];
    bool want_md5 = spec && spec->expected_md5 && spec->expected_md5[0];
    if (!spec || !spec->url || !spec->dest_path || (!want_sha && !want_md5)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "download spec is incomplete");
        }
        return -1;
    }
    if (!pm_download_url_allowed(spec->url, spec->allow_http)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "URL is not allowed: %s", spec->url);
        }
        return -1;
    }
    if (ensure_curl() != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "curl initialization failed");
        }
        return -1;
    }

    char partial[PM_PATH_MAX];
    if (pm_download_partial_path(spec->dest_path, partial, sizeof(partial)) != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "download destination path too long");
        }
        return -1;
    }

    char preflight_err[256];
    if (preflight_download_target(spec, preflight_err, sizeof(preflight_err)) != 0) {
        log_download_detail(spec->dest_path, preflight_err);
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", preflight_err);
        }
        return -1;
    }

    unlink(partial);
    FILE *fp = fopen(partial, "wb");
    if (!fp) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot create %s: %s", partial, strerror(errno));
        }
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        unlink(partial);
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "curl_easy_init failed");
        }
        return -1;
    }

    char curl_error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, spec->url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    configure_ca(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    pm_file_download download = {
        .fp = fp,
        .single_file_limit = path_is_vfat(spec->dest_path) ? fat32_single_file_limit() : 0,
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "portmaster-mlp1/0.1");

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (fclose(fp) != 0 && download.write_errno == 0) {
        download.write_errno = errno ? errno : EIO;
    }

    if (download.write_errno || rc != CURLE_OK || http < 200 || http >= 300) {
        unlink(partial);
        if (err && err_size > 0) {
            if (download.write_errno) {
                classify_write_failure(spec, &download, err, err_size);
            } else {
                snprintf(err, err_size, "download failed: curl=%d http=%ld %s",
                         (int)rc, http, curl_error);
            }
            log_download_detail(spec->dest_path, err);
        }
        return -1;
    }

    off_t size = pm_file_size(partial);
    if (spec->expected_size > 0 && size != (off_t)spec->expected_size) {
        unlink(partial);
        if (err && err_size > 0) {
            snprintf(err, err_size, "size mismatch: got %lld expected %llu",
                     (long long)size, (unsigned long long)spec->expected_size);
        }
        return -1;
    }

    if (want_sha) {
        char sha[65], sha_err[128];
        if (pm_sha256_file_hex(partial, sha, sha_err, sizeof(sha_err)) != 0 ||
            strcmp(sha, spec->expected_sha256) != 0) {
            unlink(partial);
            if (err && err_size > 0) {
                snprintf(err, err_size, "sha256 mismatch: got %s expected %s",
                         sha[0] ? sha : sha_err, spec->expected_sha256);
            }
            return -1;
        }
    }

    if (want_md5) {
        char md5[33], md5_err[128];
        if (pm_md5_file_hex(partial, md5, md5_err, sizeof(md5_err)) != 0 ||
            strcmp(md5, spec->expected_md5) != 0) {
            unlink(partial);
            if (err && err_size > 0) {
                snprintf(err, err_size, "md5 mismatch: got %s expected %s",
                         md5[0] ? md5 : md5_err, spec->expected_md5);
            }
            return -1;
        }
    }

    if (rename(partial, spec->dest_path) != 0) {
        unlink(partial);
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot promote %s: %s", spec->dest_path, strerror(errno));
        }
        return -1;
    }
    return 0;
}

int pm_download_text_with_timeout(const char *url, size_t max_bytes, bool allow_http,
                                  long timeout_seconds,
                                  char **out_text, char *err, size_t err_size)
{
    if (out_text) {
        *out_text = NULL;
    }
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!out_text || !url || !url[0] || max_bytes == 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "text download spec is incomplete");
        }
        return -1;
    }
    if (!pm_download_url_allowed(url, allow_http)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "URL is not allowed: %s", url);
        }
        return -1;
    }
    if (ensure_curl() != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "curl initialization failed");
        }
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "curl_easy_init failed");
        }
        return -1;
    }

    pm_text_download dl = {
        .max = max_bytes,
    };
    char curl_error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    configure_ca(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (timeout_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_text_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "portmaster-mlp1/0.1");

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http < 200 || http >= 300 || dl.too_large) {
        free(dl.data);
        if (err && err_size > 0) {
            snprintf(err, err_size, "text download failed: curl=%d http=%ld %s%s",
                     (int)rc, http, curl_error, dl.too_large ? " response too large" : "");
        }
        return -1;
    }
    if (!dl.data) {
        dl.data = (char *)calloc(1, 1);
        if (!dl.data) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "%s", "out of memory");
            }
            return -1;
        }
    }
    *out_text = dl.data;
    return 0;
}

int pm_download_text(const char *url, size_t max_bytes, bool allow_http,
                     char **out_text, char *err, size_t err_size)
{
    return pm_download_text_with_timeout(url, max_bytes, allow_http, 0,
                                         out_text, err, err_size);
}
