# QNX 8.0 ABI values this project depends on

Every number below is one the project *uses*: a translation table, a message
field, a mount flag or an ability. Each entry says where the value comes from
and what breaks if it changes.

Two things keep this list honest:

- the project never hard-codes a QNX number in its own tables. The errno, open
  flag and file type translations live once in `common/src/err.c` and
  `common/src/flags.c`, and they read the target's real `<errno.h>`,
  `<fcntl.h>` and `<sys/stat.h>`. This document says what those headers are
  expected to contain;
- `drivers/tests/qnx_abi_assert.c` checks the values below with
  `_Static_assert` **on QNX only**. A change in the target's headers therefore
  breaks the *target* build, not the PC one, which is where the change should
  be noticed.

The PC build compiles that file to nothing: the QNX headers do not exist
there, and comparing the host's numbers with QNX's would prove nothing.

## Sources

| Short name | Document |
|---|---|
| ERRNO | [errno](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/e/errno.html) |
| OPEN | [open](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html) |
| STAT | [stat](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/stat.html) |
| IOMSG | [`_io_connect`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/_io_connect.html) |
| INTR | [InterruptAttachThread](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/interruptattachthread.html) |
| TRACE | [TraceEvent](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/traceevent.html) |
| SEC | [Abilities Reference](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html) |
| SECPOL | [Security policy language](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/secpol_language.html) |
| RESMGR | [Writing a Resource Manager](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/about.html) |

## errno

The wire (9P `Rlerror`, FUSE `error`) carries **Linux** numbers; the guest
returns **QNX** numbers. The two differ above 34, which is why the project
never casts between them: `common/src/err.c` holds one table with both columns.

| Name | QNX 8.0 | Linux | Used by |
|---|---|---|---|
| `EOK` | 0 | 0 | every return path |
| `EPERM` | 1 | 1 | permission checks (FS-09) |
| `ENOENT` | 2 | 2 | a missing path |
| `EINTR` | 4 | 4 | an interrupted read or write (QNX-05) |
| `EIO` | 5 | 5 | the fallback for an unknown error (ROB-01) |
| `EBADF` | 9 | 9 | an operation on a closed handle |
| `EAGAIN` | 11 | 11 | a saturated queue (QNX-04) |
| `ENOMEM` | 12 | 12 | an allocation failure |
| `EACCES` | 13 | 13 | a refused permission |
| `EBUSY` | 16 | 16 | removing the mount point |
| `EEXIST` | 17 | 17 | `O_CREAT` on an existing file |
| `EXDEV` | 18 | 18 | a cross-device link |
| `ENODEV` | 19 | 19 | no device with that mount tag (VIO-05) |
| `ENOTDIR` | 20 | 20 | a file used as a directory |
| `EISDIR` | 21 | 21 | a directory used as a file |
| `EINVAL` | 22 | 22 | a malformed request |
| `EMFILE` | 24 | 24 | too many open files |
| `ENOSPC` | 28 | 28 | the disk is full |
| `EROFS` | 30 | 30 | a write on a read-only mount (SEC-01) |
| `EDOM` | 33 | 33 | an out-of-range argument |
| `ERANGE` | 34 | 34 | an out-of-range result |
| `EDEADLK` | 45 | 45 | a deadlock on a byte-range lock (FS-14) |
| `ENOLCK` | 46 | 46 | no lock available |
| `ECANCELED` | 47 | 47 | a cancelled request (P9-06) |
| `ENOTSUP` | 48 | 95 | an operation the protocol cannot do |
| `ELOOP` | 90 | 40 | too many symlink redirects (FS-08) |
| `ENOTEMPTY` | 93 | 39 | `rmdir` on a non-empty directory |
| `EOPNOTSUPP` | 103 | 95 | distinct from `ENOTSUP` on QNX |
| `EOVERFLOW` | 79 | 75 | a value that does not fit |
| `ENAMETOOLONG` | 78 | 36 | a name over 255 bytes (FS-15) |
| `ENOSYS` | 89 | 38 | the classic Linux/QNX difference |

Note the three that most often trip a port: `ENOSYS` (38 on Linux, 89 on
QNX), `ENOTSUP`/`EOPNOTSUPP` (QNX keeps them apart, Linux merges them), and
`ELOOP` (40 on Linux, 90 on QNX).

## Open flags

From **OPEN**. QNX keeps the POSIX values of the access mode, so
`pgw_flags_from_host()` (in `common/src/flags.c`) can read them directly.

| Name | Value | Note |
|---|---|---|
| `O_RDONLY` | 0 | access mode |
| `O_WRONLY` | 1 | access mode |
| `O_RDWR` | 2 | access mode |
| `O_ACCMODE` | 3 | mask of the three above |
| `O_CREAT` | flag | creates the file |
| `O_EXCL` | flag | fails if the file exists (FS-04) |
| `O_TRUNC` | flag | truncates at open (FS-04) |
| `O_APPEND` | flag | appends at write |
| `O_SYNC` / `O_DSYNC` | flags | durability |
| `O_DIRECTORY` | flag | the path must be a directory |
| `O_NOFOLLOW` | flag | the last component must not be a link |

The *connect message* carries something else: QNX rewrites the access mode
into two bits, `_IO_FLAG_RD` (0x01) and `_IO_FLAG_WR` (0x02), so that they do
not overlap any other `O_*` flag (**IOMSG**). The driver reads those two bits
and hands the rest of the word to the shared translator.

## File types

