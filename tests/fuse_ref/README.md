# `tests/fuse_ref/` — a reference FUSE backend

The Passerelle FUSE client (`protofuse/`) used to be tested only against
`protofuse/tests/fake_virtiofsd.c`, an in-memory server written by the same
people, from the same reading of the specification. Every assertion of that
suite was therefore *self-referential*: if our reading of the wire format was
wrong, the fake server was wrong in exactly the same way and the test still
passed.

This directory adds a second, independent implementation — a **reference FUSE
backend** written in Python (standard library only) that serves a **real
directory** — and runs the client against it.

```
fuse_ref_server.py    the reference server (CLI entry point)
fuse_ref_wire.py      wire constants, framing, struct pack/unpack helpers
fuse_ref_ops.py       namespace operations of the server (mixin)
fuse_ref_harness.[ch] fork+exec the server, UNIX-socket pgw_fuse_rpc_fn
fuse_ref_truth.c      the oracle: read the served directory with plain POSIX
test_fuse_ref.c       INIT, lookup, getattr, open, read, write, create,
                      readdir, setattr, statfs — each checked against the disk
test_fuse_ref_ns.c    mkdir, rmdir, unlink, rename, symlink, readlink, link,
                      release, forget, interrupt, fsync — and the error codes
test_fuse_ref_robust.c  the safety valve: malformed replies from the server
CMakeLists.txt        three pgw_test() targets
```

## What the server is

`fuse_ref_server.py` speaks the **FUSE kernel wire protocol** — `fuse_in_header`
/ `fuse_out_header`, `fuse_entry_out`, `fuse_attr`, `fuse_dirent`, negative
Linux errno in `fuse_out_header.error` — over a **UNIX stream socket**. Each
message is prefixed with its length as **4 bytes, big-endian**, which is the
framing our transport uses.

It does **not** go through `/dev/fuse`: that device is unavailable or reserved
in most sandboxes and in CI, and it is not what we need. The socket carries
exactly the byte stream the client is written for, so the test runs anywhere.

Implemented operations: `INIT`, `LOOKUP`, `FORGET`, `BATCH_FORGET`, `GETATTR`,
`SETATTR`, `OPEN`, `READ`, `WRITE`, `RELEASE`, `OPENDIR`, `READDIR`,
`RELEASEDIR`, `CREATE`, `MKDIR`, `UNLINK`, `RMDIR`, `RENAME`/`RENAME2`,
`SYMLINK`, `READLINK`, `LINK`, `STATFS`, `FLUSH`, `FSYNC`/`FSYNCDIR`,
`INTERRUPT`, `DESTROY`.

The state is the directory itself: node ids map to real paths, file handles map
to real descriptors, attributes come from `lstat()`, and errors come from the
errno of the failed syscall. There is no second copy of the truth to disagree
with.

```
python3 tests/fuse_ref/fuse_ref_server.py --help
```

| option | meaning |
| --- | --- |
| `--root DIR` | the directory to serve (required) |
| `--socket PATH` | where to listen; default `<root>.sock`, **outside** the root so it never shows up in `READDIR` |
| `--ready-fd N` | write the socket path plus a newline to fd `N` once listening — the caller waits on this instead of sleeping |
| `--minor N` | protocol minor announced in `INIT` (default 36) |
| `--max-write N` | payload ceiling announced in `INIT` (default 65536) |
| `--fault LIST` | comma-separated malformed replies, one per request after `INIT` |
| `--idle-timeout S` | give up after `S` idle seconds (default 60) |

The server serves exactly one client, exits when that client disconnects, exits
when its parent dies, and exits after `--idle-timeout`: a crashed test cannot
leave one behind.

## What it proves

Requirements **VFS-01 … VFS-05** of the specification, now against something
that is not our own fake:

* **VFS-01 / VFS-03** — version negotiation with a server that announces 7.36
  and with one that announces 7.12; the whole operation set round-trips.
* **VFS-02** — the session opens, queues and closes over a real transport
  (socket, framing, `pgw_fuse_rpc_fn`), including the no-reply requests
  (`FORGET`, `BATCH_FORGET`, `INTERRUPT`) that the transport must not wait for.
