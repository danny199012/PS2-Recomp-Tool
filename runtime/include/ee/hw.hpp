// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// PS2 hardware devices for the recompilation runtime: DMAC (DMA controller with
// chain-tag walking), VIF (command stream + UNPACK), GIF (tags -> GS), and a GS
// device (register file + VRAM; actual rendering is M6). DMA completes
// synchronously when a channel is kicked (games poll for completion).

#include <ee/types.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <unordered_map>
#include <ee/iop.hpp>
#include <unordered_map>
#include <vector>
#include <ee/cdvd.hpp>

namespace ee::rt {

struct Memory; // defined in runtime.hpp
struct Runtime; // defined in runtime.hpp
class Hw;

// --- GS (Graphics Synthesizer): register file + 4 MB VRAM ----------------------

class Gs {
public:
    std::array<u8, 4 * 1024 * 1024> vram{};
    std::unordered_map<u8, u64> hwregs;      // 64-bit GS registers (via GIF)
    std::array<u8, 0x120> priv{};            // privileged registers 0x12000000+
    u64 csr = 0;
    u64 imr = 0;
    u32 vertices_received = 0;               // stats (tests)
    u32 image_bytes_written = 0;

    // VRAM write pointer for GIF IMAGE mode (from BITBLTBUF/TRX* registers).
    u32 image_dst = 0;
    u32 image_width = 0; // in pixels (TODO: swizzled 2D layout)

    u64 read_priv(u32 addr) const;
    void write_priv(u32 addr, u64 value);
    void write_hwreg(u8 reg, u64 value);
    u64 read_hwreg(u8 reg) const;
    void add_vertex(u64 lo, u64 hi, u32 reg);
    void image_write(u64 lo, u64 hi);
};

// --- GIF: tag processing -> GS --------------------------------------------------

class Gif {
public:
    // Process a GIF stream (called by the DMAC GIF channel or VIF DIRECT).
    void feed(Gs& gs, const u8* data, size_t size);

    u32 tags_processed = 0; // stats (tests)

private:
    void packed_write(Gs& gs, u32 reg, u64 dlo, u64 dhi);
};

// --- VIF: command stream processing ---------------------------------------------

class Vif {
public:
    // Process a VIF command stream. Returns bytes consumed.
    size_t feed(Hw& hw, int which, const u8* data, size_t size);

    // Registers/state.
    u32 cycle_cl = 1, cycle_wl = 1; // STCYCL
    u32 offset = 0;                 // OFFSET
    u32 base = 0;                   // BASE
    u32 itop = 0;                   // ITOP
    u32 mode = 0;                   // STMOD
    u32 mask = 0;                   // STMASK
    u32 row[4] = {};                // STROW
    u32 col[4] = {};                // STCOL
    u32 mark = 0;                   // MARK
    // MSCAL: a VU microprogram kick was requested (M7 runs the interpreter).
    bool micro_kick = false;
    u32 micro_kick_addr = 0;

    u32 commands_processed = 0; // stats (tests)

private:
    size_t unpack(Hw& hw, int which, const u8* data, size_t size, size_t pos, u32 cmd, u32 num, u32 imm);
};

// --- DMAC -------------------------------------------------------------------------

class Dmac {
public:
    u32 read(u32 addr) const;
    void write(Hw& hw, u32 addr, u32 value); // kicks channels on CHCR.STR

    struct Channel {
        u32 chcr = 0;
        u32 madr = 0;
        u32 qwc = 0;
        u32 tadr = 0;
        u32 asr0 = 0;
        u32 asr1 = 0;
        u32 sadr = 0;
    };
    Channel ch[10];
    u32 d_ctrl = 0, d_stat = 0, d_pcr = 0, d_sqwc = 0, d_rbsr = 0, d_rbor = 0, d_stadr = 0;
    u32 d_enabler = 0, d_enablew = 0;

    u32 transfers = 0; // stats (tests)

    static int channel_from_addr(u32 addr);

private:
    void start(Hw& hw, int c);
    void run_chain(Hw& hw, int c);
    void transfer(Hw& hw, int c, u32 addr, u32 qwc, bool to_spr);
};

// --- Hw: owns all devices + VU memories, routes MMIO -------------------------------

class Hw {
public:
    Hw(); // seeds IOP-firmware boot state (SIF/allocator list heads in IOP RAM)

    Memory* mem = nullptr; // guest memory (set by Runtime)
    Runtime* runtime = nullptr; // set by Runtime ctor (used for the console sink)

    Gs gs;
    Gif gif;
    Vif vif[2];
    Dmac dmac;

    // VU local memories (shared with the EE for VU0 macro mode).
    std::array<u8, 0x1000> vu0_dmem{}; // 4 KB
    std::array<u8, 0x1000> vu0_imem{}; // 4 KB
    std::array<u8, 0x4000> vu1_dmem{}; // 16 KB
    std::array<u8, 0x4000> vu1_imem{}; // 16 KB

    std::array<u8, 0x100> timers{}; // stub register file

    Iop iop;               // IOP HLE (SIF, CDVD, pad, MC)

    // The disc image backing the CDVD HLE (empty = no disc loaded). The Iop's
    // cdvdfsv RPC server reads sectors/files through this (see iop.cpp).
    Cdvd cdvd;

    // IOP shared memory as seen from the EE: the IOP's RAM/scratchpad/SBUS
    // region 0x1F000000-0x1FFFFFFF aliased into EE physical space. The game
    // installs IOP-side structures (thread tables, RPC servers, function
    // pointers) here and reads them back, so it must be backed by storage.
    // The SIF register file inside this range is handled by Iop separately.
    std::vector<u8> iop_mem = std::vector<u8>(0x1000000, 0);

    bool vu_micro_run = false;  // set by VIF MSCAL before VU1 step

    u64 mmio_read(u32 addr, u32 size);
    void mmio_write(u32 addr, u64 value, u32 size);

    // Guest memory access for DMA (physical addresses).
    const u8* read(u32 addr, u32 size) const;
    u8* write_ptr(u32 addr, u32 size);
    u32 read32(u32 addr) const;
    void write32(u32 addr, u32 value);

    std::set<u32> reported_mmio; // log-once
};

} // namespace ee::rt
