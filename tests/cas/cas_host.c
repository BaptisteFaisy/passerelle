/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_host.c - the host side of the truth (plain POSIX, never the code
 * under test).
 *
 * The replace case (ROB-04) and the hard-link case (gap 68) both let the PC
 * change a file behind the share's back: a new file renamed over the old
 * one, an in-place truncate-and-rewrite, an unlink, and a known modification
 * time so that "the client still serves the old mtime" is a statement about
 * the cache and not about the clock. These four helpers are the whole of
 * that host side, written once for every adapter under test.
 *
 * Covers ROB-04, FS-12, FS-13 and the hard-link semantics of gap 68.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

bool cas_host_write(const char *root, const char *rel, const void *data, size_t len)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            return false;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    close(fd);
    return true;
}

bool cas_host_rename(const char *root, const char *from, const char *to)
{
    char a[CAS_PATH_MAX];
    char b[CAS_PATH_MAX];
    cas_path_join(a, sizeof(a), root, from);
    cas_path_join(b, sizeof(b), root, to);
    return rename(a, b) == 0;
}

bool cas_host_unlink(const char *root, const char *rel)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    return unlink(path) == 0;
}

bool cas_host_set_mtime(const char *root, const char *rel, long long sec)
{
    char path[CAS_PATH_MAX];
    struct timespec ts[2];
    cas_path_join(path, sizeof(path), root, rel);
    ts[0].tv_sec = (time_t)sec;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = (time_t)sec;
    ts[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, ts, 0) == 0;
}
