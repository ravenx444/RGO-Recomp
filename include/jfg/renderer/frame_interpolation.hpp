#pragma once

// RGO frame interpolation: the pure decisions shared by the renderer shell,
// the RT64 frame-grouping patch, the game hooks and their tests. See
// docs/rgo/framerate.md.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jfg/renderer/rt64_frame_grouping.hpp"
#include "jfg/renderer/video_settings.hpp"

namespace jfg {
inline constexpr std::uint32_t pause_menu_animation_ticks(std::uint32_t ticks,bool paused) noexcept {
  return paused && ticks<=6U ? ticks*3U : ticks;
}


// RT64's refresh-rate configuration for a Frame rate setting.
enum class Rt64RefreshMode : std::uint8_t { original, display, manual };

struct Rt64RefreshPlan {
    Rt64RefreshMode mode = Rt64RefreshMode::original;
    int target = 60;
    friend bool operator==(const Rt64RefreshPlan&, const Rt64RefreshPlan&) = default;
};

// RT64 blends frames only above the game's own rate: with true 60 fps logic
// that means Display or Unlocked, and only when "Interpolate extra frames"
// is on.
[[nodiscard]] constexpr bool interpolation_requested(const VideoSettings& settings) noexcept {
    return settings.interpolate_frames &&
        (settings.framerate == FrameRate::display || settings.framerate == FrameRate::unlocked);
}

[[nodiscard]] constexpr Rt64RefreshPlan refresh_plan(const VideoSettings& settings) noexcept {
    if (!interpolation_requested(settings))
        return {};
    return settings.framerate == FrameRate::unlocked
        ? Rt64RefreshPlan{Rt64RefreshMode::manual, kUnlockedRefreshTarget}
        : Rt64RefreshPlan{Rt64RefreshMode::display, 60};
}

// The fixed True 60 preset may trade cadence for a steady 30 under load.
// Display and Unlocked request variable delivery, so they retain one-VI
// scheduling and let the wall-clock floor account for actual missed frames.
[[nodiscard]] constexpr bool true60_uses_backoff(const VideoSettings& settings) noexcept {
    return settings.framerate == FrameRate::sixty;
}

// True 60 fps logic: around every viFrameSync (US 0x80054FBC, gameVi.c) the
// hook sets the game's own runInOneFrame flag (0x800FECCC), which makes it
// wait for one VI per frame and pass updateRate 1 to every tick function.
inline constexpr std::uint32_t kViFrameSync = 0x80054FBCU;
inline constexpr std::uint32_t kRunInOneFrame = 0x800FECCCU;
// gVideoDeltaTime (u8): the VIs viFrameSync waits for, 1 / 2 / 3.
inline constexpr std::uint32_t kVideoDeltaTime = 0x800FECACU;
// gSkipVideoDeltaAdjust (u8): while set, a frame that took three or more
// VIs does not switch the game to its 20 fps mode (viFrameSync clears it).
// The game sets it around loads (viFrameRateReset).
inline constexpr std::uint32_t kSkipVideoDeltaAdjust = 0x800FECADU;

// runInOneFrame alone is not enough: the game's scheduler holds the frame's
// "graphics done" message until two VIs have passed since the last one
// (__scTaskComplete, US 0x80050A08, sched.c: "if (sc->frameCount <= 1) defer
// to the next retrace"), and the main loop waits for that message, so frames
// still took two VIs. True 60 raises sc->frameCount (OSSched + 0x300) to 2
// before the call, so the message goes out as soon as the frame is drawn.
inline constexpr std::uint32_t kScTaskComplete = 0x80050A08U;
inline constexpr std::uint32_t kSchedFrameCountOffset = 0x300U;

[[nodiscard]] constexpr std::uint32_t true60_frame_count(
    const std::uint32_t frame_count) noexcept {
    return frame_count < 2U ? 2U : frame_count;
}

// True 60 needs the host to finish every frame within one VI. When it cannot
// (a slow PC, a heavy scene), frames take one, two, three VIs in turn, which
// plays at the right speed but judders. Like the game's own 30/20 fps
// switch, the governor then hands the game back its steady 30.
//
// It decides when to go back to 60 from evidence: every frame reports how
// long the host was busy with it (wall time minus the time spent waiting for
// the next VI). A frame at 30 runs the same game step and draws the same
// picture as a frame at 60, so its busy time is about what a 60 frame costs
// (a little more: it also carries a second VI's audio work). After a
// minimum time at 30 (2 s, doubling after each try that fails within 10 s,
// up to 30 s), it looks at the busy time 90 % of the last 60 frames stayed
// within:
// - at most 15 ms: 60 fits, resume now;
// - at most 20 ms: borderline (a frame at 60 misses its VI after about
//   21 ms), try 60 again at twice the minimum time;
// - more: a 60 frame cannot fit, stay at a steady 30. The old governor tried
//   60 every 4-30 s here, and each failed try was one to two seconds of
//   frames of one, two and three VIs, which read as 20-something on the
//   counter.
// Without busy times (0) it retries when the minimum time has passed.
class True60Governor {
public:
    static constexpr std::uint32_t kWindow = 60U;       // frames
    static constexpr std::uint32_t kLateLimit = 6U;     // late frames in the last 60
    static constexpr std::uint64_t kFirstPause = 120U;  // VIs (2 s) at 30, at least
    static constexpr std::uint64_t kLongestPause = 1800U;
    static constexpr std::uint64_t kSettled = 600U;     // VIs at 60 that reset the wait
    // Busy time of the slowest tenth of the last 60 frames at 30: up to
    // kResumeBusyUs 60 resumes after the minimum time, up to kRetryBusyUs it
    // is tried again at twice the minimum time, above it stays at 30.
    static constexpr std::uint32_t kResumeBusyUs = 15000U;
    static constexpr std::uint32_t kRetryBusyUs = 20000U;
    // A scene that has become light (busy up to kLightBusyUs at 30) resumes
    // after kFirstPause however long the back-off has grown: the back-off
    // is for heavy scenes, and holding 30 for up to 30 s after one is what
    // reads as "locked at 30".
    static constexpr std::uint32_t kLightBusyUs = 12000U;

