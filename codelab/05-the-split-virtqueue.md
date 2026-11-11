# Chapter 5 — The split virtqueue

**Goal.** Put a request on the queue, let the device take it, and take the
reply back — knowing exactly what each of the three rings contains at every
step.

**Time.** 60 minutes. This is the chapter where the work is.

## 5.1 The three rings

A virtqueue is three structures in memory shared between the driver and the
device (virtio 1.2, §2.7):

```
  descriptor table          the buffers, and how they are chained
  available ring            what the driver offers:  flags, idx, ring[]
  used ring                 what the device has finished: flags, idx, ring[]
```

### The descriptor table

16 bytes per entry, and every field is little-endian:

```
  offset 0   8 bytes   addr     the address of the buffer
  offset 8   4 bytes   len      its length, in bytes
  offset 12  2 bytes   flags    NEXT (0x1), WRITE (0x2), INDIRECT (0x4)
  offset 14  2 bytes   next     the next descriptor in the chain
```

Three flags matter here:

- **NEXT**: this descriptor is not the last of the chain; `next` says which is.
- **WRITE**: the buffer is *device-writable*. Without it, the buffer is
  device-readable. This flag is the whole difference between a request and a
  reply.
- **INDIRECT**: this descriptor does not describe a buffer; it describes a
  *table* of descriptors elsewhere. It is how a request with many
  scatter-gather elements fits in a queue with few descriptors.

### The available ring

```
  offset 0   2 bytes   flags    NO_INTERRUPT (0x1), when the driver is asked
  offset 2   2 bytes   idx      the number of entries the driver has published
  offset 4   2*n bytes ring[]   the head descriptor of each chain
```

### The used ring

```
  offset 0   2 bytes   flags    (unused by the driver)
  offset 2   2 bytes   idx      the number of entries the device has published
  offset 4   8*n bytes ring[]   id (the head descriptor) and len (bytes used)
```

## 5.2 One request, step by step

This is the sequence the driver performs for one 9P request. The exercise
prints the ring at every step, which is the only reason this chapter is worth
reading twice.

**Step 1 — fill the descriptors.** A 9P request is two buffers: the request
the device reads, and the reply it writes.

```c
/* The request: the device reads it. */
vq->desc[0].addr = request_phys;
vq->desc[0].len  = request_len;
vq->desc[0].flags = VRING_DESC_F_NEXT;
vq->desc[0].next  = 1;

/* The reply: the device writes it. */
vq->desc[1].addr = reply_phys;
vq->desc[1].len  = reply_capacity;
vq->desc[1].flags = VRING_DESC_F_WRITE;   /* device-writable */
vq->desc[1].next  = 0;
```

**Step 2 — publish the head in the available ring.**

```c
vq->avail->ring[vq->avail->idx % qsize] = 0;   /* the head descriptor */
```

**Step 3 — publish the index.** This is the step that makes the device look
at the entry, and the order is not negotiable: the entry must be written
*before* the index.

```c
atomic_thread_fence(memory_order_release);   /* the entry is visible... */
vq->avail->idx++;
```

**Step 4 — notify the device.** Write the queue index to the notification
address the NOTIFY capability designates:

```c
__cpu_membarrier();          /* ...and the index is visible to the device */
*(volatile uint16_t *)(notify_base + notify_off * mult) = queue_index;
```

The barrier between the index store and the notify write deserves a note: the
notify register is *device* memory, and a C11 `atomic_thread_fence` orders
normal memory against normal memory. `__cpu_membarrier()` (a `dmb sy` on
ARM64, a `mfence` on x86_64) is what orders the two together. A driver that
uses the C11 fence alone works most of the time and fails when the
hypervisor is fast, which is the worst way to fail.

**Step 5 — the device works.** It reads the chain, writes the reply into the
device-writable buffer, and publishes the completion:

```c
used->ring[used->idx % qsize].id  = head;   /* the head descriptor */
used->ring[used->idx % qsize].len = bytes_written;
used->idx++;                                  /* then the index */
```

**Step 6 — the driver takes the completion.** The decision is made from
`used->idx`, *never* from the fact that an interrupt arrived:

