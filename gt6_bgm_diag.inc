/* GT6 SGX compatibility and read-only diagnostics. Include after peek_ok()/peek32(); call
 * gt6_bgm_diag_tick(pass) after the existing peek thread's Sleep(period).
 * Off by default. GT6_BGM_PEEK=1: <=1 Hz summaries; GT6_BGM_PEEK_RAW=1:
 * also dump the matched 0x248-byte voice. No guest functions or writes.
 * Samples are best-effort: lifecycle changes detected during the read are
 * discarded, but this intentionally does not lock the guest audio engine.
 * GT6_BGM_TRACE=1 independently enables at most 128 start-stage records.
 * Known custom-file returns have a separate 32-record stage6 budget.
 * gt6_sgx_file_handle_is_pointer() is an always-on compatibility guard,
 * independent of diagnostic environment variables; it performs reads only.
 */
#include <atomic>
static bool gt6_bgm_diag_words(uint32_t ea, uint32_t* dst, unsigned count)
{
    const uint64_t end = (uint64_t)ea + (uint64_t)count * 4u;
    if (ea < 0x10000u || end > 0x100000000ull || !peek_ok(ea, count * 4u)) return false;
    for (unsigned i = 0; i < count; ++i) dst[i] = peek32(ea + i * 4u);
    return true;
}

static bool gt6_bgm_diag_equal(uint32_t ea, const uint32_t* words, unsigned count)
{
    if (!peek_ok(ea, count * 4u)) return false;
    for (unsigned i = 0; i < count; ++i) if (peek32(ea + i * 4u) != words[i]) return false;
    return true;
}

static unsigned gt6_bgm_diag_byte(const uint32_t* words, unsigned offset)
{
    return (words[offset >> 2] >> (24u - (offset & 3u) * 8u)) & 0xffu;
}

static int gt6_bgm_diag_s16(const uint32_t* words, unsigned offset)
{
    const unsigned u = (gt6_bgm_diag_byte(words, offset) << 8) | gt6_bgm_diag_byte(words, offset + 1);
    return u < 0x8000u ? (int)u : (int)u - 0x10000;
}

/* The game's custom file-open callback returns a guest pool pointer, or
 * exactly -1 on allocation failure. High guest addresses sign-extend to
 * negative values, so CAF668's generic signed-handle test rejects them.
 * Recognize only this callback and a readable, aligned record inside its
 * current pool. Other callbacks keep their original negative-error rules.
 * The caller uses this only to qualify the existing signed-negative branch.
 * Do not inspect the changing free list: the callback already allocated the
 * returned record, and asynchronous file workers can change other slots. */
extern "C" int gt6_sgx_file_handle_is_pointer(ppu_context* ctx)
{
    if (!ctx || (uint32_t)ctx->gpr[28] != 0x0162AD20u) return 0;
    const uint32_t handle = (uint32_t)ctx->gpr[3];
    if (!(handle & 0x80000000u) || handle == 0xFFFFFFFFu) return 0;
    uint32_t opd[2], pool[4];
    if (!gt6_bgm_diag_words(0x0162AD20u, opd, 2) ||
        opd[0] != 0x00909EB4u || opd[1] != 0x01644F38u ||
        !gt6_bgm_diag_words(0x01710B00u, pool, 4)) return 0;
    const uint32_t base = pool[1], bytes = pool[2], stride = pool[3];
    const uint64_t end = (uint64_t)base + bytes;
    if (stride != 0x440u || !bytes || bytes % stride ||
        base < 0x10000u || end > 0x100000000ull ||
        handle < base || (uint64_t)handle + stride > end ||
        (handle - base) % stride || !peek_ok(handle, stride)) return 0;
    return gt6_bgm_diag_equal(0x01710B04u, pool + 1, 3) ? 1 : 0;
}

/* func_00CC29A8: 0 entry, 1 CBBDE0 returned, 2 CC7E5C returned,
 * 3 CAF668 returned, 4 streaming CCBD24 returned, 5 direct CCBD24 returned.
 * Later stages retain voice in r31. Stage6 is the known custom file-open
 * callback return inside CAF668; its r31 is a file slot, not a voice.
 * This helper intentionally never calls guest code or changes ctx/memory.
 * Return registers are the callee's actual post-DRAIN_TRAMPOLINE values. */
