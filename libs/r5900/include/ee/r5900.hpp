// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/types.hpp>

#include <string>

namespace ee::r5900 {

// Every instruction the R5900 (Emotion Engine) executes, in one X-macro list:
//   X(EnumName, "mnemonic")
// Single source of truth for the Op enum and the mnemonic table.
// Encoding references: EE Core User's Manual; cross-checked against PCSX2's
// R5900 opcode tables.
#define EE_R5900_OP_LIST(X)                                                          \
    X(Unknown, "???")                                                               \
    /* --- SPECIAL class (primary opcode 0x00, selected by funct) --- */            \
    X(Sll, "sll") X(Srl, "srl") X(Sra, "sra") X(Sllv, "sllv") X(Srlv, "srlv")        \
    X(Srav, "srav") X(Jr, "jr") X(Jalr, "jalr") X(Movz, "movz") X(Movn, "movn")      \
    X(Syscall, "syscall") X(Break, "break") X(Sync, "sync") X(Mfhi, "mfhi")          \
    X(Mthi, "mthi") X(Mflo, "mflo") X(Mtlo, "mtlo") X(Dsllv, "dsllv")                \
    X(Dsrlv, "dsrlv") X(Dsrav, "dsrav") X(Mult, "mult") X(Multu, "multu")            \
    X(Div, "div") X(Divu, "divu") X(Add, "add") X(Addu, "addu") X(Sub, "sub")        \
    X(Subu, "subu") X(And, "and") X(Or, "or") X(Xor, "xor") X(Nor, "nor")            \
    X(Mfsa, "mfsa") X(Mtsa, "mtsa") X(Slt, "slt") X(Sltu, "sltu")                    \
    X(Dadd, "dadd") X(Daddu, "daddu") X(Dsub, "dsub") X(Dsubu, "dsubu")              \
    X(Tge, "tge") X(Tgeu, "tgeu") X(Tlt, "tlt") X(Tltu, "tltu") X(Teq, "teq")        \
    X(Tne, "tne") X(Dsll, "dsll") X(Dsrl, "dsrl") X(Dsra, "dsra")                    \
    X(Dsll32, "dsll32") X(Dsrl32, "dsrl32") X(Dsra32, "dsra32")                      \
    /* --- REGIMM class (primary opcode 0x01, selected by rt) --- */                \
    X(Bltz, "bltz") X(Bgez, "bgez") X(Bltzl, "bltzl") X(Bgezl, "bgezl")              \
    X(Tgei, "tgei") X(Tgeiu, "tgeiu") X(Tlti, "tlti") X(Tltiu, "tltiu")              \
    X(Teqi, "teqi") X(Tnei, "tnei") X(Bltzal, "bltzal") X(Bgezal, "bgezal")          \
    X(Bltzall, "bltzall") X(Bgezall, "bgezall") X(Mtsab, "mtsab") X(Mtsah, "mtsah")  \
    /* --- Primary opcodes --- */                                                   \
    X(J, "j") X(Jal, "jal") X(Beq, "beq") X(Bne, "bne") X(Blez, "blez")              \
    X(Bgtz, "bgtz") X(Addi, "addi") X(Addiu, "addiu") X(Slti, "slti")                \
    X(Sltiu, "sltiu") X(Andi, "andi") X(Ori, "ori") X(Xori, "xori") X(Lui, "lui")    \
    X(Beql, "beql") X(Bnel, "bnel") X(Blezl, "blezl") X(Bgtzl, "bgtzl")              \
    X(Daddi, "daddi") X(Daddiu, "daddiu") X(Ldl, "ldl") X(Ldr, "ldr")                \
    X(Lq, "lq") X(Sq, "sq") X(Lb, "lb") X(Lh, "lh") X(Lwl, "lwl") X(Lw, "lw")        \
    X(Lbu, "lbu") X(Lhu, "lhu") X(Lwr, "lwr") X(Lwu, "lwu") X(Sb, "sb")              \
    X(Sh, "sh") X(Swl, "swl") X(Sw, "sw") X(Sdl, "sdl") X(Sdr, "sdr")                \
    X(Swr, "swr") X(Cache, "cache") X(Ll, "ll") X(Lwc1, "lwc1") X(Pref, "pref")      \
    X(Lld, "lld") X(Ldc1, "ldc1") X(Lqc2, "lqc2") X(Ld, "ld") X(Sc, "sc")            \
    X(Swc1, "swc1") X(Scd, "scd") X(Sdc1, "sdc1") X(Sqc2, "sqc2") X(Sd, "sd")        \
    /* --- MMI class (primary opcode 0x1C, selected by funct) --- */                \
    X(Madd, "madd") X(Maddu, "maddu") X(Plzcw, "plzcw") X(Mfhi1, "mfhi1")           \
    X(Mthi1, "mthi1") X(Mflo1, "mflo1") X(Mtlo1, "mtlo1") X(Mult1, "mult1")         \
    X(Multu1, "multu1") X(Div1, "div1") X(Divu1, "divu1") X(Madd1, "madd1")         \
    X(Maddu1, "maddu1") X(Pmfhl, "pmfhl") X(Pmthl, "pmthl") X(Psllh, "psllh")       \
    X(Psrlh, "psrlh") X(Psrah, "psrah") X(Psllw, "psllw") X(Psrlw, "psrlw")         \
    X(Psraw, "psraw")                                                               \
    /* --- MMI0 subclass (selected by sa field) --- */                              \
    X(Paddw, "paddw") X(Psubw, "psubw") X(Pcgtw, "pcgtw") X(Pmaxw, "pmaxw")         \
    X(Paddh, "paddh") X(Psubh, "psubh") X(Pcgth, "pcgth") X(Pmaxh, "pmaxh")         \
    X(Paddb, "paddb") X(Psubb, "psubb") X(Pcgtb, "pcgtb") X(Paddsw, "paddsw")       \
    X(Psubsw, "psubsw") X(Pextlw, "pextlw") X(Ppacw, "ppacw") X(Paddsh, "paddsh")   \
    X(Psubsh, "psubsh") X(Pextlh, "pextlh") X(Ppach, "ppach") X(Paddsb, "paddsb")   \
    X(Psubsb, "psubsb") X(Pextlb, "pextlb") X(Ppacb, "ppacb") X(Pext5, "pext5")     \
    X(Ppac5, "ppac5")                                                               \
    /* --- MMI1 subclass (selected by sa field) --- */                              \
    X(Pabsw, "pabsw") X(Pceqw, "pceqw") X(Pminw, "pminw") X(Padsbh, "padsbh")       \
    X(Pabsh, "pabsh") X(Pceqh, "pceqh") X(Pminh, "pminh") X(Pceqb, "pceqb")         \
    X(Padduw, "padduw") X(Psubuw, "psubuw") X(Pextuw, "pextuw")                     \
    X(Padduh, "padduh") X(Psubuh, "psubuh") X(Pextuh, "pextuh")                     \
    X(Paddub, "paddub") X(Psubub, "psubub") X(Pextub, "pextub") X(Qfsrv, "qfsrv")   \
    /* --- MMI2 subclass (selected by sa field) --- */                              \
    X(Pmaddw, "pmaddw") X(Psllvw, "psllvw") X(Psrlvw, "psrlvw")                     \
    X(Pmsubw, "pmsubw") X(Pmfhi, "pmfhi") X(Pmflo, "pmflo") X(Pinth, "pinth")       \
    X(Pmultw, "pmultw") X(Pdivw, "pdivw") X(Pcpyld, "pcpyld")                       \
    X(Pmaddh, "pmaddh") X(Phmadh, "phmadh") X(Pand, "pand") X(Pxor, "pxor")         \
    X(Pmsubh, "pmsubh") X(Phmsbh, "phmsbh") X(Pexeh, "pexeh") X(Prevh, "prevh")     \
    X(Pmulth, "pmulth") X(Pdivbw, "pdivbw") X(Pexew, "pexew") X(Prot3w, "prot3w")   \
    /* --- MMI3 subclass (selected by sa field) --- */                              \
    X(Pmadduw, "pmadduw") X(Psravw, "psravw") X(Pmthi, "pmthi") X(Pmtlo, "pmtlo")   \
    X(Pinteh, "pinteh") X(Pmultuw, "pmultuw") X(Pdivuw, "pdivuw")                   \
    X(Pcpyud, "pcpyud") X(Por, "por") X(Pnor, "pnor") X(Pexch, "pexch")             \
    X(Pcpyh, "pcpyh") X(Pexcw, "pexcw")                                             \
    /* --- COP0 --- */                                                              \
    X(Mfc0, "mfc0") X(Mtc0, "mtc0") X(Bc0f, "bc0f") X(Bc0t, "bc0t")                 \
    X(Bc0fl, "bc0fl") X(Bc0tl, "bc0tl") X(Tlbr, "tlbr") X(Tlbwi, "tlbwi")           \
    X(Tlbwr, "tlbwr") X(Tlbp, "tlbp") X(Eret, "eret") X(Ei, "ei") X(Di, "di")       \
    /* --- COP1 (FPU) --- */                                                        \
    X(Mfc1, "mfc1") X(Cfc1, "cfc1") X(Mtc1, "mtc1") X(Ctc1, "ctc1")                 \
    X(Bc1f, "bc1f") X(Bc1t, "bc1t") X(Bc1fl, "bc1fl") X(Bc1tl, "bc1tl")             \
    X(AddS, "add.s") X(SubS, "sub.s") X(MulS, "mul.s") X(DivS, "div.s")             \
    X(SqrtS, "sqrt.s") X(AbsS, "abs.s") X(MovS, "mov.s") X(NegS, "neg.s")           \
    X(TruncwS, "trunc.w.s") X(RsqrtS, "rsqrt.s") X(AddaS, "adda.s")                 \
    X(SubaS, "suba.s") X(MulaS, "mula.s") X(MaddS, "madd.s") X(MsubS, "msub.s")     \
    X(MaddaS, "madda.s") X(MsubaS, "msuba.s") X(CvtsW, "cvt.s.w")                   \
    X(CfS, "c.f.s") X(CeqS, "c.eq.s") X(ColtS, "c.olt.s") X(ColeS, "c.ole.s")       \
    /* --- COP2 register moves / branches --- */                                    \
    X(Qmfc2, "qmfc2") X(Cfc2, "cfc2") X(Qmtc2, "qmtc2") X(Ctc2, "ctc2")             \
    X(Bc2f, "bc2f") X(Bc2t, "bc2t") X(Bc2fl, "bc2fl") X(Bc2tl, "bc2tl")             \
    /* --- VU0 macro SPECIAL1 (selected by funct) --- */                            \
    X(VaddX, "vaddx") X(VaddY, "vaddy") X(VaddZ, "vaddz") X(VaddW, "vaddw")         \
    X(VsubX, "vsubx") X(VsubY, "vsuby") X(VsubZ, "vsubz") X(VsubW, "vsubw")         \
    X(VmaddX, "vmaddx") X(VmaddY, "vmaddy") X(VmaddZ, "vmaddz")                     \
    X(VmaddW, "vmaddw") X(VmsubX, "vmsubx") X(VmsubY, "vmsuby")                     \
    X(VmsubZ, "vmsubz") X(VmsubW, "vmsubw") X(VmaxX, "vmaxx") X(VmaxY, "vmaxy")     \
    X(VmaxZ, "vmaxz") X(VmaxW, "vmaxw") X(VminiX, "vminix") X(VminiY, "vminiy")     \
    X(VminiZ, "vminiz") X(VminiW, "vminiw") X(VmulX, "vmulx") X(VmulY, "vmuly")     \
    X(VmulZ, "vmulz") X(VmulW, "vmulw") X(Vmulq, "vmulq") X(Vmaxi, "vmaxi")         \
    X(Vmuli, "vmuli") X(Vminii, "vminii") X(Vaddq, "vaddq") X(Vmaddq, "vmaddq")     \
    X(Vaddi, "vaddi") X(Vmaddi, "vmaddi") X(Vsubq, "vsubq") X(Vmsubq, "vmsubq")     \
    X(Vsubi, "vsubi") X(Vmsubi, "vmsubi") X(Vadd, "vadd") X(Vmadd, "vmadd")         \
    X(Vmul, "vmul") X(Vmax, "vmax") X(Vsub, "vsub") X(Vmsub, "vmsub")               \
    X(Vopmsub, "vopmsub") X(Vmini, "vmini") X(Viadd, "viadd") X(Visub, "visub")     \
    X(Viaddi, "viaddi") X(Viand, "viand") X(Vior, "vior") X(Vcallms, "vcallms")     \
    X(Vcallmsr, "vcallmsr")                                                         \
    /* --- VU0 macro SPECIAL2 (funct 0x3C-0x3F, extended index) --- */              \
    X(VaddaX, "vaddax") X(VaddaY, "vadday") X(VaddaZ, "vaddaz")                     \
    X(VaddaW, "vaddaw") X(VsubaX, "vsubax") X(VsubaY, "vsubay")                     \
    X(VsubaZ, "vsubaz") X(VsubaW, "vsubaw") X(VmaddaX, "vmaddax")                   \
    X(VmaddaY, "vmadday") X(VmaddaZ, "vmaddaz") X(VmaddaW, "vmaddaw")               \
    X(VmsubaX, "vmsubax") X(VmsubaY, "vmsubay") X(VmsubaZ, "vmsubaz")               \
    X(VmsubaW, "vmsubaw") X(Vitof0, "vitof0") X(Vitof4, "vitof4")                   \
    X(Vitof12, "vitof12") X(Vitof15, "vitof15") X(Vftoi0, "vftoi0")                 \
    X(Vftoi4, "vftoi4") X(Vftoi12, "vftoi12") X(Vftoi15, "vftoi15")                 \
    X(VmulaX, "vmulax") X(VmulaY, "vmulay") X(VmulaZ, "vmulaz")                     \
    X(VmulaW, "vmulaw") X(Vmulaq, "vmulaq") X(Vabs, "vabs") X(Vmulai, "vmulai")     \
    X(Vclipw, "vclipw") X(Vaddaq, "vaddaq") X(Vmaddaq, "vmaddaq")                   \
    X(Vaddai, "vaddai") X(Vmaddai, "vmaddai") X(Vsubaq, "vsubaq")                   \
    X(Vmsubaq, "vmsubaq") X(Vsubai, "vsubai") X(Vmsubai, "vmsubai")                 \
    X(Vadda, "vadda") X(Vmadda, "vmadda") X(Vmula, "vmula") X(Vsuba, "vsuba")       \
    X(Vmsuba, "vmsuba") X(Vopmula, "vopmula") X(Vnop, "vnop") X(Vmove, "vmove")     \
    X(Vmr32, "vmr32") X(Vlqi, "vlqi") X(Vsqi, "vsqi") X(Vlqd, "vlqd")               \
    X(Vsqd, "vsqd") X(Vdiv, "vdiv") X(Vsqrt, "vsqrt") X(Vrsqrt, "vrsqrt")           \
    X(Vwaitq, "vwaitq") X(Vmtir, "vmtir") X(Vmfir, "vmfir") X(Vilwr, "vilwr")       \
    X(Viswr, "viswr") X(Vrnext, "vrnext") X(Vrget, "vrget") X(Vrinit, "vrinit")     \
    X(Vrxor, "vrxor")

enum class Op : u16 {
#define EE_R5900_OP_ENUM(name, text) name,
    EE_R5900_OP_LIST(EE_R5900_OP_ENUM)
#undef EE_R5900_OP_ENUM
    Count
};

// Operand layout; drives disassembler formatting (and later, codegen).
enum class Form : u8 {
    None,           // no operands
    RdRsRt,         // op rd, rs, rt
    RdRsRtHideRd0,  // mult/div family: "op rs, rt" when rd == 0
    RdRtSa,         // op rd, rt, sa
    RdRtRs,         // op rd, rt, rs (variable shifts)
    RdRs,           // op rd, rs
    RdRt,           // op rd, rt
    Rd,             // op rd
    Rs,             // op rs
    RsRt,           // op rs, rt
    RtRsImm,        // op rt, rs, imm (signed)
    RtRsImmU,       // op rt, rs, 0ximm (unsigned hex)
    RtImm,          // op rt, imm
    RtOffRs,        // op rt, off(rs)
    FtOffRs,        // op $fN, off(rs)   (COP1 load/store)
    VtOffRs,        // op $vfN, off(rs)  (COP2 load/store)
    BranchRsRt,     // op rs, rt, target
    BranchRs,       // op rs, target
    Jump,           // op target
    Jr,             // op rs
    Jalr,           // op rd, rs
    Syscall,        // syscall
    Break,          // break
    TrapRsRt,       // op rs, rt
    TrapRsImm,      // op rs, imm
    MtsabLike,      // op rs, imm
    CacheLike,      // op rt, off(rs)
    PrefLike,       // op rt, off(rs)
    Mfc0, Mtc0,     // op rt, cop0reg
    BranchCop,      // bcNf/bcNt ... target
    Mfc1, Cfc1, Mtc1, Ctc1,
    Fpu3,           // op.s fd, fs, ft   (fs=rd field, ft=rt field, fd=sa field)
    Fpu2,           // op.s fd, fs
    Fpu1,           // mov/neg/abs.s fd, fs
    FpuCmp,         // c.cond.s fs, ft
    FpuAcc2,        // op.s fs, ft (accumulator dest)
    FpuCvt,         // cvt/trunc fd, fs
    Qmfc2, Qmtc2,   // op rt, $vfN
    Cfc2, Ctc2,     // op rt, vu-control-reg
    Vu3,            // op.dest vfd, vfs, vft
    VuQ2,           // op.dest vfd, vfs, Q
    VuI2,           // op.dest vfd, vfs, I
    VuAcc2,         // op.dest vfs, vft
    VuAccQ,         // op.dest vfs, Q
    VuAccI,         // op.dest vfs, I
    Vu2,            // op.dest vfd, vfs
    VuClipw,        // vclipw.xyz vfs, vft
    VuQ,            // op Q, vfs.fsf, vft.ftf
    VuSqrt,         // vsqrt Q, vft.ftf
    VuI3,           // op vid, vis, vit
    VuIaddi,        // viaddi vit, vis, imm5
    VuCallms,       // vcallms imm28
    VuCallmsr,      // vcallmsr vis
    VuMtir,         // vmtir vid, vfs.fsf
    VuMfir,         // vmfir.dest vfd, vis
    VuIlwr, VuIswr, // op.field vit, (vis)
    VuLq, VuSq,     // op.dest vft, (vis)++ / (vis)--
    VuRnext, VuRget,// op.dest vft, R
    VuRinit, VuRxor,// op R, vfs.fsf
};

// Instruction property flags (bitmask).
enum : u32 {
    F_None = 0,
    F_Branch = 1u << 0,    // conditional branch
    F_Jump = 1u << 1,      // unconditional jump
    F_Call = 1u << 2,      // call (jal/jalr/b*al/vcallms)
    F_Return = 1u << 3,    // jr $ra
    F_Likely = 1u << 4,    // branch-likely (delay slot nullified when not taken)
    F_DelaySlot = 1u << 5, // has a branch delay slot
    F_Syscall = 1u << 6,
    F_Load = 1u << 7,
    F_Store = 1u << 8,
    F_Trap = 1u << 9,
    F_Cop = 1u << 10,      // coprocessor instruction (COP0/COP1/COP2)
};

struct Instruction {
    Op op = Op::Unknown;
    Form form = Form::None;
    u32 raw = 0;
    u32 flags = 0;
    // MIPS-format fields.
    u8 rs = 0, rt = 0, rd = 0, sa = 0;
    u16 imm = 0;
    u32 target = 0; // 26-bit jump index (raw)
    // VU-format fields (COP2 macro ops).
    u8 vu_fd = 0, vu_fs = 0, vu_ft = 0;
    u8 dest = 0;        // VU dest mask: bit3=x, bit2=y, bit1=z, bit0=w
    u8 fsf = 0, ftf = 0; // VU broadcast element selectors
    u8 aux = 0;         // sub-selector (pmfhl/pmthl suffix)

