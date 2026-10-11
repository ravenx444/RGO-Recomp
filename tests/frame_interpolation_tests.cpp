// RGO frame interpolation: rate mapping, IDs, frame grouping and camera cuts.
#include "jfg/renderer/frame_interpolation.hpp"
#include "jfg/renderer/frame_stats.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

using Matrix = std::array<std::array<float, 4>, 4>;

// Row-vector view matrix for a camera at `eye` turned `yaw` radians about Y.
Matrix view(const float x, const float y, const float z, const float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    // World -> camera rotation is the transpose of the camera's orientation.
    Matrix m{};
    m[0] = {c, 0.0F, s, 0.0F};
    m[1] = {0.0F, 1.0F, 0.0F, 0.0F};
    m[2] = {-s, 0.0F, c, 0.0F};
    // t = -eye * R
    m[3] = {-(x * c + z * -s), -y, -(x * s + z * c), 1.0F};
    return m;
}

bool near(const float a, const float b, const float tolerance = 1e-3F) {
    return std::abs(a - b) < tolerance;
}

}  // namespace

int main() {
    using namespace jfg;

    // Frame rate setting -> RT64 refresh configuration. Blending only above
    // the game's own (true 60) rate, and only with Interpolate extra frames.
    auto with = [](FrameRate rate, bool interpolate) {
        VideoSettings settings;
        settings.framerate = rate;
        settings.interpolate_frames = interpolate;
        return settings;
    };
    require(refresh_plan(with(FrameRate::original, true)) == Rt64RefreshPlan{}, "original");
    require(refresh_plan(with(FrameRate::sixty, true)) == Rt64RefreshPlan{},
        "true 60 needs no blending");
    require(refresh_plan(with(FrameRate::display, true)).mode == Rt64RefreshMode::display,
        "display");
    require(refresh_plan(with(FrameRate::unlocked, true)) ==
                Rt64RefreshPlan{Rt64RefreshMode::manual, kUnlockedRefreshTarget},
        "unlocked");
    require(refresh_plan(with(FrameRate::display, false)) == Rt64RefreshPlan{} &&
                !interpolation_requested(with(FrameRate::unlocked, false)),
        "interpolation off: true 60 only");
    require(!interpolation_requested(with(FrameRate::original, true)) &&
                !interpolation_requested(with(FrameRate::sixty, true)) &&
                interpolation_requested(with(FrameRate::display, true)),
        "interpolation only above true 60");
    require(!true_logic_60(with(FrameRate::original, true)) &&
                true_logic_60(with(FrameRate::sixty, false)) &&
                true_logic_60(with(FrameRate::unlocked, true)),
        "true 60 logic above original");
    {
        VideoSettings slow = with(FrameRate::sixty, true);
        slow.n64_slowdown = true;
        require(fast_clock(slow) && fast_clock(with(FrameRate::original, true)),
            "true 60 always uses the fast clock");
        slow.framerate = FrameRate::original;
        require(!fast_clock(slow), "slowdown applies at the Original frame rate");
    }
    // The true 60 governor: occasional late frames (a level load) keep 60;
    // a host that keeps missing VIs gets the game's steady 30 until its busy
    // time per frame shows it can hold 60 again.
    {
        using G = True60Governor;
        constexpr std::uint32_t fast = 9000U, borderline = 17000U, slow = 25000U;
        // Frames of `vis` VIs and `busy` microseconds; returns the frames run.
        const auto run = [](G& governor, std::uint64_t& vi, int frames, std::uint32_t vis,
                            std::uint32_t busy) {
            for (int frame = 0; frame < frames; ++frame, vi += vis)
                governor.frame(vis, vi, busy);
        };
        G governor;
        std::uint64_t vi = 1000U;
        for (int frame = 0; frame < 120; ++frame, ++vi)
            governor.frame(frame % 20 == 0 ? 6U : 1U, vi, fast);
        require(governor.active(vi) && !governor.holding_30(), "a few late frames keep true 60");
        run(governor, vi, 60, 1U, fast);  // a clean window
        run(governor, vi, 6, 2U, slow);
        require(governor.active(vi), "six late frames keep 60");
        run(governor, vi, 1, 2U, slow);
        require(!governor.active(vi) && governor.holding_30(),
            "the seventh late frame holds 30 at once");
        // A heavy scene: it stays at a steady 30, with no tries in between.
        run(governor, vi, 900, 2U, slow);
        require(!governor.active(vi), "a host that is still too slow stays at 30");
        require(governor.busy_p90() == slow, "busy time of the slowest tenth");
        // The scene gets lighter: 60 resumes once 90 % of the last 60 frames
        // at 30 were fast enough for 60.
        run(governor, vi, 53, 2U, fast);
        require(!governor.active(vi), "not before 90 % of the window is fast");
        run(governor, vi, 1, 2U, fast);
        require(governor.active(vi), "60 resumes on evidence");
        // A try that fails at once doubles the minimum time at 30 (4 s).
        run(governor, vi, 7, 2U, fast);
        require(!governor.active(vi), "failed again");
        run(governor, vi, 119, 2U, fast);
        require(!governor.active(vi), "the minimum time at 30 doubled");
        run(governor, vi, 1, 2U, fast);
        require(governor.active(vi), "and 60 resumes after it");
        // Holding 60 for 10 s makes the next minimum short again.
        run(governor, vi, static_cast<int>(G::kSettled) + 60, 1U, fast);
        run(governor, vi, 7, 2U, slow);
        require(!governor.active(vi), "paused");
        run(governor, vi, 59, 2U, fast);
        require(!governor.active(vi), "not before the 2 s minimum and a fast window");
        run(governor, vi, 1, 2U, fast);
        require(governor.active(vi), "after holding 60 the minimum is 2 s again");
        // Borderline busy times: tried again at twice the minimum time.
        {
            G edge;
            std::uint64_t at = 0U;
            run(edge, at, 7, 2U, borderline);
            require(!edge.active(at), "borderline: paused");
            run(edge, at, 65, 2U, borderline);
            require(!edge.active(at), "borderline: not at the minimum time");
            run(edge, at, 55, 2U, borderline);
            require(edge.active(at), "borderline: tried at twice the minimum");
        }
        // Without busy times it retries when the minimum time has passed.
        G timed;
        vi = 0U;
        run(timed, vi, 7, 2U, 0U);
        require(!timed.active(vi), "timed: paused");
        run(timed, vi, 60, 2U, 0U);
        require(timed.active(vi), "timed: retries after the minimum");
        // A new scene tries 60 at once.
        run(timed, vi, 7, 2U, 0U);
        require(!timed.active(vi), "timed: paused again");
        timed.new_scene(vi);
        require(timed.active(vi), "a new scene tries 60 at once");
    }
    // The governor's busy percentile with few frames.
    {
        True60Governor governor;
        require(governor.busy_p90() == 0U, "no frames, no busy time");
        governor.frame(1U, 1U, 5000U);
        require(governor.busy_p90() == 5000U, "one frame");
    }
    // The scheduler's frame-done message is released at once at true 60.
    require(true60_frame_count(0U) == 2U && true60_frame_count(1U) == 2U &&
                true60_frame_count(2U) == 2U && true60_frame_count(5U) == 5U,
        "true 60 lifts the scheduler's frame count to 2");

    // True 60 fixes: batched texture scroller ticks, literal-2 particle
    // bursts and repeated controller presses.
    {
        std::uint32_t pending = 0U;
        require(batch_ticks(pending, 1U) == 0U && pending == 1U, "first tick waits");
        require(batch_ticks(pending, 1U) == 2U && pending == 0U, "second tick runs both");
        require(batch_ticks(pending, 2U) == 2U, "a 2-tick frame runs at once");
        require(batch_ticks(pending, 1U) == 0U && batch_ticks(pending, 3U) == 4U,
            "a slow frame flushes pending ticks");
        std::uint32_t total = 0U, ticks = 0U;
        pending = 0U;
        for (int frame = 0; frame < 61; ++frame) {
            total += batch_ticks(pending, 1U);
            ++ticks;
        }
        require(total + pending == ticks, "no ticks lost");
    }
    {
        const HalfRateFunction* squaddie = half_rate_function(kSquaddieControl);
        const HalfRateFunction* module = half_rate_function(kSquadsModuleTick);
        require(squaddie != nullptr && module != nullptr &&
                    is_half_rate_function(kSquadronControl) && !is_half_rate_function(kJoyRead),
            "the squads AI runs together at half rate");
        require(squaddie->rate_register == 5U && module->rate_register == 4U,
            "rate registers: a1 for controls, a0 for the module tick");
        require(half_rate_call(*squaddie, 1U, 1U, 0x80008FF8U), "a per-frame call is batched");
        require(!half_rate_call(*squaddie, 7U, 1U, 0x80008FF8U),
            "a call that does not pass the frame's rate runs as it is");
        require(!half_rate_call(*squaddie, 1U, 1U, 0x80051F00U) &&
                    half_rate_call(*squaddie, 1U, 1U, kSquadsSetupEnd),
            "the level setup's SquaddieControl calls are left alone");
        for (const HalfRateFunction& entry : kHalfRateFunctions)
            require(entry.rate_register >= 4U && entry.rate_register <= 7U &&
                        half_rate_function(entry.address) == &entry,
                "half-rate entries: argument registers, no duplicates");
    }
    require(is_half_rate_function(kSplashControl) && is_half_rate_function(kWakeUpdateRipple),
        "splashes and wakes run at half rate");
    require(odd_tick_compensated(kControlUpdatePlayerAim) && odd_tick_compensated(kShirukenControl) &&
                odd_tick_compensated(kCamlightVisibilityCheck) && !odd_tick_compensated(kSplashControl),
        "aim timers, shurikens and lens flares are compensated on odd ticks");
    {
        // The call decrements hold and spin-up when hold is set (spin-up
        // clamped at 0) and the lock-on nibble when it is set.
        require(aim_counters_for_odd_tick({15, 4, 0x3AU}) == AimCounters{16, 5, 0x3BU},
            "aim: raise what the call counts down");
        require(aim_counters_for_odd_tick({15, 0, 0x30U}) == AimCounters{16, 0, 0x30U},
            "aim: a spin-up at 0 stays (the call clamps it)");
        require(aim_counters_for_odd_tick({0, 4, 0x0FU}) == AimCounters{0, 4, 0x0FU},
            "aim: nothing counts while the hold is 0; a full nibble is left alone");
        require(aim_counters_for_odd_tick({127, 127, 0x01U}) == AimCounters{127, 127, 0x02U},
            "aim: no overflow");
    }
    {
        const StateCounter* tank = state_counter(0x06A00290U);
        const StateCounter* bolt = state_counter(0x09501450U);
        require(tank != nullptr && bolt != nullptr && tank->bytes == 1U && bolt->bytes == 2U &&
                    odd_tick_compensated(0x06A00290U),
            "hover tank spin ramp and flower bolt arming are compensated");
        require(counter_stepped(0x1FU, 0x20U, 1U) && counter_stepped(0x20U, 0x1FU, 1U) &&
                    counter_stepped(0xFFU, 0x00U, 1U) && counter_stepped(9U, 10U, 2U) &&
                    counter_stepped(0U, 0xFFFFU, 2U),
            "a one step either way is taken back");
        require(!counter_stepped(5U, 5U, 1U) && !counter_stepped(0U, 0x20U, 1U) &&
                    !counter_stepped(0x00U, 0x100U, 2U),
            "anything else (a state change) is kept");
    }
    require(angle_stepped(0xFFF0U, static_cast<std::uint16_t>(0xFFF0U + kShirukenSpinStep),
                          kShirukenSpinStep) &&
                !angle_stepped(0x1000U, 0x1001U, kShirukenSpinStep),
        "angle steps wrap at 16 bits");
    require(trigger_ticks(2U, 1U) == 1U, "literal 2 at one VI per frame");
    require(trigger_ticks(2U, 2U) == 2U, "unchanged at 30 fps");
    require(trigger_ticks(1U, 1U) == 1U && trigger_ticks(3U, 1U) == 3U,
        "other tick counts unchanged");
    {
        std::array<std::uint8_t, 48> before{}, after{};
        require(!controller_read_consumed(before, after), "no new read");
        after[30] = 1U;
        require(controller_read_consumed(before, after), "new read");
    }

    // FPS counter "Detailed": message encoding, content tracking, gaps.
    {
        FrameStats stats{600U, 600U, 598U, 301U, 300U, 2U, 300U, 158U, 342U, 69U, 70001U,
                         98U, 142U, 60U, 31U, 2U, 7U, 183U, 4U,
                         11000U, 12U, 9000U, 2000U, 150U, 143U, 3U};
        const auto words = encode_frame_stats(stats);
        FrameStats decoded{};
        for (unsigned index = 0U; index < kFrameDetailMessageCount; ++index)
            decode_frame_stats(decoded, index, words[index * 2U], words[index * 2U + 1U]);
        stats.display_max = 0xFFFFU;  // Clamped to 16 bits.
        require(decoded == stats, "frame stats round trip");
        require(format_frame_host(stats) ==
                    "host 9.8 ms, longest 14.2 ms | gfx 6.0 tasks, 3.1 ms | VI wake +0.4 ms\n"
                    "True 60: holding 30, busy 18.3 ms\n"
                    "MMIO 11000/s, stepped 12 | switches 9000/s, woken 2000 | audio 143/150 ms, "
                    "3 dropouts",
            "host line, holding 30");
        stats.true60 = 1U;
        require(format_frame_host(stats).find("True 60: 60, late 7/60, busy 18.3 ms\n") !=
                    std::string::npos,
            "host line at 60");
        stats.true60 = 0U;
        require(format_frame_host(stats).find("True 60: off\n") != std::string::npos,
            "host line, Original");
    }
    {
        FrameContentTracker tracker;
        const std::uint32_t a = 0x00200000U, b = 0x00300000U;
        tracker.drawn(a, 1U, 10U);
        auto shown = tracker.presented(a);
        require(shown.fresh && shown.scene_changed && shown.camera_changed, "first frame is new");
        require(!tracker.presented(a).fresh, "showing it again is a repeat");
        tracker.drawn(b, 1U, 10U);
        shown = tracker.presented(b);
        require(shown.fresh && !shown.scene_changed && !shown.camera_changed,
            "same matrices in the other buffer: drawn but not new");
        tracker.drawn(a, 2U, 10U);
        shown = tracker.presented(a);
        require(shown.fresh && shown.scene_changed && !shown.camera_changed,
            "scene moved, camera still");
        tracker.drawn(b, 3U, 11U);
        tracker.drawn(0x00400000U, 9U, 9U);  // An off-screen target.
        shown = tracker.presented(b);
        require(shown.fresh && shown.scene_changed && shown.camera_changed, "camera moved");
        tracker.drawn(a, 3U, 11U);
        tracker.drawn(a, 4U, 12U);  // Two tasks into one picture combine.
        require(tracker.presented(a).scene_changed, "combined tasks");
        require(!tracker.presented(0x00500000U).fresh && !tracker.presented(0U).fresh,
            "unknown buffers are repeats");
        // Alternate frames with unchanged content: 60 drawn, 30 new.
        FrameContentTracker pairs;
        unsigned drawn = 0U, fresh_content = 0U;
        for (std::uint64_t frame = 0U; frame < 60U; ++frame) {
            const std::uint32_t target = (frame & 1U) ? a : b;
            pairs.drawn(target, frame / 2U, frame / 2U);
            const auto result = pairs.presented(target);
            drawn += result.fresh ? 1U : 0U;
            fresh_content += result.scene_changed ? 1U : 0U;
        }
        require(drawn == 60U && fresh_content == 30U, "every other frame unchanged");
    }
    {
        IntervalRange range;
        require(range.minimum_tenths() == 0U && range.maximum_tenths() == 0U, "no gaps");
        range.add(16667U);
        range.add(33333U);
        range.add(15950U);
        require(range.minimum_tenths() == 160U && range.maximum_tenths() == 333U, "gap range");
        const std::array<std::uint32_t, 2> words{1U, 2U}, swapped{2U, 1U};
        require(mix_content_hash(kContentHashSeed, words) !=
                    mix_content_hash(kContentHashSeed, swapped),
            "hash depends on order");
    }

    // The game's own rate per frame.
    require(game_rate_from_retraces(2U) == 30U, "two VIs: 30 fps");
    require(game_rate_from_retraces(3U) == 20U, "three VIs: 20 fps");
    require(game_rate_from_retraces(1U) == 60U, "one VI: 60 fps");
    require(game_rate_from_retraces(0U) == 60U, "no measurement");
    require(game_rate_from_retraces(40U) == 10U, "loading stall clamps");

    // Main frames versus off-screen work.
    require(is_main_frame_target(320U, 240U, 320U, 240U), "screen buffer");
    require(is_main_frame_target(320U, 180U, 320U, 240U), "letterboxed screen");
    require(!is_main_frame_target(64U, 64U, 320U, 240U), "shadow mask");
    require(!is_main_frame_target(320U, 16U, 320U, 240U), "thin strip");
    require(!is_main_frame_target(160U, 120U, 320U, 240U), "quarter buffer");
    require(is_main_frame_target(320U, 240U, 0U, 0U), "unknown VI, screen size");

    // IDs never collide with RT64's ignore/auto or with each other's ranges.
    std::set<std::uint32_t> cameras;
    for (const bool ortho : {false, true})
        for (const bool screen : {false, true})
            for (std::uint32_t ordinal = 0U; ordinal < 8U; ++ordinal)
                cameras.insert(camera_group_id(ortho, screen, ordinal));
    require(cameras.size() == 32U, "camera ids distinct");
    require(*cameras.begin() > 0U && *cameras.rbegin() < kObjectIdBase,
        "camera ids in range");
    std::set<std::uint32_t> objects;
    for (std::uint32_t address = 0x80100000U; address < 0x80100000U + 64U * 0x40U;
         address += 0x40U)
        for (std::uint32_t push = 0U; push < 4U; ++push) {
            const auto id = object_group_id(address, push);
            require(id >= kObjectIdBase && id != 0xFFFFFFFFU, "object id range");
            objects.insert(id);
            const auto bone = bone_group_id(id, push);
            require(bone >= kObjectIdBase && bone != 0xFFFFFFFFU && bone != id,
                "bone id range");
        }
    require(objects.size() == 256U, "object ids distinct");
    require(object_group_id(0x80123450U, 1U) == object_group_id(0x80123450U, 1U),
        "object ids stable");
    require(bone_group_id(0U, 3U) == 0U, "no object, no bone id");

    // Camera pose from a view matrix.
    const auto pose = camera_pose_from_view(view(100.0F, 50.0F, -30.0F, 0.3F), 1.5F);
    require(pose.valid && near(pose.position[0], 100.0F) &&
                near(pose.position[1], 50.0F) && near(pose.position[2], -30.0F),
        "camera position");
    require(near(pose.forward[0] * pose.forward[0] + pose.forward[1] * pose.forward[1] +
                     pose.forward[2] * pose.forward[2],
                 1.0F),
        "unit forward");
    require(!camera_pose_from_view(Matrix{}, 1.0F).valid, "degenerate view");

    // Cuts: steady motion blends; jumps, sharp turns and zooms snap.
    const auto at = [](float x, float yaw, float focal = 1.5F) {
        return camera_pose_from_view(view(x, 0.0F, 0.0F, yaw), focal);
    };
    require(!camera_cut(at(0.0F, 0.0F), at(30.0F, 0.0F), at(60.0F, 0.05F)),
        "walking camera blends");
    require(!camera_cut(at(0.0F, 0.0F), at(300.0F, 0.0F), at(600.0F, 0.0F)),
        "fast but steady camera blends");
    require(camera_cut(at(0.0F, 0.0F), at(30.0F, 0.0F), at(3000.0F, 0.0F)),
        "teleport snaps");
    require(camera_cut(CameraPose{}, at(0.0F, 0.0F), at(5000.0F, 0.0F)),
        "jump without history snaps");
    require(camera_cut(at(0.0F, 0.0F), at(0.0F, 0.0F), at(0.0F, 1.2F)),
        "sharp turn snaps");
    require(!camera_cut(at(0.0F, 0.0F), at(0.0F, 0.0F), at(0.0F, 0.4F)),
        "normal turn blends");
    require(camera_cut(at(0.0F, 0.0F), at(0.0F, 0.0F), at(0.0F, 0.0F, 2.5F)),
        "zoom snaps");
    require(camera_cut(at(0.0F, 0.0F), CameraPose{}, at(0.0F, 0.0F)),
        "no previous camera");
    return 0;
}
