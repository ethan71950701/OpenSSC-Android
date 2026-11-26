#include "guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int write_count(const char *path, int n) {
    char tmp[512], buf[16];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int len = snprintf(buf, sizeof(buf), "%d\n", n);
    int ok = write(fd, buf, (size_t)len) == len && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) return -1;
    return 0;
}

/* Missing file = 0. Anything unreadable or not a small number = -1. */
static int read_count(const char *path) {
    char buf[16];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r < 1 || buf[0] < '0' || buf[0] > '9') return -1;
    int n = 0;
    for (ssize_t i = 0; i < r && buf[i] >= '0' && buf[i] <= '9' && n < 1000; i++) n = n * 10 + buf[i] - '0';
    return n;
}

int guard_begin(const char *path) {
    int n = read_count(path);
    if (n < 0) return -1;
    if (n >= GUARD_LIMIT) return 0;
    return write_count(path, n + 1) == 0 ? n + 1 : -1;
}

int guard_success(const char *path) { return write_count(path, 0); }

int guard_count(const char *path) { return read_count(path); }
