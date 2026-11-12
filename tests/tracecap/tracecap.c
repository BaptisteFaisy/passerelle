/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/tracecap/tracecap.c - capture a real 9P exchange (gap 55).
 *
 * The trace fixture of host/tests/fixtures/ was written by hand once and was
 * never regenerated, so a change of the protocol library could keep passing
 * on data that no longer describes any real run. Here the production client
 * of proto9p/ talks to the independent reference server of tests/ref9p/ over
 * a socket, every frame is captured, and the exchange is rendered as QEMU
 * trace lines - the format the parser of host/bench/trace.py reads.
 *
 * The capture is deterministic by construction:
 *   - the tree is created here, with fixed names and fixed bytes;
 *   - the clock is the injectable monotonic clock of common/ (pgw/time.h),
 *     never the wall clock;
 *   - only the protocol facts that do not depend on the machine are rendered
 *     (no inode, no uid/gid, no wall-clock time).
 *
 * Covers P9-09 (every response is validated) and P9-10 (the client runs
 * unchanged against a second implementation).
 */
#define _POSIX_C_SOURCE 200809L
#include "tracecap.h"

#include "ref9p_harness.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The tree the capture runs against: the same names and the same bytes every
 * time, so the rendered trace never depends on the machine. */
#define TC_FILE_NAME "hello.txt"
#define TC_FILE_BODY "hello 9p\n"
#define TC_DIR_NAME "sub"
#define TC_NESTED_NAME "nested.txt"
#define TC_MISSING_NAME "missing.txt"
#define TC_PATH_MAX 512
/* A path is a root plus a name: the buffer is bigger so that the compiler can
 * prove that the concatenation never truncates. */
#define TC_FULL_MAX (TC_PATH_MAX + 64)

static uint64_t g_fake_ns;

/* The injectable monotonic clock of common/: the capture never reads the wall
 * clock, so the session behaves the same on every machine. */
static uint64_t fake_clock(void *user)
{
    uint64_t *now = (uint64_t *)user;
    *now += 1000000ULL; /* one millisecond per reading */
    return *now;
}

/* --- the captured log ------------------------------------------------------ */

void tracecap_log_reset(tracecap_log *log)
{
    if (log == NULL)
        return;
    for (size_t i = 0; i < log->count; i++)
        free(log->frames[i].data);
    log->count = 0;
}

pgw_err tracecap_log_add(tracecap_log *log, const void *data, size_t len, bool is_reply)
{
    if (log == NULL || (data == NULL && len > 0))
        return PGW_EINVAL;
    if (log->count >= TRACECAP_MAX_FRAMES)
        return PGW_ENOBUFS;
    uint8_t *copy = NULL;
    if (len > 0) {
        copy = malloc(len);
        if (copy == NULL)
            return PGW_ENOMEM;
        memcpy(copy, data, len);
    }
    log->frames[log->count].data = copy;
    log->frames[log->count].len = len;
    log->frames[log->count].is_reply = is_reply;
    log->count++;
    return PGW_OK;
}

/* --- the tree -------------------------------------------------------------- */

