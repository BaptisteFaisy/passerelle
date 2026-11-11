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


def render(markdown: str) -> Doc:
    """Render Markdown into an HTML body plus its heading anchors."""
    text = _HTML_COMMENT.sub("", markdown)
    lines = text.split("\n")
    refs = _collect_refs(lines)
    doc = Doc()
    out: list[str] = []
    seen: dict[str, int] = {}
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            lang = stripped[3:].strip()
            i += 1
            code: list[str] = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            body = _escape("\n".join(code))
            out.append(f"<pre><code{cls}>{body}</code></pre>")
            continue

        if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped):
            out.append("<hr>")
            i += 1
            continue

        m = re.match(r"^(#{1,6})\s+(.*)$", stripped)
        if m:
            level = len(m.group(1))
            title = m.group(2).strip()
            base = slugify(title)
            count = seen.get(base, 0)
            seen[base] = count + 1
            anchor = base if count == 0 else f"{base}-{count}"
            doc.headings.append((title, anchor, level))
            out.append(f'<h{level} id="{anchor}">{_inline(title, refs)}</h{level}>')
            i += 1
            continue

        if (
            stripped.startswith("|")
            and i + 1 < len(lines)
            and _is_separator(_split_row(lines[i + 1]))
        ):
            rows = [_split_row(lines[i])]
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(_split_row(lines[i]))
                i += 1
            out.append(_render_table(rows, refs))
            continue

        if stripped.startswith(">"):
            quote: list[str] = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip().lstrip(">").strip())
                i += 1
            joined = _inline(" ".join(quote), refs)
            out.append(f"<blockquote><p>{joined}</p></blockquote>")
            continue

        if re.match(r"^[-*+]\s+", stripped) or re.match(r"^\d+[.)]\s+", stripped):
            block, i = _collect_list(lines, i)
            out.append(block)
            continue

        if stripped.startswith("<"):
            block = []
            while i < len(lines) and lines[i].strip():
                block.append(lines[i])
                i += 1
            out.append("\n".join(block))
            continue

        para: list[str] = []
        while i < len(lines) and lines[i].strip() and not re.match(
            r"^(#{1,6}\s|```|\||>|\s*[-*+]\s|\s*\d+[.)]\s)", lines[i]
        ):
            para.append(lines[i].strip())
            i += 1
        if para:
            joined = _inline(" ".join(para), refs)
            out.append(f"<p>{joined}</p>")
        else:
            i += 1
    doc.body = "\n".join(out)
    return doc
