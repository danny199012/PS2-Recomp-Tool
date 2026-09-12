// SPDX-License-Identifier: GPL-3.0-only
#include <ee/kernel.hpp>
#include <ee/hw.hpp>
#include <ee/iop.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

// Load-address sampler (bring-up diagnostics, defined in runtime.cpp).
extern "C" void ee_dump_loads();

namespace ee::rt {

Kernel::Kernel(Runtime& rt) : rt(rt) {
    m_clock_thread = std::thread(&Kernel::clock_thread_main, this);
}

Kernel::~Kernel() {
    m_destroying = true;
    m_quit = true;
    if (m_clock_thread.joinable())
        m_clock_thread.join();
    for (auto& t : m_threads) {
        if (!t->host.joinable())
            continue;
        {
            std::lock_guard lk(t->m);
            t->exit_requested = true;
            t->allowed = true;
        }
        t->cv.notify_one();
    }
    for (auto& t : m_threads)
        if (t->host.joinable())
            t->host.join();
}

// --- scheduler core ------------------------------------------------------------

Kernel::Thread* Kernel::ensure_main_thread(EEContext& ctx) {
    if (m_threads.empty()) {
        auto t = std::make_unique<Thread>();
        t->id = 0;
        t->priority = 0;
        t->init_priority = 0;
        t->status = THS_RUN;
        t->ctx = &ctx; // the runner's own context
        m_current = t.get();
        m_threads.push_back(std::move(t));
    }
    return m_threads[0].get();
}

Kernel::Thread* Kernel::find_thread(u32 id) {
    if (id < m_threads.size())
        return m_threads[id].get();
    return nullptr;
}

Kernel::Sema* Kernel::find_sema(u32 id) {
    if (id >= 1 && id <= m_semas.size())
        return m_semas[id - 1].get();
    return nullptr;
}

void Kernel::ready_insert(Thread* t) {
    // priority-ordered: lower value = higher priority; FIFO within a priority
    auto it = m_ready.begin();
    while (it != m_ready.end() && (*it)->priority <= t->priority)
        ++it;
    m_ready.insert(it, t);
}

void Kernel::ready_remove(Thread* t) {
    for (auto it = m_ready.begin(); it != m_ready.end(); ++it)
        if (*it == t) {
            m_ready.erase(it);
            return;
        }
}

Kernel::Thread* Kernel::pick_next() {
    return m_ready.empty() ? nullptr : m_ready.front();
}

// Hand the baton to `target` and park `prev`'s host thread until it is
// scheduled again (or torn down). Queue state must already be updated.
void Kernel::switch_to(Thread* target, Thread* prev) {
    {
        std::lock_guard lk(target->m);
        target->allowed = true;
    }
    target->cv.notify_one();
    if (prev) {
        std::unique_lock lk(prev->m);
        prev->cv.wait(lk, [&] { return prev->allowed || prev->exit_requested; });
        prev->allowed = false;
    }
}

// Block the current thread and switch to the next ready one.
void Kernel::block_current(ThreadStatus status, WaitReason reason, u32 wait_id) {
    Thread* prev = m_current;
    Thread* next = nullptr;
    {
        std::lock_guard lk(m_sched);
        prev->status = status;
        prev->wait_reason = reason;
        prev->wait_id = wait_id;
        next = pick_next();
        if (next) {
            ready_remove(next);
            m_current = next;
            next->status = THS_RUN;
        }
    }
    if (m_destroying)
        return; // never park during teardown
    if (!next) {
        // No runnable thread: either program end or a genuine deadlock.
        std::fprintf(stderr, "[kernel] no runnable thread while %u blocks (deadlock?)\n", prev->id);
        m_quit = true;
        return; // let the caller continue so the runner can exit
    }
    switch_to(next, prev);
}

// Preempt the current thread if a higher-priority thread is ready.
void Kernel::maybe_preempt() {
    if (m_destroying || m_dispatch_disabled > 0)
        return;
    Thread* prev = m_current;
    Thread* next = nullptr;
    {
        std::lock_guard lk(m_sched);
        next = pick_next();
        if (next && prev && next->priority < prev->priority) {
            ready_remove(next);
            prev->status = THS_READY;
            ready_insert(prev);
            m_current = next;
            next->status = THS_RUN;
        } else {
            next = nullptr;
        }
    }
    if (next)
        switch_to(next, prev);
}

void Kernel::thread_main(Thread* t) {
    {
        std::unique_lock lk(t->m);
        t->cv.wait(lk, [&] { return t->allowed || t->exit_requested; });
        t->allowed = false;
    }
    if (!t->exit_requested)
        rt.call(*t->ctx, t->entry);
    finish_thread(t);
}

void Kernel::finish_thread(Thread* t) {
    Thread* next = nullptr;
    {
        std::lock_guard lk(m_sched);
        t->status = THS_DORMANT;
        t->wait_reason = WAIT_NONE;
        // wake join waiters
        for (auto& other : m_threads) {
            if (other->status == THS_WAIT && other->wait_reason == WAIT_JOIN && other->wait_id == t->id) {
                other->status = THS_READY;
                other->wait_reason = WAIT_NONE;
                ready_insert(other.get());
            }
        }
        if (m_current == t) {
            next = pick_next();
            if (next) {
                ready_remove(next);
                m_current = next;
                next->status = THS_RUN;
            }
        }
    }
    if (next)
        switch_to(next, t);
    park_forever(t);
}

void Kernel::park_forever(Thread* t) {
    std::unique_lock lk(t->m);
    t->cv.wait(lk, [&] { return t->exit_requested; });
}

// --- thread syscalls -------------------------------------------------------------
// Param block layouts follow ps2sdk (ee_thread_t / ee_thread_status_t).

void Kernel::sys_create_thread(EEContext& ctx) {
    const u32 params = gpr32(ctx, 4); // a0
    auto t = std::make_unique<Thread>();
    t->id = u32(m_threads.size());
    t->entry = ld32(ctx, params + 4);
    t->stack = ld32(ctx, params + 8);
    t->gp = ld32(ctx, params + 12);
    t->priority = ld32(ctx, params + 16);
    t->init_priority = t->priority;
    t->stack_size = ld32(ctx, params + 20);
    t->status = THS_DORMANT;
    t->ctx = &t->ctx_storage;
    t->ctx_storage.rt = &rt;
    const u32 id = t->id;
    m_threads.push_back(std::move(t));
    set32(ctx, 2, id); // $v0 = thread id
}

void Kernel::sys_delete_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (!t || t == m_current || t->status == THS_RUN) {
        set32(ctx, 2, u32(-1));
        return;
    }
    ready_remove(t);
    for (auto& s : m_semas)
        if (s)
            s->waiters.erase(std::remove(s->waiters.begin(), s->waiters.end(), t), s->waiters.end());
    t->status = THS_DORMANT;
    if (t->host.joinable()) {
        {
            std::lock_guard lk(t->m);
            t->exit_requested = true;
            t->allowed = true;
        }
        t->cv.notify_one();
        t->host.join();
    }
    set32(ctx, 2, 0);
}

