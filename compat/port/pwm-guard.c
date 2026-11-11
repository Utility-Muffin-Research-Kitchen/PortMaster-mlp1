#define _GNU_SOURCE
/* This file defines open() and friends itself, so it must not see the fortify
   wrappers or the large-file renames that would turn them into other symbols. */
#undef _FORTIFY_SOURCE
#undef _FILE_OFFSET_BITS

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/*
 * Part of leaf-port-shim.so (see sdl-hints.c for the other part).
 *
 * The MLP1's rumble motor is a PWM channel that jawakad owns: it reads the
 * channel's polarity, applies Leaf's Game Rumble switch and strength, and
 * hands the motor to games through force feedback on its virtual gamepad.
 * Ports built for other handhelds sometimes drive a PWM motor directly with
 * that device's values. On the MLP1 those can mean full power (Apotris writes
 * the full period as "off" every frame), and they bypass Leaf's settings.
 *
 * Port processes can still read PWM sysfs files, but opening one for writing
 * fails with EACCES, so a port that also rumbles through SDL falls back to
 * that path, which reaches the motor through Leaf. Paths are matched as given
 * and after resolving symlinks, so /sys/class/pwm/... and the
 * /sys/devices/.../pwm/pwmchipN/... path it points to are both covered.
 * Relative paths, raw syscalls and statically linked programs bypass the
 * guard. LEAF_PM_PWM_GUARD=0 turns it off.
 */

#define LEAF_NEEDS_MODE(flags) (((flags) & O_CREAT) || (((flags) & O_TMPFILE) == O_TMPFILE))

typedef int (*leaf_open_fn)(const char *, int, ...);
typedef int (*leaf_openat_fn)(int, const char *, int, ...);
typedef int (*leaf_open_2_fn)(const char *, int);
typedef int (*leaf_openat_2_fn)(int, const char *, int);
typedef FILE *(*leaf_fopen_fn)(const char *, const char *);

static void *leaf_next(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

#define LEAF_RESOLVE(type, var, name)              \
    static type var;                               \
    if (!var) {                                    \
        union {                                    \
            void *object;                          \
            type function;                         \
        } symbol = { leaf_next(name) };            \
        var = symbol.function;                     \
    }

static int leaf_guard_enabled(void)
{
    const char *value = getenv("LEAF_PM_PWM_GUARD");
    return !value || value[0] != '0';
}

static int leaf_is_pwm_sysfs(const char *path)
{
    if (strncmp(path, "/sys/class/pwm/", 15) == 0 || strstr(path, "/pwm/pwmchip")) {
        return 1;
    }
    return 0;
}

static int leaf_blocks(const char *path, int writing)
{
    if (!writing || !path || strncmp(path, "/sys/", 5) != 0 || !leaf_guard_enabled()) {
        return 0;
    }

    char resolved[PATH_MAX];
    int blocked = leaf_is_pwm_sysfs(path) ||
                  (realpath(path, resolved) && leaf_is_pwm_sysfs(resolved));
    if (blocked) {
        fprintf(stderr,
                "[leaf-port-shim] refused write access to %s: Leaf owns the PWM rumble "
                "motor, so ports rumble through SDL instead\n",
                path);
        errno = EACCES;
    }
    return blocked;
}

static int leaf_writes(int flags)
{
    return (flags & O_ACCMODE) != O_RDONLY;
}

static int leaf_fopen_writes(const char *mode)
{
    return mode && strpbrk(mode, "wa+") != NULL;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (LEAF_NEEDS_MODE(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_open_fn, real_open, "open");
    if (!real_open) {
        errno = ENOSYS;
        return -1;
    }
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (LEAF_NEEDS_MODE(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_open_fn, real_open64, "open64");
    if (!real_open64) {
        errno = ENOSYS;
        return -1;
    }
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (LEAF_NEEDS_MODE(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_openat_fn, real_openat, "openat");
    if (!real_openat) {
        errno = ENOSYS;
        return -1;
    }
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (LEAF_NEEDS_MODE(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_openat_fn, real_openat64, "openat64");
    if (!real_openat64) {
        errno = ENOSYS;
        return -1;
    }
    return real_openat64(dirfd, path, flags, mode);
}

/* _FORTIFY_SOURCE builds call these for open() and openat() without a mode. */
int __open_2(const char *path, int flags)
{
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_open_2_fn, real_open_2, "__open_2");
    if (!real_open_2) {
        errno = ENOSYS;
        return -1;
    }
    return real_open_2(path, flags);
}

int __open64_2(const char *path, int flags)
{
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_open_2_fn, real_open64_2, "__open64_2");
    if (!real_open64_2) {
        errno = ENOSYS;
        return -1;
    }
    return real_open64_2(path, flags);
}

int __openat_2(int dirfd, const char *path, int flags)
{
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_openat_2_fn, real_openat_2, "__openat_2");
    if (!real_openat_2) {
        errno = ENOSYS;
        return -1;
    }
    return real_openat_2(dirfd, path, flags);
}

int __openat64_2(int dirfd, const char *path, int flags)
{
    if (leaf_blocks(path, leaf_writes(flags))) {
        return -1;
    }
    LEAF_RESOLVE(leaf_openat_2_fn, real_openat64_2, "__openat64_2");
    if (!real_openat64_2) {
        errno = ENOSYS;
        return -1;
    }
    return real_openat64_2(dirfd, path, flags);
}

/* glibc's fopen opens the file internally, so it has to be caught here; this
   also covers C++ file streams, which libstdc++ builds on fopen. */
FILE *fopen(const char *path, const char *mode)
{
    if (leaf_blocks(path, leaf_fopen_writes(mode))) {
        return NULL;
    }
    LEAF_RESOLVE(leaf_fopen_fn, real_fopen, "fopen");
    if (!real_fopen) {
        errno = ENOSYS;
        return NULL;
    }
    return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    if (leaf_blocks(path, leaf_fopen_writes(mode))) {
        return NULL;
    }
    LEAF_RESOLVE(leaf_fopen_fn, real_fopen64, "fopen64");
    if (!real_fopen64) {
        errno = ENOSYS;
        return NULL;
    }
    return real_fopen64(path, mode);
}
