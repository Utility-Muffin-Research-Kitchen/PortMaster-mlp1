#include "pm_move.h"

#include "cJSON.h"
#include "pm_sha256.h"
#include "pm_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PM_MOVE_JOURNAL_VERSION 1
#define PM_MOVE_STAGE_PREFIX ".leaf-portmaster-move-"
#define PM_MOVE_QUARANTINE_PREFIX ".leaf-portmaster-quarantine-"
#define PM_MOVE_JSON_LIMIT (4u * 1024u * 1024u)
#define PM_MOVE_RESERVE_BYTES (4u * 1024u * 1024u)

typedef struct {
    char operation_id[128];
    char source_id[32];
    char destination_id[32];
    char slug[256];
    char state[32];
    int expected_generation;
    int mapping_generation;
    int scan_ticket_generation;
    char daemon_state[32];
    unsigned long source_mount_id;
    unsigned long destination_mount_id;
    uint64_t source_st_dev;
    uint64_t destination_st_dev;
    uint64_t source_roms_st_dev;
    uint64_t destination_roms_st_dev;
    uint64_t source_images_st_dev;
    uint64_t destination_images_st_dev;
    unsigned long source_roms_mount_id;
    unsigned long destination_roms_mount_id;
    unsigned long source_images_mount_id;
    unsigned long destination_images_mount_id;
    char source_device_id[32];
    char destination_device_id[32];
    char source_roms_device_id[32];
    char destination_roms_device_id[32];
    char source_images_device_id[32];
    char destination_images_device_id[32];
    char source_fingerprint[128];
    char destination_fingerprint[128];
    char source_roms_fingerprint[128];
    char destination_roms_fingerprint[128];
    char source_images_fingerprint[128];
    char destination_images_fingerprint[128];
    char journal_path[PM_PATH_MAX];
    cJSON *root;
} pm_move_journal;

static void status_emit(pm_move_status_fn fn, void *userdata, const char *message)
{
    if (fn) {
        fn(message, userdata);
    }
}

static int fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
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

static void fsync_parent(const char *path)
{
    char parent[PM_PATH_MAX];
    if (pm_copy(parent, sizeof(parent), path) != 0) {
        return;
    }
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        (void)fsync_dir(parent);
    }
}

static int write_bytes_atomic(const char *path,
                              const char *bytes,
                              size_t size,
                              char *err,
                              size_t err_size)
{
    char temporary[PM_PATH_MAX];
    if (pm_format(temporary, sizeof(temporary), "%s.partial", path) != 0) {
        snprintf(err, err_size, "move journal path is too long");
        return -1;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, err_size, "cannot create %s: %s", temporary, strerror(errno));
        return -1;
    }
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, bytes + written, size - written);
        if (count <= 0) {
            snprintf(err, err_size, "cannot write %s: %s", temporary, strerror(errno));
            close(fd);
            unlink(temporary);
            return -1;
        }
        written += (size_t)count;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary, path) != 0) {
        snprintf(err, err_size, "cannot publish %s: %s", path, strerror(errno));
        unlink(temporary);
        return -1;
    }
    char parent[PM_PATH_MAX];
    if (pm_copy(parent, sizeof(parent), path) == 0) {
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            (void)fsync_dir(parent);
        }
    }
    return 0;
}

static int write_json_atomic(const char *path,
                             const cJSON *root,
                             char *err,
                             size_t err_size)
{
    char *printed = cJSON_Print(root);
    if (!printed) {
        snprintf(err, err_size, "cannot encode move state");
        return -1;
    }
    int rc = write_bytes_atomic(path, printed, strlen(printed), err, err_size);
    cJSON_free(printed);
    return rc;
}

static cJSON *read_json_object(const char *path)
{
    char read_err[256];
    char *text = pm_read_text_file(path, PM_MOVE_JSON_LIMIT,
                                   read_err, sizeof(read_err));
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

static int moves_dir(const pm_context *ctx, char *out, size_t out_size)
{
    return pm_join(out, out_size, ctx->leaf_dir, "moves");
}

static int ensure_moves_dir(const pm_context *ctx, char *err, size_t err_size)
{
    char path[PM_PATH_MAX];
    if (moves_dir(ctx, path, sizeof(path)) != 0) {
        snprintf(err, err_size, "move state path is too long");
        return -1;
    }
    return pm_mkdir_p(path, err, err_size);
}

static int resolve_platformctl(const pm_context *ctx, char *out, size_t out_size)
{
    const char *explicit_ctl = getenv("JAWAKA_PLATFORMCTL");
    if (explicit_ctl && explicit_ctl[0]) {
        return pm_copy(out, out_size, explicit_ctl);
    }
    const char *bin_dir = getenv("UMRK_BIN_PATH");
    if (bin_dir && bin_dir[0] &&
        pm_join(out, out_size, bin_dir, "jawaka-platformctl") == 0 &&
        pm_file_exists(out)) {
        return 0;
    }
    const char *launcher = getenv("UMRK_LAUNCHER_PATH");
    if (launcher && launcher[0] &&
        pm_join3(out, out_size, launcher, "bin", "jawaka-platformctl") == 0 &&
        pm_file_exists(out)) {
        return 0;
    }
    char bin[PM_PATH_MAX];
    if (pm_format(bin, sizeof(bin), "%s/.system/leaf/platforms/%s/launcher/bin",
                  ctx->sdcard_path, ctx->platform) == 0 &&
        pm_join(out, out_size, bin, "jawaka-platformctl") == 0 &&
        pm_file_exists(out)) {
        return 0;
    }
    return pm_copy(out, out_size, "jawaka-platformctl");
}

static int run_capture(char *const argv[],
                       char **out,
                       char *err,
                       size_t err_size)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err, err_size, "cannot create command pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(err, err_size, "cannot start helper: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t used = 0;
    size_t capacity = 4096;
    char *buffer = malloc(capacity);
    if (!buffer) {
        close(pipefd[0]);
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    for (;;) {
        if (used + 2048 + 1 > capacity) {
            if (capacity >= PM_MOVE_JSON_LIMIT) {
                free(buffer);
                close(pipefd[0]);
                kill(pid, SIGTERM);
                (void)waitpid(pid, NULL, 0);
                snprintf(err, err_size, "helper response exceeds move limit");
                return -1;
            }
            size_t next = capacity * 2;
            char *grown = realloc(buffer, next);
            if (!grown) {
                free(buffer);
                close(pipefd[0]);
                kill(pid, SIGTERM);
                (void)waitpid(pid, NULL, 0);
                return -1;
            }
            buffer = grown;
            capacity = next;
        }
        ssize_t got = read(pipefd[0], buffer + used, capacity - used - 1);
        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipefd[0]);
    buffer[used] = '\0';
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, err_size, "%s",
                 buffer[0] ? buffer : "Leaf launcher helper failed");
        free(buffer);
        return -1;
    }
    *out = buffer;
    return 0;
}

