# SPDX-License-Identifier: Apache-2.0
"""Markdown to HTML, the subset the Passerelle documentation actually uses.

No third-party dependency: the site must build on a runner that has nothing but
a Python interpreter, and the documentation uses a known subset of Markdown.

Supported: ATX headings, paragraphs, unordered and ordered lists (nested),
fenced code blocks, GFM tables, block quotes, horizontal rules, inline code,
bold, italic, links, images, reference links, raw HTML blocks, HTML comments
(dropped).

Not supported (and not used): footnotes, task lists, setext headings, nested
block quotes, definition lists.
"""

from __future__ import annotations

import html
import re
from dataclasses import dataclass, field

_INLINE_CODE = re.compile(r"`([^`]+)`")
_BOLD = re.compile(r"\*\*([^*]+)\*\*")
_ITALIC = re.compile(r"(?<![*\w])\*([^*\n]+)\*(?!\*)")
_LINK = re.compile(r"\[([^\]]*)\]\(([^)\s]+)(?:\s+\"([^\"]*)\")?\)")
_REF_LINK = re.compile(r"\[([^\]]+)\]\[([^\]]*)\]")
_IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"([^\"]*)\")?\)")
_HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)
_SLUG_STRIP = re.compile(r"[^\w\s-]", re.UNICODE)
_SLUG_SPACE = re.compile(r"[\s_]+", re.UNICODE)


@dataclass
class Doc:
    """A parsed document: an HTML body plus the anchors it defines."""

    body: str = ""
    headings: list[tuple[str, str, int]] = field(default_factory=list)

    @property
    def toc(self) -> list[tuple[str, str, int]]:
        return self.headings


