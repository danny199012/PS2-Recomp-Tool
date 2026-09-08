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
- CLIs + GUI: `ee-disasm`, `ee-analyze`, `ee-recomp`, plus two apps —
  `ee-studio` (SDL3 + optional Dear ImGui developer GUI: game library, ISO boot
  detection, analyze/recompile, disassembly view) and `app/game-launcher`
  (per-game **release** launcher template: disc-prompt GUI -> extract boot ELF ->
  run; see `docs/LAUNCHER.md`)

The pipeline is verified end-to-end: analyze an ELF, recompile to C++, compile
against the runtime, and execute natively — including a threaded "homebrew" that
creates a thread, synchronizes via semaphore, prints over the EE STDOUT MMIO
register, and exits through the kernel (see tests + `docs/PLAN.md`).

See [docs/PLAN.md](docs/PLAN.md) for the roadmap and [docs/INTEGRATION.md](docs/INTEGRATION.md)
for Aura / Ghidra / PS2Recomp interop.

## Building

Requirements: **CMake ≥ 3.20** and a C++20 compiler (MSVC 2022, GCC 11+, Clang 14+).
No external dependencies.

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
app/ee-studio   SDL3 (+ optional Dear ImGui) developer GUI: game library, ISO
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
