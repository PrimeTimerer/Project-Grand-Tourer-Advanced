/* test_native_cull.c - [GT6 0168] gt6_native_cull.c against the lifted 0x13D20 chain of the car vertex job (module
 * 9039b3cc346b), from the same state: the local store, pc, r0 - r4, r8, r80 - r127 and the rest of the context equal
 * (r5 - r79 are volatile; the lifted loop leaves speculative values of the groups after the last there). The module's
 * own bytes at LS 0x4000 (its constants and shufb tables), synthetic vertex records, index streams and output
 * buffers on top - no game data in the repository.
 *   prepare: python tools\tests\prepare_native_cull_fixture.py <dir>
 *   build:   tools\tests\build_native_cull.cmd <dir>
 *   run:     <dir>\test_native_cull.exe <dir>\mod9039.bin [--bench]
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
volatile unsigned g_ps3_ab_mask = 0;
volatile int g_ps3_ab_phase = 0;
volatile unsigned g_ps3_ab_gen = 0;
volatile unsigned g_ps3_ab_on_bits = 0;

#include "native_cull_cases.inc"
int gt6_native_cull_13d20(spu_context* ctx);

static unsigned long long checks, cases, compared, refused;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL case %llu line %d: %s\n", cases, __LINE__, #x); exit(1); } } while (0)
static spu_context base, a, b;
static uint8_t module[0x20000];
static size_t module_size;
static uint32_t rng = 0x13D20u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t frand(float lo, float hi) { return fbits(lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f); }
static const uint32_t edges[] = {
    0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
    0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
    0x7fffffffu, 0xffffffffu, 0x7f000000u, 0xff000000u, 0x4b800000u, 0xcb800000u, 0x5f000000u, 0xdf000000u
};
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

/* kind 0: offsets i * w (what the job builds; the game's w is 11), 1: any offsets, step and record alignment,
 * 2: edge floats often. force_w: that width (the bench). */
static unsigned force_w;
static unsigned make_case(unsigned kind, unsigned groups_wanted)
{
    memset(&base, 0, sizeof base);
    base.ls = base.ls_store;
    base.status = SPU_STATUS_RUNNING;
    base.pc = 0x13D20u;
    base.image_id = 249;
    base.spu_id = 0x2000u + rnd() % 5u;
    base.host_depth = 1;
    base.steps = 1;
    for (unsigned r = 0; r < 128; r++)
        for (unsigned k = 0; k < 4; k++) base.gpr[r]._u32[k] = rnd();
    uint8_t* ls = base.ls;
    memcpy(ls + 0x4000, module, module_size);
    const unsigned w = force_w ? force_w : 4u + rnd() % 9u;   /* index width 4 .. 12 */
    const unsigned groups = groups_wanted ? groups_wanted : 1u + rnd() % 48u;
    /* the records (two more quads: the 16-byte alignment of r5 + index * 16 + the stream's next bits) */
    const uint32_t vb = 0x18000u + (rnd() % 64u) * 16u + (kind == 1 ? rnd() % 16u : 0u);
    for (unsigned v = 0; v < (1u << w) + 2u; v++) {
        uint8_t* p = ls + (vb & ~15u) + 16u * v;
        uint32_t c[3] = { frand(-1200.0f, 1200.0f), frand(-900.0f, 900.0f), frand(-4.0f, 60.0f) };
        for (unsigned j = 0; j < 3; j++)
            if ((kind == 2 && rnd() % 4u == 0) || rnd() % 64u == 0) c[j] = edges[rnd() % (sizeof edges / sizeof edges[0])];
        const uint32_t flags = rnd() % 10u < 6u ? 0u : (rnd() & 0xFFFFu) >> (rnd() % 16u);
        be32(p, c[0]); be32(p + 4, c[1]); be32(p + 8, c[2]); be32(p + 12, (rnd() & 0xFFFF0000u) | flags);
    }
    /* the index stream and r6 */
    uint8_t r6b[16];
    for (unsigned i = 0; i < 16; i++) r6b[i] = (uint8_t)rnd();
    if (kind == 1) { r6b[1] = (uint8_t)w; }
    else { for (unsigned i = 0; i <= 12; i++) r6b[i] = (uint8_t)(i * w); }
    for (unsigned i = 0; i < 16; i++) base.gpr[6]._u8[i ^ 3] = r6b[i];
    const uint32_t sb = 0x2C000u + (rnd() % 256u) * 16u;
    for (uint32_t i = 0; i < groups * 32u + 96u; i++) ls[sb + i] = (uint8_t)rnd();
    const uint32_t bit0 = sb * 8u + rnd() % 128u;
    const uint32_t r4 = rnd() % 4096u;
    base.gpr[4]._u32[0] = r4;
    base.gpr[7]._u32[0] = bit0 - (r4 << 3);
    base.gpr[5]._u32[0] = vb;
    base.gpr[8]._u32[0] = groups;
    /* the output: written below r3, any alignment; what is there before is random */
    const uint32_t ob = 0x38000u + (rnd() % 256u) * 16u + rnd() % 16u;
    for (uint32_t i = ob - 24u * groups - 64u; i < ob + 32u; i++) ls[i] = (uint8_t)rnd();
    base.gpr[3]._u32[0] = ob;
    base.gpr[0]._u32[0] = 0x7B60u;
    base.gpr[1]._u32[0] = 0x3FE00u;
    return groups;
}

