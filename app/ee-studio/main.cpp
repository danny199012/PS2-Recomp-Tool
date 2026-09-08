// SPDX-License-Identifier: GPL-3.0-only
// ee-studio: SDL3-based launcher for recompiled PS2 games.
// Loads an ELF, registers recompiled functions, runs the game, and presents
// the GS framebuffer. Requires SDL3 (found via find_package or FetchContent).

#ifdef EE_HAS_SDL3

#include <ee/elf.hpp>
#include <ee/gs_renderer.hpp>
#include <ee/hw.hpp>
#include <ee/iop.hpp>
#include <ee/runtime.hpp>

#include <SDL3/SDL.h>
#include <cstdio>
#include <string>

void register_functions(ee::rt::Runtime& rt);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <game.elf>\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("EERecomp", 640, 480, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Load and run the game.
    std::string error;
    auto image = ee::elf::Image::load_file(path, &error);
    if (!image) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    ee::rt::Runtime rt;
    register_functions(rt);
    rt.load_elf(*image);

    ee::rt::GsRenderer gs_renderer(rt.hw->gs);

    ee::rt::EEContext ctx;
    ctx.rt = &rt;
    ee::rt::set64(ctx, 29, 0x1002000); // stack

    // Start the game.
    rt.call(ctx, image->entry());

    bool running = true;
    while (running && !rt.kernel->quit_requested()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                running = false;
        }

        // Render the GS framebuffer.
        gs_renderer.render_frame();
        auto fb = gs_renderer.read_framebuffer_rgba();

        if (!fb.empty()) {
            SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 gs_renderer.fb_width, gs_renderer.fb_height);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, fb.data(), gs_renderer.fb_width * 4);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, tex, nullptr, nullptr);
                SDL_DestroyTexture(tex);
            }
        }
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#else // !EE_HAS_SDL3

#include <cstdio>
int main() {
    std::fprintf(stderr, "ee-studio requires SDL3. Build with -DEE_HAS_SDL3=ON.\n");
    return 1;
}

#endif // EE_HAS_SDL3
