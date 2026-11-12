# CI helpers for the QNX 7.1 guest

Scripts used by `.github/workflows/qnx71-guest.yml` (out of pull requests) to
reproduce in CI what [`../EVIDENCE.md`](../EVIDENCE.md) recorded by hand: boot
the real QNX 7.1.0 guest, ask QNX what it sees of our `virtio-9p-pci` device,
keep the answers. Rationale, measurements and the interrupt fragment are
documented in [`docs/qnx71-guest.md`](../../../../docs/qnx71-guest.md).

Nothing here modifies the guest scripts of the parent directory: `qnx-vm.sh` and
`qnx-ssh` are called as they are.

| File | What it does |
|---|---|
| `fetch_image.sh` | Makes `$QNX_VM_DIR/qnx71-fs.img` exist **and match the pin**: pulls the container by digest (from `tools/qnx-image/qnx71-image.manifest.json`), checks that the pulled image carries that digest, extracts `/home/popcornsar/fs_3GB.img` and verifies its size and SHA256 before use. Idempotent: a disk already there is verified and reused, never downloaded again. |
| `wait_ssh.py` | Bounded poll of the four forwarded ports until one answers with an SSH banner; prints `SSH_PORT=<n>` and can write it to a file. Returns early when QEMU dies. |
| `guest_probe.sh` | Runs the guest commands (`uname`, `pidin`, `pci-tool`, `/proc/boot/pci_hw.cfg`, `/proc/boot/pci_server.cfg`), captures the host-side `info pci` **and the kernel-side trace the guest can give** (the tracing tools it ships, the system log, the VGA console), writes `summary.txt`, and asserts the device is there. |
| `qemu_monitor.py` | Sends one HMP command to the QEMU monitor and prints the cleaned answer. Reuses the connection helper of `../qnx_console.py`. |
| `test_ci_helpers.py` | Unit tests of the parts that need no guest: port-list parsing, monitor output cleaning, pid-file liveness. |

## Exit-status contract

The three entry points share one convention, so the workflow can tell an
environment problem from a real failure — and never print a green that means
nothing:

| Code | Meaning | What the workflow does |
|---|---|---|
| `0` | Done, and the device was seen | green |
| `2` | Prerequisite missing (no docker, unreachable daemon, failed pull, not enough disk, no `qnx-ssh`) | `::notice::` + green, artifact empty |
| `1` / other | The check itself failed (truncated image, no SSH banner, `1af4/1009` absent) | `::error::` + red |

Every network, disk and guest command is wrapped in `timeout`; the ceilings come
from the workflow's environment (`QNX_PULL_TIMEOUT`, `QNX_COPY_TIMEOUT`,
`QNX_GUEST_TIMEOUT`, `QNX_REQUIRED_KB`) and each has a default that works on a
workstation too.

## Running it by hand

```bash
cd passerelle
export QNX_VM_DIR=/tmp/qnx71-vm
tests/qemu/qnx71/ci/fetch_image.sh                     # 0 = ready, 2 = skip
tests/qemu/qnx71/qnx-vm.sh start                       # add -accel tcg to mimic a hosted runner
python3 tests/qemu/qnx71/ci/wait_ssh.py --vm-dir "$QNX_VM_DIR" --timeout 540 \
    --port-file /tmp/qnx-port
tests/qemu/qnx71/ci/guest_probe.sh --out /tmp/qnx71-probe \
    --vm-dir "$QNX_VM_DIR" --port "$(cat /tmp/qnx-port)"
tests/qemu/qnx71/qnx-vm.sh stop
cat /tmp/qnx71-probe/summary.txt
```

`fetch_image.sh` can be exercised without the 6 GB download by pointing it at
a manifest of your own (`QNX_IMAGE_MANIFEST`) — the disk it finds on disk is
always verified against the pin, so a file that is not the pinned one is
refused (exit 1), and a missing docker or a full disk is a skip (exit 2):

```bash
QNX_VM_DIR=/tmp/mock QNX_REQUIRED_KB=1000 QNX_MIN_IMAGE_BYTES=1000 \
  tests/qemu/qnx71/ci/fetch_image.sh
```

The checksum logic itself is unit-tested on small synthetic files, with no
docker, no network and no gigabytes:

```bash
python3 -m pytest host/tests/test_image_manifest.py -q
```

Unit tests (no guest, no docker, no network):

```bash
python3 -m pytest tests/qemu/qnx71/ci -q
```

## Note for the CI budget guard

`.github/ci-budget/check_timeouts.py` enforces "5 minutes per job" **by
filename** (`SKIP_FILES`), and `qnx71-guest.yml` never runs on a pull request, so
it belongs in that set next to `qemu-e2e.yml`:

```python
SKIP_FILES = {"qemu-e2e.yml", "qnx71-guest.yml"}  # jobs longs hors PR (justifies dans le YAML)
```

That file is the vendored CI socle, outside this directory: the one-line change
is for whoever integrates the workflow. Without it, the `ci-budget` check
reports `job 'qnx71-guest' timeout-minutes: 35 > 5`.
