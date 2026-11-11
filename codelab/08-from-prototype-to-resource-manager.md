# Chapter 8 — From prototype to resource manager

**Goal.** Turn the prototype into something a program can `open()`: a QNX
resource manager that owns a mount point, serves the POSIX calls, and does it
all at the priority of the caller.

**Time.** 60 minutes.

## 8.1 What a prototype is missing

The driver from chapter 7 does one thing: it reads a byte, on demand, from a
program that called it. A filesystem is not that. Between the two there are
four gaps:

| The prototype | A resource manager needs |
|---|---|
| one hard-coded path | a *namespace*: any path below the mount point |
| one request at a time | several requests in flight, from several threads |
| the caller's thread does the work | the work done at the **caller's priority** |
| a program calls it | a program calls `open()`, and the resource manager answers |

The last one is the QNX-specific part, and it is what this chapter is about.

## 8.2 What a resource manager is

A QNX resource manager is a program that:

1. creates a **channel** and attaches it to a **mount point**;
2. receives **messages** from the programs that use it;
3. answers them.

The kernel does the POSIX translation: when a program calls `open("/mnt/host/a")`,
the C library sends a *connect message* to whoever is attached to `/mnt/host`.
The resource manager answers with an OCB ("open control block") that the kernel
turns into a file descriptor, and the program gets an `int` back. The same
happens for `read`, `write`, `stat` and so on: each POSIX call becomes a
message.

The minimum is small:

```c
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

int main(int argc, char **argv)
{
    const char *mount_point = argc > 1 ? argv[1] : "/mnt/host";
    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL)
        return 1;

    iofunc_mount_t mount;
    iofunc_funcs_t funcs = { .ocb_alloc = my_ocb_alloc, .ocb_free = my_ocb_free };
    iofunc_attr_t root_attr;
    resmgr_attr_t attr;

    memset(&attr, 0, sizeof attr);
    attr.send_func = iofunc_send;
    attr.recv_func = iofunc_recv;
    attr.ack_func = iofunc_ack;
    attr.error_func = iofunc_error;
    attr.hangup_func = iofunc_hangup;
    attr.resmgr_funcs = &funcs;
    attr.mount_funcs = &funcs;
    attr.mount = &mount;

    resmgr_attach(dpp, &mount_point, _FTYPE_ANY, 0, &attr);
    dispatch_loop(dpp);
    return 0;
}
```

Three of those lines carry most of the meaning:

- **`_FTYPE_ANY`** and the flags: the resource manager owns *everything* below
  the mount point, whatever its type. A filesystem has to say "any file type
  here is mine".
- **`iofunc_*` handlers**: the kernel's file-management library does the
  POSIX bookkeeping (permissions, open file descriptions, `lseek` offsets) so
  the driver only implements the operations that are actually remote.
- **`dispatch_loop()`**: the message loop. Everything from here on happens in
  a thread of the dispatch pool.

## 8.3 The connection between the layers

The prototype in chapter 7 has the transport, the protocol and the "read one
byte" logic tangled together in one `main()`. The resource manager needs them
separate, and this project's architecture separates them into three layers:

```
  the POSIX call        the resource manager      the protocol       the transport
  ────────────          ────────────────────      ───────────       ───────────
  read(fd, buf, n) ──► iofunc_read_verify() ──►  Tread         ──► virtqueue
                        pgw_rmfs_read()          pgw_p9_read()      pgw_vq_push()
                        ... the FS semantics ... ... the wire ...    ... the ring ...
```

The interfaces are in this repository:

| Layer | Header | What it offers |
|---|---|---|
| transport | `virtio/include/pgw/virtio.h` | `pgw_vio_pci_open()`, `pgw_vq_push()`, `pgw_vq_pop()`, the mock device for tests |
| protocol | `proto9p/include/pgw/p9.h` | `pgw_p9_enc_tread()`, `pgw_p9_dec_rread()`, the session, the id pools |
| filesystem | `rmfs/include/pgw/rmfs.h` | `pgw_rmfs_read()`, `pgw_rmfs_open()`, the priority scheduler, the permission checks |

The point of the middle layer is that the resource manager never sees a
virtqueue, and the protocol never sees a POSIX file descriptor. A test can
drive the whole filesystem layer on a PC, with a mock device underneath
(`pgw_vio_mock_open()`), and the same code runs in the guest against the real
device.

## 8.4 Handling a read

The message the resource manager receives for a `read()` is an `_IO_READ`.
The handler has three jobs: check the request is legitimate, do the remote
read, and deliver the bytes.

