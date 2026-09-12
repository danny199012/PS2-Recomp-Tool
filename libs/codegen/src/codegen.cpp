// SPDX-License-Identifier: GPL-3.0-only
#include <ee/codegen.hpp>
#include <ee/r5900.hpp>
#include <ee/text.hpp>
#include <ee/toml.hpp>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <set>

namespace ee::codegen {

std::optional<Config> Config::from_toml(std::string_view text, std::string* error) {
    auto doc = toml_parse(text, error);
    if (!doc)
        return std::nullopt;
    Config cfg;
    if (auto* v = doc->get("general", "stubs")) {
        if (auto* arr = v->as_string_array()) {
            for (const std::string& s : *arr) {
                const size_t at = s.find('@');
                if (at != std::string::npos) {
                    if (auto addr = parse_u32(s.substr(at + 1)))
                        cfg.bound_stubs[*addr] = s.substr(0, at);
                } else {
                    cfg.stubs.push_back(s);
                }
            }
        }
    }
    if (auto* v = doc->get("general", "skip"))
        if (auto* arr = v->as_string_array())
            cfg.skip = *arr;
    if (auto* v = doc->get("patches", "instructions"))
        if (auto* tbl = v->as_int_table())
            for (const auto& [key, val] : *tbl)
                if (auto addr = parse_u32(key))
                    cfg.instruction_patches[*addr] = u32(val);
    return cfg;
}

namespace {

std::string label(u32 addr) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "L_%08X", addr);
    return buf;
}

std::string fname(u32 addr) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "fn_%08X", addr);
    return buf;
}

struct Emitter {
    const elf::Image& img;
    const analysis::Result& res;
    const Config& cfg;
    const Options& opt;
    std::string out;

    void line(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        out += "    ";
        out += buf;
        out += '\n';
    }
    void raw(const char* s) {
        out += s;
        out += '\n';
    }
    void comment(const r5900::Instruction& in, u32 addr) {
        if (!opt.emit_comments)
            return;
        const std::string text = r5900::disassemble(in, addr);
        line("// %08X: %s", addr, text.c_str());
    }

    bool in_function(const analysis::Function& fn, u32 addr) const {
        return addr >= fn.start && addr < fn.end;
    }

    u32 word_at(u32 addr) const {
        auto w = img.read_u32(addr);
        if (!w)
            return 0;
        if (auto it = cfg.instruction_patches.find(addr); it != cfg.instruction_patches.end())
            return it->second;
        return *w;
    }

    // Emit one non-control-transfer instruction.
    void emit_simple(const r5900::Instruction& in, u32 addr);
    // Emit a branch/jump/call with its delay slot. Returns new addr (addr + 4).
    // `slot_is_target` is true when some other instruction branches to this
    // branch's delay-slot address; in that case the slot needs a label and the
    // branch must be structured so a jump-in executes the slot and falls
    // through without re-testing the (stale) condition flag.
    void emit_control(const r5900::Instruction& in, const r5900::Instruction& slot, u32 addr,
                      const analysis::Function& fn, bool slot_is_target);

    void emit_function(const analysis::Function& fn) {
        const std::string name = fname(fn.start);
        out += '\n';
        {
            char buf[256];
            std::snprintf(buf, sizeof buf, "// %s [%08X, %08X) source=%s",
                          fn.name.empty() ? "(unnamed)" : fn.name.c_str(), fn.start, fn.end,
                          analysis::func_source_name(fn.source));
            raw(buf);
        }
        out += "static void " + name + "([[maybe_unused]] EEContext& ctx) {\n";
        // Reusable branch-condition flag. Declared at function scope so that
        // gotos never skip over its initialization (MSVC C2362, since branch
        // conditions are evaluated before the delay slot and tested after).
        out += "    bool br = false;\n";

        const bool named_stub = !fn.name.empty() &&
                                std::find(cfg.stubs.begin(), cfg.stubs.end(), fn.name) != cfg.stubs.end();
        const bool named_skip = !fn.name.empty() &&
                                std::find(cfg.skip.begin(), cfg.skip.end(), fn.name) != cfg.skip.end();
        const auto bound = cfg.bound_stubs.find(fn.start);
        if (bound != cfg.bound_stubs.end()) {
            line("stub_call(ctx, \"%s\");", bound->second.c_str());
            line("return;");
            out += "}\n";
            return;
        }
        if (named_stub) {
            line("stub_call(ctx, \"%s\");", fn.name.c_str());
            line("return;");
            out += "}\n";
            return;
        }
        if (named_skip || fn.end <= fn.start) {
            line("return;");
            out += "}\n";
            return;
        }

        // Collect local labels (branch/jump/jump-table targets inside the function).
        std::set<u32> labels;
        for (u32 a = fn.start; a + 4 <= fn.end; a += 4) {
            const r5900::Instruction in = r5900::decode(word_at(a));
            if (in.is_branch() || in.op == r5900::Op::J || in.op == r5900::Op::Jal) {
                const u32 t = in.is_branch() ? r5900::branch_target(in, a) : r5900::jump_target(in, a);
                if (in_function(fn, t))
                    labels.insert(t);
            }
            if (in.op == r5900::Op::Jr && in.rs != 31)
                if (const analysis::JumpTable* jt = res.jump_table_for(a))
                    for (u32 t : jt->targets)
                        if (in_function(fn, t))
                            labels.insert(t);
        }

        bool terminated = false;
        for (u32 a = fn.start; a + 4 <= fn.end; a += 4) {
            if (labels.count(a)) {
                out += label(a) + ":\n";
                // Interrupt poll point: deliver queued INTC/alarms at basic
                // block heads so spin loops waiting on interrupt-driven flags
                // make progress (equivalent to hardware interrupt preemption,
                // but cooperative — handlers run at an instruction boundary
                // where all guest state lives in ctx). The label address also
                // feeds the load-address sampler's PC attribution (bring-up).
                char pb[64];
                std::snprintf(pb, sizeof pb, "    ee_poll_interrupts(ctx, 0x%08Xu);\n", a);
                out += pb;
            }
            const r5900::Instruction in = r5900::decode(word_at(a));
            // Note: the delay slot is decoded from the image even when it lies
            // outside [fn.end) — degenerate 4-byte functions (jump-table targets)
            // often start on a branch, and truncating emission broke them.
            if (in.has_delay_slot()) {
                const r5900::Instruction slot = r5900::decode(word_at(a + 4));
                comment(in, a);
                emit_control(in, slot, a, fn, labels.count(a + 4) != 0);
                a += 4; // consumed the delay slot
                terminated = in.is_return() || in.op == r5900::Op::J ||
                             in.op == r5900::Op::Jr || in.op == r5900::Op::Eret;
            } else {
                comment(in, a);
                emit_simple(in, a);
                terminated = false;
            }
        }
        if (!terminated) {
            line("return; // falls off the end (no explicit terminator)");
        }
        out += "}\n";
    }
};

} // namespace

