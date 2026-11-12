/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/tests/qnx_abi_assert.c - the QNX numbers this project depends on,
 * checked against the real SDP headers.
 *
 * On QNX this file is compiled and its `_Static_assert`s compare the values
 * the drivers and the documentation quote (docs/qnx-abi.md) with the ones the
 * target's own <errno.h>, <fcntl.h>, <sys/stat.h> and <sys/iomsg.h> define.
 * A mismatch is a compile error on the target, which is where it belongs: a
 * number that differs between the PC build and the target must never reach a
 * release.
 *
 * On the PC this file does nothing at all: the QNX headers do not exist, so
 * the whole file is excluded and the PC build never depends on it.
 *
 * Covered requirements: P9-04 (Linux errno to QNX errno), FS-02 (file modes),
 * FS-09 (open flags), QNX-01 (message layout), QNX-06 (abilities), SEC-01.
 */
#if defined(__QNX__)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>

#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/trace.h>

/* --- errno (QNX 8.0 C Library Reference, "errno") --------------------------
 *
 * Source: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/e/errno.html
 * The values quoted here are the ones listed on that page. The point of the
 * check is not the numbers themselves but the *difference* from Linux: the
 * project translates between the two through pgw/err.h, and a silent change
 * on either side would corrupt every error code the driver returns (ROB-01).
 */
_Static_assert(EOK == 0, "QNX EOK must be 0");
_Static_assert(EPERM == 1, "QNX EPERM must be 1");
_Static_assert(ENOENT == 2, "QNX ENOENT must be 2");
_Static_assert(EINTR == 4, "QNX EINTR must be 4");
_Static_assert(EIO == 5, "QNX EIO must be 5");
_Static_assert(EBADF == 9, "QNX EBADF must be 9");
_Static_assert(EAGAIN == 11, "QNX EAGAIN must be 11");
_Static_assert(ENOMEM == 12, "QNX ENOMEM must be 12");
_Static_assert(EACCES == 13, "QNX EACCES must be 13");
_Static_assert(EBUSY == 16, "QNX EBUSY must be 16");
_Static_assert(EEXIST == 17, "QNX EEXIST must be 17");
_Static_assert(EXDEV == 18, "QNX EXDEV must be 18");
_Static_assert(ENODEV == 19, "QNX ENODEV must be 19");
_Static_assert(ENOTDIR == 20, "QNX ENOTDIR must be 20");
_Static_assert(EISDIR == 21, "QNX EISDIR must be 21");
_Static_assert(EINVAL == 22, "QNX EINVAL must be 22");
_Static_assert(EMFILE == 24, "QNX EMFILE must be 24");
_Static_assert(ENOSPC == 28, "QNX ENOSPC must be 28");
_Static_assert(EROFS == 30, "QNX EROFS must be 30");
_Static_assert(EDOM == 33, "QNX EDOM must be 33");
_Static_assert(ERANGE == 34, "QNX ERANGE must be 34");
_Static_assert(EDEADLK == 45, "QNX EDEADLK must be 45");
_Static_assert(ENAMETOOLONG == 78, "QNX ENAMETOOLONG must be 78");
_Static_assert(ENOLCK == 46, "QNX ENOLCK must be 46");
_Static_assert(ENOSYS == 89, "QNX ENOSYS must be 89 (Linux says 38)");
_Static_assert(ENOTEMPTY == 93, "QNX ENOTEMPTY must be 93");
_Static_assert(ELOOP == 90, "QNX ELOOP must be 90");
_Static_assert(ENOTSUP == 48, "QNX ENOTSUP must be 48 (QNX keeps it apart from EOPNOTSUPP)");
_Static_assert(EOPNOTSUPP == 103, "QNX EOPNOTSUPP must be 103");
_Static_assert(EOVERFLOW == 79, "QNX EOVERFLOW must be 79");
_Static_assert(ECANCELED == 47, "QNX ECANCELED must be 47");

/* --- open flags (QNX 8.0 C Library Reference, "open") --------------------
 *
 * Source: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html
 * The access mode is a trio of values in the POSIX range, the rest are flags
 * the project never compares numerically: what matters is that they exist and
 * that O_TRUNC / O_APPEND / O_SYNC keep their POSIX meaning (FS-04).
 */
_Static_assert(O_RDONLY == 0, "QNX O_RDONLY must be 0");
_Static_assert(O_WRONLY == 1, "QNX O_WRONLY must be 1");
_Static_assert(O_RDWR == 2, "QNX O_RDWR must be 2");
/* QNX has a fourth access mode, O_EXEC (000003): the mask covers the three
 * POSIX modes and the execute one, so it is 7 and not the 3 of Linux. Measured
 * on the SDP 8.0 headers on 12/11/2026 (usr/include/fcntl.h:72). */
