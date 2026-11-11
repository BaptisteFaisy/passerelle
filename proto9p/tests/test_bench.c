/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_bench.c - state of the session bench.
 *
 * One fake server and one session at a time: the tests are sequential (the
 * harness runs one case at a time), so a file-scope pair is enough and keeps
 * every test free of boilerplate.
 */
#include "test_bench.h"

#include "pgw/log.h"
#include "pgw/test.h"

static fake_srv *g_fs;
static pgw_p9_session *g_s;

fake_srv *bench_up(uint32_t msize_req, uint32_t server_cap)
{
    PGW_ASSERT(g_fs == NULL && g_s == NULL);
    g_fs = fake_new(server_cap);
    PGW_ASSERT(g_fs != NULL);
    pgw_stat_reset();
    g_s = pgw_p9_session_open(fake_rpc, g_fs, msize_req, "", "/share", 0);
    PGW_ASSERT(g_s != NULL);
    return g_fs;
}

pgw_p9_session *bench_session(void)
{
    return g_s;
}

fake_srv *bench_server(void)
{
    return g_fs;
}

void bench_down(void)
{
    pgw_p9_session_close(g_s);
    g_s = NULL;
    fake_free(g_fs);
    g_fs = NULL;
}

uint32_t bench_walk(const char *path)
{
    uint32_t fid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(g_s, pgw_p9_session_root_fid(g_s), path, &fid,
                                     NULL, 0),
                    PGW_OK);
    return fid;
}
