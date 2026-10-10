#pragma once

// RGO widescreen: the pure math shared by the renderer shell, the game-side
// hooks and their tests. See docs/rgo/widescreen.md. The video.ini format is
// in video_settings.hpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "jfg/renderer/video_settings.hpp"

namespace jfg {

// RT64 extended-GBI opcode the renderer shell enables while widescreen is
// active. F3DDKR has no command 0x64, so game display lists never use it.
inline constexpr std::uint8_t kWidescreenExtendedOpcode = 0x64U;

// HUD anchors stop at the edges of a 16:9 area on wider screens.
inline constexpr double kWidescreenHudAspect = 16.0 / 9.0;

// RT64 widens its picture by target / source, where source is the VI
// framebuffer's shape (320x240 = 4:3). The game's own widescreen setting keeps
// a 320x240 framebuffer but scans it into 180 lines, so the displayed shape is
// display_width / display_height. Pick the target that makes the presented
// picture match the window: target = window * source / display. Never narrow
// the picture below its source shape (narrow and portrait windows letterbox
// the original picture) and never widen it past maximum_aspect (the player's
// Max aspect ratio; wider windows are pillarboxed).
[[nodiscard]] inline double widescreen_aspect_target(
    std::uint32_t window_width, std::uint32_t window_height,
    std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
    std::uint32_t display_width, std::uint32_t display_height,
    double maximum_aspect = kMaximumWidescreenAspect) noexcept {
    const double source = (framebuffer_width != 0U && framebuffer_height != 0U)
        ? double(framebuffer_width) / double(framebuffer_height)
        : 4.0 / 3.0;
    if (window_width == 0U || window_height == 0U)
        return source;
    const double display = (display_width != 0U && display_height != 0U)
        ? double(display_width) / double(display_height)
        : source;
    const double limit = (maximum_aspect > 0.0 && std::isfinite(maximum_aspect))
        ? (std::min)(maximum_aspect, kMaximumWidescreenAspect)
        : kMaximumWidescreenAspect;
    const double window = (std::min)(
        double(window_width) / double(window_height), limit);
    const double target = window * source / display;
    if (!std::isfinite(target))
        return source;
    return (std::max)(target, source);
}

// How much wider than the game's own projection the presented 3D view is.
// 1.0 means unchanged; 16:9 from a 4:3 source is 4/3.
[[nodiscard]] inline float widescreen_horizontal_extent(
    double target, std::uint32_t framebuffer_width,
    std::uint32_t framebuffer_height) noexcept {
    if (framebuffer_width == 0U || framebuffer_height == 0U)
        return 1.0F;
    const double source = double(framebuffer_width) / double(framebuffer_height);
    const double extent = target / source;
    if (!std::isfinite(extent) || extent <= 1.0)
        return 1.0F;
    return static_cast<float>(extent);
}

// F3DDKR-style trivial rejection, as the renderer shell applies it before
// RT64 sees a triangle or a CULLDL range. horizontal_extent > 1 accepts the
// parts of the scene RT64 reveals beside the original 4:3 frustum.
[[nodiscard]] constexpr std::uint8_t widescreen_clip_code(
    float x, float y, float w, float horizontal_extent) noexcept {
    std::uint8_t clip = 0U;
    const float wx = w * horizontal_extent;
    if (x > wx)
        clip |= 0x01U;
    if (x < -wx)
        clip |= 0x02U;
    if (y > w)
        clip |= 0x04U;
    if (y < -w)
        clip |= 0x08U;
    if (w < 0.01F)
        clip |= 0x10U;
    return clip;
}

// The track renderer culls level blocks against three planes built from fixed
// camera-space triangles (US 0x800A0CF4): a near plane and the two side
// planes. Scaling every triangle's X by the horizontal extent leaves the near
// plane unchanged and opens the side planes by the same factor as the view.
inline constexpr std::uint32_t kTrackFrustumTrianglesAddress = 0x800A0CF4U;
inline constexpr std::size_t kTrackFrustumFloatCount = 27U;  // 3 x 3 x xyz

inline void widen_track_frustum(
    std::array<float, kTrackFrustumFloatCount>& triangles,
    float horizontal_extent) noexcept {
    if (!(horizontal_extent > 1.0F) || !std::isfinite(horizontal_extent))
        return;
    for (std::size_t index = 0U; index < triangles.size(); index += 3U)
        triangles[index] *= horizontal_extent;
}

}  // namespace jfg
