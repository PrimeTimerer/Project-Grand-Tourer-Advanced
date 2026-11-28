/* gt6_native_a8dec1.c - [GT6 0174] when the fused vertex loop of SPU job module a8dec1 may run.
 *
 * Module a8dec1's vertex loop (LS 0x6750; per vertex a call of the transform at 0x46A8, which calls the sine / cosine
 * at 0x76B0 twice, then one outcode byte) is the largest part of the SPU jobs that fill the command list at a 16-car
 * race start (Phase 0 sample ph0, trace ft1). tools/native_a8dec1_hook.py generates, from the module's own lifted
 * code, one function that runs the whole loop with the SPU registers in locals - the same helper calls on the same
 * values in the same order - and calls it at 0x6750 when gt6_native_a8dec1_gate says so. The fused loop leaves out
 * what the lifted code does between its blocks: the tests of every direct transfer (SPU_DIRECT), the call brackets
 * (host depth, the drain test, the image restore - done once at the end), the return through the link register. So
 * it runs only where all of those would take their ordinary path for the whole loop:
 *  - the switch on; the local-store read and write diagnostics off (they report pc and the link register);
 *  - direct transfers on, the run past its first step, no interrupt pending and no interrupt frame, no transfer pending,
 *    two more call levels inside the depth guard's fast half, no stack-reset hint on any block of the loop;
 *  - n = r90 - r84 (the vertices left) from 1 to 65,536; the outputs (48 bytes a vertex at r83, one outcode byte a
 *    vertex at r81) and the transform's stack frame [sp - 0x120, sp + 0x20) inside the local store, with no page of
 *    resident code in any of them (the lifted stores note those one by one), and the outputs clear of the link
 *    register's save slot [sp + 0x10, sp + 0x20) - the transform writes its vertex and then reloads its return
 *    address from there.
 * Everything else the loop does depends only on values it computes itself, the same in both.
 *
 * [GT6 0179] On a thread in the SPU float mode, with the module's constants in place, the hook runs the live-lane form
 * the tool also generates (tools/spu_lanes.py): float ops whose result words no store, branch or exit reads skip the
 * exact fix-up for those words; the last pass runs exactly, so the local store and every register end as the lifted
 * loop leaves them (tools/tests/test_native_a8dec1.c). In-run A/B 2026-11-28 (16 cars, 240 s, 3 s phases): -0.83 ms a
 * frame +- 0.46 over the race, -2.42 ms +- 0.52 (-10 %) in the start pack, where this job is the critical path.
 *
 * Switch: on by default ([GT6 0179]), GT6_NATIVE_A8DEC1=0 off; GT6_AB name "a8dec1" (PS3_AB_NATIVE_A8DEC1, ps3_ab.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include "ps3emu/ps3_ab.h"

static int s_a8_env = -1;
static int gt6_a8dec1_on(void)
{
    int e = s_a8_env;
    if (__builtin_expect(e < 0, 0)) {
        const char* v = getenv("GT6_NATIVE_A8DEC1");
        e = !(v && v[0] == '0' && v[1] == '\0');
        s_a8_env = e;
    }
    return ps3_ab_on(PS3_AB_NATIVE_A8DEC1, e);
}

/* a page of resident code in [a, b) (b > a) */
static int a8_code_in(const spu_context* ctx, uint32_t a, uint32_t b)
{
    for (uint32_t pg = a >> SPU_CODE_PAGE_SHIFT; pg <= ((b - 1u) >> SPU_CODE_PAGE_SHIFT); ++pg)
        if (ctx->resident_code_pages[pg >> 6] & (UINT64_C(1) << (pg & 63)))
            return 1;
    return 0;
}

