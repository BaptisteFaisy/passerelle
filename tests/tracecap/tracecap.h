/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/tracecap/tracecap.h - the capture of a real 9P exchange.
 *
 * The trace fixture of host/tests/fixtures/ used to be hand-written once and
 * never regenerated, so a change of the protocol library could keep passing
 * on data that no longer describes a real run (gap 55). This module captures
 * what the production client of proto9p/ really exchanges with the
 * independent reference server of tests/ref9p/, so the fixture can be
 * regenerated from the code itself.
 *
 * The capture is deterministic: the tree is created here with fixed names and
 * fixed bytes, the clock is the injectable monotonic clock of common/
 * (pgw/time.h) and only the protocol facts that do not depend on the machine
 * are rendered (no inode, no uid/gid, no wall-clock time).
 *
 * Covers P9-09 (every response is validated) and P9-10 (the client runs
 * unchanged against a second implementation).
 */
#ifndef TRACECAP_H
#define TRACECAP_H

#include "pgw/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A captured session never has more frames than this. */
#define TRACECAP_MAX_FRAMES 256u

/* One 9P message, without the transport framing of the reference server. */
typedef struct {
    uint8_t *data;
    size_t len;
    bool is_reply;
} tracecap_frame;

typedef struct {
    tracecap_frame frames[TRACECAP_MAX_FRAMES];
    size_t count;
} tracecap_log;

/* Forget every captured frame (the buffers themselves are freed). */
void tracecap_log_reset(tracecap_log *log);

/* Copy one message into the log. */
pgw_err tracecap_log_add(tracecap_log *log, const void *data, size_t len, bool is_reply);

/* Run the whole session against the reference server and capture it. */
pgw_err tracecap_capture(tracecap_log *log);

#endif /* TRACECAP_H */
