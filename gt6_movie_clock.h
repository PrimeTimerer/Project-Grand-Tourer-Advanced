#pragma once
/* BCES01893 1.22 movie clock adapter. Hooks are restricted to 008AC0D4,
 * the PCM producer 008AC7B0, and the initial AU selection 008AC1A4.
 * GT6_MOVIE_CLOCK=device enables the guarded device queue correction.
 * The default remains the original clock during integration validation. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include "ps3recomp/libs/audio/cellAudio.h"

static bool gt6_movie_clock_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("GT6_MOVIE_CLOCK_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}
static bool gt6_movie_clock_device_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("GT6_MOVIE_CLOCK");
        return value && std::strcmp(value, "device") == 0;
    }();
    return enabled;
}
struct Gt6MovieClockSample {
    uint32_t object = 0, state = 0, caller = 0, port = 0;
    uint32_t vtable = 0, port_addr = 0, read_index_addr = 0;
    uint32_t player = 0, video_frame = 0, video_seek = 0;
    uint64_t video_pts = ~uint64_t(0);
    uint64_t produced = 0, pending = 0, producer = 0, callback = 0;
    uint64_t anchor = 0, tag = 0, saved_stamp = 0, saved_system = 0;
    uint32_t anchor_ready = 0, consistent = 0;
    CellAudioClockSnapshot device{};
};
struct Gt6MovieClockDecision {
    uint64_t clock;
    int64_t lead_us;
    unsigned reason; /* 0=applied; 1=device; 2=shape; 3=tag; 4=lifetime;
                       5=stale; 6=original unavailable; 7=snapshot raced;
                       8=source gap/recovery; 9=source sequence reset */
    unsigned bounded = 0;
};
static Gt6MovieClockDecision gt6_movie_clock_decide(const Gt6MovieClockSample& s, uint64_t original)
{
    Gt6MovieClockDecision result{original, 0, 0};
    const auto& d = s.device;
    if (int64_t(original) < 0) { result.reason = 6; return result; }
    if (!d.running || !d.ready || !d.device_valid || !d.device_frequency ||
        !d.device_capacity_frames) { result.reason = 1; return result; }
    // Original ELF: 008AB1F0 first constructs B10838, then installs the movie
    // subclass 015CA850. Its slot0 is the same B109EC audio notification method;
    // slots10/14 override volume/fade. Do not admit the generic 015DD718 base.
    if (s.vtable != 0x015CA850 || d.channels != 2 || d.blocks != 8 ||
        s.port_addr != d.port_addr || s.read_index_addr != d.read_index_addr) {
        result.reason = 2; return result;
    }
    if (!s.consistent) { result.reason = 7; return result; }
    if (!s.anchor_ready || !d.generation || s.saved_system < d.generation_start_us ||
        s.saved_stamp < d.generation_start_us) { result.reason = 4; return result; }
    if (s.callback < s.anchor || s.tag > d.read_index ||
        s.callback - s.anchor != d.read_index - s.tag) {
        result.reason = 3; return result;
    }
    const uint64_t capacity_us = uint64_t(d.device_capacity_frames) * 1000000 / 48000;
    if (d.query_qpc_100ns < d.device_qpc_100ns ||
        d.query_qpc_100ns - d.device_qpc_100ns > capacity_us * 10 ||
        d.query_guest_us < d.sampled_guest_us ||
        d.query_guest_us - d.sampled_guest_us > capacity_us ||
        d.presentation_us < d.query_guest_us ||
        d.presentation_us - d.query_guest_us > capacity_us + 256000000 / 48000) {
        result.reason = 5; return result;
    }
    result.lead_us = int64_t(d.presentation_us - d.query_guest_us);
    result.clock = original > uint64_t(result.lead_us) ? original - uint64_t(result.lead_us) : 0;
    return result;
}

