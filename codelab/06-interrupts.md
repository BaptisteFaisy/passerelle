# Chapter 6 — Interrupts

**Goal.** Get the device's completion interrupt into the driver, on QNX 8.0,
with MSI-X when the machine allows it and the classic INTx line when it does
not.

**Time.** 45 minutes, and the part where the afternoon goes.

## 6.1 What changed in QNX 8.0

QNX 7.1 had `InterruptAttach()`, which took a handler function: the kernel
called it in interrupt context, and the handler had to do everything without
sleeping, without locking, and without allocating.

QNX 8.0 **removed it**. What replaced it is a model where the interrupt
*wakes a thread*:

| Function | What it does |
|---|---|
| `InterruptAttachThread(irq, flags)` | the **calling thread** becomes the interrupt thread: it is the one the kernel wakes |
| `InterruptAttachEvent(irq, &event, flags)` | the kernel sends an **event**; any thread can receive it |

For a driver, `InterruptAttachThread()` is almost always the right choice, and
the reason has nothing to do with style:

> **Requests are submitted from resource-manager threads, not from the
> interrupt thread.** The thread that attaches is not the thread that waits for
> a completion. The interrupt thread must therefore be able to take a mutex,
> touch the rings, and wake other threads — that is exactly what a thread in
> ordinary context can do, and exactly what an interrupt-context handler
> cannot.

The `InterruptAttachEvent()` form is for drivers that want the completion to
land in a pulse, for instance to feed an existing event-driven design. It
works, but it adds a channel, a connection and a pulse, and the wakeup still
has to be turned into "look at the used ring".

## 6.2 The interrupt thread

```c
static void *ist_main(void *arg)
{
    struct vio_pci *v = arg;

    /* The calling thread becomes the IST. Needs PROCMGR_AID_INTERRUPT. */
    int id = InterruptAttachThread(v->irq, 0);
    if (id == -1) {
        /* errno says why: EPERM if the ability is missing */
        return NULL;
    }

    for (;;) {
        /* Wait for the interrupt. The flags argument must be
         * _NTO_INTR_WAIT_FLAGS_UNMASK, and the timeout argument must be NULL. */
        if (InterruptWait(_NTO_INTR_WAIT_FLAGS_UNMASK, NULL) == -1) {
            if (errno == EINTR)
                continue;       /* a signal arrived; not an error */
            break;              /* something is wrong with the attachment */
        }

        /* Acknowledge the device, so the line drops. */
        vpci_ack_interrupt(v);

        /* Take the completed chains off the used ring and wake the waiters. */
        pthread_mutex_lock(&v->lock);
        vpci_reap_locked(v);
        pthread_mutex_unlock(&v->lock);
    }

    InterruptDetach(id);
    return NULL;
}
```

Four things in that listing are load-bearing.

**`InterruptWait()` takes two arguments, and the second must be NULL.** The
signature is `InterruptWait(int flags, const struct sigevent *timeout)`, and
passing anything but NULL is not supported.

**The `_UNMASK` flag re-arms the line.** The kernel masks the interrupt line
before waking the thread; `_NTO_INTR_WAIT_FLAGS_UNMASK` tells it to unmask it
again as part of the wait. Without it, the thread wakes once and never again.

**The interrupt is acknowledged before the work.** For an INTx device the line
is level-triggered: it stays asserted until the device is acknowledged. The
acknowledgement is a read of the ISR byte, and reading it clears the pending
bits (virtio 1.2, §4.1.4.5):

```c
void vpci_ack_interrupt(struct vio_pci *v)
{
    if (v->isr != NULL)
        (void)*(volatile uint8_t *)v->isr;
}
```

**Completion is decided from the used ring, not from the wakeup.** This is the
single most important rule in the chapter. Spurious wakeups *happen*: with
the virtio-mmio transport, the thread often wakes with an empty ISR status,
and the real completion follows a moment later. If the driver treats "I was
woken" as "a request is complete", it reads a ring that has not been updated
yet and reports garbage. So the wakeup is only a hint; the ring is the truth.

