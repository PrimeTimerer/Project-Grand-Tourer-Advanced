/* test_native_fill.c - [GT6 0171] gt6_native_fill.c against the lifted 0x4124 fill loop of a job module, from the same
 * state: the local store, all 128 registers, pc and the rest of the context equal, and the same continuation (0x407C)
 * reached. Random r3 / r4 / r126 / r5 and local stores, including no-store-beyond-one ranges, unaligned bases, a
 * non-zero r5 and the refusals.
 *   prepare: python tools\tests\prepare_native_fill_fixture.py <dir>      (writes <dir>\native_fill_case.inc)
 *   build:   tools\tests\build_native_fill.cmd <dir>
 *   run:     <dir>\test_native_fill.exe
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

static unsigned continued;
void orig_407C(spu_context* ctx) { (void)ctx; continued++; }
#include "native_fill_case.inc"          /* orig_4124: the lifted loop, its exit renamed orig_407C */
int gt6_native_fill_4124(spu_context* ctx);
static void native_4124(spu_context* ctx)
{
    if (gt6_native_fill_4124(ctx)) { SPU_DIRECT(ctx, 0x407C, orig_407C); }
    abort();                              /* every case here is one the native form takes */
}

static unsigned long long checks, cases, refused;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL case %llu line %d: %s\n", cases, __LINE__, #x); exit(1); } } while (0)
static spu_context base, a, b;
static uint32_t rng = 0x4124u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void copy_ctx(spu_context* to, const spu_context* from) { memcpy(to, from, sizeof *to); to->ls = to->ls_store; }

static void make_case(void)
{
    memset(&base, 0, sizeof base);
    base.ls = base.ls_store;
    base.status = SPU_STATUS_RUNNING;
    base.pc = 0x4124u;
    base.host_depth = 1;
    base.steps = 1;
    for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) { const uint32_t w = rnd(); memcpy(base.ls + i, &w, 4); }
    for (unsigned r = 0; r < 128; r++)
        for (unsigned k = 0; k < 4; k++) base.gpr[r]._u32[k] = rnd();
    const uint32_t r3 = 0x8000u + (rnd() % 0x8000u);         /* with r126 below: never past the end of the LS */
    uint32_t len;
    switch (rnd() % 4u) {
    case 0: len = rnd() % 32u; break;                         /* one or two stores, r4 at or below r3 too */
    case 1: len = rnd() % 4096u; break;
    case 2: len = 151328u; break;                             /* the car vertex job's .bss */
    default: len = rnd() % 0x20000u; break;
    }
    base.gpr[3]._u32[0] = r3;
    base.gpr[4]._u32[0] = (rnd() % 8u == 0) ? r3 - (rnd() % 64u) : r3 + len;
    base.gpr[126]._u32[0] = (rnd() % 2u) ? 0u : (rnd() % 0x8000u) - 0x4000u;
    if (rnd() % 3u) base.gpr[5]._u32[0] = base.gpr[5]._u32[1] = base.gpr[5]._u32[2] = base.gpr[5]._u32[3] = 0;
}

static void compare_pair(void)
{
    cases++;
    copy_ctx(&a, &base); continued = 0; g_spu_ls_wdiag = 0;
    orig_4124(&a);
    CHECK(continued == 1);
    copy_ctx(&b, &base); continued = 0;
    native_4124(&b);
    CHECK(continued == 1);
    CHECK(!memcmp(a.ls, b.ls, SPU_LS_SIZE));
    CHECK(!memcmp(a.gpr, b.gpr, sizeof a.gpr));
    a.ls = b.ls = NULL;
    CHECK(!memcmp(&a, &b, sizeof a));
}

int main(void)
{
    _putenv_s("GT6_NATIVE_FILL", "1");
    for (unsigned i = 0; i < 3000; i++) {
        make_case();
        compare_pair();
    }
    /* refused: the lifted loop runs */
    make_case(); base.gpr[3]._u32[0] = 0xFFFFFFF8u; copy_ctx(&b, &base); CHECK(gt6_native_fill_4124(&b) == 0); refused++;
    make_case(); base.gpr[4]._u32[0] = base.gpr[3]._u32[0] + 0x60000u; copy_ctx(&b, &base); CHECK(gt6_native_fill_4124(&b) == 0); refused++;
    make_case(); base.gpr[3]._u32[0] = 0x3FFF0u; base.gpr[4]._u32[0] = 0x40100u; base.gpr[126]._u32[0] = 0;
    copy_ctx(&b, &base); CHECK(gt6_native_fill_4124(&b) == 0); refused++;
    make_case(); copy_ctx(&b, &base); g_spu_ls_wdiag = 1; CHECK(gt6_native_fill_4124(&b) == 0); g_spu_ls_wdiag = 0; refused++;
    printf("PASS: %llu pairs equal (local store, 128 registers, context, continuation), %llu refusals, %llu checks\n",
           cases, refused, checks);
    return 0;
}
