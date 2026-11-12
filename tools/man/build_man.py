#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the man pages of the host tools from their argparse definitions.

Gap 98 (DX-05): `qshare`, `qrun`, `fsproof` and `pbench` ship a manual page
each, committed under `man/`. The single source of truth is the CLI itself:
this script imports each tool's `argparse` parser (the same `_parser()` the
entry point uses) and renders NAME, SYNOPSIS, DESCRIPTION, OPTIONS, EXIT
STATUS, FILES and SEE ALSO. The help text is never retyped, so a page cannot
drift from the option it documents - and `--check`, run by the CI, fails a
pull request whose committed page is stale, exactly like
`tools/site/build_site.py --check` does for the website.

Determinism: the version comes from the repository's VERSION file, and the
page date is pinned in the committed `tools/man/PAGE_DATE` file (bumped when
the CLI or VERSION changes; SOURCE_DATE_EPOCH overrides it). A git-derived
date was rejected: `git log -1 -- VERSION` answers the last VERSION-touching
commit in a full clone but the HEAD date in the CI's shallow one, so the two
would disagree and --check would fail on an up-to-date tree.

Usage:
    python3 tools/man/build_man.py            # write man/<tool>.1
    python3 tools/man/build_man.py --check    # fail on drift (CI)
    python3 tools/man/build_man.py --out DIR  # write elsewhere
