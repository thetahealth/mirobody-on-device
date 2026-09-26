#!/usr/bin/env python3
"""Fail when the Windows export table and the public C ABI disagree.

`src/platform/mirobody.def` is the DLL's export list, and nothing but Windows
reads it, so it drifted: it still listed the first twelve functions after
src/mirobody.h had grown to twenty-four, and a Windows host could not reach the
health store or the on-device model. No CI job links on Windows, so this
compares the two files instead.

Usage: tools/check_exports.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    header = (ROOT / "src" / "mirobody.h").read_text(encoding="utf-8")
    declared = set(re.findall(r"^[A-Za-z].*?\b(mirobody_[a-z_]+)\s*\(", header, re.M))
    body = (ROOT / "src" / "platform" / "mirobody.def").read_text(encoding="utf-8").split("EXPORTS", 1)[1]
    exported = set(re.findall(r"^\s*(mirobody_[a-z_]+)\s*$", body, re.M))
    problems = [f"declared in mirobody.h, not exported: {n}" for n in sorted(declared - exported)]
    problems += [f"exported, not declared in mirobody.h: {n}" for n in sorted(exported - declared)]
    for p in problems:
        print(p)
    print(f"{len(declared)} declared, {len(exported)} exported", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
