// SPDX-License-Identifier: GPL-3.0-only
#include <ee/kernel.hpp>
#include <ee/runtime.hpp>

#include <cstdint>
#include <cstdio>
#include <set>

namespace ee::rt {

Runtime::Runtime() {
    kernel = std::make_unique<Kernel>(*this);
    mem.mmio_user = kernel.get();
    mem.mmio_read = [](u32 addr, u32 size, void* user) {
        return static_cast<Kernel*>(user)->mmio_read(addr, size);
    };
    mem.mmio_write = [](u32 addr, u64 value, u32 size, void* user) {
        static_cast<Kernel*>(user)->mmio_write(addr, value, size);
    };
}

Runtime::~Runtime() = default; // unique_ptr destroys Kernel, which joins threads

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
    u32 max_addr = 0;
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
        if ((seg.vaddr & 0x1FFFFFFF) + seg.memsz > max_addr)
            max_addr = (seg.vaddr & 0x1FFFFFFF) + seg.memsz;
    }
    if (kernel)
        kernel->note_heap_base(max_addr);
    return true;
}

// --- control / calls -----------------------------------------------------------

void call(EEContext& c, u32 addr) { c.rt->call(c, addr); }

void syscall(EEContext& c, u32 code) {
    // The 20-bit code field is sign-extended (negative = i-variant syscalls).
    const s32 scode = (s32(code) << 12) >> 12;
    c.rt->kernel->syscall(c, scode);
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
// Semantics cross-checked against PCSX2 (R5900OpcodeImpl.cpp / MMI.cpp).
// Layout: lo = {LO0, LO1}, hi = {HI0, HI1} (PCSX2-style packed 128-bit pair).
// Pipeline-0 ops use .lo, pipeline-1 (MMI *1) ops use .hi.
// MULT: LO/HI get sign-extended 32-bit halves of the 64-bit product.
// MADD: 64-bit accumulate into {HI.UL[0], LO.UL[0]}, then sign-extend halves.

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

static void div32_common(s32 a, s32 b, s32& q, s32& r) {
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
}

void op_div(EEContext& c, int rs, int rt, int rd) {
    s32 q, r;
    div32_common(s32(gpr32(c, rs)), s32(gpr32(c, rt)), q, r);
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
    const s64 acc = s64(u64(u32(c.lo.lo)) | (u64(u32(c.hi.lo)) << 32));
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    const s64 t = acc + p;
    c.lo.lo = u64(s64(s32(u32(t))));
    c.hi.lo = u64(s64(s32(u32(u64(t) >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

void op_maddu(EEContext& c, int rs, int rt, int rd) {
    const u64 acc = u64(u32(c.lo.lo)) | (u64(u32(c.hi.lo)) << 32);
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    const u64 t = acc + p;
    c.lo.lo = u64(s64(s32(u32(t))));
    c.hi.lo = u64(s64(s32(u32(t >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.lo);
}

// Pipeline 1 (MMI) variants use LO1/HI1 (the .hi halves).
void op_mult1(EEContext& c, int rs, int rt, int rd) {
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    c.lo.hi = u64(s64(s32(u32(p))));
    c.hi.hi = u64(s64(s32(u32(u64(p) >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
}

void op_multu1(EEContext& c, int rs, int rt, int rd) {
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    c.lo.hi = u64(s64(s32(u32(p))));
    c.hi.hi = u64(s64(s32(u32(p >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
}

void op_div1(EEContext& c, int rs, int rt, int rd) {
    s32 q, r;
    div32_common(s32(gpr32(c, rs)), s32(gpr32(c, rt)), q, r);
    c.lo.hi = u64(s64(q));
    c.hi.hi = u64(s64(r));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
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
    c.lo.hi = u64(s64(s32(q)));
    c.hi.hi = u64(s64(s32(r)));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
}

void op_madd1(EEContext& c, int rs, int rt, int rd) {
    const s64 acc = s64(u64(u32(c.lo.hi)) | (u64(u32(c.hi.hi)) << 32));
    const s64 p = s64(s32(gpr32(c, rs))) * s64(s32(gpr32(c, rt)));
    const s64 t = acc + p;
    c.lo.hi = u64(s64(s32(u32(t))));
    c.hi.hi = u64(s64(s32(u32(u64(t) >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
}

void op_maddu1(EEContext& c, int rs, int rt, int rd) {
    const u64 acc = u64(u32(c.lo.hi)) | (u64(u32(c.hi.hi)) << 32);
    const u64 p = u64(gpr32(c, rs)) * u64(gpr32(c, rt));
    const u64 t = acc + p;
    c.lo.hi = u64(s64(s32(u32(t))));
    c.hi.hi = u64(s64(s32(u32(t >> 32))));
    if (rd != 0)
        set64(c, rd, c.lo.hi);
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
// Semantics cross-checked against PCSX2's MMI.cpp (including the PMADDW/PHMSBH
// hardware-errata quirks). {LO,HI} pair word view: word n of LO = (n<2 ? lo : hi
// of the pair), matching PCSX2's LO.UL[0..3] = {LO0.lo, LO0.hi, LO1.lo, LO1.hi}.

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

// Word access into the packed {LO0,LO1} / {HI0,HI1} pairs.
inline u32 get_lo_w(const EEContext& c, int n) { return u32(((n & 2) ? c.lo.hi : c.lo.lo) >> (32 * (n & 1))); }
inline void set_lo_w(EEContext& c, int n, u32 v) {
    u64& h = (n & 2) ? c.lo.hi : c.lo.lo;
    const int s = 32 * (n & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(v) << s);
}
inline u32 get_hi_w(const EEContext& c, int n) { return u32(((n & 2) ? c.hi.hi : c.hi.lo) >> (32 * (n & 1))); }
inline void set_hi_w(EEContext& c, int n, u32 v) {
    u64& h = (n & 2) ? c.hi.hi : c.hi.lo;
    const int s = 32 * (n & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(v) << s);
}
inline u64 get_lo64(const EEContext& c, int dd) { return dd ? c.lo.hi : c.lo.lo; }
inline void set_lo64(EEContext& c, int dd, u64 v) {
    if (dd)
        c.lo.hi = v;
    else
        c.lo.lo = v;
}
inline u64 get_hi64(const EEContext& c, int dd) { return dd ? c.hi.hi : c.hi.lo; }
inline void set_hi64(EEContext& c, int dd, u64 v) {
    if (dd)
        c.hi.hi = v;
    else
        c.hi.lo = v;
}

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

// Interleave low/high elements (rt supplies even slots, rs odd).
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

// Pack: low half of rt's elements, then low half of rs's (even-indexed elements).
void op_ppacw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[0];
    r.w[1] = b.w[2];
    r.w[2] = a.w[0];
    r.w[3] = a.w[2];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppach(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[i] = b.h[2 * i];
        r.h[i + 4] = a.h[2 * i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppacb(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i) {
        r.b[i] = b.b[2 * i];
        r.b[i + 8] = a.b[2 * i];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PINTH: interleave rt's low halfwords with rs's HIGH halfwords (per PCSX2).
void op_pinth(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[i];
        r.h[2 * i + 1] = a.h[i + 4];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PINTEH: interleave the even halfwords of rt and rs.
void op_pinteh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[2 * i] = b.h[2 * i];
        r.h[2 * i + 1] = a.h[2 * i];
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

// PCPYH: broadcast rt.h[0] into rd.h[0..3] and rt.h[4] into rd.h[4..7].
void op_pcpyh(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        r.h[i] = b.h[0];
        r.h[i + 4] = b.h[4];
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PEXEH: swap the low halfwords of the two words in each 64-bit half.
void op_pexeh(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.h[0] = b.h[2];
    r.h[1] = b.h[1];
    r.h[2] = b.h[0];
    r.h[3] = b.h[3];
    r.h[4] = b.h[6];
    r.h[5] = b.h[5];
    r.h[6] = b.h[4];
    r.h[7] = b.h[7];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PREVH: reverse the four halfwords of each 64-bit half.
void op_prevh(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.h[0] = b.h[3];
    r.h[1] = b.h[2];
    r.h[2] = b.h[1];
    r.h[3] = b.h[0];
    r.h[4] = b.h[7];
    r.h[5] = b.h[6];
    r.h[6] = b.h[5];
    r.h[7] = b.h[4];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PEXCH: swap the middle two halfwords of each 64-bit half.
void op_pexch(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.h[0] = b.h[0];
    r.h[1] = b.h[2];
    r.h[2] = b.h[1];
    r.h[3] = b.h[3];
    r.h[4] = b.h[4];
    r.h[5] = b.h[6];
    r.h[6] = b.h[5];
    r.h[7] = b.h[7];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PEXEW: swap words 0 and 2.
void op_pexew(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[2];
    r.w[1] = b.w[1];
    r.w[2] = b.w[0];
    r.w[3] = b.w[3];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PEXCW: swap the middle two words.
void op_pexcw(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[0];
    r.w[1] = b.w[2];
    r.w[2] = b.w[1];
    r.w[3] = b.w[3];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PROT3W: rotate the low three words left by one.
void op_prot3w(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    r.w[0] = b.w[1];
    r.w[1] = b.w[2];
    r.w[2] = b.w[0];
    r.w[3] = b.w[3];
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PEXT5/PPAC5: 1-5-5-5 <-> 8-8-8-8 pixel format conversion.
void op_pext5(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i)
        r.w[i] = ((b.w[i] & 0x0000001Fu) << 3) | ((b.w[i] & 0x000003E0u) << 6) |
                 ((b.w[i] & 0x00007C00u) << 9) | ((b.w[i] & 0x00008000u) << 16);
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_ppac5(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i)
        r.w[i] = ((b.w[i] >> 3) & 0x0000001Fu) | ((b.w[i] >> 6) & 0x000003E0u) |
                 ((b.w[i] >> 9) & 0x00007C00u) | ((b.w[i] >> 16) & 0x00008000u);
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pabsw(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i) {
        const s32 w = b.sw[i];
        r.w[i] = (w == INT32_MIN) ? 0x7FFFFFFFu : (w < 0 ? u32(-s64(w)) : u32(w));
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pabsh(EEContext& c, int rd, int rt) {
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 8; ++i) {
        const s16 h = b.sh[i];
        r.h[i] = (h == INT16_MIN) ? 0x7FFF : (h < 0 ? u16(-s32(h)) : u16(h));
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PADSBH: subtract low four halfwords, add high four.
void op_padsbh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int i = 0; i < 4; ++i)
        r.h[i] = u16(a.h[i] - b.h[i]);
    for (int i = 4; i < 8; ++i)
        r.h[i] = u16(a.h[i] + b.h[i]);
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// QFSRV: funnel right shift of the 256-bit {rs, rt} by sa*8 bits, low 128 out.
void op_qfsrv(EEContext& c, int rd, int rs, int rt) {
    if (rd == 0)
        return;
    const u32 sa_amt = c.sa << 3;
    const u64 rsl = c.r[rs].lo, rsh = c.r[rs].hi;
    const u64 rtl = c.r[rt].lo, rth = c.r[rt].hi;
    u64 out_lo, out_hi;
    if (sa_amt == 0) {
        out_lo = rtl;
        out_hi = rth;
    } else if (sa_amt < 64) {
        out_lo = (rtl >> sa_amt) | (rth << (64 - sa_amt));
        out_hi = (rth >> sa_amt) | (rsl << (64 - sa_amt));
    } else if (sa_amt == 64) {
        out_lo = rth;
        out_hi = rsl;
    } else if (sa_amt < 128) {
        const u32 s = sa_amt - 64;
        out_lo = (rth >> s) | (rsl << (64 - s));
        out_hi = (rsl >> s) | (rsh << (64 - s));
    } else if (sa_amt == 128) {
        out_lo = rsl;
        out_hi = rsh;
    } else {
        out_lo = 0;
        out_hi = 0;
    }
    c.r[rd].lo = out_lo;
    c.r[rd].hi = out_hi;
}

// Shifts: halfword shifts mask sa to 4 bits (per PCSX2), word shifts use full sa.
#define EE_MMI_SHIFT(name, lane_t, field, op, mask)                                 \
    void name(EEContext& c, int rd, int rt, int sa) {                               \
        const V b = load_v(c.r[rt]);                                                \
        V r;                                                                        \
        for (int i = 0; i < int(sizeof(r.field) / sizeof(r.field[0])); ++i)         \
            r.field[i] = lane_t(b.field[i] op (sa & (mask)));                       \
        if (rd != 0)                                                                \
            c.r[rd] = store_v(r);                                                   \
    }

EE_MMI_SHIFT(op_psllh, u16, h, <<, 0xF)
EE_MMI_SHIFT(op_psrlh, u16, h, >>, 0xF)
EE_MMI_SHIFT(op_psrah, s16, sh, >>, 0xF)
EE_MMI_SHIFT(op_psllw, u32, w, <<, 0x1F)
EE_MMI_SHIFT(op_psrlw, u32, w, >>, 0x1F)
EE_MMI_SHIFT(op_psraw, s32, sw, >>, 0x1F)

// Variable shifts: 32-bit lanes (words 0 and 2), result sign-extended to 64 bits.
void op_psllvw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = u64(s64(s32(u32(c.r[rt].lo) << (u32(c.r[rs].lo) & 0x1F))));
    c.r[rd].hi = u64(s64(s32(u32(c.r[rt].hi) << (u32(c.r[rs].hi) & 0x1F))));
}
void op_psrlvw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = u64(s64(s32(u32(c.r[rt].lo) >> (u32(c.r[rs].lo) & 0x1F))));
    c.r[rd].hi = u64(s64(s32(u32(c.r[rt].hi) >> (u32(c.r[rs].hi) & 0x1F))));
}
void op_psravw(EEContext& c, int rd, int rt, int rs) {
    if (rd == 0)
        return;
    c.r[rd].lo = u64(s64(s32(c.r[rt].lo) >> (u32(c.r[rs].lo) & 0x1F)));
    c.r[rd].hi = u64(s64(s32(c.r[rt].hi) >> (u32(c.r[rs].hi) & 0x1F)));
}

// PLZCW: leading sign bits minus 1, words 0 and 1 only (per PCSX2).
void op_plzcw(EEContext& c, int rd, int rs) {
    if (rd == 0)
        return;
    const V a = load_v(c.r[rs]);
    V r = load_v(c.r[rd]); // words 2-3 preserved
    for (int i = 0; i < 2; ++i) {
        const s32 w = a.sw[i];
        const u32 x = w < 0 ? ~u32(w) : u32(w);
        u32 n = 0;
        for (int bit = 31; bit >= 0 && ((x >> bit) & 1) == 0; --bit)
            ++n;
        r.w[i] = n > 0 ? n - 1 : 0;
    }
    c.r[rd] = store_v(r);
}

// PMFHL: move from the {HI,LO} pair, per suffix variant.
void op_pmfhl(EEContext& c, int rd, int which) {
    if (rd == 0)
        return;
    V r;
    switch (which) {
    case 0: // lw: low words of LO0,HI0,LO1,HI1
        r.w[0] = get_lo_w(c, 0);
        r.w[1] = get_hi_w(c, 0);
        r.w[2] = get_lo_w(c, 2);
        r.w[3] = get_hi_w(c, 2);
        break;
    case 1: // uw: high words
        r.w[0] = get_lo_w(c, 1);
        r.w[1] = get_hi_w(c, 1);
        r.w[2] = get_lo_w(c, 3);
        r.w[3] = get_hi_w(c, 3);
        break;
    case 2: { // slw: clamp each {HI.w, LO.w} pair to s32 range
        V lo = load_v(c.lo);
        V hi = load_v(c.hi);
        for (int i = 0; i < 4; ++i) {
            const s64 v = s64(u64(lo.w[i]) | (u64(hi.w[i]) << 32));
            r.sw[i] = sat_s32(v);
        }
        break;
    }
    case 3: { // lh: pack low halfwords of LO/HI words
        V lo = load_v(c.lo);
        V hi = load_v(c.hi);
        r.h[0] = lo.h[0];
        r.h[1] = lo.h[2];
        r.h[2] = hi.h[0];
        r.h[3] = hi.h[2];
        r.h[4] = lo.h[4];
        r.h[5] = lo.h[6];
        r.h[6] = hi.h[4];
        r.h[7] = hi.h[6];
        break;
    }
    case 4: { // sh: clamp each LO/HI word to s16 range
        V lo = load_v(c.lo);
        V hi = load_v(c.hi);
        r.h[0] = u16(sat_s16(lo.sw[0]));
        r.h[1] = u16(sat_s16(lo.sw[1]));
        r.h[2] = u16(sat_s16(hi.sw[0]));
        r.h[3] = u16(sat_s16(hi.sw[1]));
        r.h[4] = u16(sat_s16(lo.sw[2]));
        r.h[5] = u16(sat_s16(lo.sw[3]));
        r.h[6] = u16(sat_s16(hi.sw[2]));
        r.h[7] = u16(sat_s16(hi.sw[3]));
        break;
    }
    default:
        return;
    }
    c.r[rd] = store_v(r);
}

// PMTHL: only the lw variant exists (others are no-ops, per PCSX2).
void op_pmthl(EEContext& c, int rs, int which) {
    if (which != 0)
        return;
    const V a = load_v(c.r[rs]);
    set_lo_w(c, 0, a.w[0]);
    set_hi_w(c, 0, a.w[1]);
    set_lo_w(c, 2, a.w[2]);
    set_hi_w(c, 2, a.w[3]);
}

// PMADDW/PMSUBW: per 64-bit half dd (source word ss = dd*2). Includes the PS2
// multiply-errata behavior (divide by 2^32-1, plus the PMADDW word-0 fudge),
// matching PCSX2's hardware-tested implementation.
void op_pmaddw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const s64 prod = s64(a.sw[ss]) * s64(b.sw[ss]);
        s64 temp2 = prod + (s64(s32(get_hi_w(c, ss))) << 32);
        if (ss == 0 && ((b.sw[0] & 0x7FFFFFFF) == 0 || (b.sw[0] & 0x7FFFFFFF) == 0x7FFFFFFF) &&
            a.sw[0] != b.sw[0])
            temp2 += 0x70000000; // hardware errata (PCSX2)
        temp2 = s32(temp2 / 4294967295ll); // errata: not exactly >> 32
        const u32 lo_sum = u32(prod) + get_lo_w(c, ss); // wrapping 32-bit add
        set_lo64(c, dd, u64(s64(s32(lo_sum))));
        set_hi64(c, dd, u64(s64(s32(temp2))));
        r.w[dd * 2] = lo_sum;
        r.w[dd * 2 + 1] = u32(temp2);
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

void op_pmsubw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    V r;
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const s64 prod = s64(a.sw[ss]) * s64(b.sw[ss]);
        s64 temp2 = (s64(s32(get_hi_w(c, ss))) << 32) - prod;
        temp2 = s32(temp2 / 4294967295ll);
        const u32 lo_diff = get_lo_w(c, ss) - u32(prod); // wrapping
        set_lo64(c, dd, u64(s64(s32(lo_diff))));
        set_hi64(c, dd, u64(s64(s32(temp2))));
        r.w[dd * 2] = lo_diff;
        r.w[dd * 2 + 1] = u32(temp2);
    }
    if (rd != 0)
        c.r[rd] = store_v(r);
}

// PMULTW/PMULTUW: products of words 0 and 2; rd gets the full 64-bit products.
void op_pmultw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    u64 rd64[2] = {};
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const s64 p = s64(a.sw[ss]) * s64(b.sw[ss]);
        set_lo64(c, dd, u64(s64(s32(u32(p)))));
        set_hi64(c, dd, u64(s64(s32(u32(u64(p) >> 32)))));
        rd64[dd] = u64(p);
    }
    if (rd != 0) {
        c.r[rd].lo = rd64[0];
        c.r[rd].hi = rd64[1];
    }
}

void op_pmultuw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    u64 rd64[2] = {};
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const u64 p = u64(a.w[ss]) * u64(b.w[ss]);
        set_lo64(c, dd, u64(s64(s32(u32(p)))));
        set_hi64(c, dd, u64(s64(s32(u32(p >> 32)))));
        rd64[dd] = p;
    }
    if (rd != 0) {
        c.r[rd].lo = rd64[0];
        c.r[rd].hi = rd64[1];
    }
}

// PMADDUW: unsigned multiply + accumulate into the 64-bit {HI.w[ss], LO.w[ss]}.
void op_pmadduw(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    u64 rd64[2] = {};
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const u64 acc = u64(get_lo_w(c, ss)) | (u64(get_hi_w(c, ss)) << 32);
        const u64 t = acc + u64(a.w[ss]) * u64(b.w[ss]);
        set_lo64(c, dd, u64(s64(s32(u32(t)))));
        set_hi64(c, dd, u64(s64(s32(u32(t >> 32)))));
        rd64[dd] = t;
    }
    if (rd != 0) {
        c.r[rd].lo = rd64[0];
        c.r[rd].hi = rd64[1];
    }
}

// PMULTH: halfword products -> word lanes of LO/HI; rd = {LO.w0, HI.w0, LO.w2, HI.w2}.
void op_pmulth(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 p[8] = {a.sh[0] * b.sh[0], a.sh[1] * b.sh[1], a.sh[2] * b.sh[2], a.sh[3] * b.sh[3],
                      a.sh[4] * b.sh[4], a.sh[5] * b.sh[5], a.sh[6] * b.sh[6], a.sh[7] * b.sh[7]};
    set_lo_w(c, 0, u32(p[0]));
    set_lo_w(c, 1, u32(p[1]));
    set_hi_w(c, 0, u32(p[2]));
    set_hi_w(c, 1, u32(p[3]));
    set_lo_w(c, 2, u32(p[4]));
    set_lo_w(c, 3, u32(p[5]));
    set_hi_w(c, 2, u32(p[6]));
    set_hi_w(c, 3, u32(p[7]));
    if (rd != 0) {
        V r;
        r.w[0] = u32(p[0]);
        r.w[1] = u32(p[2]);
        r.w[2] = u32(p[4]);
        r.w[3] = u32(p[6]);
        c.r[rd] = store_v(r);
    }
}

// PDIVW: divide words 0 and 2; quotients -> LO, remainders -> HI (64-bit halves).
void op_pdivw(EEContext& c, int rd, int rs, int rt) {
    (void)rd;
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        s32 q, r;
        div32_common(a.sw[ss], b.sw[ss], q, r);
        set_lo64(c, dd, u64(s64(q)));
        set_hi64(c, dd, u64(s64(r)));
    }
}

void op_pdivuw(EEContext& c, int rd, int rs, int rt) {
    (void)rd;
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const u32 av = a.w[ss];
        const u32 bv = b.w[ss];
        u32 q, r;
        if (bv == 0) {
            q = 0xFFFFFFFF;
            r = av;
        } else {
            q = av / bv;
            r = av % bv;
        }
        set_lo64(c, dd, u64(s64(s32(q))));
        set_hi64(c, dd, u64(s64(s32(r))));
    }
}

// PDIVBW: divide all four words by rt's low halfword; quotients -> LO words,
// remainders -> HI words (32-bit lanes, adjacent word preserved).
void op_pdivbw(EEContext& c, int rd, int rs, int rt) {
    (void)rd;
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 bv = b.sh[0];
    for (int n = 0; n < 4; ++n) {
        const s32 av = a.sw[n];
        s32 q, r;
        if (u32(av) == 0x80000000u && bv == -1) {
            q = INT32_MIN;
            r = 0;
        } else if (bv != 0) {
            q = av / bv;
            r = av % bv;
        } else {
            q = av < 0 ? 1 : -1;
            r = av;
        }
        set_lo_w(c, n, u32(q));
        set_hi_w(c, n, u32(r));
    }
}

// PMADDH/PMSUBH: halfword products accumulated into LO/HI word lanes.
// rd = {LO.w0, HI.w0, LO.w2, HI.w2} (per the EE manual; PCSX2 comments this out
// for PMADDH — we follow the manual, TODO: differential-test).
void op_pmaddh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 p[8] = {a.sh[0] * b.sh[0], a.sh[1] * b.sh[1], a.sh[2] * b.sh[2], a.sh[3] * b.sh[3],
                      a.sh[4] * b.sh[4], a.sh[5] * b.sh[5], a.sh[6] * b.sh[6], a.sh[7] * b.sh[7]};
    const u32 w0 = get_lo_w(c, 0) + u32(p[0]);
    const u32 w1 = get_hi_w(c, 0) + u32(p[2]);
    const u32 w2 = get_lo_w(c, 2) + u32(p[4]);
    const u32 w3 = get_hi_w(c, 2) + u32(p[6]);
    set_lo_w(c, 0, w0);
    set_lo_w(c, 1, get_lo_w(c, 1) + u32(p[1]));
    set_hi_w(c, 0, w1);
    set_hi_w(c, 1, get_hi_w(c, 1) + u32(p[3]));
    set_lo_w(c, 2, w2);
    set_lo_w(c, 3, get_lo_w(c, 3) + u32(p[5]));
    set_hi_w(c, 2, w3);
    set_hi_w(c, 3, get_hi_w(c, 3) + u32(p[7]));
    if (rd != 0) {
        V r;
        r.w[0] = w0;
        r.w[1] = w1;
        r.w[2] = w2;
        r.w[3] = w3;
        c.r[rd] = store_v(r);
    }
}

void op_pmsubh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 p[8] = {a.sh[0] * b.sh[0], a.sh[1] * b.sh[1], a.sh[2] * b.sh[2], a.sh[3] * b.sh[3],
                      a.sh[4] * b.sh[4], a.sh[5] * b.sh[5], a.sh[6] * b.sh[6], a.sh[7] * b.sh[7]};
    const u32 w0 = get_lo_w(c, 0) - u32(p[0]);
    const u32 w1 = get_hi_w(c, 0) - u32(p[2]);
    const u32 w2 = get_lo_w(c, 2) - u32(p[4]);
    const u32 w3 = get_hi_w(c, 2) - u32(p[6]);
    set_lo_w(c, 0, w0);
    set_lo_w(c, 1, get_lo_w(c, 1) - u32(p[1]));
    set_hi_w(c, 0, w1);
    set_hi_w(c, 1, get_hi_w(c, 1) - u32(p[3]));
    set_lo_w(c, 2, w2);
    set_lo_w(c, 3, get_lo_w(c, 3) - u32(p[5]));
    set_hi_w(c, 2, w3);
    set_hi_w(c, 3, get_hi_w(c, 3) - u32(p[7]));
    if (rd != 0) {
        V r;
        r.w[0] = w0;
        r.w[1] = w1;
        r.w[2] = w2;
        r.w[3] = w3;
        c.r[rd] = store_v(r);
    }
}

// PHMADH/PHMSBH: pairwise halfword multiply-add; the odd product lands in the
// adjacent word (PHMSBH stores its complement — undocumented behavior per PCSX2).
void op_phmadh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 p[8] = {a.sh[0] * b.sh[0], a.sh[1] * b.sh[1], a.sh[2] * b.sh[2], a.sh[3] * b.sh[3],
                      a.sh[4] * b.sh[4], a.sh[5] * b.sh[5], a.sh[6] * b.sh[6], a.sh[7] * b.sh[7]};
    set_lo_w(c, 0, u32(p[1] + p[0]));
    set_lo_w(c, 1, u32(p[1]));
    set_hi_w(c, 0, u32(p[3] + p[2]));
    set_hi_w(c, 1, u32(p[3]));
    set_lo_w(c, 2, u32(p[5] + p[4]));
    set_lo_w(c, 3, u32(p[5]));
    set_hi_w(c, 2, u32(p[7] + p[6]));
    set_hi_w(c, 3, u32(p[7]));
    if (rd != 0) {
        V r;
        r.w[0] = get_lo_w(c, 0);
        r.w[1] = get_hi_w(c, 0);
        r.w[2] = get_lo_w(c, 2);
        r.w[3] = get_hi_w(c, 2);
        c.r[rd] = store_v(r);
    }
}

void op_phmsbh(EEContext& c, int rd, int rs, int rt) {
    const V a = load_v(c.r[rs]);
    const V b = load_v(c.r[rt]);
    const s32 p[8] = {a.sh[0] * b.sh[0], a.sh[1] * b.sh[1], a.sh[2] * b.sh[2], a.sh[3] * b.sh[3],
                      a.sh[4] * b.sh[4], a.sh[5] * b.sh[5], a.sh[6] * b.sh[6], a.sh[7] * b.sh[7]};
    set_lo_w(c, 0, u32(p[1] - p[0]));
    set_lo_w(c, 1, ~u32(p[1]));
    set_hi_w(c, 0, u32(p[3] - p[2]));
    set_hi_w(c, 1, ~u32(p[3]));
    set_lo_w(c, 2, u32(p[5] - p[4]));
    set_lo_w(c, 3, ~u32(p[5]));
    set_hi_w(c, 2, u32(p[7] - p[6]));
    set_hi_w(c, 3, ~u32(p[7]));
    if (rd != 0) {
        V r;
        r.w[0] = get_lo_w(c, 0);
        r.w[1] = get_hi_w(c, 0);
        r.w[2] = get_lo_w(c, 2);
        r.w[3] = get_hi_w(c, 2);
        c.r[rd] = store_v(r);
    }
}

} // namespace ee::rt
