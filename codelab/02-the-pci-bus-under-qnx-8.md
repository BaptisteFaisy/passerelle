# Chapter 2 — The PCI bus under QNX 8.0

**Goal.** Find a device by vendor and device id, attach to it, and read its
configuration space — with the QNX 8.0 API, and with the two limitations that
shape every driver written against it.

**Time.** 30 minutes.

## 2.1 The API, in four calls

QNX 8.0 reaches the PCI bus through `libpci`, a client library that talks to
the PCI server (`pci-server`, started by the proc manager). The calls a driver
needs are these four.

```c
#include <pci/pci.h>

/* 1. Find a device by identity. Returns a BDF, or PCI_BDF_NONE. */
pci_bdf_t bdf = pci_device_find(0, 0x1AF4, 0x1049, PCI_CCODE_ANY);

/* 2. Attach to it: get a handle that lets us read its configuration space. */
pci_err_t err = PCI_ERR_OK;
pci_devhdl_t hdl = pci_device_attach(bdf, pci_attachFlags_e_OWNER, &err);

/* 3. Read configuration space. */
uint32_t word = 0;
pci_device_cfg_rd32(hdl, 0x00, &word);

/* 4. Give it back. */
pci_device_detach(hdl);
```

Four things about this API are worth stating plainly, because they decide the
shape of everything that follows.

**The BDF is a number, not a pointer.** `pci_bdf_t` encodes
bus:device.function. You pass it around; you never dereference it. The macros
`PCI_BUS()`, `PCI_DEV()` and `PCI_FUNC()` take it apart.

**The handle is what gives access.** `pci_device_find()` only looks at the
device list the PCI server has; configuration space is readable once you hold
a `pci_devhdl_t`. `pci_device_attach()` with `pci_attachFlags_e_OWNER` takes
ownership of the device, so a second driver cannot attach it at the same time.
That is worth having: two drivers driving one virtio device is a debugging
session you do not want to have.

**`pci_device_find()`'s first argument is a device index, not a bus.** A value
of `0` means "the first match"; a non-zero value means "the *n*-th device with
this identity", which is how you reach the second of two identical devices.
The identity arguments are the vendor id, the device id and the class code
(`PCI_CCODE_ANY` to not care).

**Reading configuration space has a floor of `0x40`.** This is the limitation
that surprises people, and it is not in the API documentation in so many
words: on QNX, `pci_device_cfg_rd*()` works from offset `0x40` upwards.
Below that, the client library either fails or returns something
unreliable — in particular, `pci_device_cfg_rd8()` has been observed returning
0 silently at unaligned offsets.

Offset `0x34` is where the *first capability* is, so the textbook way of
walking the capability list is off the table. Chapter 3 deals with that.

## 2.2 The configuration space a driver actually reads

The first 64 bytes are the standard PCI header, and they are laid out the same
way everywhere:

| Offset | Size | Field | What it tells a virtio driver |
|---|---|---|---|
| `0x00` | 2 | vendor id | `0x1AF4` for virtio |
| `0x02` | 2 | device id | `0x1009` or `0x1049` for 9P |
| `0x06` | 2 | status | bit 4: "there is a capability list" |
| `0x08` | 1 | revision | — |
| `0x09` | 1 | class code | — |
| `0x10` | 4 | BAR0..BAR3 | the base addresses of the memory regions |
| `0x2C` | 4 | subsystem vendor | — |
| `0x34` | 1 | capability pointer | the first capability — **not readable on QNX** |

From `0x40` upwards is the *capability* area, where the virtio-specific
structures live. Chapter 3 is entirely about that area.

## 2.3 What a virtio-pci device looks like

A modern virtio PCI device does not put its registers at fixed addresses. It
publishes a set of *vendor-specific capabilities* (capability id `0x09`), each
of which says "here is a structure, in BAR *n*, at offset *o*, *l* bytes
long". The structures are:

| `cfg_type` | Structure | What is in it |
|---|---|---|
| 1 | COMMON | features, device status, per-queue setup |
| 2 | NOTIFY | where to write a queue index, and the multiplier |
| 3 | ISR | one byte: which interrupts are pending |
| 4 | DEVICE | device-specific configuration (the mount tag, for 9P) |
| 5 | PCI | an alternate window into configuration space |

Each capability's own layout is:

```
  byte 0   cap_vndr  = 0x09      vendor-specific
  byte 1   cap_next              the next capability, or 0
  byte 2   cap_len               the length of this capability, in bytes
  byte 3   cfg_type              1..5, as in the table above
  byte 4   bar                   which BAR the structure is in
  bytes 5-7 padding
  dword    offset                the byte offset inside that BAR
  dword    length                how many bytes are available there
  dword    notify_off_multiplier (NOTIFY only, when cap_len >= 20)
```

The multiplier deserves a note: the notification area of a device with several
queues has one word per queue, but the *stride* between them is the device's
choice. The NOTIFY capability's multiplier says what it is. A multiplier of
zero means every queue shares the first word, which is the layout of the older
devices.

## 2.4 The BARs, and why they must be mapped

The capabilities do not contain addresses, only positions inside a BAR. A BAR
(Base Address Register) is a window of device memory that the PCI bus
decodes to a physical address. To reach a virtio structure, the driver must:

1. read the BAR's base address and size;
2. map that physical range;
3. add the capability's offset.

On QNX, step 2 is `mmap_device_memory()`:

```c
void *bar = mmap_device_memory(NULL, size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                               0, (uint64_t)bar_base);
```

`PROT_NOCACHE` matters: these are device registers, not memory, and going
through the cache is a way to read a stale value and lose an afternoon.

