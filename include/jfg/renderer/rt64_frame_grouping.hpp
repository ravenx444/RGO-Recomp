#pragma once

// RGO frame interpolation: the two decisions the RT64 frame-grouping patch
// (cmake/rt64_frame_interpolation.cmake) compiles into RT64's workload queue.
// RT64 builds as C++17, so this header must stay C++17-compatible.

#include <cstdint>

namespace jfg {

// Manual target that the frame-grouping patch leaves uncapped ("Unlocked").
// RT64 itself caps Manual targets at the display's refresh rate.
inline constexpr int kUnlockedRefreshTarget = 1000;

// A workload is a main game frame when it draws a presentable color image at
// least as wide as the VI framebuffer and half its height. Smaller targets
// (64x64 shadow masks, effect buffers) are auxiliary: RT64 renders them but
// must not treat them as a game frame for interpolation.
[[nodiscard]] constexpr bool is_main_frame_target(
    const std::uint32_t color_width, const std::uint32_t drawn_height,
    const std::uint32_t vi_width, const std::uint32_t vi_height) noexcept {
    if (vi_width == 0U || vi_height == 0U)
        return color_width >= 256U && drawn_height >= 120U;
    return color_width >= vi_width && drawn_height * 2U >= vi_height;
}

// RT64 protects adjacent workload slots, not a main frame retained across
// auxiliary shadow/effect workloads. Stop matching once that protection ends.
class MainFrameHistory {
public:
    [[nodiscard]] constexpr bool observe(const bool auxiliary,
        const std::uint32_t current_slot, const std::uint32_t expected_previous) noexcept {
        const bool usable = !auxiliary && valid_ && previous_slot_ == expected_previous &&
                            previous_slot_ != current_slot;
        valid_ = !auxiliary;
        previous_slot_ = current_slot;
        return usable;
    }
private:
    bool valid_ = false;
    std::uint32_t previous_slot_ = 0U;
};

}  // namespace jfg
