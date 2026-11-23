/* rsx_dispatch_fifo_burst [GT6 0055] against the way the FIFO walker handed the same words over before it.
 *
 *   tools\tests\build_fifo_burst.cmd <dir>      builds and runs; exit 0 = every stream equal
 *
 * A random command stream - state runs of every kind (plain registers, transform constants, vertex program words,
 * also crossing from one kind into the next), BEGIN/END with batches, CLEAR, the immediate vertex data methods, the
 * methods the walker keeps for itself, other subchannels, non-incrementing runs, control words (no-op, jump, call,
 * return, the flip markers), words with the low bits set and a last run that passes the end of the buffer - is
 * walked twice over two register files:
 *   old   every header by itself: rsx_dispatch_run in the pieces rsx_live_draw_method_run cuts (alone at anything
 *         in 0x1800..0x19FC and CLEAR), or word by word through rsx_dispatch_method where the run holds a method
 *         of the walker's - what cellGcmSys.c and rsx_live_draw.c do;
 *   new   the same loop, asking the burst first at every subchannel-0 header (at most 64 headers a call).
 * Compared after every stream: the whole rsx_dispatch (registers, write counts, program, constants, BEGIN/END
 * state, inline data), every sink call with its arguments, the ring of decoded words the walker keeps (all of it:
 * the burst notes the headers it takes itself), the number of headers and the last ring offset. The program
 * counter g_rsx_vp_upload_gen: equal with g_rsx_vp_gen_on_change = 0; with 1 (the default) it must have moved
 * across every burst call that changed a program word.
 */
#include "../../ps3recomp/libs/video/rsx_dispatch.c"

static unsigned long long s_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (u32)(s_rng >> 16); }
static u32 rin(u32 lo, u32 hi) { return lo + rnd() % (hi - lo + 1u); }

/* ---- the sink: a log of every call ---- */
typedef struct { u32 n; u32 e[1 << 16][3]; } calllog;
static void note(void* user, u32 a, u32 b, u32 c)
{
    calllog* l = (calllog*)user;
    if (l->n < (1u << 16)) { l->e[l->n][0] = a; l->e[l->n][1] = b; l->e[l->n][2] = c; }
    l->n++;
}
static void s_clear(void* u, const rsx_dispatch* r, u32 mask) { (void)r; note(u, 1, mask, 0); }
static void s_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)r; note(u, 2, prim, 0); }
static void s_end(void* u, const rsx_dispatch* r) { note(u, 3, r->regs[0x1808 >> 2], r->regs[0x0B80 >> 2]); }
static void s_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count) { (void)r; note(u, 4, first, count); }
static void s_index(void* u, const rsx_dispatch* r, u32 first, u32 count) { (void)r; note(u, 5, first, count); }
static void s_inline(void* u, const rsx_dispatch* r, const u8* d, u32 bytes) { (void)r; note(u, 6, bytes, bytes ? d[0] : 0); }
static void s_flip(void* u, const rsx_dispatch* r, u32 arg) { (void)r; note(u, 7, arg, 0); }
static void s_imm(void* u, const rsx_dispatch* r) { note(u, 8, r->regs[0x1C00 >> 2], 0); }

/* ---- the walker's part (cellGcmSys.c) ---- */
static const u32 walker_methods[] = { 0x0050u, 0x0104u, 0x0180u, 0x01A4u, 0x01A8u, 0x1800u, 0x1D6Cu, 0x1D70u, 0x1D74u };
static int run_has_walker_method(u32 m0, u32 n)
{
    const u32 m1 = m0 + (n - 1u) * 4u;
    for (u32 i = 0; i < sizeof walker_methods / sizeof walker_methods[0]; i++)
        if (walker_methods[i] >= m0 && walker_methods[i] <= m1) return 1;
    return 0;
}
static u8 stop_table[0x800];

/* rsx_live_draw_method_run with every gate down */
static void live_method_run(rsx_dispatch* rsx, u32 method, int incrementing, const u8* w, u32 count)
{
    u32 i = 0;
    while (i < count) {
        const u32 m = incrementing ? method + i * 4u : method;
        u32 n = 1;
        if (incrementing && !(m & ~0x1FFCu)) {
            const u32 cm = m & 0x1FFCu;
            if (!((cm >= 0x1800u && cm < 0x1A00u) || cm == 0x1D94u)) {
                u32 stop = cm < 0x1800u ? 0x1800u : cm < 0x1D94u ? 0x1D94u : 0x2000u;
                n = (stop - cm) >> 2;
                if (n > count - i) n = count - i;
            }
        }
        rsx_dispatch_run(rsx, m, incrementing, w + (size_t)i * 4u, n);
        i += n;
    }
}

