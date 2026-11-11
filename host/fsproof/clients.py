# SPDX-License-Identifier: Apache-2.0
"""Client profiles for the objective comparison (PERF-02, PERF-07, VFS-05).

The conformance bench is only worth something if it can drive someone
else's implementation with the same cases. A profile says how to bring a
client up and how to run the bench against it; nothing here executes
anything by itself, so the table stays readable and testable.
"""

from __future__ import annotations

from dataclasses import dataclass

DEFAULT_TAG = "hostshare"
DEFAULT_MOUNT = "/mnt/host"


@dataclass(frozen=True)
class ClientProfile:
    """How to reach one filesystem client from the PC."""

    name: str
    label: str
    transport: str
    mount_command: str
    agent_command: str
    notes: str

    def guest_command(self, share: str) -> list[str]:
        """Command line that runs the guest agent for this client."""
        return [self.agent_command, "--share", share]


PROFILES: dict[str, ClientProfile] = {
    "passerelle": ClientProfile(
        name="passerelle",
        label="Passerelle (this project)",
        transport="virtio-pci (9P2000.L) / virtiofs",
        mount_command=f"passerelle {DEFAULT_TAG} {DEFAULT_MOUNT}",
        agent_command="/host/.fsproof/guest_agent.py",
        notes="Reference driver: 9P2000.L over virtio-pci, virtiofs at the extreme level",
    ),
    "qnx-fs-9p": ClientProfile(
        name="qnx-fs-9p",
        label="Project A: fs-9p (ARM64, virtio-mmio)",
        transport="virtio-mmio",
        mount_command=f"fs-9p {DEFAULT_TAG} {DEFAULT_MOUNT}",
        agent_command="/mnt/host/.fsproof/guest_agent.py",
        notes="aarch64 only; same bench, same seeds, so the comparison is objective",
    ),
    "eclipse": ClientProfile(
        name="eclipse",
        label="Project B: eclipse-score fs-virtio9p (PCI, MSI-X)",
        transport="virtio-pci",
        mount_command=f"mount_virtio9p -o transport=pci none {DEFAULT_MOUNT}",
        agent_command="/mnt/host/.fsproof/guest_agent.py",
        notes="Auto-discovers the 9P device on the PCI bus; MSI-X with INTx fallback",
    ),
}

ALIASES = {"a": "qnx-fs-9p", "b": "eclipse", "this": "passerelle"}


def profile_names() -> list[str]:
    """Names accepted by --client."""
    return sorted(PROFILES)


def get_profile(name: str) -> ClientProfile:
    """Resolve a profile name (aliases a/b/this are accepted)."""
    key = name.strip().lower()
    key = ALIASES.get(key, key)
    if key not in PROFILES:
        raise KeyError(f"unknown client '{name}'; choose from {', '.join(profile_names())}")
    return PROFILES[key]


def describe_profiles() -> str:
    """Render the client table for --list-clients."""
    lines = ["client profiles (PERF-07):"]
    for name in profile_names():
        profile = PROFILES[name]
        lines.append(f"  {name:12s} {profile.label}")
        lines.append(f"               transport: {profile.transport}")
        lines.append(f"               mount    : {profile.mount_command}")
        lines.append(f"               notes    : {profile.notes}")
    return "\n".join(lines)
