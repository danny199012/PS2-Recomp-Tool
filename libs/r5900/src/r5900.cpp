// SPDX-License-Identifier: GPL-3.0-only
#include <ee/r5900.hpp>

#include <cstdarg>
#include <cstdio>

namespace ee::r5900 {

const char* mnemonic(Op op) {
    switch (op) {
#define EE_R5900_OP_MNEMONIC(name, text) case Op::name: return text;
        EE_R5900_OP_LIST(EE_R5900_OP_MNEMONIC)
#undef EE_R5900_OP_MNEMONIC
    case Op::Count:
        break;
    }
    return "???";
}

namespace {

const char* const kGprNames[32] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
};

const char* const kCop0Names[32] = {
    "Index", "Random", "EntryLo0", "EntryLo1", "Context", "PageMask",
    "Wired", "C0r7", "BadVaddr", "Count", "EntryHi", "Compare", "Status",
    "Cause", "EPC", "PRId", "Config", "C0r17", "C0r18", "C0r19", "C0r20",
    "C0r21", "C0r22", "C0r23", "Debug", "Perf", "C0r26", "C0r27", "TagLo",
    "TagHi", "ErrorPC", "C0r31",
};

// VU0 control registers (CFC2/CTC2). Only indices 1-6 are architecturally named.
const char* const kVuCtlNames[32] = {
    "c2cr0", "Status", "MAC", "Clip", "R", "I", "P", "c2cr7",
    "c2cr8", "c2cr9", "c2cr10", "c2cr11", "c2cr12", "c2cr13", "c2cr14", "c2cr15",
    "c2cr16", "c2cr17", "c2cr18", "c2cr19", "c2cr20", "c2cr21", "c2cr22", "c2cr23",
    "c2cr24", "c2cr25", "c2cr26", "c2cr27", "c2cr28", "c2cr29", "c2cr30", "c2cr31",
};

struct OpInfo {
    Op op;
    Form form;
    u32 flags;
};

constexpr OpInfo U() { return {Op::Unknown, Form::None, F_None}; }
constexpr OpInfo E(Op op, Form form, u32 flags = F_None) { return {op, form, flags}; }
constexpr OpInfo Cop(Op op, Form form, u32 flags = F_None) { return {op, form, flags | F_Cop}; }
constexpr OpInfo Cop(OpInfo info) { info.flags |= F_Cop; return info; }

constexpr u32 BR = F_Branch | F_DelaySlot;
constexpr u32 BRL = F_Branch | F_DelaySlot | F_Likely;

// Primary opcode map (index = raw >> 26). Class opcodes (SPECIAL/REGIMM/COPn/MMI)
// are dispatched by decode() directly and left Unknown here.
constexpr OpInfo kPrimary[64] = {
//  0x00                                        0x01                                        0x02                                        0x03
    U(),                                        U(),                                        E(Op::J, Form::Jump, F_Jump | F_DelaySlot), E(Op::Jal, Form::Jump, F_Jump | F_Call | F_DelaySlot),
//  0x04
    E(Op::Beq, Form::BranchRsRt, BR),           E(Op::Bne, Form::BranchRsRt, BR),           E(Op::Blez, Form::BranchRs, BR),            E(Op::Bgtz, Form::BranchRs, BR),
//  0x08
    E(Op::Addi, Form::RtRsImm),                 E(Op::Addiu, Form::RtRsImm),                E(Op::Slti, Form::RtRsImm),                 E(Op::Sltiu, Form::RtRsImm),
//  0x0C
    E(Op::Andi, Form::RtRsImmU),                E(Op::Ori, Form::RtRsImmU),                 E(Op::Xori, Form::RtRsImmU),                E(Op::Lui, Form::RtImm),
//  0x10 (COP0)                                 0x11 (COP1)                                 0x12 (COP2)                                 0x13
    U(),                                        U(),                                        U(),                                        U(),
//  0x14
    E(Op::Beql, Form::BranchRsRt, BRL),         E(Op::Bnel, Form::BranchRsRt, BRL),         E(Op::Blezl, Form::BranchRs, BRL),          E(Op::Bgtzl, Form::BranchRs, BRL),
//  0x18
    E(Op::Daddi, Form::RtRsImm),                E(Op::Daddiu, Form::RtRsImm),               E(Op::Ldl, Form::RtOffRs, F_Load),          E(Op::Ldr, Form::RtOffRs, F_Load),
//  0x1C (MMI)                                  0x1D                                        0x1E                                        0x1F
    U(),                                        U(),                                        E(Op::Lq, Form::RtOffRs, F_Load),           E(Op::Sq, Form::RtOffRs, F_Store),
//  0x20
    E(Op::Lb, Form::RtOffRs, F_Load),           E(Op::Lh, Form::RtOffRs, F_Load),           E(Op::Lwl, Form::RtOffRs, F_Load),          E(Op::Lw, Form::RtOffRs, F_Load),
//  0x24
    E(Op::Lbu, Form::RtOffRs, F_Load),          E(Op::Lhu, Form::RtOffRs, F_Load),          E(Op::Lwr, Form::RtOffRs, F_Load),          E(Op::Lwu, Form::RtOffRs, F_Load),
//  0x28
    E(Op::Sb, Form::RtOffRs, F_Store),          E(Op::Sh, Form::RtOffRs, F_Store),          E(Op::Swl, Form::RtOffRs, F_Store),         E(Op::Sw, Form::RtOffRs, F_Store),
//  0x2C
    E(Op::Sdl, Form::RtOffRs, F_Store),         E(Op::Sdr, Form::RtOffRs, F_Store),         E(Op::Swr, Form::RtOffRs, F_Store),         E(Op::Cache, Form::CacheLike),
//  0x30
    E(Op::Ll, Form::RtOffRs, F_Load),           Cop(Op::Lwc1, Form::FtOffRs, F_Load),       U(),                                        E(Op::Pref, Form::PrefLike),
//  0x34
    E(Op::Lld, Form::RtOffRs, F_Load),          Cop(Op::Ldc1, Form::FtOffRs, F_Load),       Cop(Op::Lqc2, Form::VtOffRs, F_Load),       E(Op::Ld, Form::RtOffRs, F_Load),
//  0x38
    E(Op::Sc, Form::RtOffRs, F_Store),          Cop(Op::Swc1, Form::FtOffRs, F_Store),      U(),                                        U(),
//  0x3C
    E(Op::Scd, Form::RtOffRs, F_Store),         Cop(Op::Sdc1, Form::FtOffRs, F_Store),      Cop(Op::Sqc2, Form::VtOffRs, F_Store),      E(Op::Sd, Form::RtOffRs, F_Store),
};

// SPECIAL class (primary 0x00), selected by funct = raw & 0x3F.
constexpr OpInfo kSpecial[64] = {
//  0x00
    E(Op::Sll, Form::RdRtSa),                   U(),                             E(Op::Srl, Form::RdRtSa),            E(Op::Sra, Form::RdRtSa),
//  0x04
    E(Op::Sllv, Form::RdRtRs),                  U(),                             E(Op::Srlv, Form::RdRtRs),           E(Op::Srav, Form::RdRtRs),
//  0x08
    E(Op::Jr, Form::Jr, F_Jump | F_DelaySlot),  E(Op::Jalr, Form::Jalr, F_Jump | F_Call | F_DelaySlot), E(Op::Movz, Form::RdRsRt), E(Op::Movn, Form::RdRsRt),
//  0x0C
    E(Op::Syscall, Form::Syscall, F_Syscall),   E(Op::Break, Form::Break),       U(),                             E(Op::Sync, Form::None),
//  0x10
    E(Op::Mfhi, Form::Rd),                      E(Op::Mthi, Form::Rs),           E(Op::Mflo, Form::Rd),             E(Op::Mtlo, Form::Rs),
//  0x14
    E(Op::Dsllv, Form::RdRtRs),                 U(),                             E(Op::Dsrlv, Form::RdRtRs),          E(Op::Dsrav, Form::RdRtRs),
//  0x18
    E(Op::Mult, Form::RdRsRtHideRd0),           E(Op::Multu, Form::RdRsRtHideRd0), E(Op::Div, Form::RdRsRtHideRd0),  E(Op::Divu, Form::RdRsRtHideRd0),
//  0x1C
    U(),                                        U(),                             U(),                             U(),
//  0x20
    E(Op::Add, Form::RdRsRt),                   E(Op::Addu, Form::RdRsRt),       E(Op::Sub, Form::RdRsRt),          E(Op::Subu, Form::RdRsRt),
//  0x24
    E(Op::And, Form::RdRsRt),                   E(Op::Or, Form::RdRsRt),         E(Op::Xor, Form::RdRsRt),          E(Op::Nor, Form::RdRsRt),
//  0x28
    E(Op::Mfsa, Form::Rd),                      E(Op::Mtsa, Form::Rs),           E(Op::Slt, Form::RdRsRt),          E(Op::Sltu, Form::RdRsRt),
//  0x2C
    E(Op::Dadd, Form::RdRsRt),                  E(Op::Daddu, Form::RdRsRt),      E(Op::Dsub, Form::RdRsRt),         E(Op::Dsubu, Form::RdRsRt),
//  0x30
    E(Op::Tge, Form::TrapRsRt, F_Trap),         E(Op::Tgeu, Form::TrapRsRt, F_Trap), E(Op::Tlt, Form::TrapRsRt, F_Trap), E(Op::Tltu, Form::TrapRsRt, F_Trap),
//  0x34
    E(Op::Teq, Form::TrapRsRt, F_Trap),         U(),                             E(Op::Tne, Form::TrapRsRt, F_Trap), U(),
//  0x38
    E(Op::Dsll, Form::RdRtSa),                  U(),                             E(Op::Dsrl, Form::RdRtSa),         E(Op::Dsra, Form::RdRtSa),
//  0x3C
    E(Op::Dsll32, Form::RdRtSa),                U(),                             E(Op::Dsrl32, Form::RdRtSa),       E(Op::Dsra32, Form::RdRtSa),
};

// REGIMM class (primary 0x01), selected by rt.
constexpr OpInfo kRegimm[32] = {
//  0x00
    E(Op::Bltz, Form::BranchRs, BR),            E(Op::Bgez, Form::BranchRs, BR),   E(Op::Bltzl, Form::BranchRs, BRL),  E(Op::Bgezl, Form::BranchRs, BRL),
//  0x04
    U(),                                        U(),                             U(),                             U(),
//  0x08
    E(Op::Tgei, Form::TrapRsImm, F_Trap),       E(Op::Tgeiu, Form::TrapRsImm, F_Trap), E(Op::Tlti, Form::TrapRsImm, F_Trap), E(Op::Tltiu, Form::TrapRsImm, F_Trap),
//  0x0C
    E(Op::Teqi, Form::TrapRsImm, F_Trap),       U(),                             E(Op::Tnei, Form::TrapRsImm, F_Trap), U(),
//  0x10
    E(Op::Bltzal, Form::BranchRs, BR | F_Call), E(Op::Bgezal, Form::BranchRs, BR | F_Call), E(Op::Bltzall, Form::BranchRs, BRL | F_Call), E(Op::Bgezall, Form::BranchRs, BRL | F_Call),
//  0x14
    U(),                                        U(),                             U(),                             U(),
//  0x18
    U(),                                        E(Op::Mtsab, Form::MtsabLike),   E(Op::Mtsah, Form::MtsabLike),   U(),
//  0x1C
    U(),                                        U(),                             U(),                             U(),
};



// MMI class (primary 0x1C), selected by funct. Subclasses MMI0/1/2/3 are
// dispatched by decode() and left Unknown here.
constexpr OpInfo kMmi[64] = {
//  0x00
    E(Op::Madd, Form::RdRsRtHideRd0),           E(Op::Maddu, Form::RdRsRtHideRd0), U(),                             U(),
//  0x04
    E(Op::Plzcw, Form::RdRs),                   U(),                             U(),                             U(),
//  0x08 (MMI0)                                 0x09 (MMI2)
    U(),                                        U(),                             U(),                             U(),
//  0x0C
    U(),                                        U(),                             U(),                             U(),
//  0x10
    E(Op::Mfhi1, Form::Rd),                     E(Op::Mthi1, Form::Rs),          E(Op::Mflo1, Form::Rd),          E(Op::Mtlo1, Form::Rs),
//  0x14
    U(),                                        U(),                             U(),                             U(),
//  0x18
    E(Op::Mult1, Form::RdRsRtHideRd0),          E(Op::Multu1, Form::RdRsRtHideRd0), E(Op::Div1, Form::RdRsRtHideRd0), E(Op::Divu1, Form::RdRsRtHideRd0),
//  0x1C
    U(),                                        U(),                             U(),                             U(),
//  0x20
    E(Op::Madd1, Form::RdRsRtHideRd0),          E(Op::Maddu1, Form::RdRsRtHideRd0), U(),                            U(),
//  0x24
    U(),                                        U(),                             U(),                             U(),
//  0x28 (MMI1)                                 0x29 (MMI3)
    U(),                                        U(),                             U(),                             U(),
//  0x2C
    U(),                                        U(),                             U(),                             U(),
//  0x30
    E(Op::Pmfhl, Form::Rd),                     E(Op::Pmthl, Form::Rs),          U(),                             U(),
//  0x34
    E(Op::Psllh, Form::RdRtSa),                 E(Op::Psrlh, Form::RdRtSa),      E(Op::Psrah, Form::RdRtSa),      U(),
//  0x38
    U(),                                        U(),                             U(),                             U(),
//  0x3C
    E(Op::Psllw, Form::RdRtSa),                 E(Op::Psrlw, Form::RdRtSa),      E(Op::Psraw, Form::RdRtSa),      U(),
};

// MMI0 subclass, selected by sa = (raw >> 6) & 0x1F.
constexpr OpInfo kMmi0[32] = {
//  0x00
    E(Op::Paddw, Form::RdRsRt),  E(Op::Psubw, Form::RdRsRt),  E(Op::Pcgtw, Form::RdRsRt),  E(Op::Pmaxw, Form::RdRsRt),
//  0x04
    E(Op::Paddh, Form::RdRsRt),  E(Op::Psubh, Form::RdRsRt),  E(Op::Pcgth, Form::RdRsRt),  E(Op::Pmaxh, Form::RdRsRt),
//  0x08
    E(Op::Paddb, Form::RdRsRt),  E(Op::Psubb, Form::RdRsRt),  E(Op::Pcgtb, Form::RdRsRt),  U(),
//  0x0C
    U(),                         U(),                         U(),                         U(),
//  0x10
    E(Op::Paddsw, Form::RdRsRt), E(Op::Psubsw, Form::RdRsRt), E(Op::Pextlw, Form::RdRsRt), E(Op::Ppacw, Form::RdRsRt),
//  0x14
    E(Op::Paddsh, Form::RdRsRt), E(Op::Psubsh, Form::RdRsRt), E(Op::Pextlh, Form::RdRsRt), E(Op::Ppach, Form::RdRsRt),
//  0x18
    E(Op::Paddsb, Form::RdRsRt), E(Op::Psubsb, Form::RdRsRt), E(Op::Pextlb, Form::RdRsRt), E(Op::Ppacb, Form::RdRsRt),
//  0x1C
    U(),                         U(),                         E(Op::Pext5, Form::RdRt),    E(Op::Ppac5, Form::RdRt),
};

// MMI1 subclass, selected by sa.
constexpr OpInfo kMmi1[32] = {
//  0x00
    U(),                         E(Op::Pabsw, Form::RdRt),    E(Op::Pceqw, Form::RdRsRt),  E(Op::Pminw, Form::RdRsRt),
//  0x04
    E(Op::Padsbh, Form::RdRsRt), E(Op::Pabsh, Form::RdRt),    E(Op::Pceqh, Form::RdRsRt),  E(Op::Pminh, Form::RdRsRt),
//  0x08
    U(),                         U(),                         E(Op::Pceqb, Form::RdRsRt),  U(),
//  0x0C
    U(),                         U(),                         U(),                         U(),
//  0x10
    E(Op::Padduw, Form::RdRsRt), E(Op::Psubuw, Form::RdRsRt), E(Op::Pextuw, Form::RdRsRt), U(),
//  0x14
    E(Op::Padduh, Form::RdRsRt), E(Op::Psubuh, Form::RdRsRt), E(Op::Pextuh, Form::RdRsRt), U(),
//  0x18
    E(Op::Paddub, Form::RdRsRt), E(Op::Psubub, Form::RdRsRt), E(Op::Pextub, Form::RdRsRt), E(Op::Qfsrv, Form::RdRsRt),
//  0x1C
    U(),                         U(),                         U(),                         U(),
};

// MMI2 subclass, selected by sa.
constexpr OpInfo kMmi2[32] = {
//  0x00
    E(Op::Pmaddw, Form::RdRsRt), U(),                         E(Op::Psllvw, Form::RdRtRs), E(Op::Psrlvw, Form::RdRtRs),
//  0x04
    E(Op::Pmsubw, Form::RdRsRt), U(),                         U(),                         U(),
//  0x08
    E(Op::Pmfhi, Form::Rd),      E(Op::Pmflo, Form::Rd),      E(Op::Pinth, Form::RdRsRt),  U(),
//  0x0C
    E(Op::Pmultw, Form::RdRsRt), E(Op::Pdivw, Form::RsRt),    E(Op::Pcpyld, Form::RdRsRt), U(),
//  0x10
    E(Op::Pmaddh, Form::RdRsRt), E(Op::Phmadh, Form::RdRsRt), E(Op::Pand, Form::RdRsRt),   E(Op::Pxor, Form::RdRsRt),
//  0x14
    E(Op::Pmsubh, Form::RdRsRt), E(Op::Phmsbh, Form::RdRsRt), U(),                         U(),
//  0x18
    U(),                         U(),                         E(Op::Pexeh, Form::RdRt),    E(Op::Prevh, Form::RdRt),
//  0x1C
    E(Op::Pmulth, Form::RdRsRt), E(Op::Pdivbw, Form::RsRt),   E(Op::Pexew, Form::RdRt),    E(Op::Prot3w, Form::RdRt),
};

// MMI3 subclass, selected by sa.
constexpr OpInfo kMmi3[32] = {
//  0x00
    E(Op::Pmadduw, Form::RdRsRt), U(),                          U(),                          E(Op::Psravw, Form::RdRtRs),
//  0x04
    U(),                          U(),                          U(),                          U(),
//  0x08
    E(Op::Pmthi, Form::Rs),       E(Op::Pmtlo, Form::Rs),       E(Op::Pinteh, Form::RdRsRt),  U(),
//  0x0C
    E(Op::Pmultuw, Form::RdRsRt), E(Op::Pdivuw, Form::RsRt),    E(Op::Pcpyud, Form::RdRsRt),  U(),
//  0x10
    U(),                          U(),                          E(Op::Por, Form::RdRsRt),     E(Op::Pnor, Form::RdRsRt),
//  0x14
    U(),                          U(),                          U(),                          U(),
//  0x18
    U(),                          U(),                          E(Op::Pexch, Form::RdRt),     E(Op::Pcpyh, Form::RdRt),
//  0x1C
    U(),                          U(),                          E(Op::Pexcw, Form::RdRt),     U(),
};

// COP1 fmt=S (rs=16), selected by funct. Note: the R5900 FPU only implements
// the c.f/c.eq/c.olt/c.ole conditions.
constexpr OpInfo kCop1S[64] = {
//  0x00
    E(Op::AddS, Form::Fpu3),     E(Op::SubS, Form::Fpu3),     E(Op::MulS, Form::Fpu3),     E(Op::DivS, Form::Fpu3),
//  0x04
    E(Op::SqrtS, Form::Fpu2),    E(Op::AbsS, Form::Fpu1),     E(Op::MovS, Form::Fpu1),     E(Op::NegS, Form::Fpu1),
//  0x08
    U(),                         U(),                         U(),                         U(),
//  0x0C
    U(),                         E(Op::TruncwS, Form::FpuCvt), U(),                        U(),
//  0x10
    U(),                         U(),                         U(),                         U(),
//  0x14
    U(),                         U(),                         E(Op::RsqrtS, Form::Fpu2),   U(),
//  0x18
    E(Op::AddaS, Form::FpuAcc2), E(Op::SubaS, Form::FpuAcc2), E(Op::MulaS, Form::FpuAcc2), U(),
//  0x1C
    E(Op::MaddS, Form::Fpu3),    E(Op::MsubS, Form::Fpu3),    E(Op::MaddaS, Form::FpuAcc2), E(Op::MsubaS, Form::FpuAcc2),
//  0x20
    U(),                         U(),                         U(),                         U(),
//  0x24
    U(),                         U(),                         U(),                         U(),
//  0x28
    U(),                         U(),                         U(),                         U(),
//  0x2C
    U(),                         U(),                         U(),                         U(),
//  0x30
    E(Op::CfS, Form::FpuCmp),    U(),                         E(Op::CeqS, Form::FpuCmp),   U(),
//  0x34
    E(Op::ColtS, Form::FpuCmp),  U(),                         E(Op::ColeS, Form::FpuCmp),  U(),
//  0x38
    U(),                         U(),                         U(),                         U(),
//  0x3C
    U(),                         U(),                         U(),                         U(),
};

// VU0 macro SPECIAL1 (COP2 with rs >= 16), selected by funct.
constexpr OpInfo kVu1[64] = {
//  0x00
    E(Op::VaddX, Form::Vu3),  E(Op::VaddY, Form::Vu3),  E(Op::VaddZ, Form::Vu3),  E(Op::VaddW, Form::Vu3),
//  0x04
    E(Op::VsubX, Form::Vu3),  E(Op::VsubY, Form::Vu3),  E(Op::VsubZ, Form::Vu3),  E(Op::VsubW, Form::Vu3),
//  0x08
    E(Op::VmaddX, Form::Vu3), E(Op::VmaddY, Form::Vu3), E(Op::VmaddZ, Form::Vu3), E(Op::VmaddW, Form::Vu3),
//  0x0C
    E(Op::VmsubX, Form::Vu3), E(Op::VmsubY, Form::Vu3), E(Op::VmsubZ, Form::Vu3), E(Op::VmsubW, Form::Vu3),
//  0x10
    E(Op::VmaxX, Form::Vu3),  E(Op::VmaxY, Form::Vu3),  E(Op::VmaxZ, Form::Vu3),  E(Op::VmaxW, Form::Vu3),
//  0x14
    E(Op::VminiX, Form::Vu3), E(Op::VminiY, Form::Vu3), E(Op::VminiZ, Form::Vu3), E(Op::VminiW, Form::Vu3),
//  0x18
    E(Op::VmulX, Form::Vu3),  E(Op::VmulY, Form::Vu3),  E(Op::VmulZ, Form::Vu3),  E(Op::VmulW, Form::Vu3),
//  0x1C
    E(Op::Vmulq, Form::VuQ2), E(Op::Vmaxi, Form::VuI2), E(Op::Vmuli, Form::VuI2), E(Op::Vminii, Form::VuI2),
//  0x20
    E(Op::Vaddq, Form::VuQ2), E(Op::Vmaddq, Form::VuQ2), E(Op::Vaddi, Form::VuI2), E(Op::Vmaddi, Form::VuI2),
//  0x24
    E(Op::Vsubq, Form::VuQ2), E(Op::Vmsubq, Form::VuQ2), E(Op::Vsubi, Form::VuI2), E(Op::Vmsubi, Form::VuI2),
//  0x28
    E(Op::Vadd, Form::Vu3),   E(Op::Vmadd, Form::Vu3),  E(Op::Vmul, Form::Vu3),   E(Op::Vmax, Form::Vu3),
//  0x2C
    E(Op::Vsub, Form::Vu3),   E(Op::Vmsub, Form::Vu3),  E(Op::Vopmsub, Form::Vu3), E(Op::Vmini, Form::Vu3),
//  0x30
    E(Op::Viadd, Form::VuI3), E(Op::Visub, Form::VuI3), E(Op::Viaddi, Form::VuIaddi), U(),
//  0x34
    E(Op::Viand, Form::VuI3), E(Op::Vior, Form::VuI3),  U(),                      U(),
//  0x38
    E(Op::Vcallms, Form::VuCallms, F_Call), E(Op::Vcallmsr, Form::VuCallmsr, F_Call), U(), U(),
//  0x3C (SPECIAL2 dispatch, handled by decode())
    U(),                      U(),                      U(),                      U(),
};

// VU0 macro SPECIAL2, index = (raw & 0x3) | ((raw >> 4) & 0x7C).
constexpr OpInfo kVu2[128] = {
//  0x00
    E(Op::VaddaX, Form::VuAcc2), E(Op::VaddaY, Form::VuAcc2), E(Op::VaddaZ, Form::VuAcc2), E(Op::VaddaW, Form::VuAcc2),
//  0x04
    E(Op::VsubaX, Form::VuAcc2), E(Op::VsubaY, Form::VuAcc2), E(Op::VsubaZ, Form::VuAcc2), E(Op::VsubaW, Form::VuAcc2),
//  0x08
    E(Op::VmaddaX, Form::VuAcc2), E(Op::VmaddaY, Form::VuAcc2), E(Op::VmaddaZ, Form::VuAcc2), E(Op::VmaddaW, Form::VuAcc2),
//  0x0C
    E(Op::VmsubaX, Form::VuAcc2), E(Op::VmsubaY, Form::VuAcc2), E(Op::VmsubaZ, Form::VuAcc2), E(Op::VmsubaW, Form::VuAcc2),
//  0x10
    E(Op::Vitof0, Form::Vu2),   E(Op::Vitof4, Form::Vu2),   E(Op::Vitof12, Form::Vu2),  E(Op::Vitof15, Form::Vu2),
//  0x14
    E(Op::Vftoi0, Form::Vu2),   E(Op::Vftoi4, Form::Vu2),   E(Op::Vftoi12, Form::Vu2),  E(Op::Vftoi15, Form::Vu2),
//  0x18
    E(Op::VmulaX, Form::VuAcc2), E(Op::VmulaY, Form::VuAcc2), E(Op::VmulaZ, Form::VuAcc2), E(Op::VmulaW, Form::VuAcc2),
//  0x1C
    E(Op::Vmulaq, Form::VuAccQ), E(Op::Vabs, Form::Vu2),     E(Op::Vmulai, Form::VuAccI), E(Op::Vclipw, Form::VuClipw),
//  0x20
    E(Op::Vaddaq, Form::VuAccQ), E(Op::Vmaddaq, Form::VuAccQ), E(Op::Vaddai, Form::VuAccI), E(Op::Vmaddai, Form::VuAccI),
//  0x24
    E(Op::Vsubaq, Form::VuAccQ), E(Op::Vmsubaq, Form::VuAccQ), E(Op::Vsubai, Form::VuAccI), E(Op::Vmsubai, Form::VuAccI),
//  0x28
    E(Op::Vadda, Form::VuAcc2), E(Op::Vmadda, Form::VuAcc2), E(Op::Vmula, Form::VuAcc2),  U(),
//  0x2C
    E(Op::Vsuba, Form::VuAcc2), E(Op::Vmsuba, Form::VuAcc2), E(Op::Vopmula, Form::VuAcc2), E(Op::Vnop, Form::None),
//  0x30
    E(Op::Vmove, Form::Vu2),    E(Op::Vmr32, Form::Vu2),    U(),                        U(),
//  0x34
    E(Op::Vlqi, Form::VuLq, F_Load), E(Op::Vsqi, Form::VuSq, F_Store), E(Op::Vlqd, Form::VuLq, F_Load), E(Op::Vsqd, Form::VuSq, F_Store),
//  0x38
    E(Op::Vdiv, Form::VuQ),     E(Op::Vsqrt, Form::VuSqrt), E(Op::Vrsqrt, Form::VuQ),   E(Op::Vwaitq, Form::None),
//  0x3C
    E(Op::Vmtir, Form::VuMtir), E(Op::Vmfir, Form::VuMfir), E(Op::Vilwr, Form::VuIlwr, F_Load), E(Op::Viswr, Form::VuIswr, F_Store),
//  0x40
    E(Op::Vrnext, Form::VuRnext), E(Op::Vrget, Form::VuRget), E(Op::Vrinit, Form::VuRinit), E(Op::Vrxor, Form::VuRxor),
//  0x44 .. 0x7F
    U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(),
    U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(),
    U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(),
    U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(), U(),
};

OpInfo decode_cop0(const Instruction& in) {
    switch (in.rs) {
    case 0x00: return Cop(Op::Mfc0, Form::Mfc0);
    case 0x04: return Cop(Op::Mtc0, Form::Mtc0);
    case 0x08: // BC0
        switch (in.rt & 3) {
        case 0: return Cop(Op::Bc0f, Form::BranchCop, BR);
        case 1: return Cop(Op::Bc0t, Form::BranchCop, BR);
        case 2: return Cop(Op::Bc0fl, Form::BranchCop, BRL);
        default: return Cop(Op::Bc0tl, Form::BranchCop, BRL);
        }
    default:
        if (in.rs >= 16) { // CO instructions
            switch (in.raw & 0x3F) {
            case 0x01: return Cop(Op::Tlbr, Form::None);
            case 0x02: return Cop(Op::Tlbwi, Form::None);
            case 0x06: return Cop(Op::Tlbwr, Form::None);
            case 0x08: return Cop(Op::Tlbp, Form::None);
            case 0x18: return Cop(Op::Eret, Form::None);
            case 0x38: return Cop(Op::Ei, Form::None);
            case 0x39: return Cop(Op::Di, Form::None);
            default: break;
            }
        }
        break;
    }
    return U();
}

OpInfo decode_cop1(const Instruction& in) {
    switch (in.rs) {
    case 0x00: return Cop(Op::Mfc1, Form::Mfc1);
    case 0x02: return Cop(Op::Cfc1, Form::Cfc1);
    case 0x04: return Cop(Op::Mtc1, Form::Mtc1);
    case 0x06: return Cop(Op::Ctc1, Form::Ctc1);
    case 0x08: // BC1
        switch (in.rt & 3) {
        case 0: return Cop(Op::Bc1f, Form::BranchCop, BR);
        case 1: return Cop(Op::Bc1t, Form::BranchCop, BR);
        case 2: return Cop(Op::Bc1fl, Form::BranchCop, BRL);
        default: return Cop(Op::Bc1tl, Form::BranchCop, BRL);
        }
    case 0x10: // fmt = S
        return Cop(kCop1S[in.raw & 0x3F]);
    case 0x14: // fmt = W
        if ((in.raw & 0x3F) == 0x20)
            return Cop(Op::CvtsW, Form::FpuCvt);
        break;
    default:
        break;
    }
    return U();
}

OpInfo decode_cop2(const Instruction& in) {
    switch (in.rs) {
    case 0x01: return Cop(Op::Qmfc2, Form::Qmfc2);
    case 0x02: return Cop(Op::Cfc2, Form::Cfc2);
    case 0x05: return Cop(Op::Qmtc2, Form::Qmtc2);
    case 0x06: return Cop(Op::Ctc2, Form::Ctc2);
    case 0x08: // BC2
        switch (in.rt & 3) {
        case 0: return Cop(Op::Bc2f, Form::BranchCop, BR);
        case 1: return Cop(Op::Bc2t, Form::BranchCop, BR);
        case 2: return Cop(Op::Bc2fl, Form::BranchCop, BRL);
        default: return Cop(Op::Bc2tl, Form::BranchCop, BRL);
        }
    default:
        if (in.rs >= 16) { // VU0 macro instruction
            const u32 funct = in.raw & 0x3F;
            if (funct < 0x3C)
                return Cop(kVu1[funct]);
            return Cop(kVu2[(in.raw & 0x3) | ((in.raw >> 4) & 0x7C)]);
        }
        break;
    }
    return U();
}

OpInfo decode_mmi(const Instruction& in) {
    switch (in.raw & 0x3F) {
    case 0x08: return kMmi0[in.sa];
    case 0x09: return kMmi2[in.sa];
    case 0x28: return kMmi1[in.sa];
    case 0x29: return kMmi3[in.sa];
    default: return kMmi[in.raw & 0x3F];
    }
}

} // namespace

