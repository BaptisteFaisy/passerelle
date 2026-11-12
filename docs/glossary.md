# Glossary

Every term this repository uses, with a one-line definition, the French
equivalent when the project's own French documentation has one, and where the
term comes from. The glossary is the English companion of
[docs/spec-en.md](spec-en.md) §15, which lists only the terms the specification
needs in passing; this page covers the vocabulary of the code as well.

Nothing here is invented: each entry points at the specification, the protocol
document or the source file that uses the term. The French specification
[docs/spec-fr.md](spec-fr.md) remains the authoritative text.

## 1. Sharing a directory: 9P2000.L

| Term | Definition | French | Where it comes from |
|---|---|---|---|
| 9P / 9P2000.L | Remote filesystem protocol from Plan 9; the Linux variant carries errno, uid/gid and POSIX mode semantics. | 9P2000.L | 9P2000.L protocol description (diod), spec §1.2 |
| `Tversion` / `Rversion` | First exchange of a session: the client proposes a version and an `msize`, the server answers with what it accepts. | — | `proto9p/include/pgw/p9.h` (`PGW_P9_TVERSION`) |
| `Tattach` / `Rattach` | Binds a fid to the root of a share, with the credentials the client presents. | — | same (`PGW_P9_TATTACH`) |
| `Twalk` / `Rwalk` | Moves along a path one element at a time and returns the qid of every element visited. | — | same (`PGW_P9_TWALK`) |
| `Tclunk` / `Rclunk` | Releases a fid; the client will not use it again. | — | same (`PGW_P9_TCLUNK`) |
| `Tlerror` / `Rlerror` | The error reply: a Linux errno value, the only answer a server sends for a failed request. | — | same (`PGW_P9_TLERROR`) |
| `Tread` / `Twrite` | Transfer bytes at an offset on an open fid (`Rread`, `Rwrite` answer them). | — | same (`PGW_P9_TREAD`) |
| `Tlopen` / `Tlcreate` | Open an existing fid, or create and open a file; both return the IO unit size. | — | same (`PGW_P9_TLOPEN`) |
| `Tgetattr` / `Tsetattr` | Read or write the metadata of a fid, field by field, through a request mask. | — | same, masks `PGW_P9_GETATTR_*` |
| `Treaddir` | List a directory: a sequence of entries, each carrying its own qid. | — | same (`PGW_P9_TREADDIR`) |
| `Tstatfs` | Filesystem statistics of the share: size, free space, maximum name length. | — | same (`PGW_P9_TSTATFS`) |
| `Tfsync` | Ask the server to flush the pending writes of a fid to the host storage. | — | same (`PGW_P9_TFSYNC`) |
| `Tlock` / `Tgetlock` | Advisory byte-range locking over the wire (FS-14). | — | same (`PGW_P9_TLOCK`) |
| `Tmkdir` `Tlink` `Tsymlink` `Tmknod` `Trename` `Trenameat` `Tunlinkat` `Tremove` | The namespace operations of 9P2000.L: directory, hard link, symlink, device node, rename, unlink, remove. | — | same (`PGW_P9_TMKDIR` … `PGW_P9_TREMOVE`) |
| `Treadlink` | Read the target of a symbolic link. | — | same (`PGW_P9_TREADLINK`) |
| `Tauth` | Optional authentication exchange before an attach; not used by this client today. | — | same (`PGW_P9_TAUTH`) |
| `Tflush` | Ask the server to abandon a request the client no longer waits for (signal interruption). | — | same (`PGW_P9_TFLUSH`) |
| fid | The 32-bit handle a client uses to refer to a file across requests. | fid | 9P2000.L protocol |
| qid | The 128-bit identity of a file: type, version and path (or inode). | qid | 9P2000.L; `pgw_p9_qid` |
| `msize` | Maximum size of one 9P message; this project negotiates 128 KiB by default. | taille maximale de message | `common/include/pgw/limits.h` (`PGW_MSIZE_DEFAULT`) |
| round trip | One request and its reply; the unit of cost of a shared filesystem. | aller-retour | spec §7.5, `host/bench/roundtrips.py` |
| mount tag | The string configured on the QEMU command line and matched by the guest to find its share. | étiquette de montage | virtio-9p device (`PGW_VIO_9P_F_MOUNT_TAG`) |
| security model | How QEMU maps guest ownership onto the host directory: `passthrough`, `mapped-xattr`, `mapped-file`. | modèle de sécurité | QEMU 9P documentation, spec §5.3.7 |
| `multidevs=remap` | QEMU option that remaps inode numbers when one share spans several host filesystems. | — | QEMU 9P documentation, spec §5.3.8 |

## 2. Sharing a directory: FUSE and virtiofs

