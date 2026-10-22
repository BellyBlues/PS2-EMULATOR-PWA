#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Puts the TabStationToo site together for a static host such as GitHub Pages.

Copies the page (every file in pcsx2-wasm/web/), the emulator (pcsx2-wasm.js, .wasm and .data) and
the disc library's title index (game-titles.tsv) from a build's bin/ directory into an empty output directory, and nothing else: no headless runner,
no GS runner, no test or dev files. Then:

- Stamps a build id, a hash of every file's content, into index.html: app.js, style.css,
  isolation.js and icons.svg are loaded with ?v=<id>, and app.js gives the same id to everything it
  loads (pcsx2-wasm.js/.wasm/.data, disc-worker.js). A browser that still caches an older deploy
  therefore never mixes its files with this one's. coi-serviceworker.js keeps its plain URL: a
  service worker is identified by its script's URL, and a new URL on every deploy would install it
  again and reload the page each time.
- Adds .nojekyll, so a branch-based GitHub Pages deploy serves the files as they are.
- Checks the result: every page file is the same as in pcsx2-wasm/web/ (a stale build fails), the
  .data package holds only /resources/ files, no file looks like console or game data (BIOS ROM
  directory, ISO 9660 volume, BIOS, disc or memory card file names and extensions), and the sizes
  stay under GitHub's limits (100 MiB per file in a git push, 1 GiB for a Pages site).

Usage:
    python3 tools/wasm/assemble-site.py [--build-dir build-wasm] [--out _site] [--force]

Exits non-zero, with the reasons, when a check fails. Only Python 3's standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WEB_SOURCE = REPO_ROOT / "pcsx2-wasm" / "web"
EMULATOR_FILES = ("pcsx2-wasm.js", "pcsx2-wasm.wasm", "pcsx2-wasm.data")
# Made by the build next to the page (tools/wasm/make-game-titles.py), for the disc library.
BUILT_FILES = EMULATOR_FILES + ("game-titles.tsv",)
# Files the page must have; the rest of web/ is copied as it is.
REQUIRED = ("index.html", "app.js", "style.css", "isolation.js", "coi-serviceworker.js",
            "coi-serviceworker-LICENSE.txt", "disc-worker.js") + EMULATOR_FILES

MIB = 1024 * 1024
GIB = 1024 * MIB
FILE_LIMIT = 100 * MIB  # GitHub refuses larger files in a push
FILE_WARN = 50 * MIB  # and warns above this
SITE_LIMIT = 1 * GIB  # a published GitHub Pages site may be at most 1 GB

# Console and game data never belongs on the site (AGENTS.md).
FORBIDDEN_SUFFIXES = {
    ".bin", ".rom", ".rom0", ".rom1", ".rom2", ".erom", ".nvm", ".mec",  # BIOS dumps and their parts
    ".iso", ".img", ".chd", ".cso", ".zso", ".gz", ".dump", ".cue", ".mdf", ".nrg",  # disc images
    ".ps2", ".mcd", ".mcr", ".psu", ".max", ".sav", ".p2s",  # memory cards, saves and save states
    ".elf", ".irx", ".zip", ".7z", ".rar",
}
FORBIDDEN_NAME = re.compile(r"scph[-_ ]?\d", re.IGNORECASE)  # how BIOS dumps are usually named
# The first two entries of a PS2 BIOS's ROM directory: name[10], extinfo size (u16), size (u32).
BIOS_ROMDIR = re.compile(rb"RESET\x00{5}[\s\S]{6}ROMDIR\x00{4}")
ISO_SECTOR_SIZES = (2048, 2336, 2352, 2448)
ISO_DATA_OFFSETS = (0, 8, 16, 24)
DATA_PACKAGE_PREFIX = "/resources/"

