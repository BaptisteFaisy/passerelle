/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/main.c - entry point of the 9P driver.
 *
 * `fs9p [-o opt,opt...] mount_tag mount_point` is what the image's start-up
 * script and `mount -t 9p` both run (QNX-07). The process:
 *
 *   1. takes the termination signals before any thread exists, so every thread
 *      inherits the mask and a single thread can unmount cleanly (ROB-03);
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
#include "pgw/fs9p.h"
#include "pgw/fs9p_qnx.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>

#include <sys/neutrino.h>
#include <sys/procmgr.h>

void fs9p_trace_init(const pgw_mount_opts *opts);
pgw_err fs9p_log_open(void);
void fs9p_log_close(void);
void fs9p_log_mounted(const pgw_mount_opts *opts, uint32_t msize);
void fs9p_log_unmounted(void);

/* Signals that end a mount: SIGTERM (what slay sends) always; SIGINT and SIGHUP
 * only when they were not inherited as ignored, so a background job of a shell
 * without job control is not killed by a terminal signal it never wanted. */
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
    PGW_INFO("fs9p: signal %d, unmounting", sig);
    fs9p_unmount();
    fs9p_log_unmounted();
    fs9p_log_close();
    exit(EXIT_SUCCESS);
    return NULL; /* not reached */
}

/* Start the detached thread that waits for a signal and unmounts. Returns 0
 * on success, the pthread error code otherwise. */
static int start_shutdown_thread(sigset_t *sigs)
{
    pthread_attr_t tattr;
    pthread_t tid;
    int rc = pthread_attr_init(&tattr);
    if (rc == 0)
        rc = pthread_attr_setstacksize(&tattr, 8192u);
    if (rc == 0)
        rc = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (rc == 0)
        rc = pthread_create(&tid, &tattr, shutdown_main, sigs);
    (void)pthread_attr_destroy(&tattr);
    if (rc != 0)
        PGW_ERR("fs9p: cannot create the shutdown thread");
    return rc;
}

int main(int argc, char *argv[])
{
    pgw_mount_opts opts;
    char mount_point[PGW_PATH_MAX];

    pgw_err e = fs9p_parse_args(argc, argv, &opts, mount_point, sizeof mount_point);
    if (e != PGW_OK) {
        PGW_ERR("fs9p: bad arguments (expected: fs9p [-o opts] tag mount_point)");
        return EXIT_FAILURE;
    }
    /* PGW_LOG_LEVEL=debug|info|... lets a run raise the level without a
     * rebuild; an unset or unknown value keeps the default. */
    pgw_level lvl;
    const char *env = getenv("PGW_LOG_LEVEL");
    if (env != NULL && pgw_log_parse_level(env, &lvl))
        pgw_log_set_level(lvl);

    sigset_t sigs;
    block_term_signals(&sigs);
    (void)pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    e = fs9p_log_open(); /* QNX-08 */
    if (e != PGW_OK)
        PGW_WARN("fs9p: no system log, falling back to stderr");
    fs9p_trace_init(&opts); /* QNX-09 */
    fs9p_log_mounted(&opts, opts.msize);

    /* Detach from the terminal only once the mount is about to succeed: a setup
     * error must still reach whoever ran mount (QNX-07). */
    if (!opts.debug && procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCHDIR) == -1) {
        PGW_ERR("fs9p: cannot become a daemon");
        return EXIT_FAILURE;
    }

    /* The shutdown thread waits for a signal, then unmounts.
     *
     * QNX 8 has no ThreadCreate() with that signature: the one the PC stub
     * declared (entry, arg, stack size) exists in no QNX version, so the call
     * survived every compile until the first real one. A detached pthread with
     * an explicit stack size says the same thing. */
    if (start_shutdown_thread(&sigs) != 0)
        return EXIT_FAILURE;

    e = fs9p_mount_and_serve(&opts, mount_point); /* QNX-06 happens inside */
    PGW_ERR("fs9p: %s: %s", mount_point, pgw_err_name(e));
    fs9p_log_close();
    return EXIT_FAILURE;
}
