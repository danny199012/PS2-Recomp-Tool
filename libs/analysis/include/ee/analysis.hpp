// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/elf.hpp>
#include <ee/types.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ee::analysis {

enum class FuncSource : u8 {
    Entry,    // ELF entry point
    Symbol,   // symbol table (STT_FUNC)
    Import,   // imported from CSV/JSON (Aura, Ghidra)
    Call,     // discovered as a jal target
    Prologue, // found by prologue pattern scan
};

const char* func_source_name(FuncSource src);

struct Function {
    u32 start = 0;
    u32 end = 0; // exclusive
    std::string name;
    FuncSource source = FuncSource::Call;
    bool has_indirect = false; // contains an unresolved indirect jump/call
};

struct JumpTable {
    u32 jr_addr = 0;    // address of the `jr` instruction
    u32 table_addr = 0; // guest address of the u32 table
    std::vector<u32> targets;
};

struct Options {
    bool prologue_scan = true;
    u32 max_table_entries = 256;
};

struct Result {
    std::vector<Function> functions; // sorted by start address
    std::vector<JumpTable> jump_tables;
    std::vector<u32> unresolved_indirects; // addresses of jr/jalr we could not resolve
    std::map<u32, std::string> names;      // all known address -> name mappings

    const Function* function_containing(u32 addr) const;
    const Function* function_at(u32 addr) const;
    const JumpTable* jump_table_for(u32 jr_addr) const;
};

// Run function discovery over an executable image. `imports` (address -> name,
// e.g. from Aura or Ghidra) seed both labels and analysis roots.
Result analyze(const elf::Image& image, const Options& opt = {},
               const std::map<u32, std::string>& imports = {});

// --- Interchange formats -----------------------------------------------------

// Import "address,name[,size]" CSV. Address may be hex (0x prefix) or decimal.
// Tolerates a header row and either column order (Aura / Ghidra style).
std::map<u32, std::string> import_csv(std::string_view text);

// Import Aura-style JSON: [{"address": "0x00100000" | number, "name": "main"}, ...]
std::map<u32, std::string> import_json(std::string_view text);

// Export discovered functions as CSV / JSON.
std::string export_csv(const Result& res);
std::string export_json(const Result& res);

// Export a PS2Recomp-compatible TOML config skeleton (general.stubs / skip /
// patches.instructions), with analysis results summarized as comments.
std::string export_toml(const Result& res, const elf::Image& image);

} // namespace ee::analysis
