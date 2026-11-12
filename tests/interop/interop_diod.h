/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/interop/interop_diod.h - diod, a third-party 9P2000.L server.
 *
 * diod (https://github.com/davidedelvento/diod, packaged as "diod" by
 * Debian and Ubuntu) is a 9P2000.L file server we did not write. Driving
 * our client against it is the interoperability proof gap 59 asks for: the
 * two sides share no code, so a common misreading of the protocol cannot
 * pass on both.
 *
 * The server is started on a loopback port with the export it serves, and
 * stopped when the bench is done. When diod is not installed the bench
 * skips with the reason, it never pretends to have passed.
 *
 * Covers P9-01, P9-02, P9-03, P9-07, P9-08 and P9-10.
 */
#ifndef INTEROP_DIOD_H
#define INTEROP_DIOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define INTEROP_DIOD_PATH_MAX 512

struct interop_diod;

/* Look for the diod binary: PGW_DIOD first, then the usual installation
 * directories, then PATH. Returns false and fills `why` with the reason
 * when the server is not available. */
bool interop_diod_find(char *path, size_t cap, char *why, size_t why_cap);

/* Start diod on `root` - which is also the aname of the export, diod names
 * an export after its path - and connect to it. Returns NULL and fills
 * `why` with the reason (and the tail of the server log) on failure. */
struct interop_diod *interop_diod_start(const char *root, char *why, size_t why_cap);

/* Stop the server and free the handle. */
void interop_diod_stop(struct interop_diod *d);

/* The connected socket, and the user pointer of the rpc callback. */
int interop_diod_fd(const struct interop_diod *d);
void *interop_diod_rpc_user(struct interop_diod *d);

/* The last lines of the server log, for a failure message. */
void interop_diod_log_tail(const struct interop_diod *d, char *out, size_t cap);

#endif /* INTEROP_DIOD_H */
