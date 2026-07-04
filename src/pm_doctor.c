#include "pm_doctor.h"

#include "pm_env_snapshot.h"
#include "pm_launcher.h"

#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    PM_CHECK_OK = 0,
    PM_CHECK_WARN,
    PM_CHECK_FAIL,
    PM_CHECK_INFO,
    PM_CHECK_APP_LOCAL_OK,
    PM_CHECK_NOT_APP_FIXABLE,
    PM_CHECK_UNKNOWN,
} pm_check_status;

#define PM_MOUNT_PROBE_MARKER "/tmp/leaf-pm-mount-probe-ok"
#define PM_SQUASHFS_MAGIC 0x73717368u

static const char *status_text(pm_check_status status)
{
    switch (status) {
        case PM_CHECK_OK: return "OK";
        case PM_CHECK_WARN: return "WARN";
        case PM_CHECK_FAIL: return "FAIL";
        case PM_CHECK_INFO: return "INFO";
        case PM_CHECK_APP_LOCAL_OK: return "APP";
        case PM_CHECK_NOT_APP_FIXABLE: return "N/A";
        case PM_CHECK_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

static const char *status_json(pm_check_status status)
{
    switch (status) {
        case PM_CHECK_OK: return "ok";
        case PM_CHECK_WARN: return "warn";
        case PM_CHECK_FAIL: return "fail";
        case PM_CHECK_INFO: return "info";
        case PM_CHECK_APP_LOCAL_OK: return "app_local_ok";
        case PM_CHECK_NOT_APP_FIXABLE: return "not_app_fixable";
        case PM_CHECK_UNKNOWN: return "unknown";
        default: return "unknown";
    }
}

static int appendf(char *out, size_t out_size, const char *fmt, ...)
{
    size_t used = strlen(out);
    if (used >= out_size) {
        return -1;
    }

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(out + used, out_size - used, fmt, ap);
    va_end(ap);
    return (written < 0 || (size_t)written >= out_size - used) ? -1 : 0;
}

static void append_line(pm_doctor_report *r, const char *status, const char *label, const char *detail)
{
    (void)appendf(r->text, sizeof(r->text), "%s %s\n%s\n\n",
                  status, label, detail ? detail : "");
}

static void add_check(pm_doctor_report *r,
                      cJSON *checks,
                      const char *id,
                      pm_check_status status,
                      const char *severity,
                      const char *summary,
                      const char *detail)
{
    if (status == PM_CHECK_FAIL) {
        r->issues++;
    } else if (status == PM_CHECK_WARN) {
        r->warnings++;
    }

    appendf(r->text, sizeof(r->text), "%s %s [%s]\n%s%s%s\n\n",
            status_text(status),
            id ? id : "check",
            severity ? severity : "info",
            summary ? summary : "",
            detail && detail[0] ? "\n" : "",
            detail && detail[0] ? detail : "");

    if (!checks) {
        return;
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return;
    }
    cJSON_AddStringToObject(item, "id", id ? id : "check");
    cJSON_AddStringToObject(item, "status", status_json(status));
    cJSON_AddStringToObject(item, "severity", severity ? severity : "info");
    cJSON_AddStringToObject(item, "summary", summary ? summary : "");
    cJSON_AddStringToObject(item, "detail", detail ? detail : "");
    cJSON_AddItemToArray(checks, item);
}

static bool truthy_env(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
}

static void trim_trailing(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' ||
                       s[len - 1] == '\r' ||
                       s[len - 1] == ' ' ||
                       s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static const char *squashfs_compression_name(unsigned id)
{
    switch (id) {
        case 1: return "gzip";
        case 2: return "lzma";
        case 3: return "lzo";
        case 4: return "xz";
        case 5: return "lz4";
        case 6: return "zstd";
        default: return "unknown";
    }
}

static bool config_has_enabled(const char *config, const char *name)
{
    char y[64];
    char m[64];
    if (!config || !name ||
        pm_format(y, sizeof(y), "%s=y", name) != 0 ||
        pm_format(m, sizeof(m), "%s=m", name) != 0) {
        return false;
    }
    return contains(config, y) || contains(config, m);
}

typedef enum {
    PM_KERNEL_SUPPORT_NO = 0,
    PM_KERNEL_SUPPORT_YES,
    PM_KERNEL_SUPPORT_UNKNOWN,
} pm_kernel_support;

static pm_kernel_support squashfs_kernel_support(unsigned id,
                                                 const char *config,
                                                 bool has_readable_config,
                                                 bool has_squashfs)
{
    if (!has_squashfs) {
        return PM_KERNEL_SUPPORT_NO;
    }
    if (!has_readable_config) {
        return PM_KERNEL_SUPPORT_UNKNOWN;
    }

    const char *symbol = NULL;
    switch (id) {
        case 1: symbol = "CONFIG_SQUASHFS_ZLIB"; break;
        case 2: symbol = "CONFIG_SQUASHFS_LZMA"; break;
        case 3: symbol = "CONFIG_SQUASHFS_LZO"; break;
        case 4: symbol = "CONFIG_SQUASHFS_XZ"; break;
        case 5: symbol = "CONFIG_SQUASHFS_LZ4"; break;
        case 6: symbol = "CONFIG_SQUASHFS_ZSTD"; break;
        default: return PM_KERNEL_SUPPORT_NO;
    }

    return config_has_enabled(config, symbol) ? PM_KERNEL_SUPPORT_YES
                                             : PM_KERNEL_SUPPORT_NO;
}

static int read_squashfs_compression_id(const char *path,
                                        unsigned *id,
                                        char *err,
                                        size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (id) {
        *id = 0;
    }
    if (!path || !id) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "missing squashfs path");
        }
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "open failed: %s", strerror(errno));
        }
        return -1;
    }

    unsigned char header[22];
    size_t got = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    if (got != sizeof(header)) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "short squashfs header");
        }
        return -1;
    }

    uint32_t magic = (uint32_t)header[0] |
                     ((uint32_t)header[1] << 8) |
                     ((uint32_t)header[2] << 16) |
                     ((uint32_t)header[3] << 24);
    if (magic != PM_SQUASHFS_MAGIC) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "bad squashfs magic 0x%08x", magic);
        }
        return -1;
    }

    *id = (unsigned)header[20] | ((unsigned)header[21] << 8);
    return 0;
}

static int shell_quote(char *out, size_t out_size, const char *value)
{
    if (!out || out_size == 0) {
        return -1;
    }
    size_t used = 0;
    if (used + 1 >= out_size) {
        return -1;
    }
    out[used++] = '\'';
    for (const char *p = value ? value : ""; *p; p++) {
        if (*p == '\'') {
            const char *escape = "'\\''";
            size_t n = strlen(escape);
            if (n >= out_size - used) {
                return -1;
            }
            memcpy(out + used, escape, n);
            used += n;
        } else {
            if (used + 1 >= out_size) {
                return -1;
            }
            out[used++] = *p;
        }
    }
    if (used + 2 > out_size) {
        return -1;
    }
    out[used++] = '\'';
    out[used] = '\0';
    return 0;
}

static int capture_shell(const char *command, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!command || !out || out_size == 0) {
        return -1;
    }

    FILE *fp = popen(command, "r");
    if (!fp) {
        return -1;
    }

    size_t used = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (n >= out_size - used) {
            n = out_size - used - 1;
        }
        if (n > 0) {
            memcpy(out + used, buf, n);
            used += n;
            out[used] = '\0';
        }
        if (used + 1 >= out_size) {
            break;
        }
    }

    int rc = pclose(fp);
    trim_trailing(out);
    return rc;
}

