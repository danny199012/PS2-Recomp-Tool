// SPDX-License-Identifier: GPL-3.0-only
//
// ee-tools: GUI for the ee-disasm / ee-analyze / ee-recomp toolchain.
// Same functionality as the three command-line tools, in a single tabbed
// window (ELF info, disassembly, analysis, recompilation). The CLI tools
// remain for scripting.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ee/analysis.hpp>
#include <ee/codegen.hpp>
#include <ee/elf.hpp>
#include <ee/r5900.hpp>

#include <SDL3/SDL.h>

#ifdef EE_HAS_IMGUI
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../ui_filebrowser.hpp"
#endif

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using ee::u8;
using ee::u32;
using ee::s32;

// --- generic helpers ----------------------------------------------------------

void logf(std::string& out, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out += buf;
    out += '\n';
    if (out.size() > (1u << 20)) out.erase(0, out.size() - (1u << 20));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << data;
    return true;
}

#ifdef EE_HAS_IMGUI
// Larger UI font (same approach as ee-studio / game-launcher).
void enlarge_ui_font(ImGuiIO& io) {
    const char* candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    for (const char* f : candidates) {
        std::FILE* fp = std::fopen(f, "rb");
        if (!fp) continue;
        std::fclose(fp);
        io.Fonts->AddFontFromFileTTF(f, 24.0f);
        return;
    }
    io.FontGlobalScale = 1.6f;
}
#endif // EE_HAS_IMGUI

// --- tool state --------------------------------------------------------------

struct ToolState {
    // ELF
    std::string elf_path;
    std::optional<ee::elf::Image> image;
    std::string elf_info; // prebuilt header/segment/section dump
    std::string load_error;

    // Disassemble
    std::string section_name;
    bool use_range = false;
    ee::u32 range_start = 0x00100000;
    ee::u32 range_end = 0x00110000;
    char start_buf[16] = "00100000";
    char end_buf[16] = "00110000";

    // Analyze
    std::vector<std::string> imports;
    char import_buf[512] = {};
    bool prologue_scan = true;
    char toml_out_buf[512] = {};
    char csv_out_buf[512] = {};
    char json_out_buf[512] = {};

    // Recompile
    char config_buf[512] = {};
    char recomp_out_buf[512] = {};
    bool emit_comments = true;

    // PS2 executables often have no .elf extension (e.g. "SLUS_210.66");
    // default to showing all files so those are selectable.
    bool browse_all_files = true;
};

// Load the ELF and rebuild the info dump (mirrors ee-disasm header output).
bool load_elf(ToolState& st, std::string& log) {
    st.image.reset();
    st.elf_info.clear();
    if (st.elf_path.empty()) {
        logf(log, "no ELF file selected");
        return false;
    }
    std::string err;
    auto img = ee::elf::Image::load_file(st.elf_path, &err);
    if (!img) {
        logf(log, "error: %s: %s", st.elf_path.c_str(), err.c_str());
        return false;
    }
    st.image = std::move(*img);
    const ee::elf::Image& image = *st.image;

    char b[128];
    std::ostringstream o;
    o << "file:     " << st.elf_path << "\n";
    std::snprintf(b, sizeof b, "type:     %u  machine: %u%s\n", unsigned(image.type()),
                  unsigned(image.machine()), image.is_mips() ? " (MIPS)" : "");
    o << b;
    std::snprintf(b, sizeof b, "entry:    0x%08X\n", image.entry());
    o << b;
    std::snprintf(b, sizeof b, "segments: %zu\n", image.segments().size());
    o << b;
    for (const auto& seg : image.segments()) {
        if (seg.type != ee::elf::PT_LOAD) continue;
        std::snprintf(b, sizeof b,
                      "  LOAD  off=0x%08X vaddr=0x%08X filesz=0x%08X memsz=0x%08X flags=%c%c%c\n",
                      seg.offset, seg.vaddr, seg.filesz, seg.memsz,
                      (seg.flags & 4) ? 'r' : '-', (seg.flags & 2) ? 'w' : '-',
                      (seg.flags & 1) ? 'x' : '-');
        o << b;
    }
    std::snprintf(b, sizeof b, "sections: %zu\n", image.sections().size());
    o << b;
    for (const auto& sec : image.sections()) {
        if (sec.name.empty()) continue;
        std::snprintf(b, sizeof b, "  %-16s addr=0x%08X size=0x%08X %c%c%c\n", sec.name.c_str(),
                      sec.addr, sec.size, sec.allocated() ? 'a' : '-',
                      sec.writable() ? 'w' : '-', sec.executable() ? 'x' : '-');
        o << b;
    }
    size_t funcs = 0;
    for (const auto& sym : image.symbols())
        if (sym.is_function()) ++funcs;
    std::snprintf(b, sizeof b, "symbols:  %zu (%zu functions)\n", image.symbols().size(), funcs);
    o << b;
    st.elf_info = o.str();
    logf(log, "loaded %s", st.elf_path.c_str());
    return true;
}

