/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/tracecap/trace_render.h - render a captured exchange as a QEMU trace.
 *
 * The parser of host/bench/trace.py reads the format QEMU writes when it is
 * started with -trace 'v9fs_*':
 *
 *     <pid>@<seconds>.<micros>:v9fs_read tag 5 id 116 fid 2 off 0 max_count 4096
 *
 * The renderer writes that format from the captured frames, so the fixture
 * stays readable by the parser that consumes it. Only the facts that do not
 * depend on the machine are rendered: an inode, a uid or a wall-clock time
 * would make the fixture differ from one machine to another.
 */
#ifndef TRACE_RENDER_H
#define TRACE_RENDER_H

#include "tracecap.h"

#include <stdio.h>

/* Write the captured exchange as QEMU trace lines. */
pgw_err tracecap_render(const tracecap_log *log, FILE *out);

#endif /* TRACE_RENDER_H */
