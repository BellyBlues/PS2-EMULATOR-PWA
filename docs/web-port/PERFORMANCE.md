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
  scripted button presses; GTA San Andreas (PAL) on foot in Grove Street, from a savestate taken
  after the intro, 2400 vsyncs with a scripted walk, speed counted from vsync 600 (the JITs compile
  from cold after a savestate). Game data never enters the repository.
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
| The frame limiter keeps the time late frames lost (up to three frames), so lighter frames catch it up instead of sleeping. PCSX2's limiter forgives a whole frame as soon as one runs a frame late; GTA San Andreas alternates heavy and light vsyncs (35-60 ms, then 5-15 ms) and hit that every other vsync, so it ran at 85-95 % although its average work fits. Only real-time pacing changes, not emulation; after a stall the game runs up to about 108 % for a moment while it catches up. | `pcsx2/VMManager.cpp` | GTA SA gameplay, headless with the limiter on: 90.0 → 96.3 %. Replaying the owner's page recording: 94.8 → 98-99 % average, worst second 78 → 87 % | `5d2e6ebaa` |

### VU1 JIT (vertex processing)

| What | Where | Gain | Commit |
|---|---|---|---|
| Loops run inside generated code; MAX/MINI and DIV/SQRT/RSQRT are generated natively | `pcsx2/wasm/VU1JitEmit.cpp` and related | VU1 time 29.4 → 22.9 ms per vsync (benchmark) | `0b63055ae` |
| VU registers and flags kept in wasm locals; clamps skipped on clean values | same | 22.9 → 15.4 ms | `d7d7e53ba` |
| Remaining simple lower instructions generated natively | same | 15.4 → 15.2 ms; Chrome, 100 figures: 106 → 224 % | `75b98ac60` |
| A program's blocks jump to each other inside one wasm module, with learned JR/JALR targets and cheaper XGKICK catch-up. Guitar Hero's branchy VU1 code went back to the C++ dispatcher about every 9 instructions. | `pcsx2/wasm/VU1Jit.cpp`, `VU1JitEmit.cpp` | Guitar Hero in a song: VU1 15.1 → 10.0 ms per vsync (12.1 → 7.1 ns per instruction); song opening in Chrome 87 → 130-138 % | `ced73c7b6` |
| A branch or XGKICK in a branch's delay slot runs in generated code | same | (part of the above) | `c45dbbc50` |
| The same generator runs VU0 micro programs, now on by default (`EnableVU0`). GTA San Andreas ran VU0 programs on the interpreter for about 13 % of the EE thread. Checked with `-vu0jit-lockstep` on GTA gameplay (600 vsyncs, 1,008,250 programs, 0 differences), the fuzzer on VU0 (about 17,000 programs each with Liftoff only, TurboFan only and default tiering, 0 differences), GTA state hashes (VU0 registers included, 1000 vsyncs) and software-renderer screenshots. | `pcsx2-wasm/Main.cpp`, `pcsx2-headless/Main.cpp` | GTA SA gameplay (headless): 46.2 → 50.2 VPS | `6c4a3d41f` |

### EE and IOP JITs (game code)