typedef struct { u32 headers, last_ring, tr_i; u32* tr_off; u32* tr_w; u32 bursts, burst_headers, gen_faults; } walk_out;
#define TR_N (1u << 20)

static void walk(rsx_dispatch* rsx, const u8* fifo, u32 bytes, u32 put, int use_burst, walk_out* o)
{
    u32 off = 0;
    while (off + 4u <= bytes) {
        const u32 w = rsx_run_word(fifo + off, 0);
        o->headers++;
        if (off < put) o->last_ring = off + 4u;
        o->tr_off[o->tr_i & (TR_N - 1u)] = off; o->tr_w[o->tr_i & (TR_N - 1u)] = w; o->tr_i++;
        const u32 type = w >> 29, count = (w >> 18) & 0x7FFu, method = w & 0x1FFCu, subch = (w >> 13) & 7u;
        if (!w) { off += 4u; continue; }                                        /* no-op */
        if ((w & 0xFFFF0000u) == 0xFEAD0000u) { off += 4u; continue; }          /* flip / queue markers */
        if (type == 1u) { off += 4u; continue; }                                /* jump (not followed here) */
        if ((w & 3u) == 2u) { off += 4u; continue; }                            /* call */
        if (w == 0x00020000u) { off += 4u; continue; }                          /* return */
        if (type == 0u || type == 2u) {
            if (off + 4u + count * 4u > bytes) break;                           /* the stream ends inside this run */
            if (subch == 0u && (method == 0x64u || method == 0x68u || method == 0x6Cu)) { off += 4u + count * 4u; continue; }
            if (count && subch <= 1u) {
                const u8* d = fifo + off + 4u;
                if (use_burst && subch == 0u) {
                    rsx_fifo_burst b;
                    b.stop = stop_table; b.max_headers = 64u;
                    b.trace_off = o->tr_off; b.trace_w = o->tr_w; b.trace_i = &o->tr_i; b.trace_mask = TR_N - 1u; b.io = off;
                    static u32 vp_before[RSX_DSP_VP_WORDS];
                    memcpy(vp_before, rsx->vp, sizeof vp_before);
                    const unsigned gen0 = g_rsx_vp_upload_gen;
                    const u32 lim = (put > off && put < bytes) ? put : bytes;
                    const u32 used = rsx_dispatch_fifo_burst(rsx, fifo + off, lim - off, &b);
                    if (memcmp(vp_before, rsx->vp, sizeof vp_before) && g_rsx_vp_upload_gen == gen0) o->gen_faults++;
                    if (used) {
                        if (off + b.last < put) o->last_ring = off + b.last + 4u;
                        o->headers += b.headers - 1u;
                        o->bursts++; o->burst_headers += b.headers;
                        off += used;
                        continue;
                    }
                }
                const u32 sm = subch << 13;
                if (!run_has_walker_method(method, type == 0u ? count : 1u))
                    live_method_run(rsx, sm | method, type == 0u, d, count);
                else
                    for (u32 i = 0; i < count; i++)
                        rsx_dispatch_method(rsx, sm | (type == 0u ? method + i * 4u : method), rsx_run_word(d, i));
            }
            off += 4u + count * 4u;
            continue;
        }
        off += 4u;
    }
}