extern "C" void gt6_bgm_diag_trace(ppu_context* ctx, unsigned stage)
{
    static const bool enabled = []() {
        const char* e = getenv("GT6_BGM_TRACE");
        return e && atoi(e) != 0;
    }();
    if (!enabled || !ctx || stage > 6u) return;
    if (stage == 6u) {
        if ((uint32_t)ctx->gpr[28] != 0x0162AD20u) return;
        static std::atomic<unsigned> file_count{0};
        unsigned sequence = file_count.load(std::memory_order_relaxed);
        while (sequence < 32u && !file_count.compare_exchange_weak(sequence, sequence + 1u,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        if (sequence >= 32u) return;
        uint32_t pool[4] = {};
        const bool pok = gt6_bgm_diag_words(0x01710B00u, pool, 4);
        /* Reserve sequence129..160 so the original128 start records cannot
         * exhaust this known-callback-only budget or duplicate its IDs. */
        fprintf(stderr,
            "[bgm-trace %u] stage=6 tid=%llu lr=%016llX r3=%016llX ret32=%d opd=%08X file_slot=%08X ptr_ok=%d pok=%u pool_base=%08X pool_bytes=%08X pool_stride=%08X\n",
            129u + sequence, (unsigned long long)ctx->thread_id,
            (unsigned long long)ctx->lr, (unsigned long long)ctx->gpr[3],
            (int32_t)ctx->gpr[3], (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[31],
            gt6_sgx_file_handle_is_pointer(ctx), (unsigned)pok, pool[1], pool[2], pool[3]);
        return;
    }
    static std::atomic<unsigned> count{0};
    unsigned sequence = count.load(std::memory_order_relaxed);
    while (sequence < 128u && !count.compare_exchange_weak(sequence, sequence + 1u,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
    if (sequence >= 128u) return;

    const uint32_t ea = (uint32_t)ctx->gpr[stage ? 31 : 3];
    uint32_t table[2] = {}, words[0x248 / 4] = {};
    bool vok = gt6_bgm_diag_words(0x017ED670u, table, 2) &&
        table[1] && table[1] <= 4096u && ea >= table[0] &&
        (ea - table[0]) % 0x248u == 0u && (ea - table[0]) / 0x248u < table[1] &&
        gt6_bgm_diag_words(ea, words, 0x248 / 4);
    if (vok) vok = words[8 / 4] == (ea - table[0]) / 0x248u &&
        gt6_bgm_diag_equal(0x017ED670u, table, 2) &&
        gt6_bgm_diag_equal(ea + 8u, words + 8 / 4, 3);
    /* Stage 1's r16 points to CBBDE0's resource-object output. Header
     * fields are logged as raw BE words: the SGXD wave integers are LE. */
    uint32_t wave[16] = {}, resource_ptr = 0, resource[3] = {};
    const bool wok = stage == 1u && gt6_bgm_diag_words((uint32_t)ctx->gpr[3], wave, 16);
    const bool rok = stage == 1u &&
        gt6_bgm_diag_words((uint32_t)ctx->gpr[16], &resource_ptr, 1) &&
        gt6_bgm_diag_words(resource_ptr, resource, 3);
    fprintf(stderr,
        "[bgm-trace %u] stage=%u tid=%llu lr=%016llX r3=%016llX r4=%016llX r5=%016llX r6=%016llX r7=%016llX r8=%016llX ret32=%d ea=%08X vok=%u voice=%u resource=%d type=%u flags11=%02X gain16=%d stream=%d state126=%02X state127=%02X state128=%02X state129=%02X codec=%08X cb16c=%08X init134=%d init138=%d init13c=%d cursor228=%016llX wok=%u wave08=%08X wave10=%08X wave14=%08X wave30=%08X rok=%u resobj=%08X resmode=%u resbase=%08X resfile=%08X\n",
        sequence + 1u, stage, (unsigned long long)ctx->thread_id,
        (unsigned long long)ctx->lr,
        (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
        (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6],
        (unsigned long long)ctx->gpr[7], (unsigned long long)ctx->gpr[8],
        (int32_t)ctx->gpr[3], ea, (unsigned)vok, words[8 / 4],
        gt6_bgm_diag_s16(words, 0x12), gt6_bgm_diag_byte(words, 0x10),
        gt6_bgm_diag_byte(words, 0x11), gt6_bgm_diag_s16(words, 0x24), gt6_bgm_diag_s16(words, 0x22),
        gt6_bgm_diag_byte(words, 0x126), gt6_bgm_diag_byte(words, 0x127),
        gt6_bgm_diag_byte(words, 0x128), gt6_bgm_diag_byte(words, 0x129),
        words[0x12c / 4], words[0x16c / 4], (int32_t)words[0x134 / 4],
        (int32_t)words[0x138 / 4], (int32_t)words[0x13c / 4],
        (unsigned long long)(((uint64_t)words[0x228 / 4] << 32) | words[0x22c / 4]),
        (unsigned)wok, wave[8 / 4], wave[0x10 / 4], wave[0x14 / 4], wave[0x30 / 4],
        (unsigned)rok, resource_ptr, resource[0], resource[1], resource[2]);
}

static void gt6_bgm_diag_tick(unsigned pass)
{
    static int enabled = -1, raw = 0;
    static uint64_t last_ms = 0;
    if (enabled < 0) {
        const char* e = getenv("GT6_BGM_PEEK");
        enabled = e && atoi(e) != 0;
        const char* r = getenv("GT6_BGM_PEEK_RAW");
        raw = r && atoi(r) != 0;
    }
    if (!enabled) return;
    const uint64_t now = GetTickCount64();
    if (last_ms && now - last_ms < 1000u) return;
    last_ms = now;

    uint32_t pool[7], storage[32], table[2];
    const char* reason = nullptr;
    if (!gt6_bgm_diag_words(0x0171351Cu, pool, 7)) reason = "pool-unreadable";
    else if (pool[2] != 0x80u || pool[3] != 0x20u || pool[5] > 4u || pool[6] > 4u || pool[5] + pool[6] != 4u)
        reason = "pool-layout";
    else if (!gt6_bgm_diag_words(pool[1], storage, 32)) reason = "storage-unreadable";
    else if (!gt6_bgm_diag_words(0x017ED670u, table, 2) || !table[1] || table[1] > 4096u)
        reason = "voice-table";
    if (reason) {
        fprintf(stderr, "[bgm-peek %u] ms=%llu skip=%s\n", pass, (unsigned long long)now, reason);
        return;
    }

    unsigned free_mask = 0, free_count = 0;
    uint32_t next = pool[4];
    while (next) {
        if (next < pool[1] || (uint64_t)next >= (uint64_t)pool[1] + 0x80u || ((next - pool[1]) & 0x1fu)) {
            reason = "free-list-range"; break;
        }
        const unsigned slot = (next - pool[1]) / 0x20u;
        if (free_mask & (1u << slot)) { reason = "free-list-cycle"; break; }
        free_mask |= 1u << slot;
        ++free_count;
        next = storage[slot * 8u];
    }
    if (!reason && free_count != pool[6]) reason = "free-list-count";
    if (reason) {
        fprintf(stderr, "[bgm-peek %u] ms=%llu skip=%s\n", pass, (unsigned long long)now, reason);
        return;
    }

    struct BgmSample {
        uint32_t handle, descriptor, handle_words[20], ea, words[0x248 / 4];
        const char* skip;
    } samples[4] = {};
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (free_mask & (1u << slot)) continue;
        BgmSample& s = samples[slot];
        s.handle = storage[slot * 8u + 2u];
        s.descriptor = storage[slot * 8u + 3u];
        if (!storage[slot * 8u] || !gt6_bgm_diag_words(s.handle, s.handle_words, 20)) {
            s.skip = "handle-unreadable"; continue;
        }
        if (s.handle_words[0] != 0x016CCA10u) { s.skip = "not-sgx-interface"; continue; }
        const uint32_t id = s.handle_words[0x3c / 4];
        const uint64_t ea = (uint64_t)table[0] + (uint64_t)id * 0x248u;
        if (id >= table[1] || ea + 0x248u > 0x100000000ull) { s.skip = "voice-range"; continue; }
        s.ea = (uint32_t)ea;
        if (!gt6_bgm_diag_words(s.ea, s.words, 0x248 / 4)) { s.skip = "voice-unreadable"; continue; }
        if (s.words[8 / 4] != id || gt6_bgm_diag_s16(s.words, 0x12) != (int32_t)s.handle_words[0x38 / 4]) {
            s.skip = "voice-resource-mismatch"; continue;
        }
        if (!(s.words[0x0c / 4] & 1u)) { s.skip = "voice-inactive"; continue; }
        /* Avoid combining different lifetimes; normal cursor movement is
         * permitted. A 32-bit high-word change across the 64-bit cursor read
         * would make that sample ambiguous, so discard it too. */
        if (!gt6_bgm_diag_equal(s.handle, s.handle_words, 1) ||
            !gt6_bgm_diag_equal(s.handle + 0x38u, s.handle_words + 0x38 / 4, 3) ||
            !gt6_bgm_diag_equal(s.ea + 8u, s.words + 8 / 4, 3) ||
            !gt6_bgm_diag_equal(s.ea + 0x124u, s.words + 0x124 / 4, 3) ||
            !gt6_bgm_diag_equal(s.ea + 0x228u, s.words + 0x228 / 4, 1)) s.skip = "voice-changed";
    }
    if (!gt6_bgm_diag_equal(0x0171351Cu, pool, 7) ||
        !gt6_bgm_diag_equal(pool[1], storage, 32) ||
        !gt6_bgm_diag_equal(0x017ED670u, table, 2)) {
        fprintf(stderr, "[bgm-peek %u] ms=%llu skip=pool-changed\n", pass, (unsigned long long)now);
        return;
    }
    if (!pool[5]) {
        fprintf(stderr, "[bgm-peek %u] ms=%llu active=0 voices=%u\n", pass, (unsigned long long)now, table[1]);
        return;
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (free_mask & (1u << slot)) continue;
        const BgmSample& s = samples[slot];
        if (s.skip) {
            fprintf(stderr, "[bgm-peek %u] ms=%llu slot=%u handle=%08X skip=%s\n", pass, (unsigned long long)now, slot, s.handle, s.skip);
            continue;
        }
        const int gain = gt6_bgm_diag_s16(s.words, 0x24);
        const unsigned b185 = gt6_bgm_diag_byte(s.words, 0x185);
        const int signed185 = b185 < 128u ? (int)b185 : (int)b185 - 256;
        const uint64_t cursor = ((uint64_t)s.words[0x228 / 4] << 32) | s.words[0x22c / 4];
        fprintf(stderr,
            "[bgm-peek %u] ms=%llu slot=%u handle=%08X desc=%08X voice=%u ea=%08X resource=%d type=%u flags11=%02X active=%08X gain16=%d stream=%d lookup125=%02X lookup185=%d codec=%08X cb16c=%08X init134=%d init138=%d init13c=%d status188=%08X cursor220=%08X cursor224=%08X cursor228=%016llX\n",
            pass, (unsigned long long)now, slot, s.handle, s.descriptor, s.words[8 / 4], s.ea,
            gt6_bgm_diag_s16(s.words, 0x12), gt6_bgm_diag_byte(s.words, 0x10), gt6_bgm_diag_byte(s.words, 0x11), s.words[0x0c / 4],
            gain, gt6_bgm_diag_s16(s.words, 0x22), gt6_bgm_diag_byte(s.words, 0x125), signed185,
            s.words[0x12c / 4], s.words[0x16c / 4], (int32_t)s.words[0x134 / 4], (int32_t)s.words[0x138 / 4], (int32_t)s.words[0x13c / 4],
            s.words[0x188 / 4], s.words[0x220 / 4], s.words[0x224 / 4], (unsigned long long)cursor);
        if (raw) for (unsigned o = 0; o < 0x248u; o += 32u) {
            fprintf(stderr, "[bgm-voice %u] slot=%u voice=%u ea=%08X +%03X:", pass, slot, s.words[8 / 4], s.ea, o);
            for (unsigned k = o; k < o + 32u && k < 0x248u; k += 4u) fprintf(stderr, " %08X", s.words[k / 4]);
            fputc('\n', stderr);
        }
    }
}
