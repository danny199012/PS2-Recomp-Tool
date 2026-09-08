// SPDX-License-Identifier: GPL-3.0-only
#include <ee/elf.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

using ee::u16;
using ee::u32;
using ee::u8;

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

namespace {

void wr16(std::vector<u8>& v, size_t off, u16 x) {
    v[off] = u8(x);
    v[off + 1] = u8(x >> 8);
}

void wr32(std::vector<u8>& v, size_t off, u32 x) {
    v[off] = u8(x);
    v[off + 1] = u8(x >> 8);
    v[off + 2] = u8(x >> 16);
    v[off + 3] = u8(x >> 24);
}

// Minimal but valid ELF32 MIPS executable:
//   .text @0x100000 (nop; jr ra), .symtab (null + main), .strtab, .shstrtab
std::vector<u8> make_test_elf() {
    constexpr size_t TEXT_OFF = 0x100;
    constexpr size_t STRTAB_OFF = 0x108;   // "\0main\0" (6 bytes)
    constexpr size_t SYMTAB_OFF = 0x110;   // 2 * 16 bytes
    constexpr size_t SHSTRTAB_OFF = 0x130; // 33 bytes
    constexpr size_t SHOFF = 0x154;
    constexpr size_t SHNUM = 5;
    constexpr size_t FILE_SIZE = SHOFF + SHNUM * 40;

    std::vector<u8> v(FILE_SIZE, 0);

    // ELF header.
    v[0] = 0x7F;
    v[1] = 'E';
    v[2] = 'L';
    v[3] = 'F';
    v[4] = 1; // 32-bit
    v[5] = 1; // little-endian
    v[6] = 1; // version
    wr16(v, 16, 2);        // e_type = ET_EXEC
    wr16(v, 18, 8);        // e_machine = EM_MIPS
    wr32(v, 20, 1);        // e_version
    wr32(v, 24, 0x100000); // e_entry
    wr32(v, 28, 52);       // e_phoff
    wr32(v, 32, SHOFF);    // e_shoff
    wr16(v, 40, 52);       // e_ehsize
    wr16(v, 42, 32);       // e_phentsize
    wr16(v, 44, 1);        // e_phnum
    wr16(v, 46, 40);       // e_shentsize
    wr16(v, 48, SHNUM);    // e_shnum
    wr16(v, 50, 4);        // e_shstrndx

    // Program header (PT_LOAD covering .text).
    wr32(v, 52 + 0, 1);         // p_type = PT_LOAD
    wr32(v, 52 + 4, TEXT_OFF);  // p_offset
    wr32(v, 52 + 8, 0x100000);  // p_vaddr
    wr32(v, 52 + 12, 0x100000); // p_paddr
    wr32(v, 52 + 16, 8);        // p_filesz
    wr32(v, 52 + 20, 8);        // p_memsz
    wr32(v, 52 + 24, 5);        // p_flags = R+X
    wr32(v, 52 + 28, 0x1000);   // p_align

    // .text: nop; jr ra
    wr32(v, TEXT_OFF + 0, 0x00000000);
    wr32(v, TEXT_OFF + 4, 0x03E00008);

    // .strtab
    std::memcpy(&v[STRTAB_OFF], "\0main", 6);

    // .symtab: [0] = null, [1] = main
    wr32(v, SYMTAB_OFF + 16 + 0, 1);        // st_name = "main"
    wr32(v, SYMTAB_OFF + 16 + 4, 0x100000); // st_value
    wr32(v, SYMTAB_OFF + 16 + 8, 8);        // st_size
    v[SYMTAB_OFF + 16 + 12] = 0x12;         // st_info = STB_GLOBAL | STT_FUNC
    wr16(v, SYMTAB_OFF + 16 + 14, 1);       // st_shndx = .text

    // .shstrtab
    std::memcpy(&v[SHSTRTAB_OFF], "\0.text\0.symtab\0.strtab\0.shstrtab", 33);

    // Section headers ([0] stays all-zero).
    auto shdr = [&](size_t i, u32 name, u32 type, u32 flags, u32 addr, u32 off, u32 size, u32 link,
                    u32 info, u32 align, u32 entsize) {
        const size_t s = SHOFF + i * 40;
        wr32(v, s + 0, name);
        wr32(v, s + 4, type);
        wr32(v, s + 8, flags);
        wr32(v, s + 12, addr);
        wr32(v, s + 16, off);
        wr32(v, s + 20, size);
        wr32(v, s + 24, link);
        wr32(v, s + 28, info);
        wr32(v, s + 32, align);
        wr32(v, s + 36, entsize);
    };
    shdr(1, 1, 1, 6, 0x100000, TEXT_OFF, 8, 0, 0, 4, 0);  // .text PROGBITS ALLOC|EXEC
    shdr(2, 7, 2, 0, 0, SYMTAB_OFF, 32, 3, 1, 4, 16);     // .symtab
    shdr(3, 15, 3, 0, 0, STRTAB_OFF, 6, 0, 0, 1, 0);      // .strtab
    shdr(4, 23, 3, 0, 0, SHSTRTAB_OFF, 33, 0, 0, 1, 0);   // .shstrtab
    return v;
}

} // namespace

int main() {
    auto image = ee::elf::Image::load_bytes(make_test_elf());
    CHECK(image.has_value());
    if (!image)
        return 1;
    CHECK(image->entry() == 0x100000);
    CHECK(image->is_mips());
    CHECK(image->type() == ee::elf::ET_EXEC);
    CHECK(image->sections().size() == 5);
    CHECK(image->segments().size() == 1);

    const ee::elf::Section* text = image->find_section(".text");
    CHECK(text != nullptr);
    if (text) {
        CHECK(text->addr == 0x100000);
        CHECK(text->size == 8);
        CHECK(text->executable());
        CHECK(text->allocated());
        CHECK(!text->writable());
    }
    CHECK(image->find_section(".data") == nullptr);

    // Symbols.
    CHECK(image->symbols().size() == 2);
    const ee::elf::Symbol* main_sym = image->find_symbol("main");
    CHECK(main_sym != nullptr);
    if (main_sym) {
        CHECK(main_sym->value == 0x100000);
        CHECK(main_sym->is_function());
    }
    const ee::elf::Symbol* at = image->symbol_at(0x100000);
    CHECK(at != nullptr && at->name == "main");
    CHECK(image->symbol_at(0x100004) == nullptr);

    // Guest memory reads via PT_LOAD.
    CHECK(image->contains(0x100000));
    CHECK(!image->contains(0x200000));
    auto w0 = image->read_u32(0x100000);
    auto w1 = image->read_u32(0x100004);
    CHECK(w0.has_value() && *w0 == 0x00000000);
    CHECK(w1.has_value() && *w1 == 0x03E00008);
    CHECK(!image->read_u32(0x200000).has_value());

    // Rejections.
    CHECK(!ee::elf::Image::load_bytes({}).has_value());
    std::vector<u8> bad_magic(52, 0);
    bad_magic[0] = 'N';
    CHECK(!ee::elf::Image::load_bytes(bad_magic).has_value());
    auto bad_class = make_test_elf();
    bad_class[4] = 2; // 64-bit
    CHECK(!ee::elf::Image::load_bytes(bad_class).has_value());
    auto bad_endian = make_test_elf();
    bad_endian[5] = 2; // big-endian
    CHECK(!ee::elf::Image::load_bytes(bad_endian).has_value());

    std::printf("test_elf: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
