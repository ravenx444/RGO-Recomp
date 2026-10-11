#pragma once

// RGO FPS counter (docs/rgo/framerate.md, "FPS counter").
//
// The plain counter (FrameCounter): the game reports three rates and the
// displayed frame times to the frontend twice a second, in one message
// (WM_APP + 24):
//   Game    - game logic updates per second (viFrameSync calls). This is the
//             rate the game simulates at, the only one that changes how the
//             game plays and feels.
//   Render  - real game frames handed to the renderer per second.
//   Display - frames the swap chain presented per second, including frames
//             RT64 blended in between (frame interpolation).
//   Frame time min/max - the shortest and longest gap between two presented
//             frames in the window; a wide spread is judder.
// Rates are in tenths of a frame per second, frame times in tenths of a
// millisecond (both saturating).
//
// FPS counter "Detailed" (FrameStats) adds, in eight more messages
// (WM_APP + 25 to 32), what the game actually does each second:
// - logic updates: viFrameSync calls (one per game frame), and the VI ticks
//   they hand to the game logic (60 per second at full speed in any mode);
// - drawn: game frames presented with a freshly drawn picture;
// - new: drawn frames whose matrices (scene or camera) differ from the
//   previous drawn frame's; camera: frames whose camera matrices differ;
// - repeats: presents that showed a picture already shown;
// - input: controller samples the game received (JFG starts the next read
//   only after the previous one completed);
// - frame times: the shortest and longest gap between game presents and
//   between swap-chain presents;
// - host: how long the PC was busy with each game frame (average and
//   longest, VI waits excluded), the graphics tasks per frame and the time
//   their submission took (the renderer's GPU syncs included);
// - VI wake: how late the emulation thread woke from its wait for a VI at
//   worst (a few tenths normally; milliseconds mean the OS sleeps imprecisely);
// - True 60: whether the governor runs 60 or holds 30, its late frames and
//   the busy time 90 % of recent frames stayed within;
// - runtime: device register accesses per second and how many of them
//   needed the slow single-step trap, guest thread switches per second and
//   how many of them needed an operating-system wake-up, and the audio
//   cushion (the queue refilled after an underrun).
// See docs/rgo/framerate.md ("Diagnostics").

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace jfg {

// Windows messages: WM_APP + 24 carries the plain counter (FrameCounter,
// packed), WM_APP + 25 to WM_APP + 32 carry FrameStats, two 32-bit values each.
inline constexpr unsigned kFrameStatsMessageOffset = 24U;
inline constexpr unsigned kFrameDetailMessageOffset = 25U;
inline constexpr unsigned kFrameDetailMessageCount = 8U;

struct FrameCounter {
    std::uint32_t game_tenths = 0U;
    std::uint32_t render_tenths = 0U;
    std::uint32_t display_tenths = 0U;
    std::uint16_t frame_min_tenth_ms = 0U;
    std::uint16_t frame_max_tenth_ms = 0U;
    friend bool operator==(const FrameCounter&, const FrameCounter&) = default;
};

struct PackedFrameCounter {
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
[[nodiscard]] constexpr PackedFrameCounter pack_frame_counter(const FrameCounter& stats) noexcept {
    const auto rate = [](std::uint32_t value) {
        return static_cast<std::uint64_t>(std::min<std::uint32_t>(value, 0xFFFFFFU));
    };
    return {rate(stats.game_tenths) | (rate(stats.render_tenths) << 24U),
            rate(stats.display_tenths) |
                (static_cast<std::uint64_t>(stats.frame_min_tenth_ms) << 24U) |
                (static_cast<std::uint64_t>(stats.frame_max_tenth_ms) << 40U)};
}

[[nodiscard]] constexpr FrameCounter unpack_frame_counter(const PackedFrameCounter packed) noexcept {
    return {static_cast<std::uint32_t>(packed.first & 0xFFFFFFU),
            static_cast<std::uint32_t>((packed.first >> 24U) & 0xFFFFFFU),
            static_cast<std::uint32_t>(packed.second & 0xFFFFFFU),
            static_cast<std::uint16_t>((packed.second >> 24U) & 0xFFFFU),
            static_cast<std::uint16_t>((packed.second >> 40U) & 0xFFFFU)};
}

// "Game 60 | Render 60 | Display 144 | 6.9-7.0 ms"
[[nodiscard]] inline std::string format_frame_counter(const FrameCounter& stats) {
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
    // Host time per game frame without the VI waits, in tenths of a
    // millisecond: average and longest in the last half second.
    std::uint32_t busy_avg = 0U;
    std::uint32_t busy_max = 0U;
    // Graphics tasks per game frame (tenths) and their submission time per
    // game frame (tenths of a millisecond).
    std::uint32_t tasks_per_frame = 0U;
    std::uint32_t submit_per_frame = 0U;
    // True 60 governor: 0 off (Original), 1 running 60, 2 holding 30; late
    // frames of the last 60; busy time of the slowest tenth of the last 60
    // frames (tenths of a millisecond).
    std::uint32_t true60 = 0U;
    std::uint32_t late = 0U;
    std::uint32_t busy_p90 = 0U;
    // How late the emulation thread woke from its VI pacing sleep at worst,
    // in tenths of a millisecond (up to 25.5 ms).
    std::uint32_t wake_late = 0U;
    // Per second: device register accesses, those single-stepped, guest
    // thread switches, those that needed a wake-up. Audio cushion in ms.
    std::uint32_t mmio = 0U;
    std::uint32_t mmio_stepped = 0U;
    std::uint32_t switches = 0U;
    std::uint32_t wakes = 0U;
    std::uint32_t audio_cushion = 0U;
    friend bool operator==(const FrameStats&, const FrameStats&) = default;
};

// True 60 governor state, late frames, busy p90 and the VI wake lateness in
// one message word.
[[nodiscard]] constexpr std::uint32_t pack_true60(const std::uint32_t state,
                                                  const std::uint32_t late,
                                                  const std::uint32_t busy_p90,
                                                  const std::uint32_t wake_late) noexcept {
    return (std::min)(state, 3U) | ((std::min)(late, 63U) << 2U) |
           ((std::min)(busy_p90, 0xFFFFU) << 8U) | ((std::min)(wake_late, 0xFFU) << 24U);
}

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
            pack_pair(stats.display_min, stats.display_max), stats.input_tenths,
            pack_pair(stats.busy_avg, stats.busy_max),
            pack_pair(stats.tasks_per_frame, stats.submit_per_frame),
            pack_true60(stats.true60, stats.late, stats.busy_p90, stats.wake_late),
            stats.mmio, pack_pair(stats.mmio_stepped, stats.audio_cushion),
            stats.switches, stats.wakes};
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
    case 4U:
        stats.input_tenths = first;
        stats.busy_avg = second & 0xFFFFU;
        stats.busy_max = second >> 16U;
        break;
    case 5U:
        stats.tasks_per_frame = first & 0xFFFFU;
        stats.submit_per_frame = first >> 16U;
        stats.true60 = second & 3U;
        stats.late = (second >> 2U) & 63U;
        stats.busy_p90 = (second >> 8U) & 0xFFFFU;
        stats.wake_late = second >> 24U;
        break;
    case 6U:
        stats.mmio = first;
        stats.mmio_stepped = second & 0xFFFFU;
        stats.audio_cushion = second >> 16U;
        break;
    case 7U:
        stats.switches = first;
        stats.wakes = second;
        break;
    default: break;
    }
}

