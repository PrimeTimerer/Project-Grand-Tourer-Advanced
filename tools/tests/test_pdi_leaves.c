/* Differential fixture for tools/optimize_pdi_leaves.py: every leaf in its lifted form and in its fast form, from the
 * same state, compared in full - all 128 registers, the local store, the rest of the context, pc, the pending
 * transfer, MXCSR, the x87 control word, and the stream of callbacks (LS-write notes, the LS watch with r0-r4, the
 * functions a leaf transfers to). No game, device or user data.
 *   prepare: python tools\optimize_pdi_leaves.py --fixture <dir>      (writes <dir>\pdi_leaves_cases.inc)
 *   build:   tools\tests\build_pdi_leaves.cmd <dir>                   (the flags of the lifted SPU sources)
 *   run:     <dir>\test_pdi_leaves.exe [--bench]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "spu_helpers.h"

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
int g_spu_ls_watch_n;
volatile int g_spu_direct = 1;
uint8_t g_spu_stack_reset_hint[1024];
spu_context* trace_ctx;
uint64_t trace_hash;
unsigned trace_calls;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
void spu_direct_init(void) {}
static void sentinel(spu_context* ctx) { (void)ctx; abort(); }
static void stub_call(spu_context* ctx, uint32_t pc)
{
    trace_calls++;
    trace_hash = (trace_hash ^ pc) * UINT64_C(1099511628211);
    trace_hash = (trace_hash ^ ctx->pc) * UINT64_C(1099511628211);
}
static unsigned test_budget;      /* loop passes a leaf may still make (the cases leave by SPU_RET at zero) */

#include "pdi_leaves_cases.inc"

static unsigned long long checks, pairs;
static const char* now_case = "";
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s line %d: %s\n", now_case, __LINE__, #x); exit(1); } } while (0)
static spu_context initial_ctx, old_ctx, new_ctx;
static uint32_t rng = 0x9039B3CCu;
static uint32_t random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const uint32_t edges[] = {
    0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
    0x3f800000u, 0xbf800000u, 0x3f7fffffu, 0xbf7fffffu, 0x7f7fffffu, 0xff7fffffu,
    0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7fffffffu, 0xffffffffu,
    0x40000000u, 0xc0000000u, 0x3eaaaaabu, 0xbeaaaaabu, 0x7f000000u, 0xff000000u,
    0x4b800000u, 0xcb800000u, 0x4f000000u, 0x4f800000u, 0x00000100u, 0x0000ffffu
};
static uint32_t edge(unsigned n) { return edges[n % (sizeof edges / sizeof edges[0])]; }
static void copy_ctx(spu_context* to, const spu_context* from) { memcpy(to, from, sizeof *to); to->ls = to->ls_store; }

