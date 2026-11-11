/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_dispatch.c - the message type -> handler table.
 *
 * One table line per T-message of the contract: adding a message costs one
 * handler and one line here, and an unknown type answers Rlerror(ENOSYS) the
 * way a real server does.
 */
#include "fake_serve.h"

static const fake_entry g_table[] = {
    { PGW_P9_TVERSION, fake_h_version },  { PGW_P9_TATTACH, fake_h_attach },
    { PGW_P9_TWALK, fake_h_walk },        { PGW_P9_TCLUNK, fake_h_clunk },
    { PGW_P9_TFLUSH, fake_h_flush },      { PGW_P9_TLOPEN, fake_h_lopen },
    { PGW_P9_TLCREATE, fake_h_lcreate },  { PGW_P9_TREAD, fake_h_read },
    { PGW_P9_TWRITE, fake_h_write },      { PGW_P9_TREADDIR, fake_h_readdir },
    { PGW_P9_TFSYNC, fake_h_fsync },      { PGW_P9_TGETATTR, fake_h_getattr },
    { PGW_P9_TSETATTR, fake_h_setattr },  { PGW_P9_TMKDIR, fake_h_mkdir },
    { PGW_P9_TUNLINKAT, fake_h_unlinkat },{ PGW_P9_TRENAMEAT, fake_h_renameat },
    { PGW_P9_TSYMLINK, fake_h_symlink },  { PGW_P9_TREADLINK, fake_h_readlink },
    { PGW_P9_TLINK, fake_h_link },        { PGW_P9_TSTATFS, fake_h_statfs },
};

pgw_err fake_dispatch(fake_srv *fs, const pgw_p9_hdr *h, pgw_buf *req,
                      void *rx, size_t cap, size_t *rx_len)
{
    size_t n = sizeof g_table / sizeof g_table[0];
    for (size_t i = 0; i < n; i++) {
        if (g_table[i].type == h->type)
            return g_table[i].fn(fs, h->tag, req, rx, cap, rx_len);
    }
    return fake_reply_error(rx, cap, rx_len, h->tag, L_ENOSYS);
}
