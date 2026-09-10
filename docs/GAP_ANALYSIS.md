# Gap Analysis: EERecomp vs PS2Recomp

Assessment of what's implemented, what's missing, and prioritized next steps.
This project (EERecomp) is built to complement [Ran-J's PS2Recomp](https://github.com/ran-j/PS2Recomp),
not compete with it — components are designed to be reusable/upstreamable.

## What EERecomp has (and PS2Recomp doesn't, or is incomplete)

| Component | Status | PS2Recomp |
|---|---|---|
| VU1 microcode interpreter | **Done** (VU0/VU1 micro interpreter, 540 LOC) | Explicitly incomplete |
| GS software rasterizer | Partial (tri/sprite/line, framebuffer readback) | "Needs external implementation" |
| GS VRAM swizzle helpers | Partial | Not mentioned |
| ISO9660/UDF disc reader | Done (CDVD, boot ELF extraction, file reads) | Not mentioned |
| DMA/VIF/GIF device emulation | Done (chain-tag walking, UNPACK, GIF tags) | Not mentioned |

**These are EERecomp's prime contribution to the PS2Recomp ecosystem.** The VU1
microcode interpreter and the GS/DMA/VIF/GIF pipeline are PS2Recomp's stated gaps.

## What PS2Recomp has that EERecomp now has (parity)

| Feature | EERecomp | PS2Recomp |
|---|---|---|
| ELF → C++ static recompilation | Done | Done |
| R5900 decode (core + MMI + FPU + VU0 macro) | Done | Done |
| Function discovery (symbols, recursive descent, prologue scan) | Done | Done (analyzer) |
| TOML config (stubs/skip/patches, handler@0xADDR) | Done | Done |
| CSV/JSON function import (Ghidra/Aura) | Done | Done |
| Game override system (per-ELF + CRC32) | Done | Done |
| Multi-file C++ output | **Added** | Done |
| Generic return handlers (ret0/ret1/reta0) | **Added** | Done |

## What's still missing (prioritized)

### P0 — Blocks compiling/running any real game

1. **IOP HLE services (SIF RPC, file loading, pad input)**
   - Current state: SIF DMA acks (zeros), CDVD returns zeros, pad is static,
     memory card is empty, SPU2 is a no-op. RPC handlers table exists but is empty.
   - Impact: Games can't load files (SifLoadFileInit/sceCdRead), read controller
     input, or play audio. This is the #1 blocker for commercial titles.
   - PS2Recomp has `ps2xIOP` with game profiles and a C plugin ABI for this.

2. **GS texture mapping + VRAM swizzle**
   - Current state: Software rasterizer draws flat-shaded tris/sprites to a
     linear framebuffer. No texture sampling, no proper GS VRAM block-based
     addressing (uses linear approximation).
   - Impact: Games render as flat-colored shapes at best, garbled at worst.
   - The swizzle helpers exist in `gs_swizzle.hpp` but aren't wired into the
     renderer's texture path or framebuffer readback.

3. **Syscall coverage gaps**
   - Current state: ~80 syscalls handled, many fall through to `return 0`.
     Missing: file I/O (open/read/write/close/lseek), directory ops, module
     loading (SifLoadModule), timer/alarm (SetAlarm/ReleaseAlarm are stubs),
     event flags (CreateEventFlag returns an id but Wait/Set are no-ops).
   - Impact: Games that use file I/O or timer callbacks won't function.

### P1 — Improves usability and porting workflow

4. **Relocation-symbol auto-binding at J/JAL callsites**
   - Current: `call(ctx, 0xADDR)` for all jal targets, resolved at runtime via
     the dispatch table. If the target is a known import (e.g. `sceCdRead`)
     that should be a stub, it's only stubbed if the TOML binds it by address.
   - PS2Recomp tries relocation symbols at callsites to auto-bind known names.
   - Impact: Stripped games require less manual TOML annotation.

5. **Ghidra export script**
   - Current: Accepts CSV/JSON from any tool, but ships no export script.
   - PS2Recomp ships `ExportPS2Functions.java` for Ghidra.
   - Impact: Easier onboarding for Ghidra users.

6. **Display output to SDL window**
   - Current: `GsRenderer::read_framebuffer_rgba()` produces an RGBA buffer, but
     nothing copies it to the SDL window's texture. The game-launcher and
     ee-studio don't render the framebuffer.
   - Impact: Even if the GS draws correctly, you can't see it.

### P2 — Future / nice-to-have

7. **Vulkan GS renderer** (planned in PLAN.md M6)
8. **SPU2 audio emulation** (planned, currently a stub)
9. **Differential testing vs PCSX2** (planned in PLAN.md testing strategy)
10. **Multi-threaded parallel codegen** (PS2Recomp has this)

## Recommended iteration order

1. Wire the GS framebuffer to the SDL window (P1.6) — quick win, makes progress visible
2. IOP file loading via SIF RPC (P0.1) — unblocks disc-based games
3. GS texture mapping + swizzle (P0.2) — makes graphics correct
4. Syscall file I/O (P0.3) — complements IOP work
5. Relocation auto-binding (P1.4) — reduces manual config burden
