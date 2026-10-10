#pragma once

// RGO display settings: the profile's video.ini format and the pure display
// math shared by the frontend, the renderer shell, the game hooks and their
// tests. See docs/rgo/display-settings.md.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jfg {

enum class AspectMode : std::uint8_t {
    // The original 4:3 picture (or the game's own letterboxed 16:9 mode).
    original,
    // Widen the 3D view to the window's shape instead of pillarboxing it.
    widescreen,
};

// Fullscreen kind used by F11 and by the Window mode setting.
enum class FullscreenMode : std::uint8_t {
    // A borderless window covering the monitor at its desktop resolution.
    borderless,
    // The same window after switching the monitor to the output resolution.
    exclusive,
};

enum class RenderMode : std::uint8_t {
    // Render exactly as many pixels as the picture covers on screen.
    match,
    // A percentage of the output resolution.
    scale,
    // A fixed render size, fitted to the picture's shape (pixels stay square).
    custom,
};

// RGO frame rate. Above Original the game itself runs its logic and draws
// at 60 per second (its own 1-VI mode); RT64 can blend extra frames on top
// up to the display's rate or uncapped (docs/rgo/framerate.md).
enum class FrameRate : std::uint8_t {
    original,  // the game's own 30 fps (20 in its slow mode)
    sixty,     // true 60: game logic and drawing at 60 per second
    display,   // true 60, blended up to the display's refresh rate
    unlocked,  // true 60, blended as fast as RT64 can render
};

struct PixelSize {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return width != 0U && height != 0U;
    }
    friend bool operator==(const PixelSize&, const PixelSize&) = default;
};

// Widest picture the widescreen mode renders. "Unlimited" stops here, beyond
// any real arrangement of displays (72:9); RT64 accepts up to 100:1.
inline constexpr double kMaximumWidescreenAspect = 8.0;

// The widest picture a player allows; wider windows are pillarboxed.
struct AspectLimit {
    std::uint16_t numerator = 0U;    // 0: unlimited
    std::uint16_t denominator = 0U;
    [[nodiscard]] constexpr bool unlimited() const noexcept {
        return numerator == 0U || denominator == 0U;
    }
    [[nodiscard]] constexpr double value() const noexcept {
        return unlimited() ? kMaximumWidescreenAspect
                           : double(numerator) / double(denominator);
    }
    friend bool operator==(const AspectLimit&, const AspectLimit&) = default;
};

inline constexpr std::uint32_t kMinimumRenderScale = 25U;
inline constexpr std::uint32_t kMaximumRenderScale = 200U;
inline constexpr std::uint32_t kMinimumOutputDimension = 320U;
inline constexpr std::uint32_t kMinimumRenderDimension = 160U;
inline constexpr std::uint32_t kMaximumDimension = 16384U;

struct VideoSettings {
    AspectMode aspect = AspectMode::widescreen;
    AspectLimit max_aspect{};
    FullscreenMode fullscreen = FullscreenMode::borderless;
    // Windowed: the game area size chosen last (applied when chosen).
    // Exclusive fullscreen: the display mode. {0, 0}: the desktop resolution.
    PixelSize output{};
    RenderMode render = RenderMode::match;
    std::uint32_t render_scale = 100U;  // percent, RenderMode::scale
    PixelSize render_size{1920U, 1080U};  // RenderMode::custom
    std::uint8_t msaa = 1U;  // sample count: 1, 2, 4 or 8
    bool vsync = true;
    bool fps_counter = false;
    FrameRate framerate = FrameRate::original;
    // Blend extra frames above the game's rate (Display / Unlocked).
    bool interpolate_frames = true;
    // Also blend moving objects' matrices. Without it the camera blends but
    // objects (the player the camera follows) step at the game's rate, which
    // shakes them against the scene on displays above 60 Hz.
    bool interpolate_objects = true;
    // Reproduce the N64's slowdown at the Original frame rate: heavy scenes
    // drop to 20 fps because the emulated CPU needs more than two VIs for a
    // frame. Off (the default) uses the fast clock, so the game keeps its
    // full frame rate. True 60 always uses the fast clock.
    bool n64_slowdown = false;
    friend bool operator==(const VideoSettings&, const VideoSettings&) = default;
};

// Profile file shared by the launcher and the game: "video.ini".
inline constexpr char kVideoSettingsFileName[] = "video.ini";
inline constexpr std::size_t kVideoSettingsMaximumBytes = 1024U;

