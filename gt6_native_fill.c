/* gt6_native_fill.c - [GT6 0171] the SPU job runtime's local-store fill (WWS job CRT, LS 0x4124) as native code.
 *
 * All 56 lifted job modules have the same seven instructions at 0x4124 (tools/native_fill_hook.py checks the body
 * before it puts the call in):
 *     loc_4124:  stqx  r5, r3, r126        quadword store of r5 at (r3 + r126) & ~15
 *                ai    r3, r3, 16
 *                clgt  r6, r4, r3          r6 = r4 > r3 (unsigned, per word)
 *                brnz  r6, loc_4124
 *                -> 0x407C
 * - a fill of [r3, r4) in 16-byte steps (at least one store) with r5, every time a job starts: it clears the job's
 * 151 KB .bss in the car vertex job (module 9039), about 0.6 ms of every PDI SPU thread's frame at a 16-car race start
 * (Phase 0 sample ph0), 0.13 ms more in module bdc6. Here it is one memset (or a 16-byte pattern for a non-zero r5),
 * with r3 and r6 left as the loop leaves them (r3 += 16 * stores in every word; r6 = the last clgt).
 *
 * gt6_native_fill_4124 returns 0 - the lifted loop runs - for: the switch off, the local-store write diagnostics
 * armed, a range that wraps the 32-bit address or the local store, more than 16,384 stores, or a page of resident
 * code inside it (the lifted store notes those one by one).
 *
 * Switch: on by default since [GT6 0188] (ab189 with fixvec: start -1.50 +- 0.56 ms, whole race -0.79 +- 0.27);
 * GT6_NATIVE_FILL=0 off; GT6_AB name "fill" (PS3_AB_NATIVE_FILL, ps3_ab.h).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include "ps3emu/ps3_ab.h"

static int s_fill_env = -1;
static int gt6_fill_on(void)
{
    int e = s_fill_env;
    if (__builtin_expect(e < 0, 0)) {
        const char* v = getenv("GT6_NATIVE_FILL");
        e = !(v && v[0] == '0' && v[1] == '\0');      /* [GT6 0188] on unless GT6_NATIVE_FILL=0 */
        s_fill_env = e;
    }
    return ps3_ab_on(PS3_AB_NATIVE_FILL, e);
}

int gt6_native_fill_4124(spu_context* ctx)
{
    if (!gt6_fill_on() || g_spu_ls_wdiag)
        return 0;
    const uint32_t r3 = ctx->gpr[3]._u32[0], r4 = ctx->gpr[4]._u32[0], off = ctx->gpr[126]._u32[0];
    /* stores at r3 + 16 i, i = 0 .. n - 1: the loop goes on after store i while r4 > r3 + 16 (i + 1) */
    if (r3 > 0xFFFFFFEFu)
        return 0;                                             /* r3 + 16 would wrap: the unsigned compare turns */
    uint32_t n = 1;
    if (r4 > r3 + 16u) {
        const uint64_t want = ((uint64_t)r4 - r3 + 15u) / 16u;
        if (want > 16384u || (uint64_t)r3 + 16u * want > 0xFFFFFFFFull)
            return 0;
        n = (uint32_t)want;
    }
    const uint32_t a0 = (r3 + off) & SPU_LS_MASK & ~15u;
    if (a0 + 16u * n > SPU_LS_SIZE)
        return 0;                                             /* it wraps the local store */
    for (uint32_t pg = a0 >> SPU_CODE_PAGE_SHIFT; pg <= ((a0 + 16u * n - 1u) >> SPU_CODE_PAGE_SHIFT); ++pg)
        if (ctx->resident_code_pages[pg >> 6] & (UINT64_C(1) << (pg & 63)))
            return 0;
    uint8_t* const p = ctx->ls + a0;
    const u128 v = ctx->gpr[5];
    if (!(v._u32[0] | v._u32[1] | v._u32[2] | v._u32[3])) {
        memset(p, 0, 16u * (size_t)n);
    } else {
        uint8_t pat[16];                                      /* the quadword as the store writes it (big-endian words) */
        for (int k = 0; k < 4; ++k) {
            const uint32_t w = v._u32[k];
            pat[4 * k] = (uint8_t)(w >> 24); pat[4 * k + 1] = (uint8_t)(w >> 16);
            pat[4 * k + 2] = (uint8_t)(w >> 8); pat[4 * k + 3] = (uint8_t)w;
        }
        for (uint32_t i = 0; i < n; ++i)
            memcpy(p + 16u * i, pat, 16);
    }
    for (int k = 0; k < 4; ++k) {
        const uint32_t nr3 = ctx->gpr[3]._u32[k] + 16u * n;
        ctx->gpr[3]._u32[k] = nr3;
        ctx->gpr[6]._u32[k] = ctx->gpr[4]._u32[k] > nr3 ? 0xFFFFFFFFu : 0u;
    }
    return 1;
}
