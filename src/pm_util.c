#include "pm_util.h"

#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int pm_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return -1;
    }
    if (!src) {
        src = "";
    }
    int needed = snprintf(dst, dst_size, "%s", src);
    return (needed < 0 || (size_t)needed >= dst_size) ? -1 : 0;
}

int pm_format(char *dst, size_t dst_size, const char *fmt, ...)
{
    if (!dst || dst_size == 0 || !fmt) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
    return (needed < 0 || (size_t)needed >= dst_size) ? -1 : 0;
}

int pm_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    if (!a || !a[0]) {
        return pm_copy(dst, dst_size, b);
    }
    if (!b || !b[0]) {
        return pm_copy(dst, dst_size, a);
    }
    size_t len = strlen(a);
    if (a[len - 1] == '/') {
        return pm_format(dst, dst_size, "%s%s", a, b[0] == '/' ? b + 1 : b);
    }
    return pm_format(dst, dst_size, "%s/%s", a, b[0] == '/' ? b + 1 : b);
}

int pm_join3(char *dst, size_t dst_size, const char *a, const char *b, const char *c)
{
    char tmp[PM_PATH_MAX];
    if (pm_join(tmp, sizeof(tmp), a, b) != 0) {
        return -1;
    }
    return pm_join(dst, dst_size, tmp, c);
}

bool pm_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool pm_dir_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int pm_mkdir_p(const char *path, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!path || !path[0]) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "missing directory path");
        }
        return -1;
    }

    char tmp[PM_PATH_MAX];
    if (pm_copy(tmp, sizeof(tmp), path) != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "directory path too long");
        }
        return -1;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "mkdir %s failed: %s", tmp, strerror(errno));
            }
            return -1;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "mkdir %s failed: %s", tmp, strerror(errno));
        }
        return -1;
    }
    return 0;
}

int pm_rm_rf(const char *path, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "refusing to remove unsafe path");
        }
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "cannot open directory %s: %s", path, strerror(errno));
            }
            return -1;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char child[PM_PATH_MAX];
            if (pm_join(child, sizeof(child), path, ent->d_name) != 0) {
                closedir(dir);
                if (err && err_size > 0) {
                    snprintf(err, err_size, "path too long under %s", path);
                }
                return -1;
            }
            if (pm_rm_rf(child, err, err_size) != 0) {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
        if (rmdir(path) != 0) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "cannot remove directory %s: %s", path, strerror(errno));
            }
            return -1;
        }
        return 0;
    }

    if (unlink(path) != 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot remove %s: %s", path, strerror(errno));
        }
        return -1;
    }
    return 0;
}

off_t pm_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    return st.st_size;
}

char *pm_read_text_file(const char *path, size_t max_bytes, char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot open %s", path ? path : "(null)");
        }
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot seek %s", path);
        }
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || (size_t)size > max_bytes) {
        fclose(fp);
        if (err && err_size > 0) {
            snprintf(err, err_size, "file too large: %s", path);
        }
        return NULL;
    }
    rewind(fp);

    char *buf = (char *)calloc((size_t)size + 1, 1);
    if (!buf) {
        fclose(fp);
        if (err && err_size > 0) {
            snprintf(err, err_size, "%s", "out of memory");
        }
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot read %s", path);
        }
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

int pm_run_argv_env_in_dir(const char *cwd, char *const argv[], const pm_env_override *env,
                           char *err, size_t err_size)
{
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (!argv || !argv[0]) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "missing command");
        }
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "fork failed: %s", strerror(errno));
        }
        return -1;
    }
    if (pid == 0) {
        if (cwd && cwd[0] && chdir(cwd) != 0) {
            _exit(126);
        }
        if (env) {
            for (size_t i = 0; env[i].name; i++) {
                if (setenv(env[i].name, env[i].value ? env[i].value : "", 1) != 0) {
                    _exit(125);
                }
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            if (err && err_size > 0) {
                snprintf(err, err_size, "waitpid failed: %s", strerror(errno));
            }
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (err && err_size > 0) {
            if (WIFEXITED(status)) {
                snprintf(err, err_size, "%s exited with %d", argv[0], WEXITSTATUS(status));
            } else {
                snprintf(err, err_size, "%s did not exit normally", argv[0]);
            }
        }
        return -1;
    }
    return 0;
}

int pm_run_argv_in_dir(const char *cwd, char *const argv[], char *err, size_t err_size)
{
    return pm_run_argv_env_in_dir(cwd, argv, NULL, err, err_size);
}

int pm_run_argv(char *const argv[], char *err, size_t err_size)
{
    return pm_run_argv_in_dir(NULL, argv, err, err_size);
}

const char *pm_env(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0]) ? value : fallback;
}