/* ---- the stream ---- */
static u32 put32(u8* p, u32 off, u32 v) { p[off] = (u8)(v >> 24); p[off + 1] = (u8)(v >> 16); p[off + 2] = (u8)(v >> 8); p[off + 3] = (u8)v; return off + 4u; }
static u32 hdr(u32 method, u32 count, int incrementing, u32 subch) { return (incrementing ? 0u : 0x40000000u) | (count << 18) | (subch << 13) | method; }
static u32 value(void)
{
    switch (rnd() % 6u) {
    case 0: return 0;
    case 1: return rnd() % 600u;                 /* small: load pointers and constant ids stay in range mostly */
    case 2: return 0xFFFFFFFFu;
    default: return rnd() ^ (rnd() << 16);
    }
}
static u32 emit_run(u8* p, u32 off, u32 cap, u32 method, u32 count, int inc, u32 subch)
{
    if (off + 4u + count * 4u > cap) return off;
    off = put32(p, off, hdr(method, count, inc, subch));
    for (u32 i = 0; i < count; i++) off = put32(p, off, value());
    return off;
}
static u32 make_stream(u8* p, u32 cap)
{
    static const u32 special[] = { 0x1808u, 0x1814u, 0x1818u, 0x1824u, 0x1D94u, 0x1C0Cu, 0x0050u, 0x0064u, 0x0068u, 0x006Cu, 0x0104u,
                                   0x0180u, 0x01A4u, 0x01A8u, 0x1800u, 0x1D6Cu, 0x1D70u, 0x1D74u, 0x0A80u, 0x1500u, 0x1508u,
                                   0x1880u, 0x1884u, 0x1900u, 0x1940u, 0x1980u, 0x1984u, 0x1E40u, 0x0A84u };
    u32 off = 0;
    const u32 want = rin(40u, 4000u);
    for (u32 h = 0; h < want && off + 8u < cap; h++) {
        const u32 r = rnd() % 100u;
        if (r < 30u)        off = emit_run(p, off, cap, (rnd() % 0x800u) << 2, rin(1u, 8u), 1, 0);            /* anything */
        else if (r < 42u)   off = emit_run(p, off, cap, 0x0B80u + ((rnd() % 4u) << 2) * (rnd() % 3u == 0), rin(1u, 40u), 1, 0);   /* program words, sometimes on past the window */
        else if (r < 45u)   off = emit_run(p, off, cap, 0x1E9Cu, rin(1u, 2u), 1, 0);                          /* load pointer (and start) */
        else if (r < 57u)   off = emit_run(p, off, cap, 0x1EFCu + ((rnd() % 3u) << 2), rin(1u, 34u), 1, 0);   /* constant id, constants, past the window */
        else if (r < 63u)   off = emit_run(p, off, cap, 0x1700u + ((rnd() % 96u) << 2), rin(1u, 70u), 1, 0);  /* formats ... across 0x1800 */
        else if (r < 68u)   off = emit_run(p, off, cap, 0x1714u, rin(1u, 4u), 0, 0);                          /* not incrementing, plain */
        else if (r < 71u)   off = emit_run(p, off, cap, special[rnd() % (sizeof special / sizeof special[0])], rin(1u, 4u), 0, 0);
        else if (r < 80u)   off = emit_run(p, off, cap, special[rnd() % (sizeof special / sizeof special[0])], rin(1u, 3u), 1, 0);
        else if (r < 84u) { off = emit_run(p, off, cap, 0x1808u, 1u, 1, 0);                                   /* BEGIN ... END, mostly well formed */
                            if (off >= 8u && rnd() % 8u) put32(p, off - 4u, rin(1u, 10u));
                            if (rnd() % 2u) off = emit_run(p, off, cap, 0x1824u, rin(1u, 3u), 0, 0);
                            else            off = emit_run(p, off, cap, (rnd() % 0x800u) << 2, rin(1u, 5u), 1, 0);
                            off = emit_run(p, off, cap, 0x1808u, 1u, 1, 0);
                            if (off >= 8u && rnd() % 8u) put32(p, off - 4u, 0u); }
        else if (r < 88u)   off = emit_run(p, off, cap, (rnd() % 0x800u) << 2, rin(1u, 6u), rnd() % 2u, rin(1u, 7u));   /* other subchannels */
        else if (r < 90u)   off = put32(p, off, 0u);                                                           /* no-op */
        else if (r < 91u)   off = put32(p, off, 0x20000000u | ((rnd() % 0x10000u) << 2));                      /* jump */
        else if (r < 92u)   off = put32(p, off, ((rnd() % 0x10000u) << 2) | 2u);                               /* call */
        else if (r < 93u)   off = put32(p, off, 0x00020000u);                                                  /* return */
        else if (r < 94u)   off = put32(p, off, rnd() % 2u ? 0xFEAD0100u | (rnd() % 8u) : 0xFEAD0200u);        /* markers */
        else if (r < 95u)   off = put32(p, off, hdr((rnd() % 0x800u) << 2, rin(0u, 3u), 1, 0) | 1u);           /* low bit set */
        else if (r < 96u)   off = put32(p, off, hdr((rnd() % 0x800u) << 2, 0u, 1, 0));                         /* a header without words */
        else if (r < 97u)   off = put32(p, off, hdr((rnd() % 0x800u) << 2, rin(1u, 3u), 1, 0) | 0x00010000u) , off = put32(p, off, value());   /* bit 16 set: still a header to the walker */
        else                off = emit_run(p, off, cap, (rnd() % 0x800u) << 2, rin(9u, 60u), 1, 0);            /* long */
    }
    if (rnd() % 3u == 0 && off + 8u <= cap) {                  /* a last run that claims more words than there are */
        off = put32(p, off, hdr(0x0B80u, 28u, 1, 0));
        off = put32(p, off, value());
    }
    return off;
}

