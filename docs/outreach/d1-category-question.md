<!--
SPDX-License-Identifier: Apache-2.0
-->

# D1 — which category? (draft to post on the hackathon Discord)

The question is ours to ask, and the answer decides which judges see the
project. It is tracked as issue #7. **Nothing is posted without the user's
agreement**: this file is the draft, not the message.

## Why we ask

The QNX DDK, which the rules use to define the *Low-level Software* category,
covers the series, I2C, SPI, HID, touch, networking and CAN. Our driver is a
filesystem over virtio: it uses none of those parts of the DDK. It is, however,
a **real QNX resource manager**: it attaches to the pathname space, serves
`iofunc` messages, runs each request at the priority of its caller, uses
`InterruptAttachThread()` (QNX 8.0 removed `InterruptAttach()`), drives the PCI
server for MSI-X, and drops to the minimum capabilities instead of running as
root.

## The message, in English

> **Question about the submission category — filesystem driver over virtio (9P / virtiofs)**
>
> Hello, I am submitting a QNX guest driver that shares a host directory with a
> QNX 8.0 guest under QEMU, over virtio (9P2000.L and virtiofs). The driver is
> a resource manager: it attaches to the pathname space, serves `iofunc` messages,
> runs each request at the priority of its caller, uses `InterruptAttachThread()`
> since `InterruptAttach()` was removed in QNX 8.0, programs MSI-X through the PCI
> server, and drops to the minimum capabilities instead of running as root.
>
> The QNX DDK — which the rules use to define the *Low-level Software* category —
> covers the series, I2C, SPI, HID, touch, networking and CAN. A filesystem over
> virtio uses none of those parts, so I am unsure whether it belongs to *Low-level
> Software* or to *Developer Tools*.
>
> I read the category as "a driver that uses the DDK"; a filesystem resource
> manager does not use the DDK at all, but it is still low-level QNX work
> (message passing, priority inheritance, capabilities, interrupt threads,
> kernel trace).
>
> Would you count it as *Low-level Software*, or should I submit it as
> *Developer Tools*? If the answer depends on something I can show (the source,
> a trace of the requests, the security policy), I am happy to provide it.
>
> Repository: https://github.com/Tradoooo/passerelle

## What to do with the answer

| Answer | Consequence for the project |
|---|---|
| *Low-level Software* | The submission leans on the QNX-specific mechanics: message passing at the caller's priority (QNX-03), interrupt threads (VIO-04), capabilities and the security policy (QNX-06), kernel trace (QNX-09), the resource manager itself (QNX-01). The demo and the codelab become the evidence. |
| *Developer Tools* | The submission leans on the loop it removes (compile on the host, run in the guest, no copy) and on the bench: `qshare`, `fsproof`, `pbench`. The QNX mechanics become implementation detail rather than the selling point. |

Either way the repository, the README and the video script stay the same; only
the emphasis of the first minute of the video changes, and
`docs/video-script.md` already carries both emphases.
