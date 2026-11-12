# The QNX 7.1 guest in CI, and the interrupt fragment it needs

Two things live together here, because the second was measured with the first:

1. **`.github/workflows/qnx71-guest.yml`** — a CI job that boots the real QNX
   Neutrino 7.1.0 guest of [`tests/qemu/qnx71/`](../tests/qemu/qnx71/) and asks
   *QNX itself* what it sees of our `virtio-9p-pci` device. It is the automated
   form of the manual session transcribed in
   [`tests/qemu/qnx71/EVIDENCE.md`](../tests/qemu/qnx71/EVIDENCE.md).
2. **[`image/pci_hw.cfg`](../image/pci_hw.cfg)** — the interrupt-routing
   fragment for that device, in the exact format the QNX 7.1 image uses, with
   the values measured on the host side of the same bus.

Everything below was measured on 11 November 2026 on `baptiste-MS-7D18`
(QEMU 8.2.2, guest image built 2022-07-12) and is reproducible with the
commands of §4.

---

## 1. What the job is

| | |
|---|---|
| Workflow | `qnx71-guest`, out of pull requests (`push` on `main`, `workflow_dispatch`) |
| Runner | `ubuntu-24.04` (hosted): the self-hosted `passerelle-worker` machines have no QEMU, see [`docs/ci.md`](ci.md) |
| Job ceiling | `timeout-minutes: 35`, sum of the per-command ceilings (~33 min); a real run measures ~8 min, and ~1 min once the disk is already on the runner |
| Guest | QNX Neutrino 7.1.0 x86_64, from the community container `autosarai/qemu_qnx7_1:1.1.0` (11 layers, 622 MB compressed as Docker Hub reports it; the disk inside is `/home/popcornsar/fs_3GB.img`, 6 500 959 232 bytes) |
| Artifact | `qnx71-guest-probe`: text only (14 days). **The disk image is never uploaded** — it is QNX software under non-commercial terms |

Steps, in order, each with its own `timeout`:

| Step | Command | Ceiling | On failure |
|---|---|---|---|
| Install QEMU + ssh | `apt-get install qemu-system-x86 openssh-client` | 90 s + 180 s | **skip** (`::notice::`, green) |
| Fetch the disk | `tests/qemu/qnx71/ci/fetch_image.sh` | 660 s | **skip** if docker/network/space is missing, **red** if the extraction is truncated |
| Boot | `tests/qemu/qnx71/qnx-vm.sh start` | 90 s | red |
| Wait for SSH | `tests/qemu/qnx71/ci/wait_ssh.py --timeout 540` | 600 s | red, with `qemu.out` and `serial.log` in the log |
| Ask the guest | `tests/qemu/qnx71/ci/guest_probe.sh` | 420 s | **red when `1af4/1009` is absent** |
| Stop, upload | `qnx-vm.sh stop`, `actions/upload-artifact@v4` | 30 s | always runs |

The wait for SSH is a bounded poll of the four forwarded ports (2222–2225, the
image has been seen on four different DHCP addresses), every 5 s, with a ceiling
on the whole wait — never a fixed `sleep` longer than 30 s, per the CI budget
rule. Measured banner arrival: **19 s** under TCG, **30 s** under TCG on a busy
machine, **60 s** under KVM, **110 s** under KVM with the disk in snapshot mode.

## 2. What it proves, and what it does not

**It proves, on every run, on a real QNX kernel:**

- **VIO-01 (discovery)** — QNX's own PCI server enumerates the device:
  `pci-tool -V 0x1af4 -D 0x1009` exits 0 and prints `vid/did: 1af4/1009` at
  `B000:D05:F00`. The job fails if that line disappears, so a regression in the
  device model (a wrong vendor/device id, a capability layout QNX rejects, a
  QEMU option string that no longer exposes the device) turns the build red.
- **VIO-02 / VIO-04 (capabilities and address spaces)** — the artifact keeps the
  capability list (MSI-X `0x11` then the five virtio vendor capabilities `0x09`)
  and the three assigned address spaces (I/O `0xc000`, MEM `0xfeb96000`, 64-bit
  prefetchable `0xfe000000`).
- **The host side of the interrupt** — `info pci` from the QEMU monitor, captured
  in the same artifact: `IRQ 10, pin A`. That is the value
  `image/pci_hw.cfg` records, and the job prints both next to each other and says
  `DRIFT:` when they disagree, because the IRQ follows the QEMU command line, not
  the driver (measured: a bare machine with only our device gets `IRQ 11`).

**It does not prove:**