"""

from __future__ import annotations

import argparse
import datetime
import importlib
import os
import pathlib
import re
import sys
import textwrap

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
HOST = REPO / "host"
MAN = REPO / "man"

# prog name -> module exposing the argparse factory `_parser()`.
COMMANDS = {
    "qshare": "qshare.cli",
    "qrun": "qshare.qrun",
    "fsproof": "fsproof.cli",
    "pbench": "bench.cli",
}

# Prose that argparse cannot know: extra DESCRIPTION paragraphs, exit codes
# (read off each `main()`), the files a tool touches, and cross-references.
META: dict[str, dict[str, list[str]]] = {
    "qshare": {
        "description": [
            "qshare prints the exact QEMU command-line options that share a host "
            "directory with a QNX 8.0 guest over 9P2000.L or virtiofs, and can start "
            "QEMU (and virtiofsd) itself with --run. It also emits the mkqnximage "
            "snippets that install the guest driver, writes the CMake and Rust "
            "example projects with --example, and replays the whole demonstration "
            "with --demo.",
            "Inside the guest, the share is mounted with the passerelle driver at "
            "the --mount-point location; qrun(1) then runs programs from it, and "
            "fsproof(1) and pbench(1) prove and measure it.",
            "Three subcommands answer the everyday questions: qshare doctor says "
            "what is missing on this machine, qshare init scaffolds a guest "
            "project ready to be shared, and qshare smoke proves the whole chain "
            "here (configure, build, ctest, pytest, the reference benches and the "
            "guest check).",
        ],
        "exit": [
            "0 - success (options printed, snippets written, run or demo finished)",
            "2 - usage error, or invalid input (missing directory, bad tag, ...)",
            "3 - host environment error (QEMU or virtiofsd missing, run failed)",
        ],
        "files": [
            "<VM_DIR>/local/snippets/ - mkqnximage fragments written by --mkqnximage",
            "<share>/cmake_example/, <share>/rust_example/ - projects written by --example",
            "<share>/README.md, <share>/qemu-options.txt, <share>/guest-setup.txt - "
            "the project written by 'qshare init'",
            "/tmp/pgw-qemu-console.log - default guest console log of --run",
        ],
        "see_also": ["qrun(1)", "fsproof(1)", "pbench(1)", "qemu(1)", "virtiofsd(1)"],
    },
    "qrun": {
        "description": [
            "qrun runs a program that lives in the shared directory, either inside "
            "the QNX guest - by talking to the QEMU console socket given with "
            "--console - or on the PC itself with --local. It waits for the program "
            "to finish (bounded by --timeout), replays its stdout and stderr, and "
            "returns its exit code, which makes it usable from scripts and from "
            "ctest-like loops.",
        ],
        "exit": [
            "0 - the program ran and returned 0",
            "2 - usage error, missing share directory, console or host error",
            "any other value - the exit code of the program that was run",
        ],
        "files": [
            "<share> - the shared directory (--share), read-only for qrun itself",
            "--console PATH - the QEMU console unix socket (-serial unix:<path>)",
        ],
        "see_also": ["qshare(1)", "fsproof(1)", "pbench(1)", "qemu(1)"],
    },
    "fsproof": {
        "description": [
            "fsproof is the filesystem conformance bench of sections 7.2-7.4 of the "
            "specification: `diff` replays a seeded pseudo-random operation "
            "sequence against the PC truth and stores every divergence as a "
            "regression case, `exercise` runs the content exerciser of section 7.3, "
            "`cases` runs the directed cases of section 7.4, and `clients` lists the "
            "client profiles that can be put under test.",
        ],
        "exit": [
            "0 - every check passed",
            "1 - at least one divergence, regression or directed case failed",
            "2 - usage error or host environment error",
        ],
        "files": [
            "regressions/ - divergence cases stored by `diff` (--regressions)",
            "--json FILE - optional JSON result of `diff`",
        ],
        "see_also": ["qshare(1)", "qrun(1)", "pbench(1)"],
    },
    "pbench": {
        "description": [
            "pbench runs the measurements of sections 7.5-7.6 of the specification: "
            "`roundtrips` counts the 9P roundtrips per POSIX call from a QEMU "
            "trace, the workload subcommands measure throughput, metadata, tree "
            "walks, mixed load, program launches and priority reads, `mount` "
            "measures the guest mount time, `all` writes one JSON conformance "
            "file, and `report` aggregates runs into Markdown, JSON and SVG and "
            "checks the section 7.6 thresholds.",
        ],
        "exit": [
            "0 - the command ran, and the report verdict is PASS",
            "1 - the report verdict is FAIL (a section 7.6 threshold is missed)",
            "2 - usage error or host environment error",
        ],
        "files": [
            "bench_report.md, .json, .svg - default outputs of `report` (--output)",
            "--json FILE - JSON result of a workload or of `all`",
            "--markdown FILE - Markdown table of `roundtrips`",
        ],
        "see_also": ["qshare(1)", "qrun(1)", "fsproof(1)"],
    },
}

# Actions that never take a value on the command line.
_FLAG_ACTIONS = (
    argparse._StoreTrueAction,  # noqa: SLF001 - argparse has no public marker
    argparse._StoreFalseAction,  # noqa: SLF001
    argparse._CountAction,  # noqa: SLF001
    argparse._HelpAction,  # noqa: SLF001
    argparse._VersionAction,  # noqa: SLF001
)


def repo_version() -> str:
    """Version of the pages: the content of the repository's VERSION file."""
    return (REPO / "VERSION").read_text(encoding="utf-8").strip()


def source_date() -> str:
    """Page date: SOURCE_DATE_EPOCH, else the pinned tools/man/PAGE_DATE.

    The date lives in a committed file, not in the git history: a shallow
    clone (what the CI checks out) cannot answer "when did VERSION last
    change", and a wall-clock date would make regeneration differ from one
    day to the next. Bump PAGE_DATE when the CLI help or VERSION changes.
    """
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        stamp = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
        return stamp.date().isoformat()
    pinned = HERE / "PAGE_DATE"
    if pinned.is_file():
        return pinned.read_text(encoding="utf-8").strip()
    print("warning: no tools/man/PAGE_DATE, using today", file=sys.stderr)
    return datetime.date.today().isoformat()