From **STAT**. QNX uses the POSIX values, so they are *not* the Linux ones the
wire carries; `pgw_host_mode()` and `pgw_linux_mode()` convert between them.

| Name | QNX 8.0 | Linux |
|---|---|---|
| `S_IFMT` | 0170000 | 0170000 |
| `S_IFIFO` | 0010000 | 0010000 |
| `S_IFCHR` | 0020000 | 0020000 |
| `S_IFDIR` | 0040000 | 0040000 |
| `S_IFBLK` | 0060000 | 0060000 |
| `S_IFREG` | 0100000 | 0100000 |
| `S_IFLNK` | 0120000 | 0120000 |
| `S_IFSOCK` | 0140000 | 0140000 |
| `S_ISVTX` | 01000 | 01000 |

The permission bits (07777 plus the setuid, setgid and sticky bits) are the
same everywhere; the project keeps them in `pgw/err.h` as `PGW_S_*` so no file
writes them out again.

## Message numbers and flags

From **IOMSG** and **RESMGR**.

| Name | Meaning | Where the project uses it |
|---|---|---|
| `_IO_CONNECT` | the connect message (open, unlink, rename, mknod, link, readlink) | the namespace handlers |
| `_IO_READ`, `_IO_WRITE`, `_IO_STAT`, `_IO_LSEEK`, `_IO_SYNC`, `_IO_SPACE`, `_IO_CHMOD`, `_IO_CHOWN`, `_IO_UTIME`, `_IO_DEVCTL` | the I/O messages | the io handlers |
| `_IO_CONNECT_EFLAG_DIR` | the path named a directory | the open handler |
| `_IO_CONNECT_EXTRA_SYMLINK`, `_IO_CONNECT_EXTRA_LINK`, `_IO_CONNECT_EXTRA_RENAME` | the extra data of a connect message | symlink, link and rename |
| `_IO_CONNECT_RET_FLAG`, `_IO_CONNECT_RET_LINK` | the status that answers "this path is a link" | the symlink redirect (FS-08) |
| `_IO_XTYPE_NONE`, `_IO_XTYPE_OFFSET`, `_IO_XTYPE_READDIR` | the extended type of a read or write | `pread()`, `pwrite()`, `readdir()` |
| `_FTYPE_ANY` | the file type of `resmgr_attach()` | the mount point (QNX-01) |
| `_RESMGR_FLAG_DIR` | the manager serves every path below its mount point | the mount point (QNX-01) |
| `_RESMGR_DETACH_PATHNAME` | detach the path, not the channel | the umount (QNX-07) |

The **values** of the message numbers are not quoted here on purpose: the
project only ever passes them back to the resource-manager library, it never
compares them numerically. The stub headers in `tests/qnx-stub/include` use
symbolic values for them, and `drivers/tests/qnx_abi_assert.c` checks the
ones the project does compare.

## Interrupts

From **INTR**. QNX 8.0 **removed `InterruptAttach()`**: a handler can no
longer run in interrupt context. The only supported form is an Interrupt
Service Thread attached with `InterruptAttachThread()`, which the drivers use
(VIO-04).

| Name | Meaning |
|---|---|
| `_NTO_INTR_FLAGS_NO_UNMASK` | the line stays masked until the thread unmasks it |
| `_NTO_INTR_FLAGS_EXCLUSIVE` | only one thread may be attached |
| `_NTO_INTR_FLAGS_CPU_LOCAL` | the interrupt is bound to the CPU |
| `_NTO_INTR_WAIT_FLAGS_UNMASK` | `InterruptWait()` unmasks the line |

## Trace events

From **TRACE**. The user event ids must fall inside
`_NTO_TRACE_USERFIRST .. _NTO_TRACE_USERLAST`, otherwise the kernel drops the
event silently and QNX-09 is not met.

| Name | Meaning |
|---|---|
| `_NTO_TRACE_INSERTSUSEREVENT` | insert a user simple event (two integers) |
| `_NTO_TRACE_INSERTCUSEREVENT` | insert a user combine event (an array) |
| `_NTO_TRACE_INSERTUSRSTREVENT` | insert a user string event |
| `_NTO_TRACE_USERFIRST`, `_NTO_TRACE_USERLAST` | the range of user event ids |

The driver numbers its events `2 * operation` (submission) and
`2 * operation + 1` (reception), so a trace reader can pair a request with its
reply on the tag.

## Abilities

From **SEC**. The policy in `image/passerelle.sec` and the runtime privilege
drop in `drivers/common/src/privdrop_qnx.c` grant exactly these four.

| Ability | Why the driver needs it |
|---|---|
| `mem_phys` | map the BAR of its own device (VIO-02) |
| `interrupt` | attach its Interrupt Service Thread (VIO-04) |
| `pathspace` | register its mount point (QNX-01) |
| `public_channel` | create the channel `dispatch_create()` needs |

`mem_phys` and `interrupt` are **privileged** abilities: without them the
driver cannot touch the device at all. `pathspace` and `public_channel` are
not, but a policy that omits them stops the driver from mounting (see the
"Abilities Reference" page for the "Privileged?" column).

## What is deliberately *not* pinned

- The **message numbers**: the project never compares them.
- The **`IOFUNC_*` and `_MOUNT_*` bits**: the iofunc layer owns them, the
  driver only ORs them into a word it hands over.
- The **`PROCMGR_AID_*` values**: the policy language names the abilities, and
  the runtime drop uses the same names, so no number is written anywhere.
