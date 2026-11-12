#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the Passerelle website: a static site, no dependency, no build server.

The documentation lives in Markdown inside the repository; this script renders
it into a flat static site that GitHub Pages serves from `site/`. It is
deliberately dependency-free (standard library only), so the Pages publication
needs nothing but a Python interpreter, and the site can be built and previewed
locally:

    python3 tools/site/build_site.py --out /tmp/site
    python3 -m http.server -d /tmp/site 8000

Internal links are rewritten on the way: a link to another document becomes a
link to its page, a link to a file that is not a page becomes a link to GitHub.
A link to a document that does not exist is an error, so the build fails rather
than shipping a broken site.
"""

from __future__ import annotations

import argparse
import html
import json
import pathlib
import posixpath
import re
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

from markdown import Doc, render, slugify  # noqa: E402

GITHUB = "https://github.com/BaptisteFaisy/passerelle"
BLOB = f"{GITHUB}/blob/main"

# Ordered navigation. "section" groups the entries in the sidebar.
PAGES: list[tuple[str, str, str]] = [
    ("Home", "docs/home.md", "Start"),
    ("Overview", "README.md", "Start"),
    ("User guide", "docs/user-guide.md", "Use"),
    ("Architecture", "docs/architecture.md", "Use"),
    ("Building", "docs/building.md", "Use"),
    ("CI and merge process", "docs/ci.md", "Use"),
    ("State of the art", "docs/state-of-the-art.md", "Project"),
    ("Decisions", "docs/decisions.md", "Project"),
    ("Verification status", "docs/verification-status.md", "Project"),
    ("Measurements", "docs/measurements/README.md", "Project"),
    ("Linux 9P baseline", "docs/measurements/linux-baseline-9p.md", "Project"),
    ("Gap analysis", "docs/gap-analysis.md", "Project"),
    ("Video script", "docs/video-script.md", "Project"),
    ("Codelab", "codelab/README.md", "Learn"),
    ("Codelab 01 - environment", "codelab/01-environment-and-first-image.md", "Learn"),
    ("Codelab 02 - the PCI bus", "codelab/02-the-pci-bus-under-qnx-8.md", "Learn"),
    ("Codelab 03 - finding a device", "codelab/03-finding-a-modern-virtio-device.md", "Learn"),
    ("Codelab 04 - features and status", "codelab/04-features-and-the-status-sequence.md", "Learn"),
    ("Codelab 05 - the virtqueue", "codelab/05-the-split-virtqueue.md", "Learn"),
    ("Codelab 06 - interrupts", "codelab/06-interrupts.md", "Learn"),
    ("Codelab 07 - first 9P exchange", "codelab/07-the-first-9p-exchange.md", "Learn"),
    ("Codelab 08 - to a resource manager", "codelab/08-from-prototype-to-resource-manager.md", "Learn"),
    ("Upstream contributions", "upstream/README.md", "Project"),
    ("Specification (EN)", "docs/spec-en.md", "Reference"),
    ("Specification (FR)", "docs/spec-fr.md", "Reference"),
]

EXTRA_GLOBS = ("docs/submission/*.md", "docs/adr/*.md", "docs/*.md", "docs/release.md", "docs/development.md")


def page_slug(source: str) -> str:
    """Flat, stable URL name for a source document."""
    path = pathlib.PurePosixPath(source)
    parent = path.parent.name or "."
    if path.stem == "home":
        return "index"
    if path.stem == "README":
        return "overview" if parent == "." else parent
    if parent not in ("docs", "."):
        return f"{parent}-{path.stem}"
    return path.stem


def discover_extra() -> list[tuple[str, str, str]]:
    """Pages added elsewhere, picked up without editing this file."""
    known = {source for _, source, _ in PAGES}
    found: list[tuple[str, str, str]] = []
    seen: set[str] = set(known)
    for pattern in EXTRA_GLOBS:
        for path in sorted(REPO.glob(pattern)):
            rel = path.relative_to(REPO).as_posix()
            if rel in seen:
                continue
            seen.add(rel)
            title = path.stem.replace("-", " ").replace("_", " ").title()
            found.append((title, rel, "Project"))
    return found


def collect_pages() -> list[tuple[str, str, str, str]]:
    """(title, source, section, slug) for every page that exists."""
    pages: list[tuple[str, str, str, str]] = []
    seen: set[str] = set()
    slugs: dict[str, str] = {}
    for title, source, section in PAGES + discover_extra():
        if not (REPO / source).is_file() or source in seen:
            continue
        seen.add(source)
        slug = page_slug(source)
        if slug in slugs:
            raise SystemExit(
                f"two documents map to the page '{slug}': "
                f"{slugs[slug]} and {source}"
            )
        slugs[slug] = source
        pages.append((title, source, section, slug))
    return pages


def rewrite_links(body: str, source: str, by_source: dict[str, str]) -> tuple[str, list[str]]:
    """Point links at pages or at GitHub; report the broken ones."""
    broken: list[str] = []

    def fix(match: re.Match) -> str:
        attr, value = match.group(1), match.group(2)
        if value.startswith(("http://", "https://", "#", "mailto:", "data:")):
            return match.group(0)
        target, _, anchor = value.partition("#")
        if not target:
            return match.group(0)
        start = target if target.startswith("/") else posixpath.join(posixpath.dirname(source), target)
        resolved = posixpath.normpath(start).lstrip("./")
        if resolved in by_source:
            url = by_source[resolved]
        elif (REPO / resolved).is_file():
            url = f"{BLOB}/{resolved}"
        elif (REPO / resolved).is_dir():
            url = f"{GITHUB}/tree/main/{resolved}"
        else:
            broken.append(f"{source} -> {value}")
            return match.group(0)
        if anchor:
            url = f"{url}#{anchor}"
        return f'{attr}="{html.escape(url, quote=True)}"'

    body = re.sub(r'(href|src)="([^"]+)"', fix, body)
    return body, broken


def build_nav(pages: list[tuple[str, str, str, str]]) -> str:
    out: list[str] = []
    current = None
    for title, _, section, slug in pages:
        if section != current:
            current = section
            out.append(f'<p class="nav-section">{html.escape(section)}</p>')
        out.append(
            f'<a class="nav-item" data-title="{html.escape(title.lower())}" '
            f'href="{slug}.html">{html.escape(title)}</a>'
        )
    return "\n".join(out)


def page_html(title: str, body: str, nav: str, section: str, toc: list[str],
              prev_link: str, next_link: str) -> str:
    toc_html = ""
    if toc:
        items = "\n".join(
            f'<li class="toc-l{level}"><a href="#{anchor}">{html.escape(text)}</a></li>'
            for text, anchor, level in toc
        )
        toc_html = f'<nav class="toc"><p class="toc-title">On this page</p><ul>{items}</ul></nav>'
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} - Passerelle</title>
<meta name="description" content="Passerelle: a shared folder for QNX 8.0 guests under QEMU, over virtio (9P2000.L and virtiofs).">
<link rel="stylesheet" href="_assets/style.css">
<link rel="icon" href="_assets/favicon.svg" type="image/svg+xml">
</head>
<body>
<header class="topbar">
  <a class="brand" href="index.html"><span class="brand-mark">◈</span> Passerelle</a>
  <nav class="topnav">
    <a href="overview.html">Overview</a>
    <a href="user-guide.html">Use it</a>
    <a href="verification-status.html">Status</a>
    <a href="gap-analysis.html">Gaps</a>
    <a href="https://github.com/BaptisteFaisy/passerelle">GitHub</a>
  </nav>
</header>
<div class="layout">
  <aside class="sidebar">
    <input id="filter" type="search" placeholder="Filter pages" aria-label="Filter pages">
    {nav}
  </aside>
  <main class="content">
    <p class="crumb">{html.escape(section)}</p>
    {body}
    <nav class="pager">
      <a class="prev" href="{prev_link}">Previous</a>
      <a class="next" href="{next_link}">Next</a>
    </nav>
    <footer>
      <p>Passerelle - Apache-2.0. Built for the QNX Global Hackathon 2026.
      Sources on <a href="https://github.com/BaptisteFaisy/passerelle">GitHub</a>.</p>
    </footer>
  </main>
</div>
<script src="_assets/search.js"></script>
</body>
</html>
"""


