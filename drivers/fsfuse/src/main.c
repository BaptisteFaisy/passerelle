/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/main.c - entry point of the virtiofs driver.
 *
 * `fsfuse [-o opt,opt...] mount_tag mount_point` is what the image's start-up
 * script and `mount -t virtiofs` both run (QNX-07). The process:
 *
 *   1. takes the termination signals before any thread exists (ROB-03);
 *   2. parses the options (portable, tested on the PC: args.c);
 *   3. opens the system log (QNX-08) and the kernel trace (QNX-09);
 *   4. drops root and keeps only four abilities (QNX-06, SEC-01);
 *   5. mounts and serves (mount_qnx.c); on the way out it unmounts in reverse.
 *
 * Covered requirements: QNX-01, QNX-02, QNX-06, QNX-07, QNX-08, QNX-09, ROB-03,
 * SEC-01.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"
#include "pgw/err.h"
#include "pgw/fsfuse.h"
#include "pgw/fsfuse_qnx.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>

#include <sys/neutrino.h>
#include <sys/procmgr.h>

/* Signals that end a mount: SIGTERM always; SIGINT and SIGHUP only when they
 * were not inherited as ignored. */
static void block_term_signals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    static const int maybe[] = { SIGINT, SIGHUP };
    for (size_t i = 0; i < sizeof maybe / sizeof maybe[0]; i++) {
        struct sigaction sa;
        if (sigaction(maybe[i], NULL, &sa) == 0 && sa.sa_handler != SIG_IGN)
            sigaddset(set, maybe[i]);
    }
}

/* Wait for a termination signal, then unmount in the reverse order of the mount
 * so no request is in flight when the device is reset (VIO-09, ROB-03). */
static void *shutdown_main(void *arg)
{
    sigset_t *sigs = arg;
    int sig = 0;
    while (sigwait(sigs, &sig) != 0)
        ;
    PGW_INFO("fsfuse: signal %d, unmounting", sig);
    fsfuse_unmount();
    fsfuse_log_unmounted();
    fsfuse_log_close();
    exit(EXIT_SUCCESS);
    return NULL; /* not reached */
}

int main(int argc, char *argv[])
{
    pgw_mount_opts opts;
    char mount_point[PGW_PATH_MAX];

    pgw_err e = fsfuse_parse_args(argc, argv, &opts, mount_point,
                                  sizeof mount_point);
    if (e != PGW_OK) {
        PGW_ERR("fsfuse: bad arguments (expected: fsfuse [-o opts] tag mount_point)");
        return EXIT_FAILURE;
    }
    pgw_level lvl;
    const char *env = getenv("PGW_LOG_LEVEL");
    if (env != NULL && pgw_log_parse_level(env, &lvl))
        pgw_log_set_level(lvl);

    sigset_t sigs;
    block_term_signals(&sigs);
    (void)pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    e = fsfuse_log_open(); /* QNX-08 */
    if (e != PGW_OK)
        PGW_WARN("fsfuse: no system log, falling back to stderr");
    fsfuse_trace_init(&opts); /* QNX-09 */
    fsfuse_log_mounted(&opts, opts.fs.max_inflight);

    if (!opts.debug && procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCHDIR) == -1) {
        PGW_ERR("fsfuse: cannot become a daemon");
        return EXIT_FAILURE;
    }

    int rc = ThreadCreate(shutdown_main, &sigs, 8192u);
    if (rc < 0) {
        PGW_ERR("fsfuse: cannot create the shutdown thread");
        return EXIT_FAILURE;
    }

    e = fsfuse_mount_and_serve(&opts, mount_point); /* QNX-06 happens inside */
    PGW_ERR("fsfuse: %s: %s", mount_point, pgw_err_name(e));
    fsfuse_log_close();
    return EXIT_FAILURE;
}
