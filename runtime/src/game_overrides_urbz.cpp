// SPDX-License-Identifier: GPL-3.0-only
// Game override for The Sims: Urban Stories (SLUS-21066)
// Registers stub handlers and binds specific function addresses.

#include <ee/game_overrides.hpp>
#include <ee/types.hpp>
#include <ee/runtime.hpp>
#include <ee/kernel.hpp>

using namespace ee::rt;

namespace {

// --- Sims Engine Debug Print Stub ---
void DPRINT_stub(EEContext& ctx) {
    ee::u32 str_addr = gpr32(ctx, 4); // $a0
    (void)str_addr;
}

// --- Sims Memory Allocation Stubs ---
void sim_malloc(EEContext& ctx) {
    ee::u32 size = gpr32(ctx, 4); // $a0 = size argument
    void* ptr = nullptr;
    if (ctx.rt->kernel.get()) {
        ptr = ctx.rt->kernel->malloc(size);
    }
    set32(ctx, 2, ee::u64(ptr)); // $v0 = return value
}

void sim_free(EEContext& ctx) {
    ee::u32 addr = gpr32(ctx, 4); // $a0 = pointer to free
    if (ctx.rt->kernel.get() && addr != 0) {
        ctx.rt->kernel->free(reinterpret_cast<void*>(addr));
    }
    set32(ctx, 2, 0); // $v0 = return value (void)
}

// --- CDVD/IO Stubs for Sims ---
void cdvd_stub_read(EEContext& ctx) {
    ee::u32 count = gpr32(ctx, 6); // $a2
    set32(ctx, 2, count); // Return requested count as success
}

// --- Callback Stubs ---
void cb_dummy(EEContext&) {}
void cb_return_zero(EEContext& ctx) { set32(ctx, 2, 0); }
void cb_return_one(EEContext& ctx) { set32(ctx, 2, 1); }

// Helper lambdas for math stubs
static ee::rt::StubHandler make_divdi3() {
    return [](EEContext& ctx) -> void {
        ee::u64 a = gpr(ctx, 5);
        ee::u64 b = gpr(ctx, 7);
        set32(ctx, 2, ee::u64(static_cast<ee::s32>(a) / static_cast<ee::s32>(b)));
    };
}

static ee::rt::StubHandler make_moddi3() {
    return [](EEContext& ctx) -> void {
        ee::u64 a = gpr(ctx, 5);
        ee::u64 b = gpr(ctx, 7);
        set32(ctx, 2, ee::u64(static_cast<ee::s32>(a) % static_cast<ee::s32>(b)));
    };
}

static ee::rt::StubHandler make_udivdi3() {
    return [](EEContext& ctx) -> void {
        ee::u64 a = gpr(ctx, 5);
        ee::u64 b = gpr(ctx, 7);
        set32(ctx, 2, ee::u64(a / b));
    };
}

static ee::rt::StubHandler make_umoddi3() {
    return [](EEContext& ctx) -> void {
        ee::u64 a = gpr(ctx, 5);
        ee::u64 b = gpr(ctx, 7);
        set32(ctx, 2, ee::u64(a % b));
    };
}

// --- Kernel-area glue blobs ------------------------------------------------------------
//
// Old-libkernel games copy their own libkernel glue (raw code blobs stored in
// .rodata) into the kernel-resident low RAM at boot and then call into the
// kernel area. SLUS-21066 installs three blobs (observed via the boot trace):
//   0x00466158 (1960 bytes) -> 0x80074000
//   0x00467110 ( 816 bytes) -> 0x80075000
//   0x00466920 (1856 bytes) -> 0x80076000
// The recompiler emits the blobs as functions at their source addresses (they
// are seeded as analysis roots in the game's functions.csv), and these aliases
// map the kernel-area entry points onto them so Runtime::call executes the
// real installed code instead of the "call into kernel area (HLE: v0=0)" nop.
void register_kernel_blob_aliases(Runtime& rt) {
    rt.alias(0x80074000u, 0x00466158u);
    rt.alias(0x80075000u, 0x00467110u);
    rt.alias(0x80076000u, 0x00466920u);
}

// --- Register stubs for the Sims engine ---
void register_urbz_stubs(Runtime& rt) {
    register_kernel_blob_aliases(rt);

    // Memory management
    rt.add_stub("_malloc_r", sim_malloc);
    rt.add_stub("malloc_r", sim_malloc);
    rt.add_stub("free_r", sim_free);
    rt.add_stub("calloc_r", sim_malloc);
    rt.add_stub("realloc_r", sim_malloc);

    // File I/O (CDVD-based for Sims)
    rt.add_stub("open_r", cb_return_one);
    rt.add_stub("close_r", cb_dummy);
    rt.add_stub("read_r", cdvd_stub_read);
    rt.add_stub("write_r", cb_dummy);
    rt.add_stub("lseek_r", cb_return_zero);
    rt.add_stub("stat_r", cb_return_zero);

    // Debug/logging
    rt.add_stub("DPRINT", DPRINT_stub);

    // Math library (Sims uses these heavily for vector math)
    rt.add_stub("__divdi3", make_divdi3());
    rt.add_stub("__moddi3", make_moddi3());
    rt.add_stub("__udivdi3", make_udivdi3());
    rt.add_stub("__umoddi3", make_umoddi3());

    // Float conversion stubs
    ee::rt::StubHandler fixsfdi = [](EEContext& ctx) -> void { set64(ctx, 2, 0); };
    rt.add_stub("__fixsfdi", fixsfdi);

    ee::rt::StubHandler floatdidf = [](EEContext& ctx) -> void {
        set32(ctx, 12, 0);
        set32(ctx, 13, 0);
    };
    rt.add_stub("__floatdidf", floatdidf);

    ee::rt::StubHandler floatundidf = [](EEContext& ctx) -> void {
        set32(ctx, 12, 0);
        set32(ctx, 13, 0);
    };
    rt.add_stub("__floatundidf", floatundidf);

    // Exception handling (Sims uses C++ exceptions)
    rt.add_stub("__gxx_personality_v0", cb_dummy);
    rt.add_stub("__cxa_allocate_exception", sim_malloc);
    rt.add_stub("__cxa_free_exception", sim_free);
    rt.add_stub("__cxa_throw", cb_dummy);
    rt.add_stub("__cxa_pure_virtual", cb_dummy);
    rt.add_stub("__cxa_atexit", cb_dummy);

    // Global constructors/destructors
    rt.add_stub("__do_global_dtors_aux", cb_dummy);
    rt.add_stub("__frame_dummy_init_array_entry", cb_dummy);
    rt.add_stub("__do_global_ctors_aux", cb_dummy);

    // SIF (Synchronous Interface for Sims)
    rt.add_stub("SifSetVuCode", cb_return_zero);
    rt.add_stub("SifCallVmu", cb_return_zero);

    // GS (Graphics Synthesizer) stubs
    rt.add_stub("GsChangeClearMode", cb_dummy);

    printf("[urbz] All Sims engine stubs registered\n");
}

} // namespace

// Register the game override via the static initialization macro
EE_REGISTER_GAME_OVERRIDE(urbz, "SLUS_210.66", 0, [](Runtime& rt) {
    register_urbz_stubs(rt);
});
