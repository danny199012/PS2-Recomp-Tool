// SPDX-License-Identifier: GPL-3.0-only
// Kernel HLE test: hand-written functions in the shape of recompiled code,
// exercising the thread scheduler + semaphores through the syscall path.
#include <ee/kernel.hpp>
#include <ee/runtime.hpp>

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace ee;
using namespace ee::rt;

static std::vector<std::string> g_log;

// "main": create sema, create+start thread (arg = sema id), wait, log, exit.
static void fn_main(EEContext& ctx) {
    // sema params at 0x1000: { count=0, max=1, attr=0, option=0 }
    st32(ctx, 0x1000, 0);
    st32(ctx, 0x1004, 1);
    st32(ctx, 0x1008, 0);
    st32(ctx, 0x100C, 0);
    set64(ctx, 4, 0x1000);
    syscall(ctx, 0x40); // CreateSema
    const u32 sema = gpr32(ctx, 2);
    CHECK(sema == 1);

    // thread params at 0x1010: { status=0, func=0x2000, stack=0x1003000, gp=0,
    //                            priority=32, stack_size=0x1000, ... }
    st32(ctx, 0x1010, 0);
    st32(ctx, 0x1014, 0x2000);
    st32(ctx, 0x1018, 0x1003000);
    st32(ctx, 0x101C, 0);
    st32(ctx, 0x1020, 32);
    st32(ctx, 0x1024, 0x1000);
    set64(ctx, 4, 0x1010);
    syscall(ctx, 0x20); // CreateThread
    const u32 tid = gpr32(ctx, 2);
    CHECK(tid == 1);

    set64(ctx, 4, tid);
    set64(ctx, 5, sema);
    syscall(ctx, 0x22); // StartThread

    set64(ctx, 4, sema);
    syscall(ctx, 0x44); // WaitSema (blocks until the thread signals)

    g_log.push_back("main done");
    syscall(ctx, 4); // KExit
}

// "thread": a0 = sema id; log, signal, exit.
static void fn_thread(EEContext& ctx) {
    g_log.push_back("thread hello");
    syscall(ctx, 0x42); // SignalSema
    syscall(ctx, 0x23); // ExitThread
}

int main() {
    Runtime rt;
    rt.add(0x100000, fn_main);
    rt.add(0x2000, fn_thread);

    EEContext ctx;
    ctx.rt = &rt;
    set64(ctx, 29, 0x1002000); // main stack

    rt.call(ctx, 0x100000);

    CHECK(rt.kernel->quit_requested());
    CHECK(g_log.size() == 2);
    if (g_log.size() == 2) {
        CHECK(g_log[0] == "thread hello");
        CHECK(g_log[1] == "main done");
    }

    // Misc syscall behavior checks (on the same runtime, fresh context).
    EEContext c2;
    c2.rt = &rt;
    set64(c2, 4, 0); // a0 = 0
    syscall(c2, 0x2F);          // GetThreadId
    CHECK(gpr32(c2, 2) == 0);   // main thread id
    syscall(c2, 0x7F);          // GetMemorySize
    CHECK(gpr32(c2, 2) == 32u * 1024 * 1024);
    syscall(c2, 0x7E);          // MachineType
    CHECK(gpr32(c2, 2) == 0x59);
    set64(c2, 4, 3);
    syscall(c2, 0x14);          // _EnableIntc(3)
    set64(c2, 4, 5);
    syscall(c2, 0x14);          // _EnableIntc(5)
    set64(c2, 4, 3);
    syscall(c2, 0x15);          // _DisableIntc(3) -> returns previous mask
    CHECK(gpr32(c2, 2) == (1u << 3 | 1u << 5));

    std::printf("test_kernel: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
