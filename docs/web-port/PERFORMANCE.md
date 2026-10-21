# Performance: what has been optimized, and what is in progress

The running record of the speed work on TabStationToo: every optimization, where it lives, what it
gained and how it was checked, then what is being worked on now and what is known to be left.
Update it with every optimization (newest entries at the bottom of each list). Numbers are from an
Apple M1 Max unless noted; "speed" is the emulator's speed in percent of a real PS2, with the frame
limiter off when it is above 100 %.

## How speed is measured

- **Crowd benchmark** (`tools/wasm/bench/`): a PS2 program with no menu that draws 1, 100, 200 and
  300 animated figures the way games draw characters. Even at 300 figures a real PS2 draws every
  frame in one vsync, so the emulator's speed measures only the emulator.
  `run-bench.mjs` runs it in Zen or Chrome and reports each phase's speed and thread load.
- **Real games, silently in Node** (`pcsx2-headless`, Emscripten's Node 24, no sound or window):
  Guitar Hero into a song and Naruto Shippuden Ultimate Ninja 5's menus and intro, driven by
  scripted button presses. Game data never enters the repository.
- **The page**, while playing:
  - `?threads=1` adds how busy the EE, GS and VU1 threads are to the speed readout. The thread
    near 100 % is the one holding a game back.
  - **Recorder** (same switch): records every vsync's speed, thread load, JIT compiling and GPU
    pipeline creation. Crop to a drop, then Copy or Download.

## Where things started

The page ran the emulator at about 10-20 % in fights (interpreters, software renderer), and in Zen
always on the software renderer. The JITs and the WebGPU renderer then existed, but games such as
Guitar Hero and Naruto still ran under 50 % in Zen and 80-90 % in Chrome.

PCSX2's own speed comes from things a browser does not allow:
- recompilers that write machine code;
- memory tricks (`mmap`, page protection);
- hand-written SSE/AVX.

TabStationToo replaces them with WebAssembly JITs, software checks and SIMD128, and pays browser
costs PCSX2 never had: waking threads goes through JavaScript, and GPU pipelines compile in the
browser.

## Optimizations so far

Every optimization keeps emulation exact unless its row says otherwise. Each was checked with the JIT
fuzzers, lockstep against the interpreters, EE/IOP state hashes, and pixel-identical screenshots, as
noted. Changes inside PCSX2's own code are guarded for WebAssembly (`ARCH_WASM` or
`__EMSCRIPTEN__`) unless noted, so desktop builds are unaffected.

### Browser and page

| What | Where | Gain | Commit |
|---|---|---|---|
| WebGPU in Firefox and Zen. Firefox drops a WebGPU canvas's configuration when a frame is handed to the page, so only the first frame showed and the page used the software renderer in Firefox. The canvas is now reconfigured after each frame where needed. | `pcsx2/GS/Renderers/WebGPU/webgpu_host.js`, `pcsx2-wasm/web/app.js` | Zen moved from the software renderer to WebGPU | `220b6c393` |
| Present at most 512 lines in Firefox and on the 2D path, scaled up by CSS. Firefox copies every WebGPU frame back from the GPU: 9.6 ms a frame at a Retina canvas size, 2.9 ms at 640x512. | `pcsx2-wasm/web/app.js` | Zen, 25 figures: 164 → 367 % | `e034ac41a` |

### Threads and data movement

| What | Where | Gain | Commit |
|---|---|---|---|
| MTGS spins briefly for each VU1 program's XGKICK before sleeping. A sleeping thread costs the VU1 thread a JavaScript `Atomics.notify` per program, which was 13 % of its time. The spin adapts its length. | `pcsx2/MTGS.cpp` | Chrome, 100 figures: 102 → 104-110 % | `0715f8848` |
| Unmasked V4-32 VIF unpacks are copied straight into VU memory instead of through a function call per 16 bytes, later in at most two `memcpy`s. | `pcsx2/Vif_Unpack.cpp` | Chrome, 300 figures: 145 → 157.5 % | `b982eb59e`, `acb0eab2b` |
| The EE and VU threads' hot fields sit on separate 128-byte cache lines (Apple Silicon's line size). | `pcsx2/MTVU.h`, `common/Pcsx2Defs.h` | 0.16 ms per vsync on each thread | `be7b868ff` |

### VU1 JIT (vertex processing)

| What | Where | Gain | Commit |
|---|---|---|---|
| Loops run inside generated code; MAX/MINI and DIV/SQRT/RSQRT are generated natively | `pcsx2/wasm/VU1JitEmit.cpp` and related | VU1 time 29.4 → 22.9 ms per vsync (benchmark) | `0b63055ae` |
| VU registers and flags kept in wasm locals; clamps skipped on clean values | same | 22.9 → 15.4 ms | `d7d7e53ba` |
| Remaining simple lower instructions generated natively | same | 15.4 → 15.2 ms; Chrome, 100 figures: 106 → 224 % | `75b98ac60` |
| A program's blocks jump to each other inside one wasm module, with learned JR/JALR targets and cheaper XGKICK catch-up. Guitar Hero's branchy VU1 code went back to the C++ dispatcher about every 9 instructions. | `pcsx2/wasm/VU1Jit.cpp`, `VU1JitEmit.cpp` | Guitar Hero in a song: VU1 15.1 → 10.0 ms per vsync (12.1 → 7.1 ns per instruction); song opening in Chrome 87 → 130-138 % | `ced73c7b6` |
| A branch or XGKICK in a branch's delay slot runs in generated code | same | (part of the above) | `c45dbbc50` |