static bool path_contains_dir(const char *path, const char *dir)
{
    if (!path || !dir || !dir[0]) {
        return false;
    }
    size_t dir_len = strlen(dir);
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == dir_len && strncmp(p, dir, len) == 0) {
            return true;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static bool executable_file(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static bool path_exists_any(const char *path)
{
    struct stat st;
    return path && lstat(path, &st) == 0;
}

static bool find_command(const char *name, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!name || !name[0]) {
        return false;
    }
    if (strchr(name, '/')) {
        if (executable_file(name)) {
            return pm_copy(out, out_size, name) == 0;
        }
        return false;
    }

    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        path = "/usr/bin:/usr/sbin:/bin:/sbin";
    }

    const char *p = path;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char dir[PM_PATH_MAX];
        if (len == 0) {
            pm_copy(dir, sizeof(dir), ".");
        } else if (len < sizeof(dir)) {
            memcpy(dir, p, len);
            dir[len] = '\0';
        } else {
            dir[0] = '\0';
        }
        if (dir[0]) {
            char candidate[PM_PATH_MAX];
            if (pm_join(candidate, sizeof(candidate), dir, name) == 0 &&
                executable_file(candidate)) {
                return pm_copy(out, out_size, candidate) == 0;
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static void check_setup_baseline(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    add_check(r, checks, "setup.platform", PM_CHECK_INFO, "info",
              ctx->platform, NULL);

    if (ctx->lock_loaded) {
        char summary[1024];
        pm_lock_summary(&ctx->lock, summary, sizeof(summary));
        add_check(r, checks, "setup.portmaster_lock", PM_CHECK_OK, "required",
                  "Stable PortMaster lock loaded", summary);
    } else {
        add_check(r, checks, "setup.portmaster_lock", PM_CHECK_FAIL, "required",
                  "Stable PortMaster lock is not loaded", ctx->lock_path);
    }

    if (ctx->runtime_lock_loaded) {
        char summary[1024];
        pm_ui_runtime_lock_summary(&ctx->runtime_lock, summary, sizeof(summary));
        add_check(r, checks, "setup.ui_runtime_lock", PM_CHECK_OK, "required",
                  "PortMaster UI runtime lock loaded", summary);
    } else {
        add_check(r, checks, "setup.ui_runtime_lock", PM_CHECK_FAIL, "required",
                  "PortMaster UI runtime lock is not loaded", ctx->runtime_lock_path);
    }

    add_check(r, checks, "setup.manager_data",
              pm_dir_exists(ctx->data_dir) ? PM_CHECK_OK : PM_CHECK_WARN,
              "required",
              pm_dir_exists(ctx->data_dir) ? "Manager data directory exists" : "Manager data directory missing",
              ctx->data_dir);

    add_check(r, checks, "setup.portmaster_install",
              pm_dir_exists(ctx->portmaster_dir) ? PM_CHECK_OK : PM_CHECK_WARN,
              "required",
              pm_dir_exists(ctx->portmaster_dir) ? "Upstream PortMaster tree exists" : "Upstream PortMaster tree missing",
              ctx->portmaster_dir);

    char runtime_python[PM_PATH_MAX];
    bool uses_system_python = false;
    bool has_runtime = pm_portmaster_runtime_available(ctx, runtime_python,
                                                       sizeof(runtime_python),
                                                       &uses_system_python);
    add_check(r, checks, "setup.python_runtime",
              has_runtime ? (uses_system_python ? PM_CHECK_OK : PM_CHECK_APP_LOCAL_OK) : PM_CHECK_FAIL,
              "required",
              has_runtime ? "Python runtime available for managed GUI" : "Python runtime missing",
              has_runtime ? runtime_python : "Install or repair the managed UI runtime.");

    add_check(r, checks, "setup.ports_dir",
              pm_dir_exists(ctx->ports_dir) ? PM_CHECK_OK : PM_CHECK_WARN,
              "required",
              pm_dir_exists(ctx->ports_dir) ? "Ports directory exists" : "Ports directory missing",
              ctx->ports_dir);

    add_check(r, checks, "setup.port_images_dir",
              pm_dir_exists(ctx->port_images_dir) ? PM_CHECK_OK : PM_CHECK_WARN,
              "recommended",
              pm_dir_exists(ctx->port_images_dir) ? "Port artwork directory exists" : "Port artwork directory missing",
              ctx->port_images_dir);
}

static void check_bash(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    (void)ctx;
    const char *bash = "/bin/bash";
    if (!executable_file(bash)) {
        bash = "/usr/bin/bash";
    }
    if (!executable_file(bash)) {
        add_check(r, checks, "shell.bash", PM_CHECK_FAIL, "required",
                  "GNU Bash is missing", "Port scripts use #!/bin/bash and Bash-specific syntax.");
        return;
    }

    char qbash[PM_PATH_MAX + 8];
    char cmd[PM_PATH_MAX + 256];
    char output[512];
    char version[512];
    shell_quote(qbash, sizeof(qbash), bash);
    pm_format(cmd, sizeof(cmd), "%s --version 2>&1 | head -n 1", qbash);
    capture_shell(cmd, version, sizeof(version));

    char feature_cmd[PM_PATH_MAX + 512];
    pm_format(feature_cmd, sizeof(feature_cmd),
              "%s -c 'arr=(A B); [[ \"${arr[0],,}\" == a ]] && cat <(printf ok) >/dev/null' >/dev/null 2>&1",
              qbash);
    int feature_rc = capture_shell(feature_cmd, output, sizeof(output));
    pm_check_status status = contains(version, "GNU bash") || contains(version, "GNU Bash")
                                 ? PM_CHECK_OK
                                 : PM_CHECK_WARN;
    if (feature_rc != 0) {
        status = PM_CHECK_FAIL;
    }

    char detail[1024];
    pm_format(detail, sizeof(detail), "%s\nFeature smoke: %s",
              version[0] ? version : bash,
              feature_rc == 0 ? "arrays, [[ ]], lowercase expansion, and process substitution passed"
                              : "failed");
    add_check(r, checks, "shell.bash", status, "required",
              status == PM_CHECK_OK ? "GNU Bash is available" : "Bash compatibility issue",
              detail);
}

typedef struct {
    const char *name;
    const char *severity;
    bool require_gnu;
} pm_tool_spec;

static void check_tool(const pm_tool_spec *spec, pm_doctor_report *r, cJSON *checks)
{
    char path[PM_PATH_MAX];
    if (!find_command(spec->name, path, sizeof(path))) {
        pm_check_status status;
        if (strcmp(spec->severity, "required") == 0) {
            status = PM_CHECK_FAIL;
        } else if (strcmp(spec->severity, "nice_to_have") == 0) {
            status = PM_CHECK_INFO;
        } else {
            status = PM_CHECK_WARN;
        }
        char id[96];
        pm_format(id, sizeof(id), "tool.%s", spec->name);
        add_check(r, checks, id, status, spec->severity,
                  status == PM_CHECK_INFO ? "Optional developer tool missing from effective PATH"
                                          : "Tool missing from effective PATH",
                  spec->name);
        return;
    }

    char cmd[256];
    char output[512];
    pm_format(cmd, sizeof(cmd), "%s --version 2>&1 | head -n 1", spec->name);
    capture_shell(cmd, output, sizeof(output));

    pm_check_status status = PM_CHECK_OK;
    const char *summary = "Tool available";
    if (spec->require_gnu) {
        bool says_not_gnu = contains(output, "not GNU") || contains(output, "Not GNU");
        if (contains(output, "GNU") && !says_not_gnu) {
            status = PM_CHECK_OK;
            summary = "GNU-compatible tool available";
        } else if (contains(output, "BusyBox") || output[0]) {
            status = PM_CHECK_FAIL;
            summary = "Tool is present but not GNU-compatible";
        } else {
            status = PM_CHECK_WARN;
            summary = "Tool is present but GNU compatibility is unknown";
        }
    }

    char id[96];
    char detail[1024];
    pm_format(id, sizeof(id), "tool.%s", spec->name);
    pm_format(detail, sizeof(detail), "%s%s%s",
              path,
              output[0] ? "\n" : "",
              output[0] ? output : "");
    add_check(r, checks, id, status, spec->severity, summary, detail);
}

static void check_tools(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char tools_dir[PM_PATH_MAX];
    bool have_tools_dir = pm_join3(tools_dir, sizeof(tools_dir), ctx->data_dir,
                                   "compat/tools", "aarch64/bin") == 0 &&
                          pm_dir_exists(tools_dir);
    const char *path = getenv("PATH");
    bool tools_on_path = have_tools_dir && path_contains_dir(path, tools_dir);
    char detail[PM_PATH_MAX + 128];
    pm_format(detail, sizeof(detail), "%s\nEffective PATH: %s",
              have_tools_dir ? tools_dir : "not installed",
              path ? path : "");
    add_check(r, checks, "tool.leaf_tools_path",
              have_tools_dir ? (tools_on_path ? PM_CHECK_APP_LOCAL_OK : PM_CHECK_WARN) : PM_CHECK_WARN,
              "required",
              tools_on_path ? "Leaf PortMaster tool directory is on PATH"
                            : "Leaf PortMaster tool directory is not on effective PATH",
              detail);

    const pm_tool_spec specs[] = {
        { "sed", "required", true },
        { "find", "required", true },
        { "grep", "required", true },
        { "awk", "required", false },
        { "tar", "required", false },
        { "unzip", "required", false },
        { "gzip", "required", false },
        { "gunzip", "required", false },
        { "mount", "required", false },
        { "umount", "required", false },
        { "md5sum", "required", false },
        { "xargs", "required", false },
        { "sudo", "recommended", false },
        { "doas", "recommended", false },
        { "systemctl", "required", false },
        { "dos2unix", "recommended", false },
        { "leaf-squashfs-check", "recommended", false },
        { "squashfuse", "recommended", false },
        { "xdelta3", "recommended", false },
        { "dialog", "recommended", false },
        { "rsync", "recommended", false },
        { "7z", "recommended", false },
        { "7za", "recommended", false },
        { "innoextract", "recommended", false },
        { "getconf", "nice_to_have", false },
        { "ldd", "nice_to_have", false },
        { "readelf", "nice_to_have", false },
        { "file", "nice_to_have", false },
        { "strace", "nice_to_have", false },
        { "perf", "nice_to_have", false },
    };
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        check_tool(&specs[i], r, checks);
    }
}

static void check_python(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char path[PM_PATH_MAX];
    if (find_command("python3", path, sizeof(path))) {
        char output[512];
        int rc = capture_shell("python3 -c 'import sys,lzma; print(sys.version.split()[0]); print(\"lzma ok\")' 2>&1",
                               output, sizeof(output));
        add_check(r, checks, "python.system_lzma",
                  rc == 0 ? PM_CHECK_OK : PM_CHECK_FAIL,
                  "required",
                  rc == 0 ? "System python3 imports lzma" : "System python3 cannot import lzma",
                  output[0] ? output : path);
    } else {
        add_check(r, checks, "python.system_lzma", PM_CHECK_NOT_APP_FIXABLE,
                  "required",
                  "Stock system python3 is missing",
                  "Managed PortMaster can still work through Leaf's UI runtime.");
    }

    char managed[PM_PATH_MAX];
    if (pm_join3(managed, sizeof(managed), ctx->runtime_dir, "bin", "python3") != 0 ||
        !pm_file_exists(managed)) {
        add_check(r, checks, "python.managed_lzma", PM_CHECK_FAIL,
                  "required",
                  "Managed Python runtime is missing",
                  ctx->runtime_dir);
        return;
    }

    char qruntime[PM_PATH_MAX + 16];
    char qpython[PM_PATH_MAX + 16];
    char cmd[PM_PATH_MAX * 5];
    char output[512];
    if (shell_quote(qruntime, sizeof(qruntime), ctx->runtime_dir) != 0 ||
        shell_quote(qpython, sizeof(qpython), managed) != 0 ||
        pm_format(cmd, sizeof(cmd),
                  "LD_LIBRARY_PATH=%s/lib:${LD_LIBRARY_PATH:-} PYTHONHOME=%s PYTHONPATH=%s/lib/python3.10:%s/lib/python3.10/site-packages:%s/lib %s -c 'import sys,lzma; print(sys.version.split()[0]); print(\"lzma ok\")' 2>&1",
                  qruntime, qruntime, qruntime, qruntime, qruntime, qpython) != 0) {
        add_check(r, checks, "python.managed_lzma", PM_CHECK_FAIL,
                  "required", "Managed Python command is too long", managed);
        return;
    }

    int rc = capture_shell(cmd, output, sizeof(output));
    add_check(r, checks, "python.managed_lzma",
              rc == 0 ? PM_CHECK_APP_LOCAL_OK : PM_CHECK_FAIL,
              "required",
              rc == 0 ? "Managed Python runtime imports lzma" : "Managed Python runtime failed",
              output[0] ? output : managed);
}

static bool read_file_contains(const char *path, const char *needle)
{
    char err[128];
    char *text = pm_read_text_file(path, 1024 * 1024, err, sizeof(err));
    if (!text) {
        return false;
    }
    bool found = contains(text, needle);
    free(text);
    return found;
}

static int count_loop_nodes(void)
{
    DIR *dir = opendir("/dev");
    if (!dir) {
        return -1;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "loop", 4) != 0) {
            continue;
        }
        const char *p = ent->d_name + 4;
        if (!isdigit((unsigned char)*p)) {
            continue;
        }
        bool numeric = true;
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static bool port_runtime_helper_available(const pm_context *ctx, char *out, size_t out_size)
{
    char path[PM_PATH_MAX];
    if (!ctx ||
        pm_join3(path, sizeof(path), ctx->pak_dir,
                 "scripts", "prepare-port-runtime.sh") != 0 ||
        !executable_file(path)) {
        if (out && out_size > 0) {
            out[0] = '\0';
        }
        return false;
    }
    return pm_copy(out, out_size, path) == 0;
}

static bool find_runtime_squashfs(const pm_context *ctx, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!ctx || !out || out_size == 0) {
        return false;
    }

    char libs_dir[PM_PATH_MAX];
    if (pm_join(libs_dir, sizeof(libs_dir), ctx->portmaster_dir, "libs") != 0) {
        return false;
    }

    DIR *dir = opendir(libs_dir);
    if (!dir) {
        return false;
    }

    char best[PM_PATH_MAX] = "";
    off_t best_size = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 9 || strcmp(ent->d_name + len - 9, ".squashfs") != 0) {
            continue;
        }

        char candidate[PM_PATH_MAX];
        if (pm_join(candidate, sizeof(candidate), libs_dir, ent->d_name) != 0) {
            continue;
        }

        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (!best[0] || st.st_size < best_size) {
            pm_copy(best, sizeof(best), candidate);
            best_size = st.st_size;
        }
    }
    closedir(dir);

    return best[0] && pm_copy(out, out_size, best) == 0;
}

