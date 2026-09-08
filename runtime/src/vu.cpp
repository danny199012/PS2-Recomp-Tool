// SPDX-License-Identifier: GPL-3.0-only
// VU0 macro-mode (COP2) helpers called by generated code.
// Semantics per the VU User's Manual; MAC/status flags and the clamp-to-max
// overflow behavior per PCSX2's VUflags.cpp.
#include <ee/hw.hpp>
#include <ee/runtime.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ee::rt {

namespace {

inline u32 f2u(float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    return v;
}
inline float u2f(u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// VU FPU: overflow clamps to +-max float (no inf/NaN propagation).
inline float vu_clamp(float f) {
    const u32 v = f2u(f);
    if (((v >> 23) & 0xFF) == 0xFF)
        return u2f((v & 0x80000000u) | 0x7F7FFFFFu);
    return f;
}

// Per-lane MAC flag update. Lane i: 0=x, 1=y, 2=z, 3=w; shift = 3 - i (per PCSX2).
inline void vu_mac_update(EEContext& c, int shift, float f) {
    const u32 v = f2u(f);
    const u32 exp = (v >> 23) & 0xFF;
    const u32 sign = v & 0x80000000u;
    if (sign)
        c.vu_mac |= (0x0010u << shift);
    else
        c.vu_mac &= ~(0x0010u << shift);
    if (f == 0.0f) {
        c.vu_mac = (c.vu_mac & ~(0x1100u << shift)) | (0x0001u << shift);
        return;
    }
    if (exp == 0) { // underflow (denormal)
        c.vu_mac = (c.vu_mac & ~(0x1000u << shift)) | (0x0101u << shift);
        return;
    }
    if (exp == 255) { // overflow
        c.vu_mac = (c.vu_mac & ~(0x0101u << shift)) | (0x1000u << shift);
        return;
    }
    c.vu_mac &= ~(0x1101u << shift);
}

inline void vu_stat_update(EEContext& c) {
    u32 f = 0;
    if (c.vu_mac & 0x000F)
        f |= 1;
    if (c.vu_mac & 0x00F0)
        f |= 2;
    if (c.vu_mac & 0x0F00)
        f |= 4;
    if (c.vu_mac & 0xF000)
        f |= 8;
    c.vu_status = (c.vu_status & ~0xFu) | f;
}

// Lane access: 0=x (lo low), 1=y (lo high), 2=z (hi low), 3=w (hi high).
inline float lane(u128 v, int i) {
    return u2f(u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1))));
}
inline void set_lane(u128& v, int i, float f) {
    u64& h = (i < 2) ? v.lo : v.hi;
    const int s = 32 * (i & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(f2u(f)) << s);
}
inline u32 lane_u(u128 v, int i) {
    return u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1)));
}
inline void set_lane_u(u128& v, int i, u32 x) {
    u64& h = (i < 2) ? v.lo : v.hi;
    const int s = 32 * (i & 1);
    h = (h & ~(0xFFFFFFFFull << s)) | (u64(x) << s);
}

inline bool dest_bit(u8 dest, int i) { return (dest >> (3 - i)) & 1; }

} // namespace

void vu_arith(EEContext& c, VuOp op, int fd, int fs, int ft, u8 dest, int bc) {
    for (int i = 0; i < 4; ++i) {
        const float a = lane(c.vf[fs], i);
        const float b = bc >= 0 ? lane(c.vf[ft], bc) : lane(c.vf[ft], i);
        const float acc = lane(c.vacc, i);
        float r = 0.0f;
        bool write_fd = true;
        switch (op) {
        case VuOp::Add: r = a + b; break;
        case VuOp::Sub: r = a - b; break;
        case VuOp::Mul: r = a * b; break;
        case VuOp::Max: r = a > b ? a : b; break;
        case VuOp::Min: r = a < b ? a : b; break;
        case VuOp::Adda: r = a + b; write_fd = false; break;
        case VuOp::Suba: r = a - b; write_fd = false; break;
        case VuOp::Mula: r = a * b; write_fd = false; break;
        case VuOp::Madd: r = acc + a * b; break;
        case VuOp::Msub: r = acc - a * b; break;
        case VuOp::Madda: r = acc + a * b; write_fd = false; break;
        case VuOp::Msuba: r = acc - a * b; write_fd = false; break;
        default: break;
        }
        vu_mac_update(c, 3 - i, r);
        r = vu_clamp(r);
        if (!dest_bit(dest, i))
            continue;
        if (write_fd)
            set_lane(c.vf[fd], i, r);
        else
            set_lane(c.vacc, i, r);
    }
    vu_stat_update(c);
}

