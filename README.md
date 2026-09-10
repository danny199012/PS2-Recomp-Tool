# EERecomp

**EERecomp** is an experimental **static recompiler for PlayStation 2** games (Emotion
Engine / MIPS R5900), targeting **Windows and Linux**. It parses PS2 ELF binaries,
decodes R5900 machine code — including the PS2-specific MMI multimedia instructions,
the FPU, and VU0 macro-mode instructions — and (in later milestones) translates them
to portable C++ that is compiled against a runtime to produce native PC ports.

> Working title. The name may change; the code won't care.

Inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp) and designed to
complement [PS2Recomp](https://github.com/ran-j/PS2Recomp) (see below).

## Status

The core pipeline and a growing runtime are implemented. What exists today:

- `ee::elf` — dependency-free ELF32 parser for PS2 executables (sections, segments, symbols)
- `ee::r5900` — table-driven R5900 decoder + disassembler: core MIPS, MMI (MMI0–MMI3),
  COP0, COP1 (FPU), COP2 (VU0 macro mode, SPECIAL1/SPECIAL2 tables)
- `ee::analysis` — function discovery (symbols, recursive descent, jump-table
  resolution, prologue scan) with CSV/JSON import (Aura / Ghidra) and
  PS2Recomp-compatible TOML export
- `ee::codegen` — C++ code generator: core MIPS, FPU, a large MMI subset,
  delay slots, branch-likely, jump tables, calls, stubs/skips/patches
- `ee::runtime` — guest memory (32 MB RDRAM + scratchpad, MMIO hooks), dispatch,
  full MMI helper set (PCSX2-verified semantics incl. hardware errata), kernel HLE:
  syscall dispatch (ps2sdk numbering), cooperative thread scheduler with
  priorities, semaphores, EE STDOUT console, `_print`
- `ee::hw` --- DMAC (DMA controller with chain-tag walking: REFE/CNT/NEXT/REF/
  REFs/CALL/RET/END), VIF (command stream + UNPACK S/V2/V3/V4 formats),
  GIF (PACKED/REGLIST/IMAGE tags -> GS), GS (register file + 4 MB VRAM), VU memories
- `ee::vu` --- VU0 macro-mode (COP2): full arithmetic (add/sub/mul/max/min/madd/
  msub with broadcast, Q/I sources, accumulator), conversions (itof/ftoi), integer
  ops, moves, divide/sqrt/rsqrt, MAC/status flags (PCSX2-verified), clip
- `ee::vu1` --- VU microcode interpreter (VU0/VU1): runs microprograms uploaded via
  VIF `MPG` (MSCAL), 64-bit upper/lower instruction pairs, vf/vi/acc/Q/P/I/R state
- `ee::iop` --- IOP HLE: SIF (SIF0/SIF1/SIF2 DMA), CDVD, pad, memory card, SPU2
  stub, RPC service table
- `ee::cdvd` --- ISO-based disc drive: opens `.iso`/`.bin` (ISO9660 + basic UDF),
  raw sector reads, SYSTEM.CNF / boot-ELF discovery, on-disc file reads
- `ee::gs_renderer` / `ee::gs_swizzle` --- software GS rasterizer with framebuffer
  readback, plus PSM_CT32 VRAM swizzle helpers
- `ee::game_overrides` --- per-game override/profile system (match by ELF name +
  CRC32, bind addresses to stub handlers, `EE_REGISTER_GAME_OVERRIDE`)
- CLIs + GUI: `ee-disasm`, `ee-analyze`, `ee-recomp`, plus three apps —
  `ee-tools` (SDL3 + Dear ImGui GUI for all three tools: ELF info,
  disassemble, analyze, recompile; loads `.elf` **and** PS2 disc images
  `.iso`/`.bin` — the boot ELF is auto-extracted via SYSTEM.CNF — and runs
  every operation on a background thread with a progress bar and Cancel so
  the window never freezes on large binaries), `ee-studio` (developer GUI: game library,
  ISO boot detection, analyze/recompile, disassembly view), and
  `app/game-launcher` (per-game **release** launcher template: disc-prompt
  GUI -> extract boot ELF -> run; see `docs/LAUNCHER.md`)

The pipeline is verified end-to-end: analyze an ELF, recompile to C++, compile
against the runtime, and execute natively — including a threaded "homebrew" that
creates a thread, synchronizes via semaphore, prints over the EE STDOUT MMIO
register, and exits through the kernel (see tests + `docs/PLAN.md`).

See [docs/PLAN.md](docs/PLAN.md) for the roadmap and [docs/INTEGRATION.md](docs/INTEGRATION.md)
for Aura / Ghidra / PS2Recomp interop.

## Building

Requirements: **CMake ≥ 3.20**, a C++20 compiler (MSVC 2022, GCC 11+, Clang 14+),
and **git** (the GUI apps fetch SDL3 + Dear ImGui on first configure).

The core pipeline (`libs/`, `tools/`, `runtime/`, tests) has **no external
dependencies**. The three GUI apps — `ee-tools`, `ee-studio`, `game-launcher` —
need **SDL3** and Dear ImGui. Dear ImGui is always fetched automatically. SDL3
is resolved as follows (see `cmake/ee_sdl3.cmake`):

1. A system/vendored SDL3 found by `find_package(SDL3)` — e.g. the official
   `SDL3-devel-*.zip` located via `-DSDL3_DIR=...` or `-DCMAKE_PREFIX_PATH=...`.
2. Otherwise, by default, SDL3 is **downloaded and built from source**
   (`-DEE_FETCH_SDL3=ON`, the default). This works on a clean clone but makes
   the **first** configure/build noticeably longer while SDL3 compiles.
3. `-DEE_FETCH_SDL3=OFF` disables the fetch; if no system SDL3 is found the GUI
   apps build as small stubs that print an explanatory message (and wait, so the
   window does not flash closed) instead of a window.

> If you previously configured without SDL3, delete the build directory before
> reconfiguring so the SDL3 fetch is picked up: `rmdir /s /q build` (Windows) or
> `rm -rf build` (Linux).

**Windows (Visual Studio 2022):**
```bat
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**Linux:**
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Usage

```
ee-disasm  <file.elf> [--section .text] [--start 0xADDR --end 0xADDR] [--no-disasm]
ee-analyze <file.elf> [--import funcs.csv|.json] [--toml out.toml] [--csv out.csv] [--json out.json]
ee-recomp  <file.elf> [--config game.toml] [--import funcs.csv|.json] [--out game.recomp.cpp]
```

Typical flow: `ee-analyze` to discover functions and emit a config skeleton,
edit the TOML (stubs/skip/patches), then `ee-recomp` to generate C++, then
compile the output together with `runtime/`.

## How the pieces fit together (read this if the GUIs confuse you)

The `.csv` / `.json` (Aura / Ghidra function exports) and `.toml` config are
**build-time** inputs to the analyzer/recompiler — they are *not* things the
launcher asks for at runtime.

1. **Analyze + recompile an ELF into C++** (build time):
   `ee-analyze boot.elf --import aura_funcs.csv --toml game.toml`
   `ee-recomp  boot.elf --config game.toml --out game.recomp.cpp`
   The `ee-tools` GUI does the same three steps in a window (ELF info →
   Analyze → Recompile). `ee-recomp`/`ee-tools` emit **one** `.cpp` per ELF.

2. **`game-launcher` is a per-game template, not a general emulator.** It does
   *not* "load any disc and play it." You build it with **your game's
   recompiled `.cpp` linked in**:
   ```
   cmake -S . -B build -DEE_GAME_RECOMP_SOURCES="C:/.../game.recomp.cpp" ...
   ```
   Only then does **Launch game** do anything. Built with no game code
   (`EE_GAME_RECOMP_SOURCES` empty, the default), the launcher only extracts
   the boot ELF and shows disc info — **Launch game is disabled** with a
   tooltip explaining why. See `docs/LAUNCHER.md` for the full per-game flow.

3. **`ee-studio`** is the developer GUI (game library, analyze/recompile,
   disassembly view). It needs Dear ImGui, which is now **on by default**
   (`-DEE_WITH_IMGUI=ON`). Without it, ee-studio opens an empty window (it used
   to close after 100 ms, which looked like "ee-studio doesn't work").

> Note on output size: `ee-recomp` emits one C++ translation unit per ELF. A
> stripped commercial PS2 ELF with thousands of functions produces a large
> `.cpp` (turn off comments with `--no-comments` / the *Emit comments* checkbox
> in `ee-tools` to roughly halve it). A single multi-hundred-MB `.cpp` is not
> practically compilable by MSVC in one file; this is a known limitation of the
> current single-file emitter. The analyzer now produces non-overlapping
> function ranges so code is no longer emitted multiple times (which previously
> caused a ~450 MB blowup from a ~4 MB ELF).

## Repository layout

```
libs/ee-base    common types + text/JSON/TOML helpers (header-only)
libs/elf        ELF32 loader
libs/r5900      R5900 instruction decoder + disassembler
libs/analysis   function discovery + interchange formats
libs/codegen    C++ code generator
runtime/        runtime for recompiled code (memory, dispatch, devices, helpers)
                incl. kernel HLE, HW (DMA/VIF/GIF/GS), VU0 macro + VU microcode,
                IOP HLE, CDVD ISO reader, GS renderer, game overrides
app/ee-tools     SDL3 + Dear ImGui GUI for the toolchain: ELF info, disassemble,
                 analyze (imports + TOML/CSV/JSON export), recompile (TOML config)
app/ee-studio    SDL3 (+ optional Dear ImGui) developer GUI: game library, ISO
                 boot detection, analyze / recompile / run, disassembly view
app/game-launcher per-game release launcher template: disc-prompt GUI -> extract
                 boot ELF -> run (see docs/LAUNCHER.md)
tools/          ee-disasm, ee-analyze, ee-recomp (CLIs)
tests/          unit tests (no framework, plain asserts)
docs/           PLAN.md (roadmap), INTEGRATION.md (Aura/Ghidra/PS2Recomp)
libs/vu         (future: standalone VU micro-mode decoder)
```

## Relationship to PS2Recomp

This project is deliberately shaped so its components can benefit
[Ran-J's PS2Recomp](https://github.com/ran-j/PS2Recomp):

- same stack (C++20 + CMake) and license (GPL-3.0) — code can flow both ways
- the R5900 decoder, and later the VU1 microcode and GS implementations
  (PS2Recomp's stated gaps), are written as standalone libraries that could be
  dropped into or upstreamed to PS2Recomp
- the analyzer/recompiler TOML config schema will be kept compatible with
  PS2Recomp's (`general.stubs`, `general.skip`, `patches.instructions`,
  `handler@0xADDR` bindings), so configs and Ghidra exports work with both tools

## License

GPL-3.0-only. See [LICENSE](LICENSE).
