// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/analysis.hpp>
#include <ee/elf.hpp>
#include <ee/types.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ee::codegen {

// Recompiler configuration. Parsed from a PS2Recomp-compatible TOML subset:
//   [general] stubs = ["name", "handler@0xADDR"], skip = ["name"], ...
//   [patches] instructions = { "0xADDR" = 0xWORD }
struct Config {
    std::vector<std::string> stubs;         // function names replaced by runtime stub handlers
    std::vector<std::string> skip;          // function names emitted as empty wrappers
    std::map<u32, u32> instruction_patches; // address -> replacement instruction word
    std::map<u32, std::string> bound_stubs; // address -> handler name (from handler@0xADDR)

    static std::optional<Config> from_toml(std::string_view text, std::string* error = nullptr);
};

struct Options {
    bool emit_comments = true;  // annotate generated code with addresses + disassembly
    // Instrument unresolved indirect transfers (jr/jalr the analyzer could not
    // resolve to a function or jump table): generated code calls the runtime
    // helper ee_indirect_site(site, target) before the dynamic dispatch, which
    // histograms the taken targets (bring-up diagnostics; see runtime.hpp).
    bool indirect_trace = true;
};

// Emit a single C++ translation unit implementing all analyzed functions.
// The result registers itself via `void register_functions(ee::rt::Runtime&)`.
std::string emit_module(const elf::Image& image, const analysis::Result& res, const Config& cfg,
                        const Options& opt = {});

} // namespace ee::codegen
