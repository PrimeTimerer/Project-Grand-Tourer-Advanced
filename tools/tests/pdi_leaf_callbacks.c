#include "spu_context.h"
#include <assert.h>

extern spu_context* trace_ctx;
extern uint64_t trace_hash;
extern unsigned trace_calls;

static void trace_word(uint32_t value)
{
    trace_hash = (trace_hash ^ value) * UINT64_C(1099511628211);
}

/* Separate TU matches the production leaf's visibility of these callbacks.
 * The metadata-only stub records the exact access stream and deliberately
 * mutates residency metadata; it never reads or changes cached r5+ or FP mode.
 */
void spu_overlay_note_ls_write(spu_context* ctx, uint32_t address, uint32_t size)
{
    assert(ctx == trace_ctx);
    trace_calls++;
    trace_word(address); trace_word(size);
    ctx->dcache_residency++;
    for (unsigned i = 0; i < ctx->resident_code_count; i++) {
        uint32_t begin = ctx->resident_code[i].lsa;
        uint32_t end = begin + ctx->resident_code[i].size;
        if (address < end && address + size > begin)
            ctx->resident_code[i].verify_state = 2;
    }
}

void spu_ls_watch_slow(uint32_t address, int write, const uint8_t* bytes,
                       uint32_t pc, uint32_t lr)
{
    trace_calls++;
    trace_word(address); trace_word((uint32_t)write); trace_word(pc); trace_word(lr);
    for (unsigned i = 0; i < 16; i++) trace_word(bytes[i]);
    /* Exactly the observable register set in the real LS diagnostics. */
    for (unsigned r = 0; r < 5; r++)
        for (unsigned lane = 0; lane < 4; lane++) trace_word(trace_ctx->gpr[r]._u32[lane]);
}