// The Detailed counter's last two lines:
// "host 9.8 ms, longest 14.2 ms | gfx 6.0 tasks, 3.1 ms | VI wake +0.1 ms"
// "True 60: 60, late 2/60, busy 11.0 ms" (or "holding 30", or "off")
// "MMIO 11000/s, stepped 0 | switches 9000/s, woken 2000 | audio 100 ms"
[[nodiscard]] inline std::string format_frame_host(const FrameStats& stats) {
    const auto t = [](std::uint32_t tenths) {
        return std::to_string(tenths / 10U) + "." + std::to_string(tenths % 10U);
    };
    std::string text = "host " + t(stats.busy_avg) + " ms, longest " + t(stats.busy_max) +
                       " ms | gfx " + t(stats.tasks_per_frame) + " tasks, " +
                       t(stats.submit_per_frame) + " ms | VI wake +" + t(stats.wake_late) +
                       " ms";
    text += "\nTrue 60: ";
    if (stats.true60 == 0U) {
        text += "off";
    } else {
        text += stats.true60 == 1U ? "60, late " + std::to_string(stats.late) + "/60"
                                   : std::string("holding 30");
        text += ", busy " + t(stats.busy_p90) + " ms";
    }
    text += "\nMMIO " + std::to_string(stats.mmio) + "/s, stepped " +
            std::to_string(stats.mmio_stepped) + " | switches " +
            std::to_string(stats.switches) + "/s, woken " + std::to_string(stats.wakes) +
            " | audio " + std::to_string(stats.audio_cushion) + " ms";
    return text;
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