| Term | Definition | French | Where it comes from |
|---|---|---|---|
| FUSE | The Linux protocol between the kernel and a userspace filesystem server. | FUSE | Linux kernel / libfuse |
| FUSE opcode | The numeric type of a FUSE request (`LOOKUP` = 1, `FORGET` = 2, `GETATTR` = 3 …). | code d'opération | `protofuse/include/pgw/fusep.h` |
| `LOOKUP` / `FORGET` | The pair that holds the reference count of an inode in a virtiofs session. | — | same (`PGW_FUSE_LOOKUP`, `PGW_FUSE_FORGET`) |
| `BATCH_FORGET` | One request that drops several inode references at once. | — | same (`PGW_FUSE_BATCH_FORGET`) |
| `INTERRUPT` | Control request asking the server to abandon a request still in flight. | — | same (`PGW_FUSE_INTERRUPT`) |
| virtiofs | Shared filesystem for virtual machines: FUSE transported over virtio. | virtiofs | virtio-fs project, spec §1.2 |
| vhost-user | The protocol by which a hypervisor hands a virtqueue over to a userspace device process. | — | vhost-user specification, `docs/vhost-user-front-end.md` |
| `virtiofsd` | The reference virtiofs daemon: the userspace device that serves a directory to QEMU. | — | virtio-fs project |
| inode | The unique identity of a file in a POSIX filesystem; virtiofs counts references to it. | inode | POSIX |
| hiprio queue | The virtqueue that carries the priority FUSE requests (`INTERRUPT`, `FORGET`). | file de haute priorité | spec §6.9 (VFS-02) |

## 3. The virtio transport

| Term | Definition | French | Where it comes from |
|---|---|---|---|
| virtio | The standard for paravirtualized devices: guest and hypervisor share descriptor rings. | virtio | virtio specification |
| virtqueue | The ring of descriptors shared by the guest driver and the device (`pgw_virtq`). | file virtuelle | virtio specification, `virtio/include/pgw/virtio.h` |
| split virtqueue | The virtqueue layout with three separate regions: descriptor table, avail ring, used ring. | file virtuelle scindée | virtio specification §2.7 |
| descriptor | One entry of a virtqueue: buffer address, length, flags and next index. | descripteur | virtio specification |
| indirect descriptor | A descriptor pointing at a table of descriptors, so a chain can carry more buffers than the ring holds (VIO-08). | descripteur indirect | `PGW_VIO_F_RING_INDIRECT_DESC` |
| avail ring | The ring the guest fills with the descriptor chains it hands to the device. | file de disponibilité | virtio specification §2.7 |
| used ring | The ring the device fills with the chains it has finished serving. | file d'utilisation | virtio specification §2.7 |
| `eventfd` | The Linux notification primitive QEMU uses for kick and call; the only signal a vhost-user backend needs. | — | Linux kernel, `host/vhost/` |
| doorbell | The MMIO or PCI write that tells the device new descriptors are available. | — | virtio specification §2.1.2 |
| `EVENT_IDX` | The feature that lets each side say when it actually wants to be notified. | — | `PGW_VIO_F_RING_EVENT_IDX` |
| feature bits | The 64-bit flags negotiated between driver and device before the device is used. | bits de fonctionnalité | `PGW_VIO_F_VERSION_1` |
| status register | The register walked through `ACKNOWLEDGE` → `DRIVER` → `FEATURES_OK` → `DRIVER_OK`. | registre d'état | `PGW_VIO_STATUS_*` |
| scatter-gather list | The list of (address, length) pairs describing one transfer without copying it (VIO-07). | liste de dispersion-réunion | `pgw_vio_sg` |
| zero copy | A transfer whose payload is never copied between layers. | copie nulle | spec §6.1 |
| MSI-X | The message-signaled interrupt mechanism of modern PCI devices. | MSI-X | PCI specification, spec §5.3.5 |
| INTx | The legacy pin-based PCI interrupt; the fallback when MSI-X is unavailable. | INTx | PCI specification |
| PCI capability | One entry of a device's capability list; virtio uses vendor capabilities (id `0x09`). | capacité PCI | PCI specification, `docs/qnx-abi.md` |
| BAR | Base address register: the PCI address space a device claims. | registre d'adresse de base | PCI specification, `tests/qemu/qnx71/` |
| MMIO | Memory-mapped I/O: the device registers appear at fixed addresses (ARM64 guests). | E/S mappées en mémoire | `pgw_mmio_io` |
| `libpci` | The QNX 8.0 library a driver uses to attach to a PCI device and read its capabilities. | — | QNX SDP 8.0 documentation |
| in-memory mock transport | A backend simulating the device rings on the PC, so the stack is testable without QEMU (VIO-10). | transport simulé en mémoire | `virtio/` (VIO-10) |

## 4. The QNX guest

