# Chapter 1 — The environment and your first image

**Goal.** Get a QNX 8.0 guest booting under QEMU with a virtio-9p device
attached, and check the handful of facts the rest of this tutorial depends on.

**Time.** 30 minutes.

## 1.1 What the pieces are

Three programs are involved, and it is worth being precise about which is
which, because most of the confusion in this area comes from mixing them up.

```
  your PC                        the hypervisor                the guest
  ───────                        ──────────────                ────────
  a directory          ──┐
                       ├──►  qemu-system-x86_64  ────  a disk image  ──►  QNX 8.0
  the 9P server (QEMU) ─┘        (the device)         (the kernel)      (your driver)
```

- **QEMU** is the hypervisor, and it is also the 9P *server*: the directory
  you point it at is served to the guest over a virtio device. There is no
  separate daemon to install on the PC, unlike virtiofs.
- **The disk image** holds the guest: a kernel, a small filesystem, and the
  drivers that are started at boot. `mkqnximage` builds it.
- **Your driver** runs inside the guest, as a program or as a resource
  manager. It is the thing you write.

The device QEMU presents is called *virtio-9p*. It appears on the PCI bus in
two flavours, and which one you get depends on how you ask for it:

| How you ask QEMU | What it creates | Where it appears |
|---|---|---|
| `-device virtio-9p-device,...` | a virtio-mmio device | a fixed MMIO window (ARM64 `virt` machines) |
| `-device virtio-9p-pci,...` | a virtio-pci device | the PCI bus |
| `-virtfs local,...` | shorthand for the second | the PCI bus |

For x86_64, which is what this tutorial uses, it is the PCI bus. Chapter 2 is
about that bus.

## 1.2 The identities you will be looking for

The driver does not find "the 9P device" by name; it looks for a vendor and a
device id on the PCI bus.

| | Value | Where it comes from |
|---|---|---|
| PCI vendor | `0x1AF4` | the virtio vendor id, assigned by the PCI-SIG |
| Device id, transitional | `0x1009` | `0x1000 + 9`: 9 is the virtio device type for 9P |
| Device id, modern only | `0x1049` | `0x1040 + 9` |
| virtio device type 9P | `9` | virtio 1.2, §5.3 |
| virtio device type virtiofs | `26` | virtio 1.2, §5.4 |

The `0x1000 + type` and `0x1040 + type` formulas are worth remembering: the
"modern only" interface numbers the device from `0x1040` upwards, and QEMU
emulates the transitional interface (`0x1000` upwards) for guests that predate
virtio 1.0. A driver that only knows `0x1049` will not find a transitional
device, and vice versa. Most drivers accept both.

The exercise checks these numbers and the byte order, on the machine you are
about to use:

```sh
cmake -B /tmp/codelab-build -S codelab/code
cmake --build /tmp/codelab-build
/tmp/codelab-build/ex01_env
```

```
chapter 1 - environment check
virtio 9P:  vendor 0x1AF4  pci id 0x1009 (transitional) 0x1049 (modern)
virtio-fs:  vendor 0x1AF4  pci id 0x105A (modern)
types: u8=1 u16=2 u32=4 u64=8 ptr=8
byte order: little-endian
```

If the byte order line says *big-endian*, stop: this tutorial, and the virtio
specification's wire format, assume little-endian. Neither QNX 8.0 target is
big-endian, so this should never happen — but a driver that finds out the hard
way spends a day.

## 1.3 Building the first image

`mkqnximage` is the tool that turns a description of a guest into a disk
image. The simplest useful invocation asks for a 64-bit x86 guest with
network and serial console:

```sh
mkqnximage --type=vm --arch=x86_64 --memory=1024 \
           --shuffle=no --no-build \
           --kernelfile=$SDP/../target/qnx/qnx-kernel.so \
           --boot-image=$SDP/../target/qnx/boot/qnx-ifs.ifs \
           --script=/dev/null --filesystem=/dev/null \
           --image=myimage.qnx --elf=myimage.elf
```

The exact arguments vary between SDP versions and are documented in the
`mkqnximage` page of the QNX 8.0 documentation; the ones that matter for this
tutorial are:

- `--arch=x86_64` — the architecture of the guest, and therefore of the
  driver you will cross-compile.
- `--boot-image` — the filesystem image the guest boots from.
- `--image` — the disk image QEMU will boot.

The two things you will add to the image as the tutorial progresses are the
**driver binary** and a **startup script** that runs it. The mechanism is a
build file: a text file listing the binaries to add, one per line, with
`[+script]` sections for the startup commands.

```
[+script]
procmgr_symlink::/dev/ser1:/dev/con1 &
[+raw]mytest
```

A more convenient approach, and the one this tutorial recommends for the
first four chapters, is to avoid touching the image at all: build the driver
for the guest, run it from the serial console by hand, and only bake it into
the image once it works. You can copy a binary into a running guest over the
serial line, or simply rebuild the image; the loop is fast either way, and
much faster than it sounds while the driver does not yet work.