static cJSON *run_platformctl_json(const pm_context *ctx,
                                   char *command_argv[],
                                   char *err,
                                   size_t err_size)
{
    char ctl[PM_PATH_MAX];
    if (resolve_platformctl(ctx, ctl, sizeof(ctl)) != 0) {
        snprintf(err, err_size, "cannot resolve the launcher helper");
        return NULL;
    }
    command_argv[0] = ctl;
    char *output = NULL;
    if (run_capture(command_argv, &output, err, err_size) != 0) {
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(output, &end, 1);
    if (!cJSON_IsObject(root)) {
        snprintf(err, err_size, "the launcher helper returned malformed JSON");
        cJSON_Delete(root);
        root = NULL;
    }
    free(output);
    return root;
}

int pm_move_probe_capability(pm_context *ctx,
                             pm_move_capability *out,
                             char *err,
                             size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    if (!ctx || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char *argv[] = { NULL, "capabilities", NULL };
    cJSON *root = run_platformctl_json(ctx, argv, err, err_size);
    if (!root) {
        (void)pm_copy(out->detail, sizeof(out->detail),
                      "The launcher is unavailable; moves are disabled.");
        return -1;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *feature =
        cJSON_GetObjectItemCaseSensitive(root, "relocate-games-v1");
    out->supported = cJSON_IsString(type) &&
                     !strcmp(type->valuestring, "capabilities") &&
                     cJSON_IsTrue(feature);
    (void)pm_copy(out->detail, sizeof(out->detail),
                  out->supported
                      ? "Safe package moves are supported by the launcher."
                      : "Update Leaf to enable safe package moves.");
    cJSON_Delete(root);
    return 0;
}

static int library_generation(const pm_context *ctx,
                              int *generation,
                              char *err,
                              size_t err_size)
{
    char *argv[] = {
        NULL, "request", "{\"type\":\"library-status\"}", NULL
    };
    cJSON *root = run_platformctl_json(ctx, argv, err, err_size);
    const cJSON *value = root
        ? cJSON_GetObjectItemCaseSensitive(root, "generation") : NULL;
    if (!root || !cJSON_IsNumber(value) || value->valueint < 0) {
        if (root) {
            cJSON_Delete(root);
        }
        snprintf(err, err_size, "the launcher returned no valid library generation");
        return -1;
    }
    *generation = value->valueint;
    cJSON_Delete(root);
    return 0;
}

static int relocation_command(const pm_context *ctx,
                              const char *command,
                              const char *operation_id,
                              int expected_generation,
                              const char *items_json,
                              pm_move_journal *journal,
                              char *err,
                              size_t err_size)
{
    char generation[32];
    snprintf(generation, sizeof(generation), "%d", expected_generation);
    char *prepare_argv[] = {
        NULL, (char *)command, (char *)operation_id,
        generation, (char *)items_json, NULL
    };
    char *simple_argv[] = {
        NULL, (char *)command, (char *)operation_id, NULL
    };
    cJSON *root = run_platformctl_json(
        ctx, !strcmp(command, "relocate-prepare") ? prepare_argv : simple_argv,
        err, err_size);
    if (!root) {
        return -1;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "operation_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *mapping =
        cJSON_GetObjectItemCaseSensitive(root, "mapping_generation");
    const cJSON *ticket =
        cJSON_GetObjectItemCaseSensitive(root, "scan_ticket_generation");
    if (!cJSON_IsString(type) ||
        strcmp(type->valuestring, "library-relocate-status") ||
        !cJSON_IsString(id) || strcmp(id->valuestring, operation_id) ||
        !cJSON_IsString(state) || !cJSON_IsNumber(mapping) ||
        !cJSON_IsNumber(ticket)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "the launcher returned an invalid relocation reply");
        return -1;
    }
    if (journal) {
        journal->mapping_generation = mapping->valueint;
        journal->scan_ticket_generation = ticket->valueint;
        (void)pm_copy(journal->daemon_state, sizeof(journal->daemon_state),
                      state->valuestring);
    }
    cJSON_Delete(root);
    return 0;
}

static int journal_store(pm_move_journal *journal, char *err, size_t err_size)
{
    cJSON_ReplaceItemInObjectCaseSensitive(
        journal->root, "state", cJSON_CreateString(journal->state));
    cJSON_ReplaceItemInObjectCaseSensitive(
        journal->root, "mapping_generation",
        cJSON_CreateNumber(journal->mapping_generation));
    cJSON_ReplaceItemInObjectCaseSensitive(
        journal->root, "scan_ticket_generation",
        cJSON_CreateNumber(journal->scan_ticket_generation));
    return write_json_atomic(journal->journal_path, journal->root, err, err_size);
}

static int journal_transition(pm_move_journal *journal,
                              const char *state,
                              char *err,
                              size_t err_size)
{
    if (pm_copy(journal->state, sizeof(journal->state), state) != 0) {
        return -1;
    }
    int rc = journal_store(journal, err, err_size);
    const char *interrupt_after = getenv("PORTMASTER_MOVE_TEST_INTERRUPT_AFTER");
    if (rc == 0 && interrupt_after && !strcmp(interrupt_after, state)) {
        snprintf(err, err_size,
                 "fixture interruption after durable state %s", state);
        return -2;
    }
    return rc;
}

static int json_string(cJSON *root, const char *name, char *out, size_t out_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) && value->valuestring
        ? pm_copy(out, out_size, value->valuestring) : -1;
}

static int journal_load(const char *path,
                        pm_move_journal *journal,
                        char *err,
                        size_t err_size)
{
    memset(journal, 0, sizeof(*journal));
    journal->root = read_json_object(path);
    cJSON *version = journal->root
        ? cJSON_GetObjectItemCaseSensitive(journal->root, "version") : NULL;
    cJSON *expected = journal->root
        ? cJSON_GetObjectItemCaseSensitive(journal->root, "expected_generation") : NULL;
    cJSON *mapping = journal->root
        ? cJSON_GetObjectItemCaseSensitive(journal->root, "mapping_generation") : NULL;
    cJSON *ticket = journal->root
        ? cJSON_GetObjectItemCaseSensitive(journal->root, "scan_ticket_generation") : NULL;
    if (!journal->root || !cJSON_IsNumber(version) ||
        version->valueint != PM_MOVE_JOURNAL_VERSION ||
        json_string(journal->root, "operation_id", journal->operation_id,
                    sizeof(journal->operation_id)) != 0 ||
        json_string(journal->root, "source_id", journal->source_id,
                    sizeof(journal->source_id)) != 0 ||
        json_string(journal->root, "destination_id", journal->destination_id,
                    sizeof(journal->destination_id)) != 0 ||
        json_string(journal->root, "slug", journal->slug,
                    sizeof(journal->slug)) != 0 ||
        json_string(journal->root, "state", journal->state,
                    sizeof(journal->state)) != 0 ||
        !cJSON_IsNumber(expected) || !cJSON_IsNumber(mapping) ||
        !cJSON_IsNumber(ticket) ||
        pm_copy(journal->journal_path, sizeof(journal->journal_path), path) != 0) {
        cJSON_Delete(journal->root);
        journal->root = NULL;
        snprintf(err, err_size, "move journal is malformed: %.400s", path);
        return -1;
    }
    journal->expected_generation = expected->valueint;
    journal->mapping_generation = mapping->valueint;
    journal->scan_ticket_generation = ticket->valueint;
    cJSON *identity = cJSON_GetObjectItemCaseSensitive(journal->root, "identity");
    cJSON *source = cJSON_IsObject(identity)
        ? cJSON_GetObjectItemCaseSensitive(identity, "source") : NULL;
    cJSON *destination = cJSON_IsObject(identity)
        ? cJSON_GetObjectItemCaseSensitive(identity, "destination") : NULL;
#define LOAD_ID_NUM(object, key, field) do { \
    cJSON *v = cJSON_GetObjectItemCaseSensitive((object), (key)); \
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) goto malformed; \
    journal->field = (__typeof__(journal->field))v->valuedouble; \
} while (0)
    if (!cJSON_IsObject(source) || !cJSON_IsObject(destination)) {
        goto malformed;
    }
    LOAD_ID_NUM(source, "mount_id", source_mount_id);
    LOAD_ID_NUM(destination, "mount_id", destination_mount_id);
    LOAD_ID_NUM(source, "st_dev", source_st_dev);
    LOAD_ID_NUM(destination, "st_dev", destination_st_dev);
    LOAD_ID_NUM(source, "roms_st_dev", source_roms_st_dev);
    LOAD_ID_NUM(destination, "roms_st_dev", destination_roms_st_dev);
    LOAD_ID_NUM(source, "images_st_dev", source_images_st_dev);
    LOAD_ID_NUM(destination, "images_st_dev", destination_images_st_dev);
    LOAD_ID_NUM(source, "roms_mount_id", source_roms_mount_id);
    LOAD_ID_NUM(destination, "roms_mount_id", destination_roms_mount_id);
    LOAD_ID_NUM(source, "images_mount_id", source_images_mount_id);
    LOAD_ID_NUM(destination, "images_mount_id", destination_images_mount_id);
    (void)json_string(source, "fingerprint", journal->source_fingerprint,
                      sizeof(journal->source_fingerprint));
    (void)json_string(destination, "fingerprint", journal->destination_fingerprint,
                      sizeof(journal->destination_fingerprint));
    (void)json_string(source, "device_id", journal->source_device_id,
                      sizeof(journal->source_device_id));
    (void)json_string(destination, "device_id", journal->destination_device_id,
                      sizeof(journal->destination_device_id));
    (void)json_string(source, "roms_device_id", journal->source_roms_device_id,
                      sizeof(journal->source_roms_device_id));
    (void)json_string(destination, "roms_device_id",
                      journal->destination_roms_device_id,
                      sizeof(journal->destination_roms_device_id));
    (void)json_string(source, "images_device_id",
                      journal->source_images_device_id,
                      sizeof(journal->source_images_device_id));
    (void)json_string(destination, "images_device_id",
                      journal->destination_images_device_id,
                      sizeof(journal->destination_images_device_id));
    (void)json_string(source, "roms_fingerprint",
                      journal->source_roms_fingerprint,
                      sizeof(journal->source_roms_fingerprint));
    (void)json_string(destination, "roms_fingerprint",
                      journal->destination_roms_fingerprint,
                      sizeof(journal->destination_roms_fingerprint));
    (void)json_string(source, "images_fingerprint",
                      journal->source_images_fingerprint,
                      sizeof(journal->source_images_fingerprint));
    (void)json_string(destination, "images_fingerprint",
                      journal->destination_images_fingerprint,
                      sizeof(journal->destination_images_fingerprint));
#undef LOAD_ID_NUM
    return 0;
malformed:
    cJSON_Delete(journal->root);
    journal->root = NULL;
    snprintf(err, err_size, "move journal identity is malformed: %.390s", path);
    return -1;
}

static void journal_close(pm_move_journal *journal)
{
    cJSON_Delete(journal ? journal->root : NULL);
    if (journal) {
        journal->root = NULL;
    }
}

static cJSON *source_identity_json(const pm_source *source)
{
    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddNumberToObject(root, "mount_id", (double)source->mount_id) ||
        !cJSON_AddNumberToObject(root, "st_dev", (double)source->st_dev) ||
        !cJSON_AddNumberToObject(root, "roms_st_dev", (double)source->roms_st_dev) ||
        !cJSON_AddNumberToObject(root, "images_st_dev", (double)source->images_st_dev) ||
        !cJSON_AddNumberToObject(root, "roms_mount_id",
                                 (double)source->roms_mount_id) ||
        !cJSON_AddNumberToObject(root, "images_mount_id",
                                 (double)source->images_mount_id) ||
        !cJSON_AddStringToObject(root, "device_id", source->device_id) ||
        !cJSON_AddStringToObject(root, "roms_device_id",
                                source->roms_device_id) ||
        !cJSON_AddStringToObject(root, "images_device_id",
                                source->images_device_id) ||
        !cJSON_AddStringToObject(root, "fingerprint",
                                source->filesystem_fingerprint) ||
        !cJSON_AddStringToObject(root, "roms_fingerprint",
                                source->roms_filesystem_fingerprint) ||
        !cJSON_AddStringToObject(root, "images_fingerprint",
                                source->images_filesystem_fingerprint)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static int journal_create(const pm_context *ctx,
                          const pm_port_package *package,
                          const pm_source *source,
                          const pm_source *destination,
                          int generation,
                          pm_move_journal *journal,
                          char *err,
                          size_t err_size)
{
    memset(journal, 0, sizeof(*journal));
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (pm_format(journal->operation_id, sizeof(journal->operation_id),
                  "pm-%lld-%ld-%d-%s",
                  (long long)now.tv_sec, now.tv_nsec, (int)getpid(),
                  package->slug) != 0) {
        snprintf(err, err_size, "operation id is too long");
        return -1;
    }
    for (char *p = journal->operation_id; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
            *p = '-';
        }
    }
    (void)pm_copy(journal->source_id, sizeof(journal->source_id), source->id);
    (void)pm_copy(journal->destination_id, sizeof(journal->destination_id),
                  destination->id);
    (void)pm_copy(journal->slug, sizeof(journal->slug), package->slug);
    (void)pm_copy(journal->state, sizeof(journal->state), "created");
    journal->expected_generation = generation;
    journal->source_mount_id = source->mount_id;
    journal->destination_mount_id = destination->mount_id;
    journal->source_st_dev = source->st_dev;
    journal->destination_st_dev = destination->st_dev;
    journal->source_roms_st_dev = source->roms_st_dev;
    journal->destination_roms_st_dev = destination->roms_st_dev;
    journal->source_images_st_dev = source->images_st_dev;
    journal->destination_images_st_dev = destination->images_st_dev;
    journal->source_roms_mount_id = source->roms_mount_id;
    journal->destination_roms_mount_id = destination->roms_mount_id;
    journal->source_images_mount_id = source->images_mount_id;
    journal->destination_images_mount_id = destination->images_mount_id;
    (void)pm_copy(journal->source_device_id,
                  sizeof(journal->source_device_id), source->device_id);
    (void)pm_copy(journal->destination_device_id,
                  sizeof(journal->destination_device_id), destination->device_id);
    (void)pm_copy(journal->source_roms_device_id,
                  sizeof(journal->source_roms_device_id), source->roms_device_id);
    (void)pm_copy(journal->destination_roms_device_id,
                  sizeof(journal->destination_roms_device_id),
                  destination->roms_device_id);
    (void)pm_copy(journal->source_images_device_id,
                  sizeof(journal->source_images_device_id),
                  source->images_device_id);
    (void)pm_copy(journal->destination_images_device_id,
                  sizeof(journal->destination_images_device_id),
                  destination->images_device_id);
    (void)pm_copy(journal->source_fingerprint,
                  sizeof(journal->source_fingerprint),
                  source->filesystem_fingerprint);
    (void)pm_copy(journal->destination_fingerprint,
                  sizeof(journal->destination_fingerprint),
                  destination->filesystem_fingerprint);
    (void)pm_copy(journal->source_roms_fingerprint,
                  sizeof(journal->source_roms_fingerprint),
                  source->roms_filesystem_fingerprint);
    (void)pm_copy(journal->destination_roms_fingerprint,
                  sizeof(journal->destination_roms_fingerprint),
                  destination->roms_filesystem_fingerprint);
    (void)pm_copy(journal->source_images_fingerprint,
                  sizeof(journal->source_images_fingerprint),
                  source->images_filesystem_fingerprint);
    (void)pm_copy(journal->destination_images_fingerprint,
                  sizeof(journal->destination_images_fingerprint),
                  destination->images_filesystem_fingerprint);

    char directory[PM_PATH_MAX];
    if (moves_dir(ctx, directory, sizeof(directory)) != 0 ||
        pm_format(journal->journal_path, sizeof(journal->journal_path),
                  "%s/%s.json", directory, journal->operation_id) != 0) {
        snprintf(err, err_size, "journal path is too long");
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *identity = cJSON_CreateObject();
    cJSON *source_json = source_identity_json(source);
    cJSON *destination_json = source_identity_json(destination);
    cJSON *items = cJSON_CreateArray();
    cJSON *launchers = cJSON_CreateArray();
    if (!root || !identity || !source_json || !destination_json ||
        !items || !launchers) {
        cJSON_Delete(root);
        cJSON_Delete(identity);
        cJSON_Delete(source_json);
        cJSON_Delete(destination_json);
        cJSON_Delete(items);
        cJSON_Delete(launchers);
        return -1;
    }
    cJSON_AddNumberToObject(root, "version", PM_MOVE_JOURNAL_VERSION);
    cJSON_AddStringToObject(root, "operation_id", journal->operation_id);
    cJSON_AddStringToObject(root, "source_id", source->id);
    cJSON_AddStringToObject(root, "destination_id", destination->id);
    cJSON_AddStringToObject(root, "slug", package->slug);
    cJSON_AddStringToObject(root, "state", journal->state);
    cJSON_AddNumberToObject(root, "expected_generation", generation);
    cJSON_AddNumberToObject(root, "mapping_generation", 0);
    cJSON_AddNumberToObject(root, "scan_ticket_generation", 0);
    cJSON_AddItemToObject(identity, "source", source_json);
    cJSON_AddItemToObject(identity, "destination", destination_json);
    cJSON_AddItemToObject(root, "identity", identity);
    for (size_t i = 0; i < package->item_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "relpath", package->items[i].relpath);
        cJSON_AddBoolToObject(item, "directory", package->items[i].directory);
        cJSON_AddNumberToObject(item, "bytes", (double)package->items[i].bytes);
        cJSON_AddItemToArray(items, item);
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        cJSON *launcher = cJSON_CreateObject();
        cJSON_AddStringToObject(launcher, "relpath",
                               package->launchers[i].relpath);
        cJSON_AddStringToObject(launcher, "artwork",
                               package->launchers[i].artwork_name);
        cJSON_AddBoolToObject(launcher, "artwork_present",
                             package->launchers[i].artwork_present);
        cJSON_AddItemToArray(launchers, launcher);
    }
    cJSON_AddItemToObject(root, "items", items);
    cJSON_AddItemToObject(root, "launchers", launchers);
    cJSON_AddItemToObject(root, "verified_files", cJSON_CreateArray());
    journal->root = root;
    return journal_store(journal, err, err_size);
}

