// SPDX-License-Identifier: GPL-3.0-only
#include <ee/kernel.hpp>

#include <algorithm>
#include <cstdio>

namespace ee::rt {

Kernel::Kernel(Runtime& rt) : rt(rt) {}

Kernel::~Kernel() {
    m_destroying = true;
    m_quit = true;
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
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    set32(ctx, 2, 0);
}

void Kernel::log_unimplemented(s32 code, const char* name) {
    if (m_reported_syscalls.insert(code).second)
        std::fprintf(stderr, "[kernel] unimplemented syscall %d (%s)\n", code, name);
}

// --- dispatch ----------------------------------------------------------------------
// Numbers follow ps2sdk's syscallnr.h. Negative codes are i-variants (callable from
// interrupt context); we treat them identically for now.

void Kernel::syscall(EEContext& ctx, s32 code) {
    ensure_main_thread(ctx);

    // user-registered handlers (SetSyscall) take precedence
    if (auto it = m_user_syscalls.find(code); it != m_user_syscalls.end()) {
        rt.call(ctx, it->second);
        return;
    }

    const u32 a0 = gpr32(ctx, 4);
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
    case 0x10: // AddIntcHandler(cause, handler, arg)
        m_intc_handlers[a0] = gpr32(ctx, 5);
        set32(ctx, 2, a0);
        break;
    case 0x11: // RemoveIntcHandler
        m_intc_handlers.erase(a0);
        set32(ctx, 2, 0);
        break;
    case 0x12: // AddDmacHandler
        m_dmac_handlers[a0] = gpr32(ctx, 5);
        set32(ctx, 2, a0);
        break;
    case 0x13:
        m_dmac_handlers.erase(a0);
        set32(ctx, 2, 0);
        break;
    case 0x14: case -0x1A: { // _EnableIntc / _iEnableIntc
        const u32 old = m_intc_mask;
        m_intc_mask |= (1u << a0);
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
    case 0x18: case 0xFC: case -0x1E: case -0xFF: { // SetAlarm family
        const u32 id = m_next_alarm++;
        m_alarms[id] = {gpr32(ctx, 5), gpr(ctx, 6)}; // handler, time (not fired yet — M5)
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
        m_heap_end = a0;
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
    case 0x74: // SetSyscall(num, handler)
        m_user_syscalls[a0] = gpr32(ctx, 5);
        set32(ctx, 2, 0);
        break;
    case 0x75: sys_print(ctx); break;                        // _print
    case 0x76: case -0x76: set32(ctx, 2, 0); break;          // SifDmaStat (complete)
    case 0x77: case -0x77: set32(ctx, 2, 1); break;          // SifSetDma (fake id)
    case 0x78: case -0x78: set32(ctx, 2, 0); break;          // SifSetDChain
    case 0x79: set32(ctx, 2, 0); break;                      // SifSetReg
    case 0x7A: set32(ctx, 2, 0); break;                      // SifGetReg
    case 0x7D: set32(ctx, 2, 0); break;                      // PSMode (0 = PS2)
    case 0x7E: set32(ctx, 2, 0x59); break;                   // MachineType
    case 0x7F: set32(ctx, 2, 32u * 1024 * 1024); break;      // GetMemorySize
    case 0x80: set32(ctx, 2, 0); break;                      // _GetGsDxDyOffset
    case 0x82: set32(ctx, 2, 0); break;                      // _InitTLB
    case 0x83: set32(ctx, 2, 0); break;                      // FindAddress
    case 0x85: set32(ctx, 2, 0); break;                      // SetMemoryMode
    case 0x86: set32(ctx, 2, 0); break;                      // GetMemoryMode
    default:
        log_unimplemented(code, "?");
        set32(ctx, 2, 0);
        break;
    }
}

} // namespace ee::rt