### Cross-compiling

The driver is a normal C program with two extras: it is built for the guest's
architecture, and it links against the QNX libraries.

```sh
source ~/qnx800/qnxsdp-env.sh
qcc -Vgcc_ntox86_64 -o mytest mytest.c
```

`qnxsdp-env.sh` sets `QNX_HOST` and `QNX_TARGET`, which the compiler wrapper
needs. `-Vgcc_ntox86_64` selects the target; `gcc_ntoaarch64le` is the ARM64
one. Everything else — warnings, optimisation — is the compiler's usual
business.

The QNX-specific headers your driver will need are:

| Header | What it gives |
|---|---|
| `<sys/neutrino.h>` | `InterruptAttachThread()`, `InterruptWait()`, `InterruptDetach()` |
| `<sys/mman.h>` | `mmap_device_memory()`, `mmap()` with `MAP_PHYS`, `mem_offset64()` |
| `<sys/dispatch.h>` | `name_attach()`, the "who owns this device" trick |
| `<sys/rsrcdbmgr.h>` | `rsrcdbmgr_attach()`, used to claim an MSI IRQ |
| `<sys/iofunc.h>`, `<sys/resmgr.h>` | the resource manager, chapter 8 |

## 1.4 Running it

```sh
qemu-system-x86_64 -M pc -m 1024 -smp 2 \
    -kernel myimage.elf -initrd myimage.qnx \
    -serial mon:stdio \
    -fsdev local,id=fs0,path=$HOME/share,security_model=mapped-xattr \
    -device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare
```

Three options deserve a comment:

- **`-serial mon:stdio`** puts the guest's serial console on your terminal,
  with the QEMU monitor behind `Ctrl-a c`. This is where the driver's output
  goes.
- **`-fsdev local,...`** defines the share: a directory on the PC, and how
  ownership is recorded. `mapped-xattr` stores guest ownership in extended
  attributes on the host files, which is the model to use unless you have a
  reason not to.
- **`mount_tag=hostshare`** is the name the guest uses to ask for this
  particular share. If QEMU has two shares, they have two different tags, and
  the guest mounts them one at a time.

The guest should now boot to a prompt. Log in (the image's default account is
usually `root`, with no password on a development image) and check that the
device is visible at all — QNX's own PCI tools will tell you:

```
pidin pci
```

Look for `1af4` as the vendor and `1009` or `1049` as the device id. If it is
not there, the problem is on the QEMU command line, not in your driver: check
that the `-device` option is on the same command line as the `-fsdev` it
refers to, and that the machine type supports PCI devices (`-M pc` does).

## 1.5 What to observe

- `ex01_env` prints the identities above, on the PC.
- `pidin pci` in the guest lists the virtio-9p device.
- The guest boots to a prompt and `pidin` works.

## 1.6 Traps

- **Wrong architecture.** A driver built for aarch64le on an x86_64 guest
  fails in a way that looks like a bug in the driver: it cannot even print
  anything. Check `file mytest` and the `-V` flag you compiled with.
- **`MAP_PHYS` needs a physical address.** `mmap()` with `MAP_PHYS|MAP_ANON`
  and `mem_offset64()` is the way to get one; chapter 5 comes back to this.
- **The tag is not optional.** Without `mount_tag=`, QEMU refuses to start the
  device. The guest cannot find a share it cannot name.
- **`security_model=none` is tempting and wrong.** It makes every file in the
  guest belong to the user running QEMU, and `chown` fails. Use
  `mapped-xattr`.

## Exercise

1. Build and run `ex01_env` on the PC. Write down the vendor and device ids
   it prints.
2. Start QEMU with the command line above, boot the guest, and run `pidin
   pci`. Find the virtio device and note its BDF (bus:device.function), its
   vendor and its device id.
3. Change `security_model` to `none`, restart QEMU, and run `pidin pci` again.
   The device is still there; note that the *host* behaviour of the share
   changes, not the guest's view of the bus.
4. Add a second share with a different `mount_tag`, and find both devices in
   `pidin pci`. They have the same vendor and device id, and only the tag
   tells them apart. That is the whole reason requirement VIO-05 of this
   project's specification asks for the tag to be read.

## Solution

The exercise program is `code/src/ex01_env.c`; its checks are in
`code/tests/test_clab.c`.

For steps 2 to 4, the expected output is a `pidin pci` table with one row per
device. Two shares means two rows with the same vendor and device id, at
different BDFs. Nothing in the PCI configuration space distinguishes them —
the tag lives in the device-specific configuration space, which is what
chapter 3 is about.

## Where we are

We have a guest that boots and a device on its PCI bus. Chapter 2 is about
how a QNX program talks to that bus.