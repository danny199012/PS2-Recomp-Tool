// SPDX-License-Identifier: GPL-3.0-only
#include "fixture.hpp"

#include <ee/analysis.hpp>

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

    CHECK(res.functions.size() == 3);
    const ee::analysis::Function* main_fn = res.function_at(ee::test::kMainAddr);
    CHECK(main_fn != nullptr);
    if (main_fn) {
        CHECK(main_fn->name == "main");
        CHECK(main_fn->source == ee::analysis::FuncSource::Entry);
        CHECK(main_fn->end == 0x10004C);
    }
    const ee::analysis::Function* helper = res.function_at(ee::test::kHelperAddr);
    CHECK(helper != nullptr);
    if (helper)
        CHECK(helper->name == "helper");
    const ee::analysis::Function* hidden = res.function_at(ee::test::kHiddenAddr);
    CHECK(hidden != nullptr);
    if (hidden)
        CHECK(hidden->source == ee::analysis::FuncSource::Prologue);

    CHECK(res.jump_tables.size() == 1);
    if (!res.jump_tables.empty()) {
        CHECK(res.jump_tables[0].table_addr == ee::test::kTableAddr);
        CHECK(res.jump_tables[0].targets.size() == 4);
        CHECK(res.jump_tables[0].targets[0] == 0x100024);
        CHECK(res.jump_tables[0].targets[1] == 0x100030);
    }
    CHECK(res.unresolved_indirects.empty());
    CHECK(res.function_containing(0x100024) == main_fn);
    CHECK(res.function_containing(0x100080) == helper);

    // CSV round-trip (Ghidra-style interchange).
    const std::string csv = ee::analysis::export_csv(res);
    CHECK(csv.find("0x00100000,main") != std::string::npos);
    const auto names = ee::analysis::import_csv(csv);
    CHECK(names.size() == 2); // hidden has no name and is skipped
    CHECK(names.at(0x100000) == "main");
    CHECK(names.at(0x100080) == "helper");

    // JSON import (Aura-style interchange).
    const auto jn = ee::analysis::import_json(
        "[{\"address\": \"0x100000\", \"name\": \"main\"},"
        " {\"address\": 1048580, \"name\": \"sub_100004\"}]");
    CHECK(jn.size() == 2);
    CHECK(jn.at(0x100000) == "main");
    CHECK(jn.at(0x100004) == "sub_100004");

    // TOML config skeleton (PS2Recomp-compatible).
    const std::string toml = ee::analysis::export_toml(res, *image);
    CHECK(toml.find("[general]") != std::string::npos);
    CHECK(toml.find("stubs") != std::string::npos);
    CHECK(toml.find("[patches]") != std::string::npos);

    std::printf("test_analysis: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
