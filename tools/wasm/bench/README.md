# Crowd benchmark

`bench.elf` is a small PS2 program, written for TabStationToo, that measures the emulator the way
games load it: no menu, no input, always the same frames. It draws a crowd of animated figures
on a floor, each the way a game draws a character, with the work games do around them:

- the EE runs per-figure game logic (24 dust particles for near figures, a path search on a
  16 x 16 grid every 8 frames per figure, as games spread their AI),
  poses a 15-bone skeleton per figure (matrix products in VU0 macro mode) and builds one VIF1
  DMA chain per frame;
- VU1 (`bench_vu1.vsm`) transforms, lights and projects every vertex and sends it to the GS
  with XGKICK: textured, Gouraud-shaded, Z-buffered triangle strips, with a see-through shadow
  under each figure. The 50 nearest figures have 72 triangles per limb, the others 24 (level of
  detail); the floor is 512. The VU1 code is paired and software-pipelined like a game's, so 300
  figures still fit in one frame on a PS2;
- each figure uses its own palette of an 8-bit texture (a CLUT load): every VU1 batch sends its
  TEX0 ahead of its primitives, as games switch textures inside their VU1 packets; the near
  figures' dust is drawn as sprites the EE positioned, in one PATH2 packet;
- the texture and its four palettes are uploaded again every frame through PATH3;
- a glow pass renders the frame into a half-size target, blurs it twice and adds it back
  (render-to-texture, as games do for glow and blur).

The number of figures steps through 1, 100, 200 and 300, 300 frames each, then starts again. At
each step the program prints `BENCH phase N=<figures> frame=<frame>`, and at the end of a phase
`BENCH load ...`: how many vsyncs a frame took and how the EE's time split between building
frames, waiting for VU1 and the GS, and waiting for vsync (from its cycle counter). One vsync per
frame means a PS2 keeps up; the emulator then has to do all of the phase's work every vsync.

The program draws at most one frame per vsync and sleeps on interrupts while it waits, like a
game, so with the frame limiter off the emulator's speed shows how much faster than a PS2 it runs;
with it on, anything under 100 % is what a player would see as slowdown.

It is a stand-in, not a copy of any game: it does not reproduce everything a game does (no sound,
no streaming from disc, one VU1 program, little game logic), so a gain here is a gain in the paths
games share with it.

## Running

The emulator still boots a PS2 BIOS first (fast boot), so you need your own BIOS file.

```sh
# Node, no browser:
node build-wasm/bin/pcsx2-headless.js -approot "$PWD/bin" -dataroot "$PWD/build-wasm/data" \
  -bios <bios file> -elf tools/wasm/bench/bench.elf -frames 2000 -renderer sw
# A browser, with the speed of each phase: see run-bench.mjs --help.
node tools/wasm/bench/run-bench.mjs --bios <bios file> --elf tools/wasm/bench/bench.elf --unlimited --phases 5
# Chrome with a CPU profile of every thread (here 4 s from vsync 1750, in the 16-figure phase):
node tools/wasm/run-app-headless.mjs --root build-wasm/bin --bios-dir <dir> --bios <file> \
  --elf tools/wasm/bench/bench.elf --query unlimited=1 --gpu plain --chrome <path> \
  --frames 0 --duration 45 --profile 1750:4 --out <dir>
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