static int make_tree(char *root, size_t cap)
{
    const char *tmp = getenv("TMPDIR");
    const char *base = (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp";
    char tmpl[TC_FULL_MAX];
    int n = snprintf(tmpl, sizeof tmpl, "%s/pgw-tracecap-XXXXXX", base);
    if (n < 0 || (size_t)n >= sizeof tmpl)
        return -1;
    char *dir = mkdtemp(tmpl);
    if (dir == NULL)
        return -1;
    snprintf(root, cap, "%s", dir);

    char path[TC_FULL_MAX];
    snprintf(path, sizeof path, "%s/%s", root, TC_FILE_NAME);
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    size_t written = fwrite(TC_FILE_BODY, 1u, sizeof TC_FILE_BODY - 1u, f);
    fclose(f);
    if (written != sizeof TC_FILE_BODY - 1u)
        return -1;

    snprintf(path, sizeof path, "%s/%s", root, TC_DIR_NAME);
    if (mkdir(path, 0755) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/%s/%s", root, TC_DIR_NAME, TC_NESTED_NAME);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    written = fwrite(TC_FILE_BODY, 1u, sizeof TC_FILE_BODY - 1u, f);
    fclose(f);
    if (written != sizeof TC_FILE_BODY - 1u)
        return -1;
    return 0;
}

static void remove_tree(const char *root)
{
    char path[TC_FULL_MAX];
    snprintf(path, sizeof path, "%s/%s", root, TC_FILE_NAME);
    (void)unlink(path);
    snprintf(path, sizeof path, "%s/%s/%s", root, TC_DIR_NAME, TC_NESTED_NAME);
    (void)unlink(path);
    snprintf(path, sizeof path, "%s/%s", root, TC_DIR_NAME);
    (void)rmdir(path);
    (void)rmdir(root);
}

/* --- the session ----------------------------------------------------------- */

struct capture {
    struct ref9p_server *srv;
    tracecap_log *log;
};

/* The RPC callback of the session: it forwards to the reference server and
 * keeps a copy of both sides of every exchange. */
static pgw_err capture_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                           size_t *rx_len, uint32_t prio, uint32_t timeout_ms, void *user)
{
    struct capture *cap = (struct capture *)user;
    pgw_err e = ref9p_rpc(tx, tx_len, rx, rx_cap, rx_len, prio, timeout_ms,
                          ref9p_rpc_user(cap->srv));
    if (e != PGW_OK)
        return e;
    if (tracecap_log_add(cap->log, tx, tx_len, false) != PGW_OK)
        return PGW_ENOMEM;
    if (tracecap_log_add(cap->log, rx, *rx_len, true) != PGW_OK)
        return PGW_ENOMEM;
    return PGW_OK;
}

/* The script of the capture: the operations a guest performs, in a fixed
 * order, so that two runs exchange exactly the same frames. */
static void run_script(pgw_p9_session *s, uint32_t root_fid)
{
    uint32_t fid = PGW_FID_NONE;
    pgw_p9_qid qid;
    pgw_p9_attr attr;
    uint8_t buf[256];
    size_t got = 0;

    (void)pgw_p9_walk_path(s, root_fid, TC_FILE_NAME, &fid, &qid, 0);
    (void)pgw_p9_getattr(s, fid, &attr, 0);
    (void)pgw_p9_lopen(s, fid, PGW_O_RDWR, 0);
    (void)pgw_p9_read(s, fid, 0, buf, sizeof buf, &got, 0);
    (void)pgw_p9_read(s, fid, 4096, buf, sizeof buf, &got, 0);
    (void)pgw_p9_write(s, fid, 0, TC_FILE_BODY, sizeof TC_FILE_BODY - 1u, &got, 0);
    (void)pgw_p9_fsync(s, fid, 0);
    (void)pgw_p9_clunk(s, fid, 0);

    fid = PGW_FID_NONE;
    (void)pgw_p9_walk_path(s, root_fid, TC_DIR_NAME, &fid, &qid, 0);
    (void)pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0);
    pgw_p9_dirent ents[8];
    size_t nents = 0;
    uint64_t next = 0;
    (void)pgw_p9_readdir(s, fid, 0, ents, 8, &nents, &next, 0);
    /* The error path: a directory cannot be opened for writing. The server
     * answers Rlerror, which the trace shows as v9fs_rerror. */
    (void)pgw_p9_lopen(s, fid, PGW_O_WRONLY, 0);
    (void)pgw_p9_clunk(s, fid, 0);

    /* The path that is not there: the server answers a walk that reached no
     * name, which the client reports as ENOENT. */
    fid = PGW_FID_NONE;
    (void)pgw_p9_walk_path(s, root_fid, TC_MISSING_NAME, &fid, &qid, 0);
}

pgw_err tracecap_capture(tracecap_log *log)
{
    char root[TC_PATH_MAX];
    if (log == NULL)
        return PGW_EINVAL;
    tracecap_log_reset(log);
    if (make_tree(root, sizeof root) != 0) {
        remove_tree(root);
        return PGW_EIO;
    }
    struct ref9p_server *srv = ref9p_start(root, "none");
    if (srv == NULL) {
        remove_tree(root);
        return PGW_EIO;
    }
    struct capture cap = {srv, log};
    g_fake_ns = 0;
    pgw_time_set_clock(fake_clock, &g_fake_ns);
    pgw_p9_session *s = pgw_p9_session_open(capture_rpc, &cap, PGW_MSIZE_DEFAULT,
                                            "passerelle", "", 0);
    pgw_err e = PGW_OK;
    if (s == NULL) {
        e = PGW_EIO;
    } else {
        run_script(s, pgw_p9_session_root_fid(s));
        pgw_p9_session_close(s); /* the closing clunk of the root is captured */
    }
    pgw_time_set_clock(NULL, NULL);
    ref9p_stop(srv);
    remove_tree(root);
    return e;
}