namespace {

using r5900::Op;
using r5900::Instruction;

void Emitter::emit_control(const Instruction& in, const Instruction& slot, u32 addr,
                           const analysis::Function& fn, bool slot_is_target) {
    auto emit_slot = [&]() {
        comment(slot, addr + 4);
        if (slot.raw != 0) // skip nops
            emit_simple(slot, addr + 4);
    };
    auto tail_or_goto = [&](u32 t) {
        if (in_function(fn, t)) {
            line("goto %s;", label(t).c_str());
        } else {
            line("call(ctx, 0x%08Xu);", t);
            line("return; // tail call");
        }
    };

    switch (in.op) {
    case Op::J:
        if (slot_is_target) line("%s:", label(addr + 4).c_str());
        emit_slot();
        tail_or_goto(r5900::jump_target(in, addr));
        return;
    case Op::Jal:
        line("set32(ctx, 31, 0x%08Xu);", addr + 8);
        if (slot_is_target) line("%s:", label(addr + 4).c_str());
        emit_slot();
        line("call(ctx, 0x%08Xu);", r5900::jump_target(in, addr));
        return;
    case Op::Jalr:
        line("set64(ctx, %u, 0x%08Xu);", unsigned(in.rd), addr + 8);
        if (slot_is_target) line("%s:", label(addr + 4).c_str());
        emit_slot();
        if (opt.indirect_trace)
            line("ee_indirect_site(0x%08Xu, gpr32(ctx, %u));", addr, unsigned(in.rs));
        line("call(ctx, gpr32(ctx, %u));", unsigned(in.rs));
        return;
    case Op::Jr: {
        if (slot_is_target) line("%s:", label(addr + 4).c_str());
        emit_slot();
        if (in.rs == 31) {
            line("return;");
            return;
        }
        if (const analysis::JumpTable* jt = res.jump_table_for(addr)) {
            line("switch (gpr32(ctx, %u)) {", unsigned(in.rs));
            std::set<u32> emitted;
            for (u32 t : jt->targets)
                if (in_function(fn, t) && emitted.insert(t).second)
                    line("case 0x%08Xu: goto %s;", t, label(t).c_str());
            line("default: break;");
            line("}");
            if (opt.indirect_trace)
                line("ee_indirect_site(0x%08Xu, gpr32(ctx, %u));", addr, unsigned(in.rs));
            line("call(ctx, gpr32(ctx, %u)); // unresolved jump-table entry", unsigned(in.rs));
            line("return;");
            return;
        }
        if (opt.indirect_trace)
            line("ee_indirect_site(0x%08Xu, gpr32(ctx, %u));", addr, unsigned(in.rs));
        line("call(ctx, gpr32(ctx, %u)); // indirect tail call", unsigned(in.rs));
        line("return;");
        return;
    }
    default:
        break;
    }

    // Conditional branches.
    std::string cond;
    switch (in.op) {
    case Op::Beq:
    case Op::Beql:
        cond = "gpr(ctx, " + std::to_string(in.rs) + ") == gpr(ctx, " + std::to_string(in.rt) + ")";
        break;
    case Op::Bne:
    case Op::Bnel:
        cond = "gpr(ctx, " + std::to_string(in.rs) + ") != gpr(ctx, " + std::to_string(in.rt) + ")";
        break;
    case Op::Blez:
    case Op::Blezl:
        cond = "s64(gpr(ctx, " + std::to_string(in.rs) + ")) <= 0";
        break;
    case Op::Bgtz:
    case Op::Bgtzl:
        cond = "s64(gpr(ctx, " + std::to_string(in.rs) + ")) > 0";
        break;
    case Op::Bltz:
    case Op::Bltzl:
    case Op::Bltzal:
    case Op::Bltzall:
        cond = "s64(gpr(ctx, " + std::to_string(in.rs) + ")) < 0";
        break;
    case Op::Bgez:
    case Op::Bgezl:
    case Op::Bgezal:
    case Op::Bgezall:
        cond = "s64(gpr(ctx, " + std::to_string(in.rs) + ")) >= 0";
        break;
    case Op::Bc1f:
    case Op::Bc1fl:
        cond = "!fcc(ctx)";
        break;
    case Op::Bc1t:
    case Op::Bc1tl:
        cond = "fcc(ctx)";
        break;
    case Op::Bc0f:
    case Op::Bc0fl:
        // COP0 condition (Status.ETS-style) is never set in our HLE runtime,
        // so branch-if-false is always taken.
        cond = "true";
        break;
    case Op::Bc0t:
    case Op::Bc0tl:
        cond = "false";
        break;
    default:
        line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // unsupported branch", in.raw, addr);
        emit_slot();
        return;
    }

    // Branch-and-link sets ra whether or not the branch is taken.
    if (in.op == Op::Bltzal || in.op == Op::Bgezal || in.op == Op::Bltzall || in.op == Op::Bgezall)
        line("set32(ctx, 31, 0x%08Xu);", addr + 8);

    (void)addr;
    line("br = %s;", cond.c_str());

    const u32 t = r5900::branch_target(in, addr);
    const bool local = in_function(fn, t);
    if (slot_is_target) {
        // Another instruction branches to this branch's delay slot. Emit the
        // taken path (slot runs, then jump to the target), then the labeled
        // slot itself: a jump-in executes the slot and falls through to the
        // code after the branch without re-testing the (stale) `br` flag.
        line("if (br) {");
        emit_slot();
        if (local)
            line("goto %s;", label(t).c_str());
        else {
            line("call(ctx, 0x%08Xu);", t);
            line("return; // branch out of function");
        }
        line("}");
        line("%s:", label(addr + 4).c_str());
        emit_slot();
        return;
    }
    if (in.is_likely()) {
        line("if (br) {");
        emit_slot();
        if (local) {
            line("goto %s;", label(t).c_str());
        } else {
            line("call(ctx, 0x%08Xu);", t);
            line("return; // branch out of function");
        }
        line("}");
    } else {
        emit_slot();
        if (local) {
            line("if (br) goto %s;", label(t).c_str());
        } else {
            line("if (br) {");
            line("call(ctx, 0x%08Xu);", t);
            line("return; // branch out of function");
            line("}");
        }
    }
}

void Emitter::emit_simple(const Instruction& in, u32 addr) {
    const int rs = in.rs, rt = in.rt, rd = in.rd, sa = in.sa;
    const s32 simm = s32(s16(in.imm));
    char ea[64]; // effective address for loads/stores
    std::snprintf(ea, sizeof ea, "gpr32(ctx, %d) + (u32)(%d)", rs, simm);

    switch (in.op) {
    // --- core ALU (32-bit ops sign-extend into 64-bit on the EE) ---
    case Op::Sll: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) << %u);", rd, rt, sa); break;
    case Op::Srl: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) >> %u);", rd, rt, sa); break;
    case Op::Sra: if (rd) line("set32(ctx, %d, u32(s32(gpr32(ctx, %d)) >> %u));", rd, rt, sa); break;
    case Op::Sllv: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) << (gpr32(ctx, %d) & 31));", rd, rt, rs); break;
    case Op::Srlv: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) >> (gpr32(ctx, %d) & 31));", rd, rt, rs); break;
    case Op::Srav: if (rd) line("set32(ctx, %d, u32(s32(gpr32(ctx, %d)) >> (gpr32(ctx, %d) & 31)));", rd, rt, rs); break;
    case Op::Dsll: if (rd) line("set64(ctx, %d, gpr(ctx, %d) << %u);", rd, rt, sa); break;
    case Op::Dsrl: if (rd) line("set64(ctx, %d, gpr(ctx, %d) >> %u);", rd, rt, sa); break;
    case Op::Dsra: if (rd) line("set64(ctx, %d, u64(s64(gpr(ctx, %d)) >> %u));", rd, rt, sa); break;
    case Op::Dsll32: if (rd) line("set64(ctx, %d, gpr(ctx, %d) << %u);", rd, rt, sa + 32); break;
    case Op::Dsrl32: if (rd) line("set64(ctx, %d, gpr(ctx, %d) >> %u);", rd, rt, sa + 32); break;
    case Op::Dsra32: if (rd) line("set64(ctx, %d, u64(s64(gpr(ctx, %d)) >> %u));", rd, rt, sa + 32); break;
    case Op::Dsllv: if (rd) line("set64(ctx, %d, gpr(ctx, %d) << (gpr(ctx, %d) & 63));", rd, rt, rs); break;
    case Op::Dsrlv: if (rd) line("set64(ctx, %d, gpr(ctx, %d) >> (gpr(ctx, %d) & 63));", rd, rt, rs); break;
    case Op::Dsrav: if (rd) line("set64(ctx, %d, u64(s64(gpr(ctx, %d)) >> (gpr(ctx, %d) & 63)));", rd, rt, rs); break;
    case Op::Movz: if (rd) line("if (gpr(ctx, %d) == 0) set64(ctx, %d, gpr(ctx, %d));", rt, rd, rs); break;
    case Op::Movn: if (rd) line("if (gpr(ctx, %d) != 0) set64(ctx, %d, gpr(ctx, %d));", rt, rd, rs); break;
    case Op::Add:
    case Op::Addu: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) + gpr32(ctx, %d));", rd, rs, rt); break;
    case Op::Sub:
    case Op::Subu: if (rd) line("set32(ctx, %d, gpr32(ctx, %d) - gpr32(ctx, %d));", rd, rs, rt); break;
    case Op::And: if (rd) line("set64(ctx, %d, gpr(ctx, %d) & gpr(ctx, %d));", rd, rs, rt); break;
    case Op::Or: if (rd) line("set64(ctx, %d, gpr(ctx, %d) | gpr(ctx, %d));", rd, rs, rt); break;
    case Op::Xor: if (rd) line("set64(ctx, %d, gpr(ctx, %d) ^ gpr(ctx, %d));", rd, rs, rt); break;
    case Op::Nor: if (rd) line("set64(ctx, %d, ~(gpr(ctx, %d) | gpr(ctx, %d)));", rd, rs, rt); break;
    case Op::Slt: if (rd) line("set32(ctx, %d, s64(gpr(ctx, %d)) < s64(gpr(ctx, %d)) ? 1 : 0);", rd, rs, rt); break;
    case Op::Sltu: if (rd) line("set32(ctx, %d, gpr(ctx, %d) < gpr(ctx, %d) ? 1 : 0);", rd, rs, rt); break;
    case Op::Dadd: case Op::Daddu: if (rd) line("set64(ctx, %d, gpr(ctx, %d) + gpr(ctx, %d));", rd, rs, rt); break;
    case Op::Dsub: case Op::Dsubu: if (rd) line("set64(ctx, %d, gpr(ctx, %d) - gpr(ctx, %d));", rd, rs, rt); break;
    case Op::Addi:
    case Op::Addiu: if (rt) line("set32(ctx, %d, gpr32(ctx, %d) + (u32)(%d));", rt, rs, simm); break;
    case Op::Daddi:
    case Op::Daddiu: if (rt) line("set64(ctx, %d, gpr(ctx, %d) + (u64)(s64)(%d));", rt, rs, simm); break;
    case Op::Slti: if (rt) line("set32(ctx, %d, s64(gpr(ctx, %d)) < s64(%d) ? 1 : 0);", rt, rs, simm); break;
    case Op::Sltiu: if (rt) line("set32(ctx, %d, gpr(ctx, %d) < u64(s64(%d)) ? 1 : 0);", rt, rs, simm); break;
    case Op::Andi: if (rt) line("set64(ctx, %d, gpr(ctx, %d) & 0x%04Xull);", rt, rs, in.imm); break;
    case Op::Ori: if (rt) line("set64(ctx, %d, gpr(ctx, %d) | 0x%04Xull);", rt, rs, in.imm); break;
    case Op::Xori: if (rt) line("set64(ctx, %d, gpr(ctx, %d) ^ 0x%04Xull);", rt, rs, in.imm); break;
    case Op::Lui: if (rt) line("set32(ctx, %d, 0x%08Xu);", rt, u32(in.imm) << 16); break;
    // --- HI/LO, mult/div ---
    case Op::Mfhi: if (rd) line("set64(ctx, %d, ctx.hi.lo);", rd); break;
    case Op::Mthi: line("ctx.hi.lo = gpr(ctx, %d);", rs); break;
    case Op::Mflo: if (rd) line("set64(ctx, %d, ctx.lo.lo);", rd); break;
    case Op::Mtlo: line("ctx.lo.lo = gpr(ctx, %d);", rs); break;
    case Op::Mult: line("op_mult(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Multu: line("op_multu(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Div: line("op_div(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Divu: line("op_divu(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Madd: line("op_madd(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Maddu: line("op_maddu(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Mult1: line("op_mult1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Multu1: line("op_multu1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Div1: line("op_div1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Divu1: line("op_divu1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Madd1: line("op_madd1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Maddu1: line("op_maddu1(ctx, %d, %d, %d);", rs, rt, rd); break;
    case Op::Mfsa: if (rd) line("set64(ctx, %d, ctx.sa);", rd); break;
    case Op::Mtsa: line("ctx.sa = gpr32(ctx, %d);", rs); break;
    case Op::Mtsab: line("ctx.sa = (gpr32(ctx, %d) & 0xF) ^ 0x%X;", rs, unsigned(in.imm) & 0xF); break;
    case Op::Mtsah: line("ctx.sa = ((gpr32(ctx, %d) & 7) ^ %u) << 1;", rs, unsigned(in.imm) & 7); break;
    // --- loads ---
    case Op::Lb: if (rt) line("set32(ctx, %d, ld8s(ctx, %s));", rt, ea); break;
    case Op::Lbu: if (rt) line("set32(ctx, %d, ld8u(ctx, %s));", rt, ea); break;
    case Op::Lh: if (rt) line("set32(ctx, %d, ld16s(ctx, %s));", rt, ea); break;
    case Op::Lhu: if (rt) line("set32(ctx, %d, ld16u(ctx, %s));", rt, ea); break;
    case Op::Lw: if (rt) line("set32(ctx, %d, ld32(ctx, %s));", rt, ea); break;
    case Op::Lwu: if (rt) line("set64(ctx, %d, ld32(ctx, %s));", rt, ea); break;
    case Op::Ld: if (rt) line("set64(ctx, %d, ld64(ctx, %s));", rt, ea); break;
    case Op::Lq: if (rt) line("set128(ctx, %d, ld128(ctx, %s));", rt, ea); break;
    case Op::Ll: if (rt) line("set32(ctx, %d, ld32(ctx, %s));", rt, ea); break; // no LL/SC emulation
    case Op::Lld: if (rt) line("set64(ctx, %d, ld64(ctx, %s));", rt, ea); break;
    // --- stores ---
    case Op::Sb: line("st8(ctx, %s, gpr32(ctx, %d));", ea, rt); break;
    case Op::Sh: line("st16(ctx, %s, gpr32(ctx, %d));", ea, rt); break;
    case Op::Sw: line("st32(ctx, %s, gpr32(ctx, %d));", ea, rt); break;
    case Op::Sd: line("st64(ctx, %s, gpr(ctx, %d));", ea, rt); break;
    case Op::Sq: line("st128(ctx, %s, get128(ctx, %d));", ea, rt); break;
    case Op::Sc: line("st32(ctx, %s, gpr32(ctx, %d));", ea, rt); if (rt) line("set32(ctx, %d, 1);", rt); break;
    case Op::Scd: line("st64(ctx, %s, gpr(ctx, %d));", ea, rt); if (rt) line("set32(ctx, %d, 1);", rt); break;
    // --- unaligned ---
    case Op::Lwl: if (rt) line("op_lwl(ctx, %s, %d);", ea, rt); break;
    case Op::Lwr: if (rt) line("op_lwr(ctx, %s, %d);", ea, rt); break;
    case Op::Swl: line("op_swl(ctx, %s, %d);", ea, rt); break;
    case Op::Swr: line("op_swr(ctx, %s, %d);", ea, rt); break;
    case Op::Ldl: if (rt) line("op_ldl(ctx, %s, %d);", ea, rt); break;
    case Op::Ldr: if (rt) line("op_ldr(ctx, %s, %d);", ea, rt); break;
    case Op::Sdl: line("op_sdl(ctx, %s, %d);", ea, rt); break;
    case Op::Sdr: line("op_sdr(ctx, %s, %d);", ea, rt); break;
    // --- system ---
    case Op::Syscall: line("syscall(ctx, 0x%05Xu);", (in.raw >> 6) & 0xFFFFF); break;
    case Op::Break: line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // break", in.raw, addr); break;
    case Op::Sync: case Op::Pref: case Op::Cache: case Op::Ei: case Op::Di:
        break; // nops for recompilation purposes
    case Op::Tge: line("if (s64(gpr(ctx, %d)) >= s64(gpr(ctx, %d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Tgeu: line("if (gpr(ctx, %d) >= gpr(ctx, %d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Tlt: line("if (s64(gpr(ctx, %d)) < s64(gpr(ctx, %d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Tltu: line("if (gpr(ctx, %d) < gpr(ctx, %d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Teq: line("if (gpr(ctx, %d) == gpr(ctx, %d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Tne: line("if (gpr(ctx, %d) != gpr(ctx, %d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, rt, in.raw, addr); break;
    case Op::Tgei: line("if (s64(gpr(ctx, %d)) >= s64(%d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    case Op::Tgeiu: line("if (gpr(ctx, %d) >= u64(s64(%d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    case Op::Tlti: line("if (s64(gpr(ctx, %d)) < s64(%d)) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    case Op::Tltiu: line("if (gpr(ctx, %d) < u64(s64(%d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    case Op::Teqi: line("if (gpr(ctx, %d) == u64(s64(%d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    case Op::Tnei: line("if (gpr(ctx, %d) != u64(s64(%d))) unimplemented(ctx, 0x%08Xu, 0x%08Xu);", rs, simm, in.raw, addr); break;
    // --- MMI ---
    case Op::Mfhi1: if (rd) line("set64(ctx, %d, ctx.hi.hi);", rd); break;
    case Op::Mthi1: line("ctx.hi.hi = gpr(ctx, %d);", rs); break;
    case Op::Mflo1: if (rd) line("set64(ctx, %d, ctx.lo.hi);", rd); break;
    case Op::Mtlo1: line("ctx.lo.hi = gpr(ctx, %d);", rs); break;
    case Op::Pmthi: line("ctx.hi = get128(ctx, %d);", rs); break;
    case Op::Pmtlo: line("ctx.lo = get128(ctx, %d);", rs); break;
    case Op::Pmfhi: if (rd) line("set128(ctx, %d, ctx.hi);", rd); break;
    case Op::Pmflo: if (rd) line("set128(ctx, %d, ctx.lo);", rd); break;
    case Op::Plzcw: if (rd) line("op_plzcw(ctx, %d, %d);", rd, rs); break;
    case Op::Pmfhl: if (rd) line("op_pmfhl(ctx, %d, %d);", rd, unsigned(in.aux)); break;
    case Op::Pmthl: line("op_pmthl(ctx, %d, %d);", rs, unsigned(in.aux)); break;
    case Op::Psllh: if (rd) line("op_psllh(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psrlh: if (rd) line("op_psrlh(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psrah: if (rd) line("op_psrah(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psllw: if (rd) line("op_psllw(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psrlw: if (rd) line("op_psrlw(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psraw: if (rd) line("op_psraw(ctx, %d, %d, %d);", rd, rt, sa); break;
    case Op::Psllvw: if (rd) line("op_psllvw(ctx, %d, %d, %d);", rd, rt, rs); break;
    case Op::Psrlvw: if (rd) line("op_psrlvw(ctx, %d, %d, %d);", rd, rt, rs); break;
    case Op::Psravw: if (rd) line("op_psravw(ctx, %d, %d, %d);", rd, rt, rs); break;
    case Op::Paddw: if (rd) line("op_paddw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubw: if (rd) line("op_psubw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddh: if (rd) line("op_paddh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubh: if (rd) line("op_psubh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddb: if (rd) line("op_paddb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubb: if (rd) line("op_psubb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddsw: if (rd) line("op_paddsw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubsw: if (rd) line("op_psubsw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddsh: if (rd) line("op_paddsh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubsh: if (rd) line("op_psubsh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddsb: if (rd) line("op_paddsb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubsb: if (rd) line("op_psubsb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Padduw: if (rd) line("op_padduw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubuw: if (rd) line("op_psubuw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Padduh: if (rd) line("op_padduh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubuh: if (rd) line("op_psubuh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Paddub: if (rd) line("op_paddub(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Psubub: if (rd) line("op_psubub(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcgtw: if (rd) line("op_pcgtw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcgth: if (rd) line("op_pcgth(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcgtb: if (rd) line("op_pcgtb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pceqw: if (rd) line("op_pceqw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pceqh: if (rd) line("op_pceqh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pceqb: if (rd) line("op_pceqb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmaxw: if (rd) line("op_pmaxw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmaxh: if (rd) line("op_pmaxh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pminw: if (rd) line("op_pminw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pminh: if (rd) line("op_pminh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pand: if (rd) line("op_pand(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Por: if (rd) line("op_por(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pxor: if (rd) line("op_pxor(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pnor: if (rd) line("op_pnor(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextlw: if (rd) line("op_pextlw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextlh: if (rd) line("op_pextlh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextlb: if (rd) line("op_pextlb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextuw: if (rd) line("op_pextuw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextuh: if (rd) line("op_pextuh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pextub: if (rd) line("op_pextub(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Ppacw: if (rd) line("op_ppacw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Ppach: if (rd) line("op_ppach(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Ppacb: if (rd) line("op_ppacb(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pinth: if (rd) line("op_pinth(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pinteh: if (rd) line("op_pinteh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcpyld: if (rd) line("op_pcpyld(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcpyud: if (rd) line("op_pcpyud(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pcpyh: if (rd) line("op_pcpyh(ctx, %d, %d);", rd, rt); break;
    case Op::Pext5: if (rd) line("op_pext5(ctx, %d, %d);", rd, rt); break;
    case Op::Ppac5: if (rd) line("op_ppac5(ctx, %d, %d);", rd, rt); break;
    case Op::Qfsrv: if (rd) line("op_qfsrv(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pabsw: if (rd) line("op_pabsw(ctx, %d, %d);", rd, rt); break;
    case Op::Pabsh: if (rd) line("op_pabsh(ctx, %d, %d);", rd, rt); break;
    case Op::Padsbh: if (rd) line("op_padsbh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pexeh: if (rd) line("op_pexeh(ctx, %d, %d);", rd, rt); break;
    case Op::Prevh: if (rd) line("op_prevh(ctx, %d, %d);", rd, rt); break;
    case Op::Pexch: if (rd) line("op_pexch(ctx, %d, %d);", rd, rt); break;
    case Op::Pexew: if (rd) line("op_pexew(ctx, %d, %d);", rd, rt); break;
    case Op::Pexcw: if (rd) line("op_pexcw(ctx, %d, %d);", rd, rt); break;
    case Op::Prot3w: if (rd) line("op_prot3w(ctx, %d, %d);", rd, rt); break;
    // multiply/divide families write LO/HI even when rd == 0 (helpers check rd)
    case Op::Pmaddw: line("op_pmaddw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmsubw: line("op_pmsubw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmultw: line("op_pmultw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmultuw: line("op_pmultuw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmadduw: line("op_pmadduw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmulth: line("op_pmulth(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pdivw: line("op_pdivw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pdivbw: line("op_pdivbw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pdivuw: line("op_pdivuw(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmaddh: line("op_pmaddh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Phmadh: line("op_phmadh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Pmsubh: line("op_pmsubh(ctx, %d, %d, %d);", rd, rs, rt); break;
    case Op::Phmsbh: line("op_phmsbh(ctx, %d, %d, %d);", rd, rs, rt); break;
    // --- COP0 ---
    case Op::Mfc0: if (rt) line("set32(ctx, %d, ctx.cop0[%d]);", rt, rd); break;
    case Op::Mtc0: line("ctx.cop0[%d] = gpr32(ctx, %d);", rd, rt); break;
    case Op::Eret:
        // The game's exception stubs end with ERET after mtc0 ErrorPC. Since
        // we never take real exceptions, treat it as a plain return.
        line("return; // eret");
        break;
    case Op::Tlbr: case Op::Tlbwi: case Op::Tlbwr: case Op::Tlbp:
        // TLB maintenance is a no-op in the HLE kernel (no real TLB).
        break;
    // --- COP1 (FPU) ---  (fields: fs = rd, ft = rt, fd = sa)
    case Op::AddS: line("fset(ctx, %d, fget(ctx, %d) + fget(ctx, %d));", sa, rd, rt); break;
    case Op::SubS: line("fset(ctx, %d, fget(ctx, %d) - fget(ctx, %d));", sa, rd, rt); break;
    case Op::MulS: line("fset(ctx, %d, fget(ctx, %d) * fget(ctx, %d));", sa, rd, rt); break;
    case Op::DivS: line("fset(ctx, %d, fget(ctx, %d) / fget(ctx, %d));", sa, rd, rt); break;
    case Op::SqrtS: line("fset(ctx, %d, sqrtf(fget(ctx, %d)));", sa, rd); break;
    case Op::AbsS: line("fset(ctx, %d, fabsf(fget(ctx, %d)));", sa, rd); break;
    case Op::MovS: line("fset(ctx, %d, fget(ctx, %d));", sa, rd); break;
    case Op::NegS: line("fset(ctx, %d, -fget(ctx, %d));", sa, rd); break;
    case Op::RsqrtS: line("fset(ctx, %d, 1.0f / sqrtf(fget(ctx, %d)));", sa, rd); break;
    case Op::TruncwS: line("ctx.f[%d] = u32(s32(truncf(fget(ctx, %d))));", sa, rd); break;
    case Op::CvtsW: line("fset(ctx, %d, float(s32(ctx.f[%d])));", sa, rd); break;
    case Op::AddaS: line("facc_set(ctx, fget(ctx, %d) + fget(ctx, %d));", rd, rt); break;
    case Op::SubaS: line("facc_set(ctx, fget(ctx, %d) - fget(ctx, %d));", rd, rt); break;
    case Op::MulaS: line("facc_set(ctx, fget(ctx, %d) * fget(ctx, %d));", rd, rt); break;
    case Op::MaddS: line("fset(ctx, %d, facc_get(ctx) + fget(ctx, %d) * fget(ctx, %d));", sa, rd, rt); break;
    case Op::MsubS: line("fset(ctx, %d, facc_get(ctx) - fget(ctx, %d) * fget(ctx, %d));", sa, rd, rt); break;
    case Op::MaddaS: line("facc_set(ctx, facc_get(ctx) + fget(ctx, %d) * fget(ctx, %d));", rd, rt); break;
    case Op::MsubaS: line("facc_set(ctx, facc_get(ctx) - fget(ctx, %d) * fget(ctx, %d));", rd, rt); break;
    case Op::CfS: line("set_fcc(ctx, false);"); break;
    case Op::CeqS: line("set_fcc(ctx, fget(ctx, %d) == fget(ctx, %d));", rd, rt); break;
    case Op::ColtS: line("set_fcc(ctx, fget(ctx, %d) < fget(ctx, %d));", rd, rt); break;
    case Op::ColeS: line("set_fcc(ctx, fget(ctx, %d) <= fget(ctx, %d));", rd, rt); break;
    case Op::Mfc1: if (rt) line("set32(ctx, %d, ctx.f[%d]);", rt, rd); break;
    case Op::Mtc1: line("ctx.f[%d] = gpr32(ctx, %d);", rd, rt); break;
    case Op::Cfc1: if (rt) line("set32(ctx, %d, %d == 31 ? ctx.fcr31 : 0u);", rt, rd); break;
    case Op::Ctc1: if (rd == 31) line("ctx.fcr31 = gpr32(ctx, %d);", rt); break;
    case Op::Lwc1: line("ctx.f[%d] = ld32(ctx, %s);", rt, ea); break;
    case Op::Swc1: line("st32(ctx, %s, ctx.f[%d]);", ea, rt); break;
    case Op::Ldc1:
        if (rt < 31) {
            line("{ u64 v_ = ld64(ctx, %s); ctx.f[%d] = u32(v_); ctx.f[%d] = u32(v_ >> 32); }", ea, rt, rt + 1);
        } else {
            line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // ldc1 $f31", in.raw, addr);
        }
        break;
    case Op::Sdc1:
        if (rt < 31) {
            line("{ u64 v_ = u64(ctx.f[%d]) | (u64(ctx.f[%d]) << 32); st64(ctx, %s, v_); }", rt, rt + 1, ea);
        } else {
            line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // sdc1 $f31", in.raw, addr);
        }
        break;
    // --- COP2 (VU0 macro) ---
    case Op::Qmfc2: if (rt) line("set128(ctx, %d, ctx.vf[%d]);", rt, rd); break;
    case Op::Qmtc2: line("ctx.vf[%d] = get128(ctx, %d);", rd, rt); break;
    // --- VU0 macro mode (helpers in vu.cpp; fields: vu_fd/vu_fs/vu_ft, dest, fsf/ftf) ---
    case Op::Vadd: line("vu_arith(ctx, VuOp::Add, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vsub: line("vu_arith(ctx, VuOp::Sub, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmul: line("vu_arith(ctx, VuOp::Mul, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmax: line("vu_arith(ctx, VuOp::Max, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmini: line("vu_arith(ctx, VuOp::Min, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmadd: line("vu_arith(ctx, VuOp::Madd, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmsub: line("vu_arith(ctx, VuOp::Msub, %d, %d, %d, 0x%X, -1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vopmsub: line("vu_opmula(ctx, true, %d, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vopmula: line("vu_opmula(ctx, false, %d, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    // broadcast variants (x/y/z/w = bc 0/1/2/3)
    case Op::VaddX: line("vu_arith(ctx, VuOp::Add, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddY: line("vu_arith(ctx, VuOp::Add, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddZ: line("vu_arith(ctx, VuOp::Add, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddW: line("vu_arith(ctx, VuOp::Add, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubX: line("vu_arith(ctx, VuOp::Sub, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubY: line("vu_arith(ctx, VuOp::Sub, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubZ: line("vu_arith(ctx, VuOp::Sub, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubW: line("vu_arith(ctx, VuOp::Sub, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulX: line("vu_arith(ctx, VuOp::Mul, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulY: line("vu_arith(ctx, VuOp::Mul, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulZ: line("vu_arith(ctx, VuOp::Mul, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulW: line("vu_arith(ctx, VuOp::Mul, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddX: line("vu_arith(ctx, VuOp::Madd, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddY: line("vu_arith(ctx, VuOp::Madd, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddZ: line("vu_arith(ctx, VuOp::Madd, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddW: line("vu_arith(ctx, VuOp::Madd, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubX: line("vu_arith(ctx, VuOp::Msub, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubY: line("vu_arith(ctx, VuOp::Msub, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubZ: line("vu_arith(ctx, VuOp::Msub, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubW: line("vu_arith(ctx, VuOp::Msub, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaxX: line("vu_arith(ctx, VuOp::Max, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaxY: line("vu_arith(ctx, VuOp::Max, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaxZ: line("vu_arith(ctx, VuOp::Max, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaxW: line("vu_arith(ctx, VuOp::Max, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VminiX: line("vu_arith(ctx, VuOp::Min, %d, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VminiY: line("vu_arith(ctx, VuOp::Min, %d, %d, %d, 0x%X, 1);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VminiZ: line("vu_arith(ctx, VuOp::Min, %d, %d, %d, 0x%X, 2);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VminiW: line("vu_arith(ctx, VuOp::Min, %d, %d, %d, 0x%X, 3);", in.vu_fd, in.vu_fs, in.vu_ft, in.dest); break;
    // Q / I sources
    case Op::Vaddq: line("vu_arith_qi(ctx, VuOp::Add, %d, %d, 0x%X, true);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmaddq: line("vu_arith_qi(ctx, VuOp::Madd, %d, %d, 0x%X, true);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vsubq: line("vu_arith_qi(ctx, VuOp::Sub, %d, %d, 0x%X, true);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmsubq: line("vu_arith_qi(ctx, VuOp::Msub, %d, %d, 0x%X, true);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmulq: line("vu_arith_qi(ctx, VuOp::Mul, %d, %d, 0x%X, true);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vaddi: line("vu_arith_qi(ctx, VuOp::Add, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmaddi: line("vu_arith_qi(ctx, VuOp::Madd, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vsubi: line("vu_arith_qi(ctx, VuOp::Sub, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmsubi: line("vu_arith_qi(ctx, VuOp::Msub, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmuli: line("vu_arith_qi(ctx, VuOp::Mul, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmaxi: line("vu_arith_qi(ctx, VuOp::Max, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vminii: line("vu_arith_qi(ctx, VuOp::Min, %d, %d, 0x%X, false);", in.vu_fd, in.vu_fs, in.dest); break;
    // accumulator destinations
    case Op::Vadda: line("vu_arith(ctx, VuOp::Adda, 0, %d, %d, 0x%X, -1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vsuba: line("vu_arith(ctx, VuOp::Suba, 0, %d, %d, 0x%X, -1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmula: line("vu_arith(ctx, VuOp::Mula, 0, %d, %d, 0x%X, -1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmadda: line("vu_arith(ctx, VuOp::Madda, 0, %d, %d, 0x%X, -1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vmsuba: line("vu_arith(ctx, VuOp::Msuba, 0, %d, %d, 0x%X, -1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddaX: line("vu_arith(ctx, VuOp::Adda, 0, %d, %d, 0x%X, 0);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddaY: line("vu_arith(ctx, VuOp::Adda, 0, %d, %d, 0x%X, 1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddaZ: line("vu_arith(ctx, VuOp::Adda, 0, %d, %d, 0x%X, 2);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VaddaW: line("vu_arith(ctx, VuOp::Adda, 0, %d, %d, 0x%X, 3);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubaX: line("vu_arith(ctx, VuOp::Suba, 0, %d, %d, 0x%X, 0);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubaY: line("vu_arith(ctx, VuOp::Suba, 0, %d, %d, 0x%X, 1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubaZ: line("vu_arith(ctx, VuOp::Suba, 0, %d, %d, 0x%X, 2);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VsubaW: line("vu_arith(ctx, VuOp::Suba, 0, %d, %d, 0x%X, 3);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulaX: line("vu_arith(ctx, VuOp::Mula, 0, %d, %d, 0x%X, 0);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulaY: line("vu_arith(ctx, VuOp::Mula, 0, %d, %d, 0x%X, 1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulaZ: line("vu_arith(ctx, VuOp::Mula, 0, %d, %d, 0x%X, 2);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmulaW: line("vu_arith(ctx, VuOp::Mula, 0, %d, %d, 0x%X, 3);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddaX: line("vu_arith(ctx, VuOp::Madda, 0, %d, %d, 0x%X, 0);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddaY: line("vu_arith(ctx, VuOp::Madda, 0, %d, %d, 0x%X, 1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddaZ: line("vu_arith(ctx, VuOp::Madda, 0, %d, %d, 0x%X, 2);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmaddaW: line("vu_arith(ctx, VuOp::Madda, 0, %d, %d, 0x%X, 3);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubaX: line("vu_arith(ctx, VuOp::Msuba, 0, %d, %d, 0x%X, 0);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubaY: line("vu_arith(ctx, VuOp::Msuba, 0, %d, %d, 0x%X, 1);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubaZ: line("vu_arith(ctx, VuOp::Msuba, 0, %d, %d, 0x%X, 2);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::VmsubaW: line("vu_arith(ctx, VuOp::Msuba, 0, %d, %d, 0x%X, 3);", in.vu_fs, in.vu_ft, in.dest); break;
    case Op::Vaddaq: line("vu_arith_qi(ctx, VuOp::Adda, 0, %d, 0x%X, true);", in.vu_fs, in.dest); break;
    case Op::Vmaddaq: line("vu_arith_qi(ctx, VuOp::Madda, 0, %d, 0x%X, true);", in.vu_fs, in.dest); break;
    case Op::Vsubaq: line("vu_arith_qi(ctx, VuOp::Suba, 0, %d, 0x%X, true);", in.vu_fs, in.dest); break;
    case Op::Vmsubaq: line("vu_arith_qi(ctx, VuOp::Msuba, 0, %d, 0x%X, true);", in.vu_fs, in.dest); break;
    case Op::Vmulaq: line("vu_arith_qi(ctx, VuOp::Mula, 0, %d, 0x%X, true);", in.vu_fs, in.dest); break;
    case Op::Vaddai: line("vu_arith_qi(ctx, VuOp::Adda, 0, %d, 0x%X, false);", in.vu_fs, in.dest); break;
    case Op::Vmaddai: line("vu_arith_qi(ctx, VuOp::Madda, 0, %d, 0x%X, false);", in.vu_fs, in.dest); break;
    case Op::Vsubai: line("vu_arith_qi(ctx, VuOp::Suba, 0, %d, 0x%X, false);", in.vu_fs, in.dest); break;
    case Op::Vmsubai: line("vu_arith_qi(ctx, VuOp::Msuba, 0, %d, 0x%X, false);", in.vu_fs, in.dest); break;
    case Op::Vmulai: line("vu_arith_qi(ctx, VuOp::Mula, 0, %d, 0x%X, false);", in.vu_fs, in.dest); break;
    // conversions / moves / integer
    case Op::Vitof0: line("vu_itof(ctx, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vitof4: line("vu_itof(ctx, %d, %d, 0x%X, 4);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vitof12: line("vu_itof(ctx, %d, %d, 0x%X, 12);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vitof15: line("vu_itof(ctx, %d, %d, 0x%X, 15);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vftoi0: line("vu_ftoi(ctx, %d, %d, 0x%X, 0);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vftoi4: line("vu_ftoi(ctx, %d, %d, 0x%X, 4);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vftoi12: line("vu_ftoi(ctx, %d, %d, 0x%X, 12);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vftoi15: line("vu_ftoi(ctx, %d, %d, 0x%X, 15);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmove: line("vu_move(ctx, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vmr32: line("vu_mr32(ctx, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vabs: line("vu_abs(ctx, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Viadd: line("vu_iadd(ctx, %d, %d, %d);", in.vu_fd, in.vu_fs, in.vu_ft); break;
    case Op::Visub: line("vu_isub(ctx, %d, %d, %d);", in.vu_fd, in.vu_fs, in.vu_ft); break;
    case Op::Viaddi: { const s32 imm5 = s32(s8(in.sa << 3)) >> 3; line("vu_iaddi(ctx, %d, %d, %d);", in.vu_ft, in.vu_fs, imm5); break; }
    case Op::Viand: line("vu_iand(ctx, %d, %d, %d);", in.vu_fd, in.vu_fs, in.vu_ft); break;
    case Op::Vior: line("vu_ior(ctx, %d, %d, %d);", in.vu_fd, in.vu_fs, in.vu_ft); break;
    // divide / sqrt / Q
    case Op::Vdiv: line("vu_div(ctx, %d, %d, %d, %d);", in.vu_fs, in.fsf, in.vu_ft, in.ftf); break;
    case Op::Vsqrt: line("vu_sqrt(ctx, %d, %d);", in.vu_ft, in.ftf); break;
    case Op::Vrsqrt: line("vu_rsqrt(ctx, %d, %d, %d, %d);", in.vu_fs, in.fsf, in.vu_ft, in.ftf); break;
    case Op::Vwaitq: line("vu_waitq(ctx);"); break;
    // VU memory load/store
    case Op::Vlqi: line("vu_lqi(ctx, %d, %d, 0x%X);", in.vu_ft, in.vu_fs, in.dest); break;
    case Op::Vsqi: line("vu_sqi(ctx, %d, %d, 0x%X);", in.vu_ft, in.vu_fs, in.dest); break;
    case Op::Vlqd: line("vu_lqd(ctx, %d, %d, 0x%X);", in.vu_ft, in.vu_fs, in.dest); break;
    case Op::Vsqd: line("vu_sqd(ctx, %d, %d, 0x%X);", in.vu_ft, in.vu_fs, in.dest); break;
    case Op::Vilwr: line("vu_ilwr(ctx, %d, %d, %d);", in.vu_ft, in.vu_fs, in.ftf); break;
    case Op::Viswr: line("vu_iswr(ctx, %d, %d, %d);", in.vu_ft, in.vu_fs, in.ftf); break;
    // misc
    case Op::Vmtir: line("vu_mtir(ctx, %d, %d, %d);", in.vu_fd, in.vu_fs, in.fsf); break;
    case Op::Vmfir: line("vu_mfir(ctx, %d, %d, 0x%X);", in.vu_fd, in.vu_fs, in.dest); break;
    case Op::Vrnext: line("vu_rnext(ctx, %d, 0x%X);", in.vu_ft, in.dest); break;
    case Op::Vrget: line("vu_rget(ctx, %d, 0x%X);", in.vu_ft, in.dest); break;
    case Op::Vrinit: line("vu_rinit(ctx, %d, %d);", in.vu_fs, in.fsf); break;
    case Op::Vrxor: line("vu_rxor(ctx, %d, %d);", in.vu_fs, in.fsf); break;
    case Op::Vclipw: line("vu_clipw(ctx, %d, %d);", in.vu_fs, in.vu_ft); break;
    case Op::Vnop: break;
    case Op::Vcallms:
    case Op::Vcallmsr:
        line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // VU micro call (M7)", in.raw, addr);
        break;
    default:
        line("unimplemented(ctx, 0x%08Xu, 0x%08Xu); // %s", in.raw, addr, r5900::mnemonic(in.op));
        break;
    }
}

} // namespace

std::string emit_module(const elf::Image& image, const analysis::Result& res, const Config& cfg,
                        const Options& opt) {
    Emitter e{image, res, cfg, opt, {}};
    e.raw("// Auto-generated by ee-recomp (EERecomp). Do not edit.");
    e.raw("#include <cmath>");
    e.raw("#include \"ee/runtime.hpp\"");
    e.raw("");
    e.raw("using ee::rt::EEContext;");
    e.raw("using namespace ee;");
    e.raw("using namespace ee::rt;");
    e.raw("");
    for (const analysis::Function& fn : res.functions)
        e.emit_function(fn);
    e.raw("");
    e.raw("void register_functions(ee::rt::Runtime& rt) {");
    for (const analysis::Function& fn : res.functions)
        e.line("rt.add(0x%08Xu, %s);", fn.start, fname(fn.start).c_str());
    e.raw("}");
    return e.out;
}

} // namespace ee::codegen