static void check_runtime_squashfs_formats(const pm_context *ctx,
                                           pm_doctor_report *r,
                                           cJSON *checks,
                                           const char *squashfs_config,
                                           bool has_readable_config,
                                           bool has_squashfs)
{
    char libs_dir[PM_PATH_MAX];
    if (!ctx || pm_join(libs_dir, sizeof(libs_dir), ctx->portmaster_dir, "libs") != 0) {
        add_check(r, checks, "kernel.squashfs_runtime_formats", PM_CHECK_UNKNOWN,
                  "required",
                  "Could not resolve PortMaster runtime libs directory",
                  ctx ? ctx->portmaster_dir : "");
        return;
    }

    DIR *dir = opendir(libs_dir);
    if (!dir) {
        add_check(r, checks, "kernel.squashfs_runtime_formats", PM_CHECK_INFO,
                  "required",
                  "No installed PortMaster runtime squashfs images found",
                  libs_dir);
        return;
    }

    int total = 0;
    int unsupported = 0;
    int unknown_support = 0;
    int unreadable = 0;
    char detail[16384] = "";
    bool truncated = false;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 9 || strcmp(ent->d_name + len - 9, ".squashfs") != 0) {
            continue;
        }

        char path[PM_PATH_MAX];
        if (pm_join(path, sizeof(path), libs_dir, ent->d_name) != 0) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        total++;
        unsigned compression_id = 0;
        char err[128];
        if (read_squashfs_compression_id(path, &compression_id, err, sizeof(err)) != 0) {
            unreadable++;
            if (appendf(detail, sizeof(detail), "%s: unreadable (%s)\n",
                        ent->d_name, err[0] ? err : "unknown error") != 0) {
                truncated = true;
                break;
            }
            continue;
        }

        pm_kernel_support support = squashfs_kernel_support(compression_id,
                                                            squashfs_config,
                                                            has_readable_config,
                                                            has_squashfs);
        const char *support_text = "kernel missing";
        if (support == PM_KERNEL_SUPPORT_YES) {
            support_text = "kernel ok";
        } else if (support == PM_KERNEL_SUPPORT_UNKNOWN) {
            support_text = "kernel support unknown";
            unknown_support++;
        } else {
            unsupported++;
        }

        if (appendf(detail, sizeof(detail), "%s: %s (id %u, %s)\n",
                    ent->d_name,
                    squashfs_compression_name(compression_id),
                    compression_id,
                    support_text) != 0) {
            truncated = true;
            break;
        }
    }
    closedir(dir);

    if (truncated) {
        appendf(detail, sizeof(detail), "... detail truncated\n");
    }

    if (total == 0) {
        add_check(r, checks, "kernel.squashfs_runtime_formats", PM_CHECK_INFO,
                  "required",
                  "No installed PortMaster runtime squashfs images found",
                  libs_dir);
        return;
    }

    pm_check_status status = PM_CHECK_OK;
    const char *summary = "All installed runtime squashfs images use kernel-supported compression";
    if (unsupported > 0 || unreadable > 0) {
        status = PM_CHECK_FAIL;
        summary = "One or more installed runtime squashfs images cannot be proven mountable";
    } else if (unknown_support > 0) {
        status = PM_CHECK_WARN;
        summary = "Installed runtime squashfs formats detected, but kernel decompressor coverage is unknown";
    }

    char full_detail[sizeof(detail) + PM_PATH_MAX + 160];
    pm_format(full_detail, sizeof(full_detail),
              "libs: %s\nimages=%d unsupported=%d unreadable=%d unknown=%d\n%s",
              libs_dir, total, unsupported, unreadable, unknown_support, detail);
    add_check(r, checks, "kernel.squashfs_runtime_formats", status,
              "required", summary, full_detail);
}

