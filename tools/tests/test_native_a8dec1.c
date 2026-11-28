/* test_native_a8dec1.c - [GT6 0174] the fused a8dec1 vertex loop (tools/native_a8dec1_hook.py) against the lifted
 * loop it is generated from, from the same state: the local store, all 128 registers and the rest of the context
 * equal, and the same exit (0x6840) reached - in both float modes of an SPU thread (round toward zero + flush, and
 * the host's own), with random local stores (floats of many sizes, zeros, edge values and raw bit patterns), 1 to 9
 * vertices, both transform paths (r80) and every clamp of the packer. Then the gate: it takes these states and
 * refuses the ones it must (switch off, diagnostics, interrupts, depth, a hint, n, ranges, the saved link, code).
 *   prepare: python tools\tests\prepare_native_a8dec1_fixture.py <dir>      (writes <dir>\native_a8dec1_case.inc)
 *   build:   tools\tests\build_native_a8dec1.cmd <dir>
 *   run:     <dir>\test_native_a8dec1.exe [--bench]
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
uint32_t g_spu_depth_guard_limit = 2000u;
spu_context* trace_ctx;
uint64_t trace_hash;
unsigned trace_calls;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
void spu_direct_init(void) {}
#undef spu_drain_call                     /* the real one, the slow path of spu_drain_call_fast: must never run here */
void spu_drain_call(spu_context* ctx, uint32_t return_pc) { (void)ctx; (void)return_pc; fprintf(stderr, "drain slow path\n"); abort(); }
#define spu_drain_call spu_drain_call_fast
volatile unsigned g_ps3_ab_mask = 0;
volatile int g_ps3_ab_phase = 0;
volatile unsigned g_ps3_ab_gen = 0;
volatile unsigned g_ps3_ab_on_bits = 0;

static unsigned continued;
void orig_00006840(spu_context* ctx) { (void)ctx; continued++; }
#include "native_a8dec1_case.inc"          /* orig_<pc>: the lifted functions; a8dec1_fused_6750 + a8dec1_fused_pcs */
int gt6_native_a8dec1_gate(spu_context* ctx, const uint32_t* pcs, unsigned npcs);
static void native_6750(spu_context* ctx)
{
    a8dec1_fused_6750(ctx);
    SPU_DIRECT(ctx, 0x6840, orig_00006840);
}

static unsigned long long checks, cases, refusals;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL case %llu line %d: %s\n", cases, __LINE__, #x); exit(1); } } while (0)
static spu_context base, a, b;
static uint32_t rng = 0x6750u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static const uint32_t edges[] = {
    0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
    0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
    0x44ffe000u, 0x447fc000u, 0xc4800000u, 0x43ff8000u, 0xc4000000u, 0x3f000000u, 0x40490fdbu, 0xc0490fdbu
};
static uint32_t word(void)
{
    const uint32_t k = rnd() % 100u;
    if (k < 4) return 0;
    if (k < 10) return edges[rnd() % (sizeof edges / sizeof edges[0])];
    if (k < 18) return rnd();                                            /* any bits: NaN, Inf, denormals, ints */
    if (k < 30) return fbits((float)((int)(rnd() % 4096u) - 2048));     /* around the packer's clamps */
    const int e = (int)(rnd() % 40u) - 20;                               /* 2^-20 .. 2^19, either sign */
    return (rnd() & 0x80000000u) | ((uint32_t)(127 + e) << 23) | (rnd() & 0x7FFFFFu);
}
static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void copy_ctx(spu_context* to, const spu_context* from) { memcpy(to, from, sizeof *to); to->ls = to->ls_store; }
static void configure(int rz)
{
    _mm_setcsr(rz ? SPU_MXCSR_RZFZ : 0x1f80u);
    spu__rz_mark(rz);
    g_spu_ls_probe = g_spu_smc_watch = g_spu_wws_probe = 0;
    g_spu_ls_watch_n = g_spu_ls_rdiag = g_spu_ls_wdiag = 0;
    g_spu_trampoline_fn = NULL;
    g_spu_direct = 1;
}

static void make_case(void)
{
    memset(&base, 0, sizeof base);
    base.ls = base.ls_store;
    base.status = SPU_STATUS_RUNNING;
    base.pc = 0x6750u;
    base.host_depth = 1;
    base.steps = 1 + rnd() % 1000u;
    base.image_id = (int)(rnd() % 8u);
    base.module_img_a00 = (rnd() & 1u) ? (int)(1 + rnd() % 8u) : 0;
    for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) be32(base.ls + i, word());
    for (unsigned r = 0; r < 128; r++)
        for (unsigned k = 0; k < 4; k++) base.gpr[r]._u32[k] = word();
    /* the loop's state as 0x6708 sets it up: r1 the stack pointer, r89 / r88 = sp + 48 / + 96 (the matrices, in the
     * caller's frame), r83 the outputs, r82 the inputs, r81 the outcode bytes, r84 the counter, r90 its end, r80 the
     * transform's path, r86 a vector argument */
    const uint32_t sp = 0x30000u + 16u * (rnd() % 0xF00u);
    const uint32_t n = 1 + rnd() % 9u;
    const uint32_t i0 = rnd() % 0x10000u;
    base.gpr[1]._u32[0] = sp;
    base.gpr[89]._u32[0] = sp + 48u;
    base.gpr[88]._u32[0] = sp + 96u;
    base.gpr[83]._u32[0] = 0x10000u + 16u * (rnd() % 0x800u) + ((rnd() & 3u) == 0 ? (rnd() & 15u) : 0u);
    base.gpr[82]._u32[0] = 0x18000u + 16u * (rnd() % 0x800u) + ((rnd() & 3u) == 0 ? (rnd() & 15u) : 0u);
    base.gpr[81]._u32[0] = 0x2C000u + (rnd() % 0x1000u);
    base.gpr[84]._u32[0] = i0;
    base.gpr[90]._u32[0] = i0 + n;
    base.gpr[80]._u32[0] = (rnd() & 1u);
}

