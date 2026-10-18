#pragma once

// RGO widescreen HUD anchoring. See docs/rgo/widescreen.md.
//
// The single-player HUD mixes textured geometry, dedicated number drawing,
// text and fill rectangles. Record primitive bounds to classify each complete
// overlay group, then apply a shared viewport and rectangle origin around the
// whole group. This keeps panels, icons and counters together without moving
// geometry outside the original guest clipping coordinates.

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

// Edge anchoring also needs a matching scissor scope for each moved primitive.
inline constexpr bool kHudEdgeAnchoringEnabled = true;

// US main-code entry points, qualified by their first two instructions.
struct HudFunction {
    std::uint32_t address;
    std::uint32_t prologue[2];
};
inline constexpr HudFunction kHudFrontUpdate{0x80058A6CU, {0x27BDFFE0U, 0xAFB00018U}};
inline constexpr HudFunction kHudPrintNum{0x80058EF0U, {0x27BDFF40U, 0xAFB30020U}};
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

// Viewport offsets use quarter pixels, just like rectangle alignment.
[[nodiscard]] constexpr std::array<std::uint32_t, 4U> hud_viewport_align_command(
    const HudAnchor anchor, const std::uint32_t framebuffer_width) noexcept {
    const auto rectangle = hud_rect_align_command(anchor, framebuffer_width);
    return {0x64000007U, rectangle[1] & 0xFFFU, rectangle[2], 0U};
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

// Only known HUD extensions may be decoded inside counted RDP lists.
// Other low opcodes remain data, and callers must validate the word count.
[[nodiscard]] constexpr std::size_t hud_command_words(
    const std::uint32_t word0, const std::uint32_t word1) noexcept {
    if ((word0 >> 24U) != kHudExtendedOpcode) return 0U;
    switch (word0 & 0xFFFFFFU) {
    case 5U:
        return (word1 & 0xFC000000U) == 0U &&
            ((word1 >> 2U) & 0xFFFU) <= kHudOriginNone &&
            ((word1 >> 14U) & 0xFFFU) <= kHudOriginNone ? 2U : 0U;
    case 7U:
        return word1 <= kHudOriginNone ? 2U : 0U;
    case 6U:
        return (word1 & 0xFF000000U) == 0U &&
            (word1 & 0xFFFU) <= kHudOriginNone &&
            ((word1 >> 12U) & 0xFFFU) <= kHudOriginNone ? 2U : 0U;
    case 0x15U:
    case 0x16U:
    case 0x17U:
    case 0x18U:
        return word1 == 0U ? 1U : 0U;
    default:
        return 0U;
    }
}

struct HudScissor {
    std::uint32_t top = 0U, bottom = 960U, mode = 0U;
};

// Push the existing clipping rectangle, then stretch its horizontal edges.
// Coordinates use the RDP's quarter-pixel units; preserve vertical clipping.
[[nodiscard]] constexpr std::array<std::uint32_t, 6U> hud_clip_push_command(
    const HudScissor scissor) noexcept {
    return {0x64000017U, 0U, 0x64000005U,
            (kHudOriginRight << 14U) | (scissor.mode & 3U),
            scissor.top & 0xFFFFU, scissor.bottom & 0xFFFFU};
}
inline constexpr std::array<std::uint32_t, 2U> kHudClipPopCommand{0x64000018U, 0U};

class WidescreenHud {
public:
    // frontUpdate: last frame's bounds decide this frame's anchors.
    void begin_frame(const bool enabled, const float width,
                     const std::uint32_t display_list) {
        for (auto& group : groups_) {
            // These three routines form the left weapon panel, ammo counters
            // and health meter. Animation may expand their bounds through the
            // centre or off-screen; their screen-edge ownership does not change.
            const bool left_instrument = group.function == 0x00E01AD8U ||
                group.function == 0x00E0292CU || group.function == 0x00600000U;
            // Projected weapon reticles can move through the outer thirds.
            // They remain in camera coordinates, regardless of their bounds.
            const bool reticle = group.function == 0x00D004A8U;
            group.anchor = !enabled || reticle ? HudAnchor::none : left_instrument
                ? HudAnchor::left : classify_hud_bounds(group.bounds, width);
            group.bounds = {};
        }
        stack_.clear();
        scan_cursor_ = display_list;
        scissor_full_ = true;
        scissor_ = {0U, static_cast<std::uint32_t>(width * 3.0F), 0U};
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
    [[nodiscard]] std::uint32_t current_group() const noexcept {
        return stack_.empty() ? 0U : groups_[stack_.back()].function;
    }
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

    std::uint32_t viewport_command = 0U, viewport_address = 0U;

    // Display-list scan state, advanced by the caller as commands appear.
    [[nodiscard]] std::uint32_t scan_cursor() const noexcept { return scan_cursor_; }
    void set_scan_cursor(const std::uint32_t cursor) noexcept { scan_cursor_ = cursor; }
    [[nodiscard]] HudScissor scissor() const noexcept { return scissor_; }
    void set_scissor(const HudScissor value) noexcept { scissor_ = value; }
    void observe_scissor(const std::uint32_t word0, const std::uint32_t word1) noexcept {
        scissor_ = {word0 & 0xFFFU, word1 & 0xFFFU, (word1 >> 24U) & 3U};
    }
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
    HudScissor scissor_{};
};

}  // namespace jfg::mod