_Static_assert(O_ACCMODE == 7, "QNX O_ACCMODE must be 7 (O_EXEC is a fourth mode)");
_Static_assert(O_EXEC == 3, "QNX O_EXEC must be 3");
_Static_assert(O_CREAT != 0 && O_EXCL != 0 && O_TRUNC != 0, "QNX open flags exist");
_Static_assert(O_APPEND != 0 && O_SYNC != 0 && O_DSYNC != 0, "QNX open flags exist");
_Static_assert(O_DIRECTORY != 0 && O_NOFOLLOW != 0, "QNX open flags exist");

/* --- file types (QNX 8.0 C Library Reference, "stat") --------------------
 *
 * Source: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/stat.html
 * The project converts the Linux S_IF* bits to the host ones through
 * pgw_host_mode(), so the two sets are never assumed equal; the check below
 * only pins the QNX ones, which the iofunc layer uses.
 */
_Static_assert(S_IFMT == 0170000, "QNX S_IFMT must be 0170000");
_Static_assert(S_IFREG == 0100000, "QNX S_IFREG must be 0100000");
_Static_assert(S_IFDIR == 0040000, "QNX S_IFDIR must be 0040000");
_Static_assert(S_IFLNK == 0120000, "QNX S_IFLNK must be 0120000");
_Static_assert(S_IFCHR == 0020000, "QNX S_IFCHR must be 0020000");
_Static_assert(S_IFBLK == 0060000, "QNX S_IFBLK must be 0060000");
_Static_assert(S_IFIFO == 0010000, "QNX S_IFIFO must be 0010000");
_Static_assert(S_IFSOCK == 0140000, "QNX S_IFSOCK must be 0140000");
_Static_assert(S_ISVTX == 01000, "QNX sticky bit must be 01000");

/* --- the message the driver answers a symlink with -----------------------
 *
 * Source: QNX SDP 8.0 C Library Reference, `_io_connect` and
 * `_io_connect_link_reply`; the driver sends _IO_CONNECT_RET_LINK in the
 * status of a connect reply so the client restarts the resolution (FS-08).
 */
_Static_assert(_IO_CONNECT_RET_FLAG != 0, "QNX _IO_CONNECT_RET_FLAG exists");
_Static_assert(_IO_CONNECT_RET_LINK != 0, "QNX _IO_CONNECT_RET_LINK exists");

/* --- the interrupt interface QNX 8.0 kept -------------------------------
 *
 * Source: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/interruptattachthread.html
 * InterruptAttach() is gone: the driver attaches a thread, so the flag exists
 * and the call is declared (VIO-04).
 */
_Static_assert(_NTO_INTR_FLAGS_NO_UNMASK != 0, "QNX _NTO_INTR_FLAGS_NO_UNMASK exists");
_Static_assert(_NTO_INTR_FLAGS_EXCLUSIVE != 0, "QNX _NTO_INTR_FLAGS_EXCLUSIVE exists");

/* --- the trace events the driver inserts ----------------------------------
 *
 * Source: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/traceevent.html
 * The user event ids must be inside the range the kernel accepts, otherwise
 * the event is silently dropped and QNX-09 is not met (QNX-09).
 */
_Static_assert(_NTO_TRACE_USERFIRST == 0, "QNX user trace range starts at 0");
_Static_assert(_NTO_TRACE_USERLAST > _NTO_TRACE_USERFIRST, "QNX user trace range is not empty");
/* The user range is 0..0x3ff in SDP 8.0 (sys/trace.h:168), so 1024 event ids:
 * the driver's 2 * 20 + 1 ids fit with room to spare. */
_Static_assert(2 * 20 + 1 < _NTO_TRACE_USERLAST - _NTO_TRACE_USERFIRST,
               "the driver's event ids must fit the user range");

/* --- the abilities the driver asks for -----------------------------------
 *
 * Source: QNX SDP 8.0 System Security Guide, "Abilities Reference"
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html
 * Each of the four must exist, or the privilege drop would ask for a name the
 * kernel does not know (QNX-06, SEC-01).
 */
_Static_assert(PROCMGR_AID_MEM_PHYS != 0, "QNX mem_phys ability exists");
_Static_assert(PROCMGR_AID_INTERRUPT != 0, "QNX interrupt ability exists");
_Static_assert(PROCMGR_AID_PATHSPACE != 0, "QNX pathspace ability exists");
_Static_assert(PROCMGR_AID_PUBLIC_CHANNEL != 0, "QNX public_channel ability exists");

/* Nothing to run: the file exists to be compiled. A definition keeps the
 * translation unit non-empty for the tools that complain about it. */
int pgw_qnx_abi_assert_placeholder(void);

int pgw_qnx_abi_assert_placeholder(void)
{
    return 0;
}

#else /* !__QNX__ */

/* On the PC the file is empty: the QNX headers do not exist here, and the
 * checks above would be meaningless (they would compare the host's numbers
 * with QNX's). The PC build verifies the drivers' syntax against
 * tests/qnx-stub/include instead. */

#endif /* __QNX__ */