void vu_arith_qi(EEContext& c, VuOp op, int fd, int fs, u8 dest, bool use_q) {
    const float src = u2f(use_q ? c.vq : c.vi_imm);
    for (int i = 0; i < 4; ++i) {
        const float a = lane(c.vf[fs], i);
        const float acc = lane(c.vacc, i);
        float r = 0.0f;
        bool write_fd = true;
        switch (op) {
        case VuOp::Add: r = a + src; break;
        case VuOp::Sub: r = a - src; break;
        case VuOp::Mul: r = a * src; break;
        case VuOp::Max: r = a > src ? a : src; break;
        case VuOp::Min: r = a < src ? a : src; break;
        case VuOp::Adda: r = a + src; write_fd = false; break;
        case VuOp::Suba: r = a - src; write_fd = false; break;
        case VuOp::Mula: r = a * src; write_fd = false; break;
        case VuOp::Madd: r = acc + a * src; break;
        case VuOp::Msub: r = acc - a * src; break;
        case VuOp::Madda: r = acc + a * src; write_fd = false; break;
        case VuOp::Msuba: r = acc - a * src; write_fd = false; break;
        default: break;
        }
        vu_mac_update(c, 3 - i, r);
        r = vu_clamp(r);
        if (!dest_bit(dest, i))
            continue;
        if (write_fd)
            set_lane(c.vf[fd], i, r);
        else
            set_lane(c.vacc, i, r);
    }
    vu_stat_update(c);
}

// VOPMULA / VOPMSUB: cross-product-style (xyz only).
void vu_opmula(EEContext& c, bool sub, int fd, int fs, int ft, u8 dest) {
    const float ax = lane(c.vf[fs], 0), ay = lane(c.vf[fs], 1), az = lane(c.vf[fs], 2);
    const float bx = lane(c.vf[ft], 0), by = lane(c.vf[ft], 1), bz = lane(c.vf[ft], 2);
    float r[3];
    if (!sub) { // OPMULA: ACC.xyz = fs.yzx * ft.zxy
        r[0] = ay * bz;
        r[1] = az * bx;
        r[2] = ax * by;
    } else { // VOPMSUB: fd.xyz = ACC.xyz - fs.yzx * ft.zxy
        r[0] = lane(c.vacc, 0) - ay * bz;
        r[1] = lane(c.vacc, 1) - az * bx;
        r[2] = lane(c.vacc, 2) - ax * by;
    }
    for (int i = 0; i < 3; ++i) {
        vu_mac_update(c, 3 - i, r[i]);
        r[i] = vu_clamp(r[i]);
        if (!dest_bit(dest, i))
            continue;
        if (sub)
            set_lane(c.vf[fd], i, r[i]);
        else
            set_lane(c.vacc, i, r[i]);
    }
    vu_stat_update(c);
}

// --- conversions (VITOF0/4/12/15, VFTOI0/4/12/15) --------------------------------

void vu_itof(EEContext& c, int fd, int fs, u8 dest, int shift) {
    const float scale = shift == 0 ? 1.0f : float(1.0 / double(1u << shift));
    for (int i = 0; i < 4; ++i) {
        if (!dest_bit(dest, i))
            continue;
        const s32 v = s32(lane_u(c.vf[fs], i));
        set_lane(c.vf[fd], i, vu_clamp(float(v) * scale));
    }
}

