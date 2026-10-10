// RGO display settings: video.ini format and the resolution/aspect math.
#include "jfg/renderer/video_settings.hpp"
#include "jfg/renderer/widescreen.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) < tolerance;
}

constexpr jfg::PixelSize kFramebuffer{320U, 240U};

// Everything the renderer shell decides for one window, as it decides it.
struct Frame {
    double target;
    jfg::PixelSize picture;
    double multiplier;
    jfg::PixelSize rendered;
};

Frame plan(const jfg::VideoSettings& settings, const jfg::PixelSize window,
           const jfg::PixelSize display = kFramebuffer) {
    const bool wide = settings.aspect == jfg::AspectMode::widescreen;
    const double source = 4.0 / 3.0;
    const double target = wide
        ? jfg::widescreen_aspect_target(window.width, window.height,
              kFramebuffer.width, kFramebuffer.height, display.width,
              display.height, settings.max_aspect.value())
        : source;
    const auto picture = jfg::fit_picture(
        window, jfg::presented_aspect(target, kFramebuffer, display));
    const double multiplier =
        jfg::render_multiplier(settings, picture, kFramebuffer.height);
    return {target, picture, multiplier,
            jfg::render_size(multiplier, kFramebuffer, target / source)};
}

bool square_pixels(const Frame& frame) {
    // Rendered pixels per presented pixel must match in both directions.
    const double x = double(frame.rendered.width) / double(frame.picture.width);
    const double y = double(frame.rendered.height) / double(frame.picture.height);
    return near(x, y, 0.01);
}

}  // namespace

