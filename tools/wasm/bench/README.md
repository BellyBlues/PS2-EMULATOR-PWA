# Crowd benchmark

`bench.elf` is a small PS2 program, written for TabStationToo, that measures the emulator the way
games load it: no menu, no input, always the same frames. It draws a crowd of animated figures
on a floor, each the way a game draws a character:

- the EE poses a 15-bone skeleton per figure (matrix products in VU0 macro mode) and does a little
  per-figure work (bounding points through VU0, an overlap check);
- one VIF1 DMA chain per frame points at the meshes, and VU1 (`bench_vu1.vsm`) transforms, lights
  and projects every vertex and sends it to the GS with XGKICK;
- the GS draws textured, Gouraud-shaded, Z-buffered triangle strips, with a see-through shadow
  under each figure. A figure is about 2,900 triangles; the floor is 512.

The number of figures steps through 1, 2, 4, 8 and 16, 400 frames each, then starts again. At each
step the program prints `BENCH phase N=<figures> frame=<frame>`. It draws at most one frame per
vsync, like a game, so with the frame limiter off the emulator's speed shows how much faster than
a PS2 it runs; with it on, anything under 100 % is what a player would see as slowdown.

It is a stand-in, not a copy of any game: it does not reproduce everything a game does (no game
logic to speak of, no sound, no streaming, one VU1 program), so a gain here is a gain in the paths
games share with it.

## Running

The emulator still boots a PS2 BIOS first (fast boot), so you need your own BIOS file.

```sh
# Node, no browser:
node build-wasm/bin/pcsx2-headless.js -approot "$PWD/bin" -dataroot "$PWD/build-wasm/data" \
  -bios <bios file> -elf tools/wasm/bench/bench.elf -frames 2000 -renderer sw
# A browser, with the speed of each phase: see run-bench.mjs --help.
node tools/wasm/bench/run-bench.mjs --help
```

## Building

`bench.elf` is committed, so running it needs no PS2 toolchain. To change it, install the
[ps2dev](https://github.com/ps2dev/ps2dev) toolchain (its `ps2dev-macos-latest.tar.gz` or
`ps2dev-linux-latest.tar.gz` release unpacks into `~/ps2dev`; on macOS it also needs
`brew install libmpc gmp mpfr isl`), then:

```sh
export PS2DEV=~/ps2dev PS2SDK=~/ps2dev/ps2sdk
export PATH="$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/dvp/bin"
make -C tools/wasm/bench && make -C tools/wasm/bench clean-objs
```

## Licence

The PS2 program's files (`bench.c`, `bench_vu1.vsm`, `Makefile`) are MIT-licensed, not GPL-3.0+
like the rest of the repository: the program links ps2sdk, which is under the Academic Free
License 2.0, and that licence cannot be combined with the GPL. MIT can be combined with both.
`run-bench.mjs` runs in Node and links nothing from ps2sdk; it is GPL-3.0+.
