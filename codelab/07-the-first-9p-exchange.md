# Chapter 7 — The first 9P exchange, and the first byte read

**Goal.** Negotiate `9P2000.L` with QEMU, attach to the share, walk to a file,
open it, and read its first byte — with a decoder that does not trust
anything the server sends.

**Time.** 60 minutes.

## 7.1 The 9P2000.L wire format

Every 9P message, without exception, is:

```
  size[4]  type[1]  tag[2]  then the body
```

all little-endian. `size` counts the whole message, header included. `type` is
the message number. `tag` is the request number, chosen by the client, and
every reply carries the tag of the request it answers.

A reply's type is always its request's type plus one: `Tversion` (100) is
answered by `Rversion` (101), `Tread` (116) by `Rread` (117).

Strings are `len[2]` followed by that many bytes, **without a NUL**. A qid is
13 bytes: `type[1] version[4] path[8]`.

The messages this chapter needs:

| Type | Number | Body |
|---|---|---|
| `Tversion` | 100 | `msize[4] version[s]` |
| `Rversion` | 101 | `msize[4] version[s]` |
| `Tattach` | 104 | `fid[4] afid[4] uname[s] aname[s] n_uname[4]` |
| `Rattach` | 105 | `qid[13]` |
| `Twalk` | 110 | `fid[4] newfid[4] nwname[2] wname[s]...` |
| `Rwalk` | 111 | `nwqid[2] wqid[13]...` |
| `Tlopen` | 12 | `fid[4] flags[4]` |
| `Rlopen` | 13 | `qid[13] iounit[4]` |
| `Tread` | 116 | `fid[4] offset[8] count[4]` |
| `Rread` | 117 | `count[4] data[count]` |
| `Rlerror` | 7 | `ecode[4]` — a **Linux** errno |

Two of those deserve attention before the code.

**`Rlerror` carries a Linux errno.** Not a QNX one. The wire numbers are
Linux's, on every architecture and every operating system. A QNX driver must
translate them, and the translation is a table, not arithmetic: Linux's
`ENOTEMPTY` is 39 and QNX's is 93. This is requirement P9-04 of this
project's specification.

**`Tlopen`'s flags are Linux `O_*` values.** QNX's `_IO_FLAG_*` bits are
different numbers, and QNX's access mode is a bitmask where Linux's is an
enumeration. The driver translates before it sends.

## 7.2 The session, in five steps

The order is not negotiable, and every step depends on the previous one.

```
  Tversion  ->  Rversion      what protocol, what message size
  Tattach   ->  Rattach       the root of the export, as fid 0
  Twalk     ->  Rwalk         a path component, from fid 0 to a new fid
  Tlopen    ->  Rlopen        the file, open on the server
  Tread     ->  Rread         bytes
```

**`Tversion` first, always.** It says "I speak 9P2000.L, and I can handle
messages of up to *msize* bytes". The server answers with the version it will
speak and the size *it* will use — which may be smaller than what was asked
for. The smaller number is the one that counts. `Tversion` is the only
message sent with the tag `NOTAG` (0xFFFF), because it happens before tags
mean anything.

**`Tattach` binds fid 0 to the export root.** The `aname` is the name of the
export as QEMU knows it — with `-virtfs local,...,mount_tag=hostshare`, the
`aname` is `hostshare`. `afid` is `NOFID` (0xFFFFFFFF) when no
authentication is wanted.

**`Twalk` turns a path into a fid.** It takes a source fid, a destination
fid, and up to 16 names. The reply says how many names were resolved: if it
is fewer than were asked for, the walk stopped early, and the reason is in
the *last* name — a symbolic link, or a non-directory.

**`Tlopen` opens the file on the server.** This is where `O_TRUNC` takes
effect, and where the file is actually opened rather than just stat'ed.

**`Tread` reads.** `count` is how many bytes the client wants; the server
answers with `count` bytes it actually wrote, which may be fewer (at the end
of the file), and the data follows.

## 7.3 The encoder

The rule that makes an encoder safe: **never write past the capacity you were
given, and patch the size field at the end**, when the body length is known.

