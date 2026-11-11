# Validation — what is proven, and what is not

The four patches in `patches/` add a virtio-pci transport and an x86_64 target
to `gvergine/qnx-fs-9p`. This page states, without rounding up, what was
verified on the development PC, what was not, and how to close the gap on a
machine with the QNX SDP.

Base commit: `868a718` ("Initial commit"), the only commit in the upstream
repository when this was written.

## Proven on the PC (no SDP needed)

| What | How | Result |
|---|---|---|
| The series applies to the upstream tree | fresh `git clone` of the reference tree, then `git apply --check` and `git am` for each patch in order | all four: `check OK`, `am OK` |
| The tree builds after the series | `cmake -S . -B build && cmake --build build -j8`, host build | builds, no warning |
| The project's own strict flags hold | the host build uses the upstream `FS9P_STRICT_FLAGS` (`-Wall -Wextra -Wpedantic -Werror`) | no diagnostic |
| The existing tests still pass | `ctest` after the series | 6/6 pass: the five upstream tests (`test_encode`, `test_decode`, `test_idpool`, `test_fuzz`, `consistency`) plus the new one |
| The new decoder is tested | `tests/unit/test_virtio_pci_caps.c`, 7 cases | pass |
| The same under sanitisers | `-DFS9P_SANITIZE=ON` (ASan + UBSan, `-fno-sanitize-recover=all`) | pass |
| The `consistency` test still passes | it checks that every `-o` option in `src/main.c` appears in the `use` message and in `docs/user/reference.md` | pass, `-o transport=` is documented in both |
| The QNX-only files are syntactically valid C11 | `syntax-check/syntax-check.sh <worktree>`: declarations-only stand-ins for `<pci/pci.h>`, `<sys/neutrino.h>`, `<sys/mman.h>`, `<sys/dispatch.h>`, `<sys/rsrcdbmgr.h>`, `<sys/cpuinline.h>`, `<sys/iofunc.h>`, `<sys/resmgr.h>`, compiled `-fsyntax-only -std=c11 -Werror` | `syntax-check: OK` for `virtio_pci_caps.c`, `transport_virtio_pci.c`, `virtio_pci_ring.c`, `main.c` |
| The aarch64 build is not disturbed | diff inspection: the aarch64 toolchain file is untouched, the aarch64 sources are untouched, and the aarch64 target still links `lib9p` | by inspection |

The decoder's test cases, in detail:

1. a well-formed device with all four capabilities is decoded, BAR and offset
   included, and the notify multiplier is read;
2. a cyclic capability chain terminates instead of hanging;
3. a device without NOTIFY is rejected by `vpci_caps_usable()`;
4. when offset `0x34` cannot be read (the QNX case the fallback exists for),
   the chain scan fails and `vpci_caps_scan()` falls back to the linear sweep
   and still finds everything;
5. the linear sweep skips the interior of a capability, so a length or offset
   field that happens to look like a capability header is not decoded as one;
6. the notification address is `offset + queue_notify_off * multiplier`;
7. the queue size is clamped down to a power of two, and to at least 2.

## Not proven — the honest gap

| What | Why it is not proven | Consequence |
|---|---|---|
| The transport actually finds the device on an x86_64 QNX guest | no SDP on the development machine, so no `qcc`, no `mkqnximage`, no image | discovery is written from the virtio 1.2 specification and the QNX 8.0 PCI guide, not from a run |
| MSI-X works on a real image | same | the two magic numbers (MSI IRQ base `0x100`, first APIC vector 78) come from a kernel boot log and from a public QNX implementation, not from our own run |
| The INTx fallback works | same | it reuses the interrupt pattern already proven on ARM64, but the PCI INTx line itself has not been exercised |
| Bus mastering must be enabled explicitly | reported by a public implementation, not re-observed here | if it turns out not to be needed, the write is harmless; if it is needed and skipped, the first request times out |
| The notification write reaches the device | same | the address arithmetic is unit-tested, the write itself is not |
| A 9P session completes over PCI | same | this is the end-to-end check that matters, and it has not been run |
| Throughput and latency over PCI | same | no measurement, so no claim |

Nothing above is a regression risk for the ARM64 path: that code is unchanged.

## Closing the gap: the procedure on a machine with the SDP

```sh
# 1. Build for x86_64.
source ~/qnx800/qnxsdp-env.sh
cmake -S . -B build-qnx-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-x86_64.cmake
cmake --build build-qnx-x86

# 2. Get an x86_64 image that contains fs-9p.
#    packaging/fs-9p.build already lists what to add; the binary goes to /proc/boot.
mkqnximage --type=vm --arch=x86_64 ...

# 3. Start QEMU with a share on the PCI bus, and the tag fs-9p will look for.
qemu-system-x86_64 -M pc -m 1G -smp 2 \
    -kernel ... -initrd ... -append ... \
    -virtfs local,path=$HOME/src,mount_tag=hostshare,security_model=mapped-xattr

# 4. In the guest, watch the setup before anything else.
fs-9p -o debug,transport=pci hostshare /mnt/host 2>&1 | tee /tmp/fs9p.log
```

What the log should show, in order: the BDF of the device, the capability
table (`common`, `notify` with its multiplier, `isr`, `device`), the interrupt
line and whether it is MSI-X or INTx, the mount tag, the negotiated features,
the queue size, and then the 9P `Tversion`/`Rversion` exchange. Anything that
stops before the tag is a discovery problem; anything that stops after it but
before `Rversion` is a ring or notification problem; a request that times out
while `used->idx` has advanced is an interrupt problem.

Then, in this order:

```sh
ls -l /mnt/host                 # walk + getattr
cat /mnt/host/some/file         # read
echo hi > /mnt/host/new         # create + write
dd if=/mnt/host/big of=/dev/null bs=1M    # a transfer large enough to use several slots
slay fs-9p                      # clean unmount, device reset
fs-9p -o debug hostshare /mnt/host        # restart: the device must be found at status 0
```

The cases most likely to fail first, and what each failure means:

| Symptom | Likely cause | What to change |
|---|---|---|
| `no virtio-9p device with this mount tag` | the capability scan found nothing, or the tag was not read | run with `-o debug`: is the BDF printed? are the capabilities? |
| The tag prints as garbage | the device-configuration capability was not mapped | check the DEVICE capability's BAR and offset |
| The first request times out, `used->idx` unchanged | the device never saw the notification, or never got bus mastering | check the notify address and multiplier; check the command register |
| The first request times out, `used->idx` advanced | the interrupt never arrived | force INTx (`msix_setup()` returning `-1`); check the boot log's MSI numbers |
| Interrupt storms | the ISR byte is not being read, or the level-triggered line is not re-armed | check that `vpci_ack_interrupt()` runs on every wakeup |
| Everything works, then a timeout leaves the mount stuck | all slots stale | expected until the timeout expires; the slots free themselves when the device returns the chains |

If MSI-X is the problem, the two constants to re-derive are in
`src/transport_virtio_pci.c` (`VPCI_MSI_IRQ_BASE`, `VPCI_MSI_VECTOR_BASE`) and
the boot log prints both. If the device cannot be reached at all, the
declarations-only syntax check in `syntax-check/` at least rules out the
possibility that the code does not compile; what it cannot rule out is that the
QNX API is being used correctly, which is what the first guest run decides.

## What this means for the pull request

The series is presented as: *compiles, applies, unit-tested on the host,
untested on an x86_64 guest*. The cover letter says that in those words, and
offers to run the guest validation and report back as soon as an SDP is
available. Sending it as "works on x86_64" would be a claim we cannot support
today.