    // true: run this frame at true 60.
    [[nodiscard]] constexpr bool active(const std::uint64_t vi) const noexcept {
        static_cast<void>(vi);
        return !paused_;
    }

    // A frame took this many VIs (viFrameSync's return value) and kept the
    // host busy for busy_us microseconds (0: not measured). Report every
    // frame, at 60 and while holding 30.
    constexpr void frame(const std::uint32_t vis, const std::uint64_t vi,
                         const std::uint32_t busy_us = 0U) noexcept {
        busy_[busy_next_] = busy_us;
        busy_next_ = (busy_next_ + 1U) % kWindow;
        if (busy_count_ < kWindow)
            ++busy_count_;
        if (paused_) {
            const std::uint64_t waited = vi - pause_vi_;
            const bool measured = busy_count_ >= kWindow && busy_us != 0U;
            if (waited < pause_ &&
                !(waited >= kFirstPause && measured && busy_p90() <= kLightBusyUs))
                return;
            const std::uint32_t busy = measured ? busy_p90() : 0U;
            const bool resume = !measured || busy <= kResumeBusyUs ||
                                (busy <= kRetryBusyUs && waited >= 2U * pause_);
            if (resume) {
                paused_ = false;
                resume_vi_ = vi;
                late_ = 0U;
            }
            return;
        }
        late_ = (late_ << 1U) | (vis > 1U ? 1U : 0U);
        if (pause_ != 0U && vi >= resume_vi_ + kSettled)
            pause_ = 0U;  // held 60 long enough: the next pause is short again
        // Seven late frames are enough to give up, without waiting for a
        // full window: on a slow PC a heavy scene would otherwise judder at
        // 10-20 fps for several seconds before falling back.
        if (late_frames() <= kLateLimit)
            return;
        pause_ = pause_ == 0U ? kFirstPause : std::min(pause_ * 2U, kLongestPause);
        paused_ = true;
        pause_vi_ = vi;
        busy_count_ = 0U;  // judge resuming on frames at 30 only
        late_ = 0U;
    }

    // A new level or cutscene shot: forget the back-off and try 60 at once.
    // Without this, a heavy cutscene that paused for 30 s would keep the
    // light gameplay after it at 30 for most of that time.
    constexpr void new_scene(const std::uint64_t vi) noexcept {
        late_ = 0U;
        pause_ = 0U;
        paused_ = false;
        resume_vi_ = vi;
        busy_count_ = 0U;
    }

