#pragma once

// RGO frame-rate diagnostics (Video settings → FPS counter: Detailed). The
// game measures what it actually does each second and reports it to the
// frontend, so a frame rate that "feels like 30" can be told apart:
// - logic updates: viFrameSync calls (one per game frame), and the VI ticks
//   they hand to the game logic (60 per second at full speed in any mode);
// - drawn: game frames presented with a freshly drawn picture;
// - new: drawn frames whose matrices (scene or camera) differ from the
//   previous drawn frame's; camera: frames whose camera matrices differ;
// - repeats: presents that showed a picture already shown;
// - input: controller samples the game received (JFG starts the next read
//   only after the previous one completed);
// - frame times: the shortest and longest gap between game presents and
//   between swap-chain presents.
// See docs/rgo/framerate.md ("Diagnostics").

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jfg {

// Windows messages after the basic FPS report (WM_APP + 24): WM_APP + 25 to
// WM_APP + 29 carry FrameStats, two 32-bit values each.
inline constexpr unsigned kFrameStatsMessageOffset = 24U;
inline constexpr unsigned kFrameDetailMessageOffset = 25U;
inline constexpr unsigned kFrameDetailMessageCount = 5U;

struct FrameStats {
    // Per second, in tenths.
    std::uint32_t logic_tenths = 0U;
    std::uint32_t ticks_tenths = 0U;
    std::uint32_t drawn_tenths = 0U;
    std::uint32_t new_tenths = 0U;
    std::uint32_t camera_tenths = 0U;
    std::uint32_t repeat_tenths = 0U;
    std::uint32_t input_tenths = 0U;
    // Gaps between presents in the last half second, in tenths of a
    // millisecond (0: none measured).
    std::uint32_t frame_min = 0U;
    std::uint32_t frame_max = 0U;
    std::uint32_t display_min = 0U;
    std::uint32_t display_max = 0U;
    friend bool operator==(const FrameStats&, const FrameStats&) = default;
};

// Two 16-bit values in one 32-bit message word (milliseconds in tenths).
[[nodiscard]] constexpr std::uint32_t pack_pair(const std::uint32_t low,
                                                const std::uint32_t high) noexcept {
    return (std::min)(low, 0xFFFFU) | ((std::min)(high, 0xFFFFU) << 16U);
}

// The four messages' {wparam, lparam} words, in message order.
[[nodiscard]] constexpr std::array<std::uint32_t, kFrameDetailMessageCount * 2U>
encode_frame_stats(const FrameStats& stats) noexcept {
    return {stats.logic_tenths, stats.ticks_tenths, stats.drawn_tenths,
            stats.new_tenths, stats.camera_tenths, stats.repeat_tenths,
            pack_pair(stats.frame_min, stats.frame_max),
            pack_pair(stats.display_min, stats.display_max), stats.input_tenths, 0U};
}

// Applies message `index` (0 to 4) to `stats`.
constexpr void decode_frame_stats(FrameStats& stats, const unsigned index,
                                  const std::uint32_t first,
                                  const std::uint32_t second) noexcept {
    switch (index) {
    case 0U: stats.logic_tenths = first; stats.ticks_tenths = second; break;
    case 1U: stats.drawn_tenths = first; stats.new_tenths = second; break;
    case 2U: stats.camera_tenths = first; stats.repeat_tenths = second; break;
    case 3U:
        stats.frame_min = first & 0xFFFFU;
        stats.frame_max = first >> 16U;
        stats.display_min = second & 0xFFFFU;
        stats.display_max = second >> 16U;
        break;
    case 4U: stats.input_tenths = first; break;
    default: break;
    }
}

// FNV-1a over 32-bit words: the content hash of the matrices a frame loads.
inline constexpr std::uint64_t kContentHashSeed = 0xCBF29CE484222325ULL;

[[nodiscard]] constexpr std::uint64_t mix_content_hash(
    std::uint64_t hash, const std::span<const std::uint32_t> words) noexcept {
    for (const std::uint32_t word : words) {
        hash ^= word;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

// Shortest and longest interval in microseconds.
struct IntervalRange {
    std::uint64_t minimum = 0U;
    std::uint64_t maximum = 0U;
    void add(const std::uint64_t interval) noexcept {
        minimum = minimum == 0U ? interval : (std::min)(minimum, interval);
        maximum = (std::max)(maximum, interval);
    }
    // In tenths of a millisecond, rounded.
    [[nodiscard]] std::uint32_t minimum_tenths() const noexcept {
        return static_cast<std::uint32_t>((std::min<std::uint64_t>)((minimum + 50U) / 100U, 0xFFFFU));
    }
    [[nodiscard]] std::uint32_t maximum_tenths() const noexcept {
        return static_cast<std::uint32_t>((std::min<std::uint64_t>)((maximum + 50U) / 100U, 0xFFFFU));
    }
};

// Follows what each framebuffer last had drawn into it and classifies every
// present: a fresh picture or a repeat, and whether its scene and camera
// differ from the previously presented fresh picture.
class FrameContentTracker {
public:
    struct Presented {
        bool fresh = false;
        bool scene_changed = false;
        bool camera_changed = false;
    };

    // A graphics task drew into `color_image` with these matrix hashes.
    // Several tasks into the same image before a present combine.
    void drawn(const std::uint32_t color_image, const std::uint64_t scene,
               const std::uint64_t camera) noexcept {
        if (color_image == 0U)
            return;
        Entry* entry = find(color_image);
        if (entry == nullptr) {
            entry = &entries_[next_];
            next_ = (next_ + 1U) % entries_.size();
            *entry = Entry{color_image};
        }
        if (!entry->fresh) {
            entry->scene = kContentHashSeed;
            entry->camera = kContentHashSeed;
            entry->fresh = true;
        }
        const std::array<std::uint32_t, 2> scene_words{
            static_cast<std::uint32_t>(scene), static_cast<std::uint32_t>(scene >> 32U)};
        const std::array<std::uint32_t, 2> camera_words{
            static_cast<std::uint32_t>(camera), static_cast<std::uint32_t>(camera >> 32U)};
        entry->scene = mix_content_hash(entry->scene, scene_words);
        entry->camera = mix_content_hash(entry->camera, camera_words);
    }

    // `color_image` was presented.
    [[nodiscard]] Presented presented(const std::uint32_t color_image) noexcept {
        Entry* entry = color_image != 0U ? find(color_image) : nullptr;
        if (entry == nullptr || !entry->fresh)
            return {};
        entry->fresh = false;
        const Presented result{true, !have_last_ || entry->scene != last_scene_,
                               !have_last_ || entry->camera != last_camera_};
        last_scene_ = entry->scene;
        last_camera_ = entry->camera;
        have_last_ = true;
        return result;
    }

private:
    struct Entry {
        std::uint32_t color_image = 0U;
        std::uint64_t scene = kContentHashSeed;
        std::uint64_t camera = kContentHashSeed;
        bool fresh = false;
    };
    [[nodiscard]] Entry* find(const std::uint32_t color_image) noexcept {
        for (Entry& entry : entries_)
            if (entry.color_image == color_image)
                return &entry;
        return nullptr;
    }
    std::array<Entry, 16> entries_{};
    std::size_t next_ = 0U;
    std::uint64_t last_scene_ = 0U;
    std::uint64_t last_camera_ = 0U;
    bool have_last_ = false;
};

}  // namespace jfg