int main() {
    using namespace jfg;

    // --- video.ini format ------------------------------------------------
    const VideoSettings defaults{};
    require(defaults.aspect == AspectMode::widescreen, "widescreen default");
    require(defaults.max_aspect.unlimited(), "unlimited default");
    require(defaults.render == RenderMode::match, "match output default");
    require(defaults.msaa == 1U && defaults.vsync && !defaults.fps_counter,
        "msaa off, vsync on, fps counter off by default");
    require(encode_video_settings(defaults) ==
                "version=2\naspect=widescreen\nmax_aspect=unlimited\n"
                "fullscreen=borderless\noutput=auto\nrender=match\n"
                "render_scale=100\nrender_size=1920x1080\nmsaa=off\n"
                "vsync=on\nfps_counter=off\nframerate=original\n"
                "interpolate_objects=off\nn64_slowdown=off\n",
        "default encoding");
    require(!defaults.n64_slowdown && cpu_overclock_shift(defaults) == kNoSlowdownOverclockShift,
        "no N64 slowdown by default");
    {
        VideoSettings slow;
        slow.n64_slowdown = true;
        require(cpu_overclock_shift(slow) == 0U, "slowdown on: original CPU speed");
        require(parse_video_settings(encode_video_settings(slow))->n64_slowdown,
            "n64_slowdown round trip");
        require(parse_video_settings("version=2\nvsync=on\n")->n64_slowdown == false,
            "files without the key keep the default");
    }
    require(defaults.framerate == FrameRate::original && !defaults.interpolate_objects,
        "original frame rate by default");

    VideoSettings custom;
    custom.aspect = AspectMode::original;
    custom.max_aspect = {21U, 9U};
    custom.fullscreen = FullscreenMode::exclusive;
    custom.output = {2560U, 1080U};
    custom.render = RenderMode::custom;
    custom.render_scale = 35U;
    custom.render_size = {1366U, 768U};
    custom.msaa = 8U;
    custom.vsync = false;
    custom.fps_counter = true;
    custom.framerate = FrameRate::display;
    custom.interpolate_objects = true;
    custom.n64_slowdown = true;
    require(parse_video_settings(encode_video_settings(custom)) == custom,
        "full round trip");
    for (const auto scale : {25U, 100U, 155U, 200U}) {
        VideoSettings s;
        s.render = RenderMode::scale;
        s.render_scale = scale;
        require(parse_video_settings(encode_video_settings(s)) == s,
            "render scale round trip");
    }
    for (const auto rate : {FrameRate::original, FrameRate::sixty,
                            FrameRate::display, FrameRate::unlocked}) {
        VideoSettings s;
        s.framerate = rate;
        require(parse_video_settings(encode_video_settings(s))->framerate == rate,
            "frame rate round trip");
    }
    for (const std::uint8_t samples : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{8}}) {
        VideoSettings s;
        s.msaa = samples;
        require(parse_video_settings(encode_video_settings(s))->msaa == samples,
            "msaa round trip");
    }

    // Version 1 files from the first widescreen PR still load.
    require(parse_video_settings("version=1\naspect=original\n")->aspect ==
                AspectMode::original,
        "version 1 original");
    require(parse_video_settings("version=1\r\naspect=widescreen\r\n") ==
                VideoSettings{},
        "version 1 CRLF");
    // Missing keys keep defaults; unknown keys (newer builds) are ignored.
    require(parse_video_settings("version=2\nvsync=off\n")->vsync == false,
        "partial file");
    require(parse_video_settings("version=2\nfuture_option=7\nvsync=off\n")
                ->vsync == false,
        "unknown keys ignored");
    require(parse_video_settings("version=2\nframerate=60\n")->framerate ==
                FrameRate::sixty,
        "framerate=60");
    for (const char* bad : {
             "", "version=1\naspect=original",  // no final line break
             "version=3\n", "aspect=original\nversion=2\n",
             "version=2\naspect=wide\n", "version=2\naspect=original\naspect=original\n",
             "version=2\nmax_aspect=1:1\n", "version=2\nmax_aspect=0:9\n",
             "version=2\nmax_aspect=21/9\n", "version=2\noutput=100x100\n",
             "version=2\noutput=1920x\n", "version=2\noutput=99999x1080\n",
             "version=2\nrender=fast\n", "version=2\nrender_scale=10\n",
             "version=2\nrender_scale=201\n", "version=2\nrender_scale=-5\n",
             "version=2\nrender_size=100x100\n", "version=2\nmsaa=16x\n",
             "version=2\nvsync=yes\n", "version=2\nfps_counter=\n",
             "version=2\n=on\n", "version=2\nnoequals\n",
             "version=2\nBad-Key=1\n", "version=2\nframerate=144\n",
             "version=2\ninterpolate_objects=yes\n", "version=2\nn64_slowdown=1\n"})
        require(!parse_video_settings(bad), "invalid settings rejected");
    require(!parse_video_settings(std::string(2000U, 'x')), "oversized file");

    require(parse_aspect_limit("16:10")->value() == 1.6, "16:10 limit");
    require(parse_aspect_limit("4:3").has_value(), "4:3 limit allowed");
    require(!parse_aspect_limit("5:4"), "narrower than 4:3 rejected");
    require(parse_typed_size(" 3440", "1440 ", kMinimumOutputDimension) ==
                PixelSize{3440U, 1440U},
        "typed size");
    require(!parse_typed_size("3440", "abc", kMinimumOutputDimension),
        "typed garbage rejected");
    require(!parse_typed_size("200", "1440", kMinimumOutputDimension),
        "typed size too small");

    // --- picture fitting -------------------------------------------------
    require(fit_picture({1920U, 1080U}, 4.0 / 3.0) == PixelSize{1440U, 1080U},
        "4:3 pillarboxed in 16:9");
    require(fit_picture({800U, 1280U}, 4.0 / 3.0) == PixelSize{800U, 600U},
        "4:3 letterboxed in portrait");
    require(fit_picture({5120U, 1440U}, 16.0 / 9.0) == PixelSize{2560U, 1440U},
        "16:9 limit on 32:9");
    require(near(presented_aspect(4.0 / 3.0, kFramebuffer, {320U, 180U}),
                 16.0 / 9.0),
        "game widescreen mode presents 16:9");

    // --- the brief's resolutions, widescreen, Match output ----------------
    struct Case {
        PixelSize window;
        PixelSize picture;  // expected presented picture
    };
    const Case cases[] = {
        {{1920U, 1080U}, {1920U, 1080U}},  // 16:9
        {{2560U, 1440U}, {2560U, 1440U}},
        {{3840U, 2160U}, {3840U, 2160U}},
        {{1280U, 720U}, {1280U, 720U}},
        {{1366U, 768U}, {1366U, 768U}},    // odd 16:9-ish laptop panel
        {{1920U, 1200U}, {1920U, 1200U}},  // 16:10
        {{1280U, 800U}, {1280U, 800U}},
        {{2560U, 1080U}, {2560U, 1080U}},  // 21:9 (64:27)
        {{3440U, 1440U}, {3440U, 1440U}},  // 21:9 (43:18)
        {{3840U, 1600U}, {3840U, 1600U}},  // 24:10
        {{5120U, 1440U}, {5120U, 1440U}},  // 32:9
        {{5760U, 1080U}, {5760U, 1080U}},  // triple 16:9
        {{1024U, 768U}, {1024U, 768U}},    // 4:3 itself
        {{1280U, 1024U}, {1280U, 960U}},   // 5:4: letterboxed 4:3
        {{800U, 1280U}, {800U, 600U}},     // portrait: letterboxed 4:3
        {{1080U, 1920U}, {1080U, 810U}},   // portrait phone shape
        {{1001U, 579U}, {1001U, 579U}},    // arbitrary dragged window
        {{640U, 480U}, {640U, 480U}},
    };
    for (const auto& c : cases) {
        const Frame frame = plan(defaults, c.window);
        require(frame.picture == c.picture, "widescreen picture size");
        // Match output: exactly one rendered pixel per presented pixel.
        require(frame.rendered.height == c.picture.height,
            "match output renders the picture's height");
        require(frame.rendered.width + 1U >= c.picture.width &&
                    frame.rendered.width <= c.picture.width + 1U,
            "match output renders the picture's width");
        require(square_pixels(frame), "square pixels");
        // Hor+: the vertical view never changes, only the width widens.
        require(frame.target >= 4.0 / 3.0 - 1e-9, "never narrower than 4:3");
        require(frame.multiplier >= kMinimumRenderMultiplier &&
                    frame.multiplier <= kMaximumRenderMultiplier,
            "multiplier in range");
    }
    // Exact non-integer multipliers.
    require(near(plan(defaults, {1920U, 1080U}).multiplier, 4.5), "1080p is 4.5x");
    require(near(plan(defaults, {1366U, 768U}).multiplier, 3.2), "768p is 3.2x");
    require(near(plan(defaults, {3440U, 1440U}).target, 3440.0 / 1440.0),
        "3440x1440 aspect");
    require(near(widescreen_horizontal_extent(
                     plan(defaults, {5120U, 1440U}).target, 320U, 240U),
                 (32.0 / 9.0) / (4.0 / 3.0)),
        "32:9 culling extent");

    // Original 4:3 at any window: pillarboxed or letterboxed, never stretched.
    VideoSettings original;
    original.aspect = AspectMode::original;
    for (const auto& c : cases) {
        const Frame frame = plan(original, c.window);
        require(near(double(frame.picture.width) / frame.picture.height,
                     4.0 / 3.0, 0.01),
            "original stays 4:3");
        require(square_pixels(frame), "original square pixels");
    }

    // Max aspect ratio pillarboxes beyond the limit.
    VideoSettings limited;
    limited.max_aspect = {16U, 9U};
    require(plan(limited, {5120U, 1440U}).picture == PixelSize{2560U, 1440U},
        "16:9 limit on 32:9");
    require(plan(limited, {3440U, 1440U}).picture == PixelSize{2560U, 1440U},
        "16:9 limit on 21:9");
    require(plan(limited, {1920U, 1200U}).picture == PixelSize{1920U, 1200U},
        "16:9 limit leaves 16:10 alone");
    limited.max_aspect = {21U, 9U};
    require(plan(limited, {5120U, 1440U}).picture == PixelSize{3360U, 1440U},
        "21:9 limit on 32:9");
    require(square_pixels(plan(limited, {5120U, 1440U})), "limit square pixels");

    // Render scale percentages.
    VideoSettings scaled;
    scaled.render = RenderMode::scale;
    scaled.render_scale = 50U;
    require(plan(scaled, {1920U, 1080U}).rendered == PixelSize{960U, 540U},
        "50% of 1080p");
    scaled.render_scale = 200U;
    require(plan(scaled, {1920U, 1080U}).rendered == PixelSize{3840U, 2160U},
        "200% of 1080p");
    scaled.render_scale = 25U;
    require(plan(scaled, {1280U, 720U}).rendered == PixelSize{320U, 180U},
        "25% of 720p");
    require(plan(scaled, {640U, 400U}).multiplier == kMinimumRenderMultiplier,
        "25% of a tiny window clamps to 120 lines");
    scaled.render_scale = 75U;
    require(square_pixels(plan(scaled, {3440U, 1440U})), "75% square pixels");

    // Custom render size: fitted to the picture's shape.
    VideoSettings fixed;
    fixed.render = RenderMode::custom;
    fixed.render_size = {1280U, 720U};
    require(plan(fixed, {1920U, 1080U}).rendered == PixelSize{1280U, 720U},
        "720p render in a 1080p window");
    require(plan(fixed, {3440U, 1440U}).rendered.width == 1280U,
        "custom size is fitted, width-limited on ultrawide");
    require(square_pixels(plan(fixed, {3440U, 1440U})), "custom square pixels");
    fixed.render_size = {7680U, 4320U};
    require(plan(fixed, {1920U, 1080U}).rendered == PixelSize{7680U, 4320U},
        "8K supersampling");

    // The game's own widescreen option (240 lines shown as 180).
    const Frame stock = plan(defaults, {1920U, 1080U}, {320U, 180U});
    require(near(stock.target, 4.0 / 3.0), "stock widescreen needs no widening");
    require(stock.picture == PixelSize{1920U, 1080U}, "stock fills 16:9");
    require(stock.rendered.height == 1080U, "stock renders the output height");

    // Output size list for the settings page.
    const PixelSize modes[] = {{1920U, 1080U}, {1280U, 720U}, {1920U, 1080U},
                               {640U, 400U},  {2560U, 1440U}, {1280U, 1024U},
                               {800U, 600U}};
    const auto sizes = unique_output_sizes(modes);
    require(sizes.size() == 5U && sizes.front() == PixelSize{2560U, 1440U} &&
                sizes.back() == PixelSize{800U, 600U},
        "unique sorted output sizes");
    return 0;
}
