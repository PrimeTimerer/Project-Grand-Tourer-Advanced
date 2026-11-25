/* Who writes a guest word? (test tool, [GT6 0145]). Included only by gt6_host.cpp, after peek32 / peek_ok.
 *
 * GT6_WATCH_WRITE=<hex ea>:<seconds>            the word at that guest address
 * GT6_WATCH_WRITE=@<hex ptr>+<hex off>:<seconds> the word at (the pointer read at <ptr> when the watch starts) + <off>
 *
 * <seconds> after the start the page of that word is made read-only; a write to the page then faults, the page is
 * opened again, the one instruction runs under the CPU's single-step flag and the page is closed behind it. Writes that
 * land on the watched word are counted by the address of the writing instruction (an RVA of the exe: tools\rva_names.py
 * names the lifted function, whose name is the guest function's address). After 4 seconds or 4,096 writes the page
 * stays open and the list is printed. gt6_watch_write_tick() is called once a frame by the render thread's wait hook.
 * Nothing of this exists unless the variable is set. */
#pragma once

namespace {
struct Gt6WatchWrite {
    bool wanted = false, armed = false, done = false, indirect = false;
    uint32_t ptr = 0, off = 0, ea = 0, page = 0;
    double at = 0;
    LARGE_INTEGER freq{}, t0{}, started{};
    void* handler = nullptr;
    volatile LONG hits = 0, page_faults = 0;
    struct { uint64_t rva; LONG n; uint32_t last; } who[32] = {};
    CRITICAL_SECTION lock;
};
Gt6WatchWrite g_watch_write;
thread_local bool t_watch_write_step = false;

LONG CALLBACK gt6_watch_write_veh(EXCEPTION_POINTERS* x)
{
    Gt6WatchWrite& W = g_watch_write;
    const DWORD code = x->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_watch_write_step) {
        t_watch_write_step = false;
        if (W.armed) { DWORD old; VirtualProtect(vm_base + W.page, 0x1000, PAGE_READONLY, &old); }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || x->ExceptionRecord->NumberParameters < 2 ||
        x->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t where = (uintptr_t)x->ExceptionRecord->ExceptionInformation[1];
    const uintptr_t lo = (uintptr_t)(vm_base + W.page);
    if (where < lo || where >= lo + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(vm_base + W.page, 0x1000, PAGE_READWRITE, &old);
    InterlockedIncrement(&W.page_faults);
    const uint32_t at = (uint32_t)(where - (uintptr_t)vm_base);
    if (at + 8 > W.ea && at < W.ea + 4) {          /* a store of up to 8 bytes that covers the word */
        const uint64_t rva = (uint64_t)((uintptr_t)x->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(nullptr));
        EnterCriticalSection(&W.lock);
        for (auto& w : W.who) {
            if (w.n && w.rva != rva) continue;
            w.rva = rva; w.n++; w.last = at;
            break;
        }
        LeaveCriticalSection(&W.lock);
        InterlockedIncrement(&W.hits);
    }
    if (W.armed) { x->ContextRecord->EFlags |= 0x100; t_watch_write_step = true; }   /* close the page after this instruction */
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

static void gt6_watch_write_tick()
{
    Gt6WatchWrite& W = g_watch_write;
    static bool read = false;
    if (!read) {
        read = true;
        const char* e = getenv("GT6_WATCH_WRITE");
        if (e && *e) {
            unsigned a = 0, b = 0; double s = 0;
            if (*e == '@' && sscanf(e + 1, "%x+%x:%lf", &a, &b, &s) == 3) { W.indirect = true; W.ptr = a; W.off = b; W.at = s; W.wanted = true; }
            else if (sscanf(e, "%x:%lf", &a, &s) == 2) { W.ea = a; W.at = s; W.wanted = true; }
            if (W.wanted) { QueryPerformanceFrequency(&W.freq); QueryPerformanceCounter(&W.t0); InitializeCriticalSection(&W.lock); }
            else fprintf(stderr, "[gt6-watch] GT6_WATCH_WRITE=%s not understood (<hex ea>:<seconds> or @<hex ptr>+<hex off>:<seconds>)\n", e);
        }
    }
    if (!W.wanted || W.done) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!W.armed) {
        if ((double)(now.QuadPart - W.t0.QuadPart) / W.freq.QuadPart < W.at) return;
        if (W.indirect) {
            if (!peek_ok(W.ptr, 4)) return;
            const uint32_t p = peek32(W.ptr);
            if (p < 0x10000u) return;
            W.ea = p + W.off;
        }
        if (!peek_ok(W.ea, 4)) { fprintf(stderr, "[gt6-watch] 0x%08X is not readable - no watch\n", W.ea); W.done = true; return; }
        W.page = W.ea & ~0xFFFu;
        W.handler = AddVectoredExceptionHandler(1, gt6_watch_write_veh);
        W.started = now;
        W.armed = true;
        DWORD old;
        VirtualProtect(vm_base + W.page, 0x1000, PAGE_READONLY, &old);
        fprintf(stderr, "[gt6-watch] watching writes to 0x%08X (now 0x%08X) for 4 s\n", W.ea, peek32(W.ea));
        return;
    }
    if ((double)(now.QuadPart - W.started.QuadPart) / W.freq.QuadPart < 4.0 && W.hits < 4096) return;
    W.armed = false; W.done = true;
    DWORD old;
    VirtualProtect(vm_base + W.page, 0x1000, PAGE_READWRITE, &old);
    fprintf(stderr, "[gt6-watch] done: %ld write(s) to 0x%08X among %ld to its page; by writing instruction (exe rva):\n",
            (long)W.hits, W.ea, (long)W.page_faults);
    for (const auto& w : W.who)
        if (w.n) fprintf(stderr, "[gt6-watch]   rva %llx  x%ld  (last at 0x%08X)\n", (unsigned long long)w.rva, (long)w.n, w.last);
}