static const pm_source *validated_source(pm_context *ctx,
                                         const char *source_id,
                                         const pm_move_journal *journal,
                                         bool destination,
                                         bool recovery,
                                         char *err,
                                         size_t err_size)
{
    if (pm_context_refresh_sources(ctx, err, err_size) != 0) {
        return NULL;
    }
    const pm_source *source = pm_sources_by_id(&ctx->sources, source_id);
    if (!source || !source->configured || !source->available) {
        snprintf(err, err_size, "%s is not currently available", source_id);
        return NULL;
    }
    if (!journal) {
        return source;
    }
    unsigned long mount_id = destination
        ? journal->destination_mount_id : journal->source_mount_id;
    uint64_t st_dev = destination
        ? journal->destination_st_dev : journal->source_st_dev;
    uint64_t roms_st_dev = destination
        ? journal->destination_roms_st_dev : journal->source_roms_st_dev;
    uint64_t images_st_dev = destination
        ? journal->destination_images_st_dev : journal->source_images_st_dev;
    unsigned long roms_mount_id = destination
        ? journal->destination_roms_mount_id : journal->source_roms_mount_id;
    unsigned long images_mount_id = destination
        ? journal->destination_images_mount_id : journal->source_images_mount_id;
    const char *device_id = destination
        ? journal->destination_device_id : journal->source_device_id;
    const char *roms_device_id = destination
        ? journal->destination_roms_device_id : journal->source_roms_device_id;
    const char *images_device_id = destination
        ? journal->destination_images_device_id : journal->source_images_device_id;
    const char *fingerprint = destination
        ? journal->destination_fingerprint : journal->source_fingerprint;
    const char *roms_fingerprint = destination
        ? journal->destination_roms_fingerprint
        : journal->source_roms_fingerprint;
    const char *images_fingerprint = destination
        ? journal->destination_images_fingerprint
        : journal->source_images_fingerprint;
    bool exact = source->mount_id == mount_id &&
                 source->st_dev == st_dev &&
                 source->roms_st_dev == roms_st_dev &&
                 source->images_st_dev == images_st_dev &&
                 source->roms_mount_id == roms_mount_id &&
                 source->images_mount_id == images_mount_id &&
                 !strcmp(source->device_id, device_id) &&
                 !strcmp(source->roms_device_id, roms_device_id) &&
                 !strcmp(source->images_device_id, images_device_id);
    bool stable_rebind =
        recovery &&
        fingerprint[0] && source->filesystem_fingerprint[0] &&
        roms_fingerprint[0] && source->roms_filesystem_fingerprint[0] &&
        images_fingerprint[0] && source->images_filesystem_fingerprint[0] &&
        !strcmp(source->filesystem_fingerprint, fingerprint) &&
        !strcmp(source->roms_filesystem_fingerprint, roms_fingerprint) &&
        !strcmp(source->images_filesystem_fingerprint, images_fingerprint);
    if (!exact && !stable_rebind) {
        snprintf(err, err_size,
                 "%s no longer identifies the filesystem recorded by the move",
                 source_id);
        return NULL;
    }
    return source;
}

static bool casefold_child_exists(const char *directory,
                                  const char *name,
                                  char *actual,
                                  size_t actual_size)
{
    DIR *dir = opendir(directory);
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcasecmp(entry->d_name, name)) {
            found = pm_copy(actual, actual_size, entry->d_name) == 0;
            break;
        }
    }
    closedir(dir);
    return found;
}

static bool casefold_path_exists(const char *root, const char *relpath)
{
    char normalized[PM_PATH_MAX];
    char current[PM_PATH_MAX];
    char parts[PM_PATH_MAX];
    if (pm_port_normalize_relative(relpath, normalized, sizeof(normalized)) != 0 ||
        pm_copy(current, sizeof(current), root) != 0 ||
        pm_copy(parts, sizeof(parts), normalized) != 0) {
        return true;
    }
    char *save = NULL;
    for (char *part = strtok_r(parts, "/", &save);
         part;
         part = strtok_r(NULL, "/", &save)) {
        char actual[256];
        if (!casefold_child_exists(current, part, actual, sizeof(actual))) {
            return false;
        }
        char next[PM_PATH_MAX];
        if (pm_join(next, sizeof(next), current, actual) != 0 ||
            pm_copy(current, sizeof(current), next) != 0) {
            return true;
        }
    }
    return true;
}

static int validate_destination(const pm_context *ctx,
                                const pm_port_inventory *inventory,
                                const pm_port_package *package,
                                const pm_source *destination,
                                char *err,
                                size_t err_size)
{
    for (size_t i = 0; i < inventory->package_count; i++) {
        const pm_port_package *other = &inventory->packages[i];
        if (!strcmp(other->source_id, destination->id) &&
            !strcasecmp(other->slug, package->slug)) {
            snprintf(err, err_size, "%s already exists on %s",
                     package->display_name, destination->id);
            return -1;
        }
    }
    for (size_t i = 0; i < package->item_count; i++) {
        if (casefold_path_exists(destination->ports_path,
                                 package->items[i].relpath)) {
            snprintf(err, err_size,
                     "destination has a case-insensitive collision at %s",
                     package->items[i].relpath);
            return -1;
        }
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        if (package->launchers[i].artwork_present &&
            casefold_path_exists(destination->port_images_path,
                                 package->launchers[i].artwork_name)) {
            snprintf(err, err_size,
                     "destination has an artwork collision at %s",
                     package->launchers[i].artwork_name);
            return -1;
        }
    }
    return 0;
}

static int space_available(const char *path,
                           uint64_t required,
                           char *err,
                           size_t err_size)
{
    struct statvfs fs;
    if (statvfs(path, &fs) != 0) {
        snprintf(err, err_size, "cannot check free space at %s: %s",
                 path, strerror(errno));
        return -1;
    }
    uint64_t unit = fs.f_frsize ? (uint64_t)fs.f_frsize :
                    fs.f_bsize ? (uint64_t)fs.f_bsize : 4096u;
    uint64_t rounded = required;
    if (unit > 1) {
        if (required > UINT64_MAX - (unit - 1)) {
            return -1;
        }
        rounded = ((required + unit - 1) / unit) * unit;
    }
    if (rounded > UINT64_MAX - PM_MOVE_RESERVE_BYTES) {
        return -1;
    }
    uint64_t needed = rounded + PM_MOVE_RESERVE_BYTES;
    uint64_t available = fs.f_bavail > UINT64_MAX / unit
        ? UINT64_MAX : (uint64_t)fs.f_bavail * unit;
    if (available < needed) {
        snprintf(err, err_size,
                 "not enough free space at %s (need %llu bytes plus reserve)",
                 path, (unsigned long long)rounded);
        return -1;
    }
    return 0;
}

static int allocated_tree_requirement(const char *path,
                                      uint64_t unit,
                                      uint64_t *required)
{
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        uint64_t size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
        if (size > UINT64_MAX - (unit - 1)) return -1;
        uint64_t rounded = ((size + unit - 1) / unit) * unit;
        if (UINT64_MAX - *required < rounded) return -1;
        *required += rounded;
        return 0;
    }
    if (!S_ISDIR(st.st_mode) || UINT64_MAX - *required < unit) {
        return -1;
    }
    *required += unit;
    int dir_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = dir_fd >= 0 ? fdopendir(dir_fd) : NULL;
    if (!dir) {
        if (dir_fd >= 0) close(dir_fd);
        return -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PM_PATH_MAX];
        if (pm_join(child, sizeof(child), path, entry->d_name) != 0 ||
            allocated_tree_requirement(child, unit, required) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    return rc;
}

