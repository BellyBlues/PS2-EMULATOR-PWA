#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

"""Local dev server for TabStationToo, the PCSX2 WebAssembly page.

Serves a build's bin/ directory (index.html, pcsx2-wasm.js/.wasm/.data) on 127.0.0.1 with the
headers that make the page cross-origin isolated, which SharedArrayBuffer (and so the emulator's
threads) requires:

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
    Cross-Origin-Resource-Policy: same-origin

Every response, errors included, carries them, plus Cache-Control: no-store so a rebuilt .wasm is
always picked up.

With --bios-dir DIR the files directly inside DIR are also served, read-only, under /dev-bios/, so
automated runs can open index.html?bios=dev-bios/<file> and boot without picking a file. The server
only ever binds to 127.0.0.1 and never serves anything outside those two directories. Requests whose
Host header is not 127.0.0.1/localhost on this port are refused, so a website cannot use DNS
rebinding to reach the server (and the BIOS files) through its own hostname.

Usage:
    python3 tools/wasm/serve.py [--port 8080] [--bios-dir DIR] [--quiet] [DIRECTORY]
"""

from __future__ import annotations

import argparse
import email.utils
import functools
import http.server
import os
import sys
import urllib.parse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "build-wasm" / "bin"
BIND_ADDRESS = "127.0.0.1"
DEV_BIOS_PREFIX = "/dev-bios/"

EXTRA_HEADERS = (
    ("Cross-Origin-Opener-Policy", "same-origin"),
    ("Cross-Origin-Embedder-Policy", "require-corp"),
    ("Cross-Origin-Resource-Policy", "same-origin"),
    ("Cache-Control", "no-store"),
    ("X-Content-Type-Options", "nosniff"),
)

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".htm": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".map": "application/json",
    ".wasm": "application/wasm",
    ".data": "application/octet-stream",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
    ".md": "text/plain; charset=utf-8",
}

BIOS_USUAL_MIN = 4 * 1024 * 1024
BIOS_USUAL_MAX = 8 * 1024 * 1024


def is_within(path: Path, base: Path) -> bool:
    return path == base or base in path.parents


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    server_version = "pcsx2-wasm-serve"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def __init__(self, *args, root: Path, bios_dir: Path | None, quiet: bool, **kwargs):
        # Set before super().__init__(), which handles the request immediately.
        self.root = root
        self.bios_dir = bios_dir
        self.quiet = quiet
        super().__init__(*args, directory=str(root), **kwargs)

    def version_string(self) -> str:
        return self.server_version

    # Every response goes through end_headers(), including send_error() pages and redirects.
    def end_headers(self) -> None:
        for name, value in EXTRA_HEADERS:
            self.send_header(name, value)
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        if not self.quiet:
            super().log_message(format, *args)

    def list_directory(self, path):  # never list directories
        self.send_error(404, "Not found")
        return None

    def copyfile(self, source, outputfile) -> None:
        try:
            super().copyfile(source, outputfile)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the browser went away mid-download (reload, cancelled fetch)

    def resolve_request(self) -> tuple[Path | None, bool, str | None]:
        """Maps the request path to a file. Returns (file, from_bios_dir, redirect_location)."""
        raw_path = urllib.parse.urlsplit(self.path).path
        try:
            decoded = urllib.parse.unquote(raw_path, errors="strict")
        except UnicodeDecodeError:
            return None, False, None
        if "\x00" in decoded or "\\" in decoded or not decoded.startswith("/"):
            return None, False, None

        if decoded == DEV_BIOS_PREFIX.rstrip("/") or decoded.startswith(DEV_BIOS_PREFIX):
            if self.bios_dir is None:
                return None, True, None
            name = decoded[len(DEV_BIOS_PREFIX):]
            # A plain file name directly inside the BIOS directory: no sub-paths, no dot files.
            if not name or "/" in name or name.startswith("."):
                return None, True, None
            candidate = self.bios_dir / name
            try:
                resolved = candidate.resolve(strict=True)
            except (OSError, RuntimeError):
                return None, True, None
            if resolved.parent != self.bios_dir or not resolved.is_file():
                return None, True, None
            return resolved, True, None

        parts = [p for p in decoded.split("/") if p]
        if any(p in (".", "..") or p.startswith(".") for p in parts):
            return None, False, None
        candidate = self.root.joinpath(*parts)
        try:
            is_dir = candidate.is_dir()
        except OSError:  # e.g. ENAMETOOLONG
            return None, False, None
        if is_dir:
            if not raw_path.endswith("/"):
                return None, False, raw_path + "/"
            candidate = candidate / "index.html"
        try:
            resolved = candidate.resolve(strict=True)
        except (OSError, RuntimeError):
            return None, False, None
        # resolve() follows symlinks, so a link pointing outside the web root is refused here.
        if not is_within(resolved, self.root) or not resolved.is_file():
            return None, False, None
        return resolved, False, None

    def send_head(self):
        host = (self.headers.get("Host") or "").strip().lower()
        if host not in self.server.allowed_hosts:
            self.send_error(421, "Misdirected Request")  # still carries the isolation headers
            return None

        path, from_bios_dir, redirect = self.resolve_request()
        if redirect is not None:
            self.send_response(301)
            self.send_header("Location", redirect)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if path is None:
            self.send_error(404, "Not found")
            return None

        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "Not found")
            return None
        try:
            st = os.fstat(f.fileno())
            if from_bios_dir:
                ctype = "application/octet-stream"
            else:
                ctype = MIME_TYPES.get(path.suffix.lower()) or self.guess_type(str(path))
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(st.st_size))
            self.send_header("Last-Modified", email.utils.formatdate(st.st_mtime, usegmt=True))
            self.end_headers()
            return f
        except Exception:
            f.close()
            raise


