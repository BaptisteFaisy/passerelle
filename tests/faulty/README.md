<!--
SPDX-License-Identifier: Apache-2.0
-->

# tests/faulty - a server that lies, and the client that survives it

Requirement **ROB-07** ("a deliberately faulty server must not crash the
client") was unproven: no faulty server existed, so nothing ever attacked the
client. This directory is that attacker.

`faulty_server.py` is a real 9P2000.L server - it *imports* the reference
implementation of `tests/ref9p/` and answers every request correctly - with one
rule broken per run, chosen by `--fault`. It accepts the same command line as
`ref9p_server.py`, so the C harness of `tests/ref9p/` starts it unchanged: no
second process/socket helper is written here.

`test_faulty.c` drives the production client (`proto9p/`, the same session the
driver uses) against it and, for every armed fault, checks that the client
answers with the expected error, stays inside a time budget, keeps exactly the
root fid open, and - under `-DPGW_SANITIZERS=ON`, which the `sanitize` CI job
runs over the whole suite - reads nothing out of bounds and leaks nothing.

## The faults

Message level (the frame is well formed, the 9P message is not). They apply
once the session is attached, so the handshake always succeeds and the fault
lands on the data path:

| `--fault` | What the server does | The client must answer |
| --- | --- | --- |
| `truncate` | the body loses its last byte, `size[4]` stays consistent | `EINVAL` |
| `short-frame` | the whole reply loses its last byte, so `size[4]` lies | `EMSGSIZE` |
| `oversize` | `size[4]` claims 2 GiB for a short message | `EMSGSIZE` |
| `bad-tag` | the reply answers a tag the client never sent | `EINVAL` |
| `out-of-order` | the reply carries the tag of an earlier request | `EINVAL` |
| `garbage` | 64 bytes that are not a message at all | `EMSGSIZE` |
| `read-count-lie` | `Rread` claims 100 more data bytes than it carries | `EMSGSIZE` |
| `walk-overflow` | `Rwalk` claims three more qids than it carries | `EINVAL` |
| `version-mismatch` | `Rversion` answers `9P2000`, not `9P2000.L` | no session at all |

Transport level (the message would be fine, the framing is not):

| `--fault` | What the server does | The client must answer |
| --- | --- | --- |
| `unsolicited` | a reply for a request that was never sent comes first | `EINVAL` |
| `huge-frame` | the length prefix claims 4 GiB | `EMSGSIZE` |
| `never-ends` | the prefix promises 4096 bytes that never arrive | `ETIMEDOUT` |
| `late-reply` | the reply comes 1.2 s late, after the client gave up | `ETIMEDOUT` |
| `close-mid-frame` | the connection dies in the middle of a frame | `EIO` |

## What it found

`close-mid-frame` killed the whole client process with **SIGPIPE** (exit 141):
after the server closed the socket, `pgw_p9_session_close()` still sent its
final `Tclunk`, and the reference transport called `send()` without
`MSG_NOSIGNAL`. A hostile server could therefore crash the client with a single
well-timed `close()` - exactly what ROB-07 forbids. Fixed in
`tests/ref9p/ref9p_harness.c`; the transport now reports `PGW_EIO`.

Reproducer:

```sh
cmake -B build && cmake --build build -j8 --target faulty_9p_client
./build/tests/faulty/faulty_9p_client        # before the fix: rc=141 (SIGPIPE)
```

## Running it

```sh
cmake -B build && cmake --build build -j8
(cd build && ctest -R faulty_9p_client --output-on-failure)
cmake -B build-asan -DPGW_SANITIZERS=ON && cmake --build build-asan -j8
(cd build-asan && ctest -R faulty_9p_client --output-on-failure)
```

One server is started per fault (fourteen in total); the two stalling faults
are bounded by the client's own 300 ms deadline, so the whole binary runs in a
few seconds and stays inside the 5-minute CI budget.

## Scope

Only the client side is attacked, as gap 20 asks. Our code acting as a *server*
facing a hostile client is not covered here.
