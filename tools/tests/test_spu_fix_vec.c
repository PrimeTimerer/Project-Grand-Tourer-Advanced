/* test_spu_fix_vec.c - [GT6 0176] the four-lane exact forms of the SPU float fix-up path (spu__xv_fa / fm / fma in
 * runtime/spu/spu_helpers.h) against the per-lane exact forms they stand in for (spu__fa_any / spu__fm_any /
 * spu__fma_any), on a thread in the SPU float mode (MXCSR round toward zero + flush, the x87 mark set): every lane of
 * random operands from all classes - biased exponent 255 (the SPU's large numbers), zero exponents with and without a
 * fraction, both ends of the range, equal magnitudes of either sign, any exponent gap, raw bits. Then the whole
 * helpers (spu_fa, spu_fs, spu_fm, spu_fma, spu_fms, spu_fnms: fast path + fix-up) with g_spu_fix_vec 0 and 1.
 *   build: tools\tests\build_spu_fix_vec.cmd <dir>      run: <dir>\test_spu_fix_vec.exe [cases]
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

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t operand(void)
{
    const uint32_t s = rnd() & 0x80000000u, f = rnd() & 0x7FFFFFu;
    switch (rnd() % 12u) {
    case 0: case 1: return s | 0x7F800000u | f;                           /* biased exponent 255 */
    case 2: return s | f;                                                 /* exponent 0: an SPU zero, with a fraction */
    case 3: return s | (rnd() % 3u ? 0u : 1u);                            /* zero */
    case 4: return s | ((240u + rnd() % 16u) << 23) | f;                  /* the top of the range */
    case 5: return s | ((1u + rnd() % 12u) << 23) | f;                    /* the bottom */
    case 6: return rnd();                                                 /* any bits */
    case 7: return s | (0xFFu << 23) | 0x7FFFFFu;                         /* the largest */
    default: return s | ((1u + rnd() % 254u) << 23) | f;                  /* an ordinary number */
    }
}
static unsigned long long lanes, fails;
static void check(const char* what, uint32_t got, uint32_t want, uint32_t x, uint32_t y, uint32_t z)
{
    lanes++;
    if (got != want && fails++ < 20)
        fprintf(stderr, "FAIL %s: %08X %08X %08X -> %08X, exact %08X\n", what, x, y, z, got, want);
}

