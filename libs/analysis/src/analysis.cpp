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

// Cooperative progress reporting / cancellation for long analyses.
struct ProgressCtl {
    ProgressFn fn;       // may be empty (no-op)
    u64 total = 0;       // total executable instruction count (denominator)
    u64 covered = 0;     // instructions covered by recursive walks
    u64 scanned = 0;     // instructions scanned by the prologue sweep
    bool cancelled = false;

    // Returns true to continue, false if cancellation was requested.
    bool report(double frac, const char* status) {
        if (!fn)
            return true;
        if (!fn(frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac), status)) {
            cancelled = true;
            return false;
        }
        return true;
    }
};

struct Walker {
    const elf::Image& img;
    const Options& opt;
    Result& res;
    std::map<u32, u32> claimed; // code address -> owning function start
    std::set<u32> known;        // function starts
    ProgressCtl* ctl = nullptr; // optional progress / cancel hook

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
            case r5900::Op::Sll:
            case r5900::Op::Sllv:
            case r5900::Op::Srl:
            case r5900::Op::Srlv:
            case r5900::Op::Sra:
            case r5900::Op::Srav:
                // Shifts appear in the standard jump-table index*4 chain:
                //   lui $base,hi; sll $idx,$idx,2; addiu $base,lo;
                //   addu $t,$idx,$base; lw $tgt,0($t); jr $tgt
                // The original code fell into `default:` and cleared `rd`,
                // which broke the chain whenever the shift's destination
                // is the same register the subsequent `lw` uses as its base
                // (the SLUS-21066 sub_0038B800 dispatch is exactly that
                // shape: `sll $v1,$a0,2; ... lw $v1,0($v1); jr $v1`).
                // The lookback window is small (11 instructions), so we
                // don't need to track shift semantics; we just need to
                // keep the base register alive across the shift so the
                // chain works. The destination may become a constant we
                // don't know the value of, but `addu`'s
                // `base[rs]||base[rt]` fallback already handles "one
                // constant + one unknown" correctly.
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
            // Stop on misaligned targets (real jump-table entries are
            // 4-byte aligned) and on an out-of-window jump: merged
            // dispatch tables (e.g. SLUS-21066 has two switch
            // dispatchers sharing one rodata page with their tables
            // adjacent) would otherwise bleed into the next dispatcher's
            // targets and claim its case bodies for the wrong function.
            // Real switch cases stay within a small window of each other;
            // a 64 KiB gap from the first valid target is the hard stop.
            // We compare against the first target (not the previous) so
            // legitimate non-monotonic tables (deduplicated case labels)
            // don't get truncated early.
            const u32 e = *entry;
            if ((e & 3) != 0) break;
            if (!jt.targets.empty()) {
                const u32 first = jt.targets.front();
                const u32 span = (e > first) ? (e - first) : (first - e);
                if (span > 0x10000u) break;
            }
            jt.targets.push_back(e);
        }
        if (jt.targets.empty())
            return std::nullopt;
        return jt;
    }

    void walk(size_t fi) {
        // IMPORTANT: `add_function()` push_backs into res.functions *during*
        // this walk, which may reallocate the vector and invalidate any
        // reference into it. Holding `Function& fn = res.functions[fi]`
        // here was undefined behaviour: after a realloc, `fn.start` was
        // read from freed memory (poisoning `claimed[addr] = fn.start`
        // with a garbage owner) and the final `fn.end = max_end` /
        // `fn.has_indirect` writes were lost. The garbage owners then
        // made the finalize rescue pass promote every claimed address
        // of the body to its own function, and the follow-up truncate()
        // cut each one to the next start -- shredding whole switch
        // bodies into 4-byte "functions" that return immediately,
        // skipping the shared epilogue (observed on SLUS-21066
        // sub_0038B800: $sp/$s0-$s7 were never restored, corrupting the
        // global-heap arena pointer). Copy the start out and write
        // results back by index instead.
        const u32 fn_start = res.functions[fi].start;
        std::vector<u32> queue{fn_start};
        std::set<u32> seen;
        u32 max_end = fn_start;
        bool has_indirect = false;

        auto cover = [&](u32 addr) {
            seen.insert(addr);
            claimed[addr] = fn_start;
            max_end = std::max(max_end, addr + 4);
            if (ctl) {
                ++ctl->covered;
                if ((ctl->covered & 0x3FF) == 0) // every 1024 instructions
                    ctl->report(0.5 * double(ctl->covered) / double(ctl->total + 1),
                                "discovering functions");
            }
        };

        while (!queue.empty() && !(ctl && ctl->cancelled)) {
            u32 addr = queue.back();
            queue.pop_back();
            while (!(ctl && ctl->cancelled)) {
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
                        } else if (t < fn_start) {
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
                    has_indirect = true;
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
                            has_indirect = true;
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
        // Write back by index: push_backs during the walk may have
        // reallocated the vector, but indices stay valid (nothing is ever
        // erased).
        res.functions[fi].end = max_end;
        res.functions[fi].has_indirect = has_indirect;
    }
};

} // namespace