namespace video_settings_detail {

[[nodiscard]] inline std::optional<std::uint32_t> parse_number(
    std::string_view text) noexcept {
    if (text.empty() || text.size() > 6U)
        return std::nullopt;
    std::uint32_t value = 0U;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

[[nodiscard]] inline std::optional<std::pair<std::uint32_t, std::uint32_t>>
parse_pair(std::string_view text, const char separator) noexcept {
    const std::size_t split = text.find(separator);
    if (split == std::string_view::npos)
        return std::nullopt;
    const auto first = parse_number(text.substr(0U, split));
    const auto second = parse_number(text.substr(split + 1U));
    if (!first || !second)
        return std::nullopt;
    return std::pair{*first, *second};
}

[[nodiscard]] inline std::optional<PixelSize> parse_size(
    std::string_view text, const std::uint32_t minimum) noexcept {
    const auto pair = parse_pair(text, 'x');
    if (!pair || pair->first < minimum || pair->second < minimum ||
        pair->first > kMaximumDimension || pair->second > kMaximumDimension)
        return std::nullopt;
    return PixelSize{pair->first, pair->second};
}

[[nodiscard]] inline std::string size_text(const PixelSize size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

}  // namespace video_settings_detail

// "W:H" with a ratio of at least 4:3, or "unlimited".
[[nodiscard]] inline std::optional<AspectLimit> parse_aspect_limit(
    std::string_view text) noexcept {
    if (text == "unlimited")
        return AspectLimit{};
    const auto pair = video_settings_detail::parse_pair(text, ':');
    if (!pair || pair->first == 0U || pair->second == 0U ||
        pair->first > 1000U || pair->second > 1000U)
        return std::nullopt;
    // Narrower than the original picture would mean nothing to limit.
    if (std::uint64_t(pair->first) * 3U < std::uint64_t(pair->second) * 4U)
        return std::nullopt;
    return AspectLimit{static_cast<std::uint16_t>(pair->first),
                       static_cast<std::uint16_t>(pair->second)};
}

[[nodiscard]] inline std::string aspect_limit_text(const AspectLimit limit) {
    return limit.unlimited() ? std::string("unlimited")
                             : std::to_string(limit.numerator) + ":" +
                                   std::to_string(limit.denominator);
}

// Width and height typed into the settings page's two fields; spaces around
// the numbers are ignored.
[[nodiscard]] inline std::optional<PixelSize> parse_typed_size(
    std::string_view width, std::string_view height,
    const std::uint32_t minimum) noexcept {
    auto trim = [](std::string_view text) {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
        while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
        return text;
    };
    const auto w = video_settings_detail::parse_number(trim(width));
    const auto h = video_settings_detail::parse_number(trim(height));
    if (!w || !h || *w < minimum || *h < minimum || *w > kMaximumDimension ||
        *h > kMaximumDimension)
        return std::nullopt;
    return PixelSize{*w, *h};
}

[[nodiscard]] inline std::string encode_video_settings(
    const VideoSettings& settings) {
    using video_settings_detail::size_text;
    std::string text = "version=2\naspect=";
    text += settings.aspect == AspectMode::original ? "original" : "widescreen";
    text += "\nmax_aspect=" + aspect_limit_text(settings.max_aspect);
    text += "\nfullscreen=";
    text += settings.fullscreen == FullscreenMode::exclusive ? "exclusive"
                                                             : "borderless";
    text += "\noutput=";
    text += settings.output.valid() ? size_text(settings.output) : "auto";
    text += "\nrender=";
    text += settings.render == RenderMode::scale    ? "scale"
            : settings.render == RenderMode::custom ? "custom"
                                                    : "match";
    text += "\nrender_scale=" + std::to_string(settings.render_scale);
    text += "\nrender_size=" + size_text(settings.render_size);
    text += "\nmsaa=";
    text += settings.msaa <= 1U ? std::string("off")
                                : std::to_string(settings.msaa) + "x";
    text += settings.vsync ? "\nvsync=on" : "\nvsync=off";
    text += settings.fps_counter ? "\nfps_counter=on" : "\nfps_counter=off";
    text += "\nframerate=";
    text += settings.framerate == FrameRate::sixty      ? "60"
            : settings.framerate == FrameRate::display  ? "display"
            : settings.framerate == FrameRate::unlocked ? "unlocked"
                                                        : "original";
    text += settings.interpolate_frames ? "\ninterpolate_frames=on"
                                        : "\ninterpolate_frames=off";
    text += settings.interpolate_objects ? "\ninterpolate_objects=on"
                                         : "\ninterpolate_objects=off";
    text += settings.n64_slowdown ? "\nn64_slowdown=on\n" : "\nn64_slowdown=off\n";
    return text;
}

// Reads what encode_video_settings writes (version 2) and the original
// version 1 file (aspect only), with LF or CRLF line endings. Missing keys keep
// their defaults and unknown keys are ignored, so an older game build can read
// a newer file. A malformed value, a duplicate key or a missing final line
// break rejects the whole file, so a partial write keeps the last settings.
[[nodiscard]] inline std::optional<VideoSettings> parse_video_settings(
    std::string_view text) noexcept {
    using namespace video_settings_detail;
    if (text.size() > kVideoSettingsMaximumBytes)
        return std::nullopt;
    VideoSettings settings{};
    std::uint32_t seen = 0U;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        if (end == std::string_view::npos)
            return std::nullopt;  // The final line must be terminated.
        std::string_view line = text.substr(0U, end);
        text.remove_prefix(end + 1U);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1U);
        if (first) {
            if (line != "version=1" && line != "version=2")
                return std::nullopt;
            first = false;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || equals == 0U)
            return std::nullopt;
        const std::string_view key = line.substr(0U, equals);
        const std::string_view value = line.substr(equals + 1U);
        constexpr std::array<std::string_view, 14U> keys{
            "aspect", "max_aspect", "fullscreen", "output", "render",
            "render_scale", "render_size", "msaa", "vsync", "fps_counter",
            "framerate", "interpolate_objects", "n64_slowdown",
            "interpolate_frames"};
        const auto found = std::find(keys.begin(), keys.end(), key);
        if (found == keys.end()) {
            for (const char ch : key)
                if (!((ch >= 'a' && ch <= 'z') || ch == '_' ||
                      (ch >= '0' && ch <= '9')))
                    return std::nullopt;
            continue;  // A newer setting this build does not know.
        }
        const auto bit = 1U << static_cast<unsigned>(found - keys.begin());
        if ((seen & bit) != 0U)
            return std::nullopt;
        seen |= bit;
        auto on_off = [&](bool& target) {
            if (value == "on") target = true;
            else if (value == "off") target = false;
            else return false;
            return true;
        };
        bool ok = true;
        if (key == "aspect") {
            if (value == "original") settings.aspect = AspectMode::original;
            else if (value == "widescreen") settings.aspect = AspectMode::widescreen;
            else ok = false;
        } else if (key == "max_aspect") {
            const auto limit = parse_aspect_limit(value);
            ok = limit.has_value();
            if (ok) settings.max_aspect = *limit;
        } else if (key == "fullscreen") {
            if (value == "borderless") settings.fullscreen = FullscreenMode::borderless;
            else if (value == "exclusive") settings.fullscreen = FullscreenMode::exclusive;
            else ok = false;
        } else if (key == "output") {
            if (value == "auto") {
                settings.output = {};
            } else {
                const auto size = parse_size(value, kMinimumOutputDimension);
                ok = size.has_value();
                if (ok) settings.output = *size;
            }
        } else if (key == "render") {
            if (value == "match") settings.render = RenderMode::match;
            else if (value == "scale") settings.render = RenderMode::scale;
            else if (value == "custom") settings.render = RenderMode::custom;
            else ok = false;
        } else if (key == "render_scale") {
            const auto scale = parse_number(value);
            ok = scale && *scale >= kMinimumRenderScale &&
                 *scale <= kMaximumRenderScale;
            if (ok) settings.render_scale = *scale;
        } else if (key == "render_size") {
            const auto size = parse_size(value, kMinimumRenderDimension);
            ok = size.has_value();
            if (ok) settings.render_size = *size;
        } else if (key == "msaa") {
            if (value == "off") settings.msaa = 1U;
            else if (value == "2x") settings.msaa = 2U;
            else if (value == "4x") settings.msaa = 4U;
            else if (value == "8x") settings.msaa = 8U;
            else ok = false;
        } else if (key == "vsync") {
            ok = on_off(settings.vsync);
        } else if (key == "fps_counter") {
            ok = on_off(settings.fps_counter);
        } else if (key == "framerate") {
            if (value == "original") settings.framerate = FrameRate::original;
            else if (value == "60") settings.framerate = FrameRate::sixty;
            else if (value == "display") settings.framerate = FrameRate::display;
            else if (value == "unlocked") settings.framerate = FrameRate::unlocked;
            else ok = false;
        } else if (key == "interpolate_objects") {
            ok = on_off(settings.interpolate_objects);
        } else if (key == "interpolate_frames") {
            ok = on_off(settings.interpolate_frames);
        } else {
            ok = on_off(settings.n64_slowdown);
        }
        if (!ok)
            return std::nullopt;
    }
    if (first)
        return std::nullopt;  // Empty file.
    return settings;
}