def load_parser(prog: str) -> argparse.ArgumentParser:
    """Import the tool's module and build the very parser its entry uses."""
    if str(HOST) not in sys.path:
        sys.path.insert(0, str(HOST))
    module = importlib.import_module(COMMANDS[prog])
    return module._parser()  # noqa: SLF001 - the tool's own factory


def esc(text: str) -> str:
    """Escape free text for roff: backslashes, then hyphens."""
    return text.replace("\\", "\\e").replace("-", "\\-").replace("\n", " ")


def takes_value(action: argparse.Action) -> bool:
    """True when the option expects a value on the command line."""
    return not isinstance(action, _FLAG_ACTIONS) and action.nargs != 0


def value_word(action: argparse.Action) -> str:
    """Italic placeholder for the value of *action*."""
    word = action.metavar or action.dest.upper()
    if action.nargs == argparse.REMAINDER:
        return f"\\fI{esc(word)}\\fR ..."
    return f"\\fI{esc(word)}\\fR"


def help_body(action: argparse.Action) -> str:
    """Help text of *action*, completed with choices and default."""
    text = esc(action.help or "")
    choices = getattr(action, "choices", None)
    if choices and "choices" not in text and not isinstance(choices, range):
        text += f" (choices: {esc(', '.join(str(c) for c in choices))})"
    default = action.default
    skip = default is None or default is argparse.SUPPRESS or default == ""
    skip = skip or (isinstance(default, (list, tuple, dict)) and not default)
    skip = skip or isinstance(action, _FLAG_ACTIONS)
    if not skip and "default" not in text.lower():
        text += f" (default: {esc(str(default))})"
    return text


def subparsers_of(parser: argparse.ArgumentParser) -> list:
    """[(name, help, sub-parser)] of *parser*, empty when it has none."""
    for action in parser._actions:  # noqa: SLF001 - documented argparse tree
        if isinstance(action, argparse._SubParsersAction):  # noqa: SLF001
            helps = {a.dest: a.help or "" for a in action._choices_actions}  # noqa: SLF001
            return [(n, helps.get(n, ""), p) for n, p in action.choices.items()]
    return []


def option_entry(action: argparse.Action) -> list[str]:
    """One .TP entry: the option in bold, then its wrapped help."""
    if action.option_strings:
        parts = []
        for opt in action.option_strings:
            piece = f"\\fB{esc(opt)}\\fR"
            if takes_value(action):
                piece += f" {value_word(action)}"
            parts.append(piece)
        label = ", ".join(parts)
    else:
        label = value_word(action)
    lines = [".TP", label]
    body = help_body(action)
    if body:
        lines += textwrap.wrap(body, 78) or [""]
    return lines


def synopsis(parser: argparse.ArgumentParser, prog: str) -> list[str]:
    """SYNOPSIS lines: prog, option summary, then the positional words."""
    lines = [".B " + prog, ".RI [ OPTION ]..."]
    subs = subparsers_of(parser)
    for action in parser._actions:  # noqa: SLF001
        if action.option_strings or isinstance(action, argparse._SubParsersAction):  # noqa: SLF001
            continue
        word = value_word(action)
        if action.nargs == "?":
            lines.append(f".RI [ {word} ]")
        else:
            lines.append(word)
    if subs:
        lines += [".I COMMAND", ".RI [ COMMAND\\-OPTION ]..."]
    return lines


def options_section(parser: argparse.ArgumentParser, prog: str) -> list[str]:
    """OPTIONS: every option of *parser*, then one .SS block per subcommand."""
    lines: list[str] = []
    subs = subparsers_of(parser)
    sub_names = {name for name, _, _ in subs}
    for action in parser._actions:  # noqa: SLF001
        if isinstance(action, argparse._SubParsersAction):  # noqa: SLF001
            continue
        lines += option_entry(action)
    for name, help_text, sub in subs:
        lines += [f".SS {prog} {name}", ".TP", f"\\fB{esc(name)}\\fR"]
        lines += textwrap.wrap(esc(help_text or sub.description or ""), 78) or [""]
        for sub_action in sub._actions:  # noqa: SLF001
            if sub_action.dest in sub_names or isinstance(
                sub_action, argparse._SubParsersAction  # noqa: SLF001
            ):
                continue
            lines += option_entry(sub_action)
    return lines