void Kernel::sys_start_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    const u32 arg = gpr32(ctx, 5);
    Thread* t = find_thread(id);
    if (!t || t->status != THS_DORMANT) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (t->host.joinable())
        t->host.join(); // previous run finished; reclaim
    t->ctx_storage = EEContext{};
    t->ctx = &t->ctx_storage;
    t->ctx_storage.rt = &rt;
    const u32 sp = (t->stack + t->stack_size) & ~0xFu;
    set64(*t->ctx, 29, sp);        // $sp
    set64(*t->ctx, 28, t->gp);     // $gp
    set64(*t->ctx, 4, arg);        // $a0 = arg
    t->ctx_storage.pc = t->entry;
    t->status = THS_READY;
    t->wait_reason = WAIT_NONE;
    t->exit_requested = false;
    t->allowed = false;
    t->host = std::thread(&Kernel::thread_main, this, t);
    {
        std::lock_guard lk(m_sched);
        ready_insert(t);
    }
    set32(ctx, 2, id);
    maybe_preempt();
}

void Kernel::sys_exit_thread(EEContext& ctx) {
    (void)ctx;
    Thread* t = m_current;
    if (!t)
        return;
    if (t->id == 0) {
        m_quit = true; // main thread exiting = program exit
        return;        // syscall returns; main's code runs to jr $ra -> runner
    }
    {
        std::lock_guard lk(m_sched);
        t->status = THS_DORMANT;
        for (auto& other : m_threads) {
            if (other->status == THS_WAIT && other->wait_reason == WAIT_JOIN && other->wait_id == t->id) {
                other->status = THS_READY;
                other->wait_reason = WAIT_NONE;
                ready_insert(other.get());
            }
        }
    }
    Thread* next = nullptr;
    {
        std::lock_guard lk(m_sched);
        next = pick_next();
        if (next) {
            ready_remove(next);
            m_current = next;
            next->status = THS_RUN;
        }
    }
    if (next)
        switch_to(next, t); // parks this thread mid-syscall; resumed only at teardown
    else
        park_forever(t);
}

void Kernel::sys_terminate_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (t == m_current) {
        sys_exit_thread(ctx);
        return;
    }
    ready_remove(t);
    for (auto& s : m_semas)
        if (s)
            s->waiters.erase(std::remove(s->waiters.begin(), s->waiters.end(), t), s->waiters.end());
    t->status = THS_DORMANT;
    set32(ctx, 2, 0);
}

void Kernel::sys_change_thread_priority(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    const u32 prio = gpr32(ctx, 5);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    const u32 old = t->priority;
    {
        std::lock_guard lk(m_sched);
        const bool was_ready = t->status == THS_READY;
        if (was_ready)
            ready_remove(t);
        t->priority = prio;
        if (was_ready)
            ready_insert(t);
    }
    set32(ctx, 2, old);
    maybe_preempt();
}

void Kernel::sys_rotate_ready(EEContext& ctx) {
    const u32 prio = gpr32(ctx, 4);
    Thread* prev = m_current;
    Thread* next = nullptr;
    {
        std::lock_guard lk(m_sched);
        for (Thread* t : m_ready)
            if (t->priority == prio) {
                next = t;
                break;
            }
        if (next && prev && prev->priority == prio) {
            ready_remove(next);
            prev->status = THS_READY;
            ready_insert(prev); // lands behind the rest of the same priority
            m_current = next;
            next->status = THS_RUN;
        } else {
            next = nullptr;
        }
    }
    if (next)
        switch_to(next, prev);
    set32(ctx, 2, 0);
}

