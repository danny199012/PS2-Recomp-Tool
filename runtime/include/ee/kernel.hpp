// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// EE kernel HLE: syscall dispatch (ps2sdk syscallnr.h numbering), a cooperative
// thread scheduler with priority-based preemption at syscall boundaries,
// semaphores, and MMIO stubs (EE STDOUT at 0x1000F180, GS privileged regs).
//
// Threading model: each EE thread gets one host thread; a global "baton"
// ensures exactly one EE thread runs at a time (deterministic, matches the
// PS2's single-EE-core semantics). Blocking syscalls park the host thread.

#include <ee/runtime.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ee::rt {

class Kernel {
public:
    explicit Kernel(Runtime& rt);
    ~Kernel(); // stops and joins all threads

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // Dispatch a syscall from recompiled code. `code` is the sign-extended
    // 20-bit code field of the syscall instruction (negative = i-variant).
    void syscall(EEContext& ctx, s32 code);

    bool quit_requested() const { return m_quit; }
    s32 exit_code() const { return m_exit_code; }

    // Called by Runtime::load_elf.
    void note_heap_base(u32 addr) { m_heap_start = addr; m_heap_free = addr; m_heap_end = addr + 4 * 1024 * 1024; }


    // --- dynamic heap allocator (cooperative bump allocator) -----------------------
    void*  malloc(u32 size);
    void   free(void* ptr);

    // --- clock / deferred interrupt delivery ---------------------------------------
    // Host-driven simulated bus clock (147.456 MHz, advanced by a clock thread).
    // Alarms (SetAlarm) and INTC interrupts (AddIntcHandler) are queued by the
    // clock thread and delivered to the guest at syscall boundaries, on the
    // interrupted thread's EE context (matches HLE recomp runtime practice).
    u64 busclock() const { return m_busclock.load(std::memory_order_relaxed); }
    void raise_intc(u32 cause, u32 arg); // thread-safe: queue + fire-at-boundary
    void raise_dmac(u32 channel, u32 arg); // thread-safe: queue + fire-at-boundary
    void fire_pending(EEContext& ctx);   // deliver queued ints/alarms (syscall ctx)
    // Fast check for injected guest-side polling (see ee_poll_interrupts).
    bool has_pending() const { return m_pending_count.load(std::memory_order_relaxed) > 0; }

private:
    struct PendingInt { u32 handler; u32 cause; u32 arg; };

public:
    // Run one queued interrupt handler on ctx (saves/restores scratch regs).
    void invoke_handler(EEContext& ctx, const PendingInt& p);

private:
    std::atomic<u64> m_busclock{0};
    std::thread m_clock_thread;
    std::deque<PendingInt> m_pending; // guarded by m_sched
    std::atomic<int> m_pending_count{0}; // fast check for guest-side polling
    int m_in_handler = 0; // >0 while an interrupt handler runs (INTC masked)
    void clock_thread_main();

    Runtime& rt;

    enum ThreadStatus : u32 {
        THS_RUN = 0x01,
        THS_READY = 0x02,
        THS_WAIT = 0x04,
        THS_SUSPEND = 0x08,
        THS_DORMANT = 0x10,
    };
    enum WaitReason : u32 { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2, WAIT_JOIN = 3 };

    struct Thread {
        u32 id = 0;
        u32 entry = 0;
        u32 stack = 0;
        u32 stack_size = 0;
        u32 gp = 0;
        u32 priority = 0;
        u32 init_priority = 0;
        u32 status = THS_DORMANT;
        WaitReason wait_reason = WAIT_NONE;
        u32 wait_id = 0;
        u32 wakeup_count = 0;
        u32 suspend_count = 0;
        EEContext ctx_storage;
        EEContext* ctx = nullptr; // ctx_storage, or the runner's context for the main thread
        std::thread host;
        std::mutex m;
        std::condition_variable cv;
        bool allowed = false; // scheduler baton
        bool exit_requested = false;
    };

    struct Sema {
        u32 id = 0;
        s32 count = 0;
        s32 max_count = 1;
        u32 attr = 0;
        u32 option = 0;
        std::deque<Thread*> waiters; // FIFO
    };

    // --- scheduler state ---
    std::mutex m_sched;
    std::vector<std::unique_ptr<Thread>> m_threads;
    std::deque<Thread*> m_ready; // sorted: lower priority value first
    Thread* m_current = nullptr;
    u32 m_dispatch_disabled = 0;
    bool m_quit = false;
    bool m_destroying = false;
    s32 m_exit_code = 0;

    std::vector<std::unique_ptr<Sema>> m_semas;

    Thread* ensure_main_thread(EEContext& ctx);
    Thread* find_thread(u32 id);
    Sema* find_sema(u32 id);
    void ready_insert(Thread* t);
    void ready_remove(Thread* t);
    Thread* pick_next();
    void switch_to(Thread* target, Thread* prev);
    void block_current(ThreadStatus status, WaitReason reason, u32 wait_id);
    void maybe_preempt();
    void thread_main(Thread* t);
    void finish_thread(Thread* t);
    void park_forever(Thread* t);

    // --- syscall handlers (args in $a0-$a3, result to $v0) ---
    void sys_create_thread(EEContext& ctx);
    void sys_delete_thread(EEContext& ctx);
    void sys_start_thread(EEContext& ctx);
    void sys_exit_thread(EEContext& ctx);
    void sys_terminate_thread(EEContext& ctx);
    void sys_change_thread_priority(EEContext& ctx);
    void sys_rotate_ready(EEContext& ctx);
    void sys_release_wait_thread(EEContext& ctx);
    void sys_refer_thread_status(EEContext& ctx);
    void sys_sleep_thread(EEContext& ctx);
    void sys_wakeup_thread(EEContext& ctx);
    void sys_cancel_wakeup(EEContext& ctx);
    void sys_suspend_thread(EEContext& ctx);
    void sys_resume_thread(EEContext& ctx);
    void sys_create_sema(EEContext& ctx);
    void sys_delete_sema(EEContext& ctx);
    void sys_signal_sema(EEContext& ctx);
    void sys_wait_sema(EEContext& ctx);
    void sys_poll_sema(EEContext& ctx);
    void sys_refer_sema_status(EEContext& ctx);
    void sys_print(EEContext& ctx);

    // --- misc state ---
    u32 m_intc_mask = 0;
    u32 m_dmac_mask = 0;
    u32 m_gs_imr = 0;
    u32 m_heap_end = 0;
    u32 m_heap_start = 0;
    u32 m_heap_free = 0;
    std::unordered_map<u32, std::pair<u32, u32>> m_intc_handlers; // cause -> {handler, arg}
    std::unordered_map<u32, std::pair<u32, u32>> m_dmac_handlers; // channel -> {handler, arg}
    std::unordered_map<u32, u32> m_user_syscalls;
    struct Alarm { u32 handler; u32 arg; u64 deadline; };
    std::unordered_map<u32, Alarm> m_alarms; // id -> alarm (fired by clock thread)
    u32 m_next_alarm = 1;
    std::set<s32> m_reported_syscalls;
    bool m_trace = true; // kernel bring-up tracing (AddIntcHandler/EnableIntc/alarms/SIF)



    void log_unimplemented(s32 code, const char* name);
};

} // namespace ee::rt