```c
void vpci_reap_locked(struct vio_pci *v)
{
    uint16_t idx = v->used_hdr[1];
    atomic_thread_fence(memory_order_acquire);
    bool any = false;
    while (v->last_used != idx) {
        volatile struct vpci_used_elem *e = &vq->used_ring[v->last_used % vq->qsize];
        uint32_t id = e->id, len = e->len;
        v->last_used++;
        /* mark the slot done, wake the waiter */
        any = true;
    }
    if (any)
        pthread_cond_broadcast(&v->cond);
}
```

The acquire fence is before the read of `used->idx`, so the device's writes
are visible.

## 6.3 MSI-X, and the fallback that is not optional

The two mechanisms:

- **INTx** is the classic interrupt line: one line per device function,
  shared with other devices, level-triggered. It is what the firmware assigns,
  and it is what the driver gets for free from `pci_device_read_irq()`.
- **MSI-X** gives the device its own message-signalled interrupts, one per
  queue. No sharing, no line to re-arm, and much lower latency. It is the
  right mechanism, and on QNX it is the one that needs work.

### The QNX situation

With the PCI server configured not to do bus configuration (`DO_BUS_CONFIG=no`,
which is common on QNX x86 images because SeaBIOS has already enumerated the
bus), the PCI server allocates **no MSI vectors**. The usual QNX calls —
`pci_device_cfg_cap_enable()`, `cap_msix_set_nirq()`, `cap_msix_set_irq_entry()`
— fail with `PCI_ERR_NO_MODULE` or a similar error.

The workaround is to program the capability by hand. This is the method the
public `eclipse-score/qnx_unit_tests` virtio-9p driver uses, and it is
described in detail in `upstream/eclipse-score/findings.md` in this
repository; the steps are:

1. find the MSI-X capability in configuration space (capability id `0x11`)
   and read its Message Control (the table size) and its Table BIR and Table
   Offset;
2. map that BAR if it is not mapped yet — one page is enough when the PCI
   server reports a size of 0;
3. claim one IRQ from the kernel's MSI range with `rsrcdbmgr_attach()`, using
   `RSRCDBMGR_IRQ | RSRCDBMGR_FLAG_RANGE`;
4. write table entry 0 through the BAR: message address `0xFEE00000`
   (physical, destination 0), message data = the APIC vector, then the control
   word (mask, program, unmask);
5. enable MSI-X in Message Control: write `0xC000` (enable + function mask),
   then `0x8000` (enable, mask cleared);
6. set the INTx-disable bit in the PCI command register, so the device does
   not raise both;
7. tell the virtio device which vector to use: `queue_msix_vector = 0` for
   queue 0, and `config_msix_vector = 0xFFFF` (no vector).

Two of those numbers come from the kernel's boot log rather than from a
header: the MSI IRQ range starts at `0x100`, and the first APIC vector used
for it is 78. The boot log prints both (`MSI interrupt = 0x100`, `MSI vector
no = 78`). **They are machine-specific**, and a driver that hard-codes them
without checking the log will fail on a machine that numbers them
differently — silently, since MSI-X then delivers to nobody.

### The fallback ladder

This is the part to take away. Every step may fail, and each failure must
degrade to something that works:

```
  MSI-X available?
    ├─ capability present?      no  -> INTx
    ├─ table BAR mapped?        no  -> INTx
    ├─ vector allocated?        no  -> INTx
    ├─ device accepted it?      no  -> INTx
    └─ the interrupt arrived?   no  -> INTx (after one timeout)
  INTx available?
    ├─ a line assigned?         no  -> polling
    └─ InterruptAttachThread() ok? no -> polling
  polling: always possible, it just burns CPU
```

The exercise implements exactly this ladder, with the facts as inputs:

```c
clab_irq_mode clab_irq_choose(const clab_irq_facts *facts)
{
    if (clab_irq_why(facts, CLAB_IRQ_MSIX) == CLAB_IRQ_OK)
        return CLAB_IRQ_MSIX;
    if (clab_irq_why(facts, CLAB_IRQ_INTX) == CLAB_IRQ_OK)
        return CLAB_IRQ_INTX;
    return CLAB_IRQ_POLL;
}
```

And the last rung of the ladder deserves emphasis: **"the interrupt did not
arrive" is a failure mode you must handle at run time, not only at setup
time.** The MSI-X path can be set up perfectly and still deliver nothing,
because the vector numbers were wrong. The robust driver notices that the
first request completed (the used ring advanced) but no interrupt arrived,
and re-arms the device as INTx.

