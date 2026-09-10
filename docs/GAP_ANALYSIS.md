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

6. **Display output to SDL window** — **Done** (game-launcher)
   - `GIF FINISH -> Runtime::on_frame` wiring now flushes the software renderer
     and copies the GS framebuffer to a buffer the GUI thread presents via an
     SDL texture in a "Game Display" window. Games that send a GS FINISH tag
     (most 3D titles) show their output live.
   - Caveats: the GS rasterizer still lacks texture mapping / proper VRAM
     swizzle, so visuals are flat-shaded and colors may be wrong; and ee-studio
     does not display the framebuffer yet (only game-launcher does).

### P0.5 — IOP host-services foundation (partially done)

The IOP HLE is the key to "works per-game without individual patches": instead
of patching each game's functions, emulate the **standard PS2 SDK services**
every game calls through the SIF (CDVD file reads, pad input, memory card,
audio). Games using the stock SDK then work generically; per-game overrides are
only needed for non-standard cases.

- **Done**: `Iop::set_cdvd()` attaches the host ISO so `cdvd_read()` reads real
  disc sectors; `disc_type()` reports DVD; pad get/set is thread-safe for
  host-sourced input; the game-launcher injects the opened disc and keyboard
  (as pad port 0) into the running game.
- **Still needed**: bind the sdk-named functions (from Aura/Ghidra imports) to
  handlers that use these services — `sceCdRead/ReadSync`, `sceCdSearchFile`,
  `scePadRead`, `sceMc*`, `SifLoadModule`, `printf`/`scePrintf`. The import
  names are already available (e.g. "386 SDK-named" in the user's Aura export),
  so `stubs = ["sceCdRead", ...]` can bind them once the handlers exist.

### P2 — Future / nice-to-have

7. **Vulkan GS renderer** (planned in PLAN.md M6)
8. **SPU2 audio emulation** (planned, currently a stub)
9. **Differential testing vs PCSX2** (planned in PLAN.md testing strategy)
10. **Multi-threaded parallel codegen** (PS2Recomp has this)

## Recommended iteration order

1. **Done**: GS framebuffer -> SDL window (game-launcher "Game Display").
2. Standard-SDK handler set (sceCd*, scePad*, sceMc*, printf) — unblocks most
   games' boot path without per-game patches. The IOP plumbing (host ISO + pad)
   is already in place; this is binding + a few dozen ABI-correct handlers.
3. GS texture mapping + swizzle (P0.2) — makes graphics correct.
4. Syscall file I/O (P0.3) — complements the IOP work.
5. Relocation auto-binding (P1.4) — reduces manual config burden.