void vu_ftoi(EEContext& c, int fd, int fs, u8 dest, int shift) {
    const float scale = shift == 0 ? 1.0f : float(1u << shift);
    for (int i = 0; i < 4; ++i) {
        if (!dest_bit(dest, i))
            continue;
        float f = lane(c.vf[fs], i) * scale;
        // truncate toward zero, clamped to s32 range
        if (f >= 2147483648.0f)
            f = 2147483647.0f;
        else if (f < -2147483648.0f)
            f = -2147483648.0f;
        set_lane_u(c.vf[fd], i, u32(s32(f)));
    }
}

// --- integer ops (16-bit vi registers) ---------------------------------------------

void vu_iadd(EEContext& c, int fd, int fs, int ft) { c.vi[fd] = u16(c.vi[fs] + c.vi[ft]); }
void vu_isub(EEContext& c, int fd, int fs, int ft) { c.vi[fd] = u16(c.vi[fs] - c.vi[ft]); }
void vu_iaddi(EEContext& c, int ft, int fs, s32 imm5) { c.vi[ft] = u16(c.vi[fs] + imm5); }
void vu_iand(EEContext& c, int fd, int fs, int ft) { c.vi[fd] = u16(c.vi[fs] & c.vi[ft]); }
void vu_ior(EEContext& c, int fd, int fs, int ft) { c.vi[fd] = u16(c.vi[fs] | c.vi[ft]); }

// --- moves ---------------------------------------------------------------------------

void vu_move(EEContext& c, int fd, int fs, u8 dest) {
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[fd], i, lane_u(c.vf[fs], i));
}

void vu_mr32(EEContext& c, int fd, int fs, u8 dest) {
    // rotate words: fd.x = fs.y, fd.y = fs.z, fd.z = fs.w, fd.w = fs.x
    const u32 x = lane_u(c.vf[fs], 0), y = lane_u(c.vf[fs], 1), z = lane_u(c.vf[fs], 2),
              w = lane_u(c.vf[fs], 3);
    if (dest_bit(dest, 0))
        set_lane_u(c.vf[fd], 0, y);
    if (dest_bit(dest, 1))
        set_lane_u(c.vf[fd], 1, z);
    if (dest_bit(dest, 2))
        set_lane_u(c.vf[fd], 2, w);
    if (dest_bit(dest, 3))
        set_lane_u(c.vf[fd], 3, x);
}

void vu_abs(EEContext& c, int fd, int fs, u8 dest) {
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[fd], i, lane_u(c.vf[fs], i) & 0x7FFFFFFFu);
}

// --- divide / square root (Q register) ------------------------------------------------

void vu_div(EEContext& c, int fs, int fsf, int ft, int ftf) {
    const float a = lane(c.vf[fs], fsf);
    const float b = lane(c.vf[ft], ftf);
    float q = a / b; // inf/NaN handled by clamp
    c.vq = f2u(vu_clamp(q));
    // TODO-verify: VU status division-by-zero flag (bit 5/6 area)
    if (b == 0.0f)
        c.vu_status |= 0x20;
}

void vu_sqrt(EEContext& c, int ft, int ftf) {
    const float v = lane(c.vf[ft], ftf);
    c.vq = f2u(vu_clamp(sqrtf(v < 0.0f ? 0.0f : v)));
}

void vu_rsqrt(EEContext& c, int fs, int fsf, int ft, int ftf) {
    const float a = lane(c.vf[fs], fsf);
    const float b = lane(c.vf[ft], ftf);
    c.vq = f2u(vu_clamp(a / sqrtf(b < 0.0f ? 0.0f : b)));
}

void vu_waitq(EEContext& c) { (void)c; } // synchronous in our model

// --- VU0 memory load/store (via Hw; address = vi in 128-bit word units) ---------------
// TODO-verify exact addressing/masking.

void vu_lqi(EEContext& c, int ft, int fs, u8 dest) {
    const u32 addr = (c.vi[fs] & 0xFF) * 16;
    u128 v{};
    const u8* src = c.rt->hw->vu0_dmem.data() + addr;
    std::memcpy(&v.lo, src, 8);
    std::memcpy(&v.hi, src + 8, 8);
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[ft], i, lane_u(v, i));
    c.vi[fs] = u16(c.vi[fs] + 1);
}

