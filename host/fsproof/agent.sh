#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Guest-side entry point of the fsproof differential bench.
#
# One stable command line for whatever runs the bench inside the guest, so the
# host never has to know what the guest can execute:
#
#     agent.sh --work <dir> --sequence <file> --snapshot <file>
#
# Resolution order: PGW_AGENT_BIN (a native agent built for the guest), then
# PGW_PYTHON, then python3, then python, driving the Python agent that lives
# next to this script (guest_agent.py). Exit status 3 means "nothing here can
# run the bench", and says so on stderr; see docs/fsproof-guest-agent.md.

case $0 in
    */*) dir=${0%/*} ;;
    *) dir=. ;;
esac

if [ -n "${PGW_AGENT_BIN}" ]; then
    exec "${PGW_AGENT_BIN}" "$@"
fi

if [ -n "${PGW_PYTHON}" ]; then
    exec "${PGW_PYTHON}" "${dir}/guest_agent.py" "$@"
fi

for interpreter in python3 python; do
    if command -v "${interpreter}" >/dev/null 2>&1; then
        exec "${interpreter}" "${dir}/guest_agent.py" "$@"
    fi
done

echo "fsproof guest agent: this guest cannot run the bench." >&2
echo "  the agent is a Python 3 program: ${dir}/guest_agent.py" >&2
echo "  no python3 or python was found on PATH, and PGW_AGENT_BIN is unset." >&2
echo "  QNX Neutrino ships no Python interpreter: either add one to the guest" >&2
echo "  image, or set PGW_AGENT_BIN to a native agent for this target." >&2
echo "  See docs/fsproof-guest-agent.md." >&2
exit 3
