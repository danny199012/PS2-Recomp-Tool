// SPDX-License-Identifier: GPL-3.0-only
#include <ee/elf.hpp>
#include <ee/r5900.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s <file.elf> [options]\n", argv0);
    std::printf("  --section NAME     disassemble only this section\n");
    std::printf("  --start A --end B  disassemble guest address range [A, B)\n");
    std::printf("  --no-disasm        print ELF info only\n");
}

ee::u32 parse_u32(const char* s) {
    return static_cast<ee::u32>(std::strtoul(s, nullptr, 0));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    std::string path;
    std::string section_name;
    ee::u32 range_start = 0, range_end = 0;
    bool have_range = false;
    bool no_disasm = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--section" && i + 1 < argc) {
            section_name = argv[++i];
        } else if (arg == "--start" && i + 1 < argc) {
            range_start = parse_u32(argv[++i]);
            have_range = true;
        } else if (arg == "--end" && i + 1 < argc) {
            range_end = parse_u32(argv[++i]);
        } else if (arg == "--no-disasm") {
            no_disasm = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return 1;
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        usage(argv[0]);
        return 1;
    }

    std::string error;
    auto image = ee::elf::Image::load_file(path, &error);
    if (!image) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }

    std::printf("file:     %s\n", path.c_str());
    std::printf("type:     %u  machine: %u%s\n", unsigned(image->type()), unsigned(image->machine()),
                image->is_mips() ? " (MIPS)" : "");
    std::printf("entry:    0x%08X\n", image->entry());
    std::printf("segments: %zu\n", image->segments().size());
    for (const auto& seg : image->segments()) {
        if (seg.type != ee::elf::PT_LOAD)
            continue;
        std::printf("  LOAD  off=0x%08X vaddr=0x%08X filesz=0x%08X memsz=0x%08X flags=%c%c%c\n",
                    seg.offset, seg.vaddr, seg.filesz, seg.memsz,
                    (seg.flags & 4) ? 'r' : '-', (seg.flags & 2) ? 'w' : '-', (seg.flags & 1) ? 'x' : '-');
    }
    std::printf("sections: %zu\n", image->sections().size());
    for (const auto& sec : image->sections()) {
        if (sec.name.empty())
            continue;
        std::printf("  %-16s addr=0x%08X size=0x%08X %c%c%c\n", sec.name.c_str(), sec.addr, sec.size,
                    sec.allocated() ? 'a' : '-', sec.writable() ? 'w' : '-', sec.executable() ? 'x' : '-');
    }
    size_t funcs = 0;
    for (const auto& sym : image->symbols())
        if (sym.is_function())
            ++funcs;
    std::printf("symbols:  %zu (%zu functions)\n", image->symbols().size(), funcs);

    if (no_disasm)
        return 0;

    // Label map from symbols.
    std::map<ee::u32, std::string> labels;
    for (const auto& sym : image->symbols())
        if (!sym.name.empty() && sym.value != 0)
            labels.emplace(sym.value, sym.name);

    // Decide what to disassemble.
    struct Range {
        ee::u32 start, end;
        std::string tag;
    };
    std::vector<Range> ranges;
    if (have_range && range_end > range_start) {
        ranges.push_back({range_start, range_end, "address range"});
    } else if (!section_name.empty()) {
        const ee::elf::Section* sec = image->find_section(section_name);
        if (!sec) {
            std::fprintf(stderr, "error: section not found: %s\n", section_name.c_str());
            return 1;
        }
        ranges.push_back({sec->addr, sec->addr + sec->size, sec->name});
    } else {
        for (const auto& sec : image->sections())
            if (sec.executable() && sec.size != 0)
                ranges.push_back({sec.addr, sec.addr + sec.size, sec.name});
    }

    for (const Range& range : ranges) {
        std::printf("\ndisassembly of %s [0x%08X, 0x%08X):\n", range.tag.c_str(), range.start, range.end);
        for (ee::u32 va = range.start; va < range.end; va += 4) {
            auto word = image->read_u32(va);
            if (!word) {
                std::printf("  %08X: <unmapped>\n", va);
                continue;
            }
            if (auto it = labels.find(va); it != labels.end())
                std::printf("\n%s:\n", it->second.c_str());
            const ee::r5900::Instruction insn = ee::r5900::decode(*word);
            std::string text = ee::r5900::disassemble(insn, va);
            // Annotate direct branch/jump targets with labels when known.
            if (insn.is_branch() || insn.op == ee::r5900::Op::J || insn.op == ee::r5900::Op::Jal) {
                const ee::u32 tgt = insn.is_branch() ? ee::r5900::branch_target(insn, va)
                                                     : ee::r5900::jump_target(insn, va);
                if (auto it = labels.find(tgt); it != labels.end())
                    text += "  <" + it->second + ">";
            }
            std::printf("  %08X: %08X  %s\n", va, *word, text.c_str());
        }
    }
    return 0;
}