| Term | Definition | French | Where it comes from |
|---|---|---|---|
| QNX Neutrino | The microkernel operating system the guest runs. | — | QNX documentation |
| resource manager | A userspace process serving part of the POSIX namespace through messages. | gestionnaire de ressources | QNX resource manager documentation, spec §5.1 |
| `resmgr_attach()` | The call that binds a resource manager to a pathname in the namespace. | — | QNX documentation |
| `iofunc` | The QNX library layer implementing the common filesystem messages on top of a resource manager. | — | `docs/qnx-abi.md` |
| QNX message (IPC) | The Neutrino inter-process communication: `MsgSend`, `MsgReceive`, `MsgReply`. | message QNX | QNX documentation |
| priority inheritance | A worker thread runs at the priority of the client it currently serves (QNX-03). | héritage de priorité | spec §5.2 |
| IST | Interrupt service thread: the thread created by `InterruptAttachThread()` that waits on interrupts. | thread de service d'interruption | QNX 8.0 documentation |
| `InterruptAttachThread()` | The QNX 8.0 API replacing `InterruptAttach()`; no user ISR runs in kernel context. | — | QNX 8.0 documentation |
| `procnto` | The QNX 8.0 kernel process that owns the pathname space. | — | QNX documentation |
| SDP | The QNX Software Development Platform: `qcc`, `mkifs`, `mkqnximage`, the guest toolchain. | plateforme de développement QNX | QNX documentation |
| `qcc` | The QNX compiler driver; `-V gcc_ntox86_64` or `-V gcc_ntoaarch64le` selects the target. | — | QNX SDP documentation |
| IFS | The Initial Filesystem: the bootable image a QNX guest starts from. | système de fichiers initial | QNX documentation |
| IFS fragment (build file) | `image/passerelle.build`: the list of files an image build file must contain for the driver to boot and mount the share. | fragment de fichier de construction | `image/README.md` |
| `mkqnximage` | The QNX utility that builds and launches a virtual-machine disk image. | — | QNX documentation, spec §15 |
| abilities | The fine-grained privileges a QNX process keeps after dropping root. | capacités | `image/passerelle.sec` |
| security policy | The QNX 8.0 file granting abilities to processes (`image/passerelle.sec`). | politique de sécurité | QNX 8.0 documentation |

## 5. Host tools, testing and project vocabulary

| Term | Definition | French | Where it comes from |
|---|---|---|---|
| `qshare` | Host command preparing and starting QEMU with the share (DX-01, DX-02). | — | `host/qshare/` |
| `qrun` | Host command running a program from the share inside the guest (DX-03). | — | `host/qshare/qrun.py` |
| `fsproof` | Host conformance bench: differential test, content exerciser, directed cases (spec 7.2-7.4). | — | `host/fsproof/` |
| `pbench` | Host comparative benchmark: round trips, throughput, metadata, mixed load (PERF-01 to PERF-07). | — | `host/bench/` |
| guest agent | The program running *inside* the guest: it replays a sequence and writes back a snapshot. | agent invité | `host/fsproof/guest_agent.py` |
| sequence | The JSON list of operations a bench sends to the agent. | séquence | `host/fsproof/gen.py` |
| snapshot | The JSON description of a tree: path, kind, size, mode, mtime, SHA-256. | instantané | `host/fsproof/guest_agent.py` |
| differential test | Run the same sequence in the guest and on the PC, then compare the two trees; the PC is the truth. | test différentiel | spec §7.2 |
| reduction | Shrinking a failing sequence to the shortest prefix that still reproduces the divergence. | réduction | `host/fsproof/diff.py` |
| regression | The stored failing case, replayable with `fsproof diff --replay`. | régression | `host/fsproof/replay.py` |
| reference server | An independent implementation (`tests/ref9p`, `tests/fuse_ref`) our client is tested against, so the test is not self-referential. | serveur de référence | `tests/ref9p/README.md` |
| content exerciser | Workers hammering a file with writes and checking every read-back (spec §7.3). | — | `host/fsproof/exercise.py` |
| directed case | One row of the edge-case table of spec §7.4. | cas dirigé | `host/fsproof/cases.py` |
| feasibility test (E1-E5) | The five experiments that decide whether a design point works on real QNX. | test de faisabilité | `docs/verification-status.md` §3 |
| traceability matrix | The generated table linking each requirement to the test that proves it. | matrice de traçabilité | `docs/traceability.md` |
| gap analysis | The numbered list of what the repository still lacks. | analyse des écarts | `docs/gap-analysis.md` |
| sanitizer | ASan and UBSan: compile-time instrumentation turning memory and undefined behaviour into a failure. | assainisseur | spec §6.6, `cmake/` |
| fuzz | Deterministic seeded random input against a wire decoder. | fuzzing | `tests/fuzz/` |
| size guard | The check keeping every file under 500 lines and every function under 50. | garde de taille | `tools/check-size.py` |
| SPDX header | The one-line licence identifier at the top of every source file. | en-tête SPDX | `tools/check-spdx.py` |
| ambition tier (S / C / X) | The three levels of ambition a requirement belongs to: foundation, complete, extreme. | niveau d'ambition | spec §0, §6 |
