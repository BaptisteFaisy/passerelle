<!--
SPDX-License-Identifier: Apache-2.0
-->

# Security policy

Passerelle is a filesystem driver: it runs inside a guest kernel's driver
process, it parses untrusted bytes coming from a server, and its host tools
start privileged daemons. A defect here is not a cosmetic bug, so security
reports are handled privately, before anything else.

**Please do not open a public issue for a security problem.** This project is a
pilot with a small team and no public disclosure channel: a public report is, in
practice, a published exploit for everybody running the driver.

---

## 1. How to report

Use whichever of these reaches you best; all of them are private:

1. **GitHub private vulnerability reporting** - the *Security* tab of
   [`BaptisteFaisy/passerelle`](https://github.com/BaptisteFaisy/passerelle),
   then *Report a vulnerability*. This is the preferred channel: it creates a
   private draft advisory, keeps the discussion out of public view and lets a
   fix ship before the report is published.
2. **A private message to the maintainer** (`@BaptisteFaisy` on GitHub) asking
   for a private channel. Do not describe the vulnerability in the first
   message - ask for the channel, then use it.

There is deliberately no mailing list, no chat room and no public tracker for
this.

### What to include

- the version or the commit (`git rev-parse HEAD`), and how you got the code
  (source tarball, `pip install passerelle-host`, a clone);
- the component: `virtio/`, `proto9p/`, `protofuse/`, `rmfs/`, `drivers/`,
  `host/` (`qshare`, `qrun`, `fsproof`, `pbench`) or the release tooling;
- the smallest reproducer you have: the message bytes, the sequence of
  operations, the command line, the QEMU version and the host kernel;
- what an attacker controls in your scenario - a malicious server, a malicious
  share, a malicious guest, a malicious file on the host - and what they gain;
- whether the defect is reachable from a default configuration
  (`qshare` defaults to `mapped-xattr`, `nosuid`, and an unprivileged
  `virtiofsd`).

Encrypted mail is not required; if you want it, ask for the key through the
private channel first.

---

## 2. What we consider a vulnerability

In scope, with examples of the properties the code is supposed to keep:

- **Memory safety on untrusted input.** Every decoder of `proto9p/` and
  `protofuse/` reads bytes produced by a server the guest does not control.
  A read or write outside a buffer, a length or count that is not validated
  before use, an integer overflow in a size computation (P9-09, ROB-06).
- **Escaping the share.** A path that resolves outside the shared directory, a
  symlink or a `..` component that is not re-validated after a walk, a rename or
  a link target that crosses the boundary (FS-08, FS-09, SEC-01).
- **The permission model.** A mount that accepts writes when it was asked to be
  read-only, `nosuid`/`noexec` not enforced, a mode, owner or capability check
  that can be bypassed, a privilege drop that does not actually drop (QNX-06,
  SEC-01, SEC-02).
- **Resource exhaustion triggered remotely.** Unbounded allocation per request,
  a fid, node or descriptor leak a server can force, a request that never times
  out (ROB-02, ROB-05, P9-07).
- **The host tools.** `qshare --run` builds a QEMU command line and starts
  `virtiofsd`: a shell injection through a directory name, a tag or an option, a
  socket or temporary file created with permissive modes, an option string that
  silently weakens the security model (DX-01, SEC-03, VFS-06).
- **The supply chain of a release.** A tarball whose content does not match its
  published SHA256, a package that installs something the repository does not
  contain, a workflow that leaks a secret (see
  [docs/release.md](docs/release.md)).

Out of scope, unless you can show a concrete path back into Passerelle:

- a vulnerability in QEMU, in `virtiofsd`, in the Linux or QNX kernel, or in a
  third-party project listed in `NOTICE` - report it upstream, we will happily
  carry the workaround;
- a defect that needs physical access to the host, or an already-root attacker
  on the host;
- anything that requires the user to disable the recommended options on purpose;
- the known limitations published in
  [CHANGELOG.md](CHANGELOG.md) and
  [docs/verification-status.md](docs/verification-status.md): the QNX driver has
  never run, because no SDP is available. That is documented, not a secret.

---

## 3. Timelines

Best effort, from a small team - stated so that you know what to expect:

| Step | Target |
| --- | --- |
| Acknowledgement of the report | 3 working days |
| First assessment (in scope? reachable? how severe?) | 10 working days |
| A fix, or a documented workaround, for a confirmed in-scope report | 30 working days |
| Public disclosure | coordinated with you, and not before the fix ships |

If we miss a deadline, tell us in the private thread - or, after 90 days without
any answer, disclose it yourself. Coordinated disclosure at 90 days is the
default expectation, and we will ask for an extension only with a reason.

We will not pursue legal action against a reporter who followed this policy, and
we will credit you in the advisory unless you ask us not to. There is no bug
bounty: this is a hackathon-stage pilot, and the reward is a fixed driver and
your name in the advisory.

---

## 4. Supported versions

| Version | Supported |
| --- | --- |
| `0.1.0` (and the current `main`) | yes - best effort |
| anything older | no - upgrade |

`0.x` means the interfaces can still change; a fix may land as a new PATCH or
MINOR release rather than as a backport. Only the latest release is maintained.

---

## 5. Hardening advice while you run it

- Share a directory that contains only what the guest needs, never a home
  directory or a directory with setuid binaries.
- Keep the defaults: `security_model=mapped-xattr` (no root needed on the host),
  `nosuid`, and a read-only mount when the guest only reads.
- Treat the server as untrusted: the client validates every message, but a
  malicious host is the threat model, not a supported use case.
- Verify what you download: every release publishes the SHA256 of its source
  tarball, in the artefacts and in the release body
  ([docs/release.md](docs/release.md) §5).