static int doctor_loop_stress_count(void)
{
    const char *value = getenv("LEAF_PM_DOCTOR_LOOP_STRESS_COUNT");
    if (!value || !value[0]) {
        return 16;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1) {
        return 16;
    }
    if (parsed > 64) {
        return 64;
    }
    return (int)parsed;
}

static void check_mounts_and_kernel(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char squashfs_config[2048] = "";
    bool has_squashfs_config = false;
    bool has_readable_squashfs_config = pm_file_exists("/proc/config.gz");
    if (has_readable_squashfs_config) {
        capture_shell("zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_SQUASHFS(=|_(ZLIB|LZMA|LZO|XZ|ZSTD|LZ4)=)'",
                      squashfs_config, sizeof(squashfs_config));
        has_squashfs_config = contains(squashfs_config, "CONFIG_SQUASHFS=y") ||
                              contains(squashfs_config, "CONFIG_SQUASHFS=m");
    }

    bool has_squashfs = read_file_contains("/proc/filesystems", "squashfs");
    pm_check_status squashfs_status = PM_CHECK_FAIL;
    const char *squashfs_summary = "Kernel does not advertise squashfs";
    const char *squashfs_detail = "/proc/filesystems";
    if (has_squashfs) {
        squashfs_status = PM_CHECK_OK;
        squashfs_summary = "Kernel advertises squashfs";
    } else if (has_squashfs_config) {
        squashfs_status = PM_CHECK_OK;
        squashfs_summary = "Kernel config includes squashfs support";
        squashfs_detail = squashfs_config;
    }
    add_check(r, checks, "kernel.squashfs",
              squashfs_status,
              "required",
              squashfs_summary,
              squashfs_detail);

    if (pm_file_exists("/proc/config.gz")) {
        pm_check_status status = PM_CHECK_OK;
        if (!contains(squashfs_config, "CONFIG_SQUASHFS_ZSTD=y")) {
            status = PM_CHECK_NOT_APP_FIXABLE;
        }
        add_check(r, checks, "kernel.squashfs_compression", status,
                  "required",
                  status == PM_CHECK_OK ? "Required squashfs compression support appears present"
                                        : "Kernel lacks squashfs zstd support",
                  squashfs_config[0] ? squashfs_config : "No squashfs config lines found.");
    } else {
        add_check(r, checks, "kernel.squashfs_compression", PM_CHECK_UNKNOWN,
                  "required", "Kernel config is not readable", "/proc/config.gz");
    }

    check_runtime_squashfs_formats(ctx, r, checks, squashfs_config,
                                   has_readable_squashfs_config,
                                   has_squashfs || has_squashfs_config);

    char runtime_helper[PM_PATH_MAX];
    bool has_runtime_helper = port_runtime_helper_available(ctx, runtime_helper,
                                                            sizeof(runtime_helper));

    int loop_count = count_loop_nodes();
    char detail[PM_PATH_MAX + 128];
    pm_format(detail, sizeof(detail), "loop block nodes now: %d%s%s",
              loop_count,
              has_runtime_helper ? "\nOn-demand helper: " : "",
              has_runtime_helper ? runtime_helper : "");
    add_check(r, checks, "kernel.loop_devices",
              loop_count >= 16 ? PM_CHECK_OK
                               : (has_runtime_helper ? PM_CHECK_APP_LOCAL_OK
                                                     : (loop_count >= 8 ? PM_CHECK_WARN
                                                                       : PM_CHECK_FAIL)),
              "required",
              loop_count >= 16 ? "At least 16 loop nodes are present"
                               : (has_runtime_helper
                                      ? "Loop nodes are provisioned on demand before port launch"
                                      : "Fewer than 16 loop nodes are present"),
              loop_count >= 0 ? detail : "Could not inspect /dev.");

    if (path_exists_any("/dev/uinput")) {
        add_check(r, checks, "kernel.uinput",
                  access("/dev/uinput", W_OK) == 0 ? PM_CHECK_OK : PM_CHECK_WARN,
                  "required",
                  access("/dev/uinput", W_OK) == 0 ? "/dev/uinput is writable"
                                                   : "/dev/uinput exists but is not writable",
                  "/dev/uinput");
    } else {
        add_check(r, checks, "kernel.uinput", PM_CHECK_FAIL, "required",
                  "/dev/uinput is missing", NULL);
    }

    bool zram_exists = path_exists_any("/dev/zram0");
    bool zram_active = pm_file_exists("/proc/swaps") &&
                       read_file_contains("/proc/swaps", "/dev/zram0");
    char zram_detail[PM_PATH_MAX + 160];
    pm_format(zram_detail, sizeof(zram_detail), "%s%s%s",
              zram_active ? "zram swap appears active"
                          : "zram swap is not active in the current shell",
              has_runtime_helper ? "\nOn-demand helper: " : "",
              has_runtime_helper ? runtime_helper : "");
    add_check(r, checks, "kernel.zram",
              zram_active ? PM_CHECK_OK
                          : (zram_exists && has_runtime_helper ? PM_CHECK_APP_LOCAL_OK
                                                               : PM_CHECK_WARN),
              "recommended",
              zram_active ? "zram swap is active"
                          : (zram_exists && has_runtime_helper
                                 ? "zram swap is provisioned on demand before port launch"
                                 : (zram_exists ? "/dev/zram0 exists but swap may not be active"
                                                : "/dev/zram0 is missing")),
              zram_detail);

    add_check(r, checks, "runtime.tmp",
              pm_dir_exists("/tmp") ? PM_CHECK_OK : PM_CHECK_FAIL,
              "required",
              pm_dir_exists("/tmp") ? "/tmp exists" : "/tmp is missing",
              read_file_contains("/proc/mounts", " /tmp tmpfs ") ? "tmpfs" : "mount type unknown");

    add_check(r, checks, "runtime.dev_shm",
              pm_dir_exists("/dev/shm") ? PM_CHECK_OK : PM_CHECK_FAIL,
              "required",
              pm_dir_exists("/dev/shm") ? "/dev/shm exists" : "/dev/shm is missing",
              read_file_contains("/proc/mounts", " /dev/shm tmpfs ") ? "tmpfs" : "mount type unknown");

    if (truthy_env("LEAF_PM_DOCTOR_MOUNT_TEST")) {
        char squashfs[PM_PATH_MAX] = "";
        find_runtime_squashfs(ctx, squashfs, sizeof(squashfs));
        if (squashfs[0]) {
            char qimg[PM_PATH_MAX + 16];
            char cmd[PM_PATH_MAX * 2];
            char output[512];
            shell_quote(qimg, sizeof(qimg), squashfs);
            pm_format(cmd, sizeof(cmd),
                      "m=/tmp/leaf-pm-doctor-squashfs-$$; rm -rf \"$m\"; mkdir -p \"$m\" && mount -o loop,ro %s \"$m\" && umount \"$m\"; rc=$?; rmdir \"$m\" 2>/dev/null || true; exit $rc",
                      qimg);
            int rc = capture_shell(cmd, output, sizeof(output));
            if (rc == 0) {
                FILE *marker = fopen(PM_MOUNT_PROBE_MARKER, "wb");
                if (marker) {
                    fputs("ok\n", marker);
                    fclose(marker);
                }
            } else {
                unlink(PM_MOUNT_PROBE_MARKER);
            }
            add_check(r, checks, "kernel.squashfs_mount_probe",
                      rc == 0 ? PM_CHECK_OK : PM_CHECK_FAIL,
                      "required",
                      rc == 0 ? "Read-only squashfs loop mount probe passed"
                              : "Read-only squashfs loop mount probe failed",
                      squashfs);
        } else {
            add_check(r, checks, "kernel.squashfs_mount_probe", PM_CHECK_UNKNOWN,
                      "required", "No installed runtime squashfs found for mount probe",
                      ctx->portmaster_dir);
        }
    } else {
        add_check(r, checks, "kernel.squashfs_mount_probe", PM_CHECK_INFO,
                  "required",
                  "Mount probe skipped",
                  "Set LEAF_PM_DOCTOR_MOUNT_TEST=1 to run a reboot-clean /tmp read-only mount probe.");
    }

    if (truthy_env("LEAF_PM_DOCTOR_LOOP_STRESS")) {
        int target = doctor_loop_stress_count();
        char squashfs[PM_PATH_MAX] = "";
        find_runtime_squashfs(ctx, squashfs, sizeof(squashfs));
        if (squashfs[0]) {
            char qimg[PM_PATH_MAX + 16];
            char qhelper[PM_PATH_MAX + 16];
            char helper_prefix[PM_PATH_MAX + 192] = "";
            char cmd[PM_PATH_MAX * 4 + 4096];
            char output[2048];
            shell_quote(qimg, sizeof(qimg), squashfs);
            if (has_runtime_helper &&
                shell_quote(qhelper, sizeof(qhelper), runtime_helper) == 0) {
                pm_format(helper_prefix, sizeof(helper_prefix),
                          "LEAF_PM_LOOP_COUNT=%d LEAF_PM_ZRAM=0 %s >\"$root/prep.log\" 2>&1 || true; ",
                          target, qhelper);
            }

            pm_format(cmd, sizeof(cmd),
                      "set -u; img=%s; target=%d; root=/tmp/leaf-pm-doctor-loop-stress-$$; "
                      "mounted=''; loops=''; created_nodes=''; "
                      "cleanup(){ for m in $mounted; do umount \"$m\" 2>/dev/null || true; done; "
                      "for d in $loops; do losetup -d \"$d\" 2>/dev/null || true; done; "
                      "for n in $created_nodes; do losetup -d \"$n\" 2>/dev/null || true; rm -f \"$n\" 2>/dev/null || true; done; "
                      "rm -rf \"$root\"; }; "
                      "trap cleanup EXIT HUP INT TERM; "
                      "rm -rf \"$root\"; mkdir -p \"$root\" || exit 10; "
                      "preexisting=\"$root/preexisting\"; : >\"$preexisting\"; "
                      "i=0; while [ \"$i\" -lt \"$target\" ]; do [ -e \"/dev/loop$i\" ] && echo \"$i\" >>\"$preexisting\"; i=$((i + 1)); done; "
                      "%s"
                      "i=0; while [ \"$i\" -lt \"$target\" ]; do "
                      "if [ -e \"/dev/loop$i\" ] && ! grep -qx \"$i\" \"$preexisting\" 2>/dev/null; then created_nodes=\"/dev/loop$i $created_nodes\"; fi; "
                      "i=$((i + 1)); done; "
                      "i=0; while [ \"$i\" -lt \"$target\" ]; do "
                      "dev=$(losetup -f 2>/dev/null) || { echo \"losetup -f failed at $i\"; exit 11; }; "
                      "losetup -r \"$dev\" \"$img\" 2>/dev/null || { echo \"losetup attach failed for $dev at $i\"; exit 12; }; "
                      "loops=\"$dev $loops\"; m=\"$root/m$i\"; mkdir -p \"$m\" || exit 13; "
                      "mount -t squashfs -o ro \"$dev\" \"$m\" 2>/dev/null || { echo \"mount failed for $dev at $i\"; exit 14; }; "
                      "mounted=\"$m $mounted\"; i=$((i + 1)); done; "
                      "active=$(grep -c \" $root/m\" /proc/mounts 2>/dev/null || true); "
                      "echo \"mounted=$active target=$target image=$img\"; "
                      "[ \"$active\" -eq \"$target\" ] || exit 15",
                      qimg, target, helper_prefix);
            int rc = capture_shell(cmd, output, sizeof(output));
            char detail[PM_PATH_MAX + 512];
            pm_format(detail, sizeof(detail), "%s%s%s",
                      squashfs,
                      output[0] ? "\n" : "",
                      output[0] ? output : "");
            add_check(r, checks, "kernel.squashfs_loop_stress",
                      rc == 0 ? PM_CHECK_OK : PM_CHECK_FAIL,
                      "required",
                      rc == 0 ? "Concurrent read-only squashfs loop mount stress passed"
                              : "Concurrent read-only squashfs loop mount stress failed",
                      detail);
        } else {
            add_check(r, checks, "kernel.squashfs_loop_stress", PM_CHECK_UNKNOWN,
                      "required", "No installed runtime squashfs found for loop stress",
                      ctx->portmaster_dir);
        }
    } else {
        add_check(r, checks, "kernel.squashfs_loop_stress", PM_CHECK_INFO,
                  "required",
                  "Loop stress probe skipped",
                  "Set LEAF_PM_DOCTOR_LOOP_STRESS=1 to mount a runtime squashfs 16 times under /tmp.");
    }
}

