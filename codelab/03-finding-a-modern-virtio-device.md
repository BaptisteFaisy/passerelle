# Chapter 3 — Finding a modern virtio device by its capabilities

**Goal.** Read the virtio PCI capability table, map the BARs it points at, and
know the address of every structure the driver will use: the common
configuration, the notification area, the ISR byte and the device
configuration.

**Time.** 45 minutes.

## 3.1 Why capabilities, and why that is a problem worth solving

A PCI device is described by two things: the standard header, and a list of
*capabilities* — optional structures that extend the header. A virtio device
uses them to say where its registers are, because the modern virtio PCI
interface deliberately does not fix any addresses: the device may put them
anywhere, in any BAR, and the driver has to be told.

The list is a linked list, and the standard way to walk it is:

```
  status register (0x06), bit 4:   "there is a capability list"
  capability pointer (0x34):       the first capability
  each capability:  cap_next points at the next one, 0 ends the list
```

On QNX, the capability pointer is at `0x34`, and QNX's libpci cannot read
below `0x40`. The textbook walk is therefore unavailable, and a driver has to
do something else.

## 3.2 The scan that works everywhere: a linear sweep

Since QEMU's virtio devices put their capabilities in the `0x40..0xFF` region
and the region is small, the practical answer on QNX is to **sweep the region
and recognise the capabilities by their header**. A virtio capability always
starts with:

```
  byte 0 = 0x09   (cap_vndr: vendor-specific)
  byte 1 = cap_next
  byte 2 = cap_len
  byte 3 = cfg_type
```

So the sweep is: read 4 bytes at a time from `0x40` to `0xFC`; if byte 0 is
`0x09` and byte 3 is between 1 and 5, decode it; then **skip the rest of that
capability** and continue.

The skip is the part people get wrong. If you do not skip, a length field
that happens to start with `0x09` is decoded as a second capability, and you
end up with a capability table that is subtly wrong. `cap_len` tells you how
many bytes the capability occupies; the next one starts at the next 4-byte
boundary after it.

## 3.3 The decoder

Here is the complete decoder, which is also the exercise's `code/src/clab_pci.c`.
It takes a *callback* to read configuration space, rather than calling libpci
itself — that is what makes it testable on a PC with no device at all, and it
is how this project's own transport does it
(`virtio/src/virtio_pci_caps.c`).

```c
/* Reads `len` bytes of config space at `off`. Returns 0, or -1 on failure. */
typedef int (*clab_cfg_read)(void *ctx, uint32_t off, void *dst, size_t len);

typedef struct {
    uint8_t cfg_type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t notify_mult;   /* NOTIFY only */
} clab_pci_cap;

typedef struct {
    clab_pci_cap common, notify, isr, device, pci;
    bool have_common, have_notify, have_isr, have_device, have_pci;
} clab_pci_caps;
```

The decode of one capability:

```c
static void decode_vendor(clab_cfg_read read, void *ctx, uint32_t off,
                          const uint8_t *hdr, clab_pci_caps *out)
{
    clab_pci_cap cap;
    uint32_t bar_word = 0;
    uint32_t mult = 0;

    if (hdr[2] < CAP_MIN_LEN || hdr[3] < CLAB_VIRTIO_CFG_COMMON ||
        hdr[3] > CLAB_VIRTIO_CFG_PCI)
        return;
    if (read(ctx, off + 4u, &bar_word, sizeof bar_word) != 0)
        return;
    memset(&cap, 0, sizeof cap);
    cap.cfg_type = hdr[3];
    cap.bar = (uint8_t)(bar_word & 0xFFu);
    if (read(ctx, off + 8u, &cap.offset, sizeof cap.offset) != 0)
        return;
    if (read(ctx, off + 12u, &cap.length, sizeof cap.length) != 0)
        return;
    if (cap.cfg_type == CLAB_VIRTIO_CFG_NOTIFY && hdr[2] >= 20u) {
        if (read(ctx, off + 16u, &mult, sizeof mult) != 0)
            return;
        cap.notify_mult = mult;
    }
    if (cap.bar > 5u || cap.length == 0u)
        return;
    store(out, &cap);
}
```

and the sweep:

