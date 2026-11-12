# Running the fsproof guest agent inside a guest

**Gap 97** — "the `host/fsproof` guest agent has no way to run inside a guest
yet (needs a cross-compiler)". This page establishes what the agent really
needs, and ships the part that can be shipped today.

## 1. What the agent is

`host/fsproof/guest_agent.py` is a **single Python 3 file** that uses only the
standard library and imports nothing from the rest of the repository: it can
be copied to the guest on its own. It reads a JSON *sequence* of operations,
replays them in a work directory, and writes a JSON *snapshot* of the tree it
sees.

The host side is `GuestReplayer` in `host/fsproof/diff.py`: it writes the
sequence next to the work directory and appends three options to the guest
command:

```
<guest command> --work <dir> --sequence <file> --snapshot <file>
```

So the guest command must be a program that accepts exactly these three
options. That is the whole interface, and it is now fixed.

## 2. What it really needs: an interpreter, not a cross-compiler

The gap text says the agent "needs a cross-compiler". That is the wrong
diagnosis: **the agent is not compiled**. It needs a `python3` (3.9 or later)
*inside the guest*, and:

- QNX Neutrino 8.0 ships no Python interpreter, and QNX publishes no CPython
  port for Neutrino;
- the QNX 7.1.0 guest used by our CI has no toolchain at all — see
  [qnx71-guest.md](qnx71-guest.md) §2 — so it has no interpreter either.

**Verdict.** With today's guest images the agent cannot run inside a QNX
guest, and the blocker is *an interpreter in the guest*, not a cross-compiler.
A cross-compiler is only needed for the second option below.

## 3. The stable interface, ready to run today

`host/fsproof/agent.sh` is the guest-side entry point. It takes the three
options above and resolves, in order: `PGW_AGENT_BIN` (a native agent built
for the guest), `PGW_PYTHON`, `python3`, `python` — the last three driving
`guest_agent.py`, which sits next to it. When nothing can run the bench it
exits 3 and says what is missing.

```bash
# Once per share: put the entry point and the agent in the share.
cp host/fsproof/agent.sh host/fsproof/guest_agent.py "$SHARE"/

# In the guest (the share is /host in the default image):
sh /host/agent.sh --work /host/.fsproof-work \
                   --sequence /host/seq.json \
                   --snapshot /host/snap.json

# On the host: the bench appends the three options itself.
python3 -m fsproof diff --seed 7 --count 200 \
    --guest-cmd "qrun --console /tmp/qnx-console.sock /host/agent.sh"
```

The sequence and the snapshot must live **inside the share**: the bench writes
them next to the work directory and reads the snapshot back from the same
place, and the guest and the PC then look at one and the same directory — that
is what makes the PC folder the truth.

## 4. The two ways to get an interpreter into the guest

| | What it needs | State |
|---|---|---|
| **(a) A Python 3.9+ interpreter in the image** | A cross-build of CPython for the guest target: SDP 8.0, `qcc -V gcc_ntox86_64` (x86_64 guest) or `qcc -V gcc_ntoaarch64le` (ARM64 guest). CPython has no official Neutrino support, so this is a port, not a build. | Not done: no SDP on this machine, and no public Neutrino port to reuse. |
| **(b) A native agent written in C** | The SDP 8.0 toolchain for the guest target, and a C implementation of the same three options and the same JSON snapshot format. | Not done: the C source does not exist yet. The interface above is fixed precisely so that it can be dropped in unchanged. |

Either way, `PGW_AGENT_BIN` (or `PGW_PYTHON`) is the switch that tells the
entry point what to run, and the host side of the bench does not change.

## 5. What is proven here, and what is not

**Proven on this PC** — `host/tests/test_fsproof_guest_agent.py`:

- the entry point runs the real agent and produces the expected snapshot;
- it forwards the three options the differential test appends;
- it works whatever the working directory is (the guest runs it from the share);
- `PGW_AGENT_BIN` is honoured, which is the slot a native agent takes;
- with no interpreter and no native agent it exits 3 with a diagnosis instead
  of failing silently.

**Not proven:**

- that the agent runs *inside* a QNX guest — no QNX guest with an interpreter
  exists on this machine, and none exists in CI;
- that the QNX 7.1.0 CI guest can run the bench: it has no Python at all;
- the native C agent: it does not exist, and it cannot be written and built
  here without an SDP (option (b) above).

The bench itself, on the PC, is already covered by the differential test of
[docs/verification-status.md](verification-status.md); what is missing is only
the last hop — an interpreter inside the guest.
