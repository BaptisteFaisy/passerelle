/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/op.c - the 9P2000.L message table.
 *
 * Portable on purpose: the trace label of a request is decided here, so the PC
 * bench checks every row (drivers/tests/test_traceop.c). The first version of
 * this table lived in the QNX half of the driver, where nothing could run it,
 * and it answered PGW_OP__MAX for Twalk - the message the session sends most
 * (path resolution) - so the transport trace labelled every request as an OPEN.
 *
 * Covered requirements: QNX-09.
 */
#include "pgw/fs9p_op.h"

#include "pgw/p9.h"

enum pgw_op fs9p_op_of(uint8_t p9_type)
{
    switch (p9_type) {
    case PGW_P9_TLOPEN:
    case PGW_P9_TLCREATE:
        return PGW_OP_OPEN;
    case PGW_P9_TREAD:
        return PGW_OP_READ;
    case PGW_P9_TWRITE:
        return PGW_OP_WRITE;
    case PGW_P9_TGETATTR:
        return PGW_OP_STAT;
    case PGW_P9_TREADDIR:
        return PGW_OP_READDIR;
    case PGW_P9_TSETATTR:
        return PGW_OP_CHMOD;
    case PGW_P9_TFSYNC:
        return PGW_OP_SYNC;
    case PGW_P9_TSTATFS:
        return PGW_OP_DEVCTL;
    case PGW_P9_TUNLINKAT:
    case PGW_P9_TREMOVE:
        return PGW_OP_UNLINK;
    case PGW_P9_TRENAMEAT:
    case PGW_P9_TRENAME:
        return PGW_OP_RENAME;
    case PGW_P9_TMKDIR:
        return PGW_OP_MKDIR;
    case PGW_P9_TSYMLINK:
        return PGW_OP_SYMLINK;
    case PGW_P9_TLINK:
        return PGW_OP_LINK;
    case PGW_P9_TREADLINK:
        return PGW_OP_READLINK;
    case PGW_P9_TCLUNK:
        return PGW_OP_CLOSE;
    /* Path resolution is not an operation a client asked for: it is how the
     * kernel reaches the one it did. Labelling it OPEN would hide it. */
    case PGW_P9_TWALK:
        return PGW_OP_LOOKUP;
    /* The session's own messages: no file operation behind them. */
    case PGW_P9_TVERSION:
    case PGW_P9_TAUTH:
    case PGW_P9_TATTACH:
    case PGW_P9_TFLUSH:
        return PGW_OP_SESSION;
    default:
        /* TMKNOD, TLOCK, TGETLOCK and anything the protocol adds later: not
         * implemented here, but still traced as what they are. */
        return PGW_OP_OTHER;
    }
}