/* R includes silent host reads. S includes only copied movie samples. Equal
 * tag counters alone therefore cannot prove that a device queue contains a
 * contiguous section of movie PCM. Recovery is measured in consumed frames,
 * using the actual backend buffer capacity, not an elapsed-time constant. */
struct Gt6MovieClockContinuity {
    uint64_t generation = 0, read = 0, consumed = 0, produced = 0;
    uint64_t recovery_read = 0, output = 0, query_us = 0;
    uint32_t object = 0, state = 0;
    bool have_sample = false, have_output = false, recovering = false;
};
static Gt6MovieClockDecision gt6_movie_clock_filter(const Gt6MovieClockSample& s,
    uint64_t original, Gt6MovieClockDecision result, Gt6MovieClockContinuity& h)
{
    const auto& d = s.device;
    if (h.generation != d.generation || h.object != s.object || h.state != s.state) {
        h = {};
        h.generation = d.generation; h.object = s.object; h.state = s.state;
    }
    if (result.reason == 1 || result.reason == 2 || result.reason == 4 || result.reason == 6) {
        // An unavailable/new lifetime is not a continuation of a presentation
        // clock. Never interpolate across it, a closed port, or an alien object.
        h.have_sample = h.have_output = false;
        h.recovering = result.reason != 6;
        return result;
    }
    if (h.have_output && s.produced < h.produced) {
        // A real guest seek can reset S without reopening the native port.
        // Preserve that explicit discontinuity, then require a fresh queue.
        h.have_sample = h.have_output = false; h.recovering = true;
        result.clock = original; result.reason = 9;
        return result;
    }
    // S is captured under the guest audio-state mutex even when its separate
    // notification counter raced. Keep the latest sequence value for seeks.
    h.produced = s.produced;
    if (result.reason == 0) {
        const bool valid_count = s.pending <= s.produced / 256;
        const uint64_t consumed = valid_count ? s.produced - s.pending * 256 : 0;
        bool contiguous = valid_count && s.pending != 0;
        if (h.have_sample) {
            contiguous &= d.read_index >= h.read && consumed >= h.consumed;
            if (contiguous) {
                const uint64_t reads = d.read_index - h.read;
                contiguous = reads <= ~uint64_t(0) / 256 &&
                             consumed - h.consumed == reads * 256;
            }
        } else if (h.recovering) {
            h.recovery_read = d.read_index;
        }
        if (!contiguous) {
            h.recovering = true; h.recovery_read = d.read_index;
        } else if (h.recovering) {
            const uint64_t needed = (uint64_t(d.device_capacity_frames) + 255) / 256;
            if (d.read_index >= h.recovery_read && d.read_index - h.recovery_read >= needed)
                h.recovering = false;
        }
        h.read = d.read_index; h.consumed = consumed; h.produced = s.produced;
        h.have_sample = valid_count;
        if (h.recovering) { result.clock = original; result.reason = 8; }
    } else {
        // Callback lag, an incoherent guest snapshot, and stale device data
        // leave source continuity unknown until a complete queue is observed.
        h.have_sample = false; h.recovering = true;
        result.clock = original;
    }

    /* Changing from a queue-corrected target to the guest target must not add
     * the entire backend queue in one call. Conversely, recovery must not move
     * time backwards. One 256-frame quantum is the original clock's resolution;
     * it permits quantization/catch-up while bounding a transition by observed
     * elapsed time plus that quantum. Seeks/generation changes above reset it. */
    if (h.have_output && d.query_guest_us < h.query_us) {
        // A stale host observation cannot create a backwards presentation
        // clock. A genuine new lifetime is handled by the generation above.
        result.clock = h.output; result.bounded = 1;
        h.have_sample = false; h.recovering = true;
        return result;
    }
    if (h.have_output) {
        const uint64_t elapsed = d.query_guest_us - h.query_us;
        const uint64_t quantum = 256000000 / 48000;
        const uint64_t step = elapsed > ~uint64_t(0) - quantum ? ~uint64_t(0) : elapsed + quantum;
        const uint64_t limit = h.output > ~uint64_t(0) - step ? ~uint64_t(0) : h.output + step;
        if (result.clock < h.output) { result.clock = h.output; result.bounded = 1; }
        if (result.clock > limit) { result.clock = limit; result.bounded = 1; }
    }
    h.output = result.clock; h.query_us = d.query_guest_us; h.have_output = true;
    return result;
}