// Disassemble the loaded ELF (mirrors ee-disasm).
void do_disasm(ToolState& st, std::string& log) {
    if (!st.image) {
        logf(log, "load an ELF first");
        return;
    }
    const ee::elf::Image& image = *st.image;
    logf(log, "--- disassembly ---");
    const bool have_range = st.use_range && st.range_end > st.range_start;

    // Label map from symbols.
    std::map<ee::u32, std::string> labels;
    for (const auto& sym : image.symbols())
        if (!sym.name.empty() && sym.value != 0) labels.emplace(sym.value, sym.name);

    struct Range {
        ee::u32 start, end;
        std::string tag;
    };
    std::vector<Range> ranges;
    if (have_range) {
        ranges.push_back({st.range_start, st.range_end, "address range"});
    } else if (!st.section_name.empty()) {
        const ee::elf::Section* sec = image.find_section(st.section_name);
        if (!sec) {
            logf(log, "error: section not found: %s", st.section_name.c_str());
            return;
        }
        ranges.push_back({sec->addr, sec->addr + sec->size, sec->name});
    } else {
        for (const auto& sec : image.sections())
            if (sec.executable() && sec.size != 0)
                ranges.push_back({sec.addr, sec.addr + sec.size, sec.name});
    }

    for (const Range& range : ranges) {
        logf(log, "disassembly of %s [0x%08X, 0x%08X):", range.tag.c_str(), range.start, range.end);
        char line[512];
        for (ee::u32 va = range.start; va < range.end; va += 4) {
            auto word = image.read_u32(va);
            if (!word) {
                std::snprintf(line, sizeof line, "  %08X: <unmapped>", va);
                logf(log, "%s", line);
                continue;
            }
            if (auto it = labels.find(va); it != labels.end())
                logf(log, "%s:", it->second.c_str());
            const ee::r5900::Instruction insn = ee::r5900::decode(*word);
            std::string text = ee::r5900::disassemble(insn, va);
            if (insn.is_branch() || insn.op == ee::r5900::Op::J || insn.op == ee::r5900::Op::Jal) {
                const ee::u32 tgt = insn.is_branch() ? ee::r5900::branch_target(insn, va)
                                                     : ee::r5900::jump_target(insn, va);
                if (auto it = labels.find(tgt); it != labels.end())
                    text += "  <" + it->second + ">";
            }
            std::snprintf(line, sizeof line, "  %08X: %08X  %s", va, *word, text.c_str());
            logf(log, "%s", line);
        }
    }
}
// No ImGui: keep SDL-based enum/settings out of the way.
// Analyze the loaded ELF (mirrors ee-analyze) and run any exports.
void do_analyze(ToolState& st, std::string& log) {
    if (!st.image) {
        logf(log, "load an ELF first");
        return;
    }
    const ee::elf::Image& image = *st.image;

    std::map<ee::u32, std::string> imported;
    for (const std::string& imp : st.imports) {
        std::string text;
        if (!read_file(imp, text)) {
            logf(log, "error: cannot read import %s", imp.c_str());
            return;
        }
        std::map<ee::u32, std::string> m =
            (ends_with(lower(imp), ".json")) ? ee::analysis::import_json(text)
                                             : ee::analysis::import_csv(text);
        logf(log, "imported %zu names from %s", m.size(), imp.c_str());
        imported.insert(m.begin(), m.end());
    }

    ee::analysis::Options opt;
    opt.prologue_scan = st.prologue_scan;
    const ee::analysis::Result res = ee::analysis::analyze(image, opt, imported);

    logf(log, "functions:  %zu", res.functions.size());
    size_t by_source[5] = {};
    for (const auto& f : res.functions)
        ++by_source[size_t(f.source) <= 4 ? size_t(f.source) : 4];
    logf(log, "  entry=%zu symbol=%zu import=%zu call=%zu prologue=%zu", by_source[0],
         by_source[1], by_source[2], by_source[3], by_source[4]);
    logf(log, "jump tables: %zu", res.jump_tables.size());
    logf(log, "unresolved indirect sites: %zu", res.unresolved_indirects.size());
    for (ee::u32 addr : res.unresolved_indirects)
        logf(log, "  indirect @ 0x%08X", addr);

    if (st.toml_out_buf[0]) {
        if (!write_file(st.toml_out_buf, ee::analysis::export_toml(res, image)))
            logf(log, "error: cannot write %s", st.toml_out_buf);
        else
            logf(log, "wrote TOML config: %s", st.toml_out_buf);
    }
    if (st.csv_out_buf[0]) {
        if (!write_file(st.csv_out_buf, ee::analysis::export_csv(res)))
            logf(log, "error: cannot write %s", st.csv_out_buf);
        else
            logf(log, "wrote CSV: %s", st.csv_out_buf);
    }
    if (st.json_out_buf[0]) {
        if (!write_file(st.json_out_buf, ee::analysis::export_json(res)))
            logf(log, "error: cannot write %s", st.json_out_buf);
        else
            logf(log, "wrote JSON: %s", st.json_out_buf);
    }
}

