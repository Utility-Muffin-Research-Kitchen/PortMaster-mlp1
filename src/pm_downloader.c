#include "pm_downloader.h"

#include "pm_sha256.h"
#include "pm_util.h"

#include <curl/curl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int curl_ready;

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

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

int pm_download_file(const pm_download_spec *spec, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!spec || !spec->url || !spec->dest_path || !spec->expected_sha256) {
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "portmaster-mlp1/0.1");

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (rc != CURLE_OK || http < 200 || http >= 300) {
        unlink(partial);
        if (err && err_size > 0) {
            snprintf(err, err_size, "download failed: curl=%d http=%ld %s",
                     (int)rc, http, curl_error);
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

    if (rename(partial, spec->dest_path) != 0) {
        unlink(partial);
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot promote %s: %s", spec->dest_path, strerror(errno));
        }
        return -1;
    }
    return 0;
}
