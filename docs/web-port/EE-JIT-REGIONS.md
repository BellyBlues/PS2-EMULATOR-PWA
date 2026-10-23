# Future project: a faster EE compiler ("regions instead of blocks")

*Written 2026-10-23 at `b65e149ab` (branch `Yousri-v2`). Not started. A plan to come back to; every number
below was measured on an M1 Max and is recorded in `docs/web-port/PERFORMANCE.md`.*

## In plain words

**The EE** is the PS2's main processor, the chip that runs the game's own code (its rules, its physics, its
AI). The emulator recreates it in software on one core of your computer. In every game we measured, the EE
is the part that limits speed: when a game slows down, it is almost always because the EE could not keep up.

**A JIT** ("just-in-time compiler") is a translator. The PS2's code is written in the PS2's own machine
language, which your computer cannot run. The emulator could read it one instruction at a time and act it
out (that is the *interpreter*: correct, but very slow). The JIT instead translates the game's code, the
first time it is about to run often, into code the browser can run (WebAssembly), which the browser then
turns into your computer's own machine code. Translated code runs many times faster than interpreted code.

**The problem with how our JIT translates today.** It translates in very small pieces, called *blocks*:
usually 4 or 5 PS2 instructions, from one branch (a jump, an "if") to the next. After every block it stops to
do paperwork: it writes all the values it was working with back to memory, updates the cycle count (the
emulated clock), checks whether a timer or interrupt is due, and looks up which block comes next, which then
reads everything back from memory. Games branch constantly, so this paperwork happens every few
instructions. It is like a translator who stops after every sentence to file a report before starting the
next one.

