// SPDX-License-Identifier: GPL-3.0-only
#include <ee/analysis.hpp>
#include <ee/elf.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s <file.elf> [options]\n", argv0);
    std::printf("  --import FILE    import function names (.csv or .json); repeatable\n");
    std::printf("  --no-scan        disable the prologue scan\n");
    std::printf("  --toml FILE      write PS2Recomp-compatible config TOML\n");
    std::printf("  --csv FILE       write functions CSV (Ghidra-style interchange)\n");
    std::printf("  --json FILE      write functions JSON (Aura-style interchange)\n");
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << data;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    std::string path;
    std::vector<std::string> imports;
    std::string toml_out, csv_out, json_out;
    ee::analysis::Options opt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--import" && i + 1 < argc) {
            imports.emplace_back(argv[++i]);
        } else if (arg == "--toml" && i + 1 < argc) {
            toml_out = argv[++i];
        } else if (arg == "--csv" && i + 1 < argc) {
            csv_out = argv[++i];
        } else if (arg == "--json" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (arg == "--no-scan") {
            opt.prologue_scan = false;
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

    std::map<ee::u32, std::string> imported;
    for (const std::string& imp : imports) {
        std::string text;
        if (!read_file(imp, text)) {
            std::fprintf(stderr, "error: cannot read %s\n", imp.c_str());
            return 1;
        }
        std::map<ee::u32, std::string> m =
            (imp.size() >= 5 && imp.substr(imp.size() - 5) == ".json") ? ee::analysis::import_json(text)
                                                                      : ee::analysis::import_csv(text);
        std::fprintf(stderr, "imported %zu names from %s\n", m.size(), imp.c_str());
        imported.insert(m.begin(), m.end());
    }

    const ee::analysis::Result res = ee::analysis::analyze(*image, opt, imported);

    std::printf("functions:  %zu\n", res.functions.size());
    size_t by_source[5] = {};
    for (const auto& f : res.functions)
        ++by_source[size_t(f.source) <= 4 ? size_t(f.source) : 4];
    std::printf("  entry=%zu symbol=%zu import=%zu call=%zu prologue=%zu\n", by_source[0], by_source[1],
                by_source[2], by_source[3], by_source[4]);
    std::printf("jump tables: %zu\n", res.jump_tables.size());
    std::printf("unresolved indirect sites: %zu\n", res.unresolved_indirects.size());
    for (ee::u32 addr : res.unresolved_indirects)
        std::printf("  indirect @ 0x%08X\n", addr);

    if (!toml_out.empty() && !write_file(toml_out, ee::analysis::export_toml(res, *image))) {
        std::fprintf(stderr, "error: cannot write %s\n", toml_out.c_str());
        return 1;
    }
    if (!csv_out.empty() && !write_file(csv_out, ee::analysis::export_csv(res))) {
        std::fprintf(stderr, "error: cannot write %s\n", csv_out.c_str());
        return 1;
    }
    if (!json_out.empty() && !write_file(json_out, ee::analysis::export_json(res))) {
        std::fprintf(stderr, "error: cannot write %s\n", json_out.c_str());
        return 1;
    }
    return 0;
}
