// SPDX-License-Identifier: GPL-3.0-only

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS // MSVC: fopen/strncpy/etc. are fine for us here
#endif

// ee-studio: Game library + pipeline runner GUI for EERecomp.
// Built with SDL3 + Dear ImGui (when available).
// When SDL3 is not found, compiles as a headless CLI stub.

#ifdef EE_HAS_SDL3

#include <ee/analysis.hpp>
#include <ee/cdvd.hpp>
#include <ee/codegen.hpp>
#include <ee/elf.hpp>
#include <ee/gs_renderer.hpp>
#include <ee/hw.hpp>
#include <ee/iop.hpp>
#include <ee/r5900.hpp>
#include <ee/runtime.hpp>

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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

void register_functions(ee::rt::Runtime& rt);

namespace {

struct GameEntry {
    std::string name;
    std::string path;
    bool is_iso = false;
};

// PS2 executables often have no .elf extension (e.g. "SLUS_210.66"), so detect
// them by content: ELF magic + MIPS machine, not just by file extension.
bool looks_like_ps2_elf(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    unsigned char hdr[20] = {};
    f.read(reinterpret_cast<char*>(hdr), sizeof hdr);
    if (f.gcount() < 20) return false;
    if (hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') return false;
    const unsigned machine = unsigned(hdr[18]) | (unsigned(hdr[19]) << 8);
    return machine == 8; // EM_MIPS
}

std::vector<GameEntry> scan_directory(const std::string& dir) {
    std::vector<GameEntry> games;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(dir),
                                        fs::directory_options::skip_permission_denied, ec);
    if (ec) return games;
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        std::error_code lec;
        if (it->is_directory(lec)) continue;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::string lower = name;
        for (auto& c : lower) c = char(std::tolower((unsigned char)c));
        const bool has_elf_ext = lower.size() > 4 && lower.substr(lower.size() - 4) == ".elf";
        const bool is_elf = has_elf_ext || looks_like_ps2_elf(it->path());
        const bool is_iso = lower.size() > 4 &&
                            (lower.substr(lower.size() - 4) == ".iso" ||
                             lower.substr(lower.size() - 4) == ".bin");
        if (!is_elf && !is_iso) continue;
        GameEntry g;
        g.name = fs::relative(it->path(), fs::path(dir), ec).generic_string();
        g.path = it->path().string();
        g.is_iso = is_iso;
        games.push_back(std::move(g));
    }
    return games;
}

#ifdef EE_HAS_IMGUI
// Larger UI font: prefer a real TTF at a bigger point size, else scale the
// default font. Called once after ImGui::CreateContext().
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
    io.FontGlobalScale = 1.6f; // no font file found: scale the built-in font
}
#endif // EE_HAS_IMGUI

} // namespace

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("EERecomp Studio", 1280, 800, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

#ifdef EE_HAS_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    enlarge_ui_font(io);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
#endif

    std::string games_dir = ".";
    std::vector<GameEntry> games;
    int selected_game = -1;
    std::string log_text;
    std::vector<std::string> disasm_lines;
    bool show_disasm = false;
    bool show_console = true;
    std::string status_msg = "Ready. Scan a directory to find games.";
#ifdef EE_HAS_IMGUI
    uitools::FileBrowser browser;
#endif

    auto log_msg = [&](const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log_text += buf;
        log_text += '\n';
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            games_dir = argv[++i];
            games = scan_directory(games_dir);
            status_msg = "Scanned " + std::to_string(games.size()) + " game(s) in " + games_dir;
        }
    }

    bool quit = false;
#ifndef EE_HAS_IMGUI
    bool noted_no_imgui = false;
#endif
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
#ifdef EE_HAS_IMGUI
            ImGui_ImplSDL3_ProcessEvent(&event);
#endif
        }
        SDL_RenderClear(renderer);

