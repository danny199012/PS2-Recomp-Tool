// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ee::elf {

// ELF32 constants (subset relevant to PS2 executables).
inline constexpr u16 ET_EXEC = 2;
inline constexpr u16 EM_MIPS = 8;
inline constexpr u32 PT_LOAD = 1;
inline constexpr u32 SHT_SYMTAB = 2;
inline constexpr u32 SHF_WRITE = 0x1;
inline constexpr u32 SHF_ALLOC = 0x2;
inline constexpr u32 SHF_EXECINSTR = 0x4;
inline constexpr u8 STT_NOTYPE = 0;
inline constexpr u8 STT_OBJECT = 1;
inline constexpr u8 STT_FUNC = 2;

struct Section {
    std::string name;
    u32 type = 0;
    u32 flags = 0;
    u32 addr = 0;   // guest virtual address
    u32 offset = 0; // file offset
    u32 size = 0;
    u32 link = 0;
    u32 info = 0;
    u32 addralign = 0;
    u32 entsize = 0;

    bool allocated() const { return (flags & SHF_ALLOC) != 0; }
    bool executable() const { return (flags & SHF_EXECINSTR) != 0; }
    bool writable() const { return (flags & SHF_WRITE) != 0; }
};

struct Symbol {
    std::string name;
    u32 value = 0;
    u32 size = 0;
    u8 info = 0;
    u8 other = 0;
    u16 shndx = 0;

    u8 type() const { return info & 0xF; }
    u8 binding() const { return info >> 4; }
    bool is_function() const { return type() == STT_FUNC; }
};

struct Segment {
    u32 type = 0;
    u32 offset = 0;
    u32 vaddr = 0;
    u32 paddr = 0;
    u32 filesz = 0;
    u32 memsz = 0;
    u32 flags = 0;
    u32 align = 0;
};

// A parsed ELF32 image. PS2 executables are 32-bit little-endian MIPS ELFs.
class Image {
public:
    static std::optional<Image> load_file(const std::string& path, std::string* error = nullptr);
    static std::optional<Image> load_bytes(std::vector<u8> bytes, std::string* error = nullptr);

    u32 entry() const { return m_entry; }
    u16 machine() const { return m_machine; }
    u16 type() const { return m_type; }
    bool is_mips() const { return m_machine == EM_MIPS; }

    const std::vector<Section>& sections() const { return m_sections; }
    const std::vector<Segment>& segments() const { return m_segments; }
    const std::vector<Symbol>& symbols() const { return m_symbols; }

    const Section* find_section(const std::string& name) const;
    const Symbol* find_symbol(const std::string& name) const;
    const Symbol* symbol_at(u32 addr) const; // exact-address match (e.g. function starts)

    // Guest-address reads via PT_LOAD segments.
    bool contains(u32 vaddr) const;
    std::optional<u32> read_u32(u32 vaddr) const;
    std::vector<u8> read_bytes(u32 vaddr, u32 count) const;

private:
    bool parse(std::string* error);

    std::vector<u8> m_bytes;
    u32 m_entry = 0;
    u16 m_machine = 0;
    u16 m_type = 0;
    std::vector<Section> m_sections;
    std::vector<Segment> m_segments;
    std::vector<Symbol> m_symbols;
};

} // namespace ee::elf
