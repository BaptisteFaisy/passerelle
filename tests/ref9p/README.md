# tests/ref9p — reference 9P2000.L server

Until now the 9P client in `proto9p/` was only ever exercised against an
in-memory fake server written by the same author. That made the conformance
claims P9-01 … P9-10 self-referential: the two sides of the conversation shared
the same assumptions, and a bug in the encode/decode of a field would pass
because both halves made the same mistake.

This directory removes that blind spot with a **second, independent
implementation** of the protocol:

* `ref9p_server.py` (plus its two small modules `ref9p_wire.py` and
  `ref9p_ops.py`) is a reference 9P2000.L server in standard-library Python 3.
  It serves a *real* directory tree, so every operation — read, write,
  create, getattr, setattr, readdir, mkdir, symlink, readlink, link, rename,
  unlink, fsync, statfs — is performed by the host file system, not by a
  simulation.
* `test_ref9p.c` drives the client through a full session against that server
  and compares every result with the real directory (the "truth"), then checks
  the error codes a real server returns (ENOENT, EEXIST, EBADF, ENOTDIR,
  ELOOP), and finally checks the client's validation path against a server
  that deliberately breaks the protocol.
* `test_ref9p_fields.c` and `test_ref9p_encode.c` check the *bytes*: both our
  encoders and our decoders are read at the offsets of the normative 9P2000.L
  message table (the `diod` protocol description, the same source
  `docs/spec-en.md` references), through an independent oracle
  (`ref9p_fields.c`) that shares no code with `pgw_buf`.

## What it proves

* **P9-01** version/msize negotiation against a real server (which may lower
  `msize`, never raise it).
* **P9-02 / P9-03** the full namespace and data path, byte-for-byte against a
  real directory.
* **P9-04** Linux errno values cross the wire and come back as the right
  internal codes; an unknown errno is never a success.
* **P9-07** after a long session the only fid left open is the root.
* **P9-08** a write larger than one message is split and reassembled correctly.
* **P9-09** truncated messages and unknown errnos are rejected, never decoded
  halfway, never crash the client.
* **P9-10** the client runs unchanged against a second implementation.

## Running it by hand

The server takes no non-standard dependencies; Python 3 with only the standard
library is enough:

```sh
python3 tests/ref9p/ref9p_server.py --root /tmp/share --port 0 --ready-fd 3
```

`--port 0` lets the kernel choose a free port, which the server prints to the
descriptor given by `--ready-fd` (default stdout when omitted). A negative
`--ready-fd` disables the announcement. `--fault unknown-errno` or
`--fault truncate` injects a deliberate protocol violation once the session is
attached.

The C benches are ordinary `pgw_test` executables; see `CMakeLists.txt`. They
locate `python3` and `ref9p_server.py` through the `PGW_REF9P_PYTHON` /
`PGW_REF9P_SERVER` environment variables, through the compile-time definitions
that CMake sets, or through relative paths when compiled by hand.

## Known limits

This is a *reference*, not a production server:

* It implements the subset of 9P2000.L the client uses, not the full
  protocol: `auth`, `mknod`, `xattrwalk`/`xattrcreate`, `lock`/`getlock`,
  `rename`/`remove` (the legacy forms) and `read` of directory contents are not
  served (they answer `Rlerror`/`ENOSYS`).
* It is single-user and single-writer: every request is served synchronously,
  so `Tflush` always returns immediately.
* `readdir` does not return the `.`/`..` pseudo-entries, and uses the entry
  index as the seek cookie rather than `telldir` — deterministic and correct,
  but not what the Linux kernel would emit.
* `setattr` uses `st_atim`/`st_mtim`, and the `ctime`/`atime`-without-`_SET`
  cases use the server's current time, not the file's.