```c
static int my_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_HANDLE_T *handle,
                    void *ocb)
{
    iofunc_ocb_t *my_ocb = ocb;
    my_node_t *node = (my_node_t *)my_ocb->attr;

    /* 1. Let the kernel's library check the mode, the range and the file
     *    type. A read on a directory, or a write on a read-only file, stops
     *    here. */
    if (iofunc_read_verify(ctp, msg, my_ocb, NULL) != EOK)
        return EOK;

    /* 2. Do the remote read. The reply data is a view into the transport's
     *    buffer: it is valid until the next request on the same slot, which
     *    is why it is delivered straight away and never kept. */
    size_t got = 0;
    pgw_err rc = pgw_rmfs_read(fs, node->fd, node->offset, msg->i.buf,
                               msg->i.nbytes, &got, priority_of(ctp));
    if (rc != PGW_OK)
        return errno_for(rc);

    /* 3. Deliver the bytes to the client, straight from the transport's
     *    buffer: resmgr_msgwrite() copies them into the client's message on
     *    the way out. */
    if (got > 0)
        resmgr_msgwrite(ctp, msg, (char *)node->reply_view, (int)got, 1);

    node->offset += got;
    _IO_SETUP_NBYTES(msg, got);
    return EOK;
}
```

Three properties of that handler are worth stating, because they are the
differences between a filesystem and a toy:

**The permission check is done by the kernel's library**, with the caller's
credentials. `iofunc_read_verify()` uses the iofunc attribute to know the
file's mode and owner, and the message to know who is asking. The driver
never re-implements POSIX.

**The reply data is delivered, not copied.** `resmgr_msgwrite()` writes
straight into the client's message buffer. With a 128 KiB read, that is the
difference between one copy and two.

**The priority is the caller's.** `priority_of(ctp)` is the priority the
program that called `read()` is running at. The request is submitted with
that priority, so a high-priority program is not stuck behind a low-priority
one. This is requirement QNX-03 of this project's specification, and it is
what a filesystem for a real-time system is judged on.

## 8.5 Priority, and the queue behind it

The priority is not decoration: it decides the order in which requests are
submitted when the transport cannot take them all at once.

The transport has a fixed number of slots (8 in this project's transport, 4
by default in the configuration). When all of them are busy, a new request
waits. The filesystem layer's scheduler keeps those waiting requests in
priority order:

```c
/* The scheduler keeps waiting requests in priority order, so that a
 * high-priority read overtakes a bulk copy. */
pgw_prio_sched *sched = pgw_prio_sched_create(max_inflight);
pgw_prio_sched_enqueue(sched, priority, work_fn, ctx);
```

When a slot frees up, the scheduler drains the *highest* priority waiting
request first. Requirement QNX-04 of this project's specification, and
requirement PERF-05: "a 4 KiB priority read, launched during a bulk
low-priority copy, takes no more than twice its empty-state time".

Without it, a `dd` of a gigabyte through the share blocks a `cat` of a
configuration file for as long as it takes, and the guest looks like it has
hung.

## 8.6 The OCB and the node

Two structures, and the difference between them is a common source of bugs:

- The **node** holds the state of the *file*: its attributes (from
  `Tgetattr`), its 9P fid, its offset, whether it is open on the server. It
  belongs to the file, not to the descriptor.
- The **OCB** holds the state of the *open*: which node it refers to, and
  the kernel's own bookkeeping.

A file can have several OCBs: when a program `mmap`s it — which happens every
time a binary is executed from the share — the memory manager attaches a
second one. The node's attribute must therefore be reachable from the OCB in
one step, because the kernel's library expects `ocb->attr` to point at the
attribute:

```c
typedef struct {
    iofunc_ocb_t ocb;      /* must be first */
    my_node_t *node;
} my_ocb_t;
```

and the node:

```c
typedef struct {
    iofunc_attr_t attr;    /* must be first: ocb->attr points here */
    pgw_rmfs_fd *fd;       /* the open file, as the filesystem layer sees it */
    uint32_t fid;          /* the 9P fid, until it is walked */
    uint64_t offset;
    bool opened;           /* Tlopen done */
    atomic_uint ocbs;      /* OCBs bound to this node */
} my_node_t;
```

The counting matters: the node may not be freed while an OCB still refers to
it. The reference implementation had a double free here, found by running a
binary from the share — a good illustration of why this chapter exists.

## 8.7 Mounting it