STAMPED = (  # (attribute value in index.html, how many times it must appear)
    ('src="isolation.js"', 1),
    ('src="app.js"', 1),
    ('href="style.css"', 1),
    ('href="library/disc-library.css"', 1),
    ('src="library/player-library.js"', 1),
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def page_files() -> list[str]:
    """The page: every regular file under pcsx2-wasm/web/, as paths relative to it."""
    names = []
    for path in sorted(WEB_SOURCE.rglob("*")):
        rel = path.relative_to(WEB_SOURCE)
        if any(part.startswith(".") for part in rel.parts):
            continue
        if path.is_file():
            names.append(rel.as_posix())
    return names


def looks_like_disc(data: bytes) -> bool:
    for sector in ISO_SECTOR_SIZES:
        for offset in ISO_DATA_OFFSETS:
            at = 16 * sector + offset
            if data[at:at + 6] == b"\x01CD001":
                return True
    return False


def data_package_problems(js: bytes, data_size: int) -> list[str]:
    """Checks the file packager's manifest in pcsx2-wasm.js against pcsx2-wasm.data. The manifest is
    a JavaScript object literal, loadPackage({files:[{filename:"...",start:0,end:N},...],
    remote_package_size:N}), with or without quoted keys."""
    m = re.search(rb"loadPackage\(\{[\"']?files[\"']?\s*:\s*\[(.*?)\]\s*,(.*?)\}\)", js, re.DOTALL)
    if not m:
        return ["pcsx2-wasm.js: the .data package manifest (loadPackage({files:...})) was not found"]
    names = [n.decode("utf-8", "replace") for n in re.findall(rb"[\"']?filename[\"']?\s*:\s*\"([^\"]*)\"", m.group(1))]
    size = re.search(rb"[\"']?remote_package_size[\"']?\s*:\s*(\d+)", m.group(2))
    problems = []
    if not names:
        problems.append("pcsx2-wasm.js: the .data package manifest lists no files")
    for name in names:
        if not name.startswith(DATA_PACKAGE_PREFIX) or ".." in name:
            problems.append(f"pcsx2-wasm.data holds {name!r}, outside {DATA_PACKAGE_PREFIX}")
        elif Path(name).suffix.lower() in FORBIDDEN_SUFFIXES or FORBIDDEN_NAME.search(Path(name).name):
            problems.append(f"pcsx2-wasm.data holds {name!r}, which looks like console or game data")
    if not size or int(size.group(1)) != data_size:
        expected = size.group(1).decode() if size else "an unknown size"
        problems.append(f"pcsx2-wasm.data is {data_size} bytes but pcsx2-wasm.js expects {expected}")
    return problems


def content_problems(name: str, data: bytes) -> list[str]:
    problems = []
    base = name.rsplit("/", 1)[-1]
    suffix = Path(base).suffix.lower()
    if suffix in FORBIDDEN_SUFFIXES:
        problems.append(f"{name}: {suffix} files are not published (BIOS, disc or save data)")
    if FORBIDDEN_NAME.search(base):
        problems.append(f"{name}: the name looks like a BIOS file")
    if BIOS_ROMDIR.search(data):
        problems.append(f"{name}: holds a PS2 BIOS ROM directory (RESET, ROMDIR)")
    if looks_like_disc(data):
        problems.append(f"{name}: holds an ISO 9660 volume (a disc image)")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="Put the TabStationToo site together for a static host.")
    parser.add_argument("--build-dir", default=str(REPO_ROOT / "build-wasm"),
                        help="the wasm build directory, whose bin/ holds the built page (default: build-wasm)")
    parser.add_argument("--out", default=str(REPO_ROOT / "_site"), help="output directory (default: _site)")
    parser.add_argument("--force", action="store_true", help="replace the output directory if it exists")
    args = parser.parse_args()

    bin_dir = Path(args.build_dir).expanduser().resolve() / "bin"
    out = Path(args.out).expanduser().resolve()
    if not bin_dir.is_dir():
        print(f"error: {bin_dir} does not exist; build the pcsx2-wasm target first", file=sys.stderr)
        return 1
    if out.exists():
        if not args.force:
            print(f"error: {out} exists (use --force to replace it)", file=sys.stderr)
            return 1
        # Only ever replace a directory this script made (it has .nojekyll), never one holding sources.
        if (any(p.is_relative_to(out) for p in (REPO_ROOT, WEB_SOURCE, bin_dir))
                or not (out / ".nojekyll").is_file()):
            print(f"error: refusing to replace {out}: it is not a site made by this script", file=sys.stderr)
            return 1
        shutil.rmtree(out)

    problems: list[str] = []
    page = page_files()
    for name in REQUIRED:
        if name not in page and name not in EMULATOR_FILES:
            problems.append(f"pcsx2-wasm/web/{name} is missing")

    files: dict[str, bytes] = {}
    for name in page + list(BUILT_FILES):
        built = bin_dir / name
        if not built.is_file():
            problems.append(f"{built} is missing")
            continue
        data = built.read_bytes()
        if name in page and data != (WEB_SOURCE / name).read_bytes():
            problems.append(f"{built} differs from pcsx2-wasm/web/{name}: rebuild the pcsx2-wasm target")
        files[name] = data

    if "pcsx2-wasm.js" in files and "pcsx2-wasm.data" in files:
        problems += data_package_problems(files["pcsx2-wasm.js"], len(files["pcsx2-wasm.data"]))
    for name, data in files.items():
        problems += content_problems(name, data)
        if len(data) > FILE_LIMIT:
            problems.append(f"{name}: {len(data) / MIB:.1f} MiB, over GitHub's 100 MiB file limit")
        elif len(data) > FILE_WARN:
            print(f"warning: {name} is {len(data) / MIB:.1f} MiB; GitHub warns about files over 50 MiB")
    total = sum(len(d) for d in files.values())
    if total > SITE_LIMIT:
        problems.append(f"the site is {total / MIB:.0f} MiB, over GitHub Pages' 1 GB limit")

    # The build id: a hash of every file, so it changes exactly when the site does.
    digest = hashlib.sha256()
    for name in sorted(files):
        digest.update(f"{name}\0{sha256(files[name])}\n".encode())
    build_id = digest.hexdigest()[:12]

    if "index.html" in files:
        html = files["index.html"].decode("utf-8")
        for attribute, expected in STAMPED:
            found = html.count(attribute)
            if found != expected:
                problems.append(f"index.html: expected {attribute} {expected} time(s), found {found}")
            name = attribute.split('"')[1]
            html = html.replace(attribute, attribute.replace(name, f"{name}?v={build_id}"))
        if 'src="coi-serviceworker.js"' not in html:
            problems.append('index.html: expected src="coi-serviceworker.js" (unversioned)')
        html = html.replace('"icons.svg#', f'"icons.svg?v={build_id}#')
        files["index.html"] = html.encode("utf-8")

    if problems:
        print("The site was not assembled:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    for name, data in files.items():
        dest = out / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    (out / ".nojekyll").write_bytes(b"")

    width = max(len(n) for n in files)
    for name in sorted(files, key=lambda n: (-len(files[n]), n)):
        print(f"  {name:<{width}}  {len(files[name]) / 1024:>10.1f} KiB")
    print(f"{len(files)} files, {total / MIB:.1f} MiB, build id {build_id}, in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