/* each reason once in the log (as gt6_native_cull.c) */
static volatile long s_a8_told[16];
static int a8_refuse(int why)
{
    static const char* const text[] = {
        "switch off", "local-store diagnostics armed", "the lifted chain would stop (direct calls / steps)",
        "an interrupt pending or an interrupt frame", "a transfer pending", "depth guard", "a stack-reset hint on a block",
        "vertex count", "stack pointer or outputs outside the local store", "outputs over the saved return address",
        "resident code in a written range" };
    if (why > 0 && why < (int)(sizeof text / sizeof text[0]) && !_InterlockedExchange(&s_a8_told[why], 1))
        fprintf(stderr, "[native-a8dec1] 6750 left to the lifted code: %s\n", text[why]);
    return 0;
}

/* 0: the lifted loop runs; 1: the fused loop (the hook picks its live-lane form where it may) */
int gt6_native_a8dec1_gate(spu_context* ctx, const uint32_t* pcs, unsigned npcs)
{
    if (!gt6_a8dec1_on())
        return 0;
    if (g_spu_ls_wdiag || g_spu_ls_rdiag)
        return a8_refuse(1);
    if (g_spu_direct <= 0 || !ctx->steps)
        return a8_refuse(2);
    /* an interrupt that is pending now is taken at the next transfer of the lifted loop: leave it to that. One that
     * arrives while the fused loop runs is taken at its exit (the hook's transfer to 0x6840) - as if it had arrived
     * that much later; the arrival of an asynchronous event is not tied to an instruction. */
    if ((ctx->int_enable && (ctx->event_status & ctx->event_mask)) || ctx->irq_frame)
        return a8_refuse(3);
    if (g_spu_trampoline_fn)
        return a8_refuse(4);
    const uint32_t limit = g_spu_depth_guard_limit;
    if (!limit || ctx->host_depth + 2u >= limit / 2u)
        return a8_refuse(5);
    for (unsigned i = 0; i < npcs; ++i)
        if (g_spu_stack_reset_hint[(pcs[i] >> 2) & 1023u])
            return a8_refuse(6);
    const uint32_t n = ctx->gpr[90]._u32[0] - ctx->gpr[84]._u32[0];
    if (n == 0 || n > 65536u)
        return a8_refuse(7);
    const uint32_t sp = ctx->gpr[1]._u32[0], o = ctx->gpr[83]._u32[0], c = ctx->gpr[81]._u32[0];
    if ((sp & 15u) || sp < 0x120u || sp > SPU_LS_SIZE - 0x20u || o > SPU_LS_SIZE || c > SPU_LS_SIZE)
        return a8_refuse(8);
    const uint32_t o0 = o & ~15u, o1 = o0 + 48u * n;             /* (r83 + 48 k) & ~15 = o0 + 48 k */
    const uint32_t c0 = c & ~15u, c1 = ((c + n - 1u) & ~15u) + 16u;
    if (o1 > SPU_LS_SIZE || c1 > SPU_LS_SIZE)
        return a8_refuse(8);
    if (o0 < sp + 0x20u && sp + 0x10u < o1)
        return a8_refuse(9);                                     /* a vertex over the saved return address */
    if (a8_code_in(ctx, sp - 0x120u, sp + 0x20u) || a8_code_in(ctx, o0, o1) || a8_code_in(ctx, c0, c1))
        return a8_refuse(10);
    { static volatile long told; if (!_InterlockedExchange(&told, 1))
        fprintf(stderr, "[native-a8dec1] 6750 native: first call (%u vertices, interrupts %s)\n", n,
                ctx->int_enable ? "enabled" : "disabled"); }
    return 1;
}

