# A real QNX guest on the development PC

This directory holds what is needed to boot a **real QNX Neutrino 7.1.0 x86_64
guest** on this PC under QEMU, with a `virtio-9p-pci` device exported from a
host directory by the same options `qshare` emits.

It exists because feasibility test E1 of the specification — *"under QEMU
x86_64, does a QNX driver obtain the virtio PCI interrupt and exchange a first
9P message?"* — needs a QNX side to look at. We have no QNX SDP, so we cannot
build a driver for it; but we can boot QNX and ask **its own PCI server** what it
sees. What that answered is in [EVIDENCE.md](EVIDENCE.md).

## What this is not

- **Not a QNX SDP.** `qcc`, `mkifs` and `mkqnximage` are absent from the image,
  so nothing can be cross-compiled here. E1/E2/E3/E4 still need the SDP.
- **Not QNX 8.0.** The image is QNX **7.1.0** (2022-07-12). It uses the same
  `pci-server` + `libpci` framework the driver targets, so its view of a virtio
  device is meaningful, but QNX 8.0's kernel changes (no `InterruptAttach()`,
  capability model) are not exercised by it.
- **Not redistributable.** The disk image is QNX software under QNX's
  non-commercial terms. It is *not* in this repository: it is extracted locally
  from a public community container (below) and stays on the machine.

## One-off: get the image

The image comes from the community container `autosarai/qemu_qnx7_1:1.1.0`
(QNX 7.1.0, 6.1 GiB raw disk with an installed QNX system, sshd and qconn).

```bash
docker pull autosarai/qemu_qnx7_1:1.1.0
CID=$(docker create autosarai/qemu_qnx7_1:1.1.0)
mkdir -p ~/qnx71-vm
docker cp "$CID:/home/popcornsar/fs_3GB.img" ~/qnx71-vm/qnx71-fs.img
docker rm "$CID"
```

Then, every time:

```bash
export QNX_VM_DIR=~/qnx71-vm          # optional, this is the default
./qnx-vm.sh start                     # boots in the background
./qnx-ssh 'uname -a; pci-tool -vv'    # shell in the guest (root/root)
./qnx-vm.sh console "ls /" /tmp/s.png # type on the VGA console, save a PNG
./qnx-vm.sh stop
```

The guest gets two virtio-family devices: `vmxnet3` (its network, driven by
`io-pkt-v6-hc -dvmxnet3`, which is what carries SSH) and our `virtio-9p-pci`
share, mount tag `passerelle`. The share directory is `$QNX_VM_DIR/share`.

## Files

| File | Role |
|---|---|
| `qnx-vm.sh` | start / stop / status, ssh, console — one entry point |
| `qnx-ssh` | run a command in the guest; the password goes through `SSH_ASKPASS`, never on the command line |
| `qnx_console.py` | drive the VGA console through the QEMU monitor (`sendkey`) and dump the screen to a PNG, because the image has no serial console |
| `EVIDENCE.md` | the transcript of what the guest reported, dated, with what it means for the requirements |

## Known quirks of this image

- **No serial console.** Everything the machine prints goes to VGA. That is why
  `qnx_console.py` exists; SSH is much more practical once the network is up.
- **SSH arrives late** (~40 s after boot: the image brings up `io-pkt` and then
  `sshd`). `qnx-ssh` fails fast with a refusal before that; retry.
- **Four forwarded ports.** The image has been seen at 192.168.100.10, .12, .2
  and .100 depending on how fast DHCP answers, so all four are forwarded
  (2222–2225) and only the live one answers with an SSH banner.
- **`DO_BUS_CONFIG=no`** in `/proc/boot/pci_server.cfg`: the PCI server does not
  configure BARs itself. QEMU's firmware already assigns them here, so the
  address spaces come out assigned (see EVIDENCE.md) — but a driver must not
  *assume* that on real hardware.
