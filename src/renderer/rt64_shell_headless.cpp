// RGO headless Linux build (JFG_HEADLESS_LIVE): an Rt64Shell without a GPU.
//
// The live runtime keeps its RDP/VI handshake: tasks are accepted, every
// color image counts as resident, and a present only counts frames. Nothing
// is drawn, so CPU framebuffer effects see unrendered memory. That does not
// change the game's logic or timing, which is what the headless build is for
// (docs/rgo/framerate.md, "Headless measurement").
#include "jfg/renderer/rt64_shell.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

namespace jfg {

struct Rt64Shell::Impl {
    std::atomic<std::uint64_t> presents{0U};
    std::size_t submitted_tasks = 0U;
    std::chrono::steady_clock::time_point last_present{};
    std::uint64_t present_min_us = UINT64_MAX;
    std::uint64_t present_max_us = 0U;
};

Rt64Shell::Rt64Shell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Rt64Shell::~Rt64Shell() = default;

std::unique_ptr<Rt64Shell> Rt64Shell::create(
    const Rt64ShellConfiguration& configuration,
    Rt64ShellError& error) noexcept {
    if (configuration.rom_header.size() < kRt64RequiredHeaderBytes ||
        configuration.rdram.size() != kRt64RequiredRdramBytes ||
        configuration.vi == nullptr) {
        error = Rt64ShellError::invalid_memory;
        return nullptr;
    }
    error = Rt64ShellError::none;
    return std::unique_ptr<Rt64Shell>(
        new (std::nothrow) Rt64Shell(std::make_unique<Impl>()));
}

Rt64ShellError Rt64Shell::submit(const Rt64GraphicsTask&) noexcept {
    ++impl_->submitted_tasks;
    return Rt64ShellError::none;
}

Rt64ShellError Rt64Shell::replace_rdram_snapshot(
    std::span<const std::byte>, Rt64MemoryLayout) noexcept {
    return Rt64ShellError::none;
}

Rt64ShellError Rt64Shell::copy_private_rdram_snapshot(
    std::span<std::byte>, Rt64MemoryLayout) const noexcept {
    return Rt64ShellError::none;
}

Rt64ShellError Rt64Shell::commit_cpu_writeback(
    std::span<const std::byte>, std::span<std::byte>) noexcept {
    return Rt64ShellError::none;
}

bool Rt64Shell::has_color_framebuffer(std::uint32_t) const noexcept {
    return true;
}

Rt64ShellError Rt64Shell::present(bool) noexcept {
    impl_->presents.fetch_add(1U, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (impl_->last_present != std::chrono::steady_clock::time_point{}) {
        const auto interval = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - impl_->last_present).count());
        impl_->present_min_us = (std::min)(impl_->present_min_us, interval);
        impl_->present_max_us = (std::max)(impl_->present_max_us, interval);
    }
    impl_->last_present = now;
    return Rt64ShellError::none;
}

Rt64Shell::DisplayFrameTimes Rt64Shell::take_display_frame_times() noexcept {
    const DisplayFrameTimes times{
        impl_->present_min_us == UINT64_MAX ? 0U : impl_->present_min_us,
        impl_->present_max_us};
    impl_->present_min_us = UINT64_MAX;
    impl_->present_max_us = 0U;
    return times;
}

void Rt64Shell::set_video_settings(const VideoSettings&) noexcept {}

double Rt64Shell::render_multiplier() const noexcept { return 1.0; }

void Rt64Shell::set_game_rate(std::uint16_t) noexcept {}

void Rt64Shell::tag_matrix(std::uint32_t, std::uint32_t,
                           std::uint32_t) noexcept {}

std::uint64_t Rt64Shell::display_presents() const noexcept {
    return impl_->presents.load(std::memory_order_relaxed);
}

float Rt64Shell::horizontal_extent() const noexcept { return 1.0F; }

bool Rt64Shell::developer_debugger_available() const noexcept { return false; }

std::size_t Rt64Shell::last_command_count() const noexcept { return 0U; }

// Nothing is decoded, so report a placeholder color image: the runtime then
// treats each completed task as a drawn frame and presents at the next VI.
// It never equals a real VI framebuffer, so off-screen task checks hold.
inline constexpr std::uint32_t kHeadlessColorImage = 0x00FFFFC0U;

Rt64GraphicsDiagnostics Rt64Shell::last_graphics_diagnostics() const noexcept {
    Rt64GraphicsDiagnostics diagnostics{};
    diagnostics.color_image_address = kHeadlessColorImage;
    return diagnostics;
}

Rt64FramebufferInfo Rt64Shell::last_framebuffer() const noexcept { return {}; }

Rt64FrameView Rt64Shell::last_presented_frame() const noexcept { return {}; }

}  // namespace jfg
