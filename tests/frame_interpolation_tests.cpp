// RGO frame interpolation: rate mapping, IDs, frame grouping and camera cuts.
#include "jfg/renderer/frame_interpolation.hpp"

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
    require(cpu_overclock_shift(with(FrameRate::sixty, true)) == kTrue60OverclockShift &&
                cpu_overclock_shift(with(FrameRate::original, true)) == kNoSlowdownOverclockShift,
        "true 60 gets a larger overclock");

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
