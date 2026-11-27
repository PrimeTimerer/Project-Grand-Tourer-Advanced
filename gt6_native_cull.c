/* gt6_native_cull.c - [GT6 0168] the triangle cull and pack of Gran Turismo 6's car vertex job as native code:
 * job module jobmod_9039b3cc346b ("SmoothMesh", image 249), function 0x13D20 and its software-pipelined loop
 * 0x13E88 - the job's heaviest block (about 0.6 - 0.8 ms of SPU time a frame in a 16-car race).
 *
 * What the SPU code does (the module's own bytes: EMAIN.ELF at 0x0143F280, streamed to LS 0x4000; the constants and
 * tables it loads are compared with what follows on every call):
 *   inputs  r3 = the end of the output (it is written downwards), r5 = the vertex records, r8 = the number of groups
 *           (4 triangles each), r4 * 8 + r7 = the bit position of the first group in the packed index stream,
 *           r6 = per index i of a group (byte i, i = 0..11) its bit offset; byte 1 is also the index width w and
 *           byte 12 a group's length in bits.
 *   a group twelve w-bit indices out of a 32-byte window of the stream (aligned at the group's bit position):
 *           index i starts at byte ((bo + r6[i]) >> 3) & 31 of the window, bit (bo + r6[i]) & 7 (bo = the position
 *           & 127); the 32 bits there, shifted left by that bit and right by 28 - w, are index * 16 plus the next
 *           stream bits, which the 16-byte aligned load of the record drops. Triangle t = indices 3t, 3t+1, 3t+2;
 *           a record is x, y, w, (output index << 16) | flags. Kept = the low half of the three flags' AND is zero
 *           and not 0 > the 3x3 determinant of the three (x, y, w). The kept triangles' output indices (u16 each)
 *           are right-aligned in 32 bytes by the module's two shufb tables (0x165E0 and 0x166E0, by the 4-bit kept
 *           mask), stored as the 32 bytes below r3 (three quadword stores, the one at r3 merged with what is
 *           there), and r3 goes down by 6 per kept triangle.
 * The native form does the groups in program order. Every value a store or r3 depends on is computed as the lifted
 * code computes it: the determinant with the native single-precision ops of the SPU float mode and the helpers' -0
 * rule where no input of the group can take any step out of their range (else with the helpers themselves - the
 * same results and the same MXCSR exception flags either way), the compare with the SPU float compare; the pack and the stores from the same tables, as the bytes the three quadword stores leave.
 * tools\tests\test_native_cull.c compares the local store and the registers with the lifted chain's. The caller
 * (0x7B20, returning to 0x7B60) reads r3 only; r2, r4 and r8 are left as the lifted code leaves them as well; r5 -
 * r79 are not (volatile under the SPU ABI; the lifted loop leaves speculative values of later groups there).
 *
 * gt6_native_cull_13d20 returns 0 - the lifted code runs - when: the switch is off, the thread is not in the SPU
 * float mode, the local-store diagnostics are armed, the lifted chain would not run straight through (direct
 * calls off, a pending interrupt, a stack-reset hint), the group count is 0 (the lifted loop's 2^32 passes) or
 * above 65536, the constants or tables in the local store are not the module's, the stream position would wrap,
 * the output range overlaps the index stream or the vertex records (the lifted loop reads the next group before it
 * stores this one) or a page of resident code (the lifted stores note those).
 *
 * Switch: on by default, GT6_NATIVE_CULL=0 off; GT6_AB name "cull" (bit 0x80, main.cpp). Measured 2026-11-28 in two
 * in-run A/Bs (16-car race, 240 s, 8 s phases): -0.88 ms a frame +- 0.46 (-4.5 %) and -0.92 +- 0.51 (-4.8 %).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include "ps3emu/ps3_ab.h"

#define GT6_AB_NATIVE_CULL 0x0080u   /* GT6_AB "cull" (main.cpp); the other bits: ps3recomp/include/ps3emu/ps3_ab.h */

#if defined(__AVX2__) && defined(__FMA__) && defined(SPU_MXCSR_RZFZ) && !defined(SPU_SCALAR_HELPERS)

static int s_cull_env = -1;
static int gt6_cull_on(void)
{
    int e = s_cull_env;
    if (__builtin_expect(e < 0, 0)) {
        const char* v = getenv("GT6_NATIVE_CULL");
        e = !(v && v[0] == '0' && v[1] == '\0');
        s_cull_env = e;
    }
    return ps3_ab_on(GT6_AB_NATIVE_CULL, e);
}

