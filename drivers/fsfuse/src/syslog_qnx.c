/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/syslog_qnx.c - the virtiofs driver's system log identity.
 *
 * drivers/common/src/syslog_qnx.c owns the slog2 backend and the counter
 * export; this file owns what is specific to this driver: the buffer set name
 * (`slog2info -b fsfuse` finds it), the start-up banner with the negotiated
 * parameters, and the shutdown line that closes the accounting.
 *
 * Covered requirements: QNX-08 (writes to the system log and exposes per
 * operation counters), QNX-02 (slog2 is part of libc: no extra dependency).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/log.h"

#include <stdint.h>

pgw_err fsfuse_log_open(void)
{
    return pgw_slog2_open("fsfuse");
}

void fsfuse_log_close(void)
{
    pgw_slog2_close();
}

void fsfuse_log_mounted(const pgw_mount_opts *opts, uint32_t queues)
{
    if (opts == NULL)
        return;
    PGW_INFO("fsfuse: tag=%s mount=%s queues=%u max_write=%u timeout=%ums "
             "%s%s%s",
             opts->tag, opts->mount_point != NULL ? opts->mount_point : "?",
             queues, opts->msize, opts->timeout_ms,
             opts->fs.readonly ? "ro " : "rw ",
             opts->fs.noexec ? "noexec " : "",
             opts->fs.nosuid ? "nosuid" : "suid");
}

void fsfuse_log_unmounted(void)
{
    PGW_INFO("fsfuse: unmounted: req=%llu resp=%llu err=%llu timeout=%llu "
             "bytes_read=%llu bytes_written=%llu",
             (unsigned long long)pgw_stat_get(PGW_STAT_REQ),
             (unsigned long long)pgw_stat_get(PGW_STAT_RESP),
             (unsigned long long)pgw_stat_get(PGW_STAT_ERR),
             (unsigned long long)pgw_stat_get(PGW_STAT_TIMEOUT),
             (unsigned long long)pgw_stat_get(PGW_STAT_BYTES_READ),
             (unsigned long long)pgw_stat_get(PGW_STAT_BYTES_WRITTEN));
}
