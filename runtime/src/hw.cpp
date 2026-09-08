// SPDX-License-Identifier: GPL-3.0-only
#include <ee/hw.hpp>
#include <ee/iop.hpp>
#include <ee/runtime.hpp>
#include <ee/vu1.hpp>

#include <cstdio>
#include <cstring>

namespace ee::rt {

// --- GS --------------------------------------------------------------------------

u64 Gs::read_priv(u32 addr) const {
    if (addr == 0x12001000)
        return csr;
    if (addr == 0x12001010)
        return imr;
    if (addr >= 0x12000000 && addr < 0x12000120) {
        u64 v = 0;
        for (u32 i = 0; i < 8 && addr + i < 0x12000120; ++i)
            v |= u64(priv[addr - 0x12000000 + i]) << (8 * i);
        return v;
    }
    return 0;
}

void Gs::write_priv(u32 addr, u64 value) {
    if (addr == 0x12001000) { // CSR: write 1s reset flags (simplified)
        csr &= ~value;
        return;
    }
    if (addr == 0x12001010) {
        imr = value;
        return;
    }
    if (addr >= 0x12000000 && addr < 0x12000120) {
        for (u32 i = 0; i < 8 && addr + i < 0x12000120; ++i)
            priv[addr - 0x12000000 + i] = u8(value >> (8 * i));
    }
}

void Gs::write_hwreg(u8 reg, u64 value) {
    hwregs[reg] = value;
    if (reg == 0x50) // BITBLTBUF: DBP at bits 32-45 (TODO: verify unit = 2048 bytes)
        image_dst = u32((value >> 32) & 0x3FFF) << 11;
    if (reg == 0x53) // TRXREG: pixel width
        image_width = u32(value & 0xFFF);
}

u64 Gs::read_hwreg(u8 reg) const {
    auto it = hwregs.find(reg);
    return it != hwregs.end() ? it->second : 0;
}

void Gs::add_vertex(u64 lo, u64 hi, u32 reg) {
    ++vertices_received;
    (void)lo;
    (void)hi;
    (void)reg; // M6: vertex processing / drawing
}

void Gs::image_write(u64 lo, u64 hi) {
    // TODO(M6): GS VRAM uses a swizzled 2D block layout; this is a linear
    // approximation sufficient for pipeline testing.
    if (image_dst + 16 <= vram.size()) {
        std::memcpy(vram.data() + image_dst, &lo, 8);
        std::memcpy(vram.data() + image_dst + 8, &hi, 8);
    }
    image_dst += 16;
    image_bytes_written += 16;
}

// --- GIF ---------------------------------------------------------------------------

void Gif::packed_write(Gs& gs, u32 reg, u64 dlo, u64 dhi) {
    switch (reg) {
    case 0x00: gs.write_hwreg(0x00, dlo); break; // PRIM
    case 0x01: gs.write_hwreg(0x01, dlo); break; // RGBAQ (RGBA bytes + Q float)
    case 0x02: gs.write_hwreg(0x02, dlo); break; // ST
    case 0x03: gs.write_hwreg(0x03, dlo); break; // UV
    case 0x04:                                   // XYZF2
    case 0x05:                                   // XYZ2
    case 0x0C:                                   // XYZF3
    case 0x0D:                                   // XYZ3
        gs.add_vertex(dlo, dhi, reg);
        break;
    case 0x06: case 0x07: // TEX0_1 / TEX0_2
    case 0x08: case 0x09: // CLAMP_1 / CLAMP_2
        gs.write_hwreg(reg, dlo);
        break;
    case 0x0A: // FOG: A at quad bits 96-103 (TODO: verify)
        gs.write_hwreg(0x0A, (dhi >> 32) & 0xFF);
        break;
    case 0x0E: // AD: address in high-qword low byte, value in low qword
        gs.write_hwreg(u8(dhi & 0xFF), dlo);
        break;
    case 0x0F: // NOP
    default:
        break;
    }
}

void Gif::feed(Gs& gs, const u8* data, size_t size) {
    size_t pos = 0;
    while (pos + 16 <= size) {
        u64 lo, hi;
        std::memcpy(&lo, data + pos, 8);
        std::memcpy(&hi, data + pos + 8, 8);
        pos += 16;
        ++tags_processed;

        const u32 nloop = u32(lo & 0x7FFF);
        const bool pre = (lo >> 46) & 1;
        const u32 prim = u32((lo >> 33) & 0x7FF);
        const u32 flg = u32((lo >> 44) & 3);
        u32 nreg = u32((lo >> 47) & 0xF);
        const u64 regs = hi;
        if (nreg == 0)
            nreg = 16;
        if (pre)
            gs.write_hwreg(0x00, prim); // PRIM

        if (flg >= 2) { // IMAGE / IMAGE2: nloop quads of pixel data
            for (u32 i = 0; i < nloop && pos + 16 <= size; ++i) {
                u64 dlo, dhi;
                std::memcpy(&dlo, data + pos, 8);
                std::memcpy(&dhi, data + pos + 8, 8);
                pos += 16;
                gs.image_write(dlo, dhi);
            }
            continue;
        }

        for (u32 l = 0; l < nloop; ++l) {
            for (u32 r = 0; r < nreg; ++r) {
                const u32 reg = u32((regs >> (r * 4)) & 0xF);
                if (flg == 0) { // PACKED: 128-bit data
                    if (pos + 16 > size)
                        return;
                    u64 dlo, dhi;
                    std::memcpy(&dlo, data + pos, 8);
                    std::memcpy(&dhi, data + pos + 8, 8);
                    pos += 16;
                    packed_write(gs, reg, dlo, dhi);
                } else { // REGLIST: 64-bit data
                    if (pos + 8 > size)
                        return;
                    u64 val;
                    std::memcpy(&val, data + pos, 8);
                    pos += 8;
                    gs.write_hwreg(u8(reg), val);
                }
            }
        }
    }
}

// --- VIF ---------------------------------------------------------------------------
// Command word: bits 24-31 = command, bits 16-23 = NUM, bits 0-15 = immediate.
// UNPACK = 0x60 | (us<<4) | (vn<<2) | vl  (vn: 0=S,1=V2,2=V3,3=V4; vl: 0=32,1=16,2=8,
// 3=V4_5 for vn=3; us: unsigned flag). Semantics cross-checked with PCSX2
// Vif_Unpack.cpp (V2 repeats v0v1 into zw; V3 uses V4 logic with w overwritten).

namespace {

u32 sext(u32 v, int bits) {
    const u32 sign = 1u << (bits - 1);
    return (v ^ sign) - sign;
}

} // namespace

size_t Vif::unpack(Hw& hw, int which, const u8* data, size_t size, size_t pos, u32 cmd, u32 num, u32 imm) {
    u8* dmem = which == 0 ? hw.vu0_dmem.data() : hw.vu1_dmem.data();
    const u32 dmem_size = which == 0 ? 0x1000 : 0x4000;
    const u32 vn = (cmd >> 2) & 3;
    const u32 vl = cmd & 3;
    const bool uns = (cmd >> 4) & 1;
    // TODO: STCYCL cycling (cl/wl) and STMOD/OFFSET/BASE/STMASK are simplified
    // (cl=1, wl=1, no mask) — the common case for VU uploads.
    u32 addr = (imm & 0x3FF) * 16; // destination in VU memory (128-bit word units)

    auto write_quad = [&](u32 x, u32 y, u32 z, u32 w) {
        if (addr + 16 > dmem_size)
            return;
        u32* q = reinterpret_cast<u32*>(dmem + addr);
        q[0] = x;
        q[1] = y;
        q[2] = z;
        q[3] = w;
        addr += 16;
    };
    auto read_elem = [&](u32 idx) -> u32 { // read element idx from source stream
        const u32 bits = vl == 0 ? 32 : (vl == 1 ? 16 : 8);
        const u32 bytes = bits / 8;
        const size_t p = pos + size_t(idx) * bytes;
        if (p + bytes > size)
            return 0;
        u32 v = 0;
        for (u32 i = 0; i < bytes; ++i)
            v |= u32(data[p + i]) << (8 * i);
        if (!uns)
            v = sext(v, int(bits));
        return v;
    };

    const u32 elems = vn == 0 ? 1 : (vn == 1 ? 2 : (vn == 2 ? 3 : 4));
    for (u32 n = 0; n < num; ++n) {
        if (vl == 3 && vn == 3) { // V4_5: one u32 -> four 8-bit channels
            const u32 v32 = read_elem(n);
            const u32 x = (v32 & 0x1F) << 3;
            const u32 y = ((v32 >> 5) & 0x1F) << 3;
            const u32 z = ((v32 >> 10) & 0x1F) << 3;
            const u32 w = (v32 & 0x8000) ? 0xFF : 0;
            write_quad(x, y, z, w);
            continue;
        }
        const u32 base = n * elems;
        const u32 v0 = read_elem(base + 0);
        const u32 v1 = elems >= 2 ? read_elem(base + 1) : 0;
        const u32 v2 = elems >= 3 ? read_elem(base + 2) : 0;
        const u32 v3 = elems >= 4 ? read_elem(base + 3) : 0;
        switch (vn) {
        case 0: write_quad(v0, 0, 0, 0); break;          // S
        case 1: write_quad(v0, v1, v0, v1); break;       // V2: v1v0v1v0 (per PCSX2)
        case 2: write_quad(v0, v1, v2, 0); break;        // V3 (w overwritten next unpack)
        default: write_quad(v0, v1, v2, v3); break;      // V4
        }
    }
    // advance source position
    if (vl == 3 && vn == 3)
        pos += size_t(num) * 4;
    else
        pos += size_t(num) * elems * (vl == 0 ? 4 : (vl == 1 ? 2 : 1));
    return pos;
}

size_t Vif::feed(Hw& hw, int which, const u8* data, size_t size) {
    size_t pos = 0;
    while (pos + 4 <= size) {
        u32 cmd_word;
        std::memcpy(&cmd_word, data + pos, 4);
        pos += 4;
        ++commands_processed;
        const u32 cmd = cmd_word >> 24;
        const u32 num = (cmd_word >> 16) & 0xFF;
        const u32 imm = cmd_word & 0xFFFF;
        switch (cmd) {
        case 0x00: break; // NOP
        case 0x01: // STCYCL
            cycle_cl = imm & 0xFF;
            cycle_wl = (imm >> 8) & 0xFF;
            break;
        case 0x02: offset = imm & 0x3FF; break;  // OFFSET
        case 0x03: base = imm & 0x3FF; break;    // BASE
        case 0x04: itop = imm & 0x3FF; break;    // ITOP
        case 0x05: mode = imm & 3; break;        // STMOD
        case 0x06: break;                        // MSKPATH3 (TODO)
        case 0x07: mark = imm; break;            // MARK
        case 0x08: case 0x09: case 0x13: break;  // FLUSHE/FLUSH/FLUSHA (sync; nop here)
        case 0x14: case 0x15: { // MSCAL / MSCALF: kick VU microprogram
            micro_kick = true;
            micro_kick_addr = imm;
            run_vu_microcode(hw, which, imm);
            break;
        }
        case 0x17: break; // MSCNT
        case 0x20: // STMASK
            if (pos + 4 > size) return pos;
            std::memcpy(&mask, data + pos, 4);
            pos += 4;
            break;
        case 0x30: // STROW
            if (pos + 16 > size) return pos;
            for (int i = 0; i < 4; ++i) {
                std::memcpy(&row[i], data + pos, 4);
                pos += 4;
            }
            break;
        case 0x31: // STCOL
            if (pos + 16 > size) return pos;
            for (int i = 0; i < 4; ++i) {
                std::memcpy(&col[i], data + pos, 4);
                pos += 4;
            }
            break;
        case 0x4A: { // MPG: upload num 64-bit microcode words to VU imem at imm
            u8* imem = which == 0 ? hw.vu0_imem.data() : hw.vu1_imem.data();
            const u32 imem_size = which == 0 ? 0x1000 : 0x4000;
            u32 addr = (imm & 0x1FF) * 8;
            for (u32 i = 0; i < num; ++i) {
                if (pos + 8 > size)
                    return pos;
                if (addr + 8 <= imem_size)
                    std::memcpy(imem + addr, data + pos, 8);
                addr += 8;
                pos += 8;
            }
            break;
        }
        case 0x50: case 0x51: { // DIRECT / DIRECTHL: num quads -> GIF (VIF1)
            const u32 quads = num == 0 ? 0x10000 : num;
            const size_t bytes = size_t(quads) * 16;
            if (pos + bytes > size)
                return pos;
            if (which == 1)
                hw.gif.feed(hw.gs, data + pos, bytes);
            pos += bytes;
            break;
        }
        default:
            if ((cmd & 0x60) == 0x60) { // UNPACK
                pos = unpack(hw, which, data, size, pos, cmd, num, imm);
            } else {
                std::fprintf(stderr, "[vif%d] unknown command 0x%02X (skipped)\n", which, cmd);
            }
            break;
        }
    }
    return pos;
}

// --- DMAC --------------------------------------------------------------------------
// Register map verified against PCSX2's Hw.h. Channels: 0=VIF0, 1=VIF1, 2=GIF,
// 3=fromIPU, 4=toIPU, 5=SIF0, 6=SIF1, 7=SIF2, 8=fromSPR, 9=toSPR.
// Tag control word: QWC bits 0-15, PCE 26-27, ID 28-30, IRQ 31; ADDR bits 0-30,
// SPR bit 31. CHCR: bit 0 = DIR, bits 2-3 = mode (0=normal, 1=chain), bit 8 = STR.

int Dmac::channel_from_addr(u32 addr) {
    switch (addr & 0xFFFFFF00) {
    case 0x10008000: return 0;
    case 0x10009000: return 1;
    case 0x1000A000: return 2;
    case 0x1000B000: return 3;
    case 0x1000B400: return 4;
    case 0x1000C000: return 5;
    case 0x1000C400: return 6;
    case 0x1000C800: return 7;
    case 0x1000D000: return 8;
    case 0x1000D400: return 9;
    default: return -1;
    }
}

u32 Dmac::read(u32 addr) const {
    const int c = channel_from_addr(addr);
    if (c >= 0) {
        switch (addr & 0xFF) {
        case 0x00: return ch[c].chcr;
        case 0x10: return ch[c].madr;
        case 0x20: return ch[c].qwc;
        case 0x30: return ch[c].tadr;
        case 0x40: return ch[c].asr0;
        case 0x50: return ch[c].asr1;
        case 0x80: return ch[c].sadr;
        default: return 0;
        }
    }
    switch (addr) {
    case 0x1000E000: return d_ctrl;
    case 0x1000E010: return d_stat;
    case 0x1000E020: return d_pcr;
    case 0x1000E030: return d_sqwc;
    case 0x1000E040: return d_rbsr;
    case 0x1000E050: return d_rbor;
    case 0x1000E060: return d_stadr;
    case 0x1000E0F0: return d_enabler;
    case 0x1000E0F4: return d_enablew;
    default: return 0;
    }
}

void Dmac::write(Hw& hw, u32 addr, u32 value) {
    const int c = channel_from_addr(addr);
    if (c >= 0) {
        Channel& chn = ch[c];
        switch (addr & 0xFF) {
        case 0x00: { // CHCR
            const bool was_str = (chn.chcr & 0x100) != 0;
            chn.chcr = value;
            if ((value & 0x100) && !was_str)
                start(hw, c); // STR rising edge: run the transfer synchronously
            break;
        }
        case 0x10: chn.madr = value; break;
        case 0x20: chn.qwc = value; break;
        case 0x30: chn.tadr = value; break;
        case 0x40: chn.asr0 = value; break;
        case 0x50: chn.asr1 = value; break;
        case 0x80: chn.sadr = value; break;
        default: break;
        }
        return;
    }
    switch (addr) {
    case 0x1000E000: d_ctrl = value; break;
    case 0x1000E010: d_stat = value; break;
    case 0x1000E020: d_pcr = value; break;
    case 0x1000E030: d_sqwc = value; break;
    case 0x1000E040: d_rbsr = value; break;
    case 0x1000E050: d_rbor = value; break;
    case 0x1000E060: d_stadr = value; break;
    case 0x1000E0F0: d_enabler = value; break;
    case 0x1000E0F4: d_enablew = value; break;
    default: break;
    }
}

void Dmac::start(Hw& hw, int c) {
    Channel& chn = ch[c];
    const u32 mode = (chn.chcr >> 2) & 3;
    ++transfers;
    if (mode == 1) {
        run_chain(hw, c);
    } else {
        // normal mode: transfer QWC quads from MADR
        transfer(hw, c, chn.madr, chn.qwc, false);
    }
    chn.chcr &= ~0x100u; // transfer complete: clear STR
}

void Dmac::run_chain(Hw& hw, int c) {
    Channel& chn = ch[c];
    u32 tadr = chn.tadr;
    u32 asr_stack[2] = {chn.asr0, chn.asr1};
    int asr_sp = 0;
    bool done = false;
    int guard = 100000;
    while (!done && guard-- > 0) {
        const u32 tag = hw.read32(tadr);
        const u32 qwc = tag & 0xFFFF;
        const u32 id = (tag >> 28) & 7;
        const u32 addr = tag & 0x7FFFFFFF;
        const bool spr = (tag >> 31) & 1;
        u32 data_addr = 0;
        u32 next_tadr = 0;
        switch (id) {
        case 0: // REFE: transfer from ADDR, end
            data_addr = addr;
            done = true;
            break;
        case 1: // CNT: data follows tag, next tag follows data
            data_addr = tadr + 16;
            next_tadr = tadr + 16 + qwc * 16;
            break;
        case 2: // NEXT: data follows tag, next tag at ADDR
            data_addr = tadr + 16;
            next_tadr = addr;
            break;
        case 3: // REF:  transfer from ADDR, next tag follows
        case 4: // REFS: same (stall control ignored)
            data_addr = addr;
            next_tadr = tadr + 16;
            break;
        case 5: // CALL: data follows tag, push return, jump to ADDR
            data_addr = tadr + 16;
            if (asr_sp < 2)
                asr_stack[asr_sp++] = tadr + 16 + qwc * 16;
            next_tadr = addr;
            break;
        case 6: // RET: data follows tag, pop return; empty stack = end
            data_addr = tadr + 16;
            if (asr_sp > 0)
                next_tadr = asr_stack[--asr_sp];
            else
                done = true;
            break;
        case 7: // END: data follows tag, end
        default:
            data_addr = tadr + 16;
            done = true;
            break;
        }
        if (qwc > 0)
            transfer(hw, c, data_addr, qwc, spr);
        tadr = next_tadr;
    }
    chn.tadr = tadr;
}

void Dmac::transfer(Hw& hw, int c, u32 addr, u32 qwc, bool to_spr) {
    (void)to_spr; // SPR flag handled by channel direction
    const size_t bytes = size_t(qwc) * 16;
    if (bytes == 0)
        return;
    switch (c) {
    case 0: // VIF0
    case 1: { // VIF1
        const u8* src = hw.read(addr, bytes);
        if (src)
            hw.vif[c].feed(hw, c, src, bytes);
        break;
    }
    case 2: { // GIF
        const u8* src = hw.read(addr, bytes);
        if (src)
            hw.gif.feed(hw.gs, src, bytes);
        break;
    }
    case 8: { // fromSPR: scratchpad -> RAM
        Channel& chn = ch[c];
        const u32 sadr = chn.sadr & 0x3FFF;
        const u8* src = hw.mem->translate(0x70000000 + sadr);
        u8* dst = hw.write_ptr(addr, bytes);
        if (dst)
            std::memcpy(dst, src, bytes);
        break;
    }
    case 9: { // toSPR: RAM -> scratchpad
        Channel& chn = ch[c];
        const u32 sadr = chn.sadr & 0x3FFF;
        const u8* src = hw.read(addr, bytes);
        u8* dst = hw.mem->translate(0x70000000 + sadr);
        if (src)
            std::memcpy(dst, src, bytes);
        break;
    }
    case 5: { // SIF0: IOP -> EE (receive)
        const u8* src = hw.read(addr, bytes);
        if (src)
            hw.iop.sif0_recv(hw, src, u32(bytes));
        break;
    }
    case 6: { // SIF1: EE -> IOP (send)
        u8* dst = hw.write_ptr(addr, bytes);
        if (dst)
            hw.iop.sif1_send(hw, dst, u32(bytes));
        break;
    }
    case 7: { // SIF2: control
        hw.iop.sif2_control(hw, 0);
        break;
    }
    default:
        std::fprintf(stderr, "[dmac] channel %d transfer of %u quads (stub)\n", c, qwc);
        break;
    }
}

// --- Hw: MMIO routing + guest memory access ----------------------------------------

const u8* Hw::read(u32 addr, u32 size) const {
    (void)size;
    if (!mem)
        return nullptr;
    // DMA reads are physical; translate handles RAM + scratchpad.
    return mem->translate(addr);
}

u8* Hw::write_ptr(u32 addr, u32 size) {
    (void)size;
    if (!mem)
        return nullptr;
    return mem->translate(addr);
}

u32 Hw::read32(u32 addr) const {
    u32 v = 0;
    if (mem)
        std::memcpy(&v, mem->translate(addr), 4);
    return v;
}

void Hw::write32(u32 addr, u32 value) {
    if (mem)
        std::memcpy(mem->translate(addr), &value, 4);
}

u64 Hw::mmio_read(u32 addr, u32 size) {
    (void)size;
    // DMAC
    if ((addr >= 0x10008000 && addr < 0x1000E000) || (addr >= 0x1000E000 && addr < 0x1000F000))
        return dmac.read(addr);
    // GS privileged registers
    if (addr >= 0x12000000 && addr < 0x12001000)
        return gs.read_priv(addr);
    // VIF/GIF status registers (stub)
    if (addr >= 0x10003000 && addr < 0x10004000)
        return 0; // GIF_CTRL/STAT, VIF0/1 STAT/FBRST etc.
    if (addr >= 0x10000000 && addr < 0x10002000) // timers
        return 0;
    if (reported_mmio.insert(addr).second)
        std::fprintf(stderr, "[hw] mmio read  0x%08X (size %u) -> 0 (stub)\n", addr, size);
    return 0;
}

void Hw::mmio_write(u32 addr, u64 value, u32 size) {
    (void)size;
    // EE STDOUT (sio / debug console)
    if (addr == 0x1000F180) {
        std::putchar(char(value & 0xFF));
        std::fflush(stdout);
        return;
    }
    // DMAC (may kick a channel)
    if ((addr >= 0x10008000 && addr < 0x1000E000) || (addr >= 0x1000E000 && addr < 0x1000F000)) {
        dmac.write(*this, addr, u32(value));
        return;
    }
    // GS privileged registers
    if (addr >= 0x12000000 && addr < 0x12001000) {
        gs.write_priv(addr, value);
        return;
    }
    // VIF/GIF control registers (stub)
    if (addr >= 0x10003000 && addr < 0x10004000)
        return;
    if (addr >= 0x10000000 && addr < 0x10002000) // timers
        return;
    if (reported_mmio.insert(addr).second)
        std::fprintf(stderr, "[hw] mmio write 0x%08X = 0x%llX (size %u) (stub)\n", addr,
                     (unsigned long long)value, size);
}

} // namespace ee::rt