static const char *mount_type_for_path(const char *path, char *out, size_t out_size)
{
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    FILE *fp = fopen("/proc/mounts", "rb");
    if (!fp) {
        return NULL;
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
        if (strncmp(path, mountpoint, ml) == 0 &&
            (path[ml] == '\0' || mountpoint[ml - 1] == '/' || path[ml] == '/') &&
            ml > strlen(best_mount)) {
            pm_copy(best_mount, sizeof(best_mount), mountpoint);
            pm_copy(best_type, sizeof(best_type), type);
        }
    }
    fclose(fp);

    if (!best_type[0]) {
        return NULL;
    }
    pm_copy(out, out_size, best_type);
    return out;
}

static void check_storage(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char fs_type[64];
    const char *type = mount_type_for_path(ctx->sdcard_path, fs_type, sizeof(fs_type));
    pm_check_status status = PM_CHECK_UNKNOWN;
    const char *summary = "Could not determine SD filesystem";
    if (type) {
        if (strcmp(type, "exfat") == 0) {
            status = PM_CHECK_OK;
            summary = "Active SD root is exFAT";
        } else if (strcmp(type, "vfat") == 0) {
            status = PM_CHECK_NOT_APP_FIXABLE;
            summary = "Active SD root is vfat/FAT32";
        } else {
            status = PM_CHECK_WARN;
            summary = "Active SD root uses a nonstandard filesystem";
        }
    }
    char detail[PM_PATH_MAX + 128];
    pm_format(detail, sizeof(detail), "%s\nfilesystem: %s%s",
              ctx->sdcard_path,
              type ? type : "unknown",
              type && strcmp(type, "vfat") == 0 ? "\nFiles over 4 GiB are not supported on this card." : "");
    add_check(r, checks, "storage.sd_filesystem", status, "recommended", summary, detail);

    char secondary[PM_PATH_MAX] = "/media/sdcard1";
    if (pm_dir_exists(secondary)) {
        add_check(r, checks, "storage.secondary_sd", PM_CHECK_INFO,
                  "info", "Secondary SD mount candidate exists", secondary);
    }
}

