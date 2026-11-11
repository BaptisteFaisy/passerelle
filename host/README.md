# Passerelle host tools

Host-side tools that share a PC folder with a QNX 8.0 guest under QEMU,
prove that the share behaves like a POSIX filesystem, and publish the
measurements the specification asks for.

Python 3.10+ and the **standard library only**: no runtime dependency, no
tracing dependency, no plotting dependency. `pytest` is used by the test
suite and by nothing else.

## The four commands

| Command | What it does | Requirements |
|---|---|---|
| `qshare` | Prints the exact QEMU options for a 9P or virtiofs share, writes the `mkqnximage` snippets, starts QEMU (and `virtiofsd`) itself | DX-01, VFS-06, SEC-03 |
| `qrun` | Runs a program from the share inside the guest and returns its output and exit status | DX-03 |
| `fsproof` | Differential test, content exerciser and directed cases, against this driver or against an existing client | 7.2, 7.3, 7.4, PERF-07 |
| `pbench` | Roundtrips per POSIX call, the six workloads of 7.5, and the comparative report with the thresholds of 7.6 | PERF-01..PERF-07 |

## Install

```sh
cd host
pip install -e .
qshare --help && qrun --help && fsproof --help && pbench --help
```

Without installing, everything works from the repository root:

```sh
PYTHONPATH=host python3 -m qshare.cli --help
PYTHONPATH=host python3 -m fsproof.cli --help
PYTHONPATH=host python3 -m bench.cli --help
```

## The demonstration, in one command (DX-02)

```sh
qshare --demo ~/projet
```

Five steps, in order: the QEMU options for the share, the `mkqnximage`
snippets that mount it at boot, a program executed straight from the share,
a file edited on the PC and seen changed at once, then a short differential
test and a short content exercise. Add `--run <image>` to start QEMU on a
real image; without it the guest steps run locally and say so.

## Share a folder (DX-01)

```sh
qshare ~/projet                       # QEMU options for a 9P share
qshare ~/projet --emit                # one shell-ready line
qshare ~/projet --tag projet --proto virtiofs --emit
qshare ~/projet --mkqnximage ~/qnxvm  # write <vm>/local/snippets/*.custom
qshare ~/projet --emit-image --ifs    # IFS layout instead of system files
qshare --security-help                # the QEMU security models (SEC-03)
```

For 9P the tool emits:

```
-fsdev local,id=host,path=/home/you/projet,security_model=mapped-xattr \
-device virtio-9p-pci,fsdev=host,mount_tag=projet
```

For virtiofs it emits a shared memory backend, a vhost-user socket and the
device, and `--run` starts `virtiofsd` on that socket for you (VFS-06):

```
-object memory-backend-memfd,id=mem,size=1G,share=on -numa node,memdev=mem \
-chardev socket,id=char0,path=/tmp/pgw-vhost-fs-projet.sock \
-device vhost-user-fs-pci,chardev=char0,tag=projet
```

Inputs are validated before anything is printed: the directory must exist,
the tag must be at most **31 bytes for 9P** and **36 bytes for virtiofs**
(measured against QEMU 8.2, which refuses longer tags), and the security
model must be one the tool documents.

### Security models (SEC-03)

`mapped-xattr` is the default and the recommendation: guest ownership and
permissions are stored in `user.virtfs.*` extended attributes on the host
file, so **QEMU never needs to run as root**. `mapped-file` does the same
with a side file. `passthrough` uses host credentials verbatim and therefore
requires root — a guest root can then change host files. `none` treats every
guest user as the QEMU user.

## Run a program from the share (DX-03)

```sh
# guest console served by QEMU: -serial unix:/tmp/ser.sock,server=on,wait=off
qrun --share ~/projet --console /tmp/ser.sock build/mon_programme --fast
qrun --share ~/projet --local build/mon_programme     # on the PC, for the demo
```

The exit status of the command is the exit status of `qrun`. The guest side
needs nothing but a shell: `qrun` writes a framed command to the console
(`qrun-begin`/`qrun-rc`/`qrun-end`) and reads the transcript back.

## Examples: CMake and Rust projects (DX-04)

```sh
qshare ~/projet --example all        # writes cmake_example/ and rust_example/
qrun --share ~/projet --console /tmp/ser.sock cmake_example/run_tests.sh
qrun --share ~/projet --console /tmp/ser.sock rust_example/run_tests.sh
```

Each example is a minimal, complete project whose `run_tests.sh` builds and
runs on the PC and is executed from the share in the guest: no copy, no image
rebuild.

## Conformance bench (7.2 to 7.4)

```sh
fsproof diff --local --seed 1 --count 200        # differential test, on the PC
fsproof diff --seed 1 --count 2000 \
    --guest-cmd "qrun --share ~/projet --console /tmp/ser.sock python3" \
    --regressions regressions/                   # against the client in the guest
fsproof exercise --ops 10000 --workers 16        # content exerciser
fsproof exercise --ops 10000 --workers 16 --shared
fsproof cases                                    # the directed table of 7.4
fsproof cases --slow                             # + the 100 000-entry directory
fsproof clients                                  # what --client accepts
```