```c
int clab_pci_scan_linear(clab_cfg_read read, void *ctx, clab_pci_caps *out)
{
    if (read == NULL || out == NULL)
        return -1;
    memset(out, 0, sizeof *out);

    for (uint32_t off = 0x40u; off <= 0xFCu; off += 4u) {
        uint32_t word = 0;
        uint8_t hdr[4];
        if (read(ctx, off, &word, sizeof word) != 0)
            return -1;
        hdr[0] = (uint8_t)(word & 0xFFu);
        hdr[1] = (uint8_t)((word >> 8u) & 0xFFu);
        hdr[2] = (uint8_t)((word >> 16u) & 0xFFu);
        hdr[3] = (uint8_t)((word >> 24u) & 0xFFu);
        if (hdr[0] != CLAB_PCI_CAP_VENDOR)
            continue;
        decode_vendor(read, ctx, off, hdr, out);
        if (hdr[2] >= CAP_MIN_LEN)
            off += ((uint32_t)hdr[2] + 3u) / 4u * 4u - 4u;
    }
    return 0;
}
```

The skip line is the whole trick: `cap_len` bytes, rounded up to a multiple of
four, and the loop continues after them.

## 3.4 Two more scans, for two more situations

The linear sweep works on QEMU's virtio devices. Two other scans are worth
having, and the real implementation has all three:

**The chain walk**, for hosts where reading `0x34` works. It is the
specification's method and it is exact:

```c
if (read(ctx, 0x06u, &status, sizeof status) != 0)
    return -1;
if ((status & 0x10u) == 0u)
    return 0;                     /* no capability list at all */
if (read(ctx, 0x34u, &off, sizeof off) != 0)
    return -1;
for (unsigned hops = 0; hops < CHAIN_MAX && off >= 0x40u; hops++) {
    uint8_t hdr[4];
    if (read(ctx, off, hdr, sizeof hdr) != 0 || hdr[0] == 0u)
        break;
    if (hdr[0] == CLAB_PCI_CAP_VENDOR)
        decode_vendor(read, ctx, off, hdr, out);
    if (hdr[1] == 0u || hdr[1] == off)
        break;                     /* end of list, or a self-loop */
    off = hdr[1];
}
```

The `hops` bound and the self-loop test are not paranoia: a malformed or
hostile device must not hang the driver.

**The combined entry point**, which tries the chain and falls back:

```c
int clab_pci_scan(clab_cfg_read read, void *ctx, clab_pci_caps *out)
{
    clab_pci_caps chain;

    if (clab_pci_scan_chain(read, ctx, &chain) == 0 && clab_pci_usable(&chain)) {
        *out = chain;
        return 0;
    }
    return clab_pci_scan_linear(read, ctx, out);
}
```

`clab_pci_usable()` asks the only question that matters: are the two
mandatory structures there? COMMON and NOTIFY are mandatory; the ISR byte and
the device configuration are not.

## 3.5 Mapping the BARs, and the addresses that come out of it

Now that the capabilities are decoded, the driver can compute every address it
needs:

```c
/* Map the BARs the capabilities name. */
for (int_t i = 0; i < nba; i++) {
    uint8_t num = (uint8_t)bars[i].bar_num;
    size_t size = (size_t)bars[i].size;
    if (size == 0u)
        size = 4096u;            /* the PCI server may not have sized it */
    bar[num] = mmap_device_memory(NULL, size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                  0, (uint64_t)bars[i].addr);
    bar_len[num] = size;
}

/* Then the structures. */
common = bar[caps.common.bar] + caps.common.offset;
notify = bar[caps.notify.bar] + caps.notify.offset;
isr    = caps.have_isr    ? bar[caps.isr.bar]    + caps.isr.offset    : NULL;
devcfg = caps.have_device ? bar[caps.device.bar] + caps.device.offset : NULL;
```

and the notification address for a queue:

```c
uint32_t notify_addr = caps.notify.offset + queue_notify_off * caps.notify.notify_mult;
```

The `queue_notify_off` is itself a field of the COMMON structure (offset
`0x1E`), so the driver reads it after mapping. A multiplier of zero means
every queue uses the first word.

## 3.6 The common configuration structure

Once mapped, the COMMON structure is a fixed layout of 8-bit, 16-bit and
32-bit fields, and it is where the driver spends most of its time:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | device_feature_select |
| `0x04` | 4 | device_feature |
| `0x08` | 4 | driver_feature_select |
| `0x0C` | 4 | driver_feature |
| `0x10` | 2 | config_msix_vector |
| `0x12` | 2 | num_queues |
| `0x14` | 1 | device_status |
| `0x15` | 1 | config_generation |
| `0x16` | 2 | queue_select |
| `0x18` | 2 | queue_size |
| `0x1A` | 2 | queue_msix_vector |
| `0x1C` | 2 | queue_enable |
| `0x1E` | 2 | queue_notify_off |
| `0x20` | 8 | queue_desc |
| `0x28` | 8 | queue_driver (the available ring) |
| `0x30` | 8 | queue_device (the used ring) |

Chapter 4 uses the status and feature fields; chapter 5 uses the queue ones.

## 3.7 The device configuration and the mount tag