static void check_libraries(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    static const char *required[] = {
        "libc.so.6",
        "libstdc++.so.6",
        "libgcc_s.so.1",
        "libz.so.1",
        "libSDL2-2.0.so.0",
        "libSDL2_mixer-2.0.so.0",
        "libSDL2_image-2.0.so.0",
        "libSDL2_ttf-2.0.so.0",
        "libpng16.so.16",
        "libjpeg.so.62",
        "libfreetype.so.6",
        "libfontconfig.so.1",
        "libopenal.so.1",
        "libasound.so.2",
    };
    static const char *older[] = {
        "libFLAC.so.8",
        "libjpeg.so.8",
        "libavcodec.so.58",
        "libavformat.so.58",
        "libavutil.so.56",
        "libswresample.so.3",
        "libswscale.so.5",
        "libvpx.so.6",
        "libwebp.so.6",
        "libaom.so.0",
        "libdav1d.so.4",
        "libcodec2.so.0.9",
        "libx264.so.160",
        "libx265.so.192",
        "libwavpack.so.1",
    };
    static const char *system_dirs[] = {
        "/usr/lib",
        "/lib",
        "/usr/lib/aarch64-linux-gnu",
        "/lib/aarch64-linux-gnu",
    };
    const char *older_dirs[sizeof(system_dirs) / sizeof(system_dirs[0]) + 1];
    size_t older_dir_count = 0;
    for (size_t i = 0; i < sizeof(system_dirs) / sizeof(system_dirs[0]); i++) {
        older_dirs[older_dir_count++] = system_dirs[i];
    }
    char compat_lib_dir[PM_PATH_MAX];
    if (ctx &&
        pm_join3(compat_lib_dir, sizeof(compat_lib_dir),
                 ctx->data_dir, "compat/libs", "aarch64") == 0 &&
        pm_dir_exists(compat_lib_dir)) {
        older_dirs[older_dir_count++] = compat_lib_dir;
    } else {
        compat_lib_dir[0] = '\0';
    }

    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        char found[PM_PATH_MAX] = "";
        for (size_t d = 0; d < sizeof(system_dirs) / sizeof(system_dirs[0]); d++) {
            char path[PM_PATH_MAX];
            if (pm_join(path, sizeof(path), system_dirs[d], required[i]) == 0 &&
                pm_file_exists(path)) {
                pm_copy(found, sizeof(found), path);
                break;
            }
        }
        char id[128];
        pm_format(id, sizeof(id), "lib.required.%s", required[i]);
        add_check(r, checks, id,
                  found[0] ? PM_CHECK_OK : PM_CHECK_FAIL,
                  "required",
                  found[0] ? "Required library present" : "Required library missing",
                  found[0] ? found : required[i]);
    }

    int missing_older = 0;
    char missing[2048] = "";
    for (size_t i = 0; i < sizeof(older) / sizeof(older[0]); i++) {
        bool found = false;
        for (size_t d = 0; d < older_dir_count; d++) {
            char path[PM_PATH_MAX];
            if (pm_join(path, sizeof(path), older_dirs[d], older[i]) == 0 &&
                pm_file_exists(path)) {
                found = true;
                break;
            }
        }
        if (!found) {
            missing_older++;
            appendf(missing, sizeof(missing), "%s%s", missing[0] ? ", " : "", older[i]);
        }
    }
    add_check(r, checks, "lib.compat_older_sonames",
              missing_older == 0 ? PM_CHECK_OK : PM_CHECK_WARN,
              "recommended",
              missing_older == 0 ? "Older compatibility sonames are present"
                                 : "Some older compatibility sonames are missing",
              missing_older == 0
                  ? (compat_lib_dir[0]
                         ? "System and app-local compatibility paths cover the checked set."
                         : "System library paths cover the checked set.")
                  : missing);
}

static void check_graphics_audio(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    (void)ctx;
    add_check(r, checks, "graphics.drm",
              path_exists_any("/dev/dri/card0") || path_exists_any("/dev/dri/renderD128")
                  ? PM_CHECK_OK
                  : PM_CHECK_WARN,
              "recommended",
              "DRM device node check",
              "/dev/dri/card0 or /dev/dri/renderD128");
    add_check(r, checks, "graphics.framebuffer",
              path_exists_any("/dev/fb0") ? PM_CHECK_OK : PM_CHECK_WARN,
              "recommended",
              path_exists_any("/dev/fb0") ? "/dev/fb0 exists" : "/dev/fb0 missing",
              NULL);
    add_check(r, checks, "graphics.wayland",
              path_exists_any("/run/wayland-0") || path_exists_any("/var/run/wayland-0")
                  ? PM_CHECK_OK
                  : PM_CHECK_WARN,
              "recommended",
              "Wayland socket check",
              "/run/wayland-0 or /var/run/wayland-0");
    add_check(r, checks, "audio.alsa",
              pm_dir_exists("/dev/snd") ? PM_CHECK_OK : PM_CHECK_FAIL,
              "required",
              pm_dir_exists("/dev/snd") ? "ALSA device directory exists" : "ALSA device directory missing",
              "/dev/snd");
    add_check(r, checks, "audio.pulseaudio",
              path_exists_any("/tmp/pulse-socket") ? PM_CHECK_OK : PM_CHECK_WARN,
              "recommended",
              path_exists_any("/tmp/pulse-socket") ? "PulseAudio socket exists" : "PulseAudio socket missing",
              "/tmp/pulse-socket");
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    if (!path || !prefix || !prefix[0]) {
        return false;
    }
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 &&
           (path[n] == '\0' || prefix[n - 1] == '/' || path[n] == '/');
}

