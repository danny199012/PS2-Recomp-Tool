// Runner: loads an ELF, registers recompiled functions, applies game overrides,
// and starts execution at the ELF entry point.
//
// Usage: ee-runner <file.elf> [--config FILE] [--import FILE]...
//
// This ties together:
//  - The C++ runtime (ee_runtime.lib)
//  - The generated .recomp.cpp file (compiled separately and linked in)
//  - Game-specific overrides (registered via static init)

#include <ee/elf.hpp>
#include <ee/runtime.hpp>
#include <ee/game_overrides.hpp>
#include <ee/kernel.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>

// Load-address sampler (bring-up diagnostics, defined in ee_runtime).
extern "C" void ee_dump_loads();

// Forward declaration from the generated recomp file.
// The .recomp.cpp file must be compiled and linked alongside this runner.
void register_functions(ee::rt::Runtime& rt);

using namespace ee;
using namespace ee::rt;

static void usage(const char* prog) {
    fprintf(stderr, "usage: %s <file.elf> [options]\n", prog);
    fprintf(stderr, "  --config FILE    PS2Recomp-compatible TOML config\n");
    fprintf(stderr, "  --import FILE    import function names (.csv or .json)\n");
    fprintf(stderr, "  --no-scan        disable prologue scan\n");
}

// Crash attribution: SEH wrapper around the guest call loop (MSVC). Reports
// the guest PC (Runtime::call records ctx.pc) and the native exception code,
// then exits so the trace shows exactly where execution died.
static void safe_guest_call(Runtime& rt, EEContext& ctx) {
    __try {
        rt.call(ctx, ctx.pc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "\n[crash] native exception 0x%08lX at guest PC 0x%08X (a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X sp=0x%08X ra=0x%08X)\n",
                (unsigned long)GetExceptionCode(), ctx.pc,
                ee::u32(ee::rt::gpr32(ctx, 4)), ee::u32(ee::rt::gpr32(ctx, 5)),
                ee::u32(ee::rt::gpr32(ctx, 6)), ee::u32(ee::rt::gpr32(ctx, 7)),
                ee::u32(ee::rt::gpr32(ctx, 29)), ee::u32(ee::rt::gpr32(ctx, 31)));
        fflush(stderr);
        ExitProcess(3);
    }
}

// First-chance fault logger: prints the faulting virtual address and the
// module-relative offset of the faulting instruction (map with the .map file).
static const unsigned char* g_ram_base = nullptr; // set after Runtime ctor
static LONG CALLBACK fault_logger(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        HMODULE base = GetModuleHandleA(nullptr);
        const uintptr_t pc = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        fprintf(stderr, "[seh] AV access=%s fault_addr=0x%llX pc=%p (module+0x%llX)",
                ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "READ",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long long)(pc - reinterpret_cast<uintptr_t>(base)));
        if (g_ram_base) {
            const long long off = (long long)(intptr_t)ep->ExceptionRecord->ExceptionInformation[1]
                                - (long long)(intptr_t)g_ram_base;
            fprintf(stderr, " ram%+lld (guest 0x%llX)", off,
                    off >= 0 ? (unsigned long long)off : 0ull);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    std::string elf_path = argv[1];
    std::string config_path;
    std::vector<std::string> imports;
    bool do_scan = true;
    std::string iso_path;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--import" && i + 1 < argc) {
            imports.emplace_back(argv[++i]);
        } else if (arg == "--iso" && i + 1 < argc) {
            iso_path = argv[++i];
        } else if (arg == "--no-scan") {
            do_scan = false;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
    }

    // Step 1: Load the ELF image
    std::string error;
    auto image = elf::Image::load_file(elf_path, &error);
    if (!image) {
        fprintf(stderr, "Error loading ELF: %s\n", error.c_str());
        return 1;
    }

    printf("Loaded ELF: %s\n", elf_path.c_str());
    printf("  Entry point: 0x%08X\n", image->entry());
    printf("  Segments: %zu\n", image->segments().size());

    AddVectoredExceptionHandler(1, fault_logger);

    // Step 2: Create runtime and load ELF sections into guest memory
    Runtime rt;
    g_ram_base = rt.mem.ram.data();
    if (!rt.load_elf(*image, &error)) {
        fprintf(stderr, "Error loading ELF into memory: %s\n", error.c_str());
        return 1;
    }

    printf("ELF loaded into guest memory.\n");

    // Step 2b: open the disc image for the CDVD HLE (cdvdfsv RPC server).
    if (!iso_path.empty() && !rt.open_disc(iso_path)) {
        fprintf(stderr, "Warning: disc image %s could not be opened (CDVD reads return no data)\n",
                iso_path.c_str());
    }

    // Step 3: Apply game-specific overrides based on ELF name/CRC
    const std::string elf_name = elf_path.substr(elf_path.find_last_of("/\\") + 1);
    ee::u32 crc = 0;
    {
        std::ifstream f(elf_path, std::ios::binary);
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        // Standard CRC-32 (IEEE 802.3, reflected, as used by run_recomp.py).
        crc = 0xFFFFFFFFu;
        for (unsigned char b : buf) {
            crc ^= b;
            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        crc ^= 0xFFFFFFFFu;
    }
    GameOverrideRegistry::instance().apply_for_game(rt, elf_name, crc);

    printf("Game overrides applied for: %s (CRC: 0x%08X)\n", elf_name.c_str(), crc);

    // Step 4: Analyze and register recompiled functions from the .recomp.cpp file
    // The register_functions() call binds addresses to compiled MIPS code.
    try {
        register_functions(rt);
        printf("Registered %zu recompiled functions.\n", rt.functions.size());
    } catch (const std::exception& e) {
        fprintf(stderr, "Error registering functions: %s\n", e.what());
        return 1;
    }

    // Step 5: Start execution at the ELF entry point
    EEContext ctx;
    ctx.rt = &rt;
    ctx.pc = image->entry();

    printf("Starting execution at entry point 0x%08X...\n", image->entry());
    printf("(Press Ctrl+C to stop)\n\n");

    // Run the main loop - execute functions until kernel requests quit
    u64 iterations = 0;
    while (!(rt.kernel && rt.kernel->quit_requested())) {
        safe_guest_call(rt, ctx);
        // Bring-up diagnostics: dump the top polled guest addresses.
        if (++iterations % 2000000ull == 0)
            ee_dump_loads();
    }

    int exit_code = rt.kernel ? rt.kernel->exit_code() : 0;
    printf("\nProgram exited with code: %d\n", exit_code);
    return exit_code;
}
