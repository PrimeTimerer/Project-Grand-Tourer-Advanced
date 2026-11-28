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
 * Switch: GT6_NATIVE_A8DEC1=1 (default off until measured), GT6_AB name "a8dec1" (PS3_AB_NATIVE_A8DEC1, ps3_ab.h).
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
        e = v && v[0] == '1' && v[1] == '\0';
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