    bool is_branch() const { return (flags & F_Branch) != 0; }
    bool is_jump() const { return (flags & F_Jump) != 0; }
    bool is_call() const { return (flags & F_Call) != 0; }
    bool is_return() const { return (flags & F_Return) != 0; }
    bool is_likely() const { return (flags & F_Likely) != 0; }
    bool has_delay_slot() const { return (flags & F_DelaySlot) != 0; }
    bool is_syscall() const { return (flags & F_Syscall) != 0; }
    bool is_load() const { return (flags & F_Load) != 0; }
    bool is_store() const { return (flags & F_Store) != 0; }
    bool is_unknown() const { return op == Op::Unknown; }
};

// Decode one 32-bit instruction word.
Instruction decode(u32 raw);

// Mnemonic string for an opcode (e.g. "paddw", "add.s").
const char* mnemonic(Op op);

// Format an instruction as assembly text. `va` is the instruction's guest
// address (used to compute branch/jump targets).
std::string disassemble(const Instruction& insn, u32 va);

// Branch/jump target computation.
u32 branch_target(const Instruction& insn, u32 va);
u32 jump_target(const Instruction& insn, u32 va);

// ABI register names ("$zero", "$at", "$v0", ...).
const char* gpr_name(u8 index);

} // namespace ee::r5900