Two QNX-specific facts:

- **Mapping device memory needs the `mem_phys` ability.** Without it,
  `mmap_device_memory()` fails with `EPERM`.
- **On a machine without ECAM** (which is most QNX x86 images), libpci reads
  configuration space through the `0xCF8`/`0xCFC` I/O ports *inside the client
  process*, so the driver also needs the `io` ability — and without it, the
  process can crash inside `pci_device_attach()` rather than fail cleanly.

The BARs of a virtio-pci device are memory BARs, not I/O BARs. A device whose
BAR is an I/O BAR belongs to the legacy interface, which this tutorial does not
cover.

## 2.5 A first program: find the device and read its ids

```c
/* SPDX-License-Identifier: Apache-2.0 */
#include <pci/pci.h>
#include <stdio.h>

int main(void)
{
    pci_bdf_t bdf = pci_device_find(0, 0x1AF4, 0x1049, PCI_CCODE_ANY);
    if (bdf == PCI_BDF_NONE) {
        bdf = pci_device_find(0, 0x1AF4, 0x1009, PCI_CCODE_ANY);
        if (bdf == PCI_BDF_NONE) {
            printf("no virtio-9p device\n");
            return 1;
        }
    }
    printf("found at %02x:%02x.%x\n", PCI_BUS(bdf), PCI_DEV(bdf), PCI_FUNC(bdf));

    pci_err_t err = PCI_ERR_OK;
    pci_devhdl_t hdl = pci_device_attach(bdf, pci_attachFlags_e_OWNER, &err);
    if (err != PCI_ERR_OK) {
        printf("attach failed: %s\n", pci_strerror(err));
        return 1;
    }

    uint32_t vendor = 0, device = 0, class_code = 0;
    (void)pci_device_cfg_rd16(hdl, 0x00, (uint16_t *)&vendor);
    (void)pci_device_cfg_rd16(hdl, 0x02, (uint16_t *)&device);
    (void)pci_device_cfg_rd32(hdl, 0x08, &class_code);
    printf("vendor %04x device %04x class %08x\n", vendor, device, class_code);

    pci_device_detach(hdl);
    return 0;
}
```

Note the two `pci_device_find()` calls: the modern-only id first, then the
transitional one. A driver that only looks for one of them finds nothing on
half the machines.

(The `pci_device_cfg_rd16()` above takes a `uint16_t *`; the cast through a
pointer to a `uint32_t` is only to keep the listing short. In real code, use
a `uint16_t` variable.)

## 2.6 What to observe

- The program prints a BDF, then the vendor and device ids.
- The ids match what `pidin pci` said in chapter 1.
- `pci_device_attach()` succeeds without root, if the process has the `pci`
  ability.

## 2.7 Traps

- **Forgetting the transitional id.** QEMU's `-virtfs` creates a transitional
  device (`0x1009`). A driver that looks only for `0x1049` finds nothing and
  reports "no device", which reads like a QEMU problem.
- **Using `pci_device_cfg_rd8()` below `0x40`.** It returns 0 without an
  error on some systems. The capability pointer at `0x34` is exactly the byte
  you want, and exactly the byte you cannot read. Chapter 3's fallback exists
  because of this.
- **Reading a BAR that was never sized.** With a PCI server configured not to
  do bus configuration (`DO_BUS_CONFIG=no`), the MSI-X table BAR in particular
  can be reported with a size of 0. Mapping one page is enough.
- **Not enabling bus mastering.** `pci_device_attach()` does *not* set the
  command register's memory-space and bus-master bits. Without bus mastering
  the device cannot write into your ring, and the symptom is a request that
  times out with no diagnostic at all. Set the bits yourself:

  ```c
  pci_cmd_t cmd = 0;
  if (pci_device_read_cmd(bdf, &cmd) == PCI_ERR_OK) {
      const pci_cmd_t needed = 0x06;   /* memory space + bus master */
      if ((cmd & needed) != needed)
          (void)pci_device_write_cmd(hdl, (pci_cmd_t)(cmd | needed), NULL);
  }
  ```

## Exercise

1. Write the program above, cross-compile it for the guest, and run it in the
   guest. Print the BDF and the ids.
2. Add the capability-pointer read at `0x34` and print it. On QNX it will be
   0 or garbage; on a host with a different libpci it will be a plausible
   value such as `0x40`. Write down what you got: it tells you which scan
   chapter 3 will have to use.
3. Print the six BARs with `pci_device_read_ba()`. The virtio device's BARs
   are memory BARs; the one that holds the common configuration structure is
   the one the capability table will point at.
4. Detach and re-attach twice in a row. The second attach must succeed: the
   first detached cleanly.

## Solution

The listing above is the solution, with the printing of the BARs left to the
reader. The "solution" for step 2 is that there isn't one: the read is
unreliable on QNX, which is why the next chapter does not use it.

`pci_device_read_ba()` fills an array of `pci_ba_t`:

```c
pci_ba_t bars[6];
int_t nba = 6;
if (pci_device_read_ba(hdl, &nba, bars, pci_reqType_e_UNSPECIFIED) == PCI_ERR_OK) {
    for (int_t i = 0; i < nba; i++)
        printf("BAR%u addr %llx size %llx type %u\n", bars[i].bar_num,
               (unsigned long long)bars[i].addr, (unsigned long long)bars[i].size,
               bars[i].type);
}
```

A virtio-9p-pci device under QEMU typically shows one or two memory BARs, the
first of which holds everything the capabilities point at.

## Where we are

We can find the device and read its ids, but we do not yet know where its
registers are. Chapter 3 reads the capability table and finds out.