#ifndef GT6_MOVIE_CLOCK_CORE_ONLY
static thread_local Gt6MovieClockSample gt6_movie_clock_sample;
static thread_local Gt6MovieClockContinuity gt6_movie_clock_continuity[CELL_AUDIO_PORT_MAX];

static void gt6_movie_clock_begin(uint32_t object, uint32_t state,
                                 uint64_t produced, uint64_t pending, uint32_t caller,
                                 uint32_t player = 0)
{
    if (!gt6_movie_clock_trace_enabled() && !gt6_movie_clock_device_enabled()) return;
    auto& s = gt6_movie_clock_sample;
    s = {};
    s.object = object; s.state = state; s.caller = caller;
    s.produced = produced; s.pending = pending;
    // 0079B868 keeps the movie player in r31, saved by 008AC0D4 at SP+98.
    // That original caller itself reads these fields. This is observation only,
    // never a guessed PTS origin or a read through an unrelated caller's r31.
    if (gt6_movie_clock_trace_enabled() && caller == 0x0079B868 && player >= 0x10000 &&
        uint64_t(player) + 0x281 <= 0x100000000ull) {
        s.player = player; s.video_pts = vm_read64(player + 0x170);
        s.video_frame = vm_read32(player + 0x268); s.video_seek = vm_read8(player + 0x280);
    }
    s.port = vm_read32(object + 0x58);
    s.vtable = vm_read32(object);
    s.port_addr = vm_read32(object + 0x54);
    s.read_index_addr = vm_read32(object + 0x38);
    s.callback = vm_read64(object + 0x60);
    s.anchor = vm_read64(object + 0x68);
    s.tag = vm_read64(object + 0x70);
    s.saved_system = vm_read64(object + 0x78);
    s.saved_stamp = vm_read64(object + 0x80);
    s.producer = vm_read64(object + 0xD0);
    s.anchor_ready = vm_read8(object + 0x5D);
    const uint64_t expected_pending = s.producer > s.callback ?
                                     s.producer - s.callback - 1 : 0;
    s.consistent = expected_pending == pending &&
                   produced == vm_read64(state + 0xE0);
    ps3_audio_get_clock_snapshot(s.port, &s.device);
    s.consistent &= s.callback == vm_read64(object + 0x60) &&
                    s.producer == vm_read64(object + 0xD0);
}
static uint64_t gt6_movie_clock_finish(uint64_t original)
{
    if (!gt6_movie_clock_trace_enabled() && !gt6_movie_clock_device_enabled()) return original;
    const auto& s = gt6_movie_clock_sample;
    const auto decision = gt6_movie_clock_filter(s, original,
        gt6_movie_clock_decide(s, original), gt6_movie_clock_continuity[s.port < CELL_AUDIO_PORT_MAX ? s.port : 0]);
    const uint64_t result = gt6_movie_clock_device_enabled() ? decision.clock : original;
    static thread_local uint64_t count = 0, last_second = ~uint64_t(0), generation = 0;
    if (generation != s.device.generation) {
        count = 0; last_second = ~uint64_t(0); generation = s.device.generation;
    }
    const uint64_t second = s.device.query_guest_us / 1000000;
    if (gt6_movie_clock_trace_enabled() && (++count <= 120 || second != last_second)) {
        last_second = second;
        const int64_t mapped_tag = int64_t(s.tag) + int64_t(s.callback) - int64_t(s.anchor);
        const int64_t delta_tag = int64_t(s.device.read_index) - mapped_tag;
        const int64_t tag_time = int64_t(s.device.presentation_us) - delta_tag * 256000000 / 48000;
        std::fprintf(stderr, "[movie-clock] qpc_s=%.9f caller=%08X object=%08X state=%08X port=%u gen=%llu S=%llu P=%llu C=%llu A=%llu K=%llu pending=%llu R=%llu mapped=%lld original_us=%lld device_tag_us=%lld now_us=%llu lead_us=%lld saved_stamp=%llu saved_system=%llu ready=%u/%u running=%u consistent=%u device_valid=%u device_position=%llu device_frequency=%llu device_qpc=%llu padding=%u corrected_us=%lld candidate_us=%lld guard=%u bounded=%u capacity=%u vtable=%08X player=%08X video_pts=%lld video_frame=%u video_seek=%u\n",
            s.device.query_qpc_100ns / 1e7, s.caller, s.object, s.state, s.port,
            (unsigned long long)s.device.generation, (unsigned long long)s.produced,
            (unsigned long long)s.producer, (unsigned long long)s.callback,
            (unsigned long long)s.anchor, (unsigned long long)s.tag,
            (unsigned long long)s.pending, (unsigned long long)s.device.read_index,
            (long long)mapped_tag, (long long)original, (long long)tag_time,
            (unsigned long long)s.device.query_guest_us,
            (long long)(tag_time - int64_t(s.device.query_guest_us)),
            (unsigned long long)s.saved_stamp, (unsigned long long)s.saved_system,
            s.anchor_ready, s.device.ready, s.device.running, s.consistent,
            s.device.device_valid, (unsigned long long)s.device.device_position,
            (unsigned long long)s.device.device_frequency,
            (unsigned long long)s.device.device_qpc_100ns, s.device.padding_frames,
            (long long)result, (long long)decision.clock, decision.reason, decision.bounded,
            s.device.device_capacity_frames, s.vtable, s.player, (long long)s.video_pts,
            s.video_frame, s.video_seek);
    }
    return result;
}
static void gt6_movie_clock_seek(uint32_t queue, uint64_t target,
                                uint32_t removed, uint64_t selected_pts)
{
    if (!gt6_movie_clock_trace_enabled()) return;
    static std::atomic<unsigned> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[movie-clock-seek] queue=%08X target_pts=%llu removed=%u selected_pts=%llu\n",
                     queue, (unsigned long long)target, removed, (unsigned long long)selected_pts);
}
static void gt6_movie_clock_produce(uint32_t state, uint32_t item,
                                   uint32_t destination, uint32_t samples)
{
    if (!gt6_movie_clock_trace_enabled()) return;
    static thread_local unsigned count = 0;
    static thread_local uint64_t generation = 0;
    const uint32_t object = vm_read32(state);
    CellAudioClockSnapshot device{};
    ps3_audio_get_clock_snapshot(vm_read32(object + 0x58), &device);
    if (generation != device.generation) { generation = device.generation; count = 0; }
    if (++count <= 144) {
        std::fprintf(stderr, "[movie-clock-pcm] qpc_s=%.9f state=%08X object=%08X item=%08X source=%08X pts=%llu offset=%u samples=%u dest=%08X S=%llu P=%llu C=%llu R=%llu slot=%llu gen=%llu\n",
            device.query_qpc_100ns / 1e7, state, object, item, vm_read32(item),
            (unsigned long long)vm_read64(item + 8), vm_read32(state + 0xD8), samples,
            destination, (unsigned long long)vm_read64(state + 0xE0),
            (unsigned long long)vm_read64(object + 0xD0),
            (unsigned long long)vm_read64(object + 0x60),
            (unsigned long long)device.read_index,
            (unsigned long long)vm_read64(object + 0xD8),
            (unsigned long long)device.generation);
    }
}
#endif /* GT6_MOVIE_CLOCK_CORE_ONLY */