Result analyze(const elf::Image& image, const Options& opt, const std::map<u32, std::string>& imports,
               ProgressFn progress) {
    Result res;
    for (const elf::Symbol& sym : image.symbols())
        if (!sym.name.empty() && sym.value != 0)
            res.names[sym.value] = sym.name;
    for (const auto& [addr, name] : imports)
        res.names[addr] = name;

    ProgressCtl ctl;
    ctl.fn = std::move(progress);
    for (const elf::Section& sec : image.sections())
        if (sec.executable())
            ctl.total += u64(sec.size) / 4;

    Walker w{image, opt, res, {}, {}, &ctl};
    ctl.report(0.0, "seeding functions");

    if (image.entry() != 0)
        w.add_function(image.entry(), FuncSource::Entry);
    for (const elf::Symbol& sym : image.symbols())
        if (sym.is_function() && is_exec_addr(image, sym.value))
            w.add_function(sym.value, FuncSource::Symbol);
    for (const auto& [addr, name] : imports)
        w.add_function(addr, FuncSource::Import);

    auto process_pending = [&]() {
        for (size_t fi = 0; fi < res.functions.size() && !ctl.cancelled; ++fi)
            if (res.functions[fi].end == 0)
                w.walk(fi);
    };
    process_pending();

    if (opt.prologue_scan && !ctl.cancelled) {
        ctl.report(0.5, "scanning for prologues");
        for (const elf::Section& sec : image.sections()) {
            if (!sec.executable() || ctl.cancelled)
                continue;
            for (u32 addr = sec.addr; addr + 4 <= sec.addr + sec.size; addr += 4) {
                ++ctl.scanned;
                if ((ctl.scanned & 0x3FF) == 0) {
                    ctl.report(0.5 + 0.5 * double(ctl.scanned) / double(ctl.total + 1),
                               "scanning for prologues");
                    if (ctl.cancelled)
                        break;
                }
                if (w.claimed.count(addr))
                    continue;
                auto word = image.read_u32(addr);
                if (word && is_prologue(*word))
                    w.add_function(addr, FuncSource::Prologue);
            }
            if (ctl.cancelled)
                break;
        }
        if (!ctl.cancelled)
            process_pending();
    }

    if (!ctl.cancelled)
        ctl.report(1.0, "finalizing");

    std::sort(res.functions.begin(), res.functions.end(),
              [](const Function& a, const Function& b) { return a.start < b.start; });
    std::sort(res.unresolved_indirects.begin(), res.unresolved_indirects.end());

    // Make function ranges non-overlapping so the code generator emits each
    // instruction at most once, and rescue any code that the change orphans.
    //
    // A recursive-descent walk sets `end` to the furthest *reachable* address
    // (max_end), while the `claimed` map only records reached instructions --
    // not the whole [start, end) span. The prologue sweep therefore finds
    // prologues inside the unreached "holes" of a walked function and adds
    // overlapping functions. Because emit_function() linearly emits every
    // address in [start, end), overlapping ranges make the recompiler emit the
    // same code several times: on a real stripped PS2 ELF this turned a 3.8 MB
    // binary into a ~450 MB .cpp.
    //
    // Step 1: remember each function's original (walk-reached) end.
    // Step 2: truncate each end to the next function's start (non-overlapping).
    //         Functions with end==0 (unwalked, e.g. analysis cancelled) are
    //         left untouched (0 is never > the next start).
    // Step 3: a branch/jump target that a walk reached (claimed) but which now
    //         falls outside its owner's truncated range -- typically a `jr`/branch
    //         that jumped *over* a prologue-scanned inner function -- would
    //         otherwise become an unregistered `call(ctx, addr)` at runtime.
    //         Rescue each such address as its own function, bounded by the
    //         original owner end so no new holes are introduced.
    // Step 4: re-sort and re-truncate so the rescued functions are ordered and
    //         non-overlapping too.
    std::map<u32, u32> orig_end; // function start -> pre-truncation end
    for (const Function& f : res.functions)
        if (f.end)
            orig_end[f.start] = f.end;

    auto truncate = [&]() {
        for (size_t i = 0; i + 1 < res.functions.size(); ++i)
            if (res.functions[i].end > res.functions[i + 1].start)
                res.functions[i].end = res.functions[i + 1].start;
    };
    truncate();

    {
        std::set<u32> starts;
        for (const Function& f : res.functions)
            starts.insert(f.start);
        std::map<u32, u32> end_of; // start -> truncated end
        for (const Function& f : res.functions)
            end_of[f.start] = f.end;

        bool added = false;
        for (const auto& [addr, owner] : w.claimed) {
            if (starts.count(addr))
                continue; // already a function entry
            auto it = end_of.find(owner);
            if (it != end_of.end() && addr < it->second)
                continue; // still inside its owner's (truncated) range
            Function f;
            f.start = addr;
            f.source = FuncSource::Call;
            auto oe = orig_end.find(owner);
            f.end = oe != orig_end.end() ? oe->second : 0; // bounded by owner reach
            res.functions.push_back(std::move(f));
            starts.insert(addr);
            added = true;
        }
        if (added) {
            std::sort(res.functions.begin(), res.functions.end(),
                      [](const Function& a, const Function& b) { return a.start < b.start; });
            truncate();
        }
    }
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
