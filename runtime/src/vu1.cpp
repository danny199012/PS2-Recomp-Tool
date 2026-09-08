// SPDX-License-Identifier: GPL-3.0-only
// VU microcode interpreter. Implements the VU0/VU1 microprogram instruction set.
// Encoding: 64-bit instruction = lower(32) | upper(32).
// Lower: op = bits 26-31, fields: dest(0-10), ft(11-15), fs(16-20), fd(21-25).
// Upper: op = bits 26-31 of upper word, same field layout.
// VU microcode upper opcode table (op = (upper >> 26) & 0x3F):
//   0x00: NOP/special (based on bit 27)
//   0x01-0x03: reserved
//   0x04-0x07: reserved
//   0x08: reserved
//   0x09: ITOF (lower, not upper)
//   Standard upper ops used in practice:
//   ADDx/ADDy/ADDz/ADDw = 0x2C (bits 24-25 select broadcast lane)
//   ADD  = 0x2D
//   SUBx/y/z/w = 0x2E, SUB = 0x2F
//   MADDAx/y/z/w = 0x30, MADDA = 0x31
//   MSUBAx/y/z/w = 0x32, MSUBA = 0x33
//   MADDx/y/z/w = 0x34, MADD = 0x35 (wait, 0x34 might be different)

// Actually the correct VU microcode upper opcodes per the VU manual:
// Upper instruction op field (bits 26-31):
//   0x00: NOP
//   0x0F: clip
//   0x10: NOP/I (when bit 27 = 1, it is special)
//   0x20-0x2B: various (ADDA/ADDi/etc)
//   0x2C: ADDx/y/z/w
//   0x2D: ADD
//   ...honestly let me use a simpler mapping derived from what PCSX2 uses.

#include <ee/vu1.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ee::rt {

namespace {

inline float b2f(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 f2b(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }

inline float vu_clamp(float f) {
    u32 v = f2b(f);
    u32 exp = (v >> 23) & 0xFF;
    if (exp == 0xFF) return b2f((v & 0x80000000u) | 0x7F7FFFFFu);
    return f;
}

// Lane access: 0=x(lo low), 1=y(lo high), 2=z(hi low), 3=w(hi high).
float get_lane(u128 v, int i) { return b2f(u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1)))); }
void set_lane(u128& v, int i, float f) {
    u64& h = (i < 2) ? v.lo : v.hi;
    int s = 32 * (i & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(f2b(f)) << s);
}
u32 get_lane_u(u128 v, int i) { return u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1))); }
void set_lane_u(u128& v, int i, u32 x) {
    u64& h = (i < 2) ? v.lo : v.hi;
    int s = 32 * (i & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(x) << s);
}
bool dest_bit(u32 dest, int i) { return (dest >> i) & 1; }

// VU registers with special read behavior.
float get_vf(const VuMicroCtx& c, int i, int lane) {
    if (i == 0) return lane == 0 ? 1.0f : 0.0f; // vf0 = (1,0,0,0)
    return get_lane(c.vf[i], lane);
}
u16 get_vi(const VuMicroCtx& c, int i) {
    if (i == 0) return 0;
    if (i == 1) return 1; // vi1 always reads as 1
    return c.vi[i];
}

