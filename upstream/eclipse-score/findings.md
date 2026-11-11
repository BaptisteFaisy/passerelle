# eclipse-score/qnx_unit_tests, `src/common/virtio9p` — findings

Read on 10–11 November 2026 from the reference tree in
`passerelle-refs/qnx_unit_tests/src/common/virtio9p`. Apache-2.0. Nothing here
was run: the project needs a QNX SDP and a Bazel build, neither of which is on
the development machine. Everything below is read from its code, and the
observations are labelled as such.

This is not a patch. The file is written as a list of suggestions to its
maintainers, in the order a reviewer would probably want to take them, and the
section on MSI-X is there because that method is what Passerelle's feasibility
test E1 relies on: it is the only public description of programming MSI-X on
QNX from a user-space driver without a root PCI server configuration.

## What the client is, and what it does well

A user-space QNX resource manager that speaks 9P2000.L over virtio, used by the
QNX unit-test infrastructure to hand test binaries and runfiles to a QEMU guest
without building a disk image. Two transports behind one `Transport` interface:
PCI on x86_64 with MSI-X, MMIO on ARM64 with explicit base address and IRQ,
both handling virtio versions 1 and 2. About 5 200 lines of C++17 across
`protocol/`, `transport/` and `resmgr/`, plus a `mount_virtio9p` helper script.

Things worth saying plainly, because a findings document that only lists
defects is not a fair reading:

- **It is the only public QNX driver that drives virtio-9p over PCI at all.**
  That is the whole reason it is cited here.
- **The capability scan is honest about a real QNX limitation.** The comment
  above `DiscoverDevice()` says that `pci_device_read_cap()` with the
  `cap_vend` module fails with `PCI_ERR_NO_MODULE` when the module cannot be
  loaded, and that `pci_device_cfg_rd*()` only works from offset `0x40` up. The
  linear 32-bit sweep that follows from that is the same conclusion we reached
  independently, and it is why our own decoder offers a sweep as a fallback.
- **It separates the transports behind an interface** (`Transport` with
  `Initialize`/`Exchange`/`GetMountTag`/`ResetDevice`/`Shutdown`), which is what
  made it readable in an afternoon.
- **The reset path after a timeout is deliberate and documented**: a full device
  reset is the only spec-compliant way to reclaim descriptors the device still
  owns, and the code says so.

## Suggestions

### 1. MSI-X: two constants taken from one boot log

`DiscoverDevice()` allocates an IRQ from the kernel's MSI range by calling
`rsrcdbmgr_create()` then `rsrcdbmgr_attach()` for `start = end = 0x100`, and
programs the MSI-X table entry with message data `78 + (irq - 0x100)`. The
comment explains where those numbers come from: the kernel's boot log prints
`MSI interrupt = 0x100`, `MSI vector no = 78`, `MSI vec count = 177`.

That is a reasonable thing to do to get a driver working, and the comment is
what makes it maintainable. Two suggestions:

- **Read the numbers instead of hard-coding them.** `rsrcdbmgr_query()` (or a
  scan of the IRQ range the kernel publishes) can find the MSI base at run
  time; the APIC vector base is printed but not otherwise exposed, so if it
  cannot be discovered, a mount option or an environment variable would at
  least make a mismatch diagnosable without a recompile. As written, a machine
  whose kernel prints different numbers produces a driver that enables MSI-X,
  receives nothing, and times out — with no log line pointing at the cause.
- **Fall back when the first interrupt does not arrive.** Today, `use_msix_` is
  decided once, before the first request. A cheap improvement: if the very
  first completion times out and `used->idx` has advanced (the device did its
  job, the interrupt did not arrive), detach, re-program as INTx and retry
  once. That turns an unhelpful hang into a slow-but-working mount, and the
  information needed to decide it is already in `WaitForCompletion()`.

This is exactly the trade-off Passerelle's own PCI transport makes — MSI-X
first, INTx when anything in the setup fails — and the reason we could write
that path at all is that this file documents the method.

### 2. A full device reset per timeout drops every in-flight request

`Exchange()` calls `ResetDevice()` on any failure. `ResetDevice()` writes
status 0, tears down the virtqueue, detaches the IRQ, frees both DMA buffers,
then re-runs `InitializeDevice()`. Correct in the sense that it reclaims
everything, and simple. But it is a transport-wide operation triggered by one
request: every other request in flight is silently lost, and the 9P session is
re-established afterwards (`NinePSession::Reinitialize()`), which clunks and
re-attaches the root fid.

