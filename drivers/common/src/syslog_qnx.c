/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/syslog_qnx.c - system log backend and counter export.
 *
 * Two jobs, both QNX-08:
 *   - install a slog2 backend into the project logger (pgw/log.h), so every
 *     PGW_ERR/PGW_WARN/PGW_INFO line reaches slogger2 and `slog2info`;
 *   - answer devctl(DCMD_PGW_STATS) with the per-operation counter table of
 *     pgw/log.h, so `cat`/`devctl` on the mount point shows where the round
 *     trips go without stopping the driver.
 *
 * Logging never allocates: slog2f() formats into its own buffer and the counter
 * table is formatted into a thread-local buffer.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <sys/slog2.h>

static slog2_buffer_t g_buffers[1];
static bool g_open;

static uint16_t severity_of(pgw_level lvl)
{
    switch (lvl) {
    case PGW_LOG_ERROR:
        return SLOG2_ERROR;
    case PGW_LOG_WARN:
        return SLOG2_WARNING;
    case PGW_LOG_INFO:
        return SLOG2_INFO;
    case PGW_LOG_DEBUG:
        return SLOG2_DEBUG1;
    case PGW_LOG_TRACE:
        return SLOG2_DEBUG2;
    default:
        return SLOG2_NOTICE;
    }
}

static void slog_backend(pgw_level lvl, const char *msg, void *user)
{
    (void)user;
    if (!g_open || msg == NULL)
        return;
    (void)slog2f(&g_buffers[0], severity_of(lvl), "passerelle", "driver", 0u,
                 "%s", msg);
}

pgw_err pgw_slog2_open(const char *set_name)
{
    if (set_name == NULL)
        return PGW_EINVAL;
    if (g_open)
        return PGW_OK;
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.verbosity_level = (uint8_t)SLOG2_DEBUG2;
    cfg.buffer_size = 8; /* pages: 32 KiB of history per driver */
    cfg.buffer_set_name = set_name;
    cfg.buffer[0].buffer_name = "passerelle";
    cfg.buffer[0].num_lines = 1000;
    if (slog2_register(&cfg, g_buffers, 0) != 0)
        return pgw_code_from_host(errno);
    (void)slog2_set_default_buffer(&g_buffers[0]);
    g_open = true;
    pgw_log_set_backend(slog_backend, NULL);
    return PGW_OK;
}

void pgw_slog2_close(void)
{
    if (!g_open)
        return;
    pgw_log_set_backend(NULL, NULL); /* back to stderr before the buffer goes */
    g_open = false;
}

int pgw_stats_devctl(resmgr_context_t *ctp, io_devctl_t *msg)
{
    /* Built in a thread-local buffer: in a combine message the room left after
     * the devctl header is not fixed, so the reply must not be assembled in
     * place. */
    static _Thread_local struct {
        struct _io_devctl_reply hdr;
        char text[1024];
    } rep;
    size_t n = pgw_stat_format(rep.text, sizeof rep.text - 1u);
    if (n >= sizeof rep.text)
        return EOVERFLOW;
    if (msg->i.nbytes < (uint32_t)n + 1u)
        return EOVERFLOW; /* the client's buffer is too small: never truncate */
    memset(&rep.hdr, 0, sizeof rep.hdr);
    rep.hdr.nbytes = (int32_t)n + 1;
    return _RESMGR_PTR(ctp, &rep, sizeof rep.hdr + n + 1u);
}
