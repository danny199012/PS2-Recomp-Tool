// SPDX-License-Identifier: GPL-3.0-only
#include <ee/analysis.hpp>
#include <ee/json.hpp>
#include <ee/r5900.hpp>
#include <ee/text.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

namespace ee::analysis {

const char* func_source_name(FuncSource src) {
    switch (src) {
    case FuncSource::Entry: return "entry";
    case FuncSource::Symbol: return "symbol";
    case FuncSource::Import: return "import";
    case FuncSource::Call: return "call";
    case FuncSource::Prologue: return "prologue";
    }
    return "?";
}

const Function* Result::function_containing(u32 addr) const {
    if (functions.empty())
        return nullptr;
    size_t lo = 0;
    size_t hi = functions.size() - 1;
    const Function* best = nullptr;
    while (lo <= hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (functions[mid].start <= addr) {
            best = &functions[mid];
            lo = mid + 1;
        } else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }
    if (best && addr < best->end)
        return best;
    return nullptr;
}

const Function* Result::function_at(u32 addr) const {
    for (const Function& f : functions)
        if (f.start == addr)
            return &f;
    return nullptr;
}

const JumpTable* Result::jump_table_for(u32 jr_addr) const {
    for (const JumpTable& jt : jump_tables)
        if (jt.jr_addr == jr_addr)
            return &jt;
    return nullptr;
}

namespace {

bool is_exec_addr(const elf::Image& img, u32 addr) {
    for (const elf::Section& sec : img.sections())
        if (sec.executable() && addr >= sec.addr && addr < sec.addr + sec.size)
            return true;
    return false;
}

bool is_prologue(u32 raw) {
    // addiu/daddiu $sp, $sp, -imm (stack frame setup)
    if ((raw & 0xFFFF0000) == 0x27BD0000 && (raw & 0x8000))
        return true;
    if ((raw & 0xFFFF0000) == 0x67BD0000 && (raw & 0x8000))
        return true;
    return false;
}

struct Walker {
    const elf::Image& img;
    const Options& opt;
    Result& res;
    std::map<u32, u32> claimed; // code address -> owning function start
    std::set<u32> known;        // function starts

    bool add_function(u32 addr, FuncSource src) {
        if (!is_exec_addr(img, addr) || known.count(addr))
            return false;
        known.insert(addr);
        Function f;
        f.start = addr;
        f.source = src;
        if (auto it = res.names.find(addr); it != res.names.end())
            f.name = it->second;
        res.functions.push_back(f);
        return true;
    }

    // Try to resolve a jump table feeding `jr reg` at jr_addr, by tracking
    // constant base addresses through a lookback window (lui/addiu/ori/addu/lw).
    std::optional<JumpTable> resolve_jump_table(u32 jr_addr, u8 reg) {
        std::array<std::optional<u32>, 32> base{};
        const u32 begin = jr_addr >= 44 ? jr_addr - 44 : 0;
        std::optional<u32> table;
        for (u32 a = begin; a < jr_addr; a += 4) {
            auto w = img.read_u32(a);
            if (!w)
                continue;
            const r5900::Instruction in = r5900::decode(*w);
            auto clear = [&](u8 r) { base[r & 31] = std::nullopt; };
            switch (in.op) {
            case r5900::Op::Lui:
                base[in.rt] = u32(in.imm) << 16;
                break;
            case r5900::Op::Addiu:
            case r5900::Op::Addi:
                base[in.rt] = base[in.rs] ? std::optional<u32>(*base[in.rs] + u32(s32(s16(in.imm))))
                                         : std::nullopt;
                break;
            case r5900::Op::Ori:
                base[in.rt] = base[in.rs] ? std::optional<u32>(*base[in.rs] | in.imm) : std::nullopt;
                break;
            case r5900::Op::Addu:
            case r5900::Op::Daddu:
                if (base[in.rs] && base[in.rt])
                    base[in.rd] = *base[in.rs] + *base[in.rt];
                else if (base[in.rs])
                    base[in.rd] = base[in.rs]; // const base + variable index
                else if (base[in.rt])
                    base[in.rd] = base[in.rt];
                else
                    base[in.rd] = std::nullopt;
                break;
            case r5900::Op::Lw:
                if (in.rt == reg && base[in.rs])
                    table = *base[in.rs] + u32(s32(s16(in.imm)));
                clear(in.rt);
                break;
            default:
                switch (in.form) {
                case r5900::Form::RdRsRt:
                case r5900::Form::RdRtRs:
                case r5900::Form::RdRtSa:
                case r5900::Form::RdRs:
                case r5900::Form::RdRt:
                case r5900::Form::Rd:
                    clear(in.rd);
                    break;
                case r5900::Form::RtRsImm:
                case r5900::Form::RtRsImmU:
                case r5900::Form::RtImm:
                case r5900::Form::RtOffRs:
                    clear(in.rt);
                    break;
                default:
                    break;
                }
                break;
            }
        }
        if (!table)
            return std::nullopt;
        JumpTable jt;
        jt.jr_addr = jr_addr;
        jt.table_addr = *table;
        for (u32 i = 0; i < opt.max_table_entries; ++i) {
            auto entry = img.read_u32(*table + i * 4);
            if (!entry || !is_exec_addr(img, *entry))
                break;
            jt.targets.push_back(*entry);
        }
        if (jt.targets.empty())
            return std::nullopt;
        return jt;
    }