def tagged_items(items: list[str], italic: bool = False) -> list[str]:
    """Render 'label - text' items as a roff tagged list (.TP)."""
    font = "\\fI%s\\fR" if italic else "\\fB%s\\fR"
    lines: list[str] = []
    for item in items:
        label, _, text = item.partition(" - ")
        lines += [".TP", font % esc(label.strip())]
        lines += textwrap.wrap(esc(text.strip()), 74) or [""]
    return lines


def render_page(prog: str, parser: argparse.ArgumentParser, version: str, date: str) -> str:
    """Render the whole man page of *prog* as roff."""
    meta = META[prog]
    short = re.sub(r"\s*\([^)]*\)\s*$", "", parser.description or prog)
    lines = [
        '.\\" SPDX-License-Identifier: Apache-2.0',
        f'.TH "{prog.upper()}" "1" "{date}" "passerelle {version}" "Passerelle host tools"',
        ".SH NAME",
        f"{prog} \\- {esc(short)}",
        ".SH SYNOPSIS",
        *synopsis(parser, prog),
        ".SH DESCRIPTION",
    ]
    paras = [p for p in [parser.description or "", *meta["description"]] if p]
    for index, para in enumerate(paras):
        lines += textwrap.wrap(esc(para), 78)
        if index < len(paras) - 1:
            lines.append(".P")
    lines += [".SH OPTIONS", *options_section(parser, prog), ".SH EXIT STATUS"]
    lines += tagged_items(meta["exit"])
    lines += [".SH FILES", *tagged_items(meta["files"], italic=True)]
    lines += [".SH SEE ALSO", esc(", ".join(meta["see_also"]))]
    return "\n".join(lines) + "\n"


def generate() -> dict[str, str]:
    """prog -> rendered page text, for every command."""
    version, date = repo_version(), source_date()
    return {
        prog: render_page(prog, load_parser(prog), version, date) for prog in COMMANDS
    }


def write_pages(out: pathlib.Path) -> int:
    """Write every page under *out* and say what was written."""
    out.mkdir(parents=True, exist_ok=True)
    for prog, text in generate().items():
        path = out / f"{prog}.1"
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path.relative_to(REPO) if REPO in path.parents else path}")
    return 0


def check(out: pathlib.Path) -> int:
    """Compare the generated pages with the committed ones (CI gate)."""
    drift: list[str] = []
    pages = generate()
    committed = {p.stem for p in out.glob("*.1")} if out.is_dir() else set()
    for prog, text in pages.items():
        path = out / f"{prog}.1"
        if not path.exists():
            drift.append(f"missing page: {prog}.1")
        elif path.read_text(encoding="utf-8") != text:
            drift.append(f"outdated page: {prog}.1")
    for name in sorted(committed - set(pages)):
        drift.append(f"stale page: {name}.1")
    for item in drift:
        print(f"::error::man drift: {item}", file=sys.stderr)
    if drift:
        print("regenerate with: python3 tools/man/build_man.py", file=sys.stderr)
        return 1
    print("man pages are up to date")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point of the generator."""
    parser = argparse.ArgumentParser(
        prog="build_man.py",
        description="generate the host tools' man pages from their argparse definitions",
    )
    parser.add_argument("--out", default=str(MAN), help="output directory (default: man/)")
    parser.add_argument("--check", action="store_true", help="fail when a committed page drifts")
    args = parser.parse_args(argv)
    out = pathlib.Path(args.out)
    return check(out) if args.check else write_pages(out)


if __name__ == "__main__":
    raise SystemExit(main())