`diff` generates a seeded sequence, has it replayed by the client under test
and by the PC reference, snapshots both trees and compares names, types,
sizes, permissions, dates, symlink targets and a SHA-256 of every content.
A divergence is reduced to the shortest sequence that still reproduces it
(bisection) and stored in `regressions/`, which is the non-regression corpus.
A stored case is replayed with `fsproof diff --replay regressions/<file>`.

The same bench drives another implementation, which is what makes the
comparison objective (PERF-07):

```sh
fsproof --client qnx-fs-9p diff --guest-cmd "..."   # project A
fsproof --client eclipse   diff --guest-cmd "..."   # project B
```

`exercise` writes, reads, truncates and extends files while keeping the
expected bytes in memory and checking every read: alone, with N workers on
distinct files, and with N workers on one shared file. `cases` runs the
directed table: rename over an existing file, unlink of an open file,
concurrent exclusive creation, permission matrix, symlink loop, 255-byte
name, 5 GiB sparse file, maximum-length path, hard-link identity, and the
100 000-entry directory behind `--slow`.

## Measurements (7.5, 7.6, PERF-01..PERF-07)

```sh
# roundtrips per POSIX call, from a QEMU 9P trace
qshare ~/projet --run ~/qnxvm/output/ifs.bin --trace /tmp/9p.log
pbench roundtrips /tmp/9p.log --markdown roundtrips.md --json roundtrips.json

# the six workloads of 7.5, plus the conformance figures, in one file
pbench all --client passerelle --json runs/passerelle.json --conformance

# the comparative report: Markdown + JSON + SVG, thresholds of 7.6 checked
pbench report --run runs/passerelle.json --run runs/qnx-fs-9p.json \
              --run runs/eclipse.json --level socle -o bench_report
```

`pbench all` writes one JSON file per target; `pbench report` aggregates
them into `bench_report.md`, `bench_report.json` and `bench_report.svg`
(a hand-written SVG, so matplotlib is never required — if it happens to be
installed a PNG is written too). Each threshold of 7.6 gets a PASS/FAIL row
and the command exits non-zero when one fails.

### Capturing a real 9P trace

Start QEMU with the 9P trace backend enabled:

```sh
qemu-system-x86_64 -m 1G -enable-kvm \
  -kernel <ifs.bin> -fsdev local,id=host,path=$HOME/projet,security_model=mapped-xattr \
  -device virtio-9p-pci,fsdev=host,mount_tag=hostshare \
  -trace 'enable=v9fs_*' -trace file=/tmp/9p.log
```

`qshare --run <image> --trace /tmp/9p.log` does the same and starts
`virtiofsd` when needed. QEMU 8.2 names the 9P events `v9fs_*` (not
`virtio_9p_*`), and it traces **no timestamp** by default: the roundtrip
table then reports `n/a` for the latencies instead of a fake 0.000 ms, and
says so. The events `clunk`, `remove`, `fsync`, `link`, `flush`, `wstat`,
`xattrcreate` and `rcancel` have no `*_return` counterpart, so a request of
that kind is never counted as unanswered.

A real trace is included in the tests (`tests/fixtures/qemu-9p-real-trace.log`),
captured on this host from a Linux guest mounted on a share.

Workloads: `throughput` (sequential read/write), `metadata` (10 000 files
created, stat'ed, removed), `tree` (50 000-entry walk), `mixed` (source-like
tree then a text search), `exec` (1 000 launches from the share) and
`priority` (a 4 KiB read during a bulk copy, PERF-05). `--scale 0.01` runs
a fast rehearsal of all of them.

`pbench mount --console /tmp/ser.sock` measures the guest mount time
(PERF-06, under 50 ms).

## Tests

```sh
cd host
python3 -m pytest tests -q          # 133 passed, 2 skipped, under 10 s
python3 -m compileall -q qshare fsproof bench
python3 ../tools/check-size.py --root .
```

The suite is hermetic and fast: no network, no QEMU, no QNX image. Tests
that would need one are marked `skipif` with an explicit reason, never
silently skipped.

## Layout

```
host/
├── pgwutil/     shared helpers: errors, atomic writes, JSON, SVG charts
├── qshare/      options.py (QEMU arguments), image.py (mkqnximage),
│                runner.py (virtiofsd/QEMU), qrun.py, examples.py, demo.py
├── fsproof/     model.py, gen.py (seeded sequences), replay.py,
│                guest_agent.py (guest side), diff.py (compare + reduce),
│                exercise.py, cases.py, clients.py
├── bench/       trace.py (QEMU 9P trace), roundtrips.py, loads.py,
│                conformance.py, mount.py, report.py
└── tests/       pytest suite + two 9P trace fixtures (synthetic and real)
```
