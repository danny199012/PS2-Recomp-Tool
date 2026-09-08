// SPDX-License-Identifier: GPL-3.0-only
// VU0 macro-mode tests: arithmetic, conversions, integer ops, moves,
// divide/sqrt, clip, MAC/status flags, accumalator.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <ee/hw.hpp>
#include <ee/runtime.hpp>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace ee;
using namespace ee::rt;

static float lane_f(u128 v, int i) {
    u32 bits = u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1)));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
static u32 lane_u(u128 v, int i) {
    return u32(((i < 2) ? v.lo : v.hi) >> (32 * (i & 1)));
}
static u128 vec(float x, float y, float z, float w) {
    u32 bx, by, bz, bw;
    std::memcpy(&bx, &x, 4);
    std::memcpy(&by, &y, 4);
    std::memcpy(&bz, &z, 4);
    std::memcpy(&bw, &w, 4);
    return {u64(bx) | (u64(by) << 32), u64(bz) | (u64(bw) << 32)};
}

int main() {
    Runtime rt;
    EEContext c;
    c.rt = &rt;

    // VADD (xyzw): 1+2=3, 3+4=7, 5+6=11, 7+8=15
    c.vf[1] = vec(1, 3, 5, 7);
    c.vf[2] = vec(2, 4, 6, 8);
    vu_arith(c, VuOp::Add, 3, 1, 2, 0xF, -1);
    CHECK(lane_f(c.vf[3], 0) == 3.0f);
    CHECK(lane_f(c.vf[3], 1) == 7.0f);
    CHECK(lane_f(c.vf[3], 2) == 11.0f);
    CHECK(lane_f(c.vf[3], 3) == 15.0f);

    // VSUB with dest mask .xy (only x,y written)
    c.vf[1] = vec(10, 20, 30, 40);
    c.vf[2] = vec(1, 2, 3, 4);
    c.vf[3] = vec(0, 0, 0, 0);
    vu_arith(c, VuOp::Sub, 3, 1, 2, 0xC, -1); // .xy = bits 3,2 = 0xC
    CHECK(lane_f(c.vf[3], 0) == 9.0f);
    CHECK(lane_f(c.vf[3], 1) == 18.0f);
    CHECK(lane_f(c.vf[3], 2) == 0.0f); // not written
    CHECK(lane_f(c.vf[3], 3) == 0.0f); // not written

    // VMUL with broadcast .x (bc=0): each lane = fs.i * ft.x
    c.vf[1] = vec(1, 2, 3, 4);
    c.vf[2] = vec(3, 5, 7, 11);
    vu_arith(c, VuOp::Mul, 3, 1, 2, 0xF, 0); // ft.x=3 broadcast
    CHECK(lane_f(c.vf[3], 0) == 3.0f);  // 1*3
    CHECK(lane_f(c.vf[3], 1) == 6.0f);  // 2*3
    CHECK(lane_f(c.vf[3], 2) == 9.0f);  // 3*3
    CHECK(lane_f(c.vf[3], 3) == 12.0f); // 4*3

    // VMADD: fd = ACC + fs*ft
    c.vacc = vec(100, 200, 300, 400);
    c.vf[1] = vec(1, 2, 3, 4);
    c.vf[2] = vec(10, 20, 30, 40);
    vu_arith(c, VuOp::Madd, 3, 1, 2, 0xF, -1);
    CHECK(lane_f(c.vf[3], 0) == 110.0f);
    CHECK(lane_f(c.vf[3], 1) == 240.0f);
    CHECK(lane_f(c.vf[3], 2) == 390.0f);
    CHECK(lane_f(c.vf[3], 3) == 560.0f);

    // VMULA (writes accumulator, not fd)
    c.vacc = vec(0, 0, 0, 0);
    c.vf[1] = vec(2, 3, 0, 0);
    c.vf[2] = vec(4, 5, 0, 0);
    vu_arith(c, VuOp::Mula, 0, 1, 2, 0xF, -1);
    CHECK(lane_f(c.vacc, 0) == 8.0f);
    CHECK(lane_f(c.vacc, 1) == 15.0f);

    // VMUL.q (Q source)
    c.vq = 0;
    float q = 3.0f;
    std::memcpy(&c.vq, &q, 4);
    c.vf[1] = vec(1, 2, 3, 4);
    vu_arith_qi(c, VuOp::Mul, 3, 1, 0xF, true);
    CHECK(lane_f(c.vf[3], 0) == 3.0f);
    CHECK(lane_f(c.vf[3], 1) == 6.0f);
    CHECK(lane_f(c.vf[3], 2) == 9.0f);
    CHECK(lane_f(c.vf[3], 3) == 12.0f);

    // VADD.i (I source)
    float i_val = 5.0f;
    std::memcpy(&c.vi_imm, &i_val, 4);
    c.vf[1] = vec(1, 2, 3, 4);
    vu_arith_qi(c, VuOp::Add, 3, 1, 0xF, false);
    CHECK(lane_f(c.vf[3], 0) == 6.0f);
    CHECK(lane_f(c.vf[3], 3) == 9.0f);

    // VMAX with broadcast .w (bc=3)
    c.vf[1] = vec(1, 2, 3, 4);
    c.vf[2] = vec(0, 0, 0, 100);
    vu_arith(c, VuOp::Max, 3, 1, 2, 0xF, 3);
    CHECK(lane_f(c.vf[3], 0) == 100.0f);
    CHECK(lane_f(c.vf[3], 3) == 100.0f);

    // VABS (clear sign bits)
    c.vf[1] = vec(-1, -2, 3, -4);
    vu_abs(c, 2, 1, 0xF);
    CHECK(lane_f(c.vf[2], 0) == 1.0f);
    CHECK(lane_f(c.vf[2], 1) == 2.0f);
    CHECK(lane_f(c.vf[2], 2) == 3.0f);
    CHECK(lane_f(c.vf[2], 3) == 4.0f);

    // VMOVE (with dest mask .zw)
    c.vf[1] = vec(1, 2, 3, 4);
    c.vf[2] = vec(0, 0, 0, 0);
    vu_move(c, 2, 1, 0x3); // .zw = bits 1,0 = 0x3
    CHECK(lane_f(c.vf[2], 0) == 0.0f); // not written
    CHECK(lane_f(c.vf[2], 1) == 0.0f); // not written
    CHECK(lane_f(c.vf[2], 2) == 3.0f);
    CHECK(lane_f(c.vf[2], 3) == 4.0f);

    // VMR32: rotate words left (x->w, y->x, z->y, w->z)
    // Actually per PCSX2: fd.x=fs.y, fd.y=fs.z, fd.z=fs.w, fd.w=fs.x
    c.vf[1] = vec(1, 2, 3, 4);
    vu_mr32(c, 2, 1, 0xF);
    CHECK(lane_f(c.vf[2], 0) == 2.0f); // x <- y
    CHECK(lane_f(c.vf[2], 1) == 3.0f); // y <- z
    CHECK(lane_f(c.vf[2], 2) == 4.0f); // z <- w
    CHECK(lane_f(c.vf[2], 3) == 1.0f); // w <- x

    // VITOF0: integer -> float (no scaling)
    c.vf[1] = vec(float(s32(10)), float(s32(20)), float(s32(30)), float(s32(40)));
    // But wait -- VITOF reads the vf register as raw u32 bits, not float
    c.vf[1] = {};
    c.vf[1].lo = u64(10) | (u64(20) << 32);
    c.vf[1].hi = u64(30) | (u64(40) << 32);
    vu_itof(c, 2, 1, 0xF, 0);
    CHECK(lane_f(c.vf[2], 0) == 10.0f);
    CHECK(lane_f(c.vf[2], 1) == 20.0f);
    CHECK(lane_f(c.vf[2], 2) == 30.0f);
    CHECK(lane_f(c.vf[2], 3) == 40.0f);

    // VFTOI0: float -> integer (truncation, no scaling)
    c.vf[1] = vec(3.7f, -1.5f, 100.9f, 0.1f);
    vu_ftoi(c, 2, 1, 0xF, 0);
    CHECK(lane_u(c.vf[2], 0) == u32(3)); // truncation toward zero
    CHECK(lane_u(c.vf[2], 1) == u32(s32(-1)));
    CHECK(lane_u(c.vf[2], 2) == u32(100));
    CHECK(lane_u(c.vf[2], 3) == u32(0));

    // VFTOI4: float * 16 -> integer
    c.vf[1] = vec(1.0f, 2.0f, 3.0f, 4.0f);
    vu_ftoi(c, 2, 1, 0xF, 4);
    CHECK(lane_u(c.vf[2], 0) == 16);
    CHECK(lane_u(c.vf[2], 3) == 64);

    // VITOF4: integer / (1<<4) -> float
    c.vf[1] = {};
    c.vf[1].lo = u64(160) | (u64(320) << 32);
    c.vf[1].hi = u64(480) | (u64(640) << 32);
    vu_itof(c, 2, 1, 0xF, 4);
    CHECK(lane_f(c.vf[2], 0) == 10.0f);
    CHECK(lane_f(c.vf[2], 3) == 40.0f);

    // Integer ops
    c.vi[1] = 100;
    c.vi[2] = 60;
    vu_iadd(c, 3, 1, 2);
    CHECK(c.vi[3] == 160);
    vu_isub(c, 3, 1, 2);
    CHECK(c.vi[3] == 40);
    vu_iaddi(c, 3, 1, -5);
    CHECK(c.vi[3] == 95);
    c.vi[1] = 0xF0F0;
    c.vi[2] = 0x0F0F;
    vu_iand(c, 3, 1, 2);
    CHECK(c.vi[3] == 0x0000);
    vu_ior(c, 3, 1, 2);
    CHECK(c.vi[3] == 0xFFFF);

    // VDIV: 10 / 2 = 5, stored in Q
    c.vf[1] = vec(10, 0, 0, 0);
    c.vf[2] = vec(2, 0, 0, 0);
    c.vq = 0;
    vu_div(c, 1, 0, 2, 0);
    float qv;
    std::memcpy(&qv, &c.vq, 4);
    CHECK(qv == 5.0f);

    // VSQRT: sqrt(16) = 4
    c.vf[1] = vec(16, 0, 0, 0);
    c.vq = 0;
    vu_sqrt(c, 1, 0);
    std::memcpy(&qv, &c.vq, 4);
    CHECK(qv == 4.0f);

    // VRSQRT: 10 / sqrt(4) = 5
    c.vf[1] = vec(10, 0, 0, 0);
    c.vf[2] = vec(4, 0, 0, 0);
    c.vq = 0;
    vu_rsqrt(c, 1, 0, 2, 0);
    std::memcpy(&qv, &c.vq, 4);
    CHECK(qv == 5.0f);

    // VOPMULA: ACC = fs.yzx * ft.zxy
    c.vacc = {};
    c.vf[1] = vec(1, 2, 3, 0);
    c.vf[2] = vec(4, 5, 6, 0);
    vu_opmula(c, false, 0, 1, 2, 0xE); // .xyz
    // ACC.x = fs.y * ft.z = 2*6=12
    // ACC.y = fs.z * ft.x = 3*4=12
    // ACC.z = fs.x * ft.y = 1*5=5
    CHECK(lane_f(c.vacc, 0) == 12.0f);
    CHECK(lane_f(c.vacc, 1) == 12.0f);
    CHECK(lane_f(c.vacc, 2) == 5.0f);

    // VOPMSUB: fd = ACC - fs.yzx * ft.zxy
    c.vacc = vec(20, 20, 20, 0);
    c.vf[1] = vec(1, 2, 3, 0);
    c.vf[2] = vec(4, 5, 6, 0);
    vu_opmula(c, true, 3, 1, 2, 0xE); // .xyz
    // fd.x = 20 - 2*6 = 8
    // fd.y = 20 - 3*4 = 8
    // fd.z = 20 - 1*5 = 15
    CHECK(lane_f(c.vf[3], 0) == 8.0f);
    CHECK(lane_f(c.vf[3], 1) == 8.0f);
    CHECK(lane_f(c.vf[3], 2) == 15.0f);

    // VMFIR: vi -> vf (sign-extended 16-bit)
    c.vi[1] = s16(-42);
    vu_mfir(c, 2, 1, 0xF);
    CHECK(lane_u(c.vf[2], 0) == u32(s32(-42)));
    CHECK(lane_u(c.vf[2], 3) == u32(s32(-42)));

    // VMTIR: vf.fsf -> vi (raw u32 of the float)
    c.vf[1] = vec(42.0f, 0, 0, 0);
    vu_mtir(c, 3, 1, 0); // fsf=0 (x lane)
    float chk = (float)42.0f;
    u32 chk_bits;
    std::memcpy(&chk_bits, &chk, 4);
    CHECK(c.vi[3] == u16(chk_bits));

    // MAC flags: overflow should set flag
    c.vu_mac = 0;
    // Use INFINITY to guarantee exp=255 (overflow path)
    c.vf[1] = vec(INFINITY, 0, 0, 0);
    c.vf[2] = vec(0, 0, 0, 0);
    vu_arith(c, VuOp::Add, 3, 1, 2, 0x8, -1); // INFINITY + 0 = INFINITY
    // overflow: result should clamp to max, MAC bit should be set
    CHECK((c.vu_mac & 0x8000) != 0); // overflow flag for x lane (shift 3)

    std::printf("test_vu: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
