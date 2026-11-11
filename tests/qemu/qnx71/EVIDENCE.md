# What a real QNX guest reports about a virtio-9p device

Measured on **11 November 2026, 16:40–17:30 CET**, on `baptiste-MS-7D18`:
QNX Neutrino **7.1.0** x86_64 (image built 2022-07-12), booted by QEMU **8.2.2**
with the option string `qshare` emits, plus `-device vmxnet3` for the guest's own
network.

The host side, exactly as `qshare` produces it for a 9P share:

```
-fsdev local,id=pgwshare,path=<dir>,security_model=mapped-xattr
-device virtio-9p-pci,fsdev=pgwshare,mount_tag=passerelle
```

Every block below is copy-pasted from the guest (`./qnx-ssh '<command>'`), not
paraphrased.

---

## 1. The guest boots

```
# uname -a
QNX vm1 7.1.0 2022/07/12-18:14:39EDT x86pc x86_64
```

Running processes include the QNX PCI server and the packet stack that carries
SSH — the machine is alive and its drivers work:

```
     pid Arguments
       1 procnto-smp-instr -v -mr -d 0777 -u 0777
   28676 pci-server --config=/proc/boot/pci_server.cfg
  213010 io-pkt-v6-hc -dvmxnet3
  274451 /system/xbin/sshd -f /system/etc/ssh/sshd_config
```

## 2. QNX's PCI server enumerates our device

On the host side, QEMU places it at bus 0, device 5, function 0:

```
Bus 0, device 5, function 0:
    Class 0002: PCI device 1af4:1009
      PCI subsystem 1af4:0009
      IRQ 10, pin A
      BAR0: I/O at 0xc000 [0xc03f]
      BAR1: 32 bit memory at 0xfeb96000 [0xfeb96fff]
      BAR4: 64 bit prefetchable memory at 0xfe000000 [0xfe003fff]
```

Inside QNX, `pci-tool` lists the same device and — this is the point — reports
**six capabilities, MSI-X first, then five vendor-specific ones**:

```
# pci-tool -vv
B000:D05:F00 @ idx 8
	vid/did: 1af4/1009
		<vendor id - unknown>, <device id - unknown>
	class/subclass/reg: 00/02/00
		<Classcode is undefined>
	revid: 0
	cmd/status registers: 103/10
	Capabilities list (6):
	             11 (MSI-X) --> 09 (VEND) --> 09 (VEND) --> 09 (VEND) --> 09 (VEND) --+
	             09 (VEND)
	Address Space list - 3 assigned
	    [0] I/O, addr=c000, size=40, align: 40, attr: 32bit CONTIG ENABLED
	    [1] MEM, addr=feb96000, size=0, align: 0, attr: 32bit CONTIG ENABLED
	    [4] MEM, addr=fe000000, size=4000, align=4000, attr: 64bit PREFETCH CONTIG ENABLED
	Interrupt list - 0 assigned
	hdrType: 0
		ssvid: 1af4  ?
		ssid:  0009
```

### What each line means for the specification

| Observation | Requirement | What it establishes |
|---|---|---|
| `vid/did: 1af4/1009` | **VIO-01** | QNX 7.1's PCI server finds the device with exactly the identifiers the driver looks for (`PGW_VIRTIO_PCI_VENDOR` = `0x1AF4`, `PGW_VIRTIO_PCI_DID_9P_TRANSITIONAL` = `0x1009`). The discovery *premise* is now observed on QNX instead of taken from a datasheet |
| `Capabilities list (6): 11 (MSI-X) --> 09 (VEND) × 5` | **VIO-02**, **VIO-04** | The five vendor capabilities a modern virtio device must expose (COMMON, NOTIFY, ISR, DEVICE, PCI_CFG) are visible to userspace through QNX's own PCI API, and the MSI-X capability is there too. Capability walking is the right approach; it does not need raw config-space access |
| BARs `[0] I/O 0xc000`, `[1] MEM 0xfeb96000`, `[4] MEM 64-bit prefetch 0xfe000000` | VIO-02, VIO-07 | QEMU's firmware assigned three address spaces, and QNX reports them: the I/O BAR is the legacy PIO window of a transitional device, BAR1 is the modern configuration window, BAR4 is the shared-memory window virtio uses for the queues |
| `Interrupt list - 0 assigned` | **VIO-04**, **E1** | **No interrupt is routed to this device.** See §4 — this is the concrete finding, and it is a warning, not a bug in our code |
| `class/subclass/reg: 00/02/00` | — | QEMU reports a transitional virtio device as an ethernet-class device, which is a known quirk of the transitional ID range; a driver must match on vendor/device IDs, not on class |

## 3. The image's PCI configuration explains both the BARs and the missing IRQ

```
# cat /proc/boot/pci_server.cfg
[buscfg]
DO_BUS_CONFIG=no

[envars]
PCI_DEBUG_MODULE=pci_debug2.so
PCI_HW_MODULE=pci_hw-Intel_x86.so
PCI_HW_CONFIG_FILE=/proc/boot/pci_hw.cfg

# cat /proc/boot/pci_hw.cfg
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

- `DO_BUS_CONFIG=no`: the PCI server does not program BARs itself. Here the
  address spaces still come out assigned, because QEMU's firmware did it. **A
  driver must not assume that**: on real hardware with this file, BARs may be
  unassigned, which is exactly the constraint `kodebooth/qnx-virtio` documents
  and decision D3 records.
- The `[interrupts]` section routes legacy INTx only for `B0:D17:F0` and
  `B3:D0:F0`. Our device is at `B0:D05:F0`, **so nothing routes its interrupts** —
  which is precisely what `Interrupt list - 0 assigned` says. This is the
  actionable part of E1: on this image a driver would have to add its own entry
  (bus, device and the INTx line QEMU chose, here pin A) or take the MSI-X path,
  for which the image already ships the capability module `pci_cap-0x11.so`.

## 4. What is in the image, and what is not

```
# ls /proc/boot | grep -E '^pci_'
pci_bkwd_compat.so   pci_cap-0x05.so   pci_cap-0x10.so   pci_cap-0x11.so
pci_debug2.so        pci_hw-Intel_x86.so   pci_slog2.so   pci_strings.so
```

| Present | Absent |
|---|---|
| `pci-server` v2.13 with `libpci`, the **MSI-X capability module** (`pci_cap-0x11.so`), the Intel x86 hardware module, a *backward compatibility* module for the deprecated `pci_*()` API | `qcc`, `gcc`, `mkifs`, `mkqnximage` — **no toolchain at all** |
| `fs-dos.so`, `fs-qnx6.so` (disk filesystems), `mount`, `libsecpol.so`, `ability`, `qconn` | any 9P or virtiofs client: nothing to mount a share with, which is the gap this project fills, now **observed** rather than asserted |

Two consequences, both recorded in
[`docs/verification-status.md`](../../../docs/verification-status.md):

1. **Enumeration and capability discovery work on QNX** — the first half of E1.
2. **The interrupt is not routed, and there is no compiler here.** The second
   half of E1 and all of E2/E3/E4 still require the QNX SDP (or an image with
   `qcc` in it).

## 5. Reproducing

```bash
export QNX_VM_DIR=~/qnx71-vm
tests/qemu/qnx71/qnx-vm.sh start
sleep 50
tests/qemu/qnx71/qnx-ssh 'uname -a; pci-tool -vv' | sed -n "/1af4\/1009/,+12p"
tests/qemu/qnx71/qnx-vm.sh stop
```

The VGA console is the only local console (`qnx_console.py` dumps it to a PNG);
SSH on `127.0.0.1:2225` as `root`/`root` is what the commands above use.