    void walk(size_t fi) {
        Function& fn = res.functions[fi];
        std::vector<u32> queue{fn.start};
        std::set<u32> seen;
        u32 max_end = fn.start;

        auto cover = [&](u32 addr) {
            seen.insert(addr);
            claimed[addr] = fn.start;
            max_end = std::max(max_end, addr + 4);
        };

        while (!queue.empty()) {
            u32 addr = queue.back();
            queue.pop_back();
            while (true) {
                if ((addr & 3) || seen.count(addr) || claimed.count(addr) || !is_exec_addr(img, addr))
                    break;
                auto word = img.read_u32(addr);
                if (!word)
                    break;
                const r5900::Instruction in = r5900::decode(*word);
                if (in.is_unknown())
                    break;
                cover(addr);
                const u32 slot = addr + 4;
                auto cover_slot = [&]() {
                    if (is_exec_addr(img, slot) && !seen.count(slot) && !claimed.count(slot))
                        cover(slot);
                };

                if (in.op == r5900::Op::J) {
                    const u32 t = r5900::jump_target(in, addr);
                    if (is_exec_addr(img, t)) {
                        if (known.count(t)) {
                            // tail call into a known function: don't absorb it
                        } else if (t < fn.start) {
                            add_function(t, FuncSource::Call); // backward jump out: tail call
                        } else {
                            queue.push_back(t); // local forward jump
                        }
                    }
                    cover_slot();
                    break;
                }
                if (in.op == r5900::Op::Jal) {
                    add_function(r5900::jump_target(in, addr), FuncSource::Call);
                    addr += 4; // delay slot next iteration, then the return path
                    continue;
                }
                if (in.op == r5900::Op::Jalr) {
                    res.unresolved_indirects.push_back(addr);
                    fn.has_indirect = true;
                    addr += 4;
                    continue;
                }
                if (in.op == r5900::Op::Jr) {
                    if (in.rs != 31) { // not a return
                        auto jt = resolve_jump_table(addr, in.rs);
                        if (jt) {
                            for (u32 t : jt->targets)
                                queue.push_back(t);
                            res.jump_tables.push_back(std::move(*jt));
                        } else {
                            res.unresolved_indirects.push_back(addr);
                            fn.has_indirect = true;
                        }
                    }
                    cover_slot();
                    break;
                }
                if (in.is_branch()) {
                    const u32 t = r5900::branch_target(in, addr);
                    if (is_exec_addr(img, t))
                        queue.push_back(t);
                    addr += 4; // delay slot next iteration, then the not-taken path
                    continue;
                }
                addr += 4;
            }
        }
        fn.end = max_end;
    }
};

} // namespace

Result analyze(const elf::Image& image, const Options& opt, const std::map<u32, std::string>& imports) {
    Result res;
    for (const elf::Symbol& sym : image.symbols())
        if (!sym.name.empty() && sym.value != 0)
            res.names[sym.value] = sym.name;
    for (const auto& [addr, name] : imports)
        res.names[addr] = name;

    Walker w{image, opt, res, {}, {}};

    if (image.entry() != 0)
        w.add_function(image.entry(), FuncSource::Entry);
    for (const elf::Symbol& sym : image.symbols())
        if (sym.is_function() && is_exec_addr(image, sym.value))
            w.add_function(sym.value, FuncSource::Symbol);
    for (const auto& [addr, name] : imports)
        w.add_function(addr, FuncSource::Import);

    auto process_pending = [&]() {
        for (size_t fi = 0; fi < res.functions.size(); ++fi)
            if (res.functions[fi].end == 0)
                w.walk(fi);
    };
    process_pending();

    if (opt.prologue_scan) {
        for (const elf::Section& sec : image.sections()) {
            if (!sec.executable())
                continue;
            for (u32 addr = sec.addr; addr + 4 <= sec.addr + sec.size; addr += 4) {
                if (w.claimed.count(addr))
                    continue;
                auto word = image.read_u32(addr);
                if (word && is_prologue(*word))
                    w.add_function(addr, FuncSource::Prologue);
            }
        }
        process_pending();
    }

    std::sort(res.functions.begin(), res.functions.end(),
              [](const Function& a, const Function& b) { return a.start < b.start; });
    std::sort(res.unresolved_indirects.begin(), res.unresolved_indirects.end());
    return res;
}

// --- Interchange formats -----------------------------------------------------

std::map<u32, std::string> import_csv(std::string_view text) {
    std::map<u32, std::string> out;
    for (const std::string& raw_line : split(text, '\n')) {
        const std::string line = trim(raw_line);
        if (line.empty() || line.front() == '#')
            continue;
        const auto fields = split(line, ',');
        if (fields.size() < 2)
            continue;
        std::optional<u32> addr;
        std::string name;
        // Accept either "address,name" or "name,address" column order.
        if (auto v = parse_u32(fields[0])) {
            addr = v;
            name = trim(fields[1]);
        } else if (auto v2 = parse_u32(fields[1])) {
            addr = v2;
            name = trim(fields[0]);
        }
        if (addr && !name.empty())
            out[*addr] = name;
    }
    return out;
}

std::map<u32, std::string> import_json(std::string_view text) {
    std::map<u32, std::string> out;
    auto doc = json_parse(text);
    if (!doc || !doc->as_array())
        return out;
    for (const JsonValue& item : *doc->as_array()) {
        const JsonValue* addr = item.find("address");
        const JsonValue* name = item.find("name");
        if (!addr || !name)
            continue;
        std::optional<u32> a;
        if (addr->is_string())
            a = parse_u32(addr->as_string());
        else if (addr->is_number())
            a = u32(addr->as_number());
        if (a)
            out[*a] = name->as_string();
    }
    return out;
}

std::string export_csv(const Result& res) {
    std::string out = "address,name,size,source\n";
    char line[512];
    for (const Function& f : res.functions) {
        std::string name = f.name;
        std::replace(name.begin(), name.end(), ',', '_');
        std::snprintf(line, sizeof line, "0x%08X,%s,%u,%s\n", f.start, name.c_str(),
                      unsigned(f.end - f.start), func_source_name(f.source));
        out += line;
    }
    return out;
}

std::string export_json(const Result& res) {
    std::string out = "[\n";
    char line[512];
    bool first = true;
    for (const Function& f : res.functions) {
        if (!first)
            out += ",\n";
        first = false;
        std::string name = f.name;
        std::replace(name.begin(), name.end(), '"', '\'');
        std::snprintf(line, sizeof line,
                      "  {\"address\": \"0x%08X\", \"name\": \"%s\", \"size\": %u, \"source\": \"%s\"}",
                      f.start, name.c_str(), unsigned(f.end - f.start), func_source_name(f.source));
        out += line;
    }
    out += "\n]\n";
    return out;
}

std::string export_toml(const Result& res, const elf::Image& image) {
    std::string out;
    out += "# Generated by ee-analyze (EERecomp).\n";
    out += "# Config schema compatible with PS2Recomp (ps2xAnalyzer / ps2xRecomp).\n\n";
    out += "[general]\n";
    out += "# Runtime stub handlers by name; also accepts handler@0xADDRESS to bind a\n";
    out += "# stripped function address directly to a runtime handler.\n";
    out += "stubs = []\n";
    out += "# Functions to skip (emit an empty wrapper).\n";
    out += "skip = []\n";
    out += "patch_cache = false\n\n";
    out += "[patches]\n";
    out += "# Raw instruction replacements by address, e.g.:\n";
    out += "# instructions = { \"0x00100000\" = 0x00000000 }\n\n";
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "# --- analysis summary ---\n"
                  "# functions: %zu (entry 0x%08X)\n"
                  "# jump tables: %zu\n"
                  "# unresolved indirect sites: %zu\n"
                  "# Function list: see the .functions.csv / .json next to this file.\n",
                  res.functions.size(), image.entry(), res.jump_tables.size(),
                  res.unresolved_indirects.size());
    out += buf;
    return out;
}

} // namespace ee::analysis