void Kernel::sys_release_wait_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (t && t->status == THS_WAIT) {
        std::lock_guard lk(m_sched);
        t->status = THS_READY;
        t->wait_reason = WAIT_NONE;
        ready_insert(t);
    }
    set32(ctx, 2, 0);
    maybe_preempt();
}

void Kernel::sys_refer_thread_status(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    const u32 info = gpr32(ctx, 5);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (info) {
        st32(ctx, info + 0, t->status);
        st32(ctx, info + 4, t->entry);
        st32(ctx, info + 8, t->stack);
        st32(ctx, info + 12, t->gp);
        st32(ctx, info + 16, t->init_priority);
        st32(ctx, info + 20, t->priority);
        st32(ctx, info + 24, t->stack_size);
        st32(ctx, info + 28, 0); // attr
        st32(ctx, info + 32, 0); // option
        st32(ctx, info + 36, u32(t->wait_reason));
        st32(ctx, info + 40, t->wait_id);
        st32(ctx, info + 44, t->wakeup_count);
    }
    set32(ctx, 2, id);
}

void Kernel::sys_sleep_thread(EEContext& ctx) {
    Thread* t = m_current;
    if (t->wakeup_count > 0) {
        --t->wakeup_count;
        set32(ctx, 2, 0);
        return;
    }
    set32(ctx, 2, 0); // return value once woken
    block_current(THS_WAIT, WAIT_SLEEP, 0);
}

void Kernel::sys_wakeup_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    {
        std::lock_guard lk(m_sched);
        if (t->status == THS_WAIT && t->wait_reason == WAIT_SLEEP) {
            t->status = THS_READY;
            t->wait_reason = WAIT_NONE;
            ready_insert(t);
        } else {
            ++t->wakeup_count;
        }
    }
    set32(ctx, 2, 0);
    maybe_preempt();
}

void Kernel::sys_cancel_wakeup(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (t)
        t->wakeup_count = 0;
    set32(ctx, 2, 0);
}

void Kernel::sys_suspend_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    ++t->suspend_count;
    if (t->status == THS_READY) {
        std::lock_guard lk(m_sched);
        ready_remove(t);
        t->status = THS_SUSPEND;
    }
    set32(ctx, 2, 0);
    if (t == m_current && t->suspend_count == 1)
        block_current(THS_SUSPEND, WAIT_SLEEP, 0); // resume via ResumeThread
}

void Kernel::sys_resume_thread(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Thread* t = find_thread(id);
    if (!t) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (t->suspend_count > 0)
        --t->suspend_count;
    if (t->suspend_count == 0 && t->status == THS_SUSPEND) {
        std::lock_guard lk(m_sched);
        t->status = THS_READY;
        ready_insert(t);
    }
    set32(ctx, 2, 0);
    maybe_preempt();
}

// --- semaphore syscalls ----------------------------------------------------------

void Kernel::sys_create_sema(EEContext& ctx) {
    const u32 params = gpr32(ctx, 4);
    auto s = std::make_unique<Sema>();
    s->count = s32(ld32(ctx, params + 0));
    s->max_count = s32(ld32(ctx, params + 4));
    s->attr = ld32(ctx, params + 8);
    s->option = ld32(ctx, params + 12);
    s->id = u32(m_semas.size()) + 1;
    const u32 id = s->id;
    m_semas.push_back(std::move(s));
    set32(ctx, 2, id);
}

void Kernel::sys_delete_sema(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Sema* s = find_sema(id);
    if (!s) {
        set32(ctx, 2, u32(-1));
        return;
    }
    // wake any waiters (they'll see the semaphore gone)
    for (Thread* t : s->waiters) {
        t->status = THS_READY;
        t->wait_reason = WAIT_NONE;
        ready_insert(t);
    }
    s->waiters.clear();
    s->id = 0; // deleted marker
    set32(ctx, 2, 0);
}

void Kernel::sys_signal_sema(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Sema* s = find_sema(id);
    if (!s || s->id == 0) {
        set32(ctx, 2, u32(-1));
        return;
    }
    {
        std::lock_guard lk(m_sched);
        if (!s->waiters.empty()) {
            Thread* t = s->waiters.front();
            s->waiters.pop_front();
            t->status = THS_READY;
            t->wait_reason = WAIT_NONE;
            ready_insert(t);
        } else if (s->count < s->max_count) {
            ++s->count;
        }
    }
    set32(ctx, 2, 0);
    maybe_preempt();
}

void Kernel::sys_wait_sema(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Sema* s = find_sema(id);
    if (!s || s->id == 0) {
        set32(ctx, 2, u32(-1));
        return;
    }
    set32(ctx, 2, 0); // return value once acquired
    {
        std::lock_guard lk(m_sched);
        if (s->count > 0) {
            --s->count;
            return;
        }
        s->waiters.push_back(m_current);
    }
    block_current(THS_WAIT, WAIT_SEMA, id);
}