static rsx_dispatch A, B;
static calllog LA, LB;
static u32 tra_off[TR_N], tra_w[TR_N], trb_off[TR_N], trb_w[TR_N];

int main(int argc, char** argv)
{
    const u32 streams = argc > 1 ? (u32)atoi(argv[1]) : 3000u;
    static u8 fifo[1u << 18];
    for (u32 i = 0; i < sizeof walker_methods / sizeof walker_methods[0]; i++) stop_table[walker_methods[i] >> 2] = 1;
    stop_table[0x64u >> 2] = stop_table[0x68u >> 2] = stop_table[0x6Cu >> 2] = 1;
    rsx_dispatch_sink sink = { 0 };
    sink.clear = s_clear; sink.begin = s_begin; sink.end = s_end; sink.draw_arrays = s_arrays;
    sink.draw_index_array = s_index; sink.inline_array = s_inline; sink.flip = s_flip; sink.immediate_vertex = s_imm;
    unsigned long long total_headers = 0, total_burst_headers = 0, total_bursts = 0, total_words = 0;
    for (int on_change = 0; on_change < 2; on_change++) {
        g_rsx_vp_gen_on_change = on_change;
        sink.user = &LA; rsx_dispatch_init(&A, &sink);
        sink.user = &LB; rsx_dispatch_init(&B, &sink);
        for (u32 s = 0; s < streams; s++) {
            const u32 bytes = make_stream(fifo, sizeof fifo);
            /* put: at the end, somewhere inside (a header boundary is not promised), or behind the start */
            const u32 put = rnd() % 4u == 0 ? (rnd() % (bytes / 4u + 1u)) * 4u : rnd() % 16u == 0 ? 0u : bytes;
            walk_out oa = { 0 }, ob = { 0 };
            oa.tr_off = tra_off; oa.tr_w = tra_w; ob.tr_off = trb_off; ob.tr_w = trb_w;
            LA.n = LB.n = 0;
            const unsigned gen0 = g_rsx_vp_upload_gen;
            walk(&A, fifo, bytes, put, 0, &oa);
            const unsigned gen_a = g_rsx_vp_upload_gen - gen0;
            g_rsx_vp_upload_gen = gen0;
            walk(&B, fifo, bytes, put, 1, &ob);
            const unsigned gen_b = g_rsx_vp_upload_gen - gen0;
            const char* bad = NULL;
            if (memcmp(A.regs, B.regs, sizeof A.regs)) bad = "registers";
            else if (memcmp(A.seen, B.seen, sizeof A.seen)) bad = "write counts";
            else if (memcmp(A.vp, B.vp, sizeof A.vp)) bad = "vertex program";
            else if (memcmp(A.constants, B.constants, sizeof A.constants)) bad = "transform constants";
            else if (A.in_begin_end != B.in_begin_end || A.current_primitive != B.current_primitive ||
                     A.inline_len != B.inline_len || A.inline_dropped != B.inline_dropped) bad = "BEGIN/END state";
            else if (memcmp(A.inline_data, B.inline_data, sizeof A.inline_data)) bad = "inline data";
            else if (LA.n != LB.n || memcmp(LA.e, LB.e, sizeof LA.e[0] * (LA.n < (1u << 16) ? LA.n : (1u << 16)))) bad = "sink calls";
            else if (oa.headers != ob.headers) bad = "header count";
            else if (oa.last_ring != ob.last_ring) bad = "last ring offset";
            else if (oa.tr_i != ob.tr_i || memcmp(tra_off, trb_off, sizeof(u32) * (oa.tr_i < TR_N ? oa.tr_i : TR_N)) ||
                     memcmp(tra_w, trb_w, sizeof(u32) * (oa.tr_i < TR_N ? oa.tr_i : TR_N))) bad = "ring of decoded words";
            else if (!on_change && gen_a != gen_b) bad = "program counter (every word counted)";
            else if (ob.gen_faults) bad = "program counter stood still across a burst that changed a program word";
            if (bad) {
                printf("FAIL stream %u (gen_on_change %d, %u bytes, put %u): %s differ\n", s, on_change, bytes, put, bad);
                return 1;
            }
            total_headers += oa.headers; total_burst_headers += ob.burst_headers; total_bursts += ob.bursts; total_words += bytes / 4u;
        }
    }
    printf("PASS %u streams x 2: %llu words, %llu headers and control words, %llu headers taken in %llu bursts "
           "(registers, write counts, program, constants, BEGIN/END state, inline data, sink calls, decoded-word ring, "
           "header count, last ring offset, program counter: equal)\n",
           streams, total_words, total_headers, total_burst_headers, total_bursts);
    return 0;
}