/* --------------------------------------------------------------------------------------------------------------------
 * [GT6 0185] GT6_A8DEC1_AVX2 (default OFF; =1 on; A/B "a8avx"): the loop eight vertices at once.
 *
 * Off because the in-run A/B found no gain (ab185, 2026-11-28, 240 s: B - A = +0.15 +- 0.14 ms a frame, the start
 * 0 - 35 s +0.03 +- 0.29 ms): the loops are short (10 - 17 vertices nearly always, so one batch of eight a call), and at
 * most ~49,000 batches a second save well under 1 ms of SPU time a frame, spread over five SPU threads.
 *
 * The transform (0x46A8), per vertex: the record's 12 words - position 0-2, two angles 3-4 (with the rotation flag;
 * without it the direction 3-5), the tangent 6-8, words passed on 9-11; with the flag the sine and cosine of both angles
 * (0x76B0: a polynomial after a reduction by pi) and the direction from them; the model matrix with scale and offset
 * for the position, the matrix for the direction and the tangent; the direction packed 11-11-10 with its clamps; the
 * view-projection matrix for the clip coordinates and the four-bit outcode; the sign of a 3x3 determinant - 48 bytes
 * and one outcode byte. Here the same operations in the same order on eight vertices at once (one AVX2 vector a word of
 * the SPU register, for eight vertices). The thread is in the SPU float mode (round toward zero, flush, denormals as
 * zero), so a host add, multiply or fused multiply-add is the SPU's whenever its result is below FLT_MAX in magnitude:
 * every result is checked for that and its -0 made +0 (as spu__xr_* do); the compares use the SPU's own keys
 * (spu_fcgt); the conversions are exact in the ranges checked. A batch with any result out of range is made again by
 * the live-lane loop (its exact helpers), and so are the last 2 to 9 vertices - the last pass is the exact copy, so the
 * local store and every register end as the lifted loop leaves them, except r78 / r79: two temporaries the transform
 * writes only for a packed x below 1023, which then hold the last such vertex's values - volatile registers of the
 * called transform, which by the SPU ABI its caller never reads.
 * Only where the inputs (the records, both matrices, the offset at sp + 32) and the writes (outputs, outcodes, the
 * transform's stack frame) do not overlap: reading the inputs once and leaving out the frame's stores (the last two
 * passes make them: a vertex reads the direction its predecessor left at [sp - 288 + 0x50]) cannot change a result
 * then. tools/tests/test_native_a8dec1.c: the same states as the lifted loop. */
#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#include <time.h>

static int s_a8x_env = -1;
static int a8x_on(void)
{
    int e = s_a8x_env;
    if (__builtin_expect(e < 0, 0)) {
        const char* v = getenv("GT6_A8DEC1_AVX2");
        e = v && v[0] && !(v[0] == '0' && v[1] == '\0');   /* off unless set (ab185: no gain) */
        s_a8x_env = e;
    }
    return ps3_ab_on(PS3_AB_A8DEC1_AVX2, e);
}

static volatile long long g_a8x_stat[3];   /* batches made here, batches made again exactly, calls */
volatile long long g_a8x_batches_total;   /* batches made here since the start (tools/tests/test_native_a8dec1.c) */

