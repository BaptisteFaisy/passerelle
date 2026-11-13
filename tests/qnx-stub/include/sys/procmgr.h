/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/procmgr.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-02 (start from the boot image and detach), QNX-06
 * and SEC-01 (drop root, keep only the abilities the driver needs).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, procmgr_daemon() and
 * procmgr_ability(); ability names and PROCMGR_AID_* constants from the
 * "Abilities Reference" of the System Security Guide:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_ability.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html
 * The PROCMGR_AID_* values are symbolic here: the drivers only OR them into a
 * mask, and the QNX build uses the SDP's own numbers.
 */
#ifndef PGW_STUB_SYS_PROCMGR_H
#define PGW_STUB_SYS_PROCMGR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* --- daemonisation -------------------------------------------------------- */

#define PROCMGR_DAEMON_NOCHDIR      0x00000001u
#define PROCMGR_DAEMON_NOCLOSE      0x00000002u
#define PROCMGR_DAEMON_NODEVNULL    0x00000004u
#define PROCMGR_DAEMON_KEEPUMASK    0x00000008u

int procmgr_daemon(int status, unsigned flags);

/* --- abilities (QNX-06, SEC-01) ------------------------------------------- */

/* One ability request is a single packed descriptor: the ability id (low 16
 * bits, PROCMGR_AID_*), the operation (PROCMGR_AOP_*) and the domain(s)
 * (PROCMGR_ADN_*), and every procmgr_ability() call is terminated by
 * PROCMGR_AID_EOL. The values below are the real QNX 8.0 ones (sys/procmgr.h,
 * "ability_desc_t" layout), so a QNX file that compiles against this stub
 * compiles against the SDK. */
#define PROCMGR_AID_MASK        0x0000ffffu
#define PROCMGR_AID_EOL         PROCMGR_AID_MASK

/* Operations. PROCMGR_AOP_INHERIT_YES/NO control inheritance across spawn() or
 * exec(); PROCMGR_AOP_LOCK makes an ability unchangeable from then on. */
#define PROCMGR_AOP_DENY        0x00010000u
#define PROCMGR_AOP_ALLOW       0x00020000u
#define PROCMGR_AOP_SUBRANGE    0x00040000u
#define PROCMGR_AOP_LOCK        0x00080000u
#define PROCMGR_AOP_INHERIT_YES 0x00400000u
#define PROCMGR_AOP_INHERIT_NO  0x00800000u

/* Domains: whether the request applies while running as root, nonroot, or
 * both. */
#define PROCMGR_ADN_ROOT        0x10000000u
#define PROCMGR_ADN_NONROOT     0x20000000u

/* The static abilities the drivers use, by name and real QNX 8 value (see the
 * Abilities Reference). */
#define PROCMGR_AID_MEM_PHYS        16u  /* map device / DMA memory */
#define PROCMGR_AID_INTERRUPT       29u  /* InterruptAttachThread/Event (VIO-04) */
#define PROCMGR_AID_PATHSPACE        5u  /* resmgr_attach() (QNX-01) */
#define PROCMGR_AID_PUBLIC_CHANNEL  44u  /* ChannelCreate() without PRIVATE */

int procmgr_ability(pid_t pid, unsigned ability, ...);

#endif /* PGW_STUB_SYS_PROCMGR_H */