def example_bios(bios_dir: Path) -> str | None:
    names = sorted(p.name for p in bios_dir.iterdir()
                   if p.is_file() and not p.name.startswith(".")
                   and BIOS_USUAL_MIN <= p.stat().st_size <= BIOS_USUAL_MAX)
    if "scph39001.bin" in names:
        return "scph39001.bin"
    return names[0] if names else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Serve TabStationToo (the PCSX2 WebAssembly page) on 127.0.0.1 with cross-origin isolation headers.")
    parser.add_argument("directory", nargs="?", default=str(DEFAULT_ROOT),
                        help=f"web root to serve (default: {DEFAULT_ROOT.relative_to(REPO_ROOT)})")
    parser.add_argument("--port", type=int, default=8080, help="port on 127.0.0.1 (default: 8080)")
    parser.add_argument("--bios-dir", metavar="DIR",
                        help="also serve the files directly inside DIR, read-only, under /dev-bios/ "
                             "(for index.html?bios=dev-bios/<file>)")
    parser.add_argument("--quiet", action="store_true", help="do not log each request")
    args = parser.parse_args()

    root = Path(args.directory).expanduser().resolve()
    if not root.is_dir():
        print(f"error: {root} does not exist. Build the page first:\n"
              f"  source ~/emsdk/emsdk_env.sh && ninja -C build-wasm pcsx2-wasm", file=sys.stderr)
        return 1
    if not (root / "index.html").is_file():
        print(f"warning: {root} has no index.html (did the pcsx2-wasm POST_BUILD copy run?)", file=sys.stderr)

    bios_dir = None
    if args.bios_dir:
        bios_dir = Path(args.bios_dir).expanduser().resolve()
        if not bios_dir.is_dir():
            print(f"error: --bios-dir {bios_dir} is not a directory", file=sys.stderr)
            return 1

    handler = functools.partial(IsolatedHandler, root=root, bios_dir=bios_dir, quiet=args.quiet)
    try:
        server = http.server.ThreadingHTTPServer((BIND_ADDRESS, args.port), handler)
    except OSError as e:
        print(f"error: cannot listen on {BIND_ADDRESS}:{args.port}: {e.strerror or e} (try --port)", file=sys.stderr)
        return 1
    server.daemon_threads = True

    port = server.server_address[1]
    allowed_hosts = {f"{BIND_ADDRESS}:{port}", f"localhost:{port}"}
    if port == 80:
        allowed_hosts |= {BIND_ADDRESS, "localhost"}  # browsers omit the default port in Host
    server.allowed_hosts = allowed_hosts
    base = f"http://{BIND_ADDRESS}:{port}/"
    print(f"Serving {root}")
    print(f"  {base}")
    if bios_dir is not None:
        print(f"Dev BIOS directory {bios_dir} (read-only, under {DEV_BIOS_PREFIX})")
        name = example_bios(bios_dir)
        if name:
            # Encoded twice: the page reads it with URLSearchParams (one decode) and then
            # resolves it as a URL path (second decode), so '#', '?' and '%' survive.
            print(f"  {base}index.html?bios=dev-bios/{urllib.parse.quote(urllib.parse.quote(name), safe='')}")
    print("Press Ctrl+C to stop.", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
