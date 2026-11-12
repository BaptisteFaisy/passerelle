/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_truth.c - POSIX access to the directory the server serves.
 *
 * Every function here goes straight to the host file system: open/read/write,
 * lstat, mkdir, symlink, opendir. The benches compare what the 9P client
 * reported against these results, which is what makes the conformance claims
 * independent of the code under test (P9-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_truth.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRUTH_PATH_MAX 1024

void ref9p_truth_path(const char *root, const char *rel, char *out, size_t cap)
{
    if (root == NULL || out == NULL || cap == 0)
        return;
    if (rel == NULL || rel[0] == '\0')
        snprintf(out, cap, "%s", root);
    else
        snprintf(out, cap, "%s/%s", root, rel);
}

bool ref9p_truth_write(const char *root, const char *rel, const void *data,
                       size_t len)
{
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    const uint8_t *p = (const uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += (size_t)n;
    }
    return close(fd) == 0;
}

bool ref9p_truth_read(const char *root, const char *rel, void *buf, size_t cap,
                      size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, p + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (out_len != NULL)
        *out_len = got;
    return true;
}

bool ref9p_truth_stat(const char *root, const char *rel, struct stat *out)
{
    if (out == NULL)
        return false;
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    return lstat(path, out) == 0;
}

bool ref9p_truth_exists(const char *root, const char *rel)
{
    struct stat st;
    return ref9p_truth_stat(root, rel, &st);
}

bool ref9p_truth_mkdir(const char *root, const char *rel, mode_t mode)
{
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    return mkdir(path, mode) == 0;
}

bool ref9p_truth_symlink(const char *root, const char *target, const char *rel)
{
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    return symlink(target, path) == 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

size_t ref9p_truth_names(const char *root, const char *rel, char names[][256],
                         size_t max_names)
{
    char path[TRUTH_PATH_MAX];
    ref9p_truth_path(root, rel, path, sizeof path);
    DIR *dir = opendir(path);
    if (dir == NULL)
        return (size_t)-1;
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_names) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(names[count], 256, "%s", ent->d_name);
        count++;
    }
    closedir(dir);
    qsort(names, count, sizeof names[0], name_cmp);
    return count;
}
