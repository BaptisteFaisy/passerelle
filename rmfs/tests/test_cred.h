/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_cred.h - caller identities for the rmfs tests.
 *
 * The fake backend creates its files with the identity of the *test process*,
 * so a caller hardcoded to uid 1000 only matches on a machine whose user
 * happens to be 1000. It passed locally and failed on the GitHub-hosted runner
 * (uid 1001) with EACCES on every mkdir, symlink and link: the caller fell into
 * the "other" class of a 0755 directory. Deriving the identity from the process
 * makes the suite portable, and tc_other() keeps a way to be somebody else.
 */
#ifndef PGW_RMFS_TEST_CRED_H
#define PGW_RMFS_TEST_CRED_H

#include "pgw/rmfs.h"

#include <unistd.h>

/* The owner of the files this test process creates. */
static inline pgw_fs_cred tc_self(uint32_t prio)
{
    pgw_fs_cred c;
    c.uid = (uint32_t)getuid();
    c.gid = (uint32_t)getgid();
    c.prio = prio;
    return c;
}

/* Somebody who is neither the owner nor root: the "other" class of the mode
 * bits. Avoids 0 (root bypasses every check) and the owner's own ids. */
static inline pgw_fs_cred tc_other(uint32_t prio)
{
    pgw_fs_cred c = tc_self(prio);
    c.uid = (c.uid == 0u) ? 65534u : c.uid + 1u;
    c.gid = (c.gid == 0u) ? 65534u : c.gid + 1u;
    return c;
}

#endif /* PGW_RMFS_TEST_CRED_H */
