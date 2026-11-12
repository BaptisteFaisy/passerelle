# SPDX-License-Identifier: Apache-2.0
"""One configuration file shared by qshare, qrun, fsproof and pbench (gap 99).

The four host tools used to re-read their own options: a tag, a timeout or a
client name set for one tool meant nothing to the others. This module is the
single place where an option is resolved, and every tool goes through it:

    command line  >  environment  >  configuration file  >  built-in default

The file is an INI document read with `configparser`, one section per tool
plus a `[common]` section every tool reads:

    $XDG_CONFIG_HOME/passerelle/passerelle.ini   (~/.config/passerelle/...)
    $PASSERELLE_CONFIG                           (an explicit path wins)

    [common]                 ; applies to all four tools
    tag = projet
    proto = virtiofs
    timeout = 60

    [qshare]                 ; overrides [common] for qshare only
    driver-bin = /opt/passerelle/sbin/passerelle

    [qrun]
    share = ~/projet

    [fsproof]
    workers = 8

    [pbench]
    scale = 0.1

The environment overrides the file, one variable per setting, named
`PASSERELLE_<SECTION>_<KEY>` (upper case, dashes replaced by underscores):
`PASSERELLE_COMMON_TAG=projet`, `PASSERELLE_QRUN_SHARE=~/projet`. A tool
section wins over `[common]` at both levels.

A CLI registers its configurable options with `default=None`; `apply()` then
fills the ones the user did not type and records the ones the user did type,
so `--show-config` reports the effective value *and* where it came from.

Covers DX-01..DX-04 (the four tools read one configuration) and gap 99.
Standard library only: argparse, configparser, os, pathlib.
"""

from __future__ import annotations

import argparse
import configparser
import os
import pathlib
from dataclasses import dataclass
from typing import Mapping

from pgwutil import ValidationError, dump_json

APP_NAME = "passerelle"
CONFIG_FILE = "passerelle.ini"
CONFIG_PATH_ENV = "PASSERELLE_CONFIG"
XDG_ENV = "XDG_CONFIG_HOME"
ENV_PREFIX = "PASSERELLE"
COMMON = "common"
TOOLS = ("qshare", "qrun", "fsproof", "pbench")

# Where a value came from, in increasing precedence.
FROM_DEFAULT = "default"
FROM_FILE = "file"
FROM_ENV = "env"
FROM_CLI = "command line"

TRUE_WORDS = ("1", "true", "yes", "on")


@dataclass(frozen=True)
class Setting:
    """One configurable option: its key, its type and its built-in default."""

    key: str
    default: str
    kind: str = "str"
    help: str = ""

    @property
    def dest(self) -> str:
        """argparse destination of the option (dashes become underscores)."""
        return self.key.replace("-", "_")

    def cast(self, text: str):
        """Convert a textual value; a bad value is a readable error."""
        try:
            if self.kind == "int":
                return int(text.strip())
            if self.kind == "float":
                return float(text.strip())
            if self.kind == "bool":
                return text.strip().lower() in TRUE_WORDS
            return text
        except ValueError as err:
            raise ValidationError(
                f"configuration value for '{self.key}' must be {self.kind}, got {text!r}"
            ) from err


# The defaults live here and nowhere else: qshare.options imports DEFAULT_TAG
# and RECOMMENDED_SECURITY from this table, so a value set once applies to the
# library, to the four CLIs and to `--show-config`.
COMMON_SETTINGS: tuple[Setting, ...] = (
    Setting("tag", "hostshare", help="mount tag of the share"),
    Setting("proto", "9p", help="share protocol: 9p or virtiofs"),
    Setting("security", "mapped-xattr", help="QEMU security model (SEC-03)"),
    Setting("mount-point", "/host", help="guest mount point"),
    Setting("driver", "passerelle", help="guest driver name"),
    Setting("client", "passerelle", help="client under test (fsproof, pbench)"),
    Setting("timeout", "120", kind="float", help="seconds to wait for a guest answer"),
)

TOOL_SETTINGS: dict[str, tuple[Setting, ...]] = {
    "qshare": (
        Setting(
            "driver-bin",
            "/opt/passerelle/sbin/passerelle",
            help="absolute host path of the driver binary",
        ),
        Setting("arch", "x86_64", help="guest architecture for --run"),
    ),
    "qrun": (
        Setting("share", ".", help="shared directory"),
        Setting("console", "", help="guest console unix socket served by QEMU"),
    ),
    "fsproof": (
        Setting("regressions", "regressions", help="regression corpus directory"),
        Setting("seed", "1", kind="int", help="seed of the differential test"),
        Setting("count", "200", kind="int", help="operations of the differential test"),
        Setting("ops", "10000", kind="int", help="operations per exercise run"),
        Setting("workers", "1", kind="int", help="concurrent exercise workers"),
        Setting("exercise-seed", "2026", kind="int", help="seed of the content exerciser"),
    ),
    "pbench": (
        Setting("scale", "1.0", kind="float", help="1.0 = specification size"),
        Setting("level", "socle", help="threshold level of the report"),
        Setting("output", "bench_report", help="report output prefix"),
    ),
}