void Kernel::sys_poll_sema(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    Sema* s = find_sema(id);
    if (!s || s->id == 0) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (s->count > 0) {
        --s->count;
        set32(ctx, 2, 1);
    } else {
        set32(ctx, 2, u32(-1));
    }
}

void Kernel::sys_refer_sema_status(EEContext& ctx) {
    const u32 id = gpr32(ctx, 4);
    const u32 info = gpr32(ctx, 5);
    Sema* s = find_sema(id);
    if (!s) {
        set32(ctx, 2, u32(-1));
        return;
    }
    if (info) {
        st32(ctx, info + 0, u32(s->count));
        st32(ctx, info + 4, u32(s->max_count));
        st32(ctx, info + 8, s->attr);
        st32(ctx, info + 12, s->option);
        st32(ctx, info + 16, u32(s->waiters.size()));
    }
    set32(ctx, 2, id);
}

// --- _print (syscall 0x75): kernel debug print -------------------------------------
// a0 = format string (guest pointer); args in a1-a3, then stack at sp+16.

void Kernel::sys_print(EEContext& ctx) {
    const u32 fmt_addr = gpr32(ctx, 4);
    std::string fmt;
    for (u32 a = fmt_addr; a < fmt_addr + 4096; ++a) {
        const u32 ch = ld8u(ctx, a);
        if (ch == 0)
            break;
        fmt += char(ch);
    }
    const u32 sp = gpr32(ctx, 29);
    u64 args[7] = {gpr(ctx, 5),          gpr(ctx, 6),          gpr(ctx, 7),
                   ld32(ctx, sp + 16),   ld32(ctx, sp + 20),   ld32(ctx, sp + 24),
                   ld32(ctx, sp + 28)};
    int argi = 0;
    std::string out;
    char buf[128];
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        if (++i >= fmt.size())
            break;
        // skip flags/width/precision/length
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == '.' || fmt[i] == '-' ||
                                  fmt[i] == '0' || fmt[i] == ' ' || fmt[i] == '+' ||
                                  (fmt[i] >= '1' && fmt[i] <= '9')))
            ++i;
        if (i >= fmt.size())
            break;
        const char conv = fmt[i];
        if (conv == '%') {
            out += '%';
            continue;
        }
        const u64 a = args[argi < 7 ? argi++ : 6];
        switch (conv) {
        case 'd': case 'i': std::snprintf(buf, sizeof buf, "%d", s32(a)); out += buf; break;
        case 'u': std::snprintf(buf, sizeof buf, "%u", u32(a)); out += buf; break;
        case 'x': std::snprintf(buf, sizeof buf, "%x", u32(a)); out += buf; break;
        case 'X': std::snprintf(buf, sizeof buf, "%X", u32(a)); out += buf; break;
        case 'o': std::snprintf(buf, sizeof buf, "%o", u32(a)); out += buf; break;
        case 'c': out += char(a); break;
        case 'f': {
            float f;
            const u32 bits = u32(a);
            std::memcpy(&f, &bits, 4);
            std::snprintf(buf, sizeof buf, "%f", double(f));
            out += buf;
            break;
        }
        case 's': {
            const u32 str_addr = u32(a);
            for (u32 p = str_addr; p < str_addr + 4096; ++p) {
                const u32 ch = ld8u(ctx, p);
                if (ch == 0)
                    break;
                out += char(ch);
            }
            break;
        }
        default:
            out += conv;
            break;
        }
    }
    if (rt.console)
        rt.console(out.data(), out.size());
    else
        std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    set32(ctx, 2, 0);
}


// --- dynamic heap allocator (bump allocator) -----------------------------------
void* Kernel::malloc(u32 size) {
    if (!m_heap_start || size == 0) return nullptr;
    const u32 aligned_size = (size + 7u) & ~7u;
    if (m_heap_free >= m_heap_start && m_heap_free + aligned_size <= m_heap_end) {
        u32 addr = m_heap_free;
        m_heap_free += aligned_size;
        return reinterpret_cast<void*>(static_cast<u64>(addr));
    }
    std::fprintf(stderr, "[kernel] malloc: out of heap (%u bytes at 0x%08X)\\n", size, m_heap_free);
    return nullptr;
}

void Kernel::free(void* ptr) {
    (void)ptr;
}
void Kernel::log_unimplemented(s32 code, const char* name) {
    if (m_reported_syscalls.insert(code).second)
        std::fprintf(stderr, "[kernel] unimplemented syscall %d (%s)\n", code, name);
}

// --- clock thread / deferred interrupt delivery ------------------------------------

