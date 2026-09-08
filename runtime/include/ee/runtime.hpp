// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Runtime support for recompiled code. Generated C++ calls these helpers.

#include <ee/elf.hpp>
#include <ee/types.hpp>

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ee::rt {

// Guest memory: 32 MB RDRAM + 16 KB scratchpad.
// KSEG0/KSEG1 aliases are handled by masking; scratchpad lives at 0x70000000.
struct Memory {
    static constexpr u32 kRamSize = 32u * 1024 * 1024;
    static constexpr u32 kSprBase = 0x70000000;
    static constexpr u32 kSprSize = 16 * 1024;

    std::vector<u8> ram;
    std::array<u8, kSprSize> spr{};

    Memory() : ram(kRamSize, 0) {}

    u8* translate(u32 addr) {
        if ((addr & 0xFFFF0000) == kSprBase)
            return spr.data() + (addr & 0x3FFF);
        return ram.data() + (addr & 0x1FFFFFFF); // TODO(M5): MMIO at 0x10000000+
    }
    const u8* translate(u32 addr) const {
        if ((addr & 0xFFFF0000) == kSprBase)
            return spr.data() + (addr & 0x3FFF);
        return ram.data() + (addr & 0x1FFFFFFF);
    }
};

struct EEContext;

using Function = void (*)(EEContext&);
using StubHandler = void (*)(EEContext&);

struct Runtime {
    Memory mem;
    std::unordered_map<u32, Function> functions;
    std::unordered_map<std::string, StubHandler> stubs;

    void add(u32 addr, Function fn) { functions[addr] = fn; }
    void add_stub(const std::string& name, StubHandler fn) { stubs[name] = fn; }
    Function find(u32 addr) const;
    void call(EEContext& ctx, u32 addr);
    bool load_elf(const elf::Image& image, std::string* error = nullptr);
};

struct EEContext {
    u128 r[32]{}; // GPRs; r[0] is never written (codegen skips writes to $zero)
    u128 lo{}, hi{};     // mult/div pipeline 0
    u128 lo1{}, hi1{};   // pipeline 1 (MMI)
    u32 sa = 0;          // shift-amount register (MTSAB/MTSAH/QFSRV)
    u32 f[32]{};         // FPU registers (raw bits)
    u32 facc = 0;        // FPU accumulator (raw bits)
    u32 fcr31 = 0;       // FPU control/status; C (condition) flag at bit 23
    u32 cop0[32]{};      // COP0 registers (minimal)
    u128 vf[32]{};       // VU0 vector registers
    u16 vi[32]{};        // VU0 integer registers
    u32 vq = 0;          // VU0 Q (divide, raw float bits)
    u32 vp = 0;          // VU0 P (EFU)
    u32 vi_imm = 0;      // VU0 I (immediate, raw float bits)
    u32 vu_status = 0;
    u32 vu_mac = 0;
    u32 vu_clip = 0;
    u32 pc = 0;
    Runtime* rt = nullptr;
};

// --- register access ---------------------------------------------------------

inline u64 gpr(const EEContext& c, int i) { return c.r[i].lo; }
inline u32 gpr32(const EEContext& c, int i) { return u32(c.r[i].lo); }
inline void set32(EEContext& c, int i, u32 v) { c.r[i].lo = u64(s64(s32(v))); } // 32-bit ops sign-extend
inline void set64(EEContext& c, int i, u64 v) { c.r[i].lo = v; }
inline u128 get128(const EEContext& c, int i) { return c.r[i]; }
inline void set128(EEContext& c, int i, u128 v) { c.r[i] = v; }

// --- memory access -----------------------------------------------------------

inline u32 eff(u32 base, u16 imm) { return base + u32(s32(s16(imm))); }

inline u32 ld32(EEContext& c, u32 a) {
    u32 v;
    std::memcpy(&v, c.rt->mem.translate(a), 4);
    return v;
}
inline u64 ld64(EEContext& c, u32 a) {
    u64 v;
    std::memcpy(&v, c.rt->mem.translate(a), 8);
    return v;
}
inline u128 ld128(EEContext& c, u32 a) {
    u128 v;
    std::memcpy(&v.lo, c.rt->mem.translate(a), 8);
    std::memcpy(&v.hi, c.rt->mem.translate(a) + 8, 8);
    return v;
}
inline u32 ld16u(EEContext& c, u32 a) {
    u16 v;
    std::memcpy(&v, c.rt->mem.translate(a), 2);
    return v;
}
inline u32 ld8u(EEContext& c, u32 a) { return *c.rt->mem.translate(a); }
inline u32 ld16s(EEContext& c, u32 a) { return u32(s32(s16(ld16u(c, a)))); }
inline u32 ld8s(EEContext& c, u32 a) { return u32(s32(s8(ld8u(c, a)))); }

inline void st8(EEContext& c, u32 a, u32 v) { *c.rt->mem.translate(a) = u8(v); }
inline void st16(EEContext& c, u32 a, u32 v) {
    const u16 x = u16(v);
    std::memcpy(c.rt->mem.translate(a), &x, 2);
}
inline void st32(EEContext& c, u32 a, u32 v) { std::memcpy(c.rt->mem.translate(a), &v, 4); }
inline void st64(EEContext& c, u32 a, u64 v) { std::memcpy(c.rt->mem.translate(a), &v, 8); }
inline void st128(EEContext& c, u32 a, u128 v) {
    std::memcpy(c.rt->mem.translate(a), &v.lo, 8);
    std::memcpy(c.rt->mem.translate(a) + 8, &v.hi, 8);
}

// --- FPU access ----------------------------------------------------------------
// TODO(M4): the EE FPU is not IEEE-754 (no NaN/denormal propagation, clamping
// overflow). These helpers currently use host float semantics.

inline float fget(const EEContext& c, int i) {
    float v;
    std::memcpy(&v, &c.f[i], 4);
    return v;
}
inline void fset(EEContext& c, int i, float v) { std::memcpy(&c.f[i], &v, 4); }
inline float facc_get(const EEContext& c) {
    float v;
    std::memcpy(&v, &c.facc, 4);
    return v;
}
inline void facc_set(EEContext& c, float v) { std::memcpy(&c.facc, &v, 4); }
inline void set_fcc(EEContext& c, bool cond) {
    c.fcr31 = (c.fcr31 & ~(1u << 23)) | (u32(cond) << 23);
}
inline bool fcc(const EEContext& c) { return (c.fcr31 >> 23) & 1; }

// --- control / calls -----------------------------------------------------------

void call(EEContext& c, u32 addr);
void syscall(EEContext& c, u32 code);
void unimplemented(EEContext& c, u32 raw, u32 pc);
void stub_call(EEContext& c, const char* name);

// --- mult/div (implemented in runtime.cpp; handle EE edge cases) --------------

void op_mult(EEContext& c, int rs, int rt, int rd);
void op_multu(EEContext& c, int rs, int rt, int rd);
void op_div(EEContext& c, int rs, int rt, int rd);
void op_divu(EEContext& c, int rs, int rt, int rd);
void op_madd(EEContext& c, int rs, int rt, int rd);
void op_maddu(EEContext& c, int rs, int rt, int rd);
void op_mult1(EEContext& c, int rs, int rt, int rd);
void op_multu1(EEContext& c, int rs, int rt, int rd);
void op_div1(EEContext& c, int rs, int rt, int rd);
void op_divu1(EEContext& c, int rs, int rt, int rd);
void op_madd1(EEContext& c, int rs, int rt, int rd);
void op_maddu1(EEContext& c, int rs, int rt, int rd);

// --- unaligned load/store (implemented in runtime.cpp) ------------------------

void op_lwl(EEContext& c, u32 addr, int rt);
void op_lwr(EEContext& c, u32 addr, int rt);
void op_swl(EEContext& c, u32 addr, int rt);
void op_swr(EEContext& c, u32 addr, int rt);
void op_ldl(EEContext& c, u32 addr, int rt);
void op_ldr(EEContext& c, u32 addr, int rt);
void op_sdl(EEContext& c, u32 addr, int rt);
void op_sdr(EEContext& c, u32 addr, int rt);

// --- MMI helpers (implemented in runtime.cpp) ----------------------------------

void op_paddw(EEContext& c, int rd, int rs, int rt);
void op_psubw(EEContext& c, int rd, int rs, int rt);
void op_paddh(EEContext& c, int rd, int rs, int rt);
void op_psubh(EEContext& c, int rd, int rs, int rt);
void op_paddb(EEContext& c, int rd, int rs, int rt);
void op_psubb(EEContext& c, int rd, int rs, int rt);
void op_paddsw(EEContext& c, int rd, int rs, int rt);
void op_psubsw(EEContext& c, int rd, int rs, int rt);
void op_paddsh(EEContext& c, int rd, int rs, int rt);
void op_psubsh(EEContext& c, int rd, int rs, int rt);
void op_paddsb(EEContext& c, int rd, int rs, int rt);
void op_psubsb(EEContext& c, int rd, int rs, int rt);
void op_padduw(EEContext& c, int rd, int rs, int rt);
void op_psubuw(EEContext& c, int rd, int rs, int rt);
void op_padduh(EEContext& c, int rd, int rs, int rt);
void op_psubuh(EEContext& c, int rd, int rs, int rt);
void op_paddub(EEContext& c, int rd, int rs, int rt);
void op_psubub(EEContext& c, int rd, int rs, int rt);
void op_pcgtw(EEContext& c, int rd, int rs, int rt);
void op_pcgth(EEContext& c, int rd, int rs, int rt);
void op_pcgtb(EEContext& c, int rd, int rs, int rt);
void op_pceqw(EEContext& c, int rd, int rs, int rt);
void op_pceqh(EEContext& c, int rd, int rs, int rt);
void op_pceqb(EEContext& c, int rd, int rs, int rt);
void op_pmaxw(EEContext& c, int rd, int rs, int rt);
void op_pmaxh(EEContext& c, int rd, int rs, int rt);
void op_pminw(EEContext& c, int rd, int rs, int rt);
void op_pminh(EEContext& c, int rd, int rs, int rt);
void op_pand(EEContext& c, int rd, int rs, int rt);
void op_por(EEContext& c, int rd, int rs, int rt);
void op_pxor(EEContext& c, int rd, int rs, int rt);
void op_pnor(EEContext& c, int rd, int rs, int rt);
void op_pextlw(EEContext& c, int rd, int rs, int rt);
void op_pextlh(EEContext& c, int rd, int rs, int rt);
void op_pextlb(EEContext& c, int rd, int rs, int rt);
void op_pextuw(EEContext& c, int rd, int rs, int rt);
void op_pextuh(EEContext& c, int rd, int rs, int rt);
void op_pextub(EEContext& c, int rd, int rs, int rt);
void op_ppacw(EEContext& c, int rd, int rs, int rt);
void op_ppach(EEContext& c, int rd, int rs, int rt);
void op_ppacb(EEContext& c, int rd, int rs, int rt);
void op_pinth(EEContext& c, int rd, int rs, int rt);
void op_pinteh(EEContext& c, int rd, int rs, int rt);
void op_pcpyld(EEContext& c, int rd, int rs, int rt);
void op_pcpyud(EEContext& c, int rd, int rs, int rt);
void op_pcpyh(EEContext& c, int rd, int rt);
void op_psllh(EEContext& c, int rd, int rt, int sa);
void op_psrlh(EEContext& c, int rd, int rt, int sa);
void op_psrah(EEContext& c, int rd, int rt, int sa);
void op_psllw(EEContext& c, int rd, int rt, int sa);
void op_psrlw(EEContext& c, int rd, int rt, int sa);
void op_psraw(EEContext& c, int rd, int rt, int sa);
void op_psllvw(EEContext& c, int rd, int rt, int rs);
void op_psrlvw(EEContext& c, int rd, int rt, int rs);
void op_psravw(EEContext& c, int rd, int rt, int rs);
void op_plzcw(EEContext& c, int rd, int rs);
void op_pmfhl(EEContext& c, int rd, int which); // which: 0=lw 1=uw 2=slw 3=lh 4=sh
void op_pmthl(EEContext& c, int rs, int which);

} // namespace ee::rt