/* the module's constants as 13D20 loads them (local-store bytes = SPU byte order) */
static const uint8_t k_162E0[16] = { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3 };                           /* r2  */
static const uint8_t k_165A0[32] = { 0x08,0x09,0x0A,0x0B, 0x18,0x19,0x1A,0x1B, 0x0C,0x0D,0x0E,0x0F, 0x1C,0x1D,0x1E,0x1F,  /* r15 */
                                     0x00,0x01,0x02,0x03, 0x10,0x11,0x12,0x13, 0x04,0x05,0x06,0x07, 0x14,0x15,0x16,0x17 }; /* r14 */
static const uint8_t k_169E0[48] = { 8,8,8,8, 9,9,9,9, 10,10,10,10, 11,11,11,11,                     /* r11 */
                                     4,4,4,4, 5,5,5,5, 6,6,6,6, 7,7,7,7,                                 /* r9  */
                                     0x00,0x01,0x10,0x11, 0x04,0x05,0x14,0x15, 0x08,0x09,0x18,0x19, 0x0C,0x0D,0x1C,0x1D }; /* r16 */

static inline u128 cu(__m128i v) { u128 r; memcpy(&r, &v, 16); return r; }
static inline __m128i cm(u128 v) { __m128i r; memcpy(&r, &v, 16); return r; }
static inline int span_overlap(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi) { return alo < bhi && blo < ahi; }

/* Tables made once from the rules the module's code and data follow (the local store is compared with them on
 * every call, so a module that differs is only refused):
 *   s_pack: the module's two shufb tables (0x165E0 = the first 16 bytes, 0x166E0 = the last 16 of 32) - for kept
 *           mask m (triangle t in bit 3 - t), 0x80 (a zero byte) for 32 - 6 * kept bytes, then for each kept t in
 *           order the bytes of its three output indices: 4t, 4t+1 (r62's word t, high half = v0), 4t+2, 4t+3 (v1),
 *           0x10 + 4t, 0x11 + 4t (v2's flags word, high half).
 *   s_pa / s_pb: the same as two pshufb controls, one on each source (0x80 = a zero byte).
 *   s_rot: a quadword rotated right by k bytes (what rotqby by 16 - k does).
 *   s_dc0 / s_dc1 / s_dsh: the twelve indices of a group in the usual layout (r6 byte i = i * w, w <= 16; the game
 *           uses 11): from the 32 stream bytes at the group's byte position (two vectors), index i's big-endian word
 *           at byte (b0 + i * w) >> 3 into lane i & 3 of vector i >> 2 (a pshufb on each half), and its left shift
 *           (b0 + i * w) & 7 (b0 = the position's bit, 0..7). Used for a group whose twelve words all lie inside
 *           the lifted code's 32-byte window (bo + 11 w <= 231: no wrap at its end, no carry between its bytes). */