// INTC causes (EE): 2=VBLANK start, 3=VBLANK end, 13=SIF.
void Kernel::clock_thread_main() {
    std::fprintf(stderr, "[clock] thread started\n");
    // EE bus clock: 147.456 MHz. Advance in 1 ms host ticks.
    constexpr u64 kBusClockHz = 147456000ull;
    const u64 per_tick = kBusClockHz / 1000;
    u64 next_vblank = 0;
    u64 next_timer_tick = 0;
    u32 vblank_id = 0;
    u32 dump_ticks = 0;
    while (!m_destroying) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Bring-up diagnostics: dump the top polled guest addresses every ~5 s.
        if (++dump_ticks % 500 == 0) {
            std::fprintf(stderr, "[clock] tick=%u pending=%d\n", dump_ticks,
                         m_pending_count.load(std::memory_order_relaxed));
            if (dump_ticks % 2500 == 0)
                ee_dump_loads();
        }
        const u64 now = m_busclock.fetch_add(per_tick, std::memory_order_relaxed) + per_tick;

        {
            std::lock_guard lk(m_sched);
            // Fire due alarms.
            for (auto it = m_alarms.begin(); it != m_alarms.end();) {
                if (now >= it->second.deadline) {
                    m_pending.push_back({it->second.handler, 0xFFFFFFFFu, it->second.arg});
                    m_pending_count.fetch_add(1, std::memory_order_relaxed);
                    it = m_alarms.erase(it);
                } else {
                    ++it;
                }
            }
            // ~59.94 Hz VBlank pulses (start + end causes).
            const u64 vblank_period = kBusClockHz / 60;
            if (now >= next_vblank) {
                next_vblank = now + vblank_period;
                const u32 vid = vblank_id++;
                auto h2 = m_intc_handlers.find(2);
                if (h2 != m_intc_handlers.end() && (m_intc_mask & (1u << 2))) {
                    m_pending.push_back({h2->second.first, 2, h2->second.second});
                    m_pending_count.fetch_add(1, std::memory_order_relaxed);
                }
                auto h3 = m_intc_handlers.find(3);
                if (h3 != m_intc_handlers.end() && (m_intc_mask & (1u << 3))) {
                    m_pending.push_back({h3->second.first, 3, h3->second.second});
                    m_pending_count.fetch_add(1, std::memory_order_relaxed);
                }
                (void)vid;
            }
            // Timer interrupts (INTC causes 10/11/12 = TIMER0/1/2) at ~1 kHz.
            // Games' scheduler loops wait on these; real timers are programmable
            // but a steady tick unblocks the common polling patterns.
            if (now >= next_timer_tick) {
                next_timer_tick = now + kBusClockHz / 1000;
                for (u32 cause = 10; cause <= 12; ++cause) {
                    auto ht = m_intc_handlers.find(cause);
                    if (ht != m_intc_handlers.end() && (m_intc_mask & (1u << cause))) {
                        m_pending.push_back({ht->second.first, cause, ht->second.second});
                        m_pending_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
}

void Kernel::raise_intc(u32 cause, u32 arg) {
    std::lock_guard lk(m_sched);
    if (m_destroying)
        return;
    auto it = m_intc_handlers.find(cause);
    if (it == m_intc_handlers.end())
        return;
    if (!(m_intc_mask & (1u << (cause & 31))))
        return;
    m_pending.push_back({it->second.first, cause, arg});
    m_pending_count.fetch_add(1, std::memory_order_relaxed);
}

void Kernel::fire_pending(EEContext& ctx) {
    // Hardware masks the INTC while a handler runs. Without this, handlers
    // that spin re-enter fire_pending via guest-side polls and nest
    // recursively (each queued tick stacking another handler level).
    if (m_in_handler > 0)
        return;
    for (;;) {
        PendingInt p;
        {
            std::lock_guard lk(m_sched);
            if (m_destroying || m_pending.empty())
                return;
            p = m_pending.front();
            m_pending.pop_front();
            m_pending_count.fetch_sub(1, std::memory_order_relaxed);
        }
        if (!p.handler || !rt.functions.count(p.handler))
            continue; // handler not recompiled (or not registered) — drop
        static std::set<u32> g_delivered;
        if (g_delivered.insert(p.handler).second)
            std::fprintf(stderr, "[intc] delivering handler 0x%08X (cause %u)\n",
                         p.handler, p.cause);
        ++m_in_handler;
        invoke_handler(ctx, p);
        --m_in_handler;
    }
}

void Kernel::invoke_handler(EEContext& ctx, const PendingInt& p) {
    // Save caller-saved scratch + return address, invoke, restore.
    const u128 a0 = ctx.r[4], a1 = ctx.r[5], a2 = ctx.r[6], a3 = ctx.r[7], ra = ctx.r[31];
    const u128 t0 = ctx.r[8], t1 = ctx.r[9], t2 = ctx.r[10], t3 = ctx.r[11];
    const u128 t4 = ctx.r[12], t5 = ctx.r[13], t6 = ctx.r[14], t7 = ctx.r[15];
    if (p.cause == 0xFFFFFFFFu) {
        // Alarm callback: handler(alarm_id, time, arg) — id/time unused here.
        set32(ctx, 4, 0);
        set32(ctx, 5, 0);
        set32(ctx, 6, p.arg);
    } else {
        // INTC: handler(cause, arg)
        set32(ctx, 4, p.cause);
        set32(ctx, 5, p.arg);
    }
    rt.call(ctx, p.handler);
    ctx.r[4] = a0; ctx.r[5] = a1; ctx.r[6] = a2; ctx.r[7] = a3;
    ctx.r[8] = t0; ctx.r[9] = t1; ctx.r[10] = t2; ctx.r[11] = t3;
    ctx.r[12] = t4; ctx.r[13] = t5; ctx.r[14] = t6; ctx.r[15] = t7;
    ctx.r[31] = ra;
}


// --- dispatch ----------------------------------------------------------------------
// Numbers follow ps2sdk's syscallnr.h. Negative codes are i-variants (callable from
// interrupt context); we treat them identically for now.

void Kernel::syscall(EEContext& ctx, s32 code) {
    ensure_main_thread(ctx);

    // Deliver queued interrupts/alarms at this boundary (main thread's ctx).
    fire_pending(ctx);

    // user-registered handlers (SetSyscall) take precedence
    if (auto it = m_user_syscalls.find(code); it != m_user_syscalls.end()) {
        rt.call(ctx, it->second);
        fire_pending(ctx);
        return;
    }

    const u32 a0 = gpr32(ctx, 4);
    // Old libkernel convention: the syscall instruction's code field is 0 and
    // the actual syscall number is passed in $v1. Re-dispatch through the
    // standard table using $v1 as the syscall number.
    if (code == 0) {
        const u32 v1 = gpr32(ctx, 3);
        if (v1 != 0) {
            syscall(ctx, s32(v1));
            return;
        }
        // code==0 and v1==0: treat as an old-style SIF no-op.
        set32(ctx, 2, 0);
        return;
    }
    switch (code) {
    case 0x01: set32(ctx, 2, 0); break; // ResetEE
    case 0x02: set32(ctx, 2, 0); break; // SetGsCrt (recorded; GS comes in M6)
    case 0x04: // KExit
        m_quit = true;
        m_exit_code = s32(a0);
        set32(ctx, 2, 0);
        break;
    case 0x0D: case 0x0E: case 0x0F: // SetV*Handler
        set32(ctx, 2, 0);
        break;
    case 0x10: { // AddIntcHandler(cause, handler, arg)
        if (m_trace)
            std::fprintf(stderr, "[kernel] AddIntcHandler cause=%u handler=0x%08X arg=0x%08X\n",
                         a0, gpr32(ctx, 5), gpr32(ctx, 6));
        m_intc_handlers[a0] = {gpr32(ctx, 5), gpr32(ctx, 6)};
        set32(ctx, 2, a0);
        break;
    }
    case 0x11: // RemoveIntcHandler
        m_intc_handlers.erase(a0);
        set32(ctx, 2, 0);
        break;
    case 0x12: // AddDmacHandler(channel, handler, arg)
        m_dmac_handlers[a0] = {gpr32(ctx, 5), gpr32(ctx, 6)};
        set32(ctx, 2, a0);
        break;
    case 0x13:
        m_dmac_handlers.erase(a0);
        set32(ctx, 2, 0);
        break;
    case 0x14: case -0x1A: { // _EnableIntc / _iEnableIntc
        const u32 old = m_intc_mask;
        m_intc_mask |= (1u << a0);
        if (m_trace)
            std::fprintf(stderr, "[kernel] EnableIntc cause=%u (mask 0x%08X)\n", a0, m_intc_mask);
        set32(ctx, 2, old);
        break;
    }
    case 0x15: case -0x1B: { // _DisableIntc
        const u32 old = m_intc_mask;
        m_intc_mask &= ~(1u << a0);
        set32(ctx, 2, old);
        break;
    }
    case 0x16: case -0x1C: { // _EnableDmac
        const u32 old = m_dmac_mask;
        m_dmac_mask |= (1u << a0);
        set32(ctx, 2, old);
        break;
    }
    case 0x17: case -0x1D: {
        const u32 old = m_dmac_mask;
        m_dmac_mask &= ~(1u << a0);
        set32(ctx, 2, old);
        break;
    }
    case 0x18: case 0xFC: case -0x1E: case -0xFF: { // SetAlarm(time, handler, arg)
        if (m_trace)
            std::fprintf(stderr, "[kernel] SetAlarm time=%u handler=0x%08X arg=0x%08X\n",
                         a0, gpr32(ctx, 5), gpr32(ctx, 6));
        const u32 id = m_next_alarm++;
        // a0 = relative time in busclock ticks; deadline = now + time.
        m_alarms[id] = {gpr32(ctx, 5), gpr32(ctx, 6), busclock() + a0};
        set32(ctx, 2, id);
        break;
    }
    case 0x19: case 0xFE: case -0x1F: case -0x100: // ReleaseAlarm family
        m_alarms.erase(a0);
        set32(ctx, 2, 0);
        break;
    case 0x20: sys_create_thread(ctx); break;
    case 0x21: sys_delete_thread(ctx); break;
    case 0x22: sys_start_thread(ctx); break;
    case 0x23: case 0x24: sys_exit_thread(ctx); break; // ExitThread / ExitDeleteThread
    case 0x25: case -0x26: sys_terminate_thread(ctx); break;
    case 0x27: ++m_dispatch_disabled; set32(ctx, 2, 0); break;
    case 0x28:
        if (m_dispatch_disabled > 0)
            --m_dispatch_disabled;
        set32(ctx, 2, 0);
        maybe_preempt();
        break;
    case 0x29: case -0x2A: sys_change_thread_priority(ctx); break;
    case 0x2B: case -0x2C: sys_rotate_ready(ctx); break;
    case 0x2D: case -0x2E: sys_release_wait_thread(ctx); break;
    case 0x2F: set32(ctx, 2, m_current ? m_current->id : 0); break; // GetThreadId
    case 0x30: case -0x31: sys_refer_thread_status(ctx); break;
    case 0x32: sys_sleep_thread(ctx); break;
    case 0x33: case -0x34: sys_wakeup_thread(ctx); break;
    case 0x35: case -0x36: sys_cancel_wakeup(ctx); break;
    case 0x37: case -0x38: sys_suspend_thread(ctx); break;
    case 0x39: case -0x3A: sys_resume_thread(ctx); break;
    case 0x3B: { // JoinThread
        Thread* t = find_thread(a0);
        if (!t) {
            set32(ctx, 2, u32(-1));
            break;
        }
        set32(ctx, 2, 0);
        if (t->status != THS_DORMANT)
            block_current(THS_WAIT, WAIT_JOIN, a0);
        break;
    }
    case 0x3C: // SetupThread: reconfigure the current thread's gp/sp
        set64(ctx, 28, gpr(ctx, 4));
        set64(ctx, 29, (gpr(ctx, 5) + gpr(ctx, 6)) & ~0xFull);
        set32(ctx, 2, 0);
        break;
    case 0x3D: // SetupHeap
        m_heap_start = a0; m_heap_free = a0;
        set32(ctx, 2, 0);
        break;
    case 0x3E: set32(ctx, 2, m_heap_end); break; // EndOfHeap
    case 0x40: sys_create_sema(ctx); break;
    case 0x41: case -0x49: sys_delete_sema(ctx); break;
    case 0x42: case -0x43: sys_signal_sema(ctx); break;
    case 0x44: sys_wait_sema(ctx); break;
    case 0x45: case -0x46: sys_poll_sema(ctx); break;
    case 0x47: case -0x48: sys_refer_sema_status(ctx); break;
    case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: // osd config / GS params
        set32(ctx, 2, 0);
        break;
    case 0x50: { // RFU080_CreateEventFlag (minimal: return an id)
        static u32 next_ef = 1;
        set32(ctx, 2, next_ef++);
        break;
    }
    case 0x51: case 0x52: case 0x53: case -0x54: case 0x55: case -0x56: // event flags / TLB
        set32(ctx, 2, 0);
        break;
    case 0x56: case -0x57: case 0x57: case -0x58: case 0x58: case 0x59: // TLB / scratchpad
        set32(ctx, 2, 0);
        break;
    case 0x5B: set32(ctx, 2, 0); break; // GetEntryAddress (TODO: track entry)
    case 0x5C: case -0x5C: case 0x5D: case -0x5D: // intc handler enable/disable
    case 0x5E: case -0x5E: case 0x5F: case -0x5F: // dmac handler enable/disable
        set32(ctx, 2, 0);
        break;
    case 0x60: case 0x61: case 0x62: set32(ctx, 2, 0); break; // KSeg0 / cache on/off
    case 0x63: set32(ctx, 2, ctx.cop0[a0 & 31]); break;      // GetCop0
    case -0x67: set32(ctx, 2, ctx.cop0[a0 & 31]); break;     // iGetCop0
    case 0x64: case -0x68: set32(ctx, 2, 0); break;          // FlushCache / iFlushCache
    case 0x66: case -0x6A: set32(ctx, 2, 0); break;          // CpuConfig
    case 0x6B: set32(ctx, 2, 0); break;                      // sceSifStopDma
    case 0x5A: {                                             // Copy(dst, src, size)
        // The kernel's block-copy — old-libkernel games use it to install their
        // own kernel-mode glue into the kernel-reserved RAM area (e.g. Urbz
        // memcpy's its SIF/CDVD glue blobs into 0x8007xxxx).
        const u32 dst = a0, src = gpr32(ctx, 5), size = gpr32(ctx, 6);
        if (size > 0 && size <= 0x100000)
            std::memcpy(rt.mem.translate(dst), rt.mem.translate(src), size);
        if (m_trace) {
            static std::set<u32> g_copy_reported;
            if (g_copy_reported.insert(dst).second)
                std::fprintf(stderr, "[kernel] Copy dst=0x%08X src=0x%08X size=0x%X\n", dst, src, size);
        }
        set32(ctx, 2, dst);
        break;
    }
    case 0x83: {                                             // FindAddress / block install
        // Observed use (old-libkernel InitSystemCallTableAddress): installs a
        // block of syscall glue functions (a2, game RAM) into the kernel area
        // at a0 and returns the end of the installed copy. The caller installs
        // two blocks (524 and 360 bytes here) and compares the returned
        // "start" pointers (end - block size) until they coincide, so the
        // return must be a0 + block size. Block size is cached per source
        // (seeded from the observed Urbz install); unknown sources get a log
        // line so the next size is visible.
        const u32 dst = a0, src = gpr32(ctx, 6);
        static std::unordered_map<u32, u32> g_block_sizes = {
            {0x00440568u, 524}, // Urbz syscall-glue block 1 (InitSystemCallTableAddress)
            {0x00440530u, 360}, // Urbz syscall-glue block 2
        };
        static std::set<u32> g_install_reported;
        static std::set<u64> g_aliases_done;
        auto it = g_block_sizes.find(src);
        if (it == g_block_sizes.end()) {
            it = g_block_sizes.emplace(src, 4).first;
            if (g_install_reported.insert(src).second)
                std::fprintf(stderr, "[kernel] FindAddress install dst=0x%08X src=0x%08X (size unknown, using 4)\n", dst, src);
        }
        const u32 size = it->second;
        {
            u8* d = rt.mem.translate(dst);
            const u8* s = rt.mem.translate(src);
            std::memcpy(d, s, size);
        }
        // Register the installed range once so calls into the kernel-area copy
        // execute the compiled functions at the source addresses.
        const u64 key = (u64(dst) << 32) | src;
        if (g_aliases_done.insert(key).second)
            rt.alias_range(dst, src, size);
        set32(ctx, 2, dst + size);
        break;
    }
    case 0x6C: case 0x6D: set32(ctx, 2, 0); break;           // CPUTimer
    case 0x6E: case 0x6F: set32(ctx, 2, 0); break;           // osd config 2
    case 0x70: case -0x70: set32(ctx, 2, m_gs_imr); break;   // GsGetIMR
    case 0x71: case -0x71: {                                 // GsPutIMR
        const u32 old = m_gs_imr;
        m_gs_imr = a0;
        set32(ctx, 2, old);
        break;
    }
    case 0x72: case 0x73: set32(ctx, 2, 0); break; // SetPgifHandler / SetVSyncFlag
    case 0x74: { // SetSyscall(num, handler)
        // The retail kernel keeps its syscall handler table in kernel-reserved
        // RAM at 0x80000000 (entry N at 0x80000000 + N*4). Old-libkernel games
        // (SLUS-21066) register their own glue via SetSyscall and then verify
        // the table with a FindAddress(0x80000000, 0x80080000, handler) scan,
        // so the handler must actually appear in RAM at num*4 — mirror it.
        m_user_syscalls[a0] = gpr32(ctx, 5);
        const u32 slot = 0x80000000u + a0 * 4;
        if (a0 < 0x2000000u / 4)
            std::memcpy(rt.mem.translate(slot), &m_user_syscalls[a0], 4);
        if (m_trace) {
            static std::set<u32> g_setsys_reported;
            if (g_setsys_reported.insert(a0).second)
                std::fprintf(stderr, "[kernel] SetSyscall num=%u handler=0x%08X (table 0x%08X)\n",
                             a0, gpr32(ctx, 5), slot);
        }
        set32(ctx, 2, 0);
        break;
    }
    case 0x75: sys_print(ctx); break;                        // _print
    case 0x76: case -0x76: set32(ctx, 2, 0); break;          // SifDmaStat (complete)
    case 0x77: case -0x77: {                                 // SifSetDma
        // Old libkernel: the EE's whole SIF protocol rides on this syscall.
        // a0 = SifDmaTransfer_t list {src, dest, size, attr}, a1 = count.
        // The IOP HLE walks the descriptors, stages payloads into IOP RAM and
        // dispatches command packets (CHANGE_SADDR/INIT/BIND/CALL/RDATA...),
        // answering with REND packets into the game's packet buffer + the SIF
        // interrupt (see Iop::sif_set_dma / sif_process_cmd).
        const u32 id = rt.hw->iop.sif_set_dma(*rt.hw, a0, (s32)gpr32(ctx, 5));
        set32(ctx, 2, id);
        break;
    }
    case 0x78: case -0x78: set32(ctx, 2, 0); break;          // SifSetDChain
    case 0x79: {                                             // SifSetReg(reg, val)
        rt.hw->iop.sif_set_sysreg(*rt.hw, a0, gpr32(ctx, 5));
        set32(ctx, 2, 0);
        break;
    }
    case 0x7A:                                               // SifGetReg(reg)
        set32(ctx, 2, rt.hw->iop.sif_get_sysreg(a0));
        break;
    case 0x7D: set32(ctx, 2, 0); break;                      // PSMode (0 = PS2)
    case 0x7E: set32(ctx, 2, 0x59); break;                   // MachineType
    case 0x7B: case 0x7C:                                    // ExecPS2 / RFU060
        set32(ctx, 2, 0);
        break;
    case 0x7F: set32(ctx, 2, 32u * 1024 * 1024); break;      // GetMemorySize
    case 0x80: set32(ctx, 2, 0); break;                      // _GetGsDxDyOffset
    case 0x82: set32(ctx, 2, 0); break;                      // _InitTLB
    // 0x83 (FindAddress / block install) is handled above.
    case 0x85: set32(ctx, 2, 0); break;                      // SetMemoryMode
    case 0x86: set32(ctx, 2, 0); break;                      // GetMemoryMode
    default:
        log_unimplemented(code, "?");
        set32(ctx, 2, 0);
        break;
    }

    // Deliver interrupts/alarms queued during handling (e.g. SifSetDma).
    fire_pending(ctx);
}

} // namespace ee::rt