// Analyze + codegen (mirrors ee-recomp).
void do_recomp(ToolState& st, std::string& log) {
    if (!st.image) {
        logf(log, "load an ELF first");
        return;
    }
    const ee::elf::Image& image = *st.image;

    ee::codegen::Config cfg;
    if (st.config_buf[0]) {
        std::string text, err;
        if (!read_file(st.config_buf, text)) {
            logf(log, "error: cannot read config %s", st.config_buf);
            return;
        }
        auto parsed = ee::codegen::Config::from_toml(text, &err);
        if (!parsed) {
            logf(log, "error: bad config %s: %s", st.config_buf, err.c_str());
            return;
        }
        cfg = std::move(*parsed);
    }

    std::map<ee::u32, std::string> imported;
    for (const std::string& imp : st.imports) {
        std::string text;
        if (!read_file(imp, text)) {
            logf(log, "error: cannot read import %s", imp.c_str());
            return;
        }
        std::map<ee::u32, std::string> m =
            (ends_with(lower(imp), ".json")) ? ee::analysis::import_json(text)
                                             : ee::analysis::import_csv(text);
        imported.insert(m.begin(), m.end());
    }

    ee::analysis::Options analyze_opt;
    analyze_opt.prologue_scan = st.prologue_scan;
    ee::codegen::Options codegen_opt;
    codegen_opt.emit_comments = st.emit_comments;

    const ee::analysis::Result res = ee::analysis::analyze(image, analyze_opt, imported);
    logf(log, "recompile: %zu functions, %zu jump tables, %zu unresolved indirects",
         res.functions.size(), res.jump_tables.size(), res.unresolved_indirects.size());

    const std::string module = ee::codegen::emit_module(image, res, cfg, codegen_opt);

    std::string out_path = st.recomp_out_buf;
    if (out_path.empty()) out_path = st.elf_path + ".recomp.cpp";
    if (!write_file(out_path, module)) {
        logf(log, "error: cannot write %s", out_path.c_str());
        return;
    }
    logf(log, "wrote %s (%zu bytes)", out_path.c_str(), module.size());
}
// --- GUI --------------------------------------------------------------------

#ifdef EE_HAS_IMGUI

