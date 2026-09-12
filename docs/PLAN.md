# EERecomp — Architecture & Roadmap

Static recompilation of PS2 (Emotion Engine / R5900) binaries to C++, for native
Windows/Linux ports. This document is the living plan; update it as decisions land.

## Goals

1. Statically recompile PS2 ELF binaries (R5900 core + MMI + FPU + VU0 macro) to
   portable C++ (N64Recomp-style output), compiled against a runtime.
2. Cross-platform from day one: Windows (MSVC) + Linux (GCC/Clang), CMake, CI-gated.
3. Components reusable by / upstreamable to Ran-J's PS2Recomp (same stack, GPL-3.0,
   compatible TOML schema). Our differentiators map to his stated gaps:
   **VU1 microcode** and **GS (Graphics Synthesizer)** implementations.

## Non-goals (for now)

- Dynamic recompilation / JIT. Output is C++; the host compiler optimizes.
- Cycle accuracy. We aim for functional correctness, not timing accuracy.
- IOP recompilation. IOP is HLE'd (SIF/RPC level), like PS2Recomp's ps2xIOP.

## Hardware scope and strategy

| Unit | Strategy |
|---|---|
| R5900 core (MIPS III-ish, 64-bit) | Static recompilation to C++ |
| MMI (128-bit SIMD) | Static recompilation (128-bit via host `__int128` / software helpers) |
| FPU (COP1, non-IEEE) | Static recompilation + runtime helpers for non-IEEE quirks |
| VU0 macro mode (COP2) | Static recompilation |
| VU0/VU1 micro mode | Runtime interpreter first (microcode is uploaded at runtime via VIF/DMA, so pure static analysis can't see it); optional microcode recompiler later |
| Kernel / BIOS syscalls | HLE (~100+ syscalls: threads, semas, handlers, DMAC/GS control, SIF) |
| Threads | Fiber-based cooperative scheduler on one host thread (deterministic) |
| DMA / VIF / GIF | Faithful device-level emulation; games build display lists and kick DMA |
| GS | Null → software rasterizer → Vulkan. Consumes GIF packets/register writes |
| IOP (R3000A) | HLE of SIF services: CDVD, pad, memory card, audio, IRX module loading |
| SPU2 | HLE/audio emulation (later milestone) |

## Code layout (target)

```
libs/ee-base     common types + text/JSON/TOML helpers          [done]
libs/elf         ELF32 loader                                   [done]
libs/r5900       R5900 decoder + disassembler + VU0 macro ops   [done]
libs/analysis    function discovery + interchange formats       [done]
libs/codegen     C++ code generator                             [done]
libs/vu          VU0/VU1 micro-mode ISA standalone lib          [future]
tools/ee-disasm  disassembler CLI                               [done]
tools/ee-analyze function discovery -> TOML (PS2Recomp schema)  [done]
tools/ee-recomp  ELF + TOML -> C++ codegen                      [done]
runtime/core     guest memory, dispatch, kernel HLE, MMI        [done]
runtime/hw       DMA, VIF, GIF, GS regs + VRAM, VU memories     [done: subset]
runtime/gs       GS device + software renderer + readback       [partial]
runtime/vu       VU0 macro codegen + VU0/VU1 micro interpreter  [micro: done]
runtime/iop      SIF/CDVD/pad/MC/SPU2 HLE + RPC services        [partial: HLE stubs]
runtime/cdvd     ISO9660/UDF disc reader (boot ELF, files)      [done]
app/ee-studio    SDL3 (+ ImGui) studio GUI                      [skeleton]
app/game-launcher per-game release launcher (disc prompt GUI)   [in progress]
```

## Recompiler design (M3 preview)

- Per-function C++ output: `void fn_00100000(EEContext& ctx)`.
- `EEContext`: 32x128-bit GPRs, LO/HI/LO1/HI1 (128-bit), SA, 32 FPRs + ACC,
  VU0 state (32x128 vf, 16x16 vi, Q/P/R/I, status/MAC/clip flags), PC.
- Guest memory: flat 32 MB RDRAM + 16 KB scratchpad (0x70000000); KSEG0/KSEG1
  aliases handled by address masking; loads/stores via inlineable helpers.
- Branches within a function become `goto`/labels; calls go through a dispatch
  table populated at load (enables stubs/overrides by name or address — same
  model as PS2Recomp's runtime).
- Delay slots: recompiled inline (execute slot instruction, then branch).
- Config TOML compatible with PS2Recomp: `general.stubs`, `general.skip`,
  `patches.instructions`, `handler@0xADDR` address binding, Ghidra CSV/TOML import.

## Function discovery (M2)

1. Symbols (unstripped ELFs) + ELF entry point.
2. Recursive descent from entry/roots following direct `jal`/`j`/branches.
3. Jump-table pattern matching (`lui/addu/lw/addu/jr` over `.rodata` tables).
4. Prologue linear sweep over remaining executable gaps.
5. Unresolved indirects -> TOML for manual annotation (ps2xAnalyzer-style workflow).

## Testing strategy

- Decoder unit tests: known encodings (cross-checked vs PCSX2 tables / binutils
  `mips64r5900el`), including MMI subclasses and VU0 SPECIAL1/2.
- ELF loader tests: synthetic ELF32 images built in-test.
- Later: differential execution vs PCSX2 interpreter (random instruction streams,
  compare register/memory state); ps2sdk homebrew demos as legal end-to-end targets.

## Milestones

- **M0** Skeleton: CMake, CI (Win+Linux), ELF loader, decoder core, disasm CLI  [done]
- **M1** Full R5900 decode audit vs EE manual/binutils; operand-format polish; VU0 macro operands  [done]
- **M2** `ee-analyze`: function discovery, jump tables, TOML output (PS2Recomp schema)  [done]
- **M3** `ee-recomp`: C++ codegen (integer core) + runtime skeleton (memory, dispatch)  [done]
- **M4** FPU + MMI codegen; kernel HLE (threads/semas/syscalls, EE STDOUT MMIO); first homebrew runs  [done: threaded homebrew verified end-to-end]
- **M5** DMA/VIF/GIF pipeline; VU0 macro codegen  [done: DMA/VIF/GIF subset + VU0 macro]
- **M6** GS (null → software); SDL3 app shell; first visible homebrew graphics  [partial: software renderer + framebuffer readback, ee-studio shell; Vulkan + visible graphics still open]
- **M7** VU1 microcode interpreter (standalone lib — prime PS2Recomp contribution)  [done: VU0/VU1 micro interpreter]
- **M8** IOP HLE: SIF, CDVD, pad, memory card  [partial: SIF/CDVD/pad/MC/SPU2 stubs + RPC table; **SIF command processing live (Play! SifCmd port)**: sceSifSetDma descriptor walk, CHANGE_SADDR/SET_SREG/INIT/BIND/CALL/RDATA/REND dispatch, HLE service registry, SIF sysregs + SBUS regs, DMA-kick packet dispatch; IOP-firmware boot list heads seeded — Urbz TIMER2 handler no longer spins on zeroed IOP nodes]
- **M9** First commercial game boots; per-game override/profile system  [partial: override registry + address binding exist; **SLUS-21066 (Urbz) boot verified end-to-end to the game's own EE-heap phase**. The presumed "game's arena init not run" blocker was diagnosed and fixed: the game's heap-init (GetGlobalHeap 0x384F30 -> HeapInit 0x385138 -> HeapCreate 0x38B318 -> ArenaInit 0x38B550, which self-links the 127 dlmalloc bins at 0x4DAC20) *does* run correctly. The real defect was in the **analyzer/recompiler**: switch (jump-table) bodies were shredded into one 4-byte "function" per case — the jump-table resolver cleared the `sll` destination in the index*4 chain, merged dispatch tables bled into each other (resolver read unbounded entries), and the Walker held a dangling `Function&` into a vector that `add_function()` reallocated mid-walk, poisoning `claimed` owners so the finalize pass promoted every body address. The resulting switches returned before running their shared epilogue, clobbering `$s0`/`$sp` across every dispatch — GetGlobalHeap had its `lui $s0,0x004E` lost, so malloc got arena `0xFFFFAC20` (zeroed RAM) and its bin scan walked a NULL list forever. Fixed the resolver (shift-aware + window-bounded) and the Walker (index-based writeback); added regression tests; Urbz now self-links its arena and allocates from it, with the malloc bin-scan spin gone. **Next phase landed: (1) CDVD file loading — the `Cdvd` ISO reader is wired into `Hw`/`Iop` and the EE libcdvd's `cdvdfsv` SIF-RPC servers are answered (INIT 0x80000592 / READ 0x80000593 / SEARCHFILE 0x80000597 / DISKREADY 0x8000059A; disc reads go through `Runtime::open_disc`, and unknown sids log-once for bring-up). (2) Kernel-area calls — boot trace showed the game memcpy's its own libkernel glue blobs from rodata into kernel RAM (0x00466158→0x80074000, 0x00467110→0x80075000, 0x00466920→0x80076000) and then calls into them; the blobs are seeded as analysis roots and `Runtime::alias` maps the kernel-area entry points onto the compiled sources (Runtime::call resolves kseg0 entries through aliases before the HLE nop). SLUS-21066 (Urbz) now runs its own old-libkernel self-hosting init end-to-end: the game replaces syscalls 131/90/91/84+ via SetSyscall, verifies them with FindAddress scans over the kernel table at 0x80000000+N*4, installs its SIF/CDVD glue blobs into kernel RAM (Copy syscall 0x5A) and proceeds into its timer-gated boot loop with live dlmalloc allocations. Supporting work: (1) SetSyscall mirrors handlers into the kernel table at 0x80000000+N*4 (kernel RAM), which is what the game's FindAddress scans verify; kernel Copy (0x5A) and block-install (0x83) HLE implemented; (2) Runtime::alias_range maps installed blob copies onto their compiled sources (the blobs are position-dependent and call each other through installed addresses); (3) analysis seeds forced-import functions outside .text (the glue blobs live in a non-exec data section); (4) Memory guards: 8-byte tail slack + mirror-wrap for >32MB guest pointers (real-HW tail reads / TLB-miss regions); (5) bring-up tooling: crash attribution (ctx.pc + SEH + first-chance AV logger with RAM-relative fault address), runner --iso support, build/relink/run scripts. CDVD: cdvdfsv SIF-RPC server (INIT/READ/SEARCHFILE/DISKREADY) answers EE libcdvd; unknown old-SIF packet formats hexdumped for the next iteration (cid=0x0C1102BE / 0x42000038 headers observed and logged).]
- **M10** Per-game launcher releases: disc-prompt GUI, boot-ELF/asset extraction, one shippable .exe per title  [in progress: app/game-launcher template]

## Decisions log

- C++20 + CMake, fresh repo, schema-compatible with PS2Recomp (chosen by owner).
- GPL-3.0-only license (required for two-way sharing with GPL-3.0 PS2Recomp).
- No external deps for core libs (ELF parser written in-house; no fmt — `snprintf`).
- CI: GitHub Actions, `ubuntu-latest` (GCC + Clang) + `windows-latest` (MSVC).

## Launcher apps

Two launchers share the ImGui + SDL3 stack (same C++ toolchain, Windows/Linux):

- `app/ee-studio` — developer/studio app (like ps2xStudio): game library, scan
  directories and ISOs, analyze -> configure (stub/skip editor) -> recompile ->
  build -> run, disassembly view, log console. [exists: skeleton]
- `app/game-launcher` — per-game **release** launcher template. The game's
  recompiled C++ and overrides are linked into a single executable; on first
  launch it shows a GUI that asks for the game's disc image (.iso/.bin), reads
  SYSTEM.CNF, extracts the boot ELF, and runs the game. No game assets are
  shipped — the user provides a legally obtained dump, the same model as
  N64Recomp / decomp ports (e.g. silent-hill-decomp). Extra launcher features
  (renderer settings, audio, controls, mods) plug into the Settings panel.
  [in progress: template]

See docs/LAUNCHER.md for the per-game release workflow.

## Open questions

- Project name: "EERecomp" is a placeholder.
- First target title(s) for M9 (suggest: something small, 2D, single ELF, no IOP modules).
- Whether/when to engage Ran-J (e.g., offer the decoder or VU1 lib once proven).

