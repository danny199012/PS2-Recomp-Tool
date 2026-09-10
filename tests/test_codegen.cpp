// SPDX-License-Identifier: GPL-3.0-only
#include "fixture.hpp"

#include <ee/codegen.hpp>

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

int main() {
    auto image = ee::elf::Image::load_bytes(ee::test::make_analysis_elf());
    CHECK(image.has_value());
    if (!image)
        return 1;

    const auto res = ee::analysis::analyze(*image);
    const ee::codegen::Config cfg{};
    const std::string m = ee::codegen::emit_module(*image, res, cfg);

    CHECK(m.find("static void fn_00100000([[maybe_unused]] EEContext& ctx)") != std::string::npos);
    CHECK(m.find("set32(ctx, 29, gpr32(ctx, 29) + (u32)(-32));") != std::string::npos);
    CHECK(m.find("set32(ctx, 31, 0x0010000Cu);") != std::string::npos); // jal link
    CHECK(m.find("call(ctx, 0x00100080u);") != std::string::npos);      // jal helper
    CHECK(m.find("switch (gpr32(ctx, 3))") != std::string::npos);       // jump table
    CHECK(m.find("case 0x00100024u: goto L_00100024;") != std::string::npos);
    CHECK(m.find("L_00100024:") != std::string::npos);
    CHECK(m.find("static void fn_00100080([[maybe_unused]] EEContext& ctx)") != std::string::npos);
    CHECK(m.find("rt.add(0x00100000u, fn_00100000);") != std::string::npos);
    CHECK(m.find("rt.add(0x00100080u, fn_00100080);") != std::string::npos);

    // PS2Recomp-compatible config parsing.
    const char* toml =
        "[general]\n"
        "stubs = [ \"printf\", \"sceCdRead@0x00123456\" ]\n"
        "skip = [ \"debug_menu\" ]\n"
        "patch_cache = false\n"
        "\n"
        "[patches]\n"
        "instructions = { \"0x00100000\" = 0x00000000 }\n";
    std::string err;
    auto parsed = ee::codegen::Config::from_toml(toml, &err);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->stubs.size() == 1 && parsed->stubs[0] == "printf");
        CHECK(parsed->skip.size() == 1 && parsed->skip[0] == "debug_menu");
        CHECK(parsed->bound_stubs.size() == 1 && parsed->bound_stubs.at(0x00123456) == "sceCdRead");
        CHECK(parsed->instruction_patches.size() == 1);
        CHECK(parsed->instruction_patches.at(0x00100000) == 0);
    }

    // Stub emission for a named function.
    ee::codegen::Config cfg2;
    cfg2.stubs.push_back("helper");
    const std::string m2 = ee::codegen::emit_module(*image, res, cfg2);
    CHECK(m2.find("stub_call(ctx, \"helper\");") != std::string::npos);

    // Instruction patch: replace main's first instruction with jr $ra.
    ee::codegen::Config cfg3;
    cfg3.instruction_patches[0x100000] = 0x03E00008;
    const std::string m3 = ee::codegen::emit_module(*image, res, cfg3);
    CHECK(m3.find("// 00100000: jr $ra") != std::string::npos);

    // Function-overlap bloat regression: without the non-overlap finalize pass,
    // main's [start, max_end) span contains the inner function, so emit_function()
    // emits the inner function's first instruction both inside main AND inside the
    // inner function (the ~450 MB blowup). With the fix, each instruction is
    // emitted at most once -- the inner function's prologue comment appears once.
    {
        auto ov = ee::elf::Image::load_bytes(ee::test::make_overlap_elf());
        CHECK(ov.has_value());
        if (ov) {
            const auto r = ee::analysis::analyze(*ov);
            const ee::codegen::Config c{};
            const std::string mm = ee::codegen::emit_module(*ov, r, c);
            const std::string inner_comment = "// 00100080:";
            size_t pos = 0, hits = 0;
            while ((pos = mm.find(inner_comment, pos)) != std::string::npos) {
                ++hits;
                pos += inner_comment.size();
            }
            CHECK(hits == 1); // exactly one emission of the inner function's entry
            // The rescued branch target is registered so it can be called.
            CHECK(mm.find("rt.add(0x001000C0u, fn_001000C0);") != std::string::npos);
        }
    }

    std::printf("test_codegen: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
