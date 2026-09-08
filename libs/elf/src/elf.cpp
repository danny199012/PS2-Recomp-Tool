// SPDX-License-Identifier: GPL-3.0-only
#include <ee/elf.hpp>

#include <fstream>

namespace ee::elf {
namespace {

u16 rd16(const u8* p) {
    return static_cast<u16>(u16(p[0]) | (u16(p[1]) << 8));
}

u32 rd32(const u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

void set_error(std::string* error, const char* message) {
    if (error)
        *error = message;
}

// Bounded string read from a string table.
std::string safe_str(const u8* base, u32 max) {
    u32 len = 0;
    while (len < max && base[len] != 0)
        ++len;
    return std::string(reinterpret_cast<const char*>(base), len);
}

} // namespace

std::optional<Image> Image::load_file(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        set_error(error, "cannot open file");
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        set_error(error, "failed to read file");
        return std::nullopt;
    }
    return load_bytes(std::move(bytes), error);
}

std::optional<Image> Image::load_bytes(std::vector<u8> bytes, std::string* error) {
    Image image;
    image.m_bytes = std::move(bytes);
    if (!image.parse(error))
        return std::nullopt;
    return image;
}

bool Image::parse(std::string* error) {
    const std::vector<u8>& b = m_bytes;
    if (b.size() < 52) {
        set_error(error, "file too small for ELF header");
        return false;
    }
    if (b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') {
        set_error(error, "bad ELF magic");
        return false;
    }
    if (b[4] != 1) {
        set_error(error, "not a 32-bit ELF");
        return false;
    }
    if (b[5] != 1) {
        set_error(error, "not a little-endian ELF");
        return false;
    }

    m_type = rd16(&b[16]);
    m_machine = rd16(&b[18]);
    m_entry = rd32(&b[24]);
    const u32 phoff = rd32(&b[28]);
    const u32 shoff = rd32(&b[32]);
    const u16 phentsize = rd16(&b[42]);
    const u16 phnum = rd16(&b[44]);
    const u16 shentsize = rd16(&b[46]);
    const u16 shnum = rd16(&b[48]);
    const u16 shstrndx = rd16(&b[50]);

    // Program headers.
    if (phnum != 0 && phoff != 0) {
        if (phentsize < 32 || u64(phoff) + u64(phentsize) * phnum > b.size()) {
            set_error(error, "program headers out of range");
            return false;
        }
        for (u16 i = 0; i < phnum; ++i) {
            const u8* p = &b[phoff + u32(i) * phentsize];
            Segment seg;
            seg.type = rd32(p + 0);
            seg.offset = rd32(p + 4);
            seg.vaddr = rd32(p + 8);
            seg.paddr = rd32(p + 12);
            seg.filesz = rd32(p + 16);
            seg.memsz = rd32(p + 20);
            seg.flags = rd32(p + 24);
            seg.align = rd32(p + 28);
            m_segments.push_back(seg);
        }
    }

    // Section headers (+ names via shstrtab).
    if (shnum != 0 && shoff != 0) {
        if (shentsize < 40 || u64(shoff) + u64(shentsize) * shnum > b.size()) {
            set_error(error, "section headers out of range");
            return false;
        }
        const u8* shstrtab = nullptr;
        u32 shstrtab_size = 0;
        if (shstrndx < shnum) {
            const u8* s = &b[shoff + u32(shstrndx) * shentsize];
            const u32 off = rd32(s + 16);
            shstrtab_size = rd32(s + 20);
            if (u64(off) + shstrtab_size <= b.size())
                shstrtab = &b[off];
        }
        for (u16 i = 0; i < shnum; ++i) {
            const u8* s = &b[shoff + u32(i) * shentsize];
            Section sec;
            const u32 name_off = rd32(s + 0);
            sec.type = rd32(s + 4);
            sec.flags = rd32(s + 8);
            sec.addr = rd32(s + 12);
            sec.offset = rd32(s + 16);
            sec.size = rd32(s + 20);
            sec.link = rd32(s + 24);
            sec.info = rd32(s + 28);
            sec.addralign = rd32(s + 32);
            sec.entsize = rd32(s + 36);
            if (shstrtab && name_off < shstrtab_size)
                sec.name = safe_str(shstrtab + name_off, shstrtab_size - name_off);
            m_sections.push_back(std::move(sec));
        }

        // Symbol tables (SHT_SYMTAB with linked string table).
        for (const Section& sec : m_sections) {
            if (sec.type != SHT_SYMTAB || sec.entsize < 16)
                continue;
            if (sec.link >= m_sections.size())
                continue;
            const Section& strsec = m_sections[sec.link];
            if (u64(sec.offset) + sec.size > b.size() || u64(strsec.offset) + strsec.size > b.size())
                continue;
            const u8* strtab = &b[strsec.offset];
            for (u32 off = 0; off + 16 <= sec.size; off += sec.entsize) {
                const u8* p = &b[sec.offset + off];
                Symbol sym;
                const u32 name_off = rd32(p + 0);
                sym.value = rd32(p + 4);
                sym.size = rd32(p + 8);
                sym.info = p[12];
                sym.other = p[13];
                sym.shndx = rd16(p + 14);
                if (name_off < strsec.size)
                    sym.name = safe_str(strtab + name_off, strsec.size - name_off);
                m_symbols.push_back(std::move(sym));
            }
        }
    }
    return true;
}

const Section* Image::find_section(const std::string& name) const {
    for (const Section& sec : m_sections)
        if (sec.name == name)
            return &sec;
    return nullptr;
}

const Symbol* Image::find_symbol(const std::string& name) const {
    for (const Symbol& sym : m_symbols)
        if (sym.name == name)
            return &sym;
    return nullptr;
}

const Symbol* Image::symbol_at(u32 addr) const {
    for (const Symbol& sym : m_symbols)
        if (sym.value == addr && !sym.name.empty())
            return &sym;
    return nullptr;
}

bool Image::contains(u32 vaddr) const {
    for (const Segment& seg : m_segments)
        if (seg.type == PT_LOAD && vaddr >= seg.vaddr && vaddr - seg.vaddr < seg.filesz)
            return true;
    return false;
}

std::optional<u32> Image::read_u32(u32 vaddr) const {
    for (const Segment& seg : m_segments) {
        if (seg.type != PT_LOAD)
            continue;
        if (vaddr >= seg.vaddr && u64(vaddr - seg.vaddr) + 4 <= seg.filesz) {
            const u64 off = u64(seg.offset) + (vaddr - seg.vaddr);
            if (off + 4 <= m_bytes.size())
                return rd32(&m_bytes[static_cast<size_t>(off)]);
        }
    }
    return std::nullopt;
}

std::vector<u8> Image::read_bytes(u32 vaddr, u32 count) const {
    std::vector<u8> out;
    for (const Segment& seg : m_segments) {
        if (seg.type != PT_LOAD)
            continue;
        if (vaddr >= seg.vaddr && u64(vaddr - seg.vaddr) + count <= seg.filesz) {
            const u64 off = u64(seg.offset) + (vaddr - seg.vaddr);
            if (off + count <= m_bytes.size()) {
                const auto begin = m_bytes.begin() + static_cast<std::ptrdiff_t>(off);
                out.assign(begin, begin + static_cast<std::ptrdiff_t>(count));
                return out;
            }
        }
    }
    return out;
}

} // namespace ee::elf