#define A8X_I(x) _mm256_set1_epi32((int)(x))
#define A8X_F(bits) _mm256_castsi256_ps(A8X_I(bits))
static inline __m256i a8x_bswap(__m256i v)
{
    const __m256i m = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                       3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    return _mm256_shuffle_epi8(v, m);
}
/* an SPU float result: below FLT_MAX in magnitude (else the batch is made exactly), and +0 for -0 */
static inline __m256 a8x_fix(__m256 r, __m256i* bad)
{
    const __m256i ri = _mm256_castps_si256(r);
    *bad = _mm256_or_si256(*bad, _mm256_cmpgt_epi32(_mm256_and_si256(ri, A8X_I(0x7FFFFFFF)), A8X_I(0x7F7FFFFE)));
    return _mm256_castsi256_ps(_mm256_andnot_si256(_mm256_cmpeq_epi32(ri, A8X_I(0x80000000u)), ri));
}
#define FM(a, b)      a8x_fix(_mm256_mul_ps((a), (b)), &bad)
#define FA(a, b)      a8x_fix(_mm256_add_ps((a), (b)), &bad)
#define FMA(a, b, c)  a8x_fix(_mm256_fmadd_ps((a), (b), (c)), &bad)
#define FMS(a, b, c)  a8x_fix(_mm256_fmsub_ps((a), (b), (c)), &bad)
#define FNMS(a, b, c) a8x_fix(_mm256_fnmadd_ps((a), (b), (c)), &bad)
/* spu_fcgt: every zero (biased exponent 0) is 0, the rest by sign and magnitude - exact for any bits */
static inline __m256i a8x_key(__m256 x)
{
    const __m256i xi = _mm256_castps_si256(x);
    const __m256i zero = _mm256_cmpeq_epi32(_mm256_and_si256(xi, A8X_I(0x7F800000)), _mm256_setzero_si256());
    const __m256i mag = _mm256_andnot_si256(zero, _mm256_and_si256(xi, A8X_I(0x7FFFFFFF)));
    const __m256i neg = _mm256_srai_epi32(xi, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}
#define FCGT(a, b) _mm256_cmpgt_epi32(a8x_key(a), a8x_key(b))
/* cflts (scale 0): truncation; at 2^31 and above the SPU saturates - such a batch is made exactly */
static inline __m256i a8x_cflts(__m256 x, __m256i* bad)
{
    *bad = _mm256_or_si256(*bad, _mm256_cmpgt_epi32(_mm256_and_si256(_mm256_castps_si256(x), A8X_I(0x7FFFFFFF)),
                                                    A8X_I(0x4EFFFFFF)));
    return _mm256_cvttps_epi32(x);
}
#define SELB(a, b, m) _mm256_or_si256(_mm256_andnot_si256((m), (a)), _mm256_and_si256((m), (b)))   /* m ? b : a */
#define NOT(a) _mm256_xor_si256((a), A8X_I(0xFFFFFFFFu))
#define FBITS(x) _mm256_castps_si256(x)
#define BITSF(x) _mm256_castsi256_ps(x)

/* 0x76B0: *a = the word it stores at [r5] (r10), *b = the one at [r4] (r3) */
static inline void a8x_sincos(__m256 x, __m256* a, __m256* b, __m256i* pbad)
{
    __m256i bad = *pbad;
    const __m256 INVPI = A8X_F(0x3EA2F983), HALFPI = A8X_F(0x3FC90FDA), NHALFPI = A8X_F(0xBFC90FDA), NPI = A8X_F(0xC0490FDA);
    const __m256 C3 = A8X_F(0xBE2AAAAA), C5 = A8X_F(0x3C088888), C7 = A8X_F(0xB9500D00), C9 = A8X_F(0x3638EF1D);
    const __m256i SIGN = A8X_I(0x80000000u), ONE = A8X_I(1), ZERO = _mm256_setzero_si256();
    const __m256 r62 = FM(x, INVPI);
    const __m256 r59 = FA(x, HALFPI);
    const __m256i r54 = _mm256_sub_epi32(ZERO, NOT(_mm256_cmpgt_epi32(FBITS(x), A8X_I(-1))));      /* x negative: 1 */
    const __m256i r56 = a8x_cflts(r62, &bad);
    const __m256 r52 = FM(r59, INVPI);
    const __m256i r45 = _mm256_sub_epi32(ZERO, NOT(_mm256_cmpgt_epi32(FBITS(r59), A8X_I(-1))));
    const __m256 r50 = _mm256_cvtepi32_ps(_mm256_sub_epi32(r56, r54));                            /* csflt, scale 0 */
    const __m256i r46 = a8x_cflts(r52, &bad);
    const __m256i r40 = NOT(_mm256_cmpeq_epi32(_mm256_and_si256(r56, ONE), r54));
    const __m256 r48 = FMA(r50, NPI, x);
    const __m256 r37 = _mm256_cvtepi32_ps(_mm256_sub_epi32(r46, r45));
    const __m256i r34 = NOT(_mm256_cmpeq_epi32(_mm256_and_si256(r46, ONE), r45));
    const __m256 r39 = FA(r48, NHALFPI);
    const __m256 r33 = FMA(r37, NPI, x);
    const __m256 r32 = BITSF(SELB(_mm256_xor_si256(FBITS(r39), SIGN), FBITS(r39), r40));
    const __m256 r25 = FM(r32, r32);
    const __m256 r28 = BITSF(SELB(FBITS(r33), _mm256_xor_si256(FBITS(r33), SIGN), r34));
    const __m256 r20 = FM(r28, r28);
    const __m256 r31 = FM(r32, r25);
    const __m256 r26 = FM(r28, r20);
    const __m256 r29 = FM(r25, r31);
    const __m256 r30 = FMA(r31, C3, r32);
    const __m256 r11 = FM(r20, r26);
    const __m256 r23 = FM(r25, r29);
    const __m256 r24 = FMA(r29, C5, r30);
    const __m256 r13 = FMA(r26, C3, r28);
    const __m256 r8 = FM(r20, r11);
    const __m256 r9 = FM(r25, r23);
    const __m256 r7 = FMA(r23, C7, r24);
    const __m256 r14 = FMA(r11, C5, r13);
    const __m256 r6 = FM(r20, r8);
    *a = FMA(r9, C9, r7);
    const __m256 r12 = FMA(r8, C7, r14);
    *b = FMA(r6, C9, r12);
    *pbad = bad;
}

typedef struct {
    __m256 m[3][3];        /* model matrix rows 0-2, words 0-2 */
    __m256 vp[4][4];       /* view-projection rows 0-3 */
    __m256 t[3], s, rs[3]; /* offset, scale, rows summed (r42) */
    int rot;               /* the rotation flag (r80) */
} a8x_inv;

static inline __m256 a8x_splat(uint32_t bits) { return A8X_F(bits); }

/* eight vertices: 1 = made and stored, 0 = a result out of range (nothing stored) */
static int a8x_batch(spu_context* ctx, uint32_t rec, uint32_t out, uint32_t oc, const a8x_inv* v)
{
    uint8_t* const ls = ctx->ls;
    __m256i bad = _mm256_setzero_si256();
    const __m256i at48 = _mm256_setr_epi32(0, 48, 96, 144, 192, 240, 288, 336);
    __m256i w[12];
    for (int q = 0; q < 3; q++) {
        const __m256i base = _mm256_and_si256(_mm256_add_epi32(A8X_I(rec + 16u * (uint32_t)q), at48),
                                              A8X_I(SPU_LS_MASK & ~0xFu));
        for (int k = 0; k < 4; k++)
            w[4 * q + k] = a8x_bswap(_mm256_i32gather_epi32((const int*)ls, _mm256_add_epi32(base, A8X_I(4 * k)), 1));
    }
    __m256 dx, dy, dz;
    __m256i r90w;
    if (v->rot) {
        __m256 sa0, sb0, sa1, sb1;
        a8x_sincos(BITSF(w[3]), &sa0, &sb0, &bad);
        a8x_sincos(BITSF(w[4]), &sa1, &sb1, &bad);
        dx = FM(sb0, sa1);                 /* r4 = fm([r1+0x20], [r1+0x30]) */
        dy = FM(sb0, sb1);                 /* r5 = fm([r1+0x20], [r1+0x50]) */
        dz = sa0;                          /* [r1+0x40] */
        r90w = w[5];
    } else {
        dx = BITSF(w[3]); dy = BITSF(w[4]); dz = BITSF(w[5]);
        r90w = _mm256_setzero_si256();
    }
    const __m256 px = BITSF(w[0]), py = BITSF(w[1]), pz = BITSF(w[2]);
    const __m256 tx = BITSF(w[6]), ty = BITSF(w[7]), tz = BITSF(w[8]);
    __m256 wv[3], n[3], tg[3];
    for (int j = 0; j < 3; j++) {
        __m256 a = FM(v->m[0][j], px);
        a = FMA(v->m[1][j], py, a);
        a = FMA(v->m[2][j], pz, a);
        wv[j] = FMA(a, v->s, v->t[j]);
        __m256 b = FM(v->m[0][j], dx);
        b = FMA(v->m[1][j], dy, b);
        n[j] = FMA(v->m[2][j], dz, b);
        __m256 c = FM(v->m[0][j], tx);
        c = FMA(v->m[1][j], ty, c);
        tg[j] = FMA(v->m[2][j], tz, c);
    }
    /* the direction packed: x and y in 11 bits (2047 / 2, clamped to -1024 .. 1023), z in 10 (1023 / 2, -512 .. 511) */
    const __m256 F1 = A8X_F(0x3F800000), FH = A8X_F(0x3F000000), F2047 = A8X_F(0x44FFE000), F1023 = A8X_F(0x447FC000);
    const __m256 FN1024 = A8X_F(0xC4800000), F511 = A8X_F(0x43FF8000), FN512 = A8X_F(0xC4000000);
    const __m256 vx = FM(FMS(n[0], F2047, F1), FH), vy = FM(FMS(n[1], F2047, F1), FH), vz = FM(FMS(n[2], F1023, F1), FH);
    __m256i c1 = FCGT(F1023, vx), c2 = FCGT(vx, FN1024);
    const __m256i qx = SELB(A8X_I(1023), SELB(A8X_I(1024), _mm256_and_si256(_mm256_cvttps_epi32(vx), A8X_I(2047)), c2), c1);
    c1 = FCGT(F1023, vy); c2 = FCGT(vy, FN1024);
    const __m256i qy = SELB(A8X_I(1023u << 11), SELB(A8X_I(0x200000),
                            _mm256_slli_epi32(_mm256_and_si256(_mm256_cvttps_epi32(vy), A8X_I(2047)), 11), c2), c1);
    c1 = FCGT(F511, vz); c2 = FCGT(vz, FN512);
    const __m256i qz = SELB(A8X_I(511u << 22), SELB(A8X_I(0x80000000u), _mm256_slli_epi32(_mm256_cvttps_epi32(vz), 22), c2), c1);
    const __m256i packed = _mm256_or_si256(qx, _mm256_or_si256(qy, qz));
    /* clip x, y, w and the outcode (x > w, -x > w, -y > w, y > w as bits 3 .. 0) */
    __m256 cl[4];
    for (int j = 0; j < 4; j++) {
        if (j == 2) continue;
        const __m256 r40 = FM(v->vp[0][j], wv[0]);
        const __m256 r38 = FMA(v->vp[1][j], wv[1], v->vp[3][j]);
        const __m256 r37 = FMA(v->vp[2][j], wv[2], r40);
        cl[j] = FA(r37, r38);
    }
    const __m256 negx = BITSF(_mm256_xor_si256(FBITS(cl[0]), A8X_I(0x80000000u)));
    const __m256 negy = BITSF(_mm256_xor_si256(FBITS(cl[1]), A8X_I(0x80000000u)));
    const __m256i code = _mm256_or_si256(
        _mm256_or_si256(_mm256_and_si256(FCGT(cl[0], cl[3]), A8X_I(8)), _mm256_and_si256(FCGT(negx, cl[3]), A8X_I(4))),
        _mm256_or_si256(_mm256_and_si256(FCGT(negy, cl[3]), A8X_I(2)), _mm256_and_si256(FCGT(cl[1], cl[3]), A8X_I(1))));
    /* the sign of the frame's determinant, as the transform computes it */
    const __m256 t1rs2 = FM(tg[1], v->rs[2]), t0rs1 = FM(tg[0], v->rs[1]), t2rs0 = FM(tg[2], v->rs[0]);
    const __m256 t1rs0 = FM(tg[1], v->rs[0]), t0rs2 = FM(tg[0], v->rs[2]);
    const __m256 n0t = FM(n[0], t1rs2), t2rs1 = FM(tg[2], v->rs[1]);
    __m256 d = FMA(n[2], t0rs1, n0t);
    d = FMA(n[1], t2rs0, d);
    d = FNMS(n[0], t2rs1, d);
    d = FNMS(n[2], t1rs0, d);
    d = FNMS(n[1], t0rs2, d);
    const __m256i negdet = _mm256_xor_si256(FBITS(d), A8X_I(0x80000000u));
    if (!_mm256_testz_si256(bad, bad))
        return 0;
    /* the stores, vertex by vertex in the transform's order: [out+0x20], [out], [out+0x10], then the outcode byte */
    uint32_t o[12][8], cd[8];
    const __m256i ov[12] = { FBITS(wv[0]), FBITS(wv[1]), FBITS(wv[2]), packed, FBITS(tg[0]), FBITS(tg[1]), FBITS(tg[2]), negdet,
                             w[9], w[10], w[11], r90w };
    for (int k = 0; k < 12; k++) _mm256_storeu_si256((__m256i*)o[k], a8x_bswap(ov[k]));
    _mm256_storeu_si256((__m256i*)cd, code);
    for (uint32_t i = 0; i < 8; i++) {
        const uint32_t a = out + 48u * i;
        uint8_t* q2 = ls + ((a + 0x20u) & (SPU_LS_MASK & ~0xFu));
        uint8_t* q0 = ls + (a & (SPU_LS_MASK & ~0xFu));
        uint8_t* q1 = ls + ((a + 0x10u) & (SPU_LS_MASK & ~0xFu));
        for (int k = 0; k < 4; k++) memcpy(q2 + 4 * k, &o[8 + k][i], 4);
        for (int k = 0; k < 4; k++) memcpy(q0 + 4 * k, &o[k][i], 4);
        for (int k = 0; k < 4; k++) memcpy(q1 + 4 * k, &o[4 + k][i], 4);
        ls[(oc + i) & SPU_LS_MASK] = (uint8_t)cd[i];
    }
    return 1;
}
#undef FM
#undef FA
#undef FMA
#undef FMS
#undef FNMS

/* [a, a + n) and [b, b + m) share a byte */
static inline int a8x_overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m) { return a < b + m && b < a + n; }