### EE and IOP JITs (game code)

| What | Where | Gain | Commit |
|---|---|---|---|
| The common branch end runs in generated code, through one function per module. LDL/LDR/SDL/SDR are generated natively; they were 41 M interpreter calls per run. | `pcsx2/wasm/EEJit.cpp` | EE thread per vsync at 300 figures: 14.2 → 13.1 ms | `8a80a5fb7`, `07e93ed5b` |
| Compile hot code only. New code runs on the interpreter until it has run 128 times. Modules are larger, and compiling has a budget of 4 ms per vsync. | `pcsx2/wasm/JitHeat.h`, `EEJit.cpp`, `IopJit.cpp` | Worst compile in one vsync: Guitar Hero 69 → 7.9 ms, Naruto menus 56.7 → 5.0 ms; total compile 1.3 → 0.34 s | `7852633ef`, `c88cd23f3` |

### GPU pipelines (stutter when something new appears)

| What | Where | Gain | Commit |
|---|---|---|---|
| TFX pipelines are created in the background. A draw waits at most 3 ms (4 ms per vsync); if its pipeline is not ready, that draw is skipped for that frame. This is the owner's choice: an object can be missing for a frame or a few the first time an effect appears. `?pipelines=wait` restores waiting. | `pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPUHW.cpp` and related | Naruto boot screens: 658 → 11 ms waiting, worst frame 417 → 29 ms | `d7ae5b791` |
| Each game's pipeline list is stored in IndexedDB (by serial) and prewarmed in the background at the game's next boot | same, `pcsx2-wasm/web/app.js` | Guitar Hero second boot: 1 ms waiting, nothing skipped; Naruto menus second boot: 0 ms | `d7ae5b791`, `4b37cfc7c` |

### Video (FMV)

| What | Where | Gain | Commit |
|---|---|---|---|
| WebAssembly SIMD colour conversion for the IPU (yuv2rgb), byte-identical for every input | `pcsx2/IPU/yuv2rgb.cpp` | about 1 % of the EE thread in a video | `6f4323163` |
| IOP import calls skip a debug name lookup unless IOP BIOS tracing is on (shared code, not guarded; the name only feeds that log) | `pcsx2/IopBios.cpp` | about 1 % of the EE thread | `6f4323163` |

Together: Naruto's intro video, headless, 95.3 → 96.9 %.

### Measuring tools (no speed of their own)

- **The crowd benchmark** and its runners: `cfc473aed`, `2298389fb`, `f28aa05f8`, `df4d8a645`,
  `a1dbda87e`, `280678e8b`, `03b02602a`, `091fced07`.
- **Thread usage on wasm** (wall time minus waits) and `?threads=1`: `511b26cc9`, `1e2bb736d`.
- **The Recorder**: `556d474ad`.
- **A Node runner crash fix**: `b16508154`.
- **Use Node 24 for headless runs**: Node 22's V8 runs out of heap on games. `36c826032`.

## What it gives now

The benchmark changed along the way (its 100-figure version became the 300-figure version, with
simpler distant figures), so each row compares one version with itself.

| Test | Start | Now |
|---|---|---|
| Benchmark, first version, 100 figures, Chrome | 102 % | 224 % (last measured on that version) |
| Benchmark, first version, 100 figures, Zen | 92.5 % | about 160 % (last measured on that version) |
| Benchmark, 300 figures, Chrome | 146 % | about 155-160 % |
| Benchmark, 300 figures, Zen | 114 % | about 115 % (EE-bound) |
| Guitar Hero in a song, Chrome, limiter on | 80-87 % | nearly always 100 %, with rare dips |
| Naruto: menus, scene changes, special moves | long freezes on first appearance | first boot short skips; later boots smooth |
| Naruto intro video, Chrome | | 82-84 % (EE-bound: see below) |

## In progress

- **EE JIT: guest registers in wasm locals** (started 2026-10-21). Guest registers stay in fast locals
  while a block runs, instead of being loaded and stored in memory for every instruction. This is
  what gave VU1 its largest gain. It targets everything EE-bound: Naruto's intro video, game logic,
  and Zen at 300 figures. Checked with EE/IOP state hashes and pixels on the BIOS, Naruto and
  Guitar Hero.

## Known to be left

- **Naruto's intro video** runs at 82-84 % in Chrome. While the IPU decodes, the game's idle thread
  loops on a kernel syscall (RotateThreadReadyQueue), and each call saves and restores a thread
  context. Share of the EE thread:

  | Share | What |
  |---|---|
  | 48 % | EE JIT code |
  | 20 % | IOP (on the same thread) |
  | 7 % | event tests |
  | 7 % | IPU decoding |

  A "wait loop" skip like PCSX2's WaitLoop hack would gain a lot there, but it is a new timing hack:
  opt-in only, not started.
- **IOP JIT** per-block overhead: about 8 % of the EE thread in that video.
- **Zen** is 20-25 % slower than Chrome on the same work. There is no automatic Zen profiler yet;
  the Recorder works there.
- **The first time a game shows a new effect**, a draw can be skipped for a frame (by design). A
  skipped draw into a texture the game draws only once stays wrong until it is redrawn.
- **The VU1 JIT**:
  - 21 % of block-to-block steps still go through a `br_table` (natural loops could become real
    wasm loops);
  - flag computation costs about 10 instructions per flag;
  - a few rare delay-slot cases still go to the interpreter.
- **SPU2 mixing**: 5-10 % of the EE thread. It processes all 48 voices every sample by design.