static void check_root_drift(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char scan_path[PM_PATH_MAX];
    if (pm_join(scan_path, sizeof(scan_path), ctx->leaf_dir, "armhf-scan.json") != 0 ||
        !pm_file_exists(scan_path)) {
        add_check(r, checks, "paths.scan_root", PM_CHECK_INFO,
                  "info", "No armhf scan report exists yet", ctx->leaf_dir);
        return;
    }

    char err[128];
    char *text = pm_read_text_file(scan_path, 256 * 1024, err, sizeof(err));
    if (!text) {
        add_check(r, checks, "paths.scan_root", PM_CHECK_WARN,
                  "recommended", "Could not read armhf scan report", scan_path);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        add_check(r, checks, "paths.scan_root", PM_CHECK_WARN,
                  "recommended", "Could not parse armhf scan report", scan_path);
        return;
    }

    cJSON *ports_dir = cJSON_GetObjectItemCaseSensitive(root, "ports_dir");
    const char *ports_value = cJSON_IsString(ports_dir) ? ports_dir->valuestring : "";
    pm_check_status status = path_has_prefix(ports_value, ctx->sdcard_path)
                                 ? PM_CHECK_OK
                                 : PM_CHECK_WARN;
    char detail[PM_PATH_MAX * 2];
    pm_format(detail, sizeof(detail), "active=%s\nreport=%s",
              ctx->ports_dir, ports_value && ports_value[0] ? ports_value : "(missing)");
    add_check(r, checks, "paths.scan_root", status,
              "required",
              status == PM_CHECK_OK ? "Scan report matches active SD root"
                                    : "Scan report appears to reference a different root",
              detail);
    cJSON_Delete(root);

    char hook_path[PM_PATH_MAX];
    if (pm_join(hook_path, sizeof(hook_path), ctx->portmaster_dir, "leaf-armhf-env.sh") == 0 &&
        pm_file_exists(hook_path)) {
        char hook_err[128];
        char *hook = pm_read_text_file(hook_path, 1024 * 1024, hook_err, sizeof(hook_err));
        if (hook) {
            bool mentions_active = contains(hook, ctx->data_dir);
            add_check(r, checks, "paths.hook_fallback_root",
                      mentions_active ? PM_CHECK_OK : PM_CHECK_WARN,
                      "required",
                      mentions_active ? "Generated hook mentions active data root"
                                      : "Generated hook fallback may reference a stale data root",
                      hook_path);
            free(hook);
        }
    }
}

static void check_unresolved_sonames(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char scan_path[PM_PATH_MAX];
    if (pm_join(scan_path, sizeof(scan_path), ctx->leaf_dir, "armhf-scan.json") != 0 ||
        !pm_file_exists(scan_path)) {
        add_check(r, checks, "lib.unresolved_sonames", PM_CHECK_INFO,
                  "info", "No armhf scan report exists yet", ctx->leaf_dir);
        return;
    }

    char err[128];
    char *text = pm_read_text_file(scan_path, 512 * 1024, err, sizeof(err));
    if (!text) {
        add_check(r, checks, "lib.unresolved_sonames", PM_CHECK_INFO,
                  "info", "Could not read unresolved SONAME inventory", scan_path);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        add_check(r, checks, "lib.unresolved_sonames", PM_CHECK_INFO,
                  "info", "Could not parse unresolved SONAME inventory", scan_path);
        return;
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "unresolved_sonames");
    if (!cJSON_IsArray(items)) {
        add_check(r, checks, "lib.unresolved_sonames", PM_CHECK_INFO,
                  "info", "Scan report predates unresolved SONAME inventory", scan_path);
        cJSON_Delete(root);
        return;
    }

    int ports = cJSON_GetArraySize(items);
    char detail[4096] = "";
    appendf(detail, sizeof(detail), "scan=%s", scan_path);

    int shown_ports = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (shown_ports >= 12) {
            appendf(detail, sizeof(detail), "\n...and %d more port%s",
                    ports - shown_ports, ports - shown_ports == 1 ? "" : "s");
            break;
        }
        cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
        cJSON *sonames = cJSON_GetObjectItemCaseSensitive(item, "sonames");
        if (!cJSON_IsString(port) || !port->valuestring || !cJSON_IsArray(sonames)) {
            continue;
        }

        appendf(detail, sizeof(detail), "\n%s: ", port->valuestring);
        int shown_sonames = 0;
        int total_sonames = cJSON_GetArraySize(sonames);
        cJSON *soname = NULL;
        cJSON_ArrayForEach(soname, sonames) {
            if (!cJSON_IsString(soname) || !soname->valuestring) {
                continue;
            }
            if (shown_sonames >= 8) {
                appendf(detail, sizeof(detail), "%s...and %d more",
                        shown_sonames > 0 ? ", " : "",
                        total_sonames - shown_sonames);
                break;
            }
            appendf(detail, sizeof(detail), "%s%s",
                    shown_sonames > 0 ? ", " : "",
                    soname->valuestring);
            shown_sonames++;
        }
        shown_ports++;
    }

    add_check(r, checks, "lib.unresolved_sonames", PM_CHECK_INFO,
              "info",
              ports > 0 ? "Latest scan found unresolved aarch64 port SONAMEs"
                        : "Latest scan has no unresolved aarch64 port SONAMEs",
              detail);
    cJSON_Delete(root);
}

static void check_env_contract(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char detail[4096];
    pm_format(detail, sizeof(detail),
              "CFW_NAME=Leaf\nCFW_VERSION=<Leaf release manifest or manager version>\n"
              "DEVICE_NAME=Miniloong Pocket 1\nDEVICE_CPU=RK3566\n"
              "DEVICE_ARCH=aarch64\nDEVICE_RAM=1\nDISPLAY_WIDTH=960\nDISPLAY_HEIGHT=720\n"
              "PM_CAN_MOUNT=<Y only after %s exists>\nTASKSET=<taskset 0xF only after execution probe>\n"
              "SDCARD_PATH=%s\nUSERDATA_PATH=%s\nPORTMASTER_CONTROLFOLDER=%s\n"
              "HM_PORTS_DIR=%s\nHM_SCRIPTS_DIR=%s",
              PM_MOUNT_PROBE_MARKER,
              ctx->sdcard_path,
              ctx->userdata_path,
              ctx->portmaster_dir,
              ctx->ports_dir,
              ctx->ports_dir);
    add_check(r, checks, "env.manager_contract", PM_CHECK_INFO,
              "required", "Expected managed launch environment", detail);
}

static cJSON *load_launch_snapshot(const pm_context *ctx,
                                   const char *mode,
                                   char *path,
                                   size_t path_size)
{
    if (pm_env_snapshot_path(ctx, mode, "json", path, path_size) != 0 ||
        !pm_file_exists(path)) {
        return NULL;
    }

    char read_err[128];
    char *text = pm_read_text_file(path, 1024 * 1024, read_err, sizeof(read_err));
    if (!text) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static bool launch_snapshot_value(cJSON *root, const char *name, const char **value)
{
    if (value) {
        *value = "";
    }
    if (!root || !name) {
        return false;
    }

    cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(entries)) {
        return false;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, entries) {
        cJSON *entry_name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (!cJSON_IsString(entry_name) || !entry_name->valuestring ||
            strcmp(entry_name->valuestring, name) != 0) {
            continue;
        }
        cJSON *entry_value = cJSON_GetObjectItemCaseSensitive(entry, "value");
        if (cJSON_IsString(entry_value) && entry_value->valuestring) {
            if (value) {
                *value = entry_value->valuestring;
            }
            return true;
        }
        return false;
    }
    return false;
}

