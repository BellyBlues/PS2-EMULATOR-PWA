#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Writes game-titles.tsv for the disc library page (pcsx2-wasm/web/library.html).

One line per serial in PCSX2's game database, "SERIAL<TAB>Title<TAB>Region", so the page can name a
disc from the serial in its SYSTEM.CNF before anything boots. The English name is used where the
database has one (the page's fonts cover Latin only). About 630 KB, 165 KB compressed.

Usage:
    python3 tools/wasm/make-game-titles.py bin/resources/GameIndex.yaml OUTPUT.tsv
"""

import re
import sys

SERIAL_RE = re.compile(r"^([A-Z0-9]{2,6}-[0-9A-Z]+):\s*$")
FIELD_RE = re.compile(r'^  (name|name-en|region):\s*"?(.*?)"?\s*$')


def main(src: str, dst: str) -> None:
    games: dict[str, dict[str, str]] = {}
    current = None
    with open(src, encoding="utf-8") as f:
        for line in f:
            m = SERIAL_RE.match(line)
            if m:
                current = games.setdefault(m.group(1), {})
                continue
            if line[:1] not in (" ", "\t"):
                current = None
                continue
            m = FIELD_RE.match(line)
            if m and current is not None:
                current[m.group(1)] = m.group(2).replace("\t", " ")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        for serial, game in games.items():
            title = game.get("name-en") or game.get("name")
            if title:
                f.write(f"{serial}\t{title}\t{game.get('region', '')}\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
