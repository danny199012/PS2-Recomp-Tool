// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/analysis.hpp>
#include <ee/elf.hpp>
#include <ee/types.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    bool emit_comments = true; // annotate generated code with addresses + disassembly

    // Multi-file output: when true, emit_module_multi splits the generated code
    // across multiple .cpp files (each with at most functions_per_file functions)
    // plus a shared header with forward declarations. This is essential for large
    // stripped ELFs: a single multi-hundred-MB .cpp cannot be compiled by MSVC.
    bool multi_file = false;
    size_t functions_per_file = 500; // functions per .cpp in multi-file mode

    // Base name for multi-file output (e.g. "game" -> game.recomp.h, game.recomp.0.cpp, ...).
    // When empty, uses "recomp".
    std::string output_base;
};

// Emit a single C++ translation unit implementing all analyzed functions.
// The result registers itself via `void register_functions(ee::rt::Runtime&)`.
std::string emit_module(const elf::Image& image, const analysis::Result& res, const Config& cfg,
                        const Options& opt = {});

// Multi-file output: a shared header + multiple .cpp files. Each .cpp implements
// a subset of functions; the header declares them all with external linkage so
// cross-file calls resolve at link time. The last file contains register_functions.
//   .header        — "game.recomp.h": forward declarations + register_functions decl
//   .files[i]      — { "game.recomp.0.cpp", content } implementing a chunk
//   .files.back()  — { "game.recomp.register.cpp", register_functions impl }
struct MultiFileResult {
    std::string header;                              // header file content
    std::vector<std::pair<std::string, std::string>> files; // {filename, content}
};

MultiFileResult emit_module_multi(const elf::Image& image, const analysis::Result& res,
                                  const Config& cfg, const Options& opt = {});

} // namespace ee::codegen