| What | Where | Gain | Commit |
|---|---|---|---|
| The common branch end runs in generated code, through one function per module. LDL/LDR/SDL/SDR are generated natively; they were 41 M interpreter calls per run. | `pcsx2/wasm/EEJit.cpp` | EE thread per vsync at 300 figures: 14.2 → 13.1 ms | `8a80a5fb7`, `07e93ed5b` |
| Compile hot code only. New code runs on the interpreter until it has run 128 times. Modules are larger, and compiling has a budget of 4 ms per vsync. | `pcsx2/wasm/JitHeat.h`, `EEJit.cpp`, `IopJit.cpp` | Worst compile in one vsync: Guitar Hero 69 → 7.9 ms, Naruto menus 56.7 → 5.0 ms; total compile 1.3 → 0.34 s | `7852633ef`, `c88cd23f3` |
| Guest registers in wasm locals. The EE keeps the low 64 bits of the 32 GPRs plus HI/LO; the IOP keeps its GPRs plus HI/LO. They load on first use and are written back before every C++ call and at exits. MMI lane ops, MFC0/MTC0 to plain registers and ADDI are now generated natively: the Naruto video's interpreter calls went from 189 M to 19 M per 600 vsyncs. | `pcsx2/wasm/RegCache.h`, `EEJit.cpp`, `IopJit.cpp` | Naruto video (headless): about +11 % | `0391d4a0d` |
| IOP branch start and end stores emitted inline; one EE helper fewer per module | `pcsx2/wasm/IopJit.cpp`, `EEJit.cpp` | (part of the above) | `0db7d0b22` |
| Runs of loads and stores through one base register checked once (alignment, page, mapping). This roughly halves the kernel's thread context save and restore. | `pcsx2/wasm/EEJit.cpp` | Naruto video: +7-9 % more, about +24 % in total (47 → 58.5 VPS headless); Zen at 300 figures: 133 → 150-154 % | `693c92743` |
| Idle loops skipped up to the next event. GTA San Andreas waits for a DMA interrupt by polling two counters (a 10-instruction loop at `0x34dfd8`, run 250 M times per 1200 vsyncs): a third of the EE thread. At a taken backward branch whose loop has only register ALU ops, loads, branches and jumps, one iteration runs on the interpreter, checked step by step; if it changed no register, the iterations whose last branch test comes before the next event only add cycles (memory changes only in event tests), so they are skipped. The site remembers the registers and the values loaded, so the same state skips without running the iteration again. Exact: `-eejit-trace` replays every skipped branch test with its state, and the hashes over 1000 GTA vsyncs (906 M branch tests, 210 M of them skipped) are the same with the skip on, off (`-eejit-spin 0`) and in the build before it; the end state of 1000 non-traced vsyncs is the same on and off. | `pcsx2/wasm/EEJit.cpp` | GTA SA gameplay (headless): 34.7 → 46.2 VPS | `0b9ac18f0`, `582755a74`, `ef46ac2fc` |
| PEXEH, PEXEW, PREVH, PROT3W as byte shuffles, and DIV.S, generated natively: GTA's interpreter calls 21.4 M → 13.6 M per 1200 vsyncs | `pcsx2/wasm/EEJit.cpp` | within noise | `ef46ac2fc`, `035e32569` |
| One function per module (its unit): a module's blocks run in one loop, jump to each other directly, and keep the guest registers in locals from one block to the next. Exact: state hashes identical to the build before over GTA gameplay (600 vsyncs), the BIOS and the benchmark. On its own it does not change the speed: GTA's blocks are 4-5 instructions, and the time goes into the bookkeeping at each taken branch (PC, branch flag, cycles, the branch end call, the next block's checks: about 30 memory operations), which the units make it possible to keep in locals next. | `pcsx2/wasm/EEJit.cpp`, `RegCache.h`, `WasmEmitter.h` | GTA SA headless: 98.6 → 97.8 % (within noise) | `7f41a4f0a` |

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
- **Save states in the Recorder** (Save state, Load state, `?state=`), to measure from the same frame
  every time: `5b006b75c`.
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
| Benchmark, 300 figures, Zen | 114 % | 150-154 % (EE-bound) |
| Guitar Hero in a song, Chrome, limiter on | 80-87 % | nearly always 100 %, with rare dips |
| Naruto: menus, scene changes, special moves | long freezes on first appearance | first boot short skips; later boots smooth |
| Naruto intro video, Chrome | 82-84 % (measured by the owner before the EE register work) | not yet measured in the page; headless it runs about 24 % faster |
| GTA San Andreas on foot, headless (median of 3, from vsync 600 of 2400) | 34.7 VPS (69 %) | 50.8 VPS (101.5 %); the owner's page recording before this was 78 % on average (57-92 %), EE-bound. In one build with the features switched (a busier machine, medians of 3 interleaved): 31.1 VPS with both off, 43.8 with the idle loop skip, 47.8 with the VU0 JIT too |
| GTA San Andreas on foot, in the page (the owner's recording, after the idle loop skip and the VU0 JIT, before the limiter change) | 78 % average | 94.8 % average in gameplay, 59 % of the time at 99 % or more; dips to 76-85 % where the EE is busy |

## In progress

- **EE JIT: the bookkeeping at each taken branch** (started 2026-10-22). GTA's hottest blocks are 1-7
  instructions long, and each taken branch costs about 30 memory operations of bookkeeping (the PC and
  branch flag, the cycle counters, the branch end function, the next block's code and mapping checks)
  against the one or two of the game's own work. Plan: keep the cycle count and the next event's cycle in
  locals within a unit and compare them there, skip the PC and branch flag stores where the delay slot
  cannot reach C++, and skip the next block's checks on jumps within a unit when nothing could have
  changed its code. Each step checked with state hashes.
- **GTA San Andreas in the page** after the limiter change: the owner's recording shows 99.3 % on
  average in gameplay, with EE-bound dips to 81-89 % at the start of the game.

## Known to be left

- **GTA San Andreas** (on foot, headless, after the idle loop skip and the VU0 JIT; second half of
  2400 vsyncs). Share of the EE thread:

  | Share | What |
  |---|---|
  | 60 % | EE JIT code, flat: the 50 hottest blocks are 11 %, the branch end function 4 %, the lookup 1 % |
  | 15 % | event tests, of which the IOP 5 %, SPU2 mixing 3.5 %, VIF1 3 %, GIF 1.4 % |
  | 3 % | the idle loop skip itself (about 6,000 calls per vsync: the event that ends a skip comes every 1,000-1,500 EE cycles) |
  | 2 % | the interpreter (code not hot yet) |

  Before the skip and the VU0 JIT (whole run, from the savestate): the idle loop 33 %, VU0 micro
  programs on the interpreter 12 %, events 10 %, the interpreter while the JITs warm up 8 %. The EE
  JIT's interpreter calls are 21 M per 1200 vsyncs (DIV.S 4.6 M, CACHE 2.8 M, PROT3W 1.9 M, PEXEW
  1.3 M, LWL/LWR/SWL/SWR 0.9 M each, SQRT.S 0.9 M): about 1 % of the thread.

- **Naruto's intro video** runs at 82-84 % in Chrome. While the IPU decodes, the game's idle thread
  loops on a kernel syscall (RotateThreadReadyQueue), and each call saves and restores a thread
  context. Share of the EE thread:

  | Share | What |
  |---|---|
  | 48 % | EE JIT code |
  | 20 % | IOP (on the same thread) |
  | 7 % | event tests |
  | 7 % | IPU decoding |

  These shares are from before the EE register work, which made that video about 24 % faster
  headless. A "wait loop" skip like PCSX2's WaitLoop hack would gain a lot there, but it is a new
  timing hack: opt-in only, not started.
- **IOP JIT**: its dispatcher and event test on every IOP branch (about 8 % of the EE thread in that
  video). IOP block linking is not started.
- **EE JIT**:
  - **V8 tiering:** moderately hot functions stay in V8's baseline tier (Liftoff). The video ran
    6.6 % faster with V8 forced to optimise everything; merging a page's blocks into one function
    may get part of that. GTA, whose EE time is spread over 20,000 blocks, ran slower with
    `--no-liftoff` (37.9 against 46.2 VPS headless).
  - **Kernel thread switches:** SYSCALL, ERET and MTC0 Status still call the interpreter (about
    6,000 thread switches per frame in the video).
  - **The FPU's registers** are not cached in locals yet.
  - **Generated code size:** register write-backs made the EE's wasm 39 % larger and compiling
    38 % longer.
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
