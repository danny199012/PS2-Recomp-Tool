// SPDX-License-Identifier: GPL-3.0-only
#include "fixture.hpp"

#include <ee/runtime.hpp>

#include <cstdio>

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

int main() {
    Runtime rt;
    EEContext ctx;
    ctx.rt = &rt;

    // Memory: kuseg/kseg0 aliasing and scratchpad.
    st32(ctx, 0x00100000, 0xDEADBEEF);
    CHECK(ld32(ctx, 0x80100000) == 0xDEADBEEF); // kseg0 alias
    st32(ctx, 0x70000004, 0x12345678);
    CHECK(ld32(ctx, 0x70000004) == 0x12345678); // scratchpad
    const ee::u128 q{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    st128(ctx, 0x2000, q);
    const ee::u128 q2 = ld128(ctx, 0x2000);
    CHECK(q2.lo == q.lo && q2.hi == q.hi);

    // Register write semantics: 32-bit ops sign-extend, $zero never written.
    set32(ctx, 5, 0x80000000u);
    CHECK(gpr(ctx, 5) == 0xFFFFFFFF80000000ull);
    set64(ctx, 6, 0x0102030405060708ull);
    CHECK(gpr32(ctx, 6) == 0x05060708u);

    // mult/div edge cases.
    set64(ctx, 8, 3);
    set64(ctx, 9, u64(s64(-2)));
    op_mult(ctx, 8, 9, 10);
    CHECK(s64(ctx.lo.lo) == -6);
    CHECK(s64(ctx.hi.lo) == -1);
    CHECK(s64(gpr(ctx, 10)) == -6);
    set64(ctx, 8, 0xFFFFFFFFu);
    set64(ctx, 9, 2);
    op_multu(ctx, 8, 9, 0);
    CHECK(ctx.lo.lo == 0xFFFFFFFFFFFFFFFEull);
    CHECK(ctx.hi.lo == 1);
    set64(ctx, 8, 7);
    set64(ctx, 9, 2);
    op_div(ctx, 8, 9, 0);
    CHECK(s64(ctx.lo.lo) == 3 && s64(ctx.hi.lo) == 1);
    set64(ctx, 9, 0);
    op_div(ctx, 8, 9, 0); // division by zero
    CHECK(s64(ctx.lo.lo) == -1 && s64(ctx.hi.lo) == 7);
    set64(ctx, 8, u64(s64(s32(0x80000000))));
    set64(ctx, 9, u64(s64(-1)));
    op_div(ctx, 8, 9, 0); // INT32_MIN / -1 (would trap on x86)
    CHECK(s64(ctx.lo.lo) == s32(0x80000000) && ctx.hi.lo == 0);
    set64(ctx, 9, 0);
    op_divu(ctx, 8, 9, 0);
    CHECK(ctx.lo.lo == 0xFFFFFFFFFFFFFFFFull); // sext32(0xFFFFFFFF)
    ctx.lo.lo = u64(5);
    ctx.hi.lo = 0;
    set64(ctx, 8, 3);
    set64(ctx, 9, 4);
    op_madd(ctx, 8, 9, 0);
    CHECK(s64(ctx.lo.lo) == 17);

    // Unaligned access: bytes 01 02 03 04 05 06 07 08 at 0x1000.
    for (int i = 0; i < 8; ++i)
        st8(ctx, 0x1000 + i, u32(i + 1));
    set32(ctx, 11, 0xDEADBEEF);
    op_lwl(ctx, 0x1004, 11);
    op_lwr(ctx, 0x1001, 11);
    CHECK(gpr32(ctx, 11) == 0x05040302);
    set32(ctx, 12, 0x11223344);
    op_swl(ctx, 0x1004, 12);
    op_swr(ctx, 0x1001, 12);
    set32(ctx, 13, 0);
    op_lwl(ctx, 0x1004, 13);
    op_lwr(ctx, 0x1001, 13);
    CHECK(gpr32(ctx, 13) == 0x11223344);

    // MMI lane ops.
    const ee::u128 a{0x0000000200000001ull, 0x0000000400000003ull}; // words 1,2,3,4
    const ee::u128 b{0x0000000500000005ull, 0x0000000500000005ull}; // words 5,5,5,5
    ctx.r[1] = a;
    ctx.r[2] = b;
    op_paddw(ctx, 3, 1, 2);
    CHECK(ctx.r[3].lo == 0x0000000700000006ull);
    CHECK(ctx.r[3].hi == 0x0000000900000008ull);
    op_psubw(ctx, 4, 2, 1);
    CHECK(ctx.r[4].lo == 0x0000000300000004ull); // 5-2, 5-1
    op_pextlw(ctx, 5, 1, 2);                     // {b.w0, a.w0, b.w1, a.w1}
    CHECK(ctx.r[5].lo == 0x0000000100000005ull);
    CHECK(ctx.r[5].hi == 0x0000000200000005ull);
    op_pcpyld(ctx, 6, 1, 2);                     // {rt.lo, rs.lo}
    CHECK(ctx.r[6].lo == b.lo);
    CHECK(ctx.r[6].hi == a.lo);
    // paddsb saturation: 127 + 1 == 127, -128 - 1 == -128
    ee::u128 s{};
    ee::u128 one{};
    for (int i = 0; i < 8; ++i) {
        st8(ctx, 0x3000 + i, 127);
        st8(ctx, 0x3008 + i, 0x80); // -128
        st8(ctx, 0x3010 + i, 1);
    }
    s = ld128(ctx, 0x3000);
    one = ld128(ctx, 0x3010);
    ctx.r[1] = s;
    ctx.r[2] = one;
    op_paddsb(ctx, 3, 1, 2);
    CHECK((ctx.r[3].lo & 0xFF) == 127);
    ctx.r[1] = ld128(ctx, 0x3008);
    op_psubsb(ctx, 3, 1, 2);
    CHECK((ctx.r[3].lo & 0xFF) == 0x80); // -128 saturated
    // psllw
    ctx.r[1] = a;
    op_psllw(ctx, 3, 1, 4);
    CHECK(ctx.r[3].lo == 0x0000002000000010ull);
    // pmfhl.lw / slw
    ctx.lo.lo = 0xAABBCCDD;
    ctx.hi.lo = 0x11223344;
    op_pmfhl(ctx, 7, 0);
    CHECK(ctx.r[7].lo == 0xAABBCCDD && ctx.r[7].hi == 0);
    op_pmfhl(ctx, 7, 2);
    CHECK(ctx.r[7].lo == 0xAABBCCDD && ctx.r[7].hi == 0x11223344);

    // ELF loading into guest memory (fixture).
    {
        auto image = ee::elf::Image::load_bytes(ee::test::make_analysis_elf());
        CHECK(image.has_value());
        if (image) {
            Runtime rt2;
            std::string err;
            CHECK(rt2.load_elf(*image, &err));
            EEContext ctx2;
            ctx2.rt = &rt2;
            CHECK(ld32(ctx2, 0x100000) == 0x27BDFFE0); // first instruction of main
            CHECK(ld32(ctx2, 0x200000) == 0x00100024); // jump table entry
        }
    }
    std::printf("test_runtime: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
