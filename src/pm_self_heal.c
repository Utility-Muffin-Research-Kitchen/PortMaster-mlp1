#include "pm_self_heal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long pm_self_heal_mtime_nsec(const struct stat *st)
{
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#else
    return st->st_mtim.tv_nsec;
#endif
}

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

static bool pm_self_heal_source_newer(const struct stat *src,
                                      const struct stat *dst)
{
    if (src->st_mtime != dst->st_mtime) {
        return src->st_mtime > dst->st_mtime;
    }
    return pm_self_heal_mtime_nsec(src) > pm_self_heal_mtime_nsec(dst);
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

static int pm_self_heal_copy_atomic(const char *src,
                                    const char *dst,
                                    const struct stat *src_st,
                                    char *detail,
                                    size_t detail_size)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(detail, detail_size, "cannot open packaged ports launcher: %s",
                 strerror(errno));
        return -1;
    }

    char tmp[PM_PATH_MAX];
    if (pm_format(tmp, sizeof(tmp), "%s.tmp.%ld", dst, (long)getpid()) != 0) {
        fclose(in);
        snprintf(detail, detail_size, "%s", "Leaf ports launcher path too long");
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
        snprintf(detail, detail_size, "cannot promote Leaf ports launcher: %s",
                 strerror(saved_errno));
        return -1;
    }

    chmod(dst, 0755);
    if (pm_self_heal_stamp_times(dst, src_st) != 0) {
        snprintf(detail, detail_size,
                 "updated %s but could not preserve timestamp: %s",
                 dst, strerror(errno));
        return 1;
    }

    snprintf(detail, detail_size, "updated %s from packaged ports launcher", dst);
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
    const char *env_platform = getenv("UMRK_PLATFORM_PATH");
    if (env_platform && env_platform[0] &&
        pm_self_heal_path_under(env_platform, ctx->sdcard_path)) {
        if (pm_copy(platform_root, sizeof(platform_root), env_platform) != 0) {
            if (detail && detail_size > 0) {
                snprintf(detail, detail_size, "%s", "UMRK_PLATFORM_PATH is too long");
            }
            return -1;
        }
    } else if (pm_format(platform_root, sizeof(platform_root),
                        "%s/.system/leaf/platforms/%s",
                        ctx->sdcard_path, ctx->platform) != 0) {
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "%s", "Leaf platform path too long");
        }
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
        should_update = pm_self_heal_source_newer(&src_st, &dst_st);
    }

    if (!should_update) {
        return 0;
    }

    if (pm_mkdir_p(dst_dir, detail, detail_size) != 0) {
        return -1;
    }

    return pm_self_heal_copy_atomic(src, dst, &src_st, detail, detail_size);
}