#define NC_WMAX 16u
static uint8_t s_pack[16][32];
static __m128i s_pa[16][2], s_pb[16][2], s_rot[16];
static __m128i s_dc0[NC_WMAX + 1][8][3], s_dc1[NC_WMAX + 1][8][3], s_dsh[NC_WMAX + 1][8][3];
static volatile int s_tables;
static void make_tables(void)
{
    for (unsigned m = 0; m < 16; ++m) {
        const unsigned n = (unsigned)__builtin_popcount(m);
        unsigned at = 32u - 6u * n;
        memset(s_pack[m], 0x80, 32);
        for (unsigned t = 0; t < 4; ++t)
            if (m & (8u >> t)) {
                const uint8_t c[6] = { (uint8_t)(4 * t), (uint8_t)(4 * t + 1), (uint8_t)(4 * t + 2), (uint8_t)(4 * t + 3),
                                       (uint8_t)(0x10 + 4 * t), (uint8_t)(0x11 + 4 * t) };
                memcpy(s_pack[m] + at, c, 6);
                at += 6;
            }
        for (unsigned h = 0; h < 2; ++h) {
            uint8_t pa[16], pb[16];
            for (unsigned i = 0; i < 16; ++i) {
                const uint8_t c = s_pack[m][16 * h + i];
                pa[i] = c < 0x10 ? c : 0x80;
                pb[i] = (c >= 0x10 && c < 0x20) ? (uint8_t)(c - 0x10) : 0x80;
            }
            memcpy(&s_pa[m][h], pa, 16);
            memcpy(&s_pb[m][h], pb, 16);
        }
    }
    for (unsigned k = 0; k < 16; ++k) {
        uint8_t r[16];
        for (unsigned i = 0; i < 16; ++i) r[i] = (uint8_t)((i - k) & 15u);
        memcpy(&s_rot[k], r, 16);
    }
    for (unsigned w = 0; w <= NC_WMAX; ++w)
        for (unsigned b0 = 0; b0 < 8; ++b0)
            for (unsigned k = 0; k < 3; ++k) {
                uint8_t c0[16], c1[16];
                uint32_t sh[4];
                for (unsigned j = 0; j < 4; ++j) {
                    const unsigned bit = b0 + (4 * k + j) * w, q = bit >> 3;
                    for (unsigned e = 0; e < 4; ++e) {          /* host byte e of the lane = stream byte q + 3 - e */
                        const unsigned src = q + 3u - e;
                        c0[4 * j + e] = src < 16u ? (uint8_t)src : 0x80;
                        c1[4 * j + e] = (src >= 16u && src < 32u) ? (uint8_t)(src - 16u) : 0x80;
                    }
                    sh[j] = bit & 7u;
                }
                memcpy(&s_dc0[w][b0][k], c0, 16);
                memcpy(&s_dc1[w][b0][k], c1, 16);
                memcpy(&s_dsh[w][b0][k], sh, 16);
            }
    s_tables = 1;
}

static inline __m128 xr_pos0(__m128 s)
{
    const __m128i i = _mm_castps_si128(s);
    return _mm_castsi128_ps(_mm_andnot_si128(_mm_cmpeq_epi32(i, _mm_set1_epi32((int)0x80000000u)), i));
}
#define XR_NEG(x) _mm_xor_ps((x), _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u)))

/* one log line for the first call taken and for the first refusal of each kind (the log shows the path is used) */
static volatile int s_said[16];
static __attribute__((noinline, cold)) int nc_refused(int why)
{
    static const char* const text[] = { "switch off", "not in the SPU float mode", "LS diagnostics armed",
        "the lifted chain would stop (direct calls / steps / stack hint)", "an interrupt pending", "group count",
        "constants not the module's", "tables not the module's", "stream position wraps", "stream out of the LS",
        "records span", "output out of the LS", "output over the stream or the records", "output over resident code" };
    if (!s_said[why]) {
        s_said[why] = 1;
        fprintf(stderr, "[native-cull] 13D20 left to the lifted code: %s\n", text[why]);
    }
    return 0;
}