/* kind 0: every word an edge value, 1: every word random, 2: ordinary floats and in-range pointers */
static void seed(unsigned id, unsigned kind, unsigned count, int counter, const char* name)
{
    memset(&initial_ctx, 0, sizeof initial_ctx);
    initial_ctx.ls = initial_ctx.ls_store;
    initial_ctx.status = SPU_STATUS_RUNNING;
    initial_ctx.pc = 0x4000u + (id & 0xFFCu);
    initial_ctx.image_id = 249;
    initial_ctx.spu_id = 0x2000u + (id % 5);
    initial_ctx.host_depth = (id % 3) ? 1u + (id % 17) : 0;
    initial_ctx.steps = id % 4;
    initial_ctx.event_status = random_word();
    initial_ctx.event_mask = random_word();
    initial_ctx.mfc_lsa = random_word();
    initial_ctx.mfc_size = random_word();
    initial_ctx.resv_ea = random_word();
    initial_ctx.resv_valid = 1;
    initial_ctx.dcache_id = 0x5a5a5a5a;
    initial_ctx.dcache_residency = 3;
    initial_ctx.resident_code_count = 2;
    for (unsigned i = 0; i < 2; i++) {
        initial_ctx.resident_code[i].lsa = i * 0x20000;
        initial_ctx.resident_code[i].size = 0x20000;
        initial_ctx.resident_code[i].image_id = 249;
        initial_ctx.resident_code[i].verify_state = 1;
        initial_ctx.resident_code[i].source_ea = 0x143f280;
    }
    for (unsigned r = 0; r < 128; r++)
        for (unsigned k = 0; k < 4; k++)
            initial_ctx.gpr[r]._u32[k] = kind == 1 ? random_word() : kind == 0 ? edge(r * 4 + k + id)
                                       : (k == 0 && (r & 1)) ? 0x10000u + ((random_word() & 0xFFFu) << 4)      /* a pointer */
                                       : 0x3d000000u + (random_word() & 0x03ffffffu);                           /* 0.03 .. 8 */
    for (unsigned p = 0; p < SPU_LS_SIZE; p += 4) {
        const uint32_t v = kind == 1 ? random_word() : kind == 0 ? edge(p / 4 + id) : 0x3d000000u + (random_word() & 0x03ffffffu);
        initial_ctx.ls[p] = (uint8_t)(v >> 24); initial_ctx.ls[p + 1] = (uint8_t)(v >> 16);
        initial_ctx.ls[p + 2] = (uint8_t)(v >> 8); initial_ctx.ls[p + 3] = (uint8_t)v;
    }
    static const uint32_t pointers[] = { 0, 1, 0x3fff0u, 0x3fffcu, 0xfffffff8u, 0x1fff0u, 0x20004u, 0x4000fu };
    initial_ctx.gpr[0]._u32[0] = 0x3210;
    if (kind != 2) {
        initial_ctx.gpr[3]._u32[0] = pointers[id % 8];
        initial_ctx.gpr[9]._u32[0] = pointers[(id + 3) % 8];
        initial_ctx.gpr[7]._u32[0] = pointers[(id + 5) % 8];
    }
    if (counter >= 0) initial_ctx.gpr[counter]._u32[0] = count;
    if (!strcmp(name, "00004124")) {                 /* runs while r4 > r3, r3 += 16 */
        if (kind == 2) initial_ctx.gpr[3]._u32[0] = 0x20000u;
        initial_ctx.gpr[4]._u32[0] = initial_ctx.gpr[3]._u32[0] + 16u * count;
    }
    if (!strcmp(name, "00008EF8") && kind == 2) initial_ctx.gpr[3]._u32[0] = 0x20000u;
}
static void configure(spu_context* ctx, unsigned rz, unsigned flags)
{
    _mm_setcsr(rz ? SPU_MXCSR_RZFZ : 0x1f80u);
    spu__rz_mark((int)rz);
    g_spu_ls_probe = g_spu_smc_watch = g_spu_wws_probe = 0;
    g_spu_ls_watch_n = g_spu_ls_rdiag = g_spu_ls_wdiag = (int)(flags & 1);
    if (flags & 1) memset(ctx->resident_code_pages, 255, sizeof ctx->resident_code_pages);
    trace_ctx = ctx; trace_hash = UINT64_C(1469598103934665603); trace_calls = 0;
    g_spu_trampoline_fn = (flags & 2) ? sentinel : NULL;
    g_spu_direct = (flags & 4) ? 0 : 1;
}
static unsigned short control_word(void) { unsigned short cw; __asm__("fnstcw %0" : "=m"(cw)); return cw; }
static void compare_one(unsigned c, unsigned rz, unsigned flags, unsigned budget)
{
    copy_ctx(&old_ctx, &initial_ctx); configure(&old_ctx, rz, flags);
    test_budget = budget;
    leaf_cases[c].original(&old_ctx);
    const unsigned old_fp = _mm_getcsr(), old_cw = control_word(), old_calls = trace_calls;
    const uint64_t old_hash = trace_hash;
    void (*old_trampoline)(spu_context*) = g_spu_trampoline_fn;
    copy_ctx(&new_ctx, &initial_ctx); configure(&new_ctx, rz, flags);
    test_budget = budget;
    leaf_cases[c].native(&new_ctx);
    CHECK(old_fp == _mm_getcsr()); CHECK(old_cw == control_word());
    CHECK(old_calls == trace_calls); CHECK(old_hash == trace_hash);
    CHECK(old_trampoline == g_spu_trampoline_fn);
    CHECK(!memcmp(old_ctx.gpr, new_ctx.gpr, sizeof old_ctx.gpr));
    CHECK(!memcmp(old_ctx.ls, new_ctx.ls, SPU_LS_SIZE));
    CHECK(old_ctx.pc == new_ctx.pc);
    old_ctx.ls = new_ctx.ls = NULL;
    CHECK(!memcmp(&old_ctx, &new_ctx, sizeof old_ctx));
    old_ctx.ls = old_ctx.ls_store; new_ctx.ls = new_ctx.ls_store;
    pairs++;
}
static double bench(void (*fn)(spu_context*), unsigned reps)
{
    LARGE_INTEGER start, end, freq; QueryPerformanceFrequency(&freq);
    long long ticks = 0;
    for (unsigned r = 0; r < reps; r++) {
        copy_ctx(&new_ctx, &initial_ctx); configure(&new_ctx, 1, 0);
        test_budget = 0x40000000u;
        QueryPerformanceCounter(&start); fn(&new_ctx); QueryPerformanceCounter(&end);
        ticks += end.QuadPart - start.QuadPart;
    }
    return (double)ticks * 1e9 / (double)freq.QuadPart / reps;
}
int main(int argc, char** argv)
{
    const unsigned saved = _mm_getcsr(); const unsigned short saved_cw = control_word();
    const unsigned ncases = (unsigned)(sizeof leaf_cases / sizeof leaf_cases[0]);
    _putenv_s("GT6_NATIVE_PDI_LEAVES", "1");
    CHECK(gt6_pdi_leaves2_enabled() == 1);
    static const unsigned lengths[] = { 1, 2, 3, 7, 16, 31, 64, 129 };
    for (unsigned c = 0; c < ncases; c++) {
        now_case = leaf_cases[c].name;
        const unsigned long long before = pairs;
        /* to the natural end: the counter is the pass count */
        for (unsigned id = 0; id < 96; id++) {
            seed(id, id % 3, lengths[id % 8], leaf_cases[c].counter, leaf_cases[c].name);
            for (unsigned rz = 0; rz < 2; rz++) for (unsigned flags = 0; flags < 8; flags++) compare_one(c, rz, flags, 0x40000000u);
        }
        /* a given number of passes from any state (counters and pointers are whatever the seed left) */
        for (unsigned id = 0; id < 48; id++) {
            seed(1000 + id, id % 3, (id & 1) ? 0xffffffffu : 0, leaf_cases[c].counter, leaf_cases[c].name);
            for (unsigned rz = 0; rz < 2; rz++) for (unsigned flags = 0; flags < 8; flags++) compare_one(c, rz, flags, 1 + id * 5);
        }
        /* the switch off: the lifted body, whatever the float mode */
        g_gt6_pdi_leaves2 = 0;
        for (unsigned id = 0; id < 6; id++) {
            seed(2000 + id, id % 3, 5, leaf_cases[c].counter, leaf_cases[c].name);
            compare_one(c, 1, id & 7, 0x40000000u);
        }
        g_gt6_pdi_leaves2 = 1;
        printf("%s: %llu pairs equal\n", leaf_cases[c].name, pairs - before);
    }
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        for (unsigned c = 0; c < ncases; c++) {
            seed(17, 2, 256, leaf_cases[c].counter, leaf_cases[c].name);
            double best_a = 1e30, best_b = 1e30;
            for (unsigned round = 0; round < 8; round++) {
                double a, b;
                if (round & 1) { b = bench(leaf_cases[c].native, 300); a = bench(leaf_cases[c].original, 300); }
                else { a = bench(leaf_cases[c].original, 300); b = bench(leaf_cases[c].native, 300); }
                if (a < best_a) best_a = a;
                if (b < best_b) best_b = b;
            }
            printf("BENCH %s: lifted %.0f ns, fast %.0f ns, ratio %.3f\n", leaf_cases[c].name, best_a, best_b, best_a / best_b);
        }
    }
    _mm_setcsr(saved); __asm__ volatile("fldcw %0" :: "m"(saved_cw));
    printf("PASS checks=%llu pairs=%llu (registers, local store, context, pc, pending transfer, MXCSR, x87, callbacks: equal)\n", checks, pairs);
    return 0;
}
