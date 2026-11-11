# SPDX-License-Identifier: Apache-2.0
"""Parser for QEMU 9P trace events (PERF-01).

QEMU writes one line per event when started with `-trace 'v9fs_*'`, in the
form:

    <pid>@<seconds>.<micros>:v9fs_read tag 4 id 116 fid 2 off 0 max_count 4096
    <pid>@<seconds>.<micros>:v9fs_read_return tag 4 id 116 count 4096 err 0

This module turns those lines into events and pairs each request with its
reply, which is what the roundtrip table counts. It never guesses: an
unparsable line is reported as a parse error, not silently dropped.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

LINE_RE = re.compile(
    r"^(?:(?P<pid>\d+)@(?P<sec>\d+)\.(?P<usec>\d+):)?"
    r"(?P<event>v9fs_[a-z_]+)\s*(?P<args>.*)$"
)
TAG_RE = re.compile(r"\btag\s+(-?\d+)")
ID_RE = re.compile(r"\bid\s+(-?\d+)")
FID_RE = re.compile(r"\bfid\s+(-?\d+)")
COUNT_RE = re.compile(r"\bcount\s+(-?\d+)")
OFFSET_RE = re.compile(r"\boff\s+(-?\d+)")

# Request name -> POSIX operation it serves (PERF-01 table).
POSIX_OPERATIONS: dict[str, str] = {
    "v9fs_version": "mount (Tversion)",
    "v9fs_attach": "mount (Tattach)",
    "v9fs_walk": "lookup / path resolution",
    "v9fs_getattr": "stat / fstat",
    "v9fs_stat": "stat (legacy)",
    "v9fs_open": "open",
    "v9fs_lcreate": "creat / open(O_CREAT)",
    "v9fs_create": "mkdir / symlink / mknod",
    "v9fs_read": "read",
    "v9fs_write": "write",
    "v9fs_fsync": "fsync",
    "v9fs_clunk": "close",
    "v9fs_remove": "unlink / rmdir",
    "v9fs_rename": "rename",
    "v9fs_link": "link",
    "v9fs_symlink": "symlink",
    "v9fs_readlink": "readlink",
    "v9fs_readdir": "getdents",
    "v9fs_setattr": "truncate / chmod / chown / utime",
    "v9fs_wstat": "wstat (legacy)",
    "v9fs_mkdir": "mkdir",
    "v9fs_mknod": "mknod",
    "v9fs_lock": "fcntl(F_SETLK)",
    "v9fs_getlock": "fcntl(F_GETLK)",
    "v9fs_xattrwalk": "getxattr",
    "v9fs_xattrcreate": "setxattr",
}


# Requests for which QEMU 8.2 has no *_return event at all: the reply is not
# traced, so an occurrence must never be counted as an unanswered request.
# Verified against `qemu-system-x86_64 -trace help` (47 v9fs_* events).
NO_REPLY_EVENTS = frozenset(
    {
        "v9fs_clunk",
        "v9fs_fsync",
        "v9fs_remove",
        "v9fs_flush",
        "v9fs_link",
        "v9fs_wstat",
        "v9fs_xattrcreate",
        "v9fs_rcancel",
    }
)


@dataclass(frozen=True)
class TraceEvent:
    """One parsed trace line."""

    event: str
    tag: int
    msg_id: int
    timestamp: float
    args: str = ""

    @property
    def is_reply(self) -> bool:
        """True for *_return events and for rerror."""
        return self.event.endswith("_return") or self.event == "v9fs_rerror"

    @property
    def has_reply_event(self) -> bool:
        """False for the requests QEMU traces without a reply event."""
        return self.event not in NO_REPLY_EVENTS

    @property
    def request_name(self) -> str:
        """Request name of a reply event, or the event name itself."""
        if self.event.endswith("_return"):
            return self.event[: -len("_return")]
        if self.event == "v9fs_rerror":
            return "v9fs_rerror"
        return self.event

    def integer(self, pattern: re.Pattern[str]) -> int:
        """Extract an integer field from the argument string, or -1."""
        match = pattern.search(self.args)
        return int(match.group(1)) if match else -1


@dataclass
class Exchange:
    """One request and its reply."""

    request: TraceEvent
    reply: TraceEvent | None = None
    error: bool = False

    @property
    def answered(self) -> bool:
        """True when the trace shows a reply, or when QEMU traces none at all."""
        return self.reply is not None or not self.request.has_reply_event

    @property
    def latency_s(self) -> float:
        """Wall time between the request and its reply."""
        if self.reply is None:
            return 0.0
        return max(0.0, self.reply.timestamp - self.request.timestamp)


@dataclass
class TraceLog:
    """Parsed trace: events in order, plus pairing statistics."""

    events: list[TraceEvent] = field(default_factory=list)
    bad_lines: list[str] = field(default_factory=list)

    def exchanges(self) -> list[Exchange]:
        """Pair requests with replies on (tag, id), in request order.

        QEMU names the reply of a few requests with the request name itself
        (clunk, fsync and remove have no `*_return` event), so a repeated
        (tag, id) with the same name is treated as that request's reply.
        """
        pending: dict[tuple[int, int], TraceEvent] = {}
        paired: list[Exchange] = []
        for event in self.events:
            key = (event.tag, event.msg_id)
            if event.is_reply:
                request = pending.pop(key, None)
                if request is None:
                    continue
                paired.append(Exchange(request, event, event.event == "v9fs_rerror"))
                continue
            earlier = pending.get(key)
            if earlier is not None and earlier.event == event.event:
                pending.pop(key)
                paired.append(Exchange(earlier, event))
                continue
            pending[key] = event
        for request in pending.values():
            paired.append(Exchange(request, None))
        return paired

    def request_counts(self) -> dict[str, int]:
        """Number of requests per event name."""
        counts: dict[str, int] = {}
        for event in self.events:
            if not event.is_reply:
                counts[event.event] = counts.get(event.event, 0) + 1
        return counts

    def __len__(self) -> int:
        return len(self.events)


def parse_line(line: str) -> TraceEvent | None:
    """Parse one trace line, or return None when it is not a 9P event."""
    match = LINE_RE.match(line.strip())
    if match is None:
        return None
    seconds = int(match.group("sec") or 0)
    micros = int(match.group("usec") or 0)
    args = match.group("args")
    tag_match = TAG_RE.search(args)
    id_match = ID_RE.search(args)
    return TraceEvent(
        event=match.group("event"),
        tag=int(tag_match.group(1)) if tag_match else -1,
        msg_id=int(id_match.group(1)) if id_match else -1,
        timestamp=seconds + micros / 1_000_000.0,
        args=args,
    )


def parse_trace(text: str) -> TraceLog:
    """Parse a whole trace file content."""
    log = TraceLog()
    for raw in text.splitlines():
        if not raw.strip():
            continue
        event = parse_line(raw)
        if event is None:
            if "v9fs" in raw:
                log.bad_lines.append(raw)
            continue
        log.events.append(event)
    return log


def posix_operation(event_name: str) -> str:
    """Map a 9P request name to the POSIX call it serves."""
    return POSIX_OPERATIONS.get(event_name, "other")