* **VFS-03** — every answer is compared with the folder on disk: content read
  back byte for byte, sizes, types, permission bits, `st_nlink`, `st_ino`,
  symlink targets, and the names returned by `READDIR`. Writes are checked by
  reading the real file with `open()`/`read()`, not by asking the server.
* **VFS-04** — a `LOOKUP` takes a reference, `FORGET` gives it back, and the
  server really forgets the node: a later `GETATTR` answers `ENOENT` and a new
  `LOOKUP` of the same name hands out a different node id.
* **VFS-05** — error codes travel back unchanged (`ENOENT`, `EEXIST`,
  `ENOTDIR`, `EISDIR`, `ENOTEMPTY`, `ELOOP` on a symlink loop), and a server
  that answers with an **unknown FUSE error number**, an **inconsistent
  declared length**, a **truncated header**, a **mismatched `unique`** or a
  **positive error field** is refused: the client reports `PGW_EIO`, keeps its
  reference table clean, and keeps working.

`--fault` is how the last point is produced. The names are `unknown-errno`,
`bad-length`, `short-header`, `bad-unique`, `positive-error`; they are consumed
in order, one per request, after `INIT` (so the session always negotiates), and
the server serves normally once the list is exhausted.

## What it does not prove

* **It is not the Linux kernel.** No VFS, no dentry cache, no page cache, no
  mount, no `fusermount`. Semantics the kernel owns — path resolution across
  symlinks, `ELOOP` depth counting, permission checks against the caller's
  credentials, `O_TRUNC`/`O_APPEND` interactions with the page cache — are not
  exercised here. `ELOOP` is produced the way a real file system would produce
  it (the `open()` of a self-referencing symlink fails in the kernel), but the
  client is still not talking to a mounted file system.
* **It is not virtiofsd.** A second implementation raises confidence; it does
  not replace the real server, and it does not cover the virtio transport
  itself (that is `virtio/` and `tests/qemu/`).
* **It is single-threaded and synchronous.** No concurrency, no locking, no
  interrupted-request races. `INTERRUPT` is accepted and ignored, as the kernel
  allows.
* **Only the protocol is under test here**, on the PC, on Linux. The QNX side
  is `tests/qnx/`.

## Running it

Through CMake, once the coordinator has wired this directory into the root
build (nothing outside `tests/fuse_ref/` is touched here):

```bash
cmake -B /tmp/build-fuse-ref -S . \
      "-DPGW_MODULES=common;virtio;proto9p;protofuse;rmfs;tests;tests/fuse_ref"
cmake --build /tmp/build-fuse-ref -j4
ctest --test-dir /tmp/build-fuse-ref -R "^fuse_ref" --output-on-failure
```

(`-R "^fuse_ref"` and not `-R fuse_ref`: the latter also matches the
pre-existing `protofuse_refs` test.)

By hand, from the repository root (no CMake needed):

```bash
cc -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual \
   -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wconversion \
   -Wsign-conversion -Werror \
   -I common/include -I protofuse/include -I tests/fuse_ref \
   tests/fuse_ref/test_fuse_ref.c tests/fuse_ref/fuse_ref_harness.c \
   tests/fuse_ref/fuse_ref_truth.c protofuse/src/*.c common/src/*.c \
   -o /tmp/test_fuse_ref && /tmp/test_fuse_ref
```

Replace `test_fuse_ref.c` with `test_fuse_ref_ns.c` or
`test_fuse_ref_robust.c` for the other two binaries. The interpreter and the
server script can be overridden with `PGW_FUSE_REF_PY` and
`PGW_FUSE_REF_SERVER`; the defaults are `python3` and
`tests/fuse_ref/fuse_ref_server.py` (hence: run from the repository root).

The server can also be driven by hand, which is the quickest way to inspect a
reply:

```bash
mkdir -p /var/tmp/truth && echo hello > /var/tmp/truth/a.txt
python3 tests/fuse_ref/fuse_ref_server.py --root /var/tmp/truth \
        --socket /var/tmp/ref.sock --fault bad-unique
# then connect to /var/tmp/ref.sock and send length-prefixed FUSE messages
```

Each test case creates its own temporary directory under `/tmp` (removed on
exit, even when a case fails), starts one server, and kills it on the way out.
The three binaries run in about one second in total.