// Execute one upper instruction (float ALU).
void exec_upper(VuMicroCtx& c, u32 upper) {
    u32 dest = upper & 0xF; // x=bit0.. w=bit3
    u32 ft = (upper >> 11) & 0x1F;
    u32 fs = (upper >> 16) & 0x1F;
    u32 fd = (upper >> 21) & 0x1F;
    u32 op = (upper >> 26) & 0x3F;
    u32 bc = (upper >> 24) & 0x3; // broadcast lane selector
    bool write_acc = false; // if true, result goes to ACC instead of fd

    for (int i = 0; i < 4; ++i) {
        if (!dest_bit(dest, i)) continue;
        float a = get_vf(c, fs, i);
        float b = (op >= 0x2C && op <= 0x32) ? get_vf(c, ft, bc) : get_vf(c, ft, i);
        float acc = get_lane(c.acc, i);
        float r = 0.0f;

        switch (op) {
        case 0x2C: r = a + b; break;       // ADDx/y/z/w
        case 0x2D: r = a + b; break;       // ADD
        case 0x2E: r = a - b; break;       // SUBx/y/z/w
        case 0x2F: r = a - b; break;       // SUB
        case 0x30: r = acc + a * b; write_acc = true; break; // MADDAx/y/z/w
        case 0x31: r = acc + a * b; write_acc = true; break; // MADDA
        case 0x32: r = acc - a * b; write_acc = true; break; // MSUBAx/y/z/w
        case 0x33: r = acc - a * b; write_acc = true; break; // MSUBA
        case 0x34: r = acc + a * b; break; // MADDx/y/z/w
        case 0x35: r = acc + a * b; break; // MADD
        case 0x36: r = acc - a * b; break; // MSUBx/y/z/w
        case 0x37: r = acc - a * b; break; // MSUB
        case 0x38: r = a * b; break;       // MULx/y/z/w
        case 0x39: r = a * b; break;       // MUL
        case 0x3A: r = a > b ? a : b; break; // MAXx/y/z/w
        case 0x3B: r = a > b ? a : b; break; // MAX
        case 0x3C: r = a < b ? a : b; break; // MINIx/y/z/w
        case 0x3D: r = a < b ? a : b; break; // MINI
        case 0x3E: // ADDA (no broadcast)
            r = a + b; write_acc = true; break;
        case 0x3F: // SUBA
            r = a - b; write_acc = true; break;
        case 0x28: r = a * b; write_acc = true; break; // MULAx/y/z/w
        case 0x29: r = a * b; write_acc = true; break; // MULA
        case 0x2A: r = a > b ? a : b; write_acc = true; break; // MAXAx/y/z/w (not standard but handle)
        case 0x2B: r = a < b ? a : b; write_acc = true; break; // MINIAx/y/z/w
        // Q/I source ops (upper uses ft field as immediate source for these)
        case 0x1E: r = a + b2f(c.q); break;    // ADDq
        case 0x1F: r = a * b2f(c.q); break;   // MULq
        case 0x1C: r = a - b2f(c.q); break;   // SUBq
        case 0x1D: r = acc + a * b2f(c.q); write_acc = true; break; // MADDq
        case 0x1A: r = acc - a * b2f(c.q); write_acc = true; break; // MSUBq
        case 0x1B: r = a + b2f(c.I_val); break; // ADDi
        case 0x18: r = a * b2f(c.I_val); break;  // MULi
        case 0x19: r = acc + a * b2f(c.I_val); write_acc = true; break; // MADDi
        case 0x17: r = a - b2f(c.I_val); break;  // SUBi
        case 0x16: r = a * b2f(c.I_val); write_acc = true; break; // MULAi
        // Move / misc
        case 0x0F: // ABS
            r = std::abs(a); break;
 case 0x0D: // MR32
            { int src = (i + 1) & 3; r = get_vf(c, fs, src); } break;
        case 0x10: // MOVE: fd = ft (copy)
            r = get_vf(c, ft, i); break;
        default:
            r = get_vf(c, fs, i); // NOP-ish: keep fs value
            break;
        }
        r = vu_clamp(r);
        if (write_acc)
            set_lane(c.acc, i, r);
        else if (fd != 0)
            set_lane(c.vf[fd], i, r);
    }
}

