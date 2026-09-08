# Integration

EERecomp is built to interoperate with the surrounding tooling ecosystem.

## Aura Decomp Tool (danny199012/Aura-Decomp-Tool)

Aura is a Ghidra-style analysis front-end (Tauri app + headless `aura-cli`) for
PS1/PS2 MIPS binaries, with function detection and SDK symbol fingerprinting
(SHA-1) — exactly what a recompiler wants as input, especially for stripped ELFs.

### Aura -> EERecomp

Export Aura's function/symbol list and feed it to `ee-analyze` / `ee-recomp`.
Two accepted formats:

CSV (Ghidra-style, either column order tolerated):

    address,name
    0x00100000,main
    0x00100080,helper

JSON (Aura-style):

    [
      {"address": "0x00100000", "name": "main"},
      {"address": 1048576, "name": "helper"}
    ]

Usage:

    ee-analyze game.elf --import aura_functions.json --toml game.toml --csv out.csv
    ee-recomp  game.elf --import aura_functions.json --config game.toml --out game.recomp.cpp

With `aura-cli` today, shape its JSON output with `jq`, e.g.:

    aura-cli <binary> --json | jq '[.[] | {address: .start, name: .name}]' > functions.json

(A dedicated `aura-cli export --format eerecomp` can be added on the Aura side
later; the formats above are the stable contract.)

### EERecomp -> Aura (future)

Aura's disassembler currently covers the R3000 (PS1) ISA. Our `libs/r5900`
decoder covers the full R5900 (MMI, FPU, VU0 macro mode). A small C ABI wrapper
(`extern "C" ee_r5900_disasm(word, va, out, cap)`) would let Aura's Rust core
use it via FFI for PS2 Emotion Engine disassembly.

## PS2Recomp (ran-j/PS2Recomp)

- TOML config schema compatible: `[general]` `stubs` / `skip`, `handler@0xADDR`
  address binding, `[patches]` `instructions`.
- Function lists use the same Ghidra-style CSV interchange PS2Recomp documents.
- Our decoder / analyzer / runtime are GPL-3.0 C++20 libraries that can be
  dropped into or upstreamed to PS2Recomp (its stated gaps: VU1 microcode, GS).

## Ghidra

Any Ghidra export that can be shaped as `address,name` CSV works via `--import`.