static void compare_pair(void)
{
    cases++;
    copy_ctx(&a, &base); continued = 0;
    orig_00006750(&a);
    CHECK(continued == 1);
    copy_ctx(&b, &base); continued = 0;
    native_6750(&b);
    CHECK(continued == 1);
    CHECK(!memcmp(a.ls, b.ls, SPU_LS_SIZE));
    CHECK(!memcmp(a.gpr, b.gpr, sizeof a.gpr));
    a.ls = b.ls = NULL;
    CHECK(!memcmp(&a, &b, sizeof a));
}

static void refuse(void)
{
    copy_ctx(&b, &base);
    CHECK(gt6_native_a8dec1_gate(&b, a8dec1_fused_pcs, A8DEC1_FUSED_NPCS) == 0);
    refusals++;
}

int main(int argc, char** argv)
{
    _putenv_s("GT6_NATIVE_A8DEC1", "1");
    for (int rz = 1; rz >= 0; rz--) {
        configure(rz);
        for (unsigned i = 0; i < 2000; i++) {
            make_case();
            if (i & 1) { base.int_enable = 1; base.event_mask = rnd() & 0xFFu; base.event_status = (rnd() & 0xFFu) & ~base.event_mask; }   /* enabled, none pending */
            copy_ctx(&b, &base);
            CHECK(gt6_native_a8dec1_gate(&b, a8dec1_fused_pcs, A8DEC1_FUSED_NPCS) == 1);
            compare_pair();
        }
    }
    configure(1);
    /* the gate's refusals */
    make_case(); g_spu_ls_wdiag = 1; refuse(); g_spu_ls_wdiag = 0;
    make_case(); g_spu_ls_rdiag = 1; refuse(); g_spu_ls_rdiag = 0;
    make_case(); g_spu_direct = 0; refuse(); g_spu_direct = 1;
    make_case(); base.steps = 0; refuse();
    make_case(); base.int_enable = 1; base.event_mask = 1; base.event_status = 1; refuse();
    make_case(); base.irq_frame = &base; refuse();
    make_case(); base.host_depth = g_spu_depth_guard_limit / 2u - 2u; refuse();
    make_case(); g_spu_stack_reset_hint[(0x76D0u >> 2) & 1023u] = 1; refuse(); memset(g_spu_stack_reset_hint, 0, sizeof g_spu_stack_reset_hint);
    make_case(); base.gpr[90]._u32[0] = base.gpr[84]._u32[0]; refuse();
    make_case(); base.gpr[90]._u32[0] = base.gpr[84]._u32[0] + 65537u; refuse();
    make_case(); base.gpr[1]._u32[0] = 0x100u; refuse();
    make_case(); base.gpr[1]._u32[0] += 8u; refuse();
    make_case(); base.gpr[83]._u32[0] = SPU_LS_SIZE - 16u; refuse();
    make_case(); base.gpr[81]._u32[0] = SPU_LS_SIZE - 2u; base.gpr[90]._u32[0] = base.gpr[84]._u32[0] + 9u; refuse();
    make_case(); base.gpr[83]._u32[0] = base.gpr[1]._u32[0] - 16u; refuse();           /* a vertex over the saved link */
    make_case(); { const uint32_t pg = (base.gpr[83]._u32[0] + 20u) >> SPU_CODE_PAGE_SHIFT;
                   base.resident_code_pages[pg >> 6] |= UINT64_C(1) << (pg & 63); } refuse();
    make_case(); { const uint32_t pg = (base.gpr[1]._u32[0] - 0x100u) >> SPU_CODE_PAGE_SHIFT;
                   base.resident_code_pages[pg >> 6] |= UINT64_C(1) << (pg & 63); } refuse();
    _putenv_s("GT6_NATIVE_A8DEC1", "0");
    printf("PASS: %llu pairs equal (local store, 128 registers, context, exit) in both float modes, %llu refusals, %llu checks\n",
           cases, refusals, checks);
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        configure(1);
        make_case(); base.gpr[90]._u32[0] = base.gpr[84]._u32[0] + 9u;
        LARGE_INTEGER f, t0, t1, t2; QueryPerformanceFrequency(&f);
        copy_ctx(&a, &base); copy_ctx(&b, &base);   /* then only the registers each time: the context holds the 256 KB local store */
        const int reps = 20000;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < reps; i++) { memcpy(a.gpr, base.gpr, sizeof a.gpr); a.pc = base.pc; orig_00006750(&a); }
        QueryPerformanceCounter(&t1);
        for (int i = 0; i < reps; i++) { memcpy(b.gpr, base.gpr, sizeof b.gpr); b.pc = base.pc; native_6750(&b); }
        QueryPerformanceCounter(&t2);
        const double us_l = 1e6 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / reps;
        const double us_n = 1e6 * (double)(t2.QuadPart - t1.QuadPart) / (double)f.QuadPart / reps;
        printf("bench (9 vertices, registers copied each time): lifted %.3f us, fused %.3f us (%.2fx)\n", us_l, us_n, us_l / us_n);
    }
    return 0;
}