If the transport ever gains more than one outstanding request, this becomes a
correctness problem rather than a performance one. `qnx-fs-9p` solves the same
problem with a per-slot `stale` flag: the timed-out slot stays out of use until
the device returns its chain, and the other slots carry on
(`src/transport_virtio.c`, `vio_rpc()`). It is worth considering before adding
concurrency.

### 3. One synchronous exchange at a time

`Exchange(request, response)` copies the request into a fixed bounce buffer,
kicks, waits, copies the response out. There is one pair of DMA buffers, so the
whole client serialises on it. For the unit-test use case that is fine, and the
README says so. It does mean that a `read()` of a large file is a sequence of
strictly serial round trips, and that the resource-manager thread pool cannot
overlap two clients' requests — which is where a real-time system would notice.

A minimal version of the fix is in `qnx-fs-9p` too: N slots, each with its own
request and reply buffer and its own descriptor pair, with the slot number as
the 9P tag. The interface change is small (`Exchange` gains a slot parameter,
or becomes `Get`/`Send`/`Wait`), and it makes the resource manager's existing
thread pool useful.

### 4. Reply validation

`NinePMessage::GetU8/16/32/64` return `0` when a read would cross the end of the
buffer, and `GetString()`/`GetBytes()` return an empty value. There is no sticky
failure flag and no way for a caller to tell "the field was zero" from "the
field was not there". A truncated or hostile server reply therefore decodes
into a well-formed message full of zeroes rather than into an error, and
`ParseRversion`/`ParseRattach`/`ParseRread` cannot detect it.

The pattern that fixes it without changing every call site: one `bool failed`
member, set by any out-of-range access, and one check at the end of each
`ParseR*()`; the same rule `qnx-fs-9p`'s `lib9p` uses. `GetSize()` also returns
the value of the size field without comparing it to `data_.size()`, so a reply
whose header claims more bytes than arrived is accepted.

This matters more than it looks: P9-09 in Passerelle's specification makes
"every response is validated" a requirement, and random-input testing of the
decoder is one of the acceptance criteria (ROB-06). The decoder in this project
is a good place to add that property, because it is the only 9P decoder in the
tree.

### 5. `Transact()` retries by re-initialising the session, recursively

`Transact()` loops up to `kMaxRetries`, and on failure calls
`Reinitialize()`, which resets the fid pool, calls `Initialize()`, which calls
`Transact()` again for `Tversion` and `Tattach`. The `reinitializing_` flag is
what stops the recursion, and it is checked *after* the failing exchange, so
the control flow is one flag away from unbounded recursion. A loop with an
explicit "reconnect once, then fail" step would be easier to reason about, and
it would let the caller decide whether a retry is safe: retrying a `Twrite`
after a device reset is fine, retrying a `Tlcreate` is not (the file may exist).

### 6. Smaller things

- **`TranslateQnxToLinuxFlags()` covers `O_TRUNC` and `O_APPEND` but not
  `O_EXCL`, `O_NOFOLLOW`, `O_DIRECTORY` or `O_SYNC`.** The comment is explicit
  that only the differing flags are translated, which is fair; the consequence
  is that `open(O_EXCL|O_CREAT)` on an existing file succeeds. QNX's
  `_IO_FLAG_*` bits and Linux's `O_*` bits differ enough that a table is
  probably safer than a chain of `if`s.
- **The capability scan does not read `cap_next` at all**, by design (it cannot
  read `0x34`). It therefore cannot detect that it is looking at a device whose
  capabilities are not contiguous from `0x40`, and it skips interior dwords only
  for capabilities it recognises as vendor-specific (`0x09`) — an MSI or MSI-X
  capability's interior is still swept, which is harmless but noisy in the
  debug log. A `cap_len`-driven skip for the two known non-virtio capability IDs
  would make the log readable.
- **`pci_device_read_cmd()` is called with the BDF before `pci_device_attach()`
  has enabled bus mastering**, and the code then sets bits 1 and 2 explicitly.
  Worth a comment saying that `pci_device_attach()` does *not* enable bus
  mastering: it is the single most common reason a virtio driver's first
  request hangs, and it is invisible in the code as written.
- **`GetMountTag()` returns `-EINVAL` when the tag is empty**, and the caller
  treats that as fatal. A device that does not offer
  `VIRTIO_9P_F_MOUNT_TAG` has no tag at all rather than an invalid one; the two
  cases could be distinguished, which would make the error message say what is
  actually wrong.
