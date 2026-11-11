# Video Script — Under Three Minutes

**Deliverable**: a single video shorter than **3:00**, in English, submitted with the
hackathon entry. Derived from Section 8 of the [specification](spec-en.md).

**Rules of the exercise**
- Hard ceiling 3:00. Target **2:52** to leave margin.
- Every segment shows something *running*, never a slide of bullet points alone.
- The narration states facts that the footage proves in the same shot.
- Prior art is named out loud (spec §3.3 — one of the judges leads QNX open-source engineering).

**Recording setup**
- 1920×1080, two sources side by side: left = host PC terminal, right = QNX guest console.
- Font ≥ 16 px in both terminals so text is readable after compression.
- The kernel trace window (`traceprinter` or the IDE trace view) is a third pane for the priority segment.
- Record the guest at real time; never speed up footage that is supposed to prove latency.

---

## Timeline

### 0:00 – 0:20 — The problem (20 s)

| On screen | Narration |
|---|---|
| Left: `make` finishes. Right: `scp app qnx:/tmp` then `./app` — then the same with an image rebuild (`mkqnximage`), progress bar crawling. A counter overlay: "one iteration = 1 copy". | "Cross-compiling for QNX is fast. Testing is not. Every change means copying the binary into the guest, or rebuilding the disk image. Each cycle pays a copy tax." |

Cut on the progress bar mid-way — that is the hook.

### 0:20 – 0:55 — Passerelle: zero copy (35 s)

| On screen | Narration |
|---|---|
| Host: `qshare ~/project`. Guest boots, `ls /host` lists the project tree. Host: `make`. Guest: `/host/build/my_app` runs instantly. Then a split-second edit on the host (`echo` into a file) and `cat /host/README` in the guest showing the new line. | "Passerelle mounts a host directory inside the QNX guest. The image stays minimal — kernel and driver. Compile here, run there. Edit on the host, and the guest sees it on the next read, with no remount and no cache to invalidate." |

### 0:55 – 1:25 — It is not a toy (30 s)

| On screen | Narration |
|---|---|
| `fsproof diff` scrolling: operations counter climbing past 1,000,000, "divergences: 0". Brief cut to the case table (symlink loops, 255-byte names, 5 GiB file, 100,000-entry directory) all green. | "A differential harness runs randomized operation sequences in the guest and compares the tree — names, types, sizes, permissions, timestamps, symlink targets, content hashes — against the host, which is the ground truth. One million operations, zero divergence. And the same harness runs against the existing clients, so the comparison is apples to apples." |

### 1:25 – 2:00 — The numbers (35 s)

| On screen | Narration |
|---|---|
| Benchmark chart: bars for `gvergine/qnx-fs-9p`, `eclipse-score` virtio9p, Passerelle 9P, Passerelle virtiofs, and `scp`. Two panels: sequential throughput, and round trips per POSIX call. | "Round trips per call, measured from QEMU traces, and sequential throughput. This is where the project earns its place: 9P on x86_64 — which neither existing client offers with full POSIX semantics — and virtiofs, the FUSE protocol over virtio, which had no public QNX guest implementation." |

Chart values must come from `bench_report`, generated in the same session. If a bar is
not yet measured, it is labelled "to be measured" on screen — no invented numbers.

### 2:00 – 2:30 — Real-time behaviour (30 s)

| On screen | Narration |
|---|---|
| Guest: a high-priority reader (priority 30) issues a 4 KiB read while a low-priority `cp` of a large file (priority 10) saturates the share. Stopwatch on the read. Cut to the kernel trace showing the driver thread running at the caller's priority and the request overtaking the bulk I/O. | "This is a real-time operating system, so the driver behaves like one. Requests are handled at the caller's priority, and when the queue saturates they leave in priority order. A priority read during a bulk copy does not queue behind it — you can see the inherited priority in the kernel trace." |

### 2:30 – 2:45 — Least privilege (15 s)

| On screen | Narration |
|---|---|
| `pidin` showing the driver running as a non-root user with only `mem_phys`, `interrupt` and `pathspace`. Mount options line: `nosuid` by default. | "The driver runs without root, with the minimum capabilities: map the device, take its interrupt, attach the mount point. Shared files never elevate privileges." |

### 2:45 – 3:00 — Honesty and credit (15 s)

| On screen | Narration |
|---|---|
| README on screen, with the prior-art table and the NOTICE file. Then the repository URL and the tutorial title. | "Passerelle builds on public work: `gvergine/qnx-fs-9p` for the 9P client design, `eclipse-score/qnx_unit_tests` for MSI-X on QNX, `kodebooth/qnx-virtio` for the transport API. What we add — x86_64 PCI, virtiofs, the conformance bench, priority scheduling — is written down, tested, and offered back upstream. Code, docs and tutorial: github.com/BaptisteFaisy/passerelle." |

---

## Shot Checklist (for the editing pass)

| # | Segment | Must be visible | Recorded from |
|---|---|---|---|
| 1 | Copy tax | `scp` or `mkqnximage` actually running | host terminal |
| 2 | `qshare` | command line + resulting QEMU launch | host terminal |
| 3 | Mount | `ls /host`, `mount` output | guest console |
| 4 | Live edit | host write + guest read, same take | both panes |
| 5 | Differential run | counter + "0 divergences" | guest console |
| 6 | Edge cases | table of green cases | guest console |
| 7 | Chart | `bench_report` output | host, rendered SVG/PNG |
| 8 | Priority read | stopwatch + kernel trace | guest + trace viewer |
| 9 | Least privilege | `pidin` + ability list | guest console |
| 10 | Credits | README table + NOTICE | host editor |

## Text Overlays (English, short)

- 0:00 `compile → copy → run` · 0:20 `zero copy` · 0:55 `1,000,000 ops, 0 divergences`
- 1:25 `round trips per call` · 2:00 `priority inherited` · 2:30 `no root`
- 2:45 `prior art cited, additions stated`

## Things That Would Invalidate the Video

- Showing a benchmark bar that was not produced by the harness in this repository.
- Speeding up footage of the priority test.
- Claiming virtiofs works before feasibility test E5 and the read/write path are done.
- Omitting the two existing 9P clients — the spec forbids it (§3.3), and the judges
  will know.

Status of each segment depends on the schedule in §10 of the specification; anything
not yet runnable is shot only once it is, and
[verification-status.md](verification-status.md) is the source of truth for that.
