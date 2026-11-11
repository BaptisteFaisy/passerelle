/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/syslog_qnx.c - the 9P driver's system log identity.
 *
 * drivers/common/src/syslog_qnx.c owns the slog2 backend and the counter
 * export; this file owns what is specific to this driver: the buffer set name
 * (`slog2info -b fs9p` finds it), the start-up banner with the negotiated
 * parameters, and the shutdown line that closes the accounting.
 *
 * Covered requirements: QNX-08 (writes to the system log and exposes per
 * operation counters), QNX-02 (nothing but libc, the kernel and the PCI server
 * is needed: slog2 is part of libc).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/log.h"
#include "pgw/p9.h"

#include <stdint.h>

pgw_err fs9p_log_open(void)
{
    return pgw_slog2_open("fs9p");
}

void fs9p_log_close(void)
{
    pgw_slog2_close();
}

/* One line per mount: what an operator needs to know from `slog2info`, without
 * having to ask the driver anything (QNX-08). */
void fs9p_log_mounted(const pgw_mount_opts *opts, uint32_t msize)
{
    if (opts == NULL)
        return;
    PGW_INFO("fs9p: tag=%s mount=%s msize=%u timeout=%ums requests=%u "
             "%s%s%s",
             opts->tag, opts->mount_point != NULL ? opts->mount_point : "?",
             msize, opts->timeout_ms, opts->fs.max_inflight,
             opts->fs.readonly ? "ro " : "rw ",
             opts->fs.noexec ? "noexec " : "",
             opts->fs.nosuid ? "nosuid" : "suid");
}

/* Called on umount: the counters that matter for the report, so a run leaves
 * its numbers in the log even when nobody reads the stats file (PERF-01). */
void fs9p_log_unmounted(void)
{
    PGW_INFO("fs9p: unmounted: req=%llu resp=%llu err=%llu timeout=%llu "
             "bytes_read=%llu bytes_written=%llu",
             (unsigned long long)pgw_stat_get(PGW_STAT_REQ),
             (unsigned long long)pgw_stat_get(PGW_STAT_RESP),
             (unsigned long long)pgw_stat_get(PGW_STAT_ERR),
             (unsigned long long)pgw_stat_get(PGW_STAT_TIMEOUT),
             (unsigned long long)pgw_stat_get(PGW_STAT_BYTES_READ),
             (unsigned long long)pgw_stat_get(PGW_STAT_BYTES_WRITTEN));
}