[[nodiscard]] constexpr bool true_logic_60(const VideoSettings& settings) noexcept {
    return settings.framerate != FrameRate::original;
}

// The emulated CPU's clock (docs/rgo/framerate.md). The fast clock charges
// almost no emulated time for CPU work and skips waits to the next device
// event, so a game frame never needs more than its minimum VIs: 2 at
// Original (a steady 30 fps), 1 at true 60. The original VR4300 cost model
// (the N64's own slowdown to 20 fps) is kept only for Original N64 slowdown
// at the Original frame rate.
[[nodiscard]] constexpr bool fast_clock(const VideoSettings& settings) noexcept {
    return true_logic_60(settings) || !settings.n64_slowdown;
}

// ---------------------------------------------------------------------------
// Display math. "Framebuffer" is the game's VI framebuffer (320x240); its
// "display" shape is what the VI scans it out as (320x240, or 320x180 in the
// game's own widescreen mode). RT64 widens the picture by target / source.

// The picture shape RT64 presents for an aspect target.
[[nodiscard]] inline double presented_aspect(
    const double target, const PixelSize framebuffer,
    const PixelSize display) noexcept {
    if (!framebuffer.valid() || !display.valid())
        return target > 0.0 ? target : 4.0 / 3.0;
    const double source = double(framebuffer.width) / double(framebuffer.height);
    const double shape = double(display.width) / double(display.height);
    return target * shape / source;
}

