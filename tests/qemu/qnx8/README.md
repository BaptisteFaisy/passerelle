<!-- SPDX-License-Identifier: Apache-2.0 -->
# QNX 8.0 guest — booting and enumerating the virtio-9p device

This directory lays the ground for running the Passerelle drivers (`fs9p`,
`fsfuse`) inside a real QNX 8.0 guest under QEMU. What is proven here, on this
PC (QEMU 8.2.2, the SDP 8.0 at `~/qnx800`, `/dev/kvm` usable):

1. a **QNX 8.0 guest that boots** and **enumerates the virtio-9p-pci device**
   (`vid/did 1af4/1009`);
2. the **exact QEMU command line** that maps the host share onto that device;
3. the **interrupt line** QEMU really wires to the device, which matches
   `image/pci_hw.cfg` already.

`boot.log` is the serial console of that boot, captured and sanitized (control
characters stripped). It shows the kernel banner, the PCI server starting, and
`pci-tool -v` listing the device.

## The QEMU command line

```sh
qemu-system-x86_64 \
  -machine pc -cpu host -smp 1 -m 512 -enable-kvm \
  -kernel <ifs_image> \
  -fsdev "local,id=host,path=<host_share_dir>,security_model=mapped-xattr" \
  -device "virtio-9p-pci,fsdev=host,mount_tag=passerelle,addr=0x05" \
  -display none -serial file:<console.log> -monitor none -no-reboot
```

* `-machine pc` (i440fx) — the machine the SDP 8.0 boots its own QEMU guest on
  (see `host/common/mkqnximage/qemu/runimage`). No `/dev/kvm` → replace
  `-enable-kvm -cpu host` with `-accel tcg,thread=multi -cpu max`.
* `addr=0x05` pins the device to PCI slot 5, so its bus/device/function and its
  interrupt line are stable (below). Without it, the slot and IRQ shift with the
  other `-device` lines.
* `security_model=mapped-xattr` needs no root (SEC-03, `host/qshare/options.py`).
* `mount_tag` ≤ 31 bytes for 9P (QEMU-enforced).

`run-guest.sh` wraps this line and captures the console to a file; `console.py`
drives it back through a `-serial unix:` socket when interaction is needed.

## The virtio-9p device as QNX sees it

`pci-tool -v` (in `boot.log` in this directory) reports:

```
B000:D05:F00 @ idx 6
	vid/did: 1af4/1009
	class/subclass/reg: 00/02/00
		<Classcode is undefined>
```

`B000:D05:F00` is the device QEMU places on `addr=0x05`; `1af4/1009` is the
virtio PCI vendor/device id, the same one `image/pci_hw.cfg` names.

## The interrupt line: measured 10, not a guess

`info pci` in the QEMU monitor (machine running, SeaBIOS IRQ routing applied)
reports for the device:

```
Bus  0, device   5, function 0:
    Class 0002: PCI device 1af4:1009
    IRQ 10, pin A
```

So QEMU wires **IRQ 10, pin A** to `B0:D5:F0`. That is exactly the entry the
project already carries:

```
# image/pci_hw.cfg
[interrupts]
B0:D5:F0    A    10
```

**No correction was needed** — the existing `image/pci_hw.cfg` is right once
the device is pinned with `addr=0x05`. The same entry was added to the
`pci_hw.cfg` block that `image/passerelle.build` embeds.

## Building the guest image

`image/passerelle.build` is a fragment. `mkifs` (the tool `mkqnximage` itself
calls to build the IFS) builds a complete image from the fragment plus a search
path that resolves the SDP 8.0 layout (`$QNX_TARGET/x86_64/...`):

```sh
./build-image.sh          # -> build/qnx8/passerelle.ifs
```

`build-image.sh` sources `~/qnx800/qnxsdp-env.sh` and runs:

```sh
mkifs -v -v tests/qemu/qnx8/passerelle.build build/qnx8/passerelle.ifs
```

`tests/qemu/qnx8/passerelle.build` sets the search path and includes the
fragment (`[+include] image/passerelle.build`). The fragment's driver lines are
`[+optional]`, so the image builds **before** `fs9p`/`fsfuse` exist and still
carries the kernel, the C library, the PCI server and the tools.

## Status and the remaining step

Proven, captured in this directory:

* the QEMU command line and the device/interrupt mapping (above);

Open, and handed to the next step:

* booting the **minimal IFS built from `image/passerelle.build`**: the kernel
  boots and the start-up script runs (the `Passerelle: …` `display_msg` lines
  appear on the console), but every program start fails with
  `Unable to start "…" (2)` (ENOENT — the dynamic interpreter
  `/usr/lib/ldqnx-64.so.2` is not found). `boot-ifs.log` was not kept because
  it is trivially reproduced with `build-image.sh` + `run-guest.sh`.

  The SDP 8.0 reference guest (probed in `boot.log`) shows the layout that
  works: the IFS of a `-kernel` guest is at `/proc/boot` and its files are
  **flat** there (`pci-server`, `libc.so.6`, `ldqnx-64.so.2`, the `pci_*.so`
  modules all directly under `/proc/boot`), with `PATH=/proc/boot`,
  `LD_LIBRARY_PATH=/proc/boot` and a single
  `procmgr_symlink /proc/boot/ldqnx-64.so.2 /usr/lib/ldqnx-64.so.2` for the
  interpreter. `image/passerelle.build` already follows that layout; the exact
  mkifs mount/prefix combination that reproduces the reference image on a
  `-kernel`-only guest (without the SDP disk) is what still needs to be nailed
  down.

## Recipe for the next step (drop-in the drivers)

```sh
# 1. build the drivers (qcc target, see image/README.md for the cmake line)
cmake --build build-qnx -j8                # -> build-qnx/drivers/{fs9p,fsfuse}
# 2. stage them where the fragment expects them (build/qnx8/stage)
./tests/qemu/qnx8/build-image.sh           # now embeds fs9p and fsfuse
# 3. boot with the share
TAG=passerelle ./tests/qemu/qnx8/run-guest.sh
# 4. inside the guest, the mount is run by qshare-mount.sh at boot:
#      fs9p -o ro,nosuid,noexec passerelle /host
#    (QSHARE_TAG / QSHARE_MOUNT / QSHARE_OPTS / QSHARE_DRIVER select the case)
```