```c
uint16_t idx = vq->used_hdr[1];
atomic_thread_fence(memory_order_acquire);
while (vq->last_used != idx) {
    volatile struct vring_used_elem *e = &vq->used_ring[vq->last_used % vq->qsize];
    uint32_t id = e->id, len = e->len;
    vq->last_used++;
    /* the chain at `id` is complete; `len` bytes were written */
}
```

The acquire fence comes *before* reading `used->idx` and the elements, so the
reply bytes the device wrote are visible before the driver reads them.

**Step 7 — recycle the descriptors.** The chain's descriptors go back on the
free list.

## 5.3 Why "the chain is the unit"

Look at step 6: the used ring's `id` is the *head* descriptor of the chain,
not the last one. The device does not report "descriptor 1 is done"; it
reports "the chain starting at descriptor 0 is done". That is why the
descriptor table has a `next` field and why the driver has to walk the chain
to recycle it.

It is also why the free list lives *inside* the descriptor table: a free
descriptor's `next` field points at the next free descriptor, so recycling a
chain is a matter of walking it and pushing each descriptor onto the list.

```c
void clab_vring_recycle(clab_vring *vq, uint16_t head)
{
    uint16_t idx = head;

    for (;;) {
        const bool has_next = (vq->desc[idx].flags & CLAB_VRING_DESC_F_NEXT) != 0u;
        const uint16_t next = vq->desc[idx].next;
        vq->desc[idx].flags = 0;
        vq->desc[idx].next = vq->free_head;
        vq->free_head = idx;
        vq->num_free++;
        if (!has_next)
            break;
        idx = next;
    }
}
```

Forgetting this step leaks one descriptor per request, and the queue fills up
after `size / 2` requests. The symptom is a driver that works for a while and
then stops with "no descriptor available".

## 5.4 The exercise

```sh
/tmp/codelab-build/ex04_vring
```

```
chapter 5 - the split virtqueue

direct descriptors
empty ring:
  desc  addr   len  flags  next
  [0]      0     0   0x00   1
  [1]      0     0   0x00   2
  ...
  avail: idx=0  ring[0]=0
  used:  idx=0  last_used=0  free=8
after add(), head = 0, 6 descriptors left:
  desc  addr   len  flags  next
  [0]      0    18   0x01   1
  [1]     32    32   0x02   0
  ...
after publish(), avail.idx = 1 (this is what the device sees):
device wrote 18 bytes, used.idx = 1
reply: "TVERSION MSIZE=8192"
after recycle(), free = 8
```

Read the flags column: descriptor 0 has `0x01` (NEXT) because the chain
continues, and descriptor 1 has `0x02` (WRITE) because the device writes
there. The two runs differ in one thing: with indirect descriptors enabled,
the whole chain is described by *one* descriptor, and `free` drops by one
instead of two.

## 5.5 The queue size, and how to set it up

Before any of the above can happen, the driver tells the device about the
queue:

```c
/* 1. select the queue */
set16(v, VPCI_COMMON_QUEUE_SELECT, 0);

/* 2. read the maximum the device supports, and check the queue is free */
uint16_t max = cfg16(v, VPCI_COMMON_QUEUE_SIZE);
if (max < 2u || cfg16(v, VPCI_COMMON_QUEUE_ENABLE) != 0u)
    return -EIO;

/* 3. choose a size: a power of two, no more than the device supports */
vq->qsize = vpci_queue_size_clamp(max, 128);

/* 4. tell the device */
set16(v, VPCI_COMMON_QUEUE_SIZE, vq->qsize);

/* 5. give the device the three addresses */
set64(v, VPCI_COMMON_QUEUE_DESC,   desc_phys);
set64(v, VPCI_COMMON_QUEUE_DRIVER, avail_phys);
set64(v, VPCI_COMMON_QUEUE_DEVICE, used_phys);

/* 6. and enable it */
set16(v, VPCI_COMMON_QUEUE_ENABLE, 1);
```

**The size must be a power of two.** The ring uses modular arithmetic
(`idx % size`), and a size that is not a power of two is not supported by the
specification. A driver that asks for 100 and gets 100 has a bug that appears
as a ring that desynchronises after a few hundred requests.

`vpci_queue_size_clamp()` rounds down:

```c
uint16_t vpci_queue_size_clamp(uint16_t device_max, uint16_t wanted)
{
    uint16_t size = wanted < device_max ? wanted : device_max;

    if (size < 2u)
        return 2u;
    while ((size & (uint16_t)(size - 1u)) != 0u)
        size = (uint16_t)(size & (uint16_t)(size - 1u));
    return size;
}
```

## 5.6 The memory the ring lives in

The three rings must be in memory the device can reach: on QNX, memory that
is physically contiguous and whose physical address the driver can give to the
device.

```c
void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_PHYS | MAP_ANON, NOFD, 0);
off64_t phys;
size_t contig;
mem_offset64(p, NOFD, len, &phys, &contig);
if (contig < len)
    return -ENOMEM;              /* not contiguous: the device cannot use it */
```

`mem_offset64()` gives the physical address *and* the size of the contiguous
run. The check matters: on a machine with an IOMMU, the address the device
wants is not the physical one, and the translation has to be done by the
driver. On QEMU, without an IOMMU, the guest physical address is the device
address.

Two practical points:

- **Allocate the ring in one block.** Three separate mappings mean three
  physical addresses, and the driver has to know all three. One block, one
  address, and the offsets are computed from it.
- **Do not allocate per request.** A mapping per request is a syscall per
  request and it fragments the address space. The buffers are allocated once,
  at queue setup, and reused.

## 5.7 What to observe

- The descriptor table shows `NEXT` on the request and `WRITE` on the reply.
- `avail->idx` goes from 0 to 1 when the request is published, and the entry
  in `ring[]` is the head descriptor.
- `used->idx` goes from 0 to 1 when the device finishes, and `len` is the
  number of bytes it wrote.
- The reply buffer contains the device's answer.
- After the recycle, the free count is back to 8.

## 5.8 Traps

- **Publishing the index before the entry.** The device may read a stale
  entry, and the failure is intermittent.
- **A C11 fence instead of `__cpu_membarrier()` before the notify.** The
  notify register is device memory; only the full barrier orders the two.
- **Reading `used->idx` before the acquire fence.** The reply bytes may not
  be visible yet, and the driver reads yesterday's answer.
- **Deciding completion from the interrupt instead of from `used->idx`.**
  Spurious wakeups are normal — they were observed with virtio-mmio — and
  completion must be decided from the ring.
- **A queue size that is not a power of two.**
- **Forgetting to recycle.** The queue fills up and the driver stalls.
- **One mapping per request.** Slow, and it fragments memory.
- **Reusing the buffers of a request that timed out.** The device may still
  complete it later; this is why the real transport marks such a slot
  *stale* instead of freeing it.

## Exercise

1. Run `ex04_vring` and follow the descriptor table through the six steps.
   Write down the flags of descriptors 0 and 1, and where the head descriptor
   index appears in the three rings.
2. In a copy of `clab_vring.c`, swap the two lines of `clab_vring_publish()`
   — write the index first, then the entry. Run the test. What fails, and
   what would the symptom be on a real device?
3. Comment out the `clab_vring_recycle()` call. The test `test_ch5_round_trip`
   checks the free count; what would a real driver do after `size/2` requests?
4. Add a test that fills the ring completely and checks that the next add
   fails with -1. The exercise `test_ch5_indirect_and_full` already does this
   for the indirect case; do it for the direct case too.

## Solution

Exercise 2: the test that fails is the one checking `avail->idx`, because
the entry is written to `ring[avail->idx % size]` *after* the index was
incremented, so it lands one slot further than the index points at. On a
real device the request is simply never seen, and the driver waits for a
completion that will not come.

Exercise 3: after `size/2` requests (8 descriptors, 2 per request), the free
list is empty, `clab_vring_add()` returns -1, and the driver reports "no
descriptor available" — a stall that appears after a fixed number of
requests, which is a much easier symptom to diagnose than an intermittent
one.

Exercise 4:

```c
unsigned added = 0;
while (clab_vring_add(&vq, sg, 1) >= 0) {
    clab_vring_publish(&vq);
    added++;
}
CLAB_CHECK_EQ(added, CLAB_VRING_SIZE);   /* one descriptor per request */
CLAB_CHECK_EQ(clab_vring_add(&vq, sg, 1), -1);
```

## Where we are

We can talk to the device. Chapter 6 makes the device talk back: the
interrupt.