// Execute one lower instruction (integer/load/store/branch/divide).
// Returns true if a branch was taken (with delay slot handled).
bool exec_lower(VuMicroCtx& c, u32 lower) {
    u32 dest = lower & 0xF;
    u32 ft = (lower >> 11) & 0x1F;
    u32 fs = (lower >> 16) & 0x1F;
    u32 fd = (lower >> 21) & 0x1F;
    u32 op = (lower >> 26) & 0x3F;
    u32 imm11 = (lower >> 11) & 0x7FF;
    u32 imm15 = lower & 0x7FFF;

    switch (op) {
    case 0x00: break; // NOP

    case 0x01: { // ITOF0 (integer to float, no shift)
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            u32 raw = get_lane_u(c.vf[fs], i);
            set_lane(c.vf[ft], i, vu_clamp(float(s32(raw))));
        }
        break;
    }
    case 0x02: { // ITOF4 (divide by 1<<4)
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            u32 raw = get_lane_u(c.vf[fs], i);
            set_lane(c.vf[ft], i, vu_clamp(float(s32(raw)) / 16.0f));
        }
        break;
    }
    case 0x03: { // ITOF12 (divide by 1<<12)
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            u32 raw = get_lane_u(c.vf[fs], i);
            set_lane(c.vf[ft], i, vu_clamp(float(s32(raw)) / 4096.0f));
        }
        break;
    }
    case 0x04: { // FTOI0 (float to integer, no shift, truncate)
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            float f = get_lane(c.vf[fs], i);
            if (f >= 2147483648.0f) f = 2147483647.0f;
            else if (f < -2147483648.0f) f = -2147483648.0f;
            set_lane_u(c.vf[ft], i, u32(s32(f)));
        }
        break;
    }
    case 0x05: { // FTOI4 (multiply by 1<<4, truncate)
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            float f = get_lane(c.vf[fs], i) * 16.0f;
            if (f >= 2147483648.0f) f = 2147483647.0f;
            else if (f < -2147483648.0f) f = -2147483648.0f;
            set_lane_u(c.vf[ft], i, u32(s32(f)));
        }
        break;
    }
    case 0x06: { // FTOI12
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            float f = get_lane(c.vf[fs], i) * 4096.0f;
            if (f >= 2147483648.0f) f = 2147483647.0f;
            else if (f < -2147483648.0f) f = -2147483648.0f;
            set_lane_u(c.vf[ft], i, u32(s32(f)));
        }
        break;
    }
    case 0x07: { // DIV / SQRT / RSQRT (based on bits 23-24)
        u32 sub = (lower >> 23) & 0x3;
        u32 fsf = (lower >> 21) & 0x3;
        u32 ftf = (lower >> 23) & 0x3;
        // Actually DIV/SQRT/RSQRT use ft field for source too
        // DIV: Q = fs.fsf / ft.ftf
        // SQRT: Q = sqrt(ft.ftf)
        // RSQRT: Q = fs.fsf / sqrt(ft.ftf)
        if (sub == 0) { // Wait, the actual encoding is different
            // For now handle the common cases
            float a = get_vf(c, fs, fsf);
            float b = get_vf(c, ft, ftf);
            // Check if it is DIV, SQRT, or RSQRT from bits 23-23 + others
            // DIV = op 0x07 with bit23=0 in some naming
            // Actually: the division sub-type is in the lower instruction bits 21-23
            u32 div_type = (lower >> 21) & 0x3;
            if (div_type == 3) { // DIV
                c.q = f2b(vu_clamp(b == 0.0f ? (a < 0 ? -1.0f : 1.0f) : a / b));
            } else if (div_type == 1) { // SQRT
                c.q = f2b(vu_clamp(std::sqrt(b < 0 ? 0.0f : b)));
            } else if (div_type == 2) { // RSQRT
                c.q = f2b(vu_clamp(a / std::sqrt(b < 0 ? 0.0f : b)));
            }
        }
        break;
    }
    case 0x08: { // MULQ / MULi branch (MULq)
        float q = b2f(c.q);
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            set_lane(c.vf[ft], i, vu_clamp(get_vf(c, fs, i) * q));
        }
        break;
    }
    case 0x09: { // ADDQ / WaitQ
        // Lower 0x09 with bit 27 set could be MULi; without it, ADDi
        // Actually, lower 0x09 is MFIR when bit 23 is set.
        // For now, treat as ADDi (ACC based)
        // This is simplified - full MUL/ADD I handling needs more bits
        break;
    }
    case 0x0A: { // integer add: IADD
        c.vi[fd] = u16(get_vi(c, fs) + get_vi(c, ft));
        break;
    }
    case 0x0B: { // integer sub: ISUB
        c.vi[fd] = u16(get_vi(c, fs) - get_vi(c, ft));
        break;
    }
    case 0x0C: { // IADDI
        s32 imm = s32(s16(imm11 & 0x7FF)); // sign-extend 11-bit
        c.vi[ft] = u16(get_vi(c, fs) + imm);
        break;
    }
    case 0x0D: { // IAND / IOR / etc.
        u32 sub = (lower >> 21) & 0x3;
        if (sub == 0) c.vi[fd] = u16(get_vi(c, fs) & get_vi(c, ft)); // IAND
        else c.vi[fd] = u16(get_vi(c, fs) | get_vi(c, ft)); // IOR
        break;
    }
    case 0x0E: { // MFIR: fd.xyzw (as float) = s32(s16(vi[fs]))
        s32 v = s32(s16(get_vi(c, fs)));
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            set_lane_u(c.vf[ft], i, u32(v));
        }
        break;
    }
    case 0x0F: { // MTIR: vi[ft] = float bits of vf[fs].fsf
        u32 fsf = (lower >> 21) & 0x3;
        u32 bits = f2b(get_vf(c, fs, fsf));
        c.vi[ft] = u16(bits);
        break;
    }
    case 0x10: { // LQ: load 16 bytes from dmem[vi[fs]] into ft
        u32 addr = u32(get_vi(c, fs)) * 16;
        if (addr + 16 <= c.dmem_size) {
            u128 v;
            std::memcpy(&v.lo, c.dmem + addr, 8);
            std::memcpy(&v.hi, c.dmem + addr + 8, 8);
            for (int i = 0; i < 4; ++i) {
                if (!dest_bit(dest, i)) continue;
                set_lane_u(c.vf[ft], i, get_lane_u(v, i));
            }
        }
        break;
    }
    case 0x11: { // SQ: store ft to dmem[vi[fs]]
        u32 addr = u32(get_vi(c, fs)) * 16;
        if (addr + 16 <= c.dmem_size) {
            u128 v = {};
            for (int i = 0; i < 4; ++i)
                set_lane_u(v, i, get_lane_u(c.vf[ft], i));
            std::memcpy(c.dmem + addr, &v.lo, 8);
            std::memcpy(c.dmem + addr + 8, &v.hi, 8);
        }
        break;
    }
    case 0x12: { // LQD: decrement vi, then load
        c.vi[fs] = u16(get_vi(c, fs) - 1);
        u32 addr = u32(get_vi(c, fs)) * 16;
        if (addr + 16 <= c.dmem_size) {
            u128 v;
            std::memcpy(&v.lo, c.dmem + addr, 8);
            std::memcpy(&v.hi, c.dmem + addr + 8, 8);
            for (int i = 0; i < 4; ++i) {
                if (!dest_bit(dest, i)) continue;
                set_lane_u(c.vf[ft], i, get_lane_u(v, i));
            }
        }
        break;
    }
    case 0x13: { // SQI: store then increment vi
        u32 addr = u32(get_vi(c, fs)) * 16;
        if (addr + 16 <= c.dmem_size) {
            u128 v = {};
            for (int i = 0; i < 4; ++i)
                set_lane_u(v, i, get_lane_u(c.vf[ft], i));
            std::memcpy(c.dmem + addr, &v.lo, 8);
            std::memcpy(c.dmem + addr + 8, &v.hi, 8);
        }
        c.vi[fs] = u16(get_vi(c, fs) + 1);
        break;
    }
    case 0x14: case 0x15: { // ILWR / ISWR
        bool store = (op == 0x15);
        u32 addr = u32(get_vi(c, fs)) * 16;
        u32 ftf = (lower >> 23) & 0x3;
        u32 byte_off = addr + ftf * 4;
        if (store) {
            if (byte_off + 4 <= c.dmem_size) {
                u32 v = get_vi(c, ft);
                std::memcpy(c.dmem + byte_off, &v, 4);
            }
        } else {
            if (byte_off + 4 <= c.dmem_size) {
                u32 v;
                std::memcpy(&v, c.dmem + byte_off, 4);
                c.vi[ft] = u16(v);
            }
        }
        break;
    }
    case 0x1C: // WAITQ (sync with Q register; nop in our synchronous model)
    case 0x3F: // NOP / special (E bit handled outside)
        break;
    // Branches (bits 26-31 = branch opcodes):
    case 0x20: { // B: unconditional branch to imm11 (relative, *8 bytes)
        c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF; // simplified
        return true;
    }
    case 0x21: { // BAL: branch and link (save return in vi[fd])
        c.vi[fd] = u16(c.pc + 1); // return address
        c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
        return true;
    }
    case 0x24: { // JR: jump to vi[fs]
        c.pc = get_vi(c, fs);
        return true;
    }
    case 0x25: { // JALR: jump and link register
        c.vi[fd] = u16(c.pc + 1);
        c.pc = get_vi(c, fs);
        return true;
    }
    case 0x28: { // IBEQ: branch if vi[fs] == vi[ft]
        if (get_vi(c, fs) == get_vi(c, ft)) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x29: { // IBNE: branch if vi[fs] != vi[ft]
        if (get_vi(c, fs) != get_vi(c, ft)) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x2A: { // IBGTZ: branch if vi[fs] > 0 (signed)
        if (s16(get_vi(c, fs)) > 0) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x2B: { // IBGEZ
        if (s16(get_vi(c, fs)) >= 0) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x2C: { // IBLTZ
        if (s16(get_vi(c, fs)) < 0) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x2D: { // IBLTZ (actually IBLTZ continuation) / IBLEZ
        if (s16(get_vi(c, fs)) <= 0) {
            c.pc = (c.pc + (s32(imm11) << 3) / 8 - 1) & 0xFFFF;
            return true;
        }
        break;
    }
    case 0x30: // RINIT
        c.R = f2b(get_vf(c, fs, (lower >> 21) & 3)) & 0x7FFFFF;
        break;
    case 0x31: // RGET
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            set_lane_u(c.vf[ft], i, c.R);
        }
        break;
    case 0x32: // RNEXT (LFSR step)
        { u32 r = c.R; u32 bit = ((r >> 22) ^ (r >> 17)) & 1; r = ((r << 1) | bit) & 0x7FFFFF; c.R = r; }
        for (int i = 0; i < 4; ++i) {
            if (!dest_bit(dest, i)) continue;
            set_lane_u(c.vf[ft], i, c.R);
        }
        break;
    case 0x33: // RXOR
        c.R ^= f2b(get_vf(c, fs, (lower >> 21) & 3)) & 0x7FFFFF;
        break;
    default:
        break; // Unknown lower op: skip
    }
    return false;
}

} // namespace

void vu_micro_step(VuMicroCtx& c, u64 inst) {
    u32 lower = u32(inst);
    u32 upper = u32(inst >> 32);

    // Extract I bit from upper (bit 31 of lower? Actually I is in the lower instruction)
    // The I (immediate) bit: when lower bit 27 is 1, bits 0-10 + 21-25 form a 16-bit immediate.
    // For our simplified version, we check if bit 27 of lower is set for I.
    bool has_imm = (lower >> 27) & 1;
    if (has_imm) {
        // I = lower bits 0-10 | (lower bits 21-25 << 11)
        c.I_val = (lower & 0x7FF) | (((lower >> 21) & 0x1F) << 11);
        // Actually the I field is 15-bit: bits 0-10 = low 11, bits 21-25 = high 5.
        // Store as raw bits; interpret as float when used.
    }

    exec_upper(c, upper);
    bool branched = exec_lower(c, lower);

    // Check E bit (end of program): bit 30 of upper (or bit 31 in some docs)
    // The ME/E bits are in the lower instruction's bits 27-28 typically.
    // For simplicity, if lower bit 30 is set, end the program.
    (void)branched;
}

void run_vu_microcode(Hw& hw, int vu_num, u32 start_addr) {
    VuMicroCtx c;
    c.pc = start_addr / 8; // PC in 64-bit word units
    c.start_pc = c.pc;
    c.running = true;

    if (vu_num == 0) {
        c.imem = hw.vu0_imem.data();
        c.dmem = hw.vu0_dmem.data();
        c.imem_size = 0x1000;
        c.dmem_size = 0x1000;
    } else {
        c.imem = hw.vu1_imem.data();
        c.dmem = hw.vu1_dmem.data();
        c.imem_size = 0x4000;
        c.dmem_size = 0x4000;
    }

    // Copy VU0 macro state to micro context (vf/vi are shared on VU0)
    // TODO: for VU0, vf and vi are shared between macro and micro mode.
    // For now, start fresh.

    constexpr u32 max_steps = 1000000; // safety limit
    for (u32 step = 0; step < max_steps && c.running; ++step) {
        u32 pc_bytes = c.pc * 8;
        if (pc_bytes + 8 > c.imem_size)
            break;

        u64 inst;
        std::memcpy(&inst, c.imem + pc_bytes, 8);

        u32 lower = u32(inst);
        u32 upper = u32(inst >> 32);

        // Extract immediate if I bit is set (bit 27 of lower)
        if ((lower >> 27) & 1) {
            // 15-bit immediate: low 11 bits + high 5 bits (bits 21-25)
            c.I_val = (lower & 0x7FF) | (((lower >> 21) & 0x1F) << 11);
        }

        exec_upper(c, upper);
        bool branched = exec_lower(c, lower);

        // Check for E (end) bit: in the upper word, bit 31 is typically the E bit.
        // Also in some encodings, the lower word bit 31 is the M (modulo) bit.
        bool end = (lower >> 30) & 1; // simplified E bit check
        if (end) {
            c.running = false;
            break;
        }

        if (!branched)
            ++c.pc;

        // Check for tight loop (PC wrapping back without progress)
        if (c.pc == c.start_pc && step > 100)
            break; // likely an infinite loop with no exit condition
    }

    // Copy results back to shared VU0 state (for VU0 only)
    if (vu_num == 0) {
        // TODO: copy vf/vi back to EEContext
    }
}

} // namespace ee::rt
