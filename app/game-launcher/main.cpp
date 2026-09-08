// SPDX-License-Identifier: GPL-3.0-only
//
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS // MSVC: fopen/strncpy/etc. are fine for us here
#endif
//
// game-launcher: per-game release launcher template for EERecomp ports.
//
// A game's recompiled C++ (EE_GAME_RECOMP_SOURCES) and overrides
// (EE_GAME_OVERRIDES_SOURCES) are linked into this executable. The launcher
// ships no game assets: on first launch its GUI asks the user to provide a
// legally obtained PS2 disc image (.iso/.bin), reads SYSTEM.CNF, extracts the
// boot ELF, and runs the embedded recompiled game.
//
// Modes:
//   GUI  (SDL3 + Dear ImGui): disc prompt -> extract -> run, with a console
//         and a settings panel (renderer/audio placeholders).
//   CLI  (no SDL3 / EE_LAUNCHER_IMGUI=OFF):
//         game-launcher --extract <disc.iso> [--out dir]
//         game-launcher --run <disc.iso>

#include "launcher_config.h"

#include <ee/cdvd.hpp>
#include <ee/elf.hpp>
#include <ee/game_overrides.hpp>
#include <ee/kernel.hpp>
#include <ee/runtime.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(EE_HAS_SDL3) && defined(EE_HAS_IMGUI)
#define EE_GUI_MODE 1
#else
#define EE_GUI_MODE 0
#endif

#if EE_GUI_MODE
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../ui_filebrowser.hpp"
#endif

#if EE_GAME_HAS_RECOMP
void register_functions(ee::rt::Runtime& rt);
#endif

namespace fs = std::filesystem;

