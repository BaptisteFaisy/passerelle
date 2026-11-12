/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_truth.c - the truth side of the directed cases (plain POSIX, never the
 * code under test).
 *
 * These helpers read the real directory with plain POSIX calls: what the
 * layer under test reports is only accepted when the file system itself
 * agrees. They live in their own file (and not in the fixture) so that the
 * case bodies of cas_cases.h can use them whatever the adapter under test:
 * the POSIX backend, the 9P adapter or the FUSE adapter.
 *
 * Covers ROB-04 (gap 19), FS-12, FS-13 and the hard-link semantics of
 * gap 68.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"

#include "test_cred.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

uint64_t cas_fake_clock(void *user)
{
    return *(uint64_t *)user;
}

/* --- truth side ----------------------------------------------------------- */

static bool cas_truth_stat(const char *root, const char *rel, struct stat *st)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    return lstat(path, st) == 0;
}

uint64_t cas_truth_size(const char *root, const char *rel)
{
    struct stat st;
    if (!cas_truth_stat(root, rel, &st))
        return 0;
    return (uint64_t)st.st_size;
}

bool cas_truth_exists(const char *root, const char *rel)
{
    struct stat st;
    return cas_truth_stat(root, rel, &st);
}

uint64_t cas_truth_blocks(const char *root, const char *rel)
{
    struct stat st;
    if (!cas_truth_stat(root, rel, &st))
        return 0;
    return (uint64_t)st.st_blocks * 512u; /* POSIX: st_blocks is in 512 B units */
}

size_t cas_truth_count(const char *root, const char *rel)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    DIR *d = opendir(path);
    if (d == NULL)
        return (size_t)-1;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        n++;
    }
    closedir(d);
    return n;
}

bool cas_truth_read(const char *root, const char *rel, void *buf, size_t cap,
                    size_t *out_len)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    *out_len = fread(buf, 1, cap, f);
    fclose(f);
    return true;
}

int cas_truth_mkdir_errno(const char *root, const char *rel, uint32_t mode)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    if (mkdir(path, (mode_t)mode) == 0)
        return 0;
    return errno;
}

int cas_truth_stat_errno(const char *root, const char *rel)
{
    struct stat st;
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    if (lstat(path, &st) == 0)
        return 0;
    return errno;
}

int cas_truth_create_errno(const char *root, const char *rel)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;
    close(fd);
    return 0;
}
