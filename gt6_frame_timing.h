/* Frame pacing for EMAIN 1.22. Included only by gt6_host.cpp, after
 * ppu_context and the guarded peek helpers.
 *
 * The game presents every VBlank in a race and every second one in a replay,
 * in the scenes before and after a race and in its B-Spec view: its frame
 * interval (graphics root + 0x1C) is 1 or 2, and the wait at 0x008A8A44 asks
 * for "the VBlank of the last frame + the interval".
 *
 * [GT6 0133, 2026-11-25] Every VBlank everywhere (B-Spec, the replay and
 * the scene before the first drive were locked to 30 fps - all of them are
 * unlocked to 60). Only the wait's target changes, to "+ 1"; the shared interval,
 * the 60 Hz VBlank clock and guest time are untouched. That is enough: the
 * update takes the time that really passed (the first argument of 001F90FC
 * is the measured frame time, GT6_FRAME_TIMING_TRACE: 60 calls a second
 * adding up to 1.01 s of game time a second in a replay with the target
 * moved, 27 calls adding up to the same without). Test drive ind1 (a
 * 12-car race at Tsukuba, then its replay): the replay ran at 37 .. 60 fps
 * instead of 30, at the right speed, for as long as the run lasted.
 * It was opt-in from 2026-11-17 ("until real race/replay timing is
 * verified"); an early try then met a wait in the SPU job queue that never
 * ended (docs\OPTIMIZATION_20261117.md) - ind1 showed none in 330 s. Why
 * that wait is gone was not looked into.
 *
 * [GT6 0145, 2026-11-25] That was not enough: see gt6_scene_interval below.
 * The race scene's own choice of the interval is answered with 1 now; the
 * moved wait target stays for scenes whose interval comes from elsewhere.
 *
 * GT6_RENDER_INTERVAL=2: the game's own pace (off). =1: only the wait's
 * target, as [GT6 0133] was. =0: no VBlank wait at all (tests).
 * GT6_FRAME_TIMING_TRACE=1 observes raw update arguments once/sec.
 */
#pragma once
#include <cmath>
#include <cstring>

namespace {
struct Gt6FrameTimingConfig {
    int interval;
    bool trace;
    bool own;   /* [GT6 0145] the game's own choice of 2 becomes 1 (gt6_scene_interval); off with =1, =0, =2 */
    Gt6FrameTimingConfig() : interval(1), trace(false), own(true) {
        const char* value = getenv("GT6_RENDER_INTERVAL");
        if (value && (value[0] == '0' || value[0] == '1') && value[1] == 0) {
            interval = value[0] - '0';
            own = false;     /* =1: only the wait's target, as [GT6 0133] was */
        } else if (value && value[0] == '2' && value[1] == 0) {
            interval = -1;   /* the game's own pace */
            own = false;
        }
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

/* [GT6 0145] The game's own choice of the scene's frame interval, at the one place where it is made (func_001F90FC,
 * 0x001F9468 / 0x001F9470; tools\post_lift.py routes both values through here): 1 in a race, 2 in a replay, in the
 * scenes before and after a race and under the replay camera. Everything of the game reads the word this value is
 * stored in (graphics root + 0x1C). With only the wait's target moved ([GT6 0133]) the game went on taking such a
 * scene for a half-rate one: its 2D layer - the panels and the menu row before a race, the result panels - was
 * drawn in some frames only, in runs seconds long (shown 2026-11-25 by an A/B of the scene before a race: steady
 * with the game's own pace, gone in most frames with the target moved; the draw records of the frames without it
 * hold no draw of the 2D layer at all).
 * Now the choice itself becomes 1, so every reader agrees (the default; GT6_RENDER_INTERVAL=1 = only the wait's
 * target as [GT6 0133] was, =2 = the game's own pace). Measured 2026-11-25, Special Stage Route 7 at night, 16 cars,
 * 1080p real resolution, hidden runs: the scene before a race 60.0 fps for 250 s with its panels in every picture
 * (27 .. 42 fps with the target moved, 29 at the game's own pace); the replay camera while driving 46 .. 60 fps
 * (20 .. 38); the results panel and the replay's own display in every picture; game time 0.96 .. 1.02 s a second.
 * What the game chose is kept for code that must know that a scene is a replay (gt6_scene_interval_game). */
static volatile LONG g_gt6_scene_interval_game = 0;
extern "C" int gt6_scene_interval(int game)
{
    InterlockedExchange(&g_gt6_scene_interval_game, game);
    if (game == 2 && gt6_frame_timing_config().own) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr, "[gt6-render-interval] the game chooses a frame interval of 2 here (a replay, the scene "
                    "before or after a race, the replay camera): answered 1, so its own word says every VBlank\n");
        }
        return 1;
    }
    return game;
}
extern "C" int gt6_scene_interval_game(void) { return (int)g_gt6_scene_interval_game; }

extern "C" void gt6_render_wait_target(ppu_context* ctx)
{
    const Gt6FrameTimingConfig& config = gt6_frame_timing_config();
    gt6_watch_write_tick();   /* (test tool, once a frame; nothing unless GT6_WATCH_WRITE is set) */
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
        fprintf(stderr, "[gt6-render-interval] the game asks for every %d. VBlank here (a replay, "
                "the scene before or after a race): presented every %d. instead; its interval and "
                "the VBlank clock unchanged (GT6_RENDER_INTERVAL=2 = the game's own pace)\n",
                sample.interval, config.interval);
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