namespace {

using ee::u8;
using ee::u32;
using ee::s32;

std::string game_name() {
    return EE_GAME_NAME[0] ? static_cast<const char*>(EE_GAME_NAME) : "EERecomp Port";
}

std::string data_dir(const char* argv0) {
    fs::path exe = fs::path(argv0).parent_path();
    return (exe / "game_data" / game_name()).string();
}

// --- tiny helpers ------------------------------------------------------------

std::string leaf_name(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

u32 crc32_of(const u8* data, size_t n) {
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool write_file(const std::string& path, const std::vector<u8>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

bool read_file_to(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(size_t(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

// --- per-game settings (renderer / audio placeholders) ------------------------

struct Settings {
    int render_backend = 0; // 0 = Software (current), 1 = Vulkan (planned)
    int internal_res = 1;   // 0 = native, 1 = 1x, 2 = 2x ...
    bool vsync = true;
    int volume = 100; // 0..100
};

void settings_load(Settings& s, const std::string& path) {
    std::vector<u8> raw;
    if (!read_file_to(path, raw)) return;
    std::string text(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        const int val = std::atoi(v.c_str());
        if (k == "render_backend") s.render_backend = val;
        else if (k == "internal_res") s.internal_res = val;
        else if (k == "vsync") s.vsync = val != 0;
        else if (k == "volume") s.volume = val;
    }
}

void settings_save(const Settings& s, const std::string& path) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "render_backend=%d\ninternal_res=%d\nvsync=%d\nvolume=%d\n",
                  s.render_backend, s.internal_res, int(s.vsync), s.volume);
    std::ofstream f(path);
    if (f) f.write(buf, std::streamsize(std::strlen(buf)));
}

// --- disc state ---------------------------------------------------------------

struct DiscState {
    bool open = false;
    std::string path;
    ee::rt::Cdvd cdvd;
    std::string boot_elf;  // SYSTEM.CNF boot path (e.g. "SLUS_203.61")
    std::string boot_name; // leaf name
    std::vector<u8> boot_bytes;
    u32 crc = 0;
    std::optional<ee::elf::Image> image;
    std::string error;
};

bool open_disc(DiscState& st, const std::string& path) {
    st = DiscState{};
    st.path = path;
    if (!st.cdvd.open(path)) {
        st.error = "Could not open disc image: " + path;
        return false;
    }
    auto boot = st.cdvd.find_boot_elf();
    if (!boot) {
        st.error = "SYSTEM.CNF not found (not a PS2 boot disc image?)";
        return false;
    }
    st.boot_elf = *boot;
    st.boot_name = leaf_name(*boot);
    auto bytes = st.cdvd.read_file(*boot);
    if (!bytes) {
        st.error = "Could not read boot file from disc: " + *boot;
        return false;
    }
    st.boot_bytes = std::move(*bytes);
    st.crc = crc32_of(st.boot_bytes.data(), st.boot_bytes.size());
    auto img = ee::elf::Image::load_bytes(st.boot_bytes);
    if (!img) {
        st.error = "Boot file does not parse as an ELF: " + *boot;
        return false;
    }
    st.image = std::move(*img);
    st.open = true;
    return true;
}

bool save_boot_elf(const DiscState& st, const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return write_file((fs::path(dir) / "boot.elf").string(), st.boot_bytes);
}

// Copy of the disc data the worker thread needs, so it never touches the
// (non-copyable, mutating) Cdvd object while the GUI thread is alive.
struct BootCopy {
    std::string boot_name;
    u32 crc = 0;
    ee::elf::Image image;
};

// Run the embedded recompiled game. Called on the worker thread.
s32 run_game(ee::rt::Runtime& rt, const BootCopy& boot) {
#if EE_GAME_HAS_RECOMP
    register_functions(rt);
#else
    std::fprintf(stderr, "[game-launcher] no recompiled game code linked "
                         "(EE_GAME_RECOMP_SOURCES was empty); cannot run.\n");
    return -1;
#endif
    ee::rt::GameOverrideRegistry::instance().apply_for_game(rt, boot.boot_name, boot.crc);
    std::string err;
    if (!rt.load_elf(boot.image, &err)) {
        std::fprintf(stderr, "[game-launcher] load_elf: %s\n", err.c_str());
        return -1;
    }
    ee::rt::EEContext ctx;
    ctx.rt = &rt;
    rt.call(ctx, boot.image.entry());
    return rt.kernel ? rt.kernel->exit_code() : 0;
}
// --- headless CLI --------------------------------------------------------------

int cli_extract(const std::string& iso, const std::string& outdir, const char* argv0) {
    DiscState st;
    if (!open_disc(st, iso)) {
        std::fprintf(stderr, "error: %s\n", st.error.c_str());
        return 1;
    }
    std::printf("disc:       %s\n", iso.c_str());
    std::printf("sectors:    %u\n", st.cdvd.sector_count());
    std::printf("boot elf:   %s\n", st.boot_elf.c_str());
    std::printf("boot crc32: 0x%08X\n", st.crc);
    const std::string dir = outdir.empty() ? data_dir(argv0) : outdir;
    if (save_boot_elf(st, dir))
        std::printf("extracted:  %s%sboot.elf\n", dir.c_str(), dir.empty() ? "" : "/");
    else {
        std::fprintf(stderr, "error: could not write boot.elf into %s\n", dir.c_str());
        return 1;
    }
    return 0;
}

int cli_run(const std::string& iso) {
    DiscState st;
    if (!open_disc(st, iso)) {
        std::fprintf(stderr, "error: %s\n", st.error.c_str());
        return 1;
    }
    BootCopy boot;
    boot.boot_name = st.boot_name;
    boot.crc = st.crc;
    boot.image = *st.image;
    ee::rt::Runtime rt; // console sink unset -> guest output goes to stdout
    const s32 code = run_game(rt, boot);
    std::printf("\n[game-launcher] exited with code %d\n", int(code));
    return code == 0 ? 0 : 1;
}

int run_cli(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "game-launcher (per-game EERecomp port launcher)\n"
            "usage:\n"
            "  game-launcher --extract <disc.iso> [--out dir]   extract the boot ELF\n"
            "  game-launcher --run <disc.iso>                   run the game (headless)\n"
            "GUI mode is used when built with SDL3 + Dear ImGui.\n");
        return 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "--extract") {
        if (argc < 3) {
            std::fprintf(stderr, "--extract needs a disc path\n");
            return 1;
        }
        std::string outdir;
        for (int i = 3; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--out") == 0) outdir = argv[i + 1];
        return cli_extract(argv[2], outdir, argv[0]);
    }
    if (cmd == "--run") {
        if (argc < 3) {
            std::fprintf(stderr, "--run needs a disc path\n");
            return 1;
        }
        return cli_run(argv[2]);
    }
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}

} // namespace

#if EE_GUI_MODE
namespace {
int run_gui(int argc, char** argv); // implemented with the GUI (below)
}
#endif

int main(int argc, char** argv) {
#if EE_GUI_MODE
    return run_gui(argc, argv);
#else
    return run_cli(argc, argv);
#endif
}

#if EE_GUI_MODE
namespace {

struct GuiState {
    DiscState disc;
    Settings settings;
    std::string settings_path;
    std::string disc_input; // prefill from argv / drag-drop

    std::atomic<bool> running{false};
    s32 exit_code = 0;
    std::string status;
    std::string console;
    std::mutex console_mx;
    std::thread worker;

    // In-app file browser for the disc prompt (no OS-native dialog needed).
    uitools::FileBrowser browser;
};

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

void gui_log(GuiState& g, std::string text) {
    std::lock_guard<std::mutex> lk(g.console_mx);
    if (g.console.size() < (1u << 20)) g.console += std::move(text);
}

void fmt_crc(char* buf, size_t len, u32 crc) {
    std::snprintf(buf, len, "0x%08X", crc);
}

int run_gui(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("EERecomp Port", 1280, 800, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    enlarge_ui_font(io);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    GuiState g;
    {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s (EERecomp PC port)", game_name().c_str());
        SDL_SetWindowTitle(window, buf);
    }
    g.settings_path = (fs::path(data_dir(argv[0])) / "settings.txt").string();
    settings_load(g.settings, g.settings_path);
    g.status = "Provide a legally obtained dump of the game disc to begin.";

    // Prefill from argv: `game-launcher <disc.iso>` or `game-launcher --disc=<iso>`.
    if (argc >= 2) {
        std::string a = argv[1];
        if (a.rfind("--disc=", 0) == 0) a = a.substr(7);
        if (a.rfind("--", 0) != 0) g.disc_input = a;
    }

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
                g.disc_input = ev.drop.data;
                SDL_free(const_cast<char*>(ev.drop.data));
                if (open_disc(g.disc, g.disc_input)) {
                    save_boot_elf(g.disc, data_dir(argv[0]));
                    g.status = "Disc opened: " + g.disc.boot_elf;
                    gui_log(g, "disc:  " + g.disc.path + "\n");
                    gui_log(g, "boot:  " + g.disc.boot_elf + "\n");
                } else {
                    g.status = g.disc.error;
                }
            }
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();

        // In-app file browser (works without OS-native dialogs).
        g.browser.draw();
        if (g.browser.result_ready) {
            g.browser.result_ready = false;
            const std::string picked = g.browser.result;
            g.disc_input = picked;
            std::string lower = picked;
            for (auto& c : lower) c = char(std::tolower((unsigned char)c));
            const bool is_iso = lower.size() > 4 &&
                                (lower.substr(lower.size() - 4) == ".iso" ||
                                 lower.substr(lower.size() - 4) == ".bin");
            if (is_iso && open_disc(g.disc, picked)) {
                save_boot_elf(g.disc, data_dir(argv[0]));
                g.status = "Disc opened: " + g.disc.boot_elf;
                gui_log(g, "disc:  " + picked + "\n");
                gui_log(g, "boot:  " + g.disc.boot_elf + "\n");
            } else if (!is_iso) {
                g.status = "Selected file is not a disc image (.iso/.bin).";
            } else {
                g.status = g.disc.error;
            }
        }

        // --- Disc window ---
        ImGui::Begin("Disc");
        if (!g.disc.open) {
            ImGui::TextWrapped("This program does not contain any game assets.");
            ImGui::TextWrapped("Please select a legally obtained dump of the game disc "
                               "(.iso or .bin image) to begin:");
            ImGui::Separator();
            static char disc_buf[1024] = {};
            std::snprintf(disc_buf, sizeof(disc_buf), "%s", g.disc_input.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##disc", disc_buf, sizeof(disc_buf));
            g.disc_input = disc_buf;
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) g.browser.open(".iso;.bin;.elf");
            if (ImGui::Button("Open disc image")) {
                if (open_disc(g.disc, g.disc_input)) {
                    save_boot_elf(g.disc, data_dir(argv[0]));
                    g.status = "Disc opened: " + g.disc.boot_elf;
                    gui_log(g, "disc:  " + g.disc.path + "\n");
                    gui_log(g, "boot:  " + g.disc.boot_elf + "\n");
                } else {
                    g.status = g.disc.error;
                }
            }
            ImGui::TextWrapped("Tip: drag & drop a disc image onto this window.");
#if !EE_GAME_HAS_RECOMP
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Note: no recompiled game code linked (set "
                               "EE_GAME_RECOMP_SOURCES when building).");
#endif
        } else {
            ImGui::Text("Disc: %s", g.disc.path.c_str());
            ImGui::Text("Sectors: %u", g.disc.cdvd.sector_count());
            ImGui::Text("Boot ELF: %s", g.disc.boot_elf.c_str());
            char cb[32];
            fmt_crc(cb, sizeof cb, g.disc.crc);
            ImGui::Text("Boot CRC32: %s", cb);
#if EE_GAME_EXPECTED_CRC
            if (u32(EE_GAME_EXPECTED_CRC) != g.disc.crc)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                                   "Warning: disc CRC doesn't match expected %s", cb);
#endif
            if (EE_GAME_BOOT_ELF[0] && leaf_name(EE_GAME_BOOT_ELF) != g.disc.boot_name)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                                   "Warning: expected boot %s, found %s",
                                   EE_GAME_BOOT_ELF, g.disc.boot_name.c_str());
            ImGui::Separator();
            if (!g.running.load()) {
                if (ImGui::Button("Launch game")) {
                    g.running = true;
                    g.exit_code = 0;
                    save_boot_elf(g.disc, data_dir(argv[0]));
                    gui_log(g, "--- boot ---\n");
                    BootCopy boot;
                    boot.boot_name = g.disc.boot_name;
                    boot.crc = g.disc.crc;
                    boot.image = *g.disc.image;
                    g.worker = std::thread([&g, boot = std::move(boot)]() mutable {
                        ee::rt::Runtime rt;
                        rt.console = [&g](const char* p, size_t n) {
                            gui_log(g, std::string(p, n));
                        };
                        g.exit_code = run_game(rt, boot);
                        g.running = false;
                        char b[64];
                        std::snprintf(b, sizeof b, "--- exit (%d) ---\n", int(g.exit_code));
                        gui_log(g, b);
                    });
                }
                ImGui::SameLine();
                if (ImGui::Button("Change disc")) g.disc = DiscState{};
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                   "Running... (close the window to force quit)");
            }
        }
        ImGui::End();

        // --- Settings window (renderer / audio placeholders) ---
        ImGui::Begin("Settings");
        ImGui::TextWrapped("Renderer backend (Software is the current implementation; "
                           "Vulkan is planned).");
        const char* backends[] = {"Software (current)", "Vulkan (planned)"};
        ImGui::Combo("Renderer", &g.settings.render_backend, backends, 2);
        const char* res[] = {"Native", "1x", "2x", "3x", "4x"};
        ImGui::Combo("Internal resolution", &g.settings.internal_res, res, 5);
        ImGui::Checkbox("VSync", &g.settings.vsync);
        ImGui::SliderInt("Master volume", &g.settings.volume, 0, 100);
        if (ImGui::Button("Save settings")) {
            settings_save(g.settings, g.settings_path);
            g.status = "Settings saved.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) settings_load(g.settings, g.settings_path);
        ImGui::End();

        // --- Console window (guest EE stdout / _print) ---
        ImGui::Begin("Console");
        {
            std::lock_guard<std::mutex> lk(g.console_mx);
            ImGui::BeginChild("clog", ImVec2(0, 0), ImGuiChildFlags_Border);
            ImGui::TextUnformatted(g.console.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        ImGui::End();

        // --- Status bar ---
        ImGui::Begin("Status");
        ImGui::TextWrapped("%s", g.status.c_str());
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 16, 16, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    if (g.worker.joinable()) g.worker.join();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace
#endif // EE_GUI_MODE