static int preflight_space(const pm_port_package *package,
                           const pm_source *source,
                           const pm_source *destination,
                           char *err,
                           size_t err_size)
{
    uint64_t port_bytes = 0;
    uint64_t artwork_bytes = 0;
    struct statvfs ports_fs;
    struct statvfs images_fs;
    if (statvfs(destination->ports_path, &ports_fs) != 0 ||
        statvfs(destination->port_images_path, &images_fs) != 0) {
        snprintf(err, err_size, "cannot inspect destination filesystems");
        return -1;
    }
    uint64_t ports_unit = ports_fs.f_frsize ? (uint64_t)ports_fs.f_frsize :
                          ports_fs.f_bsize ? (uint64_t)ports_fs.f_bsize : 4096u;
    uint64_t images_unit = images_fs.f_frsize ? (uint64_t)images_fs.f_frsize :
                           images_fs.f_bsize ? (uint64_t)images_fs.f_bsize : 4096u;
    /* Hidden staging roots themselves consume one directory allocation. */
    port_bytes = ports_unit;
    artwork_bytes = images_unit;
    for (size_t i = 0; i < package->item_count; i++) {
        char path[PM_PATH_MAX];
        if (pm_join(path, sizeof(path), source->ports_path,
                    package->items[i].relpath) != 0 ||
            allocated_tree_requirement(path, ports_unit, &port_bytes) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        if (!package->launchers[i].artwork_present) {
            continue;
        }
        char path[PM_PATH_MAX];
        if (pm_join(path, sizeof(path), source->port_images_path,
                    package->launchers[i].artwork_name) != 0 ||
            allocated_tree_requirement(path, images_unit,
                                       &artwork_bytes) != 0) {
            return -1;
        }
    }
    if (destination->roms_st_dev == destination->images_st_dev) {
        if (UINT64_MAX - port_bytes < artwork_bytes) {
            return -1;
        }
        return space_available(destination->ports_path,
                               port_bytes + artwork_bytes, err, err_size);
    }
    if (space_available(destination->ports_path, port_bytes, err, err_size) != 0) {
        return -1;
    }
    return artwork_bytes == 0 ? 0 :
        space_available(destination->port_images_path,
                        artwork_bytes, err, err_size);
}

static int mkdir_parent_beneath(const char *root,
                                const char *relpath,
                                char *err,
                                size_t err_size)
{
    char normalized[PM_PATH_MAX];
    char parts[PM_PATH_MAX];
    char current[PM_PATH_MAX];
    if (pm_port_normalize_relative(relpath, normalized, sizeof(normalized)) != 0 ||
        pm_copy(parts, sizeof(parts), normalized) != 0 ||
        pm_copy(current, sizeof(current), root) != 0) {
        return -1;
    }
    char *last = strrchr(parts, '/');
    if (!last) {
        return 0; /* The root itself is the parent. */
    }
    *last = '\0';
    char *save = NULL;
    for (char *part = strtok_r(parts, "/", &save);
         part;
         part = strtok_r(NULL, "/", &save)) {
        char next[PM_PATH_MAX];
        struct stat st;
        if (pm_join(next, sizeof(next), current, part) != 0) {
            return -1;
        }
        if (lstat(next, &st) == 0) {
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                snprintf(err, err_size, "unsafe destination parent: %s", next);
                return -1;
            }
        } else if (errno != ENOENT || mkdir(next, 0755) != 0) {
            snprintf(err, err_size, "cannot create destination parent %s: %s",
                     next, strerror(errno));
            return -1;
        }
        if (pm_copy(current, sizeof(current), next) != 0) {
            return -1;
        }
    }
    return 0;
}

static int add_verified_file(pm_move_journal *journal,
                             const char *root_kind,
                             const char *relpath,
                             const char *sha,
                             char *err,
                             size_t err_size)
{
    cJSON *files = cJSON_GetObjectItemCaseSensitive(journal->root,
                                                    "verified_files");
    cJSON *entry = cJSON_CreateObject();
    if (!cJSON_IsArray(files) || !entry ||
        !cJSON_AddStringToObject(entry, "root", root_kind) ||
        !cJSON_AddStringToObject(entry, "relpath", relpath) ||
        !cJSON_AddStringToObject(entry, "sha256", sha) ||
        !cJSON_AddItemToArray(files, entry)) {
        cJSON_Delete(entry);
        snprintf(err, err_size, "cannot record verified move file");
        return -1;
    }
    return 0;
}

static int copy_regular(const char *source,
                        const char *destination,
                        const char *destination_root,
                        const char *root_kind,
                        const char *verified_relpath,
                        pm_move_journal *journal,
                        char *err,
                        size_t err_size)
{
    struct stat st;
    if (lstat(source, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        snprintf(err, err_size, "unsafe move source: %s", source);
        return -1;
    }
    if (mkdir_parent_beneath(destination_root, verified_relpath,
                             err, err_size) != 0) {
        return -1;
    }
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int out = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   st.st_mode & 0777);
    if (in < 0 || out < 0) {
        snprintf(err, err_size, "cannot stage %s: %s", destination, strerror(errno));
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        return -1;
    }
    char buffer[128 * 1024];
    int rc = 0;
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        size_t used = 0;
        while (used < (size_t)got) {
            ssize_t wrote = write(out, buffer + used, (size_t)got - used);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) {
                rc = -1;
                break;
            }
            used += (size_t)wrote;
        }
        if (rc != 0) break;
    }
    if (rc == 0 && fsync(out) != 0) {
        rc = -1;
    }
    int saved = errno;
    close(in);
    close(out);
    if (rc != 0) {
        errno = saved;
        unlink(destination);
        snprintf(err, err_size, "cannot copy %s: %s", source, strerror(errno));
        return -1;
    }
    char source_sha[65];
    char destination_sha[65];
    if (pm_sha256_file_hex(source, source_sha, err, err_size) != 0 ||
        pm_sha256_file_hex(destination, destination_sha, err, err_size) != 0 ||
        strcmp(source_sha, destination_sha)) {
        unlink(destination);
        if (!err[0]) {
            snprintf(err, err_size, "staged copy verification failed: %s",
                     verified_relpath);
        }
        return -1;
    }
    return add_verified_file(journal, root_kind, verified_relpath,
                             source_sha, err, err_size);
}

