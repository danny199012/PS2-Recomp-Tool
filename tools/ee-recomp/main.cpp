// SPDX-License-Identifier: GPL-3.0-only
#include <ee/analysis.hpp>
#include <ee/codegen.hpp>
#include <ee/elf.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s <file.elf> [options]\n", argv0);
    std::printf("  --config FILE      PS2Recomp-compatible TOML config (stubs/skip/patches)\n");
    std::printf("  --import FILE      import function names (.csv or .json); repeatable\n");
    std::printf("  --out FILE         output C++ file (default: <elf>.recomp.cpp)\n");
    std::printf("  --multi-file DIR   multi-file output: split into <base>.recomp.{h,0.cpp,...}\n");
    std::printf("                      in DIR (avoids a single un-compilable multi-hundred-MB .cpp)\n");
    std::printf("  --per-file N       functions per .cpp in multi-file mode (default: 500)\n");
    std::printf("  --base NAME        output base name in multi-file mode (default: <elf leaf>)\n");
    std::printf("  --no-scan          disable the prologue scan\n");
    std::printf("  --no-comments      omit address/disassembly comments\n");
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
    std::string path, config_path, out_path, multi_file_dir, base_name;
    std::vector<std::string> imports;
    ee::analysis::Options analyze_opt;
    ee::codegen::Options codegen_opt;
    bool multi_file = false;
    size_t per_file = 500;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--import" && i + 1 < argc) {
            imports.emplace_back(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--multi-file" && i + 1 < argc) {
            multi_file = true;
            multi_file_dir = argv[++i];
        } else if (arg == "--per-file" && i + 1 < argc) {
            per_file = size_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--base" && i + 1 < argc) {
            base_name = argv[++i];
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

    if (multi_file) {
        codegen_opt.multi_file = true;
        codegen_opt.functions_per_file = per_file;
        if (base_name.empty()) {
            // Derive from the ELF leaf name (strip extension).
            std::string leaf = path;
            const size_t slash = leaf.find_last_of("/\\");
            if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
            const size_t dot = leaf.find_last_of('.');
            if (dot != std::string::npos) leaf = leaf.substr(0, dot);
            base_name = leaf;
        }
        codegen_opt.output_base = base_name;
        const ee::codegen::MultiFileResult mf =
            ee::codegen::emit_module_multi(*image, res, cfg, codegen_opt);

        // Ensure the output directory exists.
        if (!multi_file_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(multi_file_dir, ec);
        }
        auto join = [&](const std::string& fname) {
            return multi_file_dir.empty() ? fname
                                          : (multi_file_dir + "/" + fname);
        };
        // Write the header.
        const std::string hdr_path = join(base_name + ".recomp.h");
        std::ofstream hdr_out(hdr_path, std::ios::binary);
        if (!hdr_out) {
            std::fprintf(stderr, "error: cannot write %s\n", hdr_path.c_str());
            return 1;
        }
        hdr_out << mf.header;
        std::fprintf(stderr, "wrote %s (%zu bytes)\n", hdr_path.c_str(), mf.header.size());
        // Write each .cpp.
        for (const auto& [fname, content] : mf.files) {
            const std::string fpath = join(fname);
            std::ofstream f(fpath, std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "error: cannot write %s\n", fpath.c_str());
                return 1;
            }
            f << content;
            std::fprintf(stderr, "wrote %s (%zu bytes)\n", fpath.c_str(), content.size());
        }
        std::fprintf(stderr, "multi-file output: %zu files (1 header + %zu source)\n",
                     mf.files.size() + 1, mf.files.size());
    } else {
        if (out_path.empty())
            out_path = path + ".recomp.cpp";
        const std::string module = ee::codegen::emit_module(*image, res, cfg, codegen_opt);
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
            return 1;
        }
        out << module;
        std::fprintf(stderr, "wrote %s (%zu bytes)\n", out_path.c_str(), module.size());
    }
    return 0;
}