# Built-in defaults, by key: the single source of truth for the whole tree.
DEFAULTS: dict[str, str] = {
    setting.key: setting.default
    for group in (COMMON_SETTINGS, *(TOOL_SETTINGS.values()))
    for setting in group
}


def settings_for(tool: str) -> tuple[Setting, ...]:
    """Settings *tool* reads: the common ones, then its own section."""
    return COMMON_SETTINGS + TOOL_SETTINGS.get(tool, ())


def default_path(environ: Mapping[str, str] | None = None) -> pathlib.Path:
    """Path of the configuration file this user's environment points at."""
    env = os.environ if environ is None else environ
    explicit = env.get(CONFIG_PATH_ENV, "").strip()
    if explicit:
        return pathlib.Path(explicit).expanduser()
    base = env.get(XDG_ENV, "").strip()
    if not base:
        home = env.get("HOME", "").strip() or str(pathlib.Path.home())
        base = str(pathlib.Path(home).expanduser() / ".config")
    return pathlib.Path(base).expanduser() / APP_NAME / CONFIG_FILE


def env_var(section: str, key: str) -> str:
    """Environment variable that overrides *key* of *section*."""
    return f"{ENV_PREFIX}_{section}_{key}".upper().replace("-", "_")


def _read_file(path: pathlib.Path) -> configparser.ConfigParser:
    """Parse the INI file; a malformed one is an error the user can act on."""
    parser = configparser.ConfigParser(interpolation=None)
    try:
        parser.read(path, encoding="utf-8")
    except (configparser.Error, OSError, UnicodeDecodeError) as err:
        raise ValidationError(f"cannot read {path}: {err}") from err
    return parser


def _known_keys(tool: str) -> set[str]:
    """Every key *tool* understands, so a typo can be reported."""
    return {setting.key for setting in settings_for(tool)}


def _file_layer(parser: configparser.ConfigParser, tool: str) -> dict[str, tuple[str, str]]:
    """Values of the file, as key -> (value, section); the tool section wins."""
    layer: dict[str, tuple[str, str]] = {}
    for section in (COMMON, tool):
        if not parser.has_section(section):
            continue
        for key, value in parser.items(section):
            layer[key] = (value, section)
    return layer


def _env_layer(
    environ: Mapping[str, str], tool: str, settings: tuple[Setting, ...]
) -> dict[str, tuple[str, str]]:
    """Values of the environment; the tool variable beats the common one."""
    layer: dict[str, tuple[str, str]] = {}
    for setting in settings:
        for section in (COMMON, tool):
            value = environ.get(env_var(section, setting.key))
            if value is not None:
                layer[setting.key] = (value, env_var(section, setting.key))
    return layer


class Config:
    """Effective configuration of one tool, with the source of every value."""

    def __init__(
        self,
        tool: str,
        path: pathlib.Path,
        settings: tuple[Setting, ...],
        values: dict[str, str],
        sources: dict[str, str],
        provenance: dict[str, str] | None = None,
        unknown: tuple[str, ...] = (),
    ) -> None:
        self.tool = tool
        self.path = path
        self.settings = settings
        self.unknown = unknown
        self._values = values
        self._sources = sources
        self._provenance = provenance or {}

    def has(self, key: str) -> bool:
        """True when *key* is a setting this tool reads."""
        return key in self._values

    def get(self, key: str, fallback: str | None = None) -> str:
        """Textual value of *key*, after the whole precedence chain."""
        if key not in self._values:
            if fallback is None:
                raise ValidationError(f"{self.tool} has no configuration key '{key}'")
            return fallback
        return self._values[key]

    def typed(self, key: str):
        """Value of *key* converted to the type of its setting."""
        setting = self.setting(key)
        return setting.cast(self._values[key])

    def setting(self, key: str) -> Setting:
        """The Setting object behind *key*."""
        for setting in self.settings:
            if setting.key == key:
                return setting
        raise ValidationError(f"{self.tool} has no configuration key '{key}'")

    def source(self, key: str) -> str:
        """Where the value of *key* came from: default, file, env or CLI."""
        return self._sources.get(key, FROM_DEFAULT)

    def provenance(self, key: str) -> str:
        """Source of *key* with its origin: the section or the variable."""
        detail = self._provenance.get(key)
        return f"{self.source(key)}: {detail}" if detail else self.source(key)

    def note_cli(self, key: str, value: object) -> None:
        """Record that the command line supplied *value* for *key*."""
        if key in self._values:
            self._values[key] = str(value)
            self._sources[key] = FROM_CLI
            self._provenance.pop(key, None)

    def rows(self) -> list[tuple[Setting, str, str]]:
        """(setting, effective value, provenance) for every key, in order."""
        return [(setting, self._values[setting.key], self.provenance(setting.key))
                for setting in self.settings]

    def as_dict(self) -> dict[str, object]:
        """Machine-readable view, used by `--show-config json`."""
        return {
            "tool": self.tool,
            "file": str(self.path),
            "file_exists": self.path.is_file(),
            "unknown_keys": list(self.unknown),
            "precedence": [FROM_CLI, FROM_ENV, FROM_FILE, FROM_DEFAULT],
            "values": {
                setting.key: {
                    "value": self._values[setting.key],
                    "source": self.source(setting.key),
                    "provenance": self.provenance(setting.key),
                    "kind": setting.kind,
                    "help": setting.help,
                }
                for setting in self.settings
            },
        }


