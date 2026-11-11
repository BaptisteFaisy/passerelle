/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/privdrop_qnx.c - drop root, keep only what is needed.
 *
 * A QNX driver that runs as root can do anything; one that holds three locked
 * abilities can only map its own device, take its own interrupt and register
 * its own path. This file applies exactly that: allow each requested ability,
 * lock the set so no later call can widen it, then leave root. The list of
 * abilities is supplied by the driver (it differs between 9P and virtiofs),
 * and the same set is what image/passerelle.sec grants through the security
 * policy.
 *
 * Covered requirements: QNX-06 (works without root, minimum capabilities, and a
 * policy is provided), SEC-01, SEC-02 (no access to any other device: mem_phys
 * is granted for the device's own BAR range only, by the security policy).
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <errno.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <sys/procmgr.h>

static pgw_err allow_ability(unsigned aid, const char *why)
{
    /* nonroot: the abilities survive the setuid() below. inherit: the threads
     * created afterwards have them too. lock: nothing can add an ability
     * later, so the drop cannot be undone. */
    if (procmgr_ability(0,
                        PROCMGR_ADN_NONROOT | PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW |
                            PROCMGR_AID_INHERIT | PROCMGR_AID_LOCK,
                        aid, NULL) != 0) {
        PGW_ERR("privdrop: ability %u (%s) refused: %s", aid, why,
                strerror(errno));
        return pgw_code_from_host(errno);
    }
    PGW_INFO("privdrop: granted ability %u (%s)", aid, why);
    return PGW_OK;
}

static pgw_err drop_to_user(const char *user)
{
    struct passwd *pw = getpwnam(user);
    if (pw == NULL) {
        PGW_ERR("privdrop: unknown user '%s'", user);
        return PGW_ENOENT;
    }
    if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
        setuid(pw->pw_uid) != 0) {
        PGW_ERR("privdrop: cannot become '%s': %s", user, strerror(errno));
        return pgw_code_from_host(errno);
    }
    return PGW_OK;
}

pgw_err pgw_privdrop_apply(const pgw_ability *list, size_t n, const char *user)
{
    for (size_t i = 0; i < n; i++) {
        pgw_err rc = allow_ability(list[i].aid, list[i].why);
        if (rc != PGW_OK)
            return rc;
    }
    if (user != NULL) {
        pgw_err rc = drop_to_user(user);
        if (rc != PGW_OK)
            return rc;
    }
    if (geteuid() == 0) {
        /* QNX-06 is about *not* needing root; staying root would hide the
         * failure of the drop. Refuse to run rather than lie. */
        PGW_ERR("privdrop: still root, refusing to serve");
        return PGW_EPERM;
    }
    PGW_INFO("privdrop: running as uid %u gid %u", (unsigned)geteuid(),
             (unsigned)getegid());
    return PGW_OK;
}