static void compare_pair(void)
{
    cases++;
    configure(1);
    copy_ctx(&a, &base);
    trace_ctx = &a;
    orig_13D20(&a);
    const unsigned fp_a = _mm_getcsr();
    configure(1);
    copy_ctx(&b, &base);
    trace_ctx = &b;
    CHECK(gt6_native_cull_13d20(&b) == 1);
    b.pc = b.gpr[0]._u32[0];                            /* SPU_RET (host_depth 1) */
    if (fp_a != _mm_getcsr())
        fprintf(stderr, "MXCSR lifted 0x%04X native 0x%04X (leaves2 %d, groups %u)\n", fp_a, _mm_getcsr(), g_gt6_pdi_leaves2, base.gpr[8]._u32[0]);
    CHECK(fp_a == _mm_getcsr());
    CHECK(!memcmp(a.ls, b.ls, SPU_LS_SIZE));
    CHECK(a.pc == b.pc);
    for (unsigned r = 0; r < 128; r++)
        if (r <= 4 || r == 8 || r >= 80) CHECK(!memcmp(&a.gpr[r], &b.gpr[r], 16));
    for (unsigned r = 5; r < 80; r++)
        if (r != 8) b.gpr[r] = a.gpr[r];
    a.ls = b.ls = NULL;
    CHECK(!memcmp(&a, &b, sizeof a));
    a.ls = a.ls_store; b.ls = b.ls_store;
    compared++;
}

static void expect_refused(const char* what)
{
    configure(1);
    copy_ctx(&b, &base);
    trace_ctx = &b;
    if (gt6_native_cull_13d20(&b) != 0) { fprintf(stderr, "FAIL: not refused: %s\n", what); exit(1); }
    CHECK(!memcmp(b.ls, base.ls, SPU_LS_SIZE));
    refused++;
}

static double bench(int native, unsigned reps)
{
    LARGE_INTEGER s, e, f; QueryPerformanceFrequency(&f);
    long long t = 0;
    for (unsigned r = 0; r < reps; r++) {
        configure(1);
        copy_ctx(&a, &base);
        QueryPerformanceCounter(&s);
        if (native) gt6_native_cull_13d20(&a); else orig_13D20(&a);
        QueryPerformanceCounter(&e);
        t += e.QuadPart - s.QuadPart;
    }
    return (double)t * 1e9 / (double)f.QuadPart / reps;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_native_cull <mod9039.bin> [--bench]\n"); return 2; }
    FILE* m = fopen(argv[1], "rb");
    if (!m) { perror(argv[1]); return 2; }
    module_size = fread(module, 1, sizeof module, m);
    fclose(m);
    if (module_size != 77264u) { fprintf(stderr, "module: %zu bytes\n", module_size); return 2; }
    const unsigned saved = _mm_getcsr();
    _putenv_s("GT6_NATIVE_CULL", "1");
    _putenv_s("GT6_NATIVE_PDI_LEAVES", "1");
    /* every kind, both forms of the lifted loop (its PDI-leaf fast path and the plain lifted body) */
    for (unsigned id = 0; id < 3000; id++) {
        make_case(id % 3u, (id % 7u == 0) ? 1u : 0u);
        g_gt6_pdi_leaves2 = (int)(id & 1u);
        compare_pair();
    }
    /* what has to stay with the lifted code */
    make_case(0, 4); base.gpr[8]._u32[0] = 0; expect_refused("no groups");
    make_case(0, 4); base.gpr[5]._u32[0] = base.gpr[3]._u32[0] - 64u; expect_refused("output over the records");
    make_case(0, 4); base.gpr[7]._u32[0] = (base.gpr[3]._u32[0] - 40u) * 8u - (base.gpr[4]._u32[0] << 3); expect_refused("output over the stream");
    make_case(0, 4); base.ls[0x165A0] ^= 1; expect_refused("a constant changed");
    make_case(0, 4); base.int_enable = 1; base.event_status = base.event_mask = 1; expect_refused("an interrupt pending");
    make_case(0, 4); base.steps = 0; expect_refused("no steps");
    make_case(0, 4); configure(0); copy_ctx(&b, &base);
    if (gt6_native_cull_13d20(&b) != 0) { fprintf(stderr, "FAIL: not refused outside the SPU float mode\n"); return 1; }
    refused++;
    if (argc > 2 && !strcmp(argv[2], "--bench")) {
        static const unsigned widths[] = { 8, 11 };
        for (unsigned wi = 0; wi < 2; wi++) {
            force_w = widths[wi];
            make_case(0, 48);
            g_gt6_pdi_leaves2 = 1;
            double best_a = 1e30, best_b = 1e30;
            for (unsigned round = 0; round < 10; round++) {
                const double x = bench(0, 400), y = bench(1, 400);
                if (x < best_a) best_a = x;
                if (y < best_b) best_b = y;
            }
            printf("BENCH 48 groups, width %u: lifted chain with the leaf fast path %.0f ns, native %.0f ns, ratio %.2f\n",
                   force_w, best_a, best_b, best_a / best_b);
        }
        force_w = 0;
    }
    _mm_setcsr(saved);
    printf("PASS: %llu pairs equal (local store, pc, r0-r4, r8, r80-r127, context), %llu refusals, %llu checks\n", compared, refused, checks);
    return 0;
}
