// SPDX-License-Identifier: GPL-3.0-only
#include <ee/r5900.hpp>

#include <cstdio>
#include <string>

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

static void check_disasm(ee::u32 raw, ee::u32 va, const char* expected) {
    ++g_checks;
    const ee::r5900::Instruction in = ee::r5900::decode(raw);
    const std::string text = ee::r5900::disassemble(in, va);
    if (text != expected) {
        std::printf("FAIL disasm 0x%08X: got \"%s\", want \"%s\"\n", raw, text.c_str(), expected);
        ++g_failures;
    }
}

int main() {
    using namespace ee::r5900;

    // Core MIPS (encodings hand-computed, cross-checked with PCSX2 tables).
    check_disasm(0x00000000, 0x100000, "nop");
    check_disasm(0x03E00008, 0x100000, "jr $ra");
    check_disasm(0x27BDFFF0, 0x100000, "addiu $sp, $sp, -16");
    check_disasm(0x11090010, 0x100000, "beq $t0, $t1, 0x00100044");
    check_disasm(0x0804080C, 0x100000, "j 0x00102030");
    check_disasm(0x0000000C, 0x100000, "syscall");
    check_disasm(0x00850018, 0x100000, "mult $a0, $a1");
    check_disasm(0x0009413C, 0x100000, "dsll32 $t0, $t1, 4");
    check_disasm(0x04990007, 0x100000, "mtsab $a0, 7");
    check_disasm(0x4C000000, 0x100000, ".word 0x4C000000");

    // COP0.
    check_disasm(0x42000018, 0x100000, "eret");
    check_disasm(0x42000038, 0x100000, "ei");
    check_disasm(0x42000039, 0x100000, "di");

    // FPU (COP1).
    check_disasm(0x46062080, 0x100000, "add.s $f2, $f4, $f6");
    check_disasm(0x4600208D, 0x100000, "trunc.w.s $f2, $f4");
    check_disasm(0x45000001, 0x100000, "bc1f 0x00100008");

    // MMI (PS2-specific 128-bit multimedia instructions).
    check_disasm(0x712A4008, 0x100000, "paddw $t0, $t1, $t2");
    check_disasm(0x70001030, 0x100000, "pmfhl.lw $v0");
    check_disasm(0x70851000, 0x100000, "madd $v0, $a0, $a1");
    check_disasm(0x7009413C, 0x100000, "psllw $t0, $t1, 4");

    // 128-bit load/store.
    check_disasm(0x7BA80010, 0x100000, "lq $t0, 16($sp)");
    check_disasm(0x7FA80020, 0x100000, "sq $t0, 32($sp)");

    // VU0 macro mode (COP2).
    check_disasm(0x4A0002FF, 0x100000, "vnop");
    check_disasm(0x48220800, 0x100000, "qmfc2 $v0, $vf1");
    check_disasm(0x48420800, 0x100000, "cfc2 $v0, Status");
    check_disasm(0x4BE62928, 0x100000, "vadd.xyzw $vf4, $vf5, $vf6");
    check_disasm(0x4A8313BC, 0x100000, "vdiv Q, $vf2.x, $vf3.y");

    // Flags and target computation.
    const Instruction beq = decode(0x11090010);
    CHECK(beq.op == Op::Beq);
    CHECK(beq.is_branch());
    CHECK(beq.has_delay_slot());
    CHECK(!beq.is_likely());
    CHECK(branch_target(beq, 0x100000) == 0x100044);

    const Instruction beql = decode(0x51090010);
    CHECK(beql.op == Op::Beql);
    CHECK(beql.is_likely());

    const Instruction jal = decode(0x0C04080C);
    CHECK(jal.op == Op::Jal);
    CHECK(jal.is_call());
    CHECK(jump_target(jal, 0x100000) == 0x102030);

    const Instruction jr = decode(0x03E00008);
    CHECK(jr.is_return());

    const Instruction jalr = decode(0x0100F809);
    CHECK(jalr.op == Op::Jalr);
    CHECK(jalr.is_call());

    const Instruction lq = decode(0x7BA80010);
    CHECK(lq.is_load());
    const Instruction sq = decode(0x7FA80020);
    CHECK(sq.is_store());
    const Instruction sc = decode(0x0000000C);
    CHECK(sc.is_syscall());
    const Instruction unk = decode(0x4C000000);
    CHECK(unk.is_unknown());

    std::printf("test_r5900: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