    [[nodiscard]] constexpr std::uint32_t late_frames() const noexcept {
        std::uint32_t count = 0U;
        for (std::uint32_t bit = 0U; bit < kWindow; ++bit)
            count += static_cast<std::uint32_t>((late_ >> bit) & 1U);
        return count;
    }

    // The busy time 90 % of the last frames (up to 60) stayed within, in
    // microseconds (0: none measured).
    [[nodiscard]] constexpr std::uint32_t busy_p90() const noexcept {
        std::array<std::uint32_t, kWindow> sorted{};
        for (std::uint32_t index = 0U; index < busy_count_; ++index)
            sorted[index] = busy_[(busy_next_ + kWindow - 1U - index) % kWindow];
        if (busy_count_ == 0U)
            return 0U;
        std::sort(sorted.begin(), sorted.begin() + busy_count_);
        return sorted[(busy_count_ * 9U) / 10U - (busy_count_ >= 10U ? 1U : 0U)];
    }

    // Holding 30 (for the FPS counter's Detailed view).
    [[nodiscard]] constexpr bool holding_30() const noexcept { return paused_; }

private:
    std::uint64_t late_ = 0U;
    std::uint64_t pause_ = 0U;      // minimum time at 30 for the current pause
    std::uint64_t pause_vi_ = 0U;   // the VI the current pause started at
    std::uint64_t resume_vi_ = 0U;  // the VI 60 last resumed at
    bool paused_ = false;
    std::array<std::uint32_t, kWindow> busy_{};
    std::uint32_t busy_next_ = 0U;
    std::uint32_t busy_count_ = 0U;
};

// The level being played (a cutscene is a level too); the governor starts
// over when it changes.
inline constexpr std::uint32_t kCurrentLevel = 0x800FB114U;

// The engine passes the VIs per frame (delayDat, 0x800A3374) to its tick
// functions, so almost all logic is already rate-independent. These places
// assume at least 2 ticks per frame (docs/rgo/framerate.md, "True 60"):
inline constexpr std::uint32_t kDelayDat = 0x800A3374U;

// 1. Overlay 47 animseqUpdateTextureScrollers(rate) computes
//    (velocity * rate) >> 8, which rounds slow scrollers to zero at rate 1.
//    True 60 batches its ticks and runs it once per 2 ticks, which gives it
//    exactly its original 30 fps inputs.
inline constexpr std::uint32_t kAnimseqUpdateTextureScrollers = 0x02F00000U;

// Ticks to run now (0: skip this frame and keep them pending).
[[nodiscard]] constexpr std::uint32_t batch_ticks(
    std::uint32_t& pending, const std::uint32_t rate,
    const std::uint32_t minimum = 2U) noexcept {
    pending += rate;
    if (pending < minimum)
        return 0U;
    const std::uint32_t run = pending;
    pending = 0U;
    return run;
}

// 2. partUpdateTriggers(object, ticks) is called with a literal 2 by some
//    effects (jet flames, droplets, bubbles). With one-VI frames that would
//    double their particles, so a literal 2 becomes the frame's real rate.
inline constexpr std::uint32_t kPartUpdateTriggers = 0x8005F5ACU;

[[nodiscard]] constexpr std::uint32_t trigger_ticks(
    const std::uint32_t requested, const std::uint32_t frame_rate) noexcept {
    return requested == 2U && frame_rate == 1U ? 1U : requested;
}

// 3. joyRead recomputes "pressed" from the last two controller reads every
//    frame. A frame without a new read (possible once frames are one VI)
//    would report the same press again, so its pressed/released masks are
//    cleared when the call left both reads unchanged.
inline constexpr std::uint32_t kJoyRead = 0x80043130U;
inline constexpr std::uint32_t kControllerCurrent = 0x800FB0C0U;   // OSContPad[4]
inline constexpr std::uint32_t kControllerPrevious = 0x800FB0D8U;  // OSContPad[4]
inline constexpr std::uint32_t kControllerPressed = 0x800FB0F0U;   // u16[4]
inline constexpr std::uint32_t kControllerReleased = 0x800FB0F8U;  // u16[4]
inline constexpr std::size_t kControllerPadBytes = 24U;            // 4 x 6

[[nodiscard]] constexpr bool controller_read_consumed(
    const std::array<std::uint8_t, 48>& before,
    const std::array<std::uint8_t, 48>& after) noexcept {
    for (std::size_t index = 0U; index < before.size(); ++index)
        if (before[index] != after[index])
            return true;
    return false;
}

// 4. The bug enemies ("squaddies", overlay 3, the squads AI) count in calls,
//    not ticks: SquaddieControl moves by its velocity, accelerates, applies
//    drag and gravity, and counts its fire delay and other timers by one per
//    call; SquadronControl counts its reinforcement timer per call;
//    squadsModuleTick ages the AI's remembered noises per call and flips an
//    every-other-update flag. At one VI per frame bugs ran, fired, tumbled
//    and spawned twice as fast, while their walk animation (which does use
//    the rate) stayed at normal speed. True 60 runs the whole squads AI
//    together once per 2 ticks with rate 2, exactly its 30 fps inputs; bug
//    positions then update 30 times a second, as on the console. The rate
//    argument is register a1 (a0 for squadsModuleTick); only calls that pass
//    the frame's rate (delayDat) are batched.
inline constexpr std::uint32_t kSquaddieControl = 0x00310600U;
inline constexpr std::uint32_t kSquadronControl = 0x00314D4CU;
inline constexpr std::uint32_t kAiPointControl = 0x0030A110U;
inline constexpr std::uint32_t kAiJunctionControl = 0x00316CB4U;
inline constexpr std::uint32_t kSquadsModuleTick = 0x00316990U;

// 5. Other code that counts per call (docs/rgo/framerate.md, "True 60
//    audit"):
//    - splashControl (0x80028034, spranim.c) spawns three droplets per call
//      until its count runs out: the splash came out in half the time.
//    - wakeUpdateRipple (0x8006B090, fx) moves the ripple's fade by 32 and,
//      through wakeUpdate, the wake's fade by 64 and pushes one trail segment
//      per call into a ring sized for one segment per two VIs: wakes behind
//      wading characters and the hovership were half as long and faded twice
//      as fast.
//    Both are void, take the rate in a1 and run at half rate (below).
inline constexpr std::uint32_t kSplashControl = 0x80028034U;
inline constexpr std::uint32_t kWakeUpdateRipple = 0x8006B090U;

//    Three more count per call but must run every frame, so on a frame the
//    half-rate functions skip (an "odd tick") they run with their per-call
//    step undone:
//    - controlUpdatePlayerAim (0x80039FCC, a1 = the player's control state)
//      counts down the aim stance hold (+0x1F4, s8) and the fire spin-up
//      (+0x1F8, s8) and the lock-on beep nibble (low 4 bits of +0x541) by one
//      per call: the aim stance dropped after 0.25 s instead of 0.5 s. Its
//      aim angles must still update at 60, so the counters are raised by one
//      before the call instead.
//    - ShirukenControl (overlay 122, 0x07A004EC) spins a flying shuriken by
//      0x1D6B (state +0x78) and tumbles a bounced one by its spin rates
//      (state +0x7A..+0x7E into the object's rotation) per call; its motion
//      does scale by the rate. The spin and tumble steps are taken back.
//    - camlightVisibilityCheck (0x80024044, from trackDraw, no rate) fades
//      lens flares by 8 per call and walks point lights round-robin from a
//      cursor (0x800A110C). The 50 fade bytes and the cursor are restored;
//      the z-buffer checks it queues still run every frame.
inline constexpr std::uint32_t kControlUpdatePlayerAim = 0x80039FCCU;
inline constexpr std::uint32_t kShirukenControl = 0x07A004ECU;
inline constexpr std::uint32_t kCamlightVisibilityCheck = 0x80024044U;
inline constexpr std::uint32_t kCamlightCursor = 0x800A110CU;
inline constexpr std::uint32_t kCamlights = 0x800F5B60U;
inline constexpr std::uint32_t kCamlightCount = 50U;
inline constexpr std::uint32_t kCamlightStride = 0x50U;
inline constexpr std::uint32_t kCamlightFadeOffset = 7U;
inline constexpr std::uint16_t kShirukenSpinStep = 0x1D6BU;

//    - Two object controls step one counter in their state (*(object+0x68))
//      by one per call; a step of exactly one is taken back:
//      hoovertankControl (overlay 106, 0x06A00290) ramps its spin speed
//      (+0x35, s8) up or down by one per call, so it spun up and down in half
//      the time; FlowerBoltControl (overlay 149, the slug boss's bolt,
//      0x09501450) counts its arming delay (+0x10, s16) per call.
struct StateCounter {
    std::uint32_t address = 0U;
    std::uint32_t offset = 0U;  // in the object's state (*(object + 0x68))
    std::uint8_t bytes = 1U;    // 1 or 2
};
inline constexpr std::array<StateCounter, 2> kStateCounters{{
    {0x06A00290U, 0x35U, 1U},  // hoovertankControl
    {0x09501450U, 0x10U, 2U},  // FlowerBoltControl
}};

[[nodiscard]] constexpr const StateCounter* state_counter(const std::uint32_t target) noexcept {
    for (const StateCounter& entry : kStateCounters)
        if (entry.address == target)
            return &entry;
    return nullptr;
}

// Whether a counter of `bytes` bytes moved by exactly one (up or down).
[[nodiscard]] constexpr bool counter_stepped(const std::uint32_t before,
                                             const std::uint32_t after,
                                             const std::uint8_t bytes) noexcept {
    const std::uint32_t mask = bytes == 1U ? 0xFFU : 0xFFFFU;
    const std::uint32_t delta = (after - before) & mask;
    return delta == 1U || delta == mask;
}

[[nodiscard]] constexpr bool odd_tick_compensated(const std::uint32_t target) noexcept {
    return target == kControlUpdatePlayerAim || target == kShirukenControl ||
           target == kCamlightVisibilityCheck || state_counter(target) != nullptr;
}

// controlUpdatePlayerAim's counters, raised so its per-call countdown leaves
// them unchanged: it decrements the hold and the spin-up (clamped at 0) when
// the hold is set, and the lock-on nibble when it is set.
struct AimCounters {
    std::int8_t hold = 0;
    std::int8_t spin_up = 0;
    std::uint8_t lock = 0;  // the byte at +0x541; low nibble counts down
    friend bool operator==(const AimCounters&, const AimCounters&) = default;
};

[[nodiscard]] constexpr AimCounters aim_counters_for_odd_tick(AimCounters counters) noexcept {
    if (counters.hold != 0 && counters.hold < 127) {
        ++counters.hold;
        if (counters.spin_up > 0 && counters.spin_up < 127)
            ++counters.spin_up;
    }
    const std::uint8_t nibble = counters.lock & 0x0FU;
    if (nibble != 0U && nibble < 15U)
        counters.lock = static_cast<std::uint8_t>((counters.lock & 0xF0U) | (nibble + 1U));
    return counters;
}

// Whether a 16-bit angle moved by exactly `step` (mod 65536) in one call.
[[nodiscard]] constexpr bool angle_stepped(const std::uint16_t before,
                                           const std::uint16_t after,
                                           const std::uint16_t step) noexcept {
    return static_cast<std::uint16_t>(after - before) == step;
}

// Functions True 60 runs once per 2 ticks with rate 2 ("half rate"): the
// game code that counts per call. All of them share one cadence, so code
// that runs together (the squads AI) stays in step. `rate_register` is the
// argument register holding the frame's rate (4 = a0, 5 = a1); only calls
// that pass the frame's rate are batched, and calls whose return address lies
// in [setup_begin, setup_end) (level setup) run as they are.
struct HalfRateFunction {
    std::uint32_t address = 0U;
    std::uint8_t rate_register = 5U;
    std::uint32_t setup_begin = 0U;
    std::uint32_t setup_end = 0U;
};

// squadsInitialiseAfterObjects (0x80051EA4-0x80052677) runs SquaddieControl
// once per bug while a level is set up.
inline constexpr std::uint32_t kSquadsSetupBegin = 0x80051EA4U;
inline constexpr std::uint32_t kSquadsSetupEnd = 0x80052678U;

inline constexpr std::array<HalfRateFunction, 7> kHalfRateFunctions{{
    {kSquaddieControl, 5U, kSquadsSetupBegin, kSquadsSetupEnd},
    {kSquadronControl, 5U, kSquadsSetupBegin, kSquadsSetupEnd},
    {kAiPointControl, 5U, kSquadsSetupBegin, kSquadsSetupEnd},
    {kAiJunctionControl, 5U, kSquadsSetupBegin, kSquadsSetupEnd},
    {kSquadsModuleTick, 4U, kSquadsSetupBegin, kSquadsSetupEnd},
    {kSplashControl, 5U},
    {kWakeUpdateRipple, 5U},
}};

// The half-rate entry for a function, or nullptr.
[[nodiscard]] constexpr const HalfRateFunction* half_rate_function(
    const std::uint32_t target) noexcept {
    for (const HalfRateFunction& entry : kHalfRateFunctions)
        if (entry.address == target)
            return &entry;
    return nullptr;
}

[[nodiscard]] constexpr bool is_half_rate_function(const std::uint32_t target) noexcept {
    return half_rate_function(target) != nullptr;
}

// Whether a call to a half-rate function is batched: it passes the frame's
// rate and does not come from the function's setup caller.
[[nodiscard]] constexpr bool half_rate_call(const HalfRateFunction& entry,
                                            const std::uint32_t rate_argument,
                                            const std::uint32_t frame_rate,
                                            const std::uint32_t return_address) noexcept {
    return rate_argument == frame_rate &&
           !(return_address >= entry.setup_begin && return_address < entry.setup_end);
}

// The game's logic rate for one game frame: 60 / the VIs it took (2 normally,
// 3 after slow frames). Long stalls (loading) are clamped to 10 per second.
[[nodiscard]] constexpr std::uint16_t game_rate_from_retraces(
    const std::uint64_t retraces) noexcept {
    if (retraces == 0U)
        return 60U;
    return static_cast<std::uint16_t>(60U / std::clamp<std::uint64_t>(retraces, 1U, 6U));
}

// Interpolation IDs. 0 is RT64's "ignore" and 0xFFFFFFFF its "auto".
inline constexpr std::uint32_t kCameraIdBase = 0x00000100U;
inline constexpr std::uint32_t kObjectIdBase = 0x01000000U;

// One ID per camera (slot 0 load) of a frame: its kind and its order among
// the frame's cameras of that kind. Off-screen targets use their own range.
[[nodiscard]] constexpr std::uint32_t camera_group_id(
    const bool orthographic, const bool screen_target,
    const std::uint32_t ordinal) noexcept {
    return kCameraIdBase + (screen_target ? 0U : 0x8000U) +
        (orthographic ? 0x4000U : 0U) + (std::min)(ordinal, 0x3FFFU);
}

// Object matrices: the game's transform pointer (stable for an object's
// lifetime) and how many matrices that transform pushed before this one in
// the frame. Never 0 or 0xFFFFFFFF, never in the camera range.
[[nodiscard]] constexpr std::uint32_t object_group_id(
    const std::uint32_t transform_address, const std::uint32_t push_index) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const std::uint32_t word : {transform_address, push_index}) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            hash ^= (word >> shift) & 0xFFU;
            hash *= 16777619U;
        }
    }
    return (hash | kObjectIdBase) & 0x7FFFFFFFU;
}

