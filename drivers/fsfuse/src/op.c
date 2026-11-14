/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/op.c - the FUSE opcode table.
 *
 * Portable on purpose: the trace label of a request is decided here, so the PC
 * bench checks every row (drivers/tests/test_traceop.c). LOOKUP used to map to
 * PGW_OP_OPEN, which hid path resolution behind the opens it serves; the 9P
 * table answers PGW_OP_LOOKUP for Twalk, and this one answers the same for the
 * same thing.
 *
 * Covered requirements: QNX-09.
 */
#include "pgw/fsfuse_op.h"

#include "pgw/fusep.h"

enum pgw_op fsfuse_op_of(uint32_t opcode)
{
    switch (opcode) {
    case PGW_FUSE_OPEN:
    case PGW_FUSE_CREATE:
        return PGW_OP_OPEN;
    case PGW_FUSE_READ:
        return PGW_OP_READ;
    case PGW_FUSE_WRITE:
        return PGW_OP_WRITE;
    case PGW_FUSE_GETATTR:
        return PGW_OP_STAT;
    case PGW_FUSE_READDIR:
        return PGW_OP_READDIR;
    case PGW_FUSE_SETATTR:
        return PGW_OP_CHMOD;
    case PGW_FUSE_FSYNC:
    case PGW_FUSE_FSYNCDIR:
        return PGW_OP_SYNC;
    case PGW_FUSE_STATFS:
        return PGW_OP_DEVCTL;
    case PGW_FUSE_UNLINK:
    case PGW_FUSE_RMDIR:
        return PGW_OP_UNLINK;
    case PGW_FUSE_RENAME:
    case PGW_FUSE_RENAME2:
        return PGW_OP_RENAME;
    case PGW_FUSE_MKDIR:
        return PGW_OP_MKDIR;
    case PGW_FUSE_SYMLINK:
        return PGW_OP_SYMLINK;
    case PGW_FUSE_LINK:
        return PGW_OP_LINK;
    case PGW_FUSE_READLINK:
        return PGW_OP_READLINK;
    case PGW_FUSE_FORGET:
    case PGW_FUSE_BATCH_FORGET:
    case PGW_FUSE_RELEASE:
    case PGW_FUSE_RELEASEDIR:
        return PGW_OP_CLOSE;
    /* Resolving a name is not opening a file: it is how the kernel reaches the
     * open it will ask for next. */
    case PGW_FUSE_LOOKUP:
        return PGW_OP_LOOKUP;
    /* INIT, DESTROY, GETXATTR, SETXATTR, FLUSH, OPENDIR, ...: no resource-manager
     * operation behind them, but still traced as what they are. */
    default:
        return PGW_OP_OTHER;
    }
}