void vu_sqi(EEContext& c, int ft, int fs, u8 dest) {
    const u32 addr = (c.vi[fs] & 0xFF) * 16;
    u8* dst = c.rt->hw->vu0_dmem.data() + addr;
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i)) {
            const u32 v = lane_u(c.vf[ft], i);
            std::memcpy(dst + i * 4, &v, 4);
        }
    c.vi[fs] = u16(c.vi[fs] + 1);
}

void vu_lqd(EEContext& c, int ft, int fs, u8 dest) {
    c.vi[fs] = u16(c.vi[fs] - 1);
    const u32 addr = (c.vi[fs] & 0xFF) * 16;
    u128 v{};
    const u8* src = c.rt->hw->vu0_dmem.data() + addr;
    std::memcpy(&v.lo, src, 8);
    std::memcpy(&v.hi, src + 8, 8);
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[ft], i, lane_u(v, i));
}

void vu_sqd(EEContext& c, int ft, int fs, u8 dest) {
    c.vi[fs] = u16(c.vi[fs] - 1);
    const u32 addr = (c.vi[fs] & 0xFF) * 16;
    u8* dst = c.rt->hw->vu0_dmem.data() + addr;
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i)) {
            const u32 v = lane_u(c.vf[ft], i);
            std::memcpy(dst + i * 4, &v, 4);
        }
}

void vu_ilwr(EEContext& c, int ft, int fs, int ftf) {
    const u32 addr = (c.vi[fs] & 0xFF) * 16 + u32(ftf) * 4;
    u32 v = 0;
    std::memcpy(&v, c.rt->hw->vu0_dmem.data() + addr, 4);
    c.vi[ft] = u16(v);
}

void vu_iswr(EEContext& c, int ft, int fs, int ftf) {
    const u32 addr = (c.vi[fs] & 0xFF) * 16 + u32(ftf) * 4;
    const u32 v = c.vi[ft];
    std::memcpy(c.rt->hw->vu0_dmem.data() + addr, &v, 4);
}

// --- misc -----------------------------------------------------------------------------

void vu_mtir(EEContext& c, int fd, int fs, int fsf) {
    c.vi[fd] = u16(f2u(lane(c.vf[fs], fsf)));
}

void vu_mfir(EEContext& c, int fd, int fs, u8 dest) {
    const u32 v = u32(s32(s16(c.vi[fs]))); // sign-extend
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[fd], i, v);
}

void vu_rget(EEContext& c, int ft, u8 dest) {
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[ft], i, c.vr);
}

void vu_rnext(EEContext& c, int ft, u8 dest) {
    // 23-bit LFSR (x^23 + x^18 + 1 approximation; TODO-verify exact VU polynomial)
    u32 r = c.vr;
    const u32 bit = ((r >> 22) ^ (r >> 17)) & 1;
    r = ((r << 1) | bit) & 0x7FFFFF;
    c.vr = r;
    for (int i = 0; i < 4; ++i)
        if (dest_bit(dest, i))
            set_lane_u(c.vf[ft], i, r);
}

void vu_rinit(EEContext& c, int fs, int fsf) {
    c.vr = f2u(lane(c.vf[fs], fsf)) & 0x7FFFFF;
}

void vu_rxor(EEContext& c, int fs, int fsf) {
    c.vr ^= f2u(lane(c.vf[fs], fsf)) & 0x7FFFFF;
}

void vu_clipw(EEContext& c, int fs, int ft) {
    // clip fs.xyz against +-ft.w; 6 new flags shifted into the 24-bit clip register
    const float w = lane(c.vf[ft], 3);
    const float x = lane(c.vf[fs], 0), y = lane(c.vf[fs], 1), z = lane(c.vf[fs], 2);
    u32 bits = 0;
    if (x > w) bits |= 0x01;
    if (x < -w) bits |= 0x02;
    if (y > w) bits |= 0x04;
    if (y < -w) bits |= 0x08;
    if (z > w) bits |= 0x10;
    if (z < -w) bits |= 0x20;
    c.vu_clip = ((c.vu_clip << 6) | bits) & 0xFFFFFF;
}

void vu_nop(EEContext& c) { (void)c; }

} // namespace ee::rt
