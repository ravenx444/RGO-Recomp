#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "jfg/renderer/video_settings.hpp"

namespace jfg {

inline constexpr char kPinnedRt64Revision[] =
    "5473732a822a4423b5696e7cb18fecc425a59875";
inline constexpr std::size_t kRt64RequiredRdramBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kRt64RequiredHeaderBytes = 0x40U;

enum class Rt64ShellError {
    none,
    invalid_window,
    invalid_memory,
    setup_dynamic_libraries_missing,
    setup_invalid_graphics_api,
    setup_graphics_api_not_found,
    setup_graphics_device_not_found,
    render_hook_unavailable,
    invalid_framebuffer,
    frame_capture_failed,
    invalid_task_address,
    unsupported_microcode,
    unsupported_commands,
    not_initialized,
    renderer_exception,
    conflicting_cpu_write,
};

enum class Rt64MemoryLayout {
    // Canonical N64 byte order, as produced by the private emulator capture.
    big_endian,
    // Four-byte words already reversed for RT64 on a little-endian host.
    host_word_swapped,
};

struct Rt64ViRegisters {
    std::uint32_t status = 0U;
    std::uint32_t origin = 0U;
    std::uint32_t width = 0U;
    std::uint32_t intr = 0U;
    std::uint32_t current_line = 0U;
    std::uint32_t timing = 0U;
    std::uint32_t vertical_sync = 0U;
    std::uint32_t horizontal_sync = 0U;
    std::uint32_t leap = 0U;
    std::uint32_t horizontal_start = 0U;
    std::uint32_t vertical_start = 0U;
    std::uint32_t vertical_burst = 0U;
    std::uint32_t x_scale = 0U;
    std::uint32_t y_scale = 0U;
};

struct Rt64ShellConfiguration {
    void* native_window = nullptr;
    std::uint32_t window_thread_id = 0U;
    // RT64 always receives private snapshots. It cannot mutate simulation
    // memory through these views.
    std::span<const std::byte> rom_header;
    std::span<const std::byte> rdram;
    Rt64ViRegisters* vi = nullptr;
    bool developer_mode = false;
    Rt64MemoryLayout memory_layout = Rt64MemoryLayout::big_endian;
    bool cpu_writeback = false;
    // Return intensity masks to the guest for its CPU postprocessing.
    bool cpu_mask_writeback = false;
    // RGO: MSAA sample count (1, 2, 4 or 8), fixed for the shell's lifetime.
    std::uint8_t msaa_samples = 1U;
};

struct Rt64RdramRange {
    std::size_t begin;
    std::size_t end;
};

// Commit completed device-owned ranges, never an old whole-memory snapshot.
// All spans are host-word-swapped. Validate every range and CPU ownership
// before writing anything; intervening CPU writes reject the entire commit.
// When supplied, previous_source advances with the commit so later CPU edits
// back to pre-render values are still recognized by snapshot merging.
[[nodiscard]] Rt64ShellError commit_rt64_rdram_ranges(
    std::span<const std::byte> submitted, std::span<const std::byte> rendered,
    std::span<std::byte> live, std::span<const Rt64RdramRange> ranges,
    std::span<std::byte> previous_source = {}) noexcept;

struct Rt64GraphicsTask {
    std::uint32_t ucode_address = 0U;
    std::uint32_t ucode_data_address = 0U;
    std::uint32_t command_address = 0U;
};

struct Rt64FramebufferInfo {
    std::uint32_t address = 0U;
    std::size_t width = 0U;
    std::size_t height = 0U;
    std::size_t bytes_per_pixel = 0U;

    [[nodiscard]] bool valid() const noexcept {
        if (width == 0U || height == 0U || bytes_per_pixel == 0U ||
            width > SIZE_MAX / bytes_per_pixel) {
            return false;
        }
        const std::size_t row_bytes = width * bytes_per_pixel;
        return height <= SIZE_MAX / row_bytes;
    }

    [[nodiscard]] std::size_t byte_count() const noexcept {
        return valid() ? width * bytes_per_pixel * height : 0U;
    }
};

struct Rt64FrameView {
    std::size_t width = 0U;
    std::size_t height = 0U;
    std::size_t row_pitch_bytes = 0U;
    std::span<const std::byte> bgra8;