int gt6_native_cull_13d20(spu_context* ctx)
{
    if (!gt6_cull_on())
        return nc_refused(0);
    if (!spu__x8_rz())
        return nc_refused(1);
    if (g_spu_ls_rdiag || g_spu_ls_wdiag)
        return nc_refused(2);
#ifdef SPU_DIRECT_TAILCALLS
    /* the chain 13D20 -> 13D50 -> 13E80 -> 13E88 goes through three SPU_DIRECTs; run only where all three continue */
    if (!(g_spu_direct > 0) || !ctx->steps || g_spu_stack_reset_hint[(0x13D50u >> 2) & 1023u] ||
        g_spu_stack_reset_hint[(0x13E80u >> 2) & 1023u] || g_spu_stack_reset_hint[(0x13E88u >> 2) & 1023u])
        return nc_refused(3);
#endif
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask))
        return nc_refused(4);
    const uint32_t groups = ctx->gpr[8]._u32[0];
    if (groups == 0 || groups > 65536u)
        return nc_refused(5);
    uint8_t* const ls = ctx->ls;
    if (!s_tables)
        make_tables();
    if (memcmp(ls + 0x162E0, k_162E0, 16) || memcmp(ls + 0x165A0, k_165A0, 32) || memcmp(ls + 0x169E0, k_169E0, 48))
        return nc_refused(6);
    for (unsigned m = 0; m < 16; ++m)
        if (memcmp(ls + 0x165E0 + 16 * m, s_pack[m], 16) || memcmp(ls + 0x166E0 + 16 * m, s_pack[m] + 16, 16))
            return nc_refused(7);

    const u128 r6 = ctx->gpr[6];
    const uint32_t vbase = ctx->gpr[5]._u32[0];
    const uint32_t bit0 = (ctx->gpr[4]._u32[0] << 3) + ctx->gpr[7]._u32[0];   /* r65 = (r4 << 3) + r7 */
    const uint32_t step = r6._u8[12 ^ 3];                                     /* r22 = (r6 << 9 bytes) & 255 */
    const uint32_t width = r6._u8[1 ^ 3];                                     /* r20 = width - 28 */
    const uint32_t cnt = (28u - width) & 63u;                                 /* rotm r20: right by -(r20) & 63 */
    uint32_t r3 = ctx->gpr[3]._u32[0];

    /* the ranges: the stream windows, the records, the output */
    const uint64_t last = (uint64_t)bit0 + (uint64_t)(groups - 1u) * step;
    if (last > 0xFFFFFFFFull)
        return nc_refused(8);
    const uint32_t s_lo = (bit0 >> 3) & ~15u, s_hi = (((uint32_t)last >> 3) & ~15u) + 32u;
    if (s_hi > SPU_LS_SIZE || ((bit0 >> 3) & ~(uint32_t)SPU_LS_MASK))
        return nc_refused(9);
    const uint32_t v_span = cnt >= 32u ? 16u : (cnt <= 13u ? (uint32_t)SPU_LS_SIZE : (1u << (32u - cnt)) + 16u);
    const uint32_t v_lo = vbase & SPU_LS_MASK & ~15u;
    if (v_span >= SPU_LS_SIZE || v_lo + v_span > SPU_LS_SIZE)
        return nc_refused(10);
    const uint64_t o_low = (uint64_t)r3 - 32u - 24ull * (groups - 1u);
    if (r3 > SPU_LS_SIZE - 16u || (int64_t)o_low < 0)
        return nc_refused(11);
    const uint32_t o_lo = (uint32_t)o_low & ~15u, o_hi = (r3 & ~15u) + 16u;
    if (span_overlap(o_lo, o_hi, s_lo, s_hi) || span_overlap(o_lo, o_hi, v_lo, v_lo + v_span))
        return nc_refused(12);
    /* a store into resident code is noted per store by the lifted code (spu_ls_code_store_check): leave that to it */
    for (uint32_t pg = o_lo >> SPU_CODE_PAGE_SHIFT; pg <= ((o_hi - 1u) >> SPU_CODE_PAGE_SHIFT); ++pg)
        if (ctx->resident_code_pages[pg >> 6] & (UINT64_C(1) << (pg & 63)))
            return nc_refused(13);
    /* the usual index layout: r6 byte i = i * w, w <= 16, and the 32 bytes the fast decode reads inside the LS */
    int usual = width <= NC_WMAX && ((uint32_t)last >> 3) + 32u <= SPU_LS_SIZE;
    for (unsigned i = 0; usual && i < 12; ++i)
        usual = r6._u8[i ^ 3] == i * width;
    const uint32_t fast_bo = usual ? 231u - 11u * width : 0u;      /* a group with bo <= this: the fast decode */
    if (!s_said[usual ? 15 : 14]) {
        s_said[usual ? 15 : 14] = 1;
        fprintf(stderr, "[native-cull] 13D20 native: first call with %s index layout (%u groups, width %u)\n",
                usual ? "the usual" : "another", groups, width);
    }

    const __m128i bswap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m128i hi16 = _mm_set1_epi32((int)0xFFFF0000u), lsq = _mm_set1_epi32((int)(SPU_LS_MASK & ~15u));
    const __m128i vb4 = _mm_set1_epi32((int)vbase), cntv = _mm_cvtsi32_si128((int)cnt);
    const u128 zero = cu(_mm_setzero_si128());
    static const uint8_t rev4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    u128 p4 = ctx->gpr[4];
    uint32_t bitpos = bit0;
    for (uint32_t g = 0; g < groups; ++g, bitpos += step) {
        /* the twelve records */
        __m128i rec[12];
        if (usual && (bitpos & 127u) <= fast_bo) {
            const __m128i u0 = _mm_loadu_si128((const __m128i*)(ls + (bitpos >> 3)));
            const __m128i u1 = _mm_loadu_si128((const __m128i*)(ls + (bitpos >> 3) + 16u));
            const unsigned b0 = bitpos & 7u;
            for (int k = 0; k < 3; ++k) {
                const __m128i be = _mm_or_si128(_mm_shuffle_epi8(u0, s_dc0[width][b0][k]), _mm_shuffle_epi8(u1, s_dc1[width][b0][k]));
                __m128i d = _mm_sllv_epi32(be, s_dsh[width][b0][k]);
                d = _mm_and_si128(_mm_add_epi32(_mm_srl_epi32(d, cntv), vb4), lsq);
                rec[4 * k + 0] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (uint32_t)_mm_cvtsi128_si32(d))), bswap);
                rec[4 * k + 1] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (uint32_t)_mm_extract_epi32(d, 1))), bswap);
                rec[4 * k + 2] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (uint32_t)_mm_extract_epi32(d, 2))), bswap);
                rec[4 * k + 3] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (uint32_t)_mm_extract_epi32(d, 3))), bswap);
            }
        } else {
            uint8_t win[36];
            memcpy(win, ls + ((bitpos >> 3) & SPU_LS_MASK & ~15u), 16);
            memcpy(win + 16, ls + (((bitpos >> 3) + 16u) & SPU_LS_MASK & ~15u), 16);
            memcpy(win + 32, win, 4);
            const uint32_t splat = (bitpos & 127u) * 0x01010101u;
            for (int k = 0; k < 3; ++k) {
                const uint32_t s = splat + r6._u32[k];               /* r64 / r18, word k = SPU bytes 4k..4k+3 */
                for (int j = 0; j < 4; ++j) {
                    const unsigned sh = 24u - 8u * (unsigned)j;
                    const uint32_t o = ((s >> 3) >> sh) & 31u, b = (s >> sh) & 7u;
                    uint32_t v;
                    memcpy(&v, win + o, 4);
                    const uint32_t x = __builtin_bswap32(v) << b;
                    const uint32_t idx16 = cnt < 32u ? x >> cnt : 0u;
                    rec[4 * k + j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + ((vbase + idx16) & SPU_LS_MASK & ~15u))), bswap);
                }
            }
        }
        /* x, y, w, flags of the three vertices of the four triangles */
        __m128i X[3], Y[3], W[3], F[3];
        for (int r = 0; r < 3; ++r) {
            const __m128i t0 = _mm_unpacklo_epi32(rec[r], rec[3 + r]), t1 = _mm_unpacklo_epi32(rec[6 + r], rec[9 + r]);
            const __m128i t2 = _mm_unpackhi_epi32(rec[r], rec[3 + r]), t3 = _mm_unpackhi_epi32(rec[6 + r], rec[9 + r]);
            X[r] = _mm_unpacklo_epi64(t0, t1); Y[r] = _mm_unpackhi_epi64(t0, t1);
            W[r] = _mm_unpacklo_epi64(t2, t3); F[r] = _mm_unpackhi_epi64(t2, t3);
        }
        /* the determinant, operation for operation as the loop (13E88) computes it */
        u128 det;
        {
            /* every input below 2^40 in magnitude (no Inf / NaN pattern = the SPU's extended range either): no product
             * or sum on the way can reach FLT_MAX (2^123 at most), so the helpers would take every lane as the native
             * op gives it and raise the same exception flags; otherwise the helpers themselves, as the loop does */
            const __m128i absm = _mm_set1_epi32(0x7FFFFFFF);
            __m128i mx = _mm_max_epu32(_mm_and_si128(X[0], absm), _mm_and_si128(X[1], absm));
            mx = _mm_max_epu32(mx, _mm_max_epu32(_mm_and_si128(X[2], absm), _mm_and_si128(Y[0], absm)));
            mx = _mm_max_epu32(mx, _mm_max_epu32(_mm_and_si128(Y[1], absm), _mm_and_si128(Y[2], absm)));
            mx = _mm_max_epu32(mx, _mm_max_epu32(_mm_and_si128(W[0], absm), _mm_and_si128(W[1], absm)));
            mx = _mm_max_epu32(mx, _mm_and_si128(W[2], absm));
            const __m128i big = _mm_cmpgt_epi32(mx, _mm_set1_epi32(0x537FFFFF));   /* biased exponent above 166 */
            if (__builtin_expect(_mm_testz_si128(big, big), 1)) {
                const __m128 x0 = _mm_castsi128_ps(X[0]), x1 = _mm_castsi128_ps(X[1]), x2 = _mm_castsi128_ps(X[2]);
                const __m128 y0 = _mm_castsi128_ps(Y[0]), y1 = _mm_castsi128_ps(Y[1]), y2 = _mm_castsi128_ps(Y[2]);
                const __m128 f59 = xr_pos0(_mm_mul_ps(x1, y2));
                const __m128 f73 = xr_pos0(_mm_mul_ps(x2, y0));
                const __m128 f78 = xr_pos0(_mm_fmadd_ps(XR_NEG(y1), x2, f59));
                const __m128 f4 = xr_pos0(_mm_mul_ps(x0, y1));
                const __m128 f39 = xr_pos0(_mm_fmadd_ps(XR_NEG(y2), x0, f73));
                const __m128 f43 = xr_pos0(_mm_mul_ps(f78, _mm_castsi128_ps(W[0])));
                const __m128 f44 = xr_pos0(_mm_fmadd_ps(XR_NEG(y0), x1, f4));
                const __m128 f57 = xr_pos0(_mm_fmadd_ps(f39, _mm_castsi128_ps(W[1]), f43));
                det = cu(_mm_castps_si128(xr_pos0(_mm_fmadd_ps(f44, _mm_castsi128_ps(W[2]), f57))));
                p4 = cu(_mm_castps_si128(f4));
            } else {
                const u128 p59 = spu__xr_fm(cu(X[1]), cu(Y[2]));
                const u128 p73 = spu__xr_fm(cu(X[2]), cu(Y[0]));
                const u128 p78 = spu__xr_fma(cu(Y[1]), cu(X[2]), p59, 0x80000000u, 0);
                p4 = spu__xr_fm(cu(X[0]), cu(Y[1]));
                const u128 p39 = spu__xr_fma(cu(Y[2]), cu(X[0]), p73, 0x80000000u, 0);
                const u128 p43 = spu__xr_fm(p78, cu(W[0]));
                const u128 p44 = spu__xr_fma(cu(Y[0]), cu(X[1]), p4, 0x80000000u, 0);
                const u128 p57 = spu__xr_fma(p39, cu(W[1]), p43, 0, 0);
                det = spu__xr_fma(p44, cu(W[2]), p57, 0, 0);
            }
        }
        const __m128i back = cm(spu_fcgt(zero, det));
        /* kept: ceqhi(flags AND, 0) and not back, bit 0 of each word; gb puts word 0 in bit 3 */
        const __m128i fand = _mm_and_si128(F[2], _mm_and_si128(F[0], F[1]));
        const __m128i keep = _mm_andnot_si128(back, _mm_cmpeq_epi16(fand, _mm_setzero_si128()));
        const unsigned mask = rev4[_mm_movemask_ps(_mm_castsi128_ps(_mm_slli_epi32(keep, 31)))];
        /* the 32 bytes below r3 (the module's tables on r62 = the v0 | v1 halves and on v2's flags word, in
         * local-store byte order), stored as the lifted loop's three quadword stores leave them: the quadword at
         * r3 - 32 - (r3 & 15) is the first 16 rotated right by r3 & 15, then the 32 bytes themselves at r3 - 32 */
        const __m128i pair = _mm_shuffle_epi8(_mm_or_si128(_mm_and_si128(F[0], hi16), _mm_srli_epi32(F[1], 16)), bswap);
        const __m128i v2f = _mm_shuffle_epi8(F[2], bswap);
        const __m128i lo = _mm_or_si128(_mm_shuffle_epi8(pair, s_pa[mask][0]), _mm_shuffle_epi8(v2f, s_pb[mask][0]));
        const __m128i hi = _mm_or_si128(_mm_shuffle_epi8(pair, s_pa[mask][1]), _mm_shuffle_epi8(v2f, s_pb[mask][1]));
        _mm_storeu_si128((__m128i*)(ls + ((r3 & ~15u) - 32u)), _mm_shuffle_epi8(lo, s_rot[r3 & 15u]));
        _mm256_storeu_si256((__m256i*)(ls + r3 - 32u), _mm256_set_m128i(hi, lo));
        r3 -= 6u * (uint32_t)__builtin_popcount(mask);
    }
    ctx->gpr[3]._u32[0] = r3;
    ctx->gpr[2] = spu_ls_read128(ctx, 0x162E0);
    ctx->gpr[4] = p4;
    for (int k = 0; k < 4; ++k)
        ctx->gpr[8]._u32[k] -= groups;
    return 1;
}

#else
int gt6_native_cull_13d20(spu_context* ctx) { (void)ctx; return 0; }
#endif
