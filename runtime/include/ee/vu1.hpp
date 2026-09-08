// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VU microcode interpreter: executes programs uploaded to VU instruction memory
// via VIF MPG commands. Called when VIF issues MSCAL. Supports both VU0 and VU1.
//
// VU microcode format: each instruction is 64 bits (lower 32 + upper 32).
// Upper instruction (HI word): float ALU ops (ADD/SUB/MUL/MAX/MIN/MADD/MSUB/etc.)
// Lower instruction (LO word): integer/load/store/branch/divide ops.
// Both execute simultaneously; we do upper first, then lower.

#include <ee/hw.hpp>
#include <ee/types.hpp>

#include <cstdint>

namespace ee::rt {

struct VuMicroCtx {
    u128 vf[32] = {};    // vector float registers (vf0 reads as 1,0,0,0)
    u16 vi[16] = {};     // integer registers (vi0 reads as 0, vi1 reads as 1)
    u128 acc = {};       // accumulator
    u32 q = 0;           // divide result (float bits)
    u32 p = 0;           // EFU result
    u32 I_val = 0;       // immediate register (float bits)
    u32 R = 0;           // random (23-bit)
    u32 status = 0;     // status flags
    u32 mac = 0;        // mac flags
    u32 clip = 0;       // clip flags
    u32 pc = 0;          // program counter (in 64-bit word units)
    u32 start_pc = 0;    // where the program started (for return)
    u32 imem_size = 0;   // instruction memory size in bytes
    u32 dmem_size = 0;   // data memory size in bytes
    u8* imem = nullptr;  // pointer to instruction memory
    u8* dmem = nullptr;  // pointer to data memory
    bool running = false;
};

// Run a VU microprogram. `vu_num` selects VU0 or VU1 (selects memory sizes).
void run_vu_microcode(Hw& hw, int vu_num, u32 start_addr);

// Execute a single VU microcode instruction (for testing).
void vu_micro_step(VuMicroCtx& ctx, u64 inst);

} // namespace ee::rt