static int copy_tree(const char *source,
                     const char *destination,
                     const char *destination_root,
                     const char *root_kind,
                     const char *verified_relpath,
                     pm_move_journal *journal,
                     char *err,
                     size_t err_size)
{
    struct stat st;
    if (lstat(source, &st) != 0 || S_ISLNK(st.st_mode)) {
        snprintf(err, err_size, "unsafe move source: %s", source);
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        return copy_regular(source, destination, destination_root,
                            root_kind, verified_relpath,
                            journal, err, err_size);
    }
    if (!S_ISDIR(st.st_mode) ||
        mkdir_parent_beneath(destination_root, verified_relpath,
                             err, err_size) != 0 ||
        mkdir(destination, st.st_mode & 0777) != 0) {
        snprintf(err, err_size, "cannot create staged directory %s: %s",
                 destination, strerror(errno));
        return -1;
    }
    int source_fd = open(source, O_RDONLY | O_DIRECTORY |
                         O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = source_fd >= 0 ? fdopendir(source_fd) : NULL;
    if (!dir) {
        if (source_fd >= 0) close(source_fd);
        return -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char source_child[PM_PATH_MAX];
        char destination_child[PM_PATH_MAX];
        char rel_child[PM_PATH_MAX];
        if (pm_join(source_child, sizeof(source_child), source, entry->d_name) != 0 ||
            pm_join(destination_child, sizeof(destination_child),
                    destination, entry->d_name) != 0 ||
            pm_join(rel_child, sizeof(rel_child),
                    verified_relpath, entry->d_name) != 0 ||
            copy_tree(source_child, destination_child, destination_root,
                      root_kind, rel_child,
                      journal, err, err_size) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    if (rc == 0) {
        (void)fsync_dir(destination);
    }
    return rc;
}

static int stage_path(const pm_source *destination,
                      const char *operation_id,
                      bool artwork,
                      char *out,
                      size_t out_size)
{
    const char *root = artwork
        ? destination->port_images_path : destination->ports_path;
    return pm_format(out, out_size, "%s/%s%s",
                     root, PM_MOVE_STAGE_PREFIX, operation_id);
}

static int quarantine_path(const pm_source *source,
                           const char *operation_id,
                           bool artwork,
                           char *out,
                           size_t out_size)
{
    const char *root = artwork ? source->port_images_path : source->ports_path;
    return pm_format(out, out_size, "%s/%s%s",
                     root, PM_MOVE_QUARANTINE_PREFIX, operation_id);
}

static int stage_package(const pm_port_package *package,
                         const pm_source *source,
                         const pm_source *destination,
                         pm_move_journal *journal,
                         char *err,
                         size_t err_size)
{
    char ports_stage[PM_PATH_MAX];
    char images_stage[PM_PATH_MAX];
    if (stage_path(destination, journal->operation_id, false,
                   ports_stage, sizeof(ports_stage)) != 0 ||
        stage_path(destination, journal->operation_id, true,
                   images_stage, sizeof(images_stage)) != 0 ||
        mkdir(ports_stage, 0755) != 0 ||
        mkdir(images_stage, 0755) != 0) {
        snprintf(err, err_size, "cannot create hidden move staging: %s",
                 strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < package->item_count; i++) {
        char from[PM_PATH_MAX];
        char to[PM_PATH_MAX];
        if (pm_join(from, sizeof(from), source->ports_path,
                    package->items[i].relpath) != 0 ||
            pm_join(to, sizeof(to), ports_stage,
                    package->items[i].relpath) != 0 ||
            copy_tree(from, to, ports_stage, "ports",
                      package->items[i].relpath,
                      journal, err, err_size) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        if (!package->launchers[i].artwork_present) {
            continue;
        }
        char from[PM_PATH_MAX];
        char to[PM_PATH_MAX];
        if (pm_join(from, sizeof(from), source->port_images_path,
                    package->launchers[i].artwork_name) != 0 ||
            pm_join(to, sizeof(to), images_stage,
                    package->launchers[i].artwork_name) != 0 ||
            copy_tree(from, to, images_stage, "images",
                      package->launchers[i].artwork_name,
                      journal, err, err_size) != 0) {
            return -1;
        }
    }
    (void)fsync_dir(ports_stage);
    (void)fsync_dir(images_stage);
    return journal_transition(journal, "staged_verified", err, err_size);
}

static char *build_relocation_items(const pm_port_package *package,
                                    const char *source_id,
                                    const char *destination_id)
{
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }
    for (size_t i = 0; i < package->launcher_count; i++) {
        char rom[PM_PATH_MAX];
        char art[PM_PATH_MAX];
        if (pm_format(rom, sizeof(rom), "PORTS/%s",
                      package->launchers[i].relpath) != 0 ||
            pm_format(art, sizeof(art), "PORTS/%s",
                      package->launchers[i].artwork_name) != 0) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON *old = cJSON_CreateObject();
        cJSON *new_identity = cJSON_CreateObject();
        if (!item || !old || !new_identity) {
            cJSON_Delete(item);
            cJSON_Delete(old);
            cJSON_Delete(new_identity);
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddStringToObject(old, "source_id", source_id);
        cJSON_AddStringToObject(old, "rom_relpath", rom);
        cJSON_AddStringToObject(new_identity, "source_id", destination_id);
        cJSON_AddStringToObject(new_identity, "rom_relpath", rom);
        if (package->launchers[i].artwork_present) {
            cJSON_AddStringToObject(old, "image_root_kind", "images");
            cJSON_AddStringToObject(old, "image_relpath", art);
            cJSON_AddStringToObject(new_identity, "image_root_kind", "images");
            cJSON_AddStringToObject(new_identity, "image_relpath", art);
        }
        cJSON_AddItemToObject(item, "old", old);
        cJSON_AddItemToObject(item, "new", new_identity);
        cJSON_AddItemToArray(array, item);
    }
    char *printed = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return printed;
}

static int json_array_path(const cJSON *entry,
                           const char *name,
                           char *out,
                           size_t out_size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, name);
    return cJSON_IsString(value) && value->valuestring &&
           pm_port_normalize_relative(value->valuestring, out, out_size) == 0
        ? 0 : -1;
}

static cJSON *new_artwork_manifest(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateObject();
    if (!root || !entries ||
        !cJSON_AddNumberToObject(root, "version", 1) ||
        !cJSON_AddItemToObject(root, "entries", entries)) {
        cJSON_Delete(entries);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *load_artwork_manifest(const char *path)
{
    if (!pm_file_exists(path)) {
        return new_artwork_manifest();
    }
    cJSON *root = read_json_object(path);
    cJSON *version = root
        ? cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
    cJSON *entries = root
        ? cJSON_GetObjectItemCaseSensitive(root, "entries") : NULL;
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        !cJSON_IsObject(entries)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static int artwork_manifest_transfer(const pm_source *source,
                                     const pm_source *destination,
                                     const cJSON *launchers,
                                     bool reverse,
                                     char *err,
                                     size_t err_size)
{
    bool have_artwork = false;
    const cJSON *probe = NULL;
    cJSON_ArrayForEach(probe, launchers) {
        cJSON *present =
            cJSON_GetObjectItemCaseSensitive(probe, "artwork_present");
        if (cJSON_IsTrue(present)) {
            have_artwork = true;
            break;
        }
    }
    if (!have_artwork) {
        return 0;
    }
    const pm_source *from = reverse ? destination : source;
    const pm_source *to = reverse ? source : destination;
    char from_lock[PM_PATH_MAX];
    char to_lock[PM_PATH_MAX];
    char from_manifest[PM_PATH_MAX];
    char to_manifest[PM_PATH_MAX];
    if (pm_join(from_lock, sizeof(from_lock), from->port_images_path,
                ".portmaster-artwork.lock") != 0 ||
        pm_join(to_lock, sizeof(to_lock), to->port_images_path,
                ".portmaster-artwork.lock") != 0 ||
        pm_join(from_manifest, sizeof(from_manifest), from->port_images_path,
                ".portmaster-artwork.json") != 0 ||
        pm_join(to_manifest, sizeof(to_manifest), to->port_images_path,
                ".portmaster-artwork.json") != 0) {
        return -1;
    }
    const char *first_path = strcmp(from_lock, to_lock) <= 0 ? from_lock : to_lock;
    const char *second_path = first_path == from_lock ? to_lock : from_lock;
    int first = open(first_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    int second = open(second_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (first < 0 || second < 0 ||
        flock(first, LOCK_EX) != 0 || flock(second, LOCK_EX) != 0) {
        if (first >= 0) close(first);
        if (second >= 0) close(second);
        snprintf(err, err_size, "cannot lock artwork provenance");
        return -1;
    }
    cJSON *from_root = load_artwork_manifest(from_manifest);
    cJSON *to_root = load_artwork_manifest(to_manifest);
    cJSON *from_entries = from_root
        ? cJSON_GetObjectItemCaseSensitive(from_root, "entries") : NULL;
    cJSON *to_entries = to_root
        ? cJSON_GetObjectItemCaseSensitive(to_root, "entries") : NULL;
    if (!from_root || !to_root || !from_entries || !to_entries) {
        cJSON_Delete(from_root);
        cJSON_Delete(to_root);
        flock(second, LOCK_UN);
        flock(first, LOCK_UN);
        close(second);
        close(first);
        snprintf(err, err_size, "artwork provenance is malformed");
        return -1;
    }
    const cJSON *launcher = NULL;
    cJSON_ArrayForEach(launcher, launchers) {
        char artwork[PM_PATH_MAX];
        const cJSON *present =
            cJSON_GetObjectItemCaseSensitive(launcher, "artwork_present");
        if (!cJSON_IsTrue(present) ||
            json_array_path(launcher, "artwork", artwork, sizeof(artwork)) != 0) {
            continue;
        }
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(from_entries, artwork);
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(to_entries, artwork);
        if (entry) {
            cJSON *copy = cJSON_Duplicate(entry, true);
            if (!copy) {
                continue;
            }
            cJSON *kind = cJSON_GetObjectItemCaseSensitive(copy, "source_kind");
            if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "installed")) {
                cJSON_ReplaceItemInObjectCaseSensitive(
                    copy, "source_id", cJSON_CreateString(to->id));
            }
            cJSON_DeleteItemFromObjectCaseSensitive(to_entries, artwork);
            cJSON_AddItemToObject(to_entries, artwork, copy);
            cJSON_DeleteItemFromObjectCaseSensitive(from_entries, artwork);
        } else if (existing) {
            /* A prior crash may already have completed this individual merge. */
        }
    }
    int rc = write_json_atomic(to_manifest, to_root, err, err_size);
    if (rc == 0) {
        rc = write_json_atomic(from_manifest, from_root, err, err_size);
    }
    cJSON_Delete(from_root);
    cJSON_Delete(to_root);
    flock(second, LOCK_UN);
    flock(first, LOCK_UN);
    close(second);
    close(first);
    return rc;
}

static bool launcher_path(const cJSON *launchers, const char *relpath)
{
    const cJSON *launcher = NULL;
    cJSON_ArrayForEach(launcher, launchers) {
        char value[PM_PATH_MAX];
        if (json_array_path(launcher, "relpath", value, sizeof(value)) == 0 &&
            !strcasecmp(value, relpath)) {
            return true;
        }
    }
    return false;
}

static bool verified_file_matches(const pm_move_journal *journal,
                                  const char *root_kind,
                                  const char *relpath,
                                  const char *path)
{
    cJSON *files = cJSON_GetObjectItemCaseSensitive(journal->root,
                                                    "verified_files");
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, files) {
        const cJSON *root =
            cJSON_GetObjectItemCaseSensitive(entry, "root");
        const cJSON *recorded_path =
            cJSON_GetObjectItemCaseSensitive(entry, "relpath");
        const cJSON *sha =
            cJSON_GetObjectItemCaseSensitive(entry, "sha256");
        if (!cJSON_IsString(root) || !cJSON_IsString(recorded_path) ||
            !cJSON_IsString(sha) || strcmp(root->valuestring, root_kind) ||
            strcasecmp(recorded_path->valuestring, relpath)) {
            continue;
        }
        char actual[65];
        char hash_err[128];
        return strlen(sha->valuestring) == 64 &&
               pm_sha256_file_hex(path, actual, hash_err, sizeof(hash_err)) == 0 &&
               !strcmp(actual, sha->valuestring);
    }
    return false;
}

static int verify_published_tree(const pm_move_journal *journal,
                                 const char *root_kind,
                                 const char *relpath,
                                 const char *destination,
                                 char *err,
                                 size_t err_size)
{
    struct stat st;
    if (lstat(destination, &st) != 0 || S_ISLNK(st.st_mode)) {
        snprintf(err, err_size, "published move item is missing: %s", relpath);
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        if (!verified_file_matches(journal, root_kind, relpath, destination)) {
            snprintf(err, err_size, "published move digest changed: %s", relpath);
            return -1;
        }
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return -1;
    }
    int dir_fd = open(destination, O_RDONLY | O_DIRECTORY |
                      O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = dir_fd >= 0 ? fdopendir(dir_fd) : NULL;
    if (!dir) {
        if (dir_fd >= 0) close(dir_fd);
        return -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char child_rel[PM_PATH_MAX];
        char child_path[PM_PATH_MAX];
        if (pm_join(child_rel, sizeof(child_rel), relpath, entry->d_name) != 0 ||
            pm_join(child_path, sizeof(child_path), destination, entry->d_name) != 0 ||
            verify_published_tree(journal, root_kind, child_rel, child_path,
                                  err, err_size) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    return rc;
}

static int promote_one(const pm_move_journal *journal,
                       const char *stage_root,
                       const char *destination_root,
                       const char *root_kind,
                       const char *relpath,
                       char *err,
                       size_t err_size)
{
    char staged[PM_PATH_MAX];
    char destination[PM_PATH_MAX];
    struct stat stage_st;
    struct stat destination_st;
    if (pm_join(staged, sizeof(staged), stage_root, relpath) != 0 ||
        pm_join(destination, sizeof(destination), destination_root, relpath) != 0) {
        return -1;
    }
    bool have_stage = lstat(staged, &stage_st) == 0;
    bool have_destination = lstat(destination, &destination_st) == 0;
    if (have_stage && have_destination) {
        snprintf(err, err_size, "both staged and final move paths exist: %s", relpath);
        return -1;
    }
    if (have_stage) {
        if (mkdir_parent_beneath(destination_root, relpath,
                                 err, err_size) != 0 ||
            rename(staged, destination) != 0) {
            snprintf(err, err_size, "cannot publish %s: %s",
                     relpath, strerror(errno));
            return -1;
        }
        fsync_parent(staged);
        fsync_parent(destination);
    } else if (!have_destination) {
        snprintf(err, err_size, "move staging disappeared: %s", relpath);
        return -1;
    }
    return verify_published_tree(journal, root_kind, relpath,
                                 destination, err, err_size);
}

static int publish_destination(const pm_source *source,
                               const pm_source *destination,
                               pm_move_journal *journal,
                               char *err,
                               size_t err_size)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(journal->root, "items");
    cJSON *launchers = cJSON_GetObjectItemCaseSensitive(journal->root, "launchers");
    if (!cJSON_IsArray(items) || !cJSON_IsArray(launchers)) {
        return -1;
    }
    char ports_stage[PM_PATH_MAX];
    char images_stage[PM_PATH_MAX];
    if (stage_path(destination, journal->operation_id, false,
                   ports_stage, sizeof(ports_stage)) != 0 ||
        stage_path(destination, journal->operation_id, true,
                   images_stage, sizeof(images_stage)) != 0) {
        return -1;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        char relpath[PM_PATH_MAX];
        if (json_array_path(item, "relpath", relpath, sizeof(relpath)) != 0 ||
            launcher_path(launchers, relpath)) {
            continue;
        }
        if (promote_one(journal, ports_stage, destination->ports_path,
                        "ports", relpath, err, err_size) != 0) {
            return -1;
        }
    }
    const cJSON *launcher = NULL;
    cJSON_ArrayForEach(launcher, launchers) {
        cJSON *present =
            cJSON_GetObjectItemCaseSensitive(launcher, "artwork_present");
        char artwork[PM_PATH_MAX];
        if (cJSON_IsTrue(present) &&
            json_array_path(launcher, "artwork", artwork, sizeof(artwork)) == 0 &&
            promote_one(journal, images_stage, destination->port_images_path,
                        "images", artwork, err, err_size) != 0) {
            return -1;
        }
    }
    if (artwork_manifest_transfer(source, destination, launchers, false,
                                  err, err_size) != 0) {
        return -1;
    }
    cJSON_ArrayForEach(launcher, launchers) {
        char relpath[PM_PATH_MAX];
        if (json_array_path(launcher, "relpath", relpath, sizeof(relpath)) != 0 ||
            promote_one(journal, ports_stage, destination->ports_path,
                        "ports", relpath, err, err_size) != 0) {
            return -1;
        }
    }
    return journal_transition(journal, "destination_published", err, err_size);
}

static int quarantine_one(const pm_move_journal *journal,
                          const char *root_kind,
                          const char *source_root,
                          const char *quarantine_root,
                          const char *relpath,
                          char *err,
                          size_t err_size)
{
    char source[PM_PATH_MAX];
    char quarantine[PM_PATH_MAX];
    struct stat source_st;
    struct stat quarantine_st;
    if (pm_join(source, sizeof(source), source_root, relpath) != 0 ||
        pm_join(quarantine, sizeof(quarantine), quarantine_root, relpath) != 0) {
        return -1;
    }
    bool have_source = lstat(source, &source_st) == 0;
    bool have_quarantine = lstat(quarantine, &quarantine_st) == 0;
    if (have_source && have_quarantine) {
        snprintf(err, err_size, "source and quarantine both exist: %s", relpath);
        return -1;
    }
    if (have_source) {
        if (verify_published_tree(journal, root_kind, relpath, source,
                                  err, err_size) != 0) {
            return -1;
        }
        if (mkdir_parent_beneath(quarantine_root, relpath,
                                 err, err_size) != 0 ||
            rename(source, quarantine) != 0) {
            snprintf(err, err_size, "cannot quarantine %s: %s",
                     relpath, strerror(errno));
            return -1;
        }
        fsync_parent(source);
        fsync_parent(quarantine);
    } else if (!have_quarantine) {
        snprintf(err, err_size, "source move item disappeared: %s", relpath);
        return -1;
    } else if (verify_published_tree(journal, root_kind, relpath, quarantine,
                                     err, err_size) != 0) {
        return -1;
    }
    return 0;
}

static int quarantine_source(const pm_source *source,
                             const pm_source *destination,
                             pm_move_journal *journal,
                             char *err,
                             size_t err_size)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(journal->root, "items");
    cJSON *launchers = cJSON_GetObjectItemCaseSensitive(journal->root, "launchers");
    char ports_quarantine[PM_PATH_MAX];
    char images_quarantine[PM_PATH_MAX];
    if (!cJSON_IsArray(items) || !cJSON_IsArray(launchers) ||
        quarantine_path(source, journal->operation_id, false,
                        ports_quarantine, sizeof(ports_quarantine)) != 0 ||
        quarantine_path(source, journal->operation_id, true,
                        images_quarantine, sizeof(images_quarantine)) != 0) {
        return -1;
    }
    const cJSON *verified_item = NULL;
    cJSON_ArrayForEach(verified_item, items) {
        char relpath[PM_PATH_MAX];
        char path[PM_PATH_MAX];
        if (json_array_path(verified_item, "relpath",
                            relpath, sizeof(relpath)) != 0 ||
            pm_join(path, sizeof(path), destination->ports_path, relpath) != 0 ||
            verify_published_tree(journal, "ports", relpath, path,
                                  err, err_size) != 0) {
            return -1;
        }
    }
    const cJSON *verified_launcher = NULL;
    cJSON_ArrayForEach(verified_launcher, launchers) {
        cJSON *present = cJSON_GetObjectItemCaseSensitive(
            verified_launcher, "artwork_present");
        char artwork[PM_PATH_MAX];
        char path[PM_PATH_MAX];
        if (cJSON_IsTrue(present) &&
            (json_array_path(verified_launcher, "artwork",
                             artwork, sizeof(artwork)) != 0 ||
             pm_join(path, sizeof(path), destination->port_images_path,
                     artwork) != 0 ||
             verify_published_tree(journal, "images", artwork, path,
                                   err, err_size) != 0)) {
            return -1;
        }
    }
    char mkdir_err[256];
    if (!pm_dir_exists(ports_quarantine) &&
        pm_mkdir_p(ports_quarantine, mkdir_err, sizeof(mkdir_err)) != 0) {
        snprintf(err, err_size, "%s", mkdir_err);
        return -1;
    }
    if (!pm_dir_exists(images_quarantine) &&
        pm_mkdir_p(images_quarantine, mkdir_err, sizeof(mkdir_err)) != 0) {
        snprintf(err, err_size, "%s", mkdir_err);
        return -1;
    }
    const cJSON *launcher = NULL;
    cJSON_ArrayForEach(launcher, launchers) {
        char relpath[PM_PATH_MAX];
        if (json_array_path(launcher, "relpath", relpath, sizeof(relpath)) != 0 ||
            quarantine_one(journal, "ports", source->ports_path, ports_quarantine,
                           relpath, err, err_size) != 0) {
            return -1;
        }
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        char relpath[PM_PATH_MAX];
        if (json_array_path(item, "relpath", relpath, sizeof(relpath)) != 0 ||
            launcher_path(launchers, relpath)) {
            continue;
        }
        if (quarantine_one(journal, "ports", source->ports_path, ports_quarantine,
                           relpath, err, err_size) != 0) {
            return -1;
        }
    }
    cJSON_ArrayForEach(launcher, launchers) {
        cJSON *present =
            cJSON_GetObjectItemCaseSensitive(launcher, "artwork_present");
        char artwork[PM_PATH_MAX];
        if (cJSON_IsTrue(present) &&
            json_array_path(launcher, "artwork", artwork, sizeof(artwork)) == 0 &&
            quarantine_one(journal, "images", source->port_images_path, images_quarantine,
                           artwork, err, err_size) != 0) {
            return -1;
        }
    }
    return journal_transition(journal, "source_quarantined", err, err_size);
}

static int cleanup_hidden(const pm_source *source,
                          const pm_source *destination,
                          const char *operation_id,
                          bool include_quarantine,
                          char *err,
                          size_t err_size)
{
    char paths[4][PM_PATH_MAX];
    if (stage_path(destination, operation_id, false, paths[0], sizeof(paths[0])) != 0 ||
        stage_path(destination, operation_id, true, paths[1], sizeof(paths[1])) != 0 ||
        quarantine_path(source, operation_id, false, paths[2], sizeof(paths[2])) != 0 ||
        quarantine_path(source, operation_id, true, paths[3], sizeof(paths[3])) != 0) {
        return -1;
    }
    size_t count = include_quarantine ? 4 : 2;
    for (size_t i = 0; i < count; i++) {
        if ((pm_file_exists(paths[i]) || pm_dir_exists(paths[i])) &&
            pm_rm_rf(paths[i], err, err_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int remove_published(const pm_source *source,
                            const pm_source *destination,
                            pm_move_journal *journal,
                            char *err,
                            size_t err_size)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(journal->root, "items");
    cJSON *launchers = cJSON_GetObjectItemCaseSensitive(journal->root, "launchers");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        char relpath[PM_PATH_MAX];
        char path[PM_PATH_MAX];
        if (json_array_path(item, "relpath", relpath, sizeof(relpath)) != 0 ||
            pm_join(path, sizeof(path), destination->ports_path, relpath) != 0) {
            return -1;
        }
        if ((pm_file_exists(path) || pm_dir_exists(path)) &&
            (verify_published_tree(journal, "ports", relpath, path,
                                   err, err_size) != 0 ||
             pm_rm_rf(path, err, err_size) != 0)) {
                return -1;
        }
    }
    const cJSON *launcher = NULL;
    cJSON_ArrayForEach(launcher, launchers) {
        cJSON *present =
            cJSON_GetObjectItemCaseSensitive(launcher, "artwork_present");
        char artwork[PM_PATH_MAX];
        char path[PM_PATH_MAX];
        if (cJSON_IsTrue(present) &&
            json_array_path(launcher, "artwork", artwork, sizeof(artwork)) == 0 &&
            pm_join(path, sizeof(path), destination->port_images_path, artwork) == 0 &&
            (pm_file_exists(path) || pm_dir_exists(path)) &&
            (verify_published_tree(journal, "images", artwork, path,
                                   err, err_size) != 0 ||
             pm_rm_rf(path, err, err_size) != 0)) {
            return -1;
        }
    }
    if (artwork_manifest_transfer(source, destination, launchers, true,
                                  err, err_size) != 0) {
        return -1;
    }
    return cleanup_hidden(source, destination, journal->operation_id,
                          false, err, err_size);
}

static int socket_path(char *out, size_t out_size)
{
    const char *socket = getenv("UMRK_DAEMON_SOCKET");
    if (!socket || !socket[0]) {
        socket = getenv("JAWAKA_SOCKET_PATH");
    }
    if (socket && socket[0]) {
        return pm_copy(out, out_size, socket);
    }
    return pm_join(out, out_size,
                   pm_env("UMRK_RUNTIME_PATH", "/tmp/jawaka-runtime"),
                   "jawakad.sock");
}

static int resolve_inhibitctl(const pm_context *ctx, char *out, size_t out_size)
{
    const char *explicit_ctl = getenv("JAWAKA_INHIBITCTL");
    if (explicit_ctl && explicit_ctl[0]) {
        return pm_copy(out, out_size, explicit_ctl);
    }
    char platformctl[PM_PATH_MAX];
    if (resolve_platformctl(ctx, platformctl, sizeof(platformctl)) == 0 &&
        strchr(platformctl, '/')) {
        char *slash = strrchr(platformctl, '/');
        *slash = '\0';
        if (pm_join(out, out_size, platformctl, "jawaka-inhibitctl") == 0 &&
            pm_file_exists(out)) {
            return 0;
        }
    }
    return pm_copy(out, out_size, "jawaka-inhibitctl");
}

static pid_t start_inhibitor(const pm_context *ctx, char *err, size_t err_size)
{
    char helper[PM_PATH_MAX];
    char socket[PM_PATH_MAX];
    if (resolve_inhibitctl(ctx, helper, sizeof(helper)) != 0 ||
        socket_path(socket, sizeof(socket)) != 0) {
        snprintf(err, err_size, "cannot resolve the Leaf suspend helper");
        return -1;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp(helper, helper, "--socket", socket, "hold",
               "--reason", "PortMaster package move",
               "--seconds", "600", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char line[256];
    size_t used = 0;
    while (used + 1 < sizeof(line)) {
        char ch;
        ssize_t got = read(pipefd[0], &ch, 1);
        if (got == 1) {
            line[used++] = ch;
            if (ch == '\n') break;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipefd[0]);
    line[used] = '\0';
    if (strncmp(line, "acquired ", 9) != 0) {
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        snprintf(err, err_size, "cannot acquire suspend inhibitor: %s",
                 line[0] ? line : "helper failed");
        return -1;
    }
    return pid;
}

static void stop_inhibitor(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
}

static int operation_lock(const pm_context *ctx,
                          char *path,
                          size_t path_size,
                          char *err,
                          size_t err_size)
{
    char directory[PM_PATH_MAX];
    if (moves_dir(ctx, directory, sizeof(directory)) != 0 ||
        pm_join(path, path_size, directory, ".operation.lock") != 0) {
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        snprintf(err, err_size,
                 "another PortMaster package operation is already active");
        return -1;
    }
    return fd;
}

static int finish_relocation(pm_context *ctx,
                             pm_move_journal *journal,
                             char *err,
                             size_t err_size)
{
    if (relocation_command(ctx, "relocate-finish", journal->operation_id,
                           0, NULL, journal, err, err_size) != 0) {
        return -1;
    }
    if (strcmp(journal->daemon_state, "finished") &&
        strcmp(journal->daemon_state, "finishing")) {
        snprintf(err, err_size, "the launcher returned unexpected finish state: %s",
                 journal->daemon_state);
        return -1;
    }
    for (int attempt = 0;
         attempt < 300 && strcmp(journal->daemon_state, "finished");
         attempt++) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&pause, NULL);
        if (relocation_command(ctx, "relocate-status", journal->operation_id,
                               0, NULL, journal, err, err_size) != 0) {
            return -1;
        }
    }
    if (strcmp(journal->daemon_state, "finished") ||
        journal->scan_ticket_generation <= journal->mapping_generation) {
        snprintf(err, err_size,
                 "Launcher reconciliation is still pending; recovery will resume it");
        return -1;
    }
    return 0;
}

static int update_last_known_inventory(const pm_context *ctx,
                                       const char *slug,
                                       const char *source_id,
                                       const char *destination_id,
                                       char *err,
                                       size_t err_size)
{
    char path[PM_PATH_MAX];
    if (pm_join(path, sizeof(path), ctx->leaf_dir,
                "installed-inventory.json") != 0) {
        return -1;
    }
    cJSON *root = read_json_object(path);
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddItemToObject(root, "sources", cJSON_CreateObject());
    }
    cJSON *sources = cJSON_GetObjectItemCaseSensitive(root, "sources");
    if (!cJSON_IsObject(sources)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "last-known PortMaster inventory is malformed");
        return -1;
    }
    const char *ids[2] = { source_id, destination_id };
    for (size_t index = 0; index < 2; index++) {
        cJSON *record = cJSON_GetObjectItemCaseSensitive(sources, ids[index]);
        if (!cJSON_IsObject(record)) {
            record = cJSON_CreateObject();
            cJSON_AddItemToObject(record, "ports", cJSON_CreateArray());
            cJSON_AddItemToObject(sources, ids[index], record);
        }
        cJSON *ports = cJSON_GetObjectItemCaseSensitive(record, "ports");
        if (!cJSON_IsArray(ports)) {
            cJSON_DeleteItemFromObjectCaseSensitive(record, "ports");
            ports = cJSON_CreateArray();
            cJSON_AddItemToObject(record, "ports", ports);
        }
        for (int i = cJSON_GetArraySize(ports) - 1; i >= 0; i--) {
            cJSON *item = cJSON_GetArrayItem(ports, i);
            if (cJSON_IsString(item) && !strcasecmp(item->valuestring, slug)) {
                cJSON_DeleteItemFromArray(ports, i);
            }
        }
        if (index == 1) {
            cJSON_AddItemToArray(ports, cJSON_CreateString(slug));
        }
    }
    int rc = write_json_atomic(path, root, err, err_size);
    cJSON_Delete(root);
    return rc;
}

static int finalize_move(pm_context *ctx,
                         const pm_source *source,
                         const pm_source *destination,
                         pm_move_journal *journal,
                         char *err,
                         size_t err_size)
{
    if (finish_relocation(ctx, journal, err, err_size) != 0) {
        return -1;
    }
    if (cleanup_hidden(source, destination, journal->operation_id,
                       true, err, err_size) != 0 ||
        update_last_known_inventory(ctx, journal->slug, source->id,
                                    destination->id, err, err_size) != 0 ||
        journal_transition(journal, "complete", err, err_size) != 0) {
        return -1;
    }
    if (unlink(journal->journal_path) != 0 && errno != ENOENT) {
        snprintf(err, err_size, "cannot remove completed move journal: %s",
                 strerror(errno));
        return -1;
    }
    return 0;
}

static int rollback_committed(pm_context *ctx,
                              const pm_source *source,
                              const pm_source *destination,
                              pm_move_journal *journal,
                              char *err,
                              size_t err_size)
{
    if (relocation_command(ctx, "relocate-revert", journal->operation_id,
                           0, NULL, journal, err, err_size) != 0 ||
        strcmp(journal->daemon_state, "reverted") ||
        journal_transition(journal, "db_reverted", err, err_size) != 0 ||
        remove_published(source, destination, journal, err, err_size) != 0 ||
        finish_relocation(ctx, journal, err, err_size) != 0) {
        return -1;
    }
    if (cleanup_hidden(source, destination, journal->operation_id,
                       false, err, err_size) != 0) {
        return -1;
    }
    (void)journal_transition(journal, "complete", err, err_size);
    (void)unlink(journal->journal_path);
    return 0;
}

int pm_move_package(pm_context *ctx,
                    const pm_port_package *requested_package,
                    const char *destination_source_id,
                    pm_move_status_fn status_fn,
                    void *status_userdata,
                    char *err,
                    size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!ctx || !requested_package || !destination_source_id) {
        return -1;
    }
    if (ensure_moves_dir(ctx, err, err_size) != 0) {
        return -1;
    }
    char lock_path[PM_PATH_MAX];
    int lock_fd = operation_lock(ctx, lock_path, sizeof(lock_path), err, err_size);
    if (lock_fd < 0) {
        return -1;
    }
    int result = -1;
    pid_t inhibitor = -1;
    pm_port_inventory inventory = {0};
    pm_move_journal journal = {0};
    bool have_journal = false;
    bool reservation_possible = false;

    status_emit(status_fn, status_userdata, "Checking launcher move support...");
    pm_move_capability capability;
    if (pm_move_probe_capability(ctx, &capability, err, err_size) != 0 ||
        !capability.supported) {
        if (!err[0]) snprintf(err, err_size, "%s", capability.detail);
        goto done;
    }
    inhibitor = start_inhibitor(ctx, err, err_size);
    if (inhibitor < 0) {
        goto done;
    }
    status_emit(status_fn, status_userdata, "Validating installed package...");
    if (pm_port_inventory_load(ctx, &inventory, err, err_size) != 0) {
        goto done;
    }
    const pm_port_package *package = pm_port_inventory_find(
        &inventory, requested_package->source_id, requested_package->slug);
    if (!package) {
        snprintf(err, err_size, "installed package changed; reopen Manage Ports");
        goto done;
    }
    if (!package->movable) {
        snprintf(err, err_size, "%s", package->blocked_reason);
        goto done;
    }
    const pm_source *source = validated_source(
        ctx, package->source_id, NULL, false, false, err, err_size);
    const pm_source *destination = source
        ? validated_source(ctx, destination_source_id, NULL, true, false,
                           err, err_size) : NULL;
    if (!source || !destination) {
        goto done;
    }
    if (!strcmp(source->id, destination->id)) {
        snprintf(err, err_size, "package is already on %s", destination->id);
        goto done;
    }
    char mkdir_err[512];
    if (pm_mkdir_p(destination->ports_path,
                   mkdir_err, sizeof(mkdir_err)) != 0 ||
        pm_mkdir_p(destination->port_images_path,
                   mkdir_err, sizeof(mkdir_err)) != 0) {
        snprintf(err, err_size,
                 "cannot prepare destination content roots: %s", mkdir_err);
        goto done;
    }
    source = validated_source(ctx, package->source_id, NULL, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, NULL, true,
                           false, err, err_size) : NULL;
    if (!source || !destination) {
        goto done;
    }
    if (validate_destination(ctx, &inventory, package, destination,
                             err, err_size) != 0 ||
        preflight_space(package, source, destination, err, err_size) != 0) {
        goto done;
    }
    int generation = 0;
    if (library_generation(ctx, &generation, err, err_size) != 0 ||
        journal_create(ctx, package, source, destination, generation,
                       &journal, err, err_size) != 0) {
        goto done;
    }
    have_journal = true;

    status_emit(status_fn, status_userdata, "Copying and verifying package...");
    int stage_rc = stage_package(package, source, destination, &journal,
                                 err, err_size);
    if (stage_rc != 0) {
        if (stage_rc == -2) {
            goto done;
        }
        (void)cleanup_hidden(source, destination, journal.operation_id,
                             false, err, err_size);
        (void)unlink(journal.journal_path);
        goto done;
    }
    source = validated_source(ctx, package->source_id, &journal, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, &journal, true,
                           false, err, err_size) : NULL;
    if (!source || !destination ||
        validate_destination(ctx, &inventory, package, destination,
                             err, err_size) != 0) {
        goto abort_before_commit;
    }

    char *items_json = build_relocation_items(
        package, source->id, destination->id);
    if (!items_json) {
        snprintf(err, err_size, "cannot encode launcher relocation batch");
        goto abort_before_commit;
    }
    status_emit(status_fn, status_userdata, "Reserving launcher library entries...");
    int transition_rc = journal_transition(
        &journal, "daemon_prepared", err, err_size);
    if (transition_rc != 0) {
        cJSON_free(items_json);
        if (transition_rc == -2) goto done;
        goto abort_before_commit;
    }
    int prepare_rc = relocation_command(
        ctx, "relocate-prepare", journal.operation_id,
        generation, items_json, &journal, err, err_size);
    cJSON_free(items_json);
    if (prepare_rc != 0) {
        bool explicit_rejection = strstr(err, "relocation prepare failed:") != NULL;
        char status_err[512];
        if (relocation_command(ctx, "relocate-status", journal.operation_id,
                               0, NULL, &journal,
                               status_err, sizeof(status_err)) == 0 &&
            !strcmp(journal.daemon_state, "prepared")) {
            reservation_possible = true;
        } else if (explicit_rejection) {
            reservation_possible = false;
            goto abort_before_commit;
        } else {
            /* A transport failure after acceptance is uncertain. Keep the
             * staged copy and conservative journal for explicit recovery. */
            goto done;
        }
    } else if (strcmp(journal.daemon_state, "prepared")) {
        snprintf(err, err_size, "the launcher returned unexpected prepare state: %s",
                 journal.daemon_state);
        goto done;
    } else {
        reservation_possible = true;
    }
    source = validated_source(ctx, package->source_id, &journal, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, &journal, true,
                           false, err, err_size) : NULL;
    if (!source || !destination ||
        validate_destination(ctx, &inventory, package, destination,
                             err, err_size) != 0) {
        goto abort_before_commit;
    }

    status_emit(status_fn, status_userdata, "Preserving launcher game identity...");
    if (relocation_command(ctx, "relocate-commit", journal.operation_id,
                           0, NULL, &journal, err, err_size) != 0) {
        char status_err[512];
        if (relocation_command(ctx, "relocate-status", journal.operation_id,
                               0, NULL, &journal,
                               status_err, sizeof(status_err)) != 0) {
            /* Commit may have succeeded despite the lost reply. */
            goto done;
        }
        if (strcmp(journal.daemon_state, "committed")) {
            goto abort_before_commit;
        }
    }
    transition_rc = journal_transition(
        &journal, "db_committed", err, err_size);
    if (strcmp(journal.daemon_state, "committed") ||
        transition_rc != 0) {
        goto done;
    }

    source = validated_source(ctx, package->source_id, &journal, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, &journal, true,
                           false, err, err_size) : NULL;
    if (!source || !destination) {
        goto done;
    }
    status_emit(status_fn, status_userdata, "Publishing destination files...");
    int publish_rc = publish_destination(source, destination, &journal,
                                         err, err_size);
    if (publish_rc != 0) {
        if (publish_rc == -2) {
            goto done;
        }
        char original[512];
        (void)pm_copy(original, sizeof(original), err);
        char rollback_err[512];
        if (rollback_committed(ctx, source, destination, &journal,
                               rollback_err, sizeof(rollback_err)) != 0) {
            snprintf(err, err_size,
                     "%s; rollback is pending recovery: %s",
                     original, rollback_err);
        } else {
            snprintf(err, err_size, "%s; the launcher mapping was safely reverted",
                     original);
        }
        goto done;
    }
    source = validated_source(ctx, package->source_id, &journal, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, &journal, true,
                           false, err, err_size) : NULL;
    if (!source || !destination) {
        goto done;
    }
    status_emit(status_fn, status_userdata, "Removing source publication...");
    if (quarantine_source(source, destination, &journal, err, err_size) != 0) {
        goto done;
    }
    source = validated_source(ctx, package->source_id, &journal, false,
                              false, err, err_size);
    destination = source
        ? validated_source(ctx, destination_source_id, &journal, true,
                           false, err, err_size) : NULL;
    if (!source || !destination) {
        goto done;
    }
    status_emit(status_fn, status_userdata, "Reconciling launcher library...");
    if (finalize_move(ctx, source, destination, &journal,
                      err, err_size) != 0) {
        goto done;
    }
    result = 0;
    goto done;

abort_before_commit:
    if (have_journal && !strcmp(journal.state, "daemon_prepared") &&
        reservation_possible) {
        char abort_err[256];
        if (relocation_command(ctx, "relocate-abort", journal.operation_id,
                               0, NULL, &journal,
                               abort_err, sizeof(abort_err)) != 0 ||
            strcmp(journal.daemon_state, "aborted")) {
            if (!err[0]) {
                snprintf(err, err_size,
                         "Launcher abort is uncertain; move remains pending");
            }
            goto done;
        }
    }
    if (source && destination) {
        char cleanup_err[256];
        (void)cleanup_hidden(source, destination, journal.operation_id,
                             false, cleanup_err, sizeof(cleanup_err));
    }
    if (have_journal) {
        (void)unlink(journal.journal_path);
    }

done:
    journal_close(&journal);
    pm_port_inventory_free(&inventory);
    stop_inhibitor(inhibitor);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return result;
}

static int recover_one(pm_context *ctx,
                       pm_move_journal *journal,
                       pm_move_status_fn status_fn,
                       void *status_userdata,
                       char *err,
                       size_t err_size)
{
    const pm_source *source = validated_source(
        ctx, journal->source_id, journal, false, true, err, err_size);
    const pm_source *destination = source
        ? validated_source(ctx, journal->destination_id, journal, true,
                           true, err, err_size) : NULL;
    if (!source || !destination) {
        return 1;
    }

    if (!strcmp(journal->state, "complete")) {
        if (unlink(journal->journal_path) != 0 && errno != ENOENT) {
            return -1;
        }
        return 0;
    }
    if (!strcmp(journal->state, "created") ||
        !strcmp(journal->state, "staged_verified")) {
        status_emit(status_fn, status_userdata,
                    "Cleaning an interrupted pre-reservation move...");
        if (cleanup_hidden(source, destination, journal->operation_id,
                           false, err, err_size) != 0) {
            return 1;
        }
        if (unlink(journal->journal_path) != 0 && errno != ENOENT) {
            return -1;
        }
        return 0;
    }
    if (!strcmp(journal->state, "daemon_prepared")) {
        status_emit(status_fn, status_userdata,
                    "Checking an interrupted launcher reservation...");
        if (relocation_command(ctx, "relocate-status", journal->operation_id,
                               0, NULL, journal, err, err_size) != 0) {
            return 1;
        }
        if (!strcmp(journal->daemon_state, "committed")) {
            if (journal_transition(journal, "db_committed",
                                   err, err_size) != 0) {
                return 1;
            }
        } else if (!strcmp(journal->daemon_state, "prepared")) {
            if (relocation_command(ctx, "relocate-abort", journal->operation_id,
                                   0, NULL, journal, err, err_size) != 0 ||
                cleanup_hidden(source, destination, journal->operation_id,
                               false, err, err_size) != 0) {
                return 1;
            }
            if (unlink(journal->journal_path) != 0 && errno != ENOENT) {
                return -1;
            }
            return 0;
        } else if (!strcmp(journal->daemon_state, "aborted")) {
            if (cleanup_hidden(source, destination, journal->operation_id,
                               false, err, err_size) != 0) {
                return 1;
            }
            (void)unlink(journal->journal_path);
            return 0;
        } else {
            snprintf(err, err_size, "unexpected launcher recovery state: %s",
                     journal->daemon_state);
            return 1;
        }
    }
    if (!strcmp(journal->state, "db_committed")) {
        status_emit(status_fn, status_userdata,
                    "Resuming destination publication...");
        if (publish_destination(source, destination, journal,
                                err, err_size) != 0) {
            return 1;
        }
        source = validated_source(ctx, journal->source_id, journal,
                                  false, true, err, err_size);
        destination = source
            ? validated_source(ctx, journal->destination_id, journal,
                               true, true, err, err_size) : NULL;
        if (!source || !destination) return 1;
    }
    if (!strcmp(journal->state, "destination_published")) {
        status_emit(status_fn, status_userdata,
                    "Resuming source quarantine...");
        if (quarantine_source(source, destination, journal, err, err_size) != 0) {
            return 1;
        }
        source = validated_source(ctx, journal->source_id, journal,
                                  false, true, err, err_size);
        destination = source
            ? validated_source(ctx, journal->destination_id, journal,
                               true, true, err, err_size) : NULL;
        if (!source || !destination) return 1;
    }
    if (!strcmp(journal->state, "source_quarantined")) {
        status_emit(status_fn, status_userdata,
                    "Resuming launcher reconciliation...");
        return finalize_move(ctx, source, destination, journal,
                             err, err_size) == 0 ? 0 : 1;
    }
    if (!strcmp(journal->state, "db_reverted")) {
        status_emit(status_fn, status_userdata,
                    "Finishing a reverted package move...");
        if (remove_published(source, destination, journal,
                             err, err_size) != 0 ||
            finish_relocation(ctx, journal, err, err_size) != 0 ||
            cleanup_hidden(source, destination, journal->operation_id,
                           false, err, err_size) != 0) {
            return 1;
        }
        (void)journal_transition(journal, "complete", err, err_size);
        (void)unlink(journal->journal_path);
        return 0;
    }
    snprintf(err, err_size, "unknown move journal state: %s", journal->state);
    return -1;
}

int pm_move_pending_count(const pm_context *ctx)
{
    char directory[PM_PATH_MAX];
    if (!ctx || moves_dir(ctx, directory, sizeof(directory)) != 0) {
        return 0;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (entry->d_name[0] != '.' && len > 5 &&
            !strcasecmp(entry->d_name + len - 5, ".json")) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

int pm_move_recover_all(pm_context *ctx,
                        pm_move_status_fn status_fn,
                        void *status_userdata,
                        char *summary,
                        size_t summary_size)
{
    if (summary && summary_size) summary[0] = '\0';
    int initial = pm_move_pending_count(ctx);
    if (initial == 0) {
        if (summary && summary_size) {
            snprintf(summary, summary_size, "No pending package moves.");
        }
        return 0;
    }
    char err[512];
    if (ensure_moves_dir(ctx, err, sizeof(err)) != 0) {
        if (summary && summary_size) snprintf(summary, summary_size, "%s", err);
        return -1;
    }
    char lock_path[PM_PATH_MAX];
    int lock_fd = operation_lock(ctx, lock_path, sizeof(lock_path),
                                 err, sizeof(err));
    if (lock_fd < 0) {
        if (summary && summary_size) snprintf(summary, summary_size, "%s", err);
        return -1;
    }
    pm_move_capability capability;
    if (pm_move_probe_capability(ctx, &capability, err, sizeof(err)) != 0 ||
        !capability.supported) {
        if (summary && summary_size) {
            snprintf(summary, summary_size,
                     "Pending moves remain: %s",
                     err[0] ? err : capability.detail);
        }
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 1;
    }
    pid_t inhibitor = start_inhibitor(ctx, err, sizeof(err));
    if (inhibitor < 0) {
        if (summary && summary_size) snprintf(summary, summary_size, "%s", err);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 1;
    }
    char directory[PM_PATH_MAX];
    (void)moves_dir(ctx, directory, sizeof(directory));
    DIR *dir = opendir(directory);
    int recovered = 0;
    int pending = 0;
    int malformed = 0;
    if (!dir) {
        pending = initial;
    } else {
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (entry->d_name[0] == '.' || len <= 5 ||
                strcasecmp(entry->d_name + len - 5, ".json")) {
                continue;
            }
            char path[PM_PATH_MAX];
            pm_move_journal journal;
            if (pm_join(path, sizeof(path), directory, entry->d_name) != 0 ||
                journal_load(path, &journal, err, sizeof(err)) != 0) {
                malformed++;
                continue;
            }
            int rc = recover_one(ctx, &journal, status_fn, status_userdata,
                                 err, sizeof(err));
            if (rc == 0) recovered++;
            else if (rc > 0) pending++;
            else malformed++;
            journal_close(&journal);
        }
        closedir(dir);
    }
    stop_inhibitor(inhibitor);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    if (summary && summary_size) {
        snprintf(summary, summary_size,
                 "Move recovery: recovered=%d pending=%d malformed=%d%s%s",
                 recovered, pending, malformed,
                 (pending || malformed) && err[0] ? "\n" : "",
                 (pending || malformed) && err[0] ? err : "");
    }
    return pending || malformed ? 1 : 0;
}