```c
/* In the guest: */
fs-9p -o msize=131072 hostshare /mnt/host
```

or, with the mount helper:

```
mount -T fs-9p /mnt/host
```

and then, from any program:

```
ls -l /mnt/host
cat /mnt/host/some/file
```

What to look for:

- `df -g` shows the mount point, with the share's statistics.
- `mount` lists it with its type.
- A file opened on the PC appears, and a file created in the guest appears on
  the PC. That last check is the one that proves the write path.

## 8.8 What to observe

- The mount point appears in `/proc/mounts`.
- `ls -l /mnt/host` lists the host directory with the host's sizes and dates.
- `cat` of a file returns its exact contents.
- A file created in the guest appears in the host directory within a second.

## 8.9 Traps

- **Not setting `_FTYPE_ANY`.** The mount point refuses to accept files.
- **Forgetting `iofunc_read_verify()`.** The driver answers a read on a
  directory, or a read past the end of a file, and the kernel's library never
  gets to complain.
- **Copying the reply data.** The transport's buffer is valid only until the
  next request; keeping a pointer to it is a use-after-free waiting to happen.
  The reference implementation delivers it immediately with
  `resmgr_msgwrite()` for exactly this reason.
- **Handling the request at the resource manager's priority.** The whole
  point of priority inheritance is lost, and a bulk copy starves the
  interactive requests.
- **Freeing the node on the first close.** A mapped file has a second OCB.
- **Not clunking the 9P fid.** The server keeps the fid open forever, and
  the guest's fids run out. Requirement P9-07 of this project's
  specification: no leak after a million open/close cycles.
- **Not resetting the device on unmount.** Requirement VIO-09: at unmount, the
  device is reset and no interrupt remains attached.
- **Attaching the interrupt in a request thread.** The interrupt thread must
  be its own.

## Exercise

1. Write the smallest resource manager that mounts an empty directory and
   answers a `stat` on the mount point. Compile it for the guest
   (`qcc -Vgcc_ntox86_64`) and run it: `ls -l /mnt/host` must show the
   directory.
2. Add a `read` handler that answers with a fixed string instead of a remote
   read. `cat /mnt/host/x` must print it. This is the fastest way to see the
   message flow: every `cat` produces a read message, and `dmesg` or the
   `msg` utility shows them.
3. Write the OCB and node structures of section 8.6 and check with
   `-Wpadded` that the attribute is the first member of the OCB's node.
4. Read `rmfs/include/pgw/rmfs.h` and find the function that implements the
   priority scheduler. Write two lines describing what it does with a
   request that arrives while the transport is full.

## Solution

Exercise 1: the skeleton of section 8.2, with a `stat` handler that fills an
`iofunc_attr_t` and calls `iofunc_attr_update()`. The mount point appears in
`/proc/mounts`, and `ls -l` on it returns the attributes the handler set.

Exercise 2: the handler is the one of section 8.4, with the remote read
replaced by `resmgr_msgwrite(ctp, msg, "hello\n", 6, 1)`. The `msg` utility
(or `dmesg | grep MsgSend`) shows the connect and read messages arriving.

Exercise 3: the attribute must be the first member of the node, and the node
must be reachable from the OCB, because `iofunc_read_verify()` and the kernel
read `ocb->attr` directly. The check that catches a mistake is compiling with
`-Wpadded` and reading the warnings.

Exercise 4: the scheduler is `pgw_prio_sched_enqueue()` and
`pgw_prio_sched_drain_next()` in `rmfs/include/pgw/rmfs.h`. A request that
arrives while the transport is full is enqueued with the caller's priority;
when a slot frees up, `drain_next()` takes the highest-priority waiting
request first, so a priority read overtakes a bulk copy.

## Where we are

The driver is a filesystem: a program can `open()` a file on the host PC from
inside a QNX guest, and the guest sees a normal directory tree.

## Where to go next

- **virtiofs.** The successor of 9P: faster, and the protocol is FUSE
  instead of 9P2000.L. The transport is the same, which is why the layering
  in section 8.3 pays off twice. Requirement VFS-01 of this project's
  specification.
- **The priority scheduler.** Section 8.5 is the sketch; the interesting part
  is measuring it (requirement PERF-05).
- **The other side of the bridge.** A QNX guest *serving* its filesystem to a
  Linux host over the network is the same protocol in the other direction
  (requirement SRV-01).
- **The real driver.** `virtio/`, `proto9p/` and `rmfs/` in this repository
  are the versions of the code in this tutorial with the exercises' corners
  cut off.