Instruction decode(u32 raw) {
    Instruction in{};
    in.raw = raw;
    in.rs = (raw >> 21) & 0x1F;
    in.rt = (raw >> 16) & 0x1F;
    in.rd = (raw >> 11) & 0x1F;
    in.sa = (raw >> 6) & 0x1F;
    in.imm = raw & 0xFFFF;
    in.target = raw & 0x03FFFFFF;
    in.vu_fd = (raw >> 6) & 0x1F;
    in.vu_fs = (raw >> 11) & 0x1F;
    in.vu_ft = (raw >> 16) & 0x1F;
    in.dest = (raw >> 21) & 0xF;
    in.fsf = (raw >> 21) & 0x3;
    in.ftf = (raw >> 23) & 0x3;

    OpInfo info = U();
    switch (raw >> 26) {
    case 0x00: info = kSpecial[raw & 0x3F]; break;
    case 0x01: info = kRegimm[in.rt]; break;
    case 0x10: info = decode_cop0(in); break;
    case 0x11: info = decode_cop1(in); break;
    case 0x12: info = decode_cop2(in); break;
    case 0x1C: info = decode_mmi(in); break;
    default: info = kPrimary[raw >> 26]; break;
    }
    in.op = info.op;
    in.form = info.form;
    in.flags = info.flags;

    if (in.op == Op::Jr && in.rs == 31)
        in.flags |= F_Return;
    if (in.op == Op::Pmfhl || in.op == Op::Pmthl)
        in.aux = in.sa;
    return in;
}

