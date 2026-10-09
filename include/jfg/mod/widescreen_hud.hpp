#pragma once

// RGO widescreen HUD anchoring. See docs/rgo/widescreen.md.
//
// The single-player HUD is drawn by a handful of overlay functions (the
// "groups" below) through three main-code primitives: frontDrawObj (textured
// quads under an orthographic camera), fontPrintXY (text as texture
// rectangles) and frontDrawRectangles (fill rectangles). Each frame records
// where every group drew. The next frame anchors a group to the left or right
// screen edge when all of it sits on that side, so an element's parts (icon,
// number, bar) move together. Groups that span the middle stay centred.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "jfg/renderer/widescreen.hpp"

namespace jfg::mod {

enum class HudAnchor : std::uint8_t { none, left, right };

// US main-code entry points, qualified by their first two instructions.
struct HudFunction {
    std::uint32_t address;
    std::uint32_t prologue[2];
};
inline constexpr HudFunction kHudFrontUpdate{0x80058A6CU, {0x27BDFFE0U, 0xAFB00018U}};
inline constexpr HudFunction kHudFontPrintXY{0x8006FCA8U, {0x27BDFFE0U, 0xAFA60028U}};
inline constexpr HudFunction kHudDrawRectangles{0x800595F4U, {0x27BDFF90U, 0xAFB20020U}};
inline constexpr HudFunction kHudDrawObj{0x8005A0D0U, {0x3C0F8010U, 0x25EFF780U}};

// frontgfx: the 2D display-list cursor frontUpdate hands to the HUD code.
inline constexpr std::uint32_t kHudFrontGfx = 0x800FF398U;
// currentobjects: 32-byte transforms; X (orthographic pixels from the screen
// centre, positive right) is the float at +0x0C.
inline constexpr std::uint32_t kHudCurrentObjects = 0x800FF780U;
inline constexpr std::uint32_t kHudCurrentObjectStride = 0x20U;
inline constexpr std::uint32_t kHudCurrentObjectX = 0x0CU;
inline constexpr std::uint32_t kHudCurrentObjectCount = 54U;
// frontDrawRectangles' JfgFrontRectangle: s16 left, top, right, bottom; u32 rgba.
inline constexpr std::uint32_t kHudRectangleStride = 12U;
inline constexpr std::int32_t kHudPositionCentred = -0x8000;

// Overlay functions that draw one part of the single-player HUD. Overlays
// dispatch at (overlay << 20) | offset.
inline constexpr std::array<std::uint32_t, 13U> kHudGroups{
    0x00E00074U, 0x00E00220U, 0x00E00B30U,  // overlay 14: frontSingleInstruments
    0x00E010F4U, 0x00E01328U, 0x00E019F4U,  // and the functions it calls
    0x00E01AD8U, 0x00E0292CU, 0x00E02CF4U,
    0x00E03418U, 0x00E03C68U,
    0x00600000U,  // overlay 6: instDrawHealth
    0x00D004A8U,  // overlay 13: frontDrawTarget (crosshair, stays centred)
};

[[nodiscard]] constexpr bool is_hud_group(const std::uint32_t target) noexcept {
    return std::find(kHudGroups.begin(), kHudGroups.end(), target) !=
        kHudGroups.end();
}

// The HUD is held at 16:9 on wider screens, as Zelda 64: Recompiled does.
inline constexpr float kHudMaximumExtent = (16.0F / 9.0F) / (4.0F / 3.0F);

[[nodiscard]] constexpr float hud_extent(const float view_extent) noexcept {
    return view_extent > 1.0F ? (std::min)(view_extent, kHudMaximumExtent) : 1.0F;
}

struct HudBounds {
    float left = std::numeric_limits<float>::infinity();
    float right = -std::numeric_limits<float>::infinity();
    [[nodiscard]] constexpr bool empty() const noexcept { return left > right; }
    constexpr void add(const float a, const float b) noexcept {
        left = (std::min)(left, (std::min)(a, b));
        right = (std::max)(right, (std::max)(a, b));
    }
};

// Anchor only groups that keep clear of the middle and sit in the outer third.
[[nodiscard]] constexpr HudAnchor classify_hud_bounds(
    const HudBounds& bounds, const float width) noexcept {
    if (bounds.empty() || !(width > 0.0F))
        return HudAnchor::none;
    const float middle = width * 0.5F;
    const float margin = width * 0.05F;
    const float centre = (bounds.left + bounds.right) * 0.5F;
    if (bounds.right <= middle - margin && centre < width / 3.0F)
        return HudAnchor::left;
    if (bounds.left >= middle + margin && centre > width * 2.0F / 3.0F)
        return HudAnchor::right;
    return HudAnchor::none;
}

// Orthographic X offset that keeps an element's distance from its edge when
// RT64 widens the picture by `extent` (centre-origin pixels).
[[nodiscard]] constexpr float hud_object_shift(
    const HudAnchor anchor, const float extent, const float width) noexcept {
    const float shift = width * 0.5F * (extent - 1.0F);
    return anchor == HudAnchor::left ? -shift
        : anchor == HudAnchor::right ? shift : 0.0F;
}

// RT64 extended GBI (rt64_extended_gbi.h): G_EX_SETRECTALIGN_V1 is two
// commands under the extended opcode, which the renderer shell enables.
inline constexpr std::uint8_t kHudExtendedOpcode = kWidescreenExtendedOpcode;
inline constexpr std::uint32_t kHudSetRectAlign = 0x000006U;
inline constexpr std::uint32_t kHudOriginNone = 0x800U;
inline constexpr std::uint32_t kHudOriginLeft = 0x000U;
inline constexpr std::uint32_t kHudOriginRight = 0x400U;

// Rectangle coordinates stay absolute; RT64 adds the origin's framebuffer
// offset, so a right-anchored rectangle subtracts the width (10.2 format).
[[nodiscard]] constexpr std::array<std::uint32_t, 4U> hud_rect_align_command(
    const HudAnchor anchor, const std::uint32_t framebuffer_width) noexcept {
    const std::uint32_t origin = anchor == HudAnchor::left ? kHudOriginLeft
        : anchor == HudAnchor::right ? kHudOriginRight : kHudOriginNone;
    const std::uint32_t x = anchor == HudAnchor::right
        ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(framebuffer_width * 4U)) & 0xFFFFU
        : 0U;
    return {
        (std::uint32_t{kHudExtendedOpcode} << 24U) | kHudSetRectAlign,
        (origin & 0xFFFU) | ((origin & 0xFFFU) << 12U),
        x << 16U,
        x << 16U,
    };
}

