// SPDX-License-Identifier: GPL-3.0-only
//
// ee-tools: GUI for the ee-disasm / ee-analyze / ee-recomp toolchain.
// Same functionality as the three command-line tools, in a single tabbed
// window (ELF info, disassembly, analysis, recompilation). The CLI tools
// remain for scripting.
//
// Long operations (load / disassemble / analyze / recompile) run on a
// background worker thread with progress reporting and cancellation, so the
// window never freezes while crunching a large PS2 ELF. Disc images (.iso /
// .bin) are accepted directly: the boot ELF is extracted via SYSTEM.CNF, the
// same way the game-launcher does it.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ee/analysis.hpp>
#include <ee/codegen.hpp>
#include <ee/elf.hpp>
#include <ee/r5900.hpp>
#include <ee/cdvd.hpp>

#ifdef EE_HAS_SDL3
#include <SDL3/SDL.h>
#endif

#ifdef EE_HAS_IMGUI
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../ui_filebrowser.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using ee::u8;
using ee::u32;
using ee::u64;
using ee::s32;

// --- generic helpers ----------------------------------------------------------

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
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

bool is_disc_image(const std::string& path) {
    const std::string low = lower(path);
    return ends_with(low, ".iso") || ends_with(low, ".bin");
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
#endif

// Build the ELF header/segment/section dump (mirrors ee-disasm header output).
std::string build_elf_info(const ee::elf::Image& image, const std::string& display_path) {
    char b[128];
    std::ostringstream o;
    o << "file:     " << display_path << "\n";
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
    return o.str();
}

// --- background worker --------------------------------------------------------
// Long operations run here so the GUI stays responsive. Progress is published
// as 0..100 (or -1 for indeterminate) plus a short status string; the user may
// request cancellation, which jobs poll cooperatively.
struct Worker {
    std::thread th;
    std::mutex mtx;
    std::atomic<bool> busy{false};
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{-1};
    std::string status;                 // guarded by mtx
    std::vector<std::string> logq;      // guarded by mtx: live log lines

    bool is_busy() const { return busy.load(std::memory_order_relaxed); }
    void request_cancel() { cancel.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void set_status(const char* s) { std::lock_guard<std::mutex> lk(mtx); status = s; }
    void set_status(const std::string& s) { std::lock_guard<std::mutex> lk(mtx); status = s; }
    std::string get_status() { std::lock_guard<std::mutex> lk(mtx); return status; }
    void set_progress(int p) { progress.store(p); }
    int get_progress() const { return progress.load(std::memory_order_relaxed); }

    void log(const char* fmtstr, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmtstr);
        std::vsnprintf(buf, sizeof buf, fmtstr, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lk(mtx);
        logq.emplace_back(buf);
        if (logq.size() > 4'000'000) logq.erase(logq.begin(), logq.begin() + 1'000'000);
    }

    // Move pending log lines into `out` (call from the GUI thread).
    void drain(std::vector<std::string>& out) {
        std::vector<std::string> chunk;
        {
            std::lock_guard<std::mutex> lk(mtx);
            chunk.swap(logq);
        }
        out.insert(out.end(), std::make_move_iterator(chunk.begin()),
                   std::make_move_iterator(chunk.end()));
        if (out.size() > 4'000'000) out.erase(out.begin(), out.begin() + 1'000'000);
    }

    template <class F>
    void start(const char* initial_status, F&& fn) {
        if (is_busy()) return;
        cancel.store(false);
        progress.store(-1);
        set_status(initial_status);
        {
            std::lock_guard<std::mutex> lk(mtx);
            logq.clear();
        }
        if (th.joinable()) th.join();
        busy.store(true);
        th = std::thread([this, fn = std::forward<F>(fn)]() mutable {
            fn();
            busy.store(false);
        });
    }

    void join() { if (th.joinable()) th.join(); }
};

// --- tool state --------------------------------------------------------------

struct ToolState {
    std::string elf_path;
    std::optional<ee::elf::Image> image;
    std::string elf_info;
    std::string load_error;

    // Disassemble
    std::string section_name;
    bool use_range = false;
    ee::u32 range_start = 0x00100000;
    ee::u32 range_end = 0x00110000;
    char start_buf[16] = "00100000";
    char end_buf[16] = "00110000";

    // Analyze / Recompile shared imports (paths to CSV/JSON name files)
    std::vector<std::string> imports;
    bool prologue_scan = true;
    char toml_out_buf[512] = {};
    char csv_out_buf[512] = {};
    char json_out_buf[512] = {};

    // Recompile
    char config_buf[512] = {};
    char recomp_out_buf[512] = {};
    bool emit_comments = true;

    bool browse_all_files = true;

    // Pending load result handed from the worker thread to the GUI thread.
    // `load_result_ready` is guarded by Worker::mtx.
    struct LoadResult {
        bool ok = false;
        std::optional<ee::elf::Image> image;
        std::string elf_info;
        std::string error;
        std::string display_path;
    };
    LoadResult pending_load;
    bool load_result_ready = false;
};

// --- jobs (run on the worker thread) -----------------------------------------

// Load an ELF, or extract the boot ELF from a PS2 disc image (.iso/.bin).
void run_load(ToolState& st, Worker& wk) {
    ToolState::LoadResult& out = st.pending_load;
    out = ToolState::LoadResult{};
    if (st.elf_path.empty()) { out.error = "no file selected"; return; }
    const std::string path = st.elf_path;

    std::vector<u8> bytes;
    std::string display = path;
    if (is_disc_image(path)) {
        wk.set_status("Opening disc image...");
        wk.set_progress(10);
        ee::rt::Cdvd cdvd;
        if (!cdvd.open(path)) { out.error = "could not open disc image: " + path; return; }
        wk.log("disc: %s (%u sectors)", path.c_str(), cdvd.sector_count());
        wk.set_progress(30);
        wk.set_status("Reading SYSTEM.CNF...");
        auto boot = cdvd.find_boot_elf();
        if (!boot) { out.error = "SYSTEM.CNF not found (not a PS2 boot disc image?)"; return; }
        wk.log("boot ELF: %s", boot->c_str());
        wk.set_progress(50);
        wk.set_status("Reading boot ELF " + *boot);
        auto fbytes = cdvd.read_file(*boot);
        if (!fbytes) { out.error = "could not read boot file from disc: " + *boot; return; }
        bytes = std::move(*fbytes);
        display = *boot;
    } else {
        wk.set_status("Reading ELF...");
        wk.set_progress(20);
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { out.error = "cannot open file: " + path; return; }
        const std::streamoff sz = f.tellg();
        if (sz < 0) { out.error = "cannot size file: " + path; return; }
        f.seekg(0);
        bytes.resize(static_cast<size_t>(sz));
        if (sz > 0 && !f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
            out.error = "failed to read file: " + path;
            return;
        }
    }
    if (wk.cancelled()) { out.error = "cancelled"; return; }
    wk.set_progress(80);
    wk.set_status("Parsing ELF...");
    std::string perr;
    auto img = ee::elf::Image::load_bytes(std::move(bytes), &perr);
    if (!img) { out.error = perr.empty() ? "ELF parse failed" : perr; return; }
    out.ok = true;
    out.image = std::move(*img);
    out.elf_info = build_elf_info(*out.image, display);
    out.display_path = display;
    wk.set_progress(100);
    wk.set_status("Loaded");
    wk.log("loaded %s%s", display.c_str(), is_disc_image(path) ? " (extracted from disc)" : "");
}

// Disassemble the loaded ELF (mirrors ee-disasm).
void run_disasm(ToolState& st, Worker& wk) {
    if (!st.image) { wk.log("load an ELF first"); return; }
    const ee::elf::Image& image = *st.image;
    wk.log("--- disassembly ---");
    const bool have_range = st.use_range && st.range_end > st.range_start;

    std::map<u32, std::string> labels;
    for (const auto& sym : image.symbols())
        if (!sym.name.empty() && sym.value != 0) labels.emplace(sym.value, sym.name);

    struct Range { u32 start, end; std::string tag; };
    std::vector<Range> ranges;
    if (have_range) {
        ranges.push_back({st.range_start, st.range_end, "address range"});
    } else if (!st.section_name.empty()) {
        const ee::elf::Section* sec = image.find_section(st.section_name);
        if (!sec) { wk.log("error: section not found: %s", st.section_name.c_str()); return; }
        ranges.push_back({sec->addr, sec->addr + sec->size, sec->name});
    } else {
        for (const auto& sec : image.sections())
            if (sec.executable() && sec.size != 0)
                ranges.push_back({sec.addr, sec.addr + sec.size, sec.name});
    }

    u64 total = 0;
    for (const auto& r : ranges) total += u64(r.end - r.start) / 4;
    u64 done = 0;
    const u64 cap = 200000;       // soft cap on emitted instruction lines
    u64 emitted = 0, omitted = 0;

    for (const Range& range : ranges) {
        if (wk.cancelled()) { wk.log("(disassembly cancelled)"); return; }
        wk.log("disassembly of %s [0x%08X, 0x%08X):", range.tag.c_str(), range.start, range.end);
        char line[512];
        for (u32 va = range.start; va < range.end; va += 4) {
            if ((++done & 0x3FF) == 0) {
                wk.set_progress(total ? int(100.0 * double(done) / double(total)) : 100);
                if (wk.cancelled()) { wk.log("(disassembly cancelled)"); return; }
            }
            if (emitted < cap) {
                if (auto it = labels.find(va); it != labels.end())
                    wk.log("%s:", it->second.c_str());
            }
            auto word = image.read_u32(va);
            if (!word) {
                if (emitted < cap) { wk.log("  %08X: <unmapped>", va); ++emitted; }
                else ++omitted;
                continue;
            }
            const ee::r5900::Instruction insn = ee::r5900::decode(*word);
            std::string text = ee::r5900::disassemble(insn, va);
            if (insn.is_branch() || insn.op == ee::r5900::Op::J || insn.op == ee::r5900::Op::Jal) {
                const u32 tgt = insn.is_branch() ? ee::r5900::branch_target(insn, va)
                                                 : ee::r5900::jump_target(insn, va);
                if (auto it = labels.find(tgt); it != labels.end())
                    text += "  <" + it->second + ">";
            }
            std::snprintf(line, sizeof line, "  %08X: %08X  %s", va, *word, text.c_str());
            if (emitted < cap) { wk.log("%s", line); ++emitted; }
            else ++omitted;
        }
    }
    if (omitted)
        wk.log("  ... %llu more instruction line(s) omitted (cap %llu). Use an address range"
               " or the ee-disasm CLI for the full listing.",
               (unsigned long long)omitted, (unsigned long long)cap);
    wk.set_progress(100);
    wk.set_status("Disassembly complete");
}

// Read all import files; skip (with a warning) any that can't be read instead
// of aborting the whole operation.
std::map<u32, std::string> load_imports(ToolState& st, Worker& wk) {
    std::map<u32, std::string> imported;
    for (const std::string& imp : st.imports) {
        std::string text;
        if (!read_file(imp, text)) {
            wk.log("warning: cannot read import %s (skipped)", imp.c_str());
            continue;
        }
        std::map<u32, std::string> m =
            ends_with(lower(imp), ".json") ? ee::analysis::import_json(text)
                                           : ee::analysis::import_csv(text);
        wk.log("imported %zu names from %s", m.size(), imp.c_str());
        imported.insert(m.begin(), m.end());
    }
    return imported;
}

ee::analysis::ProgressFn make_progress(Worker& wk) {
    return [&wk](double frac, const char* status) {
        wk.set_progress(int(frac * 100.0 + 0.5));
        if (status) wk.set_status(status);
        return !wk.cancelled();
    };
}

// Analyze the loaded ELF (mirrors ee-analyze) and run any exports.
void run_analyze(ToolState& st, Worker& wk) {
    if (!st.image) { wk.log("load an ELF first"); return; }
    const ee::elf::Image& image = *st.image;

    std::map<u32, std::string> imported = load_imports(st, wk);

    ee::analysis::Options opt;
    opt.prologue_scan = st.prologue_scan;
    const ee::analysis::Result res = ee::analysis::analyze(image, opt, imported, make_progress(wk));

    if (wk.cancelled()) { wk.log("(analysis cancelled)"); return; }

    wk.log("functions:  %zu", res.functions.size());
    size_t by_source[5] = {};
    for (const auto& f : res.functions)
        ++by_source[size_t(f.source) <= 4 ? size_t(f.source) : 4];
    wk.log("  entry=%zu symbol=%zu import=%zu call=%zu prologue=%zu", by_source[0],
           by_source[1], by_source[2], by_source[3], by_source[4]);
    wk.log("jump tables: %zu", res.jump_tables.size());
    wk.log("unresolved indirect sites: %zu", res.unresolved_indirects.size());
    for (u32 addr : res.unresolved_indirects)
        wk.log("  indirect @ 0x%08X", addr);

    if (st.toml_out_buf[0]) {
        if (!write_file(st.toml_out_buf, ee::analysis::export_toml(res, image)))
            wk.log("error: cannot write %s", st.toml_out_buf);
        else
            wk.log("wrote TOML config: %s", st.toml_out_buf);
    }
    if (st.csv_out_buf[0]) {
        if (!write_file(st.csv_out_buf, ee::analysis::export_csv(res)))
            wk.log("error: cannot write %s", st.csv_out_buf);
        else
            wk.log("wrote CSV: %s", st.csv_out_buf);
    }
    if (st.json_out_buf[0]) {
        if (!write_file(st.json_out_buf, ee::analysis::export_json(res)))
            wk.log("error: cannot write %s", st.json_out_buf);
        else
            wk.log("wrote JSON: %s", st.json_out_buf);
    }
    wk.set_progress(100);
    wk.set_status("Analysis complete");
}

// Analyze + codegen (mirrors ee-recomp).
void run_recomp(ToolState& st, Worker& wk) {
    if (!st.image) { wk.log("load an ELF first"); return; }
    const ee::elf::Image& image = *st.image;

    ee::codegen::Config cfg;
    if (st.config_buf[0]) {
        std::string text, err;
        if (!read_file(st.config_buf, text)) {
            wk.log("error: cannot read config %s", st.config_buf);
            return;
        }
        auto parsed = ee::codegen::Config::from_toml(text, &err);
        if (!parsed) {
            wk.log("error: bad config %s: %s", st.config_buf, err.c_str());
            return;
        }
        cfg = std::move(*parsed);
    }

    std::map<u32, std::string> imported = load_imports(st, wk);

    ee::analysis::Options analyze_opt;
    analyze_opt.prologue_scan = st.prologue_scan;
    const ee::analysis::Result res = ee::analysis::analyze(image, analyze_opt, imported, make_progress(wk));
    if (wk.cancelled()) { wk.log("(recompile cancelled)"); return; }

    wk.log("recompile: %zu functions, %zu jump tables, %zu unresolved indirects",
           res.functions.size(), res.jump_tables.size(), res.unresolved_indirects.size());

    wk.set_status("Generating C++...");
    ee::codegen::Options codegen_opt;
    codegen_opt.emit_comments = st.emit_comments;
    const std::string module = ee::codegen::emit_module(image, res, cfg, codegen_opt);

    std::string out_path = st.recomp_out_buf;
    if (out_path.empty()) out_path = st.elf_path + ".recomp.cpp";
    if (!write_file(out_path, module)) {
        wk.log("error: cannot write %s", out_path.c_str());
        return;
    }
    wk.log("wrote %s (%zu bytes)", out_path.c_str(), module.size());
    wk.set_progress(100);
    wk.set_status("Recompile complete");
}

// --- GUI ---------------------------------------------------------------------

#ifdef EE_HAS_IMGUI

// Start a load job from the GUI thread.
void start_load(ToolState& st, Worker& wk) {
    if (wk.is_busy()) return;
    st.image.reset();          // clear immediately; UI shows "loading..."
    st.elf_info.clear();
    st.load_error.clear();
    {
        std::lock_guard<std::mutex> lk(wk.mtx);
        st.pending_load = ToolState::LoadResult{};
        st.load_result_ready = false;
    }
    wk.start("Loading...", [&st, &wk]() {
        run_load(st, wk);
        std::lock_guard<std::mutex> lk(wk.mtx);
        st.load_result_ready = true;
    });
}

void start_disasm(ToolState& st, Worker& wk) {
    if (wk.is_busy()) return;
    if (!st.image) { wk.log("load an ELF first"); return; }
    wk.start("Disassembling...", [&st, &wk]() { run_disasm(st, wk); });
}

void start_analyze(ToolState& st, Worker& wk) {
    if (wk.is_busy()) return;
    if (!st.image) { wk.log("load an ELF first"); return; }
    wk.start("Analyzing...", [&st, &wk]() { run_analyze(st, wk); });
}

void start_recomp(ToolState& st, Worker& wk) {
    if (wk.is_busy()) return;
    if (!st.image) { wk.log("load an ELF first"); return; }
    wk.start("Recompiling...", [&st, &wk]() { run_recomp(st, wk); });
}

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
    Worker wk;
    uitools::FileBrowser browser;        // for Open File
    uitools::FileBrowser import_browser; // for adding CSV/JSON imports
    std::vector<std::string> log_lines;
    log_lines.emplace_back("ee-tools ready. Open an ELF or a PS2 disc image (.iso/.bin) to begin.");

    auto elapsed_str = [](std::chrono::steady_clock::time_point start) {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        char b[32];
        std::snprintf(b, sizeof b, "%.1fs", s);
        return std::string(b);
    };
    std::chrono::steady_clock::time_point job_start = std::chrono::steady_clock::now();
    float indet_phase = 0.0f;

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
                st.elf_path = ev.drop.data;
                SDL_free(const_cast<char*>(ev.drop.data));
                start_load(st, wk);
                job_start = std::chrono::steady_clock::now();
            }
        }
        if (browser.result_ready) {
            browser.result_ready = false;
            st.elf_path = browser.result;
            start_load(st, wk);
            job_start = std::chrono::steady_clock::now();
        }
        if (import_browser.result_ready) {
            import_browser.result_ready = false;
            if (!wk.is_busy()) st.imports.push_back(import_browser.result);
        }

        // Drain worker log lines into the visible log.
        wk.drain(log_lines);

        // Commit a finished load result on the GUI thread.
        if (!wk.is_busy()) {
            std::lock_guard<std::mutex> lk(wk.mtx);
            if (st.load_result_ready) {
                st.load_result_ready = false;
                if (st.pending_load.ok) {
                    st.image = std::move(st.pending_load.image);
                    st.elf_info = std::move(st.pending_load.elf_info);
                    st.load_error.clear();
                } else {
                    st.image.reset();
                    st.load_error = std::move(st.pending_load.error);
                }
                st.pending_load = ToolState::LoadResult{};
            }
        }

        const bool busy = wk.is_busy();
        if (busy) indet_phase += io.DeltaTime;

        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();

        browser.draw();
        import_browser.draw();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open File...")) browser.open(st.browse_all_files ? "" : ".elf;.bin");
                if (ImGui::MenuItem("Quit")) quit = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // --- Input pane ---
        ImGui::Begin("Input", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("ELF or disc image (.iso/.bin):");
        ImGui::SetNextItemWidth(-1.0f);
        static char elf_buf[512] = {};
        std::snprintf(elf_buf, sizeof elf_buf, "%s", st.elf_path.c_str());
        ImGui::InputText("##elf", elf_buf, sizeof elf_buf, busy ? ImGuiInputTextFlags_ReadOnly : 0);
        if (!busy) st.elf_path = elf_buf;
        ImGui::Checkbox("Show all files in browser (some PS2 execs have no .elf)", &st.browse_all_files);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Browse...")) browser.open(st.browse_all_files ? "" : ".elf;.bin");
        ImGui::SameLine();
        if (ImGui::Button("Load")) { start_load(st, wk); job_start = std::chrono::steady_clock::now(); }
        ImGui::EndDisabled();
        if (!st.load_error.empty())
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "error: %s", st.load_error.c_str());
        ImGui::Separator();
        if (busy) {
            const std::string status = wk.get_status();
            ImGui::TextDisabled("Working: %s (%s)", status.c_str(), elapsed_str(job_start).c_str());
            const int p = wk.get_progress();
            float frac;
            if (p < 0) {  // indeterminate: bouncing bar
                frac = 0.5f + 0.5f * std::sin(indet_phase * 3.0f);
                ImGui::ProgressBar(frac, ImVec2(0, 0), "working...");
            } else {
                frac = p / 100.0f;
                ImGui::ProgressBar(frac, ImVec2(0, 0), status.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) wk.request_cancel();
        } else {
            ImGui::TextDisabled("Idle.");
        }
        ImGui::End();

        // --- Tools tabs ---
        ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTabBar("maintabs")) {
            if (ImGui::BeginTabItem("ELF Info")) {
                if (st.image) {
                    ImGui::BeginChild("info", ImVec2(0, 0), ImGuiChildFlags_Border);
                    ImGui::TextUnformatted(st.elf_info.c_str());
                    ImGui::EndChild();
                } else if (busy) {
                    ImGui::TextDisabled("Loading...");
                } else {
                    ImGui::TextDisabled("No ELF loaded.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Disassemble")) {
                ImGui::TextWrapped("Options for ee-disasm:");
                ImGui::BeginDisabled(busy);
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
                    ImGui::SetNextItemWidth(-1.0f);
                    static char sec_buf[128] = {};
                    std::snprintf(sec_buf, sizeof sec_buf, "%s", st.section_name.c_str());
                    ImGui::InputText("section (blank = all executable)", sec_buf, sizeof sec_buf);
                    st.section_name = sec_buf;
                }
                if (ImGui::Button("Disassemble")) { start_disasm(st, wk); job_start = std::chrono::steady_clock::now(); }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Analyze")) {
                ImGui::TextWrapped("Options for ee-analyze:");
                ImGui::BeginDisabled(busy);
                ImGui::Checkbox("Prologue scan", &st.prologue_scan);
                ImGui::Separator();
                ImGui::TextUnformatted("Import function-name files (CSV/JSON from Aura/Ghidra):");
                static char imp_buf[512] = {};
                ImGui::SetNextItemWidth(-130);
                ImGui::InputText("##import", imp_buf, sizeof imp_buf);
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) import_browser.open(".csv;.json");
                ImGui::SameLine();
                if (ImGui::Button("Add")) {
                    if (imp_buf[0]) { st.imports.emplace_back(imp_buf); imp_buf[0] = 0; }
                }
                for (size_t i = 0; i < st.imports.size(); ++i) {
                    ImGui::PushID((int)i);
                    ImGui::Bullet();
                    ImGui::TextUnformatted(st.imports[i].c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("remove")) {
                        st.imports.erase(st.imports.begin() + static_cast<long>(i));
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (!st.imports.empty() && ImGui::Button("Clear imports")) st.imports.clear();
                ImGui::Separator();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("TOML out", st.toml_out_buf, sizeof st.toml_out_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("CSV out", st.csv_out_buf, sizeof st.csv_out_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("JSON out", st.json_out_buf, sizeof st.json_out_buf);
                if (ImGui::Button("Analyze")) { start_analyze(st, wk); job_start = std::chrono::steady_clock::now(); }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Recompile")) {
                ImGui::TextWrapped("Options for ee-recomp:");
                ImGui::BeginDisabled(busy);
                ImGui::Checkbox("Prologue scan", &st.prologue_scan);
                ImGui::SameLine();
                ImGui::Checkbox("Emit comments", &st.emit_comments);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("Config TOML", st.config_buf, sizeof st.config_buf);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("Output .cpp", st.recomp_out_buf, sizeof st.recomp_out_buf);
                if (ImGui::Button("Recompile")) { start_recomp(st, wk); job_start = std::chrono::steady_clock::now(); }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        // --- Log console (clipper so huge disassemblies stay fast) ---
        ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::Button("Clear log")) log_lines.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu lines", log_lines.size());
        ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_Border);
        ImGuiListClipper clipper;
        clipper.Begin((int)log_lines.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                ImGui::TextUnformatted(log_lines[(size_t)i].c_str());
        }
        clipper.End();
        if (busy && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 16, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    wk.request_cancel();
    wk.join();

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
