/* [GT6 0052] The rewritten SPU helpers against their references, bit for bit:
 *   spu_shufb (no byte swaps)            vs spu_shufb_ref
 *   spu_shlqbii / spu_rotqbii (vector)   vs the scalar _ref forms
 *   spu_csflt / spu_cuflt (native under the SPU float mode) vs the per-lane _ref forms, every i8 as a constant
 * Build: see run_test.bat (clang-cl, the flags of the lifted SPU sources). Exit code 0 = all equal. */
#define _CRT_SECURE_NO_WARNINGS
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t s0 = 0x9E3779B97F4A7C15ull, s1 = 0xD1B54A32D192ED03ull;
static uint64_t rnd(void)
{
    uint64_t a = s0, b = s1;
    s0 = b;
    a ^= a << 23;
    s1 = a ^ b ^ (a >> 17) ^ (b >> 26);
    return s1 + b;
}
static u128 rnd128(void)
{
    u128 r;
    r._u64[0] = rnd();
    r._u64[1] = rnd();
    return r;
}
static unsigned long long fails, checks;
static void cmp(const char* what, u128 got, u128 want, u128 a, u128 b, u128 c, int imm)
{
    checks++;
    if (memcmp(&got, &want, 16)) {
        if (fails++ < 12)
            fprintf(stderr, "FAIL %s imm=%d\n  a=%08X %08X %08X %08X\n  b=%08X %08X %08X %08X\n  c=%08X %08X %08X %08X\n got=%08X %08X %08X %08X\nwant=%08X %08X %08X %08X\n",
                    what, imm, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0], c._u32[1],
                    c._u32[2], c._u32[3], got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2],
                    want._u32[3]);
    }
}

/* every i8 as a compile-time constant: the fast forms are taken only for constants */
#define I8_16(f, a, b) f(a, b + 0) f(a, b + 1) f(a, b + 2) f(a, b + 3) f(a, b + 4) f(a, b + 5) f(a, b + 6) f(a, b + 7) \
                       f(a, b + 8) f(a, b + 9) f(a, b + 10) f(a, b + 11) f(a, b + 12) f(a, b + 13) f(a, b + 14) f(a, b + 15)
#define I8_ALL(f, a) I8_16(f, a, 0) I8_16(f, a, 16) I8_16(f, a, 32) I8_16(f, a, 48) I8_16(f, a, 64) I8_16(f, a, 80) I8_16(f, a, 96) I8_16(f, a, 112) \
                     I8_16(f, a, 128) I8_16(f, a, 144) I8_16(f, a, 160) I8_16(f, a, 176) I8_16(f, a, 192) I8_16(f, a, 208) I8_16(f, a, 224) I8_16(f, a, 240)
#define CU(a, i) cmp("cuflt", spu_cuflt(a, (i)), spu_cuflt_ref(a, (i)), a, a, a, (i));
#define CS(a, i) cmp("csflt", spu_csflt(a, (i)), spu_csflt_ref(a, (i)), a, a, a, (i));
static __attribute__((noinline)) void conv_all(u128 a)
{
    I8_ALL(CU, a)
    I8_ALL(CS, a)
}

int main(void)
{
    /* shufb: uniform control bytes (every special selector), then plain selectors, then a == b */
    for (int i = 0; i < 30000000; i++) {
        const u128 a = rnd128(), b = rnd128();
        u128 c = rnd128();
        if (i % 3 == 1)
            for (int k = 0; k < 16; k++)
                c._u8[k] &= 0x1F;
        cmp("shufb", spu_shufb(a, b, c), spu_shufb_ref(a, b, c), a, b, c, 0);
        if (i % 5 == 0)
            cmp("shufb a==b", spu_shufb(a, a, c), spu_shufb_ref(a, a, c), a, a, c, 0);
    }
    for (int v = 0; v < 256; v++) {   /* every selector value in every lane */
        u128 a, b, c;
        for (int k = 0; k < 16; k++) {
            a._u8[k] = (uint8_t)(k * 7 + 1);
            b._u8[k] = (uint8_t)(0x80 + k * 5);
            c._u8[k] = (uint8_t)v;
        }
        cmp("shufb const", spu_shufb(a, b, c), spu_shufb_ref(a, b, c), a, b, c, v);
    }
    printf("shufb: %llu checks, %llu failures\n", checks, fails);

    /* quadword bit shifts, every count (the helpers mask it to 0..7) */
    for (int i = 0; i < 4000000; i++) {
        const u128 a = rnd128();
        for (int sh = 0; sh < 16; sh++) {
            cmp("shlqbii", spu_shlqbii(a, sh), spu_shlqbii_ref(a, sh), a, a, a, sh);
            cmp("rotqbii", spu_rotqbii(a, sh), spu_rotqbii_ref(a, sh), a, a, a, sh);
        }
    }
    printf("qbii: %llu checks, %llu failures\n", checks, fails);

    /* conversions, in the SPU float mode (the fast forms) and outside it (the exact forms) */
    for (int mode = 0; mode < 2; mode++) {
        const unsigned old = _mm_getcsr();
        if (mode == 0) {
            _mm_setcsr(SPU_MXCSR_RZFZ);
            spu__rz_mark(1);
        }
        static const uint32_t edge[] = {0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0xFFFFu, 0x10000u, 0x00FFFFFFu, 0x01000000u, 0x01000001u,
                                        0x01FFFFFFu, 0x02000001u, 0x7FFFFF80u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0x800000FFu,
                                        0xFFFFFF00u, 0xFFFFFF7Fu, 0xFFFFFF80u, 0xFFFFFFFFu, 0xC0000000u, 0xFFFFFFFEu};
        const int ne = (int)(sizeof edge / sizeof edge[0]);
        for (int i = 0; i < 400000; i++) {
            u128 a;
            for (int k = 0; k < 4; k++) {
                const uint64_t r = rnd();
                const unsigned kind = (unsigned)(r >> 60);
                a._u32[k] = kind < 3 ? edge[(r >> 8) % (uint64_t)ne]                         /* edges */
                            : kind < 9 ? (uint32_t)r >> ((r >> 40) & 31)                     /* every magnitude */
                            : kind < 12 ? 0u - ((uint32_t)r >> ((r >> 40) & 31))             /* and its negative */
                                        : (uint32_t)r;
            }
            conv_all(a);
        }
        if (mode == 0) {
            spu__rz_mark(0);
            _mm_setcsr(old);
        }
        printf("conversions (%s): %llu checks, %llu failures\n", mode == 0 ? "SPU float mode" : "host default mode", checks, fails);
    }
    printf("%s: %llu checks, %llu failures\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
