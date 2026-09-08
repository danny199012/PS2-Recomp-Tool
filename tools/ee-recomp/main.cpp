// SPDX-License-Identifier: GPL-3.0-only
#include <ee/analysis.hpp>
#include <ee/codegen.hpp>
#include <ee/elf.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s <file.elf> [options]\n", argv0);
    std::printf("  --config FILE    PS2Recomp-compatible TOML config (stubs/skip/patches)\n");
    std::printf("  --import FILE    import function names (.csv or .json); repeatable\n");
    std::printf("  --out FILE       output C++ file (default: <elf>.recomp.cpp)\n");
    std::printf("  --no-scan        disable the prologue scan\n");
    std::printf("  --no-comments    omit address/disassembly comments\n");
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    std::string path, config_path, out_path;
    std::vector<std::string> imports;
    ee::analysis::Options analyze_opt;
    ee::codegen::Options codegen_opt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--import" && i + 1 < argc) {
            imports.emplace_back(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--no-scan") {
            analyze_opt.prologue_scan = false;
        } else if (arg == "--no-comments") {
            codegen_opt.emit_comments = false;
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
    if (out_path.empty())
        out_path = path + ".recomp.cpp";

    std::string error;
    auto image = ee::elf::Image::load_file(path, &error);
    if (!image) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }

    ee::codegen::Config cfg;
    if (!config_path.empty()) {
        std::string text;
        if (!read_file(config_path, text)) {
            std::fprintf(stderr, "error: cannot read config %s\n", config_path.c_str());
            return 1;
        }
        auto parsed = ee::codegen::Config::from_toml(text, &error);
        if (!parsed) {
            std::fprintf(stderr, "error: bad config %s: %s\n", config_path.c_str(), error.c_str());
            return 1;
        }
        cfg = std::move(*parsed);
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
        imported.insert(m.begin(), m.end());
    }

    const ee::analysis::Result res = ee::analysis::analyze(*image, analyze_opt, imported);
    std::fprintf(stderr, "ee-recomp: %zu functions, %zu jump tables, %zu unresolved indirects\n",
                 res.functions.size(), res.jump_tables.size(), res.unresolved_indirects.size());

    const std::string module = ee::codegen::emit_module(*image, res, cfg, codegen_opt);

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
        return 1;
    }
    out << module;
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", out_path.c_str(), module.size());
    return 0;
}
