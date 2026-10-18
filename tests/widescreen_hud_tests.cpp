#include "jfg/mod/widescreen_hud.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

jfg::mod::HudBounds bounds(const float left, const float right) {
    jfg::mod::HudBounds result;
    result.add(left, right);
    return result;
}

}  // namespace

int main() {
    using namespace jfg::mod;

    require(is_hud_group(0x00E00B30U) && is_hud_group(0x00600000U),
        "instrument groups are recognised");
    require(!is_hud_group(kHudFontPrintXY.address) && !is_hud_group(0U),
        "primitives are not groups");

    require(hud_extent(1.0F) == 1.0F && hud_extent(0.5F) == 1.0F,
        "no anchoring without widening");
    require(hud_extent(1.2F) == 1.2F, "HUD follows narrower wide screens");
    require(hud_extent(2.0F) == kHudMaximumExtent, "HUD clamps to 16:9");

    for (std::uint8_t mode=0;mode<4;++mode) {
        require(jfg::native_screen_mode(mode,jfg::AspectMode::widescreen)==(mode|1U),
            "native widescreen keeps high-resolution preference");
        require(jfg::native_screen_mode(mode,jfg::AspectMode::original)==(mode&~1U),
            "native original clears only widescreen");
    }
    require(jfg::native_screen_flags(0xA5U,jfg::AspectMode::widescreen)==0xE5U &&
        jfg::native_screen_flags(0xE5U,jfg::AspectMode::original)==0xA5U,
        "widescreen sync preserves unrelated saved option bits");

    // Classification on a 320-pixel framebuffer.
    require(classify_hud_bounds({}, 320.0F) == HudAnchor::none, "empty group");
    require(classify_hud_bounds(bounds(16.0F, 120.0F), 320.0F) == HudAnchor::left,
        "top-left health display");
    require(classify_hud_bounds(bounds(220.0F, 304.0F), 320.0F) == HudAnchor::right,
        "right-hand ammo display");
    require(classify_hud_bounds(bounds(150.0F, 170.0F), 320.0F) == HudAnchor::none,
        "crosshair stays centred");
    require(classify_hud_bounds(bounds(20.0F, 300.0F), 320.0F) == HudAnchor::none,
        "a group on both sides stays centred");
    require(classify_hud_bounds(bounds(100.0F, 150.0F), 320.0F) == HudAnchor::none,
        "a group near the middle stays centred");
    require(classify_hud_bounds(bounds(20.0F, 20.0F), 0.0F) == HudAnchor::none,
        "invalid width");

    // At 16:9, an element 10 px from the 4:3 edge stays 10 px from the edge.
    const float extent = hud_extent(4.0F / 3.0F);
    require(hud_object_shift(HudAnchor::left, extent, 320.0F) < -53.3F &&
                hud_object_shift(HudAnchor::left, extent, 320.0F) > -53.4F,
        "left shift");
    require(hud_object_shift(HudAnchor::right, extent, 320.0F) ==
                -hud_object_shift(HudAnchor::left, extent, 320.0F),
        "right shift mirrors left");
    require(hud_object_shift(HudAnchor::none, extent, 320.0F) == 0.0F,
        "no shift when centred");

    // gEXSetRectAlign(cmd, origin, origin, x, 0, x, 0) encodings.
    const auto left = hud_rect_align_command(HudAnchor::left, 320U);
    require(left[0] == 0x64000006U && left[1] == 0U && left[2] == 0U &&
                left[3] == 0U,
        "left alignment command");
    const auto right = hud_rect_align_command(HudAnchor::right, 320U);
    require(right[0] == 0x64000006U && right[1] == 0x00400400U &&
                right[2] == 0xFB000000U && right[3] == 0xFB000000U,
        "right alignment command offsets by -320 px");
    const auto none = hud_rect_align_command(HudAnchor::none, 320U);
    require(none[1] == 0x00800800U && none[2] == 0U && none[3] == 0U,
        "reset command");

    // gDPSetScissor(G_SC_NON_INTERLACE, 0, 0, 320, 240) and a window.
    require(hud_scissor_full_width(0xED000000U, (1280U << 12U) | 960U, 320U),
        "full-width scissor");
    require(!hud_scissor_full_width(0xED000000U | (96U << 12U),
                                    (1280U << 12U) | 960U, 320U),
        "scissor with a left margin");
    require(!hud_scissor_full_width(0xED000000U, (800U << 12U) | 960U, 320U),
        "scissor with a right margin");

    require(hud_command_words(0x64000006U, 0x00400400U) == 2U,
        "counted list consumes both rectangle alignment words");
    require(hud_command_words(0x64000005U, 0x01000002U) == 2U,
        "counted list consumes both scissor words");
    require(hud_command_words(0x64000017U, 0U) == 1U &&
        hud_command_words(0x64000018U, 0U) == 1U, "counted scissor stack commands");
    require(hud_command_words(0x64000099U, 0U) == 0U &&
        hud_command_words(0x64000017U, 1U) == 0U &&
        hud_command_words(0x64000006U, 0xFFFFFFFFU) == 0U &&
        hud_command_words(0x64000005U, 0xFFFFFFFFU) == 0U &&
        hud_command_words(0xE4000006U, 0U) == 0U,
        "raw data and invalid extension headers stay data");

    require(hud_viewport_align_command(HudAnchor::left, 320U) ==
        std::array<std::uint32_t, 4U>{0x64000007U, 0U, 0U, 0U},
        "left viewport origin shares rectangle coordinates");
    require(hud_viewport_align_command(HudAnchor::right, 320U) ==
        std::array<std::uint32_t, 4U>{0x64000007U, 0x400U, 0xFB000000U, 0U},
        "right viewport subtracts one framebuffer width");
    require(hud_viewport_align_command(HudAnchor::none, 320U) ==
        std::array<std::uint32_t, 4U>{0x64000007U, 0x800U, 0U, 0U},
        "unanchored viewport resets alignment");
    require(hud_command_words(0x64000007U, 0x800U) == 2U &&
        hud_command_words(0x64000007U, 0x801U) == 0U &&
        hud_command_words(0x64000015U, 0U) == 1U &&
        hud_command_words(0x64000016U, 1U) == 0U,
        "counted viewport commands validate origins and stack payloads");

    const auto clip = hud_clip_push_command({12U, 948U, 2U});
    require(clip == std::array<std::uint32_t, 6U>{0x64000017U, 0U,
        0x64000005U, 0x01000002U, 12U, 948U},
        "HUD clipping stretches both horizontal edges and preserves Y/mode");
    require(kHudClipPopCommand == std::array<std::uint32_t, 2U>{0x64000018U, 0U},
        "HUD clipping restores the prior rectangle and origins");

    // Last frame's bounds decide this frame's anchor; nesting is innermost.
    WidescreenHud hud;
    hud.begin_frame(true, 320.0F, 0x80200000U);
    require(!hud.in_group() && hud.anchor() == HudAnchor::none,
        "outside any group");
    hud.enter(0x00E00B30U);
    hud.add(150.0F, 170.0F);
    hud.enter(0x00600000U);
    hud.add(16.0F, 90.0F);
    require(hud.anchor() == HudAnchor::none, "first frame is unanchored");
    hud.exit();
    hud.add(160.0F, 160.0F);
    hud.exit();
    hud.add(300.0F, 300.0F);  // Outside every group: ignored.
    require(!hud.in_group(), "stack unwound");

    hud.begin_frame(true, 320.0F, 0x80200000U);
    require(hud.scan_cursor() == 0x80200000U && hud.scissor_full(),
        "scan restarts each frame");
    require(hud.anchor_of(0x00600000U) == HudAnchor::left, "health anchors left");
    require(hud.anchor_of(0x00E00B30U) == HudAnchor::none,
        "the outer group keeps its own bounds");
    hud.enter(0x00600000U);
    require(hud.anchor() == HudAnchor::left, "anchor applies inside the group");
    hud.exit();

    hud.enter(0x00E01AD8U);
    hud.add(-30.0F, 310.0F);
    hud.exit();
    hud.enter(0x00E0292CU);
    hud.add(-50.0F, 110.0F);
    hud.exit();
    hud.begin_frame(true, 320.0F, 0U);
    require(hud.anchor_of(0x00E01AD8U) == HudAnchor::left &&
        hud.anchor_of(0x00E0292CU) == HudAnchor::left,
        "expanded weapon panel and sliding ammo counters retain a shared edge");

    for(const auto bounds : {std::array<float,2>{4.0F,24.0F},
                              std::array<float,2>{296.0F,316.0F}}) {
        hud.enter(0x00D004A8U);
        hud.add(bounds[0],bounds[1]);
        hud.exit();
        hud.begin_frame(true,320.0F,0U);
        require(hud.anchor_of(0x00D004A8U)==HudAnchor::none,
            "projected reticle near either edge stays in camera coordinates");
    }

    hud.begin_frame(false, 320.0F, 0U);
    require(hud.anchor_of(0x00600000U) == HudAnchor::none,
        "disabling clears anchors");
    return 0;
}