**What this project would change.** Translate whole *regions*: a complete loop, or a complete function, as
one piece, the way an ordinary compiler would. Inside a region the values stay where they are (in the
processor's own registers), the cycle count and the "is an event due?" check become a single comparison
instead of trips to memory, and moving from one part of the loop to the next is a normal jump. The paperwork
happens only when leaving the region. The browser's own optimiser can also do a much better job on a whole
loop than on hundreds of separate little pieces.

**What it could give.** The game's translated code is 50 to 70 % of the EE's time in every game we measured.
If regions halve its cost, games run roughly 25 to 35 % faster overall: enough for FFXII's speed mode (L1)
and GTA's heaviest scenes to reach 100 % without Underclock. This is an estimate, not a measurement, which is
why the project starts with a test bench that measures how much there really is to gain before anything is
built.

**What stays the same.** The emulation must stay exact: the translated code has to do precisely what the
real PS2 would, down to the cycle. Every step is checked against the interpreter with the same tools used for
everything merged so far (state hashes, lockstep comparison, op tests). Today's JIT stays as the first tier
and the fallback, and the whole thing can be switched off.

**Cost and risk.** Two to four weeks of work, in phases that can each be stopped. The main risk is that the
gain is smaller than estimated (an earlier, smaller attempt, the "units", gained nothing; see "What was
tried" below for why this is different). The second risk is subtle bugs, which the checks above are designed
to catch.

## Where the time goes today

From CPU profiles of the EE thread (headless, `/tmp/a6e/tools/prof.py`-style), 2026-10-22/23:

| Workload | Speed | Translated EE code | IOP | Other |
|---|---|---|---|---|
| GTA San Andreas, walking | about 96 % | 66-70 % | 9 % | hardware events, VU0, sound |
| FFXII, speed mode (L1) | 82-83 % | about 58 % (72 % with the code around it) | 11.5 % | VU0 programs 6 %, sound 2.6 % |
| Naruto UN5, intro video | 103 % | 52 % | 24 % | video decoding 6 % |

The translated code's cost is spread thinly (GTA: about 20,000 blocks, the hottest under 0.5 %), so no single
routine can be optimised by hand: only a change in how all code is translated helps.

## What was tried (and why regions are different)

All in `docs/web-port/PERFORMANCE.md`, "Tried and dropped":

- **Units** (one WebAssembly function per module, registers kept across its blocks): no gain, slower
  compiling, reverted. The blocks still ended with the same per-branch memory traffic (cycle count, event
  check, PC) and jumped through a dispatch table, so the browser's optimiser saw the same pattern.
- **Lean branch ends** (fewer stores at branches): exact, 0.9 % fewer instructions.
- **Native fallbacks** (the last instructions the JIT handed to the interpreter, including FFXII's vector
  ops): exact, 96 % fewer interpreter calls, about 1 % faster. Kept on the branch `perf-experiments`.
- **Main RAM without the address table**, **32-bit values in 32-bit locals**, **copy-loop acceleration**,
  **IOP block linking**: each under 1-2 %.

What these show: no single kind of overhead is large. The cost is the *structure* (tiny pieces, memory round
trips between them), which only regions change.

## Design sketch

- **Regions.** Start from hot code the current JIT already finds (`JitHeat.h`). Phase 2: a loop (a backward
  branch to an earlier address in the same function, with every block of the loop body hot). Phase 3: a
  function (from a `JAL` target to its `JR $ra`), including the loops inside it.
- **Structured control flow.** WebAssembly has no `goto`: blocks of a region are laid out with `loop`,
  `block`, `if` and `br` using a "stackifier" (the algorithm LLVM uses for WebAssembly) on the region's
  control-flow graph. Edges leaving the region exit to today's dispatcher.
- **State in locals.** Guest registers (`RegCache.h`), `cpuBlockCycles`, `cpuRegs.cycle` and
  `nextEventCycle` in locals for the whole region. Written back before any call into C++ (interpreter
  fallbacks, memory slow paths, the event test), reloaded after, and at every exit.
- **Exact events.** The interpreter tests for events at every taken branch (`intJitBranchEnd()`): the region
  keeps that test at every branch, as a comparison of two locals, and leaves the region (with everything
  written back) to run the event test in C++ when it is due. The branch-end rules (wait loop skip, the idle
  loop skip, BIOS hooks, the cycle-rate scaling for Underclock) stay as today, taken out of line.
- **Self-modifying code.** The region checks its pages' write generations (`CodeWrites.h`) on entry, as
  blocks do; a write into a region's own code while it runs ends the region at the next branch.
- **Tiering.** Blocks stay the first tier. A region is built on the code generation thread (as modules are
  today, `CompileJob`) once its blocks are hot, and installed in place of its entry block. Code lists
  (compiling ahead) can name regions too.

## Phases and exit criteria

1. **Test bench (about 1 day).** Small synthetic EE programs (an ELF like `tools/wasm/bench/`): integer
   loops, loads and stores, branches, calls, FPU and VU0 macro code. Time each in today's JIT, in the
   interpreter, and against a hand-written WebAssembly version of the same loop (the ceiling).
   *Exit:* if today's JIT is within 1.5x of the hand-written version on loop-heavy code, stop: regions cannot
   give enough. If it is 2x or more slower, continue.
2. **Loop regions (about 1 week).** *Exit:* state hashes identical to blocks on GTA, FFXII speed mode, the
   BIOS and the crowd benchmark; EE lockstep against the interpreter on GTA and FFXII; at least 10 % less EE
   time in GTA or FFXII speed mode. Otherwise stop and record why.
3. **Function regions (about 1 week).** Same checks; target 25 % less EE time.
4. **Tuning (a few days).** Compile time (regions on the build thread, in code lists), browser differences
   (Zen runs the current translated code about 13 % slower than Chrome; check regions in both).

## Verification

Everything that merged so far was checked this way; regions use the same tools:

- EE/IOP state hashes (`-eejit-trace` with `-mtvu-sync` and a fixed RTC) identical with regions on and off,
  and against the build before; the end state of non-traced runs (`-savestate`, members compared after zstd
  decompression, `/tmp/t3v/scripts/p2s_extract.py`).
- Lockstep against the interpreter, and the op test (`/tmp/t3v/fb/optest/`, a PS2 program that runs every
  instruction on edge-case operands).
- Frame captures identical in Chrome and Zen.
- Speed measured with the crowd benchmark, the game states in `/tmp/t3v/` (GTA, FFXII, Naruto, DBZ, Guitar
  Hero, Moto X, RE4), and timings only in quiet windows when several helpers run.

## Other open ideas (smaller)

- **Frame pacing** (30 minutes to measure): whether the page shows game frames unevenly beyond what 25 frames
  per second on a 60 or 120 Hz screen forces. GTA looks choppy at 20-25 FPS; part of that is the game itself
  (as on a real PAL PS2), part may be fixable.
- **Zen graphics** (a few days): Firefox copies every frame back from the GPU when it is handed to the page;
  another presentation path could make graphics-heavy games much faster in Zen.
- **PCSX2's game patches** (`patches.zip`, from the `pcsx2_patches` repository): per-game fixes and optional
  widescreen, which desktop PCSX2 downloads and this build does not include.
- **IOP in larger slices** (an opt-in timing setting): the I/O processor is 15-24 % of the EE thread in most
  games, about half of it the cost of starting and stopping it 48,000 times a frame. Running it in larger
  slices would cut that, but the two processors would no longer stay in step to the cycle, so it could only be
  an option, like Underclock.