// Bone matrices ("multiply" loads) follow the last tagged object matrix.
[[nodiscard]] constexpr std::uint32_t bone_group_id(
    const std::uint32_t object_id, const std::uint32_t bone_index) noexcept {
    return object_id == 0U ? 0U
        : ((object_id ^ ((bone_index + 1U) * 0x9E3779B1U)) | kObjectIdBase) & 0x7FFFFFFFU;
}

// Content check for object tags: the game hook hashes the 16 words of the
// fixed-point matrix it saw written, and the shell accepts a tag only for a
// matrix with the same words. Stale tags for reused matrix memory are
// rejected without knowing the game's exact frame boundaries.
[[nodiscard]] constexpr std::uint32_t matrix_words_hash(
    const std::array<std::uint32_t, 16>& words) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const std::uint32_t word : words) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            hash ^= (word >> shift) & 0xFFU;
            hash *= 16777619U;
        }
    }
    return hash;
}

// Game functions that write object matrices (US addresses, camera.c). Each
// writes one F3DDKR slot-1 matrix at the Mtx** cursor in a1.
inline constexpr std::uint32_t kCamSetProjMtx = 0x80040AA4U;
inline constexpr std::uint32_t kCamPushFloatModelMtx = 0x800419ACU;
inline constexpr std::uint32_t kCamPushMuzzleMtx = 0x80041A90U;
inline constexpr std::uint32_t kCamPushModelMtx = 0x80041CA0U;

