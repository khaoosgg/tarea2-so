/* Habilita las extensiones POSIX/BSD (mmap anonimo, mutex robustos, rand_r...)
 * incluso si se compila con 'gcc -std=c17' sin -D. Debe ir antes de todo #include. */
#define _DEFAULT_SOURCE

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int log_fd = -1;

int log_open(const char *path) {
    if (!path || !*path) return 0;
    log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return log_fd < 0 ? -1 : 0;
}

void log_close(void) {
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
}

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) { if (errno == EINTR) continue; return; }
        buf += w;
        len -= (size_t)w;
    }
}

void log_block(const char *buf, size_t len) {
    write_all(STDOUT_FILENO, buf, len);
    if (log_fd >= 0) write_all(log_fd, buf, len);
}

void log_msg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    log_block(buf, len);
}
