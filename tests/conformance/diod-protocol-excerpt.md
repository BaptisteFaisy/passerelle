<!--
SPDX-License-Identifier: Apache-2.0
-->

# diod's protocol description - the excerpt the conformance table cites

This file is the citation target of the conformance table
(`tests/conformance/conformance_table.c`): every rule of that table names a
section heading of *this* file, and the table bench fails when a heading is
missing. It is an excerpt, not a copy: the wire layouts (the machine-readable
part of the description) are reproduced so that a rule can be checked against
bytes, and the sentences a rule depends on are quoted.

## Provenance

| | |
| --- | --- |
| Document | `protocol.md` - diod's description of the 9P2000.L protocol |
| Upstream | <https://github.com/chaos/diod> - the `diod` file server, packaged as `diod` by Debian and Ubuntu |
| File | <https://github.com/chaos/diod/blob/master/protocol.md> |
| Commit | `de51d1ee1bd5ccf1d8c16b96227c8bb03ec501db` (2026-06-23) |
| Licence of the upstream file | GPL-2.0-or-later (`COPYING`, `README.md`) |
| Excerpted | the sections named by the conformance table, nothing else |

The upstream document is the authority. This excerpt is a frozen copy so the
table cites a versioned text and the bench needs no network access. The wire
layouts below are the 9P2000.L wire format as published by the Plan 9
documentation that diod itself references; they are reproduced here for
reference, with attribution.

---

### Data Structures

> 1, 2, 4, and 8 byte integers, denoted `name[1]`, `name[2]`, `name[4]`,
> and `name[8]` respectively, are represented on the wire in little-endian
> format (least significant byte first).

A string, denoted `name[s]`, is sent on the wire as
```
length[2] string[length]
```
with no terminating NULL.

A _qid_, a 13 byte value representing a unique file system object,
is represented on the wire as:
```
type[1] version[4] path[8]
```

### 9P2000 Operations (.L subset)

#### version -- negotiate protocol version
```
size[4] Tversion tag[2] msize[4] version[s]
size[4] Rversion tag[2] msize[4] version[s]
```
> *version* establishes the `msize`, which is the maximum message size
> inclusive of the size value that can be handled by both client and server.

> It also establishes the protocol version. For 9P2000.L version must be the
> string `9P2000.L`.

#### flush -- abort a message
```
size[4] Tflush tag[2] oldtag[2]
size[4] Rflush tag[2]
```
> flush aborts an in-flight request referenced by oldtag, if any.

#### walk -- descend a directory hierarchy
```
size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s])
size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13])
```
> walk is used to descend a directory represented by fid using successive path
> elements provided in the wname array. If successful, newfid represents the
> new path.

> fid can be cloned to newfid by calling walk with nwname set to zero.

### read, write -- transfer data from and to a file
```
size[4] Tread tag[2] fid[4] offset[8] count[4]
size[4] Rread tag[2] count[4] data[count]

size[4] Twrite tag[2] fid[4] offset[8] count[4] data[count]
size[4] Rwrite tag[2] count[4]
```
> Under 9P2000.L, read cannot be used on directories. See readdir below.

#### clunk -- destroy a fid
```
size[4] Tclunk tag[2] fid[4]
size[4] Rclunk tag[2]
```
> clunk signifies that fid is no longer needed by the client.

### 9P2000.u Operations (.L subset)

#### attach, auth -- messages to establish a connection
```
size[4] Tauth tag[2] afid[4] uname[s] aname[s] n_uname[4]
size[4] Rauth tag[2] aqid[13]

size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4]
size[4] Rattach tag[2] qid[13]
```
> attach introduces a new user to the server, and establishes fid as the root
> for that user on the file tree selected by aname.

> afid can be `P9_NOFID` (~0) or the fid from a previous auth handshake. The
> afid can be clunked immediately after the attach.

> n_uname, if not set to `P9_NONUNAME` (~0), is the uid of the user and is used
> in preference to uname.

### 9P2000.L Operations

#### lerror -- return error code
```
size[4] Rlerror tag[2] ecode[4]
```
> lerror replaces the reply message used in a successful call. ecode is a
> numerical Linux errno.

#### statfs -- get file system information
```
size[4] Tstatfs tag[2] fid[4]
size[4] Rstatfs tag[2] type[4] bsize[4] blocks[8] bfree[8] bavail[8]
                       files[8] ffree[8] fsid[8] namelen[4]
```

#### lopen -- open a file
```
size[4] Tlopen tag[2] fid[4] flags[4]
size[4] Rlopen tag[2] qid[13] iounit[4]
```
> lopen prepares fid for file I/O. flags contains Linux open(2) flags bits,
> e.g. O_RDONLY, O_RDWR, O_WRONLY.

