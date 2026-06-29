#ifndef PM_UTIL_H
#define PM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef PM_PATH_MAX
#define PM_PATH_MAX 4096
#endif

int pm_copy(char *dst, size_t dst_size, const char *src);
int pm_format(char *dst, size_t dst_size, const char *fmt, ...);
int pm_join(char *dst, size_t dst_size, const char *a, const char *b);
int pm_join3(char *dst, size_t dst_size, const char *a, const char *b, const char *c);

bool pm_file_exists(const char *path);
bool pm_dir_exists(const char *path);
int pm_mkdir_p(const char *path, char *err, size_t err_size);
int pm_rm_rf(const char *path, char *err, size_t err_size);
off_t pm_file_size(const char *path);
char *pm_read_text_file(const char *path, size_t max_bytes, char *err, size_t err_size);

typedef struct {
    const char *name;
    const char *value;
} pm_env_override;

int pm_run_argv(char *const argv[], char *err, size_t err_size);
int pm_run_argv_in_dir(const char *cwd, char *const argv[], char *err, size_t err_size);
int pm_run_argv_env_in_dir(const char *cwd, char *const argv[], const pm_env_override *env,
                           char *err, size_t err_size);

const char *pm_env(const char *name, const char *fallback);

#endif /* PM_UTIL_H */
