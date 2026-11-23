/* Diagnostic frame pacing experiment for EMAIN 1.22. Included only by
 * gt6_host.cpp, after ppu_context and the guarded peek helpers.
 *
 * GT6_RENDER_INTERVAL=0/1 changes only the VBlank target at 0x008A8A44.
 * The shared frame interval, the 60 Hz VBlank clock, and guest time remain
 * untouched. This is deliberately opt-in until real race/replay timing is
 * verified. GT6_FRAME_TIMING_TRACE=1 observes raw update arguments once/sec.
 */
#pragma once
#include <cmath>
#include <cstring>

namespace {
struct Gt6FrameTimingConfig {
    int interval;
    bool trace;
    Gt6FrameTimingConfig() : interval(-1), trace(false) {
        const char* value = getenv("GT6_RENDER_INTERVAL");
        if (value && (value[0] == '0' || value[0] == '1') && value[1] == 0)
            interval = value[0] - '0';
        const char* diagnostic = getenv("GT6_FRAME_TIMING_TRACE");
        trace = diagnostic && diagnostic[0] == '1';
    }
};

const Gt6FrameTimingConfig& gt6_frame_timing_config()
{
    static const Gt6FrameTimingConfig config;
    return config;
}

uint64_t gt6_frame_read64(uint32_t address)
{
    return ((uint64_t)peek32(address) << 32) | peek32(address + 4);
}

struct Gt6FrameTimingSample {
    uint32_t root, graphics;
    int32_t interval;
    uint64_t vblank, previous_vblank, timer_us;
};

bool gt6_frame_timing_sample(Gt6FrameTimingSample& sample)
{
    sample = {};
    if (!peek_ok(0x016ED698u, 4)) return false;
    sample.root = peek32(0x016ED698u);
    if (sample.root < 0x10000u || sample.root > UINT32_MAX - 0xA0u ||
        !peek_ok(sample.root, 0xA0)) return false;
    sample.interval = (int32_t)peek32(sample.root + 0x1C);
    sample.graphics = peek32(sample.root + 0x9C);
    if (sample.graphics < 0x10000u || sample.graphics > UINT32_MAX - 0x2B0u ||
        !peek_ok(sample.graphics, 0x2B0) || peek32(sample.graphics) != sample.root)
        return false;
    sample.vblank = gt6_frame_read64(sample.graphics + 0x1D8);
    sample.previous_vblank = gt6_frame_read64(sample.graphics + 0x2A8);
    /* AD321C stores consecutive sys_time_get_system_time differences in the
     * timer at graphics+B8, field +38. This is microseconds, not race time. */
    sample.timer_us = gt6_frame_read64(sample.graphics + 0xF0);
    return true;
}

struct Gt6FrameTimingStats {
    LARGE_INTEGER frequency{}, first{}, last{};
    uint64_t calls = 0, interval_calls = 0, invalid_values = 0;
    double total_first = 0, total_second = 0;
    double window_first = 0, window_second = 0;
};
} // namespace

extern "C" void gt6_render_wait_target(ppu_context* ctx)
{
    const Gt6FrameTimingConfig& config = gt6_frame_timing_config();
    if (config.interval < 0 || !ctx || ctx->lr != 0x008A8A48u || ctx->gpr[5] != 1)
        return;
    Gt6FrameTimingSample sample;
    if (!gt6_frame_timing_sample(sample) || ctx->gpr[3] != sample.graphics ||
        ctx->gpr[31] != sample.graphics || ctx->gpr[9] != sample.root ||
        ctx->gpr[0] != sample.previous_vblank || sample.interval < 1 ||
        sample.interval > 2 || config.interval >= sample.interval ||
        sample.previous_vblank > UINT64_MAX - (uint32_t)sample.interval ||
        ctx->gpr[4] != sample.previous_vblank + (uint32_t)sample.interval)
        return;
    /* The guest still executes its complete condvar / flip wait. Only its
     * requested VBlank target changes; all other registers/data are intact. */
    ctx->gpr[4] = sample.previous_vblank + (uint32_t)config.interval;
    static thread_local bool reported = false;
    if (!reported) {
        fprintf(stderr, "[gt6-render-interval] experimental wait target %d -> %d; "
                "guest interval and VBlank clock unchanged\n", sample.interval, config.interval);
        reported = true;
    }
}

extern "C" void gt6_frame_timing_observe(ppu_context* ctx)
{
    if (!gt6_frame_timing_config().trace || !ctx) return;
    static thread_local Gt6FrameTimingStats stats;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!stats.frequency.QuadPart) {
        QueryPerformanceFrequency(&stats.frequency);
        stats.first = stats.last = now;
    }
    ++stats.calls;
    ++stats.interval_calls;
    const double first = ctx->fpr[1], second = ctx->fpr[2];
    if (std::isfinite(first) && std::isfinite(second)) {
        stats.total_first += first;
        stats.total_second += second;
        stats.window_first += first;
        stats.window_second += second;
    } else {
        ++stats.invalid_values;
    }
    if (now.QuadPart - stats.last.QuadPart < stats.frequency.QuadPart) return;
    const double wall = (double)(now.QuadPart - stats.first.QuadPart) / stats.frequency.QuadPart;
    const double span = (double)(now.QuadPart - stats.last.QuadPart) / stats.frequency.QuadPart;
    Gt6FrameTimingSample sample;
    const bool valid = gt6_frame_timing_sample(sample);
    /* These are the unmodified arguments to 001F90FC, not asserted physics
     * seconds. Their sums / wall time can reveal acceleration when compared
     * with a baseline, and must be checked against the visible race clock. */
    fprintf(stderr, "[gt6-frame-time] qpc=%lld tid=%u wall=%.6f span=%.6f calls=%llu "
            "calls_window=%llu arg1=%.9g arg2=%.9g sum1=%.9g sum2=%.9g "
            "window1=%.9g window2=%.9g invalid=%llu root=%08X valid=%u "
            "interval=%d vblank=%llu previous_vblank=%llu timer_us=%llu\n",
            (long long)now.QuadPart, (unsigned)ctx->thread_id, wall, span,
            (unsigned long long)stats.calls, (unsigned long long)stats.interval_calls,
            first, second, stats.total_first, stats.total_second,
            stats.window_first, stats.window_second, (unsigned long long)stats.invalid_values,
            sample.root, (unsigned)valid, sample.interval,
            (unsigned long long)sample.vblank, (unsigned long long)sample.previous_vblank,
            (unsigned long long)sample.timer_us);
    stats.last = now;
    stats.interval_calls = 0;
    stats.window_first = stats.window_second = 0;
}
