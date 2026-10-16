<img src="pcsx2-wasm/web/favicon.svg" width="44" alt="">

# TabStationToo

TabStationToo runs PlayStation 2 emulation in a browser tab. It is the
[PCSX2](https://github.com/PCSX2/pcsx2) emulator compiled to WebAssembly, with a page of its own
around it. You bring the BIOS from your own console; nothing is uploaded anywhere.

TabStationToo is built on PCSX2 and would not exist without the PCSX2 team and its contributors,
who have developed it for more than twenty years. It is an independent project: it is not
affiliated with, endorsed by or supported by the PCSX2 project or Sony Interactive Entertainment.
Please do not report TabStationToo problems to PCSX2.

## Status

Early and experimental.

- The BIOS boots in the browser: the start-up animation, the console's menus and its settings
  work, driven from the keyboard.
- Games do not run yet: there is no disc support, no audio, no memory cards and no save states.
- Speed: the emulated CPU keeps up, but drawing is done in software. Menus run near full speed;
  heavy scenes, such as the start-up animation, run at a fraction of it for now.
- Desktop Chrome and Edge are the targets. Other browsers may work; phones are not supported yet.

## How it works

- PCSX2's core is compiled with [Emscripten](https://emscripten.org) to WebAssembly. The
  emulated PS2 processors run on PCSX2's interpreters (its recompilers emit x86 or ARM code,
  which a browser cannot run), on threads backed by `SharedArrayBuffer`.
- The PS2's graphics are drawn by PCSX2's software renderer and shown in a `<canvas>`. A WebGPU
  renderer is planned.
- The page (`pcsx2-wasm/web/`) is plain HTML, CSS and JavaScript with no build step and no
  third-party requests. The BIOS you pick stays in your browser (IndexedDB).

The porting plan, with the measurements and decisions behind it, is in
[docs/web-port/PLAN.md](docs/web-port/PLAN.md). The interface design is in
[docs/web-port/UI-SPEC.md](docs/web-port/UI-SPEC.md).

## Build

Requirements: [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) 6.0.12,
CMake and Ninja, and Python 3. The dependencies and a Release build take about 1 GB of disk space,
plus Emscripten itself.

```sh
source ~/emsdk/emsdk_env.sh

# Once: build the third-party libraries for WebAssembly into deps-wasm/
tools/wasm/build-dependencies-wasm.sh deps-wasm

emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF \
  -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" -DCMAKE_FIND_ROOT_PATH="$PWD/deps-wasm" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C build-wasm pcsx2-wasm
```

`build-wasm/bin/` is then the web root: `index.html`, the page's assets, and
`pcsx2-wasm.js`/`.wasm`/`.data`.

## Run

Browsers only allow the emulator's shared-memory threads on a cross-origin isolated page, so
opening `index.html` from disk does not work. Use the included dev server, which sends the
required headers and listens on localhost only:

```sh
python3 tools/wasm/serve.py
```

Open http://127.0.0.1:8080/, choose your BIOS file and press Boot. Click the screen to give it
the keyboard, and press Esc to take the keyboard back. The Keys panel shows the controls.

More detail, including the frontend's API and testing options, is in
[pcsx2-wasm/README.md](pcsx2-wasm/README.md).

## BIOS and games

A PS2 BIOS is copyrighted software. TabStationToo does not include one and never downloads one:
dump it from a console you own (see PCSX2's
[BIOS dumping guide](https://pcsx2.net/docs/setup/bios/)). The same applies to games once disc
support exists. Please do not share BIOS files or game images in issues or anywhere else in this
repository.

## Where things are

| Path | What it is |
|---|---|
| `pcsx2-wasm/` | The browser frontend: `Main.cpp` (the emulator's entry point and the API the page uses) and `web/` (the page) |
| `pcsx2-headless/` | A windowless runner for Node.js, used to test and benchmark without a browser |
| `common/Emscripten/`, `pcsx2/wasm/` | WebAssembly platform code for PCSX2's core |
| `tools/wasm/` | Dependency build script and the dev server |
| `docs/web-port/` | Porting plan and interface design |
| `docs/upstream/` | PCSX2's own guidance kept for reference (its agent guidelines) |
| everything else | PCSX2, with changes for WebAssembly where needed |

## Licence and credits

TabStationToo is free software under the GNU General Public License v3.0, like PCSX2: see
[COPYING.GPLv3](COPYING.GPLv3). Its source is this repository.

- [PCSX2](https://github.com/PCSX2/pcsx2) and its contributors: the emulator at the heart of
  this project. Upstream code keeps its original copyright notices.
- [Archivo](https://github.com/Omnibus-Type/Archivo) and
  [Chivo Mono](https://github.com/Omnibus-Type/Chivo) by Omnibus-Type, SIL Open Font License 1.1.
- [Phosphor Icons](https://phosphoricons.com), MIT License.
- The third-party libraries PCSX2 uses are listed with their licences in
  [bin/docs/ThirdPartyLicenses.html](bin/docs/ThirdPartyLicenses.html).

"PlayStation" and "PS2" are trademarks of Sony Interactive Entertainment Inc. They are used here
only to describe what the software emulates.
