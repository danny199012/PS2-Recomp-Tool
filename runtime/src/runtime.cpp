// SPDX-License-Identifier: GPL-3.0-only
#include <ee/runtime.hpp>

#include <cstdint>
#include <cstdio>
#include <set>

namespace ee::rt {

Function Runtime::find(u32 addr) const {
    auto it = functions.find(addr);
    return it != functions.end() ? it->second : nullptr;
}

void Runtime::call(EEContext& ctx, u32 addr) {
    if (Function fn = find(addr)) {
        fn(ctx);
        return;
    }
    static std::set<u32> reported;
    if (reported.insert(addr).second)
        std::fprintf(stderr, "[runtime] missing function at 0x%08X (treated as nop)\n", addr);
}

bool Runtime::load_elf(const elf::Image& image, std::string* error) {
    for (const elf::Segment& seg : image.segments()) {
        if (seg.type != elf::PT_LOAD || seg.memsz == 0)
            continue;
        const std::vector<u8> bytes = image.read_bytes(seg.vaddr, seg.filesz);
        if (bytes.size() != seg.filesz) {
            if (error)
                *error = "failed to read segment from image";
            return false;
        }
        // TODO(perf): copy contiguous ranges instead of byte-by-byte.
        for (u32 i = 0; i < seg.filesz; ++i)
            *mem.translate(seg.vaddr + i) = bytes[i];
        for (u32 i = seg.filesz; i < seg.memsz; ++i)
            *mem.translate(seg.vaddr + i) = 0; // .bss
    }
    return true;
}

// --- control / calls -----------------------------------------------------------

void call(EEContext& c, u32 addr) { c.rt->call(c, addr); }

void syscall(EEContext& c, u32 code) {
    (void)c;
    static std::set<u32> reported;
    if (reported.insert(code).second)
        std::fprintf(stderr, "[runtime] syscall 0x%05X unhandled (treated as nop)\n", code);
}

void unimplemented(EEContext& c, u32 raw, u32 pc) {
    (void)c;
    static std::set<u32> reported;
    if (reported.insert(raw).second)
        std::fprintf(stderr, "[runtime] unimplemented instruction 0x%08X at pc=0x%08X (nop)\n", raw, pc);
}

void stub_call(EEContext& c, const char* name) {
    auto it = c.rt->stubs.find(name);
    if (it != c.rt->stubs.end()) {
        it->second(c);
        return;
    }
    static std::set<std::string> reported;
    if (reported.insert(name).second)
        std::fprintf(stderr, "[runtime] missing stub: %s (nop)\n", name);
}

// --- mult/div --------------------------------------------------------------------
// EE pipeline-0 semantics: LO/HI get sign-extended 32-bit halves of the product.
// MADD accumulates the 64-bit product into the 128-bit {HI, LO} pair.

void op_mult(EEContext& c, int rs, int rt, int rd) {
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    c.lo.lo = u64(s64(s32(u32(p))));
    c.hi.lo = u64(s64(s32(u32(u64(p) >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_multu(EEContext& c, int rs, int rt, int rd) {
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    c.lo.lo = u64(s64(s32(u32(p))));
    c.hi.lo = u64(s64(s32(u32(p >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_div(EEContext& c, int rs, int rt, int rd) {
    const s32 a = s32(gpr32(c, rs));
    const s32 b = s32(gpr32(c, rt));
    s32 q, r;
    if (b == 0) { // MIPS UNPREDICTABLE; common emulator behavior
        q = a < 0 ? 1 : -1;
        r = a;
    } else if (a == INT32_MIN && b == -1) { // would trap on x86
        q = INT32_MIN;
        r = 0;
    } else {
        q = a / b;
        r = a % b;
    }
    c.lo.lo = u64(s64(q));
    c.hi.lo = u64(s64(r));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_divu(EEContext& c, int rs, int rt, int rd) {
    const u32 a = gpr32(c, rs);
    const u32 b = gpr32(c, rt);
    u32 q, r;
    if (b == 0) {
        q = 0xFFFFFFFF;
        r = a;
    } else {
        q = a / b;
        r = a % b;
    }
    c.lo.lo = u64(s64(s32(q)));
    c.hi.lo = u64(s64(s32(r)));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_madd(EEContext& c, int rs, int rt, int rd) {
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    const u64 old_lo = c.lo.lo;
    c.lo.lo += u64(p);
    const u64 carry = c.lo.lo < old_lo ? 1 : 0;
    c.hi.lo += u64(p >> 63) + carry;
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_maddu(EEContext& c, int rs, int rt, int rd) {
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    const u64 old_lo = c.lo.lo;
    c.lo.lo += p;
    const u64 carry = c.lo.lo < old_lo ? 1 : 0;
    c.hi.lo += carry;
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

// Pipeline 1 (MMI) variants write LO1/HI1.
void op_mult1(EEContext& c, int rs, int rt, int rd) {
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    c.lo1.lo = u64(s64(s32(u32(p))));
    c.hi1.lo = u64(s64(s32(u32(u64(p) >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

void op_multu1(EEContext& c, int rs, int rt, int rd) {
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    c.lo1.lo = u64(s64(s32(u32(p))));
    c.hi1.lo = u64(s64(s32(u32(p >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

void op_div1(EEContext& c, int rs, int rt, int rd) {
    const s32 a = s32(gpr32(c, rs));
    const s32 b = s32(gpr32(c, rt));
    s32 q, r;
    if (b == 0) {
        q = a < 0 ? 1 : -1;
        r = a;
    } else if (a == INT32_MIN && b == -1) {
        q = INT32_MIN;
        r = 0;
    } else {
        q = a / b;
        r = a % b;
    }
    c.lo1.lo = u64(s64(q));
    c.hi1.lo = u64(s64(r));
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

void op_divu1(EEContext& c, int rs, int rt, int rd) {
    const u32 a = gpr32(c, rs);
    const u32 b = gpr32(c, rt);
    u32 q, r;
    if (b == 0) {
        q = 0xFFFFFFFF;
        r = a;
    } else {
        q = a / b;
        r = a % b;
    }
    c.lo1.lo = u64(s64(s32(q)));
    c.hi1.lo = u64(s64(s32(r)));
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

void op_madd1(EEContext& c, int rs, int rt, int rd) {
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    const u64 old_lo = c.lo1.lo;
    c.lo1.lo += u64(p);
    const u64 carry = c.lo1.lo < old_lo ? 1 : 0;
    c.hi1.lo += u64(p >> 63) + carry;
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

void op_maddu1(EEContext& c, int rs, int rt, int rd) {
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    const u64 old_lo = c.lo1.lo;
    c.lo1.lo += p;
    const u64 carry = c.lo1.lo < old_lo ? 1 : 0;
    c.hi1.lo += carry;
    if (rd != 0)
        set64(c, rd, c.lo1.lo);
}

// --- unaligned load/store (little-endian semantics) ----------------------------

void op_lwl(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 3;
    const u32 mem = ld32(c, addr & ~3u);
    const int shift = 8 * (3 - int(b));
    const u32 keep = shift == 0 ? 0 : ((1u << shift) - 1);
    set32(c, rt, (mem << shift) | (gpr32(c, rt) & keep));
}

void op_lwr(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 3;
    const u32 mem = ld32(c, addr & ~3u);
    if (b == 0) {
        set32(c, rt, mem);
        return;
    }
    const u32 keep = 0xFFFFFFFFu << (8 * (4 - int(b)));
    set32(c, rt, (mem >> (8 * b)) | (gpr32(c, rt) & keep));
}

void op_swl(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 3;
    const u32 old = ld32(c, addr & ~3u);
    if (b == 3) {
        st32(c, addr & ~3u, gpr32(c, rt));
        return;
    }
    const u32 keep = 0xFFFFFFFFu << (8 * (int(b) + 1));
    st32(c, addr & ~3u, (gpr32(c, rt) >> (8 * (3 - int(b)))) | (old & keep));
}

void op_swr(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 3;
    const u32 old = ld32(c, addr & ~3u);
    if (b == 0) {
        st32(c, addr & ~3u, gpr32(c, rt));
        return;
    }
    const u32 keep = 0xFFFFFFFFu >> (8 * (4 - int(b)));
    st32(c, addr & ~3u, (gpr32(c, rt) << (8 * b)) | (old & keep));
}

void op_ldl(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 7;
    const u64 mem = ld64(c, addr & ~7u);
    const int shift = 8 * (7 - int(b));
    const u64 keep = shift == 0 ? 0 : ((1ull << shift) - 1);
    set64(c, rt, (mem << shift) | (gpr(c, rt) & keep));
}

void op_ldr(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 7;
    const u64 mem = ld64(c, addr & ~7u);
    if (b == 0) {
        set64(c, rt, mem);
        return;
    }
    const u64 keep = ~0ull << (8 * (8 - int(b)));
    set64(c, rt, (mem >> (8 * b)) | (gpr(c, rt) & keep));
}

void op_sdl(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 7;
    const u64 old = ld64(c, addr & ~7u);
    if (b == 7) {
        st64(c, addr & ~7u, gpr(c, rt));
        return;
    }
    const u64 keep = ~0ull << (8 * (int(b) + 1));
    st64(c, addr & ~7u, (gpr(c, rt) >> (8 * (7 - int(b)))) | (old & keep));
}

void op_sdr(EEContext& c, u32 addr, int rt) {
    const u32 b = addr & 7;
    const u64 old = ld64(c, addr & ~7u);
    if (b == 0) {
        st64(c, addr & ~7u, gpr(c, rt));
        return;
    }
    const u64 keep = ~0ull >> (8 * (8 - int(b)));
    st64(c, addr & ~7u, (gpr(c, rt) << (8 * b)) | (old & keep));
}

// --- MMI helpers -----------------------------------------------------------------
// 128-bit packed operations on GPRs. Lane views via a union; all wrapping ops use
// unsigned arithmetic (well-defined), saturating ops clamp per the EE manual.
// TODO(M4): differential-test these against PCSX2.

namespace {

union V {
    u8 b[16];
    s8 sb[16];
    u16 h[8];
    s16 sh[8];
    u32 w[4];
    s32 sw[4];
    u64 q[2];
};

V load_v(u128 x) {
    V v;
    v.q[0] = x.lo;
    v.q[1] = x.hi;
    return v;
}
u128 store_v(V v) {
    u128 x;
    x.lo = v.q[0];
    x.hi = v.q[1];
    return x;
}

s32 sat_s32(s64 v) { return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : s32(v)); }
s16 sat_s16(s32 v) { return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : s16(v)); }
s8 sat_s8(s32 v) { return v > INT8_MAX ? INT8_MAX : (v < INT8_MIN ? INT8_MIN : s8(v)); }
u32 sat_u32(u64 v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(v); }
u16 sat_u16(u32 v) { return v > 0xFFFF ? 0xFFFF : u16(v); }
u8 sat_u8(u32 v) { return v > 0xFF ? 0xFF : u8(v); }
s64 clamp0_s(s64 v) { return v < 0 ? 0 : v; }

} // namespace

#define EE_MMI_LANES(name, lane_t, field, expr)                                     \
    void name(EEContext& c, int rd, int rs, int rt) {                               \
        const V a = load_v(c.r[rs]);                                                \
        const V b = load_v(c.r[rt]);                                                \
        V r;                                                                        \
        for (int i = 0; i < int(sizeof(r.field) / sizeof(r.field[0])); ++i)         \
            r.field[i] = lane_t(expr);                                              \
        if (rd != 0)                                                                \
            c.r[rd] = store_v(r);                                                   \
    }

EE_MMI_LANES(op_paddw, u32, w, a.w[i] + b.w[i])
EE_MMI_LANES(op_psubw, u32, w, a.w[i] - b.w[i])
EE_MMI_LANES(op_paddh, u16, h, a.h[i] + b.h[i])
EE_MMI_LANES(op_psubh, u16, h, a.h[i] - b.h[i])
EE_MMI_LANES(op_paddb, u8, b, a.b[i] + b.b[i])
EE_MMI_LANES(op_psubb, u8, b, a.b[i] - b.b[i])
EE_MMI_LANES(op_paddsw, s32, sw, sat_s32(s64(a.sw[i]) + s64(b.sw[i])))
EE_MMI_LANES(op_psubsw, s32, sw, sat_s32(s64(a.sw[i]) - s64(b.sw[i])))
EE_MMI_LANES(op_paddsh, s16, sh, sat_s16(s32(a.sh[i]) + s32(b.sh[i])))
EE_MMI_LANES(op_psubsh, s16, sh, sat_s16(s32(a.sh[i]) - s32(b.sh[i])))
EE_MMI_LANES(op_paddsb, s8, sb, sat_s8(s32(a.sb[i]) + s32(b.sb[i])))
EE_MMI_LANES(op_psubsb, s8, sb, sat_s8(s32(a.sb[i]) - s32(b.sb[i])))
EE_MMI_LANES(op_padduw, u32, w, sat_u32(u64(a.w[i]) + u64(b.w[i])))
EE_MMI_LANES(op_psubuw, u32, w, sat_u32(u64(clamp0_s(s64(a.w[i]) - s64(b.w[i])))))
EE_MMI_LANES(op_padduh, u16, h, sat_u16(u32(a.h[i]) + u32(b.h[i])))
EE_MMI_LANES(op_psubuh, u16, h, sat_u16(u32(clamp0_s(s32(a.h[i]) - s32(b.h[i])))))
EE_MMI_LANES(op_paddub, u8, b, sat_u8(u32(a.b[i]) + u32(b.b[i])))
EE_MMI_LANES(op_psubub, u8, b, sat_u8(u32(clamp0_s(s32(a.b[i]) - s32(b.b[i])))))
EE_MMI_LANES(op_pcgtw, u32, w, a.sw[i] > b.sw[i] ? 0xFFFFFFFFu : 0)
EE_MMI_LANES(op_pcgth, u16, h, a.sh[i] > b.sh[i] ? 0xFFFF : 0)
EE_MMI_LANES(op_pcgtb, u8, b, a.sb[i] > b.sb[i] ? 0xFF : 0)
EE_MMI_LANES(op_pceqw, u32, w, a.w[i] == b.w[i] ? 0xFFFFFFFFu : 0)
EE_MMI_LANES(op_pceqh, u16, h, a.h[i] == b.h[i] ? 0xFFFF : 0)
EE_MMI_LANES(op_pceqb, u8, b, a.b[i] == b.b[i] ? 0xFF : 0)
EE_MMI_LANES(op_pmaxw, s32, sw, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
EE_MMI_LANES(op_pmaxh, s16, sh, a.sh[i] > b.sh[i] ? a.sh[i] : b.sh[i])
EE_MMI_LANES(op_pminw, s32, sw, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
EE_MMI_LANES(op_pminh, s16, sh, a.sh[i] < b.sh[i] ? a.sh[i] : b.sh[i])
EE_MMI_LANES(op_pand, u64, q, a.q[i] & b.q[i])
EE_MMI_LANES(op_por, u64, q, a.q[i] | b.q[i])
EE_MMI_LANES(op_pxor, u64, q, a.q[i] ^ b.q[i])
EE_MMI_LANES(op_pnor, u64, q, ~(a.q[i] | b.q[i]))

void op_pextlw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[0];
    r.w[1] = a.w[0];
    r.w[2] = b.w[1];
    r.w[3] = a.w[1];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pextuw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[2];
    r.w[1] = a.w[2];
    r.w[2] = b.w[3];
    r.w[3] = a.w[3];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pextlh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[i];
        r.h[2 * i + 1] = a.h[i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pextuh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[i + 4];
        r.h[2 * i + 1] = a.h[i + 4];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pextlb(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i) {
        r.b[2 * i] = b.b[i];
        r.b[2 * i + 1] = a.b[i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pextub(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i) {
        r.b[2 * i] = b.b[i + 8];
        r.b[2 * i + 1] = a.b[i + 8];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppacw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[0];
    r.w[1] = b.w[1];
    r.w[2] = a.w[0];
    r.w[3] = a.w[1];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppach(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[i] = b.h[i];
        r.h[i + 4] = a.h[i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppacb(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i) {
        r.b[i] = b.b[i];
        r.b[i + 8] = a.b[i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pinth(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[i];
        r.h[2 * i + 1] = a.h[i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pinteh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[i + 4];
        r.h[2 * i + 1] = a.h[i + 4];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pcpyld(EEContext& c, int rd, int rs, int rt) {
    if (rd == 0)
        return;
    c.r[rd].lo = c.r[rt].lo;
    c.r[rd].hi = c.r[rs].lo;
}

void op_pcpyud(EEContext& c, int rd, int rs, int rt) {
    if (rd == 0)
        return;
    c.r[rd].lo = c.r[rs].hi;
    c.r[rd].hi = c.r[rt].hi;
}

void op_pcpyh(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i)
        r.h[i] = b.h[i & 3];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

#define EE_MMI_SHIFT(name, lane_t, field, op)                                       \
    void name(EEContext& c, int rd, int rt, int sa) {                               \
        const V b = load_v(c.r[rt]);                                                \
        V r;                                                                        \
        for (int i = 0; i < int(sizeof(r.field) / sizeof(r.field[0])); ++i)         \
            r.field[i] = lane_t(b.field[i] op sa);                                  \
        if (rd != 0)                                                                \
            c.r[rd] = store_v(r);                                                   \
    }

EE_MMI_SHIFT(op_psllh, u16, h, <<)
EE_MMI_SHIFT(op_psrlh, u16, h, >>)
EE_MMI_SHIFT(op_psrah, s16, sh, >>)
EE_MMI_SHIFT(op_psllw, u32, w, <<)
EE_MMI_SHIFT(op_psrlw, u32, w, >>)
EE_MMI_SHIFT(op_psraw, s32, sw, >>)

// Variable shifts operate on the two 64-bit lanes (per EE manual).
void op_psllvw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = c.r[rt].lo << (c.r[rs].lo & 0x3F);
    c.r[rd].hi = c.r[rt].hi << (c.r[rs].hi & 0x3F);
}
void op_psrlvw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = c.r[rt].lo >> (c.r[rs].lo & 0x3F);
    c.r[rd].hi = c.r[rt].hi >> (c.r[rs].hi & 0x3F);
}
void op_psravw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = u64(s64(c.r[rt].lo) >> (c.r[rs].lo & 0x3F));
    c.r[rd].hi = u64(s64(c.r[rt].hi) >> (c.r[rs].hi & 0x3F));
}

void op_plzcw(EEContext& c, int rd, int rs) {
    const V a = load_v(c.r[rs]);
    V r;
    for (int i = 0; i < 4; ++i) {
        u32 w = a.w[i];
        if (w & 0x80000000)
            w = ~w;
        u32 n = 0;
        for (int bit = 31; bit >= 0 && ((w >> bit) & 1) == 0; --bit)
            ++n;
        r.w[i] = n > 0 ? n - 1 : 0; // leading sign bits - 1
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pmfhl(EEContext& c, int rd, int which) {
    if (rd == 0)
        return;
    switch (which) {
    case 0: // lw
        c.r[rd].lo = c.lo.lo;
        c.r[rd].hi = 0;
        break;
    case 1: // uw
        c.r[rd].lo = c.lo.hi;
        c.r[rd].hi = 0;
        break;
    case 2: // slw
        c.r[rd].lo = c.lo.lo;
        c.r[rd].hi = c.hi.lo;
        break;
    case 3: { // lh: pack low halfwords of LO and HI
        const V lo = load_v(c.lo);
        const V hi = load_v(c.hi);
        V r;
        for (int i = 0; i < 4; ++i) {
            r.h[i] = lo.h[i];
            r.h[i + 4] = hi.h[i];
        }
        c.r[rd] = store_v(r);
        break;
    }
    default:
        unimplemented(c, 0x70000030, 0); // pmfhl.sh and friends
        break;
    }
}

void op_pmthl(EEContext& c, int rs, int which) {
    switch (which) {
    case 0: // lw
        c.lo.lo = c.r[rs].lo;
        break;
    case 1: // uw
        c.lo.hi = c.r[rs].lo;
        break;
    case 2: // slw
        c.lo.lo = c.r[rs].lo;
        c.hi.lo = c.r[rs].hi;
        break;
    default:
        unimplemented(c, 0x70000031, 0); // pmthl.lh/sh
        break;
    }
}

} // namespace ee::rt