#ifdef EE_HAS_IMGUI
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // In-app file browser (no OS-native dialog dependency).
        browser.draw();
        if (browser.result_ready) {
            browser.result_ready = false;
            const std::string picked = browser.result;
            std::error_code ec;
            if (fs::is_directory(picked, ec)) {
                games_dir = picked;
                games = scan_directory(games_dir);
                status_msg = "Scanned " + std::to_string(games.size()) +
                             " game(s) in " + picked;
            } else {
                std::string lower = picked;
                for (auto& c : lower) c = char(std::tolower((unsigned char)c));
                const bool is_elf = lower.size() > 4 &&
                                    lower.substr(lower.size() - 4) == ".elf";
                const bool is_iso = lower.size() > 4 &&
                                    (lower.substr(lower.size() - 4) == ".iso" ||
                                     lower.substr(lower.size() - 4) == ".bin");
                if (is_elf || is_iso) {
                    GameEntry g;
                    g.name = fs::path(picked).filename().string();
                    g.path = picked;
                    g.is_iso = is_iso;
                    games.insert(games.begin(), std::move(g));
                    selected_game = 0;
                    status_msg = "Added " + g.name;
                } else {
                    status_msg = "Unsupported file: " + picked;
                }
            }
        }

        // Menu Bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Directory...")) browser.open("", true);
                if (ImGui::MenuItem("Open Game...")) browser.open("");
                if (ImGui::MenuItem("Quit")) quit = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Game Library Panel
        ImGui::Begin("Game Library", nullptr, ImGuiWindowFlags_NoCollapse);
        char dir_buf[256];
        strncpy(dir_buf, games_dir.c_str(), 255);
        dir_buf[255] = 0;
        ImGui::InputText("Directory", dir_buf, 256);
        games_dir = dir_buf;
        ImGui::SameLine();
        if (ImGui::Button("Scan")) {
            games = scan_directory(games_dir);
            status_msg = "Scanned " + std::to_string(games.size()) + " game(s)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) browser.open("", true);
        ImGui::Separator();
        if (games.empty()) {
            ImGui::TextDisabled("No games found. Scan a directory.");
        } else {
            for (int i = 0; i < (int)games.size(); ++i) {
                if (ImGui::Selectable(games[i].name.c_str(), selected_game == i)) {
                    selected_game = i;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && selected_game == i) {
                    auto image = ee::elf::Image::load_file(games[i].path);
                    if (image) {
                        auto res = ee::analysis::analyze(*image);
                        log_msg("[studio] %s: %zu functions, %zu jump tables",
                                games[i].name.c_str(), res.functions.size(), res.jump_tables.size());
                        ee::codegen::Config cfg;
                        std::string module = ee::codegen::emit_module(*image, res, cfg);
                        log_msg("[studio] Generated %zu bytes of C++", module.size());
                        disasm_lines.clear();
                        for (const auto& sec : image->sections()) {
                            if (!sec.executable()) continue;
                            for (ee::u32 va = sec.addr; va + 4 <= sec.addr + sec.size; va += 4) {
                                auto w = image->read_u32(va);
                                if (!w) continue;
                                auto in = ee::r5900::decode(*w);
                                char line[256];
                                snprintf(line, sizeof(line), "  %08X: %08X  %s",
                                         va, *w, ee::r5900::disassemble(in, va).c_str());
                                disasm_lines.push_back(line);
                            }
                        }
                        show_disasm = true;
                    }
                }
            }
        }
        ImGui::End();

        // Pipeline Panel
        if (selected_game >= 0 && selected_game < (int)games.size()) {
            ImGui::Begin("Pipeline", nullptr, ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Game: %s", games[selected_game].name.c_str());
            if (ImGui::Button("Analyze")) {
                auto image = ee::elf::Image::load_file(games[selected_game].path);
                if (image) {
                    auto res = ee::analysis::analyze(*image);
                    log_msg("[studio] Functions: %zu", res.functions.size());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Recompile")) {
                auto image = ee::elf::Image::load_file(games[selected_game].path);
                if (image) {
                    auto res = ee::analysis::analyze(*image);
                    std::string out = games[selected_game].path + ".recomp.cpp";
                    ee::codegen::Config cfg;
                    auto module = ee::codegen::emit_module(*image, res, cfg);
                    std::ofstream(out) << module;
                    log_msg("[studio] Wrote %s", out.c_str());
                }
            }
            if (games[selected_game].is_iso) {
                ImGui::Separator();
                ee::rt::Cdvd cdvd;
                if (cdvd.open(games[selected_game].path)) {
                    ImGui::Text("ISO sectors: %u", cdvd.sector_count());
                    auto boot = cdvd.find_boot_elf();
                    if (boot) ImGui::Text("Boot: %s", boot->c_str());
                }
            }
            ImGui::End();
        }

        // Disassembly Panel
        if (show_disasm) {
            ImGui::Begin("Disassembly", &show_disasm);
            ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_Border);
            for (const auto& line : disasm_lines)
                ImGui::TextUnformatted(line.c_str());
            ImGui::EndChild();
            ImGui::End();
        }

        // Console
        if (show_console) {
            ImGui::Begin("Console", &show_console);
            ImGui::BeginChild("cscroll", ImVec2(0, 0), ImGuiChildFlags_Border);
            ImGui::TextUnformatted(log_text.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::End();
        }

        ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("%s", status_msg.c_str());
        ImGui::End();

        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
#else
        // No Dear ImGui: there is nothing to render, so just keep the window
        // open (do NOT auto-quit after a delay -- that made ee-studio look like
        // it "doesn't work", flashing a blank window closed). Build with
        // -DEE_WITH_IMGUI=ON (the default) for the real UI.
        if (!noted_no_imgui) {
            std::fprintf(stderr, "ee-studio: built without Dear ImGui "
                                 "(-DEE_WITH_IMGUI=OFF); showing an empty window. "
                                 "Rebuild with ImGui for the UI.\n");
            noted_no_imgui = true;
        }
#endif
        SDL_RenderPresent(renderer);
    }

#ifdef EE_HAS_IMGUI
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
#endif
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#else // !EE_HAS_SDL3
#include "../sdl3_required.hpp"
int main() {
    return ee::app::sdl3_required("ee-studio");
}
#endif