// Object key for a hooked call: the stable per-object pointer in a2 (the
// object transform, float matrix or muzzle position) and which function.
[[nodiscard]] constexpr std::uint32_t object_key(
    const std::uint32_t function, const std::uint32_t pointer) noexcept {
    return pointer ^ (function == kCamPushFloatModelMtx ? 0x40000000U
                      : function == kCamPushMuzzleMtx   ? 0x20000000U
                                                        : 0U);
}

// ---------------------------------------------------------------------------
// Camera cuts. Blending across a cut shows one frame between two unrelated
// views, so the shell snaps instead.

struct CameraPose {
    std::array<float, 3> position{};
    std::array<float, 3> forward{};  // unit vector
    float focal = 0.0F;              // projection [1][1] (vertical FOV scale)
    bool valid = false;
};

// Pose from a row-vector view matrix (world -> camera) and the projection's
// [1][1] element. Returns an invalid pose for a degenerate view.
[[nodiscard]] inline CameraPose camera_pose_from_view(
    const std::array<std::array<float, 4>, 4>& view, const float focal) noexcept {
    // Rows 0-2, columns 0-2 hold the rotation (and any uniform scale); row 3
    // the translation. The camera sits where p * R + t = 0: p = -t * R^-1.
    const auto& m = view;
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    CameraPose pose;
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return pose;
    // Inverse of the 3x3 (adjugate / det).
    const double inv[3][3] = {
        {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
        {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
        {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
    for (std::size_t column = 0U; column < 3U; ++column) {
        double value = 0.0;
        for (std::size_t row = 0U; row < 3U; ++row)
            value -= double(m[3][row]) * inv[row][column];
        pose.position[column] = static_cast<float>(value);
    }
    // The camera's view axis in world space is the rotation's third column.
    const double length = std::sqrt(c * c + f * f + i * i);
    if (!(length > 0.0))
        return pose;
    pose.forward = {static_cast<float>(c / length), static_cast<float>(f / length),
                    static_cast<float>(i / length)};
    pose.focal = focal;
    pose.valid = std::isfinite(pose.position[0]) && std::isfinite(pose.position[1]) &&
        std::isfinite(pose.position[2]);
    return pose;
}

inline constexpr float kCutMinimumDistance = 400.0F;    // world units
inline constexpr float kCutMotionRatio = 4.0F;          // vs the previous step
inline constexpr float kCutMaximumTurnCosine = 0.7071F; // 45 degrees
inline constexpr float kCutMaximumFocalChange = 0.25F;  // 25 % FOV change

// True when the camera jumped between the previous and the current game
// frame: far beyond its previous step, a sharp turn, or a field-of-view jump.
// before_previous may be invalid (then the jump alone decides).
[[nodiscard]] inline bool camera_cut(
    const CameraPose& before_previous, const CameraPose& previous,
    const CameraPose& current) noexcept {
    if (!previous.valid || !current.valid)
        return true;
    auto distance = [](const CameraPose& x, const CameraPose& y) {
        const float dx = x.position[0] - y.position[0];
        const float dy = x.position[1] - y.position[1];
        const float dz = x.position[2] - y.position[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const float moved = distance(previous, current);
    const float step = before_previous.valid ? distance(before_previous, previous) : 0.0F;
    if (moved > kCutMinimumDistance && moved > kCutMotionRatio * step + 100.0F)
        return true;
    const float cosine = previous.forward[0] * current.forward[0] +
        previous.forward[1] * current.forward[1] + previous.forward[2] * current.forward[2];
    if (cosine < kCutMaximumTurnCosine)
        return true;
    if (previous.focal != 0.0F &&
        std::abs(current.focal / previous.focal - 1.0F) > kCutMaximumFocalChange)
        return true;
    return false;
}

}  // namespace jfg
