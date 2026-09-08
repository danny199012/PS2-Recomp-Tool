// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Shared synthetic PS2 ELF fixture: `main` (stack frame + call + jump table),
// `helper` (called by main), and `hidden` (only reachable via prologue scan).
#include <cstddef>
#include <cstring>
#include <vector>
#include <ee/types.hpp>

namespace ee::test {

inline void wr16(std::vector<u8>& v, size_t off, u16 x) { v[off] = u8(x); v[off + 1] = u8(x >> 8); }
inline void wr32(std::vector<u8>& v, size_t off, u32 x) {
    v[off] = u8(x); v[off + 1] = u8(x >> 8); v[off + 2] = u8(x >> 16); v[off + 3] = u8(x >> 24);
}

constexpr u32 kTextBase = 0x100000;
constexpr u32 kMainAddr = 0x100000;
constexpr u32 kHelperAddr = 0x100080;
constexpr u32 kHiddenAddr = 0x1000C0;
constexpr u32 kTableAddr = 0x200000;

inline std::vector<u8> make_analysis_elf() {
    constexpr size_t TEXT_OFF = 0x100;
    constexpr size_t TEXT_SIZE = 0x100;
    constexpr size_t RODATA_OFF = 0x200;
    constexpr size_t STRTAB_OFF = 0x210;
    constexpr size_t SYMTAB_OFF = 0x220;
    constexpr size_t SHSTRTAB_OFF = 0x250;
    constexpr size_t SHOFF = 0x27C;
    constexpr size_t SHNUM = 6;
    constexpr size_t FILE_SIZE = SHOFF + SHNUM * 40;

    std::vector<u8> v(FILE_SIZE, 0);
    v[0] = 0x7F; v[1] = 'E'; v[2] = 'L'; v[3] = 'F'; v[4] = 1; v[5] = 1; v[6] = 1;
    wr16(v, 16, 2); wr16(v, 18, 8); wr32(v, 20, 1); wr32(v, 24, kMainAddr);
    wr32(v, 28, 52); wr32(v, 32, SHOFF); wr16(v, 40, 52); wr16(v, 42, 32); wr16(v, 44, 2);
    wr16(v, 46, 40); wr16(v, 48, SHNUM); wr16(v, 50, 5);

    // phdr 0: .text (R+X)
    wr32(v, 52 + 0, 1); wr32(v, 52 + 4, TEXT_OFF); wr32(v, 52 + 8, kTextBase); wr32(v, 52 + 12, kTextBase);
    wr32(v, 52 + 16, TEXT_SIZE); wr32(v, 52 + 20, TEXT_SIZE); wr32(v, 52 + 24, 5); wr32(v, 52 + 28, 0x1000);
    // phdr 1: .rodata (R)
    wr32(v, 84 + 0, 1); wr32(v, 84 + 4, RODATA_OFF); wr32(v, 84 + 8, kTableAddr); wr32(v, 84 + 12, kTableAddr);
    wr32(v, 84 + 16, 0x10); wr32(v, 84 + 20, 0x10); wr32(v, 84 + 24, 4); wr32(v, 84 + 28, 0x1000);

    // .text
    const u32 code[] = {
        0x27BDFFE0, // 0x100000 addiu $sp, $sp, -32
        0x0C040020, // 0x100004 jal 0x100080 (helper)
        0x00000000, // 0x100008 nop (delay slot)
        0x3C030020, // 0x10000C lui $v1, 0x0020
        0x00041080, // 0x100010 sll $v0, $a0, 2
        0x00621821, // 0x100014 addu $v1, $v1, $v0
        0x8C630000, // 0x100018 lw $v1, 0($v1)
        0x00600008, // 0x10001C jr $v1
        0x00000000, // 0x100020 nop (delay slot)
        0x24020001, // 0x100024 case0: addiu $v0, $zero, 1
        0x08040010, // 0x100028 j 0x100040
        0x00000000, // 0x10002C nop
        0x24020002, // 0x100030 case1: addiu $v0, $zero, 2
        0x08040010, // 0x100034 j 0x100040
        0x00000000, // 0x100038 nop
        0x00000000, // 0x10003C nop (padding)
        0x8FBF0010, // 0x100040 lw $ra, 16($sp)
        0x03E00008, // 0x100044 jr $ra
        0x27BD0020, // 0x100048 addiu $sp, $sp, 32
    };
    for (size_t i = 0; i < sizeof(code) / 4; ++i)
        wr32(v, TEXT_OFF + i * 4, code[i]);
    wr32(v, TEXT_OFF + 0x80, 0x03E00008); // helper: jr $ra
    wr32(v, TEXT_OFF + 0x84, 0x00000000); //         nop
    wr32(v, TEXT_OFF + 0xC0, 0x27BDFFF8); // hidden: addiu $sp, $sp, -8
    wr32(v, TEXT_OFF + 0xC4, 0x03E00008); //         jr $ra
    wr32(v, TEXT_OFF + 0xC8, 0x27BD0008); //         addiu $sp, $sp, 8

    // .rodata: jump table
    wr32(v, RODATA_OFF + 0, 0x100024);
    wr32(v, RODATA_OFF + 4, 0x100030);
    wr32(v, RODATA_OFF + 8, 0x100024);
    wr32(v, RODATA_OFF + 12, 0x100030);

    // .strtab / .symtab / .shstrtab
    std::memcpy(&v[STRTAB_OFF], "\0main\0helper", 13);
    wr32(v, SYMTAB_OFF + 16 + 0, 1); wr32(v, SYMTAB_OFF + 16 + 4, kMainAddr); wr32(v, SYMTAB_OFF + 16 + 8, 0x30);
    v[SYMTAB_OFF + 16 + 12] = 0x12; wr16(v, SYMTAB_OFF + 16 + 14, 1);
    wr32(v, SYMTAB_OFF + 32 + 0, 6); wr32(v, SYMTAB_OFF + 32 + 4, kHelperAddr); wr32(v, SYMTAB_OFF + 32 + 8, 8);
    v[SYMTAB_OFF + 32 + 12] = 0x12; wr16(v, SYMTAB_OFF + 32 + 14, 1);
    std::memcpy(&v[SHSTRTAB_OFF], "\0.text\0.rodata\0.symtab\0.strtab\0.shstrtab", 41);

    auto shdr = [&](size_t i, u32 name, u32 type, u32 flags, u32 addr, u32 off, u32 size, u32 link,
                    u32 info, u32 align, u32 entsize) {
        const size_t s = SHOFF + i * 40;
        wr32(v, s + 0, name); wr32(v, s + 4, type); wr32(v, s + 8, flags); wr32(v, s + 12, addr);
        wr32(v, s + 16, off); wr32(v, s + 20, size); wr32(v, s + 24, link); wr32(v, s + 28, info);
        wr32(v, s + 32, align); wr32(v, s + 36, entsize);
    };
    shdr(1, 1, 1, 6, kTextBase, TEXT_OFF, TEXT_SIZE, 0, 0, 4, 0); // .text
    shdr(2, 7, 1, 2, kTableAddr, RODATA_OFF, 0x10, 0, 0, 4, 0);   // .rodata
    shdr(3, 15, 2, 0, 0, SYMTAB_OFF, 48, 4, 1, 4, 16);            // .symtab
    shdr(4, 23, 3, 0, 0, STRTAB_OFF, 13, 0, 0, 1, 0);             // .strtab
    shdr(5, 31, 3, 0, 0, SHSTRTAB_OFF, 41, 0, 0, 1, 0);           // .shstrtab
    return v;
}

} // namespace ee::test

