// SPDX-License-Identifier: GPL-3.0-only
//
// Shared "SDL3 is required" notifier used by the GUI apps (ee-tools,
// ee-studio, game-launcher) when they are compiled without SDL3.
//
// The GUI apps degrade to a tiny stub when SDL3 is absent (see cmake/ee_sdl3).
// Previously that stub wrote one line to stderr and exited, so a console
// opened by double-clicking the exe in Explorer would flash and close before
// the message could be read. This helper prints a clear, actionable message
// and -- when running in an interactive console -- waits for Enter so the
// window stays open. It has no dependencies beyond the C runtime, so it is
// safe to include in the no-SDL3 build path.

#pragma once

#include <cstdio>

#if defined(_WIN32)
#include <io.h>
namespace ee::app::detail {
inline bool stdin_is_interactive() { return _isatty(_fileno(stdin)) != 0; }
}
#else
#include <unistd.h>
namespace ee::app::detail {
inline bool stdin_is_interactive() { return isatty(STDIN_FILENO) != 0; }
}
#endif

namespace ee::app {

// Prints the "SDL3 required" notice for `app_name` and, in an interactive
// console, blocks until Enter so the user can read it. Returns 1 (suitable as
// the stub's exit code).
inline int sdl3_required(const char* app_name) {
    std::fprintf(stderr,
        "%s: this is the headless stub build -- no GUI was compiled in.\n"
        "%s needs SDL3 to show its window, but SDL3 was not found when the\n"
        "project was configured.\n\n"
        "To get the GUI, reconfigure so SDL3 is available, then rebuild:\n"
        "    rmdir /s /q build          (or: Remove-Item -Recurse -Force build)\n"
        "    cmake -S . -B build\n"
        "    cmake --build build --config Release\n\n"
        "SDL3 is fetched and built from source automatically by default\n"
        "(EE_FETCH_SDL3=ON), so a clean configure usually just works -- expect\n"
        "a longer first build while SDL3 compiles.\n\n"
        "To use a preinstalled SDL3 instead, point CMake at it, e.g.:\n"
        "    cmake -S . -B build -DSDL3_DIR=\"C:/SDL3/cmake\"\n"
        "  or\n"
        "    cmake -S . -B build -DCMAKE_PREFIX_PATH=\"C:/SDL3\"\n",
        app_name, app_name);

    if (detail::stdin_is_interactive()) {
        std::fputs("\nPress Enter to exit... ", stderr);
        std::fflush(stderr);
        int c;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
            // discard until newline / EOF so the console stays open
        }
    }
    return 1;
}

} // namespace ee::app
