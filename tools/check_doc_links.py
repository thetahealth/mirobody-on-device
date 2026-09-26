#!/usr/bin/env python3
"""Fail on a relative link in a tracked Markdown file that points nowhere.

The 2026-09 cut deleted most of this tree, and every README that linked into
the deleted part kept pointing at it: the links were retargeted by hand, and
this is what keeps the next deletion from needing the same afternoon. It
checks the file a link names and, for a link into a Markdown file, the
`#anchor` against that file's headings (GitHub's slug rule). Absolute URLs
are not fetched: that is a network check, and CI should not fail because a
vendor moved a page.

Usage: tools/check_doc_links.py   (from anywhere inside the checkout)
"""

from __future__ import annotations

import re
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

ROOT = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())

LINK = re.compile(r"!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML = re.compile(r"""(?:src|href|srcset)\s*=\s*["']([^"']+)["']""")
FENCE = re.compile(r"^\s*(```|~~~)")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")


def _slug(text: str) -> str:
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"`|\*\*|__|\*|~~", "", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def _prose(path: Path) -> list[tuple[int, str]]:
    """Lines outside fenced code blocks, with their line numbers."""
    out, fenced = [], False
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            out.append((n, line))
    return out


@lru_cache(maxsize=None)
def _anchors(path: Path) -> frozenset[str]:
    seen: dict[str, int] = {}
    out = set()
    for _, line in _prose(path):
        m = HEADING.match(line)
        if not m:
            continue
        base = _slug(m.group(2))
        k = seen.get(base, 0)
        seen[base] = k + 1
        out.add(base if k == 0 else f"{base}-{k}")
    for _, line in _prose(path):
        out.update(re.findall(r"""<a\s+(?:name|id)=["']([^"']+)["']""", line))
    return frozenset(out)


def check(md: Path) -> list[str]:
    problems = []
    for n, line in _prose(md):
        line = re.sub(r"`[^`]*`", "", line)
        targets = LINK.findall(line) + [t for v in HTML.findall(line) for t in v.split(",")]
        for raw in targets:
            target = raw.strip().split(" ")[0]
            if not target or re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                continue
            path_part, _, anchor = target.partition("#")
            dest = md if not path_part else (md.parent / path_part).resolve()
            where = f"{md.relative_to(ROOT)}:{n}"
            if not dest.exists():
                problems.append(f"{where}: {target}: no such file")
            elif anchor and dest.suffix == ".md" and anchor.lower() not in _anchors(dest):
                problems.append(f"{where}: {target}: no heading #{anchor}")
    return problems


def main() -> int:
    tracked = subprocess.check_output(["git", "ls-files", "*.md"], cwd=ROOT, text=True).split()
    problems = [p for f in tracked for p in check(ROOT / f)]
    for p in problems:
        print(p)
    print(f"{len(tracked)} files, {len(problems)} broken link(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
