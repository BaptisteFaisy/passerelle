# Passerelle guest image

This directory holds the pieces of the QNX 8.0 guest image that the project
owns: the build file fragment, the mount script, and the security policy.

## What is here

| File | Role | Requirement |
|---|---|---|
| `passerelle.build` | IFS build file fragment: kernel, C library, PCI server, the two drivers, the mount script | QNX-02, DX-01, DX-06 |
| `qshare-mount.sh` | runs in the guest: waits for the device, mounts the share on `/host`, logs the result | QNX-07, QNX-08, VIO-05 |
| `passerelle.sec` | QNX 8.0 security policy: the minimum abilities of each driver | QNX-06, SEC-01, SEC-02 |

## Regenerating a complete image

The fragment in `passerelle.build` is *not* a complete build file: it is meant
to be included by the build file of the image the guest boots from, so that the
project's files can be added to an image that already exists (a reference
image, the one `qshare` prepares, or your own).

The full sequence, with the SDP 8.0 installed:

```sh
# 1. Build the drivers for the target (the toolchain file selects qcc).
cd /path/to/passerelle
cmake -B build-qnx \
      -DCMAKE_TOOLCHAIN_FILE=cmake/qnx-aarch64.cmake \
      -DPGW_MODULES="common;virtio;proto9p;protofuse;rmfs;drivers" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-qnx -j8

# 2. Stage the binaries where the build file expects them.
mkdir -p stage/usr/bin
cp build-qnx/drivers/fs9p  stage/usr/bin/fs9p
cp build-qnx/drivers/fsfuse stage/usr/bin/fsfuse
cp image/qshare-mount.sh  stage/usr/bin/qshare-mount.sh

# 3. Assemble the image: your build file includes the fragment.
cp image/passerelle.build build-file.build
cat >> build-file.build <<'EOF'
[perms=0755] /usr/bin/fs9p=fs9p
[perms=0755] /usr/bin/fsfuse=fsfuse
[perms=0755] /usr/bin/qshare-mount.sh=qshare-mount.sh
EOF

mkqnximage -v -i build-file.build \
            -o stage/usr/bin/fs9p \
            -o stage/usr/bin/fsfuse \
            -o stage/usr/bin/qshare-mount.sh \
            passerelle.ifs

# 4. Compile and push the security policy (optional but recommended).
secpolcompile -o passerelle.sec.bin image/passerelle.sec
#   on the target:  secpolpush passerelle.sec.bin
```

`mkqnximage` needs the target's boot files: `procnto-smp-instr` and the
`ldqnx-*.so` dynamic loader. They come from the SDP
(`$QNX_TARGET/proc/` and `$QNX_TARGET/lib/`), and the fragment copies them
from the boot image's own directory, so the usual sequence is:

```sh
cp $QNX_TARGET/proc/procnto-smp-instr .
cp $QNX_TARGET/lib/ldqnx-64.so.2 .
```

## Where each piece goes

- The **fragment** (`passerelle.build`) is included by the build file of the
  image. It declares the boot section, the start-up script and the libraries
  and binaries the guest needs. Everything it declares exists in a reference
  image, so it can be pasted into one.
- The **mount script** (`qshare-mount.sh`) is installed as
  `/usr/bin/qshare-mount.sh` and started by the start-up script of the image.
  It reads its parameters from the environment (`QSHARE_TAG`,
  `QSHARE_MOUNT`, `QSHARE_OPTS`, `QSHARE_DRIVER`), which is how the same
  image serves a 9P share and a virtiofs one.
- The **policy** (`passerelle.sec`) is compiled with `secpolcompile` and pushed
  with `secpolpush`. It grants each driver type the four abilities it needs
  (`pathspace`, `public_channel`, `interrupt`, `mem_phys`) and lets it attach
  its mount point and nothing else.

## The guest disk of the CI job is pinned, and it is not here

The CI job of [`docs/qnx71-guest.md`](../docs/qnx71-guest.md) boots a real QNX
Neutrino 7.1.0 guest on a hosted runner. That disk is QNX software, so it is
**not** in this repository: what the repository carries is the pin —
[`tools/qnx-image/qnx71-image.manifest.json`](../tools/qnx-image/qnx71-image.manifest.json),
which records the source container **by digest**, the path of the disk inside
it, its size, its SHA256 and the date it was verified. Anyone can fetch
bit-for-bit what CI used, and the job refuses to boot a disk that does not
match the pin:

```bash
python3 tools/qnx-image/image_manifest.py show \
    --manifest tools/qnx-image/qnx71-image.manifest.json --field docker-reference
# -> docker.io/autosarai/qemu_qnx7_1@sha256:07319f04...  (pull that, not the tag)
python3 tools/qnx-image/image_manifest.py verify \
    --manifest tools/qnx-image/qnx71-image.manifest.json --file ~/qnx71-vm/qnx71-fs.img
```

The files of *this* directory (`passerelle.build`, `passerelle.sec`,
`pci_hw.cfg`, `qshare-mount.sh`) are what the project owns; the pin is how the
disk the project does not own is still reproducible.

## Notes on the two requirements this directory serves

- **DX-01**: `qshare` on the PC prints the QEMU options and the fragment that
  embeds the driver and mounts the share at boot. The fragment is
  `passerelle.build`; `qshare` only has to embed it.
- **DX-06**: the image contains only the kernel, the C library, the PCI server
  and the driver. The guest's working directory is the shared folder, so the
  image itself stays minimal. The fragment is deliberately small: every binary
  it names is there because the driver cannot work without it.

## The physical memory range

`passerelle.sec` grants `mem_phys` over `0x0-0x10000000`, which covers the
virtio device BARs on the machines this project targets. On a machine with a
different memory layout, use a **named range** instead: the range file
(`secpolgenerate -r`, or a hand-written one) gives the actual address of the
device at boot, and the policy refers to it by name. This is the documented
way of keeping the ability bound to the hardware rather than to a guess
(SEC-02).
