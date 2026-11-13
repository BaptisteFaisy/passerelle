/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/slog2.h> - building on the PC only.
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * QNX 8's slog2 API differs from QNX 6/7: slog2_buffer_t is an opaque pointer,
 * slog2f() is (buffer, code, severity, format, ...), and a buffer set is
 * configured through buffer_config[] entries of {name, num_pages}. The shapes
 * below mirror sys/slog2.h of the SDP.
 *
 * Covered requirement: QNX-08 (write to the system log; the driver's counters
 * are published through the same backend).
 */
#ifndef PGW_STUB_SYS_SLOG2_H
#define PGW_STUB_SYS_SLOG2_H

#include <stdarg.h>
#include <stdint.h>

/* A buffer handle, opaque like the real one (struct _slog2_buffer_meta *). */
typedef struct _slog2_buffer_meta *slog2_buffer_t;

#define SLOG2_MAX_BUFFERS 4

/* Severity levels (sys/slog2.h). */
#define SLOG2_SHUTDOWN 0
#define SLOG2_CRITICAL 1
#define SLOG2_ERROR    2
#define SLOG2_WARNING  3
#define SLOG2_NOTICE   4
#define SLOG2_INFO     5
#define SLOG2_DEBUG1   6
#define SLOG2_DEBUG2   7

/* One buffer of a set: a name and a size in 4 KiB pages. */
typedef struct {
    const char *buffer_name;
    int num_pages;
} slog2_buffer_config_t;

/* A buffer set, field for field from slog2_register(). */
typedef struct {
    int num_buffers;
    const char *buffer_set_name;
    uint8_t verbosity_level;
    slog2_buffer_config_t buffer_config[SLOG2_MAX_BUFFERS];
    uint32_t max_retries;
} slog2_buffer_set_config_t;

int slog2_register(const slog2_buffer_set_config_t *config,
                   slog2_buffer_t *handles, uint32_t flags);
slog2_buffer_t slog2_set_default_buffer(slog2_buffer_t buffer);
int slog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity,
           const char *format, ...);

#endif /* PGW_STUB_SYS_SLOG2_H */