def slugify(text: str) -> str:
    """GitHub-compatible anchor slug."""
    text = _INLINE_CODE.sub(r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = _SLUG_STRIP.sub("", text)
    return _SLUG_SPACE.sub("-", text)


def _escape(text: str) -> str:
    return html.escape(text, quote=False)


def _inline(text: str, refs: dict[str, tuple[str, str]]) -> str:
    """Render the inline constructs of one paragraph."""
    placeholders: list[str] = []

    def stash(markup: str) -> str:
        placeholders.append(markup)
        return f"\x00{len(placeholders) - 1}\x00"

    def on_code(m: re.Match) -> str:
        return stash(f"<code>{_escape(m.group(1))}</code>")

    def on_image(m: re.Match) -> str:
        alt, src, title = m.group(1), m.group(2), m.group(3)
        attr = f' title="{html.escape(title)}"' if title else ""
        return stash(
            f'<img src="{html.escape(src, quote=True)}" '
            f'alt="{html.escape(alt, quote=True)}"{attr}>'
        )

    def on_link(m: re.Match) -> str:
        label, href, title = m.group(1), m.group(2), m.group(3)
        attr = f' title="{html.escape(title)}"' if title else ""
        return stash(f'<a href="{html.escape(href, quote=True)}"{attr}>{_escape(label)}</a>')

    def on_ref(m: re.Match) -> str:
        label, key = m.group(1), (m.group(2) or m.group(1)).lower()
        href, title = refs.get(key, ("#", None))
        attr = f' title="{html.escape(title)}"' if title else ""
        return stash(f'<a href="{html.escape(href, quote=True)}"{attr}>{_escape(label)}</a>')

    text = _IMAGE.sub(on_image, text)
    text = _INLINE_CODE.sub(on_code, text)
    text = _LINK.sub(on_link, text)
    text = _REF_LINK.sub(on_ref, text)
    text = _BOLD.sub(r"<strong>\1</strong>", text)
    text = _ITALIC.sub(r"<em>\1</em>", text)
    text = _escape(text)
    for index, markup in enumerate(placeholders):
        text = text.replace(f"\x00{index}\x00", markup)
    return text


def _split_row(line: str) -> list[str]:
    row = line.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|"):
        row = row[:-1]
    return [cell.strip() for cell in row.split("|")]


def _is_separator(cells: list[str]) -> bool:
    return bool(cells) and all(
        re.fullmatch(r":?-{2,}:?", c.replace(" ", "")) for c in cells
    )


def _render_table(rows: list[list[str]], refs: dict[str, tuple[str, str]]) -> str:
    header, *body = rows
    out = ["<table>", "<thead>", "<tr>"]
    out += [f"<th>{_inline(c, refs)}</th>" for c in header]
    out += ["</tr>", "</thead>", "<tbody>"]
    for row in body:
        out.append("<tr>")
        out += [f"<td>{_inline(c, refs)}</td>" for c in row]
        out.append("</tr>")
    out += ["</tbody>", "</table>"]
    return "\n".join(out)


def _collect_refs(lines: list[str]) -> dict[str, tuple[str, str]]:
    refs: dict[str, tuple[str, str]] = {}
    for line in lines:
        m = re.fullmatch(r'\[([^\]]+)\]:\s*(\S+)(?:\s+"([^"]*)")?', line.strip())
        if m:
            refs[m.group(1).lower()] = (m.group(2), m.group(3))
    return refs


def _collect_list(lines: list[str], start: int) -> tuple[str, int]:
    """Collect one (possibly nested) list starting at `start`."""
    i = start
    ordered = bool(re.match(r"^\d+[.)]\s", lines[i].strip()))
    items: list[str] = []
    while i < len(lines):
        stripped = lines[i].strip()
        if not stripped:
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
            if not nxt or not re.match(r"^([-*+]|\d+[.)])\s", nxt):
                break
            i += 1
            continue
        m = re.match(r"^([-*+]|\d+[.)])\s+(.*)$", stripped)
        if not m:
            break
        if ordered and not re.match(r"^\d+[.)]\s", stripped):
            break
        content = [m.group(2)]
        i += 1
        while i < len(lines):
            nxt = lines[i]
            if not nxt.strip():
                break
            indent = len(nxt) - len(nxt.lstrip())
            if indent == 0:
                break
            content.append(nxt.strip())
            i += 1
        items.append(" ".join(content))
    tag = "ol" if ordered else "ul"
    body = "\n".join(f"<li>{_escape(text)}</li>" for text in items)
    return f"<{tag}>\n{body}\n</{tag}>", i


@dataclass
class _Ctx:
    refs: dict[str, tuple[str, str]]
    seen: dict[str, int]
    headings: list[tuple[str, str, int]]


def _block_code(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    lang = lines[i].strip()[3:].strip()
    i += 1
    code: list[str] = []
    while i < len(lines) and not lines[i].strip().startswith("```"):
        code.append(lines[i])
        i += 1
    i += 1
    cls = f' class="language-{html.escape(lang)}"' if lang else ""
    return f"<pre><code{cls}>{_escape(chr(10).join(code))}</code></pre>", i


def _block_heading(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    m = re.match(r"^(#{1,6})\s+(.*)$", lines[i].strip())
    assert m
    level = len(m.group(1))
    title = m.group(2).strip()
    base = slugify(title)
    count = ctx.seen.get(base, 0)
    ctx.seen[base] = count + 1
    anchor = base if count == 0 else f"{base}-{count}"
    ctx.headings.append((title, anchor, level))
    return f'<h{level} id="{anchor}">{_inline(title, ctx.refs)}</h{level}>', i + 1


def _block_table(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    rows = [_split_row(lines[i])]
    i += 2
    while i < len(lines) and lines[i].strip().startswith("|"):
        rows.append(_split_row(lines[i]))
        i += 1
    return _render_table(rows, ctx.refs), i


def _block_quote(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    quote: list[str] = []
    while i < len(lines) and lines[i].strip().startswith(">"):
        quote.append(lines[i].strip().lstrip(">").strip())
        i += 1
    joined = _inline(" ".join(quote), ctx.refs)
    return f"<blockquote><p>{joined}</p></blockquote>", i


def _block_html(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    block: list[str] = []
    while i < len(lines) and lines[i].strip():
        block.append(lines[i])
        i += 1
    return "\n".join(block), i


def _block_para(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    para: list[str] = []
    while i < len(lines) and lines[i].strip() and not re.match(
        r"^(#{1,6}\s|```|\||>|\s*[-*+]\s|\s*\d+[.)]\s)", lines[i]
    ):
        para.append(lines[i].strip())
        i += 1
    if not para:
        return "", i + 1
    return f"<p>{_inline(' '.join(para), ctx.refs)}</p>", i


def _dispatch(lines: list[str], i: int, ctx: _Ctx) -> tuple[str, int]:
    """Pick the block handler for the line at `i`."""
    stripped = lines[i].strip()
    if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped):
        return "<hr>", i + 1
    if re.match(r"^#{1,6}\s+", stripped):
        return _block_heading(lines, i, ctx)
    if (
        stripped.startswith("|")
        and i + 1 < len(lines)
        and _is_separator(_split_row(lines[i + 1]))
    ):
        return _block_table(lines, i, ctx)
    if stripped.startswith(">"):
        return _block_quote(lines, i, ctx)
    if re.match(r"^[-*+]\s+", stripped) or re.match(r"^\d+[.)]\s+", stripped):
        return _collect_list(lines, i)
    if stripped.startswith("<"):
        return _block_html(lines, i, ctx)
    return _block_para(lines, i, ctx)


def render(markdown: str) -> Doc:
    """Render Markdown into an HTML body plus its heading anchors."""
    lines = _HTML_COMMENT.sub("", markdown).split("\n")
    ctx = _Ctx(refs=_collect_refs(lines), seen={}, headings=[])
    doc = Doc()
    out: list[str] = []
    i = 0
    while i < len(lines):
        if not lines[i].strip():
            i += 1
            continue
        if lines[i].strip().startswith("```"):
            block, i = _block_code(lines, i, ctx)
        else:
            block, i = _dispatch(lines, i, ctx)
        if block:
            out.append(block)
    doc.body = "\n".join(out)
    doc.headings = ctx.headings
    return doc