The DEVICE capability points at the device's own configuration space. For a
9P device it starts with the mount tag:

```
  2 bytes   tag_len
  N bytes   the tag, not NUL-terminated
```

So reading the tag is: read `tag_len`, check it is sane, read that many
bytes, NUL-terminate them yourself. QEMU's tag is the `mount_tag=` value from
the command line.

Two rules a driver needs here, and they are the reason the tag is read *after*
the feature negotiation has started:

- **Reading device configuration is only allowed once the driver has begun
  negotiating** (virtio 1.2, §3.1.1 step 4). Before that, the device may
  change the configuration under you.
- **The tag distinguishes shares.** Two virtio-9p devices on the same bus have
  the same vendor and device id; only the tag tells them apart. This is
  requirement VIO-05 of this project's specification, and it is what lets a
  guest mount two shares.

## 3.8 The exercise

`code/src/ex02_pci_caps.c` builds a synthetic 256-byte configuration space
that looks like QEMU's device, decodes it three ways, and prints the result:

```
chapter 3 - virtio PCI capabilities
capability chain (from 0x34):
  COMMON  bar 4  offset 0x1000  length 0x1000
  NOTIFY  bar 4  offset 0x2000  length 0x1000  multiplier 4
  ISR     bar 4  offset 0x3000  length 0x1000
  DEVICE  bar 4  offset 0x4000  length 0x1000
linear sweep (0x40..0xFF, 32-bit reads):
  COMMON  bar 4  offset 0x1000  length 0x1000
  ...
same device, config space unreadable below 0x40:
  ...
notify address: queue 0 -> +0x2000, queue 3 -> +0x200c
queue size: device max 1024, we want 128 -> 128; want 100 -> 64
```

Run it with:

```sh
cmake --build /tmp/codelab-build
/tmp/codelab-build/ex02_pci_caps
```

## 3.9 What to observe

- The three scans produce the same table.
- The notify address for queue 3 is the notify offset plus three times the
  multiplier.
- The queue size is clamped to a power of two: a request for 100 gives 64,
  because a virtqueue size must be a power of two.

## 3.10 Traps

- **Not skipping a capability's interior.** A `length` field whose low byte is
  `0x09` is decoded as a capability header, and the table becomes nonsense.
  The exercise's test case 5 reproduces this.
- **Reading `0x34` on QNX.** It does not work. The fallback is not a
  convenience, it is the only option on this platform.
- **Forgetting `PROT_NOCACHE`.** Reading a register through the cache gives
  you a value from before your own last write.
- **Trusting a capability that points past the end of its BAR.** A malformed
  or lying device can make the driver read outside its mapping. Check
  `offset + length` against the mapped size.
- **Reading the tag before the device status is set.** The device is allowed
  to change its configuration while the driver is still looking at it; read it
  after ACK|DRIVER, and read the `config_generation` field around the read if
  you want to be sure it did not change under you.

## Exercise

1. Run `ex02_pci_caps` and read its output. Explain, in one sentence each,
   why the chain walk fails when `0x34` cannot be read, and why the linear
   sweep does not.
2. Break the skip rule in a *copy* of `clab_pci.c` (comment out the `off +=`
   line) and run the test again. Which check fails, and what does the broken
   table look like? This is the bug you will meet in the field.
3. Add a case to `test_clab.c`: a device whose NOTIFY capability has a
   multiplier of 0 (the legacy-compatible layout), and check that
   `clab_pci_notify_addr()` returns the same address for queues 0 and 3.
4. On the guest, print the real capability table of QEMU's device. The
   expected result is four capabilities — COMMON, NOTIFY, ISR, DEVICE — all in
   the same BAR, with the NOTIFY multiplier at 4.

## Solution

Exercise 1 is answered by the two scans' implementations: the chain walk
*starts* at `0x34` and has nowhere to go if that read fails, while the linear
sweep starts at `0x40` and never looks below it.

Exercise 2: the failing check is `caps.isr.offset == 0x3000` — without the
skip, the dword inside NOTIFY's multiplier field is decoded as an ISR
capability at BAR 0, offset 0, and the real ISR capability is then either
overwritten or never reached.

Exercise 3: with a multiplier of 0, `notify_addr` is `offset + 0` for every
queue, so all the queues share one word. The check is

```c
CLAB_CHECK_EQ(clab_pci_notify_addr(&caps, 0), clab_pci_notify_addr(&caps, 3));
```

Exercise 4: the real table is in the driver's `-o debug` output; the
multiplier is 4 on QEMU's modern devices.

## Where we are

We know where everything is. Chapter 4 does what the specification says to do
with it: reset the device, acknowledge it, negotiate features, and set
`DRIVER_OK`.