    [[nodiscard]] bool valid() const noexcept {
        return width != 0U && height != 0U && width <= SIZE_MAX / 4U &&
            row_pitch_bytes >= width * 4U &&
            row_pitch_bytes <= SIZE_MAX / height &&
            bgra8.size() == row_pitch_bytes * height;
    }
};

struct Rt64GraphicsDiagnostics {
    std::size_t matrix_commands = 0U;
    std::size_t vertex_batches = 0U;
    std::size_t vertices_loaded = 0U;
    std::size_t vertices_finite = 0U;
    std::size_t vertices_in_clip = 0U;
    std::size_t triangle_batches = 0U;
    std::size_t triangles_drawn = 0U;
    std::size_t dma_display_lists = 0U;
    std::uint32_t color_image_address = 0U;
    std::uint32_t rejected_command_word0 = 0U;
    std::uint32_t rejected_command_word1 = 0U;
    std::uint32_t rejected_command_address = 0U;
    std::uint32_t rejection_reason = 0U;
    std::uint32_t rejection_detail = 0U;
    std::uint32_t rejection_source_address = 0U;
    std::uint32_t rejection_triangle = 0U;
    std::uint64_t loaded_vertices_low = 0U;
    std::uint16_t loaded_vertices_high = 0U;
    std::uint64_t full_sync_microseconds = 0U;
    std::uint64_t rdram_check_microseconds = 0U;
    std::size_t display_list_branches = 0U;
    std::uint32_t last_display_list_address = 0U;
    std::uint32_t last_display_list_target = 0U;
};

[[nodiscard]] Rt64ShellError validate_rt64_shell_configuration(
    const Rt64ShellConfiguration& configuration) noexcept;

[[nodiscard]] Rt64ShellError copy_rt64_rdram_snapshot(
    std::span<const std::byte> source,
    std::span<std::byte> destination,
    Rt64MemoryLayout layout) noexcept;

// Imports a new simulation snapshot without erasing bytes that RT64 wrote to
// its private render-to-RAM copy. Bytes changed by the simulation always win;
// unchanged source bytes retain the renderer's current value.
[[nodiscard]] Rt64ShellError merge_rt64_rdram_snapshot(
    std::span<const std::byte> source,
    std::span<std::byte> destination,
    std::span<std::byte> previous_source,
    Rt64MemoryLayout layout) noexcept;

// Refresh CPU-owned bytes even when the CPU rewrites their previous value.
// Only the sorted (possibly overlapping) live framebuffer ranges retain GPU data.
[[nodiscard]] Rt64ShellError refresh_rt64_cpu_memory(
    std::span<const std::byte> source, std::span<std::byte> destination,
    std::span<const Rt64RdramRange> gpu_ranges, Rt64MemoryLayout layout) noexcept;

[[nodiscard]] Rt64ShellError inspect_rt64_framebuffer(
    const Rt64ViRegisters& vi,
    std::size_t rdram_size,
    Rt64FramebufferInfo& framebuffer) noexcept;

class Rt64Shell final {
public:
    ~Rt64Shell();
    Rt64Shell(const Rt64Shell&) = delete;
    Rt64Shell& operator=(const Rt64Shell&) = delete;
    Rt64Shell(Rt64Shell&&) = delete;
    Rt64Shell& operator=(Rt64Shell&&) = delete;

    [[nodiscard]] static std::unique_ptr<Rt64Shell> create(
        const Rt64ShellConfiguration& configuration,
        Rt64ShellError& error) noexcept;

    [[nodiscard]] Rt64ShellError submit(
        const Rt64GraphicsTask& task) noexcept;
    [[nodiscard]] Rt64ShellError replace_rdram_snapshot(
        std::span<const std::byte> rdram,
        Rt64MemoryLayout layout) noexcept;
    [[nodiscard]] Rt64ShellError copy_private_rdram_snapshot(
        std::span<std::byte> rdram,
        Rt64MemoryLayout layout) const noexcept;
    [[nodiscard]] Rt64ShellError commit_cpu_writeback(
        std::span<const std::byte> submitted, std::span<std::byte> live) noexcept;
    // Query on the same frontend thread as submit/present. This follows RT64's
    // resident framebuffer registry, independently of host snapshot lifetimes.
    [[nodiscard]] bool has_color_framebuffer(std::uint32_t address) const noexcept;
    [[nodiscard]] Rt64ShellError present(bool capture_frame = true) noexcept;
    // RGO display settings (aspect, max aspect, render resolution, VSync).
    // Apply from the next submitted task; MSAA is fixed at creation.
    void set_video_settings(const VideoSettings& settings) noexcept;
    // RT64's resolution multiplier for the last submitted frame (0 before).
    [[nodiscard]] double render_multiplier() const noexcept;
    // RGO frame interpolation. The game's logic rate (60 / VIs per game
    // frame) for the next tasks; RT64 blends frames up to the Frame rate
    // setting's target.
    void set_game_rate(std::uint16_t rate) noexcept;
    // An object matrix the game wrote at physical_address with these words,
    // and its interpolation ID (frame_interpolation.hpp).
    void tag_matrix(std::uint32_t physical_address, std::uint32_t words_hash,
                    std::uint32_t id) noexcept;
    // Frames presented to the swap chain so far, interpolated ones included.
    [[nodiscard]] std::uint64_t display_presents() const noexcept;
    // RGO FPS counter: the shortest and longest gap between two presented
    // frames since the previous call, in microseconds (0 when fewer than two
    // frames were presented).
    struct DisplayFrameTimes {
        std::uint64_t min_us = 0U;
        std::uint64_t max_us = 0U;
    };
    [[nodiscard]] DisplayFrameTimes take_display_frame_times() noexcept;
    // How much wider than the game's own projection the last submitted frame
    // is presented (1.0 when widescreen is off or the window is 4:3).
    [[nodiscard]] float horizontal_extent() const noexcept;
    [[nodiscard]] bool developer_debugger_available() const noexcept;
    [[nodiscard]] std::size_t last_command_count() const noexcept;
    [[nodiscard]] Rt64GraphicsDiagnostics last_graphics_diagnostics()
        const noexcept;
    [[nodiscard]] Rt64FramebufferInfo last_framebuffer() const noexcept;
    [[nodiscard]] Rt64FrameView last_presented_frame() const noexcept;

private:
    struct Impl;
    explicit Rt64Shell(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* rt64_shell_error_message(
    Rt64ShellError error) noexcept;

}  // namespace jfg