u32 branch_target(const Instruction& insn, u32 va) {
    return va + 4 + (static_cast<u32>(static_cast<s32>(static_cast<s16>(insn.imm))) << 2);
}

u32 jump_target(const Instruction& insn, u32 va) {
    return (va & 0xF0000000) | (insn.target << 2);
}

const char* gpr_name(u8 index) { return kGprNames[index & 31]; }

namespace {

std::string fmt(const char* format, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return buf;
}

s32 sext16(u16 v) { return static_cast<s16>(v); }
s32 sext5(u8 v) { return static_cast<s32>(static_cast<s8>(v << 3)) >> 3; }

std::string dest_suffix(u8 dest) {
    if (dest == 0xF)
        return ".xyzw";
    if (dest == 0)
        return "";
    std::string s = ".";
    if (dest & 8) s += 'x';
    if (dest & 4) s += 'y';
    if (dest & 2) s += 'z';
    if (dest & 1) s += 'w';
    return s;
}

char field_char(u8 f) { return "xyzw"[f & 3]; }

const char* const kPmfhlSuffix[8] = {"lw", "uw", "slw", "lh", "sh", "??", "??", "??"};

} // namespace

std::string disassemble(const Instruction& in, u32 va) {
    if (in.raw == 0)
        return "nop";
    if (in.op == Op::Unknown)
        return fmt(".word 0x%08X", in.raw);

    const char* m = mnemonic(in.op);
    switch (in.form) {
    case Form::None: return m;
    case Form::RdRsRt: return fmt("%s %s, %s, %s", m, gpr_name(in.rd), gpr_name(in.rs), gpr_name(in.rt));
    case Form::RdRsRtHideRd0:
        if (in.rd == 0)
            return fmt("%s %s, %s", m, gpr_name(in.rs), gpr_name(in.rt));
        return fmt("%s %s, %s, %s", m, gpr_name(in.rd), gpr_name(in.rs), gpr_name(in.rt));
    case Form::RdRtSa: return fmt("%s %s, %s, %u", m, gpr_name(in.rd), gpr_name(in.rt), unsigned(in.sa));
    case Form::RdRtRs: return fmt("%s %s, %s, %s", m, gpr_name(in.rd), gpr_name(in.rt), gpr_name(in.rs));
    case Form::RdRs: return fmt("%s %s, %s", m, gpr_name(in.rd), gpr_name(in.rs));
    case Form::RdRt: return fmt("%s %s, %s", m, gpr_name(in.rd), gpr_name(in.rt));
    case Form::Rd:
        if (in.op == Op::Pmfhl)
            return fmt("pmfhl.%s %s", kPmfhlSuffix[in.aux & 7], gpr_name(in.rd));
        return fmt("%s %s", m, gpr_name(in.rd));
    case Form::Rs:
        if (in.op == Op::Pmthl)
            return fmt("pmthl.%s %s", kPmfhlSuffix[in.aux & 7], gpr_name(in.rs));
        return fmt("%s %s", m, gpr_name(in.rs));
    case Form::RsRt: return fmt("%s %s, %s", m, gpr_name(in.rs), gpr_name(in.rt));
    case Form::RtRsImm: return fmt("%s %s, %s, %d", m, gpr_name(in.rt), gpr_name(in.rs), sext16(in.imm));
    case Form::RtRsImmU: return fmt("%s %s, %s, 0x%04X", m, gpr_name(in.rt), gpr_name(in.rs), unsigned(in.imm));
    case Form::RtImm: return fmt("%s %s, 0x%04X", m, gpr_name(in.rt), unsigned(in.imm));
    case Form::RtOffRs: return fmt("%s %s, %d(%s)", m, gpr_name(in.rt), sext16(in.imm), gpr_name(in.rs));
    case Form::FtOffRs: return fmt("%s $f%u, %d(%s)", m, unsigned(in.rt), sext16(in.imm), gpr_name(in.rs));
    case Form::VtOffRs: return fmt("%s $vf%u, %d(%s)", m, unsigned(in.rt), sext16(in.imm), gpr_name(in.rs));
    case Form::BranchRsRt: return fmt("%s %s, %s, 0x%08X", m, gpr_name(in.rs), gpr_name(in.rt), branch_target(in, va));
    case Form::BranchRs: return fmt("%s %s, 0x%08X", m, gpr_name(in.rs), branch_target(in, va));
    case Form::Jump: return fmt("%s 0x%08X", m, jump_target(in, va));
    case Form::Jr: return fmt("%s %s", m, gpr_name(in.rs));
    case Form::Jalr:
        if (in.rd == 31)
            return fmt("jalr %s", gpr_name(in.rs));
        return fmt("jalr %s, %s", gpr_name(in.rd), gpr_name(in.rs));
    case Form::Syscall: return "syscall";
    case Form::Break: return "break";
    case Form::TrapRsRt: return fmt("%s %s, %s", m, gpr_name(in.rs), gpr_name(in.rt));
    case Form::TrapRsImm: return fmt("%s %s, %d", m, gpr_name(in.rs), sext16(in.imm));
    case Form::MtsabLike: return fmt("%s %s, %d", m, gpr_name(in.rs), sext16(in.imm));
    case Form::CacheLike:
    case Form::PrefLike: return fmt("%s 0x%02X, %d(%s)", m, unsigned(in.rt), sext16(in.imm), gpr_name(in.rs));
    case Form::Mfc0:
    case Form::Mtc0: return fmt("%s %s, %s", m, gpr_name(in.rt), kCop0Names[in.rd]);
    case Form::BranchCop: return fmt("%s 0x%08X", m, branch_target(in, va));
    case Form::Mfc1:
    case Form::Mtc1: return fmt("%s %s, $f%u", m, gpr_name(in.rt), unsigned(in.rd));
    case Form::Cfc1:
    case Form::Ctc1: return fmt("%s %s, $fcr%u", m, gpr_name(in.rt), unsigned(in.rd));
    case Form::Fpu3: return fmt("%s $f%u, $f%u, $f%u", m, unsigned(in.sa), unsigned(in.rd), unsigned(in.rt));
    case Form::Fpu2:
    case Form::Fpu1:
    case Form::FpuCvt: return fmt("%s $f%u, $f%u", m, unsigned(in.sa), unsigned(in.rd));
    case Form::FpuCmp:
    case Form::FpuAcc2: return fmt("%s $f%u, $f%u", m, unsigned(in.rd), unsigned(in.rt));
    case Form::Qmfc2:
    case Form::Qmtc2: return fmt("%s %s, $vf%u", m, gpr_name(in.rt), unsigned(in.rd));
    case Form::Cfc2:
    case Form::Ctc2: return fmt("%s %s, %s", m, gpr_name(in.rt), kVuCtlNames[in.rd]);
    case Form::Vu3: return fmt("%s%s $vf%u, $vf%u, $vf%u", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fd), unsigned(in.vu_fs), unsigned(in.vu_ft));
    case Form::VuQ2: return fmt("%s%s $vf%u, $vf%u, Q", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fd), unsigned(in.vu_fs));
    case Form::VuI2: return fmt("%s%s $vf%u, $vf%u, I", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fd), unsigned(in.vu_fs));
    case Form::VuAcc2: return fmt("%s%s $vf%u, $vf%u", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fs), unsigned(in.vu_ft));
    case Form::VuAccQ: return fmt("%s%s $vf%u, Q", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fs));
    case Form::VuAccI: return fmt("%s%s $vf%u, I", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fs));
    case Form::Vu2: return fmt("%s%s $vf%u, $vf%u", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fd), unsigned(in.vu_fs));
    case Form::VuClipw: return fmt("vclipw.xyz $vf%u, $vf%u", unsigned(in.vu_fs), unsigned(in.vu_ft));
    case Form::VuQ: return fmt("%s Q, $vf%u.%c, $vf%u.%c", m, unsigned(in.vu_fs), field_char(in.fsf), unsigned(in.vu_ft), field_char(in.ftf));
    case Form::VuSqrt: return fmt("%s Q, $vf%u.%c", m, unsigned(in.vu_ft), field_char(in.ftf));
    case Form::VuI3: return fmt("%s $vi%u, $vi%u, $vi%u", m, unsigned(in.vu_fd), unsigned(in.vu_fs), unsigned(in.vu_ft));
    case Form::VuIaddi: return fmt("%s $vi%u, $vi%u, %d", m, unsigned(in.vu_ft), unsigned(in.vu_fs), sext5(in.sa));
    case Form::VuCallms: return fmt("%s 0x%08X", m, (in.raw & 0x03FFFFFF) << 3);
    case Form::VuCallmsr: return fmt("%s $vi%u", m, unsigned(in.vu_fs));
    case Form::VuMtir: return fmt("%s $vi%u, $vf%u.%c", m, unsigned(in.vu_fd), unsigned(in.vu_fs), field_char(in.fsf));
    case Form::VuMfir: return fmt("%s%s $vf%u, $vi%u", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_fd), unsigned(in.vu_fs));
    case Form::VuIlwr:
    case Form::VuIswr: return fmt("%s.%c $vi%u, ($vi%u)", m, field_char(in.ftf), unsigned(in.vu_ft), unsigned(in.vu_fs));
    case Form::VuLq: return fmt("%s%s $vf%u, ($vi%u)%s", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_ft), unsigned(in.vu_fs), in.op == Op::Vlqd ? "--" : "++");
    case Form::VuSq: return fmt("%s%s $vf%u, ($vi%u)%s", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_ft), unsigned(in.vu_fs), in.op == Op::Vsqd ? "--" : "++");
    case Form::VuRnext:
    case Form::VuRget: return fmt("%s%s $vf%u, R", m, dest_suffix(in.dest).c_str(), unsigned(in.vu_ft));
    case Form::VuRinit:
    case Form::VuRxor: return fmt("%s R, $vf%u.%c", m, unsigned(in.vu_fs), field_char(in.fsf));
    }
    return fmt("%s ; <unhandled form>", m);
}

} // namespace ee::r5900