def load(
    tool: str,
    path: pathlib.Path | str | None = None,
    environ: Mapping[str, str] | None = None,
) -> Config:
    """Resolve the configuration of *tool* from the file and the environment.

    The command line is applied later, by `apply()`, because only the parsed
    namespace knows which options the user actually typed.
    """
    env = os.environ if environ is None else environ
    cfg_path = default_path(env) if path is None else pathlib.Path(path).expanduser()
    settings = settings_for(tool)
    values = {setting.key: setting.default for setting in settings}
    sources = {setting.key: FROM_DEFAULT for setting in settings}
    provenance: dict[str, str] = {}
    unknown: list[str] = []
    layers: list[tuple[dict[str, tuple[str, str]], str]] = []
    if cfg_path.is_file():
        file_layer = _file_layer(_read_file(cfg_path), tool)
        unknown = sorted(key for key in file_layer if key not in _known_keys(tool))
        layers.append((file_layer, FROM_FILE))
    layers.append((_env_layer(env, tool, settings), FROM_ENV))
    for layer, origin in layers:
        for key, (value, where) in layer.items():
            if key not in values:
                continue
            values[key] = value
            sources[key] = origin
            provenance[key] = where
    return Config(tool, cfg_path, settings, values, sources, provenance, tuple(unknown))


def apply(namespace: argparse.Namespace, config: Config) -> Config:
    """Fill the options argparse left unset, and record the ones it did not.

    A configurable option is registered with ``default=None``: argparse then
    leaves it alone when the user did not type it, which is exactly the
    information the precedence rule needs.
    """
    for setting in config.settings:
        dest = setting.dest
        if not hasattr(namespace, dest):
            continue
        given = getattr(namespace, dest)
        if given is None:
            setattr(namespace, dest, config.typed(setting.key))
        else:
            config.note_cli(setting.key, given)
    return config


def help_text(key: str, text: str) -> str:
    """Help line of a configurable option: it names the configuration default.

    One implementation for the four CLIs, so `--help` and `--show-config`
    can never disagree about a built-in default.
    """
    return f"{text} (default: configuration '{key}' = {DEFAULTS[key]!r})"


def add_show_config(parser: argparse.ArgumentParser) -> None:
    """Register `--show-config` (one implementation, four tools)."""
    parser.add_argument(
        "--show-config",
        nargs="?",
        const="text",
        choices=("text", "json"),
        metavar="FORMAT",
        default=None,
        help="print the effective configuration (text or json) and exit",
    )


def render(config: Config, fmt: str = "text") -> str:
    """Human-readable or JSON view of the effective configuration."""
    if fmt == "json":
        return dump_json(config.as_dict())
    return "\n".join(_render_text(config))


def _render_text(config: Config) -> list[str]:
    """Table of every setting, its effective value and its origin."""
    exists = config.path.is_file()
    lines = [
        f"{APP_NAME} configuration for {config.tool}",
        f"file: {config.path}" + ("" if exists else " (absent: defaults and environment only)"),
        "",
        f"  {'KEY':<15} {'VALUE':<36} SOURCE",
    ]
    for setting, value, source in config.rows():
        shown = value if value else "(empty)"
        lines.append(f"  {setting.key:<15} {shown:<36} {source}")
    lines += [
        "",
        f"precedence: {FROM_CLI} > {FROM_ENV} ({env_var(config.tool, 'KEY')}) "
        f"> {FROM_FILE} > {FROM_DEFAULT}",
    ]
    if config.unknown:
        lines.append(f"ignored unknown key(s) of the file: {', '.join(config.unknown)}")
    return lines