namespace ee::test {

// A minimal "homebrew": main prints via EE STDOUT MMIO, creates a semaphore and
// a thread; the thread prints and signals; main waits, prints, and KExits.
constexpr u32 kHbMain = 0x100000;
constexpr u32 kHbPrint = 0x100100;
constexpr u32 kHbThread = 0x100180;
constexpr u32 kHbData = 0x200000;

inline std::vector<u8> make_homebrew_elf() {
    constexpr size_t TEXT_OFF = 0x100;
    constexpr size_t TEXT_SIZE = 0x200;
    constexpr size_t RODATA_OFF = 0x300;
    constexpr size_t RODATA_SIZE = 0xA0;
    constexpr size_t STRTAB_OFF = 0x3A0;
    constexpr size_t SYMTAB_OFF = 0x3C0;
    constexpr size_t SHSTRTAB_OFF = 0x400;
    constexpr size_t SHOFF = 0x42C;
    constexpr size_t SHNUM = 6;
    constexpr size_t FILE_SIZE = SHOFF + SHNUM * 40;

    std::vector<u8> v(FILE_SIZE, 0);
    v[0] = 0x7F; v[1] = 'E'; v[2] = 'L'; v[3] = 'F'; v[4] = 1; v[5] = 1; v[6] = 1;
    wr16(v, 16, 2); wr16(v, 18, 8); wr32(v, 20, 1); wr32(v, 24, kHbMain);
    wr32(v, 28, 52); wr32(v, 32, SHOFF); wr16(v, 40, 52); wr16(v, 42, 32); wr16(v, 44, 2);
    wr16(v, 46, 40); wr16(v, 48, SHNUM); wr16(v, 50, 5);

    wr32(v, 52 + 0, 1); wr32(v, 52 + 4, TEXT_OFF); wr32(v, 52 + 8, kTextBase); wr32(v, 52 + 12, kTextBase);
    wr32(v, 52 + 16, TEXT_SIZE); wr32(v, 52 + 20, TEXT_SIZE); wr32(v, 52 + 24, 5); wr32(v, 52 + 28, 0x1000);
    wr32(v, 84 + 0, 1); wr32(v, 84 + 4, RODATA_OFF); wr32(v, 84 + 8, kHbData); wr32(v, 84 + 12, kHbData);
    wr32(v, 84 + 16, RODATA_SIZE); wr32(v, 84 + 20, RODATA_SIZE); wr32(v, 84 + 24, 4); wr32(v, 84 + 28, 0x1000);

    // main @ 0x100000
    const u32 main_code[] = {
        0x27BDFFE0, // addiu $sp, $sp, -32
        0xFFBF0010, // sd $ra, 16($sp)
        0x3C050020, // lui $a1, 0x20        (msg1)
        0x0C040040, // jal print
        0x00000000, // nop
        0x3C040020, // lui $a0, 0x20
        0x34840060, // ori $a0, $a0, 0x60   (sema params)
        0x0000100C, // syscall 0x40 (CreateSema)
        0x00408021, // addu $s0, $v0, $zero
        0x3C040020, // lui $a0, 0x20
        0x34840080, // ori $a0, $a0, 0x80   (thread params)
        0x0000080C, // syscall 0x20 (CreateThread)
        0x00408821, // addu $s1, $v0, $zero
        0x02202021, // addu $a0, $s1, $zero
        0x02002821, // addu $a1, $s0, $zero (arg = sema id)
        0x0000088C, // syscall 0x22 (StartThread)
        0x02002021, // addu $a0, $s0, $zero
        0x0000110C, // syscall 0x44 (WaitSema)
        0x3C050020, // lui $a1, 0x20
        0x34A50040, // ori $a1, $a1, 0x40   (msg done)
        0x0C040040, // jal print
        0x00000000, // nop
        0x0000010C, // syscall 4 (KExit)
        0xDFBF0010, // ld $ra, 16($sp)
        0x03E00008, // jr $ra
        0x27BD0020, // addiu $sp, $sp, 32
    };
    for (size_t i = 0; i < sizeof(main_code) / 4; ++i)
        wr32(v, TEXT_OFF + i * 4, main_code[i]);

    // print @ 0x100100 (a1 = string)
    const u32 print_code[] = {
        0x90A20000, // lbu $v0, 0($a1)
        0x10400006, // beq $v0, $zero, +6 (to jr $ra)
        0x3C081000, // lui $t0, 0x1000
        0x3508F180, // ori $t0, $t0, 0xF180  (EE STDOUT)
        0xA1020000, // sb $v0, 0($t0)
        0x24A50001, // addiu $a1, $a1, 1
        0x08040040, // j print
        0x00000000, // nop
        0x03E00008, // jr $ra
        0x00000000, // nop
    };
    for (size_t i = 0; i < sizeof(print_code) / 4; ++i)
        wr32(v, TEXT_OFF + 0x100 + i * 4, print_code[i]);

    // thread_func @ 0x100180 (a0 = sema id)
    const u32 thread_code[] = {
        0x27BDFFF0, // addiu $sp, $sp, -16
        0xFFBF0000, // sd $ra, 0($sp)
        0x00808021, // addu $s0, $a0, $zero
        0x3C050020, // lui $a1, 0x20
        0x34A50020, // ori $a1, $a1, 0x20   (msg thread)
        0x0C040040, // jal print
        0x00000000, // nop
        0x02002021, // addu $a0, $s0, $zero
        0x0000108C, // syscall 0x42 (SignalSema)
        0x000008CC, // syscall 0x23 (ExitThread)
        0xDFBF0000, // ld $ra, 0($sp)
        0x03E00008, // jr $ra
        0x27BD0010, // addiu $sp, $sp, 16
    };
    for (size_t i = 0; i < sizeof(thread_code) / 4; ++i)
        wr32(v, TEXT_OFF + 0x180 + i * 4, thread_code[i]);

    // .rodata
    std::memcpy(&v[RODATA_OFF + 0x00], "main: start\n", 13);
    std::memcpy(&v[RODATA_OFF + 0x20], "thread: hello\n", 15);
    std::memcpy(&v[RODATA_OFF + 0x40], "main: done\n", 12);
    // sema params @ 0x200060: { count=0, max=1, attr=0, option=0 }
    wr32(v, RODATA_OFF + 0x60 + 0, 0);
    wr32(v, RODATA_OFF + 0x60 + 4, 1);
    // thread params @ 0x200080: { status=0, func=thread, stack=0x1003000, gp=0,
    //                           priority=32, stack_size=0x1000, attr=0, option=0 }
    wr32(v, RODATA_OFF + 0x80 + 0, 0);
    wr32(v, RODATA_OFF + 0x80 + 4, kHbThread);
    wr32(v, RODATA_OFF + 0x80 + 8, 0x1003000);
    wr32(v, RODATA_OFF + 0x80 + 12, 0);
    wr32(v, RODATA_OFF + 0x80 + 16, 32);
    wr32(v, RODATA_OFF + 0x80 + 20, 0x1000);

    // symbols
    std::memcpy(&v[STRTAB_OFF], "\0main\0print\0thread_func", 24);
    auto sym = [&](size_t i, u32 name, u32 value, u32 size) {
        const size_t s = SYMTAB_OFF + i * 16;
        wr32(v, s + 0, name); wr32(v, s + 4, value); wr32(v, s + 8, size);
        v[s + 12] = 0x12; wr16(v, s + 14, 1);
    };
    sym(1, 1, kHbMain, 26 * 4);
    sym(2, 6, kHbPrint, 40);
    sym(3, 12, kHbThread, 13 * 4);
    std::memcpy(&v[SHSTRTAB_OFF], "\0.text\0.rodata\0.symtab\0.strtab\0.shstrtab", 41);

    auto shdr = [&](size_t i, u32 name, u32 type, u32 flags, u32 addr, u32 off, u32 size, u32 link,
                    u32 info, u32 align, u32 entsize) {
        const size_t s = SHOFF + i * 40;
        wr32(v, s + 0, name); wr32(v, s + 4, type); wr32(v, s + 8, flags); wr32(v, s + 12, addr);
        wr32(v, s + 16, off); wr32(v, s + 20, size); wr32(v, s + 24, link); wr32(v, s + 28, info);
        wr32(v, s + 32, align); wr32(v, s + 36, entsize);
    };
    shdr(1, 1, 1, 6, kTextBase, TEXT_OFF, TEXT_SIZE, 0, 0, 4, 0);
    shdr(2, 7, 1, 2, kHbData, RODATA_OFF, RODATA_SIZE, 0, 0, 4, 0);
    shdr(3, 15, 2, 0, 0, SYMTAB_OFF, 64, 4, 1, 4, 16);
    shdr(4, 23, 3, 0, 0, STRTAB_OFF, 24, 0, 0, 1, 0);
    shdr(5, 31, 3, 0, 0, SHSTRTAB_OFF, 41, 0, 0, 1, 0);
    return v;
}

} // namespace ee::test