- **`mmap_device_memory()` is asked for `bars[i].size`, or one page when the
  PCI server reports 0.** The comment explains the `DO_BUS_CONFIG=no` case. It
  would be worth also checking that `offset + length` from the capability fits
  inside the size actually mapped: a capability that points past the end of the
  BAR is currently trusted.

## The MSI-X method, precisely

This is the part of the file that Passerelle depends on, so it is described here
in enough detail to be re-implemented (it is described, not copied: no line of
the project appears in Passerelle).

Context: the QNX x86_64 image is started with `DO_BUS_CONFIG=no` in the PCI
server's configuration. SeaBIOS has already enumerated the bus and assigned the
INTx lines, but the PCI server allocates no MSI vectors, so
`pci_device_cfg_cap_enable()` and the `cap_msix_*` helpers fail. The driver
programs the capability itself:

1. **Find the MSI-X capability** (`cap_id 0x11`) by sweeping config space from
   `0x40` to `0xFC` in 4-byte reads, and keep its offset.
2. **Read Message Control** at `cap_offset + 2`: bits `[10:0]` give
   `table_size - 1`, bit 15 is the enable, bit 14 the function mask.
3. **Read Table BIR and Table Offset** at `cap_offset + 4`: bits `[2:0]` are
   the BAR index, the rest is the offset inside that BAR. Map that BAR if it is
   not mapped yet (`mmap_device_memory`, `PROT_NOCACHE`), and use one page when
   the PCI server reports a size of 0.
4. **Claim one IRQ from the kernel's MSI range**: `rsrcdbmgr_create()` then
   `rsrcdbmgr_attach()` with `flags = RSRCDBMGR_IRQ | RSRCDBMGR_FLAG_RANGE`,
   `start = end = 0x100` (the base printed at boot). The resulting number is
   what `InterruptAttachEvent()` will be given.
5. **Program table entry 0** at `table_base + table_offset`, four 32-bit words:
   message address `0xFEE00000` (APIC, physical destination mode, destination 0),
   message address high `0`, message data = the APIC vector
   (`78 + (irq - 0x100)` with the numbers from the boot log), then the control
   word: `1` to mask while programming, `0` to unmask.
6. **Enable MSI-X**: write `0xC000` to Message Control (enable + function mask),
   then `0x8000` (enable, function mask cleared).
7. **Disable INTx**: set bit 10 (`0x0400`) of the PCI command register, so the
   device cannot raise both.
8. **Tell the virtio device which vector to use**: `common_cfg.queue_msix_vector
   = 0` for queue 0, and `config_msix_vector` left at `0xFFFF` (no vector). If
   the device reads back `0xFFFF` from `queue_msix_vector`, it rejected the
   vector and the driver falls back to INTx.
9. **Attach**: `SIGEV_PULSE_INIT` with `SIGEV_PULSE_PRIO_INHERIT` on a
   `ConnectAttach`ed side channel, then
   `InterruptAttachEvent(irq, &event, _NTO_INTR_FLAGS_TRK_MSK)`. Completion is
   awaited with `MsgReceivePulse()` under a `TimerTimeout()`, and
   `InterruptUnmask()` is called after each pulse.

Three details in that sequence are worth flagging to whoever maintains it,
because they are the fragile ones: the IRQ base `0x100`, the APIC vector base
`78`, and the assumption that table entry 0 is the right entry for queue 0
(true for a single queue, not for several). Everything else follows the virtio
and PCI specifications.

## What Passerelle takes from this, and what it does differently

**Taken:** the shape of the MSI-X path above, including the
`rsrcdbmgr`-then-program-the-table approach and the INTx fallback; the
observation that `pci_device_attach()` does not enable bus mastering; and the
conclusion that config-space reads below `0x40` cannot be relied on, which is
why our capability decoder has a 32-bit sweep.

**Different:** our transport lives behind `qnx-fs-9p`'s `transport.h` rather
than in its own tree, because the client it plugs into is the complete one; the
capability decoding is portable and unit-tested on a PC, which is a property
this project cannot have (its transport is QNX-only by construction); and
timeouts are per slot rather than per device reset.

**Not taken:** nothing is copied. The numbers, the register offsets and the
capability layouts are facts of the virtio and PCI specifications; the code
around them is written independently, and `NOTICE` records this project as the
source of the MSI-X method.