int main(int argc, char** argv)
{
    const unsigned long long n = argc > 1 ? strtoull(argv[1], 0, 10) : 4000000ull;
    _mm_setcsr(SPU_MXCSR_RZFZ);
    spu__rz_mark(1);
    if (!spu__xv_rz()) { fprintf(stderr, "not in the SPU float mode\n"); return 1; }
    u128 a, b, c;
    for (unsigned long long i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            a._u32[k] = operand(); b._u32[k] = operand(); c._u32[k] = operand();
            switch (rnd() % 8u) {                     /* related operands: cancellation, equal exponents, near gaps */
            case 0: b._u32[k] = a._u32[k] ^ 0x80000000u; break;
            case 1: c._u32[k] = a._u32[k] ^ (rnd() & 0x80000001u); break;
            case 2: b._u32[k] = (a._u32[k] & 0xFF800000u) | (rnd() & 0x7FFFFFu); break;
            case 3: { const uint32_t e = (a._u32[k] >> 23) & 0xFFu, g = rnd() % 64u;
                      if (e > g) b._u32[k] = (rnd() & 0x80000000u) | ((e - g) << 23) | (rnd() & 0x7FFFFFu); } break;
            default: break;
            }
        }
        for (int neg = 0; neg < 2; neg++) {
            const uint32_t nb = neg ? 0x80000000u : 0u;
            const u128 v = spu__xv_fa(a, b, nb);
            for (int k = 0; k < 4; k++) check("fa", v._u32[k], spu__fa_any(a._u32[k], b._u32[k] ^ nb), a._u32[k], b._u32[k], nb);
        }
        { const u128 v = spu__xv_fm(a, b);
          for (int k = 0; k < 4; k++) check("fm", v._u32[k], spu__fm_any(a._u32[k], b._u32[k]), a._u32[k], b._u32[k], 0); }
        for (int form = 0; form < 3; form++) {
            const uint32_t np = form == 2 ? 0x80000000u : 0u, nc = form == 1 ? 0x80000000u : 0u;
            const u128 v = spu__xv_fma(a, b, c, np, nc);
            for (int k = 0; k < 4; k++)
                check(form == 0 ? "fma" : form == 1 ? "fms" : "fnms", v._u32[k],
                      spu__fma_any(a._u32[k], b._u32[k], c._u32[k], np, nc), a._u32[k], b._u32[k], c._u32[k]);
        }
        if (i % 4u == 0) {                            /* the whole helpers: fix-up path off and on */
            u128 r0[6], r1[6];
            g_spu_fix_vec = 0;
            r0[0] = spu_fa(a, b); r0[1] = spu_fs(a, b); r0[2] = spu_fm(a, b);
            r0[3] = spu_fma(a, b, c); r0[4] = spu_fms(a, b, c); r0[5] = spu_fnms(a, b, c);
            g_spu_fix_vec = 1;
            r1[0] = spu_fa(a, b); r1[1] = spu_fs(a, b); r1[2] = spu_fm(a, b);
            r1[3] = spu_fma(a, b, c); r1[4] = spu_fms(a, b, c); r1[5] = spu_fnms(a, b, c);
            for (int j = 0; j < 6; j++)
                for (int k = 0; k < 4; k++) check("helper", r1[j]._u32[k], r0[j]._u32[k], a._u32[k], b._u32[k], (uint32_t)j);
        }
    }
    if (fails) { printf("FAIL: %llu of %llu lanes differ\n", fails, lanes); return 1; }
    printf("PASS: %llu lanes equal (fa fs fm fma fms fnms, four-lane exact vs per-lane exact, and the helpers with the "
           "fix-up off / on)\n", lanes);
    if (argc > 2 && !strcmp(argv[2], "--bench")) {
        /* throughput over 4096 operand pairs like the race's: ordinary numbers, and in a random lane or two (here
         * 60 % of the vectors) an operand of exponent 255 - the fix-up's everyday case */
        static u128 xa[4096], xb[4096], xc[4096];
        for (int i = 0; i < 4096; i++)
            for (int k = 0; k < 4; k++) {
                xa[i]._u32[k] = (rnd() & 0x80000000u) | ((100u + rnd() % 50u) << 23) | (rnd() & 0x7FFFFFu);
                xb[i]._u32[k] = (rnd() & 0x80000000u) | ((100u + rnd() % 50u) << 23) | (rnd() & 0x7FFFFFu);
                xc[i]._u32[k] = (rnd() & 0x80000000u) | ((100u + rnd() % 50u) << 23) | (rnd() & 0x7FFFFFu);
                if (rnd() % 100u < 25u) xa[i]._u32[k] |= 0x7F800000u;
            }
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
        for (int form = 0; form < 2; form++)
            for (int vec = 0; vec < 2; vec++) {
                g_spu_fix_vec = vec;
                uint32_t acc = 0;
                QueryPerformanceCounter(&t0);
                for (int rep = 0; rep < 500; rep++)
                    for (int i = 0; i < 4096; i++) {
                        const u128 r = form ? spu_fma(xa[i], xb[i], xc[i]) : spu_fm(xa[i], xb[i]);
                        acc ^= r._u32[0] ^ r._u32[1] ^ r._u32[2] ^ r._u32[3];
                    }
                QueryPerformanceCounter(&t1);
                printf("bench %s, fix-up %s: %.2f ns an op (%08X)\n", form ? "fma" : "fm", vec ? "four-lane" : "per-lane",
                       1e9 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / (500.0 * 4096.0), acc);
            }
    }
    return 0;
}