#### lcreate -- create regular file
```
size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4]
size[4] Rlcreate tag[2] qid[13] iounit[4]
```
> lcreate creates a regular file name in directory fid and prepares it for
> I/O.

> fid initially represents the parent directory of the new file. After the
> call it represents the new file.

#### symlink -- create symbolic link
```
size[4] Tsymlink tag[2] fid[4] name[s] symtgt[s] gid[4]
size[4] Rsymlink tag[2] qid[13]
```

#### readlink -- read value of symbolic link
```
size[4] Treadlink tag[2] fid[4]
size[4] Rreadlink tag[2] target[s]
```

#### getattr -- get file attributes
```
size[4] Tgetattr tag[2] fid[4] request_mask[8]
size[4] Rgetattr tag[2] valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8]
                 rdev[8] size[8] blksize[8] blocks[8]
                 atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
                 ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8]
                 gen[8] data_version[8]
```
> Not all fields are valid in every call. request_mask is a bitmask indicating
> which fields are requested. valid is a bitmask indicating which fields are
> valid in the response.

#### setattr -- set file attributes
```
size[4] Tsetattr tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] size[8]
                 atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
size[4] Rsetattr tag[2]
```

#### readdir -- read a directory
```
size[4] Treaddir tag[2] fid[4] offset[8] count[4]
size[4] Rreaddir tag[2] count[4] data[count]
```
> readdir requests that the server return directory entries from the directory
> represented by fid, previously opened with lopen. offset is zero on the
> first call.

> Directory entries are represented as variable-length records:
```
qid[13] offset[8] type[1] name[s]
```
> At most count bytes will be returned in data. If count is not zero in the
> response, more data is available. On subsequent calls, offset is the offset
> returned in the last directory entry of the previous call.

### mkdir - create directory
```
size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4]
size[4] Rmkdir tag[2] qid[13]
```

#### link - create hard link
```
size[4] Tlink tag[2] dfid[4] fid[4] name[s]
size[4] Rlink tag[2]
```

### Sample Session

> The client negotiates the protocol version and msize with version, then
> introduces itself with an attach as root. This attach is for the uname=root
> mount option. The client then runs a getattr on the mount point.

> Next the users opendir triggers a clone (walk with nwnames=0) of fid (1) to
> fid (2), which is used in lopen to open the directory.

> This is followed by a readdir which transfers the entire directory in one
> request. A second readdir returning no data signifies the end of the
> directory, which is then closed, resulting in a clunk of fid 2.

> Now user uid=500 stats a nonexistent file with /bin/ls /tmp/9/foo. This
> triggers an error response (ecode=2, no such file or directory):
```
P9_TWALK tag 1 fid 1 newfid 2 nwname 1 'foo'
P9_RLERROR tag 1 ecode 2
```

> The file is created with echo hello >/tmp/9/foo:
```
P9_TWALK tag 1 fid 1 newfid 2 nwname 0
P9_RWALK tag 1 nwqid 0
P9_TLCREATE tag 1 fid 2 name 'foo' flags 0x8241 mode 0100644 gid 500
P9_RLCREATE tag 1 qid (00000000002c1bd1 0 '') iounit 4096
P9_TWRITE tag 1 fid 2 offset 0 count 6
68656c6c 6f0a
P9_RWRITE tag 1 count 6
P9_TCLUNK tag 1 fid 2
P9_RCLUNK tag 1
```

> A file is read with cat /tmp/9/foo2:
```
P9_TWALK tag 1 fid 6 newfid 5 nwname 0
P9_RWALK tag 1 nwqid 0
P9_TLOPEN tag 1 fid 5 mode 0100000
P9_RLOPEN tag 1 qid (00000000002c1de4 0 '') iounit 0
P9_TGETATTR tag 1 fid 6 request_mask 0x3fff
P9_RGETATTR tag 1 valid 0x3fff qid (00000000002c1de4 0 '') mode 0100644 uid 500 gid 500 nlink 1 rdev 0 size 6 blksize 4096 blocks 8 atime Fri Feb  4 17:57:18 2011 mtime Fri Feb  4 17:57:18 2011 ctime Fri Feb  4 17:57:18 2011 btime 0 gen 0 data_version 0
P9_TREAD tag 1 fid 5 offset 0 count 32768
P9_RREAD tag 1 count 6
68656c6c 6f0a
P9_TREAD tag 1 fid 5 offset 6 count 32768
P9_RREAD tag 1 count 0
P9_TCLUNK tag 1 fid 5
P9_RCLUNK tag 1
```