int run_gui() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("EERecomp Tools", 1280, 800, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    enlarge_ui_font(io);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ToolState st;
    uitools::FileBrowser browser;
    std::string log = "ee-tools ready. Open an ELF to begin.\n";

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
                st.elf_path = ev.drop.data;
                SDL_free(const_cast<char*>(ev.drop.data));
                load_elf(st, log);
            }
        }
        // In-app file browser (no OS-native dialog dependency).
        if (browser.result_ready) {
            browser.result_ready = false;
            st.elf_path = browser.result;
            load_elf(st, log);
        }

        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();

        browser.draw();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open File...")) browser.open(st.browse_all_files ? "" : ".elf");
                if (ImGui::MenuItem("Quit")) quit = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // File pane (always visible).
        ImGui::Begin("Input", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::SetNextItemWidth(-1.0f);
        static char elf_buf[512] = {};
        std::snprintf(elf_buf, sizeof elf_buf, "%s", st.elf_path.c_str());
        ImGui::InputText("##elf", elf_buf, sizeof elf_buf);
        st.elf_path = elf_buf;
        ImGui::Checkbox("Show all files (some PS2 execs have no .elf)", &st.browse_all_files);
        if (ImGui::Button("Browse...")) browser.open(st.browse_all_files ? "" : ".elf");
        ImGui::SameLine();
        if (ImGui::Button("Load ELF")) load_elf(st, log);
        if (!st.image && !st.load_error.empty())
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", st.load_error.c_str());
        ImGui::End();

        // Tools tabs.
        ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTabBar("maintabs")) {
            if (ImGui::BeginTabItem("ELF Info")) {
                if (st.image) {
                    ImGui::BeginChild("info", ImVec2(0, 0), ImGuiChildFlags_Border);
                    ImGui::TextUnformatted(st.elf_info.c_str());
                    ImGui::EndChild();
                } else {
                    ImGui::TextDisabled("No ELF loaded.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Disassemble")) {
                ImGui::TextWrapped("Options for ee-disasm:");
                ImGui::Checkbox("Address range", &st.use_range);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputText("start", st.start_buf, sizeof st.start_buf);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputText("end", st.end_buf, sizeof st.end_buf);
                st.range_start = static_cast<ee::u32>(std::strtoul(st.start_buf, nullptr, 16));
                st.range_end = static_cast<ee::u32>(std::strtoul(st.end_buf, nullptr, 16));
                if (!st.use_range) {
                    ImGui::SetNextItemWidth(200);
                    static char sec_buf[128] = {};
                    std::snprintf(sec_buf, sizeof sec_buf, "%s", st.section_name.c_str());
                    ImGui::InputText("section", sec_buf, sizeof sec_buf);
                    st.section_name = sec_buf;
                }
                if (ImGui::Button("Disassemble")) do_disasm(st, log);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Analyze")) {
                ImGui::TextWrapped("Options for ee-analyze:");
                ImGui::Checkbox("Prologue scan", &st.prologue_scan);
                ImGui::SetNextItemWidth(-1.0f);
                static char imp_buf[512] = {};
                ImGui::InputText("##import", imp_buf, sizeof imp_buf);
                if (ImGui::Button("Add import")) {
                    if (imp_buf[0]) {
                        st.imports.emplace_back(imp_buf);
                        imp_buf[0] = 0;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear imports")) st.imports.clear();
                for (const auto& imp : st.imports) ImGui::BulletText("%s", imp.c_str());
                ImGui::Separator();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("TOML out", st.toml_out_buf, sizeof st.toml_out_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("CSV out", st.csv_out_buf, sizeof st.csv_out_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("JSON out", st.json_out_buf, sizeof st.json_out_buf);
                if (ImGui::Button("Analyze")) do_analyze(st, log);
                ImGui::EndTabItem();
            }
if (ImGui::BeginTabItem("Recompile")) {
                ImGui::TextWrapped("Options for ee-recomp:");
                ImGui::Checkbox("Prologue scan", &st.prologue_scan);
                ImGui::SameLine();
                ImGui::Checkbox("Emit comments", &st.emit_comments);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("Config TOML", st.config_buf, sizeof st.config_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("Output .cpp", st.recomp_out_buf, sizeof st.recomp_out_buf);
                if (ImGui::Button("Recompile")) do_recomp(st, log);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        // Bottom log console.
        ImGui::Begin("Log");
        ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_Border);
        ImGui::TextUnformatted(log.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 16, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#else // !EE_HAS_IMGUI

int run_gui() {
    std::fprintf(stderr, "ee-tools requires SDL3 + Dear ImGui (EE_HAS_IMGUI).\n");
    return 1;
}

#endif // EE_HAS_IMGUI

} // namespace

int main(int, char**) {
    return run_gui();
}