// The largest rectangle of the given shape inside the window: the picture
// fills the window's height (pillarbox) or its width (letterbox).
[[nodiscard]] inline PixelSize fit_picture(
    const PixelSize window, const double aspect) noexcept {
    if (!window.valid() || !(aspect > 0.0) || !std::isfinite(aspect))
        return window;
    const double window_aspect = double(window.width) / double(window.height);
    if (window_aspect >= aspect) {
        const auto width = static_cast<std::uint32_t>(
            std::lround(double(window.height) * aspect));
        return {std::clamp(width, 1U, window.width), window.height};
    }
    const auto height = static_cast<std::uint32_t>(
        std::lround(double(window.width) / aspect));
    return {window.width, std::clamp(height, 1U, window.height)};
}

inline constexpr double kMinimumRenderMultiplier = 0.5;   // 120 lines
inline constexpr double kMaximumRenderMultiplier = 32.0;  // RT64's limit

// RT64's resolution multiplier (rendered lines / framebuffer lines) for the
// picture the window shows. Match output renders exactly the picture's pixels;
// non-integer multipliers are allowed (RT64 Resolution::Manual).
[[nodiscard]] inline double render_multiplier(
    const VideoSettings& settings, const PixelSize picture,
    const std::uint32_t framebuffer_height) noexcept {
    if (!picture.valid() || framebuffer_height == 0U)
        return 1.0;
    const double base = double(picture.height) / double(framebuffer_height);
    double multiplier = base;
    if (settings.render == RenderMode::scale) {
        multiplier = base * double(std::clamp(settings.render_scale,
            kMinimumRenderScale, kMaximumRenderScale)) / 100.0;
    } else if (settings.render == RenderMode::custom &&
               settings.render_size.valid()) {
        // Fit the requested size to the picture's shape: square pixels.
        const double fit = (std::min)(
            double(settings.render_size.width) / double(picture.width),
            double(settings.render_size.height) / double(picture.height));
        multiplier = base * fit;
    }
    if (!std::isfinite(multiplier))
        return 1.0;
    return std::clamp(multiplier, kMinimumRenderMultiplier,
                      kMaximumRenderMultiplier);
}

// The size RT64 renders the scene at, for display in the settings page.
[[nodiscard]] inline PixelSize render_size(
    const double multiplier, const PixelSize framebuffer,
    const double horizontal_scale) noexcept {
    if (!framebuffer.valid())
        return {};
    return {static_cast<std::uint32_t>(std::lround(
                double(framebuffer.width) * multiplier * horizontal_scale)),
            static_cast<std::uint32_t>(
                std::lround(double(framebuffer.height) * multiplier))};
}

// Output sizes for the settings list: largest first, duplicates (refresh
// rates, color depths) removed, nothing smaller than 640x480.
[[nodiscard]] inline std::vector<PixelSize> unique_output_sizes(
    std::span<const PixelSize> modes) {
    std::vector<PixelSize> sizes;
    for (const auto size : modes)
        if (size.width >= 640U && size.height >= 480U &&
            size.width <= kMaximumDimension && size.height <= kMaximumDimension)
            sizes.push_back(size);
    std::sort(sizes.begin(), sizes.end(), [](PixelSize a, PixelSize b) {
        const auto area_a = std::uint64_t(a.width) * a.height;
        const auto area_b = std::uint64_t(b.width) * b.height;
        return area_a != area_b ? area_a > area_b : a.width > b.width;
    });
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

}  // namespace jfg