```c
static void put8(writer *w, uint8_t v)
{
    if (w->pos + 1u > w->cap) {
        w->fail = true;
        return;
    }
    w->b[w->pos++] = v;
}

static int finish(writer *w)
{
    if (w->fail || w->pos > 0xFFFFFFFFu)
        return CLAB_ERR_INVAL;
    for (unsigned i = 0; i < 4u; i++)
        w->b[i] = (uint8_t)((uint32_t)w->pos >> (8u * i));
    return (int)w->pos;
}
```

Every integer is written byte by byte. No structure is ever cast onto a
message buffer: the same object code is then correct on any alignment and any
endianness, and the writer cannot be tricked by a length field.

## 7.4 The decoder, and why it is the important half

The reply comes from outside the guest. It may be truncated, it may be
corrupt, it may be an answer to a different request. Four checks, in this
order, before a single field is read:

```c
clab_p9_err clab_p9_check(const uint8_t *buf, size_t len, uint8_t want_type,
                          uint16_t want_tag, clab_p9_msg *out)
{
    if (buf == NULL || len < CLAB_P9_HDR)
        return CLAB_ERR_SHORT;
    const uint32_t declared = get_le32(buf);
    /* The two must agree exactly: a reply shorter than its header claims is
     * a truncation, a reply longer is padding, and both mean the stream is
     * out of step. */
    if (declared != len)
        return CLAB_ERR_SIZE;
    if (buf[4] != want_type)
        return CLAB_ERR_TYPE;
    const uint16_t tag = (uint16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8u));
    if (tag != want_tag)
        return CLAB_ERR_TAG;
    out->body = buf + CLAB_P9_HDR;
    out->len = len - CLAB_P9_HDR;
    out->type = buf[4];
    out->tag = tag;
    return CLAB_OK;
}
```

The size check is the one that is usually forgotten, and it is the one that
matters most: if the server's size field and the bytes that arrived disagree,
the driver does not know where the next message starts. Everything after that
is interpretation.

Then the body is read with a **sticky-failure cursor**: once a read would
cross the end, nothing else is read and the caller checks the flag once, at
the end.

```c
static bool need(clab_p9_rd *r, size_t n)
{
    if (r->fail || r->pos + n > r->len) {
        r->fail = true;
        return false;
    }
    return true;
}
```

And the length inside the body is checked against what is left:

```c
clab_p9_err clab_p9_dec_rread(const clab_p9_msg *m, const uint8_t **data,
                              uint32_t *count)
{
    clab_p9_rd r;

    clab_p9_rd_init(&r, m);
    *count = clab_p9_get32(&r);
    if (r.fail || (size_t)*count != r.len - r.pos)
        return CLAB_ERR_SIZE;
    *data = r.p + r.pos;   /* zero-copy: the reply buffer is the caller's */
    return CLAB_OK;
}
```

That last line is worth a moment: the reply data is a *view* into the
transport's buffer, not a copy. That is what makes a large read free, and it
is why the transport lends its buffers to the protocol layer instead of
copying them.

This is requirement P9-09 of this project's specification ("every response is
validated") and requirement ROB-06 ("the protocol library does not crash on
any response"). The exercise's test case checks all four rejections.

## 7.5 The exercise

```sh
/tmp/codelab-build/ex06_first_byte
```

```
chapter 7 - the first 9P exchange, and the first byte

Tversion (msize 8192, "9P2000.L"): 21 bytes
Rversion: msize 8192, version "9P2000.L"

Tattach (fid 0, uname "nobody", aname ""): 29 bytes

Twalk (fid 0 -> newfid 1, "hello.txt"): 28 bytes
Tlopen (fid 1, O_RDONLY): 15 bytes

Tread (fid 1, offset 0, count 4096): 23 bytes
Rread: 11 bytes: "hello, QNX
"
first byte: 0x68 'h'

malformed replies:
  short reply, wrong tag and inflated count all refused
```

The byte counts are worth checking by hand, once. `Tversion` is 7 bytes of
header, 4 for the msize, 2 for the string length and 8 for "9P2000.L": 21.

## 7.6 Putting it on the wire