## 6.4 The exercise

```sh
/tmp/codelab-build/ex05_irq
```

```
chapter 6 - interrupts
  everything works                           -> MSI-X    (MSI-X refused: ok)
  no MSI vector (DO_BUS_CONFIG=no)           -> INTx     (MSI-X refused: no MSI vector available)
  MSI-X table BAR not mapped                 -> INTx     (MSI-X refused: MSI-X table BAR not mapped)
  device rejected the vector                 -> INTx     (MSI-X refused: device rejected the vector)
  device has no MSI-X capability             -> INTx     (MSI-X refused: no MSI-X capability)
  no vector, no line                         -> polling  (MSI-X refused: no MSI-X capability)
  INTx line, attach failed                   -> polling  (MSI-X refused: no MSI-X capability)
interrupt thread order:
  ATTACH -> WAIT -> ACK -> REAP -> WAIT ... -> DONE
```

## 6.5 What to observe

- Each way the MSI-X path can fail leads to INTx, and every way the INTx path
  can fail leads to polling.
- The thread's order is ATTACH → WAIT → ACK → REAP → WAIT, and the two
  variants that are bugs are checked as such.

## 6.6 Traps

- **Using `InterruptAttach()`.** It does not exist in QNX 8.0; the compiler
  will tell you.
- **Passing a timeout to `InterruptWait()`.** The second argument must be
  NULL.
- **Forgetting `_NTO_INTR_WAIT_FLAGS_UNMASK`.** The thread wakes once and
  never again.
- **Acknowledging after the work instead of before.** On a level-triggered
  line, the line is still asserted when the thread goes back to sleep, and the
  next interrupt is lost or the line storms.
- **Deciding completion from the wakeup.** Spurious wakeups are normal.
- **Attaching the interrupt in the thread that submits requests.** With a
  resource manager, requests come from a pool of threads; the interrupt
  thread must be its own.
- **Hard-coding the MSI numbers without reading the boot log.**
- **Not detaching the interrupt at exit.** Requirement VIO-09 of this
  project's specification: on unmount, no interrupt may remain attached.
- **Not needing the `interrupt` ability.** `InterruptAttachThread()` fails
  with `EPERM` without it, and that is a configuration error, not a bug.

## Exercise

1. Run `ex05_irq` and produce the ladder for a machine with: an MSI-X
   capability, a mapped table, but no MSI vector available. What does the
   driver use, and what is the *observable difference* for the user?
2. Add a case to `clab_irq_facts`: `attach_errno` set while `line_assigned` is
   true. Which mode is chosen, and what does the driver lose?
3. Write the interrupt thread skeleton of section 6.2 in a file of your own,
   with the `vpci_ack_interrupt()` and `vpci_reap_locked()` calls left as
   empty stubs. Compile it for the guest (`qcc -Vgcc_ntox86_64`) and read the
   compiler's complaints: `InterruptWait` wants two arguments, and
   `InterruptAttachThread` wants an `unsigned` flags.
4. In the guest, run `pidin intr` before and after your driver attaches the
   interrupt. The IRQ number that appears is the one the driver chose.

## Solution

Exercise 1: INTx. The user-visible difference is a slightly higher latency
per completion and a shared line, which on a busy machine can mean the
interrupt is delivered later or not at all — and a driver that does not handle
a shared line will eventually lose a completion.

Exercise 2: polling. The driver loses the ability to sleep: it must check
the used ring in a loop, which burns a core and adds latency, but it keeps
working.

Exercise 3: the two complaints are the point of writing the skeleton early.
`InterruptWait(_NTO_INTR_WAIT_FLAGS_UNMASK, NULL)` is the correct call; the
flags must be the unmask flag, and the timeout argument must be NULL.

Exercise 4: `pidin intr` lists the IRQ with its handler. The number is the
one from `rsrcdbmgr_attach()` (MSI-X) or from `pci_device_read_irq()`
(INTx). If the driver was supposed to use INTx and the MSI IRQ number appears
there, the fallback did not trigger.

## Where we are

The transport works end to end. Chapter 7 puts the 9P protocol on top of it
and reads the first byte of a file.