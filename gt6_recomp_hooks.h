/*
 * GT6 native port -- hooks compiled into the lifted code.
 *
 * tools\post_lift.py appends `#include "gt6_recomp_hooks.h"` to recompiled\ppu_recomp.h after every lift, and
 * renames the lifted bodies named below to <name>_guest. Run it after ppu_lifter.py, before building.
 *
 * Guest setjmp / longjmp (EMAIN 1.22: setjmp 0x00C61F88, longjmp 0x00C620AC)
 * --------------------------------------------------------------------------
 * The lifted setjmp only copies the guest registers into the guest jmp_buf, and the lifted longjmp only copies them
 * back and returns -- to its HOST caller. The guest stack pointer is rewound but the host is still inside the
 * frames the guest meant to abandon, running on with the registers of another frame. Gran Turismo 6 leaves its
 * inflate loop this way (0xAC8698: `if (setjmp(buf) == 0) inflate(...)`, and the output callback longjmps when it is
 * handed an empty buffer to stop the stream): the abandoned loop carried on and wrote the file to address 0 upwards,
 * across the game's own data.
 *
 * So a guest setjmp call also does a HOST setjmp, in the frame of the lifted function that calls it (the macro below
 * expands at every direct call site), and the guest longjmp ends in a host longjmp to it. The guest halves still run,
 * so the guest jmp_buf holds what the game expects and the registers are restored from it.
 */
#pragma once
#include <setjmp.h>

void func_00C61F88_guest(ppu_context* ctx);     /* the lifted setjmp body */
void func_00C620AC_guest(ppu_context* ctx);     /* the lifted longjmp body */

#ifdef __cplusplus
extern "C" {
#endif
/* Runs the guest setjmp and returns the host jmp_buf paired with that guest buffer (never NULL). */
jmp_buf* gt6_setjmp_enter(ppu_context* ctx);
/* Called in the setjmp frame when a longjmp arrived there. */
void     gt6_setjmp_landed(ppu_context* ctx);
/* Diagnostic wrappers that tools\post_lift.py --trace puts around named functions (logged when GT6_DTRACE=1). */
void     gt6_trace_enter(unsigned addr, ppu_context* ctx);
void     gt6_trace_leave(unsigned addr, ppu_context* ctx);
/* GT6_NO_MLAA=1 (gt6_host.cpp): the post-effect executor skips its SPU post-process job and copies the scene to the
 * display buffer with the game's own GPU copy. tools\post_lift.py puts the switch into the four places the RPCS3
 * launcher's "Disable MLAA" patch changes (0x008C2004, 0x008C2060, 0x008C2080, 0x008C3044). */
int      gt6_no_spu_post(void);
int      gt6_scene_copy_args(ppu_context* ctx);     /* sets up func_00C3E050's call; 0 = nothing to copy */
void     gt6_scene_copy_done(ppu_context* ctx);
/* Opt-in EMAIN 1.22 render wait experiment and read-only timing diagnostic. */
void     gt6_render_wait_target(ppu_context* ctx);
void     gt6_frame_timing_observe(ppu_context* ctx);
#ifdef __cplusplus
}
#endif

#define func_00C61F88(c) do { \
        jmp_buf* _gt6_jb = gt6_setjmp_enter(c); \
        if (setjmp(*_gt6_jb)) gt6_setjmp_landed(c); \
    } while (0)