The five messages go onto the virtqueue like any other request: two
descriptors, the message in the device-readable one, the reply area in the
device-writable one.

```c
/* The request buffer: what the device reads. */
uint8_t *req = slot->mem;
int len = pgw_p9_enc_tversion(req, slot->cap, tag, msize, "9P2000.L");

/* Two descriptors: the request, and the place for the reply. */
pgw_vio_sg sg[2] = {
    { .addr = req,       .phys = slot->phys,             .len = (uint32_t)len, .write = false },
    { .addr = slot->mem + slot->cap, .phys = slot->phys + slot->cap,
      .len = slot->cap, .write = true },
};

pgw_vq_push(vq, sg, 2, cookie);
pgw_vq_kick(vq);      /* notify the device */

/* Wait for the completion, then decode. */
void *cookie = NULL;
uint32_t reply_len = 0;
pgw_err rc = pgw_vq_pop(vq, &cookie, &reply_len);
```

and then the validation, with the tag that was sent:

```c
pgw_p9_hdr hdr;
if (pgw_p9_dec_hdr(reply, reply_len, &hdr) != PGW_OK)
    return PGW_EIO;
if (hdr.type != PGW_P9_RVERSION || hdr.tag != tag)
    return PGW_EIO;
```

**The tag check is what keeps two requests in flight apart.** With several
requests outstanding, the completion order is not the request order — the
device may return them in any order — so the tag is the only thing that says
which reply belongs to which request. A driver that ignores the tag and
assumes "the next reply is for my request" works with one request in flight
and corrupts data with two.

## 7.7 What to observe

- The reply to `Tversion` carries the server's msize, which may be smaller
  than what was asked for.
- The first byte read is `0x68`, the `h` of "hello".
- Three malformed replies are refused rather than decoded.

## 7.8 Traps

- **Not checking the tag.** With one request in flight it does not matter;
  with two, it corrupts data.
- **Not checking the size field against the bytes received.** The stream
  desynchronises, and every later message is garbage.
- **Accepting a string whose length runs past the message.** The classic
  overflow: `len[2]` says 60000, the message is 100 bytes.
- **Ignoring trailing bytes.** A reply with extra bytes at the end means
  something is wrong; accepting it hides the fault until much later.
- **Sending QNX's open flags instead of Linux's.**
- **Translating `Rlerror`'s errno with arithmetic.** It is a table.
- **Copying the reply data out of the transport buffer.** The reply lives in
  the transport's buffer, and copying it defeats the zero-copy design (and
  costs a memcpy of up to 128 KiB per read).

## Exercise

1. Run `ex06_first_byte` and verify the byte counts by hand: the header is 7
   bytes, a string is 2 bytes of length plus its characters, and the msize is
   4.
2. Change the `Rread` count to 200 in the exercise's reply builder and run
   it. The decoder must refuse it, because 200 bytes were promised and 5
   arrived.
3. Write a `Tversion` whose version string is "9P2000" (without the `.L`),
   and check that the server's answer is rejected by the driver. The version
   string is the negotiation: a server that does not speak `9P2000.L` cannot
   carry the Linux open flags this driver sends.
4. On the guest, add a debug print of the reply's first 16 bytes after each
   request, and compare them with the exercise's output. The bytes are the
   same, except for the msize the server chose.

## Solution

Exercise 2: the check is `clab_p9_dec_rread()`'s

```c
if (r.fail || (size_t)*count != r.len - r.pos)
    return CLAB_ERR_SIZE;
```

which compares the count in the message with the bytes that are actually
there.

Exercise 3: the driver must compare the server's version string with the one
it asked for, and refuse anything that is not `9P2000.L`. The exercise's
`clab_p9_dec_rversion()` returns the string, and the caller checks it:

```c
if (strcmp(version, "9P2000.L") != 0)
    return PGW_ENOTSUP;
```

Exercise 4: the exercise builds the replies by hand, so the bytes are
identical; in the guest the msize is the server's choice, typically 8192 or
131072, and the rest of the header is the tag the driver chose.

## Where we are

The driver reads a byte from a file on the host PC. Chapter 8 turns it into
something a program can use: a QNX resource manager.