- **Nothing about the driver.** The image has no toolchain at all — no `qcc`, no
  `mkifs`, no `mkqnximage` — so nothing is compiled, loaded or run in the guest.
  E1's second half (obtain the interrupt, exchange a first 9P message) still
  needs the QNX SDP; see [`docs/verification-status.md`](verification-status.md) §3.
- **Nothing about QNX 8.0.** The guest is 7.1.0. It uses the same `pci-server` +
  `libpci` framework the driver targets, but the 8.0 kernel changes (capability
  model, no `InterruptAttach()`) are not exercised.
- **No 9P traffic.** There is no client in the image to mount the share with —
  which is the gap this project fills, and the reason the job stops at "the
  device is visible to QNX".
- **No interrupt delivery.** See §5: the fragment states the wiring, it does not
  make this image deliver an INTx to our device.

## 3. Skip versus fail — never a false green

The job is green in exactly two situations: it looked and the device was there,
or it says out loud that it could not look. Every skip prints a `::notice::`
with the reason (docker missing, daemon unreachable, pull failed, not enough
disk, QEMU not installable) and the artifact simply has no files
(`if-no-files-found: ignore`). A guest that boots and does not report
`1af4/1009` is **red**, and so is a guest that never answers on SSH — those two
are the test, not the environment.

## 4. Reproducing it by hand

On a workstation that already has the disk (see
[`tests/qemu/qnx71/README.md`](../tests/qemu/qnx71/README.md) for the one-off
extraction), the whole job is four commands:

```bash
cd passerelle
export QNX_VM_DIR=~/qnx71-vm                 # any working directory
tests/qemu/qnx71/ci/fetch_image.sh           # downloads only if the disk is absent
tests/qemu/qnx71/qnx-vm.sh start             # add "-accel tcg" to mimic a hosted runner
python3 tests/qemu/qnx71/ci/wait_ssh.py --vm-dir "$QNX_VM_DIR" --timeout 540 \
    --port-file /tmp/qnx-port
tests/qemu/qnx71/ci/guest_probe.sh --out /tmp/qnx71-probe \
    --vm-dir "$QNX_VM_DIR" --port "$(cat /tmp/qnx-port)"
tests/qemu/qnx71/qnx-vm.sh stop
cat /tmp/qnx71-probe/summary.txt             # what the artifact contains
```

A local rehearsal of that exact sequence (disk pre-seeded, `-accel tcg`) took
**70 s**: banner at 49 s, the seven guest commands and the monitor query all
answered on the first attempt, ten text files in the output directory. One readiness quirk is handled
by `guest_probe.sh`: the SSH banner shows up a few seconds before `sshd` accepts
a session, so the very first command can come back with ssh's own exit 255
(measured once); each command is therefore retried up to three times, 5 s apart,
when — and only when — ssh itself fails.

## 5. `image/pci_hw.cfg` — the interrupt fragment

### 5.1 What the file is

`pci-server` does not guess interrupt routing on x86: it loads a hardware module
(`PCI_HW_MODULE=pci_hw-Intel_x86.so`) which reads a configuration file named by
`PCI_HW_CONFIG_FILE`. In the community image that is `/proc/boot/pci_hw.cfg`, and
its whole content is:

```
[interrupts]
B0:D17:F0    A    18
B0:D17:F0    B    19
B0:D17:F0    C    16
B0:D17:F0    D    17
B3:D0:F0     A    18
B3:D0:F0     B    19
B3:D0:F0     C    16
B3:D0:F0     D    17
```

(9 lines, 181 bytes, read with `od -c` in the guest: `<BDF>` + 4 spaces + pin +
4 spaces + IRQ, columns aligned; the second record uses 5 spaces because its BDF
is one character shorter, so the separator is plain whitespace.) The module
knows four sections — `interrupts`, `slots`, `aspace`, `rbar` — and logs
`+++ Parsing section [<name>] start +++` for each.

**Our device is in none of those records**, which is the whole finding: it sits
at `B0:D5:F0`, so nothing routes its interrupts, and `pci-tool` reports
`Interrupt list - 0 assigned`. `image/pci_hw.cfg` adds the missing record:

```
[interrupts]
B0:D5:F0    A    10
```

`B0:D5:F0` is where QEMU puts the device on the options `qshare` emits, `A` is
its interrupt pin and `10` the INTx line SeaBIOS wired to it — all three read
back from the same run (QEMU monitor: `Bus 0, device 5, function 0 ... IRQ 10,
pin A`; `pci-tool`: `B000:D05:F00 ... vid/did: 1af4/1009`). The device number is
written `D5`, which is unambiguous: 5 is the same in decimal and in hexadecimal,
unlike the image's `D17`.

### 5.2 Integrating it into a QNX image