def write_assets(out: pathlib.Path) -> None:
    """Copy the static assets next to the generated pages."""
    assets = out / "_assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name in ("style.css", "search.js", "favicon.svg"):
        source = HERE / "assets" / name
        if source.exists():
            shutil.copy2(source, assets / name)


def clean_generated(out: pathlib.Path) -> None:
    """Remove what a previous build produced, and nothing else.

    The generated pages live next to their Markdown sources (GitHub Pages only
    serves the repository root or ``/docs``), so the cleaner must be precise: it
    removes the generated HTML, the asset directory and the two index files, and
    never touches a ``.md``.
    """
    for path in out.glob("*.html"):
        path.unlink()
    for name in ("index.json", "sitemap.txt"):
        candidate = out / name
        if candidate.exists():
            candidate.unlink()
    assets = out / "_assets"
    if assets.is_dir():
        shutil.rmtree(assets)


def check_against_committed(out: pathlib.Path) -> int:
    """Regenerate in a scratch directory and compare with what is committed.

    Used by the CI job: a pull request that edits the documentation without
    regenerating the site must fail instead of shipping a stale website.
    """
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        scratch = pathlib.Path(tmp) / "docs"
        scratch.mkdir(parents=True, exist_ok=True)
        clean_generated(scratch)
        saved = sys.argv
        sys.argv = [saved[0], "--out", str(scratch)]
        try:
            if main() != 0:
                return 1
        finally:
            sys.argv = saved
        drift: list[str] = []
        generated = sorted(p.name for p in scratch.glob("*.html"))
        committed = sorted(p.name for p in out.glob("*.html"))
        for name in set(generated) | set(committed):
            a, b = scratch / name, out / name
            if not a.exists():
                drift.append(f"stale page: {name}")
            elif not b.exists():
                drift.append(f"missing page: {name}")
            elif a.read_bytes() != b.read_bytes():
                drift.append(f"outdated page: {name}")
        gen_assets = scratch / "_assets"
        com_assets = out / "_assets"
        if not com_assets.is_dir() or any(
            (gen_assets / f).read_bytes() != (com_assets / f).read_bytes()
            for f in [p.name for p in gen_assets.iterdir()]
            if (com_assets / f).exists()
        ):
            drift.append("assets differ")
    for item in drift:
        print(f"::error::site drift: {item}", file=sys.stderr)
    if drift:
        print("regenerate with: python3 tools/site/build_site.py", file=sys.stderr)
        return 1
    print("site is up to date")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=str(REPO / "docs"))
    parser.add_argument("--strict-links", action="store_true", default=True)
    parser.add_argument("--check", action="store_true",
                        help="regenerate in a scratch directory and report any "
                             "drift with the committed site")
    args = parser.parse_args()

    out = pathlib.Path(args.out).resolve()
    if args.check:
        return check_against_committed(out)
    out.mkdir(parents=True, exist_ok=True)
    # Only what a previous build produced is removed: the Markdown sources live
    # next to the generated pages and must survive.
    clean_generated(out)
    pages = collect_pages()
    if not pages:
        print("no page found", file=sys.stderr)
        return 1
    by_source = {source: f"{slug}.html" for _, source, _, slug in pages}
    nav = build_nav(pages)
    index: list[dict] = []
    broken: list[str] = []

    for position, (title, source, section, slug) in enumerate(pages):
        text = (REPO / source).read_text(encoding="utf-8")
        doc: Doc = render(text)
        body, page_broken = rewrite_links(doc.body, source, by_source)
        broken += page_broken
        toc = [h for h in doc.toc if h[2] <= 3]
        prev_link = pages[position - 1][3] + ".html" if position else "index.html"
        next_link = pages[position + 1][3] + ".html" if position + 1 < len(pages) else "index.html"
        (out / f"{slug}.html").write_text(
            page_html(title, body, nav, section, toc, prev_link, next_link),
            encoding="utf-8",
        )
        index.append({
            "title": title, "url": f"{slug}.html", "section": section,
            "source": source,
            "headings": [{"text": t, "anchor": a, "level": l} for t, a, l in toc],
        })

    write_assets(out)
    (out / "_assets" / "index.json").write_text(
        json.dumps(index, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    print(f"{len(pages)} pages written to {out}")
    if broken:
        print("broken links:", file=sys.stderr)
        for item in sorted(set(broken)):
            print(f"  {item}", file=sys.stderr)
        if args.strict_links:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())