static void check_env_launch_parity(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    char gui_path[PM_PATH_MAX] = "";
    char port_path[PM_PATH_MAX] = "";
    cJSON *gui = load_launch_snapshot(ctx, "gui", gui_path, sizeof(gui_path));
    cJSON *port = load_launch_snapshot(ctx, "port", port_path, sizeof(port_path));

    if (!gui || !port) {
        char detail[PM_PATH_MAX * 2 + 256];
        pm_format(detail, sizeof(detail),
                  "gui=%s (%s)\nport=%s (%s)\n"
                  "Generate GUI evidence by launching PortMaster. Generate port evidence with: "
                  "LEAF_PM_ENV_PROBE=1 <port-script>.sh",
                  gui_path[0] ? gui_path : "(path unavailable)",
                  gui ? "present" : "missing",
                  port_path[0] ? port_path : "(path unavailable)",
                  port ? "present" : "missing");
        add_check(r, checks, "env.launch_parity", PM_CHECK_INFO,
                  "required",
                  "GUI and port launch snapshots are not both available yet",
                  detail);
        cJSON_Delete(gui);
        cJSON_Delete(port);
        return;
    }

    const char *vars[] = {
        "PLATFORM",
        "SDCARD_PATH",
        "USERDATA_PATH",
        "ROMS_PATH",
        "IMAGES_PATH",
        "HOME",
        "XDG_DATA_HOME",
        "HM_PORTS_DIR",
        "HM_SCRIPTS_DIR",
        "PORTMASTER_CONTROLFOLDER",
        "CFW_NAME",
        "CFW_VERSION",
        "DEVICE_NAME",
        "DEVICE_CPU",
        "DEVICE_ARCH",
        "DEVICE_RAM",
        "DISPLAY_WIDTH",
        "DISPLAY_HEIGHT",
        "ESUDO",
        "GPTOKEYB",
        "GPTOKEYB2",
        "directory",
        "PM_CAN_MOUNT",
        "TASKSET",
        "LEAF_PM_RETROARCH_BIN",
        "LEAF_PM_RETROARCH_CONFIG",
        NULL,
    };

    char detail[8192] = "";
    int mismatches = 0;
    for (size_t i = 0; vars[i]; i++) {
        const char *gui_value = "";
        const char *port_value = "";
        bool has_gui = launch_snapshot_value(gui, vars[i], &gui_value);
        bool has_port = launch_snapshot_value(port, vars[i], &port_value);
        if (has_gui && has_port && strcmp(gui_value, port_value) == 0) {
            continue;
        }
        mismatches++;
        if (mismatches <= 32) {
            appendf(detail, sizeof(detail),
                    "%s: gui=%s%s%s port=%s%s%s\n",
                    vars[i],
                    has_gui ? "'" : "(missing",
                    has_gui ? gui_value : "",
                    has_gui ? "'" : ")",
                    has_port ? "'" : "(missing",
                    has_port ? port_value : "",
                    has_port ? "'" : ")");
        }
    }

    if (mismatches == 0) {
        pm_format(detail, sizeof(detail),
                  "gui=%s\nport=%s\nCompared spec and Leaf path/runtime variables; "
                  "controller layout exports are intentionally mode-specific.",
                  gui_path,
                  port_path);
    } else if (mismatches > 32) {
        appendf(detail, sizeof(detail), "...and %d more mismatches\n", mismatches - 32);
    }

    add_check(r, checks, "env.launch_parity",
              mismatches == 0 ? PM_CHECK_OK : PM_CHECK_WARN,
              "required",
              mismatches == 0 ? "GUI and port launch snapshots agree"
                              : "GUI and port launch snapshots differ",
              detail);
    cJSON_Delete(gui);
    cJSON_Delete(port);
}

static void check_no_stock_os_writes(const pm_context *ctx, pm_doctor_report *r, cJSON *checks)
{
    (void)ctx;
    add_check(r, checks, "policy.stock_os_emmc",
              PM_CHECK_INFO,
              "required",
              "Stock OS/eMMC must remain untouched",
              "Compatibility shims must live on SD or tmpfs and any bind mounts/session tricks must be reboot-clean.");
}

static void finish_json(pm_doctor_report *r, cJSON *root)
{
    cJSON_AddNumberToObject(root, "issues", r->issues);
    cJSON_AddNumberToObject(root, "warnings", r->warnings);

    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        pm_copy(r->text, sizeof(r->text), "{\"schema\":1,\"error\":\"could not render doctor json\"}\n");
        r->issues++;
        return;
    }
    pm_copy(r->text, sizeof(r->text), printed);
    appendf(r->text, sizeof(r->text), "\n");
    free(printed);
}

void pm_doctor_run(const pm_context *ctx, pm_doctor_report *report)
{
    memset(report, 0, sizeof(*report));
    append_line(report, "OK", "Platform", ctx->platform);

    if (ctx->lock_loaded) {
        char summary[1024];
        pm_lock_summary(&ctx->lock, summary, sizeof(summary));
        append_line(report, "OK", "Stable PortMaster lock", summary);
    } else {
        report->issues++;
        append_line(report, "ERR", "Stable PortMaster lock", ctx->lock_path);
    }

    if (ctx->runtime_lock_loaded) {
        char summary[1024];
        pm_ui_runtime_lock_summary(&ctx->runtime_lock, summary, sizeof(summary));
        append_line(report, "OK", "PortMaster UI runtime lock", summary);
    } else {
        report->issues++;
        append_line(report, "ERR", "PortMaster UI runtime lock", ctx->runtime_lock_path);
    }

    append_line(report, pm_dir_exists(ctx->data_dir) ? "OK" : "WARN",
                "Manager data", ctx->data_dir);
    append_line(report, pm_dir_exists(ctx->portmaster_dir) ? "OK" : "WARN",
                "Upstream PortMaster install", ctx->portmaster_dir);

    char runtime_python[PM_PATH_MAX];
    bool has_runtime = pm_portmaster_runtime_available(ctx, runtime_python, sizeof(runtime_python),
                                                       NULL);
    append_line(report, has_runtime ? "OK" : "WARN",
                "PortMaster Python runtime",
                has_runtime ? runtime_python : "install managed runtime archive before launch");

    append_line(report, pm_dir_exists(ctx->ports_dir) ? "OK" : "WARN",
                "Ports folder", ctx->ports_dir);
    append_line(report, pm_dir_exists(ctx->port_images_dir) ? "OK" : "WARN",
                "Port artwork folder", ctx->port_images_dir);

    if (!pm_dir_exists(ctx->portmaster_dir)) {
        append_line(report, "INFO", "Next step",
                    "Install PortMaster, then install the managed UI runtime.");
    }
}

void pm_doctor_run_spec(const pm_context *ctx, pm_doctor_report *report, bool json)
{
    memset(report, 0, sizeof(*report));

    cJSON *root = NULL;
    cJSON *checks = NULL;
    if (json) {
        root = cJSON_CreateObject();
        checks = cJSON_CreateArray();
        if (root && checks) {
            cJSON_AddNumberToObject(root, "schema", 1);
            cJSON_AddStringToObject(root, "kind", "portmaster-cfw-spec-doctor");
            cJSON_AddStringToObject(root, "manager_version", PM_VERSION);
            cJSON_AddStringToObject(root, "platform", ctx->platform);
            cJSON_AddStringToObject(root, "sdcard_path", ctx->sdcard_path);
            cJSON_AddStringToObject(root, "data_dir", ctx->data_dir);
            cJSON_AddItemToObject(root, "checks", checks);
        }
    } else {
        appendf(report->text, sizeof(report->text),
                "PortMaster CFW Spec Doctor\nPlatform: %s\nSD root: %s\nData: %s\n\n",
                ctx->platform, ctx->sdcard_path, ctx->data_dir);
    }

    check_no_stock_os_writes(ctx, report, checks);
    check_setup_baseline(ctx, report, checks);
    check_bash(ctx, report, checks);
    check_tools(ctx, report, checks);
    check_python(ctx, report, checks);
    check_mounts_and_kernel(ctx, report, checks);
    check_storage(ctx, report, checks);
    check_libraries(ctx, report, checks);
    check_graphics_audio(ctx, report, checks);
    check_root_drift(ctx, report, checks);
    check_unresolved_sonames(ctx, report, checks);
    check_env_contract(ctx, report, checks);
    check_env_launch_parity(ctx, report, checks);

    if (json && root) {
        finish_json(report, root);
        cJSON_Delete(root);
    } else {
        appendf(report->text, sizeof(report->text),
                "Summary: issues=%d warnings=%d\n", report->issues, report->warnings);
    }
}