The fragment is meant to be **dropped into the image**, not read from a host
directory: `/proc/boot` is the boot image itself, so the file has to be part of
the IFS. With the SDP, one line of the build file does it:

```
[perms=0444] /etc/pci_hw.cfg = pci_hw.cfg
```

and the server configuration points at it:

```
[envars]
PCI_HW_MODULE=pci_hw-Intel_x86.so
PCI_HW_CONFIG_FILE=/etc/pci_hw.cfg
```

Two notes for whoever integrates it:

- [`image/passerelle.build`](../image/passerelle.build) already creates
  `/etc/pci_hw.cfg` and `/etc/pci_server.cfg` with exactly that mechanism — but
  the `[interrupts]` block it inlines is a copy of the community image's, i.e.
  it routes `B0:D17:F0` and `B3:D0:F0` and **not our device**. Replacing that
  inline block with the one-line file reference above makes `image/pci_hw.cfg`
  the single source of truth (the file is outside the scope of this change).
- The image's own `pci_hw.cfg` has no comments; the SDP's template
  (`/etc/system/config/pci/pci_hw-template.cfg`, referenced by the module's own
  usage text, not shipped in this image) is a commented file, and malformed
  records are skipped with a warning rather than aborting the parse. The `#`
  header of `image/pci_hw.cfg` follows that convention — if a stricter parser is
  ever met, keep only the `[interrupts]` section and its records.

### 5.3 What the fragment does not buy on this image (measured)

The hardware module of the image is `pci_hw-Intel_x86.so` **2.4** (2020-06-14,
QNX 7.1.0 package `com.qnx.qnx710.target.connectivity.pci.hw.pci_hw_x86`). Two
of its own strings, read out of the binary, bound what the fragment can do:

```
HW config file IRQ %d for B%u:D%u:F%u is invalid. Must >= 16
%s(): No chipset support found for 0x%x
```

- A config-file IRQ **below 16 is rejected**. Every record the image ships uses
  16–19, consistently.
- QEMU only ever routes PCI INTx to a **legacy GSI 0–15**: measured `IRQ 10`
  for our device on the `qshare` options, and `IRQ 11` on both a bare `pc` and a
  bare `q35` machine. There is no QEMU machine type whose INTx line reaches 16.
- QNX's own numbering *is* the GSI here: in the same guest, the two USB
  controllers report `Interrupt 0 on IRQ 11`, exactly what QEMU wired. So "must
  be >= 16" is a policy of this Intel module (it targets ITSS/PIRQ chipsets —
  Skylake, Denverton, Apollo Lake, Coffeelake — and knows nothing of the PIIX3
  QEMU emulates), not a QNX-wide rule.

Conclusion, and it is the actionable half of E1: **on QNX 7.1 with this module,
the INTx line of a virtio device under QEMU cannot be declared through
`pci_hw.cfg`.** The fragment still has to exist — it is the file the driver's
image reads, it records the wiring the driver must expect, and a module without
that restriction (or an SDP-built image with the right one) honours it as-is.
But the path that works today is **MSI-X**, and the same guest proves QNX
allocates MSI-X vectors: the `vmxnet3` device next to ours reports
`Interrupt list - 3 assigned` on `IRQ 256`, `257`, `258`, and the image ships the
MSI-X capability module `pci_cap-0x11.so`. Our device advertises MSI-X as its
first capability, which is what `drivers/` should attach to, with INTx as the
fallback the fragment describes.

Attempting to verify the parse live is **not** possible in this image: a second
`pci-server` shares `/dev/shmem/pci_hw` with the running one and wedges the guest
(SSH dies, `slog2info` hangs). Measured twice; the guest has to be rebooted
afterwards. The evidence above is therefore read from the module binary and from
the devices the running server already reports, not from a live parse.

## 6. Files of this job

| File | Role |
|---|---|
| `.github/workflows/qnx71-guest.yml` | the job: ceilings, skip/fail contract, artifact |
| `tests/qemu/qnx71/ci/fetch_image.sh` | pull + extract the disk, or skip (exit 2) if the environment is not there |
| `tests/qemu/qnx71/ci/wait_ssh.py` | bounded wait for the SSH banner, reports the answering port |
| `tests/qemu/qnx71/ci/guest_probe.sh` | the questions asked to the guest, the answers saved, the assertion |
| `tests/qemu/qnx71/ci/qemu_monitor.py` | host side: `info pci` from the QEMU monitor (reuses `qnx_console.py`) |
| `image/pci_hw.cfg` | the interrupt fragment itself |
| `tests/qemu/qnx71/` | the guest itself: `qnx-vm.sh`, `qnx-ssh`, `EVIDENCE.md` (unchanged by this job) |