// Follow gDPSetScissor (0xED) in a display-list stream. Anchoring is only
// safe under a full-width scissor: RT64 keeps a narrower one in the centred
// 4:3 area, where a moved element would be cut off.
[[nodiscard]] constexpr bool hud_scissor_full_width(
    const std::uint32_t word0, const std::uint32_t word1,
    const std::uint32_t framebuffer_width) noexcept {
    const std::uint32_t ulx = (word0 >> 12U) & 0xFFFU;
    const std::uint32_t lrx = (word1 >> 12U) & 0xFFFU;
    return ulx == 0U && lrx >= framebuffer_width * 4U;
}

class WidescreenHud {
public:
    // frontUpdate: last frame's bounds decide this frame's anchors.
    void begin_frame(const bool enabled, const float width,
                     const std::uint32_t display_list) {
        for (auto& group : groups_) {
            group.anchor = enabled ? classify_hud_bounds(group.bounds, width)
                                   : HudAnchor::none;
            group.bounds = {};
        }
        stack_.clear();
        scan_cursor_ = display_list;
        scissor_full_ = true;
    }
    void enter(const std::uint32_t function) {
        stack_.push_back(index(function));
    }
    void exit() noexcept {
        if (!stack_.empty())
            stack_.pop_back();
    }
    [[nodiscard]] HudAnchor anchor() const noexcept {
        return stack_.empty() ? HudAnchor::none : groups_[stack_.back()].anchor;
    }
    [[nodiscard]] bool in_group() const noexcept { return !stack_.empty(); }
    void add(const float left, const float right) noexcept {
        if (!stack_.empty())
            groups_[stack_.back()].bounds.add(left, right);
    }
    [[nodiscard]] HudAnchor anchor_of(const std::uint32_t function) const noexcept {
        for (const auto& group : groups_)
            if (group.function == function)
                return group.anchor;
        return HudAnchor::none;
    }

    // Display-list scan state, advanced by the caller as commands appear.
    [[nodiscard]] std::uint32_t scan_cursor() const noexcept { return scan_cursor_; }
    void set_scan_cursor(const std::uint32_t cursor) noexcept { scan_cursor_ = cursor; }
    [[nodiscard]] bool scissor_full() const noexcept { return scissor_full_; }
    void set_scissor_full(const bool full) noexcept { scissor_full_ = full; }

private:
    struct Group {
        std::uint32_t function = 0U;
        HudAnchor anchor = HudAnchor::none;
        HudBounds bounds{};
    };
    std::size_t index(const std::uint32_t function) {
        for (std::size_t i = 0U; i < groups_.size(); ++i)
            if (groups_[i].function == function)
                return i;
        groups_.push_back({function, HudAnchor::none, {}});
        return groups_.size() - 1U;
    }
    std::vector<Group> groups_;
    std::vector<std::size_t> stack_;
    std::uint32_t scan_cursor_ = 0U;
    bool scissor_full_ = true;
};

}  // namespace jfg::mod