void gt6_native_a8dec1_lv(spu_context* ctx, void (*lv)(spu_context*))
{
    const uint32_t i0 = ctx->gpr[84]._u32[0], end = ctx->gpr[90]._u32[0], n = end - i0;
    if (n <= 9u || !a8x_on()) { lv(ctx); return; }
    const uint32_t sp = ctx->gpr[1]._u32[0], r82 = ctx->gpr[82]._u32[0], r83 = ctx->gpr[83]._u32[0];
    const uint32_t r81 = ctx->gpr[81]._u32[0], r88 = ctx->gpr[88]._u32[0] & ~15u, r89 = ctx->gpr[89]._u32[0] & ~15u;
    /* the reads and the writes of the whole loop (the gate has the writes inside the local store) */
    const uint32_t in0 = r82 & ~15u, in1 = ((r82 + 48u * (n - 1u) + 0x20u) & ~15u) + 16u;
    const uint32_t o0 = r83 & ~15u, o1 = o0 + 48u * n, c0 = r81 & ~15u, c1 = ((r81 + n - 1u) & ~15u) + 16u;
    const uint32_t f0 = sp - 0x120u, f1 = sp + 0x20u, t0 = (sp + 32u) & ~15u;
    if (in1 > SPU_LS_SIZE || in1 <= in0 || r88 > SPU_LS_SIZE - 64u || r89 > SPU_LS_SIZE - 48u || t0 > SPU_LS_SIZE - 16u) {
        lv(ctx); return;
    }
    const uint32_t ra[4][2] = { { in0, in1 - in0 }, { r89, 48u }, { r88, 64u }, { t0, 16u } };
    const uint32_t wa[3][2] = { { o0, o1 - o0 }, { c0, c1 - c0 }, { f0, f1 - f0 } };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++)
            if (a8x_overlap(ra[i][0], ra[i][1], wa[j][0], wa[j][1])) { lv(ctx); return; }
    if (a8x_overlap(o0, o1 - o0, f0, f1 - f0) || a8x_overlap(c0, c1 - c0, f0, f1 - f0)) { lv(ctx); return; }
    /* what stays the same for the whole loop */
    a8x_inv v;
    const u128 m0 = spu_ls_read128(ctx, r89), m1 = spu_ls_read128(ctx, r89 + 0x10u), m2 = spu_ls_read128(ctx, r89 + 0x20u);
    const u128 rows[3] = { m0, m1, m2 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) v.m[i][j] = a8x_splat(rows[i]._u32[j]);
    for (int i = 0; i < 4; i++) {
        const u128 r = spu_ls_read128(ctx, r88 + 0x10u * (uint32_t)i);
        for (int j = 0; j < 4; j++) v.vp[i][j] = a8x_splat(r._u32[j]);
    }
    const u128 tq = spu_ls_read128(ctx, sp + 32u);
    for (int j = 0; j < 3; j++) v.t[j] = a8x_splat(tq._u32[j]);
    v.s = a8x_splat(ctx->gpr[86]._u32[0]);
    const u128 rsum = spu_fa(m2, spu_fa(m1, m0));                /* r42 = fa(r26, fa(r67, r68)) */
    for (int j = 0; j < 3; j++) v.rs[j] = a8x_splat(rsum._u32[j]);
    v.rot = spu_xsbh(spu_ceqbi(ctx->gpr[80], 0))._u16[0] == 0;   /* the transform's test: 0x47A0 without it */
    long long made = 0, again = 0;
    /* at least two vertices for the live-lane loop at the end: a vertex's registers depend on the frame its predecessor
     * left (its direction r46 at [sp - 288 + 0x50] reaches lanes 1-3 of r5 through the sine's read of that quad), and
     * the batches leave the frame out - so the one before the last is made by the loop as well */
    while (ctx->gpr[90]._u32[0] - ctx->gpr[84]._u32[0] > 9u) {
        if (a8x_batch(ctx, ctx->gpr[82]._u32[0], ctx->gpr[83]._u32[0], ctx->gpr[81]._u32[0], &v)) {
            ctx->gpr[82] = spu_ai(ctx->gpr[82], 384);
            ctx->gpr[83] = spu_ai(ctx->gpr[83], 384);
            ctx->gpr[81] = spu_ai(ctx->gpr[81], 8);
            ctx->gpr[84] = spu_ai(ctx->gpr[84], 8);
            made++;
        } else {                                                 /* these eight exactly */
            const u128 lim = ctx->gpr[90];
            ctx->gpr[90]._u32[0] = ctx->gpr[84]._u32[0] + 8u;
            lv(ctx);
            ctx->gpr[90] = lim;
            again++;
        }
    }
    lv(ctx);                                                     /* the last 2 .. 9, the last pass exactly */
    __atomic_fetch_add(&g_a8x_batches_total, made, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_a8x_stat[0], made, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_a8x_stat[1], again, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_a8x_stat[2], 1, __ATOMIC_RELAXED);
    { static volatile long told; if (!_InterlockedExchange(&told, 1))
        fprintf(stderr, "[native-a8dec1] eight at once (AVX2): first call, %u vertices, rotation %s\n", n, v.rot ? "on" : "off"); }
    { static clock_t t; const clock_t now = clock();
      if (!t) t = now;
      else if (now - t >= 5 * CLOCKS_PER_SEC) {
          t = now;
          const long long b = _InterlockedExchange64(&g_a8x_stat[0], 0), a = _InterlockedExchange64(&g_a8x_stat[1], 0),
                          c = _InterlockedExchange64(&g_a8x_stat[2], 0);
          if (c) fprintf(stderr, "[native-a8dec1] avx2 last 5 s: %lld loops, %lld batches of eight, %lld made again exactly\n", c, b, a);
      } }
}
#else
void gt6_native_a8dec1_lv(spu_context* ctx, void (*lv)(spu_context*)) { lv(ctx); }
#endif
