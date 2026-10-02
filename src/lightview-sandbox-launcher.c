#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef LIGHTVIEW_EXECUTABLE
#define LIGHTVIEW_EXECUTABLE "/usr/local/bin/lightview"
#endif

extern char **environ;

static void fail(const char *message)
{
    fprintf(stderr, "lightview-sandbox-launcher: %s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void fail_plain(const char *message)
{
    fprintf(stderr, "lightview-sandbox-launcher: %s\n", message);
    exit(EXIT_FAILURE);
}

static char *decode_mount_path(const char *encoded)
{
    size_t length = strlen(encoded);
    char *decoded = malloc(length + 1);
    if (!decoded) fail("malloc");

    char *out = decoded;
    for (size_t i = 0; i < length; i++) {
        if (encoded[i] == '\\' && i + 3 < length &&
            encoded[i + 1] >= '0' && encoded[i + 1] <= '7' &&
            encoded[i + 2] >= '0' && encoded[i + 2] <= '7' &&
            encoded[i + 3] >= '0' && encoded[i + 3] <= '7') {
            *out++ = (char)(((encoded[i + 1] - '0') << 6) |
                          ((encoded[i + 2] - '0') << 3) |
                          (encoded[i + 3] - '0'));
            i += 3;
        } else {
            *out++ = encoded[i];
        }
    }
    *out = '\0';
    return decoded;
}

static int path_length_descending(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    size_t a_length = strlen(*a);
    size_t b_length = strlen(*b);
    if (a_length < b_length) return 1;
    if (a_length > b_length) return -1;
    return strcmp(*a, *b);
}

static char **proc_submounts(size_t *count)
{
    FILE *mountinfo = fopen("/proc/self/mountinfo", "re");
    if (!mountinfo) fail("open /proc/self/mountinfo");

    char **paths = NULL;
    size_t used = 0, allocated = 0;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, mountinfo) >= 0) {
        char *save = NULL;
        char *field = strtok_r(line, " ", &save);
        for (unsigned number = 1; field && number < 5; number++)
            field = strtok_r(NULL, " ", &save);
        if (!field) continue;

        char *path = decode_mount_path(field);
        if (strncmp(path, "/proc/", 6) != 0) {
            free(path);
            continue;
        }
        if (used == allocated) {
            size_t next = allocated ? allocated * 2 : 16;
            char **grown = realloc(paths, next * sizeof(*paths));
            if (!grown) fail("realloc");
            paths = grown;
            allocated = next;
        }
        paths[used++] = path;
    }
    if (ferror(mountinfo)) fail("read /proc/self/mountinfo");
    free(line);
    if (fclose(mountinfo) != 0) fail("close /proc/self/mountinfo");

    if (used > 1)
        qsort(paths, used, sizeof(*paths), path_length_descending);
    *count = used;
    return paths;
}

static void detach_proc_submounts(void)
{
    size_t count = 0;
    char **paths = proc_submounts(&count);
    for (size_t i = 0; i < count; i++) {
        if (umount2(paths[i], MNT_DETACH | UMOUNT_NOFOLLOW) != 0 &&
            errno != EINVAL && errno != ENOENT)
            fail(paths[i]);
        free(paths[i]);
    }
    free(paths);

    paths = proc_submounts(&count);
    if (count != 0) {
        for (size_t i = 0; i < count; i++) free(paths[i]);
        free(paths);
        fail_plain("a nested /proc mount remains after isolation");
    }
    free(paths);
}

static int open_trusted_lightview(void)
{
    int fd = open(LIGHTVIEW_EXECUTABLE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) fail("open trusted Lightview executable");

    struct stat state;
    if (fstat(fd, &state) != 0) fail("inspect trusted Lightview executable");
    if (!S_ISREG(state.st_mode) || state.st_uid != 0 ||
        (state.st_mode & (S_ISUID | S_ISGID)) != 0 ||
        (state.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (state.st_mode & S_IXUSR) == 0)
        fail_plain("Lightview executable must be a root-owned, non-writable executable file");
    return fd;
}

int main(int argc, char **argv)
{
    (void)argc;
    uid_t user = getuid();
    gid_t group = getgid();

    if (user == 0) fail_plain("run this launcher as the unprivileged desktop user");
    if (geteuid() != 0)
        fail_plain("launcher is not installed setuid root; use Lightview directly on normal hosts");

    int executable = open_trusted_lightview();

    if (unshare(CLONE_NEWNS) != 0) fail("create private mount namespace");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        fail("make mount namespace private");
    detach_proc_submounts();

    if (setresgid(group, group, group) != 0) fail("drop group privileges");
    if (setresuid(user, user, user) != 0) fail("drop user privileges");
    if (getuid() != user || geteuid() != user || getgid() != group || getegid() != group)
        fail_plain("privilege drop verification failed");
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0 && errno != EINVAL)
        fail("clear ambient capabilities");
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        fail("set no-new-privileges");

    argv[0] = (char *)LIGHTVIEW_EXECUTABLE;
    fexecve(executable, argv, environ);
    fail("execute Lightview");
    return EXIT_FAILURE;
}
