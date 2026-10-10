#pragma once

// RGO FPS counter (docs/rgo/framerate.md, "FPS counter"). The game reports
// three rates and the displayed frame times to the frontend twice a second:
//
//   Game    - game logic updates per second (viFrameSync calls). This is the
//             rate the game simulates at, the only one that changes how the
//             game plays and feels.
//   Render  - real game frames handed to the renderer per second.
//   Display - frames the swap chain presented per second, including frames
//             RT64 blended in between (frame interpolation).
//   Frame time min/max - the shortest and longest gap between two presented
//             frames in the window; a wide spread is judder.
//
// One window message carries everything: rates in tenths of a frame per
// second, frame times in tenths of a millisecond (both saturating).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace jfg {

struct FrameStats {
    std::uint32_t game_tenths = 0U;
    std::uint32_t render_tenths = 0U;
    std::uint32_t display_tenths = 0U;
    std::uint16_t frame_min_tenth_ms = 0U;
    std::uint16_t frame_max_tenth_ms = 0U;
    friend bool operator==(const FrameStats&, const FrameStats&) = default;
};

struct PackedFrameStats {
    std::uint64_t first = 0U;   // WPARAM
    std::uint64_t second = 0U;  // LPARAM
};

[[nodiscard]] constexpr std::uint32_t frame_rate_tenths(
    const std::uint64_t frames, const std::uint64_t elapsed_us) noexcept {
    if (elapsed_us == 0U)
        return 0U;
    const std::uint64_t tenths = (frames * 10'000'000ULL + elapsed_us / 2U) / elapsed_us;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(tenths, 0xFFFFFFU));
}

[[nodiscard]] constexpr std::uint16_t frame_time_tenth_ms(
    const std::uint64_t microseconds) noexcept {
    return static_cast<std::uint16_t>(
        std::min<std::uint64_t>((microseconds + 50U) / 100U, 0xFFFFU));
}

// Rates use 24 bits each (up to 1.67 million FPS in tenths).
[[nodiscard]] constexpr PackedFrameStats pack_frame_stats(const FrameStats& stats) noexcept {
    const auto rate = [](std::uint32_t value) {
        return static_cast<std::uint64_t>(std::min<std::uint32_t>(value, 0xFFFFFFU));
    };
    return {rate(stats.game_tenths) | (rate(stats.render_tenths) << 24U),
            rate(stats.display_tenths) |
                (static_cast<std::uint64_t>(stats.frame_min_tenth_ms) << 24U) |
                (static_cast<std::uint64_t>(stats.frame_max_tenth_ms) << 40U)};
}

[[nodiscard]] constexpr FrameStats unpack_frame_stats(const PackedFrameStats packed) noexcept {
    return {static_cast<std::uint32_t>(packed.first & 0xFFFFFFU),
            static_cast<std::uint32_t>((packed.first >> 24U) & 0xFFFFFFU),
            static_cast<std::uint32_t>(packed.second & 0xFFFFFFU),
            static_cast<std::uint16_t>((packed.second >> 24U) & 0xFFFFU),
            static_cast<std::uint16_t>((packed.second >> 40U) & 0xFFFFU)};
}

// "Game 60 | Render 60 | Display 144 | 6.9-7.0 ms"
[[nodiscard]] inline std::string format_frame_stats(const FrameStats& stats) {
    const auto rate = [](std::uint32_t tenths) {
        return std::to_string((tenths + 5U) / 10U);
    };
    char times[48];
    std::snprintf(times, sizeof(times), "%u.%u-%u.%u ms",
                  stats.frame_min_tenth_ms / 10U, stats.frame_min_tenth_ms % 10U,
                  stats.frame_max_tenth_ms / 10U, stats.frame_max_tenth_ms % 10U);
    return "Game " + rate(stats.game_tenths) + " | Render " + rate(stats.render_tenths) +
           " | Display " + rate(stats.display_tenths) + " | " + times;
}

}  // namespace jfg
