#pragma once

// RGO frame interpolation: the pure decisions shared by the renderer shell,
// the RT64 frame-grouping patch, the game hooks and their tests. See
// docs/rgo/framerate.md.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "jfg/renderer/video_settings.hpp"

namespace jfg {

// RT64's refresh-rate configuration for a Frame rate setting.
enum class Rt64RefreshMode : std::uint8_t { original, display, manual };

// Manual target that the frame-grouping patch leaves uncapped ("Unlocked").
// RT64 itself caps Manual targets at the display's refresh rate.
inline constexpr int kUnlockedRefreshTarget = 1000;

struct Rt64RefreshPlan {
    Rt64RefreshMode mode = Rt64RefreshMode::original;
    int target = 60;
    friend bool operator==(const Rt64RefreshPlan&, const Rt64RefreshPlan&) = default;
};

[[nodiscard]] constexpr Rt64RefreshPlan refresh_plan(const FrameRate rate) noexcept {
    switch (rate) {
    case FrameRate::sixty:
        return {Rt64RefreshMode::manual, 60};
    case FrameRate::display:
        return {Rt64RefreshMode::display, 60};
    case FrameRate::unlocked:
        return {Rt64RefreshMode::manual, kUnlockedRefreshTarget};
    case FrameRate::original:
    default:
        return {};
    }
}

[[nodiscard]] constexpr bool interpolation_requested(const FrameRate rate) noexcept {
    return rate != FrameRate::original;
}

// The game's logic rate for one game frame: 60 / the VIs it took (2 normally,
// 3 after slow frames). Long stalls (loading) are clamped to 10 per second.
[[nodiscard]] constexpr std::uint16_t game_rate_from_retraces(
    const std::uint64_t retraces) noexcept {
    if (retraces == 0U)
        return 60U;
    return static_cast<std::uint16_t>(60U / std::clamp<std::uint64_t>(retraces, 1U, 6U));
}

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
